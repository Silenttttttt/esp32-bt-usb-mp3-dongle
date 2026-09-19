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
#include "BluetoothA2DPSink.h"

BluetoothA2DPSink a2dp_sink;

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
void send_control(const char *msg, TickType_t wait_ticks = portMAX_DELAY) {
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
MP3EncoderShine shine_encoder;
EncodedAudioStream mp3_out(&serial_sink, &shine_encoder);

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
// ~19KB, low enough that even basic BT paging broke. 4 slots at the real
// size is the minimum that's both correct and heap-safe.
#define PCM_SLOT_SIZE 4096
#define PCM_SLOT_COUNT 4

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

void audio_data_callback(const uint8_t *data, uint32_t length) {
  last_real_audio_ms = millis();
  g_audio_cb_count++;
#ifdef HEAP_TRACE
  if (length > max_chunk_seen) {
    max_chunk_seen = length;
    xSemaphoreTakeRecursive(serial_mutex, portMAX_DELAY);
    Serial.printf("[trace] new max PCM chunk length: %u\n", length);
    xSemaphoreGiveRecursive(serial_mutex);
  }
#endif
#ifdef DIAG_NO_ENCODE_TASK
  mp3_out.write(data, length);
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
  static uint32_t last_silence_ms = 0;
  uint32_t now = millis();
  // 150ms: comfortably longer than any real inter-chunk gap during active
  // playback (a single real PCM chunk covers only tens of ms), short
  // enough that a genuine pause is covered quickly, before the listener
  // notices a gap.
  if (now - last_real_audio_ms < 150) return;
  // ~23ms matches roughly one real PCM_SLOT_SIZE-sized chunk's duration
  // (4096 bytes stereo 16-bit @ 44.1kHz), so synthetic silence arrives at
  // the same cadence real audio would, keeping the downstream byte rate
  // consistent with what the rest of the pipeline already assumes.
  if (now - last_silence_ms < 23) return;
  last_silence_ms = now;
  uint8_t idx;
  if (xQueueReceive(free_slots, &idx, 0) != pdTRUE) return;  // no free slot, skip this tick
  memset(pcm_slots[idx].data, 0, PCM_SLOT_SIZE);
  pcm_slots[idx].length = PCM_SLOT_SIZE;
  xQueueSend(filled_slots, &idx, 0);
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
void downmix_stereo_to_mono(const uint8_t *stereo, uint32_t stereo_len,
                             uint8_t *mono_out, uint32_t *mono_len) {
  const int16_t *in = (const int16_t *)stereo;
  int16_t *out = (int16_t *)mono_out;
  uint32_t pairs = stereo_len / 4;  // 4 bytes = 1 stereo sample pair (L+R, 16-bit each)
  for (uint32_t i = 0; i < pairs; i++) {
    int32_t l = in[2 * i];
    int32_t r = in[2 * i + 1];
    out[i] = (int16_t)((l + r) / 2);
  }
  *mono_len = pairs * 2;  // 2 bytes per mono sample
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
      mp3_out.write(mono_buf, mono_len);
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
void connection_state_changed(esp_a2d_connection_state_t state, void *) {
  // LED2 itself is now driven continuously from loop() (see its LED-status
  // block) so it can distinguish live-audio blinking from silence-solid
  // while connected -- this callback only updates the flag loop() reads,
  // except on disconnect, where it forces the pin off immediately rather
  // than waiting for loop()'s next pass (which would in practice be within
  // ~1ms anyway, but zero-risk to just do it here too).
  g_bt_connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  if (!g_bt_connected) digitalWrite(2, LOW);
  send_control(state == ESP_A2D_CONNECTION_STATE_CONNECTED ? "BT_CONNECTED" : "BT_DISCONNECTED",
               pdMS_TO_TICKS(20));
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
void avrc_playstatus_callback(esp_avrc_playback_stat_t playback) {
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
  const char *label;
  char label_buf[16];
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:  label = "TITLE:"; break;
    case ESP_AVRC_MD_ATTR_ARTIST: label = "ARTIST:"; break;
    case ESP_AVRC_MD_ATTR_ALBUM:  label = "ALBUM:"; break;
    default:
      snprintf(label_buf, sizeof(label_buf), "META%u:", id);
      label = label_buf;
      break;
  }
  char buf[160];
  snprintf(buf, sizeof(buf), "%s%s", label, (const char *)text);
  send_control(buf, pdMS_TO_TICKS(20));
}

void setup() {
  pinMode(2, OUTPUT);
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
  Serial.begin(921600);
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
  mp3_out.begin(info);
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
  // ALBUM dropped deliberately: every packet_fragmenter.c crash tonight
  // correlated with large multi-attribute AVRCP metadata responses (real
  // titles observed 100+ chars), while a long metadata-free run (547s) and
  // the longest run yet (780s) had zero crashes. ALBUM is also the least
  // useful field for a car-radio display. Requesting one fewer attribute
  // shrinks the combined response size and fragment count without losing
  // TITLE/ARTIST, the two that actually matter here.
  a2dp_sink.set_avrc_metadata_attribute_mask(
      ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_playstatus_callback(avrc_playstatus_callback);
#endif
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
  // driving. The library already supports this properly: passing true
  // here enables set_auto_reconnect(true, AUTOCONNECT_TRY_NUM=1000), which
  // persists the last-connected address to NVS (survives reboots) and
  // retries connecting to it automatically (1s between attempts, per
  // BluetoothA2DPSink.h) without any phone-side action needed. NOT YET
  // VERIFIED on real hardware -- needs one real reconnect-after-reset test.
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_on_audio_state_changed(audio_state_changed);
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.start("ESP32-MP3-Test", true);
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);

#ifdef HEAP_TRACE
  Serial.printf("[trace] free heap after a2dp start: %u\n", ESP.getFreeHeap());
#endif

  send_control("READY");
}

void loop() {
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
    snprintf(buf, sizeof(buf), "AUDIO_CB_STATUS:count=%lu,ms_since_last=%lu",
             (unsigned long)g_audio_cb_count,
             (unsigned long)(now_ms - last_real_audio_ms));
    send_control(buf);
    char buf2[100];
    snprintf(buf2, sizeof(buf2),
             "WRITE_SIZES:last8=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,non_ff_start=%lu/%lu",
             (unsigned long)g_write_sizes[0], (unsigned long)g_write_sizes[1],
             (unsigned long)g_write_sizes[2], (unsigned long)g_write_sizes[3],
             (unsigned long)g_write_sizes[4], (unsigned long)g_write_sizes[5],
             (unsigned long)g_write_sizes[6], (unsigned long)g_write_sizes[7],
             (unsigned long)g_non_ff_start_count, (unsigned long)g_write_count);
    send_control(buf2);
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
    static bool prev_audio_live = false;
    bool audio_live = (now_ms - last_real_audio_ms < 150);
    if (g_bt_connected && audio_live != prev_audio_live) {
      send_control(audio_live ? "AUDIO_LIVE" : "AUDIO_SILENCE");
      prev_audio_live = audio_live;
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
    mp3_out.write(mono_buf_loop, mono_len);
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
      xSemaphoreTakeRecursive(serial_mutex, portMAX_DELAY);
      Serial.printf("[frag] t=%lu total_free=%u largest_block=%u\n",
                    (unsigned long)now, (unsigned)total_free, (unsigned)largest);
      xSemaphoreGiveRecursive(serial_mutex);
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
