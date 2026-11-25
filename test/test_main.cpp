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
  CHECK(x.header_id == 0x12345678 && x.manufacturer == 0x4C2D && x.access_number == 0x21);
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
  // The request is repeated twice every 5 s (manual 5.2.12.2), then the reader gives up 2.5 s later
  CHECK(r4.poll(4900) == 0);
  CHECK(r4.poll(5100) == amis::IecReader::kSend);
  tx = r4.tx(&n);
  CHECK(n == 21 && tx[0] == '/');
  CHECK(r4.poll(10200) == amis::IecReader::kSend);
  CHECK(r4.poll(12600) == 0 && r4.active());
  CHECK(r4.poll(12800) == amis::IecReader::kFailed && r4.error() == amis::IecReader::Error::kNoIdent);

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

// Changes for 0.3.0 in amis::Reader (review findings A3...)
static void test_reader_030() {
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

int main() {
  test_aes();
  test_datetime();
  test_frames();
  test_netzooe_example();
  test_reader_030();
  test_iec();
  test_iec_030();
  printf("%d checks passed, %d failed\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
