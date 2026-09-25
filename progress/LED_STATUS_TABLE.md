# S3 RGB status LED: current color table

**Keep this in sync with the code.** The authoritative table is the comment in
`esp32-s3-msc/esp32-s3-msc.ino`'s `loop()` RGB status LED block; this file mirrors it.

Last synced: 2026-09-25 (encoder-location-aware link health, S3-encoder-failed state, next/prev flash).

Rows are in priority order (first match wins).

| State | Color | Meaning |
|---|---|---|
| Native USB-OTG port disconnected | solid WHITE | Bench-only: the radio/PC port has no host while the board is powered via the debug port. Can't happen in the car (no power at all there). |
| S3 encoder failed (`ENCODE_ON_S3` only) | fast-blinking RED (250ms) | The S3's MP3 encoder didn't start; no audio can reach the radio. |
| Link to classic never seen | blinking WHITE (250ms) | No audio input from the classic since S3 boot. By default any frame counts; with `ENCODE_ON_S3` only PCM frames count. Also what you see while the two boards run mismatched builds (different link baud). |
| Link to classic lost | blinking CYAN (250ms) | Was receiving, now nothing for 2s+ (classic crash/reset/wire). Same frame rule as above. |
| Return link never confirmed (`FATDISK_MULTI_FILE`) | blinking ORANGE (500ms) | Forward link fine, S3→classic ack never seen since boot. |
| Return link lost (`FATDISK_MULTI_FILE`) | blinking YELLOW (500ms) | Ack was seen, now silent 5s+. Only the radio-button relay is affected. |
| Linked, not paired | solid RED | No phone connected. |
| Resyncing (`FATDISK_ALWAYS_SERVE_LIVE`) | slow-blinking BLUE (600ms) | Live cursor just jumped; bridging with silent frames. Not a fault, brief. |
| Paired, silent | solid BLUE | Phone connected, nothing playing. |
| Paired, playing | solid GREEN, or rainbow with `-DLED_RAINBOW_PLAYING` (current build) | Real audio flowing. |
| Next/prev relayed to phone | 3 WHITE flashes (~0.6s) | Overlay on top of whatever state is showing. |

Severity convention: blink speed encodes severity. Fast (250ms) = no audio possible;
500ms = a minor function lost; 600ms = transient, not a fault.
