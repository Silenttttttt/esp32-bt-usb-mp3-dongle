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

// UART pins to the classic ESP32's TX (its Serial/UART0 TX pin, wired
// directly -- see CLAUDE.md's real end-state architecture diagram).
// *** NOT YET HARDWARE-VERIFIED *** -- placeholder pin, pick a real
// UART-capable GPIO once the board's actual wiring is decided; these are
// just commonly-free pins on typical ESP32-S3-WROOM-1 DevKitC-1 boards.
//
// Real target board confirmed 2026-09-17 (Muni sent the actual purchase
// listing): ESP32-S3-WROOM-1 N16R8 DevKitC-1 -- 16MB Quad flash + 8MB
// Octal PSRAM. This matters for pin choice: GPIO 26-32 are dedicated to
// the Quad flash on ANY ESP32-S3 module, and Octal PSRAM (this module
// has it -- confirms PSRAM=opi below is the right build flag, not a
// guess) additionally dedicates GPIO 33-37 (Espressif's own
// ESP32-S3-WROOM-1 datasheet: "not recommended for other uses" on
// octal-PSRAM variants). GPIO 17/18 below are well outside that reserved
// 26-37 range, so no conflict -- but if this pin choice changes once the
// real wiring is decided, stay outside 26-37 on this specific module.
#define UART_S3_RX_PIN 18
#define UART_S3_TX_PIN 17  // unused (classic ESP32 -> S3 is one-way), kept for symmetry
#define UART_BAUD 921600
HardwareSerial LinkSerial(1);  // UART1

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

static bool read_exact(uint8_t *buf, uint32_t n) {
  uint32_t got = 0;
  while (got < n) {
    int avail = LinkSerial.available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int r = LinkSerial.read(buf + got, min((uint32_t)avail, n - got));
    if (r > 0) got += r;
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
      continue;  // bad header -- rescan from find_sync(), same as the Python side
    }
    if (length) read_exact(payload, length);

    if (frame_type == 'A') {
      bool ok = false;
      for (uint32_t i = 0; i < MAX_WRITE_RETRIES; i++) {
        if (disk_append(payload, length, true)) { ok = true; break; }
        delay(WRITE_RETRY_DELAY_MS);
      }
      if (!ok) disk_append(payload, length, false);  // force through, same fallback as s3_sim_serial.py
    }
    // 'C' (control/diagnostic messages from the classic ESP32, e.g.
    // BT_CONNECTED/ENCODE_US/etc.) -- nothing on this side needs to act
    // on them; they existed purely for the PC-side simulator's own
    // logging. Silently discarded here.
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
  delay(1000);  // all real work happens in link_task + USBMSC callbacks
}

#endif  // ARDUINO_USB_MODE
