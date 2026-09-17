// Standalone host program: extracts the exact same boot-sector/FAT/root-dir
// construction logic from esp32-s3-msc.ino (copy-pasted verbatim, not
// reimplemented) and dumps the three structures as raw bytes to stdout, so
// they can be diffed byte-for-byte against fat12_disk.py's own output for
// the same parameters. Pure logic, no hardware/Arduino dependency -- can
// run and verify tonight even with no ESP32-S3 board to test on.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

static const uint32_t SECTOR_SIZE = 512;
static const uint32_t SECTORS_PER_CLUSTER = 8;
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const char FILE_NAME[12] = "STREAM  MP3";
static const uint32_t DATA_CLUSTERS = 117;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;
static const uint32_t FAT_ENTRIES_NEEDED = DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = (FAT_ENTRIES_NEEDED * 3 + 1) / 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t TOTAL_SECTORS = FIRST_DATA_LBA + DATA_CLUSTERS * SECTORS_PER_CLUSTER;

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];

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
  bs[24] = 32; bs[25] = 0;
  bs[26] = 64; bs[27] = 0;
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
  entry[11] = 0x20;
  entry[16] = 0x21; entry[17] = 0x4A;
  entry[18] = 0x21; entry[19] = 0x4A;
  entry[24] = 0x21; entry[25] = 0x4A;
  entry[26] = 2; entry[27] = 0;
  entry[28] = DECLARED_FILE_SIZE & 0xFF;
  entry[29] = (DECLARED_FILE_SIZE >> 8) & 0xFF;
  entry[30] = (DECLARED_FILE_SIZE >> 16) & 0xFF;
  entry[31] = (DECLARED_FILE_SIZE >> 24) & 0xFF;
}

int main() {
  fprintf(stderr, "TOTAL_SECTORS=%u FIRST_DATA_LBA=%u FAT_SECTORS=%u "
                   "ROOT_DIR_SECTORS=%u DECLARED_FILE_SIZE=%u\n",
          TOTAL_SECTORS, FIRST_DATA_LBA, FAT_SECTORS, ROOT_DIR_SECTORS, DECLARED_FILE_SIZE);
  build_boot_sector();
  build_fat();
  build_root_dir();
  FILE *f;
  f = fopen("/home/silent/.claude/jobs/cf21d43d/tmp/c_boot_sector.bin", "wb");
  fwrite(g_boot_sector, 1, SECTOR_SIZE, f); fclose(f);
  f = fopen("/home/silent/.claude/jobs/cf21d43d/tmp/c_fat_sector.bin", "wb");
  fwrite(g_fat_sector_cache, 1, sizeof(g_fat_sector_cache), f); fclose(f);
  f = fopen("/home/silent/.claude/jobs/cf21d43d/tmp/c_root_dir.bin", "wb");
  fwrite(g_root_dir_sector, 1, sizeof(g_root_dir_sector), f); fclose(f);
  return 0;
}
