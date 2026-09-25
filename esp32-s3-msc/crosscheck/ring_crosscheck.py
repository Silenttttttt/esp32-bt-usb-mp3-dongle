import sys
sys.path.insert(0, "./sim")
from fat12_disk import GrowingFat12Disk, SECTOR_SIZE

d = GrowingFat12Disk(capacity_bytes=117 * 8 * 512)
# Cancel out the silence-primer auto-write so this test starts from the
# exact same all-zero state the C++ test does (that primer is a separate,
# already-verified feature, not what's being cross-checked here).
d.write_pos = 0
d.total_written = 0
d.last_read_offset = 0
d.buffer[:] = bytes(len(d.buffer))

DECLARED_FILE_SIZE = d.declared_file_size
CLUSTER_SIZE = 8 * 512
READ_MARGIN_BYTES = 2 * CLUSTER_SIZE

chunk = bytearray(512)
for i in range(512):
    chunk[i] = i & 0xFF

writes_done = 0
protected_rejections = 0
reads_done = 0
straddle_hits = 0

target_bytes = DECLARED_FILE_SIZE * 2 + 10000
while writes_done * 512 < target_bytes:
    chunk[0] = writes_done & 0xFF
    d.append(bytes(chunk), unread_protect=False)
    writes_done += 1

print(f"phase1: writes_done={writes_done} write_pos={d.write_pos} total_written={d.total_written}")

for step in range(2000):
    if step % 50 == 0:
        read_off = d.write_pos
    else:
        read_off = (d.write_pos + DECLARED_FILE_SIZE // 2) % DECLARED_FILE_SIZE
    lba = d.first_data_lba + read_off // SECTOR_SIZE
    result = d.read_sectors(lba, 1, avoid_straddle=True, margin=READ_MARGIN_BYTES)
    # fat12_disk.py: unsafe_end = first_data_off + length + margin, length=count*SECTOR_SIZE=512.
    # C++: unsafe_end = data_off + n + READ_MARGIN_BYTES, n=512. Same base, same margin -> equivalent.
    reads_done += 1
    if result is None:
        straddle_hits += 1

    chunk[0] = (writes_done + step) & 0xFF
    if not d.append(bytes(chunk), unread_protect=True):
        protected_rejections += 1
        d.append(bytes(chunk), unread_protect=False)
    writes_done += 1

print(f"phase2: writes_done={writes_done} reads_done={reads_done} straddle_hits={straddle_hits} "
      f"protected_rejections={protected_rejections} final write_pos={d.write_pos} "
      f"total_written={d.total_written} last_read_offset={d.last_read_offset}")
