// FAT16 variant of gen_fat12_image.cpp -- generates a real disk image for
// independent verification (real Linux vfat driver) of a potential FAT16
// fallback, in case the real car radio turns out to reject FAT12 (a real,
// unconfirmed risk found via research 2026-09-17). NOT the active
// implementation -- a verified-ready option, see progress/STATUS.md.
//
// FAT16 needs 4085-65524 data clusters. To keep the declared volume as
// small as possible (minimizing the worst-case catch-up-lag bound that
// motivated a small ring in the first place), uses 1-sector (512B)
// clusters instead of FAT12's 8-sector (4096B) clusters -- 4096 clusters
// * 512B = 2MB (~131s/~2.2min of audio at 16000 B/s), vs. ~16.7MB
// (~17.4min) if using the same 4096B clusters as FAT12. This is the
// minimum-viable size tradeoff, not an arbitrary choice.
#include <cstdio>
#include <cstdint>
#include <cstring>
using namespace std;

static const uint32_t SECTOR_SIZE = 512;
static const uint32_t SECTORS_PER_CLUSTER = 1;  // minimize size penalty of the 4085-cluster FAT16 floor
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const char FILE_NAME[12] = "STREAM  MP3";
static const uint32_t DATA_CLUSTERS = 4096;  // valid FAT16 range is 4085-65524
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;
// FAT16 entries are 2 bytes each, no bit-packing needed (unlike FAT12's 1.5 bytes/entry).
static const uint32_t FAT_ENTRIES_NEEDED = DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = FAT_ENTRIES_NEEDED * 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t TOTAL_SECTORS = FIRST_DATA_LBA + DATA_CLUSTERS * SECTORS_PER_CLUSTER;

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];

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
  memcpy(bs + 54, "FAT16   ", 8);  // only textual difference from FAT12's boot sector besides FAT table content
  bs[510] = 0x55; bs[511] = 0xAA;
}

static void build_fat() {
  memset(g_fat_sector_cache, 0, sizeof(g_fat_sector_cache));
  uint16_t *fat16 = (uint16_t *)g_fat_sector_cache;
  fat16[0] = 0xFFF8;  // reserved cluster 0: media descriptor echo
  fat16[1] = 0xFFFF;  // reserved cluster 1: end-of-chain marker convention
  for (uint32_t c = 2; c < DATA_CLUSTERS + 2; c++) {
    fat16[c] = (c < DATA_CLUSTERS + 1) ? (uint16_t)(c + 1) : 0xFFFF;
  }
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

int main(int argc, char **argv) {
  const char *out_path = argc > 1 ? argv[1] : "fat16_image.bin";
  const char *content_path = argc > 2 ? argv[2] : nullptr;

  build_boot_sector();
  build_fat();
  build_root_dir();

  FILE *f = fopen(out_path, "wb");
  fwrite(g_boot_sector, 1, SECTOR_SIZE, f);
  for (uint32_t i = 0; i < NUM_FATS; i++) fwrite(g_fat_sector_cache, 1, sizeof(g_fat_sector_cache), f);
  fwrite(g_root_dir_sector, 1, sizeof(g_root_dir_sector), f);

  uint8_t *data = new uint8_t[DECLARED_FILE_SIZE]();
  if (content_path) {
    FILE *cf = fopen(content_path, "rb");
    if (cf) {
      size_t n = fread(data, 1, DECLARED_FILE_SIZE, cf);
      fprintf(stderr, "loaded %zu bytes of real content into data region\n", n);
      fclose(cf);
    }
  }
  fwrite(data, 1, DECLARED_FILE_SIZE, f);
  fclose(f);

  fprintf(stderr, "wrote %s: %u total sectors, %u bytes total, declared file size %u (%.1fs of audio @ 16000 B/s), FAT_SECTORS=%u\n",
          out_path, TOTAL_SECTORS, TOTAL_SECTORS * SECTOR_SIZE, DECLARED_FILE_SIZE,
          DECLARED_FILE_SIZE / 16000.0, FAT_SECTORS);
  delete[] data;
  return 0;
}
