// SPDX-License-Identifier: MIT
// libamis - reader core for the Siemens AMIS smart meter. Written from EN 13757 (M-Bus),
// the OMS specification (security mode 5) and the Netz OOe description of the customer interface.
#include "amis_reader.h"

#include <string.h>

#include "amis_pgm.h"

namespace amis {

namespace {

constexpr uint8_t kStartShort = 0x10;
constexpr uint8_t kStartLong = 0x68;
constexpr uint8_t kStop = 0x16;
constexpr uint8_t kSndNke = 0x40;
constexpr uint8_t kAddrBroadcastReply = 0xFE;

uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Record header (DIF, DIFE..., VIF, VIFE...) as sent by the AMIS -> field in Reading
struct RecordKey {
  uint8_t len;
  uint8_t bytes[6];
  uint16_t field;
};
const RecordKey kRecords[] AMIS_PROGMEM = {
    {2, {0x06, 0x6D}, Reading::kTime},                                      // date/time type I
    {2, {0x04, 0x03}, Reading::kEnergyImport},                              // 1.8.0 Wh
    {3, {0x04, 0x83, 0x3C}, Reading::kEnergyExport},                        // 2.8.0 Wh, backward flow
    {5, {0x84, 0x10, 0xFB, 0x82, 0x73}, Reading::kReactiveEnergyImport},    // 3.8.1 varh, tariff 1
    {6, {0x84, 0x10, 0xFB, 0x82, 0xF3, 0x3C}, Reading::kReactiveEnergyExport},  // 4.8.1 varh
    {2, {0x04, 0x2B}, Reading::kPowerImport},                               // 1.7.0 W
    {3, {0x04, 0xAB, 0x3C}, Reading::kPowerExport},                         // 2.7.0 W
    {3, {0x04, 0xFB, 0x14}, Reading::kReactivePowerImport},                 // 3.7.0 var
    {4, {0x04, 0xFB, 0x94, 0x3C}, Reading::kReactivePowerExport},           // 4.7.0 var
    {4, {0x04, 0x83, 0xFF, 0x04}, Reading::kInkasso},                       // 1.128.0
};

// Data length by DIF (lower 4 bits); 0xFF = variable, 0xFE = special function
const uint8_t kDataLen[16] AMIS_PROGMEM = {0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, 0xFF, 6, 0xFE};

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Days since 1970-01-01 (proleptic Gregorian calendar)
int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------

bool DateTime::decode_type_i(const uint8_t b[6]) {
  second = b[0] & 0x3F;
  dst = (b[0] & 0x40) != 0;
  minute = b[1] & 0x3F;
  const bool invalid_flag = (b[1] & 0x80) != 0;
  hour = b[2] & 0x1F;
  weekday = static_cast<uint8_t>(b[2] >> 5);
  day = b[3] & 0x1F;
  month = b[4] & 0x0F;
  year = static_cast<uint16_t>(2000 + (((b[4] & 0xF0) >> 1) | ((b[3] & 0xE0) >> 5)));
  week = b[5] & 0x3F;
  valid = !invalid_flag && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 && minute < 60 &&
          second < 60;
  return valid;
}

int64_t DateTime::to_unix_utc() const {
  const int64_t local = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
  return local - 3600 * (dst ? 2 : 1);  // CET = UTC+1, CEST = UTC+2
}

namespace {
// Texts in flash on the ESP8266 (see amis_pgm.h)
const char kErr0[] AMIS_PROGMEM = "ok";
const char kErr1[] AMIS_PROGMEM = "checksum error";
const char kErr2[] AMIS_PROGMEM = "invalid frame";
const char kErr3[] AMIS_PROGMEM = "unsupported CI field";
const char kErr4[] AMIS_PROGMEM = "unsupported encryption";
const char kErr5[] AMIS_PROGMEM = "no key set";
const char kErr6[] AMIS_PROGMEM = "wrong key";
const char kErr7[] AMIS_PROGMEM = "unparseable data records";
const char kErr8[] AMIS_PROGMEM = "no known values";
const char kErrUnknown[] AMIS_PROGMEM = "?";
}  // namespace

const char *error_str(Error e) {
  switch (e) {
    case Error::kNone: return detail::pgm_str(kErr0);
    case Error::kChecksum: return detail::pgm_str(kErr1);
    case Error::kFrameFormat: return detail::pgm_str(kErr2);
    case Error::kUnsupportedCi: return detail::pgm_str(kErr3);
    case Error::kUnsupportedEncryption: return detail::pgm_str(kErr4);
    case Error::kNoKey: return detail::pgm_str(kErr5);
    case Error::kWrongKey: return detail::pgm_str(kErr6);
    case Error::kRecordFormat: return detail::pgm_str(kErr7);
    case Error::kNoData: return detail::pgm_str(kErr8);
  }
  return detail::pgm_str(kErrUnknown);
}

// ---------------------------------------------------------------------------------------------------------

Reader::Reader() { reset(); }

void Reader::reset() {
  len_ = 0;
  expected_ = 0;
}

void Reader::set_key(const uint8_t key[16]) {
  aes_.set_key(key);
  has_key_ = true;
  have_access_ = false;
}

bool Reader::set_key_hex(const char *hex) {
  if (!hex) return false;
  uint8_t key[16];
  int n = 0;
  for (const char *c = hex; *c; c++) {
    if (*c == ' ' || *c == ':' || *c == '-') continue;
    const int v = hexval(*c);
    if (v < 0 || n >= 32) return false;
    if (n & 1)
      key[n / 2] = static_cast<uint8_t>(key[n / 2] | v);
    else
      key[n / 2] = static_cast<uint8_t>(v << 4);
    n++;
  }
  if (n != 32) return false;
  set_key(key);
  return true;
}

bool Reader::online(uint32_t now_ms, uint32_t max_age_ms) const {
  return have_reading_ && static_cast<uint32_t>(now_ms - last_reading_ms_) <= max_age_ms;
}

uint8_t Reader::push(uint8_t byte, uint32_t now_ms) {
  stats_.bytes++;
  if (len_ > 0 && static_cast<uint32_t>(now_ms - last_byte_ms_) > interbyte_timeout_ms_) {
    stats_.resyncs++;
    reset();
  }
  last_byte_ms_ = now_ms;
  now_ms_ = now_ms;
  return feed_(byte);
}

void Reader::push_error(uint32_t now_ms) {
  stats_.line_errors++;
  last_byte_ms_ = now_ms;
  reset();
}

uint8_t Reader::feed_(uint8_t b) {
  if (len_ == 0) {
    // Only a start character begins a frame; drop anything else (noise, echo of our own E5)
    plain_len_ = 0;  // a new frame overwrites the buffer
    if (b == kStartShort) {
      expected_ = 5;
    } else if (b == kStartLong) {
      expected_ = 0;  // length arrives with the 2nd byte
    } else {
      return kNothing;
    }
  }
  buf_[len_++] = b;

  if (buf_[0] == kStartLong && len_ == 4) {
    if (buf_[1] != buf_[2] || buf_[3] != kStartLong || buf_[1] < 3 ||
        static_cast<size_t>(buf_[1]) + 6 > kMaxFrame) {
      stats_.resyncs++;
      return restart_from_(1);
    }
    expected_ = static_cast<size_t>(buf_[1]) + 6;
  }
  if (expected_ != 0 && len_ >= expected_) {
    const uint8_t st = (buf_[0] == kStartShort) ? handle_short_() : handle_long_();
    return st;
  }
  return kNothing;
}

// Drop an invalid start and feed the remaining bytes again (they may already be the next frame)
uint8_t Reader::restart_from_(size_t skip) {
  uint8_t tmp[8];
  size_t n = (len_ > skip) ? len_ - skip : 0;
  if (n > sizeof(tmp)) n = sizeof(tmp);
  memcpy(tmp, buf_ + skip, n);
  reset();
  uint8_t st = kNothing;
  for (size_t i = 0; i < n; i++) st |= feed_(tmp[i]);
  return st;
}

uint8_t Reader::fail_(Error e) {
  last_error_ = e;
  switch (e) {
    case Error::kChecksum: stats_.checksum_errors++; break;
    case Error::kWrongKey: stats_.key_errors++; break;
    default: stats_.other_errors++; break;
  }
  return kError;
}

// Short frame: 10 C A CS 16
uint8_t Reader::handle_short_() {
  const uint8_t c = buf_[1], a = buf_[2];
  if (buf_[3] != static_cast<uint8_t>(c + a) || buf_[4] != kStop) {
    stats_.resyncs++;
    return restart_from_(1);
  }
  reset();
  stats_.short_frames++;
  if (c == kSndNke && (a == address_ || a == kAddrBroadcastReply)) {
    have_access_ = false;  // communication restarts
    stats_.acks++;
    return kSendAck;
  }
  return kNothing;
}

// Long frame: 68 L L 68 C A CI <data> CS 16, CS = sum from C to the end of data
uint8_t Reader::handle_long_() {
  const size_t n = expected_;
  reset();
  uint8_t cs = 0;
  for (size_t i = 4; i < n - 2; i++) cs = static_cast<uint8_t>(cs + buf_[i]);
  if (buf_[n - 1] != kStop || buf_[n - 2] != cs) return fail_(Error::kChecksum);
  stats_.long_frames++;

  const uint8_t c = buf_[4], a = buf_[5];
  uint8_t st = kNothing;
  const bool snd_ud = (c & 0x4F) == 0x43;  // 0x53 / 0x73 (FCB toggles)
  if (snd_ud && (a == address_ || a == kAddrBroadcastReply)) {
    st |= kSendAck;  // every formally valid frame is acknowledged, even if its content does not fit
    stats_.acks++;
  }
  return static_cast<uint8_t>(st | decode_application_(buf_ + 6, n - 8));
}

// Application layer starting at CI. Supported: CI 0x5B / 0x72 with long header (12 bytes),
// security mode 0 (plain) and 5 (AES-128-CBC, IV from address and access number).
uint8_t Reader::decode_application_(uint8_t *app, size_t len) {
  if (len < 1) return fail_(Error::kFrameFormat);
  const uint8_t ci = app[0];
  if (ci != 0x5B && ci != 0x72) return fail_(Error::kUnsupportedCi);
  if (len < 13) return fail_(Error::kFrameFormat);

  const uint8_t *id = app + 1;      // 4 bytes, LSB first
  const uint8_t *man = app + 5;     // 2 bytes
  const uint8_t ver = app[7], med = app[8], acc = app[9];
  const uint16_t cfg = static_cast<uint16_t>(app[11] | (app[12] << 8));
  uint8_t *data = app + 13;  // decrypted in place
  const size_t dlen = len - 13;

  if (have_access_ && acc == last_access_) {
    stats_.duplicates++;  // repetition (our E5 got lost) -> already decoded
    return kNothing;
  }

  const uint8_t mode = (cfg >> 8) & 0x1F;
  plain_off_ = static_cast<size_t>(data - buf_);
  plain_len_ = dlen;
  if (mode == 5) {
    if (!has_key_) return fail_(Error::kNoKey);
    const size_t enc = static_cast<size_t>((cfg >> 4) & 0x0F) * Aes128::kBlockSize;
    if (enc == 0 || enc > dlen) return fail_(Error::kFrameFormat);
    uint8_t iv[16];
    iv[0] = man[0];
    iv[1] = man[1];
    memcpy(iv + 2, id, 4);
    iv[6] = ver;
    iv[7] = med;
    memset(iv + 8, acc, 8);
    aes_.cbc_decrypt(data, enc, iv);
    if (data[0] != 0x2F || data[1] != 0x2F) return fail_(Error::kWrongKey);
  } else if (mode != 0) {
    return fail_(Error::kUnsupportedEncryption);
  }

  Reading r;
  r.header_id = le32(id);
  r.manufacturer = static_cast<uint16_t>(man[0] | (man[1] << 8));
  r.version = ver;
  r.medium = med;
  r.access_number = acc;
  if (!parse_records_(data, dlen, r)) return fail_(Error::kRecordFormat);
  if (r.present == 0) return fail_(Error::kNoData);

  reading_ = r;
  have_reading_ = true;
  last_reading_ms_ = now_ms_;
  have_access_ = true;
  last_access_ = acc;
  last_error_ = Error::kNone;
  stats_.readings++;
  return kReading;
}

// Data records per EN 13757-3: DIF [DIFE...] VIF [VIFE...] data. Unknown records are skipped.
bool Reader::parse_records_(const uint8_t *p, size_t len, Reading &out) {
  size_t i = 0;
  while (i < len) {
    const uint8_t dif = p[i];
    if (dif == 0x2F) {  // idle filler
      i++;
      continue;
    }
    if (dif == 0x0F || dif == 0x1F) break;  // manufacturer specific data follows
    const size_t key_start = i++;
    uint8_t prev = dif;
    for (int n = 0; prev & 0x80; n++) {  // DIFE
      if (i >= len || n >= 10) return false;
      prev = p[i++];
    }
    if (i >= len) return false;
    const uint8_t vif = p[i++];
    if ((vif & 0x7F) == 0x7C) return false;  // plain-text VIF is not used by the AMIS
    prev = vif;
    for (int n = 0; prev & 0x80; n++) {  // VIFE
      if (i >= len || n >= 10) return false;
      prev = p[i++];
    }
    const size_t key_len = i - key_start;

    size_t dlen = AMIS_RD(&kDataLen[dif & 0x0F]);
    if (dlen == 0xFF) {  // variable length
      if (i >= len) return false;
      const uint8_t lvar = p[i++];
      if (lvar > 0xBF) return false;
      dlen = lvar;
    } else if (dlen == 0xFE) {
      dlen = 0;
    }
    if (i + dlen > len) return false;
    const uint8_t *v = p + i;
    i += dlen;

    for (const RecordKey &kf : kRecords) {
      RecordKey k;
      AMIS_MEMCPY_P(&k, &kf, sizeof(k));  // table lives in flash on the ESP8266
      if (k.len != key_len || memcmp(k.bytes, p + key_start, key_len) != 0) continue;
      switch (k.field) {
        case Reading::kTime:
          if (dlen != 6 || !out.time.decode_type_i(v)) continue;
          break;
        case Reading::kEnergyImport: out.energy_import_wh = le32(v); break;
        case Reading::kEnergyExport: out.energy_export_wh = le32(v); break;
        case Reading::kReactiveEnergyImport: out.reactive_energy_import_varh = le32(v); break;
        case Reading::kReactiveEnergyExport: out.reactive_energy_export_varh = le32(v); break;
        case Reading::kPowerImport: out.power_import_w = le32(v); break;
        case Reading::kPowerExport: out.power_export_w = le32(v); break;
        case Reading::kReactivePowerImport: out.reactive_power_import_var = le32(v); break;
        case Reading::kReactivePowerExport: out.reactive_power_export_var = le32(v); break;
        case Reading::kInkasso: out.inkasso = static_cast<int32_t>(le32(v)); break;
        default: continue;
      }
      out.present = static_cast<uint16_t>(out.present | k.field);
      break;
    }
  }
  return true;
}

}  // namespace amis
