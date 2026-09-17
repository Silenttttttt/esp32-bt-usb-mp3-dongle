# PhantomDrive

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

The ESP32-S3 side (the actual USB-MSC device) hasn't been built yet — everything
in `sim/` is a PC-side stand-in used to design and validate the ring-buffer /
FAT12-serving algorithm (`sim/fat12_disk.py`) before porting it to real S3
firmware. See `CLAUDE.md` for the full breakdown of what's real hardware vs.
what's a throwaway prototype.

## Repo layout

- `esp32-bt-mp3-test/` — real, flashed firmware for the classic ESP32 (Bluetooth
  A2DP sink + real-time Shine MP3 encoding).
- `sim/` — PC-side prototypes: `fat12_disk.py` (the core ring-buffer/FAT12 disk
  algorithm meant to be ported to the S3), `s3_sim_serial.py` (desktop stand-in
  for the not-yet-built ESP32-S3), `car_sim.py` (desktop stand-in for the real
  car radio's USB-MSC reads).
- `progress/` — running project log (`STATUS.md`) plus research/audit notes.
- `firmware/`, `vusb-src/`, `cdc-reference/`, `micronucleus-*` — historical
  "Stage 1" proof-of-concept on a Digispark/ATtiny board (V-USB), done before
  the ESP32 phase. Kept for reference only.
- `CLAUDE.md` — project context and working conventions for AI-assisted
  development on this repo.

## Status

Real end-to-end audio has been streamed from a phone, over Bluetooth, through
the ring-buffer/FAT12 simulation, with clean quality (no audible cutout, jaggedness,
or clipping) and working pause/resume. See `progress/STATUS.md` for the full,
detailed history — bugs found and fixed, what's still open (a residual ESP32
Bluetooth-reconnect crash, plus general further real-world hardening as the S3
side gets built).
