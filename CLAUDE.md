# digispark-msc — BT-to-USB-MP3 car radio bridge

## The actual goal

Stream Bluetooth audio (phone) into an aftermarket car radio that **only** accepts
USB-stick MP3 playback — no Bluetooth, no AUX. The radio must never be modified;
everything happens upstream of it, by making it think a real USB flash drive with
one giant MP3 file is plugged in, whose bytes are actually generated live from
whatever's currently playing over Bluetooth.

## Real end-state architecture (what eventually ships)

```
PHONE --Bluetooth A2DP--> ESP32 (classic) --wired UART/serial--> ESP32-S3 --USB-OTG (as USB-MSC)--> CAR RADIO
        real audio           BT sink +                          presents a FAT12                real hardware,
                              Shine MP3 encode                   "virtual disk" with               never modified
                                                                  one huge declared-               (project's whole
                                                                  size MP3 file;                    point)
                                                                  bytes generated
                                                                  on-demand from
                                                                  the live stream
```

**Two separate real ESP32 boards are involved, permanently, by hardware design** (not a
temporary test setup): the classic ESP32 does Bluetooth + encoding; the ESP32-S3 does the
USB-MSC device role, because only S2/S3 chips have the USB-OTG peripheral needed for that.
**The S3 side has not been built yet.** Target board once that work starts: ESP32-S3-WROOM-1
N16R8 DevKitC-1 (16MB flash, 8MB PSRAM, dual USB-C).

## What's real hardware vs. what's a PC-side prototype (READ THIS BEFORE "fixing" anything)

| Component | Real hardware, ships as-is | PC-side prototype, gets ported/rewritten, never itself ships |
|---|---|---|
| `esp32-bt-mp3-test/esp32-bt-mp3-test.ino` | ✅ **This is real, flashed firmware** running on real hardware right now. Bugs here are real bugs affecting a real drive. | |
| `sim/fat12_disk.py` (`GrowingFat12Disk`) | | ✅ Prototype — but its **logic** (ring buffer, on-demand sector generation, avoid_straddle margin, unread_protect backpressure) is explicitly meant to be ported near-verbatim into the eventual S3 firmware's `onRead()`/`onWrite()` callbacks. The *algorithm* matters; the Python file itself never ships. |
| `sim/s3_sim_serial.py` | | ✅ **Pure desktop stand-in for the not-yet-built ESP32-S3.** It runs on a PC/laptop, talks to the real classic-ESP32 over USB-serial (standing in for the eventual wired UART link), and serves the FAT12 ring over a fake local TCP "SCSI READ10" protocol. **This will never run in the car long-term** — it exists purely to validate the ring-buffer/backpressure logic before writing real C for the S3. |
| `sim/car_sim.py` | | ✅ **Pure desktop stand-in for the real car radio.** A real FAT12 client that reads the fake SCSI protocol exactly like a real head unit's USB-MSC driver would — used to verify the disk-serving side behaves correctly, without needing the real radio for every test. **Never ships; the real target is the user's actual physical car stereo.** |
| `firmware/` (Digispark/ATtiny, V-USB) | | Historical "Stage 1" proof-of-concept — the *original* validation that "declare a fixed FAT12 size, serve sectors on demand" works on real USB-MSC hardware at all, done on completely different (AVR) hardware before the ESP32 phase began. Not part of the current pipeline; kept for reference only. |

**The practical consequence, learned the hard way in this session**: hardening
`s3_sim_serial.py`/`car_sim.py` for things like "must survive unattended for hours with zero
human intervention" is only worth real effort to the extent they're *currently* the thing
physically running during a given real-world test session (e.g. a laptop actually in the car
tonight as a stand-in for the not-yet-built S3). It is **not** the same kind of priority as
fixing the real ESP32 firmware, since these Python files themselves are never the shipped
product — the *algorithm* is what gets carried forward, not the file. Don't conflate "this
PC script needs to not crash during tonight's specific test" with "this needs production-grade
resilience as if it were the real target" — ask which one actually applies before spending
significant effort, since the answer changes what's worth building.

## Safety-critical: two ESP32 boards exist, never confuse them

- `/dev/ttyACM*` paths are **not stable** — they can and do change on any reboot/re-enumeration.
  **Always identify boards by USB serial number**, never by path:
  - `udevadm info -q property -n /dev/ttyACMx | grep ID_SERIAL_SHORT`
  - Or use the stable symlinks: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_<serial>-if00`
- **`5B52096812` — the correct board for this project.** Runs `esp32-bt-mp3-test.ino`.
- **`5B07008126` — "Fin-ESP", a completely different, unrelated piece of LIVE INFRASTRUCTURE**
  (financial/weather display + Tuya lamp control for an unrelated project). **NEVER flash,
  reset, or otherwise touch this board.** Before any flash/upload command, confirm the target
  serial number matches `5B52096812`.

## Current status (see `progress/STATUS.md` for the full, detailed session-by-session log)

Six real bugs found and fixed on the real ESP32 firmware + PC-side prototype so far:
1. ~8.5s periodic audio stall — `car_sim.py` wasn't flushing its player's stdin pipe.
2. Torn-read/margin-erosion glitches — `READ_SAFETY_MARGIN`/`MAX_READ_RETRIES` tuning.
3. A PipeWire/WirePlumber audio-mute hazard on the PC test rig — fixed via a distinct
   ffmpeg client identity.
4. ESP32 encoder CPU overload (~1/sec dropped PCM chunks) — fixed via mono downmixing
   before Shine encoding.
5. ESP32 reboots on real Bluetooth reconnect (~35% rate) — root-caused to a core-affinity
   change (`set_task_core(0)`) violating a Bluedroid/HCI assumption; reverted, verified via
   26/26 clean real reconnect cycles. **Residual, smaller crash rate confirmed still present**
   — not fully solved.
6. Ring-wrap MP3 splice + "stale audio replay" on pause/disconnect — fixed via (a) an
   ESP32-side silence injector (`feed_silence_if_no_real_audio()`) that keeps the ring "live"
   through any gap instead of freezing, and (b) real write-side backpressure
   (`GrowingFat12Disk.append(..., unread_protect=True)`) that makes it structurally impossible
   for the writer to ever overwrite content the reader hasn't consumed yet — this was already
   independently identified as the right fix in `progress/RESEARCH_BT_TO_USB_MSC.md` §5 and
   `progress/DEEP_AUDIT_2026-09-15.md` before being implemented.

**Known still-open items**: the residual reconnect-crash rate (item 5 above, not zero);
`/dev/ttyACMx` path instability + a recurring USB-permission-settle race after re-enumeration
(the real ESP32 firmware self-heals its own reboots automatically, but if a PC/laptop is
in the loop as an S3 stand-in, that PC-side script needs to auto-recover too — see
`sim/s3_sim_serial.py`'s `run_serial_bridge()`); an unexplained encode-time/PCM_DROPS
discrepancy between some sessions (low priority); a mono-downmix ESP32 firmware fix for
residual CPU-budget issues is written but unflashed, blocked on an unrelated toolchain/
library compile incompatibility that needs a deliberate decision, not a silent fix.

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
