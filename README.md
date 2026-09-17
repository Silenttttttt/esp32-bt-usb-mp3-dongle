# usb-stick-mp3-via-esp32-a2dp

Stream Bluetooth audio from a phone into an aftermarket car radio that only accepts
USB-stick MP3 playback — no Bluetooth input, no AUX. The radio is never modified or
opened up; instead it's tricked into thinking a real USB flash drive holding one huge
MP3 file is plugged in, and the bytes of that "file" are generated live, on demand,
from whatever's currently playing over Bluetooth.

**See `ARCHITECTURE.md` for full diagrams** — the real end-state hardware/data/power
design, the current PC-based test setup, and the FAT12-vs-FAT16 decision.

> **The classic↔S3 link is real wired UART, not WiFi, with no flag to switch modes.**
> Both firmware images have exactly one transport implementation each, used
> unconditionally today and in the final install — see `ARCHITECTURE.md`'s FAQ section
> for the exact source lines proving this. The only difference between the current
> PC-based test setup and the real S3 board is which physical device sits on the other
> end of that same UART signal: a PC's USB-serial port today, a direct wire to the S3's
> RX pin once it arrives. No code changes either way.

## How it works

```
PHONE --Bluetooth A2DP--> ESP32 (classic) --wired UART/serial--> ESP32-S3 --USB-OTG (as USB-MSC)--> CAR RADIO
        real audio           BT sink +                          presents a FAT12                real hardware,
                              Shine MP3 encode                   "virtual disk" with               never modified
                                                                  one huge declared-
                                                                  size MP3 file;
                                                                  bytes generated
                                                                  on-demand from
                                                                  the live stream
```

A classic ESP32 acts as a Bluetooth A2DP sink and encodes incoming PCM to MP3 in
real time (via Shine). Those encoded bytes get handed off to an ESP32-S3, which
presents itself to the car radio as a standard USB mass-storage device — a "flash
drive" with one MP3 file on it. The trick: the file's declared size is fixed at
mount time (because real USB-MSC host drivers cache the size and never re-check
it), but the actual bytes are served from a live ring buffer that's continuously
overwritten with fresh audio. The radio just keeps reading further into a file
that appears enormous but is really only ever a rolling ~12.8-second window of real
content.

The physical ESP32-S3 board hasn't arrived yet, but its firmware (`esp32-s3-msc/`)
has already been written, and — critically — its actual disk logic is running live
right now as a PC-hosted stand-in (`sim/s3_real_firmware_host.cpp`, sharing real
source with the `.ino` via `fat_disk_shared.h`, not a hand-copied re-implementation)
against the real, already-flashed classic ESP32. **FAT12 has been confirmed
compatible with the real target car radio** via direct physical testing — the single
biggest previously-open risk in this project is resolved before the S3 board itself
has even shown up. See `ARCHITECTURE.md` and `CLAUDE.md` for the full breakdown of
what's real hardware vs. what's a PC-side stand-in, and `progress/STATUS.md` for the
detailed history.

## Repo layout

- `esp32-bt-mp3-test/` — real, flashed firmware for the classic ESP32 (Bluetooth
  A2DP sink + real-time Shine MP3 encoding).
- `esp32-s3-msc/` — real firmware for the ESP32-S3 (USB-MSC device role, FAT12
  primary), written before the physical board arrived. `fat_disk_shared.h` holds
  the core ring-buffer/FAT12 disk logic, shared verbatim with the PC stand-in
  below — not duplicated. `crosscheck/` holds host-only tests proving the logic
  matches the original Python reference, holds up under real concurrent load, and
  — mounted with Linux's own real vfat kernel driver via a loopback device —
  produces a genuinely valid FAT12 volume a real independent MP3 decoder can play.
- `esp32-s3-msc-fat16-fallback/` — a ready, tested FAT16 fallback (its own
  `fat16_disk_shared.h`, same structure), only for if the real car radio ever
  rejects FAT12. Brought to the same real-tested rigor as the primary — live
  PC-hosted test against the real classic ESP32, not just compiled once.
  Not the default; see `ARCHITECTURE.md` for why FAT12 wins on lag.
- `sim/` — PC-side tooling:
  - `fat12_disk.py` — the original Python reference the S3 firmware's ring-buffer
    algorithm was ported from.
  - `s3_real_firmware_host.cpp` / `s3_real_firmware_host_fat16.cpp` — PC-hosted
    stand-ins that compile and run the **real firmware's actual disk logic**
    (shared source with the `.ino` files, not a re-implementation) against the
    real classic ESP32, until the physical S3 board arrives.
  - `car_sim.py` — a genuine FAT12/FAT16 client standing in for the real car
    radio: parses the boot sector, walks the FAT chain, reads sectors exactly
    like a real USB-MSC host driver would. Supports an opt-in
    `--simulate-radio-resume-cache` flag modeling one theorized real-radio
    behavior found during hardware testing.
  - `run_resilient_real_firmware.sh` — runs the S3 stand-in + `car_sim.py`
    together with automatic restart-on-crash.
  - `s3_sim_serial.py` / `s3_sim.py` / `s3_sim_wifi.py` — older, hand-copied
    Python-only prototypes, superseded by the shared-source stand-ins above for
    anything needing fidelity to the real firmware's actual behavior.
- `progress/` — running project log (`STATUS.md`, the detailed session-by-session
  history) plus `MORNING_RUNBOOK.md` (the ordered checklist for when the S3 board
  arrives) and research/audit notes.
- `firmware/` — historical "Stage 1" proof-of-concept on a Digispark/ATtiny
  board (V-USB), done before the ESP32 phase. Kept for reference only; its
  vendored V-USB/DigiCDC/micronucleus dependencies are intentionally not
  checked in here.
- `ARCHITECTURE.md` — diagrams of the real end-state design and the current
  PC-based test setup.
- `CLAUDE.md` — project context and working conventions for AI-assisted
  development on this repo.

## Status

**FAT12 confirmed compatible with the real car radio** (2026-09-17) — the single
biggest previously-unconfirmable risk in this project, tested directly with real
physical USB drives against Muni's actual hardware, independent of the S3 board.
Real end-to-end audio has also been streamed from a real phone, over real
Bluetooth, through the real classic ESP32 and the PC-hosted real S3 firmware
logic, with clean quality and working pause/resume/reconnect. Two real bugs found
during that first real-phone test (a Bluetooth pairing failure, and a ~30s
audio-start delay) were root-caused and fixed. The Bluetooth-reconnect crash that
plagued this project since its first session appears solved (0 crashes across 52
real stress-test cycles, plus 8+ hours of continuous unattended uptime) —
root-caused to a heap-allocation failure during AVRCP-related traffic, fixed by
disabling AVRCP entirely (the phone already owns playback state, so nothing is
lost).

What's still genuinely open — all of it requiring the physical S3 board and/or
real radio to close out, not something more PC-side work can resolve — is listed
in `ARCHITECTURE.md`'s final section: real TinyUSB timing, a real power-draw
measurement, and the final UART pin choice. See `progress/STATUS.md` for the
full, detailed history.
