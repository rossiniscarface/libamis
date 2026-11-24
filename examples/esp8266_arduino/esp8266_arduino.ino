// SPDX-License-Identifier: MIT
// Minimal example: read the AMIS with an ESP8266 (Arduino). The reading head is on UART0 (GPIO1 TX / GPIO3 RX),
// debug output on UART1 (TX only, GPIO2) at 115200 baud.
#include <Arduino.h>
#include <amis.h>

static amis::Reader reader;

void setup() {
  Serial1.begin(115200);
  Serial.begin(9600, SERIAL_8E1);
  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    Serial1.println("invalid key");
  }
}

void loop() {
  while (Serial.available() > 0) {
    const uint8_t st = reader.push(static_cast<uint8_t>(Serial.read()), millis());
    if (st & amis::Reader::kSendAck) Serial.write(amis::Reader::kAck);  // acknowledge immediately
    if (st & amis::Reader::kReading) {
      const amis::Reading &r = reader.reading();
      Serial1.printf("%04u-%02u-%02u %02u:%02u:%02u  1.8.0=%lu Wh  2.8.0=%lu Wh  P+=%lu W  P-=%lu W  net=%ld W\n",
                     r.time.year, r.time.month, r.time.day, r.time.hour, r.time.minute, r.time.second,
                     (unsigned long)r.energy_import_wh, (unsigned long)r.energy_export_wh,
                     (unsigned long)r.power_import_w, (unsigned long)r.power_export_w, (long)r.power_balance_w());
    }
    if (st & amis::Reader::kError) {
      Serial1.printf("AMIS: %s\n", amis::error_str(reader.last_error()));
    }
  }
}
