// Real ESP32-S3 firmware: presents a USB-MSC "virtual disk" (one growing
// FAT12 volume, one MP3 file) to the car radio, backed by a live ring
// buffer fed over UART from the classic ESP32 (BT A2DP sink + Shine
// encoder). This is the real hardware counterpart to sim/fat12_disk.py +
// sim/s3_sim_serial.py -- see CLAUDE.md for the full real/prototype
// distinction. The ring-buffer/FAT12 algorithm here is a near-verbatim
// port of fat12_disk.py's GrowingFat12Disk; the UART framing matches
// esp32-bt-mp3-test.ino's send_framed()/send_control() wire format
// exactly (FRAME_MAGIC 0xAA, 1-byte type, 4-byte big-endian length,
// payload), since the real wired link is that same UART, just physically
// wired to another board's RX pin instead of a USB-serial chip to a PC.
//
// STATUS AS OF 2026-09-17 (written the night before the physical board
// arrives -- see progress/STATUS.md for the full writeup): this compiles
// clean against esp32:esp32:esp32s3:USBMode=default,PSRAM=opi (confirmed
// the right PSRAM setting from the actual purchase listing: ESP32-S3-
// WROOM-1 N16R8, 16MB Quad flash + 8MB Octal PSRAM), and the logic has
// been carefully cross-checked line-by-line against the already-validated
// Python prototype (see esp32-s3-msc/crosscheck/ -- boot sector, FAT
// table, root directory, ring-buffer/backpressure logic, and UART
// framing/resync all confirmed byte-for-byte or step-for-step identical
// to the proven reference), but **it has NEVER run on real ESP32-S3
// hardware** -- there was no board to test on yet. One thing still
// specifically needs real-hardware verification: UART_S3_RX_PIN below
// (the actual wiring isn't known yet). The other original open question
// -- TinyUSB's onRead call granularity -- is now resolved from TinyUSB's
// own public source/docs (github.com/hathach/tinyusb, msc_device.c),
// not just assumed: `bufsize` is a fixed compile-time constant
// (CFG_TUD_MSC_EP_BUFSIZE, commonly 512B on this platform) every call,
// and TinyUSB itself splits a larger SCSI transfer into multiple calls
// at that fixed size, advancing `offset` each time -- disk_read_at()
// below was already written to handle an arbitrary byte range generically,
// so this changes nothing about the implementation, just upgrades this
// from "reasoned assumption" to "confirmed from the actual library
// source." Treat every other "should work" comment below as "reasoned
// from the working Python prototype and the real board's own datasheet
// -- not yet hardware-confirmed."
//
// To regenerate silence_primer.h after changing sim/silence_primer.mp3:
//   python3 -c "
//   data = open('../sim/silence_primer.mp3','rb').read()
//   ... (see esp32-s3-msc.ino git history for the exact one-liner used)"

#include <Arduino.h>

#ifndef ARDUINO_USB_MODE
#error This board has no native USB -- must be an S2/S3 target
#elif ARDUINO_USB_MODE == 1
#error Build with USB Mode = "USB-OTG (TinyUSB)", not "Hardware CDC and JTAG"
#else

#include "USB.h"
#include "USBMSC.h"
#include "esp_heap_caps.h"
#include "silence_primer.h"

// ===================== Wire protocol (must match esp32-bt-mp3-test.ino) ====

static const uint8_t FRAME_MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;

// UART pins to the classic ESP32's TX (its Serial/UART0 TX pin, wired
// directly -- see CLAUDE.md's real end-state architecture diagram).
// *** NOT YET HARDWARE-VERIFIED *** -- placeholder pin, pick a real
// UART-capable GPIO once the board's actual wiring is decided; these are
// just commonly-free pins on typical ESP32-S3-WROOM-1 DevKitC-1 boards.
//
// Real target board confirmed 2026-09-17 (Muni sent the actual purchase
// listing): ESP32-S3-WROOM-1 N16R8 DevKitC-1 -- 16MB Quad flash + 8MB
// Octal PSRAM. This matters for pin choice: GPIO 26-32 are dedicated to
// the Quad flash on ANY ESP32-S3 module, and Octal PSRAM (this module
// has it -- confirms PSRAM=opi below is the right build flag, not a
// guess) additionally dedicates GPIO 33-37 (Espressif's own
// ESP32-S3-WROOM-1 datasheet: "not recommended for other uses" on
// octal-PSRAM variants). GPIO 17/18 below are well outside that reserved
// 26-37 range, so no conflict -- but if this pin choice changes once the
// real wiring is decided, stay outside 26-37 on this specific module.
#define UART_S3_RX_PIN 18
#define UART_S3_TX_PIN 17  // unused (classic ESP32 -> S3 is one-way), kept for symmetry
#define UART_BAUD 921600
HardwareSerial LinkSerial(1);  // UART1

// ===================== FAT12 ring-buffer disk (port of fat12_disk.py) ======

static const uint32_t SECTOR_SIZE = 512;
static const uint32_t SECTORS_PER_CLUSTER = 8;  // 4KB clusters
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const char FILE_NAME[12] = "STREAM  MP3";  // 8.3, space-padded (11 bytes + NUL)

// Matches sim/s3_sim_serial.py's --capacity-mb default (0.4578MB): 117
// clusters * 4096 bytes/cluster = 479232 bytes (~30s of audio at
// 16000 B/s). See fat12_disk.py's own docstring for why this is sized as
// "acceptable worst-case catch-up lag", not "how long is the drive".
static const uint32_t DATA_CLUSTERS = 117;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;  // 479232

// ceil((DATA_CLUSTERS+2) * 1.5) bytes, then ceil to sectors -- matches
// fat12_disk.py's fat_bytes/fat_sectors math exactly for this cluster count.
static const uint32_t FAT_ENTRIES_NEEDED = DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = (FAT_ENTRIES_NEEDED * 3 + 1) / 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t TOTAL_SECTORS = FIRST_DATA_LBA + DATA_CLUSTERS * SECTORS_PER_CLUSTER;

static_assert(DATA_CLUSTERS + 2 < 4085, "must stay FAT12, not FAT16 -- matches fat12_disk.py's assert");

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];

// The ring itself. 468KB doesn't fit in the S3's internal DRAM alongside
// the USB/FreeRTOS/heap needs (confirmed directly: a static array here
// overflowed dram0_0_seg by ~250KB) -- allocated from PSRAM at setup()
// instead (the target board, ESP32-S3-WROOM-1 N16R8, has 8MB of it).
// `volatile` on the position counters -- written by the UART-receive
// task, read by the USB read callback (a different task context); see
// the concurrency-design comment above read_at().
static uint8_t *g_ring = nullptr;
static volatile uint32_t g_write_pos = 0;
static volatile uint32_t g_total_written = 0;
static volatile uint32_t g_last_read_offset = 0;

// Guards g_ring/g_write_pos/g_total_written between the UART-receive task
// (writer) and the USB read callback (reader). The writer can afford to
// block briefly (it's not on the USB host's timing budget); the reader
// must not (see disk_read_at()'s short-timeout take below).
static SemaphoreHandle_t g_ring_mutex;

static void pack_fat12_entries(const uint16_t *entries, uint32_t n, uint8_t *out) {
  uint32_t i = 0, oi = 0;
  while (i < n) {
    uint16_t e0 = entries[i];
    uint16_t e1 = (i + 1 < n) ? entries[i + 1] : 0;
    out[oi++] = e0 & 0xFF;
    out[oi++] = ((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4);
    out[oi++] = (e1 >> 4) & 0xFF;
    i += 2;
  }
}

static void build_boot_sector() {
  memset(g_boot_sector, 0, SECTOR_SIZE);
  uint8_t *bs = g_boot_sector;
  bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
  memcpy(bs + 3, "BOARDSIM", 8);
  bs[11] = SECTOR_SIZE & 0xFF; bs[12] = (SECTOR_SIZE >> 8) & 0xFF;
  bs[13] = SECTORS_PER_CLUSTER;
  bs[14] = RESERVED_SECTORS & 0xFF; bs[15] = (RESERVED_SECTORS >> 8) & 0xFF;
  bs[16] = NUM_FATS;
  bs[17] = ROOT_ENTRIES & 0xFF; bs[18] = (ROOT_ENTRIES >> 8) & 0xFF;
  uint32_t total16 = (TOTAL_SECTORS < 0x10000) ? TOTAL_SECTORS : 0;
  bs[19] = total16 & 0xFF; bs[20] = (total16 >> 8) & 0xFF;
  bs[21] = 0xF8;
  bs[22] = FAT_SECTORS & 0xFF; bs[23] = (FAT_SECTORS >> 8) & 0xFF;
  bs[24] = 32; bs[25] = 0;   // sectors_per_track
  bs[26] = 64; bs[27] = 0;   // num_heads
  // bytes 28-31 hidden_sectors = 0 (already zeroed)
  uint32_t total32 = (total16 == 0) ? TOTAL_SECTORS : 0;
  bs[32] = total32 & 0xFF; bs[33] = (total32 >> 8) & 0xFF;
  bs[34] = (total32 >> 16) & 0xFF; bs[35] = (total32 >> 24) & 0xFF;
  bs[36] = 0x80;
  bs[38] = 0x29;
  uint32_t serial = 0xC0FFEE00;
  bs[39] = serial & 0xFF; bs[40] = (serial >> 8) & 0xFF;
  bs[41] = (serial >> 16) & 0xFF; bs[42] = (serial >> 24) & 0xFF;
  memcpy(bs + 43, "BOARDSIM   ", 11);
  memcpy(bs + 54, "FAT12   ", 8);
  bs[510] = 0x55; bs[511] = 0xAA;
}

static void build_fat() {
  static uint16_t entries[DATA_CLUSTERS + 2];
  entries[0] = 0xFF8;
  entries[1] = 0xFFF;
  for (uint32_t c = 2; c < DATA_CLUSTERS + 2; c++) {
    entries[c] = (c < DATA_CLUSTERS + 1) ? (uint16_t)(c + 1) : 0xFFF;
  }
  memset(g_fat_sector_cache, 0, sizeof(g_fat_sector_cache));
  pack_fat12_entries(entries, DATA_CLUSTERS + 2, g_fat_sector_cache);
}

static void build_root_dir() {
  memset(g_root_dir_sector, 0, sizeof(g_root_dir_sector));
  uint8_t *entry = g_root_dir_sector;
  memcpy(entry, FILE_NAME, 11);
  entry[11] = 0x20;  // ARCHIVE
  entry[16] = 0x21; entry[17] = 0x4A;
  entry[18] = 0x21; entry[19] = 0x4A;
  // bytes 20-21 = 0 (extended attributes / high cluster word for FAT12)
  entry[24] = 0x21; entry[25] = 0x4A;
  entry[26] = 2; entry[27] = 0;  // first cluster = 2
  entry[28] = DECLARED_FILE_SIZE & 0xFF;
  entry[29] = (DECLARED_FILE_SIZE >> 8) & 0xFF;
  entry[30] = (DECLARED_FILE_SIZE >> 16) & 0xFF;
  entry[31] = (DECLARED_FILE_SIZE >> 24) & 0xFF;
}

// REAL BUG FOUND via reasoning through long-session arithmetic, not
// caught by the byte-for-byte Python cross-check (Python ints are
// arbitrary-precision, so fat12_disk.py's total_written has no analogous
// issue): letting g_total_written grow unbounded for the whole session
// would overflow this uint32_t after 2^32/16000 =~ 74.6 HOURS of
// continuous operation. Once it wraps back to a small value, the
// unread_protect gate below (`g_total_written >= DECLARED_FILE_SIZE`)
// would go false again for the ~30s it takes to re-cross that threshold
// -- silently disabling write-side backpressure right at that mark, on
// a session long enough that someone could plausibly hit it (multi-day
// continuous power, not a single drive, but real). Fix: g_total_written
// only ever needs to answer "has the ring filled at least once" (a
// boolean-like fact) plus feed disk_valid_bytes()'s already-capped
// return value -- so cap it AT DECLARED_FILE_SIZE and never grow it
// further, eliminating the overflow risk entirely regardless of session
// length. Safe: the >= comparison stays true forever once the cap is
// reached, and disk_valid_bytes() already caps at DECLARED_FILE_SIZE
// too, so nothing downstream needed the exact unbounded count.
//
// Mirrors fat12_disk.py's append(unread_protect=...). Called only from
// the UART-receive task -- safe to block briefly on the mutex here.
static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    xSemaphoreTake(g_ring_mutex, portMAX_DELAY);
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0;
    g_total_written = DECLARED_FILE_SIZE;
    xSemaphoreGive(g_ring_mutex);
    return true;
  }
  xSemaphoreTake(g_ring_mutex, portMAX_DELAY);
  uint32_t wp = g_write_pos;
  uint32_t end = wp + n;
  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    uint32_t wrapped_end = (end > DECLARED_FILE_SIZE) ? (end - DECLARED_FILE_SIZE) : end;
    bool unsafe;
    if (end <= DECLARED_FILE_SIZE) {
      unsafe = (wp <= g_last_read_offset && g_last_read_offset < end);
    } else {
      unsafe = (g_last_read_offset >= wp || g_last_read_offset < wrapped_end);
    }
    if (unsafe) {
      xSemaphoreGive(g_ring_mutex);
      return false;
    }
  }
  if (end <= DECLARED_FILE_SIZE) {
    memcpy(g_ring + wp, data, n);
  } else {
    uint32_t first_part = DECLARED_FILE_SIZE - wp;
    memcpy(g_ring + wp, data, first_part);
    memcpy(g_ring, data + first_part, end - DECLARED_FILE_SIZE);
  }
  g_write_pos = end % DECLARED_FILE_SIZE;
  // Must not even attempt the += once already at the cap -- caught this
  // directly (2026-09-17): g_total_written + n can ITSELF overflow if
  // g_total_written is already huge, producing a small wrapped value
  // before min() ever sees it, silently defeating the whole point of
  // capping. Only grow while still below the cap; once capped, leave it
  // exactly there forever.
  if (g_total_written < DECLARED_FILE_SIZE) {
    g_total_written = min(g_total_written + n, DECLARED_FILE_SIZE);
  }
  xSemaphoreGive(g_ring_mutex);
  return true;
}

static inline uint32_t disk_valid_bytes() {
  return g_total_written;  // already capped at DECLARED_FILE_SIZE by disk_append()
}

// Concurrency design note (real-hardware-only concern, doesn't exist in
// the Python prototype which was single-threaded per-connection): unlike
// s3_sim_serial.py's read_sectors(), which can safely BLOCK/retry when a
// read would straddle the live write edge (see its avoid_straddle
// docstring for the real glitch this prevents), this callback runs on
// TinyUSB's task and blocking it risks a USB host-side timeout -- there
// is no safe unbounded wait available here. Fix: take g_ring_mutex with a
// SHORT bounded timeout; if the range would straddle the write edge
// (same margin check as the Python original) OR the mutex isn't free
// immediately, serve zero-fill for the affected bytes instead of
// blocking -- functionally identical to "not encoded yet" from the
// reader's perspective, at worst a brief silent gap instead of an
// audible splice. This has NOT been validated on real hardware for
// how often it actually triggers under real USB read timing -- may need
// tuning (e.g. the margin below) once real behavior is observable.
//
// A theoretically better primitive exists and was already identified by
// this project's own pre-porting research (progress/RESEARCH_BT_TO_USB_MSC.md
// §2, written before any S3 code existed): real TinyUSB supports
// TUD_MSC_RET_BUSY/TUD_MSC_RET_ASYNC + tud_msc_async_io_done(), letting a
// read say "not ready, I'll signal you" instead of committing to
// zero-fill immediately -- closer to the Python prototype's retry
// behavior, without blocking. **Deliberately not used here**: checked
// directly (2026-09-17) and the Arduino `USBMSC` class's own
// `tud_msc_read10_cb` glue (core's USBMSC.cpp) is a plain synchronous
// passthrough with no BUSY/ASYNC support at all -- using the real async
// primitive would mean bypassing USBMSC entirely and hand-writing the
// raw TinyUSB integration, a much bigger, harder-to-get-right, and
// currently completely untestable (no hardware) undertaking. That
// research also flagged a real maintainer-filed TinyUSB bug in this
// exact BUSY/pending-read path (hathach/tinyusb#2035, open ~2 years,
// closed Aug 2025) -- whether the bundled TinyUSB version here even has
// that fix hasn't been checked. If real-hardware testing shows the
// zero-fill gaps below are audibly bothersome, THIS is the documented
// next lever to pull -- not a reason to have guessed at it blind
// tonight.
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;  // ~0.5s headroom

// Fills `buffer[0..len)` from the FAT12 volume starting at absolute byte
// offset `abs_pos` (0-based from the start of the whole volume, i.e.
// abs_pos = lba*SECTOR_SIZE + offset). Handles a request spanning
// multiple regions (boot/FAT/root-dir/data) or wrapping past the ring's
// end by looping byte-range-wise -- more general than fat12_disk.py's
// per-sector loop, since TinyUSB's actual read chunking granularity
// isn't known without real hardware to observe it against.
static void disk_read_at(uint32_t abs_pos, uint8_t *buffer, uint32_t len) {
  memset(buffer, 0, len);  // safe default for any region below that doesn't fill it
  uint32_t pos = abs_pos;
  uint32_t remaining = len;
  uint32_t out_off = 0;

  const uint32_t fat_region_start = RESERVED_SECTORS * SECTOR_SIZE;
  const uint32_t fat_region_end = fat_region_start + NUM_FATS * FAT_SECTORS * SECTOR_SIZE;
  const uint32_t root_region_end = fat_region_end + ROOT_DIR_SECTORS * SECTOR_SIZE;
  const uint32_t data_region_start = FIRST_DATA_LBA * SECTOR_SIZE;

  while (remaining > 0) {
    if (pos < fat_region_start) {
      uint32_t n = min(remaining, fat_region_start - pos);
      memcpy(buffer + out_off, g_boot_sector + pos, n);
      pos += n; out_off += n; remaining -= n;
    } else if (pos < fat_region_end) {
      uint32_t rel = (pos - fat_region_start) % (FAT_SECTORS * SECTOR_SIZE);
      uint32_t n = min(remaining, FAT_SECTORS * SECTOR_SIZE - rel);
      memcpy(buffer + out_off, g_fat_sector_cache + rel, n);
      pos += n; out_off += n; remaining -= n;
    } else if (pos < root_region_end) {
      uint32_t rel = pos - fat_region_end;
      uint32_t n = min(remaining, ROOT_DIR_SECTORS * SECTOR_SIZE - rel);
      memcpy(buffer + out_off, g_root_dir_sector + rel, n);
      pos += n; out_off += n; remaining -= n;
    } else {
      uint32_t data_off = pos - data_region_start;
      if (data_off >= DECLARED_FILE_SIZE) {
        // Past the end of the declared file -- shouldn't normally be
        // requested, but stay safe: nothing more to serve.
        break;
      }
      uint32_t n = min(remaining, DECLARED_FILE_SIZE - data_off);

      bool took_lock = xSemaphoreTake(g_ring_mutex, pdMS_TO_TICKS(2)) == pdTRUE;
      uint32_t avail = disk_valid_bytes();
      uint32_t wp = g_write_pos;
      uint32_t unsafe_end = data_off + n + READ_MARGIN_BYTES;
      bool straddling;
      if (unsafe_end <= DECLARED_FILE_SIZE) {
        straddling = (data_off <= wp && wp < unsafe_end);
      } else {
        straddling = (wp >= data_off || wp < unsafe_end - DECLARED_FILE_SIZE);
      }
      if (took_lock && !straddling) {
        // Matches fat12_disk.py's read_sectors() exactly: last_read_offset
        // advances for every data-region byte range actually served,
        // whether or not real content existed there yet -- legitimate
        // "not encoded yet" zero-fill is not a race and still means the
        // reader genuinely moved past this position. Only skip the
        // advance when straddling (unsafe -- see docstring above) or the
        // lock wasn't free, neither of which happens in Python's normal
        // (non-forced) path either.
        if (data_off < avail) {
          uint32_t real_n = min(n, avail - data_off);
          memcpy(buffer + out_off, g_ring + data_off, real_n);
          // leave [real_n, n) as the zero-fill from the initial memset.
        }
        g_last_read_offset = (data_off + n) % DECLARED_FILE_SIZE;
      }
      // else: straddling, or lock unavailable -- zero
      // stays (already memset), no torn/spliced content possible.
      if (took_lock) xSemaphoreGive(g_ring_mutex);

      pos += n; out_off += n; remaining -= n;
    }
  }
}

// ===================== USBMSC glue ==========================================

USBMSC MSC;

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t abs_pos = lba * SECTOR_SIZE + offset;
  disk_read_at(abs_pos, (uint8_t *)buffer, bufsize);
  return bufsize;
}

static int32_t msc_on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  // Read-only volume from the host's perspective -- a real car radio never
  // writes to the file it's playing. Silently accept-and-discard rather
  // than reject, since some hosts probe writability at mount time and a
  // hard failure there could be more disruptive than a no-op write.
  (void)lba; (void)offset; (void)buffer;
  return bufsize;
}

static bool msc_on_start_stop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition; (void)start; (void)load_eject;
  return true;
}

// ===================== UART link to the classic ESP32 ======================

// Mirrors s3_sim_serial.py's find_sync()/receive_from_esp32() framing
// exactly: 1 magic byte (0xAA), 1 type byte ('A' or 'C'), 4-byte
// big-endian length, then payload. Runs as its own FreeRTOS task so it
// can block on UART reads without affecting USB responsiveness.
static const uint32_t WRITE_RETRY_DELAY_MS = 5;
static const uint32_t MAX_WRITE_RETRIES = 200;  // ~1s total, matches s3_sim_serial.py

static bool read_exact(uint8_t *buf, uint32_t n) {
  uint32_t got = 0;
  while (got < n) {
    int avail = LinkSerial.available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int r = LinkSerial.read(buf + got, min((uint32_t)avail, n - got));
    if (r > 0) got += r;
  }
  return true;
}

static void find_sync() {
  uint8_t b;
  while (true) {
    read_exact(&b, 1);
    if (b == FRAME_MAGIC) return;
    // else: resync, exactly like s3_sim_serial.py's find_sync() -- no
    // need to log discarded junk on real hardware (no stderr to see it).
  }
}

static void link_task(void *) {
  static uint8_t payload[MAX_FRAME_LEN];
  while (true) {
    find_sync();
    uint8_t header[5];
    read_exact(header, 5);
    char frame_type = (char)header[0];
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                       ((uint32_t)header[3] << 8) | (uint32_t)header[4];

    if ((frame_type != 'A' && frame_type != 'C') || length > MAX_FRAME_LEN) {
      continue;  // bad header -- rescan from find_sync(), same as the Python side
    }
    if (length) read_exact(payload, length);

    if (frame_type == 'A') {
      bool ok = false;
      for (uint32_t i = 0; i < MAX_WRITE_RETRIES; i++) {
        if (disk_append(payload, length, true)) { ok = true; break; }
        delay(WRITE_RETRY_DELAY_MS);
      }
      if (!ok) disk_append(payload, length, false);  // force through, same fallback as s3_sim_serial.py
    }
    // 'C' (control/diagnostic messages from the classic ESP32, e.g.
    // BT_CONNECTED/ENCODE_US/etc.) -- nothing on this side needs to act
    // on them; they existed purely for the PC-side simulator's own
    // logging. Silently discarded here.
  }
}

// ===================== setup/loop ===========================================

static void usb_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg; (void)event_data;
  if (event_base == ARDUINO_USB_EVENTS) {
    switch (event_id) {
      case ARDUINO_USB_STARTED_EVENT: Serial.println("[s3] USB PLUGGED"); break;
      case ARDUINO_USB_STOPPED_EVENT: Serial.println("[s3] USB UNPLUGGED"); break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);  // USB CDC debug console (separate from the UART link)
  delay(200);
  Serial.println("[s3] booting");

  build_boot_sector();
  build_fat();
  build_root_dir();

  g_ring_mutex = xSemaphoreCreateMutex();
  g_ring = (uint8_t *)heap_caps_malloc(DECLARED_FILE_SIZE, MALLOC_CAP_SPIRAM);
  if (!g_ring) {
    Serial.println("[s3] FATAL: PSRAM allocation for ring buffer failed -- "
                    "is PSRAM enabled in board settings?");
    while (true) delay(1000);
  }
  memset(g_ring, 0, DECLARED_FILE_SIZE);
  // Prime the ring with pre-encoded silence up front -- same fix as
  // fat12_disk.py's __init__, same rationale (see CLAUDE.md/STATUS.md):
  // gets the radio's decoder real bytes to engage with immediately on
  // mount instead of waiting for real audio to physically accumulate.
  disk_append(SILENCE_PRIMER, SILENCE_PRIMER_LEN, false);

  // Default HardwareSerial RX buffer (256B) is far too small at 921600
  // baud for ~100-180 audio-frame chunks/sec from the classic ESP32 --
  // must be set before begin(). 8KB gives real headroom (two full
  // MAX_FRAME_LEN frames) against link_task briefly falling behind
  // during a disk_append() retry stall.
  LinkSerial.setRxBufferSize(8192);
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, UART_S3_RX_PIN, UART_S3_TX_PIN);

  // REAL BUG FOUND (fresh adversarial review, 2026-09-17): link_task used
  // to be created AFTER MSC.begin()/USB.begin() below -- a real boot-time
  // data-loss window. The classic ESP32 starts transmitting continuous
  // silence-or-real-audio frames over UART from its own boot, independent
  // of BT state (feed_silence_if_no_real_audio()). If USB enumeration
  // (a real, variable-length handshake) takes longer than it takes to
  // fill the 8KB UART RX buffer at 921600 baud, incoming bytes get
  // silently dropped before link_task ever starts draining them.
  // Self-limiting (find_sync()'s resync logic already handles the
  // resulting misalignment gracefully, confirmed in the UART-framing
  // crosscheck), but still real, avoidable data loss on every boot.
  // Fix: start draining the UART immediately after LinkSerial.begin(),
  // before the slower USB enumeration work below.
  xTaskCreatePinnedToCore(link_task, "link_task", 8192, nullptr, 2, nullptr, 1);

  USB.onEvent(usb_event_callback);
  MSC.vendorID("PHANTOMD");     // max 8 chars
  MSC.productID("USB_MSC");     // max 16 chars
  MSC.productRevision("1.0");   // max 4 chars
  MSC.onStartStop(msc_on_start_stop);
  MSC.onRead(msc_on_read);
  MSC.onWrite(msc_on_write);
  MSC.mediaPresent(true);
  MSC.isWritable(false);
  MSC.begin(TOTAL_SECTORS, SECTOR_SIZE);
  USB.begin();

  Serial.printf("[s3] FAT12 volume: %lu sectors, declared file size %lu bytes, "
                "first data LBA %lu\n",
                (unsigned long)TOTAL_SECTORS, (unsigned long)DECLARED_FILE_SIZE,
                (unsigned long)FIRST_DATA_LBA);
  Serial.println("[s3] ready");
}

void loop() {
  delay(1000);  // all real work happens in link_task + USBMSC callbacks
}

#endif  // ARDUINO_USB_MODE
