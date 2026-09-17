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
#else
  // ---- PC host (sim/s3_real_firmware_host.cpp) ----
  #include <mutex>
  #include <chrono>
  #include <algorithm>
  using std::min;
  #define FATDISK_MUTEX_T std::timed_mutex*
  #define FATDISK_MUTEX_CREATE() (new std::timed_mutex())
  #define FATDISK_MUTEX_TAKE_BLOCKING(m) (m)->lock()
  #define FATDISK_MUTEX_TRY_TAKE_MS(m, ms) (m)->try_lock_for(std::chrono::milliseconds(ms))
  #define FATDISK_MUTEX_GIVE(m) (m)->unlock()
#endif

// ===================== Wire protocol (must match esp32-bt-mp3-test.ino) ====

static const uint8_t FRAME_MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;
static const uint32_t WRITE_RETRY_DELAY_MS = 5;
static const uint32_t MAX_WRITE_RETRIES = 200;  // ~1s total

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
  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    uint32_t wrapped_end = (end > DECLARED_FILE_SIZE) ? (end - DECLARED_FILE_SIZE) : end;
    bool unsafe;
    if (end <= DECLARED_FILE_SIZE) {
      unsafe = (wp <= g_last_read_offset && g_last_read_offset < end);
    } else {
      unsafe = (g_last_read_offset >= wp || g_last_read_offset < wrapped_end);
    }
    if (unsafe) {
      FATDISK_MUTEX_GIVE(g_ring_mutex);
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
