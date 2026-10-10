// Host test for the 'Q' PCM frames: CRC-16, sequence numbers, and loss
// concealment (common/pcm_link_rx.h, pcm_conceal.h -- the same code the S3 runs).
//
//   g++ -O2 -std=c++17 -Wall -o pcm_crc_test pcm_crc_test.cpp && ./pcm_crc_test
//
// Builds the byte stream the classic's send_pcm_crc() produces, sprays bursts
// of garbage over it (a wobbling plug on the wire), and parses it the way
// link_task does (find_sync, header check, link_q_len_ok, link_q_crc_ok, then
// PcmLinkRx). Checks:
//   - the CRC matches the published check value; a clean stream is bit-exact;
//   - no corrupted audio reaches the encoder; the output keeps the input's
//     length (lost frames are filled, not skipped);
//   - the output has no step bigger than the signal's own (+ cross-fade slack);
//   - the fill is closer to the lost audio than silence, on two signals;
//   - the same corruption through the old unchecked 'P' path makes big spikes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../common/pcm_link_rx.h"
#include "../../common/link_diag.h"

static const uint8_t MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;
static const uint32_t CHUNK = 2048;  // what the classic's loop() hands emit_mono (1024 samples)

static int fails = 0;
static void check(bool ok, const char *what) {
  printf("  %-66s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) fails++;
}

static int16_t sample(const std::vector<uint8_t> &p, size_t i) { return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8)); }

// kind 0: two tones. kind 1: a non-harmonic chord (A3, C#4 slightly sharp,
// F4 + a 2.7 kHz partial) with a slow tremolo and ~10% noise -- less periodic.
static std::vector<uint8_t> make_pcm(int chunks, int kind) {
  std::vector<uint8_t> pcm(chunks * CHUNK);
  srand(99);
  for (size_t i = 0; i < pcm.size() / 2; i++) {
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

// The classic's send_pcm_crc(): exactly LINK_Q_MAX_PCM bytes per frame, the
// remainder of a chunk carried into the next.
struct QSender {
  uint8_t seq = 0, pend[LINK_Q_MAX_PCM];
  uint32_t pend_n = 0;
  void frame(std::vector<uint8_t> &wire, const uint8_t *pcm) {
    uint32_t plen = LINK_Q_MAX_PCM + 1;
    uint8_t h[7] = {MAGIC, (uint8_t)LINK_FRAME_PCM_CRC, (uint8_t)(plen >> 24), (uint8_t)(plen >> 16),
                    (uint8_t)(plen >> 8), (uint8_t)plen, seq++};
    uint16_t crc = link_crc16(pcm, LINK_Q_MAX_PCM, link_crc16(h + 1, 6));
    wire.insert(wire.end(), h, h + 7);
    wire.insert(wire.end(), pcm, pcm + LINK_Q_MAX_PCM);
    wire.push_back(crc >> 8);
    wire.push_back(crc & 0xFF);
  }
  void send(std::vector<uint8_t> &wire, const uint8_t *pcm, uint32_t len) {
    while (len) {
      uint32_t take = std::min(LINK_Q_MAX_PCM - pend_n, len);
      memcpy(pend + pend_n, pcm, take);
      pend_n += take;
      if (pend_n == LINK_Q_MAX_PCM) { frame(wire, pend); pend_n = 0; }
      pcm += take;
      len -= take;
    }
  }
};

static void send_p(std::vector<uint8_t> &wire, const uint8_t *pcm, uint32_t len) {
  uint8_t h[6] = {MAGIC, (uint8_t)LINK_FRAME_PCM, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
  wire.insert(wire.end(), h, h + 6);
  wire.insert(wire.end(), pcm, pcm + len);
}

// Bursts of random bytes; returns the wire byte ranges hit.
static void corrupt(std::vector<uint8_t> &wire, int bursts, unsigned seed) {
  srand(seed);
  for (int b = 0; b < bursts; b++) {
    size_t at = 1000 + (size_t)rand() % (wire.size() - 2000);
    size_t n = 1 + rand() % 300;
    for (size_t i = 0; i < n && at + i < wire.size(); i++) wire[at + i] = (uint8_t)rand();
  }
}

struct Out {
  std::vector<uint8_t> pcm;
  std::vector<bool> filled;  // per output sample: concealment, not received
  PcmLinkRx rx;
  uint32_t bad_headers = 0;
};

// link_task, ENCODE_ON_S3. Time: each byte takes 5 us at 2 Mbaud.
static void parse(const std::vector<uint8_t> &wire, Out &out) {
  size_t pos = 0;
  uint8_t payload[MAX_FRAME_LEN];
  bool filling = false;
  auto emit = [&](const uint8_t *p, uint32_t n) {
    out.pcm.insert(out.pcm.end(), p, p + n);
    for (uint32_t i = 0; i < n / 2; i++) out.filled.push_back(filling);
  };
  while (true) {
    while (pos < wire.size() && wire[pos] != MAGIC) pos++;
    if (pos + 6 > wire.size()) break;
    pos++;
    const uint8_t *header = &wire[pos];
    pos += 5;
    char type = (char)header[0];
    uint32_t len = ((uint32_t)header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
    bool known = type == LINK_FRAME_CONTROL || type == LINK_FRAME_PCM ||
                 (type == LINK_FRAME_PCM_CRC && link_q_len_ok(len));
    if (!known || len > MAX_FRAME_LEN) { out.bad_headers++; continue; }
    uint32_t need = len + (type == LINK_FRAME_PCM_CRC ? 2 : 0);
    if (pos + need > wire.size()) break;
    memcpy(payload, &wire[pos], len);
    pos += len;
    uint32_t now_ms = (uint32_t)(pos * 5 / 1000);
    if (type == LINK_FRAME_PCM_CRC) {
      const uint8_t *trailer = &wire[pos];
      pos += 2;
      if (!link_q_crc_ok(header, payload, len, trailer)) { out.rx.on_crc_bad(); continue; }
      // Mark samples emitted before the good frame's own as filled.
      size_t before = out.pcm.size();
      filling = true;
      out.rx.on_good(payload[0], payload + 1, len - 1, now_ms, [&](const uint8_t *p, uint32_t n) {
        if (p == payload + 1) filling = false;  // the good frame itself
        emit(p, n);
      });
      (void)before;
      filling = false;
    } else if (type == LINK_FRAME_PCM) {
      emit(payload, len);
    }
  }
}

static int max_step(const std::vector<uint8_t> &p) {
  int m = 0;
  for (size_t i = 1; i < p.size() / 2; i++) m = std::max(m, abs(sample(p, i) - sample(p, i - 1)));
  return m;
}

int main() {
  printf("CRC and frame checks\n");
  check(link_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value 0x29B1");
  check(link_q_len_ok(513) && link_q_len_ok(3) && !link_q_len_ok(514) && !link_q_len_ok(512) && !link_q_len_ok(0x0A000201),
        "'Q' length: seq + 2..512 bytes of whole samples only");

  const int CHUNKS = 1000;  // ~23 s
  for (int kind = 0; kind < 2; kind++) {
    std::vector<uint8_t> src = make_pcm(CHUNKS, kind);
    int src_step = max_step(src);

    std::vector<uint8_t> clean;
    QSender qs;
    // uneven chunks, like phone packets split across slots (1552 + 496 B pattern)
    for (size_t off = 0, k = 0; off < src.size(); k++) {
      uint32_t n = std::min<size_t>(k % 2 ? 496 : 1552, src.size() - off);
      qs.send(clean, &src[off], n);
      off += n;
    }

    printf("== signal: %s ==\nClean 'Q' stream\n", kind ? "chord + noise" : "two tones");
    {
      Out o;
      parse(clean, o);
      check(o.pcm == src && o.rx.crc_bad == 0 && o.bad_headers == 0, "bit-exact, no CRC failures");
    }

    printf("'Q' stream with 40 garbage bursts (1-300 bytes each)\n");
    std::vector<uint8_t> q = clean;
    corrupt(q, 40, 7);
    Out o;
    parse(q, o);
    printf("  crc_bad=%u bad_headers=%u lost_frames=%u outages=%u\n", o.rx.crc_bad, o.bad_headers, o.rx.lost_frames, o.rx.outages);
    check(o.rx.crc_bad + o.bad_headers > 0 && o.rx.lost_frames > 0, "corruption detected");
    check(o.pcm.size() == src.size(), "output as long as the input (lost frames filled, not skipped)");
    bool same = o.pcm.size() == src.size();
    size_t filled = 0;
    double err_fill = 0, err_silence = 0;
    for (size_t i = 0; same && i < src.size() / 2; i++) {
      if (o.filled[i]) {
        filled++;
        double d = sample(o.pcm, i) - sample(src, i);
        err_fill += d * d;
        err_silence += (double)sample(src, i) * sample(src, i);
      } else if (sample(o.pcm, i) != sample(src, i)) {
        // allowed only inside the cross-fade right after a fill
        bool in_xfade = false;
        for (size_t k = 1; k <= PcmConceal::XFADE && k <= i; k++) if (o.filled[i - k]) in_xfade = true;
        if (!in_xfade) { same = false; printf("  corrupt sample at %zu\n", i); }
      }
    }
    check(same, "every received sample is original (no corrupt audio reaches the encoder)");
    printf("  filled %zu samples (%.1f ms) of %zu (%.3f%%)\n", filled, filled / 44.1, src.size() / 2, 100.0 * filled / (src.size() / 2));
    int q_step = max_step(o.pcm);
    printf("  max step: signal %d, output %d\n", src_step, q_step);
    check(q_step <= src_step + 1100, "no step bigger than the signal's own (+1100 cross-fade slack)");
    printf("  fill error energy vs silence: %.2f\n", err_silence > 0 ? err_fill / err_silence : 0.0);
    check(err_fill < err_silence, "the fill is closer to the lost audio than silence");
  }

  std::vector<uint8_t> src = make_pcm(CHUNKS, 0);
  int src_step = max_step(src);
  printf("Same corruption through the old unchecked 'P' path\n");
  {
    std::vector<uint8_t> p;
    for (int c = 0; c < CHUNKS; c++) send_p(p, &src[c * CHUNK], CHUNK);
    corrupt(p, 40, 7);
    Out old;
    parse(p, old);
    int p_step = max_step(old.pcm);
    printf("  max step: %d\n", p_step);
    check(p_step > 4 * src_step, "spikes get through without the CRC (the test can see them)");
  }

  printf("Long gaps: 40 frames lost in a row (232 ms), time consistent\n");
  {
    std::vector<uint8_t> src = make_pcm(200, 0), wire;
    QSender qs;
    qs.send(wire, src.data(), src.size());
    // cut frames 100..139 out of the wire (each 'Q' frame is 7+512+2 bytes)
    const size_t F = 7 + LINK_Q_MAX_PCM + 2;
    wire.erase(wire.begin() + 100 * F, wire.begin() + 140 * F);
    // parse() derives time from byte position; insert idle time by padding
    // the gap with non-magic filler bytes worth 232 ms at 2 Mbaud
    wire.insert(wire.begin() + 100 * F, 232 * 200, (uint8_t)0x00);
    Out o;
    parse(wire, o);
    printf("  silence_frames=%u lost_frames=%u outages=%u out=%zu in=%zu\n", o.rx.silence_frames, o.rx.lost_frames,
           o.rx.outages, o.pcm.size(), src.size());
    check(o.rx.silence_frames == 40 && o.rx.outages == 0 && o.pcm.size() == src.size(),
          "a 232 ms gap is filled with exactly 232 ms (cushion keeps its size)");
  }
  printf("Restart: sequence jumps with a time gap that doesn't match\n");
  {
    std::vector<uint8_t> src = make_pcm(100, 0), wire;
    QSender a1, a2;
    a1.send(wire, src.data(), src.size() / 2);
    wire.insert(wire.end(), 3 * 200000, (uint8_t)0x00);  // 3 s of nothing
    a2.seq = 7;  // a fresh classic boot starts its sequence anew
    a2.send(wire, src.data() + src.size() / 2, src.size() / 2);
    Out o;
    parse(wire, o);
    check(o.rx.outages == 1 && o.rx.silence_frames == 0 && o.rx.lost_frames == 0,
          "a 3 s stop is an outage, not filled (the S3's underrun handles it)");
  }

  printf("LINK_DIAG helpers\n");
  {
    // single-bit check on 518-byte messages (a 'Q' frame's CRC coverage)
    srand(3);
    uint8_t m[518], e[518];
    int one = 0, two = 0, burst = 0;
    const int N = 2000;
    for (int t = 0; t < N; t++) {
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
      memcpy(e, m, sizeof m);
      int at = rand() % 500;
      for (int j = 0; j < 8; j++) e[at + j] = (uint8_t)rand();
      sy = link_crc16(e, sizeof e) ^ c;
      burst += sy && link_crc16_single_bit(sy, 518 * 8);
    }
    printf("  single-bit read as single: %d/%d; 2-bit: %d/%d; 8-byte bursts: %d/%d\n", one, N, two, N, burst, N);
    check(one == N && two == 0, "single-bit check: every 1-bit error, never a 2-bit one");
    check(burst < N / 10, "bursts read as single-bit under 10% (ratio, not a verdict)");
    // spike meter: clean music none, corrupted 'P' stream many
    std::vector<uint8_t> clean_pcm = make_pcm(200, 1);
    PcmSpikeMeter a;
    a.feed(clean_pcm.data(), clean_pcm.size());
    std::vector<uint8_t> p;
    for (int c = 0; c < 200; c++) send_p(p, &clean_pcm[c * CHUNK], CHUNK);
    corrupt(p, 10, 5);
    Out old;
    parse(p, old);
    PcmSpikeMeter b;
    b.feed(old.pcm.data(), old.pcm.size());
    printf("  spikes: clean %u (max jump %d), 10 bursts through 'P' %u (max jump %d)\n", a.spikes, a.max_jump, b.spikes, b.max_jump);
    check(a.spikes == 0 && b.spikes > 10, "spike meter: silent on clean audio, fires on corruption");
  }

  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails ? 1 : 0;
}
