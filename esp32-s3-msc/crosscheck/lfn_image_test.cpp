// Host test for the VFAT long filenames in fat_disk_shared.h (FATDISK_MULTI_FILE).
// Writes the metadata region of the real disk image (boot sector, FATs, root
// directory) for a given title, padded to full size, so fsck.fat and
// car_sim.py's own directory parser can check it:
//
//   g++ -O2 -std=c++17 -Wall -DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -o lfn_image_test lfn_image_test.cpp
//   ./lfn_image_test out.img "Arctic Monkeys - 505"
//   fsck.fat -n -v -l out.img
#include <cstdint>
#include "../fat_disk_shared.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s out.img [title]\n", argv[0]); return 2; }
  init_file_names();
  build_boot_sector();
  build_fat();
  build_root_dir();
  if (argc >= 3) set_title_utf8(argv[2], (uint32_t)strlen(argv[2]));
  std::vector<uint8_t> meta(FIRST_DATA_LBA * SECTOR_SIZE);
  disk_read_at(0, meta.data(), meta.size());  // the MSC read callback path
  FILE *f = fopen(argv[1], "wb");
  fwrite(meta.data(), 1, meta.size(), f);
  std::vector<uint8_t> zero(SECTOR_SIZE, 0);
  for (uint32_t lba = FIRST_DATA_LBA; lba < TOTAL_SECTORS; lba++) fwrite(zero.data(), 1, SECTOR_SIZE, f);
  fclose(f);
  printf("root dir LBA %u, %u sectors, %u entries\n", RESERVED_SECTORS + NUM_FATS * FAT_SECTORS,
         ROOT_DIR_SECTORS, ROOT_ENTRIES);
  return 0;
}
