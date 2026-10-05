# digispark-msc — BT-to-USB-MP3 car radio bridge

Public repo: `Silenttttttt/esp32-bt-usb-mp3-dongle`. Scan for leaks (IPs, usernames, MACs, home
paths, passwords) before every push, and ask Muni before pushing.

**Start here:** `README.md` (what it is, status), `ARCHITECTURE.md` (how it works),
`BUILD_FLAGS.md` (the build), `docs/KENWOOD_RADIO.md` (the real radio, measured),
`docs/HARDWARE_USAGE.md`, `progress/LED_STATUS_TABLE.md`. The development log is
`progress/STATUS.md` (append-only, long). Car session notes: `progress/CAR_SESSION_RESULTS.md`
and `progress/CAR_TRACE_FINDINGS.md`. `progress/MORNING_RUNBOOK.md`, `PLAN_NEXT.md`,
`CAR_SESSION_HANDOFF.md`, `DEEP_AUDIT_*` and `RESEARCH_*` are historical.

## The goal

Stream Bluetooth audio from a phone into Muni's car radio (Kenwood KDC-MP8090U), which only plays
MP3s from a USB stick. The radio is never modified. The radio's own Next/Back must skip tracks on
the phone.

## Current state (v2, in the car since 2026-09-25)

```
PHONE --A2DP + AVRCP--> classic ESP32 --UART 2 Mbaud, mono PCM--> ESP32-S3 --USB mass storage--> RADIO
                             ^------------ UART 9600: RADIO_CMD:next/prev, S3_HB ------------|
```

- Classic ESP32 (`esp32-bt-mp3-test/`): A2DP sink, AVRCP (never disable it), sends mono PCM.
- ESP32-S3 (`esp32-s3-msc/`): Shine MP3 encoder, a FAT12 drive with 3 files `Stream.mp3` of
  44.6 MB (32 KB clusters, ~46 min), served live from a 5.57 MB PSRAM ring ~0.8 s behind the
  newest audio. Detects the radio's file switches and relays Next/Back.
- Delay ~5 s (mostly the radio's own ~3.7 s read-ahead).
- Wiring: classic TX0 GPIO1 → S3 GPIO8; S3 GPIO4 → classic GPIO19; shared GND. Two power
  supplies (the S3's 5V pin can't power the classic: diode D7, see ARCHITECTURE.md).
- v1 (one file, the classic encodes, 2026-09-18) is legacy. Muni: "forget about v1".

32 KB clusters car-verified 2026-09-26. Waiting on a car test: the green "USB suspended" LED, a real
second phone pairing, "Next while the phone is paused resumes".

## Safety-critical: which board is which

`/dev/ttyACM*` / `ttyUSB*` paths change. **Identify boards by USB serial number**
(`/dev/serial/by-id/...`, or `udevadm info -q property -n <dev> | grep ID_SERIAL_SHORT`). The flash
scripts check it.

| Serial | Board | |
|---|---|---|
| `5B52096812` | Classic ESP32, this project | `esp32-bt-mp3-test/flash.sh` |
| `5CE5146685` | ESP32-S3, this project | `esp32-s3-msc/flash.sh` |
| `5B07008126` | **"Fin-ESP", unrelated live infrastructure** | **Never flash, reset or open its port** |

## Machine rules

- Never reboot the PC, never log out, never restart wireplumber or other session services.
- The sudo password is in git-ignored `sim/.sudo_password`. Never commit it.
- `car_sim.py` emulates only the real radio. Change it only to match observed behavior.
- Drive the GUI through APIs/signals (`kill -USR1/-USR2`), not mouse automation.
- `pkill -f` can match its own shell (exit 144). Use the flash scripts' anchored patterns, and
  kill and relaunch in separate calls.

## Build/flash commands

**Always build and flash with the scripts: `esp32-s3-msc/flash.sh` and `esp32-bt-mp3-test/flash.sh`.**
Each board's flags live in ONE list at the top of its script -- don't retype them. A bare
`arduino-cli compile .` silently drops them (it happened twice on 2026-09-24 and reintroduced a
minutes-long delay). The scripts confirm the port's USB serial number, stop that board's serial
logger, pass the git commit in, and append each flash to `logs/flash_history.log`. The firmware
reports what it is at boot: the S3 prints `[s3] build: commit ... flags: ...` on its debug
serial; the classic sends a `BUILD:commit=... ...` control frame. Restart the logger after a flash.

```
esp32-s3-msc/flash.sh [--trace] [--no-upload] [-DEXTRA ...]
esp32-bt-mp3-test/flash.sh [--no-upload] [-DEXTRA ...]
```

**`BUILD_FLAGS.md` is the reference for every flag (intent / expected / observed / status) and
which combinations work.** v2 is the default. `--v1` / `--v1 --s3-encode` still exist for the
legacy single-file build; don't use them unless Muni asks.

**Car build flags (verified in the real Kenwood, 2026-09-25):**

| Board | Flag | What it does |
|---|---|---|
| S3 | `FATDISK_ALWAYS_SERVE_LIVE` | Reads are served from a cursor at the live write edge, not the requested offset. Without it the ring's ~4 min catch-up lag comes back |
| S3 | `FATDISK_MULTI_FILE` | 3 full-length files on the same live stream; Next/Back detection and relay (`RADIO_CMD:next/prev`) |
| S3 | `LED_RAINBOW_PLAYING` | Rainbow LED while playing (Muni's preference) instead of solid green |
| S3 + classic | `ENCODE_ON_S3` | Classic sends mono PCM `Q` frames (5.8 ms each: sequence number + PCM + CRC-16; the S3 drops a corrupted frame and fills the gap, `common/pcm_link_rx.h`) at **2 Mbaud**; the S3 runs the MP3 encoder (`common/mp3_pipeline.h`). **Both boards or neither**: a mismatch means different bauds and the S3 shows blinking WHITE ("link never seen") |
| classic | `V2_ALL` | AVRCP features: radio-button relay, auto resume, near-end auto skip. **Never add `A2DP_DISABLE_AVRC`**: AVRCP is the point; the old crash was heap fragmentation |
| classic | `DIAG_LOOP_DRAIN` | Load-bearing: encode/PCM work stays in `loop()`. A separate task breaks BT connectability (A/B test 2026-09-22) |
| classic | `DIAG_FRAG_TRACE` | `FRAG:` heap line every second |
| classic | `INT2IDX_SIZE=4000` | Shine MP3 encoder's x^(3/4) quantization table: 4000 entries instead of 10000 (~24 KB less RAM). Only matters when the classic encodes (no `ENCODE_ON_S3`) |

**Opt-in flags (not in the car build):**

| Board | Flag | What it does |
|---|---|---|
| S3 | `MSC_TRACE` (`flash.sh --trace`, or `flash_trace.sh`) | Car capture: logs every SCSI command the radio sends (CDB, status, bytes, timing, region hit), bus resets, descriptor/control requests, file switches. Debug serial becomes **921600**. Needs the `-Wl,--wrap` link flags, which the script adds. Run sessions with `logs/car_capture.sh start|mark|replay|status|stop`; `replay` re-prints the first 4096 records since power-on (cold boots with no laptop attached) |
| S3 | `FATDISK_DATA_CLUSTERS=N` | File length in clusters (v2 default 1360, the FAT12 cap with 3 files) |
| S3 | `FATDISK_SECTORS_PER_CLUSTER=N` | Cluster size in sectors (v2 default 64 = 32 KB; 8 = 4 KB otherwise) |
| S3 | `FATDISK_RING_BYTES=N` | PSRAM ring size, independent of the file size (default 5,570,560 B) |
| S3 | `FATDISK_LIVE_TARGET_LAG=N` | Live-serve target lag in bytes (default 12288, ~0.77 s) |

**Removed after the car test (2026-09-25, Muni dropped song names):** phone titles renaming the
files, buffer files, the early end (`force_track_change`, `EARLY_END_FAT`,
`EARLY_END_READ_ERROR`), `FATDISK_NO_TITLES`, `FATDISK_NO_BUFFER_FILES`, `NAME_TEST`. The Kenwood
reads a file's size and cluster chain once, at open, so a playing file can't be ended early, and
a 56-char long file name made it reject files ("unsupported file"). The 3 files keep one fixed
name, `Stream.mp3` (8.3 aliases `STREAM~1..3.MP3`); the radio shows only `F01 T-01` anyway.

**Loggers:** classic `logs/serial_logger.py --baud 2000000 --mode framed` (with `ENCODE_ON_S3`;
921600 without). `--mode line` produces garbage on the classic, which led to hours of wrong
conclusions on 2026-09-24. S3 `--baud 115200 --mode line` (921600 with `MSC_TRACE`).

**Serving the radio's file opens (`fat_disk_shared.h`, 2026-09-25):** a radio opening a file
reads probes (512 KB strides, the last 2 KB), a 40 KB head, then a 58 KB read-ahead burst from
0. Probe reads get silent frames and don't move the live cursor; a read at offset 0 places the
cursor at the file's start (seamless after a natural end; `FATDISK_RADIO_READAHEAD` (60 KB)
earlier after Next/Back/mount, since the radio dropped its read-ahead); re-reads of the same
open get the same bytes. Heartbeat: `[s3] live: underruns= opens= probes=`. Expect 0 new
underruns per Next/Back/mount and ~1 s once per natural file end (the burst after the radio's
1.9-3.5 s pause outruns what's been written; the alternative is replaying heard audio).

**car_sim (`sim/car_sim.py --device auto --gui`) is a model of the real Kenwood (rewritten
2026-09-25 from the car trace; change it only to match observed radio behavior):**
- 2 KB reads; a ~58 KB (~3.7 s) read-ahead, so audio plays ~3.7 s behind the reads (the car
  showed ~5 s end to end; the bench used to be nearly instant).
- Every open re-reads the root dir + FAT, reads the file's start, a probe every 512 KB, the last
  2 KB (ID3v1 tag) and ~40 KB from the start, then plays from 0. Size and cluster chain are read
  only at open.
- Natural end: 1.9-3.5 s pause, then the next file. Next: immediate, drops the read-ahead.
  Back: restarts the file; within 3 s of the open it goes to the previous file (quick double
  Back). Folder wrap on (`--no-folder-wrap` for the radio's default). ~0.5 s silence at each
  new file's start. Remembers the last track across restarts. Mount: peeks the last file's
  first/last 2 KB, then opens the remembered track.
- Display shows `F01 T-0N`; DISP shows the ID3v1 tag or `NO NAME` (never the file name).
- Scriptable: `kill -USR1 <pid>` = Next, `-USR2` = Back.

**Host tests (run after touching `fat_disk_shared.h`):** `esp32-s3-msc/crosscheck/live_serve_test.cpp`,
build line at the top of the file. Its switch-detector part drives a model of the real Kenwood.

When unsure what a board runs: its boot line (above), `logs/flash_history.log`, or `grep -h
customBuildProperties ~/.cache/arduino/sketches/*/build.options.json`.

**A required local library patch lives OUTSIDE this repo and can be silently lost.** The
classic ESP32 firmware will not compile at all without a 2-line patch in
`~/Arduino/libraries/audio-tools/src/AudioTools/CoreAudio/BaseConverter.h` (around line
148-172): `(T)(int)(...)` is ambiguous under this toolchain (esp32:esp32 core 3.3.11 +
audio-tools 1.2.6) when `T` is a class type with multiple integer constructors — fixed by
disambiguating to `(T)(int32_t)(...)`, semantically identical, already applied and documented
inline in that file. **If `arduino-cli compile` for the classic ESP32 ever fails with an
ambiguous-constructor/conversion error mentioning `int24_4bytes_t`, this is why** — the
library was reinstalled/updated and the patch was lost. Reapply it (the exact change is
commented in the file itself, or see `progress/STATUS.md`'s original entry for this fix) before
assuming anything else is wrong.

`PSRAM=opi` is confirmed correct for the real N16R8 module (8MB Octal PSRAM) — don't change it
without checking the actual module's datasheet first if a different board variant ever gets
used. Pins as wired: S3 `UART_S3_RX_PIN` 8 (from the classic's TX0), return TX 4 (to the
classic's GPIO19), RGB LED 48. The silkscreen "RX"/"TX" pins are the S3's console UART, not free
GPIOs; both earlier pin choices failed for that reason.

`esp32-s3-msc-fat16-fallback/` is an unused FAT16 variant from before v2. The Kenwood mounts
FAT12, so it isn't needed.

## Working conventions established this session

- **User prefers direct action over discussion** — when something needs fixing, fix it and
  report what was done, don't present analysis and ask which approach to take unless there's
  a genuine, irreducible ambiguity that documentation/code can't resolve.
- **No long-running empirical tests "to be sure"** when the underlying math/logic already
  proves the point (e.g. ring-size-vs-wrap-frequency is arithmetic, not something needing a
  20-minute soak test). Prefer fast, deterministic logic tests over real-time-paced ones
  wherever the mechanism doesn't actually depend on wall-clock time.
- **Verify claims directly** (compile, run a quick logic test, check the actual log) before
  reporting something as fixed — this session caught multiple real regressions (a
  retry-budget/margin mismatch, a stale audio-routing mute) specifically through adversarial
  self-checking, not by trusting an initial "looks right" assessment.
- **Never confuse an isolated PC-based simulation of "the source pauses" with reality** —
  `SIGSTOP` on a local player process does NOT reliably stop Bluetooth audio delivery
  (confirmed: `bluealsa` can keep sending something underneath a frozen app). Use a genuine
  `bluetoothctl disconnect`/`connect` cycle to test "the source stops sending" scenarios.
