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

## UART framing / resync logic

```
g++ -Wall -std=c++17 -o link_crosscheck link_crosscheck.cpp && ./link_crosscheck
python3 link_crosscheck.py
```
Both feed an identical synthetic byte stream (leading garbage, a valid `A` frame, a bad-type
frame, a valid `C` frame, an oversized-length frame, a final valid `A` frame) through the same
find-sync/header-validate/resync logic used in `link_task()`/`receive_from_esp32()`. Both must
report parsing exactly 3 frames with identical type/length/payload, correctly skipping and
resyncing past both malformed frames without getting stuck. Confirmed identical 2026-09-17.

## Real-time concurrent simulation (std::thread/std::mutex, real wall-clock timing)

```
g++ -Wall -std=c++17 -O2 -pthread -o realtime_concurrency_sim realtime_concurrency_sim.cpp
./realtime_concurrency_sim 180   # seconds to run; 180s = 6 full ring laps
```

Everything above is single-threaded and scripted (deterministic sequences, no real
concurrency or timing). This one runs `disk_append()`/`disk_read_data()` (copy-pasted from the
real firmware, `std::mutex` swapped for `xSemaphore`) on two genuinely concurrent threads with
REAL wall-clock pacing: a writer thread paced at the real ~16000 B/s audio bitrate in
variable-sized (256-512B) chunks, a reader thread issuing bursty 512B reads (8 back-to-back,
then a pause) matching realistic USB-MSC client behavior more closely than a perfectly
metronomic rate. Each 4-byte-aligned ring position holds a globally-monotonic counter value:
any read is checked against the *exact* value that should legitimately be there given the
writer's real total-bytes-written count at the moment of that specific read (computed
precisely from which "lap" the position belongs to, not eyeballed) — a genuine tear/splice
would produce a value from the wrong lap, which this catches with zero tolerance for false
negatives.

**Confirmed 2026-09-17, three separate real-time runs (45s/100s/180s, 1.5/3.34/6.01 full ring
laps): zero corruption in every run.** Also measured real statistics worth knowing:
straddle-avoidance triggers on ~1.7-1.8% of reads under this bursty read pattern (i.e. ~1.7-1.8%
of reads get a brief silent gap instead of real content — the documented tradeoff working as
designed), and mutex contention is essentially a non-issue (0-3 lock timeouts out of tens of
thousands of reads, using a stricter zero-wait test than the real firmware's 2ms budget).

**Caught and fixed two real bugs in this test harness itself before trusting the result** —
worth knowing if this script is ever extended: (1) the first version's corruption check
couldn't distinguish "two adjacent, legitimately different writer chunks sharing one
fixed-size read window" from a real splice, producing a huge false-positive count; fixed by
switching to a continuous per-4-byte-slot counter instead of a per-chunk pattern. (2) the
fixed version still had a bug at longer runs: it captured the "expected total written" snapshot
*after* releasing the read lock, leaving a race window for the writer to advance further in
between and describe a state later than what was actually read; fixed by capturing that value
atomically inside the same locked section as the actual memcpy.

## What this does NOT verify

Everything hardware-dependent: real UART timing/framing over an actual
wire, actual TinyUSB `onRead` call granularity, PSRAM allocation, the
2ms-mutex-timeout behavior under real USB read pressure. Those need the
real board -- see `progress/STATUS.md`'s bring-up checklist.

If `esp32-s3-msc.ino`'s `disk_append`/`disk_read_at`/`build_*` functions
change, re-run both sides of these cross-checks and update this file's
expected values before trusting the change.
