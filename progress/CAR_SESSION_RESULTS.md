# Car capture session: results and handoff (2026-09-25, 12:00-13:58 GMT-3, on the laptop)

Read this first when you pick the project back up. Detailed event-by-event log with timestamps:
`progress/CAR_TRACE_FINDINGS.md` (33 numbered observations). Raw traces: `logs/car/*/s3.log`
(**on the laptop only**, git-ignored; copy them to the desktop).

## Where things stand

**S3 as flashed right now** (serial `5CE5146685`, built with `esp32-s3-msc/flash_trace.sh`):
```
-DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -DLED_RAINBOW_PLAYING -DENCODE_ON_S3
-DMSC_TRACE -DFATDISK_NO_BUFFER_FILES -DFATDISK_NO_TITLES   (+ the MSC_TRACE -Wl,--wrap flags)
```
**Classic**: unchanged since before the session (ENCODE_ON_S3 build, as in CLAUDE.md).

**Verified working in the car** with that build:
- Live streaming, ~4-min files (3 of them, all full length, fixed name `Stream.mp3`).
- Next / Back relayed to the phone in the right direction, incl. wrap T-03 -> T-01 and T-01 -> T-03
  (needs the radio's folder-wrap menu option, which Muni turned on). A single Back restarts the
  song and is correctly not relayed; a quick double Back goes to the previous file and is.
- Natural file end is not relayed.
- Pause: no more "unsupported file".
- S3 USB auto re-attach after an S3 reset (the radio sometimes never re-enumerates on its own).
- Engine start / car off-on: radio re-mounts, classic reboots and reconnects BT, no hands.

**Commits** (laptop, `main`, NOT pushed -- Muni said ask before pushing; bring them back with
`git bundle`/rsync or push if he says so): `b5926a5`..`6cb6a72` (7 commits after `efb9183`).

## What the Kenwood actually does (the facts car_sim should be rebuilt from)

| Behavior | Detail |
|---|---|
| Enumeration | GET_MAX_LUN, INQUIRY, (TUR), READ_CAPACITY10. No MODE_SENSE, REQUEST_SENSE, PREVENT_ALLOW. Never writes. |
| Reads | Always READ10 of 4 sectors (2 KB). Playback paced at real time: 2 KB every ~130 ms (~16 KB/s). |
| File open | Re-reads the root dir (+FAT), reads the start, probes every 512 KB, reads the **last 2 KB** (ID3v1 tag), then plays from 0 with a ~58 KB (~3.7 s) prefill. |
| Mid-file | Never re-reads the directory or FAT: size and cluster chain are read once, at open. Shrinking the size or cutting the FAT chain mid-file is ignored. |
| Natural end | Reads to the last sector, pauses **1.9-3.5 s** (a TUR in between), then opens the next file. |
| Next | Immediate root re-read + open of the next file, no gap. |
| Back | 1st press: re-opens the same file at 0 (restart). Quick 2nd press: previous file. |
| Wrap | Off by default (Next dead on the last file, Back on the first); a menu option enables folder wrap. |
| Remembers | Resumes the last track number across a replug / power cycle. |
| Display | Shows only `F01 T-01` by default; DISP shows the **ID3v1 tag** (title/artist), not the file name. |
| Long names | A 56-char VFAT long name made it refuse the next file ("unsupported file") from the directory alone. |
| Re-detect | After an S3 reset with VBUS still on, it sometimes never re-enumerates (2 of 3 reflashes). |
| Car off | Radio stays on a while; USB goes to *suspend*, not disconnect (VBUS may stay on). |
| Glitch | Once, mid-file, it jumped back ~52 KB and re-read its buffer (decoder resync?). |

## Code changes made during the session (all committed)

1. `msc_trace.h` + `flash_trace.sh` + `logs/car_capture.sh` -- SCSI-level trace of the radio
   (see CLAUDE.md "Car capture build"). 921600 baud debug serial in this build.
2. **Next relay fix** (`fat_disk_shared.h`, `fatdisk_read_is_playback`): only reads continuing the
   previous one move `g_current_file_read_end`, so the open-time last-sector probe no longer makes
   every Next look like a natural end. Host test case 8 in `crosscheck/live_serve_test.cpp`
   (fails on the old header, passes on the new). Confirmed live.
3. **USB auto re-attach** (`usb_reattach_poll()` in the .ino): no host configured us 4 s after
   boot -> `tud_disconnect()` 0.3/1/3 s -> `tud_connect()`, retry every 10 s. Worked first try.
4. Build flags: `FATDISK_NO_TITLES` (ignore phone titles, fixed short name), `FATDISK_NO_BUFFER_FILES`
   (all files full length), `FATDISK_DATA_CLUSTERS` (file length, 118 = 30 s), `NAME_TEST`
   (ID3 tag test). `sim/bt_pair_laptop.py`: pair the laptop to the classic over BlueZ D-Bus
   (bluetoothctl isn't installed on the laptop).

## Muni's decisions this session

- **Drop song names entirely** (file names and ID3 tags). So: no TITLE renaming, no early end /
  `force_track_change`, no buffer files. The current flags already do this at runtime.
- 3 files are enough (folder wrap on the radio).
- If the radio doesn't re-detect the S3, the S3 re-attaches itself (done).
- The classic stays wired to the car; the laptop is debug-only.

## To do next (in order)

**Done 2026-09-25 afternoon (desktop):** #1 (car build is the default: song-name, buffer-file and
early-end code removed; flags in `esp32-s3-msc/flash.sh` / `esp32-bt-mp3-test/flash.sh`; both
boards report commit + flags at boot and every 60 s; `logs/flash_history.log`), and #5 (car_sim
rewritten as a Kenwood model; see CLAUDE.md). Both boards flashed with `4f05d0e` and bench-checked
with car_sim: Next relayed, single Back not relayed, double Back relayed `prev`, no false Next at
mount.

1. **Make the car build the default**: fold `FATDISK_NO_TITLES` + `FATDISK_NO_BUFFER_FILES`
   into the normal build (or delete the name/buffer/early-end code, since names are dropped),
   keep the auto re-attach, update CLAUDE.md's S3 build command. MSC_TRACE stays opt-in.
2. **Second-device pairing (the "only one device" bug), on the bench**: reproduced -- the laptop
   can't pair (AuthenticationTimeout; GNOME asks to "enter PIN 303657 on Golzin"), BlueZ reports
   `LegacyPairing=true`, and the classic **rebooted during 2 of 3 pairing attempts**. Leads:
   the library's `ESP_BT_GAP_PIN_REQ_EVT` handler never replies with a PIN and
   `self_healing_gap_callback` only answers `CFM_REQ`; SSP may be off in the build. Muni's view:
   the PIN itself doesn't matter -- check that first. Needs the classic's own log (USB).
3. **Classic visibility without a laptop cable**: have the S3 print the classic's 'C' frames
   (BT state, FRAG, reboots) on its debug log, so one cable sees both boards.
4. **Possible false Next at mount**: twice, right after enumeration, the radio opened the
   remembered track and 0.6 s later the next one, and the S3 relayed `next`. Unconfirmed whether
   Muni pressed Next. After the engine start (#28) it did not happen. Check; if real, ignore
   switches in the first few seconds after mount.
5. **Rebuild car_sim from the table above** (2 KB reads, open-time probes incl. last sector,
   1.9-3.5 s EOF pause, Back = restart then previous, no mid-file re-read, remembered track).
   Rule: car_sim changes only to match observed radio behavior.
6. **Crackling/stall 13:45-13:47**: corrupted frames from the classic, then silence, then a
   classic reboot; Muni suspects the classic<->S3 cable. Reseat/replace the wires.
7. **Battery drain check**: car fully off 10+ min -- is the S3 LED still lit (radio USB VBUS on)?
8. Latency: this session's S3 served ~0.7 s behind live and the radio buffers ~3.7 s; the
   ~10 s end-to-end seen before is mostly upstream of the S3 (phone/BT/classic). Not measured today.

## Laptop gotchas

- `python3` is pyenv without pyserial: use `/usr/bin/python3` (the scripts do).
- `flash_trace.sh` stops the S3 logger; restart with `logs/car_capture.sh start <name>`.
- After an S3 reflash the radio may need the auto re-attach (now built in) or a replug.
