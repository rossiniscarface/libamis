// SPDX-License-Identifier: MIT
// Minimal example: read the AMIS with an ESP8266 (Arduino). The reading head is on UART0 (GPIO1 TX / GPIO3 RX),
// debug output on UART1 (TX only, GPIO2) at 115200 baud.
//
// Note: on UART0 the boot ROM prints its banner (74880 Bd) on every reset, and the core prints crash dumps there.
// The meter (M-Bus master) ignores such bytes, but they are not E5. To keep them away from the head, call
// Serial.swap() after Serial.begin() and wire the head to GPIO15 (TX) / GPIO13 (RX); GPIO15 must be low at boot
// (most boards have a pull-down), and crash dumps still go to the original UART0 pins after swap().
#include <Arduino.h>
#include <amis.h>

static amis::Reader reader;

void setup() {
  Serial1.begin(115200);
  Serial.begin(9600, SERIAL_8E1);
  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    Serial1.println("invalid key");
  }
  reader.set_required_fields(amis::Reading::kAll);  // the AMIS always sends all values
}

void loop() {
  // Read what has arrived as one block. On the ESP8266 core only an overrun can be detected (hasRxError() is
  // also set by the RX timeout, see README "UART errors"): then drop the block.
  uint8_t buf[128];
  size_t n = 0;
  while (n < sizeof(buf) && Serial.available() > 0) buf[n++] = static_cast<uint8_t>(Serial.read());
  if (n == 0) return;
  if (Serial.hasOverrun()) {
    reader.push_error(millis());
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const uint8_t st = reader.push(buf[i], millis());
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
