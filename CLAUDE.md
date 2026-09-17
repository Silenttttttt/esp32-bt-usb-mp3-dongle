# digispark-msc — BT-to-USB-MP3 car radio bridge

**If the ESP32-S3 board just arrived, start with `progress/MORNING_RUNBOOK.md`** — a single
ordered checklist for bringing it up, pulled together from everything scattered across this
file and STATUS.md.

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
**The physical S3 board has not arrived yet** (expected 2026-09-18) — but its firmware
(`esp32-s3-msc/esp32-s3-msc.ino`) has already been written and extensively verified without
hardware (see the table below and `esp32-s3-msc/crosscheck/`). Confirmed target board (real
purchase listing seen 2026-09-17): ESP32-S3-WROOM-1 N16R8 DevKitC-1 — 16MB Quad flash, 8MB
Octal PSRAM (`PSRAM=opi` build flag, not a guess), dual USB-C, rated -40 to +65°C (a parked car
in direct summer sun can exceed that — mounting placement matters, not a firmware concern).

## What's real hardware vs. what's a PC-side prototype (READ THIS BEFORE "fixing" anything)

| Component | Real hardware, ships as-is | PC-side prototype, gets ported/rewritten, never itself ships |
|---|---|---|
| `esp32-bt-mp3-test/esp32-bt-mp3-test.ino` | ✅ **This is real, flashed firmware** running on real hardware right now. Bugs here are real bugs affecting a real drive. | |
| `sim/fat12_disk.py` (`GrowingFat12Disk`) | | ✅ Prototype — but its **logic** (ring buffer, on-demand sector generation, avoid_straddle margin, unread_protect backpressure) is explicitly meant to be ported near-verbatim into the eventual S3 firmware's `onRead()`/`onWrite()` callbacks. The *algorithm* matters; the Python file itself never ships. |
| `sim/s3_sim_serial.py` | | ✅ **Pure desktop stand-in for the not-yet-built ESP32-S3.** It runs on a PC/laptop, talks to the real classic-ESP32 over USB-serial (standing in for the eventual wired UART link), and serves the FAT12 ring over a fake local TCP "SCSI READ10" protocol. **This will never run in the car long-term** — it exists purely to validate the ring-buffer/backpressure logic before writing real C for the S3. |
| `esp32-s3-msc/esp32-s3-msc.ino` | ✅ **Real S3 firmware, written 2026-09-17, night before the board arrives. PRIMARY — use this one first.** A careful line-by-line port of `fat12_disk.py`'s ring-buffer/FAT12 algorithm into C++, using the ESP32 Arduino core's real `USBMSC` class (TinyUSB) for the actual USB-MSC device role, plus a UART receiver matching the classic ESP32's real wire protocol exactly. **Compiles clean, has NEVER run on real hardware** (no board existed yet when it was written) — but extensively verified without one: `esp32-s3-msc/crosscheck/` has host-only tests proving the FAT12 structures byte-for-byte identical to the Python reference, the ring/backpressure/straddle logic step-for-step identical, real multi-threaded concurrent simulations (real wall-clock timing, hundreds of real seconds, adversarial reader-stall scenarios) showing zero data corruption, a real uint32_t overflow bug found and fixed (see item 10 below), and — strongest check of all — a generated disk image mounts cleanly under **Linux's own real vfat kernel driver** (not just this project's own logic), with a real independent MP3 decoder confirming the file plays. See the firmware's own header comment and `progress/STATUS.md`'s 2026-09-17 entries for what STILL needs real-hardware verification (UART pin assignment, actual TinyUSB read-callback timing under real USB host pressure). | |
| `esp32-s3-msc/fat_disk_shared.h` | ✅ **Real firmware source** — the core disk logic (`build_boot_sector`/`build_fat`/`build_root_dir`/`disk_append`/`disk_valid_bytes`/`disk_read_at`) extracted out of `esp32-s3-msc.ino` into a shared header, `#include`d verbatim by BOTH the real `.ino` and `sim/s3_real_firmware_host.cpp` below (platform differences isolated to a `FATDISK_MUTEX_*` macro layer). One source of truth, not two hand-copies that could drift — see `progress/STATUS.md`'s 2026-09-17 "Real S3 firmware logic now actually running live" entry. | |
| `sim/s3_real_firmware_host.cpp` | | ✅ **PC-hosted stand-in running the REAL firmware's actual disk logic** (via `fat_disk_shared.h`, not a re-implementation) until the physical S3 board arrives — a stronger substitute for `s3_sim_serial.py` while waiting. Talks to the real classic ESP32 over the real serial link (same self-healing reconnect design) and serves `car_sim.py` unmodified over the same `sector_protocol.py` TCP wire format, but deliberately serves reads with NO retry loop (unlike `s3_sim_serial.py`'s `serve_radio()`) — matching what the real TinyUSB-based firmware will actually do, not a more lenient PC-only simulation. Also reports RAM usage against the real S3's actual 512KB SRAM / 8MB PSRAM budget. Run via `sim/run_resilient_real_firmware.sh` (drop-in swap for `run_resilient.sh`). **Still never run on real hardware** — a PC's read/scheduling timing isn't identical to TinyUSB's, so this is the best available substitute, not equivalent to real-board testing. |
| `esp32-s3-msc-fat16-fallback/` | ✅ **Real fallback firmware, written 2026-09-17. NOT primary — only use if the real radio confirms it rejects FAT12.** Research found a genuine, unconfirmed risk: many cheap embedded USB-MSC host stacks only support FAT16/32, not FAT12. This is otherwise identical to the primary firmware (same ring/backpressure/UART logic) but uses FAT16 with 512-byte clusters — the smallest cluster size that minimizes FAT16's ≥4085-cluster-minimum size penalty, still costing a real, meaningfully bigger ~2.2min catch-up-lag bound vs. the primary's ~30s (a genuine tradeoff, not a free fix). Compiles clean; also verified via a real Linux vfat mount. Never run on real hardware, never even considered the default — only reach for this if FAT12 is confirmed rejected. | |
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
   core-affinity change (`set_task_core(0)`) violating a Bluedroid/HCI assumption; reverted,
   dropping the rate to ~15%. **Then apparently actually solved (2026-09-17 night)**: pulled
   the real ESP-IDF source and found the exact assert (`host_recv_pkt_cb hci_hal_h4.c:662`) is
   a heap-allocation failure during a burst of HCI packets — not an ISR-context or
   controller-blob bug. Root cause of the burst: AVRCP traffic during reconnect (already
   identified with explicit user approval back on 2026-09-15 — "AVRCP is irrelevant to the
   actual product" — and a library patch already existed for it, but the `-DA2DP_DISABLE_AVRC`
   build flag needed to actually use that patch had never been included in any of this
   session's build commands). Restored the flag: **0 crashes in two independent 26-cycle real
   stress tests (0/52 total)**, vs. the prior ~15-19% baseline — statistically decisive
   (~0.02% chance of that happening if the rate were unchanged). **This flag is critical, see
   the Build/flash commands section below** — it's easy to silently drop since it lives in a
   build command, not the `.ino` file, which is exactly how it got lost the first time. Also
   fixed a related gap the same night: `avrc_playstatus_callback()`/`avrc_metadata_callback()`
   still used unbounded `portMAX_DELAY` on `send_control()`, the same class of Bluedroid-task
   blocking risk already fixed in `connection_state_changed()` — now consistent (moot while
   AVRCP is disabled, but correct either way). What was ALREADY fixed and verified regardless
   (see item 8 below): the system self-heals from any crash automatically, zero human action —
   the property that actually matters for unattended driving use, now hopefully needed far
   less often.
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
10. A real `uint32_t` overflow bug in the new S3 firmware — a session-lifetime byte counter
    would overflow after ~74.6 hours of continuous operation, silently disabling write-side
    backpressure for ~30s right at that mark. C++-specific (Python's ints are
    arbitrary-precision, so the earlier byte-for-byte cross-check against `fat12_disk.py`
    couldn't have caught it) — found by reasoning through fixed-width arithmetic over a long
    timescale, not by any test failure. Fixed by capping the counter instead of growing it
    forever; caught and fixed a second bug in the first fix attempt (the capping addition
    could itself overflow) before trusting it. See `progress/STATUS.md` 2026-09-17.
11. The S3 firmware's disk logic now actually runs live against the real classic-ESP32 →
    UART → disk → radio pipeline via `sim/s3_real_firmware_host.cpp` (shared source with the
    real `.ino`, see `fat_disk_shared.h` row above) — not just isolated cross-checks. Found the
    real firmware's non-retry `disk_read_at()` does produce zero-fill audio gaps, but ONLY
    during the initial ring-fill/cold-start ramp (26 straddles in the first ~37s), then zero
    over 44+ continuous steady-state seconds — `READ_MARGIN_BYTES` is not under-provisioned
    against currently-measured real timing. See `progress/STATUS.md` 2026-09-17.
12. First real phone test found two real bugs. (a) Phone couldn't pair at all — the classic
    ESP32's `set_auto_reconnect` had the desktop's address (from earlier stress testing)
    persisted in NVS and was busy dialing out to it, starving the phone's incoming connection.
    Fixed with a one-time `a2dp_sink.clean_last_connection();` call, reflashed — **must be
    removed on the next flash**, it would otherwise wipe the phone's own remembered address on
    every future crash/reboot. (b) ~30s delay before hearing any real audio — root cause: the
    car radio reader always starts at file position 0, and a continuously-overwritten ring
    buffer means position 0 can be up to a full ring duration stale; the ring was sized for
    ~30s. Fixed by reducing `DATA_CLUSTERS` in `fat_disk_shared.h` from 117 to 50 (~12.8s),
    applying to the real `.ino` too since it's shared source. See `progress/STATUS.md`
    2026-09-17's "First real phone test" entry.

**Known still-open items**: the reconnect-crash rate (item 5) — believed solved (0/52 in
testing) as of 2026-09-17 night, but 52 cycles isn't infinite and this was tested via the
desktop as a BlueZ-based BT source, not a real phone yet (see `progress/STATUS.md` for the
full writeup and methodology caveats) — worth re-confirming with real-phone use before fully
trusting it's gone; **⚠️ a real, unconfirmed risk found via research (2026-09-17): many cheap
embedded USB-MSC host stacks (exactly the class of thing in a cheap car radio) only fully
support FAT16/FAT32 and may reject FAT12 entirely** — this project's whole volume is FAT12
(chosen for the small ~470KB/~30s declared size, which FAT16 can't do at any reasonable
cluster size — FAT16 needs ≥4085 clusters). Not confirmed as an actual problem (can only be
tested with the real radio), but if the radio doesn't mount the drive at all, THIS is the
first thing to check — see `progress/STATUS.md`'s dedicated entry for the real tradeoff
(FAT16 would need a much bigger, ~2-minute-catch-up-lag volume) before picking a fix; `/dev/ttyACMx` path
instability + a recurring USB-permission-settle race after re-enumeration (the real ESP32
firmware self-heals its own reboots automatically now — item 8 — but if a PC/laptop is in the
loop as an S3 stand-in, that PC-side script needs to auto-recover too — see
`sim/s3_sim_serial.py`'s `run_serial_bridge()`); an unexplained encode-time/PCM_DROPS
discrepancy between some sessions (low priority); the real ESP32-S3 firmware
(`esp32-s3-msc/esp32-s3-msc.ino`, written 2026-09-17) has never run on real *S3* hardware —
its disk logic has now been exercised live via the PC-hosted `sim/s3_real_firmware_host.cpp`
(shared source, see the table above and `progress/STATUS.md` 2026-09-17), which is the best
available substitute for now, but a PC's read/scheduling timing isn't identical to TinyUSB's —
see its own file header and `progress/STATUS.md`'s bring-up checklist for what still needs
verification once the physical board exists.

## Build/flash commands (for tomorrow, once the S3 board exists)

Classic ESP32 (already flashed, only needed again after a code change):
```
cd esp32-bt-mp3-test
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "compiler.c.extra_flags=-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE -DA2DP_DISABLE_AVRC" \
  --build-property "compiler.cpp.extra_flags=-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE -DA2DP_DISABLE_AVRC" .
# confirm target port's serial number matches 5B52096812 (see Safety-critical section above) before uploading:
arduino-cli upload -p /dev/ttyACM<N> --fqbn esp32:esp32:esp32 .
```
**`-DA2DP_DISABLE_AVRC` is critical, not optional** — it's the fix that took the residual
Bluetooth-reconnect crash rate from ~15% to 0/52 in testing (see item 5/the "MAJOR" 2026-09-17
entry in `progress/STATUS.md`). It's a build flag, not something in the `.ino` file itself, so
it's easy to silently drop when recompiling by hand — **this already happened once** (a
2026-09-15 decision to disable AVRCP was made and even patched into the library, but the flag
never made it into any of this session's actual build commands until it was rediscovered and
restored late on 2026-09-17). If you ever compile this firmware without copy-pasting the exact
command above, double-check this flag is still there.

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

ESP32-S3 (never flashed yet — this is the actual first real upload):
```
cd esp32-s3-msc
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
# confirm the port is actually the S3, not the classic ESP32 or Fin-ESP, before uploading:
arduino-cli upload -p /dev/ttyACM<N> --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
```
`PSRAM=opi` is confirmed correct for the real N16R8 module (8MB Octal PSRAM) — don't change it
without checking the actual module's datasheet first if a different board variant ever gets
used. `UART_S3_RX_PIN`/`UART_S3_TX_PIN` in the .ino are placeholders (GPIO 18/17) — update them
to match whatever pins actually get wired to the classic ESP32's TX0 once that's decided.

ESP32-S3 FAT16 fallback (ONLY if the real radio rejects the primary FAT12 firmware above —
see the FAT12-compatibility risk in Known still-open items):
```
cd esp32-s3-msc-fat16-fallback
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
arduino-cli upload -p /dev/ttyACM<N> --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .
```
Same PSRAM/pin caveats as the primary firmware apply. This is a real, meaningful tradeoff (a
much bigger ~2.2min catch-up-lag bound vs. the primary's ~30s), not a drop-in upgrade — confirm
FAT12 actually failed before reaching for this.

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
