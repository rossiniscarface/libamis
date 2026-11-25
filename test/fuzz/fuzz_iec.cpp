// SPDX-License-Identifier: MIT
// libFuzzer target for amis::IecReader:  make -C test fuzz
// Byte 0 selects options, byte 1 the length of a device address taken from the following bytes, the rest is
// the received byte stream. 0xFF advances the clock by 3 s, 0xFE by 800 ms (then poll() runs).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "amis.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
  if (n < 2) return 0;
  amis::IecReader r;
  r.set_max_baud_char((d[0] & 1) ? '6' : '0');
  r.set_send_ack(!(d[0] & 2));
  size_t i = 2;
  char addr[40];
  size_t an = d[1] % sizeof(addr);
  if (an > n - i) an = n - i;
  memcpy(addr, d + i, an);
  addr[an] = 0;
  i += an;
  r.set_address(addr);
  const amis::IecReader::Mode mode = (d[0] & 4) ? amis::IecReader::Mode::kIdentOnly : amis::IecReader::Mode::kReadout;
  uint32_t t = 0;
  r.begin(mode, t);
  volatile size_t sink = 0;
  for (; i < n; i++) {
    uint8_t a;
    if (d[i] == 0xFF || d[i] == 0xFE) {
      t += (d[i] == 0xFF) ? 3000 : 800;
      a = r.poll(t);
    } else {
      a = r.push(d[i], t++);
      a = static_cast<uint8_t>(a | r.poll(t));
    }
    size_t tn = 0;
    const uint8_t *tx = r.tx(&tn);
    for (size_t k = 0; k < tn; k++) sink = sink + tx[k];
    if (a & amis::IecReader::kRawLine) sink = sink + strlen(r.raw_line());
    if (a & amis::IecReader::kLine) {
      const amis::IecLine &l = r.line();
      sink = sink + strlen(l.obis) + strlen(l.value) + strlen(l.unit) + strlen(l.extra);
    }
    sink = sink + strlen(r.identification()) + strlen(r.serial_number()) + strlen(r.meter_error()) +
           strlen(r.manufacturer());
    if (a & (amis::IecReader::kDone | amis::IecReader::kFailed)) {
      sink = sink + strlen(amis::error_str(r.error()));
      r.begin(mode, t);
    }
  }
  return 0;
}
