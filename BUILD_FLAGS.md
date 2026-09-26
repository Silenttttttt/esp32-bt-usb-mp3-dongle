# Build flags: profiles, what every flag does, and what it really did

Two boards are built with compile-time flags: the **classic ESP32** (Bluetooth A2DP sink,
`esp32-bt-mp3-test/`) and the **ESP32-S3** (the USB disk the car radio reads, `esp32-s3-msc/`).
Each board's flags live in one list in its flash script. Don't retype them by hand: a bare
`arduino-cli compile .` silently drops them, and that has cost hours twice.

```
esp32-s3-msc/flash.sh       [--v1] [--s3-encode] [--trace] [--no-upload] [-DEXTRA ...]
esp32-bt-mp3-test/flash.sh  [--v1] [--s3-encode]           [--no-upload] [-DEXTRA ...]
```

v2 is the default. Options can come in any order. Both boards print their commit and flags at
boot and once a minute (S3: `[s3] build: ...` on its debug serial; classic: a `BUILD:commit=...`
control frame), and every flash is appended to `logs/flash_history.log` (git-ignored).
**Flash both boards with the same options**: the classic and the S3 must agree on where the MP3
encoder runs, because that sets the link speed.

Related: [docs/KENWOOD_RADIO.md](docs/KENWOOD_RADIO.md) (how the real car radio behaves) and
[docs/HARDWARE_USAGE.md](docs/HARDWARE_USAGE.md) (what each board has and what v2 uses).

## Profiles

| | **v2** (default) | **v1** (`--v1`) | **v1 + S3 encoder** (`--v1 --s3-encode`) |
|---|---|---|---|
| Status | **The car build.** Car-verified 2026-09-25; later changes bench-verified | Car-verified 2026-09-18 (with a 25.6 s ring, raised to ~4 min right after) | Bench only (2026-09-25). Works, but crackles (below) |
| Classic flags | `INT2IDX_SIZE=4000 DIAG_LOOP_DRAIN DIAG_FRAG_TRACE V2_ALL ENCODE_ON_S3` | `INT2IDX_SIZE=4000 DIAG_LOOP_DRAIN DIAG_FRAG_TRACE A2DP_DISABLE_AVRC` | v1 + `ENCODE_ON_S3` |
| S3 flags | `FATDISK_ALWAYS_SERVE_LIVE FATDISK_MULTI_FILE LED_RAINBOW_PLAYING ENCODE_ON_S3` | none (bare) | `ENCODE_ON_S3` |
| MP3 encoder on | S3 | classic | S3 |
| Classic -> S3 link | mono PCM, 2,000,000 baud | MP3, 921,600 baud | mono PCM, 2,000,000 baud |
| Disk | 3 files `Stream.mp3`, ~46 min each (1360 × 32 KB clusters, the FAT12 maximum; a 133.7 MB disk), served from the live stream out of a 5.57 MB ring | 1 file `STREAM.MP3`, a ~8.5 min ring (2000 clusters, the PSRAM maximum) served by requested offset | same as v1 |
| Radio Next/Back | Relayed to the phone | Moves around the ring; phone not told | same as v1 |
| AVRCP | On (auto-resume, near-end skip, button relay) | Off | Off |
| Delay, phone to speaker | ~5 s in the car, ~4-5 s on the bench | ~10 s in the car on 2026-09-18 (25.6 s ring); with the ~4 min ring, whatever the reader happens to trail by, up to minutes | Up to ~30 s on the bench (25.6 s ring) |
| Known issues | Up to ~1 s of silence once per natural file end (now every ~46 min) when the radio's pause is short. 32 KB clusters and the 133.7 MB disk are bench-verified only | Audible splice at every ring wrap (every ~8.5 min; was every 25.6 s). Long, variable delay. No phone control | Crackle at every file end (measured every ~26 s with the 25.6 s ring; every ~8.5 min with the default ring); long delay |

Why v1's delay is long and variable: v1 serves the radio exactly the byte offset it asks for,
from a ring the writer keeps overwriting. How far the reader trails the writer is whatever it
happens to be, up to the ring's length. The first car test used a 25.6 s ring (~10 s delay, a
splice every 25.6 s). The ring was then raised to ~4 min (938 clusters), and on 2026-09-25 to the
maximum, ~8.5 min (2000 clusters), to make the splice rare; that also raises the possible delay. That's the problem v2 was built to solve: it serves from a
cursor kept ~0.8 s behind the newest audio, whatever offset the radio asks for. The bench test of
"v1 + S3 encoder" used `-DFATDISK_DATA_CLUSTERS=100` (the original 25.6 s ring).

Why "v1 + S3 encoder" crackles (found 2026-09-25, **not fixed**; v1 isn't the target). The trigger
is v1's write protection: the writer won't overwrite bytes the radio hasn't read yet. At every file
end the radio pauses 2-3.5 s and the writer catches up with it. Each protected write then retries
for up to ~1 s (200 × 5 ms) inside the task that drains the serial port. At 88 KB/s of PCM, the S3's
16 KB receive buffer lasts ~0.18 s, so PCM spills: the S3 logged FIFO overflows in bursts ~26 s
apart. Plain v1 has the same retry but only 16 KB/s of MP3 coming in, so it had far more headroom.
The fix, if v1 is ever needed again: don't block in `append_mp3()` (one protected try, then
overwrite).

## Every flag

### Classic ESP32 (`esp32-bt-mp3-test`)

| Flag | Profiles | Intent | Expected | Observed (real hardware) | Status |
|---|---|---|---|---|---|
| `DIAG_LOOP_DRAIN` | v1, v2 | Run the PCM work (downmix, encode or send) from `loop()` instead of a dedicated FreeRTOS task | Same audio, work on Arduino's loop task | Creating the separate `encode_task()` breaks Bluetooth connectability on this board (the phone can't connect at all); A/B tested 2026-09-22. With the flag, BT works | **Required** everywhere. The name says "diag", but it's load-bearing |
| `INT2IDX_SIZE=4000` | v1, v2 | Shrink Shine's x^(3/4) quantization table from 10,000 to 4,000 entries (~24 KB RAM) | Same MP3 quality at normal levels | No audible difference; keeps enough heap for BT when the classic encodes | Matters only when the classic encodes (v1). Harmless in v2 |
| `DIAG_FRAG_TRACE` | v1, v2 | Heap-fragmentation telemetry | A `FRAG:free=,largest=,total8=,min8=,int_total=,int_free=` line every second | Works; the source of the heap numbers in `docs/HARDWARE_USAGE.md` | Diagnostic, cheap, kept on |
| `A2DP_DISABLE_AVRC` | v1 only | Turn AVRCP off inside the ESP32-A2DP library (a separate translation unit, so it must be a build flag) | Leaner stack: audio only, no metadata or remote control | Once recommended as a crash fix; the real cause was heap fragmentation (2026-09-22). Rejected for the car build | v1 only. **Never with `V2_ALL`** (every v2 feature silently does nothing) |
| `V2_ALL` | v2 | One switch for the AVRCP features: `AVRC_AUTO_RESUME_ON_RECONNECT`, `AVRC_AUTO_RESUME_EARLY_PAUSE`, `AVRC_AUTO_SKIP_NEAR_END`, `AVRC_TRACK_POSITION`, `RADIO_CMD_RELAY`, `RADIO_TRACK_RENAME` (each also settable alone) | Resume on reconnect, undo an early pause, skip just before a song ends, forward radio Next/Back to the phone | Relay works in the car both ways incl. wrap (2026-09-25); auto-skip seen (`CMD_SENT:auto_skip_near_end`). **With the classic also encoding, the phone's AVRCP link dropped**: Shine's ~80 KB left ~19 KB free, 3 KB low-water (A/B, night of 2026-09-24/25) | v2. Needs `ENCODE_ON_S3`, and `A2DP_DISABLE_AVRC` absent. Near-end skip without track position = compile error, on purpose. `RADIO_TRACK_RENAME` output is ignored by the S3 now |
| `ENCODE_ON_S3` | v2, v1 + S3 enc | Move the MP3 encoder off the RAM-starved classic | Classic downmixes to mono and sends raw PCM `P` frames at 2 Mbaud; more free heap; the S3 encodes | Classic free heap ~100 KB with AVRCP stable; S3 encodes a 23 ms chunk in ~4.3-5.2 ms (2026-09-25) | **Both boards or neither**: a mismatch = different bauds, no valid frames, S3 blinks WHITE |

### ESP32-S3 (`esp32-s3-msc`)

| Flag | Profiles | Intent | Expected | Observed (real hardware) | Status |
|---|---|---|---|---|---|
| `FATDISK_ALWAYS_SERVE_LIVE` | v2 | Keep the radio near live: ignore the requested offset and serve from a cursor kept `FATDISK_LIVE_TARGET_LAG` behind the newest audio | Delay of seconds whatever the ring size. Byte-continuous reads (MP3 frames intact). A catch-up jump if it falls far behind; silent frames instead of old audio if the reader outruns the writer | First car test failed (2026-09-21, 3 bugs, fixed on the bench). Works in the car from 2026-09-25, ~5 s delay. The radio's file-open pattern caused 45-55 underrun reads per open on the bench; fixed the same day (`e9c9d9c`): 0 at Next/Back/mount, ~0-1 s at a natural end | v2. Use with `FATDISK_MULTI_FILE` (the open-pattern handling lives there) |
| `FATDISK_MULTI_FILE` | v2 | Detect the radio's Next/Back: 3 files alias the live stream, and a file switch is a button press | Mid-file switch relayed as `RADIO_CMD:next/prev`; a switch at the file's end not relayed; a reader starting after a >3 s gap adopted silently | First car test: never relayed (every open reads the last 2 KB, so every switch looked like a file end); fixed (only playback reads count). Car 2026-09-25: Next and Back relayed with the right direction incl. wrap, natural ends not relayed. Bench: no false relay at mount | v2. Needs the classic's `RADIO_CMD_RELAY` (in `V2_ALL`) |
| `ENCODE_ON_S3` | v2, v1 + S3 enc | S3 half of the classic flag | Shine encoder on the S3 + a 16 KB UART receive buffer | See the classic row | Must match the classic |
| `LED_RAINBOW_PLAYING` | v2 | Muni's preference | Rainbow LED while playing instead of solid green | Works | Cosmetic |
| `FATDISK_DATA_CLUSTERS=N` | all (default: 1360 with 3 files, 2000 with 1) | File length in clusters. The defaults are the maximums (Muni: bigger is better) | 1360 = the FAT12 cap with 3 files (× 32 KB = 44.6 MB, ~46 min in v2). v1: 2000 × 4 KB = 8.19 MB, ~8.5 min (its ring is the file, one PSRAM block). 100 × 4 KB = 25.6 s, 118 = ~30 s | 100: v1's first car test. 938 (~4 min): default 2026-09-18 to 2026-09-25, car-tested in v2. 1360: bench-verified 2026-09-25 (PSRAM alloc OK, 16 MB FAT12 disk, 0 underruns at opens); not yet in the car. 118: one car test | Tuning |
| `MSC_TRACE` | opt-in (`--trace`) | Car capture of how the radio reads the disk | Every SCSI command (CDB, status, bytes, timing, region), bus resets, descriptor/control requests, file switches. Debug serial at 921600 | Produced the whole Kenwood behavior table (2026-09-25), 0 records dropped | Diagnostic only. Run with `logs/car_capture.sh` |
| `FATDISK_SECTORS_PER_CLUSTER=N` | v2 = 64 (32 KB), others 8 (4 KB) | FAT cluster size. FAT12 caps the cluster COUNT (~4082), so bigger clusters = longer files | 64 → 3 files of 44.6 MB (~46 min) on a 133.7 MB FAT12 disk | 4 KB: the prototype default, car-proven with the S3 (2026-09-18, 2026-09-25). 32 KB: played on the Kenwood from a real FAT12 stick (2026-09-17). With the S3: bench-verified 2026-09-25 (fsck OK, car_sim, Next/Back relay, 0 underruns at opens); not yet in the car. The 4 KB-stick failure of 2026-09-17 had other confounders (counterfeit sticks, ID3/Xing headers) | v2 default. Margins that used to scale with it are pinned to bytes |
| `FATDISK_RING_BYTES=N` | v2 (default 5,570,560) | Size of the PSRAM audio ring, separate from the file size with live serving | ~348 s of history; the live cursor only ever needs seconds of it | Bench: allocates fine (2.7 MB PSRAM left) | Tuning. v1 always uses ring = file |
| `FATDISK_LIVE_TARGET_LAG=N` | v2 (default 12288) | How far behind live the cursor sits | ~0.77 s of cushion against Bluetooth stalls, paid for in delay | Tuned by the host-test sweep | Tuning |
| `FATDISK_RADIO_READAHEAD=N` | v2 (default 61440) | The radio's read-ahead burst at a file open | After Next/Back/mount, the file starts this far back so the burst reads real audio | Kenwood's burst is ~58 KB; with this, 0 underruns at opens (bench) | Tuning |

### Board fixes that aren't flags (apply to every profile)
- **S3 UART RX FIFO threshold 32** (was the driver's default 120 of 128). At 2 Mbaud the default
  left ~40 us of interrupt headroom, and the overflows were an occasional click (2026-09-25).
- **Classic PCM callback split** (2026-09-25). An A2DP callback bigger than one 4 KB slot used to be
  clamped, silently dropping the rest. A PC source packing 11 SBC frames lost 27% of its audio;
  phones' ~7-frame packets fit, which hid it.
- **Classic pairing** (2026-09-25):
  - legacy PIN requests are answered with `0000` (they were never answered, so hosts timed out);
  - the stuck-radio watchdog no longer reboots mid-pairing.
- **S3 USB auto re-attach** (2026-09-25). If no host configures the S3 4 s after boot, it
  disconnects and reconnects its USB. The Kenwood sometimes never re-detects it otherwise.

## Combinations

| Combination | Status |
|---|---|
| v2 on both boards | Works (the car build) |
| v1 on both boards | Worked in the car on 2026-09-18 (25.6 s ring at the time) |
| v1 + S3 encoder on both boards | Works on the bench; crackles at every file end, long delay (see above) |
| `ENCODE_ON_S3` on one board only | **Broken:** different link speeds, no audio |
| `V2_ALL` with `A2DP_DISABLE_AVRC` | **Broken:** the v2 features silently do nothing |
| `V2_ALL` without `ENCODE_ON_S3` | **Broken in practice:** heap starvation drops the phone's AVRCP link |
| `FATDISK_MULTI_FILE` with a v1 classic | Plays, but Next/Back never reach the phone |
| `FATDISK_MULTI_FILE` without `FATDISK_ALWAYS_SERVE_LIVE` | Not tested. Don't |
| `FATDISK_ALWAYS_SERVE_LIVE` without `FATDISK_MULTI_FILE` | Not recommended: failed its first car test, never re-tested, no open-pattern handling |

## Removed flags (don't look for them)

`FATDISK_NO_TITLES`, `FATDISK_NO_BUFFER_FILES`, `NAME_TEST`, `EARLY_END_FAT`,
`EARLY_END_READ_ERROR`, `EARLY_END_NO_FAT`, `FATDISK_BUFFER_FILE_BYTES`: removed 2026-09-25 with
the song-name features. The Kenwood reads a file's size and cluster chain only when it opens it,
so a playing file can't be ended early. A long file name made it reject files, and it shows the
ID3v1 tag, never the file name.
