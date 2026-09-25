# Plan — requested features/fixes

This file tracks things Muni has asked for but not yet fully built. Once implementation starts
on an item, move its writeup into `STATUS.md`'s normal session log and delete/shrink the entry
here — this file should always reflect what's still open, not a historical record.

## Implemented (2026-09-20) — see STATUS.md's "PLAN_NEXT.md implemented" entry for full detail

- **A1** — `AVRC_AUTO_RESUME_ON_RECONNECT`: auto-resume playback once after a fresh AVRCP
  connection if paused (ignition power-blip self-pause workaround).
- **A2** — `AVRC_AUTO_RESUME_EARLY_PAUSE`: auto-resume if paused within 4s of a track starting
  (YouTube attention-check dialog workaround).
- **A3** — `AVRC_AUTO_SKIP_NEAR_END`: auto-skip 7s before a track ends, to hide the real,
  measured ~10s MP3-decoder buffering floor (section F, resolved — see below).
- **B1** — S3-side link-staleness protection: serve zero-fill instead of stale/already-played
  ring content if the classic goes quiet for >2s (real crash/reboot, not a graceful disconnect).
- **B2** — S3-side full-ring boot primer: loop the silence primer at boot until it covers the
  entire declared file, fixing a real "invalid file" cold-boot report.
- **C3** — Physical next/prev button detection via a 3-file disjoint-cluster-range trick,
  S3-side detection + relay to the classic via `RADIO_CMD:next`/`prev` over the existing return
  channel, classic-side listener (`RADIO_CMD_RELAY`) calling `a2dp_sink.next()`/`.previous()`
  (+ auto-resume-if-paused refinement).
- **D** — `V2_ALL` umbrella build flag, auto-enabling every sub-flag above. Bare/default build
  unchanged. Real limitation: the build command must ALSO omit `-DA2DP_DISABLE_AVRC` for AVRCP
  itself to be active — no in-`.ino` preprocessor trick can do that across translation units.
- **E** — Cross-feature auto-command cooldown (1500ms, shared `g_last_auto_command_ms`), so two
  of A1/A2/A3/C3's auto-fired commands can't collide (e.g. a double `next()` skip).
- **F** — RESOLVED: the ~10s delay is a real, separately-measured MP3-decoder buffering floor,
  ring-size-independent — not the ring catch-up mechanism. See STATUS.md's 2026-09-17 "Latency
  investigated and explained with real log data" entry and A3 above.

**Standing open items, not yet done**: A1/A2/A3/C3 need a real phone test — `aplay`-as-source
(used for tonight's implementation/compile/boot verification, since Muni's phone was idle) cannot
produce real AVRCP playstatus/track-change/duration notifications. C3's classic-side listener has
never been tested end-to-end against the S3's real detection logic together on real hardware.

## C1 — Align the ring-wrap splice with real track boundaries, plus accurate on-screen duration

**Status (2026-09-21, redesigned): C1+C2+C4+C6 unified into one mechanism, BUILT, not yet
real-hardware-verified.**

Muni's redesign: use the already-built 3-file rotation (C3) as the track-change signal itself --
force the CURRENTLY-open file to hit a clean early EOF the instant a real track change happens
(AVRCP), so the radio's own reader naturally advances to the NEXT file in rotation, which by then
already carries the new track's name. This lands C1 (splice at a real boundary, not an arbitrary
byte count), replaces C4's whole "precisely-sized transition file as a delay timer" idea (no
separate timer file needed -- just force the EOF whenever appropriate), and gives C6 its answer
too (a phone-initiated change is now detected via the classic's own AVRCP track-change hook, not a
guessed signal).

**Built** (`fat_disk_shared.h`, `FATDISK_MULTI_FILE`-gated): `set_file_declared_size()`/
`set_file_name()` (direct root-dir-entry patches -- short 8.3 name only, not yet real VFAT LFN, see
C2 below), `force_track_change()` (shrinks the current file to "read-so-far + 1 cluster", forcing
EOF soon; renames the NEXT file in rotation), and a suppress-flag so `fatdisk_note_file_read()`
doesn't misread this self-triggered rotation as a genuine physical next/prev button press and
double-relay a command back to the phone (C3's own pre-existing collision risk). Classic side: a
new `RADIO_TRACK_RENAME` flag (added to the `V2_ALL` umbrella) sends a bare `TRACK_CHANGED` signal
on the shared AVRCP track-change dispatcher (same one A2/A3 already use) -- no track name sent yet,
Step 1 is confirming the rotation mechanism itself works. Compiled clean in every combination
(bare, `V2_ALL`, `FATDISK_MULTI_FILE` alone, together) -- zero byte-size change to the bare/default
build on either board.

**NOT yet done**: sending the real track title (needs a name-sanitizing step, arbitrary title text
-> valid 8.3 short name) and the actual VFAT LFN mechanism for a real, non-truncated "Artist -
Title" display (still C2's own separate open item). **NOT yet verified**: whether the real radio
actually auto-advances cleanly on this kind of live, in-session file-size shrink + rotation --
this is exactly the risk flagged below, still needs a real hardware test (bench GUI first, then
the actual car).

**REAL BLOCKER (2026-09-22)**: live-tested this (`V2_ALL`+`RADIO_TRACK_RENAME` on the classic,
`FATDISK_MULTI_FILE` on the S3) against a real phone pairing -- the classic crashed into a real,
active reboot loop within seconds of a successful pair (confirmed via `dmesg`, same signature as
an earlier crash-loop bug tonight). Immediately reverted the classic to the last known-stable bare
build to restore Muni's working setup; the crash itself is NOT yet root-caused. See STATUS.md's
matching entry for full detail and candidate causes. This means `force_track_change()` itself has
STILL never actually run against real hardware -- the crash happens before it gets a chance to
fire. Do not reflash this combination again until the crash is understood.

**Original C1 design notes below, still relevant background**:

**Confirmed by Muni (2026-09-21), directly explains a real GUI bug report** (car_sim.py's elapsed-
time display resetting every ~4 minutes regardless of what real song is actually playing, since
`lap_bytes` only tracks the ring's own fixed-size lap, not real song boundaries): the current fixed
~4-minute ring is fine as the DEFAULT/v1 behavior (gated behind a build flag), but any dynamic/V2
work needs the ring size to actually become dynamic, matching this section's own "declared size
matched the song's real duration" idea below -- not a quick GUI-side fix, this is the real
mechanism this section already describes.

Muni's idea: make the ring's inherent wrap-splice land at actual song boundaries instead of an
arbitrary fixed byte count, so the unavoidable splice happens during a natural break. Named
bonus: if the declared size matched the song's real duration, the radio's own displayed
duration (typically `file_size / constant_bitrate`) would be close to the real song length.

**Mechanism found**: rewrite only the file's 4-byte directory-entry size field (FAT chain/boot
sector untouched) — minimal blast radius. **Real risk**: this exact radio has already been
observed doing directory-identity-level caching (resume-position keyed on volume-serial +
filename) — real, specific evidence it might not tolerate a live size change cleanly (ignore it,
error, or force a disruptive remount). **Recommended path**: test ONLY the live declared-size
change first (Step 1), with wrap timing untouched; only attempt wrap-alignment (Step 2) if Step 1
proves the radio tolerates it. **Real coupling risk if Step 2 is ever built**: resetting
`g_write_pos` to 0 on every track change would reintroduce a FULL ring-catch-up-style delay on
EVERY track change (not just once at cold boot) — a materially worse, recurring version of a
problem this project already fixed as a one-time annoyance. Needs the S3 to actually learn about
a track change first (see transport-gap note below).

**Real transport gap, not yet designed**: the classic→S3 forward link only carries `'A'`
(audio)/`'C'` (control/log) frames today; it's unconfirmed whether the S3's `link_task()` even
parses `'C'` frames for meaning or just logs them. Needs a new message-type convention plus S3-
side parsing before C1 (or anything else needing this signal) can be built.

## C2 — Real "Artist - Title" displayed on the radio's screen

**Status: designed, NOT implemented — deliberately deferred (needs C1's groundwork + real-
hardware caching-behavior confirmation).**

**ID3 is the WRONG path** (confirmed, not just hedged): this project's own history already found
and fixed "Header missing" errors caused by an ID3-tagged silence primer — re-adding per-track
ID3 would reopen that bug, and ID3v2 is a file-HEADER convention only checked at byte offset 0
of a file by virtually every decoder, so it wouldn't even achieve the goal.

**Correct mechanism**: VFAT long filenames (LFN) — pure directory metadata, fully decoupled from
the audio byte stream/frame-boundary math. Not yet built.

**Real open question, unresolved without real hardware**: does this radio cache the entire
directory listing once at mount time, or re-read a file's name fresh each time it's opened? The
same directory-identity-caching evidence noted for C1 applies here too, but doesn't resolve this
specific question — needs a real hardware test (rename a file live, see if display updates
without a remount).

**C5** (sub-item, Muni 2026-09-20): what should the display show before ANY song metadata exists
yet (cold boot, fresh phone connection)? Not designed — decide once C2 is actually being built.

**Refinement, worth adopting regardless of C2's own status**: if C3-style multi-file switching is
active, all 3 files should always share the SAME name (matching whatever's currently playing) —
already implemented this way tonight (see C3 above), ready for whenever C2's actual VFAT-rename
mechanism exists.

**Confirmed design constraint from real-world testing** (Muni, resolved via section F): AVRCP
metadata round-trips fast (seconds); real audio catching up is the ~10s decoder-buffering-floor
cost (section F). This means the correct title/artist will typically be known several seconds
BEFORE the matching audio is actually audible — any placeholder strategy must be tied to when the
AUDIO catches up (now a well-specified ~10s, or "until real audio confirmed flowing"), not just
to when metadata arrives.

## C4 — Use a dynamically-sized "transition" file as a precise delay timer

**Status: idea captured, NOT designed in detail, NOT implemented — deferred pending C1 and a
real per-transition delay re-measurement.**

Muni's synthesis: instead of C3's plain "LOADING placeholder" mitigation, engineer a 4th
"transition" file whose declared size exactly matches the expected phone-side delay before real
audio arrives — using the filesystem's own read-time-per-declared-size mechanics as a built-in,
precise delay timer, landing on the real next-song file right as real audio should be ready.

**Fully dependent on**: C1's dynamic-resize mechanism being tolerated by the real radio (same
core open risk), and the ~10s decoder-floor figure (now known, per section F's resolution) being
stable enough per-transition to size the file against — worth re-confirming this is genuinely
constant before committing to it, since section F's own measurement was for cold-connect/general
delay, not necessarily verified as identical for every ordinary mid-session skip. **New,
unverified radio-behavior assumption**: requires the radio to auto-advance from one file to the
next on end-of-file, not just support manual button presses (C3's assumption) — needs its own
real hardware confirmation. **Detection-logic complication for C3**: needs to distinguish an
engineered self-caused transition-file-end from a genuine new driver button press, so it isn't
double-relayed to the phone — not yet designed.

## C6 — Force the radio to notice a phone-initiated track change via a fake file-end/disconnect

**Status: idea captured for later, NOT designed, NOT implemented.**

The reverse of C3: if the PHONE changes track on its own (not via the radio's buttons), how does
the radio's displayed name (C2) ever update, given a file's display name can't safely change
while the radio is actively reading it? Idea: engineer a signal (end-of-file, a corrupted FAT
entry, a brief USB disconnect/re-enumerate) that makes the radio naturally stop reading and
re-scan, creating a safe window to update the name — reusing C3's multi-file infrastructure.
Real open questions: which specific signal actually gets a real head unit to cleanly re-scan
without a disruptive multi-second "recognizing new device" experience; how this coordinates with
C3's own switch-detection so a phone-initiated forced-switch doesn't get misread as a driver
button press and relayed back to the phone redundantly.

## C7 — Pass real connection/playback status from the classic to the S3's RGB LED

**Status: mostly ALREADY BUILT (2026-09-21) — turned out most of this existed already.**

Muni's idea: the S3's onboard RGB LED shows real, live status at a glance (paired/not paired,
playing, silence, paired-but-paused-or-silent, etc.), sourced from the classic's own real
connection/audio state, never interfering with the S3's own actual job.

**Found already implemented** (before tonight): `status_led` (WS2812, GPIO48,
`esp32-s3-msc.ino`) already shows off (no link in 2s)/breathing green (real audio live)/solid blue
(linked, silent) -- driven by `g_s3_audio_live`, itself set from `AUDIO_LIVE`/`AUDIO_SILENCE`
control messages the classic already sends on every real/silence transition. The `'C'`-frame
transport convention this section used to say was "not yet designed" already exists and already
carries exactly this kind of status message -- that part of the gap was stale.

**Added tonight**: a 4th state, solid RED -- "linked but no phone paired" -- previously
indistinguishable from "paired but silent" (both solid blue). The classic already sends
`BT_CONNECTED`/`BT_DISCONNECTED` on every connection-state change; it just wasn't being parsed on
the S3 side. Added `g_s3_bt_connected`, wired into `link_task()`'s existing `'C'` handling, zero
classic-side changes needed. Compiled clean, reflashed, booted clean -- not yet visually confirmed
(no camera on the LED remotely).

**Follow-up (same night)**: added a real return-link-health state (orange blink -- the S3's own
existing return-channel heartbeat, already echoed back by the classic, just wasn't being listened
for -- but ONLY surfaced when `FATDISK_MULTI_FILE`/C3 actually needs the return channel; a one-way
link is legitimately fine otherwise, per Muni's own correction), a rainbow animation replacing the
old plain breathing-green while playing, and the no-link state changed from off to a fast blinking
red (off was ambiguous with "LED itself isn't working"). Also found and fixed a real bug the same
night: `g_s3_bt_connected` only updated on BT connection-state EDGE transitions, so an S3
reboot/reflash while a phone was already connected left it stuck showing "not paired" forever --
the classic now resends its current state every ~1s, not just on transitions, same "must self-heal
without ever needing a restart" principle as everything else in this project.

**Tried and explicitly dropped (same night)**: a flag-gated (`LED_SONG_COLOR`) audio-reactive
rainbow speed, sourced from a real, cheap loudness signal (avg |sample|, accumulated for free
inside the existing mono-downmix loop, reported once/sec). Built, flashed, live-tested -- Muni's
verdict: it didn't feel meaningfully connected to the song, not worth keeping. Fully removed from
both boards (the `LEVEL:` telemetry, the flag, the speed modulation) rather than left as an unused
option -- back to the plain, fixed-speed rainbow.

**Still open, if wanted later**: AVRCP play-status (`V2_ALL`-gated, needs a real phone AVRCP test
per PLAN_NEXT.md's own standing item) could add a distinct "paired, playing per AVRCP, but audio
not flowing yet" transitional state -- not built, not asked for yet.

## Standing note

Muni said he'll add more requests after this — treat this file as a running list, not final;
append rather than reorganize when new items come in, so nothing gets lost mid-conversation.
