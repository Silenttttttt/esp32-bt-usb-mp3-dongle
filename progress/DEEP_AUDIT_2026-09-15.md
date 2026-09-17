# Deep audit: 65-agent research-grounded code audit + adversarial verification (2026-09-15 ~22:20 GMT-3)

Five dimension-agents audited the whole pipeline (architecture-vs-research, `car_sim.py`,
the disk-serving layer, the ESP32 firmware, and a from-scratch race-condition analysis) with
strict "static reading only, do not execute anything" rules. Every finding then went through
two independent adversarial skeptics (one checking "is this real", one checking "does this
actually matter for the audible glitch symptom") — a finding only survives if a majority of
its skeptics couldn't refute it. 30 candidate findings in, 14 confirmed, 16 refuted.

## The likely actual root cause of the live skip-back/stutter bug

**`sim/fat12_disk.py:79` — critical — concurrency-granularity mismatch.**

`disk_lock` genuinely prevents byte-level tearing *within* any single `append()` or
`read_sectors()` call. But it does nothing about the fact that a **read**'s atomic unit (one
4096-byte cluster) is much larger than a **write**'s atomic unit (one ESP32 BT-audio frame,
~100-500 bytes) — and the two are not aligned to each other at all. Concretely, with the
current ring (81920B / 20 clusters of 4096B each):

- Cluster C1 spans ring bytes `[4096, 8192)`.
- Say `write_pos = 6000`. A 300-byte BT frame arrives; `append()` locks, writes
  `buffer[6000:6300]`, unlocks. `buffer[6300:8192]` (1892 bytes) is still whatever the
  *previous lap* wrote there — real, but old, audio.
- Between BT frames, `receive_from_esp32` spends nearly all its time blocked in
  `find_sync`/`read_exact`, *not* holding the lock. A pending `READ10` for C1 can acquire the
  lock in that gap and copy `buffer[4096:8192]` **whole**: `[4096:6000]` = prior-lap audio,
  `[6000:6300]` = brand-new audio, `[6300:8192]` = prior-lap audio again — a three-way splice
  inside one "atomic" read, at a byte offset that has nothing to do with MP3 frame boundaries.
- Fed straight into `mpg123`'s stdin, this is exactly the kind of corruption that produces
  audible skip/stutter — and it's **not** fixed by making the ring bigger (that only changes
  how *often* reader and writer phase-align badly, not whether the splice is possible).

This is a different, more precise bug than anything we'd previously diagnosed (bitrate
mismatch, stale-ring replay, watermark logic) — all real, all fixed, but none of them explain
a *continuously reproducing* splice like this one. This is a strong candidate for what's
still causing glitches after every other fix.

**The fix**, and it's a small one: `append()` needs to defer overwriting any physical ring
byte until the reader has definitely moved past it — i.e. exactly the "writer tracks reader's
last-read LBA and paces itself" mechanism `RESEARCH_BT_TO_USB_MSC.md` §5 recommends, which the
audit separately confirmed does not exist anywhere in the current code (`fat12_disk.py:157`,
also confirmed independently, not just theorized).

## Other confirmed findings, by priority

| Severity | File:line | Finding |
|---|---|---|
| high | `esp32-bt-mp3-test.ino:143` | `audio_data_callback` silently drops an entire ~23ms PCM chunk when `encode_task` falls behind — zero log, zero counter, nothing on the wire. Makes "glitches after N minutes" undiagnosable from device telemetry alone. |
| high | `esp32-bt-mp3-test.ino:261` | **Correction to something I told you earlier tonight:** the "BluetoothA2DPSink silently drops packets via a non-blocking `codec_raw_queue` send" mechanism from the research does **not** apply to our firmware — this sketch never calls `add_decoder()`, so it uses the library's legacy *synchronous* callback path, not the managed-decoder queue. That specific lead was a dead end; retracting it. |
| medium | `esp32-bt-mp3-test.ino:47` | The same blocking `serial_mutex` (fully blocking, no timeout) guards both audio-frame writes and AVRCP metadata writes. A burst of metadata (long TITLE/ARTIST strings, frequent track changes — the file's own comments confirm this previously correlated with crashes) can stall the audio encode task's frame write, pushing it toward the silent-drop path above. Checkable: do drops cluster around metadata events? |
| medium | `sim/fat12_disk.py:84` | `MAX_FRAME_LEN` (1,000,000 bytes) is 12x the ring capacity (81920 bytes). A single corrupted-but-in-range frame length (the code's own comment already anticipates spliced/corrupted frames as realistic) can trigger the "n >= cap" branch and wipe the *entire* ring's history in one atomic call — a full-ring discontinuity for any reader mid-lap. |
| medium | `sim/s3_sim_serial.py:59` | `receive_from_esp32` thread has no exception handling. A serial-level error kills the thread silently (traceback to stderr only); `serve_radio` keeps serving the now-frozen ring with no indication ingestion stopped — same failure shape as the earlier BT-idle "stale audio" bug, just a different trigger. |
| low | `esp32-bt-mp3-test.ino:141` | PCM chunks over 4096 bytes are silently truncated with no log/counter — this exact bug already caused real corruption once (a 1024-byte guess silently truncated 75% of every chunk) and nothing would catch it if it recurred. |
| low | `sim/sector_protocol.py:41` | `OPCODE_WATERMARK`/`query_watermark()` is now fully dead code (confirmed zero callers anywhere) — should be deleted, not left as a temptation to reintroduce the exact bug it caused before. |
| medium | `sim/car_sim.py:98` | The three startup handshake reads (boot sector/FAT/root dir) run *before* the try/except that's supposed to turn socket timeouts into a clean message — a timeout during startup still crashes with a raw traceback. |
| low | `sim/fat12_disk.py:157` | The simulator's `read_sectors()` always returns immediately — it never exercises the real TinyUSB BUSY/ASYNC deferred-read path our eventual firmware will need (research §2's real, maintainer-filed two-year yield-stall bug). None of tonight's soak-test results say anything about how that will behave. |
| low | `sim/car_sim.py:120` | The `mpg123` subprocess launch is unguarded — a missing/broken binary crashes with a raw traceback instead of the clean diagnostics used elsewhere in the same file. |

## What got refuted, and why (the adversarial pass earned its keep)

16 findings were killed, mostly for the same disciplined reason: **technically true, but
either already tracked as known future work in our own docs, or with no reachable trigger in
any code that actually exists.** Examples: "the writer doesn't track the reader's LBA" was
raised twice as separate findings and refuted both times — not because it's false (it's the
same real gap as the confirmed critical finding above), but because the *specific framing*
("design regression", "missing shared state" as its own standalone bug) overstated it against
what the research doc already flags as forward-looking work, and because the current
fixed-ring-size margin already empirically bounds the practical damage in most cases. A
cluster-chain infinite-loop risk and an unvalidated-cluster-value risk were both refuted after
tracing that our only real FAT12 producer (`GrowingFat12Disk._build_fat`) is a deterministic,
un-corruptible linear chain — real defensive-coding nits, not live bugs. A "the sim's
disk_lock will be a problem when ported to TinyUSB" finding was refuted on a straight factual
error: the lock lives in `s3_sim_serial.py` (PC-simulator glue), not in `fat12_disk.py` (the
part the docstring actually says gets ported).

## Recommended next step

Implement reader-position-aware write pacing in `fat12_disk.py`/`s3_sim_serial.py` — track the
most recent LBA `serve_radio()` has served, and have `append()` refuse to (or defer) writing
into ring positions the reader hasn't passed yet. This directly closes the critical splice bug
above and is the one piece of real, confirmed, unfinished work this audit converged on from
every angle (architecture review, disk-serving review, and the from-scratch race analysis all
landed on it independently).

## Follow-up (2026-09-15 ~22:30-23:40 GMT-3): the critical splice bug fixed, latency root-caused, PCM-drop firmware bug found and partially fixed

**Splice bug: fixed and verified.** `fat12_disk.read_sectors()` gained an `avoid_straddle` check
-- returns `None` if the requested cluster's byte range currently contains the writer's live
`write_pos`, instead of serving a read that would splice two different points in time together.
`s3_sim_serial.serve_radio()` retries (150x, 5ms apart, 750ms budget) before falling back to a
forced read. Verified live: straddles are frequent (up to ~94% of reads need at least one retry)
but **torn reads served stayed at 0** across a real multi-minute session -- the fix works.

**Latency root-caused, NOT what we assumed.** A rigorous click-track + real-recording timing
harness (generate frequency-marked clicks, stream them through the real BT pipeline, record the
actual PC audio output, cross-correlate to find real send-to-play latency) found end-to-end
latency of **~9.5-10.6s**, and proved the ring buffer was never the dominant cause: shrinking it
from 0.08MB (~5.1s) to 0.016MB (~1.0s) barely moved the number. A from-scratch isolation test
(feed mpg123/ffplay the same small-chunk, real-time-paced input pattern car_sim.py uses, with
*zero* Bluetooth/ESP32/ring involved) reproduced the *exact same* ~9.5s latency, while the
identical test with raw PCM instead of MP3 showed only ~2.3s. **Conclusion: MP3 decoders need to
buffer a large number of bytes before committing to play, and since those bytes can only arrive
at the real encoded bitrate (16KB/s @ 128kbps), that accumulation inherently costs several
real seconds -- independent of ring size, retry budget, or any of car_sim.py's logic.** This
matches the research doc's own note that Dension's commercial product needs 15-40s for the same
underlying reason. User's guidance: 5-10s is acceptable *if* everything else is otherwise clean;
latency work was deprioritized in favor of remaining quality issues. Ring size was re-tuned to
0.05MB (~3.2s) for *stability* (fewer write_pos wraps/sec, so straddle-retries fire less often)
rather than for latency, since latency no longer depends on it.

**Real firmware bug found and partially fixed: silent PCM drops.** The audit's firmware-review
lead was correct. Added a `pcm_drops` counter (incremented in `audio_data_callback`'s existing
drop branch) and an `ENCODE_US` per-chunk Shine-encode-time counter, both reported live over the
existing 'C' control channel (no new wire format). Live measurement: **~10.5ms average / ~25ms
worst-case encode time per chunk, ~100-180 chunks/sec, all on CPU core 1** -- i.e. genuinely
~100-105% of a single core's real-time budget, explaining a real, sustained ~1 drop/sec rate.
Reducing `DIAG_LOOP_DRAIN`'s idle `delay(5)` to `delay(1)` helped modestly (~18% fewer drops)
but confirmed the idle gap was never the dominant factor.

**Tried and reverted: pinning `encode_task` to CPU core 0.** Well-evidenced hypothesis (confirmed
via ESP32-A2DP library source: `BluetoothA2DPCommon.h`'s `task_core = 1` default means the
library's own BT_APP task -- which calls `audio_data_callback` -- shares core 1 with Arduino's
`loop()`, where `DIAG_LOOP_DRAIN` was doing all the encode work). Switched to a dedicated
`xTaskCreatePinnedToCore(..., 0)` task instead. **Result: real, reproducible ESP32 crash-loop**
(rebooting every ~1.5s, confirmed via repeated `READY` control events each starting fresh from
`esp32_ms≈1100`). Immediately reverted to the known-safe `DIAG_LOOP_DRAIN` config (with the
`pcm_drops`/`ENCODE_US`/`delay(1)` improvements kept) and confirmed a single clean boot, no
crash-loop, before resuming testing. **This is a materially different experiment from the
DIAG_NO_ENCODE_TASK vs. DIAG_LOOP_DRAIN comparison already in STATUS.md** (that one used an
*unpinned* `xTaskCreate`, letting the scheduler pick freely) -- explicit core-0 pinning is a real,
still-open lead for reducing PCM drops further, but needs real debugging (why does it crash?
stack size on that core? some BT-stack requirement about which core calls back into it?) before
trying again, not another blind retry.

**Currently flashed / running config:** `esp32:esp32:esp32`, flags
`-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE`, `delay(1)` (not `delay(5)`),
`pcm_drops` + `ENCODE_US` telemetry active, ring capacity 0.05MB (`--capacity-mb 0.05`), straddle
retry budget 150x/5ms (750ms). PCM drops still occurring at roughly ~1/sec under real playback --
this is the one still-open, quantified, real (not theoretical) source of residual audio quality
degradation, and the most promising next step is figuring out why core-0 pinning crashes rather
than reducing per-chunk Shine encode cost some other way.
