// Shared core: the EXACT same FAT12 ring-buffer disk logic and UART wire
// protocol constants, compiled into BOTH the real esp32-s3-msc.ino
// (Arduino/ESP32-S3 target) and sim/s3_real_firmware_host.cpp (a PC-hosted
// program standing in for the physical S3 board until it arrives). This
// is a single source of truth -- not two hand-copied parallel
// implementations that could silently drift out of sync with each other
// (exactly the kind of bug already found and fixed elsewhere tonight,
// where a fix applied to one code path wasn't propagated to another).
//
// Platform differences are isolated to a small set of macros defined
// right below, based on whether ARDUINO is defined. Everything past that
// point is byte-for-byte identical C++ on both platforms.
#ifndef FAT_DISK_SHARED_H
#define FAT_DISK_SHARED_H

#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
  // ---- ESP32-S3 / FreeRTOS ----
  #define FATDISK_MUTEX_T SemaphoreHandle_t
  #define FATDISK_MUTEX_CREATE() xSemaphoreCreateMutex()
  #define FATDISK_MUTEX_TAKE_BLOCKING(m) xSemaphoreTake(m, portMAX_DELAY)
  #define FATDISK_MUTEX_TRY_TAKE_MS(m, ms) (xSemaphoreTake((m), pdMS_TO_TICKS(ms)) == pdTRUE)
  #define FATDISK_MUTEX_GIVE(m) xSemaphoreGive(m)
  #define FATDISK_RANDOM32() esp_random()
#else
  // ---- PC host (sim/s3_real_firmware_host.cpp) ----
  #include <mutex>
  #include <chrono>
  #include <algorithm>
  #include <random>
  using std::min;
  #define FATDISK_MUTEX_T std::timed_mutex*
  #define FATDISK_MUTEX_CREATE() (new std::timed_mutex())
  #define FATDISK_MUTEX_TAKE_BLOCKING(m) (m)->lock()
  #define FATDISK_MUTEX_TRY_TAKE_MS(m, ms) (m)->try_lock_for(std::chrono::milliseconds(ms))
  static inline uint32_t fatdisk_random32_host() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;
    return dist(gen);
  }
  #define FATDISK_RANDOM32() fatdisk_random32_host()
  #define FATDISK_MUTEX_GIVE(m) (m)->unlock()
#endif

// ===================== Wire protocol (must match esp32-bt-mp3-test.ino) ====

static const uint8_t FRAME_MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;
static const uint32_t WRITE_RETRY_DELAY_MS = 5;
static const uint32_t MAX_WRITE_RETRIES = 200;  // ~1s total

// ===================== FAT12 ring-buffer disk (port of fat12_disk.py) ======

static const uint32_t SECTOR_SIZE = 512;

// OPEN TUNING QUESTION, deliberately left as-is pending real S3 hardware
// (2026-09-17): a real car-radio test found FAT12 initially failed
// (garbled playback) on a physical test drive using this same 8-sector
// (4KB) cluster size, and succeeded once switched to 64-sector (32KB)
// clusters -- among several other simultaneously-changed variables (see
// progress/STATUS.md's "MAJOR: FAT12 confirmed compatible" entry), so
// root cause isn't isolated. Deliberately NOT copying the 32KB value
// here: at this ring's tiny size (~200KB), 32KB clusters would mean only
// ~6-7 total data clusters, ballooning READ_MARGIN_BYTES (2 clusters)
// from ~4% of the ring to ~29% -- a real, known-bad ratio matching this
// project's own earlier Python-prototype history of frequent glitches
// from an oversized margin-to-ring fraction. Trading a real, understood
// glitch risk for an unproven compatibility fix isn't worth it blind.
// TEST ONCE THE REAL S3 BOARD EXISTS: if FAT12 still misbehaves on the
// real radio with the real firmware at this cluster size, that's the
// moment to try increasing it here (and re-tuning READ_MARGIN_BYTES
// alongside it, not independently) -- not before, since only the real
// hardware can show whether cluster count was ever actually the cause.
static const uint32_t SECTORS_PER_CLUSTER = 8;  // 4KB clusters
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const char FILE_NAME[12] = "STREAM  MP3";  // 8.3, space-padded (11 bytes + NUL)

// REDUCED from 117 clusters (~30s) to 59 (~15.1s) on 2026-09-17 after a
// real phone test: the car radio reader always starts reading from file
// position 0 (cluster 2), and since the ring is a continuously-
// overwritten circular buffer, whatever's CURRENTLY at position 0 can be
// up to a FULL RING DURATION stale -- exactly the mechanism
// s3_sim_serial.py's own --capacity-mb help text already documented
// ("the reader's wrap-to-start cycle... can replay content from up to
// the FULL ring duration ago"). With a fresh/idle ring (silence-injected
// since boot) sitting at position 0, this meant up to ~30s of stale
// silence had to be read through before the reader's traversal caught up
// to wherever real audio was actually being written -- a real,
// measured, user-reported ~30s time-to-first-real-audio delay, not a
// regression from any one pipeline. Halving ring duration halves this
// worst-case bound. See fat12_disk.py's own docstring for why this is
// sized as "acceptable worst-case catch-up lag", not "how long is the
// drive".
//
// DOUBLED from 50 to 100 clusters (2026-09-18, first live test against
// real hardware + real car_sim.py): READ_MARGIN_BYTES (below) is a fixed
// absolute size, so at 50 clusters it was ~4% of the ring -- confirmed
// live via the real passthrough's own read counter, 8/249 reads (~3.2%)
// came back straddle-protected zero-fill, each one enough to desync a
// strict low-buffer MP3 decoder (car_sim.py's ffmpeg config) for a
// while, producing persistent "Header missing" errors even after two
// separate, confirmed, unrelated bugs were found and fixed (a ring-wrap
// frame-splitting bug, and an ID3-tagged silence primer). Doubling the
// ring halves the margin's fraction of it (~4% -> ~2%) without touching
// the margin itself, directly cutting straddle frequency, at the real,
// accepted cost of roughly doubling the worst-case stale-replay window
// if the source pauses (~12.8s -> ~25.6s) -- a bounded, known tradeoff,
// not a new risk. 100 clusters * 4096 bytes/cluster = 409600 bytes
// (~25.6s at 16000 B/s).
static const uint32_t DATA_CLUSTERS = 100;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;  // 409600

static const uint32_t FAT_ENTRIES_NEEDED = DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = (FAT_ENTRIES_NEEDED * 3 + 1) / 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t TOTAL_SECTORS = FIRST_DATA_LBA + DATA_CLUSTERS * SECTORS_PER_CLUSTER;
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;  // ~0.5s headroom

static_assert(DATA_CLUSTERS + 2 < 4085, "must stay FAT12, not FAT16 -- matches fat12_disk.py's assert");

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];

// The ring itself. On the real S3, 468KB doesn't fit in internal DRAM
// alongside USB/FreeRTOS/heap needs (confirmed directly: a static array
// overflowed dram0_0_seg by ~250KB) -- allocated from PSRAM at setup()
// instead. On the PC host, allocated from plain heap (see
// s3_real_firmware_host.cpp's own resource-monitoring code for why this
// specific allocation size is the one apples-to-apples number worth
// comparing against the real S3's PSRAM budget).
static uint8_t *g_ring = nullptr;
static volatile uint32_t g_write_pos = 0;
static volatile uint32_t g_total_written = 0;
static volatile uint32_t g_last_read_offset = 0;

// TEMP DIAGNOSTIC (2026-09-18): investigating a real bug where content at a
// fixed ring position stays byte-identical across multiple full laps even
// though g_write_pos itself is confirmed advancing normally and the
// classic ESP32's own UART source data is confirmed 100% unique (zero
// duplicate frames over 1500+ samples) -- meaning the freeze is somewhere
// in THIS write->read path specifically (possibly PSRAM cross-core cache
// coherency between link_task on core 1 and the USB read callback, or a
// logic bug). These record a lightweight rolling hash of whatever's
// actually written/read whenever the position falls inside a fixed
// DIAG_TARGET_POS window, from the firmware's own perspective (avoids any
// ambiguity from an external USB/OS-level read path). Read via the
// existing loop() heartbeat print. Remove once root-caused.
// FIXED (2026-09-18, same session): the original DIAG_TARGET_WINDOW=512
// full-containment check could never fire on the write side -- real
// disk_append() chunks are only ~416-420 bytes (Shine's own encoded chunk
// size), always smaller than a 512-byte window, so "fully contains the
// window" was mathematically impossible and g_diag_write_count stayed 0
// for 90+ seconds across multiple full ring laps. Switched to tracking a
// single fixed byte instead of a window -- trivially satisfiable by any
// write/read that merely covers that one byte, which happens on every lap
// regardless of chunk size.
#define DIAG_TARGET_POS 100000
static volatile uint8_t g_diag_write_byte = 0;
static volatile uint32_t g_diag_write_count = 0;
static volatile uint8_t g_diag_read_byte = 0;
static volatile uint32_t g_diag_read_count = 0;
static inline uint32_t diag_hash(const uint8_t *p, uint32_t n) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// Guards g_ring/g_write_pos/g_total_written between the writer (UART
// receive) and the reader (USB read callback on real hardware; the TCP
// SCSI-READ10 handler on the PC host). The writer can afford to block
// briefly; the reader must not (see disk_read_at()'s short-timeout take).
static FATDISK_MUTEX_T g_ring_mutex;

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
  // Random per boot (2026-09-17): a real car radio confirmed to sometimes
  // cache "resume playback position" keyed on volume serial + filename --
  // a FIXED serial here would mean every boot presents an IDENTICAL
  // identity to the radio despite the ring's actual content being
  // completely different each time, risking the radio resuming into the
  // middle of unrelated content. Found via a real test that showed
  // exactly this symptom (playback starting mid-file) before other
  // factors were also changed; this fix has no downside regardless of
  // whether it was the actual cause.
  uint32_t serial = FATDISK_RANDOM32();
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
  entry[24] = 0x21; entry[25] = 0x4A;
  entry[26] = 2; entry[27] = 0;  // first cluster = 2
  entry[28] = DECLARED_FILE_SIZE & 0xFF;
  entry[29] = (DECLARED_FILE_SIZE >> 8) & 0xFF;
  entry[30] = (DECLARED_FILE_SIZE >> 16) & 0xFF;
  entry[31] = (DECLARED_FILE_SIZE >> 24) & 0xFF;
}

// See esp32-s3-msc.ino's git history (2026-09-17) for the real uint32_t
// overflow bug this capping behavior fixes, and the second bug (the
// capping addition itself overflowing) caught in the first fix attempt.
static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    FATDISK_MUTEX_TAKE_BLOCKING(g_ring_mutex);
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0;
    g_total_written = DECLARED_FILE_SIZE;
    FATDISK_MUTEX_GIVE(g_ring_mutex);
    return true;
  }
  FATDISK_MUTEX_TAKE_BLOCKING(g_ring_mutex);
  uint32_t wp = g_write_pos;
  uint32_t end = wp + n;

  // REAL BUG FOUND (2026-09-18, first live test against real hardware,
  // real car_sim.py, real S3): a normal wrap here used to byte-split a
  // chunk's data across the ring's physical end/start boundary with zero
  // awareness of where MP3 frame boundaries fall inside it. Each disk_append()
  // call's payload is already a whole number of complete Shine-encoded MP3
  // frames (esp32-bt-mp3-test.ino's send_framed('A', ...) never sends a
  // partial frame), so a byte-level split always corrupts whichever frame
  // straddles that exact boundary -- and since the wrap boundary IS byte 0,
  // and car_sim.py (matching a real dumb head unit) always starts reading
  // from byte 0 on every single pass, that ONE corrupted frame lands
  // exactly where every playback attempt begins. Confirmed: persistent
  // "Header missing" decoder errors on literally every read from byte 0,
  // not an intermittent timing issue (a real retry-based fix was tried and
  // reverted -- it didn't change the error rate at all, which is what
  // pointed here instead). Fixed by never splitting a chunk across the
  // wrap: if this append would cross the boundary, skip straight to
  // position 0 and write the WHOLE chunk there as one unbroken unit,
  // leaving whatever's between the old write_pos and DECLARED_FILE_SIZE
  // untouched (a real, complete, still-valid frame from one lap earlier)
  // instead of a byte-split, invalid one. Safe on the writer side (unlike
  // the read path) since this thread is allowed to do real work.
  if (end > DECLARED_FILE_SIZE) {
    wp = 0;
    end = n;
  }

  // TEMP DIAGNOSTIC -- see g_diag_* above. If this write touches the
  // target window, hash what's about to be memcpy'd there.
  if (wp <= DIAG_TARGET_POS && DIAG_TARGET_POS < end) {
    g_diag_write_byte = data[DIAG_TARGET_POS - wp];
    g_diag_write_count++;
  }

  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    bool unsafe = (wp <= g_last_read_offset && g_last_read_offset < end);
    if (unsafe) {
      FATDISK_MUTEX_GIVE(g_ring_mutex);
      return false;
    }
  }
  memcpy(g_ring + wp, data, n);
  g_write_pos = end;
  if (g_total_written < DECLARED_FILE_SIZE) {
    g_total_written = min(g_total_written + n, DECLARED_FILE_SIZE);
  }
  FATDISK_MUTEX_GIVE(g_ring_mutex);
  return true;
}

static inline uint32_t disk_valid_bytes() {
  return g_total_written;  // already capped at DECLARED_FILE_SIZE by disk_append()
}

// See esp32-s3-msc.ino's own comment (2026-09-17) for the full concurrency
// design rationale: on real hardware this callback must never block
// (TinyUSB's onRead has no safe unbounded wait), so it takes the mutex
// with a short bounded timeout and serves zero-fill on straddle/lock-miss
// instead of retrying. The PC host uses the exact same non-blocking
// design here even though it COULD safely retry (unlike real hardware) --
// deliberately, since the whole point of running this shared code on the
// PC is to observe how the REAL firmware's actual behavior performs, not
// a more lenient PC-only variant.
// out_straddled/out_lock_missed (optional, both default nullptr so the
// real .ino's existing call site is unaffected): diagnostic-only signal
// for the PC host's resource/behavior reporting -- lets it observe how
// often the real firmware's actual non-blocking/zero-fill design would
// trigger against a real live feed, without duplicating this straddle
// logic in a second, hand-copied place just to log it.
static void disk_read_at(uint32_t abs_pos, uint8_t *buffer, uint32_t len,
                          bool *out_straddled = nullptr, bool *out_lock_missed = nullptr) {
  memset(buffer, 0, len);
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
      if (data_off >= DECLARED_FILE_SIZE) break;
      uint32_t n = min(remaining, DECLARED_FILE_SIZE - data_off);

      bool took_lock = FATDISK_MUTEX_TRY_TAKE_MS(g_ring_mutex, 2);
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
        if (data_off < avail) {
          uint32_t real_n = min(n, avail - data_off);
          memcpy(buffer + out_off, g_ring + data_off, real_n);
          // TEMP DIAGNOSTIC -- see g_diag_* above. Records the byte actually
          // copied out if this read covers the target position.
          if (data_off <= DIAG_TARGET_POS && DIAG_TARGET_POS < data_off + real_n) {
            g_diag_read_byte = g_ring[DIAG_TARGET_POS];
            g_diag_read_count++;
          }
        }
        g_last_read_offset = (data_off + n) % DECLARED_FILE_SIZE;
      } else {
        if (out_straddled && straddling) *out_straddled = true;
        if (out_lock_missed && !took_lock) *out_lock_missed = true;
      }
      if (took_lock) FATDISK_MUTEX_GIVE(g_ring_mutex);

      pos += n; out_off += n; remaining -= n;
    }
  }
}

#endif  // FAT_DISK_SHARED_H
