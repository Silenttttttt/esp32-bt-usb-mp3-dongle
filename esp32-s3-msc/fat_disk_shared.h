// Shared core: the EXACT same FAT12 ring-buffer disk logic and UART wire
// protocol constants, compiled into BOTH the real esp32-s3-msc.ino
// (Arduino/ESP32-S3 target) and sim/s3_real_firmware_host.cpp (a PC-hosted
// program standing in for the physical S3 board until it arrives). This
// is a single source of truth -- not two hand-copied parallel
// implementations that could silently drift out of sync with each other
// (exactly the kind of bug already found and fixed elsewhere tonight,
// where a fix applied to one code path wasn't propagated to another).
//
// Platform differences are isolated to a small set of macros defined
// right below, based on whether ARDUINO is defined. Everything past that
// point is byte-for-byte identical C++ on both platforms.
#ifndef FAT_DISK_SHARED_H
#define FAT_DISK_SHARED_H

// Real, valid, standards-compliant pre-encoded silent MP3 frames (see the
// silence-bridge globals below for why this exists) -- SILENCE_MP3_FRAMES
// / SILENCE_MP3_FRAMES_LEN.
#include "silence_mp3_frames.h"

#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
  // ---- ESP32-S3 / FreeRTOS ----
  #define FATDISK_MUTEX_T SemaphoreHandle_t
  #define FATDISK_MUTEX_CREATE() xSemaphoreCreateMutex()
  #define FATDISK_MUTEX_TAKE_BLOCKING(m) xSemaphoreTake(m, portMAX_DELAY)
  #define FATDISK_MUTEX_TRY_TAKE_MS(m, ms) (xSemaphoreTake((m), pdMS_TO_TICKS(ms)) == pdTRUE)
  #define FATDISK_MUTEX_GIVE(m) xSemaphoreGive(m)
  #define FATDISK_RANDOM32() esp_random()
  #define FATDISK_MILLIS() millis()
#else
  // ---- PC host (sim/s3_real_firmware_host.cpp) ----
  #include <mutex>
  #include <chrono>
  #include <algorithm>
  #include <random>
  using std::min;
  #define FATDISK_MUTEX_T std::timed_mutex*
  #define FATDISK_MUTEX_CREATE() (new std::timed_mutex())
  #define FATDISK_MUTEX_TAKE_BLOCKING(m) (m)->lock()
  #define FATDISK_MUTEX_TRY_TAKE_MS(m, ms) (m)->try_lock_for(std::chrono::milliseconds(ms))
  static inline uint32_t fatdisk_random32_host() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;
    return dist(gen);
  }
  #define FATDISK_RANDOM32() fatdisk_random32_host()
  #define FATDISK_MUTEX_GIVE(m) (m)->unlock()
  static inline uint32_t fatdisk_millis_host() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
  }
  #define FATDISK_MILLIS() fatdisk_millis_host()
#endif

// ===================== Wire protocol (must match esp32-bt-mp3-test.ino) ====

static const uint8_t FRAME_MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;
static const uint32_t WRITE_RETRY_DELAY_MS = 5;
static const uint32_t MAX_WRITE_RETRIES = 200;  // ~1s total

// ===================== FAT12 ring-buffer disk (port of fat12_disk.py) ======

static const uint32_t SECTOR_SIZE = 512;

// OPEN TUNING QUESTION, deliberately left as-is pending real S3 hardware
// (2026-09-17): a real car-radio test found FAT12 initially failed
// (garbled playback) on a physical test drive using this same 8-sector
// (4KB) cluster size, and succeeded once switched to 64-sector (32KB)
// clusters -- among several other simultaneously-changed variables (see
// progress/STATUS.md's "MAJOR: FAT12 confirmed compatible" entry), so
// root cause isn't isolated. Deliberately NOT copying the 32KB value
// here: at this ring's tiny size (~200KB), 32KB clusters would mean only
// ~6-7 total data clusters, ballooning READ_MARGIN_BYTES (2 clusters)
// from ~4% of the ring to ~29% -- a real, known-bad ratio matching this
// project's own earlier Python-prototype history of frequent glitches
// from an oversized margin-to-ring fraction. Trading a real, understood
// glitch risk for an unproven compatibility fix isn't worth it blind.
// TEST ONCE THE REAL S3 BOARD EXISTS: if FAT12 still misbehaves on the
// real radio with the real firmware at this cluster size, that's the
// moment to try increasing it here (and re-tuning READ_MARGIN_BYTES
// alongside it, not independently) -- not before, since only the real
// hardware can show whether cluster count was ever actually the cause.
static const uint32_t SECTORS_PER_CLUSTER = 8;  // 4KB clusters
static const uint32_t RESERVED_SECTORS = 1;
static const uint32_t NUM_FATS = 2;
static const uint32_t ROOT_ENTRIES = 16;
static const char FILE_NAME[12] = "STREAM  MP3";  // 8.3, space-padded (11 bytes + NUL)

// REDUCED from 117 clusters (~30s) to 59 (~15.1s) on 2026-09-17 after a
// real phone test: the car radio reader always starts reading from file
// position 0 (cluster 2), and since the ring is a continuously-
// overwritten circular buffer, whatever's CURRENTLY at position 0 can be
// up to a FULL RING DURATION stale -- exactly the mechanism
// s3_sim_serial.py's own --capacity-mb help text already documented
// ("the reader's wrap-to-start cycle... can replay content from up to
// the FULL ring duration ago"). With a fresh/idle ring (silence-injected
// since boot) sitting at position 0, this meant up to ~30s of stale
// silence had to be read through before the reader's traversal caught up
// to wherever real audio was actually being written -- a real,
// measured, user-reported ~30s time-to-first-real-audio delay, not a
// regression from any one pipeline. Halving ring duration halves this
// worst-case bound. See fat12_disk.py's own docstring for why this is
// sized as "acceptable worst-case catch-up lag", not "how long is the
// drive".
//
// DOUBLED from 50 to 100 clusters (2026-09-18, first live test against
// real hardware + real car_sim.py): READ_MARGIN_BYTES (below) is a fixed
// absolute size, so at 50 clusters it was ~4% of the ring -- confirmed
// live via the real passthrough's own read counter, 8/249 reads (~3.2%)
// came back straddle-protected zero-fill, each one enough to desync a
// strict low-buffer MP3 decoder (car_sim.py's ffmpeg config) for a
// while, producing persistent "Header missing" errors even after two
// separate, confirmed, unrelated bugs were found and fixed (a ring-wrap
// frame-splitting bug, and an ID3-tagged silence primer). Doubling the
// ring halves the margin's fraction of it (~4% -> ~2%) without touching
// the margin itself, directly cutting straddle frequency, at the real,
// accepted cost of roughly doubling the worst-case stale-replay window
// if the source pauses (~12.8s -> ~25.6s) -- a bounded, known tradeoff,
// not a new risk.
//
// RAISED AGAIN from 100 to 938 clusters (2026-09-18, same session, after
// the FIRST REAL CAR RADIO TEST): confirmed working end to end on the
// actual physical radio, but with an audible stutter every ~25.6s at the
// ring's wrap point. This is inherent to a fixed-size looping ring, not a
// bug: the byte just before wrap (position DECLARED_FILE_SIZE-1) and the
// byte just after it (position 0) are NOT temporally adjacent in the
// original audio -- they're roughly one full ring-duration apart -- so a
// real dumb sequential reader always audibly splices two unrelated
// moments together there. Stretching the ring to ~4 minutes doesn't
// eliminate that splice, it just makes it ~9.4x rarer (once per ~4min
// instead of once per ~25.6s), which is the whole point: most real
// listening sessions/songs are shorter than that, so the wrap is rarely
// even reached. REAL, ACCEPTED TRADEOFF, same mechanism as the doubling
// above but now much larger: worst-case stale-replay-on-pause and
// worst-case cold-boot-catch-up-lag both scale with ring size too -- this
// is the exact same category of delay that was previously found bad
// enough to deliberately SHRINK the ring for (was ~30s, cut to ~12.8s,
// see the 59-cluster history above) -- raising it back up to ~4 minutes
// reintroduces a much larger version of that specific, previously-fixed
// problem. Accepted here because a real user on real hardware explicitly
// weighed "occasional ~25s stutter" against "much longer cold-start/
// pause-recovery lag" and chose the latter as the better tradeoff for
// this use case. 938 clusters * 4096 bytes/cluster = 3,842,048 bytes
// (~240.1s, ~4.0min at 16000 B/s). Comfortably inside FAT12's 4084-total-
// cluster ceiling (see the static_assert below) and well inside the
// real S3 module's 8MB PSRAM budget.
static const uint32_t DATA_CLUSTERS = 938;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;  // 409600

// FATDISK_MULTI_FILE (new, 2026-09-19): opt-in, off by default -- the real
// esp32-s3-msc.ino's build command never defines this, so its behavior is
// byte-for-byte unchanged (NUM_FILES=1 makes every formula below reduce to
// exactly what it was before this existed). Exists to test the "PLAN_NEXT.md
// C3" idea (detect the car radio's own physical next/prev buttons via
// multiple directory entries that all alias the same live ring) against
// real hardware via sim/s3_real_firmware_host.cpp + car_sim.py's new --gui
// mode, without risking the real, working single-file production firmware.
// 3 files, not more: matches C3's own plan (2 can't disambiguate direction;
// 3 is the minimum that can with a static circular table).
#ifdef FATDISK_MULTI_FILE
static const uint32_t NUM_FILES = 3;
// C1/C2/C4/C6 unification (2026-09-21, Muni's design): previously all 3
// files shared one static name (see the old comment this replaced) because
// nothing needed per-file identity yet. Now they DO -- the file-rotation
// itself is the track-change signal: force the CURRENTLY-open file to hit
// an early EOF (see force_end_current_file() below) the instant a real
// track change happens, so the radio naturally advances to the NEXT file
// in rotation, which by then already carries the new track's name. Made
// mutable (was a compile-time `const char *const` array) so
// set_file_name() below can rewrite an entry at runtime; still seeded from
// FILE_NAME at boot, identical to the old static behavior until something
// actually calls set_file_name().
static char FILE_NAMES_BUF[NUM_FILES][12];
static char *const FILE_NAMES[NUM_FILES] = {FILE_NAMES_BUF[0], FILE_NAMES_BUF[1], FILE_NAMES_BUF[2]};
// Seeds FILE_NAMES_BUF from FILE_NAME -- called once, before build_root_dir()
// -- rather than a static initializer, so there's exactly one place
// (FILE_NAME itself) that defines the default name, no risk of the two
// drifting apart.
static void init_file_names() {
  for (uint32_t f = 0; f < NUM_FILES; f++) memcpy(FILE_NAMES_BUF[f], FILE_NAME, 12);
}
#else
static const uint32_t NUM_FILES = 1;
static const char *const FILE_NAMES[NUM_FILES] = {FILE_NAME};
#endif

static const uint32_t FAT_ENTRIES_NEEDED = NUM_FILES * DATA_CLUSTERS + 2;
static const uint32_t FAT_BYTES = (FAT_ENTRIES_NEEDED * 3 + 1) / 2;
static const uint32_t FAT_SECTORS = (FAT_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE;
static const uint32_t ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) / SECTOR_SIZE;
static const uint32_t FIRST_DATA_LBA = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS;
static const uint32_t TOTAL_SECTORS = FIRST_DATA_LBA + NUM_FILES * DATA_CLUSTERS * SECTORS_PER_CLUSTER;
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;  // ~0.5s headroom

// PLAN_NEXT.md item B1 (2026-09-19 design, implemented 2026-09-20): the S3
// had ZERO independent staleness protection of its own -- disk_read_at()
// only ever guarded against a race with the writer currently mid-append
// (READ_MARGIN_BYTES/the live-serve catch-up logic above), never against
// time since a UART frame from the classic actually arrived at all. If the
// classic stops sending anything (a crash/reboot, not just a graceful BT
// disconnect -- silence-injection already covers that gracefully and is
// NOT gated on this), g_write_pos freezes and the radio marches forward
// into already-played-earlier real audio instead of silence -- a real
// field report ("radio was at ~2min into a 5min file, BT disconnected,
// radio kept reading and got served stale, already-played content"). Fix:
// serve zero-fill (the same safe default already used for a straddle/
// lock-miss) whenever the link has gone quiet for too long, using a signal
// the firmware already computes (g_link_last_frame_ms, updated on every
// frame in link_task()) but never previously consulted from the read path.
static const uint32_t LINK_STALE_MS = 2000;  // matches the existing status-LED threshold

// FATDISK_ALWAYS_SERVE_LIVE's own safety margin (see disk_read_at()'s
// live-serving branch below): how far behind the absolute live write edge
// to stay, so a read can never land on a byte the writer might still be
// mid-write on. Same purpose as READ_MARGIN_BYTES, just anchored to "now"
// (g_write_pos) instead of to the requested offset -- one cluster is
// already comfortably larger than a single Shine-encoded chunk (~416-420
// bytes), so this has real margin to spare without adding meaningfully to
// perceived latency.
static const uint32_t LIVE_SAFETY_MARGIN_BYTES = CLUSTER_SIZE;

// REAL BUG FOUND AND FIXED (2026-09-20, live test with real music): the
// FIRST version of FATDISK_ALWAYS_SERVE_LIVE recomputed "the most recent N
// bytes" completely independently on EVERY read, with no continuity
// guarantee between successive reads at all. Confirmed live: constant,
// pervasive MP3 frame corruption on nearly every single read, even with a
// correctly-paced reader and real, valid music flowing -- because two
// independently-timed reads computing "live minus margin minus size" have
// no reason to land on EXACTLY adjacent byte ranges; even sub-millisecond
// timing jitter between calls (inevitable over any real network/software
// stack) produces a small gap or overlap at nearly every read boundary,
// and MP3 frames need EXACT byte continuity to decode -- this is a
// structural flaw, not a tuning problem, and cannot be fixed by better
// pacing alone. Fixed with a PERSISTENT server-side cursor
// (g_live_read_cursor) that behaves exactly like the original offset-based
// design's implicit reader position -- advancing by exactly `n` bytes on
// every read, guaranteeing byte-exact continuity -- EXCEPT it periodically
// checks how far behind the live write edge it's fallen, and JUMPS FORWARD
// to near-live when that gap exceeds LIVE_CATCHUP_THRESHOLD_BYTES, instead
// of ever being allowed to stay minutes behind. A catch-up jump causes
// exactly one real discontinuity at that instant (the same kind of thing
// mpg123's own --resync-limit -1 is already built to skip past cleanly),
// not the pervasive corruption of the first attempt.
// REAL BUG FOUND (2026-09-21, live hardware, byte-correlation ground-truth
// test): a ~5s tolerance band is not "how far it can be live" -- it's "how
// far it's allowed to stay STALE before anyone bothers correcting it."
// Nothing in the normal per-read path ever shrinks the behind/ahead gap on
// its own (every read just advances the cursor by exactly n_safe,
// regardless of how that compares to how far wp moved in the same
// interval) -- convergence toward live ONLY happens via this threshold-
// triggered jump. Confirmed directly: tapping the classic's real UART
// (ground truth for what was actually transmitted) and byte-matching it
// against what the S3 actually served showed content NOT matching anything
// transmitted in the prior 30-40 real seconds, for the bulk of a session,
// with the S3's own debug trace showing almost no jumps firing during that
// same window -- i.e. the cursor was sitting well inside the old 5s
// tolerance band and just never being pulled in. Tightened from 5*16000 to
// 1*16000 (~1s) so a gap this large can never persist unnoticed for more
// than about a second -- still comfortably above LIVE_SAFETY_MARGIN_BYTES
// (one cluster, ~0.26s) so ordinary per-read jitter during healthy
// operation doesn't spuriously trigger this.
// SECOND real-hardware finding, same session, right after the first
// tightening (5s->1s) only partially helped (94%->60% cluster-failure
// rate, not fixed): per-read timing instrumentation on the actual bursty
// client showed reads arriving in tight pairs ~13ms apart (real I/O
// latency) followed by a ~243ms pause, repeating -- every read still
// advances the live cursor by a full n_safe (assumed to represent 256ms
// of real content) regardless of how little real wall-clock time the read
// itself actually took. Each "rapid pair" therefore races the cursor
// nearly half a second ahead of the real writer, and a 1s threshold still
// let 6 of every 10 clusters go stale before re-correcting (confirmed via
// direct byte-correlation against real UART-tapped ground truth). Tightened
// further, to 2*n_safe (two clusters, ~0.5s) -- deliberately kept above
// LIVE_SAFETY_MARGIN_BYTES (one cluster) so ordinary single-cluster read
// jitter during healthy operation doesn't spuriously re-trigger this on
// every read, but tight enough that sustained ahead-drift from a
// real-world bursty/read-ahead reader (which is plausible for the actual
// car radio too, not just this bench tool) gets corrected within roughly
// half a second instead of a full second.
static const uint32_t LIVE_CATCHUP_THRESHOLD_BYTES = 2 * 4096;  // ~0.5s (2 clusters)

// How far behind the live write edge the cursor is placed after a jump
// (measured before the read that follows it). Since 2026-09-24 the cursor
// can never be served past the writer (an underrun serves silence and
// holds it instead), so this is purely a latency-vs-dropout knob: the
// larger it is, the longer a Bluetooth delivery stall it absorbs before an
// underrun. The cursor jumps back to this lag whenever it falls more than
// LIVE_CATCHUP_THRESHOLD_BYTES further behind. Overridable for the host
// test's parameter sweep (crosscheck/live_serve_test.cpp).
#ifndef FATDISK_LIVE_TARGET_LAG
#define FATDISK_LIVE_TARGET_LAG (3 * 4096)  // ~0.77s: absorbs a ~500ms BT stall (see test sweep)
#endif
static const uint32_t LIVE_TARGET_LAG_BYTES = FATDISK_LIVE_TARGET_LAG;

// Silence-bridge (2026-09-22, Muni's request): a catch-up jump is
// deliberately, unavoidably a content DISCONTINUITY -- the bytes right
// after a jump are byte-continuous with each other, but NOT with whatever
// was served just before the jump, so a reader's MP3 decoder has to
// resync on its own (this is the real, confirmed source of the transient
// "Header missing"/"invalid block type"/"big_values too big" decode
// errors seen right after a jump, or after any pause/disconnect that
// forces the cursor to fall behind and catch up).
//
// REAL CORRECTNESS BUG CAUGHT before this ever shipped (Muni: "is that
// valid audio data, that is important" -- it was not): the first version
// of this bridged with raw 0x00 zero-fill. Zero bytes contain no MP3 sync
// word at all (a real frame header needs 0xFF followed by a byte with its
// top 3 bits set) -- that's not a "silent audio frame," it's just
// unparseable filler that a LENIENT decoder happens to skip over quietly,
// with zero guarantee a real car radio's embedded decoder chip does the
// same (it could just as easily hold/repeat its last decoded buffer, or
// produce an audible artifact, depending entirely on that decoder's own
// error-concealment behavior -- unverified and not something to bet a
// real product feature on). Fixed for real: SILENCE_MP3_FRAMES
// (silence_mp3_frames.h) is 1.0s of genuinely valid, standards-compliant
// silent MP3 data -- generated via ffmpeg/libmp3lame at 44100Hz mono
// 128kbps CBR, the EXACT same parameters the classic ESP32's own Shine
// encoder uses (see `AudioInfo(44100, 1, 16)` in esp32-bt-mp3-test.ino) --
// so any standards-compliant decoder, including a real embedded car-radio
// chip, decodes it exactly as real, correct digital silence, not a gap it
// has to guess how to handle. The underlying live-read cursor keeps
// advancing normally through this window (see its own use below) -- only
// the OUTPUT bytes get overwritten -- so real content resumes exactly
// where it would have anyway once the window ends, no separate
// resync-back-to-live step needed.
static volatile uint32_t g_silence_bridge_remaining = 0;
static const uint32_t SILENCE_BRIDGE_BYTES = 8000;  // ~0.5s at the ~16000B/s encode rate

// The silence stream is served by looping SILENCE_MP3_FRAMES from its
// second frame: frame 0 is LAME's "Info" tag frame, and frame 1 (byte 417)
// is the only later frame with main_data_begin == 0 (every frame after it
// borrows 114 bytes of bit reservoir from its predecessor -- checked by
// parsing the embedded data). Looping end -> byte 417 therefore always
// lands on a frame that decodes without needing the previous frame, and
// every silence episode starts there too.
static const uint32_t SILENCE_LOOP_START = 417;
static volatile uint32_t g_silence_offset = SILENCE_LOOP_START;
static_assert(SILENCE_LOOP_START < SILENCE_MP3_FRAMES_LEN, "silence loop start past end of data");

static void serve_silence(uint8_t *dst, uint32_t len) {
  uint32_t off = g_silence_offset;
  while (len > 0) {
    uint32_t chunk = SILENCE_MP3_FRAMES_LEN - off;
    if (chunk > len) chunk = len;
    memcpy(dst, SILENCE_MP3_FRAMES + off, chunk);
    dst += chunk; len -= chunk; off += chunk;
    if (off >= SILENCE_MP3_FRAMES_LEN) off = SILENCE_LOOP_START;
  }
  g_silence_offset = off;
}

static_assert(NUM_FILES * DATA_CLUSTERS + 2 < 4085, "must stay FAT12, not FAT16 -- matches fat12_disk.py's assert");
static_assert(NUM_FILES <= ROOT_ENTRIES, "root directory doesn't have enough entries for NUM_FILES");

static uint8_t g_boot_sector[SECTOR_SIZE];
static uint8_t g_fat_sector_cache[FAT_SECTORS * SECTOR_SIZE];
static uint8_t g_root_dir_sector[ROOT_DIR_SECTORS * SECTOR_SIZE];

// The ring itself. On the real S3, 468KB doesn't fit in internal DRAM
// alongside USB/FreeRTOS/heap needs (confirmed directly: a static array
// overflowed dram0_0_seg by ~250KB) -- allocated from PSRAM at setup()
// instead. On the PC host, allocated from plain heap (see
// s3_real_firmware_host.cpp's own resource-monitoring code for why this
// specific allocation size is the one apples-to-apples number worth
// comparing against the real S3's PSRAM budget).
static uint8_t *g_ring = nullptr;
static volatile uint32_t g_write_pos = 0;
// Where the most recently COMPLETED lap actually ended. disk_append() never
// splits a chunk across the physical end of the ring -- it restarts at 0
// instead -- so [g_ring_lap_end, DECLARED_FILE_SIZE) holds bytes from an
// even older lap, not the end of the previous one. The live-serve reader
// uses this as the logical wrap point so it never plays that stale tail.
static volatile uint32_t g_ring_lap_end = DECLARED_FILE_SIZE;
static volatile uint32_t g_total_written = 0;
static volatile uint32_t g_last_read_offset = 0;

// B1 (see LINK_STALE_MS's own comment above): moved here from the .ino so
// disk_read_at() below can consult it directly -- previously declared only
// in esp32-s3-msc.ino, used exclusively for the RGB status LED. link_task()
// still owns writing it (on every frame, 'A' or 'C'), now via this shared
// declaration instead of a local one; the .ino's own loop()/LED logic reads
// it exactly as before, unaffected by the move.
static volatile uint32_t g_link_last_frame_ms = 0;

#ifdef FATDISK_ALWAYS_SERVE_LIVE
// See LIVE_CATCHUP_THRESHOLD_BYTES's own comment above for the full
// history/rationale. This cursor is genuinely independent of the
// requested LBA -- it advances purely based on how many bytes have been
// SERVED so far, exactly like the original design's implicit reader
// position, except it can jump forward on its own when it falls too far
// behind live.
static volatile uint32_t g_live_read_cursor = 0;
static volatile bool g_live_read_cursor_init = false;
static volatile bool g_live_underrun = false;      // currently serving underrun silence
static volatile uint32_t g_live_underruns = 0;     // reads served as underrun silence, for the heartbeat
#endif

// TEMP DIAGNOSTIC (2026-09-18): investigating a real bug where content at a
// fixed ring position stays byte-identical across multiple full laps even
// though g_write_pos itself is confirmed advancing normally and the
// classic ESP32's own UART source data is confirmed 100% unique (zero
// duplicate frames over 1500+ samples) -- meaning the freeze is somewhere
// in THIS write->read path specifically (possibly PSRAM cross-core cache
// coherency between link_task on core 1 and the USB read callback, or a
// logic bug). These record a lightweight rolling hash of whatever's
// actually written/read whenever the position falls inside a fixed
// DIAG_TARGET_POS window, from the firmware's own perspective (avoids any
// ambiguity from an external USB/OS-level read path). Read via the
// existing loop() heartbeat print. Remove once root-caused.
// FIXED (2026-09-18, same session): the original DIAG_TARGET_WINDOW=512
// full-containment check could never fire on the write side -- real
// disk_append() chunks are only ~416-420 bytes (Shine's own encoded chunk
// size), always smaller than a 512-byte window, so "fully contains the
// window" was mathematically impossible and g_diag_write_count stayed 0
// for 90+ seconds across multiple full ring laps. Switched to tracking a
// single fixed byte instead of a window -- trivially satisfiable by any
// write/read that merely covers that one byte, which happens on every lap
// regardless of chunk size.
#define DIAG_TARGET_POS 100000
static volatile uint8_t g_diag_write_byte = 0;
static volatile uint32_t g_diag_write_count = 0;
static volatile uint8_t g_diag_read_byte = 0;
static volatile uint32_t g_diag_read_count = 0;
static inline uint32_t diag_hash(const uint8_t *p, uint32_t n) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// Guards g_ring/g_write_pos/g_total_written between the writer (UART
// receive) and the reader (USB read callback on real hardware; the TCP
// SCSI-READ10 handler on the PC host). The writer can afford to block
// briefly; the reader must not (see disk_read_at()'s short-timeout take).
static FATDISK_MUTEX_T g_ring_mutex;

static void pack_fat12_entries(const uint16_t *entries, uint32_t n, uint8_t *out) {
  uint32_t i = 0, oi = 0;
  while (i < n) {
    uint16_t e0 = entries[i];
    uint16_t e1 = (i + 1 < n) ? entries[i + 1] : 0;
    out[oi++] = e0 & 0xFF;
    out[oi++] = ((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4);
    out[oi++] = (e1 >> 4) & 0xFF;
    i += 2;
  }
}

static void build_boot_sector() {
  memset(g_boot_sector, 0, SECTOR_SIZE);
  uint8_t *bs = g_boot_sector;
  bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
  memcpy(bs + 3, "BOARDSIM", 8);
  bs[11] = SECTOR_SIZE & 0xFF; bs[12] = (SECTOR_SIZE >> 8) & 0xFF;
  bs[13] = SECTORS_PER_CLUSTER;
  bs[14] = RESERVED_SECTORS & 0xFF; bs[15] = (RESERVED_SECTORS >> 8) & 0xFF;
  bs[16] = NUM_FATS;
  bs[17] = ROOT_ENTRIES & 0xFF; bs[18] = (ROOT_ENTRIES >> 8) & 0xFF;
  uint32_t total16 = (TOTAL_SECTORS < 0x10000) ? TOTAL_SECTORS : 0;
  bs[19] = total16 & 0xFF; bs[20] = (total16 >> 8) & 0xFF;
  bs[21] = 0xF8;
  bs[22] = FAT_SECTORS & 0xFF; bs[23] = (FAT_SECTORS >> 8) & 0xFF;
  bs[24] = 32; bs[25] = 0;   // sectors_per_track
  bs[26] = 64; bs[27] = 0;   // num_heads
  // bytes 28-31 hidden_sectors = 0 (already zeroed)
  uint32_t total32 = (total16 == 0) ? TOTAL_SECTORS : 0;
  bs[32] = total32 & 0xFF; bs[33] = (total32 >> 8) & 0xFF;
  bs[34] = (total32 >> 16) & 0xFF; bs[35] = (total32 >> 24) & 0xFF;
  bs[36] = 0x80;
  bs[38] = 0x29;
  // Random per boot (2026-09-17): a real car radio confirmed to sometimes
  // cache "resume playback position" keyed on volume serial + filename --
  // a FIXED serial here would mean every boot presents an IDENTICAL
  // identity to the radio despite the ring's actual content being
  // completely different each time, risking the radio resuming into the
  // middle of unrelated content. Found via a real test that showed
  // exactly this symptom (playback starting mid-file) before other
  // factors were also changed; this fix has no downside regardless of
  // whether it was the actual cause.
  uint32_t serial = FATDISK_RANDOM32();
  bs[39] = serial & 0xFF; bs[40] = (serial >> 8) & 0xFF;
  bs[41] = (serial >> 16) & 0xFF; bs[42] = (serial >> 24) & 0xFF;
  memcpy(bs + 43, "BOARDSIM   ", 11);
  memcpy(bs + 54, "FAT12   ", 8);
  bs[510] = 0x55; bs[511] = 0xAA;
}

// Generalized to NUM_FILES independent chains, each DATA_CLUSTERS long,
// back-to-back in cluster-number space: file f occupies clusters
// [2+f*DATA_CLUSTERS, 2+(f+1)*DATA_CLUSTERS). With NUM_FILES=1 (the
// default) this is byte-for-byte the same single chain as before.
static void build_fat() {
  static uint16_t entries[NUM_FILES * DATA_CLUSTERS + 2];
  entries[0] = 0xFF8;
  entries[1] = 0xFFF;
  for (uint32_t f = 0; f < NUM_FILES; f++) {
    uint32_t start = 2 + f * DATA_CLUSTERS;
    for (uint32_t i = 0; i < DATA_CLUSTERS; i++) {
      uint32_t c = start + i;
      entries[c] = (i + 1 < DATA_CLUSTERS) ? (uint16_t)(c + 1) : 0xFFF;
    }
  }
  memset(g_fat_sector_cache, 0, sizeof(g_fat_sector_cache));
  pack_fat12_entries(entries, NUM_FILES * DATA_CLUSTERS + 2, g_fat_sector_cache);
}

// Generalized to NUM_FILES directory entries, each pointing at its own
// chain's first cluster above, but all declaring the SAME size -- because
// disk_read_at() below maps every file's data range back onto the SAME
// live ring content (see fatdisk file-index remap there). With NUM_FILES=1
// this writes exactly the one entry it always did.
static void build_root_dir() {
  memset(g_root_dir_sector, 0, sizeof(g_root_dir_sector));
  for (uint32_t f = 0; f < NUM_FILES; f++) {
    uint8_t *entry = g_root_dir_sector + f * 32;
    memcpy(entry, FILE_NAMES[f], 11);
    entry[11] = 0x20;  // ARCHIVE
    entry[16] = 0x21; entry[17] = 0x4A;
    entry[18] = 0x21; entry[19] = 0x4A;
    entry[24] = 0x21; entry[25] = 0x4A;
    uint32_t first_cluster = 2 + f * DATA_CLUSTERS;
    entry[26] = first_cluster & 0xFF; entry[27] = (first_cluster >> 8) & 0xFF;
    entry[28] = DECLARED_FILE_SIZE & 0xFF;
    entry[29] = (DECLARED_FILE_SIZE >> 8) & 0xFF;
    entry[30] = (DECLARED_FILE_SIZE >> 16) & 0xFF;
    entry[31] = (DECLARED_FILE_SIZE >> 24) & 0xFF;
  }
}

// See esp32-s3-msc.ino's git history (2026-09-17) for the real uint32_t
// overflow bug this capping behavior fixes, and the second bug (the
// capping addition itself overflowing) caught in the first fix attempt.
static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    FATDISK_MUTEX_TAKE_BLOCKING(g_ring_mutex);
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0;
    g_ring_lap_end = DECLARED_FILE_SIZE;
    g_total_written = DECLARED_FILE_SIZE;
    FATDISK_MUTEX_GIVE(g_ring_mutex);
    return true;
  }
  FATDISK_MUTEX_TAKE_BLOCKING(g_ring_mutex);
  uint32_t wp = g_write_pos;
  uint32_t end = wp + n;

  // REAL BUG FOUND (2026-09-18, first live test against real hardware,
  // real car_sim.py, real S3): a normal wrap here used to byte-split a
  // chunk's data across the ring's physical end/start boundary with zero
  // awareness of where MP3 frame boundaries fall inside it. Each disk_append()
  // call's payload is already a whole number of complete Shine-encoded MP3
  // frames (esp32-bt-mp3-test.ino's send_framed('A', ...) never sends a
  // partial frame), so a byte-level split always corrupts whichever frame
  // straddles that exact boundary -- and since the wrap boundary IS byte 0,
  // and car_sim.py (matching a real dumb head unit) always starts reading
  // from byte 0 on every single pass, that ONE corrupted frame lands
  // exactly where every playback attempt begins. Confirmed: persistent
  // "Header missing" decoder errors on literally every read from byte 0,
  // not an intermittent timing issue (a real retry-based fix was tried and
  // reverted -- it didn't change the error rate at all, which is what
  // pointed here instead). Fixed by never splitting a chunk across the
  // wrap: if this append would cross the boundary, skip straight to
  // position 0 and write the WHOLE chunk there as one unbroken unit,
  // leaving whatever's between the old write_pos and DECLARED_FILE_SIZE
  // untouched (a real, complete, still-valid frame from one lap earlier)
  // instead of a byte-split, invalid one. Safe on the writer side (unlike
  // the read path) since this thread is allowed to do real work.
  if (end > DECLARED_FILE_SIZE) {
    g_ring_lap_end = wp;
    wp = 0;
    end = n;
  }

  // TEMP DIAGNOSTIC -- see g_diag_* above. If this write touches the
  // target window, hash what's about to be memcpy'd there.
  if (wp <= DIAG_TARGET_POS && DIAG_TARGET_POS < end) {
    g_diag_write_byte = data[DIAG_TARGET_POS - wp];
    g_diag_write_count++;
  }

#ifndef FATDISK_ALWAYS_SERVE_LIVE
  // Under FATDISK_ALWAYS_SERVE_LIVE, disk_read_at() no longer tracks a real
  // "how far behind is the reader" position at all -- every read serves
  // whatever's most recent as of that exact moment, staying permanently
  // within LIVE_SAFETY_MARGIN_BYTES of g_write_pos by construction. Under
  // that design g_last_read_offset would sit almost exactly at g_write_pos
  // at all times, making this check spuriously fire on nearly every append
  // (retrying up to MAX_WRITE_RETRIES times, costing up to ~1s per write --
  // confirmed by direct reasoning, not yet needed to hit live to know it'd
  // be real) even though there's no actual unread content being clobbered
  // in the old sense anymore. Skip the check entirely in this mode.
  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    bool unsafe = (wp <= g_last_read_offset && g_last_read_offset < end);
    if (unsafe) {
      FATDISK_MUTEX_GIVE(g_ring_mutex);
      return false;
    }
  }
#endif
  memcpy(g_ring + wp, data, n);
  g_write_pos = end;
  if (g_total_written < DECLARED_FILE_SIZE) {
    g_total_written = min(g_total_written + n, DECLARED_FILE_SIZE);
  }
  FATDISK_MUTEX_GIVE(g_ring_mutex);
  return true;
}

static inline uint32_t disk_valid_bytes() {
  return g_total_written;  // already capped at DECLARED_FILE_SIZE by disk_append()
}

#ifdef FATDISK_MULTI_FILE
// C3's plan design: don't trust a read landing in a new file's range as a
// real driver button-press until it's been read from consistently for a
// while -- filters out a cheap head unit's own brief directory-scan/
// validation probes (which touch every file at mount time) from a genuine,
// decoder-committed switch. ~16KB matches PLAN_NEXT.md's own proposed
// threshold (roughly 1s of audio-equivalent bytes at this pipeline's rate).
static const uint32_t SWITCH_DEBOUNCE_BYTES = 16384;
static volatile uint32_t g_current_file_index = 0;
static volatile uint32_t g_candidate_file_index = 0;
static volatile uint32_t g_candidate_bytes_read = 0;
// direction: +1 = next, -1 = previous. Set by the hosting program (real
// .ino or s3_real_firmware_host.cpp) to actually relay the detected press
// onward -- this header has no opinion on HOW that happens.
static void (*g_file_switch_callback)(int direction) = nullptr;

// C1/C2/C4/C6 unification (2026-09-21, Muni's design): use the file
// rotation itself as the track-change signal. Force the CURRENTLY-open
// file to hit a clean early EOF the instant a real track change happens,
// so the radio's own reader naturally advances to the NEXT file in
// rotation -- which by then already carries the new track's name. All 3
// files still alias the SAME live ring content (see disk_read_at()'s own
// remap comment) -- nothing about the underlying audio stream changes,
// only which directory entry the radio is nominally reading from, and
// what name it shows.

// Patches ONLY the 4-byte size field of one root-dir entry (bytes 28-31,
// standard FAT least-significant-byte-first) -- everything else about the
// entry (name, start cluster) is untouched. The declared size is the ONLY
// thing a FAT reader uses to know where a file ends; it has no separate
// end-of-file marker in the data itself.
static void set_file_declared_size(uint32_t file_index, uint32_t size) {
  uint8_t *entry = g_root_dir_sector + file_index * 32;
  entry[28] = size & 0xFF;
  entry[29] = (size >> 8) & 0xFF;
  entry[30] = (size >> 16) & 0xFF;
  entry[31] = (size >> 24) & 0xFF;
}

// Patches ONLY the 11-byte 8.3 short name of one root-dir entry. `name11`
// must already be exactly 11 bytes, space-padded (same convention as
// FILE_NAME/build_root_dir()) -- this is the short-name field only, NOT a
// real VFAT long filename (that's a separate, larger piece of work, still
// its own open item -- see PLAN_NEXT.md's C2 section).
static void set_file_name(uint32_t file_index, const char *name11) {
  memcpy(FILE_NAMES_BUF[file_index], name11, 11);
  memcpy(g_root_dir_sector + file_index * 32, name11, 11);
}

// C1/Step 2 (2026-09-22, real-name forwarding): turns an arbitrary real
// AVRCP song title (any length, any characters -- UTF-8, punctuation,
// lowercase) into a valid FAT 8.3 short-name field for set_file_name()
// above. Extension is always "MP3" (these are always our synthetic MP3
// stream, regardless of title) -- only the 8-char basename comes from the
// real title. Only [A-Z0-9] survive (spaces and punctuation are dropped
// entirely rather than replaced with a filler char like '_', so a title
// like "Bohemian Rhapsody" becomes "BOHEMIAN" instead of a less readable
// "BOHEMIAN_RHAPSODY"-truncated-with-junk); falls back to the original
// generic "STREAM" placeholder if the title yields zero valid characters
// (e.g. a title that's entirely emoji/non-ASCII, or empty).
static void sanitize_to_8_3_name(const char *title, uint32_t title_len, char out11[11]) {
  char basename[8];
  uint32_t n = 0;
  for (uint32_t i = 0; i < title_len && n < 8; i++) {
    char c = title[i];
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      basename[n++] = c;
    }
  }
  if (n == 0) {
    memcpy(basename, "STREAM", 6);
    n = 6;
  }
  for (uint32_t i = 0; i < 8; i++) out11[i] = (i < n) ? basename[i] : ' ';
  memcpy(out11 + 8, "MP3", 3);
}

// Highest file-relative offset the reader has reached in the CURRENTLY
// active file (not a byte count: a reader can start mid-file, e.g. a radio
// resuming a remembered position). Used to tell a natural end-of-file
// advance from a button press, and by force_track_change() to shrink the
// file to just past where the reader already is. Reset on every switch.
static volatile uint32_t g_current_file_read_end = 0;

// Set right before forcing a switch, consumed (and cleared) the moment
// fatdisk_note_file_read() below actually confirms the radio landed on the
// next file -- suppresses that confirmation from misreading OUR OWN
// engineered rotation as a genuine physical next/prev button press and
// relaying a redundant command back to the phone (the real "detection-
// logic complication" already flagged in PLAN_NEXT.md's C4 section before
// this design existed). Also remembers the size to restore the file we're
// LEAVING to, once the radio has genuinely finished with it -- restoring
// any earlier (e.g. immediately in force_track_change() itself) would
// undo the very shrink meant to force the EOF in the first place.
static volatile bool g_suppress_next_switch_callback = false;
static volatile uint32_t g_suppress_deadline_ms = 0;
static volatile uint32_t g_restore_size_for_prev_file = 0;
static volatile uint32_t g_prev_file_index_to_restore = 0;
// A radio that honors a shrunk size reaches it within about a cluster of
// reading (~0.26s). If no switch has happened well after that, the radio
// cached the size at open (FatFs-style) and never will: stop suppressing,
// so the NEXT switch -- a real button press -- still gets relayed, and put
// the file's size back so it isn't left truncated for its next open.
static const uint32_t SUPPRESS_WINDOW_MS = 10000;

static uint32_t get_file_declared_size(uint32_t file_index) {
  const uint8_t *entry = g_root_dir_sector + file_index * 32;
  return (uint32_t)entry[28] | ((uint32_t)entry[29] << 8) |
         ((uint32_t)entry[30] << 16) | ((uint32_t)entry[31] << 24);
}

// A reader that (re)starts after a gap -- radio powering up, re-mounting
// the drive, a bench GUI relaunch -- opens whatever file it opens; that is
// not a button press relative to where an EARLIER reader left off. Seen
// live 2026-09-24: a GUI relaunch reading file 0 while this still tracked
// file 2 from the previous session relayed a spurious "next" to the phone.
// Known trade-off: pausing on the radio for longer than this and THEN
// pressing next won't be relayed.
// After a gap the detector is "unanchored": the first file to get
// SWITCH_DEBOUNCE_BYTES of sustained reading becomes current silently.
// Adopting on the very first read instead was wrong -- confirmed live, the
// host's own filesystem probing right after USB mount (blkid/udisks reading
// signature offsets, some inside file 0's data) anchored file 0, and the
// real reader then opening another file relayed a spurious "next".
static const uint32_t READER_IDLE_RESET_MS = 3000;
static volatile uint32_t g_last_file_read_ms = 0;
static volatile bool g_any_file_read = false;
static volatile bool g_reader_anchored = false;

static void fatdisk_note_file_read(uint32_t file_index, uint32_t file_rel_off, uint32_t bytes_this_read) {
  uint32_t now = FATDISK_MILLIS();
  if (g_suppress_next_switch_callback &&
      (int32_t)(now - g_suppress_deadline_ms) > 0) {
    g_suppress_next_switch_callback = false;
    set_file_declared_size(g_prev_file_index_to_restore, g_restore_size_for_prev_file);
  }
  uint32_t read_end = file_rel_off + bytes_this_read;
  bool was_idle = !g_any_file_read || (now - g_last_file_read_ms) > READER_IDLE_RESET_MS;
  g_any_file_read = true;
  g_last_file_read_ms = now;
  if (was_idle) {
    g_reader_anchored = false;
    g_candidate_file_index = file_index;
    g_candidate_bytes_read = 0;
  }
  if (!g_reader_anchored) {
    if (file_index == g_candidate_file_index) {
      g_candidate_bytes_read += bytes_this_read;
    } else {
      g_candidate_file_index = file_index;
      g_candidate_bytes_read = bytes_this_read;
    }
    if (g_candidate_bytes_read >= SWITCH_DEBOUNCE_BYTES) {
      g_reader_anchored = true;
      g_current_file_index = file_index;
      g_current_file_read_end = read_end;
      g_candidate_bytes_read = 0;
    }
    return;
  }
  if (file_index == g_current_file_index) {
    // Back on the already-active file (e.g. a probe into another file's
    // range reverted) -- reset any in-progress candidate.
    g_candidate_file_index = file_index;
    g_candidate_bytes_read = 0;
    if (read_end > g_current_file_read_end) g_current_file_read_end = read_end;
    return;
  }
  if (file_index == g_candidate_file_index) {
    g_candidate_bytes_read += bytes_this_read;
  } else {
    g_candidate_file_index = file_index;
    g_candidate_bytes_read = bytes_this_read;
  }
  if (g_candidate_bytes_read < SWITCH_DEBOUNCE_BYTES) return;

  uint32_t next_idx = (g_current_file_index + 1) % NUM_FILES;
  uint32_t prev_idx = (g_current_file_index + NUM_FILES - 1) % NUM_FILES;
  int direction = (file_index == next_idx) ? 1 : (file_index == prev_idx) ? -1 : 0;
  // REAL BUG FIXED (2026-09-24): a file simply playing out to its end also
  // makes the radio open the next file -- which used to be relayed as a
  // "next" button press, skipping the phone's song every ~4 minutes (one
  // DECLARED_FILE_SIZE lap). If the reader had reached (within two clusters
  // of) the declared end of the file it left, that's end-of-file, not a
  // button.
  bool natural_eof = (direction == 1) &&
      (g_current_file_read_end + 2 * CLUSTER_SIZE >= get_file_declared_size(g_current_file_index));
  bool suppress = g_suppress_next_switch_callback;
  g_suppress_next_switch_callback = false;
  if (suppress) {
    // The switch we were waiting for just landed -- safe to restore the
    // file we left back to full size now (the radio has, by definition,
    // already finished reading everything up to the shrunk size).
    set_file_declared_size(g_prev_file_index_to_restore, g_restore_size_for_prev_file);
#if !defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
    fprintf(stderr, "[track-rotate] confirmed switch %u -> %u, restored size for %u\n",
            g_prev_file_index_to_restore, file_index, g_prev_file_index_to_restore);
#elif defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
    Serial.printf("[track-rotate] confirmed switch %u -> %u, restored size for %u\n",
                  (unsigned)g_prev_file_index_to_restore, (unsigned)file_index,
                  (unsigned)g_prev_file_index_to_restore);
#endif
  }
  g_current_file_index = file_index;
  g_current_file_read_end = read_end;
  g_candidate_bytes_read = 0;
  // direction==0 means a non-adjacent jump (shouldn't happen with a real
  // radio's own sequential file navigation) -- still adopt the new current
  // index so debounce tracking stays correct, just don't relay a command
  // for a jump that doesn't map to a real next/previous.
  if (!suppress && !natural_eof && direction != 0 && g_file_switch_callback) g_file_switch_callback(direction);
}

// Main entry point: call this the instant a real track change is known
// (e.g. the classic's AVRCP track-change hook, forwarded over the existing
// 'C' control channel -- wiring that up on the .ino side is a separate
// step from this header). `new_name11` is the incoming track's 8.3 name,
// already formatted by the caller; pass nullptr to just force an early
// EOF without renaming anything (e.g. for a plain "loading" placeholder).
static void force_track_change(const char *new_name11) {
  // "Read so far, plus one cluster" -- lets whatever's already been read
  // finish its current cluster cleanly rather than truncating mid-cluster,
  // then the NEXT read past that hits true EOF. Never shrinks below what's
  // already been read (which would be nonsensical -- can't un-read bytes).
  uint32_t new_size = g_current_file_read_end + CLUSTER_SIZE;
  if (new_size > DECLARED_FILE_SIZE) new_size = DECLARED_FILE_SIZE;
  g_prev_file_index_to_restore = g_current_file_index;
  g_restore_size_for_prev_file = DECLARED_FILE_SIZE;
  set_file_declared_size(g_current_file_index, new_size);

  uint32_t next_idx = (g_current_file_index + 1) % NUM_FILES;
  if (new_name11) set_file_name(next_idx, new_name11);
#if !defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
  fprintf(stderr, "[track-rotate] force_track_change: shrinking file %u to %u bytes (was %u), "
                  "next file %u\n", g_current_file_index, new_size, DECLARED_FILE_SIZE, next_idx);
#elif defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
  Serial.printf("[track-rotate] force_track_change: shrinking file %u to %u bytes (was %u), "
                "next file %u\n", (unsigned)g_current_file_index, (unsigned)new_size,
                (unsigned)DECLARED_FILE_SIZE, (unsigned)next_idx);
#endif
  g_suppress_next_switch_callback = true;
  g_suppress_deadline_ms = FATDISK_MILLIS() + SUPPRESS_WINDOW_MS;
}
#endif  // FATDISK_MULTI_FILE

// See esp32-s3-msc.ino's own comment (2026-09-17) for the full concurrency
// design rationale: on real hardware this callback must never block
// (TinyUSB's onRead has no safe unbounded wait), so it takes the mutex
// with a short bounded timeout and serves zero-fill on straddle/lock-miss
// instead of retrying. The PC host uses the exact same non-blocking
// design here even though it COULD safely retry (unlike real hardware) --
// deliberately, since the whole point of running this shared code on the
// PC is to observe how the REAL firmware's actual behavior performs, not
// a more lenient PC-only variant.
// out_straddled/out_lock_missed (optional, both default nullptr so the
// real .ino's existing call site is unaffected): diagnostic-only signal
// for the PC host's resource/behavior reporting -- lets it observe how
// often the real firmware's actual non-blocking/zero-fill design would
// trigger against a real live feed, without duplicating this straddle
// logic in a second, hand-copied place just to log it.
static void disk_read_at(uint32_t abs_pos, uint8_t *buffer, uint32_t len,
                          bool *out_straddled = nullptr, bool *out_lock_missed = nullptr) {
#ifdef FATDISK_ALWAYS_SERVE_LIVE
  (void)out_straddled;  // no "straddling a fixed offset" concept in this mode
#endif
  memset(buffer, 0, len);
  uint32_t pos = abs_pos;
  uint32_t remaining = len;
  uint32_t out_off = 0;

  const uint32_t fat_region_start = RESERVED_SECTORS * SECTOR_SIZE;
  const uint32_t fat_region_end = fat_region_start + NUM_FATS * FAT_SECTORS * SECTOR_SIZE;
  const uint32_t root_region_end = fat_region_end + ROOT_DIR_SECTORS * SECTOR_SIZE;
  const uint32_t data_region_start = FIRST_DATA_LBA * SECTOR_SIZE;

  while (remaining > 0) {
    if (pos < fat_region_start) {
      uint32_t n = min(remaining, fat_region_start - pos);
      memcpy(buffer + out_off, g_boot_sector + pos, n);
      pos += n; out_off += n; remaining -= n;
    } else if (pos < fat_region_end) {
      uint32_t rel = (pos - fat_region_start) % (FAT_SECTORS * SECTOR_SIZE);
      uint32_t n = min(remaining, FAT_SECTORS * SECTOR_SIZE - rel);
      memcpy(buffer + out_off, g_fat_sector_cache + rel, n);
      pos += n; out_off += n; remaining -= n;
    } else if (pos < root_region_end) {
      uint32_t rel = pos - fat_region_end;
      uint32_t n = min(remaining, ROOT_DIR_SECTORS * SECTOR_SIZE - rel);
      memcpy(buffer + out_off, g_root_dir_sector + rel, n);
      pos += n; out_off += n; remaining -= n;
    } else {
      uint32_t data_off = pos - data_region_start;
      uint32_t total_data_size = NUM_FILES * DECLARED_FILE_SIZE;
      if (data_off >= total_data_size) break;
      // Remap: every file's own DECLARED_FILE_SIZE-long range aliases the
      // SAME live ring content (see fatdisk_note_file_read()'s own comment
      // and PLAN_NEXT.md's C3 section) -- file_rel_off feeds the exact same
      // ring math as before; file_index is purely a detection signal. With
      // NUM_FILES=1 file_index is always 0 and file_rel_off==data_off, so
      // this reduces to exactly the original single-file behavior.
      [[maybe_unused]] uint32_t file_index = data_off / DECLARED_FILE_SIZE;
      uint32_t file_rel_off = data_off % DECLARED_FILE_SIZE;
      uint32_t n = min(remaining, DECLARED_FILE_SIZE - file_rel_off);

      // B1: link gone quiet for too long (classic crashed/rebooted, not a
      // graceful BT disconnect -- silence-injection already covers that and
      // isn't gated on this) -- serve zero-fill instead of already-played
      // stale ring content. Applies to both design branches below; a stale
      // link means neither "byte-exact continuity from here" (original) nor
      // "near the live edge" (FATDISK_ALWAYS_SERVE_LIVE) means anything
      // trustworthy anymore. g_link_last_frame_ms==0 (never received a
      // frame yet, e.g. very first moments after boot) is deliberately NOT
      // treated as stale here -- that case is already safely zero-filled by
      // disk_valid_bytes() being 0, not by this check.
      bool link_stale = g_link_last_frame_ms != 0 &&
          FATDISK_MILLIS() - g_link_last_frame_ms > LINK_STALE_MS;
      if (link_stale) {
        pos += n; out_off += n; remaining -= n;
        continue;
      }
#ifdef FATDISK_MULTI_FILE
      fatdisk_note_file_read(file_index, file_rel_off, n);
#endif

#ifdef FATDISK_ALWAYS_SERVE_LIVE
      // Muni's design (2026-09-20), CORRECTED after a real, decisive
      // failure of the first attempt (see LIVE_CATCHUP_THRESHOLD_BYTES's
      // own comment for the full history): ignore file_rel_off entirely
      // for CONTENT purposes -- a SCSI READ10 response's content doesn't
      // need to correspond to the requested address the way a real file's
      // bytes would, as far as a sequentially-reading MP3 decoder is
      // concerned. Instead of recomputing "most recent bytes" fresh on
      // every read (the FIRST, broken attempt), maintain a PERSISTENT
      // cursor that advances byte-exactly like the original design
      // (guaranteeing frame continuity between reads), except it jumps
      // forward on its own whenever it's fallen more than
      // LIVE_CATCHUP_THRESHOLD_BYTES behind the live write edge -- so it's
      // never allowed to stay minutes stale, but every read that DOESN'T
      // trigger a catch-up jump is byte-continuous with the one before it.
      bool took_lock = FATDISK_MUTEX_TRY_TAKE_MS(g_ring_mutex, 2);
      if (took_lock) {
        uint32_t wp = g_write_pos;
        uint32_t avail = disk_valid_bytes();
        uint32_t n_safe = min(n, avail);
        // Too early in the session for a safe live-serving window (avail
        // hasn't even cleared one margin+chunk yet) -- leave the buffer
        // zero-filled (the existing safe default from the initial memset),
        // same as a straddle/lock-miss already does elsewhere.
        if (n_safe > 0 && avail > LIVE_SAFETY_MARGIN_BYTES + n_safe) {
          // Logical ring, in stream order: the previous lap is [wp, lap_end),
          // the current lap is [0, wp). [lap_end, DECLARED_FILE_SIZE) is an
          // older lap's leftover tail (see g_ring_lap_end) and is never
          // served. `behind` = written bytes between the cursor and the live
          // edge. History of this path (persistent cursor, catch-up
          // thresholds, silence bridge): see the constants' own comments
          // above and progress/STATUS.md.
          uint32_t lap_end = g_ring_lap_end;
          uint32_t cursor = g_live_read_cursor;
          if (cursor >= lap_end && cursor > wp) cursor = 0;
          bool need_jump = !g_live_read_cursor_init;
          uint32_t behind = 0;
          if (!need_jump) {
            behind = (cursor <= wp) ? (wp - cursor) : ((lap_end - cursor) + wp);
            // Too far behind live -- or AHEAD of the writer, which this
            // arithmetic reports as almost a full lap behind. Either way,
            // jump to near-live.
            need_jump = behind > LIVE_TARGET_LAG_BYTES + LIVE_CATCHUP_THRESHOLD_BYTES;
          }
          if (need_jump) {
            uint32_t target = (LIVE_TARGET_LAG_BYTES > n_safe) ? LIVE_TARGET_LAG_BYTES : n_safe;
            cursor = (wp >= target) ? (wp - target) : (lap_end - (target - wp));
            behind = target;
            g_live_read_cursor_init = true;
            g_silence_bridge_remaining = SILENCE_BRIDGE_BYTES;
            g_silence_offset = SILENCE_LOOP_START;
            g_live_underrun = false;
          }
          // Hysteresis: once underrunning, keep serving silence until the
          // lag is back at the full target, not just one read's worth --
          // otherwise after any dropout (e.g. a classic reboot gap, seen
          // live) the lag settles barely above n_safe and the next small
          // Bluetooth stall underruns again.
          if (behind < n_safe || (g_live_underrun && behind < LIVE_TARGET_LAG_BYTES)) {
            // REAL BUG FIXED (2026-09-24): the old code tolerated the cursor
            // sitting up to LIVE_CATCHUP_THRESHOLD_BYTES AHEAD of safe_edge
            // and still copied n_safe real ring bytes from there -- i.e.
            // bytes past g_write_pos, previous-lap audio spliced in mid-
            // frame on every read. A source running slightly under real
            // time (Bluetooth stalls before the classic's silence injector
            // kicks in, PCM drops) walks a steadily-paced reader into that
            // band, where it stayed: continuous ffmpeg "Header missing"
            // storms for minutes, ended only by a wrap or a reader restart.
            // Now: an underrun serves valid silent frames and holds the
            // cursor, so the writer regains its lead instead.
            if (!g_live_underrun) {
              g_silence_offset = SILENCE_LOOP_START;
              g_live_underrun = true;
            }
            serve_silence(buffer + out_off, n_safe);
            g_live_underruns++;
          } else {
            g_live_underrun = false;
            if (cursor <= wp) {
              memcpy(buffer + out_off, g_ring + cursor, n_safe);  // ends <= wp since behind >= n_safe
              cursor += n_safe;
            } else {
              uint32_t first = lap_end - cursor;
              if (first >= n_safe) {
                memcpy(buffer + out_off, g_ring + cursor, n_safe);
                cursor += n_safe;
              } else {
                memcpy(buffer + out_off, g_ring + cursor, first);
                memcpy(buffer + out_off + first, g_ring, n_safe - first);
                cursor = n_safe - first;
              }
            }
            if (g_silence_bridge_remaining > 0) {
              uint32_t silence_n = min(n_safe, (uint32_t)g_silence_bridge_remaining);
              serve_silence(buffer + out_off, silence_n);
              g_silence_bridge_remaining -= silence_n;
            }
          }
#if !defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
          fprintf(stderr, "[live-debug] wp=%u lap_end=%u cursor=%u behind=%u jumped=%d underrun=%d\n",
                  wp, lap_end, cursor, behind, (int)need_jump, (int)g_live_underrun);
#endif
          g_live_read_cursor = cursor;
        }
#if defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
        else {
          Serial.printf("[live-debug] ZERO-FILL: avail-too-low wp=%u avail=%u "
                        "n_safe=%u need=%u\n",
                wp, avail, n_safe, LIVE_SAFETY_MARGIN_BYTES + n_safe);
        }
#endif
      } else {
        if (out_lock_missed) *out_lock_missed = true;
#if defined(ARDUINO) && defined(FATDISK_LIVE_DEBUG)
        Serial.printf("[live-debug] ZERO-FILL: lock-miss\n");
#endif
      }
      if (took_lock) FATDISK_MUTEX_GIVE(g_ring_mutex);
#else
      bool took_lock = FATDISK_MUTEX_TRY_TAKE_MS(g_ring_mutex, 2);
      uint32_t avail = disk_valid_bytes();
      uint32_t wp = g_write_pos;
      uint32_t unsafe_end = file_rel_off + n + READ_MARGIN_BYTES;
      bool straddling;
      if (unsafe_end <= DECLARED_FILE_SIZE) {
        straddling = (file_rel_off <= wp && wp < unsafe_end);
      } else {
        straddling = (wp >= file_rel_off || wp < unsafe_end - DECLARED_FILE_SIZE);
      }
      if (took_lock && !straddling) {
        if (file_rel_off < avail) {
          uint32_t real_n = min(n, avail - file_rel_off);
          memcpy(buffer + out_off, g_ring + file_rel_off, real_n);
          // TEMP DIAGNOSTIC -- see g_diag_* above. Records the byte actually
          // copied out if this read covers the target position.
          if (file_rel_off <= DIAG_TARGET_POS && DIAG_TARGET_POS < file_rel_off + real_n) {
            g_diag_read_byte = g_ring[DIAG_TARGET_POS];
            g_diag_read_count++;
          }
        }
        g_last_read_offset = (file_rel_off + n) % DECLARED_FILE_SIZE;
      } else {
        if (out_straddled && straddling) *out_straddled = true;
        if (out_lock_missed && !took_lock) *out_lock_missed = true;
      }
      if (took_lock) FATDISK_MUTEX_GIVE(g_ring_mutex);
#endif  // FATDISK_ALWAYS_SERVE_LIVE

      pos += n; out_off += n; remaining -= n;
    }
  }
}

#endif  // FAT_DISK_SHARED_H
