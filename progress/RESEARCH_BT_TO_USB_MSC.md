# Research batch: BT-to-fake-USB-MP3 prior art (2026-09-15 ~22:15 GMT-3)

Seven parallel research agents dispatched to independently verify a large pasted
"research" writeup (from a different AI session) and dig into open questions.
Everything below is sourced; nothing here is taken at face value.

## 1. The pasted research writeup — fact-check verdict

| Claim | Verdict |
|---|---|
| GitHub discussion #566 title/content/Schatzmann quote | **VERIFIED**, verbatim accurate |
| Dension DAB+U is a Bluetooth product proving BT→fake-USB-MP3 works | **FALSE premise** — DAB+U is a DAB *radio tuner*, not Bluetooth. Dension's actual BT car kits use CD-changer/MDI bus emulation, a completely different mechanism, not fake-USB-MSC files |
| 2014 BeagleBone StackOverflow question | **FALSE** — could not be found anywhere; likely fabricated |
| 2019 Raspberry Pi Zero W forum thread | **FALSE citation** — the cited site is a spam/content-farm mirror that redirects to a tracking URL, not a real RPi forum. A **real** 2022 RPi forum thread on this exact problem does exist, just misattributed (wrong year, wrong site, wrong product) |
| TinyUSB `tud_msc_async_io_done()` / BUSY-async read support | **VERIFIED**, real API — but the pasted text omits a real, serious caveat (see §2) |
| "This has been done before, multiple times" | **Not supported** — no working implementation was found anywhere, just discussions and one commercial product (DAB+U) solving a *different* problem (DAB radio, not BT) with a similar trick |

**Bottom line on the pasted text:** a few real anchors (the GitHub discussion, the DAB+U product, the core TinyUSB API) wrapped in confident language that outran the evidence — one fabricated source, one fake site presented as a citation, and the load-bearing claim ("Dension proves BT-to-USB-MP3 works") rests on a mixed-up premise.

## 2. TinyUSB — what's actually implementable

- `tud_msc_read10_cb()` really does support partial reads, `TUD_MSC_RET_BUSY`, `TUD_MSC_RET_ASYNC`, and `tud_msc_async_io_done()` — confirmed directly in current TinyUSB source. This is the right primitive for "sector not ready yet, fill it from the BT ring buffer, signal completion later."
- **Real caveat the pasted text missed:** [hathach/tinyusb#2035](https://github.com/hathach/tinyusb/issues/2035), filed by the maintainer himself — `tud_task()` doesn't yield while a read is pending, breaking exactly this slow-source use case. Stayed open **over two years**, only closed Aug 2025. If we build on this, verify which TinyUSB version actually has the fix.
- Blocking inside the read callback is not an option — it stalls TinyUSB's whole event loop. Must use BUSY/ASYNC, never block.
- No clean existing example of a *virtual, dynamically-generated* FAT-over-MSC filesystem was found (only static RAM-disk examples). We'd be building that part from scratch.
- **Untested, project-critical risk:** how long will the real car radio's USB host stack wait on a slow/busy read before giving up? Linux's default SCSI command timeout is 30s, but embedded automotive USB stacks are typically far less patient (often 1-5s) — no vendor number found anywhere. This has to be measured empirically against the real radio, there is no way to look this up.

## 3. Real car head unit USB-MP3 read behavior

- No public protocol capture or teardown of any car radio's USB-MSC read pattern was found — a genuine evidence gap.
- Best available evidence (generic embedded USB-MSC host behavior, not car-specific): hosts typically build their FAT/directory map once at mount and don't rescan it. Rewriting a file's *content in place* (same size, same clusters) is far safer than changing its *size or allocation* — matches our current design choice, not the multi-file alternative (see §5).
- **Useful calibration:** plain USB-MP3-in-car skip/stutter is a well-documented complaint *independent of any live-streaming trick* — several forum threads describe a skip-back-then-recovers pattern nearly identical to what we're hearing, on cheap flash drives with completely static files. This means some of what we're hearing may be baseline flakiness of this product category, not proof our design is broken.

## 4. Existing prior art — does anyone actually run this combination?

- Zero GitHub code-search hits combining `BluetoothA2DPSink` with TinyUSB/USB-MSC.
- The closest real attempt is the same Jan 2023 GitHub discussion — abandoned at the idea stage specifically because of ESP32 RAM/CPU budget concerns running A2DP decode + MP3 encode + USB MSC concurrently. No implementation ever followed.
- Adjacent real (and real-but-struggling) projects exist for feeding audio into old car head units via *other* protocols: `esp32-cdc-faker` (CD-changer bus, stalled/archived), `ESPer-CDP` (ATAPI CD-ROM emulation + A2DP + internet radio, "doesn't crash most of the times"). None use USB-MSC.
- No prior art anywhere for the specific "reader catches up and re-reads a stale, not-yet-refreshed ring slot" bug we found and fixed ourselves this session — closest analog is a different mechanism (STM32 USB-Audio clock-domain sync bug).
- **This project may be genuinely first-of-its-kind if it ships.** That cuts both ways: no one to learn from, but also nothing to compare unfavorably against.

## 5. Single mutating file vs. multiple rotating files — architecture decision

Directly researched as an alternative to our current ring-buffer-in-one-file design.

**Verdict: the multi-file approach is likely LESS robust, not more.** It doesn't remove the race, it relocates it — from "byte offset within one file" to "directory-entry visibility for a new file" — and host directory-caching-at-mount (never rescanning) is the *more* consistently documented failure mode across real car head units. New files appearing mid-session risk being silently invisible until eject/reinsert, and cross-file transitions add a new, systemic gap/click risk that head units of this era handle inconsistently.

**Concrete recommendation instead** (from a real EEVblog engineering thread on this exact problem): the disk-serving side should track the **host's most recently read LBA** and pace its own ring writes to always stay a safe distance ahead of *that*, not just write continuously on a fixed schedule. This is the real fix our current design is missing — it validates the "car_sim.py is frozen, fix upstream" rule already adopted: the intelligence belongs entirely in the disk-serving layer (eventually the real S3/whatever board), which is the only side that can know both "what's fresh" and "where the reader actually is."

## 6. ESP32-A2DP itself — a concrete, source-verified lead

Real, non-theoretical finding for when we resume hands-on debugging:

- `BluetoothA2DPSink`'s managed-decoder path does `xQueueSend(codec_raw_queue, &audio_buf, 0)` — **timeout 0, non-blocking**. If our firmware's consumer is momentarily too slow to drain that queue, the library **silently drops the whole packet**, logging only a warning (`"managed decode raw queue full, dropping packet"`). This is a real, verified mechanism that could produce exactly the skip/stutter symptom we're hearing, and it lives entirely on the firmware side — nothing to do with any of the Python we've been rewriting.
- Several real GitHub issues (#370, #168, #261) describe similar stutter; the maintainer's own diagnoses land on RF/antenna weakness or BT-task CPU priority starvation, not library bugs — never conclusively resolved in most cases.
- No evidence anywhere of a *progressively worsening over session length* pattern specifically — every documented root cause is immediate/static, not cumulative. Our own "getting worse and worse" symptom isn't explained by anything found here; that specific shape still needs its own investigation (we already ruled out ESP32 heap fragmentation — it was flat).

## Net takeaways

1. The pasted research oversold its case — real technique, weaker evidentiary chain than presented.
2. TinyUSB's async MSC API is real and is the right building block, but has (or had) a real yield-stall bug, and host timeout tolerance is a completely unknown, un-lookupable risk that can only be tested on the real radio.
3. Stick with the single mutating-file design — the alternative is worse, not better — but the fix it's actually missing is **reader-position-aware pacing on the write side**, not anything in `car_sim.py`.
4. There's a concrete, real, previously-unexamined firmware-side lead (`codec_raw_queue` silent packet drop) worth checking directly against our own encoder loop's timing before assuming any more BT/RF-level explanations.
