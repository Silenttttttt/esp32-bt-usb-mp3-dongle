# The car radio: Kenwood KDC-MP8090U, as observed

Everything here was seen on the real radio. Sources:
- the FAT12 stick tests (2026-09-17);
- the first end-to-end test (2026-09-18);
- the first multi-file car test (2026-09-25 early);
- the in-car `MSC_TRACE` capture session (2026-09-25, 12:00-14:00 GMT-3), which logged every SCSI
  command the radio sent.

Event-by-event log: `progress/CAR_TRACE_FINDINGS.md`. Raw traces: `logs/car/` (git-ignored). The
bench radio model, `sim/car_sim.py --gui`, and the host test's Kenwood model are built from this
table. Change them only to match new observations.

## USB and disk

| Behavior | What the radio does | Evidence | What the firmware does about it |
|---|---|---|---|
| File system | Mounts FAT12 (and FAT16) | Real FAT12/FAT16 sticks, 2026-09-17 | The S3 serves a FAT12 volume |
| Enumeration | `GET_MAX_LUN`, `INQUIRY`, one `TEST_UNIT_READY`, `READ_CAPACITY10`. Never `MODE_SENSE`, `REQUEST_SENSE` or `PREVENT_ALLOW`. Never writes | MSC trace | - |
| Reads | Always `READ10` of 4 sectors (2 KB) | MSC trace | car_sim reads 2 KB |
| Re-detect after an S3 reset | Sometimes never re-enumerates while VBUS stays on (2 of 3 reflashes; also the first test's "N/A device") | MSC trace, 2026-09-25 | S3 USB auto re-attach: no host after 4 s -> `tud_disconnect()`/`tud_connect()`, retried every 10 s. Worked first try |
| Car off | The radio stays on for a while; USB goes to *suspend*, not disconnect (VBUS may stay on) | MSC trace (`usb_suspend`) | Open question: battery drain with both boards powered from that port |
| Engine start (half key -> on) | USB suspend, bus reset, ~4 s, bus reset, enumerate, re-opens the remembered track | MSC trace 13:43:48 | Works hands-free; no false Next |

## Opening and playing a file

| Behavior | What the radio does | Evidence | What the firmware does about it |
|---|---|---|---|
| Open | Re-reads the root directory (and FAT), reads the file's start, a 2 KB probe every 512 KB, the **last 2 KB**, ~40 KB of the head, then re-reads from 0 with a ~58 KB (~3.7 s) read-ahead burst, then plays | MSC trace | The S3 serves probes as silence without moving its cursor; re-reads of the same open get the same bytes; the burst gets real audio. car_sim does the same pattern |
| Size and cluster chain | Read **once, at open**. Shrinking the size or cutting the FAT chain mid-file is ignored | Title-change test 13:13:42: read straight past the cut | Early-end/rename tricks removed. A playing file can't be ended early |
| Playback pace | 2 KB every ~130 ms (~16 KB/s, real time), keeping ~58 KB read ahead | MSC trace | The S3 serves ~0.8 s behind live; with the radio's 3.7 s buffer that's ~4.5 s of the ~5 s delay |
| Natural end | Reads to the last sector, pauses **1.9-3.5 s** (one `TEST_UNIT_READY`), then opens the next file | MSC trace | Not relayed to the phone; the next file continues the stream seamlessly |
| Next | Immediately re-reads the root and opens the next file; drops its read-ahead | MSC trace | Relayed as `RADIO_CMD:next`; the new file starts 60 KB earlier so the burst is real audio |
| Back | 1st press re-opens the **same** file at 0 (restart). A quick 2nd press opens the previous file | MSC trace 13:18-13:19 | Restart is not relayed; the previous file is relayed as `prev`. (Once, a Back ~52 s into a file went straight to the previous one; not explained) |
| Folder wrap | Off by default: Next is dead on the last file, Back on the first. A menu option enables wrap (Muni turned it on) | Car session | 3 files are enough with wrap on |
| Remembered track | Resumes the last track **number** (from 0) across a replug or power cycle | MSC trace 13:29:26 | car_sim remembers the last track |
| Pause | Stops reading; resumes where it was | Car session | Worked once the long file name was gone |
| Glitch | Once, mid-file, jumped back ~52 KB, probed, re-read ~60 KB, continued (decoder resync?) | MSC trace 13:29:57 | Not modelled |

## Display and names

| Behavior | What the radio does | Evidence | What the firmware does about it |
|---|---|---|---|
| Default display | `F01 T-01` (folder, track number). No name | Car session | car_sim shows the same |
| DISP button | Shows the file's **ID3v1 tag** (title/artist from the last 128 bytes), never the file name | `NAME_TEST` build: `V1 TITLE THREE / V1 ARTIST THREE` | Song names dropped (Muni's decision): the probes get silence, so no tag, and DISP shows no name |
| Long file names (VFAT LFN) | A 56-character long name made it refuse the next file ("unsupported file"); short names are fine | Car session 13:35 | Fixed name `Stream.mp3` (8.3 aliases `STREAM~1..3.MP3`) |

## Delay (phone to speaker)

| Build | Delay | Where it comes from |
|---|---|---|
| v2 | ~5 s | ~0.8 s S3 live lag + ~3.7 s radio read-ahead + Bluetooth/encoder |
| v1 | ~10 s (car, 2026-09-18, 25.6 s ring) | The radio trails the writer by up to the ring length (now ~8.5 min) |
