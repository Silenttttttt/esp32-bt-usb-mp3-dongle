# S3 RGB status LED — current color table

**This file must be kept in sync with the actual code every time the LED logic changes.**
The authoritative implementation lives in `esp32-s3-msc/esp32-s3-msc.ino`'s `loop()`, in the
RGB status LED block (search for "RGB status LED" near the top of the block) — this file is a
convenience mirror of that same table, not a separate source of truth. Whoever edits the LED
code must update both the in-code comment table AND this file in the same change.

Last synced: 2026-09-22 (fast-blinking BLUE resync color fix).

| State | Color | Meaning |
|---|---|---|
| Forward link never seen | blinking WHITE | zero frames from classic since S3 boot — consistent with "off/disconnected from the start" |
| Forward link lost | blinking CYAN | was receiving frames from classic, now silent 2s+ — consistent with a mid-session crash/reset/wire loss |
| Return link never confirmed (`FATDISK_MULTI_FILE` only) | blinking ORANGE | forward link healthy, S3→classic return-channel ack never seen since boot |
| Return link confirmed then lost (`FATDISK_MULTI_FILE` only) | blinking YELLOW | return-channel ack was being seen, now silent 5s+ |
| Linked, not paired | solid RED | both links healthy, no phone currently paired |
| Resyncing (`FATDISK_ALWAYS_SERVE_LIVE` only) | fast-blinking BLUE (150ms) | live-serve cursor just jumped, bridging the discontinuity with real silent MP3 frames instead of torn audio — always brief, resolves on its own. Normal playback should almost never show this; if it does right after a bench-GUI relaunch, that's the GUI's own startup validity scan (deliberately unpaced/fast), not a real fault |
| Linked+paired, silent | solid BLUE | paired, but no real audio flowing (paused/idle) |
| Linked+paired, playing | solid GREEN (default) or rainbow hue-cycle with `-DLED_RAINBOW_PLAYING` | real audio is actually flowing right now |

## Severity/priority notes

- Forward-link states (WHITE/CYAN) always take priority — losing that link is catastrophic (no
  audio possible at all), and it's structurally impossible for the return-link ack to be
  confirmed while forward is down (the ack itself only ever arrives via a forward-link frame).
- Return-link states (ORANGE/YELLOW) only ever fire while forward is proven healthy — losing
  the return link only breaks the physical-button track-skip relay, audio itself keeps working.
- Resync (BLUE fast-blink) sits after "not paired" but before the playing/silent split, since
  audio state is transiently ambiguous during an active bridge.

## History of color changes (so past confusion isn't re-litigated)

- "Playing" was a hue-cycling rainbow (2026-09-21) → removed for "one fixed color, no cycling"
  feedback (2026-09-22) → solid GREEN → rainbow brought back as an opt-in build flag
  (`LED_RAINBOW_PLAYING`) the same night after Muni missed it. Solid GREEN is still the default.
- Forward-link-down states were originally blinking RED/MAGENTA → changed to WHITE/CYAN
  (2026-09-22) once it was established these states always mean "both links down, classic looks
  fully off," not just "one wire has an issue" — needed to look unmistakably different from the
  return-link-only ORANGE/YELLOW pair.
- Resync state was originally a red+blue "violet" → Muni correctly read this as an error/fault
  color (looked like magenta/pink, easily confused with something broken) → changed to
  fast-blinking plain BLUE (2026-09-22), same day it was added.
