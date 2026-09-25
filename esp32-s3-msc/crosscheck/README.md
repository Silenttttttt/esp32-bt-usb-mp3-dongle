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
`straddle_hits`/`protected_rejections` counts.

**Note (2026-09-17): `total_written` is expected to differ, intentionally.** A real bug was
found and fixed in the firmware (see `esp32-s3-msc.ino`'s own comment on `disk_append`):
letting `total_written` grow unbounded for an entire session would overflow its `uint32_t`
after ~74.6 hours of continuous operation, silently disabling write-side backpressure for ~30s
right at that mark. Fixed by capping it at `DECLARED_FILE_SIZE` and never growing it further —
Python's `total_written` is arbitrary-precision and has no analogous issue, so it's correctly
NOT capped there. Every other tracked value must still match exactly. Confirmed 2026-09-17:
`writes_done=3892 reads_done=2000 straddle_hits=40 protected_rejections=0 final
write_pos=75776 last_read_offset=315392` identical on both sides; `total_written` legitimately
differs (479232 capped in C++, 1992704 uncapped in Python) — that specific divergence is
correct, not a bug.

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

## Adversarial reader-stall test (real backpressure engagement)

```
g++ -Wall -std=c++17 -O2 -pthread -o reader_stall_sim reader_stall_sim.cpp
./reader_stall_sim 100 65   # total run seconds, stall seconds (must be > 2x ring duration to matter)
```

The concurrency sim above barely exercised `unread_protect` backpressure (the bursty reader
stayed too well caught-up). This variant makes the reader do a genuine, real multi-second dead
stall (simulating a crashed/stuck host) before resuming, forcing the writer to actually hit
the protection path. **A stall must exceed *two* full ring durations to matter** — the ring's
very first-ever wrap is always unprotected by design (matches `fat12_disk.py`'s own comment:
nothing before the first full lap has been "consumed" in any meaningful sense yet), so the
writer only re-approaches the stale `last_read_offset` on its *second* attempt to pass that
position, one full lap later. (First tried a 40s stall on this 30s-capacity ring — 0 rejections,
looked wrong until traced through: only 1.3 laps completed, never reached the second approach.)

**Confirmed 2026-09-17** with a 65s stall (2.17 ring durations): real backpressure engaged
exactly as designed — `write_protected_rejections=200` (one write exhausted the full
`MAX_WRITE_RETRIES` budget), `forced_writes=1` (the retry-exhausted fallback fired once,
matching `link_task()`'s real behavior) — and **zero corruption**, even through the forced
fallback. This is the exact adversarial scenario ("the reader crashes/stalls for longer than
the whole ring") the project has been designed to survive since early sessions — confirmed
here to actually hold up.

## Region-boundary-crossing reads

```
g++ -Wall -std=c++17 -o region_boundary_test region_boundary_test.cpp && ./region_boundary_test
```

All the other tests above exercise each FAT12 region (boot/FAT/root-dir/data) mostly in
isolation. In practice, real TinyUSB calls `onRead` with a fixed, sector-aligned `bufsize`
(confirmed from TinyUSB's own source), so a single call spanning two regions likely never
actually happens on real hardware — but `disk_read_at()` was written to handle an arbitrary
byte range generically anyway, in case that assumption turns out wrong. This test fills each
region with a distinct byte pattern and issues reads deliberately straddling every region
boundary (boot→FAT, FAT copy 1→copy 2, root-dir→data, and one read spanning all four regions
in a single call) — confirmed 2026-09-17: all four cases produce exactly the right bytes on
both sides of every boundary, zero corruption.

## Real disk-image mount test (Linux's own vfat kernel driver, not just our own logic)

```
g++ -Wall -o gen_fat12_image gen_fat12_image.cpp && ./gen_fat12_image /tmp/fat12_image.bin ../../sim/silence_primer.mp3
sudo losetup -fP --show /tmp/fat12_image.bin   # note the /dev/loopN it prints
mkdir -p /tmp/mnt && sudo mount -t vfat -o ro,uid=$(id -u),gid=$(id -g) /dev/loopN /tmp/mnt
ls -la /tmp/mnt/ && file /tmp/mnt/STREAM.MP3 && mpg123 -q -t /tmp/mnt/STREAM.MP3
sudo umount /tmp/mnt && sudo losetup -d /dev/loopN
```

Everything above cross-checks this project's own from-scratch logic against a from-scratch
Python reference — real, but both sides share the same author and the same potential blind
spots. This test is different: it generates a full, real disk image byte-for-byte matching
what `esp32-s3-msc.ino`'s `build_boot_sector()`/`build_fat()`/`build_root_dir()` produce, and
mounts it with **Linux's own real, independent, standards-compliant vfat kernel driver** —
completely outside this project's own code. Confirmed 2026-09-17: mounts cleanly, the file
appears as `STREAM.MP3` with the exact declared size, content bytes match exactly, and a real,
independent MP3 decoder (`mpg123`) decodes it without error. `gen_fat16_image.cpp` does the
same for the FAT16 fallback variant (see `../esp32-s3-msc-fat16-fallback/`) — also confirmed
clean.

## What this does NOT verify

Everything hardware-dependent: real UART timing/framing over an actual
wire, actual TinyUSB `onRead` call granularity, PSRAM allocation, the
2ms-mutex-timeout behavior under real USB read pressure. Those need the
real board -- see `progress/STATUS.md`'s bring-up checklist.

If `esp32-s3-msc.ino`'s `disk_append`/`disk_read_at`/`build_*` functions
change, re-run both sides of these cross-checks and update this file's
expected values before trusting the change.

## Live-serve read path + file-switch detector (`FATDISK_ALWAYS_SERVE_LIVE` / `FATDISK_MULTI_FILE`)

```
g++ -O2 -std=c++17 -Wall -DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -o live_serve_test live_serve_test.cpp
./live_serve_test
```
Feeds a numbered byte stream through `disk_append()` at exact / jittery / slow / fast rates
across several ring wraps and checks every non-silence read is a contiguous, fully written,
not-yet-overwritten stream range (catches serving past the write pointer and the stale
wrap tail). Also checks the switch detector: natural EOF, a mid-file Next press, and a reader
restart or post-mount probe noise are handled correctly. Must print `ALL OK`. Sweep the jump
target lag with `-DFATDISK_LIVE_TARGET_LAG=<bytes>`. Added 2026-09-24.

## VFAT long filenames (FATDISK_MULTI_FILE)

```
g++ -O2 -std=c++17 -Wall -Wno-unused-function -DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -o lfn_image_test lfn_image_test.cpp
./lfn_image_test /tmp/lfn.img "Arctic Monkeys - 505" && fsck.fat -n -v -l /tmp/lfn.img
```
fsck.fat must list `/Arctic Monkeys - 505.mp3 (ARCTIC~1.MP3)` and `~2`, `~3` with no errors
(the "no volume label in root directory" notice predates the long names). Confirmed
2026-09-25 with an empty title, ASCII, non-ASCII with forbidden characters, and a 300-char title
(truncated to 255). The single-file build's metadata is byte-identical to before, apart from the
per-boot volume serial.
