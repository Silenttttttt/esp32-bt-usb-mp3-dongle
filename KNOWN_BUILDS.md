# Known builds

Builds that ran somewhere real, and how to put them back exactly: same commit, same flags.
`tools/flash_build.sh <name> s3|classic|both` builds from a separate git worktree
(`.worktrees/<sha>`) with that commit's own `flash.sh`, so its own flag list. Both boards must
run the same build (the link format must match).

| Name | Commit | Where it ran | Classic flags | S3 flags | Reflash |
|---|---|---|---|---|---|
| `car-2026-09-25` | `4cef646` (firmware = `2593cf9`) | The car since 2026-09-25; 32 KB clusters car-verified 2026-09-26 | `-DINT2IDX_SIZE=4000 -DDIAG_LOOP_DRAIN -DDIAG_FRAG_TRACE -DV2_ALL -DENCODE_ON_S3` | `-DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -DLED_RAINBOW_PLAYING -DENCODE_ON_S3` | `tools/flash_build.sh car-2026-09-25 both` |
| `car-candidate-2026-10-10` | `c209365` (firmware; later commits are docs only) | Bench 2026-10-10: the link fix without debug lines. The build meant for the car | car flags + `-DLINK_Q` | car flags + `-DLINK_Q -DLINK_TOTALS` (superseded: now `-DLINK_EVENTS`) | `tools/flash_build.sh car-candidate-2026-10-10 both --fix-link` |
| `bench-2026-10-10-fixlink` | `c209365` | Bench 2026-10-10, both boards, shaken: crackle gone, "nearly glitchless"; no false Back at mount. Not yet in the car | car flags + `-DLINK_Q -DLINK_DIAG` | car flags + `-DLINK_Q -DLINK_TOTALS -DLINK_DIAG` | `tools/flash_build.sh bench-2026-10-10-fixlink both --fix-link --diag` |

`bench-2026-10-10-fixlink` is the plain v2 flag set plus `--fix-link --diag` (flash both boards
together). Drop `--diag` for a quieter build; the fix doesn't depend on it.

Evidence for `car-2026-09-25`: `logs/flash_history.log` (classic 2026-09-25 21:04 at `9c03c6c`, S3
22:13 at `2593cf9`; no classic code changed between those two commits). Rebuilt 2026-10-10: the
classic image is 1,084,884 bytes, the same as `docs/HARDWARE_USAGE.md` measured on the car build.

Everything added after this build is behind its own flag and off by default, so plain
`flash.sh` at HEAD still behaves like the car build. The opt-in flag sets are in each
`flash.sh` (`--fix-link`, `--diag`) and described in `BUILD_FLAGS.md`.
