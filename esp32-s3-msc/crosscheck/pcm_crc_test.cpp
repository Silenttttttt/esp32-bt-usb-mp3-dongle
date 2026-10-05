// Host test for the 'Q' PCM frames (CRC-16) and pcm_conceal.h.
//
//   g++ -O2 -std=c++17 -Wall -o pcm_crc_test pcm_crc_test.cpp && ./pcm_crc_test
//
// Builds a stream of 'Q' frames the way the classic's send_pcm_crc() does,
// corrupts it with bursts of garbage (a wobbling plug on the wire), and parses
// it with a copy of link_task's logic (find_sync, header check, CRC, conceal).
// Checks: the CRC matches the published check value; a clean stream comes out
// bit-exact; every output frame is either an original frame or a replacement
// (no corrupted audio gets through); the output has no step bigger than the
// signal's own; and the same corruption through the old unchecked 'P' path
// does produce big steps (so the test would notice the protection missing).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../common/link_protocol.h"
#include "../../common/pcm_conceal.h"

static const uint8_t MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;
static const uint32_t FRAME = 2048;  // one mono slot, 1024 samples

static int fails = 0;
static void check(bool ok, const char *what) {
  printf("  %-62s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) fails++;
}

static std::vector<uint8_t> make_pcm(int frames) {
  std::vector<uint8_t> pcm(frames * FRAME);
  for (size_t i = 0; i < pcm.size() / 2; i++) {
    // two tones, ~-6 dBFS: a realistic max step between samples
    double t = i / 44100.0;
    int16_t v = (int16_t)(12000 * sin(2 * M_PI * 440 * t) + 4000 * sin(2 * M_PI * 3100 * t));
    pcm[2 * i] = v & 0xFF;
    pcm[2 * i + 1] = (uint16_t)v >> 8;
  }
  return pcm;
}

static void frame_into(std::vector<uint8_t> &wire, char type, const uint8_t *p, uint32_t len) {
  uint8_t h[6] = {MAGIC, (uint8_t)type, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
  wire.insert(wire.end(), h, h + 6);
  wire.insert(wire.end(), p, p + len);
  if (type == LINK_FRAME_PCM_CRC) {
    uint16_t crc = link_crc16(h + 1, 5);
    crc = link_crc16(p, len, crc);
    wire.push_back(crc >> 8);
    wire.push_back(crc & 0xFF);
  }
}

// Bursts of random bytes at fixed positions (same seed for both paths).
static void corrupt(std::vector<uint8_t> &wire, int bursts, unsigned seed) {
  srand(seed);
  for (int b = 0; b < bursts; b++) {
    size_t at = 1000 + (size_t)rand() % (wire.size() - 2000);
    size_t n = 1 + rand() % 300;
    for (size_t i = 0; i < n && at + i < wire.size(); i++) wire[at + i] = (uint8_t)rand();
  }
}

struct Parsed {
  std::vector<uint8_t> pcm;
  std::vector<int> kind;  // per output frame: 1 = received, 0 = replacement
  uint32_t crc_bad = 0, bad_headers = 0;
};

// Copy of link_task (ENCODE_ON_S3): "time since last PCM < 100 ms" becomes
// "the previous frame was PCM".
static Parsed parse(const std::vector<uint8_t> &wire) {
  Parsed out;
  PcmConceal conceal;
  bool in_loss = false, pcm_recent = false;
  size_t pos = 0;
  uint8_t payload[MAX_FRAME_LEN];
  auto emit = [&](const uint8_t *p, uint32_t n, int k) {
    out.pcm.insert(out.pcm.end(), p, p + n);
    out.kind.push_back(k);
  };
  auto loss = [&]() {
    if (in_loss) return;
    in_loss = true;
    uint8_t fill[MAX_FRAME_LEN];
    uint32_t n = conceal.last_len();
    conceal.lost(fill, n);
    emit(fill, n, 0);
  };
  while (true) {
    while (pos < wire.size() && wire[pos] != MAGIC) pos++;
    if (pos + 6 > wire.size()) break;
    pos++;
    const uint8_t *header = &wire[pos];
    pos += 5;
    char type = (char)header[0];
    uint32_t len = ((uint32_t)header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
    bool known = type == LINK_FRAME_PCM || type == LINK_FRAME_PCM_CRC || type == LINK_FRAME_CONTROL;
    if (!known || len > MAX_FRAME_LEN) {
      out.bad_headers++;
      if (pcm_recent) loss();
      continue;
    }
    uint32_t need = len + (type == LINK_FRAME_PCM_CRC ? 2 : 0);
    if (pos + need > wire.size()) break;
    memcpy(payload, &wire[pos], len);
    pos += len;
    if (type == LINK_FRAME_PCM_CRC) {
      uint16_t crc = link_crc16(header, 5);
      crc = link_crc16(payload, len, crc);
      uint16_t got = (uint16_t)((wire[pos] << 8) | wire[pos + 1]);
      pos += 2;
      if (crc != got) { out.crc_bad++; loss(); continue; }
    }
    if (type == LINK_FRAME_PCM || type == LINK_FRAME_PCM_CRC) {
      pcm_recent = true;
      in_loss = false;
      if (type == LINK_FRAME_PCM_CRC) conceal.good(payload, len);  // 'P' = the old path, untouched
      emit(payload, len, 1);
    }
  }
  return out;
}

static int16_t sample(const std::vector<uint8_t> &p, size_t i) { return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8)); }

static int max_step(const std::vector<uint8_t> &p) {
  int m = 0;
  for (size_t i = 1; i < p.size() / 2; i++) m = std::max(m, abs(sample(p, i) - sample(p, i - 1)));
  return m;
}

int main() {
  printf("CRC\n");
  check(link_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value 0x29B1");

  const int FRAMES = 2000;  // ~46 s
  std::vector<uint8_t> src = make_pcm(FRAMES);
  int src_step = max_step(src);

  std::vector<uint8_t> clean;
  for (int f = 0; f < FRAMES; f++) frame_into(clean, LINK_FRAME_PCM_CRC, &src[f * FRAME], FRAME);
  printf("Clean 'Q' stream\n");
  Parsed c = parse(clean);
  check(c.pcm == src && c.crc_bad == 0 && c.bad_headers == 0, "bit-exact, no CRC failures");

  printf("'Q' stream with 40 garbage bursts (1-300 bytes each)\n");
  std::vector<uint8_t> q = clean;
  corrupt(q, 40, 7);
  Parsed r = parse(q);
  printf("  crc_bad=%u bad_headers=%u frames out=%zu (sent %d)\n", r.crc_bad, r.bad_headers, r.kind.size(), FRAMES);
  check(r.crc_bad > 0, "corruption detected");
  // Every received frame must equal some original frame, in order.
  size_t next = 0;
  bool all_original = true;
  for (size_t k = 0, off = 0; k < r.kind.size(); off += FRAME, k++) {
    if (!r.kind[k]) continue;
    bool found = false;
    for (size_t f = next; f < (size_t)FRAMES; f++) {
      // a received frame may have been faded in; compare its tail (after the fade)
      if (!memcmp(&r.pcm[off + 2 * PcmConceal::FADE_SAMPLES], &src[f * FRAME + 2 * PcmConceal::FADE_SAMPLES],
                  FRAME - 2 * PcmConceal::FADE_SAMPLES)) {
        next = f + 1; found = true; break;
      }
    }
    if (!found) all_original = false;
  }
  check(all_original, "every received frame is an original frame (no corrupt audio)");
  int q_step = max_step(r.pcm);
  printf("  max step: signal %d, output %d\n", src_step, q_step);
  check(q_step <= src_step + 400, "no step bigger than the signal's own (+400)");
  int lost = 0;
  for (int k : r.kind) lost += k == 0;
  check(lost <= 40 && lost > 0, "at most one replacement per burst");

  printf("Same corruption through the old unchecked 'P' path\n");
  std::vector<uint8_t> p;
  for (int f = 0; f < FRAMES; f++) frame_into(p, LINK_FRAME_PCM, &src[f * FRAME], FRAME);
  corrupt(p, 40, 7);
  Parsed old = parse(p);
  int p_step = max_step(old.pcm);
  printf("  max step: %d\n", p_step);
  check(p_step > 4 * src_step, "spikes get through without the CRC (the test can see them)");

  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails ? 1 : 0;
}
