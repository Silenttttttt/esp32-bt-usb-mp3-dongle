/* Real-hardware BT-sink + MP3 encoder, streaming to the simulated S3 over
 * the existing USB-serial link (standing in for the eventual wired UART
 * link to the real S3 -- WiFi was tried first but confirmed, by direct
 * heap measurement, to consume ~53KB on its own, which starved Bluetooth's
 * own init on this plain ESP32 WROOM (no PSRAM). Serial has no such cost:
 * it's already active for debug output and needs no extra radio stack.
 *
 * Uses the Shine MP3 encoder (via arduino-audio-tools), not LAME: LAME's
 * internal init needs ~170KB of contiguous heap, confirmed by direct
 * measurement to exceed what's available here. Shine (~108KB) fits with
 * plenty of margin left for Bluetooth.
 *
 * Wire protocol over Serial (921600 baud) to s3_sim_serial.py: 1-byte
 * magic sync (0xAA) + 1-byte type + 4-byte big-endian length + payload:
 *   type 'A' = MP3 audio bytes
 *   type 'C' = control/status event (text), for real play/pause/track
 *              logging and delay measurement
 * No other Serial output is produced once streaming starts, to keep the
 * link unambiguous -- boot/setup messages use a distinct prefix and only
 * happen before the protocol goes live.
 */

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Shine.h"
#include "../common/mp3_pipeline.h"  // shared encoder + link baud (ENCODE_ON_S3)
#include "BluetoothA2DPSink.h"
#include "esp_gap_bt_api.h"

// PLAN_NEXT.md item D (parameterization, added 2026-09-19, implemented
// 2026-09-20): Muni's explicit standing requirement -- build/run this
// exactly the bare way (a few flags, same as every prior real test) OR
// build a "v2" full version with every A/C-series integration on, without
// remembering a dozen individual -D flags every time. -DV2_ALL turns on
// every individual sub-flag below at once; the bare/default build stays
// LITERALLY unchanged (same recipe as CLAUDE.md's own documented command).
// Every sub-flag ALSO still works completely independently for
// fine-grained control -- this umbrella doesn't replace that, only adds a
// single-token shortcut on top of it.
//
// NOTE, not solvable via preprocessor alone: AVRCP itself is enabled by
// simply NOT passing -DA2DP_DISABLE_AVRC to the build command (that flag
// is consumed by the vendored ESP32-A2DP library, a SEPARATE translation
// unit -- a #undef here has no effect on how that file was already
// compiled). None of the flags below do anything unless the actual build
// command also omits -DA2DP_DISABLE_AVRC -- see CLAUDE.md's "Build/flash
// commands" section for the two full, exact recipes side by side.
#ifdef V2_ALL
  #ifndef AVRC_AUTO_RESUME_ON_RECONNECT
    #define AVRC_AUTO_RESUME_ON_RECONNECT
  #endif
  #ifndef AVRC_AUTO_RESUME_EARLY_PAUSE
    #define AVRC_AUTO_RESUME_EARLY_PAUSE
  #endif
  #ifndef AVRC_AUTO_SKIP_NEAR_END
    #define AVRC_AUTO_SKIP_NEAR_END
  #endif
  #ifndef AVRC_TRACK_POSITION
    #define AVRC_TRACK_POSITION
  #endif
  #ifndef RADIO_CMD_RELAY
    #define RADIO_CMD_RELAY
  #endif
  #ifndef RADIO_TRACK_RENAME
    #define RADIO_TRACK_RENAME
  #endif
#endif  // V2_ALL

// A3 needs AVRC_TRACK_POSITION's play_pos callback firing to have anywhere
// to actually check the near-end condition -- a real, easy-to-silently-
// forget build-time dependency (PLAN_NEXT.md's own A3 section flagged
// this), so make missing it a compile error instead of a silently-inert
// feature.
#if defined(AVRC_AUTO_SKIP_NEAR_END) && !defined(AVRC_TRACK_POSITION)
  #error AVRC_AUTO_SKIP_NEAR_END requires AVRC_TRACK_POSITION also defined \
         (it needs avrc_play_pos_callback's periodic firing to check the \
         near-end condition against) -- add -DAVRC_TRACK_POSITION too, or \
         build with -DV2_ALL which already includes both.
#endif

BluetoothA2DPSink a2dp_sink;

// Forward declaration: defined further down, needed here since
// auto_command_cooldown_ok()/poll_return_serial()/handle_pc_command()
// (below) call it before that point in the file. The compiler's own
// auto-prototype generation doesn't reliably handle this function's
// default argument, so this is explicit rather than relied-upon.
void send_control(const char *msg, TickType_t wait_ticks = portMAX_DELAY);
// Forward declaration: connection_state_changed() (defined well before
// avrc_ct_wrapper_callback's own definition further down) needs to call
// esp_avrc_ct_register_callback(avrc_ct_wrapper_callback) -- see that
// function's own comment for why this specific re-registration point
// matters.
void avrc_ct_wrapper_callback(esp_avrc_ct_cb_event_t event,
                               esp_avrc_ct_cb_param_t *param);

// PLAN_NEXT.md section E (cross-feature interaction audit, 2026-09-19):
// with multiple independent mechanisms below each auto-firing an AVRCP
// command (A1's play() on reconnect, A2's play() on early pause, A3's
// next() near track end, RADIO_CMD_RELAY's next()/previous() relay from
// the radio's own physical buttons), a real risk exists when two of them
// fire within the same moment (e.g. a driver presses the radio's physical
// "next" button at nearly the same instant A3 independently decides the
// current track is within its own last 5-10s -- both would call next()
// independently, silently skipping TWO tracks instead of one). Declared
// unconditionally (a single uint32_t, negligible cost even when every
// feature below is disabled) rather than behind a fragile multi-flag
// #if-OR chain that would need updating every time a new auto-command
// feature is added.
volatile uint32_t g_last_auto_command_ms = 0;
#define AUTO_COMMAND_COOLDOWN_MS 1500
// Scoped per TRIGGER EVENT (one full reaction to one detected condition),
// not per individual a2dp_sink call -- REAL BUG CAUGHT during design (see
// PLAN_NEXT.md section E): RADIO_CMD_RELAY's handler needs next()/
// previous() THEN a conditional play() as ONE coordinated response to ONE
// trigger; checking/updating the cooldown around each call independently
// would make the play() call see its own preceding next() as "a command
// JUST fired" and incorrectly suppress itself. Callers check this ONCE at
// their own entry point, before deciding how many a2dp_sink calls to make.
static inline bool auto_command_cooldown_ok(const char *reason) {
  uint32_t now = millis();
  if (now - g_last_auto_command_ms < AUTO_COMMAND_COOLDOWN_MS) {
    char out[48];
    snprintf(out, sizeof(out), "CMD_SUPPRESSED:%s", reason);
    // REAL BUG FOUND AND FIXED (2026-09-20, live hardware testing): this
    // used the default portMAX_DELAY wait. This helper is called from BOTH
    // loop()-context code (RADIO_CMD_RELAY's handler in poll_return_serial())
    // AND directly from Bluedroid callback tasks (A1/A3's checks inside
    // avrc_playstatus_callback()/avrc_play_pos_callback()) -- blocking one
    // of those callback tasks indefinitely on serial_mutex is the exact,
    // already-documented crash class this file's other AVRCP callbacks all
    // deliberately bound to pdMS_TO_TICKS(20) for (see
    // connection_state_changed()'s own comment on this). This one call site
    // was missed. Bounded the same way, consistent with every other
    // send_control() call reachable from a Bluedroid task.
    send_control(out, pdMS_TO_TICKS(20));
    return false;
  }
  g_last_auto_command_ms = now;
  return true;
}

// New return channel: the S3's GPIO4 (TX, see esp32-s3-msc.ino's
// UART_S3_TX_PIN) now carries messages back to the classic ESP32,
// received here on a SEPARATE physical pin/UART peripheral (UART2) from
// the existing forward link (UART0/TX0/GPIO1 -> S3's GPIO8). Deliberately
// not the classic's own RX0 (GPIO3): that pin is the USB-serial bridge's
// own RX, needed free for flashing/PC console input, and using it here
// would contend with that. Plain line-based text, not the 0xAA-framed
// binary protocol (that framing exists to keep audio bytes and control
// text unambiguous on ONE shared wire; this is a distinct wire with
// nothing else on it, so no framing is needed).
//
// Root-cause history worth keeping: the FIRST wiring attempt used the
// S3's board-silkscreen-labeled "TX" pin, which is the S3's own native
// USB/programming-console UART, not a general-purpose GPIO at all -- so
// the classic was receiving the S3's own continuous debug-log output
// (`[s3] link heartbeat...` at 115200 baud) at a mismatched baud rate,
// which looked exactly like random noise (a raw GPIO edge-counter showed
// high activity on both the "connected" pin and a deliberately
// unconnected control pin, which -- combined with not yet suspecting the
// wiring itself -- pointed toward a false "classic's own BT radio causes
// RF noise" theory before the real cause was found). The SAME class of
// mistake as this project's earlier UART_S3_RX_PIN mixup (silkscreen
// "RX" also wasn't a usable GPIO). Fixed by wiring to a genuine GPIO (4
// on the S3) instead. Confirmed clean, 100% reliable reception once
// correctly wired -- kept at a modest 9600 baud (see ReturnSerial.begin()
// below), plenty for this low-bandwidth status channel.
#define RETURN_RX_PIN 19
HardwareSerial ReturnSerial(2);

#ifdef RADIO_CMD_RELAY
// PLAN_NEXT.md's C3 section, "added refinement" (2026-09-19): kept up to
// date directly in avrc_playstatus_callback() below -- a plain, always-
// current live state (not reused from A1's reconnect-resume flag, which
// deliberately does NOT persist across a reboot; this is a different,
// simpler case where BT stays connected the whole time).
volatile bool g_avrc_known_playing = false;
#endif

// Forwards whatever the S3 sends back onto the existing 'C' control
// channel (so it's visible via the same s3_sim_serial.py-style logging
// already used for BT_CONNECTED/AUDIO_CB_STATUS/etc.), prefixed so it's
// unambiguous which board originated it.
void poll_return_serial() {
  static char buf[128];
  static uint8_t len = 0;
  while (ReturnSerial.available()) {
    char c = (char)ReturnSerial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = 0;
#ifdef RADIO_CMD_RELAY
        // PLAN_NEXT.md's C3: the S3 detected a real, debounced file-switch
        // (the car radio's own physical next/prev button, via
        // FATDISK_MULTI_FILE's disjoint-file-range trick) and relayed it
        // here as a genuine driver-initiated command -- act on it, still
        // logged via the normal S3_RX: line below either way.
        if (strcmp(buf, "RADIO_CMD:next") == 0 || strcmp(buf, "RADIO_CMD:prev") == 0) {
          bool is_next = strcmp(buf, "RADIO_CMD:next") == 0;
          if (auto_command_cooldown_ok(is_next ? "radio_next" : "radio_prev")) {
            if (is_next) a2dp_sink.next(); else a2dp_sink.previous();
            // Pressing a physical next/prev button clearly signals intent
            // to keep listening -- resume too if playback was paused.
            if (!g_avrc_known_playing) a2dp_sink.play();
            // is_avrc_connected() mirrors the CT-side link state; when it's 0
            // the stack drops the passthrough silently, so CMD_SENT alone
            // proved nothing (2026-09-24).
            send_control(is_next ? (a2dp_sink.is_avrc_connected() ? "CMD_SENT:radio_next,avrc=1" : "CMD_SENT:radio_next,avrc=0")
                                 : (a2dp_sink.is_avrc_connected() ? "CMD_SENT:radio_prev,avrc=1" : "CMD_SENT:radio_prev,avrc=0"));
          }
        }
#endif
        char out[160];
        snprintf(out, sizeof(out), "S3_RX:%s", buf);
        send_control(out);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

#ifdef AVRC_INVESTIGATION
// TEMP, investigation-only (2026-09-19): a PC-typed command interface for
// testing what a2dp_sink can send BACK to the phone (next/prev/play/pause/
// volume), answering Muni's "what kind of commands can it send back"
// question. Reads from Serial's RX (RX0/GPIO3) -- genuinely free to use
// for this, since RX0 only ever carries bytes from the PC's own USB-serial
// bridge; it was never wired to the S3 (only TX0 goes there) and is
// otherwise completely idle in this firmware. Deliberately NOT triggered
// automatically/on a timer: firing next()/pause() unprompted during a real
// listening session would audibly interrupt real music, which defeats the
// point of testing -- one command per explicit PC keystroke instead.
void handle_pc_command(const char *cmd) {
  if (strcmp(cmd, "next") == 0) {
    a2dp_sink.next();
    send_control("CMD_SENT:next");
  } else if (strcmp(cmd, "prev") == 0) {
    a2dp_sink.previous();
    send_control("CMD_SENT:prev");
  } else if (strcmp(cmd, "play") == 0) {
    a2dp_sink.play();
    send_control("CMD_SENT:play");
  } else if (strcmp(cmd, "pause") == 0) {
    a2dp_sink.pause();
    send_control("CMD_SENT:pause");
  } else if (strncmp(cmd, "vol:", 4) == 0) {
    int v = atoi(cmd + 4);
    a2dp_sink.set_volume((uint8_t)v);
    char out[32];
    snprintf(out, sizeof(out), "CMD_SENT:vol=%d", v);
    send_control(out);
  } else {
    char out[64];
    snprintf(out, sizeof(out), "CMD_UNKNOWN:%s", cmd);
    send_control(out);
  }
}

void poll_pc_commands() {
  static char buf[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = 0;
        handle_pc_command(buf);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}
#endif  // AVRC_INVESTIGATION

// audio_data_callback (Bluedroid's own task) and the AVRCP callbacks
// (a separate Bluedroid task) both call send_framed()/send_control() on
// the same Serial link. Without this, an AVRCP callback can interleave a
// header/payload between an audio frame's own two Serial.write() calls,
// splicing two frames together on the wire -- exactly what our own logs
// showed (TITLE/ARTIST/ALBUM control lines landing mid-stream while audio
// was flowing). The receiver has no resync, so a spliced frame's garbage
// length field wedges it forever. One mutex around the whole header+
// payload write makes each frame atomic on the wire.
//
// Must be RECURSIVE: a rapid connect/disconnect flap (observed directly --
// bluetoothctl briefly showed "Connected: yes" immediately followed by a
// connection failure) can re-enter connection_state_changed() on the same
// Bluedroid callback task before its first invocation's send_control() call
// has returned and released the mutex. A plain (non-recursive) mutex
// deadlocks a task against itself in that case -- observed as a total,
// silent hang (zero serial output, no panic) reproducible from BT
// connection churn alone, with no audio ever flowing.
SemaphoreHandle_t serial_mutex;

// 1-byte magic sync marker in front of every frame, so the receiver can
// resync after any corrupted/spliced byte instead of trusting a garbage
// length field and hanging forever.
static const uint8_t FRAME_MAGIC = 0xAA;

// wait_ticks lets a caller bound how long it's willing to block for the
// mutex -- default portMAX_DELAY preserves the original guarantee for
// audio/AVRCP frames (an audio byte or metadata string must never be
// silently dropped, so those callers should keep waiting). Returns false
// if the mutex wasn't acquired within wait_ticks (nothing was written).
bool send_framed(char type, const uint8_t *data, size_t len,
                  TickType_t wait_ticks = portMAX_DELAY) {
  uint8_t header[6];
  header[0] = FRAME_MAGIC;
  header[1] = (uint8_t)type;
  header[2] = (len >> 24) & 0xFF;
  header[3] = (len >> 16) & 0xFF;
  header[4] = (len >> 8) & 0xFF;
  header[5] = len & 0xFF;
  if (xSemaphoreTakeRecursive(serial_mutex, wait_ticks) != pdTRUE) return false;
  Serial.write(header, 6);
  if (len) Serial.write(data, len);
  xSemaphoreGiveRecursive(serial_mutex);
  return true;
}

// REAL BUG FOUND (long-uptime negotiation slowdown investigation,
// 2026-09-17): this used to take a `const String&` and build "millis()|msg"
// via String concatenation -- and every single call site concatenated
// Strings too (e.g. "ENCODE_US:avg=" + String(avg_us) + ",max=" + ...).
// Arduino's String does a fresh heap alloc+free per concatenation, and
// ENCODE_US alone fires every ~1s for the ENTIRE session lifetime (plus
// PCM_DROPS, AVRC metadata, RESET_REASON, AUDIO_STATE on top) -- that's
// thousands of variable-sized alloc/free cycles per hour, a textbook
// heap-fragmentation generator. Measured tonight: `largest_block` capped
// well below `total_free` (13300 vs 19892 bytes) even at rest, consistent
// with fragmented heap from exactly this pattern. AVDTP stream negotiation
// (which allocates buffers) got measurably slower on a long-uptime session
// than right after a fresh boot -- this is the most likely real cause.
// Fix: no more String, anywhere in this path. Every call site now formats
// into its own local fixed-size char buffer via snprintf (stack, not
// heap), and send_control() takes const char* -- zero heap churn per
// control message, no matter how long the session runs.
void send_control(const char *msg, TickType_t wait_ticks) {
  char withTime[200];
  int n = snprintf(withTime, sizeof(withTime), "%lu|%s", millis(), msg);
  if (n > (int)sizeof(withTime) - 1) n = sizeof(withTime) - 1;
  send_framed('C', (const uint8_t *)withTime, n, wait_ticks);
}

// TEMP DEBUG -- checking whether the audio-tools library's write() calls
// are frame-aligned at all (128kbps/44.1kHz MP3 frames are ~417-418
// bytes). Remove once the real "Header missing" root cause is confirmed.
volatile uint32_t g_write_sizes[8] = {0};
volatile uint32_t g_write_count = 0;
volatile bool g_saw_ff_start = true;
volatile uint32_t g_non_ff_start_count = 0;

class SerialPrintSink : public Print {
 public:
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    if (len > 0 && data[0] != 0xFF) g_non_ff_start_count++;
    g_write_sizes[g_write_count % 8] = len;
    g_write_count++;
    send_framed('A', data, len);
    return len;
  }
};

SerialPrintSink serial_sink;
#ifndef ENCODE_ON_S3
Mp3Pipeline mp3_out(serial_sink);  // the shared encoder, output as 'A' frames
#endif

// Every path that hands mono PCM downstream goes through here: encoded on
// this board (default) or shipped raw to the S3 as 'P' frames (ENCODE_ON_S3).
static inline void emit_mono(const uint8_t *pcm, uint32_t len) {
#ifdef ENCODE_ON_S3
  send_framed(LINK_FRAME_PCM, pcm, len);
#else
  mp3_out.write_mono(pcm, len);
#endif
}

// audio_data_callback runs directly inside Bluedroid's own receive task. Shine
// encoding is too CPU-heavy to run there in real time on this chip: real audio
// consistently crashed ~2-4s in with a Bluedroid HCI packet-reassembly alloc
// failure (`assert failed: reassemble_and_dispatch packet_fragmenter.c:205`,
// confirmed via a captured panic backtrace), independent of AVRCP or heap
// size -- the signature of Bluetooth packets backing up because the task that's
// supposed to drain them is stuck encoding instead. Fix: hand raw PCM off to a
// queue immediately and do the actual encode+serial-write on a dedicated task
// on the other core, so Bluedroid's own task is never blocked.
//
// The queue itself must not call malloc/free per chunk: repeated variable-size
// alloc/free on this same heap is exactly the kind of churn that fragments it,
// and Bluedroid's own reassembly allocation (the thing that was crashing) has
// to compete with that fragmentation for its own contiguous block. A2DP PCM
// chunks arrive well under 4096 bytes in practice (SBC frame size), so a
// preallocated fixed-size ring of fixed-size slots needs zero heap traffic
// once created at boot -- allocate it all up front, then only ever memcpy
// into pre-existing slots.
// Static footprint = PCM_SLOT_SIZE * PCM_SLOT_COUNT, taken directly out of
// the same tight free-heap margin Bluedroid itself needs. Measured (via
// HEAP_TRACE's max_chunk_seen trace, mutex-protected so the print itself
// can't corrupt the wire) real max PCM chunk length is exactly 4096 bytes --
// an earlier guess of 1024 was silently truncating 75% of every audio
// chunk (a real corruption bug, not just a margin issue), while an earlier
// guess of 8 slots (32KB total) collapsed free heap from ~45-56KB down to
// ~19KB, low enough that even basic BT paging broke.
//
// REAL ROOT-CAUSE FIX (2026-09-22, Muni: "get to the bottom of it, fully
// address it, we dont want -DA2DP_DISABLE_AVRC"): live heap instrumentation
// (HEAP_TRACE, both ESP.getFreeHeap() and heap_caps_get_largest_free_block())
// during a real PC/BlueZ connection showed TOTAL free heap healthy
// (19.8-23KB, fluctuating normally) but the LARGEST CONTIGUOUS free block
// permanently pinned at exactly 13300 bytes, never moving. Traced the
// actual `host_recv_pkt_cb hci_hal_h4.c:662` crash to its exact allocation
// (ESP-IDF v5.4 source, same generation as this Arduino core's bundled
// Bluedroid): `osi_calloc(BT_HDR_SIZE + len)` per INCOMING HCI packet --
// this is genuinely a burst-of-many-small-allocations fragmentation
// problem, not one huge packet, and it only ever showed up against a full
// desktop BlueZ stack (heavier connection-time SDP/profile traffic than a
// phone) -- confirmed `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` is
// ALREADY applied automatically by this library's own btStart() (checked
// BluetoothA2DPCommon.cpp directly), so that standard ESP32 heap-recovery
// lever was not a missing optimization. The only remaining lever actually
// available on this board (plain WROOM32, no PSRAM, Bluedroid's own
// internal buffer sizing not exposed through the Arduino build) is
// reducing THIS APPLICATION's own static RAM footprint so the allocator
// arena has more room -- pcm_slots is by far the single largest static
// buffer in this whole sketch. Reduced 4->3 slots (was never actually
// tested at 3 before -- the "4 is the minimum" claim above was inferred
// from 8 being too many, not from 3 being verified too few). Verify via
// the existing, already-instrumented pcm_drops counter during a real
// sustained playback session (objective, not subjective-listening-based)
// before trusting this doesn't reintroduce dropped/corrupted audio.
#define PCM_SLOT_SIZE 4096
#define PCM_SLOT_COUNT 3

struct PcmSlot {
  uint8_t data[PCM_SLOT_SIZE];
  uint32_t length;
};
PcmSlot pcm_slots[PCM_SLOT_COUNT];
QueueHandle_t free_slots;   // indices of slots available to fill
QueueHandle_t filled_slots; // indices of slots ready to encode

#ifdef HEAP_TRACE
volatile uint32_t max_chunk_seen = 0;
#endif

// Confirmed by direct code audit: the drop branch below used to be
// completely silent -- no counter, no log line, nothing on the wire --
// making "audio glitches after N minutes" undiagnosable from device
// telemetry alone. This makes it visible via the existing 'C' control
// channel (see loop() below) instead of adding new Serial output paths.
volatile uint32_t pcm_drops = 0;

// Direct per-chunk timing for mp3_out.write() (Shine encode + framed
// serial write combined) -- added specifically to find out WHERE the CPU
// time goes that's causing pcm_drops, since reducing the DIAG_LOOP_DRAIN
// idle delay from 5ms to 1ms only cut the drop rate ~18%, meaning the
// idle gap was never the dominant factor. Single-task access only (either
// encode_task or the DIAG_LOOP_DRAIN loop() path, never both), so plain
// (non-atomic) uint32_t is fine here -- same as pcm_drops.
volatile uint32_t encode_us_max = 0;
volatile uint32_t encode_us_sum = 0;
volatile uint32_t encode_us_count = 0;

// Real bug found via live testing tonight (a real phone, not the PC-based
// test sources used all session): pausing on the phone makes A2DP stop
// delivering PCM entirely (audio_data_callback simply stops being called).
// The PC-side ring buffer's write_pos then freezes at whatever was last
// written, but car_sim.py's reader is DELIBERATELY dumb (faithfully
// matches a real commodity USB-MP3 head unit, which has no way to know
// the phone paused) and just keeps looping through whatever's frozen in
// the ring, forever -- confirmed directly: a real pause produced audible
// looping of stale backlog that persisted even after fully disconnecting
// Bluetooth and power-cycling the ESP32, since the audio being heard was
// already sitting in the PC-side ring's memory by that point. This
// timestamp is the mechanism for the fix below: track when real PCM was
// last actually seen, so the rest of the firmware can tell "genuinely
// live" apart from "frozen/stale" without needing any new signal from the
// phone.
volatile uint32_t last_real_audio_ms = 0;
// TEMP DEBUG -- counts real audio_data_callback invocations, to directly
// confirm whether the phone is actually delivering PCM at all right now.
// Remove once real audio is confirmed reaching the encoder.
volatile uint32_t g_audio_cb_count = 0;

// Link-level BT connection state, set from connection_state_changed()
// below -- used by loop()'s LED-status block so LED2 can distinguish
// "not connected" (off) from the two connected sub-states (blinking vs
// solid), instead of connection_state_changed() just writing the pin
// directly and loop() never knowing the current state.
volatile bool g_bt_connected = false;
// Last non-empty title, resent periodically from loop() so the S3 recovers
// its file names after its own reboot or a missed message (2026-09-25: an S3
// reflash wiped the name and the classic only sends on track change).
static char g_last_title[96] = {0};

volatile uint32_t g_bt_connected_since_ms = 0;  // see manage_encoder()

// REAL DIAGNOSTIC (2026-09-22, Muni: "the phone is paired and playing audio
// with the classic, yet i hear nothing" -- direct pushback on the earlier
// conclusion that this was purely phone-side, correctly demanding this be
// checked at the actual source instead of inferred from AUDIO_LIVE timing
// alone, which only proves callbacks are firing, NOT that their CONTENT is
// real). Measures the true peak absolute sample value of the RAW A2DP PCM
// this callback receives, straight from Bluedroid, before downmix/encode
// touch it at all -- the earlier -91dB finding was measured on the FINAL
// encoded MP3 output, several pipeline stages downstream, so it couldn't
// distinguish "phone sends silence" from "phone sends real audio but
// something in THIS pipeline destroys it before encoding." This settles it
// directly at the actual source.
volatile int16_t g_pcm_peak_since_report = 0;
void audio_data_callback(const uint8_t *data, uint32_t length) {
  last_real_audio_ms = millis();
  g_audio_cb_count++;
  {
    const int16_t *samples = (const int16_t *)data;
    uint32_t n = length / 2;
    int32_t peak = g_pcm_peak_since_report;  // int32_t: -INT16_MIN (-32768) overflows int16_t
    for (uint32_t i = 0; i < n; i++) {
      int32_t v = samples[i];
      int32_t av = (v < 0) ? -v : v;
      if (av > peak) peak = av;
    }
    g_pcm_peak_since_report = (int16_t)peak;
  }
#ifdef HEAP_TRACE
  if (length > max_chunk_seen) {
    max_chunk_seen = length;
    xSemaphoreTakeRecursive(serial_mutex, portMAX_DELAY);
    Serial.printf("[trace] new max PCM chunk length: %u\n", length);
    xSemaphoreGiveRecursive(serial_mutex);
  }
#endif
#ifdef DIAG_NO_ENCODE_TASK
  emit_mono(data, length);
  return;
#endif
  if (length > PCM_SLOT_SIZE) length = PCM_SLOT_SIZE;  // never overflow a slot
  uint8_t idx;
  if (xQueueReceive(free_slots, &idx, 0) != pdTRUE) {
    pcm_drops++;  // no free slot -- drop rather than block Bluedroid's task
    return;
  }
  memcpy(pcm_slots[idx].data, data, length);
  pcm_slots[idx].length = length;
  xQueueSend(filled_slots, &idx, 0);
}

// FIX for the stale-loop-on-pause bug described above. Rather than relying
// on AVRCP play-status notifications to detect a pause (unreliable in
// practice -- confirmed directly: a real phone paused mid-song tonight
// produced ZERO AVRCP PLAY/PAUSE/STOP events on the existing control
// channel, despite the pause genuinely happening; whatever combination of
// phone OS, music app, and AVRCP registration timing was in play tonight
// never sent one), this reacts to the actual ground truth instead: is
// real audio currently arriving at all. When it isn't (for longer than any
// real inter-chunk gap during active playback), inject synthetic digital
// silence through the EXACT SAME downmix->encode->serial pipeline real
// audio uses. This keeps write_pos genuinely advancing during a pause, so
// the ring reflects "currently: silence" instead of freezing on stale old
// content -- and needs zero changes on the car_sim.py/radio side, since a
// real dumb USB-MP3 reader played back silent source content would sound
// exactly the same as this. When real audio resumes, audio_data_callback
// naturally takes back over (this function only acts when nothing else
// has fed a chunk recently). Same byte rate as real audio: Shine encodes
// at a fixed CBR bitrate regardless of content, so encoded silence costs
// the same ~16000 B/s as real music, never affecting the FAT12 ring's
// margin/timing assumptions.
void feed_silence_if_no_real_audio() {
  // One chunk is PCM_SLOT_SIZE bytes = 1024 stereo 16-bit frames = 23219.95us
  // at 44.1kHz. REAL BUG FIXED (2026-09-24): this used to fire every 23ms of
  // millis(), producing silence ~1% faster than real time; the S3's live
  // cursor then drifted behind until a catch-up jump (a few decode errors
  // at each splice) roughly every 50s while disconnected. Scheduled against
  // micros() at the exact period now, so the stream stays on real time.
  static const uint32_t SILENCE_CHUNK_US = (uint32_t)((PCM_SLOT_SIZE / 4) * 1000000ULL / 44100);
  static uint32_t next_due_us = 0;
  static bool injecting = false;
  uint32_t now = millis();
  // 150ms: comfortably longer than any real inter-chunk gap during active
  // playback (a single real PCM chunk covers only tens of ms), short
  // enough that a genuine pause is covered quickly, before the listener
  // notices a gap.
  // Signed: BT_APP can bump last_real_audio_ms AFTER `now` was sampled, and
  // the unsigned difference then wraps to ~4e9 -- which used to inject a
  // silence chunk into the middle of live music.
  if ((int32_t)(now - last_real_audio_ms) < 150) {
    injecting = false;
    return;
  }
  uint32_t now_us = micros();
  if (!injecting) {
    injecting = true;
    next_due_us = now_us;
  }
  int32_t late_us = (int32_t)(now_us - next_due_us);
  if (late_us < 0) return;
  // After a long loop() stall, resync instead of bursting a backlog of
  // chunks (the free-slot queue only holds a few anyway).
  if (late_us > 200000) next_due_us = now_us;
  uint8_t idx;
  if (xQueueReceive(free_slots, &idx, 0) != pdTRUE) return;  // no free slot, retry next pass
  memset(pcm_slots[idx].data, 0, PCM_SLOT_SIZE);
  pcm_slots[idx].length = PCM_SLOT_SIZE;
  xQueueSend(filled_slots, &idx, 0);
  next_due_us += SILENCE_CHUNK_US;
}

// UNFLASHED, PROPOSED FIX for the pcm_drops / >100%-of-core-1-budget issue
// (see ENCODE_US telemetry: ~10.5ms avg / 25ms worst-case per chunk at
// 100-180 chunks/sec, already over one core's 1000ms/s real-time budget).
// Shine's own encode cost scales with total PCM sample-CHANNELS processed
// per second, not with output bitrate -- A2DP delivers interleaved stereo
// PCM (AudioInfo declared 44100/2ch/16-bit below), so Shine is currently
// doing MPEG Layer 3 psychoacoustic/quantization work on 88,200
// sample-values/sec. Downmixing to mono here (on the already-isolated
// encode_task, core 1 but NOT the time-critical Bluedroid receive path --
// audio_data_callback itself is untouched, still just a memcpy+queue-send)
// halves that to 44,100 sample-values/sec before it ever reaches Shine,
// which should bring the real measured cost down from ~105% toward roughly
// 55-75% of one core's budget (needs re-measurement via the same
// ENCODE_US telemetry after flashing -- this is an estimate, not a
// guarantee). Deliberately does NOT touch setBitrate()/output bitrate, so
// it doesn't change the ~128kbps/16000B/s rate the rest of the pipeline
// (FAT12 disk margin, car_sim.py's read cadence) is built around -- only
// the ESP32-side CPU cost changes, not the wire format or byte rate.
// A simple (L+R)/2 average, done in native int32_t to avoid int16 overflow
// on the sum before dividing. Flashed and active (confirmed live many
// times over -- AudioInfo below is declared mono, and this function is
// called from the currently-flashed DIAG_LOOP_DRAIN path). An earlier
// version of this comment said "NOT YET FLASHED" -- stale, don't trust
// that if you see it copied anywhere else (same class of doc-drift
// CLAUDE.md warns about elsewhere).
// REAL BUG FOUND (2026-09-20, first real live-phone test tonight -- never
// exercised before, since every earlier test used feed_silence_if_no_real_audio()'s
// fixed 4096-byte silence chunks, always evenly divisible by 4): `stereo_len`
// is NOT guaranteed to be a multiple of 4 for REAL Bluetooth PCM (Bluedroid
// delivers whatever its SBC decode produced per callback, with no alignment
// guarantee at all) -- the old `pairs = stereo_len / 4` truncation silently
// DISCARDED 0-3 trailing bytes on every callback where it wasn't evenly
// divisible, forever, with no carry-over -- continuously injecting small,
// cumulative desync into the PCM stream Shine encodes from. Confirmed live:
// `non_ff_start` (esp32-bt-mp3-test.ino's own frame-alignment diagnostic)
// climbed to ~65% of all frame writes the instant a real phone started
// sending real (non-silence) audio -- something no earlier synthetic-silence
// test could ever have caught, since silence chunks are always exactly
// 4096 bytes. Fixed by carrying any leftover 0-3 bytes forward into the
// START of the next call instead of dropping them, via static state --
// safe because only ONE call site (encode_task() OR the DIAG_LOOP_DRAIN
// inline path in loop()) is ever actually active in a given build.
//
// REAL SECOND BUG FOUND IN THE FIRST FIX (2026-09-20, same night): the
// first version of this fix added a `static uint8_t combined[PCM_SLOT_SIZE
// + 4]` scratch buffer (~4.1KB of NEW static/BSS memory) to simplify the
// leftover-carry logic. That single extra flash was immediately followed
// by every single BT connection attempt (phone AND, confirmed directly via
// bluetoothctl, this desktop too) hanging forever at the AVDTP/A2DP-
// profile level -- ACL-level "Connected: yes" but connection_state_changed()
// NEVER fired BT_CONNECTED, with the classic's own [frag] heap diagnostic
// showing total_free noticeably lower than in the immediately-prior,
// working flash. This EXACT failure mode (BT connection establishment
// breaking from reduced free heap, not a crash) is already a real,
// previously-confirmed mechanism on this exact board -- see PCM_SLOT_COUNT's
// own comment history elsewhere in this file ("collapsed free heap enough
// to break BT at 8 slots"). Root-caused by retracing the actual flash
// order tonight: pairing worked fine on the flash immediately BEFORE this
// one (which already had the BT rename and the clean_last_connection()
// removal in place) -- the ONLY change in the flash that broke it was this
// function's own combined-buffer version. Fixed by removing the `combined`
// buffer entirely: the leftover carry only ever needs the pre-existing
// 4-byte `leftover` buffer, handled as its own small pre-pass, with the
// bulk of stereo_len processed directly in place (zero extra copying,
// zero extra static memory beyond the original 4 bytes).
void downmix_stereo_to_mono(const uint8_t *stereo, uint32_t stereo_len,
                             uint8_t *mono_out, uint32_t *mono_len) {
  static uint8_t leftover[4];
  static uint32_t leftover_len = 0;
  uint32_t out_pairs = 0;
  uint32_t in_off = 0;

  // Complete a pair using leftover bytes from the previous call, if any.
  if (leftover_len > 0) {
    uint32_t need = 4 - leftover_len;
    uint32_t take = (stereo_len < need) ? stereo_len : need;
    memcpy(leftover + leftover_len, stereo, take);
    leftover_len += take;
    in_off += take;
    if (leftover_len == 4) {
      const int16_t *p = (const int16_t *)leftover;
      int32_t l = p[0], r = p[1];
      ((int16_t *)mono_out)[out_pairs++] = (int16_t)((l + r) / 2);
      leftover_len = 0;
    }
  }

  // Process remaining full pairs directly out of the caller's own buffer --
  // no scratch copy needed.
  uint32_t remaining = stereo_len - in_off;
  uint32_t pairs = remaining / 4;
  const int16_t *in = (const int16_t *)(stereo + in_off);
  for (uint32_t i = 0; i < pairs; i++) {
    int32_t l = in[2 * i];
    int32_t r = in[2 * i + 1];
    ((int16_t *)mono_out)[out_pairs++] = (int16_t)((l + r) / 2);
  }
  in_off += pairs * 4;

  // Save any final 0-3 leftover bytes for next call.
  leftover_len = stereo_len - in_off;
  if (leftover_len) memcpy(leftover, stereo + in_off, leftover_len);

  *mono_len = out_pairs * 2;  // 2 bytes per mono sample
}

void encode_task(void *) {
  uint8_t idx;
  // Half of PCM_SLOT_SIZE: worst case input is PCM_SLOT_SIZE bytes of
  // stereo (PCM_SLOT_SIZE/4 sample pairs), producing PCM_SLOT_SIZE/2 bytes
  // of mono. Static allocation, single-task-owned (only encode_task ever
  // touches this), same "no malloc/free per chunk" discipline as pcm_slots.
  static uint8_t mono_buf[PCM_SLOT_SIZE / 2];
#ifdef HEAP_TRACE
  uint32_t min_stack_words = UINT32_MAX;
#endif
  for (;;) {
    if (xQueueReceive(filled_slots, &idx, portMAX_DELAY) == pdTRUE) {
      uint32_t t0 = micros();
      uint32_t mono_len = 0;
      downmix_stereo_to_mono(pcm_slots[idx].data, pcm_slots[idx].length,
                              mono_buf, &mono_len);
      emit_mono(mono_buf, mono_len);
      uint32_t elapsed = micros() - t0;
      if (elapsed > encode_us_max) encode_us_max = elapsed;
      encode_us_sum += elapsed;
      encode_us_count++;
      xQueueSend(free_slots, &idx, portMAX_DELAY);
#ifdef HEAP_TRACE
      UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
      if (words < min_stack_words) {
        min_stack_words = words;
        xSemaphoreTakeRecursive(serial_mutex, portMAX_DELAY);
        Serial.printf("[trace] encode_task min stack headroom: %u words (%u bytes)\n",
                      (unsigned)words, (unsigned)(words * sizeof(StackType_t)));
        xSemaphoreGiveRecursive(serial_mutex);
      }
#endif
    }
  }
}

// REAL BUG FOUND AND FIXED (adversarial live-hardware disconnect/reconnect
// stress test, ~40% ESP32 reboot rate on real bluetoothctl disconnect+
// connect cycles, confirmed via esp32_ms collapsing to a fresh boot with no
// assert/crash-dump -- consistent with a watchdog/brownout-style reset, not
// a debuggable software fault): this callback runs on Bluedroid's own
// BT_APP task (pinned to core 0, see set_task_core(0) above), a DIFFERENT
// FreeRTOS task from Arduino's loopTask (core 1, runs the DIAG_LOOP_DRAIN
// audio-drain loop's Serial.write() calls). The original send_control()
// blocked with portMAX_DELAY on serial_mutex -- if loopTask were mid-write
// of an encoded audio chunk (e.g. because the PC-side reader momentarily
// stalled) exactly when a real link-level connect/disconnect fired, this
// callback could block for an unbounded time on a DIFFERENT task's lock
// hold. Bluedroid's own connection-state teardown/rebuild appears to have
// its own internal timing expectations (or the callback context itself is
// watchdog-monitored) -- stalling here is exactly the kind of thing that
// can trip a reset. The serial_mutex comment above already documents a
// DIFFERENT, previously-fixed same-task reentrancy hazard on this exact
// callback (hence the mutex being recursive) -- this is a second, distinct
// hazard on the same callback: cross-task blocking, not same-task
// reentrancy, and recursion alone doesn't protect against it. Fixed by
// bounding the wait to 20ms (long enough that a normal, fast write
// virtually always still gets through and logs normally, short enough to
// never itself become an availability problem) and simply skipping the
// diagnostic log line on contention -- losing an occasional BT_CONNECTED/
// BT_DISCONNECTED log line is a strictly acceptable tradeoff against
// crashing the whole board on every few reconnects.
#ifdef AVRC_AUTO_RESUME_ON_RECONNECT
// PLAN_NEXT.md item A1 (2026-09-19 design, implemented 2026-09-20): half-
// keying the car ignition (radio on, engine start cuts power to the
// classic ESP32 for ~1s) makes the phone's own media app self-pause --
// correct standard Android behavior, not a bug -- meaning Muni has to
// manually hit play again every time this happens mid-song. Deliberately
// does NOT rely on remembering "was it playing before" (can't survive a
// real power-cycle anyway); instead just always tries to resume once if
// the FIRST post-connect playstatus comes back paused/stopped. One-shot,
// never re-arms itself until the NEXT fresh connect.
volatile bool g_reconnect_resume_pending = false;
volatile uint32_t g_reconnect_ts_ms = 0;
// Stale-flag risk (a phone with weak/partial AVRCP support that never
// sends a post-connect playstatus notification at all would otherwise
// leave this armed indefinitely, incorrectly resuming an unrelated real
// pause minutes/hours later) -- this bounds it.
#define AVRC_AUTO_RESUME_RECONNECT_WINDOW_MS 20000
#endif  // AVRC_AUTO_RESUME_ON_RECONNECT

// REAL, PERMANENT fix (2026-09-21 autonomous session, not a one-time flash-
// and-remove hack like the two above): a stale/mismatched Bluedroid bond
// for ANY peer -- previously paired with THIS classic and later gone stale
// (its own bond wiped, a firmware reflash that cleared bonds, or just BT
// stack corruption), or previously paired with a DIFFERENT classic
// identity -- makes that peer's connection attempt hang indefinitely
// ("Connecting..." forever on the phone's own UI) rather than fail
// cleanly, because the classic silently rejects/hangs on the mismatched
// auth handshake with no recovery. Muni's own explicit standing
// requirement applies directly here: he'll be driving, there is no
// "forget this device and re-pair" step available to him or to whoever's
// phone is trying to connect -- this must self-heal on its own, for any
// device, any history, every time, permanently.
//
// Mechanism: ESP32-A2DP's BluetoothA2DPSink ALREADY registers its own GAP
// callback (BluetoothA2DPSink.cpp's app_gap_callback, via the global
// ccall_app_gap_callback trampoline in BluetoothA2DPCommon.h/.cpp) and
// ALREADY receives ESP_BT_GAP_AUTH_CMPL_EVT on every auth attempt -- but
// on failure it only logs the error and resets its own internal pin-code
// state; it never clears the stale bond that caused the failure. Since
// esp_bt_gap_register_callback() only supports ONE registered callback at
// a time (registering our own here REPLACES the library's), this wraps
// the library's own handling rather than replacing it: on
// ESP_BT_GAP_AUTH_CMPL_EVT with a non-success status, immediately remove
// JUST that specific peer's bond (not a blanket clear-everything, so a
// genuinely healthy bond for a different device is never touched), then
// forward the SAME event unchanged to the library's own
// ccall_app_gap_callback (a global friend function of BluetoothA2DPSink,
// declared in BluetoothA2DPCommon.h, already transitively included via
// BluetoothA2DPSink.h) so normal pairing (PIN/passkey confirmation, etc.)
// keeps working exactly as before for every other event type.
//
// SECOND real gap found the same night, same underlying principle: a real
// phone pairing attempt hung indefinitely on "Pairing..." even after the
// stale-bond self-heal above. `is_pin_code_active` is false (the library's
// own default, never overridden anywhere in this file), which configures
// `ESP_BT_IO_CAP_NONE` -- textbook SSP rules say IO_CAP_NONE on our side
// should always negotiate Just Works (no confirmation needed on either
// side) regardless of what the phone's own IO capability is. In practice,
// real phones don't always honor that cleanly with real ESP32 Bluedroid
// stacks -- a real, previously-hung pairing is exactly what a stuck
// ESP_BT_GAP_CFM_REQ_EVT (numeric-comparison confirmation) looks like from
// the outside, and the library's own handling of that event (see
// BluetoothA2DPSink.cpp) only stores the request and invokes an optional
// app-supplied callback -- this file never registered one, so nothing
// was ever confirming it, unlike our situation. Same standing requirement
// as above applies identically: there is no one available to look at a
// confirmation dialog and press yes, ever -- so auto-accept immediately,
// unconditionally, the instant this event fires.
// Open ACL links (any device mid-pairing or connecting) and the last GAP
// activity: the stuck-radio watchdog below must never restart the board
// while a device is talking to it. Seen 2026-09-25 on the bench: with the
// phone off, the watchdog restarted every ~3 min and killed pairings.
volatile int g_acl_links = 0;
volatile uint32_t g_last_gap_activity_ms = 0;

void self_healing_gap_callback(esp_bt_gap_cb_event_t event,
                                esp_bt_gap_cb_param_t *param) {
  // Pairing diagnostics (2026-09-25: a second device can't pair). One
  // control line per pairing-related GAP event; bounded wait, since this
  // runs on a Bluedroid task.
  {
    char g[96];
    g[0] = 0;
    const uint8_t *b = nullptr;
    switch (event) {
      case ESP_BT_GAP_AUTH_CMPL_EVT:
        b = param->auth_cmpl.bda;
        snprintf(g, sizeof(g), "GAP:auth_cmpl stat=%d name=%.20s", (int)param->auth_cmpl.stat,
                 (const char *)param->auth_cmpl.device_name);
        break;
      case ESP_BT_GAP_PIN_REQ_EVT:
        b = param->pin_req.bda;
        snprintf(g, sizeof(g), "GAP:pin_req min16=%d", (int)param->pin_req.min_16_digit);
        break;
      case ESP_BT_GAP_CFM_REQ_EVT:
        b = param->cfm_req.bda;
        snprintf(g, sizeof(g), "GAP:cfm_req num=%lu", (unsigned long)param->cfm_req.num_val);
        break;
      case ESP_BT_GAP_KEY_NOTIF_EVT:
        b = param->key_notif.bda;
        snprintf(g, sizeof(g), "GAP:key_notif passkey=%lu", (unsigned long)param->key_notif.passkey);
        break;
      case ESP_BT_GAP_KEY_REQ_EVT:
        b = param->key_req.bda;
        snprintf(g, sizeof(g), "GAP:key_req");
        break;
      case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        b = param->acl_conn_cmpl_stat.bda;
        snprintf(g, sizeof(g), "GAP:acl_conn stat=%d", (int)param->acl_conn_cmpl_stat.stat);
        break;
      case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        b = param->acl_disconn_cmpl_stat.bda;
        snprintf(g, sizeof(g), "GAP:acl_disconn reason=0x%02x", (int)param->acl_disconn_cmpl_stat.reason);
        break;
      default:
        break;
    }
    if (g[0]) {
      size_t n = strlen(g);
      if (b) snprintf(g + n, sizeof(g) - n, " peer=%02x:%02x:%02x", b[3], b[4], b[5]);
      send_control(g, pdMS_TO_TICKS(20));
    }
  }
  switch (event) {
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) g_acl_links++;
      g_last_gap_activity_ms = millis();
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      if (g_acl_links > 0) g_acl_links--;
      g_last_gap_activity_ms = millis();
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    case ESP_BT_GAP_PIN_REQ_EVT:
    case ESP_BT_GAP_CFM_REQ_EVT:
    case ESP_BT_GAP_KEY_NOTIF_EVT:
    case ESP_BT_GAP_KEY_REQ_EVT:
      g_last_gap_activity_ms = millis();
      break;
    default:
      break;
  }
  if (event == ESP_BT_GAP_AUTH_CMPL_EVT &&
      param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
    esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
  } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
    esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
  } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
    // Legacy PIN pairing (2026-09-25): some hosts pair the old way even
    // though we support SSP -- the laptop does, every time. The library
    // only records the address and never replies, so the host times out
    // (AuthenticationTimeout after ~30 s). Answer with the fixed PIN every
    // car kit uses, 0000 (16 digits if the host asks for a 16-digit PIN);
    // most hosts try 0000 on their own for audio devices.
    esp_bt_pin_code_t pin;
    uint8_t len = param->pin_req.min_16_digit ? 16 : 4;
    memset(pin, '0', sizeof(pin));
    esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
  }
  ccall_app_gap_callback(event, param);
}

// Set from connection_state_changed() (a Bluedroid-owned callback, must
// never call back into the Bluedroid stack directly -- see that function's
// own comment), consumed from loop() (the plain Arduino task) instead.
volatile bool g_need_avrc_ct_reregister = false;

// STUCK-RADIO WATCHDOG (2026-09-22, real bug found live tonight, Muni: "we
// need to be able to pair one device, then turn it off, pair another, and
// not have issues or resets or power cycles needed. this will be used
// while driving"): a real, reproducible case tonight where the radio
// silently stopped accepting ANY new pairing attempt at all -- confirmed
// via direct log evidence that ZERO GAP events of any kind (not even a
// failed ESP_BT_GAP_AUTH_CMPL_EVT, which self_healing_gap_callback above
// already handles) ever reached this app for many minutes across repeated
// real phone pairing attempts, while the classic itself stayed completely
// alive and healthy (stable heap, no crash, no RESET_REASON) the whole
// time. The only thing that fixed it was a full hardware reset -- meaning
// Bluedroid's own internal security-manager state machine got wedged in
// some way that produces literally no observable app-level event to
// self-heal from, a real gap beneath everything self_healing_gap_callback
// can react to. Since there's no public API to reset just that internal
// state, and since a human will never be available to power-cycle this
// while driving, the fix is a software self-restart: if a real, complete
// A2DP connection hasn't happened in STUCK_RADIO_WATCHDOG_MS, restart
// automatically via esp_restart() -- functionally identical to the manual
// hardware reset that was just confirmed to fix this, but fully automatic,
// satisfying the same standing zero-manual-intervention requirement this
// project has held since its very first session.
//
// IMPORTANT, real correction (2026-09-22, same night, Muni's direct
// pushback): this does NOT erase any bonded/paired device -- bond keys
// live in flash (NVS) and are completely unaffected by esp_restart(), same
// as any other reboot. The FIRST version of this watchdog used a 3-minute
// threshold, which Muni correctly flagged as far too aggressive for real
// deployed use -- a car legitimately parked with the phone out of range
// for a while would hit pointless restarts, and a restart landing mid-way
// through a real, otherwise-successful reconnect attempt could itself
// interrupt it. This was then loosened to 20 minutes as a rare last-resort.
//
// SHORTENED AGAIN, same night, with real decisive evidence this time (not
// just caution the other direction): built TWO real, non-destructive
// in-place recovery attempts for this exact wedge -- forcing
// connectable+discoverable mode directly every 5s (FORCE_CONNECTABLE
// above), and clearing a possibly-stale reconnect target after 60s
// (CLEARED_STALE_RECONNECT_TARGET below). BOTH were confirmed live to
// return success/fire correctly, yet a real, independent passive
// `bluetoothctl scan` from a separate PC still could NOT see this radio
// afterward -- direct proof that once genuinely wedged, no in-place API
// call fixes it, ONLY a real restart does (matching the very first
// hard-evidence finding on this bug: a manual hardware reset was the one
// thing that worked). Since both gentler attempts get a fair chance first
// (60s + a further settling window) and still reliably fail to restore
// real discoverability when this wedge happens, waiting a full 20 minutes
// to then do the one thing already proven to work is pure, needless delay
// -- 3 minutes gives both in-place attempts genuine room to succeed on
// their own first, while still recovering in a real, bounded time instead
// of leaving the radio silently unreachable for a third of an hour.
volatile uint32_t g_last_connected_ms = 0;  // set on boot in setup(), updated on every real connect
#define STUCK_RADIO_WATCHDOG_MS (3UL * 60UL * 1000UL)  // 3 minutes -- both gentler in-place fixes get a fair shot first

void connection_state_changed(esp_a2d_connection_state_t state, void *) {
  // LED2 itself is now driven continuously from loop() (see its LED-status
  // block) so it can distinguish live-audio blinking from silence-solid
  // while connected -- this callback only updates the flag loop() reads,
  // except on disconnect, where it forces the pin off immediately rather
  // than waiting for loop()'s next pass (which would in practice be within
  // ~1ms anyway, but zero-risk to just do it here too).
  g_bt_connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  if (!g_bt_connected) digitalWrite(2, LOW);
  if (!g_bt_connected) g_last_title[0] = 0;  // a different phone may connect next
  if (g_bt_connected) {
    g_last_connected_ms = millis();  // see STUCK_RADIO_WATCHDOG_MS's own comment in loop()
    g_bt_connected_since_ms = millis();
    // REAL CRASH FOUND AND FIXED (2026-09-22, live test pairing a NEW
    // device -- the PC -- for the first time since this line was added):
    // calling esp_avrc_ct_register_callback() DIRECTLY from inside this
    // callback caused a reproducible crash-loop (4 ESP_RST_PANIC reboots
    // in ~90s, confirmed via RESET_REASON, every single connection
    // attempt) -- something this exact scenario had never actually
    // exercised before (every earlier test tonight reconnected to the
    // SAME already-bonded phone). This file's OWN prior, hard-won history
    // (see the item-5/host_recv_pkt_cb crash writeup elsewhere in this
    // file, and connection_state_changed()'s own send_control() timeout
    // fix below) already established that THIS callback runs on a
    // Bluedroid-owned task and must never make blocking/reentrant calls
    // back into the Bluedroid stack -- esp_avrc_ct_register_callback() is
    // exactly such a call, and this line violated that same constraint.
    // Fix: defer it to loop() (the plain Arduino task, not a Bluedroid
    // callback) via a flag instead of calling it here directly.
    g_need_avrc_ct_reregister = true;
  }
#ifdef AVRC_AUTO_RESUME_ON_RECONNECT
  if (g_bt_connected) {
    g_reconnect_resume_pending = true;
    g_reconnect_ts_ms = millis();
  }
#endif
  send_control(state == ESP_A2D_CONNECTION_STATE_CONNECTED ? "BT_CONNECTED" : "BT_DISCONNECTED",
               pdMS_TO_TICKS(20));
  // Heap at the exact moment the audio link changes state (the 1s FRAG
  // sample can't isolate A2DP from the AVRCP link that follows it).
  char hb[64];
  snprintf(hb, sizeof(hb), "A2DP_LINK:%s,free=%u,largest=%u",
           state == ESP_A2D_CONNECTION_STATE_CONNECTED ? "up" : "down",
           (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  send_control(hb, pdMS_TO_TICKS(20));
}

// Distinct from connection_state_changed() above: link-level "connected" can
// be true for a long time before the phone ever opens the actual AVDTP
// audio STREAM (started specifically when a track plays). This fires
// exactly on that stream-level transition, timestamped independently of
// whether real PCM has reached audio_data_callback yet -- added to find
// out whether a ~15s press-play-to-real-audio gap is a phone/AVDTP
// negotiation delay (this fires late too) or a delay inside our own
// pipeline after the stream is already open (this fires fast, PCM doesn't).
void audio_state_changed(esp_a2d_audio_state_t state, void *) {
  char buf[48];
  snprintf(buf, sizeof(buf), "AUDIO_STATE:%s", a2dp_sink.to_str(state));
  send_control(buf, pdMS_TO_TICKS(20));
}

// REAL BUG FOUND (2026-09-17, via fresh adversarial review, same night as
// the residual-crash stress test): connection_state_changed() was fixed
// to use a bounded pdMS_TO_TICKS(20) instead of the default unbounded
// portMAX_DELAY on send_control(), specifically because blocking a
// Bluedroid-owned callback task indefinitely on serial_mutex (held by
// the audio-drain loop, which runs ~100-180 times/sec) is the exact
// class of violation behind the host_recv_pkt_cb hci_hal_h4.c crash --
// but that fix was never propagated to these two functions, which run on
// the SAME Bluedroid task class (see the file-header comment on that).
// Every track change or play/pause command could still block
// indefinitely here. Plausible contributor to the ~15% residual
// reconnect-crash rate measured tonight -- not confirmed as THE cause,
// but a real gap in an already-established fix, worth closing either way.
#ifdef AVRC_AUTO_RESUME_EARLY_PAUSE
// PLAN_NEXT.md item A2 (2026-09-19 design, implemented 2026-09-20):
// YouTube's idle/attention-check dialog tends to fire right at a track
// transition, not mid-song -- Muni's insight is that "paused within ~3-5s
// of a track actually starting" is a much stronger, more specific signal
// than a generic idle timer. Set by the shared track-change dispatcher
// below on every genuine AVRCP track-change event.
volatile uint32_t g_track_start_ms = 0;
#define AVRC_AUTO_RESUME_EARLY_PAUSE_MS 4000
#endif  // AVRC_AUTO_RESUME_EARLY_PAUSE

#ifdef AVRC_AUTO_SKIP_NEAR_END
// PLAN_NEXT.md item A3 (2026-09-19 design, corrected 2026-09-20 per
// STATUS.md's own resolved section F): a real, separately-measured ~10s
// MP3-decoder buffering floor is the dominant, ring-size-independent cost
// behind "song takes a while to start after the previous one ends" --
// nudging the phone to start loading/buffering the NEXT track a few
// seconds early (via an early next()) can let that ~10s buffering happen
// DURING the outgoing track's tail instead of after it ends.
volatile uint32_t g_duration_ms = 0;
volatile bool g_skip_triggered_for_track = false;
#define AVRC_AUTO_SKIP_NEAR_END_MS 7000
#endif  // AVRC_AUTO_SKIP_NEAR_END

// Root-cause fix (2026-09-22) for tonight's real-phone crash-loop/
// pairing-forever bug: this file's own long-standing comment (see
// avrc_metadata_callback below) already flagged that requesting
// ESP_AVRC_MD_ATTR_PLAYING_TIME via the metadata mask (GetElementAttributes)
// correlates with real packet_fragmenter.c crashes (large multi-attribute
// AVRCP responses overflow the HCI reassembly path under heap pressure) --
// AVRC_AUTO_SKIP_NEAR_END had silently re-added exactly that attribute to
// get song duration, the one genuinely new variable in tonight's build vs.
// the last confirmed-stable (0/52) AVRCP-enabled config. Fix: get duration
// via the AVRCP GetPlayStatus PDU instead (esp_avrc_ct_send_get_play_status_
// cmd) -- a small, FIXED-size response (song_length/song_position/
// play_status only, no variable-length strings), structurally unable to
// trigger the same fragmentation/reassembly-allocation path
// GetElementAttributes goes through.
//
// The library only supports ONE registered esp_avrc_ct_register_callback,
// same single-callback constraint self_healing_gap_callback already works
// around for GAP -- so this wraps/chains it the same way, via the same
// ccall_app_rc_ct_callback friend function the library uses internally
// (BluetoothA2DPCommon.h). ESP_AVRC_CT_PLAY_STATUS_RSP_EVT has no case at
// all in the library's own av_rc_ct handlers (confirmed by reading
// BluetoothA2DPSink.cpp directly) -- entirely safe to claim it here, then
// forward every event (including this one, harmlessly) so nothing else the
// library does is affected.
//
// MADE UNCONDITIONAL (2026-09-22, second pass): originally gated behind
// AVRC_AUTO_SKIP_NEAR_END since that's the only feature that needed
// PLAY_STATUS_RSP -- but AVRCP TITLE/ARTIST metadata had NEVER been
// received even once this entire session (confirmed: zero TITLE:/ARTIST:/
// META* lines across 16+ min and multiple reconnects), blocking the
// real-title-forwarding feature. Needed real visibility into WHICH avrc_ct
// events actually fire during a real connection to root-cause this, so
// this wrapper now unconditionally logs every event's numeric id over the
// existing control channel (AVRC_CT_EVT:<id>, see esp_avrc_ct_cb_event_t --
// 0=CONNECTION_STATE 1=PASSTHROUGH_RSP 2=METADATA_RSP 3=PLAY_STATUS_RSP
// 4=CHANGE_NOTIFY 5=REMOTE_FEATURES 6=GET_RN_CAPABILITIES_RSP
// 7=SET_ABSOLUTE_VOLUME_RSP 8=COVER_ART_STATE 9=COVER_ART_DATA
// 10=PROF_STATE), regardless of which feature flags are active.
//
// Real race avoided here (caught before ever flashing this): unlike GAP,
// which the library registers SYNCHRONOUSLY inside a2dp_sink.start()
// (confirmed by reading BluetoothA2DPSink.cpp directly), the library
// re-registers ITS OWN AVRC CT callback ASYNCHRONOUSLY, from
// av_hdl_stack_evt()'s BT_APP_EVT_STACK_UP handler -- which can fire AFTER
// start() returns. This is a ONE-TIME race at boot (STACK_UP fires once,
// early), not an ongoing one -- so registering once in setup() (same
// placement as self_healing_gap_callback) covers most boots, but to
// robustly close the race for real, ALSO re-register from
// connection_state_changed() on every A2DP connect: a real A2DP connection
// completing cannot happen before the BT stack is fully up, so by that
// point the library's own async registration is guaranteed to have
// already happened and ours wins -- a strictly earlier and more general
// safe point than the previous approach (re-registering only inside
// avrc_track_change_callback, which can't run before a track change has
// already happened and therefore can't observe the events THAT LED UP to
// one, like GET_RN_CAPABILITIES_RSP/METADATA_RSP -- exactly the events
// this investigation needs to see).
#define APP_RC_CT_TL_GET_PLAY_STATUS (5)  // library uses 0-4, see BluetoothA2DPCommon.h
#define APP_RC_CT_TL_GET_CAPS (0)  // matches the library's own constant (BluetoothA2DPCommon.h)

// REAL FIX (2026-09-22, Muni: "its entirely your problem... figure it out"
// -- real, confirmed Android phone, live-tested tonight): title/artist
// metadata never once arrived all session. Root-caused via the AVRC_CT_EVT
// diagnostic above: the library sends esp_avrc_ct_send_get_rn_capabilities_
// cmd() exactly once, automatically, right when the AVRCP link connects
// (event 0) -- and this phone never answered it (confirmed: event 0 and 5
// fire, then nothing, ever, no event 6). Without that response, the
// library's own internal handler (which is what actually requests
// metadata, via av_new_track()) never runs at all -- metadata was never
// even ASKED for, not lost or dropped. Real Android phones are known to
// sometimes miss/ignore an AVRCP command sent too soon after connection
// (their own AVRCP service can still be initializing) -- fixed by retrying
// the SAME command ourselves, repeatedly, until a real response actually
// arrives, exactly mirroring this project's own established
// self-healing/retry pattern used everywhere else tonight.
volatile bool g_avrc_link_connected = false;
volatile uint32_t g_avrc_caps_deadline_ms = 0;
volatile bool g_avrc_caps_received = false;
#define AVRC_CAPS_RETRY_MS 3000

void avrc_ct_wrapper_callback(esp_avrc_ct_cb_event_t event,
                               esp_avrc_ct_cb_param_t *param) {
  // Details for the events that tell us whether the controller side of the
  // AVRCP link actually came up (2026-09-24: the phone's link apparently
  // opens but the CT side never reports connected -- see STATUS.md).
  char buf[64];
  if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
    snprintf(buf, sizeof(buf), "AVRC_CT_EVT:0,connected=%d,free=%u,largest=%u", (int)param->conn_stat.connected,
             (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  } else if (event == ESP_AVRC_CT_REMOTE_FEATURES_EVT) {
    snprintf(buf, sizeof(buf), "AVRC_CT_EVT:5,feat=0x%lx,tg_flag=0x%x",
             (unsigned long)param->rmt_feats.feat_mask, (unsigned)param->rmt_feats.tg_feat_flag);
  } else if (event == ESP_AVRC_CT_PASSTHROUGH_RSP_EVT) {
    snprintf(buf, sizeof(buf), "AVRC_CT_EVT:1,key=0x%x,state=%d,rsp=%d",
             (unsigned)param->psth_rsp.key_code, (int)param->psth_rsp.key_state,
             (int)param->psth_rsp.rsp_code);
  } else {
    snprintf(buf, sizeof(buf), "AVRC_CT_EVT:%d", (int)event);
  }
  send_control(buf, pdMS_TO_TICKS(20));
#ifdef AVRC_AUTO_SKIP_NEAR_END
  if (event == ESP_AVRC_CT_PLAY_STATUS_RSP_EVT) {
    g_duration_ms = param->play_status_rsp.song_length;
  }
#endif
  if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
    g_avrc_link_connected = param->conn_stat.connected;
    g_avrc_caps_received = false;
    g_avrc_caps_deadline_ms = g_avrc_link_connected ? (millis() + AVRC_CAPS_RETRY_MS) : 0;
  } else if (event == ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT) {
    g_avrc_caps_received = true;
  }
  ccall_app_rc_ct_callback(event, param);
}

// Target-side (TG) twin of the wrapper above, logging only. Bluedroid
// reports a phone-initiated AVRCP link on the TG side first; the CT side is
// only reported once its own SDP of the phone completes, which is
// best-effort and never retried. A TG "connected" with no CT connected
// after it is exactly that failure.
void avrc_tg_wrapper_callback(esp_avrc_tg_cb_event_t event,
                              esp_avrc_tg_cb_param_t *param) {
  char buf[64];
  if (event == ESP_AVRC_TG_CONNECTION_STATE_EVT) {
    snprintf(buf, sizeof(buf), "AVRC_TG_EVT:0,connected=%d", (int)param->conn_stat.connected);
  } else if (event == ESP_AVRC_TG_REMOTE_FEATURES_EVT) {
    snprintf(buf, sizeof(buf), "AVRC_TG_EVT:1,feat=0x%lx,ct_flag=0x%x",
             (unsigned long)param->rmt_feats.feat_mask, (unsigned)param->rmt_feats.ct_feat_flag);
  } else {
    snprintf(buf, sizeof(buf), "AVRC_TG_EVT:%d", (int)event);
  }
  send_control(buf, pdMS_TO_TICKS(20));
  ccall_app_rc_tg_callback(event, param);
}

#if defined(AVRC_AUTO_RESUME_EARLY_PAUSE) || defined(AVRC_AUTO_SKIP_NEAR_END) || defined(RADIO_TRACK_RENAME)
// PLAN_NEXT.md's own A2 section flagged a real collision risk: the
// library's set_avrc_rn_track_change_callback() stores a SINGLE function
// pointer, not a list -- if A2 and A3 each independently registered their
// own handler, whichever was registered LAST would silently overwrite the
// other's, with no compiler warning. Fix: ONE shared dispatcher that
// internally calls into each enabled feature's own logic.
void avrc_track_change_callback(uint8_t *) {
#ifdef RADIO_TRACK_RENAME
  // PLAN_NEXT.md's C1/C2/C4/C6 unification (2026-09-21): tell the S3 a
  // real track change happened so it can force the currently-open file to
  // an early EOF and rotate to the next file (fat_disk_shared.h's
  // force_track_change()). Deliberately just a bare signal for now, no
  // title text -- Step 1 (per C1's own recommended path) is confirming the
  // rotation mechanism itself works before adding the real-name piece.
  send_control("TRACK_CHANGED", pdMS_TO_TICKS(20));
#endif
#ifdef AVRC_AUTO_RESUME_EARLY_PAUSE
  g_track_start_ms = millis();
#endif
#ifdef AVRC_AUTO_SKIP_NEAR_END
  // Real bug caught during design (PLAN_NEXT.md's A3 section, cross-
  // feature audit): duration and position arrive via two independent
  // AVRCP round-trips with no atomicity guarantee they describe the same
  // track -- right around a track change, a stale duration could pair
  // with a fresh position and produce a wrong near-end judgment on a
  // track that just started. Resetting here closes that window.
  g_duration_ms = 0;
  g_skip_triggered_for_track = false;
  // Registration itself now happens unconditionally in setup() and
  // connection_state_changed() (see avrc_ct_wrapper_callback's own
  // comment) -- no longer needed here.
  esp_avrc_ct_send_get_play_status_cmd(APP_RC_CT_TL_GET_PLAY_STATUS);
#endif
}
#endif

void avrc_playstatus_callback(esp_avrc_playback_stat_t playback) {
#ifdef RADIO_CMD_RELAY
  g_avrc_known_playing = (playback == ESP_AVRC_PLAYBACK_PLAYING);
#endif
#ifdef AVRC_AUTO_RESUME_ON_RECONNECT
  // One-shot: clear immediately regardless of outcome, never re-arms
  // itself until the next fresh connect. Only act within the sanity
  // window -- past it, a stale flag from a phone that never sent an
  // earlier post-connect notification must not hijack an unrelated,
  // genuinely-deliberate later pause.
  if (g_reconnect_resume_pending) {
    g_reconnect_resume_pending = false;
    bool in_window = millis() - g_reconnect_ts_ms < AVRC_AUTO_RESUME_RECONNECT_WINDOW_MS;
    bool needs_resume = (playback == ESP_AVRC_PLAYBACK_PAUSED || playback == ESP_AVRC_PLAYBACK_STOPPED);
    if (in_window && needs_resume && auto_command_cooldown_ok("reconnect_resume")) {
      a2dp_sink.play();
      send_control("CMD_SENT:auto_resume_reconnect", pdMS_TO_TICKS(20));
    }
  }
#endif
#ifdef AVRC_AUTO_RESUME_EARLY_PAUSE
  if (playback == ESP_AVRC_PLAYBACK_PAUSED &&
      millis() - g_track_start_ms < AVRC_AUTO_RESUME_EARLY_PAUSE_MS) {
    if (auto_command_cooldown_ok("early_pause_resume")) {
      a2dp_sink.play();
      send_control("CMD_SENT:auto_resume_early_pause", pdMS_TO_TICKS(20));
    }
  }
#endif
  switch (playback) {
    case ESP_AVRC_PLAYBACK_PLAYING: send_control("PLAY", pdMS_TO_TICKS(20)); break;
    case ESP_AVRC_PLAYBACK_PAUSED:  send_control("PAUSE", pdMS_TO_TICKS(20)); break;
    case ESP_AVRC_PLAYBACK_STOPPED: send_control("STOP", pdMS_TO_TICKS(20)); break;
    case ESP_AVRC_PLAYBACK_FWD_SEEK: send_control("SEEK_FWD", pdMS_TO_TICKS(20)); break;
    case ESP_AVRC_PLAYBACK_REV_SEEK: send_control("SEEK_REV", pdMS_TO_TICKS(20)); break;
    default: {
      char buf[24];
      snprintf(buf, sizeof(buf), "PLAYSTATUS_%d", (int)playback);
      send_control(buf, pdMS_TO_TICKS(20));
      break;
    }
  }
}

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (id == ESP_AVRC_MD_ATTR_TITLE && text && text[0]) {
    strncpy(g_last_title, (const char *)text, sizeof(g_last_title) - 1);
    g_last_title[sizeof(g_last_title) - 1] = 0;
  }
  const char *label;
  char label_buf[16];
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:  label = "TITLE:"; break;
    case ESP_AVRC_MD_ATTR_ARTIST: label = "ARTIST:"; break;
    case ESP_AVRC_MD_ATTR_ALBUM:  label = "ALBUM:"; break;
    // TEMP, investigation-only (2026-09-19): included specifically to
    // answer Muni's "what info can the phone send, e.g. music name or
    // size" question. Requesting ALBUM/PLAYING_TIME on top of TITLE/ARTIST
    // increases combined AVRCP metadata response size, which the
    // reconnect-crash investigation above (item 5's history) already
    // found correlates with real packet_fragmenter.c crashes -- acceptable
    // ONLY because this whole build already has AVRCP force-enabled for
    // this same investigation (A2DP_DISABLE_AVRC omitted); never ship this
    // wider mask in the default, crash-free build.
    case ESP_AVRC_MD_ATTR_PLAYING_TIME:
      label = "DURATION_MS:";
      // 2026-09-22: AVRC_AUTO_SKIP_NEAR_END no longer sets g_duration_ms
      // here -- it now comes from the safe GetPlayStatus path instead (see
      // avrc_ct_wrapper_callback's own comment, right after g_duration_ms's
      // declaration). This case only still fires at all if AVRC_INVESTIGATION
      // is also defined (its own separate, deliberately time-boxed mask
      // that re-adds PLAYING_TIME here on top) -- `text` remains a numeric
      // ASCII string per the AVRCP GetElementAttributes spec either way
      // (e.g. "245123" for 245.123s), just no longer wired to g_duration_ms.
      break;
    default:
      snprintf(label_buf, sizeof(label_buf), "META%u:", id);
      label = label_buf;
      break;
  }
  char buf[160];
  snprintf(buf, sizeof(buf), "%s%s", label, (const char *)text);
  send_control(buf, pdMS_TO_TICKS(20));
}

#ifdef AVRC_TRACK_POSITION
// TEMP, investigation-only, its own dedicated opt-in flag (2026-09-19):
// elapsed PLAYBACK POSITION is a genuinely different AVRCP mechanism from
// DURATION (ESP_AVRC_MD_ATTR_PLAYING_TIME, handled above) -- duration is
// static per-track metadata, position is a separate, periodically-repeating
// notification the library re-subscribes to on each firing
// (BluetoothA2DPSink::av_play_pos_changed()). Kept behind its OWN flag,
// separate from AVRC_INVESTIGATION, specifically so it can be left out
// even when AVRCP itself is enabled -- Muni asked for it to add no extra
// weight and be independently disableable. The interval argument to
// set_avrc_rn_play_pos_callback() below directly controls how often this
// fires (and how much extra 'C' control-channel traffic it adds); higher
// values cost less.
void avrc_play_pos_callback(uint32_t play_pos) {
  char buf[32];
  snprintf(buf, sizeof(buf), "POSITION_MS:%lu", (unsigned long)play_pos);
  send_control(buf, pdMS_TO_TICKS(20));
#ifdef AVRC_AUTO_SKIP_NEAR_END
  // Precision caveat (PLAN_NEXT.md's own note): this callback's periodic
  // interval (5s, set in setup()) puts real slop on exactly when within
  // the target window this fires -- accepted, since the whole point of
  // this workaround is "a bit early is fine".
  //
  // Real bug caught during design (PLAN_NEXT.md's A3 section): a
  // pathologically short real track (duration <= the near-end threshold
  // itself) would satisfy this check almost immediately on its own fresh
  // metadata arriving, causing an instant re-skip -- guarded against below.
  if (!g_skip_triggered_for_track && g_duration_ms > AVRC_AUTO_SKIP_NEAR_END_MS &&
      g_duration_ms >= play_pos &&
      (g_duration_ms - play_pos) <= AVRC_AUTO_SKIP_NEAR_END_MS) {
    g_skip_triggered_for_track = true;
    if (auto_command_cooldown_ok("near_end_skip")) {
      a2dp_sink.next();
      send_control("CMD_SENT:auto_skip_near_end", pdMS_TO_TICKS(20));
    }
  }
#endif
}
#endif  // AVRC_TRACK_POSITION

#ifndef BUILD_GIT_SHA
#define BUILD_GIT_SHA 0
#define BUILD_GIT_DIRTY 1
#endif
// Which build this is (flash.sh passes the commit): flags compiled in. Sent
// at boot and once a minute, since a logger started after a flash misses boot.
static void send_build_frame() {
    char b[128];
    snprintf(b, sizeof(b), "BUILD:commit=%07lx%s%s%s%s", (unsigned long)BUILD_GIT_SHA,
             BUILD_GIT_DIRTY ? "+dirty" : "",
#ifdef V2_ALL
             " V2_ALL",
#else
             "",
#endif
#ifdef ENCODE_ON_S3
             " ENCODE_ON_S3",
#else
             "",
#endif
#ifdef DIAG_LOOP_DRAIN
             " DIAG_LOOP_DRAIN"
#else
             ""
#endif
             );
    send_control(b);
}

void setup() {
  pinMode(2, OUTPUT);
  // RX only (TX=-1), 9600 baud -- deliberately slower than the forward
  // link's 921600, though the real bug turned out to be wiring (see
  // ReturnSerial's own comment above), not baud/noise. Kept at 9600 since
  // it's confirmed reliable and this channel doesn't need more bandwidth.
  // See esp32-s3-msc.ino's ReturnTxSerial for the matching sender-side
  // instance (a dedicated UART on GPIO4, decoupled from LinkSerial's own
  // busy 921600 baud).
  ReturnSerial.begin(9600, SERIAL_8N1, RETURN_RX_PIN, -1);
  // ESP-IDF's native ESP_LOGx macros (used internally by ESP32-A2DP /
  // Bluedroid) have their own runtime log level, independent of Arduino's
  // "Core Debug Level" build setting, and default to printing on UART0 --
  // the same wire our binary framed protocol uses. Any such line (e.g. a
  // queue-full warning under real audio load) corrupts a length field and
  // wedges the receiver forever waiting on a phantom-sized payload. Must
  // be silenced before anything else touches the BT stack.
#ifdef DIAG_LOGS
  esp_log_level_set("*", ESP_LOG_WARN);
#else
  esp_log_level_set("*", ESP_LOG_NONE);
#endif
  serial_mutex = xSemaphoreCreateRecursiveMutex();
  Serial.begin(LINK_BAUD);  // shared with the S3 via common/mp3_pipeline.h
  delay(500);

  // Diagnostic added after a real, confirmed reboot-on-reconnect bug
  // (adversarial bluetoothctl disconnect/reconnect testing found ~33-40%
  // of cycles crash the board) turned out to have at least two distinct
  // trigger paths -- one traced and partially fixed (a cross-task mutex
  // block in connection_state_changed()), the others still unknown because
  // every crash so far has been SILENT (no assert, no crash dump), leaving
  // only inference from correlated log evidence. `esp_reset_reason()` costs
  // nothing and turns the next crash from "unknown, guess again" into an
  // actual, specific answer (watchdog vs. brownout vs. panic vs. CPU
  // lockup vs. something else) reported once per boot as a normal CONTROL
  // event, matching the existing wire protocol -- no new message type.
  {
    const char *reason;
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:    reason = "POWERON"; break;
      case ESP_RST_SW:         reason = "SW"; break;
      case ESP_RST_PANIC:      reason = "PANIC"; break;
      case ESP_RST_INT_WDT:    reason = "INT_WDT"; break;
      case ESP_RST_TASK_WDT:   reason = "TASK_WDT"; break;
      case ESP_RST_WDT:        reason = "WDT"; break;
      case ESP_RST_BROWNOUT:   reason = "BROWNOUT"; break;
      case ESP_RST_CPU_LOCKUP: reason = "CPU_LOCKUP"; break;
      case ESP_RST_USB:        reason = "USB"; break;
      case ESP_RST_JTAG:       reason = "JTAG"; break;
      default:                 reason = "OTHER"; break;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "RESET_REASON:%s", reason);
    send_control(buf);
  }
  send_build_frame();

  // Slot pool is static (see pcm_slots above); these two queues just pass
  // slot indices (a single byte each) around, so they're the only actual
  // heap-backed FreeRTOS objects here and are tiny either way.
  free_slots = xQueueCreate(PCM_SLOT_COUNT, sizeof(uint8_t));
  filled_slots = xQueueCreate(PCM_SLOT_COUNT, sizeof(uint8_t));
  for (uint8_t i = 0; i < PCM_SLOT_COUNT; i++) {
    xQueueSend(free_slots, &i, 0);
  }

#ifdef HEAP_TRACE
  Serial.printf("[trace] free heap at start: %u\n", ESP.getFreeHeap());
#endif

  // Declared mono (1 channel) to match the downmix now done in encode_task
  // (and the DIAG_LOOP_DRAIN path below) -- see downmix_stereo_to_mono()'s
  // comment for why. A2DP itself still delivers stereo PCM into
  // audio_data_callback unchanged; the downmix happens after that, right
  // before Shine encoding.
  AudioInfo info(44100, 1, 16);
  // Not begun here: see manage_encoder() -- the encoder's ~78KB is only
  // held while audio is actually streaming.
  (void)info;
  // A silent full hang (zero serial output, unlike the assert-and-reboot
  // crashes seen elsewhere) was observed with an 8192-byte stack here --
  // consistent with a stack overflow silently corrupting adjacent memory
  // rather than a clean assert. Doubled as a safety margin while
  // HEAP_TRACE's high-water-mark print (above) measures the real number.
#if !defined(DIAG_NO_ENCODE_TASK) && !defined(DIAG_LOOP_DRAIN)
  // Pinned to core 0 -- confirmed by direct measurement (ENCODE_US's core=
  // field) plus reading the ESP32-A2DP library source
  // (BluetoothA2DPCommon.h: `BaseType_t task_core = 1;`, used to pin the
  // BT_APP task that calls audio_data_callback) that Arduino's own
  // loop()/setup() task and the library's BT_APP task both default to
  // core 1. Draining inline in loop() (DIAG_LOOP_DRAIN) means the
  // ~10.5ms-average, ~25ms-worst-case Shine encode work was directly
  // sharing one core with Bluedroid's own callback dispatch -- roughly
  // 100 chunks/sec x 10.5ms averages to ~1050ms of needed CPU time per
  // real second, which is already over a single core's 1000ms/sec budget
  // before counting anything else that core has to do. Core 0 runs the
  // BT controller/HCI layer, not BT_APP, so it should have real spare
  // capacity. This is a materially different experiment from the
  // DIAG_NO_ENCODE_TASK/DIAG_LOOP_DRAIN comparison in STATUS.md -- that
  // one tested an *unpinned* xTaskCreate (scheduler's choice of core,
  // often ending up back on core 1 anyway) against loop()-draining, not
  // an explicitly-pinned-to-the-genuinely-idle-core task.
  //
  // Priority bumped from 1 -> 2 (zero extra RAM, unlike growing
  // PCM_SLOT_COUNT which already collapsed free heap enough to break BT
  // at 8 slots -- see the comment above pcm_slots). Still well below
  // Bluedroid/BTC's own task priorities, just enough to reduce how often
  // this task loses a scheduling race against Arduino's default-priority
  // loopTask and falls behind draining filled_slots, which is exactly
  // when audio_data_callback starts silently dropping chunks (see
  // pcm_drops).
  xTaskCreatePinnedToCore(encode_task, "mp3_encode", 12288, nullptr, 2, nullptr, 0);
#endif

#ifdef HEAP_TRACE
  Serial.printf("[trace] free heap after shine init: %u\n", ESP.getFreeHeap());
#endif

#ifndef DISABLE_AVRC_DIAG
  // PLAN_NEXT.md's A3 section: restructured from the original either/or
  // AVRC_INVESTIGATION #ifdef/#else into an ADDITIVE mask, so
  // AVRC_AUTO_SKIP_NEAR_END can pull in PLAYING_TIME (needed for its own
  // near-end duration check) on its own, without requiring the rest of
  // AVRC_INVESTIGATION's extra ALBUM-attribute traffic/crash-risk.
  //
  // ALBUM dropped by default deliberately: every packet_fragmenter.c crash
  // one investigation night correlated with large multi-attribute AVRCP
  // metadata responses (real titles observed 100+ chars), while a long
  // metadata-free run (547s) and the longest run yet (780s) had zero
  // crashes. ALBUM is also the least useful field for a car-radio display.
  uint32_t avrc_md_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST;
#ifdef AVRC_INVESTIGATION
  // TEMP, investigation-only (2026-09-19): adds ALBUM back on top,
  // specifically to answer "what metadata can the phone actually send"
  // for a deliberate, time-boxed test -- never the default.
  avrc_md_mask |= ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME;
#endif
  // AVRC_AUTO_SKIP_NEAR_END used to add ESP_AVRC_MD_ATTR_PLAYING_TIME here
  // too -- REMOVED 2026-09-22 after root-causing tonight's real-phone
  // crash-loop/pairing-forever bug: this re-added exactly the
  // metadata-mask attribute this file's own comment above already flagged
  // as crash-correlated, and it's the one new variable in tonight's build
  // vs. the last confirmed-stable (0/52) AVRCP-enabled config. Duration is
  // now obtained via esp_avrc_ct_send_get_play_status_cmd() instead (see
  // avrc_ct_wrapper_callback below) -- a small, FIXED-size AVRCP
  // GetPlayStatus response (song_length/song_position/play_status only),
  // structurally immune to the large-variable-length-string HCI
  // reassembly path that GetElementAttributes (this mask) goes through.
  a2dp_sink.set_avrc_metadata_attribute_mask(avrc_md_mask);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_playstatus_callback(avrc_playstatus_callback);
#ifdef AVRC_TRACK_POSITION
  // 5s interval: real elapsed-position data without meaningfully adding to
  // AVRCP traffic volume (this is a periodic re-subscription, not a
  // continuous stream -- see avrc_play_pos_callback's own comment).
  a2dp_sink.set_avrc_rn_play_pos_callback(avrc_play_pos_callback, 5);
#endif
#if defined(AVRC_AUTO_RESUME_EARLY_PAUSE) || defined(AVRC_AUTO_SKIP_NEAR_END) || defined(RADIO_TRACK_RENAME)
  // PLAN_NEXT.md's A2 section: ONE shared dispatcher registration (see
  // avrc_track_change_callback's own comment for why -- the library only
  // supports a single registered handler for this notification).
  a2dp_sink.set_avrc_rn_track_change_callback(avrc_track_change_callback);
#endif
#endif  // DISABLE_AVRC_DIAG
  // Confirmed by direct measurement (pcm_drops + ENCODE_US telemetry): the
  // Shine encode work draining in loop() (DIAG_LOOP_DRAIN) needs ~1050ms
  // of CPU per real second on average -- already over one core's 1000ms/s
  // budget before counting anything else -- and it shares core 1 with
  // this library's own BT_APP task (BluetoothA2DPCommon.h's task_core
  // default of 1), which is what calls audio_data_callback and drives
  // connection-state handling. That's a plausible, well-evidenced
  // explanation for BOTH the PCM drops AND real, frequent AVDTP
  // disconnect/reconnect cycles (BT_APP starved of CPU at the wrong
  // moment could miss whatever timing the link supervision expects).
  // PERMANENTLY REVERTED -- CONFIRMED FIX, not just a hypothesis (adversarial
  // live-hardware disconnect/reconnect stress testing originally found
  // ~33-40% of real reconnect cycles fully rebooting the board; the crash's
  // own reset-reason diagnostic plus the actual assert text recovered from
  // the serial log -- `assert failed: host_recv_pkt_cb hci_hal_h4.c:662` --
  // is the EXACT SAME assert originally seen and root-caused when OUR OWN
  // ad-hoc task was pinned to core 0 with xTaskCreatePinnedToCore(), a known
  // category of ESP-IDF Bluedroid HCI/controller-layer core-affinity
  // violation). set_task_core() was reasoned at the time to be safe because
  // it moves the library's OWN task via its OWN supported API rather than
  // an ad-hoc one -- that reasoning turned out to be an unproven assumption:
  // moving BT_APP off its default core can STILL violate an internal
  // HCI/controller core-affinity expectation in the reconnect path
  // specifically, even via the library's supported API. CONFIRMED via a
  // 26-cycle real bluetoothctl disconnect/reconnect A/B test after
  // reverting this one line: 26/26 clean, ZERO crashes (vs. the ~35%
  // pre-revert baseline -- P(0/26 by chance if the true rate were still
  // 35%) ≈ 0.006%, statistically decisive). Known, accepted tradeoff: this
  // call previously reduced ordinary BT disconnect FREQUENCY during steady
  // playback (~1/10-25s -> ~1/80+s) -- but a graceful disconnect/reconnect
  // cycle recovers within seconds with zero data corruption (confirmed
  // directly), while a full ESP32 reboot is a much worse ~5-8s hard outage.
  // A higher rate of the former is a clear net win over any rate of the
  // latter for real-world use.
  // a2dp_sink.set_task_core(0);
  // REAL BUG FOUND: auto_reconnect was false, meaning after ANY reset (a
  // crash, a power blip, or literally anyone re-opening the USB-serial
  // port -- ESP32 dev boards commonly auto-reset on DTR/RTS transitions
  // during serial open) the ESP32 just sat there waiting passively for an
  // inbound connection instead of trying to reconnect to the last-paired
  // phone. Combined with phones that don't proactively reconnect to an
  // A2DP peripheral that "disappeared" ungracefully, this produced exactly
  // the "have to unpair, then re-pair" symptom hit repeatedly tonight --
  // completely unacceptable for a device meant to run unattended while
  // driving. The library already supports this properly: set_auto_reconnect
  // persists the last-connected address to NVS (survives reboots) and
  // retries connecting to it automatically without any phone-side action
  // needed.
  //
  // REAL BUG FOUND (2026-09-21, morning-after real-phone test), THEN A
  // REAL REGRESSION FOUND IN THE FIRST FIX (2026-09-21, same day, first
  // real car test of that fix): the desktop was paired to this board the
  // night before (testing without a phone). Muni tried to pair a REAL
  // PHONE the next morning and it wouldn't connect -- root cause confirmed
  // by reading the vendored library's own source directly: each
  // individual reconnect attempt has its own `default_reconnect_timout`
  // of 10000ms (10s), and with the library's default AUTOCONNECT_TRY_NUM=
  // 1000, that's up to ~2.8 hours of the radio periodically busy re-paging
  // an address that's no longer reachable. The FIRST fix attempt bounded
  // the retry count down to 3 (AUTO_RECONNECT_TRY_COUNT) -- but this
  // introduced a REAL REGRESSION, confirmed the same day via a real car
  // test: the classic started disconnecting from the ALREADY-paired phone
  // and requiring a manual unpair/re-pair to recover. Root cause of the
  // regression, confirmed by reading the exact same library source more
  // carefully: the library ALREADY reopens connectable mode for a NEW
  // device after just 2 failed reconnect attempts (~20s), REGARDLESS of
  // the total retry count (BluetoothA2DPSink.cpp:907, `if
  // (connection_rety_count == 2) set_scan_mode_connectable(true);`) -- so
  // bounding the count to 3 was never actually necessary to let a new
  // device pair. What it DID do: make the classic hit the library's own
  // "give up" branch (BluetoothA2DPSink.cpp:908-914) after only ~30s of
  // ANY brief, normal disconnect of the CURRENT phone -- and that branch
  // calls clean_last_connection() on ESP_A2D_DISC_RSN_NORMAL, wiping the
  // classic's own persisted "last connected" address for the device it
  // should have kept trying to reconnect to. That's the real, confirmed
  // cause of the "have to unpair and re-pair" symptom from the real car
  // test -- a self-inflicted regression from the first fix, not a
  // pre-existing bug.
  //
  // Corrected fix: DON'T override the retry count at all -- the library's
  // own default (1000 tries) combined with its ALREADY-BUILT-IN
  // connectable-reopen-after-2-tries mechanism already satisfies Muni's
  // real requirement ("auto reconnect, but if not paired, allow another
  // device to pair") without needing to ever give up on and forget the
  // current phone. Back to the plain two-argument start() this project
  // used before this whole investigation began.
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_on_audio_state_changed(audio_state_changed);
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  // REMOVED 2026-09-22 (was only ever meant to be temporary -- see this
  // spot's own history a few lines up about two EARLIER one-time hacks
  // already removed for the identical reason): a
  // `clean_last_connection()` call briefly lived here during one night's
  // PC-vs-phone reconnect-contention testing. Muni correctly flagged that
  // wiping the reconnect target on EVERY boot (not just once) actively
  // breaks the real, deployed requirement -- proactively reconnecting to
  // whichever device was last used, every time, with no manual step ever
  // needed. Real bonds (the actual security keys) live in flash (NVS) and
  // are NEVER touched by this -- only the one-shot "which address to page
  // on boot" hint was being cleared. Back to the plain two-argument
  // start(), matching the corrected-fix reasoning already documented
  // immediately above.
  a2dp_sink.start("Golzin", true);
  g_last_connected_ms = millis();  // watchdog window starts from boot, not from an actual connect
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  // PERMANENT fix (2026-09-21, replaces two earlier one-time flash-and-
  // remove hacks that briefly lived here -- see self_healing_gap_callback's
  // own comment for the full reasoning). Re-registering AFTER
  // a2dp_sink.start() deliberately: that call does its own
  // esp_bt_gap_register_callback() first, so registering ours after is
  // what makes it the one actually active from here on; any earlier
  // placement would just get silently overwritten by the library's own
  // registration inside start().
  esp_bt_gap_register_callback(self_healing_gap_callback);
  // Same reasoning as self_healing_gap_callback above: a2dp_sink.start()
  // already did its own esp_avrc_ct_register_callback() internally, so
  // registering ours after is what makes it the one actually active. Now
  // unconditional (was gated behind AVRC_AUTO_SKIP_NEAR_END) -- see
  // avrc_ct_wrapper_callback's own comment. This covers the common case;
  // connection_state_changed() re-asserts it on every real connect too,
  // closing the one-time boot-order race this alone can't guarantee against.
  esp_avrc_ct_register_callback(avrc_ct_wrapper_callback);
  esp_avrc_tg_register_callback(avrc_tg_wrapper_callback);

#ifdef HEAP_TRACE
  Serial.printf("[trace] free heap after a2dp start: %u\n", ESP.getFreeHeap());
#endif

  send_control("READY");
}

// Encoder lifecycle (2026-09-24). Root cause of "no song names" and "radio
// Next does nothing": the phone's AVRCP link opened and was torn down within
// ~20ms on every connect, because Shine's ~78KB of heap (allocated at boot)
// left only ~22KB free / 13.3KB largest block. Proven by A/B: with Shine
// skipped, free heap was ~100KB, AVRCP stayed up and TITLE:/ARTIST: arrived.
// So the encoder is only held while audio actually streams: started once real
// PCM arrives AND AVRCP has had its chance to connect (connected, or 4s after
// the A2DP link came up), stopped on disconnect so the next connect gets the
// same headroom. While stopped, mp3_out.write() is a no-op and the S3 serves
// valid silent frames on its own (live-serve underrun handling).
static bool g_encoder_active = false;
static const uint32_t ENCODER_AVRC_GRACE_MS = 4000;

void manage_encoder() {
#ifndef ENCODE_ON_S3
  uint32_t now = millis();
  if (g_encoder_active && !g_bt_connected) {
    mp3_out.end();
    g_encoder_active = false;
    char b[48];
    snprintf(b, sizeof(b), "ENCODER:off,free=%u", (unsigned)ESP.getFreeHeap());
    send_control(b);
    return;
  }
  if (!g_encoder_active && g_bt_connected &&
      (int32_t)(now - last_real_audio_ms) < 500 &&
      (g_avrc_link_connected || (int32_t)(now - g_bt_connected_since_ms) > (int32_t)ENCODER_AVRC_GRACE_MS)) {
    g_encoder_active = mp3_out.begin();
    char b[64];
    snprintf(b, sizeof(b), "ENCODER:%s,avrc=%d,free=%u,largest=%u", g_encoder_active ? "on" : "FAILED",
             (int)g_avrc_link_connected, (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    send_control(b);
  }
#endif  // ENCODE_ON_S3: the S3 owns the encoder, nothing to manage here
}

void loop() {
  manage_encoder();
  // Deferred from connection_state_changed() -- see its own comment and
  // g_need_avrc_ct_reregister's declaration for why this can't happen
  // directly inside that Bluedroid-owned callback.
  if (g_need_avrc_ct_reregister) {
    g_need_avrc_ct_reregister = false;
    esp_avrc_ct_register_callback(avrc_ct_wrapper_callback);
    esp_avrc_tg_register_callback(avrc_tg_wrapper_callback);
  }

  // GetCapabilities retry -- see g_avrc_link_connected's own comment above
  // for the full real story (a real phone silently never answering the
  // library's own one-shot request, blocking metadata from ever being
  // asked for at all). Resend the identical command ourselves, repeatedly,
  // until it's actually answered.
  if (g_avrc_link_connected && !g_avrc_caps_received &&
      g_avrc_caps_deadline_ms != 0 && millis() >= g_avrc_caps_deadline_ms) {
    esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
    g_avrc_caps_deadline_ms = millis() + AVRC_CAPS_RETRY_MS;
  }

  // REAL FIX (2026-09-22, live, Muni: "still fully unable to pair my
  // phone... figure it out and address it"): confirmed via direct evidence
  // -- during a real, full pairing attempt, ZERO GAP or AVRC events of any
  // kind reached this app for the whole attempt, while the classic itself
  // stayed alive and healthy (no crash, no reset). That means the phone's
  // connection attempt never even reaches the classic's radio at the
  // page-scan level -- it's not a failed handshake, the classic isn't
  // actually connectable/discoverable at that moment, for reasons the
  // library's own internal reconnect state machine doesn't expose or
  // recover from on its own. Direct, unconditional fix: force the radio
  // back into connectable+discoverable mode ourselves, periodically,
  // whenever genuinely disconnected -- regardless of whatever internal
  // state the library's own auto-reconnect logic thinks it's in. Cheap
  // (a couple of HCI commands, only while disconnected, at most once every
  // few seconds) and can't interfere with an active connection since it's
  // gated on !g_bt_connected.
  {
    static uint32_t last_force_connectable_ms = 0;
    uint32_t nowfc = millis();
    if (!g_bt_connected && nowfc - last_force_connectable_ms >= 5000) {
      last_force_connectable_ms = nowfc;
      esp_err_t fc_res = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
      char fc_buf[40];
      snprintf(fc_buf, sizeof(fc_buf), "FORCE_CONNECTABLE:res=%d", (int)fc_res);
      send_control(fc_buf);
    }
  }

  // Stuck-radio watchdog -- see g_last_connected_ms's own declaration
  // comment for the full real-world story. Checked once per loop() pass
  // (cheap: one subtraction/compare), only ever fires while genuinely
  // disconnected.
  //
  // REAL BUG FIXED (2026-09-24, live): g_last_connected_ms used to be set
  // only at connect time, so after any session longer than the window, a
  // normal disconnect restarted the board in the same millisecond
  // (BT_DISCONNECTED immediately followed by WATCHDOG_RESTART) instead of
  // giving the in-place reconnect a chance. Refresh it every pass while
  // connected, so the window measures time SINCE disconnect.
  if (g_bt_connected) g_last_connected_ms = millis();
  // A device linked or pairing, or any recent pairing/link event, means the
  // stack isn't wedged (the wedge this watchdog exists for produces no GAP
  // events at all): hold off.
  if (g_acl_links > 0) g_last_connected_ms = millis();
  if ((int32_t)(g_last_gap_activity_ms - g_last_connected_ms) > 0) g_last_connected_ms = g_last_gap_activity_ms;
  // Signed for the same reason as last_real_audio_ms: the connect callback
  // can store a newer timestamp than the millis() sampled here.
  if (!g_bt_connected &&
      (int32_t)(millis() - g_last_connected_ms) > (int32_t)STUCK_RADIO_WATCHDOG_MS) {
    send_control("WATCHDOG_RESTART:stuck_disconnected");
    delay(50);  // give the control message a moment to actually go out before the reboot cuts it off
    esp_restart();
  }

  // REAL FIX, second half (2026-09-22, live, same real bug as
  // FORCE_CONNECTABLE above): a bounded (60s -- long enough that a normal,
  // quick reconnect to the actual last-used device is never disrupted;
  // short enough to matter right now) real-world observation: the
  // library's own internal auto-reconnect keeps retrying the SAME stored
  // address for up to ~1000 tries by design (this file's own established,
  // deliberate choice, see the long comment right after a2dp_sink.start()
  // for why that retry COUNT was never bounded down before) -- but if that
  // stored address is one that will NEVER answer again (e.g. a different
  // device that was only ever used for one-off testing, with its own
  // Bluetooth now off on purpose), the radio can end up persistently busy
  // paging out to it, which can starve its ability to respond to a genuinely
  // NEW pairing attempt from a different, actually-present device. Confirmed
  // live tonight: minutes of real disconnection with ZERO GAP/AVRC events
  // ever reaching this app during a real, repeated phone pairing attempt.
  // Clearing the stored reconnect target after a real, generous timeout
  // (not the earlier, already-reverted short-timeout experiment that broke
  // NORMAL reconnects within ~20-30s) frees the radio for a new device
  // without needing yet another manual reset.
  {
    static uint32_t disconnected_since_ms = 0;
    static bool caps_cleared_this_gap = false;
    if (g_bt_connected) {
      disconnected_since_ms = 0;
      caps_cleared_this_gap = false;
    } else {
      if (disconnected_since_ms == 0) disconnected_since_ms = millis();
      if (!caps_cleared_this_gap && millis() - disconnected_since_ms > 60000) {
        caps_cleared_this_gap = true;
        a2dp_sink.clean_last_connection();
        send_control("CLEARED_STALE_RECONNECT_TARGET");
      }
    }
  }
#ifdef HEAP_TRACE
  // Real root-cause investigation (2026-09-22, Muni: "we dont want any
  // issue to remain, get to the bottom of it") -- disabling AVRCP entirely
  // was a workaround, not a fix, for the real
  // `host_recv_pkt_cb hci_hal_h4.c:662` heap-allocation-failure crash.
  // This prints BOTH total free heap AND the LARGEST CONTIGUOUS free block
  // every 500ms -- the reassembly allocator (osi_calloc in
  // packet_fragmenter.c) needs one single contiguous block, so total free
  // heap staying healthy while largest_block collapses is the real
  // fragmentation signature to look for, distinct from genuine exhaustion
  // (both collapsing together).
  {
    static uint32_t last_heap_print_ms = 0;
    uint32_t now = millis();
    if (now - last_heap_print_ms >= 500) {
      last_heap_print_ms = now;
      char buf[64];
      snprintf(buf, sizeof(buf), "HEAP:free=%u,largest=%u",
               (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      send_control(buf, pdMS_TO_TICKS(20));
    }
  }
#endif
  poll_return_serial();
#ifdef AVRC_INVESTIGATION
  poll_pc_commands();
#endif

  // Reported via the existing 'C' control channel -- s3_sim_serial.py
  // already logs these with a timestamp, so this needs no new wire
  // format or receiver-side change. Only sent when the count actually
  // changed, so a healthy run (the common case) produces no extra
  // traffic at all.
  static uint32_t last_reported_drops = 0;
  static uint32_t last_drop_report_ms = 0;
  uint32_t now_ms = millis();
  if (pcm_drops != last_reported_drops && now_ms - last_drop_report_ms >= 1000) {
    char buf[32];
    snprintf(buf, sizeof(buf), "PCM_DROPS:%lu", (unsigned long)pcm_drops);
    send_control(buf);
    last_reported_drops = pcm_drops;
    last_drop_report_ms = now_ms;
  }

  // Snapshot + reset every second so encode_us_max reflects "worst chunk
  // in the last second" rather than an all-time max that one early outlier
  // would dominate forever.
  static uint32_t last_encode_report_ms = 0;
  if (now_ms - last_encode_report_ms >= 1000) {
    last_encode_report_ms = now_ms;
    uint32_t count = encode_us_count;
    uint32_t max_us = encode_us_max;
    uint32_t sum_us = encode_us_sum;
    encode_us_count = 0;
    encode_us_max = 0;
    encode_us_sum = 0;
    if (count > 0) {
      uint32_t avg_us = sum_us / count;
      char buf[80];
      snprintf(buf, sizeof(buf), "ENCODE_US:avg=%lu,max=%lu,n=%lu,core=%d",
               (unsigned long)avg_us, (unsigned long)max_us, (unsigned long)count,
               xPortGetCoreID());
      send_control(buf);
    }
  }

  // TEMP DEBUG -- see g_audio_cb_count above. Remove once real audio is
  // confirmed reaching audio_data_callback.
  static uint32_t last_audio_status_ms = 0;
  if (now_ms - last_audio_status_ms >= 1000) {
    last_audio_status_ms = now_ms;
    char buf[64];
    snprintf(buf, sizeof(buf), "AUDIO_CB_STATUS:count=%lu,ms_since_last=%ld",
             (unsigned long)g_audio_cb_count,
             (long)(int32_t)(now_ms - last_real_audio_ms));
    send_control(buf);
    {
      char pcm_buf[32];
      snprintf(pcm_buf, sizeof(pcm_buf), "PCM_PEAK:%d", (int)g_pcm_peak_since_report);
      send_control(pcm_buf);
      g_pcm_peak_since_report = 0;  // reset for the next 1s reporting window
    }
    char buf2[100];
    snprintf(buf2, sizeof(buf2),
             "WRITE_SIZES:last8=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,non_ff_start=%lu/%lu",
             (unsigned long)g_write_sizes[0], (unsigned long)g_write_sizes[1],
             (unsigned long)g_write_sizes[2], (unsigned long)g_write_sizes[3],
             (unsigned long)g_write_sizes[4], (unsigned long)g_write_sizes[5],
             (unsigned long)g_write_sizes[6], (unsigned long)g_write_sizes[7],
             (unsigned long)g_non_ff_start_count, (unsigned long)g_write_count);
    send_control(buf2);
    // REAL BUG FOUND (2026-09-21, live test): connection_state_changed()
    // only sends BT_CONNECTED/BT_DISCONNECTED on an actual EDGE transition
    // -- if the S3 reboots/reflashes while the phone is ALREADY connected
    // (no new transition ever happens), it has no way to learn the real
    // current state and gets stuck showing "not paired" indefinitely,
    // however long the phone stays connected. Same class of bug as every
    // other "must self-heal without a restart, ever" fix tonight -- resend
    // the CURRENT state here too, not just on transitions, so a freshly-
    // booted S3 converges to the truth within ~1s regardless of whether a
    // real transition ever fires again.
    send_control(g_bt_connected ? "BT_CONNECTED" : "BT_DISCONNECTED");
    // REAL BUG FOUND AND FIXED (2026-09-22, live, Muni: "the s3 is flasging
    // rainbow, so it should be playing... yet i hear nothing" -- traced
    // directly to a real PCM peak-amplitude diagnostic showing ZERO real
    // audio ever reached audio_data_callback this whole boot, while the S3
    // showed "playing" the entire time regardless): AUDIO_LIVE/
    // AUDIO_SILENCE (further below, the 400ms-debounced block) only ever
    // sends on a real EDGE TRANSITION, exactly the same class of gap
    // BT_CONNECTED above was already fixed for -- if that ONE transition
    // message is ever missed (a real, confirmed case: right at classic
    // boot, before its own UART link to the S3 is even up yet, the very
    // first "audio not live" transition can't reach the S3 at all), the S3
    // has no way to ever learn the truth and gets stuck showing stale
    // "playing" state indefinitely, however long reality disagrees.
    // Same fix, same pattern: resend the CURRENT state here too, every
    // ~1s, not just on transitions.
    send_control(((int32_t)(now_ms - last_real_audio_ms) < 150) ? "AUDIO_LIVE" : "AUDIO_SILENCE");
    static uint32_t last_build_ms = 0;
    if (now_ms - last_build_ms >= 60000) {
      last_build_ms = now_ms;
      send_build_frame();
    }
    static uint32_t last_title_resend_ms = 0;
    if (g_bt_connected && g_last_title[0] && now_ms - last_title_resend_ms >= 5000) {
      last_title_resend_ms = now_ms;
      char tb[110];
      snprintf(tb, sizeof(tb), "TITLE:%s", g_last_title);
      send_control(tb);
    }
  }

  // LED2 (classic ESP32 DevKit's onboard blue LED) status, requested by
  // Muni for at-a-glance debugging without a laptop attached: off when not
  // BT-connected; solid on when connected but currently injecting synthetic
  // silence (same 150ms freshness window feed_silence_if_no_real_audio()
  // itself uses, so the LED and the actual silence-injection decision can
  // never visually disagree); blinking ~1Hz when connected and genuinely
  // live audio is flowing. Only writes the pin on an actual state change or
  // blink toggle, not every loop() pass.
  //
  // Also sends an AUDIO_LIVE/AUDIO_SILENCE control message on each
  // live<->silence transition, for the S3's own RGB status LED. The S3
  // can't tell live audio from injected silence purely from 'A' frames --
  // feed_silence_if_no_real_audio() pushes silence through the exact same
  // encode->send_framed('A',...) path real audio uses (deliberately, so
  // the ring's byte rate/margin assumptions never change -- see that
  // function's own header comment), so this is the only signal that
  // actually carries the distinction across the wire. Transition-based
  // (not periodic) to match this file's existing low-traffic convention.
  {
    static uint32_t last_led_toggle_ms = 0;
    static bool led_on = false;
    bool audio_live = ((int32_t)(now_ms - last_real_audio_ms) < 150);
    // REAL BUG FOUND AND FIXED (2026-09-22, live): confirmed via the actual
    // log -- real Bluetooth PCM delivery jitter briefly (sub-150ms) crosses
    // this threshold and recovers almost instantly (AUDIO_SILENCE followed
    // by AUDIO_LIVE just ~16ms later, repeatedly), each pair getting sent
    // straight to the S3 -- which flickers its rainbow/status LED rapidly,
    // Muni: "the light is flashing on and off rapidly... not the normal
    // [rainbow]". The 150ms threshold itself stays exactly as-is for
    // feed_silence_if_no_real_audio()'s own real injection decision
    // (last_real_audio_ms, untouched) -- only the WIRE NOTIFICATION to the
    // S3 gets a separate, longer debounce: a transition to SILENCE must
    // hold for DEBOUNCE_MS before it's actually reported, so brief jitter
    // never reaches the S3 at all. Transitioning back to LIVE stays
    // immediate (no debounce) -- there's no downside to reporting "audio's
    // back" right away, only false SILENCE reports were the problem.
    static uint32_t silence_since_ms = 0;
    static bool reported_audio_live = true;
#define AUDIO_LIVE_LED_DEBOUNCE_MS 400
    if (audio_live) {
      silence_since_ms = 0;
      if (g_bt_connected && !reported_audio_live) {
        send_control("AUDIO_LIVE");
        reported_audio_live = true;
      }
    } else {
      if (silence_since_ms == 0) silence_since_ms = now_ms;
      if (g_bt_connected && reported_audio_live &&
          now_ms - silence_since_ms >= AUDIO_LIVE_LED_DEBOUNCE_MS) {
        send_control("AUDIO_SILENCE");
        reported_audio_live = false;
      }
    }
    if (!g_bt_connected) {
      if (led_on) { digitalWrite(2, LOW); led_on = false; }
    } else if (audio_live) {
      if (now_ms - last_led_toggle_ms >= 500) {  // ~1Hz blink (500ms on/off)
        last_led_toggle_ms = now_ms;
        led_on = !led_on;
        digitalWrite(2, led_on ? HIGH : LOW);
      }
    } else {
      if (!led_on) { digitalWrite(2, HIGH); led_on = true; }
    }
  }

  feed_silence_if_no_real_audio();

#ifdef DIAG_LOOP_DRAIN
  // Arduino's own loop() already runs as a FreeRTOS task (created once at
  // boot regardless of anything we do), so draining the encode queue here
  // instead of via a dedicated xTaskCreate()'d task adds ZERO new tasks to
  // the system -- testing whether it's specifically the act of creating an
  // additional task that breaks BT connectability, or any deferred-encode
  // architecture at all.
  // NOTE: this is the build flag combo (DIAG_LOOP_DRAIN) actually flashed
  // to the real hardware as of this session -- the mono downmix below
  // MUST be applied here too, not just in encode_task() above, or it has
  // zero effect on the running firmware (encode_task() is dead code while
  // DIAG_LOOP_DRAIN is defined, since its xTaskCreatePinnedToCore() call
  // is compiled out by the #if guard above setup()'s encode_task creation).
  static uint8_t mono_buf_loop[PCM_SLOT_SIZE / 2];
  uint8_t idx;
  while (xQueueReceive(filled_slots, &idx, 0) == pdTRUE) {
    uint32_t t0 = micros();
    uint32_t mono_len = 0;
    downmix_stereo_to_mono(pcm_slots[idx].data, pcm_slots[idx].length,
                            mono_buf_loop, &mono_len);
    emit_mono(mono_buf_loop, mono_len);  // encode here, or ship PCM to the S3 (ENCODE_ON_S3)
    uint32_t elapsed = micros() - t0;
    if (elapsed > encode_us_max) encode_us_max = elapsed;
    encode_us_sum += elapsed;
    encode_us_count++;
    xQueueSend(free_slots, &idx, portMAX_DELAY);
#ifdef DIAG_FRAG_TRACE
    // Distinguishing "total free heap" from "largest contiguous free block"
    // directly tests the heap-fragmentation theory: if fragmentation is the
    // real mechanism behind the sustained-load packet_fragmenter crash, the
    // largest-block number should trend down over time even while the total
    // free number stays roughly flat.
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (now - last_print_ms >= 1000) {
      last_print_ms = now;
      uint32_t total_free = ESP.getFreeHeap();
      uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      // Framed 'C' message, not a raw printf: this Serial wire is also the
      // S3's audio link, and unframed text forces its parser (and the PC
      // logger) to resync.
      // total/min added 2026-09-24 so usage can be shown as % of the real
      // heap size, and min gives the true low-water (peak-load) mark.
      char frag_buf[128];
      snprintf(frag_buf, sizeof(frag_buf),
               "FRAG:free=%u,largest=%u,total8=%u,min8=%u,int_total=%u,int_free=%u",
               (unsigned)total_free, (unsigned)largest,
               (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      send_control(frag_buf, pdMS_TO_TICKS(20));
    }
#endif
  }
#endif
  // Confirmed by direct measurement with the pcm_drops counter added above:
  // real, ongoing drops during live playback (dozens per minute), not a
  // rare edge case. In the DIAG_LOOP_DRAIN architecture this delay is the
  // ONLY thing standing between "queue just went empty" and "check again"
  // -- every millisecond here is reaction time audio_data_callback doesn't
  // get if a burst of PCM arrives right after the drain loop above found
  // nothing to do. 5ms -> 1ms still yields to the idle/watchdog task (this
  // is vTaskDelay under the hood, not a busy-wait) while cutting worst-case
  // reaction lag 5x. Re-measure pcm_drops after this change before assuming
  // it was sufficient on its own.
  delay(1);
}
