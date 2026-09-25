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

  // Switch detector + buffer files + title-driven early end, driven by a
  // model radio that behaves like car_sim / FatFs: it reads a file's name
  // and size when it OPENS it, reads one cluster at a time, and at that
  // size opens the next file. Next/Back open a neighbor from mid-file.
  g_file_switch_callback = on_switch;
  init_file_names();
  g_any_file_read = false; g_reader_anchored = false;
  struct Radio {
    uint32_t file = 0, pos = 0, size = 0;
    std::string name;
    bool honors_recheck = false;  // true: re-reads its size every cluster
    // The real Kenwood (car trace 2026-09-25): on every open it reads the
    // file's start, a 2 KB probe every 512 KB, and the LAST 2 KB, then plays from 0.
    bool kenwood_probe = false;
    void open(uint32_t f) {
      file = f; pos = 0; size = get_file_declared_size(f);
      name.clear();
      for (uint32_t i = 0; i < g_long_len; i++) name += (char)g_long_name[i];
      if (kenwood_probe) {
        fatdisk_note_file_read(f, 0, 2048);
        for (uint32_t off = 0x7f800; off < size; off += 0x80000) fatdisk_note_file_read(f, off, 2048);
        fatdisk_note_file_read(f, size - 2048, 2048);
      }
    }
    void step() {  // one cluster; natural end opens the next file
      if (honors_recheck) size = get_file_declared_size(file);
      if (pos >= size) { open((file + 1) % NUM_FILES); return; }
      fatdisk_note_file_read(file, pos, CLUSTER_SIZE);
      pos += CLUSTER_SIZE;
    }
    void steps(int n) { for (int i = 0; i < n; i++) step(); }
    void press(int dir) { open((file + NUM_FILES + dir) % NUM_FILES); }
  };
  auto title = [](const char *t) {  // the .ino's TITLE: handler
    if (set_title_utf8(t, strlen(t)) && fatdisk_reader_active()) force_track_change(nullptr);
  };
  auto expect = [&](const char *what, bool ok) {
    printf("  %-58s %s\n", what, ok ? "OK" : "FAIL");
    failures += !ok;
  };
  const int BUF = BUFFER_FILE_SIZE / CLUSTER_SIZE;
  Radio r;
  title("Song A");
  r.open(0);
  r.steps(100);
  expect("starts on the full-length file 0", r.file == 0 && r.size == DECLARED_FILE_SIZE);
  int n0 = g_callbacks_next, p0 = g_callbacks_prev;

  // 1. Next mid-file: buffer (old name) -> relayed once -> title -> the
  //    buffer ends -> full-length file with the new name.
  r.press(+1);
  expect("Next opens a short buffer", r.file == 1 && r.size == BUFFER_FILE_SIZE);
  r.steps(6);  // ~1.5s: debounce reached, relayed
  expect("Next relayed once", g_callbacks_next == n0 + 1);
  title("Song B");
  r.steps(BUF + 2);
  expect("buffer ends -> full-length file", r.file == 2 && r.size == DECLARED_FILE_SIZE);
  expect("...showing the new name", r.name == "Song B.mp3");
  r.steps(60);
  expect("no extra relay", g_callbacks_next == n0 + 1 && g_callbacks_prev == p0);

  // 2. Back mid-file: same, in the other direction.
  r.press(-1);
  expect("Back opens a short buffer", r.file == 1 && r.size == BUFFER_FILE_SIZE);
  r.steps(6);
  expect("Back relayed once", g_callbacks_prev == p0 + 1 && g_callbacks_next == n0 + 1);
  title("Song A2");
  r.steps(BUF + 2);
  expect("buffer ends -> full-length file with the new name",
         r.file == 2 && r.size == DECLARED_FILE_SIZE && r.name == "Song A2.mp3");
  r.steps(60);

  // 3. The title resent every 5s: nothing happens.
  uint32_t before = get_file_declared_size(r.file);
  title("Song A2");
  expect("resent title changes nothing", get_file_declared_size(r.file) == before);

  // 4. Song changes on the phone mid-file (no button). This radio read the
  //    size at open, so it plays on; the name catches up at the file end.
  title("Song C");
  r.steps(60);
  expect("size-at-open radio keeps playing the open file", r.file == 2);
  r.steps((DECLARED_FILE_SIZE / CLUSTER_SIZE) + BUF + 4);  // to its end, through the buffer
  expect("file end -> buffer -> full-length file, new name, nothing relayed",
         r.size == DECLARED_FILE_SIZE && r.name == "Song C.mp3" &&
         g_callbacks_next == n0 + 1 && g_callbacks_prev == p0 + 1);

  // 5. A radio that re-reads the size hops right away on a mid-file song change.
  r.honors_recheck = true;
  r.steps(40);
  uint32_t was = r.file;
  title("Song D");
  r.steps(4);
  expect("re-reading radio hops to a full-length file with the new name",
         r.file == (was + 1) % NUM_FILES && r.size == DECLARED_FILE_SIZE && r.name == "Song D.mp3" &&
         g_callbacks_next == n0 + 1);
  r.honors_recheck = false;
  r.steps(60);

  // 6. The radio opened a file while it was cut and reaches the cut after
  //    the 10s restore (01:00:02 / 01:11:12): natural, not relayed.
  title("Song E");  // cuts the playing file 1 cluster past what's read
  r.press(0);       // (re)open it now: this radio caches the cut size
  uint32_t cut_file = r.file, cut = r.size;
  // Play it at a pace that crosses the 10s restore before reaching the cut.
  uint32_t pace_ms = (SUPPRESS_WINDOW_MS + 1500) / (cut / CLUSTER_SIZE);
  bool restored_before_end = false;
  while (r.file == cut_file) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    if (r.pos + CLUSTER_SIZE >= cut) restored_before_end = get_file_declared_size(cut_file) != cut;
    r.step();
  }
  r.steps(4);
  expect("cut reached after the restore is not relayed",
         cut < DECLARED_FILE_SIZE && restored_before_end && g_callbacks_next == n0 + 1 &&
         g_callbacks_prev == p0 + 1);
  r.steps(60);

  // 7. A reader restarting after a gap (radio power-up, GUI relaunch, the
  //    PC probing at mount) is adopted silently.
  std::this_thread::sleep_for(std::chrono::milliseconds(READER_IDLE_RESET_MS + 200));
  fatdisk_note_file_read(0, 32768, 2048);
  fatdisk_note_file_read(1, 0, 4096);
  r.open((r.file + 1) % NUM_FILES);
  r.steps(20);
  expect("reader restart not relayed", g_callbacks_next == n0 + 1 && g_callbacks_prev == p0 + 1);

  // 8. The Kenwood's open-time probes (they read each file's last sector)
  //    must not make a mid-file Next look like a natural end.
  r.kenwood_probe = true;
  while (r.size != DECLARED_FILE_SIZE) r.step();
  r.steps(100);
  {
    int n1 = g_callbacks_next, p1 = g_callbacks_prev;
    r.press(+1);
    r.steps(6);
    expect("Kenwood probes: Next mid-file relayed", g_callbacks_next == n1 + 1 && g_callbacks_prev == p1);
    title("Song K");
    r.steps(BUF + 2);
    r.steps(60);
    expect("Kenwood probes: buffer end -> full file, no extra relay",
           r.size == DECLARED_FILE_SIZE && g_callbacks_next == n1 + 1 && g_callbacks_prev == p1);
    r.steps((DECLARED_FILE_SIZE / CLUSTER_SIZE) + BUF + 4);
    expect("Kenwood probes: natural file end not relayed", g_callbacks_next == n1 + 1 && g_callbacks_prev == p1);
    r.press(-1);
    r.steps(6);
    expect("Kenwood probes: Back mid-file relayed", g_callbacks_prev == p1 + 1);
  }
  r.kenwood_probe = false;
  n0 = g_callbacks_next - 1; p0 = g_callbacks_prev - 1;  // later checks expect "one relay so far"
  r.steps(60);

#if defined(EARLY_END_FAT) || defined(EARLY_END_READ_ERROR)
  // Early-end variants: a new title cuts the playing (full-length) file.
  {
    auto fat_entry = [](uint32_t cl) -> uint16_t {
      const uint8_t *p = g_fat_sector_cache + (cl * 3) / 2;
      return (cl & 1) ? (uint16_t)((p[0] >> 4) | (p[1] << 4)) : (uint16_t)(p[0] | ((p[1] & 0x0F) << 8));
    };
    auto read_err = [](uint32_t file, uint32_t rel) {
      uint8_t buf[SECTOR_SIZE]; bool err = false;
      g_link_last_frame_ms = FATDISK_MILLIS();  // link alive, else the stale-link path serves zeros first
      disk_read_at(FIRST_DATA_LBA * SECTOR_SIZE + file * DECLARED_FILE_SIZE + rel, buf, SECTOR_SIZE,
                   nullptr, nullptr, &err);
      return err;
    };
    while (r.size != DECLARED_FILE_SIZE) r.step();
    r.steps(40);
    uint32_t f = r.file;
    title("Song V");
    uint32_t end = get_file_declared_size(f);
    uint32_t last_cl = 2 + f * DATA_CLUSTERS + (end - 1) / CLUSTER_SIZE;
    bool v_ok = end < DECLARED_FILE_SIZE;
#ifdef EARLY_END_FAT
    v_ok = v_ok && fat_entry(last_cl) == 0xFFF && fat_entry(last_cl - 1) == last_cl;
#endif
#ifdef EARLY_END_READ_ERROR
    v_ok = v_ok && read_err(f, end) && read_err(f, end + 5 * CLUSTER_SIZE) && !read_err(f, end - SECTOR_SIZE) &&
           !read_err((f + 1) % NUM_FILES, end);
#endif
    r.honors_recheck = true;  // stands in for "the radio saw the cut", whichever way
    r.steps(4);
    r.honors_recheck = false;
    r.steps(8);
    v_ok = v_ok && r.file == (f + 1) % NUM_FILES && fat_entry(last_cl) == last_cl + 1 && !read_err(f, end) &&
           g_callbacks_next == n0 + 1;
    expect("early-end variant applied, then undone after the switch", v_ok);
  }
#endif

  printf(failures ? "FAILED (%d)\n" : "ALL OK\n", failures);
  return failures ? 1 : 0;
}
