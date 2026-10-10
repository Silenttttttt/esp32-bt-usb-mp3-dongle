// Host test for the LINK_Q PCM link: 'R' frames (CRC-32, fixed size, sequence
// number), one-bit correction and rescan (common/link_rx_parser.h), loss
// concealment (pcm_link_rx.h, pcm_conceal.h) -- the same code the S3 runs --
// plus the LINK_DIAG helpers (link_diag.h).
//
//   g++ -O2 -std=c++17 -Wall -o pcm_crc_test pcm_crc_test.cpp && ./pcm_crc_test
//
// Builds the byte stream the classic's send_pcm_crc() produces and damages it
// the ways the bench showed (2026-10-10): single bits turned 0 -> 1 (a contact
// break shorter than a bit, RX pulled up), bytes that vanish (a longer break),
// and garbage bursts. Checks, for each: the output is exactly as long as the
// input (no drift), no damaged sample reaches the encoder, no step bigger than
// the signal's own, and how many frames were fixed vs filled.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "../../common/link_rx_parser.h"
#include "../../common/pcm_link_rx.h"
#include "../../common/link_diag.h"

static const uint32_t MAX_FRAME_LEN = 4096;
static const size_t WIRE_FRAME = 6 + LINK_R_PAYLOAD + 4;  // 523 bytes

static int fails = 0;
static void check(bool ok, const char *what) {
  printf("  %-72s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) fails++;
}

static int16_t sample(const std::vector<uint8_t> &p, size_t i) { return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8)); }

// kind 0: two tones. kind 1: a non-harmonic chord with tremolo and ~10% noise.
static std::vector<uint8_t> make_pcm(size_t samples, int kind) {
  std::vector<uint8_t> pcm(samples * 2);
  srand(99);
  for (size_t i = 0; i < samples; i++) {
    double t = i / 44100.0;
    double x = kind == 0 ? 12000 * sin(2 * M_PI * 440 * t) + 4000 * sin(2 * M_PI * 3100 * t)
                         : (1 + 0.3 * sin(2 * M_PI * 3 * t)) *
                               (5000 * sin(2 * M_PI * 220 * t) + 4000 * sin(2 * M_PI * 279.3 * t) +
                                3500 * sin(2 * M_PI * 349.2 * t) + 1500 * sin(2 * M_PI * 2700 * t)) +
                           1500.0 * (rand() / (double)RAND_MAX * 2 - 1);
    int16_t v = (int16_t)x;
    pcm[2 * i] = v & 0xFF;
    pcm[2 * i + 1] = (uint16_t)v >> 8;
  }
  return pcm;
}

// The classic: fixed 512 B frames (remainder carried), seq, CRC-32.
struct RSender {
  uint8_t seq = 0, pend[LINK_Q_MAX_PCM];
  uint32_t pend_n = 0;
  void frame(std::vector<uint8_t> &w, const uint8_t *pcm) {
    uint32_t plen = LINK_R_PAYLOAD;
    uint8_t h[7] = {LINK_MAGIC, (uint8_t)LINK_FRAME_PCM_CRC, (uint8_t)(plen >> 24), (uint8_t)(plen >> 16),
                    (uint8_t)(plen >> 8), (uint8_t)plen, seq++};
    uint32_t crc = link_crc32(pcm, LINK_Q_MAX_PCM, link_crc32_update(h + 1, 6));
    w.insert(w.end(), h, h + 7);
    w.insert(w.end(), pcm, pcm + LINK_Q_MAX_PCM);
    for (int s = 24; s >= 0; s -= 8) w.push_back((uint8_t)(crc >> s));
  }
  void send(std::vector<uint8_t> &w, const uint8_t *pcm, uint32_t len) {
    while (len) {
      uint32_t take = std::min(LINK_Q_MAX_PCM - pend_n, len);
      memcpy(pend + pend_n, pcm, take);
      pend_n += take;
      if (pend_n == LINK_Q_MAX_PCM) { frame(w, pend); pend_n = 0; }
      pcm += take;
      len -= take;
    }
  }
};
// Uneven chunk sizes, like phone packets split across slots.
static std::vector<uint8_t> wire_for(const std::vector<uint8_t> &src) {
  std::vector<uint8_t> w;
  RSender s;
  for (size_t off = 0, k = 0; off < src.size(); k++) {
    uint32_t n = (uint32_t)std::min<size_t>(k % 2 ? 496 : 1552, src.size() - off);
    s.send(w, &src[off], n);
    off += n;
  }
  return w;
}

struct VecSrc {
  const std::vector<uint8_t> &w;
  size_t pos = 0;
  void read(uint8_t *b, uint32_t n) {
    if (pos + n > w.size()) throw std::runtime_error("end");
    memcpy(b, &w[pos], n);
    pos += n;
  }
};

struct Out {
  std::vector<uint8_t> pcm;
  std::vector<bool> filled;  // per output sample: concealment, not received
  PcmLinkRx rx;
  uint32_t corrected = 0, uncorrectable = 0, bad_headers = 0;
};

// link_task with LINK_Q. Time from byte position: 5 us per byte at 2 Mbaud.
static void parse(const std::vector<uint8_t> &wire, Out &out, bool accept_raw = false) {
  VecSrc src{wire};
  LinkParser<VecSrc> p(src, accept_raw);
  static uint8_t payload[MAX_FRAME_LEN];
  try {
    while (true) {
      LinkFrame f = p.next(payload, MAX_FRAME_LEN);
      if (f.bad_header) continue;
      if (f.crc_bad) { out.rx.on_crc_bad(); continue; }
      uint32_t now_ms = (uint32_t)(src.pos * 5 / 1000);
      if (f.type == LINK_FRAME_PCM_CRC) {
        bool filling = true;
        out.rx.on_good(payload[0], payload + 1, f.len - 1, now_ms, [&](const uint8_t *q, uint32_t n) {
          if (q == payload + 1) filling = false;
          out.pcm.insert(out.pcm.end(), q, q + n);
          for (uint32_t i = 0; i < n / 2; i++) out.filled.push_back(filling);
        });
      } else if (f.type == LINK_FRAME_PCM) {
        out.pcm.insert(out.pcm.end(), payload, payload + f.len);
        for (uint32_t i = 0; i < f.len / 2; i++) out.filled.push_back(false);
      }
    }
  } catch (const std::runtime_error &) {
  }
  out.corrected = p.corrected_data + p.corrected_trailer;
  out.uncorrectable = p.uncorrectable;
  out.bad_headers = p.bad_headers;
}

static int max_step(const std::vector<uint8_t> &p) {
  int m = 0;
  for (size_t i = 1; i < p.size() / 2; i++) m = std::max(m, abs(sample(p, i) - sample(p, i - 1)));
  return m;
}

// Damage models (never in the first 2 frames).
static size_t pos_for(const std::vector<uint8_t> &w) { return 2 * WIRE_FRAME + (size_t)rand() % (w.size() - 3 * WIRE_FRAME); }
// Returns {frames that one fix can repair, frames that can't be repaired}:
// a frame hit twice can't be, nor one hit in its magic or type byte (it can't
// be found / isn't recognized as 'R').
static std::pair<uint32_t, uint32_t> flips_0to1(std::vector<uint8_t> &w, int n, unsigned seed) {
  srand(seed);
  std::vector<int> hits(w.size() / WIRE_FRAME + 1, 0), header_hit(w.size() / WIRE_FRAME + 1, 0);
  for (int i = 0; i < n; i++) {
    size_t at = pos_for(w);
    uint8_t m = (uint8_t)(1 << (rand() % 8));
    if (w[at] & m) { i--; continue; }  // only count real flips
    w[at] |= m;
    hits[at / WIRE_FRAME]++;
    if (at % WIRE_FRAME < 2) header_hit[at / WIRE_FRAME] = 1;
  }
  uint32_t fixable = 0, unfixable = 0;
  for (size_t f = 0; f < hits.size(); f++) {
    if (!hits[f]) continue;
    if (hits[f] == 1 && !header_hit[f]) fixable++; else unfixable++;
  }
  return {fixable, unfixable};
}
static void delete_bytes(std::vector<uint8_t> &w, int n, unsigned seed) {
  srand(seed);
  for (int i = 0; i < n; i++) w.erase(w.begin() + pos_for(w));
}
static void bursts(std::vector<uint8_t> &w, int n, unsigned seed) {
  srand(seed);
  for (int b = 0; b < n; b++) {
    size_t at = pos_for(w), len = 1 + rand() % 300;
    for (size_t i = 0; i < len && at + i < w.size(); i++) w[at + i] = (uint8_t)rand();
  }
}

// Every received (non-filled) sample must equal the source, except inside the
// cross-fade right after a fill.
static bool received_samples_original(const Out &o, const std::vector<uint8_t> &src) {
  if (o.pcm.size() != src.size()) { printf("  length %zu != %zu\n", o.pcm.size(), src.size()); return false; }
  for (size_t i = 0; i < src.size() / 2; i++) {
    if (o.filled[i] || sample(o.pcm, i) == sample(src, i)) continue;
    bool in_xfade = false;
    for (size_t k = 1; k <= PcmConceal::XFADE && k <= i; k++) if (o.filled[i - k]) in_xfade = true;
    if (!in_xfade) { printf("  damaged sample at %zu\n", i); return false; }
  }
  return true;
}

int main() {
  printf("CRC-32\n");
  check(link_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926, "check value 0xCBF43926");
  {
    uint32_t c = link_crc32_update((const uint8_t *)"1234", 4);
    check(link_crc32((const uint8_t *)"56789", 5, c) == 0xCBF43926, "same value computed in pieces");
  }

  const size_t SAMPLES = 1024000;  // ~23 s
  for (int kind = 0; kind < 2; kind++) {
    std::vector<uint8_t> src = make_pcm(SAMPLES, kind);
    int src_step = max_step(src);
    std::vector<uint8_t> clean = wire_for(src);
    printf("== signal: %s, %zu frames ==\n", kind ? "chord + noise" : "two tones", clean.size() / WIRE_FRAME);
    {
      Out o;
      parse(clean, o);
      check(o.pcm == src && o.rx.crc_bad == 0 && o.corrected == 0, "clean: bit-exact, nothing fixed or lost");
    }
    struct Case { const char *name; int kind; int n; };
    const Case cases[] = {{"200 single bits 0->1", 0, 200}, {"60 vanished bytes", 1, 60}, {"40 garbage bursts (1-300 B)", 2, 40}};
    for (const Case &c : cases) {
      std::vector<uint8_t> w = clean;
      std::pair<uint32_t, uint32_t> expect{0, 0};
      if (c.kind == 0) expect = flips_0to1(w, c.n, 7);
      if (c.kind == 1) delete_bytes(w, c.n, 7);
      if (c.kind == 2) bursts(w, c.n, 7);
      Out o;
      parse(w, o);
      uint32_t lost = o.rx.lost_frames + o.rx.silence_frames;
      printf("  %-28s fixed=%3u lost=%3u frames (%.2f%% of audio)\n", c.name, o.corrected, lost,
             100.0 * lost * 256 / SAMPLES);
      char what[160];
      snprintf(what, sizeof what, "%s: exact length, no damaged sample, no big step", c.name);
      check(received_samples_original(o, src) && max_step(o.pcm) <= src_step + 1100, what);
      if (c.kind == 0) {
        printf("    expected: %u frames with one fixable flip, %u hit twice or in magic/type\n", expect.first, expect.second);
        check(o.corrected == expect.first && lost == expect.second, "  every single-flip frame fixed; only doubly hit ones lost");
      }
      if (c.kind == 1) check(lost <= (uint32_t)c.n, "  each vanished byte costs at most one frame (rescan)");
    }
  }

  printf("Long gaps\n");
  {
    std::vector<uint8_t> src = make_pcm(200 * 256, 0), wire = wire_for(src);
    wire.erase(wire.begin() + 100 * WIRE_FRAME, wire.begin() + 140 * WIRE_FRAME);
    wire.insert(wire.begin() + 100 * WIRE_FRAME, 232 * 200, (uint8_t)0x00);  // 232 ms of idle line
    Out o;
    parse(wire, o);
    check(o.rx.silence_frames == 40 && o.rx.outages == 0 && o.pcm.size() == src.size(),
          "40 frames gone for 232 ms: exactly 232 ms filled (cushion keeps its size)");
  }
  {
    std::vector<uint8_t> src = make_pcm(100 * 256, 0), wire;
    RSender a1, a2;
    a1.send(wire, src.data(), (uint32_t)src.size() / 2);
    wire.insert(wire.end(), 3 * 200000, (uint8_t)0x00);  // 3 s of nothing
    a2.seq = 7;                                           // a rebooted classic starts a new sequence
    a2.send(wire, src.data() + src.size() / 2, (uint32_t)src.size() / 2);
    Out o;
    parse(wire, o);
    check(o.rx.outages == 1 && o.rx.silence_frames == 0 && o.rx.lost_frames == 0,
          "a 3 s stop with a new sequence is an outage, not filled");
  }

  printf("Old unchecked 'P' path with the same kind of bursts (the car build)\n");
  {
    std::vector<uint8_t> src = make_pcm(SAMPLES / 4, 0), w;
    for (size_t off = 0; off < src.size(); off += 2048) {
      uint32_t n = (uint32_t)std::min<size_t>(2048, src.size() - off);
      uint8_t h[6] = {LINK_MAGIC, (uint8_t)LINK_FRAME_PCM, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
      w.insert(w.end(), h, h + 6);
      w.insert(w.end(), &src[off], &src[off] + n);
    }
    bursts(w, 10, 5);
    Out o;
    parse(w, o, true);
    PcmSpikeMeter m;
    m.feed(o.pcm.data(), o.pcm.size());
    printf("  max step %d, spikes %u\n", max_step(o.pcm), m.spikes);
    check(m.spikes > 10, "spikes get through without a check (the test can see them)");
  }

  printf("LINK_DIAG helpers\n");
  {
    std::vector<uint8_t> clean_pcm = make_pcm(100000, 1);
    PcmSpikeMeter a;
    a.feed(clean_pcm.data(), clean_pcm.size());
    check(a.spikes == 0, "spike meter silent on clean audio");
    srand(3);
    uint8_t m[518], e[518];
    int one = 0, two = 0;
    for (int t = 0; t < 2000; t++) {
      for (auto &x : m) x = (uint8_t)rand();
      uint16_t c = link_crc16(m, sizeof m);
      memcpy(e, m, sizeof m);
      int k = rand() % (518 * 8);
      e[k / 8] ^= 0x80 >> (k % 8);
      one += link_crc16_single_bit(link_crc16(e, sizeof e) ^ c, 518 * 8);
      memcpy(e, m, sizeof m);
      for (int j = 0; j < 2; j++) { int q = rand() % (518 * 8); e[q / 8] ^= 0x80 >> (q % 8); }
      uint16_t sy = link_crc16(e, sizeof e) ^ c;
      two += sy && link_crc16_single_bit(sy, 518 * 8);
    }
    check(one == 2000 && two == 0, "CRC-16 single-bit statistic: every 1-bit error, never a 2-bit one");
  }

  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails ? 1 : 0;
}
