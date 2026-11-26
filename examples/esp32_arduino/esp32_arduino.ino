// SPDX-License-Identifier: MIT
// Minimal example: read the AMIS with an ESP32 (any variant, Arduino core). The reading head is on UART1,
// debug output on the USB/serial console. Check the pins for your board (set AMIS_RX_PIN / AMIS_TX_PIN).
#include <Arduino.h>
#include <amis.h>

// Defaults that avoid the flash, PSRAM and console pins of the dev kits
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define AMIS_DEFAULT_RX 5  // C3: 18-21 are USB/UART0, 12-17 flash; C6: 16/17 are UART0
#define AMIS_DEFAULT_TX 4
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define AMIS_DEFAULT_RX 18
#define AMIS_DEFAULT_TX 17
#else
#define AMIS_DEFAULT_RX 16  // classic ESP32 (not on WROVER modules: there 16/17 belong to the PSRAM)
#define AMIS_DEFAULT_TX 17
#endif
#ifndef AMIS_RX_PIN
#define AMIS_RX_PIN AMIS_DEFAULT_RX
#endif
#ifndef AMIS_TX_PIN
#define AMIS_TX_PIN AMIS_DEFAULT_TX
#endif

static amis::Reader reader;
// The error callback runs in the UART task: only set a flag there, libamis is called from loop() only
static volatile bool uart_error = false;

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8E1, AMIS_RX_PIN, AMIS_TX_PIN);
  Serial1.onReceiveError([](hardwareSerial_error_t) { uart_error = true; });  // parity, framing, overflow
  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    Serial.println("invalid key");
  }
  reader.set_required_fields(amis::Reading::kAll);  // the AMIS always sends all values
}

void loop() {
  uint8_t buf[128];
  size_t n = 0;
  while (n < sizeof(buf) && Serial1.available() > 0) buf[n++] = static_cast<uint8_t>(Serial1.read());
  if (uart_error) {  // drop the block with the damaged byte: no E5 for that frame
    uart_error = false;
    reader.push_error(millis());
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const uint8_t st = reader.push(buf[i], millis());
    if (st & amis::Reader::kSendAck) Serial1.write(amis::Reader::kAck);  // acknowledge immediately
    if (st & amis::Reader::kReading) {
      const amis::Reading &r = reader.reading();
      Serial.printf("%04u-%02u-%02u %02u:%02u:%02u  1.8.0=%lu Wh  2.8.0=%lu Wh  net=%ld W\n", r.time.year,
                    r.time.month, r.time.day, r.time.hour, r.time.minute, r.time.second,
                    (unsigned long)r.energy_import_wh, (unsigned long)r.energy_export_wh, (long)r.power_balance_w());
    }
    if (st & amis::Reader::kError) {
      Serial.printf("AMIS: %s\n", amis::error_str(reader.last_error()));
    }
  }
}
