/* Absolute minimum A2DP sink, no encoding, no AVRCP registration, no
 * custom protocol -- isolates whether SDP non-response is caused by
 * resource pressure from the rest of the sketch, or is more fundamental. */
#include "BluetoothA2DPSink.h"

BluetoothA2DPSink a2dp_sink;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("[trace] free heap at start: %u\n", ESP.getFreeHeap());

#ifndef DUMMY_MALLOC_KB
#define DUMMY_MALLOC_KB 0
#endif
#if DUMMY_MALLOC_KB > 0
  // Isolation test: eat DUMMY_MALLOC_KB of heap with a single allocation
  // that's never touched again (no encoder, no task, no timer/DMA setup --
  // nothing Shine's own .begin() does besides consuming heap). If BT
  // connectability degrades the same way as with the real Shine encoder,
  // that's strong evidence it's the heap deficit itself, not some other
  // side effect of Shine's initialization.
  void *eaten = malloc(DUMMY_MALLOC_KB * 1024);
  Serial.printf("[trace] dummy malloc of %dKB: %s\n", DUMMY_MALLOC_KB,
                eaten ? "OK" : "FAILED");
#endif

  a2dp_sink.start("ESP32-MP3-Test");
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  Serial.printf("[trace] free heap after a2dp start: %u\n", ESP.getFreeHeap());
  Serial.println("started");
}

void loop() {
  delay(1000);
}
