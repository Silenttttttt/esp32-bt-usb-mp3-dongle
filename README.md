# esp32-bt-usb-mp3-dongle

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

This project streams Bluetooth audio from a phone into a car radio that can only play MP3s from a
USB stick. The radio has no Bluetooth and no AUX input, and it is never modified. Two ESP32 boards
make it think a USB flash drive is plugged in. The MP3 files on that "drive" are generated live
from whatever the phone is playing. The radio's own Next and Back buttons skip tracks on the phone.

It's the project asked for in [`arduino-audio-tools` discussion
#566](https://github.com/pschatzmann/arduino-audio-tools/discussions/566), "Bluetooth AD2P + Mp3
Encoder + USB Mass Storage Class", built and used daily in a real car.

## Status

It works in the target car, a Kenwood KDC-MP8090U, with the v2 build (2026-09-25):

| What | Result |
|---|---|
| Live audio, phone to speaker | Continuous, ~5 s delay |
| Radio Next / Back | Relayed to the phone over AVRCP, including folder wrap. A single Back restarts the song and is not relayed; a quick double Back goes to the previous track |
| Pause on the radio | Resumes where it stopped |
| Car off / engine start | The radio re-mounts the drive, the boards reconnect Bluetooth, nobody has to touch anything |
| Second phone or computer pairing | Works (legacy PIN 0000 or confirm) |

32 KB clusters (3 × ~46 min files) worked in the car on 2026-09-26. Still to test there: the green "USB suspended" LED, pairing a
real second phone, and "Next while the phone is paused resumes playback".

## How it works

```
PHONE --A2DP + AVRCP--> classic ESP32 --UART 2 Mbaud, mono PCM--> ESP32-S3 --USB mass storage--> CAR RADIO
                            ^                                          |
                            +------ UART 9600 baud: RADIO_CMD:next/prev, heartbeat ---+
```

- The **classic ESP32** is a Bluetooth A2DP sink with AVRCP. It downmixes the audio to mono and
  sends the PCM to the S3 at 2 Mbaud. It receives the radio's Next/Back from the S3 and sends
  them to the phone.
- The **ESP32-S3** encodes the PCM to 128 kbps MP3 (Shine) into a 5.57 MB ring in PSRAM. It
  presents a FAT12 USB drive with three files, all named `Stream.mp3`, of 44.6 MB (~46 min) each.
  Whatever offset the radio asks for, the S3 answers from a live cursor ~0.8 s behind the newest
  audio. So the radio always plays "now", while believing it is reading a normal file.
- When the radio switches file, the S3 tells which way it went and relays `RADIO_CMD:next` or
  `RADIO_CMD:prev` to the classic.

Two boards are needed. The classic ESP32 has Bluetooth Classic but no USB device port. The S3 has
the USB port but only BLE. Details and diagrams: [ARCHITECTURE.md](ARCHITECTURE.md).

## Documentation

| File | What's in it |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Data path, wiring, power, the disk layout and live serving, bench tooling |
| [BUILD_FLAGS.md](BUILD_FLAGS.md) | The v2 build, every flag (intent / expected / observed / status), working combinations |
| [docs/KENWOOD_RADIO.md](docs/KENWOOD_RADIO.md) | How the real radio behaves, measured from a USB trace in the car |
| [docs/HARDWARE_USAGE.md](docs/HARDWARE_USAGE.md) | Each board's flash / RAM / CPU / link usage in the car build |
| [progress/LED_STATUS_TABLE.md](progress/LED_STATUS_TABLE.md) | The S3's RGB LED colors |
| [progress/STATUS.md](progress/STATUS.md) | The full development log, every bug and its root cause (append-only) |
| [esp32-s3-msc/crosscheck/README.md](esp32-s3-msc/crosscheck/README.md) | Host tests for the disk logic |
| [CLAUDE.md](CLAUDE.md) | Working notes for AI-assisted development on this repo |

## Build & flash

Always use the flash scripts. Each board's flags live in one list at the top of its script; a bare
`arduino-cli compile .` silently drops them.

```
esp32-bt-mp3-test/flash.sh [--no-upload] [-DEXTRA ...]            # classic ESP32
esp32-s3-msc/flash.sh      [--trace] [--no-upload] [-DEXTRA ...]  # ESP32-S3
```

- The scripts pick each board by its USB serial number. Set `CLASSIC_SERIAL` / `S3_SERIAL` for
  your own boards.
- Each flash is appended to `logs/flash_history.log`.
- Both boards print their commit and flags at boot (the S3 again every 60 s).
- `ENCODE_ON_S3` must be on both boards or on neither. The scripts' defaults already match.
- `--trace` on the S3 logs every SCSI command the radio sends, for car capture sessions.
- The classic needs a small local patch to the `audio-tools` library to compile. See
  [CLAUDE.md](CLAUDE.md).

Toolchain: `esp32:esp32` core 3.3.x, `ESP32-A2DP` (classic), `arduino-audio-tools` with its Shine
MP3 codec (both boards), `Adafruit_NeoPixel` (S3). The S3 board is an ESP32-S3-WROOM-1 N16R8 DevKitC-1
(`PSRAM=opi`).

## Wiring

| From | To | Purpose |
|---|---|---|
| Classic TX0 (GPIO1) | S3 GPIO8 | Audio (PCM), 2 Mbaud |
| S3 GPIO4 | Classic GPIO19 | Return link: Next/Back, heartbeat, 9600 baud |
| Classic GND | S3 GND | Required, even with separate power supplies |
| S3 native USB port | Radio's USB port | The drive (and the S3's power) |
| Classic USB | A 12 V-outlet USB charger | The classic's power |

The S3 can't power the classic from its 5V pin (a diode on the DevKitC-1 drops the voltage), so
the boards use two supplies. See [ARCHITECTURE.md](ARCHITECTURE.md#power).

## LEDs

The S3's RGB LED (GPIO48) shows the whole system's state. In priority order:

| Color | Meaning |
|---|---|
| Solid white | Native USB not connected |
| Slow-blinking green | USB suspended: the radio is off/asleep, or the data wires are loose |
| Fast red | MP3 encoder failed to start |
| Blinking white | No data from the classic yet (check wiring, or mismatched `ENCODE_ON_S3`) |
| Blinking cyan | The link from the classic was lost |
| Blinking orange / yellow | Return link never seen / lost (only Next/Back are affected) |
| Solid red | No phone paired/connected |
| Slow blue | Resyncing |
| Solid blue | Phone connected, no audio |
| Rainbow | Playing |
| Three white flashes | Next/Back relayed to the phone (on top of the current color) |

The full table with causes is in [progress/LED_STATUS_TABLE.md](progress/LED_STATUS_TABLE.md). The
classic's GPIO2 LED is off with no phone, solid when connected but silent, and blinks at ~1 Hz while
audio is flowing.

## Repo layout

| Path | What |
|---|---|
| `esp32-bt-mp3-test/` | Classic ESP32 firmware: A2DP sink, AVRCP relay, PCM link, pairing handling. `bisect1_shine/` and `minimal_a2dp_test/` are old debug sketches |
| `esp32-s3-msc/` | S3 firmware: USB mass storage, the MP3 encoder, LEDs. `fat_disk_shared.h` is the disk logic (FAT12 layout, ring, live serving, Next/Back detection), shared with the host tests and PC stand-ins. `msc_trace.h` is the SCSI trace |
| `esp32-s3-msc/crosscheck/` | Host tests: `live_serve_test.cpp` (live serving against a model of the Kenwood), `lfn_image_test.cpp`, FAT image generators, ring/link cross-checks |
| `common/` | Shared between the boards: `link_protocol.h` (the UART frame format), `mp3_pipeline.h` (the encoder) |
| `sim/car_sim.py` | Bench model of the Kenwood: 2 KB reads, its read-ahead, open probes, end-of-file pause, Next/Back semantics, `F01 T-0N` display with elapsed / length. Plays the real S3 drive through the PC speakers. `--gui`; `kill -USR1` = Next, `-USR2` = Back |
| `sim/bt_*.py` | Bluetooth test tools: a desktop A2DP source, multi-device pairing/switch test, audio rounds through the laptop, laptop pairing |
| `sim/` (rest) | Older PC stand-ins and prototypes (`fat12_disk.py`, `s3_real_firmware_host*.cpp`, `real_s3_listen.py`, ...), kept for reference |
| `logs/` | `serial_logger.py`, `car_capture.sh` (car trace sessions), `flash_history.log` |
| `docs/`, `progress/` | Reference docs and the development log (see the table above) |
| `esp32-s3-msc-fat16-fallback/` | FAT16 variant, unused (FAT12 works on the radio). It predates v2 |
| `esp32-serial-test/`, `firmware/` | Early bring-up sketch and the original Digispark/ATtiny proof of concept |

## Known open items

- The v2 changes listed under Status still need a car test.
- About 1 s of silence once per natural file end (every ~46 min): the radio's read-ahead burst
  after its end-of-file pause outruns the live audio. The alternative would be replaying audio
  already heard.
- Current draw with the car off: USB goes to suspend, but the port may keep powering both boards.
  Not measured.
- `v1` (2026-09-18: one file, the classic encodes, ~10 s delay) is legacy and kept only for
  reference; see [BUILD_FLAGS.md](BUILD_FLAGS.md).
