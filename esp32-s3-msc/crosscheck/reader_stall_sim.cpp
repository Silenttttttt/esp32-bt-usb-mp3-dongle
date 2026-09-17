// Adversarial variant of realtime_concurrency_sim.cpp: the reader
// deliberately stalls for a real multi-second window (simulating a
// crashed/stuck USB host or car radio), forcing the writer to actually
// hit unread_protect's backpressure path hard, then the reader resumes
// and catches up. Verifies: (1) the writer never corrupts data even under
// sustained backpressure contention, (2) the forced-write fallback (after
// MAX_WRITE_RETRIES exhausted) doesn't itself corrupt anything, (3) the
// reader can cleanly resume and get consistent data after the stall.
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

static const uint32_t SECTORS_PER_CLUSTER = 8;
static const uint32_t SECTOR_SIZE = 512;
static const uint32_t DATA_CLUSTERS = 117;
static const uint32_t CLUSTER_SIZE = SECTORS_PER_CLUSTER * SECTOR_SIZE;
static const uint32_t DECLARED_FILE_SIZE = DATA_CLUSTERS * CLUSTER_SIZE;
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;

static uint8_t *g_ring;
static atomic<uint32_t> g_write_pos{0};
static atomic<uint32_t> g_total_written{0};
static atomic<uint32_t> g_last_read_offset{0};
static mutex g_ring_mutex;
// TEST-ONLY ground truth, uncapped -- see realtime_concurrency_sim.cpp's
// identical comment. g_total_written is now intentionally capped
// (2026-09-17 overflow fix) so it can't double as this test's verification
// reference anymore.
static uint64_t g_ground_truth_written = 0;

static atomic<bool> g_stop{false};
static atomic<bool> g_reader_stalled{true};  // starts stalled, released after N seconds
static atomic<uint64_t> g_writes_done{0};
static atomic<uint64_t> g_write_protected_rejections{0};
static atomic<uint64_t> g_forced_writes{0};
static atomic<uint64_t> g_reads_done{0};
static atomic<uint64_t> g_straddle_hits{0};
static atomic<uint64_t> g_corruption_detected{0};

static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    lock_guard<mutex> lk(g_ring_mutex);
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0; g_total_written = DECLARED_FILE_SIZE;
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

static bool disk_read_data(uint32_t data_off, uint8_t *buffer, uint32_t n, uint64_t *out_tw) {
  memset(buffer, 0, n);
  lock_guard<mutex> lk(g_ring_mutex);
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
    *out_tw = g_ground_truth_written;
    return true;
  }
  g_straddle_hits++;
  return false;
}

// Same real firmware retry pattern: MAX_WRITE_RETRIES=200 @ 5ms = ~1s,
// then forced fallback -- exactly matching link_task()'s real behavior,
// including under real, sustained backpressure this time (not the rare
// 0-2 hits seen when the reader kept pace).
static void writer_thread(double run_seconds) {
  const double bytes_per_sec = 16000.0;
  auto start = steady_clock::now();
  auto next_tick = start;
  vector<uint8_t> chunk(512);
  int chunk_sizes[] = {256, 320, 384, 448, 512};
  int csi = 0;
  uint64_t local_written = 0;  // avoids reading the shared value from this thread; see realtime_concurrency_sim.cpp

  while (duration<double>(steady_clock::now() - start).count() < run_seconds && !g_stop) {
    int n = chunk_sizes[csi % 5]; csi++;
    n -= n % 4;
    uint64_t base = local_written;
    for (int i = 0; i < n; i += 4) {
      uint32_t v = base + i;
      chunk[i] = v&0xFF; chunk[i+1]=(v>>8)&0xFF; chunk[i+2]=(v>>16)&0xFF; chunk[i+3]=(v>>24)&0xFF;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 200; attempt++) {
      if (disk_append(chunk.data(), n, true)) { ok = true; break; }
      g_write_protected_rejections++;
      this_thread::sleep_for(milliseconds(5));
    }
    if (!ok) { disk_append(chunk.data(), n, false); g_forced_writes++; }
    local_written += n;
    g_writes_done++;

    next_tick += duration_cast<steady_clock::duration>(duration<double>(n / bytes_per_sec));
    auto now = steady_clock::now();
    if (next_tick > now) this_thread::sleep_for(next_tick - now);
  }
}

static void reader_thread(double run_seconds, double stall_seconds) {
  auto start = steady_clock::now();
  uint32_t read_pos = 0;
  vector<uint8_t> buf(512);

  // Genuine stall: the reader does NOTHING for stall_seconds, exactly
  // simulating a stuck/crashed host -- not even polling, real dead time
  // from the writer's perspective, which is the scenario that actually
  // exercises unread_protect for real.
  this_thread::sleep_for(duration<double>(stall_seconds));
  printf("[reader] stall over at t=%.1fs, resuming reads\n",
         duration<double>(steady_clock::now() - start).count());

  while (duration<double>(steady_clock::now() - start).count() < run_seconds && !g_stop) {
    for (int i = 0; i < 8 && !g_stop; i++) {
      uint64_t total_after = 0;
      bool served = disk_read_data(read_pos, buf.data(), 512, &total_after);
      g_reads_done++;
      if (served) {
        for (uint32_t off = 0; off + 4 <= 512; off += 4) {
          uint32_t slot_pos = (read_pos + off) % DECLARED_FILE_SIZE;
          uint32_t v = buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24);
          uint64_t full_laps = total_after / DECLARED_FILE_SIZE;
          uint64_t cur_progress = total_after % DECLARED_FILE_SIZE;
          bool found_valid;
          if (slot_pos < cur_progress) {
            found_valid = ((uint32_t)(slot_pos + full_laps * (uint64_t)DECLARED_FILE_SIZE) == v);
          } else if (full_laps >= 1) {
            found_valid = ((uint32_t)(slot_pos + (full_laps - 1) * (uint64_t)DECLARED_FILE_SIZE) == v);
          } else {
            found_valid = (v == 0);
          }
          if (!found_valid) g_corruption_detected++;
        }
      }
      read_pos = (read_pos + 512) % DECLARED_FILE_SIZE;
    }
    this_thread::sleep_for(milliseconds(2));  // fast catch-up reading once resumed
  }
}

int main(int argc, char **argv) {
  double run_seconds = (argc > 1) ? atof(argv[1]) : 60.0;
  double stall_seconds = (argc > 2) ? atof(argv[2]) : 40.0;  // > ring duration (30s) to force real wraparound during the stall
  g_ring = new uint8_t[DECLARED_FILE_SIZE]();

  printf("Adversarial stall test: reader stalls %.0fs (ring holds only %.1fs of "
         "audio -- writer WILL need to wrap during the stall), then runs for %.0fs total\n",
         stall_seconds, DECLARED_FILE_SIZE / 16000.0, run_seconds);

  thread w(writer_thread, run_seconds);
  thread r(reader_thread, run_seconds, stall_seconds);
  w.join();
  r.join();

  printf("\n=== results ===\n");
  printf("writes_done=%lu write_protected_rejections=%lu forced_writes=%lu\n",
         (unsigned long)g_writes_done, (unsigned long)g_write_protected_rejections,
         (unsigned long)g_forced_writes);
  printf("reads_done=%lu straddle_hits=%lu\n",
         (unsigned long)g_reads_done, (unsigned long)g_straddle_hits);
  printf("CORRUPTION DETECTED: %lu%s\n", (unsigned long)g_corruption_detected,
         g_corruption_detected == 0 ? "  <-- GOOD, zero real corruption even under stall" : "  <-- REAL BUG");
  return g_corruption_detected > 0 ? 1 : 0;
}
