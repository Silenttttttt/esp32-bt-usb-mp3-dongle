# usb-stick-mp3-via-esp32-a2dp

Stream Bluetooth audio from a phone into an aftermarket car radio that only accepts
USB-stick MP3 playback — no Bluetooth input, no AUX. The radio is never modified or
opened up; instead it's tricked into thinking a real USB flash drive holding one huge
MP3 file is plugged in, and the bytes of that "file" are generated live, on demand,
from whatever's currently playing over Bluetooth.

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
that appears enormous but is really only ever a rolling ~30-second window of real
content.

The physical ESP32-S3 board hasn't arrived yet, but its firmware (`esp32-s3-msc/`)
has already been written and extensively verified without hardware — a careful
C++ port of the ring-buffer/FAT12 algorithm originally designed and validated in
`sim/fat12_disk.py`, cross-checked byte-for-byte against that Python reference and
stress-tested under real concurrent, real-time-paced load (see
`esp32-s3-msc/crosscheck/`). It has never actually run on real S3 hardware yet.
See `CLAUDE.md` for the full breakdown of what's real hardware vs. what's a
throwaway prototype.

## Repo layout

- `esp32-bt-mp3-test/` — real, flashed firmware for the classic ESP32 (Bluetooth
  A2DP sink + real-time Shine MP3 encoding).
- `esp32-s3-msc/` — real firmware for the ESP32-S3 (USB-MSC device role), written
  before the physical board arrived. `crosscheck/` holds host-only tests proving
  its core logic matches the Python reference, holds up under real concurrent
  load, and — mounted with Linux's own real vfat kernel driver via a loopback
  device — produces a genuinely valid FAT12 volume a real independent MP3
  decoder can play. See its own README for what's been verified and what still
  needs the real board.
- `esp32-s3-msc-fat16-fallback/` — a ready, tested fallback using FAT16 instead
  of FAT12, only for if the real car radio turns out to reject FAT12 (a real,
  researched risk with cheap embedded USB-MSC hosts, not confirmed either way
  yet). Not the default — see `CLAUDE.md` for when to actually use it.
- `sim/` — PC-side prototypes: `fat12_disk.py` (the core ring-buffer/FAT12 disk
  algorithm the S3 firmware above is ported from), `s3_sim_serial.py` (desktop
  stand-in for the S3 during PC-only testing), `car_sim.py` (desktop stand-in for
  the real car radio's USB-MSC reads).
- `progress/` — running project log (`STATUS.md`) plus research/audit notes.
- `firmware/` — historical "Stage 1" proof-of-concept on a Digispark/ATtiny
  board (V-USB), done before the ESP32 phase. Kept for reference only; its
  vendored V-USB/DigiCDC/micronucleus dependencies are intentionally not
  checked in here.
- `CLAUDE.md` — project context and working conventions for AI-assisted
  development on this repo.

## Status

Real end-to-end audio has been streamed from a phone, over Bluetooth, through
the ring-buffer/FAT12 simulation, with clean quality (no audible cutout, jaggedness,
or clipping) and working pause/resume, plus auto-reconnect after any crash with
zero human action. The Bluetooth-reconnect crash that plagued this project since
its first session now appears solved (0 crashes across 52 real stress-test cycles,
plus 8+ hours of continuous unattended uptime) — root-caused to a heap-allocation
failure during AVRCP-related traffic, fixed by disabling AVRCP entirely (the phone
already owns playback state, so nothing is lost). See `progress/STATUS.md` for the
full, detailed history and what's still open — a real, unconfirmed FAT12-vs-FAT16
car-radio compatibility question chief among them (a tested fallback exists if
needed), plus final real-hardware validation once the ESP32-S3 board arrives.
