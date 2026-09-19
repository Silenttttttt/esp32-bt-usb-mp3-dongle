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
#include "fat_disk_shared.h"
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
#define UART_S3_TX_PIN 17  // unused (classic ESP32 -> S3 is one-way), kept for symmetry
#define UART_BAUD 921600
HardwareSerial LinkSerial(1);  // UART1

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

// RGB status LED state, updated from link_task below. g_link_last_frame_ms
// (ANY frame, 'A' or 'C') is "are we hearing from the classic ESP32 at
// all" -- loop() treats a long gap here as not-connected. g_s3_audio_live
// tracks the AUDIO_LIVE/AUDIO_SILENCE control messages the classic now
// sends on each transition (see esp32-bt-mp3-test.ino's loop(), the block
// right after LED2's own status logic) -- 'A' frames alone can't carry
// this distinction, since injected silence goes through the exact same
// encode->send_framed('A',...) path real audio uses.
volatile uint32_t g_link_last_frame_ms = 0;
volatile bool g_s3_audio_live = false;

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

static void link_task(void *) {
  static uint8_t payload[MAX_FRAME_LEN];
  while (true) {
    find_sync();
    uint8_t header[5];
    read_exact(header, 5);
    char frame_type = (char)header[0];
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                       ((uint32_t)header[3] << 8) | (uint32_t)header[4];

    if ((frame_type != 'A' && frame_type != 'C') || length > MAX_FRAME_LEN) {
      g_link_frames_bad++;
      continue;  // bad header -- rescan from find_sync(), same as the Python side
    }
    if (length) read_exact(payload, length);
    g_link_last_frame_ms = millis();

    if (frame_type == 'A') {
      bool ok = false;
      for (uint32_t i = 0; i < MAX_WRITE_RETRIES; i++) {
        if (disk_append(payload, length, true)) { ok = true; break; }
        delay(WRITE_RETRY_DELAY_MS);
      }
      if (!ok) disk_append(payload, length, false);  // force through, same fallback as s3_sim_serial.py
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
      case ARDUINO_USB_STARTED_EVENT: Serial.println("[s3] USB PLUGGED"); break;
      case ARDUINO_USB_STOPPED_EVENT: Serial.println("[s3] USB UNPLUGGED"); break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);  // USB CDC debug console (separate from the UART link)
  delay(200);
  Serial.println("[s3] booting");

  status_led.begin();
  status_led.setBrightness(80);  // full 255 is uncomfortably bright for a status LED at close range
  status_led.setPixelColor(0, 0, 0, 0);
  status_led.show();

  build_boot_sector();
  build_fat();
  build_root_dir();

  g_ring_mutex = xSemaphoreCreateMutex();
  g_ring = (uint8_t *)heap_caps_malloc(DECLARED_FILE_SIZE, MALLOC_CAP_SPIRAM);
  if (!g_ring) {
    Serial.println("[s3] FATAL: PSRAM allocation for ring buffer failed -- "
                    "is PSRAM enabled in board settings?");
    while (true) delay(1000);
  }
  memset(g_ring, 0, DECLARED_FILE_SIZE);
  // Prime the ring with pre-encoded silence up front -- same fix as
  // fat12_disk.py's __init__, same rationale (see CLAUDE.md/STATUS.md):
  // gets the radio's decoder real bytes to engage with immediately on
  // mount instead of waiting for real audio to physically accumulate.
  disk_append(SILENCE_PRIMER, SILENCE_PRIMER_LEN, false);

  // Default HardwareSerial RX buffer (256B) is far too small at 921600
  // baud for ~100-180 audio-frame chunks/sec from the classic ESP32 --
  // must be set before begin(). 8KB gives real headroom (two full
  // MAX_FRAME_LEN frames) against link_task briefly falling behind
  // during a disk_append() retry stall.
  LinkSerial.setRxBufferSize(8192);
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, UART_S3_RX_PIN, UART_S3_TX_PIN);

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

  USB.onEvent(usb_event_callback);
  MSC.vendorID("PHANTOMD");     // max 8 chars
  MSC.productID("USB_MSC");     // max 16 chars
  MSC.productRevision("1.0");   // max 4 chars
  MSC.onStartStop(msc_on_start_stop);
  MSC.onRead(msc_on_read);
  MSC.onWrite(msc_on_write);
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

void loop() {
  // All real disk/USB work happens in link_task + USBMSC callbacks; loop()
  // only drives the RGB status LED and the debug heartbeat now. Runs every
  // 20ms (was 1000ms) so the audio-live pulse actually looks smooth instead
  // of stepping once a second.
  uint32_t now_ms = millis();

  // RGB status LED (see status_led/RGB_LED_PIN above). Off when no frame
  // (from either the UART link or a control message) has arrived from the
  // classic ESP32 in the last 2s -- covers "not wired up" and "classic
  // reset/UART link dropped", not just BT state. Solid blue when linked
  // but currently in silence (paused/no real audio -- mirrors LED2's own
  // solid-on meaning on the classic side). Breathing green when real
  // audio is actually flowing.
  {
    bool s3_connected = (g_link_last_frame_ms != 0) && (now_ms - g_link_last_frame_ms < 2000);
    if (!s3_connected) {
      status_led.setPixelColor(0, 0, 0, 0);
    } else if (g_s3_audio_live) {
      // ~1.5s breathing period, floor at 15% so it never fully blacks out.
      float phase = fmodf((float)now_ms, 1500.0f) / 1500.0f;
      float pulse = 0.15f + 0.85f * (0.5f + 0.5f * sinf(phase * 2.0f * (float)M_PI));
      status_led.setPixelColor(0, 0, (uint8_t)(255 * pulse), 0);  // green
    } else {
      status_led.setPixelColor(0, 0, 0, 255);  // solid blue
    }
    status_led.show();
  }

  // TEMP DEBUG heartbeat -- see g_link_* counters above. Remove once real
  // audio is confirmed flowing end-to-end. Still gated to 1s even though
  // loop() itself now runs every 20ms.
  static uint32_t last_heartbeat_ms = 0;
  if (now_ms - last_heartbeat_ms >= 1000) {
    last_heartbeat_ms = now_ms;
    Serial.printf("[s3] link heartbeat: bytes_read=%lu frames_ok=%lu frames_bad=%lu "
                  "write_pos=%lu total_written=%lu last_read_offset=%lu "
                  "diag_write_byte=%02x(n=%lu) diag_read_byte=%02x(n=%lu)\n",
                  (unsigned long)g_link_bytes_read, (unsigned long)g_link_frames_ok,
                  (unsigned long)g_link_frames_bad, (unsigned long)g_write_pos,
                  (unsigned long)g_total_written, (unsigned long)g_last_read_offset,
                  (unsigned)g_diag_write_byte, (unsigned long)g_diag_write_count,
                  (unsigned)g_diag_read_byte, (unsigned long)g_diag_read_count);
  }
  delay(20);
}

#endif  // ARDUINO_USB_MODE
