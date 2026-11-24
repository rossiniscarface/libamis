// SPDX-License-Identifier: MIT
// libamis - internal: constant data in flash on the ESP8266 (there .rodata would otherwise live in RAM).
#pragma once

#include <stddef.h>
#include <string.h>

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#include <pgmspace.h>
#define AMIS_PROGMEM PROGMEM
#define AMIS_RD(p) pgm_read_byte(p)
#define AMIS_MEMCPY_P(d, s, n) memcpy_P((d), (s), (n))
#else
#define AMIS_PROGMEM
#define AMIS_RD(p) (*(p))
#define AMIS_MEMCPY_P(d, s, n) memcpy((d), (s), (n))
#endif

namespace amis {
namespace detail {

// Returns a text stored with AMIS_PROGMEM as a normal string. On the ESP8266 it is copied into one shared
// buffer (valid until the next call), elsewhere the pointer is returned unchanged.
inline const char *pgm_str(const char *p) {
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
  static char buf[40];
  strncpy_P(buf, p, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  return buf;
#else
  return p;
#endif
}

}  // namespace detail
}  // namespace amis
