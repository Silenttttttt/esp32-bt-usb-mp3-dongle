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
static const uint8_t LINK_MAGIC = 0xAA;
// LINK_Q (opt-in, both boards): PCM with a check, 'R' frames. Every one is the
// same size: payload = 1-byte sequence number + exactly LINK_Q_MAX_PCM bytes of
// PCM (5.8 ms), then a 4-byte big-endian CRC-32 over the type byte, the 4
// length bytes and the payload. Fixed size: the S3 fills exactly the audio
// that went missing (sequence number), and doesn't have to trust the length.
// CRC-32 so that a one-bit error can be corrected safely (link_rx_parser.h).
// History: 2026-10-05 'Q' (CRC-16, variable size), replaced 2026-10-10 by 'R'
// after the bench showed ~45% single-bit failures. An S3 that doesn't know
// 'R' rejects it as an unknown type: blinking WHITE, like any build mismatch.
static const char LINK_FRAME_PCM_CRC = 'R';
static const uint32_t LINK_Q_MAX_PCM = 512;              // bytes of PCM per frame (256 samples)
static const uint32_t LINK_R_PAYLOAD = 1 + LINK_Q_MAX_PCM;  // seq + PCM
static const uint32_t LINK_CRC32_POLY = 0xEDB88320;      // CRC-32 (IEEE), reflected

// Raw register update (no init/xorout): link_crc32 = ~update(~0 ...).
static inline uint32_t link_crc32_raw(const uint8_t *data, uint32_t len, uint32_t crc) {
  static const uint32_t T[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                 0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  for (uint32_t i = 0; i < len; i++) {
    crc = (crc >> 4) ^ T[(crc ^ data[i]) & 0x0F];
    crc = (crc >> 4) ^ T[(crc ^ (data[i] >> 4)) & 0x0F];
  }
  return crc;
}
// CRC-32 over a message given in pieces: c = link_crc32_update(first, n);
// then link_crc32(next, m, c) for the last piece (applies the final xor).
static inline uint32_t link_crc32_update(const uint8_t *data, uint32_t len, uint32_t state = 0xFFFFFFFF) {
  return link_crc32_raw(data, len, state);
}
static inline uint32_t link_crc32(const uint8_t *data, uint32_t len, uint32_t state = 0xFFFFFFFF) {
  return ~link_crc32_raw(data, len, state);
}

// CRC-16/CCITT-FALSE (poly 0x1021), nibble table. Used by LINK_DIAG's single-bit
// statistics test (link_diag.h); the frames use CRC-32.
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
