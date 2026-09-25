// PC-hosted stand-in for the REAL ESP32-S3 firmware (esp32-s3-msc/esp32-s3-msc.ino),
// until the physical board arrives. This is NOT a separate simulator -- it
// #includes the exact same fat_disk_shared.h the real .ino compiles from
// (constants, build_boot_sector/build_fat/build_root_dir, disk_append,
// disk_valid_bytes, disk_read_at, byte-for-byte identical source), so the
// disk logic under test here is the real firmware's actual logic, not a
// hand-copied re-implementation that could silently drift out of sync
// (the same class of bug already caught once in this project: a fix
// applied to one call site not propagated to another).
//
// What's platform-specific (necessarily different from the real .ino,
// since there's no UART/USBMSC hardware on a PC):
//   - Serial link to the real classic ESP32: POSIX termios instead of
//     Arduino's HardwareSerial, but the SAME framing logic (find_sync,
//     header parse, disk_append retry-then-force) as the .ino's link_task().
//   - Disk serving: a TCP server speaking sector_protocol.py's exact wire
//     format (so car_sim.py, the mock car radio, works completely
//     unmodified as the client) instead of USBMSC's onRead callback --
//     but calling the SAME disk_read_at(), with NO retry loop, matching
//     the real firmware's actual non-blocking/zero-fill-on-straddle
//     behavior. This is a deliberate departure from the OLDER
//     s3_sim_serial.py's serve_radio(), which retries/blocks on a
//     straddle -- that retry behavior is a Python-simulator-only design,
//     not what the real hardware does, and the whole point of this
//     program is fidelity to the real firmware's behavior, not a more
//     lenient PC-only variant.
//   - Resource monitoring: RAM/CPU usage, reported against the real
//     ESP32-S3-WROOM-1 N16R8's actual hardware budget (512KB SRAM,
//     8MB Octal PSRAM, dual-core 240MHz Xtensa LX7) so a resource-budget
//     problem can be caught before the real board arrives. See
//     report_resource_usage() below for exactly what is and isn't a
//     meaningful apples-to-apples comparison.
//
// Usage (drop-in replacement for s3_sim_serial.py in the live pipeline):
//   g++ -O2 -std=c++17 -pthread -o s3_real_firmware_host s3_real_firmware_host.cpp
//   ./s3_real_firmware_host [--expected-serial SSSSSSSSSS] [--baud N] [--radio-port N]

#ifndef ARDUINO
#define PROGMEM  // silence_primer.h uses this Arduino-only macro; no-op on host
#endif

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include "../esp32-s3-msc/fat_disk_shared.h"
#include "../esp32-s3-msc/silence_primer.h"

// ===================== CLI-configurable settings (defaults match s3_sim_serial.py) =

static const char *g_expected_serial = "5B52096812";  // NEVER 5B07008126 (Fin-ESP, unrelated)
static int g_baud = 921600;
static int g_radio_port = 9401;

static const double STALE_TIMEOUT_SEC = 5.0;
static const double PORT_SETTLE_DELAY_SEC = 0.5;

static const uint8_t OPCODE_READ10 = 0x01;
static const uint8_t OPCODE_WATERMARK = 0x02;

// ===================== Rigorous delay-chain measurement (2026-09-17) ===============
//
// All three timestamps below use system_clock (real wall-clock/epoch time,
// not steady_clock's arbitrary per-process origin) specifically so they're
// directly comparable against a SEPARATE process's own epoch timestamps
// (the audio-level monitor script measuring T3, run independently). T1 is
// captured the instant this program RECEIVES the AUDIO_STATE:Started
// control frame (i.e. the moment real PCM starts being written into the
// ring) -- g_mark_pos snapshots the ring's write position at that exact
// moment. T2 is captured the instant a READ10 request actually SERVES
// real (non-zero-filled) data spanning that exact marked position back to
// the client -- i.e. the moment the reader's own traversal genuinely
// reaches the real content, not "should have by now" reasoning.
static std::atomic<bool> g_mark_active{false};
static std::atomic<uint32_t> g_mark_pos{0};
static std::atomic<bool> g_mark_found{false};

static double epoch_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ===================== Serial port resolution (mirrors s3_sim_serial.py) ===========

static bool resolve_serial_port(const char *expected_serial, char *out_path, size_t out_len) {
  glob_t g;
  if (glob("/dev/ttyACM*", 0, nullptr, &g) != 0) {
    globfree(&g);
    return false;
  }
  bool found = false;
  for (size_t i = 0; i < g.gl_pathc && !found; i++) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "udevadm info -q property -n %s 2>/dev/null", g.gl_pathv[i]);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) continue;
    char line[512];
    char needle[128];
    snprintf(needle, sizeof(needle), "ID_SERIAL_SHORT=%s", expected_serial);
    while (fgets(line, sizeof(line), pipe)) {
      line[strcspn(line, "\n")] = 0;
      if (strcmp(line, needle) == 0) {
        strncpy(out_path, g.gl_pathv[i], out_len - 1);
        out_path[out_len - 1] = 0;
        found = true;
        break;
      }
    }
    pclose(pipe);
  }
  globfree(&g);
  return found;
}

static speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return B921600;
  }
}

// Blocks (retrying indefinitely, printing status) until the board is found
// and openable -- mirrors open_serial_resilient()'s reasoning exactly:
// giving up would mean permanently losing audio until a human physically
// intervenes.
static int open_serial_resilient(const char *expected_serial, int baud) {
  int attempt = 0;
  char path[256];
  while (true) {
    attempt++;
    if (!resolve_serial_port(expected_serial, path, sizeof(path))) {
      if (attempt == 1 || attempt % 10 == 0) {
        fprintf(stderr, "[s3-host] board (serial %s) not currently found on any "
                         "/dev/ttyACM* -- waiting...\n", expected_serial);
      }
      usleep((useconds_t)(PORT_SETTLE_DELAY_SEC * 1e6));
      continue;
    }
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
      if (attempt <= 10 || attempt % 10 == 0) {
        fprintf(stderr, "[s3-host] open %s failed (attempt %d): %s -- retrying...\n",
                path, attempt, strerror(errno));
      }
      usleep((useconds_t)(PORT_SETTLE_DELAY_SEC * 1e6));
      continue;
    }
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) != 0) { close(fd); continue; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_to_speed(baud));
    cfsetospeed(&tio, baud_to_speed(baud));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;  // no hardware flow control
    // VMIN=0, VTIME=20 (2.0s in deciseconds) mirrors pyserial's ser.timeout=2.0:
    // a read() call blocks for up to 2s, returning whatever arrived (possibly
    // 0 bytes) rather than waiting forever -- lets the staleness check below
    // actually run periodically instead of hanging on a dead connection.
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 20;
    tcsetattr(fd, TCSANOW, &tio);
    fprintf(stderr, "[s3-host] opened %s (serial %s) at %d baud (attempt %d)\n",
            path, expected_serial, baud, attempt);
    return fd;
  }
}

// ===================== UART link to the classic ESP32 (mirrors link_task()) ========

// Exposed so the FATDISK_MULTI_FILE file-switch callback (below) can write
// back to the same classic ESP32 -- link_supervisor() sets this once the
// port is open, clears it on disconnect. -1 means "not currently connected,
// don't try to write."
static std::atomic<int> g_esp32_fd{-1};

#ifdef FATDISK_MULTI_FILE
// Reuses the ALREADY-WORKING PC-typed command interface (handle_pc_command()
// in esp32-bt-mp3-test.ino, gated behind AVRC_INVESTIGATION, confirmed live
// tonight for next/prev/play/pause/vol) instead of building out a separate
// RADIO_CMD-over-dedicated-return-channel message for this test -- that's
// PLAN_NEXT.md C3's eventual real-hardware mechanism, but it needs the
// physical S3's own GPIO4->classic-GPIO19 wire, which this PC stand-in has
// no way to drive. Writing "next\n"/"prev\n" over the SAME serial connection
// already used for the forward link validates the full end-to-end concept
// (does a detected file-switch really make the classic send a real AVRCP
// command) against the REAL classic hardware tonight, with zero changes to
// its firmware -- the classic already understands these exact two words.
static void send_radio_cmd_to_classic(int direction) {
  int fd = g_esp32_fd.load();
  if (fd < 0) {
    fprintf(stderr, "[s3-host][RADIO_CMD] detected a switch (direction=%d) but the "
                     "classic isn't connected right now -- dropped\n", direction);
    return;
  }
  const char *cmd = (direction > 0) ? "next\n" : "prev\n";
  ssize_t w = write(fd, cmd, strlen(cmd));
  fprintf(stderr, "[s3-host][RADIO_CMD] car radio switched files (direction=%d) -- "
                   "relayed '%s' to the classic (%zd bytes written)\n",
          direction, (direction > 0) ? "next" : "prev", w);
}
#endif

class SerialStaleError {};

static void read_exact(int fd, uint8_t *buf, uint32_t n) {
  uint32_t got = 0;
  auto last_progress = std::chrono::steady_clock::now();
  while (got < n) {
    ssize_t r = read(fd, buf + got, n - got);
    if (r > 0) {
      got += (uint32_t)r;
      last_progress = std::chrono::steady_clock::now();
    } else {
      double idle = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_progress).count();
      if (idle > STALE_TIMEOUT_SEC) throw SerialStaleError();
    }
  }
}

static void find_sync(int fd) {
  uint8_t b;
  while (true) {
    read_exact(fd, &b, 1);
    if (b == FRAME_MAGIC) return;
  }
}

// Mirrors esp32-s3-msc.ino's link_task() exactly: same framing, same
// disk_append() retry-then-force fallback (MAX_WRITE_RETRIES/
// WRITE_RETRY_DELAY_MS from the shared header), applied to the SAME
// shared disk_append() function -- not a re-implementation.
static void receive_from_esp32(int fd) {
  static uint8_t payload[MAX_FRAME_LEN];
  uint64_t total = 0;
  auto start = std::chrono::steady_clock::now();
  auto last_log = start;
  while (true) {
    find_sync(fd);
    uint8_t header[5];
    read_exact(fd, header, 5);
    char frame_type = (char)header[0];
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                       ((uint32_t)header[3] << 8) | (uint32_t)header[4];
    if ((frame_type != 'A' && frame_type != 'C') || length > MAX_FRAME_LEN) {
      continue;
    }
    if (length) read_exact(fd, payload, length);
    g_link_last_frame_ms = FATDISK_MILLIS();  // mirrors link_task()'s own update (B1)

    if (frame_type == 'A') {
      bool ok = false;
      for (uint32_t i = 0; i < MAX_WRITE_RETRIES; i++) {
        if (disk_append(payload, length, true)) { ok = true; break; }
        usleep(WRITE_RETRY_DELAY_MS * 1000);
      }
      if (!ok) disk_append(payload, length, false);
      total += length;
    }
    // 'C' control/diagnostic frames: log them, same role as
    // s3_sim_serial.py's [CONTROL] lines (real AVRCP play/pause/track
    // events from a real phone become visible here).
    else if (frame_type == 'C') {
      char text[MAX_FRAME_LEN + 1];
      memcpy(text, payload, length);
      text[length] = 0;
      fprintf(stderr, "[s3-host][CONTROL] %s\n", text);

      if (strstr(text, "AUDIO_STATE:Started")) {
        g_mark_pos.store(g_write_pos);
        g_mark_found.store(false);
        g_mark_active.store(true);
        fprintf(stderr, "[s3-host][DELAY-MEASURE] T1 epoch=%.6f mark_pos=%u "
                         "(real PCM starts arriving; ring write position snapshotted)\n",
                epoch_now(), g_mark_pos.load());
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last_log).count() >= 1.0) {
      fprintf(stderr, "[s3-host][serial-rx] t=%.2fs audio_received=%lluB\n",
              std::chrono::duration<double>(now - start).count(), (unsigned long long)total);
      last_log = now;
    }
  }
}

// Supervisor: resolves the port, opens it, runs the receive loop, and on
// ANY failure (I/O error or SerialStaleError) closes the handle and
// starts over -- forever. Mirrors run_serial_bridge()'s self-healing
// design (the real, confirmed need for this: a crashed/rebooted ESP32
// can re-enumerate to a different /dev/ttyACMx path).
static void link_supervisor() {
  while (true) {
    int fd = open_serial_resilient(g_expected_serial, g_baud);
    g_esp32_fd.store(fd);
    try {
      receive_from_esp32(fd);
    } catch (...) {
      fprintf(stderr, "[s3-host][RECONNECTING] serial ingestion stopped -- "
                       "reconnecting automatically, no restart needed\n");
    }
    g_esp32_fd.store(-1);
    close(fd);
  }
}

// ===================== TCP disk server (sector_protocol.py wire format) ============

static bool recv_all(int fd, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, buf + got, n - got, 0);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

static bool send_all(int fd, const uint8_t *buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = send(fd, buf + sent, n - sent, 0);
    if (w <= 0) return false;
    sent += (size_t)w;
  }
  return true;
}

// Serves READ10/WATERMARK requests directly from the shared disk_read_at()
// -- deliberately NO retry loop on straddle (unlike s3_sim_serial.py's
// serve_radio(), which retries up to MAX_READ_RETRIES times). Real
// hardware's TinyUSB callback can't block/retry either (see
// fat_disk_shared.h's disk_read_at() comment in the .ino for the full
// rationale) -- running that exact non-retrying logic here is the point:
// this program exists to observe how the REAL firmware's actual behavior
// performs under a real live feed, not a more lenient PC-only variant.
static void serve_radio(int conn) {
  uint64_t read_count = 0;
  uint64_t straddle_count = 0;   // reads where disk_read_at() zero-filled
                                  // part of the response because the
                                  // reader had caught up too close to the
                                  // live write edge -- an audible gap, not
                                  // a bug: this is the real firmware's
                                  // actual non-blocking behavior under
                                  // real timing, exactly what this program
                                  // exists to surface before the board
                                  // arrives.
  uint64_t lock_miss_count = 0;  // the 2ms bounded mutex try-lock lost
  auto start = std::chrono::steady_clock::now();
  auto last_log = start;
  while (true) {
    uint8_t req[7];
    if (!recv_all(conn, req, sizeof(req))) break;
    uint8_t opcode = req[0];
    uint32_t lba = ((uint32_t)req[1] << 24) | ((uint32_t)req[2] << 16) |
                   ((uint32_t)req[3] << 8) | (uint32_t)req[4];
    uint16_t count = ((uint16_t)req[5] << 8) | (uint16_t)req[6];

    if (opcode == OPCODE_READ10) {
      uint32_t len = (uint32_t)count * SECTOR_SIZE;
      static uint8_t buf[65536];
      if (len > sizeof(buf)) len = sizeof(buf);
      uint64_t abs_pos = (uint64_t)lba * SECTOR_SIZE;
      bool straddled = false, lock_missed = false;
      disk_read_at(abs_pos, buf, len, &straddled, &lock_missed);
      if (straddled) straddle_count++;
      if (lock_missed) lock_miss_count++;

      // T2: the instant the reader's own traversal genuinely reaches the
      // exact ring position marked at T1 -- checked directly against
      // whether THIS request's byte range covers that position AND was
      // actually served as real data (not zero-filled), not inferred.
      if (g_mark_active.load() && !g_mark_found.load()) {
        const uint32_t data_region_start = FIRST_DATA_LBA * SECTOR_SIZE;
        if (abs_pos >= data_region_start) {
          uint32_t data_off = (uint32_t)(abs_pos - data_region_start);
          uint32_t mark = g_mark_pos.load();
          if (data_off <= mark && mark < data_off + len && !straddled) {
            g_mark_found.store(true);
            fprintf(stderr, "[s3-host][DELAY-MEASURE] T2 epoch=%.6f "
                             "(reader traversal reached the T1-marked real content)\n",
                    epoch_now());
          }
        }
      }

      if (!send_all(conn, buf, len)) break;
      read_count++;
    } else if (opcode == OPCODE_WATERMARK) {
      uint32_t v = disk_valid_bytes();
      uint8_t resp[8] = {0, 0, 0, 0, (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                          (uint8_t)(v >> 8), (uint8_t)v};
      if (!send_all(conn, resp, 8)) break;
    } else {
      break;  // unknown opcode -- matches s3_sim_serial.py's serve_radio()
    }

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last_log).count() >= 1.0) {
      fprintf(stderr, "[s3-host][reads] t=%.2fs reads=%llu straddled(zero-fill-gap)=%llu "
                       "lock_miss=%llu\n",
              std::chrono::duration<double>(now - start).count(),
              (unsigned long long)read_count, (unsigned long long)straddle_count,
              (unsigned long long)lock_miss_count);
      last_log = now;
    }
  }
  fprintf(stderr, "[s3-host] radio disconnected (%llu reads served, %llu straddled, "
                   "%llu lock_miss)\n",
          (unsigned long long)read_count, (unsigned long long)straddle_count,
          (unsigned long long)lock_miss_count);
}

// ===================== Resource monitoring vs. real S3 hardware budget =============

// Real ESP32-S3-WROOM-1 N16R8 budget (confirmed from the actual purchase
// listing -- see esp32-s3-msc.ino's header comment): 512KB SRAM total,
// 8MB (8,388,608B) Octal PSRAM, dual-core 240MHz Xtensa LX7.
static const uint64_t S3_SRAM_BUDGET_BYTES = 512ULL * 1024;
static const uint64_t S3_PSRAM_BUDGET_BYTES = 8ULL * 1024 * 1024;

static void report_resource_usage() {
  // These sizes come directly from the shared struct/array definitions in
  // fat_disk_shared.h -- exact, known, and apples-to-apples comparable to
  // what the real firmware allocates: g_ring is PSRAM-backed on real
  // hardware (heap_caps_malloc(..., MALLOC_CAP_SPIRAM)), the boot/FAT/
  // root-dir caches are static internal-SRAM arrays on real hardware.
  uint64_t ring_bytes = DECLARED_FILE_SIZE;
  uint64_t static_cache_bytes = sizeof(g_boot_sector) + sizeof(g_fat_sector_cache) +
                                 sizeof(g_root_dir_sector);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // PC-process-only numbers -- NOT apples-to-apples with the embedded
    // target (a PC process carries libc/thread-stack/heap-allocator
    // overhead an ESP32 firmware image never has), but still useful as a
    // sanity signal that nothing is leaking/spinning unexpectedly.
    long rss_kb = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
      char line[256];
      while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
          sscanf(line + 6, "%ld", &rss_kb);
          break;
        }
      }
      fclose(f);
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double cpu_sec = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                      ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;

    fprintf(stderr,
        "[s3-host][RESOURCE] real-target-comparable: ring_buffer=%.1fKB "
        "(PSRAM budget %.0fKB, %.2f%%)  static_caches=%.1fKB (SRAM budget "
        "%.0fKB, %.2f%%)  ||  PC-process-only (not applicable to embedded "
        "target): RSS=%ldKB  cumulative_CPU=%.1fs\n",
        ring_bytes / 1024.0, S3_PSRAM_BUDGET_BYTES / 1024.0,
        100.0 * ring_bytes / S3_PSRAM_BUDGET_BYTES,
        static_cache_bytes / 1024.0, S3_SRAM_BUDGET_BYTES / 1024.0,
        100.0 * static_cache_bytes / S3_SRAM_BUDGET_BYTES,
        rss_kb, cpu_sec);

    if (static_cache_bytes > S3_SRAM_BUDGET_BYTES) {
      fprintf(stderr, "[s3-host][RESOURCE][WARNING] static caches alone "
                       "EXCEED the real S3's total SRAM budget -- this "
                       "would not fit on real hardware!\n");
    }
    if (ring_bytes > S3_PSRAM_BUDGET_BYTES) {
      fprintf(stderr, "[s3-host][RESOURCE][WARNING] ring buffer EXCEEDS "
                       "the real S3's PSRAM budget -- this would not fit "
                       "on real hardware!\n");
    }
  }
}

// ===================== main =========================================================

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--expected-serial") && i + 1 < argc) g_expected_serial = argv[++i];
    else if (!strcmp(argv[i], "--baud") && i + 1 < argc) g_baud = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--radio-port") && i + 1 < argc) g_radio_port = atoi(argv[++i]);
    else {
      fprintf(stderr, "usage: %s [--expected-serial S] [--baud N] [--radio-port N]\n", argv[0]);
      return 1;
    }
  }

  build_boot_sector();
  build_fat();
  build_root_dir();
  g_ring_mutex = FATDISK_MUTEX_CREATE();
  g_ring = (uint8_t *)malloc(DECLARED_FILE_SIZE);
  if (!g_ring) {
    fprintf(stderr, "[s3-host] FATAL: allocation for ring buffer failed\n");
    return 1;
  }
  memset(g_ring, 0, DECLARED_FILE_SIZE);
  disk_append(SILENCE_PRIMER, SILENCE_PRIMER_LEN, false);

#ifdef FATDISK_MULTI_FILE
  g_file_switch_callback = send_radio_cmd_to_classic;
  fprintf(stderr, "[s3-host] FATDISK_MULTI_FILE enabled: presenting %u files "
                   "(all aliasing the same live ring), relaying detected radio "
                   "next/prev switches to the classic as PC-command-interface text\n",
          NUM_FILES);
#endif

  fprintf(stderr, "[s3-host] running the REAL esp32-s3-msc.ino disk logic "
                   "(fat_disk_shared.h) on this PC as a stand-in until the "
                   "physical S3 board arrives\n");
  fprintf(stderr, "[s3-host] FAT12 volume: %u sectors, declared file size %u bytes, "
                   "first data LBA %u\n", TOTAL_SECTORS, DECLARED_FILE_SIZE, FIRST_DATA_LBA);

  std::thread(link_supervisor).detach();
  std::thread(report_resource_usage).detach();

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = htons(g_radio_port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "[s3-host] FATAL: bind on port %d failed: %s\n", g_radio_port, strerror(errno));
    return 1;
  }
  listen(srv, 1);
  fprintf(stderr, "[s3-host] waiting for radio on port %d...\n", g_radio_port);

  while (true) {
    int conn = accept(srv, nullptr, nullptr);
    if (conn < 0) continue;
    fprintf(stderr, "[s3-host] radio connected\n");
    serve_radio(conn);
    close(conn);
  }
}
