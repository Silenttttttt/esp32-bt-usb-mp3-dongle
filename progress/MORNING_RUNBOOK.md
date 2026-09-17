# Morning runbook — when the ESP32-S3 board arrives

Everything below is scattered across CLAUDE.md/STATUS.md in more detail; this is the single
ordered checklist to actually follow. If something here contradicts CLAUDE.md, trust
CLAUDE.md (it's the maintained source of truth) and fix this file.

## 1. Before touching anything

- Board should be: ESP32-S3-WROOM-1 N16R8 DevKitC-1 (16MB Quad flash, 8MB Octal PSRAM, dual
  USB-C). If it's a different variant, check `PSRAM=opi` still matches the actual PSRAM type
  before flashing anything.
- Two USB-C ports are expected: one native USB-OTG (for the car radio connection, runs
  `USBMSC`), one UART-bridge (for flashing/serial monitor via `Serial`).

## 2. Flash and do a basic sanity check (classic ESP32 unaffected, don't touch it yet)

```
cd esp32-s3-msc
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
arduino-cli upload -p /dev/ttyACM<N> --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
```
Open the serial monitor (the UART-bridge port) at 115200 baud. Should see:
```
[s3] booting
[s3] FAT12 volume: 940 sectors, declared file size 479232 bytes, first data LBA 4
[s3] ready
```
If instead it says `[s3] FATAL: PSRAM allocation for ring buffer failed`, the PSRAM build
setting is wrong for the actual board — stop and check the datasheet before doing anything
else.

## 3. Validate FAT12/USB-MSC in complete isolation, PC only, no radio, no classic ESP32 yet

Plug the S3's native-USB port into a PC. It should enumerate as a USB flash drive.
- **If it doesn't mount at all**: this is the FAT12-compatibility risk flagged in
  `progress/STATUS.md` manifesting even on a real OS, not just a cheap radio. Worth
  investigating before assuming it's radio-specific.
- **If it mounts**: confirm one file, `STREAM.MP3`, ~468KB, all zeros (nothing's been written
  into the ring yet at this point since nothing is feeding it). Try opening/playing it in a
  normal media player — it may error immediately since the file is a stub of encoded silence
  followed by zero bytes, that's expected before real audio is flowing.

## 4. Wire the UART link and do a real end-to-end PC test

- Wire classic ESP32's UART0 TX → S3's `UART_S3_RX_PIN` (currently GPIO18, a placeholder —
  update the `.ino` constant to match whatever pin actually gets used). Common ground between
  both boards.
- Classic ESP32 keeps running its existing, already-flashed firmware — no changes needed there
  for this step.
- With the S3 plugged into a PC (still not the real radio), pair a phone to the classic
  ESP32 and play music. Confirm the mounted `STREAM.MP3` actually plays real audio when
  reopened/re-read in a media player on the PC.

## 5. Only once step 4 works, connect to the real car radio

- If the radio doesn't mount the drive at all: see the FAT12 risk above — the tested fallback
  is `esp32-s3-msc-fat16-fallback/` (see its own README for the real tradeoff before using it;
  it's a ~2.2min catch-up-lag bound instead of ~30s, not a free swap).
- If it mounts but won't play: check file extension/name expectations for that specific radio
  model (some are picky about ID3 tags, filename case, etc. — untested territory, genuinely
  unknown until tried).
- If it plays: this is the real, actual finish line for the whole project. Test pause/resume,
  a real Bluetooth disconnect/reconnect while connected to the radio, and leaving it running
  for an extended real drive if possible.

## 6. Known, already-fixed-but-real-phone-unconfirmed items worth watching for

- **Bluetooth reconnect crash**: believed solved (0/52 in stress testing overnight via a
  desktop-as-BlueZ-source method, not a real phone) — if it still crashes with a real phone,
  this needs to be revisited; see STATUS.md's 2026-09-17 "MAJOR" entry for the full mechanism.
- **AVRCP is now intentionally disabled** — the phone's own on-screen media controls should
  work fine (phone owns playback state), but the car radio's own physical buttons (if it has
  play/pause/skip buttons for USB playback) will NOT control the phone — this is expected,
  already-approved behavior, not a bug.
- **~15s+ latency from pressing play to hearing audio** — root-caused to real Bluetooth AVDTP
  negotiation overhead, not fixable from this project's side (tried and confirmed via testing
  tonight); this is a known, accepted quality-of-life cost, not a regression to chase.

## 7. If anything doesn't compile

- Classic ESP32 build failing with an ambiguous-constructor error mentioning
  `int24_4bytes_t`: the required `audio-tools` library patch was lost (lives outside the repo,
  see CLAUDE.md's Build/flash section for the exact fix).
- Any build missing `-DA2DP_DISABLE_AVRC`: the crash-rate fix will silently not be in effect.
  Always copy the exact command from CLAUDE.md, don't retype it by hand.
