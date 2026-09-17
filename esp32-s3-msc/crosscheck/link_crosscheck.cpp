// Cross-checks the UART frame-parsing/resync logic from esp32-s3-msc.ino's
// link_task (find_sync + header validation + payload read) against
// s3_sim_serial.py's receive_from_esp32(), by feeding both an identical
// synthetic byte stream containing: leading garbage, a valid 'A' frame, a
// bad-type byte sequence that must trigger resync, a valid 'C' frame, an
// oversized-length frame that must also trigger resync, and a final valid
// 'A' frame. Host-only -- reads from an in-memory buffer instead of a real
// UART.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
using namespace std;

static const uint8_t FRAME_MAGIC = 0xAA;
static const uint32_t MAX_FRAME_LEN = 4096;

struct Frame { char type; vector<uint8_t> payload; };

static vector<uint8_t> g_stream;
static size_t g_pos = 0;

static bool read_exact(uint8_t *buf, uint32_t n) {
  if (g_pos + n > g_stream.size()) return false;  // end of synthetic stream
  memcpy(buf, g_stream.data() + g_pos, n);
  g_pos += n;
  return true;
}

static bool find_sync() {
  uint8_t b;
  while (read_exact(&b, 1)) {
    if (b == FRAME_MAGIC) return true;
  }
  return false;
}

static vector<Frame> parse_stream() {
  vector<Frame> out;
  static uint8_t payload[MAX_FRAME_LEN];
  while (find_sync()) {
    uint8_t header[5];
    if (!read_exact(header, 5)) break;
    char frame_type = (char)header[0];
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                       ((uint32_t)header[3] << 8) | (uint32_t)header[4];
    if ((frame_type != 'A' && frame_type != 'C') || length > MAX_FRAME_LEN) {
      continue;  // resync
    }
    if (length && !read_exact(payload, length)) break;
    Frame f;
    f.type = frame_type;
    f.payload.assign(payload, payload + length);
    out.push_back(f);
  }
  return out;
}

static void append_frame(vector<uint8_t> &buf, char type, const string &payload) {
  buf.push_back(FRAME_MAGIC);
  buf.push_back((uint8_t)type);
  uint32_t len = payload.size();
  buf.push_back((len >> 24) & 0xFF);
  buf.push_back((len >> 16) & 0xFF);
  buf.push_back((len >> 8) & 0xFF);
  buf.push_back(len & 0xFF);
  for (char c : payload) buf.push_back((uint8_t)c);
}

int main() {
  vector<uint8_t> stream;
  // leading garbage (no magic byte in it, by construction)
  for (uint8_t b : {0x01, 0x02, 0x03, 0x00, 0x7F}) stream.push_back(b);
  append_frame(stream, 'A', "first-audio-frame-payload");
  // bad-type frame: magic + bogus type 'Z' + a plausible length + garbage payload bytes
  // -- must be rejected and resynced past, not crash or desync permanently
  stream.push_back(FRAME_MAGIC);
  stream.push_back((uint8_t)'Z');
  stream.push_back(0); stream.push_back(0); stream.push_back(0); stream.push_back(5);
  for (uint8_t b : {0x11, 0x22, 0x33, 0x44, 0x55}) stream.push_back(b);
  append_frame(stream, 'C', "123|ENCODE_US:avg=11000,max=15000,n=50,core=1");
  // oversized-length frame -- magic + valid type + length > MAX_FRAME_LEN
  stream.push_back(FRAME_MAGIC);
  stream.push_back((uint8_t)'A');
  uint32_t bad_len = MAX_FRAME_LEN + 100;
  stream.push_back((bad_len >> 24) & 0xFF); stream.push_back((bad_len >> 16) & 0xFF);
  stream.push_back((bad_len >> 8) & 0xFF); stream.push_back(bad_len & 0xFF);
  append_frame(stream, 'A', "second-audio-frame-payload-after-recovery");

  g_stream = stream;
  g_pos = 0;
  vector<Frame> frames = parse_stream();

  printf("parsed %zu frames:\n", frames.size());
  for (auto &f : frames) {
    printf("  type=%c len=%zu payload=%.*s\n", f.type, f.payload.size(),
           (int)f.payload.size(), f.payload.data());
  }
  return 0;
}
