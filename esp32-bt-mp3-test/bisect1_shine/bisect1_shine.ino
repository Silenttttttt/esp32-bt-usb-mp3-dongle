#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Shine.h"
#include "BluetoothA2DPSink.h"

BluetoothA2DPSink a2dp_sink;

class NullSink : public Print {
 public:
  size_t write(uint8_t b) override { return 1; }
  size_t write(const uint8_t *data, size_t len) override { return len; }
};
NullSink null_sink;
MP3EncoderShine shine_encoder;
EncodedAudioStream mp3_out(&null_sink, &shine_encoder);

void audio_data_callback(const uint8_t *data, uint32_t length) {
  mp3_out.write(data, length);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("[trace] free heap at start: %u\n", ESP.getFreeHeap());
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.start("ESP32-MP3-Test");
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  Serial.printf("[trace] free heap after a2dp start: %u\n", ESP.getFreeHeap());
  AudioInfo info(44100, 2, 16);
  mp3_out.begin(info);
  Serial.printf("[trace] free heap after shine init: %u\n", ESP.getFreeHeap());
  Serial.println("started");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[trace] free heap: %u\n", ESP.getFreeHeap());
  }
}
