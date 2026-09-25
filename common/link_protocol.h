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

#endif  // LINK_PROTOCOL_H
