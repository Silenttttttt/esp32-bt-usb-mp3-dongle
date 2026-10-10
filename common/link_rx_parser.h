// Classic -> S3 frame parser, shared by esp32-s3-msc.ino and the host test
// (crosscheck/pcm_crc_test.cpp), so both run the same code.
//
// Frames: 0xAA, type, 4-byte big-endian length, payload [, trailer].
// With LINK_Q the classic sends only 'C' (control) and 'R' (PCM, fixed size,
// CRC-32 trailer, link_protocol.h). For 'R':
//  - the length field is not trusted: every 'R' frame is LINK_R_PAYLOAD bytes,
//    and the CRC covers the length bytes too, so a flipped length bit is
//    found and fixed like any other bit;
//  - a CRC mismatch explained by one flipped bit is corrected (measured on the
//    bench 2026-10-10: ~45% of failures were single-bit, from sub-bit contact
//    breaks that let the RX pull-up turn a 0 into a 1). CRC-32 (Hamming
//    distance 4 at this length) makes a wrong correction ~1 in 10^6;
//  - if it can't be fixed, the bytes after the failed frame's magic are
//    pushed back and rescanned: when a byte went missing, the failed frame
//    swallowed the start of the next one, which this recovers.
//
// Src must provide: void read(uint8_t *buf, uint32_t n) (blocking, exact).
#ifndef LINK_RX_PARSER_H
#define LINK_RX_PARSER_H

#include <stdint.h>
#include <string.h>
#include "link_protocol.h"

struct LinkFrame {
  char type;         // 0 = nothing usable (see flags)
  uint32_t len;      // payload bytes
  bool bad_header;   // magic followed by an unknown type or impossible length
  bool crc_bad;      // 'R' frame that failed its CRC and couldn't be corrected
  bool corrected;    // 'R' frame fixed by flipping one bit back
};

template <class Src>
class LinkParser {
 public:
  static const uint32_t MAX_CONTROL = 256;  // 'C' frames are short text
  static const uint32_t PB_SIZE = 4096;

  uint32_t corrected_data = 0;     // one bit fixed in type/length/seq/PCM
  uint32_t corrected_trailer = 0;  // the CRC itself had one flipped bit
  uint32_t flips_0to1 = 0, flips_1to0 = 0;  // direction of the fixed data bits
  uint32_t uncorrectable = 0, bad_headers = 0, pushback_bytes = 0;

  // accept_raw: also accept unchecked 'P'/'A' frames (old classic builds).
  // A LINK_Q classic sends only 'C' and 'R', so the S3 passes false: a
  // rescan through PCM then can't mistake a stray byte pattern for 'P' audio.
  LinkParser(Src &src, bool accept_raw) : src_(src), accept_raw_(accept_raw) {}

  // Reads until the next frame or bad event. payload must hold MAX_FRAME_LEN.
  LinkFrame next(uint8_t *payload, uint32_t max_len) {
    LinkFrame f = {0, 0, false, false, false};
    uint8_t b;
    do { get(&b, 1); } while (b != LINK_MAGIC);
    uint8_t h[5];
    get(h, 5);
    char type = (char)h[0];
    uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    if (type == LINK_FRAME_PCM_CRC) {
      get(payload, LINK_R_PAYLOAD);
      uint8_t t[4];
      get(t, 4);
      uint32_t recv = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
      uint32_t calc = link_crc32(payload, LINK_R_PAYLOAD, link_crc32_update(h, 5));
      if (calc != recv && !try_correct(h, payload, calc ^ recv)) {
        uncorrectable++;
        // rescan everything after this frame's magic
        uint8_t back[5 + LINK_R_PAYLOAD + 4];
        memcpy(back, h, 5);
        memcpy(back + 5, payload, LINK_R_PAYLOAD);
        memcpy(back + 5 + LINK_R_PAYLOAD, t, 4);
        push_back(back, sizeof back);
        f.crc_bad = true;
        return f;
      }
      f.corrected = calc != recv;
      f.type = LINK_FRAME_PCM_CRC;
      f.len = LINK_R_PAYLOAD;
      return f;
    }
    bool ok = (type == LINK_FRAME_CONTROL && len <= MAX_CONTROL) ||
              (accept_raw_ && (type == LINK_FRAME_PCM || type == LINK_FRAME_MP3) && len <= max_len);
    if (!ok) {
      bad_headers++;
      push_back(h, 5);  // the magic was bogus; keep scanning right after it
      f.bad_header = true;
      return f;
    }
    if (len) get(payload, len);
    f.type = type;
    f.len = len;
    return f;
  }

 private:
  // syndrome = calc ^ recv. One flipped bit in the CRC field: popcount 1.
  // One flipped bit in the covered bytes (5 header bytes + payload): the
  // reflected CRC's response to a single bit, d bits before the end, is
  // POLY stepped d times through zeros.
  bool try_correct(uint8_t *h, uint8_t *payload, uint32_t syn) {
    if ((syn & (syn - 1)) == 0) { corrected_trailer++; return true; }
    const uint32_t nbits = (5 + LINK_R_PAYLOAD) * 8;
    uint32_t r = LINK_CRC32_POLY;
    for (uint32_t d = 0; d < nbits; d++) {
      if (r == syn) {
        uint32_t p = nbits - 1 - d;  // processing order: byte by byte, LSB first
        uint32_t byte = p / 8, bit = p % 8;
        if (byte == 0) return false;  // the fix would change the type: it was never an 'R'
        uint8_t *at = byte < 5 ? &h[byte] : &payload[byte - 5];
        if (*at & (1u << bit)) flips_0to1++; else flips_1to0++;  // received 1 = sent 0
        *at ^= (uint8_t)(1u << bit);
        corrected_data++;
        return true;
      }
      r = (r & 1) ? (r >> 1) ^ LINK_CRC32_POLY : r >> 1;
    }
    return false;
  }

  void get(uint8_t *buf, uint32_t n) {
    uint32_t k = n < pb_len_ ? n : pb_len_;
    if (k) {
      memcpy(buf, pb_ + pb_head_, k);
      pb_head_ += k;
      pb_len_ -= k;
      if (pb_len_ == 0) pb_head_ = 0;
    }
    if (n > k) src_.read(buf + k, n - k);
  }

  void push_back(const uint8_t *data, uint32_t n) {
    if (n + pb_len_ > PB_SIZE) n = PB_SIZE - pb_len_;  // can't happen with these frame sizes
    if (pb_head_ < n) {  // make room in front
      memmove(pb_ + PB_SIZE - pb_len_, pb_ + pb_head_, pb_len_);
      pb_head_ = PB_SIZE - pb_len_;
    }
    pb_head_ -= n;
    memcpy(pb_ + pb_head_, data, n);
    pb_len_ += n;
    pushback_bytes += n;
  }

  Src &src_;
  bool accept_raw_;
  uint8_t pb_[PB_SIZE];
  uint32_t pb_head_ = 0, pb_len_ = 0;
};

#endif  // LINK_RX_PARSER_H
