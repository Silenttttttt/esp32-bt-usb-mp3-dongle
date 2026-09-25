// Single source of truth for the MP3 encode stage, shared by both boards.
//
// Which board runs it is chosen by ONE build flag, ENCODE_ON_S3, which must be
// set identically on BOTH boards' builds (see CLAUDE.md "Build/flash"):
//
//   ENCODE_ON_S3 not defined (default, the original design):
//     classic: Bluetooth PCM -> downmix to mono -> Mp3Pipeline -> MP3 'A' frames over UART
//     S3:      appends the 'A' frames to the ring
//
//   ENCODE_ON_S3 defined:
//     classic: Bluetooth PCM -> downmix to mono -> raw mono PCM 'P' frames over UART
//     S3:      Mp3Pipeline -> appends its MP3 frames to the ring
//
// Why move it (2026-09-24): Shine needs ~80KB of heap, which starved the
// classic (96% of RAM in use at full load, low-water mark 3KB, and the phone's
// AVRCP link couldn't connect while it was held) and ~11ms of every 20ms on the
// same core as the Bluetooth stack. The S3 has ~278KB internal RAM free and an
// idle core. Cost: the link carries PCM (88,200 B/s) instead of MP3
// (16,000 B/s), so it runs at a higher baud.
#ifndef MP3_PIPELINE_H
#define MP3_PIPELINE_H

#include "link_protocol.h"  // LINK_BAUD, frame types
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Shine.h"

// Mono, 44.1kHz, 16-bit in; MPEG-1 Layer III 128kbps CBR out (16,000 B/s).
// The rest of the pipeline (S3 ring sizing, silence frames, car_sim pacing)
// assumes exactly these parameters.
static const uint32_t MP3_PIPELINE_SAMPLE_RATE = 44100;
static const uint16_t MP3_PIPELINE_CHANNELS = 1;
static const uint16_t MP3_PIPELINE_BITS = 16;
static const int MP3_PIPELINE_KBPS = 128;

class Mp3Pipeline {
 public:
  // `sink` receives each MP3 frame in a single write() call (Shine emits
  // whole frames), which the S3 ring relies on to never split a frame.
  explicit Mp3Pipeline(Print &sink) : stream_(&sink, &encoder_) {}

  bool begin() {
    if (active_) return true;
    encoder_.setBitrate(MP3_PIPELINE_KBPS);
    AudioInfo info(MP3_PIPELINE_SAMPLE_RATE, MP3_PIPELINE_CHANNELS, MP3_PIPELINE_BITS);
    active_ = stream_.begin(info);
    return active_;
  }

  // Frees the encoder's heap (Shine's state). Flushes any partial frame.
  void end() {
    if (!active_) return;
    stream_.end();
    active_ = false;
  }

  bool active() const { return active_; }

  // Mono 16-bit little-endian PCM. A no-op while not begun.
  size_t write_mono(const uint8_t *pcm, size_t len) {
    return active_ ? stream_.write(pcm, len) : 0;
  }

 private:
  MP3EncoderShine encoder_;
  EncodedAudioStream stream_;
  bool active_ = false;
};

#endif  // MP3_PIPELINE_H
