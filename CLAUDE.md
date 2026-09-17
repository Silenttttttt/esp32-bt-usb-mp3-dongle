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
| `esp32-s3-msc/esp32-s3-msc.ino` | ✅ **Real S3 firmware, written 2026-09-17, night before the board arrives.** A careful line-by-line port of `fat12_disk.py`'s ring-buffer/FAT12 algorithm into C++, using the ESP32 Arduino core's real `USBMSC` class (TinyUSB) for the actual USB-MSC device role, plus a UART receiver matching the classic ESP32's real wire protocol exactly. **Compiles clean, has NEVER run on real hardware** (no board existed yet when it was written) — see its own header comment and `progress/STATUS.md`'s 2026-09-17 S3-firmware entry for the full list of what needs real-hardware verification before trusting it (UART pin assignment, actual TinyUSB read-callback chunking behavior, real timing under the no-blocking-in-onRead design). | |
| `sim/car_sim.py` | | ✅ **Pure desktop stand-in for the real car radio.** A real FAT12 client that reads the fake SCSI protocol exactly like a real head unit's USB-MSC driver would — used to verify the disk-serving side behaves correctly, without needing the real radio for every test. **Never ships; the real target is the user's actual physical car stereo.** |
| `firmware/main.c` (Digispark/ATtiny) | | Historical "Stage 1" proof-of-concept — the *original* validation that "declare a fixed FAT12 size, serve sectors on demand" works on real USB-MSC hardware at all, done on completely different (AVR) hardware before the ESP32 phase began. Not part of the current pipeline; kept for reference only. Its vendored dependencies (V-USB, DigiCDC, the micronucleus flashing tool) are intentionally **not** in this repo — they're third-party libraries, not project code; pull them from upstream if this stage is ever revisited. |

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

Real bugs found and fixed on the real ESP32 firmware + PC-side prototype so far:
1. ~8.5s periodic audio stall — `car_sim.py` wasn't flushing its player's stdin pipe.
2. Torn-read/margin-erosion glitches — `READ_SAFETY_MARGIN`/`MAX_READ_RETRIES` tuning.
3. A PipeWire/WirePlumber audio-mute hazard on the PC test rig — fixed via a distinct
   ffmpeg client identity.
4. ESP32 encoder CPU overload (~1/sec dropped PCM chunks) — fixed via mono downmixing
   before Shine encoding. **Flashed and active** (declared mono in `AudioInfo`, downmix called
   in the live `DIAG_LOOP_DRAIN` path) — an earlier version of this doc incorrectly said this
   was "written but unflashed"; don't trust that note if you see it copied anywhere else.
5. ESP32 reboots on real Bluetooth reconnect (~35% rate originally) — root-caused to a
   core-affinity change (`set_task_core(0)`) violating a Bluedroid/HCI assumption; reverted.
   **Residual, real, measured crash rate (~15% per reconnect attempt as of 2026-09-17, likely
   a deeper ESP-IDF/Bluedroid HCI-layer bug, not fixable from application code) — NOT
   eliminated.** What IS fixed and verified (see item 8 below): the system now self-heals from
   this crash automatically, with zero human action, which is the property that actually
   matters for unattended driving use.
6. Ring-wrap MP3 splice + "stale audio replay" on pause/disconnect — fixed via (a) an
   ESP32-side silence injector (`feed_silence_if_no_real_audio()`) that keeps the ring "live"
   through any gap instead of freezing, and (b) real write-side backpressure
   (`GrowingFat12Disk.append(..., unread_protect=True)`) that makes it structurally impossible
   for the writer to ever overwrite content the reader hasn't consumed yet.
7. Growing press-play-to-real-audio delay over a long session (traced to real AVDTP
   negotiation getting slower over time, via new `AUDIO_STATE` instrumentation) — root-caused
   to continuous Arduino `String`-concatenation heap churn in `send_control()` and nearly every
   call site (especially the once-per-second `ENCODE_US` heartbeat); eliminated via `snprintf`
   into fixed stack buffers. Confirmed this did NOT also fix item 5's crash rate (tested
   directly, see `progress/STATUS.md` 2026-09-17).
8. ESP32 wouldn't reconnect after ANY reset (crash, power blip, or a human touching the serial
   port) without a manual phone-side unpair/re-pair — root-caused to `auto_reconnect=false` in
   `a2dp_sink.start()`; fixed by enabling it. **Verified working in isolation**: a real crash
   followed by zero external connect attempts still recovered (`BT_CONNECTED` fired on its own
   ~20s later). This is the fix for the project's original, standing safety requirement
   ("I'll be driving, I won't be able to repair it, or power cycle").
9. A tried-and-abandoned latency fix: priming the ring with pre-encoded silence at mount time
   (targeting a suspected MP3-decoder startup floor) had **zero measured effect** — the real
   delay turned out to live in AVDTP negotiation (see item 7), a completely different layer.
   Left in place since it's harmless, just not doing anything useful.

**Known still-open items**: the residual reconnect-crash rate (item 5, ~15%, likely not
fixable from application code — see `progress/STATUS.md` for the full stress-test writeup and
an important methodology caveat about how that rate was measured); `/dev/ttyACMx` path
instability + a recurring USB-permission-settle race after re-enumeration (the real ESP32
firmware self-heals its own reboots automatically now — item 8 — but if a PC/laptop is in the
loop as an S3 stand-in, that PC-side script needs to auto-recover too — see
`sim/s3_sim_serial.py`'s `run_serial_bridge()`); an unexplained encode-time/PCM_DROPS
discrepancy between some sessions (low priority); the real ESP32-S3 firmware
(`esp32-s3-msc/esp32-s3-msc.ino`, written 2026-09-17) has never run on real hardware — see its
own file header and `progress/STATUS.md`'s bring-up checklist for what needs verification once
the physical board exists.

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
