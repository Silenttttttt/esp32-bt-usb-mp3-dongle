// LINK_DIAG helpers shared by both boards and the host test (no platform code).
//
// PcmSpikeMeter: counts adjacent mono samples that jump by more than
// SPIKE_JUMP. Music almost never does that in one 23 us sample; a corrupted
// sample usually does. Run on the classic (what goes onto the wire) and on the
// S3 (what came off it): spikes on the S3 only = the wire; on both = before it.
//
// link_crc16_single_bit(): with 'Q' frames, whether one flipped bit explains a
// CRC mismatch (CRC-16/CCITT has Hamming distance 4 up to 32767 bits, so a
// 2- or 3-bit error never looks like a 1-bit one). Tells scattered single-bit
// noise (fixable by correction) from bursts (not). Read it as a ratio over
// many failures: real single-bit noise reads ~100%; bursts read ~6-7% anyway
// (~4100 bit positions out of 65536 syndromes; measured 142/2000 for 8-byte
// bursts on 518-byte frames).
#ifndef LINK_DIAG_H
#define LINK_DIAG_H

#include <stdint.h>

struct PcmSpikeMeter {
  static const int32_t SPIKE_JUMP = 30000;
  uint32_t spikes = 0;    // since reset()
  int32_t max_jump = 0;   // since reset()
  int32_t last = 0;
  bool have_last = false;

  // Mono 16-bit little-endian. continuous=false: don't compare with the
  // previous call's last sample (a gap or a fill came in between).
  void feed(const uint8_t *pcm, uint32_t len, bool continuous = true) {
    uint32_t n = len / 2;
    if (!continuous) have_last = false;
    for (uint32_t i = 0; i < n; i++) {
      int32_t v = (int16_t)(pcm[2 * i] | (pcm[2 * i + 1] << 8));
      if (have_last) {
        int32_t d = v > last ? v - last : last - v;
        if (d > max_jump) max_jump = d;
        if (d > SPIKE_JUMP) spikes++;
      }
      last = v;
      have_last = true;
    }
  }
  void reset() { spikes = 0; max_jump = 0; }
};

// syndrome = computed CRC ^ received CRC, over a message of `bits` bits (the
// bytes the CRC covers). True if a single flipped bit in the message or in
// the 16-bit CRC itself accounts for it.
static inline bool link_crc16_single_bit(uint16_t syndrome, uint32_t bits) {
  if (syndrome == 0) return false;
  if ((syndrome & (syndrome - 1)) == 0) return true;  // one bit of the CRC field
  uint16_t r = 0x1021;  // error in the last message bit
  for (uint32_t d = 0; d < bits; d++) {
    if (r == syndrome) return true;
    r = (uint16_t)((r << 1) ^ ((r & 0x8000) ? 0x1021 : 0));
  }
  return false;
}

#endif  // LINK_DIAG_H
