// Cross-checks the RING logic (append with unread_protect, read with
// avoid_straddle) from esp32-s3-msc.ino against fat12_disk.py, for a
// scripted sequence of operations. Host-only: FreeRTOS mutex calls
// stripped (single-threaded test, the mutex isn't what's being verified
// here -- the position/byte arithmetic is).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
using namespace std;

static const uint32_t DECLARED_FILE_SIZE = 117 * 8 * 512;  // 479232
static const uint32_t CLUSTER_SIZE = 8 * 512;               // 4096
static const uint32_t READ_MARGIN_BYTES = 2 * CLUSTER_SIZE;

static uint8_t g_ring[DECLARED_FILE_SIZE];
static uint32_t g_write_pos = 0;
static uint32_t g_total_written = 0;
static uint32_t g_last_read_offset = 0;

static bool disk_append(const uint8_t *data, uint32_t n, bool unread_protect) {
  if (n >= DECLARED_FILE_SIZE) {
    if (unread_protect) return false;
    memcpy(g_ring, data + (n - DECLARED_FILE_SIZE), DECLARED_FILE_SIZE);
    g_write_pos = 0;
    g_total_written = DECLARED_FILE_SIZE;
    return true;
  }
  uint32_t wp = g_write_pos;
  uint32_t end = wp + n;
  if (unread_protect && g_total_written >= DECLARED_FILE_SIZE) {
    uint32_t wrapped_end = (end > DECLARED_FILE_SIZE) ? (end - DECLARED_FILE_SIZE) : end;
    bool unsafe;
    if (end <= DECLARED_FILE_SIZE) {
      unsafe = (wp <= g_last_read_offset && g_last_read_offset < end);
    } else {
      unsafe = (g_last_read_offset >= wp || g_last_read_offset < wrapped_end);
    }
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
  // Overflow fix (2026-09-17, real bug caught by reasoning + a direct
  // test, see esp32-s3-msc.ino's own comment on disk_append): must not
  // even attempt the += once already capped, since that addition can
  // itself overflow a uint32_t on a session long enough to matter.
  // Python's total_written is arbitrary-precision and has no analogous
  // issue, so this is an intentional, necessary divergence from the
  // Python reference -- the final total_written value WILL differ from
  // fat12_disk.py's (capped here, uncapped there); every other tracked
  // value (write_pos, last_read_offset, straddle/rejection counts) is
  // unaffected and still expected to match exactly.
  if (g_total_written < DECLARED_FILE_SIZE) {
    g_total_written = min(g_total_written + n, DECLARED_FILE_SIZE);
  }
  return true;
}

static inline uint32_t disk_valid_bytes() {
  return min(g_total_written, DECLARED_FILE_SIZE);
}

// Simplified data-region-only read (no boot/fat/root, that part's already
// proven byte-identical) -- returns true if it served (possibly
// zero-filled) content, matching disk_read_at's data-region branch with
// took_lock always true (single-threaded test).
static bool disk_read_data(uint32_t data_off, uint8_t *buffer, uint32_t n) {
  memset(buffer, 0, n);
  uint32_t avail = disk_valid_bytes();
  uint32_t wp = g_write_pos;
  uint32_t unsafe_end = data_off + n + READ_MARGIN_BYTES;
  bool straddling;
  if (unsafe_end <= DECLARED_FILE_SIZE) {
    straddling = (data_off <= wp && wp < unsafe_end);
  } else {
    straddling = (wp >= data_off || wp < unsafe_end - DECLARED_FILE_SIZE);
  }
  if (!straddling) {
    if (data_off < avail) {
      uint32_t real_n = min(n, avail - data_off);
      memcpy(buffer, g_ring + data_off, real_n);
    }
    g_last_read_offset = (data_off + n) % DECLARED_FILE_SIZE;
    return true;
  }
  return false;  // straddling -- caller sees all-zero buffer, no offset update
}

int main() {
  // Scripted sequence: small realistic-sized writes (matching real audio
  // frame sizes, ~256-512 bytes), interleaved with reads at varying
  // positions, deliberately including: normal steady-state, a read right
  // at the write edge (should straddle), a read of not-yet-written data,
  // and enough writes to wrap the ring at least twice.
  uint8_t chunk[512];
  for (int i = 0; i < 512; i++) chunk[i] = (uint8_t)(i & 0xFF);

  uint32_t writes_done = 0;
  uint32_t protected_rejections = 0;
  uint32_t reads_done = 0;
  uint32_t straddle_hits = 0;

  // Phase 1: fill past one full ring lap plus some, writer only (no
  // reader yet) -- exercises wraparound in disk_append.
  uint32_t target_bytes = DECLARED_FILE_SIZE * 2 + 10000;
  while (writes_done * 512 < target_bytes) {
    // vary chunk content per write so a wrap-around bug would show up as
    // wrong bytes at a given ring offset, not just "some bytes present"
    chunk[0] = (uint8_t)(writes_done & 0xFF);
    disk_append(chunk, sizeof(chunk), false);
    writes_done++;
  }
  printf("phase1: writes_done=%u write_pos=%u total_written=%u\n",
         writes_done, g_write_pos, g_total_written);

  // Phase 2: interleave reads (with unread_protect writes), simulating a
  // reader that mostly keeps pace but occasionally reads right at the
  // live edge to trigger straddle detection.
  uint8_t readbuf[512];
  for (int step = 0; step < 2000; step++) {
    // reader reads at a position deliberately close to write_pos every
    // 50th step, to force some straddle hits
    uint32_t read_off;
    if (step % 50 == 0) {
      read_off = g_write_pos;  // exactly at the live edge -- must straddle
    } else {
      read_off = (g_write_pos + DECLARED_FILE_SIZE / 2) % DECLARED_FILE_SIZE;  // far side, safe
    }
    bool served = disk_read_data(read_off, readbuf, sizeof(readbuf));
    reads_done++;
    if (!served) straddle_hits++;

    chunk[0] = (uint8_t)((writes_done + step) & 0xFF);
    if (!disk_append(chunk, sizeof(chunk), true)) {
      protected_rejections++;
      disk_append(chunk, sizeof(chunk), false);  // forced fallback, matches firmware/simulator
    }
    writes_done++;
  }

  printf("phase2: writes_done=%u reads_done=%u straddle_hits=%u "
         "protected_rejections=%u final write_pos=%u total_written=%u last_read_offset=%u\n",
         writes_done, reads_done, straddle_hits, protected_rejections,
         g_write_pos, g_total_written, g_last_read_offset);

  return 0;
}
