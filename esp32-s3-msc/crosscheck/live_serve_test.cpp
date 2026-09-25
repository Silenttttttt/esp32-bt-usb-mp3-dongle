// Host-only test of the FATDISK_ALWAYS_SERVE_LIVE read path and the
// FATDISK_MULTI_FILE switch detector in fat_disk_shared.h.
//
//   g++ -O2 -std=c++17 -Wall -DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -o live_serve_test live_serve_test.cpp
//   ./live_serve_test
//
// The writer appends whole "frames" of a numbered byte stream (stream byte k
// is byte k%4 of the little-endian uint32 k/4), so any served chunk can be
// mapped back to the exact stream range it came from. Every read that isn't
// silence must be one contiguous stream range that was fully written and not
// yet overwritten -- the property the old code broke (serving bytes past
// g_write_pos, and the stale tail left by disk_append's skip-to-0 wrap).

#include <cstdint>
#include "../fat_disk_shared.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>

static uint8_t stream_byte(uint64_t k) {
  uint32_t w = (uint32_t)(k / 4);
  return (uint8_t)(w >> (8 * (k % 4)));
}

// Returns the stream offset `chunk` is a contiguous copy of, or -1.
static int64_t locate(const uint8_t *chunk, uint32_t n, uint64_t written) {
  if (n < 12) return -2;
  for (int a = 0; a < 4; a++) {
    uint32_t w = (uint32_t)chunk[a] | ((uint32_t)chunk[a + 1] << 8) |
                 ((uint32_t)chunk[a + 2] << 16) | ((uint32_t)chunk[a + 3] << 24);
    int64_t start = (int64_t)w * 4 - a;  // candidate stream offset of chunk[0]
    if (start < 0 || (uint64_t)start + n > written) continue;
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) ok = (chunk[i] == stream_byte(start + i));
    if (ok) return start;
  }
  return -1;
}

static int g_callbacks_next = 0, g_callbacks_prev = 0;
static void on_switch(int dir) { (dir > 0 ? g_callbacks_next : g_callbacks_prev)++; }

struct Result { int real, underrun_reads, bridged, violations, jumps, episodes; };

// rate: writer bytes per simulated second; stall: every stall_every_ms the
// writer produces nothing for stall_ms (a Bluetooth delivery gap).
static Result run(double rate, int stall_every_ms, int stall_ms, bool burst, int sim_seconds,
                  uint32_t reader_file, bool verbose, int outage_at_ms = -1, int outage_ms = 0) {
  memset(g_ring, 0, DECLARED_FILE_SIZE);
  g_write_pos = 0; g_total_written = 0; g_ring_lap_end = DECLARED_FILE_SIZE;
  g_live_read_cursor = 0; g_live_read_cursor_init = false;
  g_live_underrun = false; g_live_underruns = 0; g_silence_bridge_remaining = 0;

  const uint32_t FRAME = 418;
  uint64_t written = 0;       // stream bytes appended so far
  double owed = 0;            // fractional writer bytes carried between ms ticks
  uint8_t frame[FRAME];
  const uint32_t data_start = FIRST_DATA_LBA * SECTOR_SIZE;
  uint32_t file_rel = 0;
  int64_t expect_next = -1;   // stream offset the next real read should start at
  Result r{0, 0, 0, 0, 0, 0};
  bool in_underrun = false;
  std::vector<uint8_t> buf(CLUSTER_SIZE);

  for (int t = 0; t < sim_seconds * 1000; t++) {
    bool stalled = stall_every_ms > 0 && (t % stall_every_ms) < stall_ms;
    bool outage = outage_at_ms >= 0 && t >= outage_at_ms && t < outage_at_ms + outage_ms;
    // burst: delivery stalls but nothing is lost -- it arrives late in a
    // catch-up burst (typical A2DP). !burst: the stalled time is lost.
    // outage: writer gone (e.g. a classic reboot) -- nothing produced, nothing owed.
    if (!outage && (!stalled || burst)) owed += rate / 1000.0;
    while (!outage && !stalled && owed >= FRAME) {
      for (uint32_t i = 0; i < FRAME; i++) frame[i] = stream_byte(written + i);
      disk_append(frame, FRAME, false);
      written += FRAME;
      owed -= FRAME;
      g_link_last_frame_ms = FATDISK_MILLIS();
    }
    if (t % 256 == 0 && t > 3000) {  // steady 4096 B every 256 ms = 16000 B/s
      uint32_t under_before = g_live_underruns;
      uint32_t bridge_before = g_silence_bridge_remaining;
      disk_read_at(data_start + reader_file * DECLARED_FILE_SIZE + file_rel, buf.data(), CLUSTER_SIZE);
      file_rel = (file_rel + CLUSTER_SIZE) % DECLARED_FILE_SIZE;
      bool underrun = g_live_underruns != under_before;
      bool bridged = bridge_before > 0 || g_silence_bridge_remaining > 0;
      if (underrun && !in_underrun) r.episodes++;
      in_underrun = underrun;
      if (underrun) { r.underrun_reads++; continue; }
      int64_t at = locate(buf.data(), CLUSTER_SIZE, written);
      if (bridged || at == -1) {
        // A bridged read's leading bytes are silence; check just the tail.
        // A jump inside this very call sets a fresh bridge that overwrites
        // this read's first SILENCE_BRIDGE_BYTES too.
        uint32_t fresh = (bridge_before == 0 && g_silence_bridge_remaining > 0) ? SILENCE_BRIDGE_BYTES : bridge_before;
        uint32_t skip = std::min<uint32_t>(fresh, CLUSTER_SIZE);
        if (bridge_before == 0 && g_silence_bridge_remaining > 0) r.jumps++;
        if (bridged && skip < CLUSTER_SIZE) {
          int64_t at2 = locate(buf.data() + skip, CLUSTER_SIZE - skip, written);
          if (at2 < 0 && CLUSTER_SIZE - skip >= 12) {
            r.violations++;
            if (verbose) printf("  t=%d VIOLATION in bridged tail\n", t);
          } else if (at2 >= 0) {
            expect_next = at2 + (CLUSTER_SIZE - skip);
          }
        } else if (!bridged) {
          r.violations++;
          if (verbose && r.violations <= 5) printf("  t=%d VIOLATION: read is not a contiguous written stream range\n", t);
        }
        r.bridged += bridged;
        continue;
      }
      if (expect_next >= 0 && at != expect_next) r.jumps++;
      if ((uint64_t)at + CLUSTER_SIZE > written) r.violations++;           // unwritten bytes
      if (written - (uint64_t)at > DECLARED_FILE_SIZE) r.violations++;     // overwritten bytes
      expect_next = at + CLUSTER_SIZE;
      r.real++;
    }
  }
  return r;
}

int main() {
  g_ring = (uint8_t *)malloc(DECLARED_FILE_SIZE);
  g_ring_mutex = FATDISK_MUTEX_CREATE();
  build_boot_sector(); build_fat(); build_root_dir();
  int failures = 0;

  printf("LIVE_TARGET_LAG_BYTES=%u (%.2fs)\n", LIVE_TARGET_LAG_BYTES, LIVE_TARGET_LAG_BYTES / 16000.0);
  struct Case { const char *name; double rate; int every, stall; bool burst; int secs; };
  Case cases[] = {
    {"exact rate, no stalls",                 16000, 0,     0,   false, 600},
    {"150ms stall/5s, caught up (jitter)",    16000, 5000,  150, true,  900},
    {"400ms stall/3s, caught up (jitter)",    16000, 3000,  400, true,  900},
    {"0.1% slow + 150ms lost/10s",            15984, 10000, 150, false, 900},
    {"writer 0.1% fast",                      16016, 0,     0,   false, 900},
  };
  for (auto &c : cases) {
    Result r = run(c.rate, c.every, c.stall, c.burst, c.secs, 0, false);
    printf("  (underrun episodes: %d)\n", r.episodes);
    bool ok = r.violations == 0 && r.real > 0;
    failures += !ok;
    printf("%-40s real=%d underrun=%d bridged=%d jumps=%d violations=%d  %s\n",
           c.name, r.real, r.underrun_reads, r.bridged, r.jumps, r.violations, ok ? "OK" : "FAIL");
  }

  {
    Result r = run(16000, 3000, 400, true, 600, 0, false, 60000, 2000);
    bool ok = r.violations == 0 && r.episodes == 1;
    failures += !ok;
    printf("%-40s real=%d underrun=%d episodes=%d violations=%d  %s\n", "2s outage, then 400ms stall/3s jitter",
           r.real, r.underrun_reads, r.episodes, r.violations, ok ? "OK" : "FAIL (want exactly 1 episode)");
  }

  // Switch detector: a file played to its end, then the next file, must NOT
  // relay a button press; a mid-file jump to the next file must.
  g_file_switch_callback = on_switch;
  g_current_file_index = 0; g_candidate_file_index = 0; g_candidate_bytes_read = 0;
  g_current_file_read_end = 0; g_suppress_next_switch_callback = false;
  g_any_file_read = false;
  for (uint32_t off = 3000000; off < DECLARED_FILE_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(0, off, CLUSTER_SIZE);  // radio resumed mid-file, plays to EOF
  for (uint32_t off = 0; off < 8 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(1, off, CLUSTER_SIZE);
  bool eof_ok = g_callbacks_next == 0 && g_current_file_index == 1;
  for (uint32_t off = 8 * CLUSTER_SIZE; off < 40 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(1, off, CLUSTER_SIZE);
  for (uint32_t off = 0; off < 8 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(2, off, CLUSTER_SIZE);  // button: left file 1 at ~160KB
  bool press_ok = g_callbacks_next == 1 && g_current_file_index == 2;
  // Reader restarts after an idle gap on file 0 (relaunch / radio power-up):
  // adopted silently, not relayed as "next" (2 -> 0 is adjacent).
  std::this_thread::sleep_for(std::chrono::milliseconds(READER_IDLE_RESET_MS + 200));
  for (uint32_t off = 0; off < 8 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(0, off, CLUSTER_SIZE);
  bool restart_ok = g_callbacks_next == 1 && g_callbacks_prev == 0 && g_current_file_index == 0;
  // Remount: host probes read a few KB scattered across files, then the
  // reader streams file 2. Must anchor on file 2 without relaying anything.
  std::this_thread::sleep_for(std::chrono::milliseconds(READER_IDLE_RESET_MS + 200));
  fatdisk_note_file_read(0, 32768, 2048);
  fatdisk_note_file_read(1, 0, 4096);
  fatdisk_note_file_read(2, DECLARED_FILE_SIZE - 512, 512);
  for (uint32_t off = 0; off < 8 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(2, off, CLUSTER_SIZE);
  restart_ok = restart_ok && g_callbacks_next == 1 && g_callbacks_prev == 0 && g_current_file_index == 2;
  printf("natural EOF not relayed: %s; mid-file Next relayed once: %s; reader restart not relayed: %s\n",
         eof_ok ? "OK" : "FAIL", press_ok ? "OK" : "FAIL", restart_ok ? "OK" : "FAIL");
  failures += !eof_ok + !press_ok + !restart_ok;

  // Title-driven early end (the .ino's TITLE: handler): a NEW title renames
  // every file, then ends the open file just past what's been read, so the
  // radio opens the next file -- which carries the new name. The hop must
  // not be relayed. Plays the reader like car_sim/FatFs: stop at the
  // declared size, then open the next file.
  init_file_names();
  auto play_until_eof_then_next = [](uint32_t file, uint32_t from) {
    uint32_t off = from;
    while (off < get_file_declared_size(file)) { fatdisk_note_file_read(file, off, CLUSTER_SIZE); off += CLUSTER_SIZE; }
    uint32_t next = (file + 1) % NUM_FILES;
    for (uint32_t o = 0; o < 8 * CLUSTER_SIZE; o += CLUSTER_SIZE) fatdisk_note_file_read(next, o, CLUSTER_SIZE);
    return off;
  };
  auto long_name_is = [](const char *want) {
    uint32_t n = strlen(want);
    if (g_long_len != n) return false;
    for (uint32_t i = 0; i < n; i++) if (g_long_name[i] != (uint8_t)want[i]) return false;
    return true;
  };
  int next0 = g_callbacks_next, prev0 = g_callbacks_prev;
  // (a) Button: radio on file 2 presses Next -> file 0 (relayed), THEN the
  // phone's new title arrives while file 0 plays with the old name.
  for (uint32_t off = 8 * CLUSTER_SIZE; off < 40 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(2, off, CLUSTER_SIZE);
  for (uint32_t off = 0; off < 8 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(0, off, CLUSTER_SIZE);
  bool a_relayed = g_callbacks_next == next0 + 1 && g_current_file_index == 0;
  bool a_changed = set_title_utf8("Song B", 6);
  if (a_changed) force_track_change(nullptr);
  uint32_t shrunk = get_file_declared_size(0);
  play_until_eof_then_next(0, 8 * CLUSTER_SIZE);
  bool a_ok = a_relayed && a_changed && shrunk < DECLARED_FILE_SIZE && g_current_file_index == 1 &&
              g_callbacks_next == next0 + 1 && get_file_declared_size(0) == DECLARED_FILE_SIZE &&
              long_name_is("Song B.mp3");
  // (b) The same title resent (every 5s) must not hop again.
  bool b_ok = !set_title_utf8("Song B", 6) && get_file_declared_size(1) == DECLARED_FILE_SIZE;
  // (c) Song changes on the phone mid-file (no button): hop, not relayed.
  for (uint32_t off = 8 * CLUSTER_SIZE; off < 60 * CLUSTER_SIZE; off += CLUSTER_SIZE)
    fatdisk_note_file_read(1, off, CLUSTER_SIZE);
  if (set_title_utf8("Song C", 6)) force_track_change(nullptr);
  play_until_eof_then_next(1, 60 * CLUSTER_SIZE);
  bool c_ok = g_current_file_index == 2 && g_callbacks_next == next0 + 1 && g_callbacks_prev == prev0 &&
              get_file_declared_size(1) == DECLARED_FILE_SIZE && long_name_is("Song C.mp3");
  printf("title after Next hops once, unrelayed: %s; resent title no hop: %s; mid-file song change hops: %s\n",
         a_ok ? "OK" : "FAIL", b_ok ? "OK" : "FAIL", c_ok ? "OK" : "FAIL");
  failures += !a_ok + !b_ok + !c_ok;

  printf(failures ? "FAILED (%d)\n" : "ALL OK\n", failures);
  return failures ? 1 : 0;
}
