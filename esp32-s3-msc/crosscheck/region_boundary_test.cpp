// Tests disk_read_at()'s region-crossing loop with genuinely non-sector-
// aligned, cross-region byte ranges -- a defensive code path that real
// TinyUSB calls likely never actually exercise (bufsize is normally a
// fixed 512B, sector-aligned), but the function was written to handle
// arbitrary ranges generically in case that assumption turns out wrong
// on real hardware. Verifies it's actually correct, not just "should be."
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
using namespace std;

static const uint32_t SECTOR_SIZE = 512;
static const uint32_t SECTORS_PER_CLUSTER = 8;
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const uint32_t DATA_CLUSTERS = 117;
static const uint32_t FAT_ENTRIES_NEEDED = DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = (FAT_ENTRIES_NEEDED * 3 + 1) / 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * SECTORS_PER_CLUSTER * SECTOR_SIZE;

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];
static uint8_t g_ring[DECLARED_FILE_SIZE];
static uint32_t g_write_pos = 0, g_total_written = DECLARED_FILE_SIZE;  // pretend ring already full of known content
static uint32_t g_last_read_offset = 0;

// Exact copy of disk_read_at() from the real .ino, mutex/margin logic
// stripped to a trivial always-safe stub since concurrency isn't what's
// being tested here (already covered elsewhere) -- only the region-
// crossing byte-range loop itself.
static void disk_read_at(uint32_t abs_pos, uint8_t *buffer, uint32_t len) {
  memset(buffer, 0, len);
  uint32_t pos = abs_pos, remaining = len, out_off = 0;
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
      memcpy(buffer + out_off, g_ring + data_off, n);
      pos += n; out_off += n; remaining -= n;
    }
  }
}

int main() {
  // Distinct fill patterns per region so a cross-region read's boundary
  // is trivially visible/verifiable byte-by-byte.
  memset(g_boot_sector, 0xAA, sizeof(g_boot_sector));
  memset(g_fat_sector_cache, 0xBB, sizeof(g_fat_sector_cache));
  memset(g_root_dir_sector, 0xCC, sizeof(g_root_dir_sector));
  memset(g_ring, 0xDD, sizeof(g_ring));

  printf("region boundaries: boot=[0,%u) fat=[%u,%u) root=[%u,%u) data=[%u,...)\n",
         RESERVED_SECTORS * SECTOR_SIZE,
         RESERVED_SECTORS * SECTOR_SIZE, (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS) * SECTOR_SIZE,
         (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS) * SECTOR_SIZE,
         FIRST_DATA_LBA * SECTOR_SIZE, FIRST_DATA_LBA * SECTOR_SIZE);

  int failures = 0;

  // Test 1: read straddling boot-sector -> FAT boundary (byte 500..600, boundary at 512)
  {
    uint8_t buf[100];
    disk_read_at(500, buf, 100);
    bool ok = true;
    for (int i = 0; i < 12; i++) if (buf[i] != 0xAA) ok = false;   // bytes 500-511 = boot
    for (int i = 12; i < 100; i++) if (buf[i] != 0xBB) ok = false; // bytes 512-599 = fat
    printf("Test 1 (boot->fat straddle): %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  }

  // Test 2: read straddling FAT copy1 -> FAT copy2 (both map to the same
  // cached sector via modulo -- should be entirely 0xBB, no visible seam)
  {
    uint32_t fat_copy_boundary = (RESERVED_SECTORS + FAT_SECTORS) * SECTOR_SIZE;
    uint8_t buf[64];
    disk_read_at(fat_copy_boundary - 32, buf, 64);
    bool ok = true;
    for (int i = 0; i < 64; i++) if (buf[i] != 0xBB) ok = false;
    printf("Test 2 (fat copy1->copy2 straddle): %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  }

  // Test 3: read straddling root-dir -> data region boundary
  {
    uint32_t root_data_boundary = FIRST_DATA_LBA * SECTOR_SIZE;
    uint8_t buf[80];
    disk_read_at(root_data_boundary - 40, buf, 80);
    bool ok = true;
    for (int i = 0; i < 40; i++) if (buf[i] != 0xCC) ok = false;  // root
    for (int i = 40; i < 80; i++) if (buf[i] != 0xDD) ok = false; // data
    printf("Test 3 (root->data straddle): %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  }

  // Test 4: a single read spanning boot -> fat -> root -> data, all four
  // regions in one call (extreme case, would need a huge bufsize on real
  // hardware, but the loop should still be correct)
  {
    uint32_t total_len = (FIRST_DATA_LBA * SECTOR_SIZE) + 20 - 100;
    uint8_t *buf = new uint8_t[total_len];
    disk_read_at(100, buf, total_len);
    bool ok = true;
    uint32_t idx = 0;
    for (uint32_t p = 100; p < RESERVED_SECTORS * SECTOR_SIZE; p++, idx++)
      if (buf[idx] != 0xAA) ok = false;
    for (uint32_t p = RESERVED_SECTORS * SECTOR_SIZE; p < (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS) * SECTOR_SIZE; p++, idx++)
      if (buf[idx] != 0xBB) ok = false;
    for (uint32_t p = (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS) * SECTOR_SIZE; p < FIRST_DATA_LBA * SECTOR_SIZE; p++, idx++)
      if (buf[idx] != 0xCC) ok = false;
    for (uint32_t p = FIRST_DATA_LBA * SECTOR_SIZE; p < FIRST_DATA_LBA * SECTOR_SIZE + 20; p++, idx++)
      if (buf[idx] != 0xDD) ok = false;
    printf("Test 4 (all four regions in one call): %s (checked %u bytes)\n", ok ? "PASS" : "FAIL", idx);
    if (!ok) failures++;
    delete[] buf;
  }

  printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  return failures;
}
