// SPDX-License-Identifier: MIT
// Minimal example: read the AMIS with an ESP32 (any variant, Arduino core). The reading head is on UART1,
// debug output on the USB/serial console. Adjust the pins to your board.
#include <Arduino.h>
#include <amis.h>

#ifndef AMIS_RX_PIN
#define AMIS_RX_PIN 17
#endif
#ifndef AMIS_TX_PIN
#define AMIS_TX_PIN 16
#endif

static amis::Reader reader;

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8E1, AMIS_RX_PIN, AMIS_TX_PIN);
  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    Serial.println("invalid key");
  }
}

void loop() {
  while (Serial1.available() > 0) {
    const uint8_t st = reader.push(static_cast<uint8_t>(Serial1.read()), millis());
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
