// Real ESP32-S3 firmware: presents a USB-MSC "virtual disk" (one growing
// FAT12 volume, one MP3 file) to the car radio, backed by a live ring
// buffer fed over UART from the classic ESP32 (BT A2DP sink + Shine
// encoder). This is the real hardware counterpart to sim/fat12_disk.py +
// sim/s3_sim_serial.py -- see CLAUDE.md for the full real/prototype
// distinction. The ring-buffer/FAT12 algorithm here is a near-verbatim
// port of fat12_disk.py's GrowingFat12Disk; the UART framing matches
// esp32-bt-mp3-test.ino's send_framed()/send_control() wire format
// exactly (FRAME_MAGIC 0xAA, 1-byte type, 4-byte big-endian length,
// payload), since the real wired link is that same UART, just physically
// wired to another board's RX pin instead of a USB-serial chip to a PC.
//
// STATUS AS OF 2026-09-17 (written the night before the physical board
// arrives -- see progress/STATUS.md for the full writeup): this compiles
// clean against esp32:esp32:esp32s3:USBMode=default,PSRAM=opi (confirmed
// the right PSRAM setting from the actual purchase listing: ESP32-S3-
// WROOM-1 N16R8, 16MB Quad flash + 8MB Octal PSRAM), and the logic has
// been carefully cross-checked line-by-line against the already-validated
// Python prototype (see esp32-s3-msc/crosscheck/ -- boot sector, FAT
// table, root directory, ring-buffer/backpressure logic, and UART
// framing/resync all confirmed byte-for-byte or step-for-step identical
// to the proven reference), but **it has NEVER run on real ESP32-S3
// hardware** -- there was no board to test on yet. One thing still
// specifically needs real-hardware verification: UART_S3_RX_PIN below
// (the actual wiring isn't known yet). The other original open question
// -- TinyUSB's onRead call granularity -- is now resolved from TinyUSB's
// own public source/docs (github.com/hathach/tinyusb, msc_device.c),
// not just assumed: `bufsize` is a fixed compile-time constant
// (CFG_TUD_MSC_EP_BUFSIZE, commonly 512B on this platform) every call,
// and TinyUSB itself splits a larger SCSI transfer into multiple calls
// at that fixed size, advancing `offset` each time -- disk_read_at()
// below was already written to handle an arbitrary byte range generically,
// so this changes nothing about the implementation, just upgrades this
// from "reasoned assumption" to "confirmed from the actual library
// source." Treat every other "should work" comment below as "reasoned
// from the working Python prototype and the real board's own datasheet
// -- not yet hardware-confirmed."
//
// To regenerate silence_primer.h after changing sim/silence_primer.mp3:
//   python3 -c "
//   data = open('../sim/silence_primer.mp3','rb').read()
//   ... (see esp32-s3-msc.ino git history for the exact one-liner used)"

#include <Arduino.h>

#ifndef ARDUINO_USB_MODE
#error This board has no native USB -- must be an S2/S3 target
#elif ARDUINO_USB_MODE == 1
#error Build with USB Mode = "USB-OTG (TinyUSB)", not "Hardware CDC and JTAG"
#else

#include "USB.h"
#include "USBMSC.h"
#include "esp_heap_caps.h"
#include "silence_primer.h"
#include "msc_trace.h"  // MSC_TRACE: declarations the shared header's hooks need
#include "fat_disk_shared.h"
#include "msc_trace.h"  // MSC_TRACE: the rest
#include "../common/link_protocol.h"  // LINK_BAUD + frame types, shared with the classic
#ifdef ENCODE_ON_S3
#include "../common/mp3_pipeline.h"   // the shared encoder (same source the classic uses by default)
#endif
#include <Adafruit_NeoPixel.h>

// UART pins to the classic ESP32's TX (its Serial/UART0 TX pin, wired
// directly -- see CLAUDE.md's real end-state architecture diagram).
//
// HARDWARE-VERIFIED 2026-09-18: GPIO8 confirmed receiving real, continuous
// electrical activity (~17-18K edges/sec, matching real 921600-baud
// traffic) via a raw interrupt-based edge-counter test, bypassing the UART
// peripheral entirely. GPIO18 was tried first and showed zero activity --
// root cause turned out to be a physical wiring mix-up, not a bad pin:
// this board's silkscreen-labeled "RX" pin near the programming/COM port
// is the board's OWN UART0 programming pin (wired to the onboard USB-
// serial bridge chip), not a general-purpose GPIO -- wiring into it
// doesn't reach GPIO18 (or GPIO8) at all, and can also corrupt the
// flashing handshake if something else is actively driving it at the same
// time. Use the header pin literally labeled "8"/"GPIO8"/"IO8", not "RX".
//
// Real target board confirmed 2026-09-17 (Muni sent the actual purchase
// listing): ESP32-S3-WROOM-1 N16R8 DevKitC-1 -- 16MB Quad flash + 8MB
// Octal PSRAM. This matters for pin choice: GPIO 26-32 are dedicated to
// the Quad flash on ANY ESP32-S3 module, and Octal PSRAM (this module
// has it -- confirms PSRAM=opi below is the right build flag, not a
// guess) additionally dedicates GPIO 33-37 (Espressif's own
// ESP32-S3-WROOM-1 datasheet: "not recommended for other uses" on
// octal-PSRAM variants). GPIO 8 below is well outside that reserved
// 26-37 range, so no conflict.
#define UART_S3_RX_PIN 8
#define UART_S3_TX_PIN 4  // moved off GPIO17 (2026-09-19) as part of a real hardware A/B test with a new short, direct cable -- see progress/STATUS.md
#define UART_BAUD LINK_BAUD  // 921600 by default, 2000000 with ENCODE_ON_S3 (common/link_protocol.h)
HardwareSerial LinkSerial(1);  // UART1

// REAL ROOT CAUSE, finally confirmed (2026-09-19, first live test of the
// return channel): a heartbeat sent via LinkSerial's own TX pin never
// arrived intact on the classic side. Several wrong theories were chased
// first (RF/EMI noise from the classic's own Bluetooth radio, adjacent-
// module-pin crosstalk on the S3, wire routing) before the real cause
// surfaced: the physical wire was connected to the S3 board's own
// silkscreen-labeled "TX" pin -- the native USB/programming-console UART,
// not a general-purpose GPIO at all -- so the classic was receiving the
// S3's own continuous debug-log output (`[s3] link heartbeat...` etc. at
// 115200 baud) at a mismatched baud rate, which looks exactly like noise.
// The SAME class of mistake as this project's earlier UART_S3_RX_PIN
// mixup (silkscreen "RX" was also not a usable GPIO). Fixed by moving to
// a genuine GPIO (4) with nothing else on it. Confirmed clean, 100%
// reliable reception once wired correctly -- kept at a modest 9600 baud
// (plenty for this low-bandwidth status channel) rather than reverting to
// full speed, no real need to.
#define RETURN_TX_BAUD 9600
HardwareSerial ReturnTxSerial(2);  // UART2, dedicated, independent baud from LinkSerial

// Onboard addressable RGB LED, requested by Muni for at-a-glance link/audio
// status without a laptop attached. GPIO48 is the standard onboard-WS2812
// pin on the official Espressif ESP32-S3-DevKitC-1 (both v1 and v1.1
// silkscreens) -- **not yet hardware-verified against the actual board in
// hand**, same open-verification status UART_S3_RX_PIN had before real
// testing corrected it from 18 to 8. If the LED doesn't light, check this
// pin against the board's own silkscreen/schematic before assuming
// anything else is wrong.
#define RGB_LED_PIN 48
#define RGB_LED_COUNT 1
Adafruit_NeoPixel status_led(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// FAT12 ring-buffer disk logic (constants, build_boot_sector/build_fat/
// build_root_dir, disk_append, disk_valid_bytes, disk_read_at) now lives
// in fat_disk_shared.h, included above -- shared verbatim with
// sim/s3_real_firmware_host.cpp (the PC-hosted stand-in running until the
// physical board arrives) so both compile from the exact same source
// instead of two hand-copies that could silently drift out of sync (the
// same class of bug already caught once tonight: a fix applied to one
// call site not propagated to another).

// ===================== USBMSC glue ==========================================

USBMSC MSC;

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t abs_pos = lba * SECTOR_SIZE + offset;
  disk_read_at(abs_pos, (uint8_t *)buffer, bufsize);
  return bufsize;
}

// Set when a next/prev is relayed; loop()'s LED block blinks the current
// color off/on until then (Muni's request, 2026-09-24: "make the s3 light
// flash when we try to go to the next song").
static volatile uint32_t g_cmd_flash_until_ms = 0;

#ifdef FATDISK_MULTI_FILE
// PLAN_NEXT.md C3: fired by fat_disk_shared.h's fatdisk_note_file_read()
// (called from disk_read_at() above, i.e. from msc_on_read()'s own task
// context -- a normal TinyUSB task, not an ISR, so a direct blocking
// ReturnTxSerial call here is safe, same as the plain loop()-driven S3_HB
// heartbeat below) once a real, debounced file-range switch is detected --
// a physical proxy for the driver pressing next/previous on the radio
// itself. Relayed to the classic over the SAME existing S3->classic return
// channel already used for the S3_HB heartbeat, same plain line-based text
// convention (see that call's own comment for the wiring/protocol history).
static void on_file_switch_detected(int direction) {
  ReturnTxSerial.printf("RADIO_CMD:%s\n", direction > 0 ? "next" : "prev");
  FATDISK_TRACE(RELAY, direction > 0 ? 1 : 0, 0, 0);
  g_cmd_flash_until_ms = millis() + 600;
}
#endif

static int32_t msc_on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  // Read-only volume from the host's perspective -- a real car radio never
  // writes to the file it's playing. Silently accept-and-discard rather
  // than reject, since some hosts probe writability at mount time and a
  // hard failure there could be more disruptive than a no-op write.
  (void)lba; (void)offset; (void)buffer;
  return bufsize;
}

static bool msc_on_start_stop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition; (void)start; (void)load_eject;
  return true;
}

// ===================== UART link to the classic ESP32 ======================

// Mirrors s3_sim_serial.py's find_sync()/receive_from_esp32() framing
// exactly: 1 magic byte (0xAA), 1 type byte ('A' or 'C'), 4-byte
// big-endian length, then payload. Runs as its own FreeRTOS task so it
// can block on UART reads without affecting USB responsiveness.
// WRITE_RETRY_DELAY_MS/MAX_WRITE_RETRIES come from fat_disk_shared.h.

// TEMP DEBUG COUNTERS -- added to diagnose whether any bytes are arriving
// over the wire at all during initial bring-up. Remove once real audio is
// confirmed flowing end-to-end.
volatile uint32_t g_link_bytes_read = 0;
volatile uint32_t g_link_frames_ok = 0;
volatile uint32_t g_link_frames_bad = 0;
// LinkSerial receive errors by hardwareSerial_error_t (break, buffer full,
// FIFO overflow, framing, parity), for the heartbeat: tells a byte lost to
// the S3 falling behind (full/overflow) from one lost on the wire (framing).
static volatile uint32_t g_uart_errs[6] = {0};

// RGB status LED state, updated from link_task below. g_link_last_frame_ms
// (ANY frame, 'A' or 'C') is "are we hearing from the classic ESP32 at
// all" -- loop() treats a long gap here as not-connected. g_s3_audio_live
// tracks the AUDIO_LIVE/AUDIO_SILENCE control messages the classic now
// sends on each transition (see esp32-bt-mp3-test.ino's loop(), the block
// right after LED2's own status logic) -- 'A' frames alone can't carry
// this distinction, since injected silence goes through the exact same
// encode->send_framed('A',...) path real audio uses.
// g_link_last_frame_ms moved to fat_disk_shared.h (PLAN_NEXT.md B1,
// 2026-09-20) so disk_read_at() can consult it directly for staleness
// protection -- still updated here in link_task() exactly as before.
volatile bool g_s3_audio_live = false;
// Muni's request (2026-09-21): distinguish "paired but silent/paused" from
// "not paired at all" -- both currently show as identical LED states, since
// g_s3_audio_live alone can't tell them apart. The classic already sends
// BT_CONNECTED/BT_DISCONNECTED on every real A2DP connection-state change
// (esp32-bt-mp3-test.ino's connection_state_changed()) -- it was already
// arriving over the wire, just never parsed here. No classic-side change
// needed, only wiring up what link_task() does with a message that was
// already being sent.
volatile bool g_s3_bt_connected = false;
// Muni's follow-up request (2026-09-21): distinguish the FORWARD link
// (classic->S3, already covered by g_link_last_frame_ms/"RX missing" below)
// from the RETURN link (S3->classic) actually reaching its destination --
// two genuinely different hardware paths, either of which can fail on its
// own (a real, previously-root-caused wiring mistake on this exact return
// wire already happened once, see ReturnTxSerial's own comment). The full
// round-trip already existed for a different reason (the S3_HB heartbeat,
// sent every 2s below, that the classic's poll_return_serial() already
// echoes straight back as "S3_RX:S3_HB:..." on the ordinary forward 'C'
// channel) -- this just starts listening for that echo as a genuine
// "my own return-channel transmission got all the way there and back"
// confirmation, nothing new needed on the classic side.
volatile uint32_t g_return_ack_last_ms = 0;
// Muni's request (2026-09-22, bench testing): a distinct LED color for
// "the native USB-OTG port (the one presenting as USB-MSC to the car
// radio/PC) isn't connected right now." Only meaningful on the bench --
// in the real car install there's no separate debug-port power source, so
// losing this port means the whole board loses power and shows nothing at
// all (Muni's own reasoning); on the bench, the debug/programming USB port
// keeps the board alive and this LED visible even while the native port is
// separately unplugged, which is exactly the scenario this is for. Driven
// by ARDUINO_USB_STARTED_EVENT/ARDUINO_USB_STOPPED_EVENT below, which
// already fired (only to Serial.println, never reflected in the LED) --
// no new detection mechanism needed, just wiring up what already existed.
volatile bool g_native_usb_connected = false;
// Tried making the "now playing" rainbow react to a real audio-level
// signal (2026-09-21) -- built and flashed, but live-tested it didn't
// actually feel connected to the song in any meaningful way. Muni's call:
// drop it, keep the rainbow a plain, un-reactive animation. Removed
// (was `LED_SONG_COLOR`, `g_audio_level`, classic-side `LEVEL:` telemetry).

static bool read_exact(uint8_t *buf, uint32_t n) {
  uint32_t got = 0;
  while (got < n) {
    int avail = LinkSerial.available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int r = LinkSerial.read(buf + got, min((uint32_t)avail, n - got));
    if (r > 0) { got += r; g_link_bytes_read += r; }
  }
  return true;
}

static void find_sync() {
  uint8_t b;
  while (true) {
    read_exact(&b, 1);
    if (b == FRAME_MAGIC) return;
    // else: resync, exactly like s3_sim_serial.py's find_sync() -- no
    // need to log discarded junk on real hardware (no stderr to see it).
  }
}

// Appends whole MP3 frames to the ring (the 'A' path, and the S3 encoder's
// output with ENCODE_ON_S3). Same retry-then-force policy either way.
static void append_mp3(const uint8_t *data, uint32_t length) {
  for (uint32_t i = 0; i < MAX_WRITE_RETRIES; i++) {
    if (disk_append(data, length, true)) return;
    delay(WRITE_RETRY_DELAY_MS);
  }
  disk_append(data, length, false);  // force through, same fallback as s3_sim_serial.py
}

#ifdef ENCODE_ON_S3
class RingSink : public Print {
 public:
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    append_mp3(data, len);
    return len;
  }
};
static RingSink g_ring_sink;
static Mp3Pipeline g_s3_encoder(g_ring_sink);
// Encode cost per 'P' frame (~20ms of audio each), for the heartbeat.
static volatile uint32_t g_enc_us_sum = 0, g_enc_us_max = 0, g_enc_n = 0, g_pcm_bytes = 0;
// Status LED inputs for this mode: encoder started OK, and when PCM last arrived.
static volatile bool g_s3_encoder_ok = false;
static volatile uint32_t g_pcm_last_ms = 0;
#endif

static void link_task(void *) {
  static uint8_t payload[MAX_FRAME_LEN];
  while (true) {
    find_sync();
    uint8_t header[5];
    read_exact(header, 5);
    char frame_type = (char)header[0];
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                       ((uint32_t)header[3] << 8) | (uint32_t)header[4];

    bool known = frame_type == LINK_FRAME_MP3 || frame_type == LINK_FRAME_CONTROL;
#ifdef ENCODE_ON_S3
    known = known || frame_type == LINK_FRAME_PCM;
#endif
    if (!known || length > MAX_FRAME_LEN) {
      g_link_frames_bad++;
      static uint32_t last_bad_log_ms = 0;  // one line per burst, for matching against clicks
      if (millis() - last_bad_log_ms > 2000) {
        last_bad_log_ms = millis();
        Serial.printf("[s3] link resync: bad header type=0x%02x len=%lu (uart errs full=%lu ovf=%lu frame=%lu)\n",
                      (unsigned)(uint8_t)frame_type, (unsigned long)length, (unsigned long)g_uart_errs[2],
                      (unsigned long)g_uart_errs[3], (unsigned long)g_uart_errs[4]);
      }
      continue;  // bad header -- rescan from find_sync(), same as the Python side
    }
    if (length) read_exact(payload, length);
    g_link_last_frame_ms = millis();

    if (frame_type == LINK_FRAME_MP3) {
      append_mp3(payload, length);
#ifdef ENCODE_ON_S3
    } else if (frame_type == LINK_FRAME_PCM) {
      uint32_t t0 = micros();
      g_pcm_last_ms = millis();
      g_s3_encoder.write_mono(payload, length);
      uint32_t dt = micros() - t0;
      g_enc_us_sum += dt; g_enc_n++; g_pcm_bytes += length;
      if (dt > g_enc_us_max) g_enc_us_max = dt;
#endif
    } else {
      // 'C' (control/diagnostic messages from the classic ESP32). Most of
      // these (BT_CONNECTED/ENCODE_US/etc.) existed purely for the PC-side
      // simulator's own logging and are still ignored here -- except
      // AUDIO_LIVE/AUDIO_SILENCE, which now drive the RGB status LED (see
      // g_s3_audio_live above). Payload format is "millis|msg" (see
      // send_control() on the classic side); only the part after '|'
      // matters here.
      char *sep = (char *)memchr(payload, '|', length);
      const uint8_t *msg = sep ? (const uint8_t *)sep + 1 : payload;
      uint32_t msg_len = sep ? (length - ((uint8_t *)sep + 1 - payload)) : length;
      if (msg_len == 10 && memcmp(msg, "AUDIO_LIVE", 10) == 0) {
        g_s3_audio_live = true;
      } else if (msg_len == 13 && memcmp(msg, "AUDIO_SILENCE", 13) == 0) {
        g_s3_audio_live = false;
      } else if (msg_len == 12 && memcmp(msg, "BT_CONNECTED", 12) == 0) {
        g_s3_bt_connected = true;
      } else if (msg_len == 15 && memcmp(msg, "BT_DISCONNECTED", 15) == 0) {
        g_s3_bt_connected = false;
        g_s3_audio_live = false;  // can't be "live" with no peer connected
      } else if (msg_len >= 12 && memcmp(msg, "S3_RX:S3_HB:", 12) == 0) {
        // Our own return-channel heartbeat, echoed all the way back --
        // confirms the S3->classic wire is genuinely reaching its
        // destination, not just that the classic->S3 forward link is up.
        g_return_ack_last_ms = millis();
        // TRACK_CHANGED / TITLE: ignored. Song names were dropped after the
        // 2026-09-25 car test (Muni): the Kenwood reads a file's size and
        // cluster chain once, at open, so the S3 can't end a playing file
        // early, and a 56-char long name made it reject files ("unsupported
        // file"). The 3 files keep one fixed name, set at boot.
      }
    }
    g_link_frames_ok++;
  }
}

// ===================== setup/loop ===========================================

static void usb_event_callback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg; (void)event_data;
  if (event_base == ARDUINO_USB_EVENTS) {
    switch (event_id) {
      case ARDUINO_USB_STARTED_EVENT: Serial.println("[s3] USB PLUGGED"); g_native_usb_connected = true; FATDISK_TRACE(USB_STARTED, 0, 0, 0); break;
      case ARDUINO_USB_STOPPED_EVENT: Serial.println("[s3] USB UNPLUGGED"); g_native_usb_connected = false; FATDISK_TRACE(USB_STOPPED, 0, 0, 0); break;
      case ARDUINO_USB_SUSPEND_EVENT: FATDISK_TRACE(USB_SUSPEND, 0, 0, 0); break;
      case ARDUINO_USB_RESUME_EVENT: FATDISK_TRACE(USB_RESUME, 0, 0, 0); break;
      default: break;
    }
  }
}

#ifndef BUILD_GIT_SHA
#define BUILD_GIT_SHA 0
#define BUILD_GIT_DIRTY 1
#endif
// Which build this is (flash.sh passes the commit): flags compiled in.
// Printed at boot and once a minute, since a logger started after a flash
// misses the boot line.
static void print_build_line() {
  Serial.printf("[s3] build: commit %07lx%s flags:%s%s%s%s%s file=%lu x %lu KB clusters (%lu B) ring=%lu B\n", (unsigned long)BUILD_GIT_SHA,
                BUILD_GIT_DIRTY ? "+dirty" : "",
#ifdef FATDISK_ALWAYS_SERVE_LIVE
                " ALWAYS_SERVE_LIVE",
#else
                "",
#endif
#ifdef FATDISK_MULTI_FILE
                " MULTI_FILE",
#else
                "",
#endif
#ifdef LED_RAINBOW_PLAYING
                " LED_RAINBOW",
#else
                "",
#endif
#ifdef ENCODE_ON_S3
                " ENCODE_ON_S3",
#else
                "",
#endif
#ifdef MSC_TRACE
                " MSC_TRACE",
#else
                "",
#endif
                (unsigned long)DATA_CLUSTERS, (unsigned long)(CLUSTER_SIZE / 1024),
                (unsigned long)DECLARED_FILE_SIZE, (unsigned long)RING_SIZE);
}

void setup() {
#ifdef MSC_TRACE
  Serial.setTxBufferSize(16384);
  Serial.begin(MSC_TRACE_BAUD);  // the trace needs more than 115200 (see msc_trace.h)
#else
  Serial.begin(115200);  // USB CDC debug console (separate from the UART link)
#endif
  delay(200);
  Serial.println("[s3] booting");
  print_build_line();

  status_led.begin();
  status_led.setBrightness(80);  // full 255 is uncomfortably bright for a status LED at close range
  status_led.setPixelColor(0, 0, 0, 0);
  status_led.show();

#ifdef FATDISK_MULTI_FILE
  init_file_names();
#endif
  build_boot_sector();
  build_fat();
  build_root_dir();

  g_ring_mutex = xSemaphoreCreateMutex();
  g_ring = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
  if (!g_ring) {
    Serial.println("[s3] FATAL: PSRAM allocation for ring buffer failed -- "
                    "is PSRAM enabled in board settings?");
    while (true) delay(1000);
  }
  memset(g_ring, 0, RING_SIZE);
#ifdef ENCODE_ON_S3
  // Shine's working state is hot, table-heavy data: keep it in internal RAM.
  // With PSRAM enabled, malloc() otherwise sends allocations above the
  // (4KB) internal threshold to slower PSRAM. Internal has ~278KB free.
  heap_caps_malloc_extmem_enable(256 * 1024);
  bool enc_ok = g_s3_encoder.begin();
  g_s3_encoder_ok = enc_ok;
  Serial.printf("[s3] encoder (ENCODE_ON_S3): %s, int_free=%u\n", enc_ok ? "on" : "FAILED",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  // Prime the ring with pre-encoded silence up front -- same fix as
  // fat12_disk.py's __init__, same rationale (see CLAUDE.md/STATUS.md):
  // gets the radio's decoder real bytes to engage with immediately on
  // mount instead of waiting for real audio to physically accumulate.
  //
  // PLAN_NEXT.md item B2 (2026-09-19 design, implemented 2026-09-20): a
  // SINGLE primer append only covers ~1.26% of DECLARED_FILE_SIZE (~3s of
  // ~4 minutes) -- disk_valid_bytes() only reports THAT much as valid, so
  // any read past it fell through to the buffer's raw zero-fill default:
  // literal null bytes with NO MP3 framing at all, not silence-as-MP3. A
  // real car radio that scans ahead at mount time (building a duration
  // estimate, checking frame-sync consistency) would find ~3s of valid
  // frames followed by ~237s of raw zeros with no sync markers anywhere --
  // a strong, concrete match for Muni's real "invalid file" report right
  // after power-on, before any real audio has ever played.
  //
  // REAL BUG FOUND AND FIXED (2026-09-21, first real car test of this
  // fix): the loop below (disk_append() repeatedly until "full") DID NOT
  // actually guarantee full coverage as originally claimed -- confirmed
  // live: Muni's real car test STILL hit "N/A device" on first connect,
  // this exact symptom. Root cause: disk_valid_bytes() (== g_total_written)
  // counts BYTES APPENDED, not DISTINCT RING POSITIONS FILLED -- and
  // disk_append()'s own wrap-avoidance logic (correct and necessary for
  // the REAL-TIME writer, see its own big comment -- never split an MP3
  // frame across the wrap boundary) SKIPS STRAIGHT TO POSITION 0 instead
  // of partial-writing whenever a chunk would cross DECLARED_FILE_SIZE,
  // rather than filling the remaining tail. Since SILENCE_PRIMER_LEN
  // (48483) does not evenly divide DECLARED_FILE_SIZE (3842048), the old
  // loop always left a real, unwritten gap at the very tail of the ring
  // (here, exactly 11891 bytes / ~0.74s) as raw zero-fill from the memset
  // above -- while g_total_written incorrectly reported the ring as fully
  // primed once total BYTES WRITTEN reached DECLARED_FILE_SIZE, even
  // though those bytes didn't cover every physical position. This gap sits
  // right at the mount-time scan a real radio does immediately on connect
  // -- before any real audio could have looped around to overwrite it --
  // a strong, concrete explanation for the symptom surviving the first
  // "fix" attempt.
  //
  // Corrected fix: fill full-sized primer chunks up to (not across) the
  // ring boundary via disk_append() as before, then handle the exact
  // remaining tail directly (bypassing disk_append()'s wrap-avoidance
  // entirely for this one boot-time-only step) -- safe to do here since
  // this runs before link_task()/USB start, so no reader or concurrent
  // writer exists yet, no race to protect against. This DOES leave one
  // truncated/partial MP3 frame right at the physical wrap boundary
  // (DECLARED_FILE_SIZE-1 -> 0) -- but that's the exact same class of
  // minor, already-accepted glitch as the ring's own intentional,
  // once-per-lap wrap splice (see DATA_CLUSTERS's own history), not raw
  // zero-fill with zero sync headers across a real stretch of the file.
  while (g_write_pos + SILENCE_PRIMER_LEN <= RING_SIZE) {
    disk_append(SILENCE_PRIMER, SILENCE_PRIMER_LEN, false);
  }
  uint32_t primer_tail_remaining = RING_SIZE - g_write_pos;
  if (primer_tail_remaining > 0) {
    memcpy(g_ring + g_write_pos, SILENCE_PRIMER, primer_tail_remaining);
    g_write_pos = (g_write_pos + primer_tail_remaining) % RING_SIZE;
    g_total_written = RING_SIZE;
  }

  // Default HardwareSerial RX buffer (256B) is far too small at 921600
  // baud for ~100-180 audio-frame chunks/sec from the classic ESP32 --
  // must be set before begin(). 8KB gives real headroom (two full
  // MAX_FRAME_LEN frames) against link_task briefly falling behind
  // during a disk_append() retry stall.
#ifdef ENCODE_ON_S3
  LinkSerial.setRxBufferSize(16384);  // ~180ms of PCM at 88KB/s, covers encode jitter
#else
  LinkSerial.setRxBufferSize(8192);
#endif
  // TX=-1: GPIO17 no longer belongs to this UART instance -- see
  // ReturnTxSerial above for why (moved to its own dedicated, slower,
  // noise-resilient UART instance instead).
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, UART_S3_RX_PIN, -1);
  // Occasional click, root-caused 2026-09-25: the driver's default RX
  // "FIFO full" interrupt fires at 120 of the 128-byte hardware FIFO -- at
  // 2 Mbaud only ~40 us of headroom, so any interrupt latency beyond that
  // overflowed it (every link resync matched a UART_FIFO_OVF_ERROR, zero
  // framing errors). At 32 bytes the ISR has ~480 us.
  LinkSerial.setRxFIFOFull(32);
  LinkSerial.onReceiveError([](hardwareSerial_error_t e) {
    if ((unsigned)e < 6) g_uart_errs[e]++;
  });
  ReturnTxSerial.begin(RETURN_TX_BAUD, SERIAL_8N1, -1, UART_S3_TX_PIN);

  // REAL BUG FOUND (fresh adversarial review, 2026-09-17): link_task used
  // to be created AFTER MSC.begin()/USB.begin() below -- a real boot-time
  // data-loss window. The classic ESP32 starts transmitting continuous
  // silence-or-real-audio frames over UART from its own boot, independent
  // of BT state (feed_silence_if_no_real_audio()). If USB enumeration
  // (a real, variable-length handshake) takes longer than it takes to
  // fill the 8KB UART RX buffer at 921600 baud, incoming bytes get
  // silently dropped before link_task ever starts draining them.
  // Self-limiting (find_sync()'s resync logic already handles the
  // resulting misalignment gracefully, confirmed in the UART-framing
  // crosscheck), but still real, avoidable data loss on every boot.
  // Fix: start draining the UART immediately after LinkSerial.begin(),
  // before the slower USB enumeration work below.
  xTaskCreatePinnedToCore(link_task, "link_task", 8192, nullptr, 2, nullptr, 1);

#ifdef MSC_TRACE
  msc_trace_begin();
#endif
  USB.onEvent(usb_event_callback);
  MSC.vendorID("PHANTOMD");     // max 8 chars
  MSC.productID("USB_MSC");     // max 16 chars
  MSC.productRevision("1.0");   // max 4 chars
  MSC.onStartStop(msc_on_start_stop);
  MSC.onRead(msc_on_read);
  MSC.onWrite(msc_on_write);
#ifdef FATDISK_MULTI_FILE
  g_file_switch_callback = on_file_switch_detected;
#endif
  MSC.mediaPresent(true);
  MSC.isWritable(false);
  MSC.begin(TOTAL_SECTORS, SECTOR_SIZE);
  USB.begin();

  Serial.printf("[s3] FAT12 volume: %lu sectors, declared file size %lu bytes, "
                "first data LBA %lu\n",
                (unsigned long)TOTAL_SECTORS, (unsigned long)DECLARED_FILE_SIZE,
                (unsigned long)FIRST_DATA_LBA);
  Serial.println("[s3] ready");
}

// Auto re-attach (car test 2026-09-25, Muni: "the s3 needs to detect that
// and 'replug' automatically"). After an S3 reset with the radio's USB port
// still powered, the Kenwood sometimes never re-enumerates it (seen twice:
// no bus reset at all until the cable was replugged). If no host has
// configured us, drop the D+ pull-up and reconnect -- electrically what the
// host sees on an unplug/replug -- with a longer disconnect each try.
// Bench note: with only the debug port connected (no host on the native
// port) this keeps retrying every ~10 s, harmlessly.
extern "C" bool tud_disconnect(void);
extern "C" bool tud_connect(void);
static void usb_reattach_poll(uint32_t now_ms) {
  static const uint32_t FIRST_WAIT_MS = 4000, RETRY_EVERY_MS = 10000;
  static const uint32_t off_ms[] = {300, 1000, 3000};
  static uint32_t next_try_ms = FIRST_WAIT_MS, tries = 0, reconnect_at_ms = 0;
  static bool disconnected = false;
  if (disconnected) {
    if ((int32_t)(now_ms - reconnect_at_ms) >= 0) {
      tud_connect();
      disconnected = false;
      next_try_ms = now_ms + RETRY_EVERY_MS;
      Serial.println("[s3] usb re-attach: reconnected");
    }
    return;
  }
  if (g_native_usb_connected) {  // a host has us; re-arm for a later loss
    tries = 0;
    next_try_ms = now_ms + FIRST_WAIT_MS;
    return;
  }
  if ((int32_t)(now_ms - next_try_ms) < 0) return;
  uint32_t off = off_ms[tries < 3 ? tries : 2];
  tries++;
  Serial.printf("[s3] usb re-attach: no host, disconnect for %lu ms (try %lu)\n", (unsigned long)off, (unsigned long)tries);
  FATDISK_TRACE(USB_STOPPED, 1000 + tries, off, 0);
  tud_disconnect();
  disconnected = true;
  reconnect_at_ms = now_ms + off;
}

void loop() {
  // All real disk/USB work happens in link_task + USBMSC callbacks; loop()
  // only drives the RGB status LED and the debug heartbeat now. Runs every
  // 20ms (was 1000ms) so the audio-live pulse actually looks smooth instead
  // of stepping once a second.
  uint32_t now_ms = millis();
  usb_reattach_poll(now_ms);

  // RGB status LED (see status_led/RGB_LED_PIN above). Priority-ordered:
  // hardware/link-health states take precedence over BT/audio states,
  // since a wiring problem is more important to surface than "paired or
  // not." One fixed color per status (2026-09-22, Muni's request -- no
  // multi-color-cycling/rainbow effects, they read as ambiguous/confusing
  // at a glance; blinking on/off is fine, changing hue is not). This is
  // the authoritative status/color table -- keep it in sync with the
  // if/else chain immediately below whenever a state is added or changed.
  //
  // STANDING CONVENTION (2026-09-22, Muni): blink SPEED encodes severity,
  // fastest = worst -- WHITE/CYAN (250ms, catastrophic: no audio possible)
  // > ORANGE/YELLOW (500ms, real but minor: button-skip relay only) >
  // resync BLUE (600ms, NOT a fault at all -- always resolves on its own).
  // A fast blink must always mean a real fault; a genuinely non-fault
  // transient state must always blink slower than every fault state above
  // it. Apply this ordering to any new blinking state added later.
  //
  // Two INDEPENDENT links exist (classic<->S3 UART), asymmetric severity:
  // forward (classic->S3, carries the actual live audio ring) losing this
  // is CATASTROPHIC -- the S3 has nothing real to serve, the whole device's
  // job breaks -- so its states take top priority (WHITE/CYAN). Return
  // (S3->classic, only carries the physical-button track-skip relay under
  // FATDISK_MULTI_FILE) losing this is a real but MINOR degradation -- audio
  // itself keeps flowing fine, you just lose radio-button track skip -- so
  // its states (ORANGE/YELLOW) are checked only once the forward link is
  // confirmed healthy.
  //
  // IMPORTANT: the return-ack round trip (g_return_ack_last_ms) can ONLY
  // ever be confirmed via a frame arriving on the FORWARD link (classic
  // echoes "S3_RX:S3_HB:..." back over the same wire carrying audio) --
  // so whenever forward is down, return is unconditionally unconfirmed too.
  // The WHITE/CYAN states below are therefore always a "BOTH links down"
  // signal, i.e. the classic looks fully off/unreachable, not just "one
  // wire has an issue" -- confirmed live 2026-09-22 (Muni physically
  // unplugged the classic; got exactly this branch). Deliberately given
  // colors OUTSIDE the red/orange/yellow family used for the return-only,
  // single-link fault below (that one CAN'T mean "board is off," since it
  // only fires while forward is proven alive) -- so the two situations
  // can't be confused with each other at a glance.
  //
  //   STATE                          | COLOR             | MEANING
  //   -------------------------------|-------------------|---------------------------------------------
  //   native USB-OTG port disconnect | solid WHITE       | BENCH-ONLY diagnostic (2026-09-22, Muni): the
  //                                  |                   | native USB-OTG port (the one presenting as
  //                                  |                   | USB-MSC to the car radio/PC) has no host right
  //                                  |                   | now, even though the board itself is up (powered
  //                                  |                   | via the separate debug/programming port). Checked
  //                                  |                   | FIRST, ahead of every other state below -- if the
  //                                  |                   | host can't even see the drive, nothing else this
  //                                  |                   | LED reports matters. Can NEVER happen in the real
  //                                  |                   | car install (no debug-port power there -- losing
  //                                  |                   | this port means the whole board loses power and
  //                                  |                   | goes dark, not white).
  //   S3 encoder failed              | fast-blinking RED | ENCODE_ON_S3 only: the S3's own MP3 encoder
  //   (ENCODE_ON_S3 only)            | (250ms)           | didn't start -- no audio can reach the radio.
  //                                  |                   | NOTE for the two rows below: with ENCODE_ON_S3,
  //                                  |                   | "frames from the classic" means PCM ('P') frames
  //                                  |                   | specifically (the classic streams PCM nonstop
  //                                  |                   | while running); by default any frame counts.
  //   both links down: never linked  | blinking WHITE    | zero frames from the classic since S3 boot --
  //                                  |                   | consistent with "off/disconnected from the start"
  //   both links down: link lost     | blinking CYAN     | was receiving frames from the classic, now silent
  //                                  |                   | 2s+ -- consistent with a mid-session crash/reset/
  //                                  |                   | wire coming loose, or classic powered off WHILE
  //                                  |                   | running (distinct from "never linked" above)
  //   return only: never confirmed   | blinking ORANGE   | forward link is healthy (classic is alive and
  //   (FATDISK_MULTI_FILE only)      |                   | sending real audio), but the S3->classic
  //                                  |                   | return-channel ack has never been seen since
  //                                  |                   | S3 boot
  //   return only: confirmed->lost   | blinking YELLOW   | return-channel ack WAS being seen, now silent
  //   (FATDISK_MULTI_FILE only)      |                   | 5s+ -- same never-vs-lost distinction, mirrored
  //                                  |                   | onto the return link
  //   linked, not paired             | solid RED         | both links healthy, no phone currently paired
  //   resyncing (FATDISK_ALWAYS_      | slow-blinking     | live-serve cursor just jumped, bridging the
  //   SERVE_LIVE only)                | BLUE (600ms,      | discontinuity with silence (see
  //                                  | NOT an error)     |
  //                                  |                   | SILENCE_BRIDGE_BYTES in fat_disk_shared.h) --
  //                                  |                   | always brief, resolves on its own. Normal
  //                                  |                   | playback should almost never show this --
  //                                  |                   | if it does, it's currently also the expected
  //                                  |                   | signature of the bench GUI's own startup
  //                                  |                   | validity scan (unpaced, deliberately fast --
  //                                  |                   | confirmed live, 2026-09-22), not a real fault
  //   linked+paired, silent          | solid BLUE        | paired, but no real audio flowing (paused/idle --
  //                                  |                   | mirrors LED2's solid-on meaning on the classic)
  //   linked+paired, playing         | solid GREEN       | real audio is actually flowing right now
  //                                  | (or rainbow with  |
  //                                  | -DLED_RAINBOW_    |
  //                                  | PLAYING)          |
  //
  //   next/prev relayed to the phone | 3 WHITE flashes    | overlay on top of any state above, ~0.6s
  //
  // History: a hue-cycling "rainbow while playing" state existed 2026-09-21
  // through early 2026-09-22 (plain fixed-speed animation, never made
  // audio-reactive -- tried that, didn't feel meaningfully connected to the
  // song when live-tested, dropped), was removed 2026-09-22 per the "one
  // fixed color, no cycling" feedback above (replaced with solid GREEN),
  // then brought BACK the same night as an opt-in build flag
  // (LED_RAINBOW_PLAYING) after Muni missed it -- solid GREEN stays the
  // default; pass the flag to get rainbow instead.
  {
    // "Forward link" = is audio input from the classic arriving. By default any
    // frame proves it (the classic's encoder sleeps while idle, so MP3 frames
    // legitimately stop). With ENCODE_ON_S3 only PCM counts: the classic
    // streams PCM (real or injected silence) nonstop while running, so
    // control frames with no PCM still means nothing can reach the radio.
#ifdef ENCODE_ON_S3
    uint32_t link_ms = g_pcm_last_ms;
#else
    uint32_t link_ms = g_link_last_frame_ms;
#endif
    bool s3_connected = (link_ms != 0) && (now_ms - link_ms < 2000);
    // REAL FEEDBACK (2026-09-21, Muni): the link being one-way is
    // legitimately fine depending on what's actually built -- the return
    // channel (S3->classic) only exists to serve RADIO_CMD_RELAY/C3's
    // physical next/prev button relay (FATDISK_MULTI_FILE on this side).
    // With that feature not compiled in, a dead/absent return path is
    // completely harmless, not a fault -- only surface it as a problem
    // when something actually depends on it.
#ifdef FATDISK_MULTI_FILE
    bool return_seen_ever = (g_return_ack_last_ms != 0);
    bool return_confirmed = return_seen_ever && (now_ms - g_return_ack_last_ms < 5000);
#else
    bool return_seen_ever = true;
    bool return_confirmed = true;
#endif
    if (!g_native_usb_connected) {
      // Solid, non-blinking WHITE (2026-09-22, Muni: tried magenta first,
      // rejected on sight -- "not good"). Blinking WHITE already exists
      // below (forward-link-never-linked) -- SOLID vs blinking is already
      // an established distinguishing axis elsewhere in this exact palette
      // (solid BLUE vs slow-blinking BLUE), so reusing white as a solid
      // variant here stays consistent with that convention instead of
      // introducing a genuinely new hue every time a new state is added.
      status_led.setPixelColor(0, 255, 255, 255);  // solid white -- native USB-OTG (car radio/PC) port disconnected
#ifdef ENCODE_ON_S3
    } else if (!g_s3_encoder_ok) {
      // The S3 runs the encoder in this mode; if it failed to start, nothing
      // can reach the radio at all. Fast red: a fault, and unlike solid red
      // ("not paired") it blinks.
      bool on = ((now_ms / 250) % 2) == 0;
      status_led.setPixelColor(0, on ? 255 : 0, 0, 0);  // fast-blinking red -- S3 encoder failed to start
#endif
    } else if (!s3_connected) {
      // Muni's real-world observation (2026-09-22): the classic being
      // fully powered off and one specific wire coming loose while the
      // classic keeps running both look IDENTICAL from here -- either way
      // the S3 simply receives nothing. That specific distinction needs a
      // genuinely separate hardware signal (e.g. sensing the classic's own
      // power rail on a spare GPIO) that doesn't exist yet. What IS real
      // and detectable with what's already here: whether the S3 has EVER
      // heard from the classic since its own boot (g_link_last_frame_ms
      // still 0 -- consistent with "off/disconnected from the start") vs.
      // it was genuinely talking to the classic and then went quiet
      // (consistent with a mid-session crash, power loss, or a wire
      // coming loose WHILE running) -- two different real situations that
      // used to look identical (both just "blinking red").
      //
      // Colors deliberately NOT in the red/orange/yellow family used below
      // for return-link-only faults (2026-09-22, Muni: live-unplugged the
      // classic to test and got blinking MAGENTA -- too close to that other
      // family to read as "the whole board is dark," which is genuinely
      // what this branch always means: the return-ack channel physically
      // CANNOT be confirmed without forward frames carrying it, so whenever
      // this branch is taken, return is unconfirmed too, always -- this IS
      // the "both links down" case, not just "one link down"). WHITE/CYAN
      // instead, unmistakably distinct from every other state's palette.
      bool on = ((now_ms / 250) % 2) == 0;
      if (link_ms == 0) {
        status_led.setPixelColor(0, on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);  // blinking white -- never heard from it at all
      } else {
        status_led.setPixelColor(0, 0, on ? 255 : 0, on ? 255 : 0);  // blinking cyan -- was talking, now silent
      }
    } else if (!return_confirmed) {
      // Same never-vs-lost distinction as the forward link above, mirrored
      // onto the return (S3->classic) channel (2026-09-22, Muni: "we have
      // two way link... one is bad if it happens, the other completely
      // breaks the functionality" -- forward-link loss is the RED/MAGENTA
      // pair above and takes priority since it kills audio entirely; this
      // return-link pair is checked next since losing it only breaks the
      // physical-button track-skip relay, audio itself keeps working).
      bool on = ((now_ms / 500) % 2) == 0;
      if (!return_seen_ever) {
        status_led.setPixelColor(0, on ? 255 : 0, on ? 140 : 0, 0);  // blinking orange -- never confirmed since boot
      } else {
        status_led.setPixelColor(0, on ? 255 : 0, on ? 255 : 0, 0);  // blinking yellow -- was confirmed, now lost
      }
    } else if (!g_s3_bt_connected) {
      status_led.setPixelColor(0, 255, 0, 0);  // solid red -- not paired
#ifdef FATDISK_ALWAYS_SERVE_LIVE
    } else if (g_silence_bridge_remaining > 0) {
      // Resyncing (2026-09-22, Muni's request): the live-serve cursor just
      // jumped (see SILENCE_BRIDGE_BYTES's own comment in
      // fat_disk_shared.h) and is bridging the discontinuity with clean
      // silence instead of handing the reader a torn frame boundary.
      // g_silence_bridge_remaining is shared straight from that header
      // (no new variable needed). Originally a red+blue "violet" -- Muni
      // correctly read that as an error/fault color at a glance (visually
      // close to magenta/pink, easy to mistake for something broken).
      // Slow-blinking BLUE instead: thematically tied to the other
      // audio-state color (solid BLUE = paired+silent). Standing
      // convention as of 2026-09-22 (Muni): FAST blink means a real fault
      // (WHITE/CYAN/ORANGE/YELLOW above), SLOW blink means something
      // transient/non-fault -- this state is genuinely never an error (a
      // resync bridge always resolves on its own), so it must read slow.
      bool on = ((now_ms / 600) % 2) == 0;
      status_led.setPixelColor(0, 0, 0, on ? 255 : 0);  // slow-blinking blue -- resyncing (not an error)
#endif
    } else if (g_s3_audio_live) {
      // Default is solid GREEN (2026-09-22, Muni: "no multi-color-cycling,
      // pick a color"). Muni later asked for the rainbow back specifically
      // as an opt-in -- LED_RAINBOW_PLAYING makes it a real build flag
      // instead of picking one or the other permanently.
#ifdef LED_RAINBOW_PLAYING
      uint16_t hue = (uint16_t)(((uint32_t)now_ms * 65536UL / 4000UL) & 0xFFFF);
      status_led.setPixelColor(0, status_led.gamma32(status_led.ColorHSV(hue, 255, 255)));
#else
      status_led.setPixelColor(0, 0, 255, 0);  // solid green -- playing
#endif
    } else {
      status_led.setPixelColor(0, 0, 0, 255);  // solid blue -- paired, silent
    }
    // Next/prev relayed to the phone: 3 bright white flashes over ~0.6s,
    // overriding the state color (Muni: "flash a color or smth" -- a dim
    // blink of the current color was hard to see against the rainbow).
    if ((int32_t)(g_cmd_flash_until_ms - now_ms) > 0) {
      bool on = ((now_ms / 100) % 2) == 0;
      status_led.setPixelColor(0, on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);
    }
    status_led.show();
  }

  // TEMP DEBUG heartbeat -- see g_link_* counters above. Remove once real
  // audio is confirmed flowing end-to-end. Still gated to 1s even though
  // loop() itself now runs every 20ms.
  static uint32_t last_heartbeat_ms = 0;
  if (now_ms - last_heartbeat_ms >= 1000) {
    last_heartbeat_ms = now_ms;
    static uint32_t heartbeat_n = 0;
    if (++heartbeat_n % 60 == 0) print_build_line();
    Serial.printf("[s3] link heartbeat: bytes_read=%lu frames_ok=%lu frames_bad=%lu "
                  "write_pos=%lu total_written=%lu last_read_offset=%lu "
                  "diag_write_byte=%02x(n=%lu) diag_read_byte=%02x(n=%lu)\n",
                  (unsigned long)g_link_bytes_read, (unsigned long)g_link_frames_ok,
                  (unsigned long)g_link_frames_bad, (unsigned long)g_write_pos,
                  (unsigned long)g_total_written, (unsigned long)g_last_read_offset,
                  (unsigned)g_diag_write_byte, (unsigned long)g_diag_write_count,
                  (unsigned)g_diag_read_byte, (unsigned long)g_diag_read_count);
    Serial.printf("[s3] uart: brk=%lu full=%lu ovf=%lu frame=%lu parity=%lu\n",
                  (unsigned long)g_uart_errs[1], (unsigned long)g_uart_errs[2],
                  (unsigned long)g_uart_errs[3], (unsigned long)g_uart_errs[4], (unsigned long)g_uart_errs[5]);
    Serial.printf("[s3] mem: int_total=%u int_free=%u int_largest=%u int_min=%u psram_total=%u psram_free=%u psram_largest=%u\n",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#ifdef ENCODE_ON_S3
    {
      uint32_t n = g_enc_n, sum = g_enc_us_sum, mx = g_enc_us_max, pcm = g_pcm_bytes;
      g_enc_n = 0; g_enc_us_sum = 0; g_enc_us_max = 0; g_pcm_bytes = 0;
      Serial.printf("[s3] enc: frames=%lu pcm_bytes=%lu avg_us=%lu max_us=%lu\n",
                    (unsigned long)n, (unsigned long)pcm,
                    (unsigned long)(n ? sum / n : 0), (unsigned long)mx);
    }
#endif
#ifdef FATDISK_ALWAYS_SERVE_LIVE
    Serial.printf("[s3] live: underruns=%lu opens=%lu probes=%lu lap_end=%lu cursor=%lu\n",
                  (unsigned long)g_live_underruns, (unsigned long)g_live_opens,
                  (unsigned long)g_live_probe_reads, (unsigned long)g_ring_lap_end,
                  (unsigned long)g_live_read_cursor);
#endif
  }

  // Return channel: GPIO4 (physical wire to classic ESP32's GPIO19) --
  // see ReturnTxSerial's own comment above for the full story of how the
  // original wiring mistake (S3's silkscreen "TX" pin, not a real GPIO)
  // was root-caused. Confirmed clean, 100% reliable, one send per
  // interval is all that's needed. Plain line-based text, matching what
  // the classic side expects on this channel.
  static uint32_t last_return_hb_ms = 0;
  static uint32_t s3_hb_counter = 0;
  if (now_ms - last_return_hb_ms >= 2000) {
    last_return_hb_ms = now_ms;
    size_t sent = ReturnTxSerial.printf("S3_HB:%lu,write_pos=%lu\n",
                       (unsigned long)(s3_hb_counter++), (unsigned long)g_write_pos);
    Serial.printf("[s3] DIAG return-send attempted, bytes_sent=%u\n", (unsigned)sent);
  }

  delay(20);
}

#endif  // ARDUINO_USB_MODE
