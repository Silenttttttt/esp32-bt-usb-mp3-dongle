// Hides PCM lost on the classic -> S3 wire (ENCODE_ON_S3, 'Q' frames).
//
// The S3 learns from the 'Q' sequence numbers how many frames are missing and
// asks for that many samples of replacement before the next good frame. The
// replacement repeats the last pitch period of the good audio (the lag where
// the newest samples best match earlier ones), so it continues the waveform
// in phase, while fading out over ~23 ms. Each join (good audio -> repeat, and
// every loop of the period) gets a decaying offset so the waveform has no
// jump. Audio with no clear period (match < 0.6) isn't repeated, since an
// out-of-phase copy is further from the truth than silence: it fades to
// silence in ~3 ms instead. The next good frame cross-fades in
// from that continuation. This is the usual packet-loss concealment of voice
// codecs (e.g. G.711 Appendix I). A few ms of music filled this way is hard
// to hear; a flipped bit passed through is a full-scale crack.
//
// Mono 16-bit little-endian. No platform dependencies (host-testable).
#ifndef PCM_CONCEAL_H
#define PCM_CONCEAL_H

#include <stdint.h>
#include <string.h>

class PcmConceal {
 public:
  static const uint32_t HIST = 1024;         // samples of good audio kept (23 ms)
  static const uint32_t XFADE = 64;          // cross-fade into the next good frame (1.5 ms)
  static const int32_t FADE_STEPS = 1024;    // replacement fades out over 1024 samples (23 ms)
  static const int32_t FAST_FADE = 128;      // ...or over 128 (2.9 ms) when there's no clear period
  static const uint32_t JOIN = 32;           // samples over which a join's offset decays

  // Replacement samples for `samples` lost samples, written to out (2 bytes each).
  void lost(uint8_t *out, uint32_t samples) {
    if (!concealing_) find_period();
    for (uint32_t i = 0; i < samples; i++) put(out, i, next_cont());
    concealing_ = true;
  }

  // The stream stopped for a while (no PCM for a long time): fade the next
  // good frame in from silence instead of from the history.
  void outage() {
    gain_ = 0;
    concealing_ = true;
  }

  // A good frame, edited in place: cross-fades in after a loss, then becomes
  // the new history.
  void good(uint8_t *pcm, uint32_t len) {
    uint32_t n = len / 2;
    if (concealing_) {
      uint32_t f = n < XFADE ? n : XFADE;
      for (uint32_t i = 0; i < f; i++) {
        int32_t c = next_cont();
        int32_t g = get(pcm, i);
        put(pcm, i, (int16_t)((g * (int32_t)i + c * (int32_t)(f - i)) / (int32_t)f));
      }
      concealing_ = false;
    }
    // Shift the new samples into the history.
    if (n >= HIST) {
      for (uint32_t i = 0; i < HIST; i++) hist_[i] = get(pcm, n - HIST + i);
    } else {
      memmove(hist_, hist_ + n, (HIST - n) * sizeof(int16_t));
      for (uint32_t i = 0; i < n; i++) hist_[HIST - n + i] = get(pcm, i);
    }
    gain_ = FADE_STEPS;
  }

 private:
  static const uint32_t WIN = 256;     // samples matched when looking for the period (5.8 ms)
  static const uint32_t MIN_LAG = 32;  // ~1.4 kHz
  static const uint32_t MAX_LAG = HIST - WIN;

  // The lag (MIN_LAG..MAX_LAG) where the newest WIN samples best match the WIN
  // samples that far back (normalized cross-correlation). Playback then starts
  // one period back and loops over that period.
  void find_period() {
    double best_score = -1, best_corr = 0;
    uint32_t best = MAX_LAG;
    int64_t xx = 0;
    for (uint32_t i = 0; i < WIN; i++) xx += (int64_t)hist_[HIST - WIN + i] * hist_[HIST - WIN + i];
    for (uint32_t lag = MIN_LAG; lag <= MAX_LAG; lag++) {
      int64_t xy = 0, yy = 0;
      for (uint32_t i = 0; i < WIN; i++) {
        int32_t x = hist_[HIST - WIN + i], y = hist_[HIST - WIN + i - lag];
        xy += (int64_t)x * y;
        yy += (int64_t)y * y;
      }
      if (xy <= 0 || yy == 0) continue;
      double score = (double)xy * (double)xy / (double)yy;  // xy / |y|, squared
      if (score > best_score) {
        best_score = score;
        best = lag;
        best_corr = xx ? score / (double)xx : 0;  // normalized correlation, squared
      }
    }
    period_ = best;
    pos_ = HIST - period_;
    decay_ = best_corr >= 0.36 ? 1 : FADE_STEPS / FAST_FADE;  // corr >= 0.6
    start_join();
  }

  // The repeat restarts at hist_[HIST - period_]; offset it toward the sample
  // it should follow (the newest one), decaying over JOIN samples.
  void start_join() {
    join_off_ = (int32_t)hist_[HIST - 1] - (int32_t)hist_[HIST - period_ - 1];
    join_n_ = 0;
  }

  int16_t next_cont() {
    int32_t v = hist_[pos_];
    if (join_n_ < JOIN) { v += join_off_ * (int32_t)(JOIN - join_n_) / (int32_t)JOIN; join_n_++; }
    v = v * gain_ / FADE_STEPS;
    gain_ = gain_ > decay_ ? gain_ - decay_ : 0;
    if (++pos_ >= HIST) { pos_ = HIST - period_; start_join(); }
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
  }
  static int16_t get(const uint8_t *p, uint32_t i) { return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8)); }
  static void put(uint8_t *p, uint32_t i, int16_t v) {
    p[2 * i] = (uint8_t)(v & 0xFF);
    p[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
  }
  int16_t hist_[HIST] = {0};
  uint32_t pos_ = 0, period_ = MAX_LAG, join_n_ = JOIN;
  int32_t join_off_ = 0, decay_ = 1;
  int32_t gain_ = 0;  // nothing heard yet: a first loss replays silence
  bool concealing_ = false;
};

#endif  // PCM_CONCEAL_H
