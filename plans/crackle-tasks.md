# Rough-road crackle: task queue

Started 2026-10-05. Context: progress/STATUS.md (2026-10-05 entries), ARCHITECTURE.md (Power).

| ID | Task | Priority | Depends On | Status | Added | Notes |
|---|---|---|---|---|---|---|
| T1 | Record the exact car build (commit + flags per board) and a one-command way to reflash it | P0 | - | done | 2026-10-10 | From logs/flash_history.log |
| T2 | Put every post-car change behind flags; default build = car build behavior | P0 | T1 | done | 2026-10-10 | CRC/Q frames, concealment, NVS totals |
| T3 | Debug tools: classic + S3 link/BT/timing diagnostics, flag-gated | P0 | T2 | done | 2026-10-10 | loss runs, error classes, BT gaps, loop timing |
| T4 | Bench session: flash, jiggle the micro USB, read diagnostics | P0 | T3, S3 debug port connected | round 1 done, round 2 blocked on S3 debug port | 2026-10-10 | S3 UART port (5CE5146685) not plugged in |
| T5 | Pick the fix from T4's data (FEC 24+8, CRC-32 single-bit fix, or physical only) | P1 | T4 | todo | 2026-10-05 | Muni wants one-way, fixed small delay, no growing delay |
| T6 | Drift fixes: exact 512 B frames, exact-length gap fill | P1 | - | done (flashed, awaiting shake test) | 2026-10-05 | |
| T7 | Push the local commits (ask first) | P2 | - | todo | 2026-09-26 | 7+ local commits |
| T8 | One-bit correction (CRC-32 'R' frames) + rescan after a failed frame | P0 | T4 | done (flashed, awaiting shake test) | 2026-10-10 | from shake data: 45% single-bit, runs of 2 |
| T9 | Shake test with T8; decide on FEC from what's still lost | P0 | T8 | done: 138 vs 239 frames, still audible -> FEC decision (Muni) | 2026-10-10 | compare: 239 frames / 13 s before |
| T10 | FEC across frames (24+8, ~190 ms fixed delay) to make remaining losses bit-exact | P1 | T9, Muni's go | todo | 2026-10-10 | runs mostly 1 frame, max 5-8 |
| T11 | Phone rewinds ~4 s at ~1:00 into tracks (not us) -- identify the app behavior | P2 | - | todo | 2026-10-10 | evidence in STATUS 2026-10-10 |
