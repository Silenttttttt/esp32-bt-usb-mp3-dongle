# Car session handoff (written 2026-09-25 ~01:50 GMT-3 on the desktop)

Read this first, then `CLAUDE.md` (build flags, flashing, logger modes) and the last few
sections of `progress/STATUS.md`.

## Goal of the car session

Find out **how the real Kenwood actually reads the S3's disk**. The 2026-09-25 car test proved
most of the bench assumptions wrong (`car_sim.py` was built on guesses). Capture the real
access pattern, then redesign the multi-file / song-name / button-detection features, and
car_sim, from that trace. Don't tune anything from the bench until then.

## What the car test showed (S3 build `d8726ed`, classic with ENCODE_ON_S3)

| Area | Result |
|---|---|
| Live streaming (`FATDISK_ALWAYS_SERVE_LIVE`) | **Works.** First time in the car. ~10 s latency (bench ~1-5 s). Keep this path. |
| First plug-in | Radio showed "N/A device"; unplug/replug fixed it |
| Next on the radio | "unsupported file" error, then it jumped to file 3 |
| On file 3 | Next doesn't go forward |
| Long file names (VFAT LFN) | Not shown |
| Button detection (S3 file-switch -> RADIO_CMD relay) | Never fired |

Things to explain with the trace, not guess: what "N/A device" was (enumeration timing? a
SCSI command we answer wrong?), what makes a file "unsupported" (the 80 KB buffer files? the
FAT chain? the MP3 header at byte 0?), why Next stops at file 3, whether it reads LFN
entries at all, and where the extra ~5 s of latency comes from (radio read-ahead?).

## Capture plan (nothing built yet)

The S3 has two USB ports: native USB goes to the radio (MSC), the debug USB-serial goes to
the laptop. So the S3 can log everything the radio does while the laptop records it:

1. Add an opt-in flag (e.g. `MSC_TRACE`) that logs each MSC callback on the debug serial,
   compactly: INQUIRY / TEST_UNIT_READY / READ_CAPACITY / MODE_SENSE / REQUEST_SENSE /
   READ10 (LBA, length, time), plus which region each read hits (boot, FAT, root dir, file
   N + offset). Watch the serial bandwidth -- READ10 is many calls per second; aggregate
   runs of sequential reads into one line.
2. Record with `logs/serial_logger.py --mode line --baud 115200` on the S3 debug port.
3. In the car, capture separately: cold plug-in/mount, playback start, Next, Back, the
   "unsupported file" moment, and a natural file end.
4. Redesign from the trace. Rebuild car_sim to match what the Kenwood really does.

## Hardware

- Classic ESP32 (BT A2DP sink + AVRCP, sends PCM with ENCODE_ON_S3), USB serial number
  `5B52096812`. Logger: `--baud 2000000 --mode framed` (not 115200 line mode; that's garbage).
- ESP32-S3 (USB MSC FAT12 disk + MP3 encoder with ENCODE_ON_S3), USB serial `5CE5146685`.
  Logger: `--baud 115200 --mode line`.
- `/dev/ttyACM*` numbers on the laptop will differ: always pick by
  `/dev/serial/by-id/*<serial>*`.
- The desktop also has the Fin-ESP board (`5B07008126`); it isn't part of this and stays at home.

## Laptop setup done from the desktop (2026-09-25)

- Repo rsynced to `~/Documents/Computarias/digispark-msc` (with `.git`, at `cb8d283`;
  logs not copied). `sim/.sudo_password` is included (git-ignored, never commit it).
- Arduino libraries copied: audio-tools (with the local BaseConverter.h `int32_t` patch that
  CLAUDE.md describes), codec-shine, ESP32-A2DP, libhelix, libLAME, Adafruit_NeoPixel.
- esp32 core 3.3.11 installed (the laptop had 3.3.0; the repo is built/tested on 3.3.11).
- `python-pyserial` installed. User is in `uucp` (serial access).
- The desktop remote exists on GitHub; nothing has been pushed. Ask Muni before pushing.
  To bring laptop commits back: `git bundle` or rsync, or push if Muni says so.

## Working rules (Muni's)

- car_sim exists to emulate what the real radio does. Only change it to match observed radio
  behavior, never to make a test pass.
- No mouse automation for tests. Use the signal hooks (SIGUSR1 = Next, SIGUSR2 = Back to
  the car_sim GUI process) or other CLI/API ways.
- Never disable AVRCP. Commit as you go; don't push without asking.
- The deployed system must never need manual intervention.
- No long passive wait-and-watch tests; keep working actively.
- Kill processes carefully: `pkill -f`/`pgrep -f` with the plain pattern matches the tool's own
  shell (exit 144). Use `[c]har` patterns, and keep the kill in its own call.
- Before flashing a board, stop its serial logger first. Confirm the port's serial number.
- Times shown to Muni in GMT-3.
