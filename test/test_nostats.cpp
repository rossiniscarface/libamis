// SPDX-License-Identifier: MIT
// Build check for -DLIBAMIS_NO_STATS (run by make -C test): the reader works the same without counters.
#include <stdio.h>
#include <string.h>

#include "amis.h"
#include "vectors.h"

#ifndef LIBAMIS_NO_STATS
#error "build with -DLIBAMIS_NO_STATS"
#endif

// stats() must not exist without counters
template <typename T>
static auto has_stats(int) -> decltype(static_cast<const T *>(nullptr)->stats(), true) { return true; }
template <typename T>
static bool has_stats(...) { return false; }

int main() {
  int fail = 0;
  amis::Reader r;
  r.set_key_hex(kTestKeyHex);
  uint8_t all = 0;
  int acks = 0;
  for (size_t i = 0; i < sizeof(kFrame1); i++) {
    const uint8_t st = r.push(kFrame1[i], static_cast<uint32_t>(i));
    if (st & amis::Reader::kSendAck) acks++;
    all |= st;
  }
  if (!(all & amis::Reader::kReading) || acks != 1 || r.reading().energy_import_wh != 12345678) fail++;
  r.push_error(500);  // still available, only not counted
  if (has_stats<amis::Reader>(0)) fail++;
  printf("LIBAMIS_NO_STATS: sizeof(Reader) = %u, %s\n", static_cast<unsigned>(sizeof(amis::Reader)),
         fail ? "FAILED" : "ok");
  return fail ? 1 : 0;
}
