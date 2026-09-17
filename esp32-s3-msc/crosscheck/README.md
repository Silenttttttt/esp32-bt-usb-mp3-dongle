# S3 firmware cross-checks (host-only, no ESP32 hardware needed)

`esp32-s3-msc.ino` was written before any ESP32-S3 board existed to test
it on. These scripts verify its core FAT12/ring-buffer logic is a
byte-for-byte, step-for-step faithful port of `../../sim/fat12_disk.py`
(the already-hardware-proven Python prototype) -- pure logic checks, no
Arduino/USB/UART dependency, so they can catch a real porting bug before
ever touching hardware.

## Static structures (boot sector / FAT table / root directory)

```
g++ -Wall -o fat12_crosscheck fat12_crosscheck.cpp && ./fat12_crosscheck
python3 -c "
import sys; sys.path.insert(0, '../../sim')
from fat12_disk import GrowingFat12Disk
d = GrowingFat12Disk(capacity_bytes=117*8*512)
open('/tmp/py_boot_sector.bin','wb').write(d._boot_sector)
open('/tmp/py_fat_sector.bin','wb').write(d._fat_sector_cache)
open('/tmp/py_root_dir.bin','wb').write(d._root_dir_sector)
"
cmp c_boot_sector.bin /tmp/py_boot_sector.bin   # or wherever fat12_crosscheck wrote its output
cmp c_fat_sector.bin /tmp/py_fat_sector.bin
cmp c_root_dir.bin /tmp/py_root_dir.bin
```
All three must report no differences. Confirmed identical 2026-09-17.

## Ring buffer / backpressure / straddle logic

```
g++ -Wall -O2 -o ring_crosscheck ring_crosscheck.cpp && ./ring_crosscheck
python3 ring_crosscheck.py
```
Both run the identical scripted sequence (fill past 2 ring laps, then
2000 interleaved read/write steps with periodic forced-straddle reads)
and print final `write_pos`/`total_written`/`last_read_offset` plus
`straddle_hits`/`protected_rejections` counts. Every value must match
exactly. Confirmed identical 2026-09-17: `writes_done=3892 reads_done=2000
straddle_hits=40 protected_rejections=0 final write_pos=75776
total_written=1992704 last_read_offset=315392` on both sides.

## What this does NOT verify

Everything hardware-dependent: real UART timing/framing over an actual
wire, actual TinyUSB `onRead` call granularity, PSRAM allocation, the
2ms-mutex-timeout behavior under real USB read pressure. Those need the
real board -- see `progress/STATUS.md`'s bring-up checklist.

If `esp32-s3-msc.ino`'s `disk_append`/`disk_read_at`/`build_*` functions
change, re-run both sides of these cross-checks and update this file's
expected values before trusting the change.
