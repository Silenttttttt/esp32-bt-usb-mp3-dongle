// S3 side of the 'Q' PCM frames (link_protocol.h), shared by esp32-s3-msc.ino
// and the host test (crosscheck/pcm_crc_test.cpp) so both run the same code.
// The caller parses frames and checks the CRC; this decides what audio the
// encoder gets: good frames as they are, and for missing sequence numbers the
// same amount of concealment (pcm_conceal.h).
#ifndef PCM_LINK_RX_H
#define PCM_LINK_RX_H

#include <stdint.h>
#include "link_protocol.h"
#include "pcm_conceal.h"

class PcmLinkRx {
 public:
  static const uint32_t MAX_GAP_FRAMES = 16;  // ~93 ms; more is an outage, not a glitch
  static const uint32_t OUTAGE_MS = 100;      // no PCM this long: fade in from silence

  uint32_t crc_bad = 0;      // 'Q' frames that failed their CRC
  uint32_t lost_frames = 0;  // frames filled with concealment (from sequence gaps)
  uint32_t outages = 0;      // gaps too long to fill (faded in from silence)
  // Loss runs by length in frames: 1, 2, 3-4, 5-8, 9-16 (longer = outages).
  uint32_t runs[5] = {0, 0, 0, 0, 0};
  uint32_t last_gap = 0;     // frames missing before the latest good frame

  void on_crc_bad() { crc_bad++; }

  // A 'Q' frame that passed its CRC. pcm is edited in place. emit(ptr, bytes)
  // receives the audio for the encoder, in order.
  template <class Emit>
  void on_good(uint8_t seq, uint8_t *pcm, uint32_t len, uint32_t now_ms, Emit emit) {
    if (have_seq_) {
      uint8_t gap = (uint8_t)(seq - (uint8_t)(last_seq_ + 1));
      if (now_ms - last_ms_ > OUTAGE_MS || gap > MAX_GAP_FRAMES) {
        if (gap) { outages++; conceal_.outage(); }
      } else if (gap) {
        for (uint32_t f = 0; f < gap; f++) {
          conceal_.lost(fill_, LINK_Q_MAX_PCM / 2);
          emit(fill_, LINK_Q_MAX_PCM);
        }
        lost_frames += gap;
        runs[gap == 1 ? 0 : gap == 2 ? 1 : gap <= 4 ? 2 : gap <= 8 ? 3 : 4]++;
      }
    }
    last_gap = have_seq_ ? (uint8_t)(seq - (uint8_t)(last_seq_ + 1)) : 0;
    conceal_.good(pcm, len);
    emit(pcm, len);
    last_seq_ = seq;
    last_ms_ = now_ms;
    have_seq_ = true;
  }

 private:
  PcmConceal conceal_;
  uint8_t fill_[LINK_Q_MAX_PCM];  // not on the caller's stack (the S3's link_task has 8 KB)
  uint8_t last_seq_ = 0;
  uint32_t last_ms_ = 0;
  bool have_seq_ = false;
};

#endif  // PCM_LINK_RX_H
