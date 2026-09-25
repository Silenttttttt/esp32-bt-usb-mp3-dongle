# Kenwood behavior, from the MSC_TRACE car session (2026-09-25, GMT-3)

Raw log: `logs/car/20260925_121628_benchtest2/s3.log` (not committed; copy it off the laptop).
File numbers below are 1-based as on the radio (trace `f0` = file 1).

| # | Event | What the radio did (trace) |
|---|---|---|
| 1 | Hot plug, 13:10:48 | Enumerate, GET_MAX_LUN, INQUIRY, 1 TUR, READ_CAPACITY. No MODE_SENSE, REQUEST_SENSE or PREVENT_ALLOW. Reads boot, root dir, peeks file 3 (start + 80 KB end), then file 1: probes every 512 KB + last sector, reads start, plays file 1 from 0. No errors. |
| 2 | Read pattern | Always READ10 of 4 sectors (2 KB). Open = root + FAT + 512 KB-stride probes + **last sector** + ~40 KB, then re-read from 0 with a ~58 KB prefill (~3.7 s), then real-time 2 KB every ~130 ms (~16 KB/s). S3 serves ~11 KB (0.7 s) behind live. |
| 3 | Title change 13:13:42 -> S3 cut file 1 (FAT chain + size) | **Ignored.** No FAT/root re-read; read straight past the cut. Size and chain are read once at open. EARLY_END_FAT can't work. |
| 4 | Natural end of file 1, 13:14:23 | Reads to last sector, **3.4 s gap** (one TUR), then root/FAT, opens file 2, plays from 0. No error. |
| 5 | NEXT #1, 13:16:13 (file 2 -> file 3 = 80 KB buffer) | Immediate (no gap) root re-read + open file 3. Plays buffer to its end, 3.4 s gap, opens file 1 (wraps). S3 flagged `natural_eof`, **not relayed**. |
| 6 | Phone changed song on its own, 13:16:58 | S3's early end was a no-op (size unchanged). |
| 7 | NEXT #2, 13:17:37 (file 1 -> file 2) | Immediate root re-read + open next file, no gap. Flagged `natural_eof`, **not relayed**. |
| 8 | BACK (restart kind), 13:18:06 and 13:18:08 | Immediate root re-read + re-open of the **same** file at 0. S3 detects nothing (same file). |

## Why button detection never fires (confirmed)

- Every open reads the file's last sector (duration probe) -> `g_current_file_read_end` = file
  size -> every switch looks like a natural EOF -> never relayed. Same cause makes
  `force_track_change()` a no-op (cut >= size).
- `READER_IDLE_RESET_MS` = 3000 < the radio's 3.4 s end-of-file gap -> re-anchors silently.

## Discriminators seen in the trace

- Button (Next/Back): the directory is re-read mid-stream with **no read gap**, then a file is opened.
- Natural end: reads reach the last sector, then a **3.4 s gap** with a TUR, then the open.
- Back (restart): same file re-opened at 0. Next: the following file.
| 9 | BACK to previous = two quick presses, 13:19:02.8 + 13:19:03.0 | 1st: re-open same file 2 at 0. 2nd (0.2 s later): open file 1 (80 KB buffer). S3 detected switch 2->1 and **relayed RADIO_CMD:prev** (first real relay; prev is never flagged natural_eof). Phone title changed 13:19:04.6. Buffer played out, 3.4 s gap, radio opened file 2 (full length) 13:19:08.8. Buffer design works end-to-end for Back. |
| 10 | Muni confirmed | The relayed `prev` reached the classic and the phone went to the previous song. Return path radio -> S3 -> classic -> phone works on the real car. |
| 11 | BACK again, 13:20:00.9 (~52 s into file 2) | Went straight to previous file 1 (buffer) -- no restart first this time. Relayed `prev` again. Buffer played out, 3.3 s gap, opened file 2 at 13:20:06.7. |
| 12 | NEXT, 13:21:08.6 (file 2 -> file 3 buffer) | Same as #7: immediate root re-read, open next file; flagged `natural_eof`, not relayed (the probe bug). |
| 13 | Natural end of file 1, 13:25:12 | 3.5 s gap -> detector re-anchored silently (`anchor file=1`), nothing relayed. Correct, via the >3 s idle reset. |
| 14 | S3 reflashed (NAME_TEST) 13:28:20 while plugged into the radio | Radio **never re-enumerated** (no bus reset / descriptors for 25+ s). The S3's USB dropped and came back on the data lines with VBUS on; the radio didn't rescan. Likely the same mechanism as the first test's "N/A device". |
| 15 | Replug of the radio cable after the reflash, 13:29:26 | Enumerated in ~1 s, resumed on T-02 (remembers the track). 0.6 s into T-02 (a 5 s buffer) it opened T-03; the new Next fix relayed `RADIO_CMD:next` (first Next relay). Whether Muni pressed Next: unconfirmed. |
| 16 | 13:29:57, mid T-03 | Radio jumped back 52 KB (~3 s), probed +18 KB/+34 KB, re-read ~60 KB from there, continued. Looks like a decoder resync. |
| 17 | **NAME_TEST result**: T-03 DISP | Shows **`V1 TITLE THREE` / `V1 ARTIST THREE`** = the **ID3v1 tag** (last 128 bytes, read by the open-time last-sector probe). The long file name `LFN NAME TEST.mp3` is NOT what's shown. Names/titles must come from served ID3 tags, generated per open. |
| 18 | Back x several, 13:30:55-13:31:45 | Every Back that changed file was relayed (`prev` x2, then again at 13:31:38); repeated Back on the same file = restarts, not relayed. |
| 19 | **Next fix live**, 13:32:21.7 and 13:32:22.3 | T-02 -> T-03 -> T-01, both relayed `RADIO_CMD:next`. Fix confirmed on the real Kenwood. |
| 20 | Next 13:32:40.7 (T-01 -> T-02) relayed; T-02 (buffer) natural end 13:32:44.9 | End-of-file gap was only **1.9 s** (so not always > 3 s); the fixed detector flagged it `natural_eof` (read_end = 0x14000 = exact size) and relayed nothing. Correct by the EOF logic, not the idle reset. |
| 21 | 30 s-file build, 13:34:45 | Radio re-enumerated on its own this time (0.5 s after boot). Resumed T-03, then opened T-01 0.6 s later and the S3 relayed `next` -- same pattern as #15 after a replug; unconfirmed whether Muni pressed Next (possible false Next at mount). |
| 22 | **"Unsupported file"**, 13:35:15-13:35:39 | T-01 (30 s) played to its last sector (natural end), radio re-read the root dir and then **never read T-02** -- rejected from the directory alone; 20 s of TUR every 2 s (error on screen), then re-opened T-01. Muni was paused around then. Active song title was 56 bytes -> very long LFN; earlier file ends with 14/22-char titles worked. Suspect: long LFN. |
| 23 | 13:36:10, 13:36:41 | At T-01's end the radio re-opened **T-01** again (not T-02). Either the menu wrap option is "repeat track", or T-02 still rejected. EOFs not relayed (correct). |
| 24 | Reflash 13:37 (4-min files, no buffers, titles ignored) | Radio again did **not** re-enumerate. |
| 25 | **Auto re-attach** build, 13:39:15 | S3: no host 4 s after boot -> `tud_disconnect()` 300 ms -> `tud_connect()`. Radio bus-reset 0.6 s later, enumerated, playing T-01. Works first try; no cable touch needed. |
| 26 | Fixed short name build (titles ignored), 13:41 | Next T-01 -> T-02 relayed, T-02 opened fine. Muni: **pausing works now**, no "unsupported file" -> the long song-name LFN was the likely cause. |
| 27 | Button run 13:42:03-13:43:18 (fixed-name build, wrap setting on) | Next T-02->T-03 relayed; Back = 1st press restarts (not relayed), quick 2nd press = previous file (relayed), incl. wrap T-01 -> T-03 (`prev`); Next wrap T-03 -> T-01 (13:43:13) and T-01 -> T-02 (13:43:16) both relayed `next`. All file changes relayed with the right direction. |
| 28 | Engine start from accessory ("half key" -> on), 13:43:48 | Radio: usb_suspend, bus reset, ~4 s, bus reset, enumerate, opened T-01 13:43:56 and anchored silently -- **no false Next**, no re-attach needed. The classic's PCM stopped for a few seconds at the same moment (cranking dip), then resumed. |
| 29 | Classic outage 13:44:43.5 -> 13:45:05 (~55 s after the engine start) | No PCM from the classic for ~21 s, then silence frames, real audio ~13:45:11 -> looks like a classic reboot + automatic BT reconnect (~27 s total, no hands). Cause (Muni): car went off and was half-keyed again -> the classic lost power; it rebooted and reconnected BT on its own. Radio read nothing 13:44:48-13:45:03 (15 s) -- pause or radio reaction, unconfirmed. Next at 13:44:35 relayed normally before it. |
| 30 | Crackling then stop, 13:45:51-13:47:14 | S3 saw corrupted frames from the classic (5 resyncs, no UART framing/overflow errors) = the crackling; then 13:46:08-13:46:56 no PCM with the line idle (no breaks) except ~5 s of audio at 13:46:34; at 13:46:57 ~900 UART breaks (boot-ROM output = classic reboot), silence frames, real audio from ~13:47:14, clean since. Muni: classic powered normally; "might just be the cable" (classic<->S3 wiring). Unconfirmed: classic log not captured (not on the laptop). |

## Decisions (Muni, 13:33)

- **Drop song names entirely** (file names and ID3 tags): too many side effects for the value,
  even though ID3v1 was shown to work. Consequences for the next build: no TITLE-driven renaming,
  no early end / force_track_change, and the 5 s buffer files no longer have a purpose (they
  only existed so a new name could appear after a skip) -- all 3 files can be full length.
- If the radio doesn't re-detect the S3, the S3 must re-attach itself (done: `usb_reattach_poll()` in the .ino).
- Next only worked from files 1-2 and Back from 2-3 with default settings; Muni found a radio
  menu option that makes the folder wrap around, so **3 files are enough**.

## Next-detection fix (written in the car; flashed 13:28 and confirmed live, #19/#27)

Working-tree edit in `fat_disk_shared.h`: `g_current_file_read_end` only advances on reads that
continue the previous read (or start at 0), so the open-time probes (512 KB strides + last
sector) no longer make every Next look like a natural EOF. `crosscheck/live_serve_test.cpp`
got a Kenwood-probe radio model (case 8) for it. Still to do: build/run the host test against
the old and the new header, then flash and retest Next in the car.

## Screen (Muni, 13:22)
- The radio shows only the index: `F01 T-01` (folder 1, track N). No file name, long or short.
- Lead: every open reads the file's first 2 KB and its LAST 2 KB -- exactly where ID3v2 (start)
  and ID3v1 (`TAG`, last 128 bytes) live. The "duration probe" may be a tag lookup, and the
  radio may show an ID3 title if one is there. Needs checking: does a DISP/text button switch
  to file name / title, and does a stick with an ID3-tagged MP3 show the title?

- 13:25 Muni: DISP exists; on our disk it shows no name at all, on a real stick the full right name.
  Unknown yet whether that "name" is the file name or the ID3 title (our stream has no ID3 tag).
  Muni: on a real stick it shows the file name minus the extension, and a setting swaps the
  artist / song-name order -- so the radio reads ID3 artist+title too (the first/last 2 KB probe).
  Test planned: stick with `FILENAME TEST.mp3` (ID3 title `TAG TITLE TEST`) + `NO TAGS HERE.mp3`.

## Open questions for Muni
- Car fully off: the radio stayed on (Muni: normal for this car). At 13:43:48 the S3 saw a USB
  *suspend*, not a disconnect -> the radio's USB port may keep VBUS on with the car off. In the
  real install both boards (~0.25 A) run from that port: check whether the S3 LED is still lit
  10+ min after the car is fully off (battery drain).
- Is there a DISP / text button that cycles what's shown?
