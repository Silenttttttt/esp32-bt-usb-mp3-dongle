// Hides lost PCM frames on the S3 (ENCODE_ON_S3, 'Q' frames). A frame that
// fails its CRC, or bytes lost to a bad header, are replaced by a frame that
// fades from the last good sample to silence, and the next good frame fades
// back in. A dropout of ~23 ms with no step in the waveform, instead of a
// full-scale spike. No platform dependencies, so the host test can include it.
#ifndef PCM_CONCEAL_H
#define PCM_CONCEAL_H

#include <stdint.h>
#include <string.h>

class PcmConceal {
 public:
  static const uint32_t FADE_SAMPLES = 128;  // ~2.9 ms at 44.1 kHz

  // A good frame (mono 16-bit LE, edited in place): fades in if the previous
  // frame was lost.
  void good(uint8_t *pcm, uint32_t len) {
    uint32_t n = len / 2;
    if (n == 0) return;
    if (fade_in_) {
      uint32_t f = n < FADE_SAMPLES ? n : FADE_SAMPLES;
      for (uint32_t i = 0; i < f; i++) put(pcm, i, (int16_t)((int32_t)get(pcm, i) * (int32_t)i / (int32_t)f));
      fade_in_ = false;
    }
    last_ = get(pcm, n - 1);
    last_len_ = len;
  }

  // Fills out[0..len) with the replacement for a lost frame.
  void lost(uint8_t *out, uint32_t len) {
    uint32_t n = len / 2;
    memset(out, 0, len);
    uint32_t f = n < FADE_SAMPLES ? n : FADE_SAMPLES;
    for (uint32_t i = 0; i < f; i++) put(out, i, (int16_t)((int32_t)last_ * (int32_t)(f - 1 - i) / (int32_t)f));
    last_ = 0;
    fade_in_ = true;
  }

  // Length of the last good frame, for sizing a replacement when the header
  // itself was bad (so the real length is unknown).
  uint32_t last_len() const { return last_len_; }

 private:
  static int16_t get(const uint8_t *p, uint32_t i) { return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8)); }
  static void put(uint8_t *p, uint32_t i, int16_t v) {
    p[2 * i] = (uint8_t)(v & 0xFF);
    p[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
  }
  int16_t last_ = 0;
  bool fade_in_ = false;
  uint32_t last_len_ = 2048;  // one mono slot, ~23 ms
};

#endif  // PCM_CONCEAL_H
