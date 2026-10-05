// Classic -> S3 UART link constants, shared by both boards in every build mode
// so the two sides can never disagree. See mp3_pipeline.h for ENCODE_ON_S3.
#ifndef LINK_PROTOCOL_H
#define LINK_PROTOCOL_H

#include <stdint.h>

// With ENCODE_ON_S3 the link carries mono PCM (88,200 B/s plus framing)
// instead of MP3 (16,000 B/s). 2,000,000 baud (8N1) carries 200,000 B/s,
// ~45% utilized, and is an exact divisor of the 80MHz APB clock on both chips.
#ifdef ENCODE_ON_S3
static const uint32_t LINK_BAUD = 2000000;
#else
static const uint32_t LINK_BAUD = 921600;
#endif

// Frames: 1-byte magic 0xAA, 1-byte type, 4-byte big-endian length, payload.
static const char LINK_FRAME_MP3 = 'A';      // MP3 frames (classic encodes)
static const char LINK_FRAME_CONTROL = 'C';  // "millis|text" control/status
static const char LINK_FRAME_PCM = 'P';      // mono 16-bit LE PCM, 44.1kHz (S3 encodes)
// Same PCM, followed by a 2-byte big-endian CRC-16 over the type byte, the 4
// length bytes and the payload. The length field counts the payload only.
// Added 2026-10-05: a wobbling micro-USB power plug on the classic made the
// car crackle on rough roads. 'P' had no check, so flipped bits went straight
// into the encoder as full-scale spikes. With the CRC the S3 drops a bad frame
// and fades over the gap (pcm_conceal.h). An S3 that predates 'Q' rejects it
// as an unknown type: blinking WHITE, same as any other build mismatch.
static const char LINK_FRAME_PCM_CRC = 'Q';

// CRC-16/CCITT-FALSE (poly 0x1021), nibble table. ~2 KB per frame, 43 frames/s.
static inline uint16_t link_crc16(const uint8_t *data, uint32_t len, uint16_t crc = 0xFFFF) {
  static const uint16_t T[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF};
  for (uint32_t i = 0; i < len; i++) {
    crc = (uint16_t)((crc << 4) ^ T[(crc >> 12) ^ (data[i] >> 4)]);
    crc = (uint16_t)((crc << 4) ^ T[(crc >> 12) ^ (data[i] & 0x0F)]);
  }
  return crc;
}

#endif  // LINK_PROTOCOL_H
