/* Stage 3 test: send a repeating test string out over UART2 (GPIO17=TX2)
 * at a slow, easy-to-bit-bang-receive baud rate, for the Digispark to pick
 * up on a plain GPIO pin. One direction only (ESP32 -> Digispark) for now. */

HardwareSerial ToDigispark(2); // UART2

void setup() {
  Serial.begin(115200);           // USB debug console (UART0)
  ToDigispark.begin(1200, SERIAL_8N1, -1, 18); // RX unused (-1), TX = GPIO18
  Serial.println("ESP32 serial test starting, sending on GPIO18 @ 1200 baud");
}

uint32_t counter = 0;

void loop() {
  char msg[32];
  int n = snprintf(msg, sizeof(msg), "HELLO-%05lu\n", (unsigned long)counter++);
  ToDigispark.write((const uint8_t*)msg, n);
  Serial.print("sent: ");
  Serial.print(msg);
  delay(1000);
}
