// v2: fixes a flawed corruption check in s3_realtime_sim.cpp (that
// version's heuristic couldn't tell "two adjacent, legitimately different
// writer chunks sharing one fixed-size read window" from a real splice --
// produced a huge false-positive count, not a real bug). This version
// embeds a single CONTINUOUS, globally-monotonic 32-bit counter at every
// 4-byte-aligned position across the ENTIRE writer stream (independent of
// variable writer chunk boundaries), so a read window's values can be
// checked for real internal consistency: every 4-byte slot's value must
// equal (ring-relative byte offset / 4) mod 2^32 semantics tracked via a
// per-slot expected value computed from the slot's OWN ring position and
// how many times the ring has wrapped past it -- a real splice would show
// a value from a stale lap, which this catches precisely, without
// flagging ordinary adjacent-chunk boundaries as false positives.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <vector>
using namespace std;
using namespace std::chrono;

static const uint32_t SECTOR_SIZE = 512;
static const uint32_t SECTORS_PER_CLUSTER = 8;
static const uint32_t DATA_CLUSTERS = 117;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;

static uint8_t *g_ring;
static atomic<uint32_t> g_write_pos{0};
static atomic<uint32_t> g_total_written{0};
static atomic<uint32_t> g_last_read_offset{0};
static mutex g_ring_mutex;
// TEST-ONLY, not part of the real firmware: g_total_written above is now
// intentionally capped at DECLARED_FILE_SIZE (2026-09-17 overflow fix,
// see esp32-s3-msc.ino), so it can no longer double as this test's
// ground-truth "how many bytes ever written" counter for verification
// purposes -- this uncapped counter, updated under the same lock, fills
// that role instead without affecting the real capped logic at all.
static uint64_t g_ground_truth_written = 0;

static atomic<bool> g_stop{false};
static atomic<uint64_t> g_writes_done{0};
static atomic<uint64_t> g_write_protected_rejections{0};
static atomic<uint64_t> g_reads_done{0};
static atomic<uint64_t> g_straddle_hits{0};
static atomic<uint64_t> g_lock_timeouts{0};
static atomic<uint64_t> g_corruption_detected{0};
// Tracks, per absolute-byte-position-mod-4-slot, which "generation" (lap
// count) currently occupies it -- lets the reader compute the TRUE
// expected value for any slot it reads, not just check local uniformity.

static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    lock_guard<mutex> lk(g_ring_mutex);
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0;
    g_total_written = DECLARED_FILE_SIZE;
    g_ground_truth_written += n;
    return true;
  }
  lock_guard<mutex> lk(g_ring_mutex);
  uint32_t wp = g_write_pos;
  uint32_t end = wp + n;
  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    uint32_t wrapped_end = (end > DECLARED_FILE_SIZE) ? (end - DECLARED_FILE_SIZE) : end;
    bool unsafe;
    uint32_t lro = g_last_read_offset;
    if (end <= DECLARED_FILE_SIZE) unsafe = (wp <= lro && lro < end);
    else unsafe = (lro >= wp || lro < wrapped_end);
    if (unsafe) return false;
  }
  if (end <= DECLARED_FILE_SIZE) {
    memcpy(g_ring + wp, data, n);
  } else {
    uint32_t first_part = DECLARED_FILE_SIZE - wp;
    memcpy(g_ring + wp, data, first_part);
    memcpy(g_ring, data + first_part, end - DECLARED_FILE_SIZE);
  }
  g_write_pos = end % DECLARED_FILE_SIZE;
  if (g_total_written < DECLARED_FILE_SIZE) {
    g_total_written = min(g_total_written + n, DECLARED_FILE_SIZE);
  }
  g_ground_truth_written += n;
  return true;
}

static inline uint32_t disk_valid_bytes() {
  uint32_t tw = g_total_written;
  return (tw < DECLARED_FILE_SIZE) ? tw : DECLARED_FILE_SIZE;
}

// out_ground_truth_at_read: the EXACT g_ground_truth_written value
// observed while still holding the lock, i.e. atomically consistent with
// whatever bytes actually got copied -- capturing this after the lock is
// released (as an earlier pass of this test did) leaves a real window
// for the writer to advance further in between, making the "expected
// value" reference describe a LATER state than what was truly read.
// Test-harness-only concern (real firmware doesn't need this, it doesn't
// verify itself against a ground truth), but matters for this simulation
// not to manufacture its own false positives. Uses the uncapped test-only
// counter, not the real (now intentionally capped) g_total_written --
// see the comment on g_ground_truth_written's declaration.
static bool disk_read_data(uint32_t data_off, uint8_t *buffer, uint32_t n,
                            uint64_t *out_ground_truth_at_read) {
  memset(buffer, 0, n);
  unique_lock<mutex> lk(g_ring_mutex, defer_lock);
  if (!lk.try_lock()) { g_lock_timeouts++; return false; }
  uint32_t avail = disk_valid_bytes();
  uint32_t wp = g_write_pos;
  uint32_t unsafe_end = data_off + n + READ_MARGIN_BYTES;
  bool straddling;
  if (unsafe_end <= DECLARED_FILE_SIZE) straddling = (data_off <= wp && wp < unsafe_end);
  else straddling = (wp >= data_off || wp < unsafe_end - DECLARED_FILE_SIZE);
  if (!straddling) {
    if (data_off < avail) {
      uint32_t real_n = min(n, avail - data_off);
      memcpy(buffer, g_ring + data_off, real_n);
    }
    g_last_read_offset = (data_off + n) % DECLARED_FILE_SIZE;
    *out_ground_truth_at_read = g_ground_truth_written;
    return true;
  }
  g_straddle_hits++;
  return false;
}

// Writer embeds, at every 4-byte-aligned ring position it touches, the
// GLOBAL byte offset (its own absolute write-stream position, a strictly
// increasing uint32 truncation of total bytes written so far) as a
// little-endian uint32. This is the ground truth: at any real instant,
// ring position P should hold either 0 (never written) or some value V
// such that V mod DECLARED_FILE_SIZE == P and V <= current total_written
// (i.e. this content was legitimately written at some point, not
// fabricated) -- and specifically must NOT show two DIFFERENT V values
// within what should be one atomic 4-byte slot (that would be a real
// sub-slot tear) NOR should adjacent 4-byte slots within one read jump
// backward by more than one full ring lap's worth (that would indicate
// stale-lap data spliced next to fresh data).
static void writer_thread(double run_seconds) {
  const double bytes_per_sec = 16000.0;
  auto start = steady_clock::now();
  auto next_tick = start;
  vector<uint8_t> chunk(512);
  int chunk_sizes[] = {256, 320, 384, 448, 512};
  int csi = 0;
  // Thread-local tracking of what this (sole) writer has produced so far
  // -- avoids ever reading the shared g_ground_truth_written from this
  // thread (only disk_append writes it, under the lock; only the reader
  // thread reads it, also under the lock -- a clean single-writer/
  // single-reader pattern with no unsynchronized cross-thread access).
  uint64_t local_written = 0;

  while (duration<double>(steady_clock::now() - start).count() < run_seconds && !g_stop) {
    int n = chunk_sizes[csi % 5]; csi++;
    n -= n % 4;  // keep 4-byte aligned for clean slot semantics
    uint64_t base = local_written;
    for (int i = 0; i < n; i += 4) {
      uint32_t v = base + i;
      chunk[i] = v & 0xFF; chunk[i+1] = (v>>8)&0xFF; chunk[i+2] = (v>>16)&0xFF; chunk[i+3] = (v>>24)&0xFF;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 200; attempt++) {
      if (disk_append(chunk.data(), n, true)) { ok = true; break; }
      g_write_protected_rejections++;
      this_thread::sleep_for(milliseconds(5));
    }
    if (!ok) disk_append(chunk.data(), n, false);
    local_written += n;
    g_writes_done++;

    next_tick += duration_cast<steady_clock::duration>(duration<double>(n / bytes_per_sec));
    auto now = steady_clock::now();
    if (next_tick > now) this_thread::sleep_for(next_tick - now);
  }
}

static void reader_thread(double run_seconds) {
  auto start = steady_clock::now();
  uint32_t read_pos = 0;
  vector<uint8_t> buf(512);

  while (duration<double>(steady_clock::now() - start).count() < run_seconds && !g_stop) {
    for (int i = 0; i < 8 && !g_stop; i++) {
      uint64_t total_after = 0;
      bool served = disk_read_data(read_pos, buf.data(), 512, &total_after);
      g_reads_done++;
      if (served) {
        for (uint32_t off = 0; off + 4 <= 512; off += 4) {
          uint32_t slot_pos = (read_pos + off) % DECLARED_FILE_SIZE;
          uint32_t v = buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24);
          // Exact expected value (not a bounded search -- v1's fixed
          // lap<=2 search was itself a bug: it silently ran out of range
          // once the ring passed lap 2, flagging perfectly legitimate
          // current-lap data as "corruption"). Writes happen strictly in
          // order, exactly DECLARED_FILE_SIZE apart per lap, so the most
          // recent legitimate value at slot_pos is uniquely determined by
          // total_after: full_laps = total_after / DECLARED_FILE_SIZE;
          // if slot_pos is already "behind" the writer's current-lap
          // progress, this lap already wrote it -- else the PREVIOUS lap
          // did (or it's legitimately still unwritten, if there was no
          // previous lap).
          uint64_t full_laps = total_after / DECLARED_FILE_SIZE;
          uint64_t cur_progress = total_after % DECLARED_FILE_SIZE;
          bool found_valid = false;
          if (slot_pos < cur_progress) {
            uint64_t expected = slot_pos + full_laps * (uint64_t)DECLARED_FILE_SIZE;
            found_valid = ((uint32_t)expected == v);
          } else if (full_laps >= 1) {
            uint64_t expected = slot_pos + (full_laps - 1) * (uint64_t)DECLARED_FILE_SIZE;
            found_valid = ((uint32_t)expected == v);
          } else {
            found_valid = (v == 0);  // never written yet, legitimately zero
          }
          if (!found_valid) g_corruption_detected++;
        }
      }
      read_pos = (read_pos + 512) % DECLARED_FILE_SIZE;
    }
    this_thread::sleep_for(milliseconds(32));
  }
}

int main(int argc, char **argv) {
  double run_seconds = (argc > 1) ? atof(argv[1]) : 45.0;
  g_ring = new uint8_t[DECLARED_FILE_SIZE]();

  printf("Running FIXED real-time concurrent simulation for %.0f seconds "
         "(ring=%u bytes, ~%.1fs of audio)...\n",
         run_seconds, DECLARED_FILE_SIZE, DECLARED_FILE_SIZE / 16000.0);

  thread w(writer_thread, run_seconds);
  thread r(reader_thread, run_seconds);
  w.join();
  r.join();

  printf("\n=== results ===\n");
  printf("writes_done=%lu write_protected_rejections=%lu\n",
         (unsigned long)g_writes_done, (unsigned long)g_write_protected_rejections);
  printf("reads_done=%lu straddle_hits=%lu (%.2f%%) lock_timeouts=%lu (%.4f%%)\n",
         (unsigned long)g_reads_done, (unsigned long)g_straddle_hits,
         100.0 * g_straddle_hits / max<uint64_t>(1, g_reads_done),
         (unsigned long)g_lock_timeouts,
         100.0 * g_lock_timeouts / max<uint64_t>(1, g_reads_done));
  printf("CORRUPTION DETECTED: %lu%s\n", (unsigned long)g_corruption_detected,
         g_corruption_detected == 0 ? "  <-- GOOD, zero real corruption" : "  <-- REAL BUG, INVESTIGATE");
  printf("final write_pos=%u total_written(capped, real firmware value)=%u "
         "ground_truth_written(test-only, uncapped)=%lu laps=%.2f\n",
         g_write_pos.load(), g_total_written.load(),
         (unsigned long)g_ground_truth_written,
         (double)g_ground_truth_written / DECLARED_FILE_SIZE);
  return g_corruption_detected > 0 ? 1 : 0;
}
