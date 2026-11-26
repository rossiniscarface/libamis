// SPDX-License-Identifier: MIT
// Host tests:  make -C test   (or: g++ -std=c++17 -Wall -Wextra -I../src ../src/*.cpp test_main.cpp)
#include <stdio.h>
#include <string.h>

#include "amis.h"
#include "vectors.h"

static int g_fail = 0, g_ok = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      g_ok++;                                                           \
    } else {                                                            \
      g_fail++;                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                   \
  } while (0)

static void hex2bin(const char *h, uint8_t *out) {
  for (size_t i = 0; h[2 * i]; i++) {
    unsigned v;
    sscanf(h + 2 * i, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

// FIPS-197 appendix C.1 and NIST SP 800-38A F.2.1/F.2.2 (CBC-AES128)
static void test_aes() {
  amis::Aes128 aes;
  uint8_t key[16], blk[16], exp[16];
  hex2bin("000102030405060708090a0b0c0d0e0f", key);
  hex2bin("00112233445566778899aabbccddeeff", blk);
  hex2bin("69c4e0d86a7b0430d8cdb78070b4c55a", exp);
  aes.set_key(key);
  aes.encrypt_block(blk);
  CHECK(memcmp(blk, exp, 16) == 0);
  aes.decrypt_block(blk);
  hex2bin("00112233445566778899aabbccddeeff", exp);
  CHECK(memcmp(blk, exp, 16) == 0);

  uint8_t iv[16], pt[64], ct[64], buf[64];
  hex2bin("2b7e151628aed2a6abf7158809cf4f3c", key);
  hex2bin("000102030405060708090a0b0c0d0e0f", iv);
  hex2bin("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
          "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", pt);
  hex2bin("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
          "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7", ct);
  aes.set_key(key);
  memcpy(buf, pt, 64);
  CHECK(aes.cbc_encrypt(buf, 64, iv));
  CHECK(memcmp(buf, ct, 64) == 0);
  CHECK(aes.cbc_decrypt(buf, 64, iv));
  CHECK(memcmp(buf, pt, 64) == 0);
  CHECK(!aes.cbc_decrypt(buf, 15, iv));
}

static void test_datetime() {
  // Examples from real frames: Sat 2026-03-28 09:20:15 (CET) and Sun 2026-03-29 23:57:55 (CEST)
  amis::DateTime t;
  const uint8_t w[6] = {0x0F, 0x14, 0xC9, 0x5C, 0x33, 0x0D};
  CHECK(t.decode_type_i(w));
  CHECK(t.year == 2026 && t.month == 3 && t.day == 28 && t.hour == 9 && t.minute == 20 && t.second == 15);
  CHECK(t.weekday == 6 && t.week == 13 && !t.dst);
  CHECK(memcmp(t.raw, w, 6) == 0);  // raw bytes kept (A13)
  CHECK(t.to_unix_utc() == 1774686015LL);  // 2026-03-28T08:20:15Z
  const uint8_t s[6] = {0x77, 0x39, 0xF7, 0x5D, 0x33, 0x0D};
  CHECK(t.decode_type_i(s));
  CHECK(t.year == 2026 && t.month == 3 && t.day == 29 && t.hour == 23 && t.minute == 57 && t.second == 55);
  CHECK(t.weekday == 7 && t.dst);
  CHECK(t.to_unix_utc() == 1774821475LL);  // 2026-03-29T21:57:55Z
  const uint8_t bad[6] = {0x0F, 0x94, 0xC9, 0x5C, 0x33, 0x0D};  // "invalid" bit set
  CHECK(!t.decode_type_i(bad));
}

static uint8_t feed(amis::Reader &r, const uint8_t *d, size_t n, uint32_t t0, int *acks) {
  uint8_t all = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t st = r.push(d[i], t0 + static_cast<uint32_t>(i));  // 1 ms per byte (9600 Bd)
    if (st & amis::Reader::kSendAck) (*acks)++;
    all |= st;
  }
  return all;
}

static void test_frames() {
  amis::Reader r;
  int acks = 0;
  CHECK(!r.set_key_hex("1234"));
  CHECK(r.set_key_hex(kTestKeyHex));

  // search request -> E5
  uint8_t st = feed(r, kSndNke, sizeof(kSndNke), 0, &acks);
  CHECK(st == amis::Reader::kSendAck && acks == 1);

  // garbage before the frame, then frame 1
  const uint8_t junk[] = {0x00, 0xE5, 0x68, 0x12, 0x99, 0x10, 0x40};
  feed(r, junk, sizeof(junk), 1000, &acks);
  acks = 0;
  st = feed(r, kFrame1, sizeof(kFrame1), 2000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 1);
  const amis::Reading &x = r.reading();
  CHECK(x.energy_import_wh == 12345678 && x.energy_export_wh == 2345678);
  CHECK(x.reactive_energy_import_varh == 345678 && x.reactive_energy_export_varh == 45678);
  CHECK(x.power_import_w == 1234 && x.power_export_w == 0 && x.power_balance_w() == 1234);
  CHECK(x.reactive_power_import_var == 210 && x.reactive_power_export_var == 30);
  CHECK(x.inkasso == -42);
  CHECK(x.present == 0x3FF);
  CHECK(x.time.valid && x.time.hour == 9 && x.time.minute == 20 && !x.time.dst);
  CHECK(x.header_id == 0 && x.manufacturer == 0x4C2D && x.access_number == 0x21);
  CHECK(r.online(2200) && !r.online(9000));

  // Same frame again (our E5 got lost): acknowledge again, but do not decode twice
  acks = 0;
  st = feed(r, kFrame2, 0, 3000, &acks);
  st = feed(r, kFrame1, sizeof(kFrame1), 3000, &acks);
  CHECK(st == amis::Reader::kSendAck && acks == 1 && r.stats().duplicates == 1);

  // Frame 2 (FCB toggled, DST, feeding into the grid)
  acks = 0;
  st = feed(r, kFrame2, sizeof(kFrame2), 4000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 1);
  CHECK(r.reading().power_export_w == 3456 && r.reading().power_balance_w() == -3456);
  CHECK(r.reading().time.dst && r.reading().inkasso == 7);

  // Two frames back to back -> two acknowledgements
  amis::Reader r2;
  r2.set_key_hex(kTestKeyHex);
  uint8_t both[sizeof(kFrame1) + sizeof(kFrame2)];
  memcpy(both, kFrame1, sizeof(kFrame1));
  memcpy(both + sizeof(kFrame1), kFrame2, sizeof(kFrame2));
  acks = 0;
  feed(r2, both, sizeof(both), 0, &acks);
  CHECK(acks == 2 && r2.stats().readings == 2);

  // Wrong key: frame is acknowledged, error kWrongKey
  amis::Reader r3;
  r3.set_key_hex("FFEEDDCCBBAA99887766554433221100");
  acks = 0;
  st = feed(r3, kFrame1, sizeof(kFrame1), 0, &acks);
  CHECK((st & amis::Reader::kError) && acks == 1 && r3.last_error() == amis::Error::kWrongKey);
  CHECK(!(st & amis::Reader::kReading));

  // No key
  amis::Reader r4;
  acks = 0;
  st = feed(r4, kFrame1, sizeof(kFrame1), 0, &acks);
  CHECK((st & amis::Reader::kError) && r4.last_error() == amis::Error::kNoKey);

  // Broken checksum: no E5
  amis::Reader r5;
  r5.set_key_hex(kTestKeyHex);
  uint8_t bad[sizeof(kFrame1)];
  memcpy(bad, kFrame1, sizeof(bad));
  bad[50] ^= 0x01;
  acks = 0;
  st = feed(r5, bad, sizeof(bad), 0, &acks);
  CHECK(st == amis::Reader::kError && acks == 0 && r5.last_error() == amis::Error::kChecksum);
  // ... the next frame works again
  st = feed(r5, kFrame1, sizeof(kFrame1), 1000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 1);

  // Gap in the middle of a frame (> 500 ms) -> dropped, the next complete frame counts
  amis::Reader r6;
  r6.set_key_hex(kTestKeyHex);
  acks = 0;
  feed(r6, kFrame1, 40, 0, &acks);
  st = feed(r6, kFrame2, sizeof(kFrame2), 1000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 1 && r6.reading().access_number == 0x22);
  CHECK(r6.stats().resyncs >= 1);

  // Long frame header with L = 250 (longer than the AMIS ever sends, longer than a small LIBAMIS_MAX_FRAME):
  // must not overflow and must not block the next frame
  amis::Reader r7;
  r7.set_key_hex(kTestKeyHex);
  uint8_t big[260];
  memset(big, 0x55, sizeof(big));
  big[0] = 0x68; big[1] = 250; big[2] = 250; big[3] = 0x68;
  acks = 0;
  feed(r7, big, 4, 0, &acks);
  if (amis::Reader::kMaxFrame < 256) CHECK(r7.stats().resyncs == 1);  // dropped right after the header
  feed(r7, big + 4, sizeof(big) - 4, 4, &acks);
  st = feed(r7, kFrame1, sizeof(kFrame1), 1000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 1 && r7.reading().energy_import_wh == 12345678);

  // The plaintext is valid until the next push() and is dropped when a new frame starts
  size_t pn = 0;
  CHECK(r7.last_plaintext(&pn)[0] == 0x2F && pn == 80);
  r7.push(0x68, 2000);
  r7.last_plaintext(&pn);
  CHECK(pn == 0);

  // A copy must keep its own plaintext
  amis::Reader r8;
  r8.set_key_hex(kTestKeyHex);
  feed(r8, kFrame2, sizeof(kFrame2), 0, &acks);
  amis::Reader r9 = r8;
  r8.push(0x68, 100);
  size_t n9 = 0;
  const uint8_t *p9 = r9.last_plaintext(&n9);
  CHECK(n9 == 80 && p9[0] == 0x2F && p9[2] == 0x06);
}

// Example from the Netz OOe specification "AMIS-Zaehler Kundenschnittstelle" (2021-11-05), section 2.4.2.9:
// telegram, key and expected values exactly as printed there.
static void test_netzooe_example() {
  const char *hex =
      "685F5F6853F05B000000002D4C010E0D0050053FD0FEB726760CC7AAF0B52B41F0C541BD6306DCD8B91B3DA2311EF13D2514D0960082161E"
      "FEC4B6CB1E0B3328BE6177DCA594C1280024A835F1D655BA7182B256E94BD33AC0A6B08DA46781EB4E91E01216";
  uint8_t f[101];
  CHECK(strlen(hex) == 202);
  hex2bin(hex, f);
  amis::Reader r;
  CHECK(r.set_key_hex("00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF"));
  int acks = 0;
  const uint8_t nke[] = {0x10, 0x40, 0xF0, 0x30, 0x16};  // section 2.4.1.3
  feed(r, nke, sizeof(nke), 0, &acks);
  const uint8_t st = feed(r, f, sizeof(f), 1000, &acks);
  CHECK((st & amis::Reader::kReading) && acks == 2);
  const amis::Reading &x = r.reading();
  CHECK(x.time.valid && x.time.year == 2014 && x.time.month == 7 && x.time.day == 1);
  CHECK(x.time.hour == 8 && x.time.minute == 12 && x.time.second == 31 && x.time.dst);
  CHECK(x.energy_import_wh == 684544 && x.energy_export_wh == 129412);
  CHECK(x.reactive_energy_import_varh == 357918 && x.reactive_energy_export_varh == 81446);
  CHECK(x.power_import_w == 0 && x.power_export_w == 117);
  CHECK(x.reactive_power_import_var == 0 && x.reactive_power_export_var == 0);
  CHECK(x.inkasso == 20);
  CHECK(x.present == 0x3FF && x.access_number == 0x0D && x.manufacturer == 0x4C2D);
  CHECK(x.header_id == 0 && x.version == 0x01 && x.medium == 0x0E);  // reading head, Netz OOe 2.4.2.6
  const uint8_t traw[6] = {0x5F, 0x0C, 0x48, 0xC1, 0x17, 0x1B};  // section 2.4.2.9
  CHECK(memcmp(x.time.raw, traw, 6) == 0);
  size_t n = 0;
  const uint8_t *p = r.last_plaintext(&n);
  const uint8_t head[] = {0x2F, 0x2F, 0x06, 0x6D, 0x5F, 0x0C, 0x48, 0xC1, 0x17, 0x1B};  // section 2.4.2.9
  CHECK(n == 80 && memcmp(p, head, sizeof(head)) == 0);
}

// ---- IEC 62056-21 (optional) with a simulated meter ----
static uint8_t iec_feed(amis::IecReader &r, const char *s, uint32_t *t) {
  uint8_t all = 0;
  for (; *s; s++) all |= r.push(static_cast<uint8_t>(*s), (*t)++);
  return all;
}

static void test_iec() {
  amis::IecReader r;
  uint32_t t = 0;
  size_t n = 0;
  CHECK(!r.active());

  // Data readout: request at 300 Bd
  uint8_t a = r.begin(amis::IecReader::Mode::kReadout, t);
  CHECK(a == amis::IecReader::kSetSerial && r.baud() == 300);  // first switch to 300 Bd ...
  CHECK(r.poll(t) == amis::IecReader::kSend);                    // ... then send the request
  const uint8_t *tx = r.tx(&n);
  CHECK(n == 5 && memcmp(tx, "/?!\r\n", 5) == 0);
  CHECK(r.begin(amis::IecReader::Mode::kReadout, t) == amis::IecReader::kNone);  // already running: no change
  CHECK(r.active() && r.error() == amis::IecReader::Error::kNone);

  r = amis::IecReader();
  r.set_max_baud_char('6');
  r.begin(amis::IecReader::Mode::kReadout, t);
  // Echo of our own request is ignored, then identification (parity bit set on '/' -> 0xAF)
  iec_feed(r, "/?!\r\n", &t);
  CHECK(r.push(0xAF, t++) == 0);
  CHECK(iec_feed(r, "SAT6AMIS2351102623000045\r\n", &t) == 0);
  CHECK(strcmp(r.manufacturer(), "SAT") == 0 && r.baud_char() == '6');
  CHECK(strcmp(r.identification(), "AMIS2351102623000045") == 0);
  // Reply only after >= 200 ms. Z must be the meter's code '6' (allowed with max '6') -> 19200 Bd
  CHECK(r.poll(t + 100) == 0);
  CHECK(r.poll(t + 600) == 0);  // default reply delay 700 ms
  t += 800;
  a = r.poll(t);
  CHECK(a == (amis::IecReader::kSend | amis::IecReader::kSetSerial) && r.baud() == 19200);
  tx = r.tx(&n);
  CHECK(n == 6 && memcmp(tx, "\x06" "060\r\n", 6) == 0);

  // Data block with BCC (XOR over everything after STX up to and including ETX)
  const char *block =
      "F.F(00000000)\r\n0.0.0(12345678)\r\n1.8.0(0684.544*kWh)\r\n1.6.0(01.234*kW)(2611231445)\r\n!\r\n\x03";
  uint8_t bcc = 0;
  for (const char *p = block; *p; p++) bcc ^= static_cast<uint8_t>(*p);
  CHECK(r.push(0x02, t++) == 0);  // STX
  int lines = 0, raw_lines = 0;
  bool saw_160 = false, saw_180 = false, saw_raw_160 = false, saw_raw_end = false;
  for (const char *p = block; *p; p++) {
    a = r.push(static_cast<uint8_t>(*p), t++);
    if (a & amis::IecReader::kRawLine) {
      raw_lines++;
      if (!strcmp(r.raw_line(), "1.6.0(01.234*kW)(2611231445)")) saw_raw_160 = true;
      if (!strcmp(r.raw_line(), "!")) saw_raw_end = true;
    }
    if (a & amis::IecReader::kLine) {
      lines++;
      const amis::IecLine &l = r.line();
      if (!strcmp(l.obis, "1.8.0")) saw_180 = !strcmp(l.value, "0684.544") && !strcmp(l.unit, "kWh");
      if (!strcmp(l.obis, "1.6.0")) saw_160 = !strcmp(l.value, "01.234") && !strcmp(l.extra, "2611231445");
    }
  }
  CHECK(lines == 4 && saw_160 && saw_180);
  CHECK(raw_lines == 5 && saw_raw_160 && saw_raw_end);
  CHECK(r.push(bcc, t++) == amis::IecReader::kDone && !r.active());
  CHECK(strcmp(r.serial_number(), "12345678") == 0);

  // Wrong block check character
  amis::IecReader r2;
  t = 0;
  r2.set_max_baud_char('0');
  r2.begin(amis::IecReader::Mode::kReadout, t);
  iec_feed(r2, "/SAT6X\r\n", &t);
  t += 800;
  a = r2.poll(t);
  CHECK(a == amis::IecReader::kSend && r2.baud() == 300);  // stays at 300 Bd, no switch
  tx = r2.tx(&n);
  CHECK(n == 6 && tx[2] == '0');
  r2.push(0x02, t++);
  iec_feed(r2, "0.0.0(1)\r\n!\r\n\x03", &t);
  CHECK(r2.push(0x00, t++) == amis::IecReader::kFailed && r2.error() == amis::IecReader::Error::kBcc);

  // Identification only: break instead of data readout
  amis::IecReader r3;
  t = 0;
  r3.begin(amis::IecReader::Mode::kIdentOnly, t);
  iec_feed(r3, "/SAT6123456789\r\n", &t);
  CHECK(r3.poll(t) == 0);
  t += 800;
  a = r3.poll(t);
  CHECK(a == (amis::IecReader::kSend | amis::IecReader::kDone) && !r3.active());
  tx = r3.tx(&n);
  const uint8_t brk[5] = {0x01, 'B', '0', 0x03, 'B' ^ '0' ^ 0x03};
  CHECK(n == 5 && memcmp(tx, brk, 5) == 0);
  CHECK(strcmp(r3.serial_number(), "123456789") == 0);

  // Meter does not answer
  amis::IecReader r4;
  r4.set_address("2351102623000045");
  r4.begin(amis::IecReader::Mode::kIdentOnly, 0);
  tx = r4.tx(&n);
  CHECK(n == 21 && memcmp(tx, "/?2351102623000045!\r\n", 21) == 0);
  CHECK(r4.poll(0) == amis::IecReader::kSend);
  // The request is sent 4 times in total, every 5 s (manual 5.2.12.2), then the reader gives up 2.5 s later
  CHECK(r4.poll(4900) == 0);
  CHECK(r4.poll(5100) == amis::IecReader::kSend);
  tx = r4.tx(&n);
  CHECK(n == 21 && tx[0] == '/');
  CHECK(r4.poll(10200) == amis::IecReader::kSend);
  CHECK(r4.poll(15300) == amis::IecReader::kSend);
  CHECK(r4.poll(17700) == 0 && r4.active());
  CHECK(r4.poll(17900) == amis::IecReader::kFailed && r4.error() == amis::IecReader::Error::kNoIdent);

  // Interval can be set; an incomplete answer is dropped and the request sent again
  amis::IecReader r4b;
  r4b.set_request_interval_ms(2000);
  r4b.begin(amis::IecReader::Mode::kIdentOnly, 0);
  r4b.poll(0);
  t = 100;
  iec_feed(r4b, "/SAT6123", &t);
  CHECK(r4b.poll(2200) == amis::IecReader::kSend);
  iec_feed(r4b, "/SAT6ABC\r\n", &t);
  CHECK(strcmp(r4b.identification(), "ABC") == 0);

  // Default baud rate: the meter reports '6' but the default max is '0' -> "ACK 0 0 0", stays at 300 Bd.
  // Something in between (e.g. '5') would never be used: the meter only switches if Z matches.
  const char maxzs[2] = {'\0', '5'};
  for (char maxz : maxzs) {
    amis::IecReader r5;
    t = 0;
    if (maxz) r5.set_max_baud_char(maxz);
    r5.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r5, "/SAT6X\r\n", &t);
    t += 800;
    a = r5.poll(t);
    tx = r5.tx(&n);
    CHECK(a == amis::IecReader::kSend && r5.baud() == 300 && n == 6 && memcmp(tx, "\x06" "000\r\n", 6) == 0);
  }

  // Error telegram from the meter: STX (ER0007) ETX BCC, without CR LF before ETX
  amis::IecReader r6;
  r6.set_ack_tries(1);
  t = 0;
  r6.begin(amis::IecReader::Mode::kReadout, t);
  iec_feed(r6, "/SAT6X\r\n", &t);
  t += 800;
  r6.poll(t);
  r6.push(0x02, t++);
  a = iec_feed(r6, "(ER0007)\x03", &t);
  CHECK(!(a & amis::IecReader::kRawLine));  // an error telegram is no data line
  const uint8_t bcc6 = static_cast<uint8_t>('(' ^ 'E' ^ 'R' ^ '0' ^ '0' ^ '0' ^ '7' ^ ')' ^ 0x03);
  CHECK(r6.push(bcc6, t++) == amis::IecReader::kFailed && r6.error() == amis::IecReader::Error::kMeterError);
  CHECK(!strcmp(r6.meter_error(), "ER0007"));

  // ... and without brackets, as a real TD-351x sends it: STX ER0007 ETX BCC
  amis::IecReader r6b;
  r6b.set_ack_tries(1);
  t = 0;
  r6b.begin(amis::IecReader::Mode::kReadout, t);
  iec_feed(r6b, "/SAT6X\r\n", &t);
  t += 800;
  r6b.poll(t);
  r6b.push(0x02, t++);
  a = iec_feed(r6b, "ER0007\x03", &t);
  CHECK(!(a & amis::IecReader::kRawLine));
  const uint8_t bcc6b = static_cast<uint8_t>('E' ^ 'R' ^ '0' ^ '0' ^ '0' ^ '7' ^ 0x03);
  CHECK(r6b.push(bcc6b, t++) == amis::IecReader::kFailed && !strcmp(r6b.meter_error(), "ER0007"));

  // Real TD-351x: the first acknowledgement is answered with ER0007, the repeated one (7 s later) with the data
  {
    amis::IecReader r9;
    t = 0;
    r9.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r9, "/SAT6TEST000000000001\r\n", &t);
    t += 800;
    CHECK(r9.poll(t) == amis::IecReader::kSend);  // ACK 0 0 0
    const uint32_t t_ack = t;
    r9.push(0x02, t++);
    iec_feed(r9, "ER0007\x03", &t);
    a = r9.push(static_cast<uint8_t>('E' ^ 'R' ^ '0' ^ '0' ^ '0' ^ '7' ^ 0x03), t++);
    CHECK(a == 0 && r9.active());
    CHECK(r9.poll(t_ack + 6900) == 0);
    CHECK(r9.poll(t_ack + 7100) == amis::IecReader::kSend);  // ACK again
    tx = r9.tx(&n);
    CHECK(n == 6 && tx[0] == 0x06);
    t = t_ack + 8000;
    r9.push(0x02, t++);  // (test 1: with STX)
    int raw = 0;
    const char *data = "F.F(00000000)\r\n0.0.0(003080207)(00-00-00 00:00)\r\n\x03";
    uint8_t bcc9 = 0;
    for (const char *p = data; *p; p++) {
      bcc9 ^= static_cast<uint8_t>(*p);
      if (r9.push(static_cast<uint8_t>(*p), t++) & amis::IecReader::kRawLine) raw++;
    }
    CHECK(raw == 2 && r9.push(bcc9, t++) == amis::IecReader::kDone && !strcmp(r9.serial_number(), "003080207"));
  }
  // As captured on a real TD-3511: after the repeated acknowledgement the data comes WITHOUT STX,
  // starting with "F.F(", ending with ETX and a byte that is no valid BCC for that block
  {
    amis::IecReader r11;
    t = 0;
    r11.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r11, "/SAT6TEST000000000001\r\n", &t);
    t += 800;
    CHECK(r11.poll(t) == amis::IecReader::kSend);
    const uint32_t t_ack = t;
    r11.push(0x02, t++);
    iec_feed(r11, "ER0007\x03\x13", &t);  // BCC 0x13 as sent by the meter
    CHECK(r11.active() && r11.poll(t_ack + 7100) == amis::IecReader::kSend);
    t = t_ack + 7500;
    int raw = 0;
    bool saw_ff = false;
    for (const char *p = "F.F(00000000)\r\n0.0.0(003080207)(00-00-00 00:00)\r\n\x03"; *p; p++) {
      if (r11.push(static_cast<uint8_t>(*p), t++) & amis::IecReader::kRawLine) {
        raw++;
        if (!strcmp(r11.raw_line(), "F.F(00000000)")) saw_ff = true;
      }
    }
    CHECK(raw == 2 && saw_ff);
    CHECK(r11.push(0x55, t++) == amis::IecReader::kDone);  // any byte after ETX: no BCC check without STX
    CHECK(r11.poll(t + 8000) == 0 && !r11.active());       // and no further acknowledgement
  }

  // No data after the acknowledgement: it is repeated every 7 s, three times in all, then timeout
  {
    amis::IecReader r10;
    t = 0;
    r10.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r10, "/SAT6X\r\n", &t);
    t += 800;
    CHECK(r10.poll(t) == amis::IecReader::kSend);
    CHECK(r10.poll(t + 7100) == amis::IecReader::kSend);
    CHECK(r10.poll(t + 14200) == amis::IecReader::kSend);
    CHECK(r10.poll(t + 21300) == amis::IecReader::kFailed && r10.error() == amis::IecReader::Error::kTimeout);
  }

  // Repeated acknowledgement after a switch to a faster baud rate: back to 300 Bd first, then ACK 0 0 0
  {
    amis::IecReader r12;
    t = 0;
    r12.set_max_baud_char('6');
    r12.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r12, "/SAT6X\r\n", &t);
    t += 800;
    CHECK(r12.poll(t) == (amis::IecReader::kSend | amis::IecReader::kSetSerial) && r12.baud() == 19200);
    const uint32_t t_ack = t;
    CHECK(r12.poll(t_ack + 7100) == amis::IecReader::kSetSerial && r12.baud() == 300);  // no data: UART to 300
    CHECK(r12.poll(t_ack + 7101) == amis::IecReader::kSend);                            // then send, no switch
    tx = r12.tx(&n);
    CHECK(n == 6 && memcmp(tx, "\x06" "000\r\n", 6) == 0);
    t = t_ack + 7200;
    r12.push(0x02, t++);
    iec_feed(r12, "0.0.0(7)\r\n\x03", &t);
    CHECK(r12.push(static_cast<uint8_t>('0' ^ '.' ^ '0' ^ '.' ^ '0' ^ '(' ^ '7' ^ ')' ^ '\r' ^ '\n' ^ 0x03), t++) ==
          amis::IecReader::kDone);
  }
  // ... and the same after an error telegram (the meter answers a refused acknowledgement at 300 Bd)
  {
    amis::IecReader r13;
    t = 0;
    r13.set_max_baud_char('6');
    r13.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r13, "/SAT6X\r\n", &t);
    t += 800;
    CHECK(r13.poll(t) == (amis::IecReader::kSend | amis::IecReader::kSetSerial));
    const uint32_t t_ack = t;
    r13.push(0x02, t++);
    iec_feed(r13, "ER0007\x03\x13", &t);
    CHECK(r13.poll(t_ack + 7100) == amis::IecReader::kSetSerial && r13.baud() == 300);
    CHECK(r13.poll(t_ack + 7101) == amis::IecReader::kSend && r13.tx(&n)[2] == '0');
  }

  // Echo of our acknowledgement and STX-less data before a repeated acknowledgement are no data
  {
    amis::IecReader r14;
    t = 0;
    r14.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r14, "/SAT6X\r\n", &t);
    t += 800;
    CHECK(r14.poll(t) == amis::IecReader::kSend);
    const uint32_t t_ack = t;
    CHECK(iec_feed(r14, "\x06" "000\r\n", &t) == 0);  // echo
    CHECK(iec_feed(r14, "1.8.0(1)\r\n", &t) == 0);     // no STX after the first acknowledgement
    CHECK(r14.poll(t_ack + 7100) == amis::IecReader::kSend);
    t = t_ack + 7200;
    CHECK(iec_feed(r14, "\x06" "000\r\n", &t) == 0);  // echo of the repeated one
    CHECK(iec_feed(r14, "F.F(00)\r\n", &t) & amis::IecReader::kRawLine);
    CHECK(!strcmp(r14.raw_line(), "F.F(00)"));
  }

  // An error telegram ended by CR LF and followed by data: the data lines count, the error is dropped
  {
    amis::IecReader r15;
    t = 0;
    r15.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r15, "/SAT6X\r\n", &t);
    t += 800;
    r15.poll(t);
    r15.push(0x02, t++);
    CHECK((iec_feed(r15, "ER0007\r\n", &t) & amis::IecReader::kRawLine) == 0);
    int raw = 0;
    for (const char *p = "F.F(00)\r\n0.0.0(9)\r\n"; *p; p++)
      if (r15.push(static_cast<uint8_t>(*p), t++) & amis::IecReader::kRawLine) raw++;
    CHECK(raw == 2 && r15.meter_error()[0] == 0);
    r15.push(0x03, t++);
    CHECK(r15.poll(t + 2100) == amis::IecReader::kDone);
  }

  // The request is not repeated while an identification is arriving
  {
    amis::IecReader r16;
    r16.begin(amis::IecReader::Mode::kIdentOnly, 0);
    CHECK(r16.poll(0) == amis::IecReader::kSend);
    t = 4990;
    iec_feed(r16, "/SAT6", &t);
    CHECK(r16.poll(5100) == 0);
    t = 5200;
    iec_feed(r16, "ABC\r\n", &t);
    CHECK(!strcmp(r16.identification(), "ABC"));
  }

  // Short acknowledgement interval: still wait at least 2.5 s for data after the last acknowledgement
  {
    amis::IecReader r17;
    t = 0;
    r17.set_ack_tries(1);
    r17.set_ack_retry_ms(300);
    r17.begin(amis::IecReader::Mode::kReadout, t);
    iec_feed(r17, "/SAT6X\r\n", &t);
    t += 800;
    CHECK(r17.poll(t) == amis::IecReader::kSend);
    CHECK(r17.poll(t + 400) == 0 && r17.active());
    CHECK(r17.poll(t + 2600) == amis::IecReader::kFailed && r17.error() == amis::IecReader::Error::kTimeout);
  }

  // Block check character missing (allowed, manual 5.2.8): done after the character timeout
  amis::IecReader r7;
  t = 0;
  r7.begin(amis::IecReader::Mode::kReadout, t);
  iec_feed(r7, "/SAT6X\r\n", &t);
  t += 800;
  r7.poll(t);
  r7.push(0x02, t++);
  iec_feed(r7, "0.0.0(42)\r\n!\r\n\x03", &t);
  CHECK(r7.poll(t + 1000) == 0 && r7.active());
  CHECK(r7.poll(t + 2100) == amis::IecReader::kDone && !strcmp(r7.serial_number(), "42"));
}

// Feeds s with ms_per_char per character and polls after every character, like a main loop does.
// Returns all actions; *done_or_failed is set at the first kDone or kFailed.
static uint8_t iec_feed_slow(amis::IecReader &r, const char *s, uint32_t *t, uint32_t ms_per_char) {
  uint8_t all = 0;
  for (; *s; s++) {
    all |= r.push(static_cast<uint8_t>(*s), *t);
    *t += ms_per_char;
    all |= r.poll(*t);
  }
  return all;
}

// Until the first acknowledgement has been sent (default settings): request, identification, ACK after 700 ms
static void iec_start_readout(amis::IecReader &r, uint32_t *t) {
  r.begin(amis::IecReader::Mode::kReadout, *t);
  r.poll(*t);
  iec_feed(r, "/SAT6TEST000000000001\r\n", t);
  *t += 800;
  r.poll(*t);
}

// Changes for 0.3.0 (review findings A1...)
static void test_iec_030() {
  // Baud rate codes (computed since 0.3.1, no table in RAM)
  {
    const uint32_t exp[10] = {300, 600, 1200, 2400, 4800, 9600, 19200, 0, 0, 115200};
    bool ok = true;
    for (int i = 0; i < 10; i++) ok = ok && amis::IecReader::baud_for_char(static_cast<char>('0' + i)) == exp[i];
    CHECK(ok && amis::IecReader::baud_for_char('A') == 0 && amis::IecReader::baud_for_char('/') == 0);
  }
  // A10: set_address() checks the characters (digits, letters, space) and the length (32)
  {
    amis::IecReader r;
    size_t n = 0;
    CHECK(r.set_address("12 AB"));
    CHECK(!r.set_address("1!2"));
    CHECK(!r.set_address("123456789012345678901234567890123"));  // 33 characters
    CHECK(r.set_address("12345678901234567890123456789012") && r.set_address("12 AB"));
    uint32_t t = 0;
    r.begin(amis::IecReader::Mode::kIdentOnly, t);
    r.poll(t);
    const uint8_t *tx = r.tx(&n);
    CHECK(n == 10 && memcmp(tx, "/?12 AB!\r\n", 10) == 0);  // the rejected addresses changed nothing
    CHECK(r.set_address(nullptr) && r.set_address(""));
  }
  // A9: "/" followed by noise once a second: the request is still repeated after 5 s
  {
    amis::IecReader r;
    uint32_t t = 0;
    r.begin(amis::IecReader::Mode::kReadout, t);
    CHECK(r.poll(t) == amis::IecReader::kSend);
    r.push('/', 100);
    uint8_t all = 0;
    for (t = 1000; t <= 5500; t += 500) {
      if (t % 1000 == 0) r.push(0x05, t);  // not printable
      all |= r.poll(t);
    }
    CHECK(all == amis::IecReader::kSend);
    // printable characters after "/" still count as an arriving identification
    amis::IecReader r2;
    t = 0;
    r2.begin(amis::IecReader::Mode::kReadout, t);
    r2.poll(t);
    all = 0;
    r2.push('/', 4000);
    for (t = 4500; t <= 7000; t += 500) {
      r2.push('S', t);
      all |= r2.poll(t);
    }
    CHECK(all == 0);
  }
  // A8: a data line of 200 characters is truncated and counted, the readout goes on
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    r.push(0x02, t++);
    char longline[220];
    memset(longline, 'X', 200);
    memcpy(longline, "1.8.0(", 6);
    strcpy(longline + 200, ")\r\n");
    const char *rest = "0.0.0(42)\r\n!\r\n\x03";
    uint8_t bcc = 0;
    for (const char *p = longline; *p; p++) bcc ^= static_cast<uint8_t>(*p);
    for (const char *p = rest; *p; p++) bcc ^= static_cast<uint8_t>(*p);
    uint8_t all = 0;
    bool saw_trunc = false;
    for (const char *p = longline; *p; p++) {
      const uint8_t a = r.push(static_cast<uint8_t>(*p), t++);
      if (a & amis::IecReader::kRawLine) saw_trunc = r.raw_line_truncated() && strlen(r.raw_line()) == 95;
      all |= a;
    }
    CHECK(saw_trunc && !(all & amis::IecReader::kLine) && r.active());
    all = iec_feed(r, rest, &t);
    CHECK((all & amis::IecReader::kLine) && !r.raw_line_truncated());
    CHECK(r.push(bcc, t++) == amis::IecReader::kDone);
    CHECK(r.truncated_lines() == 1 && !strcmp(r.serial_number(), "42"));
  }
  // A2: begin() during a running readout leaves it running; the readout then completes normally
  {
    amis::IecReader r;
    uint32_t t = 0;
    r.begin(amis::IecReader::Mode::kReadout, t);
    r.poll(t);
    CHECK(r.begin(amis::IecReader::Mode::kReadout, t) == amis::IecReader::kNone);
    CHECK(r.active());
    iec_feed(r, "/SAT6TEST000000000001\r\n", &t);
    CHECK(r.begin(amis::IecReader::Mode::kIdentOnly, t) == amis::IecReader::kNone && r.active());
    t += 800;
    CHECK(r.poll(t) == amis::IecReader::kSend);
    r.push(0x02, t++);
    iec_feed(r, "0.0.0(42)\r\n!\r\n\x03", &t);
    CHECK(r.poll(t + 2100) == amis::IecReader::kDone && !strcmp(r.serial_number(), "42"));
  }

  // A1: billing data set at 300 Bd with default settings (total timeout 120 s): about 400 s of data
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    uint8_t all = r.push(0x02, t);  // STX
    uint8_t bcc = 0;
    char line[40];
    for (int i = 0; i < 380 && !(all & (amis::IecReader::kDone | amis::IecReader::kFailed)); i++) {
      snprintf(line, sizeof(line), "1.8.1*%02d(%06d.000*kWh)\r\n", i % 100, i);  // 26 characters, about 325 s in all
      for (const char *p = line; *p; p++) bcc ^= static_cast<uint8_t>(*p);
      all |= iec_feed_slow(r, line, &t, 33);
    }
    CHECK(t > 320000 && !(all & amis::IecReader::kFailed) && r.active());
    const char end[] = "!\r\n\x03";
    for (const char *p = end; *p; p++) bcc ^= static_cast<uint8_t>(*p);
    all |= iec_feed_slow(r, end, &t, 33);
    const char b[2] = {static_cast<char>(bcc), 0};
    all |= iec_feed_slow(r, b, &t, 33);
    CHECK((all & amis::IecReader::kDone) && !(all & amis::IecReader::kFailed) && !r.active());
  }
}

// Test frames derived from kFrame1 (101 bytes: header 0..18, encrypted payload 19..98, CS, 16)
static void frame_fix_checksum(uint8_t *f) {
  uint8_t cs = 0;
  for (size_t i = 4; i < 99; i++) cs = static_cast<uint8_t>(cs + f[i]);
  f[99] = cs;
}
static void frame_iv(const uint8_t *f, uint8_t iv[16]) {
  iv[0] = f[11];
  iv[1] = f[12];
  memcpy(iv + 2, f + 7, 4);
  iv[6] = f[13];
  iv[7] = f[14];
  memset(iv + 8, f[15], 8);
}
// Decrypts the payload of f into plain[80]
static void frame_decrypt(const uint8_t *f, uint8_t plain[80]) {
  amis::Aes128 aes;
  uint8_t key[16], iv[16];
  hex2bin("00112233445566778899aabbccddeeff", key);
  aes.set_key(key);
  frame_iv(f, iv);
  memcpy(plain, f + 19, 80);
  aes.cbc_decrypt(plain, 80, iv);
}
// Encrypts plain[80] into the payload of f and fixes the checksum
static void frame_encrypt(uint8_t *f, const uint8_t plain[80]) {
  amis::Aes128 aes;
  uint8_t key[16], iv[16];
  hex2bin("00112233445566778899aabbccddeeff", key);
  aes.set_key(key);
  frame_iv(f, iv);
  memcpy(f + 19, plain, 80);
  aes.cbc_encrypt(f + 19, 80, iv);
  frame_fix_checksum(f);
}

// Changes for 0.3.0 in amis::Reader (review findings A3...)
static void test_reader_030() {
  // Unencrypted frame (security mode 0): rejected with a key set, read without a key
  {
    uint8_t f[101] = {0x68, 0x5F, 0x5F, 0x68, 0x53, 0xF0, 0x5B, 0, 0, 0, 0, 0x2D, 0x4C, 0x01, 0x0E, 0x01, 0x00, 0x00, 0x00};
    memset(f + 19, 0x2F, 80);
    const uint8_t rec[] = {0x04, 0x03, 0x40, 0x42, 0x0F, 0x00};  // 1.8.0 = 1000000 Wh
    memcpy(f + 19, rec, sizeof(rec));
    f[100] = 0x16;
    frame_fix_checksum(f);
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    uint8_t st = feed(r, f, sizeof(f), 0, &acks);
    CHECK(st == (amis::Reader::kSendAck | amis::Reader::kError) && acks == 1);
    CHECK(r.last_error() == amis::Error::kNotEncrypted && r.reading().energy_import_wh == 0);
    amis::Reader r2;  // no key
    st = feed(r2, f, sizeof(f), 0, &acks);
    CHECK((st & amis::Reader::kReading) && r2.reading().energy_import_wh == 1000000);
  }
  // A11: repetition = same access number AND unchanged FCB; SND_NKE to 0xFF resets it without E5
  {
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    r.set_replay_check(false);  // the same frame is fed again on purpose
    int acks = 0;
    uint8_t st = feed(r, kFrame1, sizeof(kFrame1), 0, &acks);  // C = 53 (FCV, FCB = 0)
    CHECK(st & amis::Reader::kReading);
    uint8_t f[sizeof(kFrame1)];
    memcpy(f, kFrame1, sizeof(f));
    f[4] = 0x73;  // FCB toggled, same access number
    f[99] = static_cast<uint8_t>(f[99] + 0x20);
    acks = 0;
    st = feed(r, f, sizeof(f), 1000, &acks);
    CHECK((st & amis::Reader::kReading) && acks == 1 && r.stats().duplicates == 0);
    acks = 0;
    st = feed(r, f, sizeof(f), 2000, &acks);  // same FCB again: repetition
    CHECK(st == amis::Reader::kSendAck && acks == 1 && r.stats().duplicates == 1);
    const uint8_t nke_ff[] = {0x10, 0x40, 0xFF, 0x3F, 0x16};
    acks = 0;
    st = feed(r, nke_ff, sizeof(nke_ff), 3000, &acks);
    CHECK(st == amis::Reader::kNothing && acks == 0);
    st = feed(r, f, sizeof(f), 4000, &acks);  // duplicate state cleared: decoded again
    CHECK((st & amis::Reader::kReading) && acks == 1 && r.stats().duplicates == 1);
  }
  // A6: frame without 1.8.0 -> kIncomplete with the full mask, a reading without a mask
  {
    uint8_t f[sizeof(kFrame1)], plain[80];
    memcpy(f, kFrame1, sizeof(f));
    frame_decrypt(f, plain);
    CHECK(plain[0] == 0x2F && plain[10] == 0x04 && plain[11] == 0x03);  // 2F 2F, time (2+6), 1.8.0 = 04 03
    plain[11] = 0x13;  // 04 13 = volume: unknown to the AMIS table, skipped
    frame_encrypt(f, plain);
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    r.set_header_check(false);  // the AMIS layout check would reject the missing 1.8.0 already
    int acks = 0;
    uint8_t st = feed(r, f, sizeof(f), 0, &acks);
    CHECK((st & amis::Reader::kReading) && !r.reading().has(amis::Reading::kEnergyImport));
    CHECK(r.reading().energy_import_wh == 0 && r.reading().has(amis::Reading::kEnergyExport));
    amis::Reader r2;
    r2.set_key_hex(kTestKeyHex);
    r2.set_header_check(false);
    r2.set_required_fields(amis::Reading::kAll);
    acks = 0;
    st = feed(r2, f, sizeof(f), 0, &acks);
    CHECK(st == (amis::Reader::kSendAck | amis::Reader::kError) && acks == 1);
    CHECK(r2.last_error() == amis::Error::kIncomplete);
    amis::Reader r3;  // default: layout check
    r3.set_key_hex(kTestKeyHex);
    st = feed(r3, f, sizeof(f), 0, &acks);
    CHECK((st & amis::Reader::kError) && r3.last_error() == amis::Error::kHeader);
    st = feed(r2, kFrame2, sizeof(kFrame2), 1000, &acks);  // complete frame
    CHECK(st & amis::Reader::kReading);
  }
  // A5: default inter-byte timeout 500 ms: a 300 ms stall in the middle of a frame is fine, 1 s is a resync
  {
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    uint8_t st = feed(r, kFrame1, 40, 0, &acks);
    st |= feed(r, kFrame1 + 40, sizeof(kFrame1) - 40, 340, &acks);
    CHECK((st & amis::Reader::kReading) && acks == 1 && r.stats().resyncs == 0);
    amis::Reader r2;
    r2.set_key_hex(kTestKeyHex);
    acks = 0;
    st = feed(r2, kFrame1, 40, 0, &acks);
    st |= feed(r2, kFrame1 + 40, sizeof(kFrame1) - 40, 1040, &acks);
    CHECK(!(st & amis::Reader::kReading) && acks == 0 && r2.stats().resyncs == 1);
  }
  // A3: UART error in the middle of a frame -> frame dropped, no E5, no reading; the next frame is read
  {
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    uint8_t st = feed(r, kFrame1, 50, 0, &acks);
    r.push_error(50);
    st |= feed(r, kFrame1 + 50, sizeof(kFrame1) - 50, 51, &acks);
    CHECK(!(st & (amis::Reader::kSendAck | amis::Reader::kReading)) && acks == 0);
    CHECK(r.stats().line_errors == 1);
    st = feed(r, kFrame2, sizeof(kFrame2), 1000, &acks);
    CHECK((st & amis::Reader::kReading) && acks == 1);
  }
  // A3: blind spot of the 8-bit checksum: bit k set in one byte and cleared in another keeps the sum
  {
    uint8_t f[sizeof(kFrame1)];
    memcpy(f, kFrame1, sizeof(f));
    size_t bi = 0, bj = 0;
    uint8_t mask = 0;
    for (size_t i = 19; i < sizeof(f) - 2 && !mask; i++)  // encrypted payload
      for (size_t j = i + 1; j < sizeof(f) - 2 && !mask; j++)
        for (int k = 0; k < 8 && !mask; k++)
          if (!(f[i] & (1 << k)) && (f[j] & (1 << k))) {
            bi = i;
            bj = j;
            mask = static_cast<uint8_t>(1 << k);
          }
    CHECK(mask != 0);
    f[bi] = static_cast<uint8_t>(f[bi] | mask);
    f[bj] = static_cast<uint8_t>(f[bj] & ~mask);
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    feed(r, f, sizeof(f), 0, &acks);
    CHECK(acks == 1);  // checksum still fits: acknowledged without push_error()
    amis::Reader r2;
    r2.set_key_hex(kTestKeyHex);
    acks = 0;
    feed(r2, f, bi, 0, &acks);
    r2.push_error(static_cast<uint32_t>(bi));  // the UART flagged the block with the damaged byte
    feed(r2, f + bi + 1, sizeof(f) - bi - 1, static_cast<uint32_t>(bi + 1), &acks);
    CHECK(acks == 0 && r2.stats().line_errors == 1);
  }
}

// Long frame 68 L L 68 C A CI <hdr 12 bytes> <data> CS 16 with a plain header (id 0, SAM, 01, 0E), mode from cfg
static size_t build_frame(uint8_t *out, uint8_t c, uint8_t a, uint8_t ci, uint16_t cfg, uint8_t acc,
                          const uint8_t *data, size_t n) {
  const uint8_t hdr[12] = {0, 0, 0, 0, 0x2D, 0x4C, 0x01, 0x0E, acc, 0x00, static_cast<uint8_t>(cfg),
                           static_cast<uint8_t>(cfg >> 8)};
  size_t k = 0;
  out[k++] = 0x68;
  const uint8_t L = static_cast<uint8_t>(3 + sizeof(hdr) + n);
  out[k++] = L;
  out[k++] = L;
  out[k++] = 0x68;
  out[k++] = c;
  out[k++] = a;
  out[k++] = ci;
  memcpy(out + k, hdr, sizeof(hdr));
  k += sizeof(hdr);
  memcpy(out + k, data, n);
  k += n;
  uint8_t cs = 0;
  for (size_t i = 4; i < k; i++) cs = static_cast<uint8_t>(cs + out[i]);
  out[k++] = cs;
  out[k++] = 0x16;
  return k;
}

// STX <body> ETX BCC (BCC = XOR from after STX up to and including ETX), fed byte by byte
static uint8_t iec_block(amis::IecReader &r, const char *body, uint32_t *t, bool good_bcc = true) {
  uint8_t all = r.push(0x02, (*t)++);
  uint8_t bcc = 0;
  for (const char *c = body; *c; c++) {
    bcc ^= static_cast<uint8_t>(*c);
    all |= r.push(static_cast<uint8_t>(*c), (*t)++);
  }
  bcc ^= 0x03;
  all |= r.push(0x03, (*t)++);
  all |= r.push(good_bcc ? bcc : static_cast<uint8_t>(bcc ^ 0x01), (*t)++);
  return all;
}

// Findings of the external review (0.4.0)
static void test_iec_040() {
  // R7/R18: verified() only after STX and a matching block check character
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    const uint8_t a = iec_block(r, "0.0.0(TEST000000000001)\r\n1.8.0(001234.567*kWh)\r\n!\r\n", &t);
    CHECK((a & amis::IecReader::kDone) && r.verified() && r.serial_from_data());
    CHECK(strcmp(r.serial_number(), "TEST000000000001") == 0);
    // without STX after the repeated acknowledgement (real TD-351x): done, but not verified
    amis::IecReader r2;
    t = 0;
    iec_start_readout(r2, &t);
    iec_block(r2, "ER0007", &t);  // refused
    t += 7100;
    r2.poll(t);  // repeated acknowledgement
    uint8_t b = iec_feed(r2, "1.8.0(001234.567*kWh)\r\n!\r\n\x03", &t);
    b |= r2.push('X', t++);
    CHECK((b & amis::IecReader::kDone) && !r2.verified() && !r2.serial_from_data());
    CHECK(strcmp(r2.serial_number(), "TEST000000000001") == 0);  // falls back to the identification
  }
  // R19: without acknowledgement an error telegram ends the readout, nothing is sent again
  {
    amis::IecReader r;
    r.set_send_ack(false);
    uint32_t t = 0;
    r.begin(amis::IecReader::Mode::kReadout, t);
    r.poll(t);
    iec_feed(r, "/SAT6TEST000000000001\r\n", &t);
    uint8_t a = iec_block(r, "ER0007", &t);
    for (uint32_t k = 0; k < 20000; k += 100) a |= r.poll(t + k);
    CHECK((a & amis::IecReader::kFailed) && !(a & amis::IecReader::kSend));
    CHECK(r.error() == amis::IecReader::Error::kMeterError);
  }
  // R20: a noise byte that looks like our ACK before STX must not swallow the data block
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    r.push(0x86, t++);  // 0x06 with the parity bit
    const uint8_t a = iec_block(r, "1.8.0(001234.567*kWh)\r\n!\r\n", &t);
    CHECK((a & amis::IecReader::kDone) && r.verified() && strcmp(r.line().obis, "1.8.0") == 0);
  }
  // R3-4: an identification needs a valid baud rate character, and a long pause inside it starts over
  {
    amis::IecReader r;
    uint32_t t = 0;
    r.begin(amis::IecReader::Mode::kIdentOnly, t);
    r.poll(t);
    uint8_t a = iec_feed(r, "/ABCxNOISE\r\n", &t);
    CHECK(a == 0 && r.active());
    iec_feed(r, "/SAT", &t);
    t += 2000;  // pause
    a = iec_feed(r, "6XYZ\r\n", &t);
    CHECK(a == 0 && r.active());
    a = iec_feed(r, "/SAT6TEST000000000001\r\n", &t);
    t += 800;
    a |= r.poll(t);
    CHECK((a & amis::IecReader::kDone) && strcmp(r.identification(), "TEST000000000001") == 0);
  }
  // R3-1: answer to the acknowledgement with a lost first character ("R0007", BCC no longer fits)
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    uint8_t a = iec_block(r, "R0007", &t, false);
    CHECK(!(a & amis::IecReader::kFailed) && r.active());
    t += 7100;
    a = r.poll(t);
    CHECK(a == amis::IecReader::kSend);  // acknowledgement repeated
    a = iec_block(r, "1.8.0(001234.567*kWh)\r\n!\r\n", &t);
    CHECK(a & amis::IecReader::kDone);
  }
  // R3-2: an error telegram after data lines ends the readout instead of silently repeating it
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    const uint8_t a = iec_block(r, "1.8.0(001234.567*kWh)\r\nER0001\r\n", &t);
    CHECK((a & amis::IecReader::kFailed) && r.error() == amis::IecReader::Error::kMeterError);
    // a BCC error after data lines stays a BCC error
    amis::IecReader r2;
    t = 0;
    iec_start_readout(r2, &t);
    const uint8_t b = iec_block(r2, "1.8.0(001234.567*kWh)\r\n!\r\n", &t, false);
    CHECK((b & amis::IecReader::kFailed) && r2.error() == amis::IecReader::Error::kBcc && !r2.verified());
  }
  // R28: begin() clears the truncation flag of the previous readout
  {
    amis::IecReader r;
    uint32_t t = 0;
    iec_start_readout(r, &t);
    char longline[140];
    memset(longline, '9', sizeof(longline));
    memcpy(longline, "1.8.0(", 6);
    memcpy(longline + sizeof(longline) - 3, "\r\n", 3);
    char body[200];
    snprintf(body, sizeof(body), "%s", longline);
    uint8_t a = iec_block(r, body, &t);
    CHECK((a & amis::IecReader::kDone) && r.raw_line_truncated());
    r.begin(amis::IecReader::Mode::kReadout, t);
    CHECK(!r.raw_line_truncated() && r.truncated_lines() == 0);
    r.abort();
    CHECK(!r.active());
  }
}

// Findings of the external review (0.4.0)
static void test_reader_040() {
  // R1: bytes after the encrypted part are not protected by the key and must not be parsed
  if (amis::Reader::kMaxFrame >= sizeof(kFrame1) + 6) {
    uint8_t f[sizeof(kFrame1) + 6];
    memcpy(f, kFrame1, 99);
    const uint8_t fake[6] = {0x04, 0x03, 0x01, 0x00, 0x00, 0x00};  // 1.8.0 = 1 Wh in plain text
    memcpy(f + 99, fake, sizeof(fake));
    f[1] = f[2] = static_cast<uint8_t>(kFrame1[1] + sizeof(fake));
    uint8_t cs = 0;
    for (size_t i = 4; i < sizeof(f) - 2; i++) cs = static_cast<uint8_t>(cs + f[i]);
    f[sizeof(f) - 2] = cs;
    f[sizeof(f) - 1] = 0x16;
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    const uint8_t st = feed(r, f, sizeof(f), 0, &acks);
    CHECK((st & amis::Reader::kReading) && r.reading().energy_import_wh == 12345678);
    size_t pn = 0;
    r.last_plaintext(&pn);
    CHECK(pn == 80);
  }
  // R2: header bytes form the IV -> a changed identification, version, medium or access number is rejected
  {
    const size_t pos[] = {7, 10, 13, 14, 15};  // id, id, version, medium, access number
    for (size_t p : pos) {
      uint8_t f[sizeof(kFrame1)];
      memcpy(f, kFrame1, sizeof(f));
      f[p] ^= 0x01;
      frame_fix_checksum(f);
      amis::Reader r;
      r.set_key_hex(kTestKeyHex);
      int acks = 0;
      const uint8_t st = feed(r, f, sizeof(f), 0, &acks);
      CHECK(!(st & amis::Reader::kReading) && acks == 1);
      CHECK(r.last_error() == amis::Error::kHeader || r.last_error() == amis::Error::kWrongKey);
      if (p != 15) CHECK(r.last_error() == amis::Error::kHeader);
    }
    // CBC bit flipping: a changed ciphertext block garbles its own plaintext block (caught by the layout check)
    for (size_t blk = 0; blk < 5; blk++) {
      uint8_t f[sizeof(kFrame1)];
      memcpy(f, kFrame1, sizeof(f));
      f[19 + 16 * blk + 10] ^= 0x01;  // would flip 1.7.0 in block 2 when blk = 1
      frame_fix_checksum(f);
      amis::Reader r;
      r.set_key_hex(kTestKeyHex);
      int acks = 0;
      const uint8_t st = feed(r, f, sizeof(f), 0, &acks);
      CHECK(!(st & amis::Reader::kReading) && acks == 1);
      CHECK(r.last_error() == amis::Error::kHeader || (blk == 0 && r.last_error() == amis::Error::kWrongKey));
    }
  }
  // R3: replay of an older frame (or the newest one with the FCB toggled) is rejected, still with E5
  {
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    uint8_t st = feed(r, kFrame2, sizeof(kFrame2), 0, &acks);
    CHECK(st & amis::Reader::kReading);
    acks = 0;
    st = feed(r, kFrame1, sizeof(kFrame1), 1000, &acks);  // older meter time
    CHECK(st == (amis::Reader::kSendAck | amis::Reader::kError) && acks == 1);
    CHECK(r.last_error() == amis::Error::kReplay && r.reading().access_number == 0x22);
    feed(r, kSndNke, sizeof(kSndNke), 2000, &acks);  // a search request does not clear it
    st = feed(r, kFrame1, sizeof(kFrame1), 3000, &acks);
    CHECK((st & amis::Reader::kError) && r.last_error() == amis::Error::kReplay);
    uint8_t f[sizeof(kFrame2)];  // newest frame with the FCB toggled: same time
    memcpy(f, kFrame2, sizeof(f));
    f[4] = 0x53;
    frame_fix_checksum(f);
    st = feed(r, f, sizeof(f), 4000, &acks);
    CHECK((st & amis::Reader::kError) && r.last_error() == amis::Error::kReplay);
    r.reset_replay_check();
    st = feed(r, kFrame1, sizeof(kFrame1), 5000, &acks);
    CHECK(st & amis::Reader::kReading);
    amis::Reader r2;
    r2.set_key_hex(kTestKeyHex);
    r2.set_replay_check(false);
    feed(r2, kFrame2, sizeof(kFrame2), 0, &acks);
    st = feed(r2, kFrame1, sizeof(kFrame1), 1000, &acks);
    CHECK(st & amis::Reader::kReading);
  }
  // R15/R26: a time record flagged invalid still counts as present (time.valid = false), kAll is met
  {
    uint8_t f[sizeof(kFrame1)], plain[80];
    memcpy(f, kFrame1, sizeof(f));
    frame_decrypt(f, plain);
    plain[5] |= 0x80;  // type I byte 1: "invalid" bit
    frame_encrypt(f, plain);
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    r.set_required_fields(amis::Reading::kAll);
    int acks = 0;
    const uint8_t st = feed(r, f, sizeof(f), 0, &acks);
    CHECK((st & amis::Reader::kReading) && r.reading().has(amis::Reading::kTime) && !r.reading().time.valid);
    CHECK(r.reading().energy_import_wh == 12345678);
  }
  // R16: days that do not exist
  {
    amis::DateTime t;
    // year 2000 + yy: bits 0..2 of yy in byte 3 (bits 5..7), bits 3..6 in byte 4 (bits 4..7)
    const uint8_t feb29_2028[6] = {0x00, 0x00, 0x0C, 0x1D | (4 << 5), 0x02 | (3 << 4), 0x09};  // yy = 28
    CHECK(t.decode_type_i(feb29_2028) && t.year == 2028 && t.month == 2 && t.day == 29);
    const uint8_t feb30_2026[6] = {0x00, 0x00, 0x0C, 0x1E | (2 << 5), 0x02 | (3 << 4), 0x09};  // 2026
    CHECK(!t.decode_type_i(feb30_2026) && t.year == 2026);
    const uint8_t feb29_2026[6] = {0x00, 0x00, 0x0C, 0x1D | (2 << 5), 0x02 | (3 << 4), 0x09};
    CHECK(!t.decode_type_i(feb29_2026));
    const uint8_t apr31_2026[6] = {0x00, 0x00, 0x0C, 0x1F | (2 << 5), 0x04 | (3 << 4), 0x09};
    CHECK(!t.decode_type_i(apr31_2026));
    const uint8_t apr30_2026[6] = {0x00, 0x00, 0x17, 0x1E | (2 << 5), 0x04 | (3 << 4), 0x09};
    CHECK(t.decode_type_i(apr30_2026) && t.to_unix_utc() == 1777586400LL);  // 23:00 CET = 22:00 UTC
  }
  // R13: power balance without overflow
  {
    amis::Reading x;
    x.power_import_w = 0x80000000u;
    CHECK(x.power_balance_w() == INT32_MAX);
    x.power_import_w = 0;
    x.power_export_w = 0xFFFFFFFFu;
    CHECK(x.power_balance_w() == INT32_MIN);
    x.power_import_w = 5;
    x.power_export_w = 7;
    CHECK(x.power_balance_w() == -2);
  }
  // R27: no "plaintext" after errors before or during decryption
  {
    amis::Reader r;
    r.set_key_hex("FFEEDDCCBBAA99887766554433221100");
    int acks = 0;
    feed(r, kFrame1, sizeof(kFrame1), 0, &acks);
    size_t pn = 1;
    r.last_plaintext(&pn);
    CHECK(r.last_error() == amis::Error::kWrongKey && pn == 0);
    amis::Reader r2;
    feed(r2, kFrame1, sizeof(kFrame1), 0, &acks);
    pn = 1;
    r2.last_plaintext(&pn);
    CHECK(r2.last_error() == amis::Error::kNoKey && pn == 0);
  }
  // R12: a reading older than about 24 days is forgotten, so online() cannot wrap after 49.7 days
  {
    amis::Reader r;
    r.set_key_hex(kTestKeyHex);
    int acks = 0;
    feed(r, kFrame1, sizeof(kFrame1), 0, &acks);
    CHECK(r.online(200));
    r.push(0x00, 0x90000000u);  // any byte after a long time without readings
    CHECK(!r.online(200));      // same value as 2^32 + 200 ms
  }
  // R24: error codes and addresses with plain frames (no key)
  {
    uint8_t f[300];
    const uint8_t fill[4] = {0x2F, 0x2F, 0x2F, 0x2F};
    const uint8_t e18[6] = {0x04, 0x03, 0x10, 0x00, 0x00, 0x00};
    const uint8_t lvar[4] = {0x0D, 0x13, 0xC1, 0x12};  // LVAR 0xC1 (BCD) is not supported
    struct Case {
      uint8_t c, a, ci;
      uint16_t cfg;
      const uint8_t *d;
      size_t n;
      bool ack;
      amis::Error e;
    } cases[] = {
        {0x53, 0xF0, 0x99, 0x0000, e18, sizeof(e18), true, amis::Error::kUnsupportedCi},
        {0x53, 0xF0, 0x5B, 0x0700, e18, sizeof(e18), true, amis::Error::kUnsupportedEncryption},
        {0x53, 0xF0, 0x5B, 0x0000, fill, sizeof(fill), true, amis::Error::kNoData},
        {0x53, 0xF0, 0x5B, 0x0000, lvar, sizeof(lvar), true, amis::Error::kRecordFormat},
        {0x53, 0xF0, 0x5B, 0x0510, e18, sizeof(e18), true, amis::Error::kNoKey},
        {0x53, 0xFE, 0x5B, 0x0000, e18, sizeof(e18), true, amis::Error::kNone},   // broadcast with reply
        {0x53, 0x01, 0x5B, 0x0000, e18, sizeof(e18), false, amis::Error::kNone},  // other address: no E5
        {0x08, 0xF0, 0x5B, 0x0000, e18, sizeof(e18), false, amis::Error::kNone},  // RSP_UD: no E5
    };
    for (const Case &k : cases) {
      const size_t n = build_frame(f, k.c, k.a, k.ci, k.cfg, 0x01, k.d, k.n);
      amis::Reader r;
      int acks = 0;
      const uint8_t st = feed(r, f, n, 0, &acks);
      CHECK(acks == (k.ack ? 1 : 0));
      CHECK(r.last_error() == k.e);
      if (k.e == amis::Error::kNone) CHECK((st & amis::Reader::kReading) && r.reading().energy_import_wh == 16);
    }
    // encrypted length 0 or longer than the data
    uint8_t enc[16];
    memset(enc, 0x2F, sizeof(enc));
    const uint16_t bad_cfg[] = {0x0500, 0x0520};
    for (uint16_t cfg : bad_cfg) {
      const size_t n = build_frame(f, 0x53, 0xF0, 0x5B, cfg, 0x01, enc, sizeof(enc));
      amis::Reader r;
      r.set_key_hex(kTestKeyHex);
      int acks = 0;
      feed(r, f, n, 0, &acks);
      CHECK(r.last_error() == amis::Error::kFrameFormat);
    }
    // too short for the long header
    const uint8_t tiny[] = {0x68, 0x04, 0x04, 0x68, 0x53, 0xF0, 0x5B, 0x00, 0x9E, 0x16};
    amis::Reader r;
    int acks = 0;
    feed(r, tiny, sizeof(tiny), 0, &acks);
    CHECK(r.last_error() == amis::Error::kFrameFormat && acks == 1);
    // primary address and inter-byte timeout setters
    amis::Reader r2;
    r2.set_primary_address(0x01);
    const size_t n = build_frame(f, 0x53, 0x01, 0x5B, 0x0000, 0x01, e18, sizeof(e18));
    acks = 0;
    feed(r2, f, n, 0, &acks);
    CHECK(acks == 1);
    amis::Reader r3;
    r3.set_interbyte_timeout_ms(5);
    acks = 0;
    feed(r3, f, 10, 0, &acks);
    feed(r3, f + 10, n - 10, 100, &acks);
    CHECK(acks == 0 && r3.stats().resyncs >= 1);
    // every error has a text
    for (int e = 0; e <= static_cast<int>(amis::Error::kReplay); e++)
      CHECK(strcmp(amis::error_str(static_cast<amis::Error>(e)), "?") != 0);
#ifndef LIBAMIS_NO_IEC
    for (int e = 0; e <= static_cast<int>(amis::IecReader::Error::kMeterError); e++)
      CHECK(strcmp(amis::error_str(static_cast<amis::IecReader::Error>(e)), "?") != 0);
#endif
  }
}

int main() {
  test_aes();
  test_datetime();
  test_frames();
  test_netzooe_example();
  test_reader_030();
  test_reader_040();
  test_iec();
  test_iec_030();
  test_iec_040();
  printf("%d checks passed, %d failed\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
