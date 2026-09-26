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
#include <string>
#include <cstring>
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


// Kenwood-pattern reader through the REAL serving path (disk_read_at) with the
// writer running: open = start, 512 KB-stride probes, last 2 KB, 40 KB head,
// re-read from 0 with a 58 KB burst, then 2 KB every 128 ms. Scripted: play,
// natural end (2.5 s pause) into the next file, Next mid-file, a Back that
// restarts, a double Back. The natural end is a real one: file 1 played to its
// full size (~4 simulated minutes). Returns problems found (0 = pass).
struct KwResult { int underrun_reads, underrun_after_button, violations, seam_breaks, opens; };
static KwResult run_kenwood(bool verbose) {
  memset(g_ring, 0, DECLARED_FILE_SIZE);
  g_write_pos = 0; g_total_written = 0; g_ring_lap_end = DECLARED_FILE_SIZE;
  g_live_read_cursor = 0; g_live_read_cursor_init = false;
  g_live_underrun = false; g_live_underruns = 0; g_silence_bridge_remaining = 0;
  g_open_file = 0xFFFFFFFF; g_live_max_lag = 0; g_live_grace_left = 0;
  g_any_file_read = false; g_reader_anchored = false; g_file_switch_callback = nullptr;
  const uint32_t FRAME = 418, RD = 2048;
  const uint32_t data_start = FIRST_DATA_LBA * SECTOR_SIZE;
  uint64_t written = 0; double owed = 0; uint8_t frame[FRAME];
  uint8_t buf[RD];
  KwResult r{0, 0, 0, 0, 0};
  bool last_open_natural = false;
  uint32_t file = 0, pos = 0;
  int64_t last_end = -1;  // stream offset just past the last playback byte served
  bool expect_seam = false;
  int next_read_ms = 0, eof_until = -1;
  auto rd = [&](uint32_t f, uint32_t off) {
    g_link_last_frame_ms = FATDISK_MILLIS();
    uint32_t u0 = g_live_underruns;
    disk_read_at(data_start + f * DECLARED_FILE_SIZE + off, buf, RD);
    return g_live_underruns != u0;
  };
  auto open = [&](uint32_t f, bool after_natural_end) {
    r.opens++;
    rd(f, 0);
    for (uint32_t off = 0x7f800; off < DECLARED_FILE_SIZE; off += 0x80000) rd(f, off);
    rd(f, DECLARED_FILE_SIZE - RD);
    for (uint32_t off = 0; off < 40 * 1024; off += RD) rd(f, off);
    file = f; pos = 0;
    expect_seam = after_natural_end;
    last_open_natural = after_natural_end;
  };
  auto play_read = [&]() {  // one playback read at `pos`, validated
    bool under = rd(file, pos);
    pos += RD;
    if (under) { r.underrun_reads++; if (!last_open_natural) r.underrun_after_button++; return; }
    int64_t at = locate(buf, RD, written);
    if (at < 0) { r.violations++; if (verbose) printf("  kw: VIOLATION at file %u pos %u\n", file, pos - RD); return; }
    if (expect_seam) {
      if (at != last_end) { r.seam_breaks++; if (verbose) printf("  kw: seam off by %lld\n", (long long)(at - last_end)); }
      expect_seam = false;
    }
    last_end = at + RD;
  };
  // Script (ms of simulated time -> action).
  // Times follow the file length, so the natural end always happens first.
  const int FILE_MS = (int)((uint64_t)DECLARED_FILE_SIZE * 1000 / 16000);
  const int T_START = 5000, T_NEXT = T_START + FILE_MS + 30000;
  const int T_BACK_RESTART = T_NEXT + 20000, T_DOUBLE_BACK = T_NEXT + 40000;
  bool did_next = false, did_restart = false, did_double = false, started = false;
  uint32_t burst_left = 0;
  for (int t = 0; t < T_DOUBLE_BACK + 20000; t++) {
    owed += 16000 / 1000.0;
    while (owed >= FRAME) {
      for (uint32_t i = 0; i < FRAME; i++) frame[i] = stream_byte(written + i);
      disk_append(frame, FRAME, false);
      written += FRAME; owed -= FRAME;
    }
    if (!started && t == T_START) { open(0, false); burst_left = 58 * 1024; started = true; }
    if (!started) continue;
    if (!did_next && t == T_NEXT) { open((file + 1) % NUM_FILES, false); burst_left = 58 * 1024; did_next = true; eof_until = -1; }
    if (!did_restart && t == T_BACK_RESTART) { open(file, false); burst_left = 58 * 1024; did_restart = true; eof_until = -1; }
    if (!did_double && t == T_DOUBLE_BACK) {
      open(file, false); open((file + NUM_FILES - 1) % NUM_FILES, false);
      burst_left = 58 * 1024; did_double = true; eof_until = -1;
    }
    if (eof_until >= 0) {
      if (t >= eof_until) { eof_until = -1; open((file + 1) % NUM_FILES, true); burst_left = 58 * 1024; }
      continue;
    }
    while (burst_left > 0) { play_read(); burst_left -= RD; }
    if (t >= next_read_ms) {
      next_read_ms = t + 128;
      if (pos >= DECLARED_FILE_SIZE) { eof_until = t + 2500; continue; }  // natural end: the radio's pause
      play_read();
    }
  }
  (void)frame;
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

  {
    // Mount/Next/Back: no silence at all. Natural end (radio pauses 2.5 s,
    // then wants 58 KB): seamless, so up to (58 KB - pause's worth + the
    // 12 KB lag rebuilt) of silence -- ~1 s, once per file.
    KwResult k = run_kenwood(true);
    bool ok = k.underrun_after_button == 0 && k.underrun_reads <= 12 && k.violations == 0 && k.seam_breaks == 0;
    failures += !ok;
    printf("%-40s opens=%d underrun_reads: button/mount=%d natural_end=%d violations=%d seam_breaks=%d  %s\n",
           "Kenwood open pattern, real serve path", k.opens, k.underrun_after_button,
           k.underrun_reads - k.underrun_after_button, k.violations, k.seam_breaks, ok ? "OK" : "FAIL");
  }

  // Switch detector, driven by a model of the real Kenwood, from the car
  // trace (progress/CAR_SESSION_RESULTS.md, 2026-09-25): every read is 2 KB;
  // opening a file reads its start, a 2 KB probe every 512 KB and its LAST
  // 2 KB, then plays from 0; size and chain are read once, at open; a
  // natural end pauses 1.9-3.5 s before opening the next file; Back
  // restarts the file, a quick second Back opens the previous one; the
  // folder-wrap option is on.
  g_file_switch_callback = on_switch;
  init_file_names();
  g_any_file_read = false; g_reader_anchored = false;
  const uint32_t RD = 2048;
  struct Kenwood {
    uint32_t file = 0, pos = 0, size = 0;
    void open(uint32_t f) {
      file = f; pos = 0; size = get_file_declared_size(f);
      fatdisk_note_file_read(f, 0, 2048);
      for (uint32_t off = 0x7f800; off < size; off += 0x80000) fatdisk_note_file_read(f, off, 2048);
      fatdisk_note_file_read(f, size - 2048, 2048);
    }
    // Plays n reads; at the end of the file, pauses gap_ms, opens the next.
    void play(int n, int gap_ms = 1900) {
      for (int i = 0; i < n; i++) {
        if (pos >= size) {
          std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
          open((file + 1) % NUM_FILES);
          continue;
        }
        fatdisk_note_file_read(file, pos, 2048);
        pos += 2048;
      }
    }
    void next() { open((file + 1) % NUM_FILES); }
    void back_restart() { open(file); }
    void back_previous() { open(file); open((file + NUM_FILES - 1) % NUM_FILES); }
  };
  auto expect = [&](const char *what, bool ok) {
    printf("  %-58s %s\n", what, ok ? "OK" : "FAIL");
    failures += !ok;
  };
  auto relays = [&](int n, int p) { return g_callbacks_next == n && g_callbacks_prev == p; };
  const int FILE_READS = DECLARED_FILE_SIZE / RD;
  expect("3 full-length files, fixed name Stream.mp3",
         get_file_declared_size(0) == DECLARED_FILE_SIZE && get_file_declared_size(1) == DECLARED_FILE_SIZE &&
         get_file_declared_size(2) == DECLARED_FILE_SIZE && g_long_len == 10 && g_long_name[0] == 'S');
  int n0 = g_callbacks_next, p0 = g_callbacks_prev;
  Kenwood r;
  r.open(1);  // mount: resumes the remembered track
  r.play(40);
  expect("mount on the remembered track: nothing relayed", relays(n0, p0) && g_current_file_index == 1);
  r.next();
  r.play(20);
  expect("Next mid-file relayed once", relays(n0 + 1, p0));
  r.back_restart();
  r.play(20);
  expect("single Back (restart) not relayed", relays(n0 + 1, p0));
  r.back_previous();
  r.play(20);
  expect("double Back (previous file) relayed once", relays(n0 + 1, p0 + 1) && r.file == 1);
  r.back_previous();
  r.play(20);
  expect("double Back from file 2 relayed", relays(n0 + 1, p0 + 2) && r.file == 0);
  r.back_previous();
  r.play(20);
  expect("Back wraps file 1 -> file 3, relayed prev", relays(n0 + 1, p0 + 3) && r.file == 2);
  r.next();
  r.play(20);
  expect("Next wraps file 3 -> file 1, relayed next", relays(n0 + 2, p0 + 3) && r.file == 0);
  r.play(FILE_READS + 20, 1900);
  expect("natural end, 1.9 s gap: not relayed", relays(n0 + 2, p0 + 3) && r.file == 1);
  r.play(FILE_READS + 20, 3500);
  expect("natural end, 3.5 s gap: not relayed", relays(n0 + 2, p0 + 3) && r.file == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(READER_IDLE_RESET_MS + 200));
  r.next();
  r.play(20);
  expect("Next after a >3 s pause: adopted silently (known trade-off)", relays(n0 + 2, p0 + 3));
  r.next();
  r.play(20);
  expect("...and the next Next is relayed again", relays(n0 + 3, p0 + 3));

  printf(failures ? "FAILED (%d)\n" : "ALL OK\n", failures);
  return failures ? 1 : 0;
}
