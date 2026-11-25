// SPDX-License-Identifier: MIT
// libFuzzer target for amis::Reader:  make -C test fuzz
// Input byte 0 selects options, the rest is the received byte stream. 0xA5 also advances the clock by 600 ms
// (inter-byte timeout), 0x5A also reports a UART error before the byte.
#include <stddef.h>
#include <stdint.h>

#include "amis.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
  if (n < 1) return 0;
  amis::Reader r;
  if (!(d[0] & 2)) r.set_key_hex("00112233445566778899AABBCCDDEEFF");
  if (d[0] & 1) r.set_required_fields(amis::Reading::kAll);
  uint32_t t = 0;
  volatile uint32_t sink = 0;
  for (size_t i = 1; i < n; i++) {
    t += (d[i] == 0xA5) ? 600 : 1;
    if (d[i] == 0x5A) r.push_error(t);
    const uint8_t st = r.push(d[i], t);
    if (st & amis::Reader::kReading) sink = sink + r.reading().energy_import_wh + r.reading().time.raw[5];
    size_t pn = 0;
    const uint8_t *p = r.last_plaintext(&pn);
    for (size_t k = 0; k < pn; k++) sink = sink + p[k];  // must stay inside the frame buffer
    sink = sink + static_cast<uint8_t>(amis::error_str(r.last_error())[0]);
  }
  return 0;
}
