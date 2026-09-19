# usb-stick-mp3-via-esp32-a2dp

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Stream Bluetooth audio from a phone into an aftermarket car radio that only accepts
USB-stick MP3 playback — no Bluetooth input, no AUX. The radio is never modified or
opened up; instead it's tricked into thinking a real USB flash drive holding one huge
MP3 file is plugged in, and the bytes of that "file" are generated live, on demand,
from whatever's currently playing over Bluetooth.

## ✅ Status: working end-to-end on real hardware, confirmed on the actual car radio

Real phone → real Bluetooth → real classic ESP32 (A2DP sink + live MP3 encode) → real
wired UART → real ESP32-S3 (USB-MSC device) → **the actual target car radio** (a Kenwood
KDC-MP8090U) — plugged in and played correctly, with clean, continuous audio and roughly
a 10-second delay from power-up to hearing sound. See `progress/STATUS.md` for the full
build history, every bug found along the way, and how each one was root-caused.

**See `ARCHITECTURE.md`** for full diagrams of the real hardware design and the PC-based
bench-test tooling used to validate everything before every real-radio test.

## How it works

```
PHONE --Bluetooth A2DP--> ESP32 (classic) --wired UART--> ESP32-S3 --USB-OTG (as USB-MSC)--> CAR RADIO
        real audio           BT sink +                    presents a FAT12                 real hardware,
                              Shine MP3 encode              "virtual disk" with               never modified
                                                             one huge declared-
                                                             size MP3 file;
                                                             bytes generated
                                                             on-demand from
                                                             the live stream
```

A classic ESP32 acts as a Bluetooth A2DP sink and encodes incoming PCM to MP3 in
real time (via Shine). Those encoded bytes get sent over a wired UART to an ESP32-S3,
which presents itself to the car radio as a standard USB mass-storage device — a "flash
drive" with one MP3 file on it. The trick: the file's declared size is fixed at mount
time (real USB-MSC host drivers cache the size and never re-check it), but the actual
bytes are served from a live ring buffer that's continuously overwritten with fresh
audio. The radio just keeps reading further into a file that appears large but is
really only ever a rolling few-minute window of real content.

Two ESP32 boards are required, permanently, by hardware design — not a temporary test
setup. The classic ESP32 has Bluetooth Classic (A2DP) but no USB-OTG; the ESP32-S3 has
USB-OTG but only BLE, not Bluetooth Classic. Neither chip alone can do both halves of
this.

## Real bugs found and fixed along the way

A representative sample — the full list with root-cause detail is in `progress/STATUS.md`:

- **Ring-wrap frame splitting**: the ring buffer used to byte-split an MP3 frame across
  its physical wrap boundary, corrupting exactly the frame every playback attempt reads
  first (real dumb USB-MSC readers always start at file position 0). Fixed by never
  splitting a chunk across the wrap.
- **ID3-tagged silence primer**: the pre-encoded silence used to prime the ring at boot
  had a stray ID3v2 tag baked in, breaking sync for readers that start at byte 0.
- **Bluetooth reconnect crashes**: root-caused to a heap allocation failure during AVRCP
  traffic on reconnect — fixed by disabling AVRCP (the phone already owns playback
  state, so nothing real is lost). Went from ~15-19% crash rate to 0/52 in stress
  testing.
- **Cold-boot / pause staleness**: a continuously-overwritten ring means whatever's
  sitting at position 0 can be stale by up to a full ring duration. A synthetic-silence
  injector keeps the ring "live" through any pause, and the ring size was tuned
  specifically to bound this worst case.
- **Stale kernel cache in the PC bench-test tool** (not a firmware bug): standard
  buffered reads against the raw USB block device on Linux were being served from a
  stale kernel cache instead of hitting the real device on repeat reads at the same
  offset — made the real, working hardware *look* broken during PC testing. Fixed by
  switching the bench tool to `O_DIRECT`. This bug is structurally impossible on the
  real car radio's own embedded USB host stack, which has no such cache layer.
- **Wrap-point audio stutter**: inherent to any fixed-size looping ring — the byte just
  before wrap and the byte just after it aren't temporally adjacent in the source audio,
  so a sequential reader always audibly splices two unrelated moments together there.
  Not eliminated, just made much rarer by sizing the ring to several minutes instead of
  tens of seconds.

## LED status indicators

Both boards have onboard LED status for debugging without a laptop attached:

- **Classic ESP32** (existing GPIO2 LED): off when not Bluetooth-connected, solid when
  connected but injecting silence (paused/idle), blinking ~1Hz when real audio is
  actively flowing.
- **ESP32-S3** (onboard RGB LED, GPIO48): off when no data has arrived from the classic
  ESP32 recently, solid blue when linked but in silence, breathing green when real audio
  is flowing.

## Repo layout

- `esp32-bt-mp3-test/` — real, flashed firmware for the classic ESP32 (Bluetooth A2DP
  sink + real-time Shine MP3 encoding + LED status).
- `esp32-s3-msc/` — real, flashed firmware for the ESP32-S3 (USB-MSC device role, FAT12
  primary, RGB LED status). `fat_disk_shared.h` holds the core ring-buffer/FAT12 disk
  logic, shared verbatim with the PC-hosted bench-test stand-in below — one source of
  truth, not a hand-copied duplicate. `crosscheck/` holds host-only tests proving the
  logic matches the original Python reference, holds up under real concurrent load, and
  — mounted with Linux's own real vfat kernel driver — produces a genuinely valid FAT12
  volume a real independent MP3 decoder can play.
- `esp32-s3-msc-fat16-fallback/` — a ready, tested FAT16 fallback, only for if the real
  car radio ever rejects FAT12 (it doesn't — FAT12 is confirmed working). Brought to the
  same real-tested rigor as the primary. Never flashed to real hardware; not the
  default.
- `sim/` — PC-side tooling:
  - `real_s3_listen.py` — reads the real, flashed S3's real mounted USB-MSC device
    directly (no root, via UDisks2) and plays it back live on the PC — the tool used to
    bench-test the real hardware before every real car-radio test.
  - `real_s3_passthrough.py` — a pure pass-through transport letting `car_sim.py` (see
    below) talk to the real S3's real disk content, live, with zero simulation.
  - `car_sim.py` — a genuine FAT12/FAT16 client standing in for the real car radio:
    parses the boot sector, walks the FAT chain, reads sectors exactly like a real
    USB-MSC host driver would.
  - `fat12_disk.py` / `s3_sim_serial.py` — the original Python reference implementation
    and a full software simulation of the S3+radio side (talking to the real classic
    ESP32 over serial) — useful for testing the classic ESP32's own BT/encoding pipeline
    without needing any S3 hardware at all.
  - `s3_real_firmware_host.cpp` / `s3_real_firmware_host_fat16.cpp` — PC-hosted programs
    that compile and run the **real firmware's actual disk logic** (shared source with
    the `.ino` files) against the real classic ESP32 — used throughout development
    before every real-hardware milestone, to validate logic changes without needing to
    reflash and re-test on physical boards every time.
- `progress/` — the full, detailed session-by-session development log (`STATUS.md`),
  research notes, and `MORNING_RUNBOOK.md`.
- `firmware/` — historical "Stage 1" proof-of-concept on a Digispark/ATtiny board
  (V-USB), done before the ESP32 phase, kept for reference only. Its vendored
  V-USB/DigiCDC/micronucleus dependencies are intentionally not checked in here.
- `ARCHITECTURE.md` — diagrams of the real hardware design and PC-based bench-test
  tooling.
- `CLAUDE.md` — project context and working conventions for AI-assisted development on
  this repo.

## Build & flash

Classic ESP32 (`esp32-bt-mp3-test/`):
```
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "compiler.c.extra_flags=-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE -DA2DP_DISABLE_AVRC" \
  --build-property "compiler.cpp.extra_flags=-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE -DA2DP_DISABLE_AVRC" .
arduino-cli upload -p /dev/ttyACM<N> --fqbn esp32:esp32:esp32 .
```

ESP32-S3 (`esp32-s3-msc/`):
```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
arduino-cli upload -p /dev/ttyACM<N> --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
```

Wiring: classic ESP32's TX0 (GPIO1) → S3's GPIO8, plus a shared ground wire between the
two boards. If powering the two boards from separate sources (e.g. the S3 from the
radio's own USB port, the classic from a separate 12V accessory outlet), the direct
ground wire between the boards is required regardless — the UART link needs a clean,
short ground reference, not just an indirect one through the vehicle's chassis.

## Known open items

- **USB port power draw was never measured on a real meter** — only estimated on paper.
  If both boards are powered from the same USB port (rather than two separate supplies,
  as used in the first successful real-radio test), verify the port can actually supply
  enough current before relying on it.
- **The wrap-point audio splice is mitigated, not eliminated** — raising the ring size
  makes it rare, not impossible. A future fix could special-case the read logic to avoid
  landing exactly on the splice, but this hasn't been attempted.
- **RGB LED pin (GPIO48) is confirmed correct** on the tested board, but hasn't been
  checked against every possible ESP32-S3-DevKitC-1 hardware revision.
