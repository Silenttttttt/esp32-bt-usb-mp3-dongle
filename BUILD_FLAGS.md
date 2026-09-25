# Build flags: known-good profiles

Two boards are built with compile-time flags: the **classic ESP32** (Bluetooth A2DP sink) and the
**ESP32-S3** (USB mass-storage disk the car radio reads). Each board's flags live in one list in
its flash script. Don't retype them by hand. A bare `arduino-cli compile .` silently drops them;
that has cost hours twice.

```
esp32-s3-msc/flash.sh       [--v1] [--trace] [--no-upload] [-DEXTRA ...]
esp32-bt-mp3-test/flash.sh  [--v1]           [--no-upload] [-DEXTRA ...]
```

Default is **v2**. `--v1` switches to the v1 profile; put it before any extra `-D` flags. Both
boards print their commit and flags at boot and once a minute (S3: `[s3] build: ...` on its debug
serial; classic: a `BUILD:commit=...` control frame). Every flash is appended to
`logs/flash_history.log` (git-ignored).

**Flash both boards with the same profile.** Mixing a v1 board with a v2 board doesn't work: the
serial link speed differs (see `ENCODE_ON_S3` below).

## The two profiles

| | **v1** (`--v1` on both) | **v2** (default on both) |
|---|---|---|
| Car-verified | 2026-09-18, Kenwood KDC-MP8090U | 2026-09-25, same radio (see the note under this table) |
| Classic flags | `INT2IDX_SIZE=4000 DIAG_LOOP_DRAIN DIAG_FRAG_TRACE A2DP_DISABLE_AVRC` | `INT2IDX_SIZE=4000 DIAG_LOOP_DRAIN DIAG_FRAG_TRACE V2_ALL ENCODE_ON_S3` |
| S3 flags | `FATDISK_DATA_CLUSTERS=100` | `FATDISK_ALWAYS_SERVE_LIVE FATDISK_MULTI_FILE LED_RAINBOW_PLAYING ENCODE_ON_S3` |
| MP3 encoder runs on | classic | S3 |
| Classic -> S3 link | MP3 frames, 921600 baud | mono PCM, 2,000,000 baud |
| Files on the disk | 1 (`STREAM.MP3`), a 25.6 s ring served by requested offset | 3 (`Stream.mp3`), ~4 min each, all served from the live stream |
| Radio Next/Back | Just moves around the ring; the phone isn't told | Relayed to the phone (next/previous song) |
| AVRCP (phone metadata/control) | Off | On: auto-resume on reconnect, near-end auto-skip, button relay |
| Delay, phone to speaker | ~10 s (car) | ~5 s (car) |
| Known issues | Audible splice every 25.6 s (the ring wrap). A bigger ring was tried and never car-tested: it trades the splice for up to minutes of lag. No phone control. | Up to ~1 s of silence once per natural file end (every ~4 min) when the radio's pause after a file is short. Second-device pairing and the file-open fix have only been verified on the bench (below). |

v1 ran in the car with the code of that date. The v1 build still compiles today, but the code
has moved on since, so treat v1 as known-good in its flags, not in today's full code path.

The v2 car session (2026-09-25) verified live streaming, Next/Back relay in both directions
with folder wrap, natural file ends, pause, engine start and power cycling, and USB re-attach.
Changes after that session have only been verified on the bench:
- song names removed and the car build made the default;
- second-device pairing (the classic answers PIN pairing; the watchdog no longer reboots it mid-pairing);
- the file-open underrun fix (no silence at Next/Back);
- the car_sim rewrite.

## Every flag

### Classic ESP32 (`esp32-bt-mp3-test`)

| Flag | v1 | v2 | What it does |
|---|---|---|---|
| `DIAG_LOOP_DRAIN` | yes | yes | **Required.** Encode/PCM work runs in `loop()`. Moving it to a separate task breaks Bluetooth connectability on this board (A/B tested 2026-09-22). |
| `INT2IDX_SIZE=4000` | yes | yes | The Shine encoder's quantization table: 4000 entries instead of 10000, ~24 KB less RAM. Only matters when the classic encodes (v1). Harmless in v2. |
| `DIAG_FRAG_TRACE` | yes | yes | A `FRAG:` heap line once a second. Diagnostic only. |
| `A2DP_DISABLE_AVRC` | yes | **no** | Turns AVRCP off in the Bluetooth library. **Never combine with `V2_ALL`.** Every v2 feature needs AVRCP. |
| `V2_ALL` | no | yes | Turns on every v2 feature: `AVRC_AUTO_RESUME_ON_RECONNECT`, `AVRC_AUTO_RESUME_EARLY_PAUSE`, `AVRC_AUTO_SKIP_NEAR_END`, `AVRC_TRACK_POSITION`, `RADIO_CMD_RELAY`, `RADIO_TRACK_RENAME`. Each one can also be set on its own. `AVRC_AUTO_SKIP_NEAR_END` needs `AVRC_TRACK_POSITION` (compile error otherwise). |
| `ENCODE_ON_S3` | no | yes | The classic sends raw mono PCM at 2 Mbaud and the S3 encodes. **Set on both boards or neither.** A mismatch means different link speeds: the S3 never sees a valid frame and blinks WHITE. |

### ESP32-S3 (`esp32-s3-msc`)

| Flag | v1 | v2 | What it does |
|---|---|---|---|
| `FATDISK_ALWAYS_SERVE_LIVE` | no | yes | Reads are served from a cursor near the newest audio, not from the offset the radio asks for. This keeps the delay at seconds instead of the ring's length. It also serves the radio's file-open pattern (probes, head, 58 KB burst) without underrun silence. |
| `FATDISK_MULTI_FILE` | no | yes | 3 files on the same live stream, with Next/Back detection and the relay to the phone (`RADIO_CMD:next/prev`). A file end the radio reaches on its own is never relayed. |
| `ENCODE_ON_S3` | no | yes | The S3 runs the MP3 encoder. Must match the classic. |
| `LED_RAINBOW_PLAYING` | no | yes | Rainbow LED while playing, instead of solid green. Cosmetic. |
| `FATDISK_DATA_CLUSTERS=N` | 100 | (938) | File/ring length in 4 KB clusters. 100 = 25.6 s (v1 as car-tested), 938 = ~4 min (default), 118 = ~30 s (a car test value). |
| `MSC_TRACE` | opt-in | opt-in | Car capture: logs every SCSI command the radio sends. Debug serial becomes 921600. Use `flash.sh --trace`, which adds the link flags it needs. |
| `FATDISK_LIVE_TARGET_LAG=N` | - | (12288) | How far behind the newest audio the live cursor stays (~0.77 s). |
| `FATDISK_RADIO_READAHEAD=N` | - | (61440) | The radio's read-ahead burst at a file open (Kenwood: ~58 KB). |

## Combinations

| Combination | Status |
|---|---|
| v1 on both boards | Works (car, 2026-09-18) |
| v2 on both boards | Works (car, 2026-09-25; later changes bench-verified) |
| `ENCODE_ON_S3` on one board only | **Broken.** Different link speeds, no audio. |
| `V2_ALL` with `A2DP_DISABLE_AVRC` | **Broken.** The v2 features silently do nothing. |
| `V2_ALL` without `ENCODE_ON_S3` (classic encodes and runs AVRCP) | **Broken in practice.** The encoder's ~80 KB starved the heap and the phone's AVRCP link dropped (A/B proven the night of 2026-09-24/25). This is why v2 moves the encoder to the S3. |
| `FATDISK_MULTI_FILE` with a v1 classic | Plays, but Next/Back never reach the phone (the relay is in `V2_ALL`). |
| `FATDISK_MULTI_FILE` without `FATDISK_ALWAYS_SERVE_LIVE` | Not tested. Don't. |
| `FATDISK_ALWAYS_SERVE_LIVE` without `FATDISK_MULTI_FILE` | Not recommended. The single-file live build failed its first car test (2026-09-21). The bugs were fixed on the bench, but it was never re-tested in the car, and the file-open handling only exists in the multi-file build. |
| v1 S3 with the default ring (938) | Never car-tested. Bigger ring = more catch-up lag; that's what v2's live serving solves. |

## Removed flags (don't look for them)

`FATDISK_NO_TITLES`, `FATDISK_NO_BUFFER_FILES`, `NAME_TEST`, `EARLY_END_FAT`,
`EARLY_END_READ_ERROR`, `EARLY_END_NO_FAT`, `FATDISK_BUFFER_FILE_BYTES`: removed 2026-09-25 with
the song-name features. The Kenwood reads a file's size and cluster chain only when it opens it,
so a playing file can't be ended early, and a long file name made it reject files.
