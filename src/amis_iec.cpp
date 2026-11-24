// SPDX-License-Identifier: MIT
// libamis - OPTIONAL: IEC 62056-21 mode C, data readout. See the warning in amis_iec.h.
#include "amis_iec.h"

#ifndef LIBAMIS_NO_IEC

#include <string.h>

#include "amis_pgm.h"

namespace amis {

namespace {
constexpr uint8_t kSoh = 0x01, kStx = 0x02, kEtx = 0x03, kAckChar = 0x06;
constexpr uint32_t kReplyDelayMs = 300;     // tr >= 200 ms before our reply
constexpr uint32_t kIdentTimeoutMs = 2500;  // the meter answers within 1500 ms
constexpr uint32_t kStxTimeoutMs = 5000;
constexpr uint32_t kCharTimeoutMs = 2000;   // ta < 1500 ms between two characters

void copy_trim(char *dst, size_t cap, const char *src, size_t n) {
  while (n && (*src == ' ')) {
    src++;
    n--;
  }
  while (n && src[n - 1] == ' ') n--;
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
}  // namespace

namespace {
// Texts in flash on the ESP8266 (see amis_pgm.h)
const char kIecErr0[] AMIS_PROGMEM = "ok";
const char kIecErr1[] AMIS_PROGMEM = "no identification from meter";
const char kIecErr2[] AMIS_PROGMEM = "timeout";
const char kIecErr3[] AMIS_PROGMEM = "block check character error";
const char kIecErr4[] AMIS_PROGMEM = "data line too long or unreadable";
const char kIecErr5[] AMIS_PROGMEM = "readout already running";
const char kIecErrUnknown[] AMIS_PROGMEM = "?";
}  // namespace

const char *error_str(IecReader::Error e) {
  switch (e) {
    case IecReader::Error::kNone: return detail::pgm_str(kIecErr0);
    case IecReader::Error::kNoIdent: return detail::pgm_str(kIecErr1);
    case IecReader::Error::kTimeout: return detail::pgm_str(kIecErr2);
    case IecReader::Error::kBcc: return detail::pgm_str(kIecErr3);
    case IecReader::Error::kFormat: return detail::pgm_str(kIecErr4);
    case IecReader::Error::kBusy: return detail::pgm_str(kIecErr5);
  }
  return detail::pgm_str(kIecErrUnknown);
}

uint32_t IecReader::baud_for_char(char z) {
  switch (z) {
    case '0': return 300;
    case '1': return 600;
    case '2': return 1200;
    case '3': return 2400;
    case '4': return 4800;
    case '5': return 9600;
    case '6': return 19200;
    case '9': return 115200;
    default: return 0;
  }
}

void IecReader::set_address(const char *addr) {
  size_t n = addr ? strlen(addr) : 0;
  if (n > 32) n = 32;
  memcpy(address_, addr ? addr : "", n);
  address_[n] = 0;
}

void IecReader::set_tx_(const char *s) {
  tx_len_ = strlen(s);
  if (tx_len_ > sizeof(tx_)) tx_len_ = sizeof(tx_);
  memcpy(tx_, s, tx_len_);
}

void IecReader::abort() { state_ = State::kIdle; }

uint8_t IecReader::fail_(Error e) {
  error_ = e;
  state_ = State::kIdle;
  return kFailed;
}

uint8_t IecReader::begin(Mode mode, uint32_t now_ms) {
  if (active()) return fail_(Error::kBusy);
  mode_ = mode;
  error_ = Error::kNone;
  manufacturer_[0] = 0;
  ident_[0] = 0;
  ident_z_ = 0;
  serial_[0] = 0;
  memset(&line_, 0, sizeof(line_));
  len_ = 0;
  baud_ = 300;
  start_ms_ = state_ms_ = last_rx_ms_ = now_ms;
  // Request "/?<address>!" CR LF
  char req[40] = "/?";
  strncat(req, address_, sizeof(req) - 6);
  strcat(req, "!\r\n");
  set_tx_(req);
  state_ = State::kWaitIdent;
  return kSetSerial | kSend;
}

uint8_t IecReader::finish_ident_(uint32_t now_ms) {
  // buf_ = "/XXXZidentification" (without CR LF)
  memcpy(manufacturer_, buf_ + 1, 3);
  manufacturer_[3] = 0;
  ident_z_ = buf_[4];
  copy_trim(ident_, sizeof(ident_), buf_ + 5, len_ - 5);
  if (mode_ == Mode::kIdentOnly) {
    // Break: SOH B 0 ETX BCC (BCC = XOR over "B0" ETX), so the meter does not keep sending at 300 Bd
    const uint8_t brk[5] = {kSoh, 'B', '0', kEtx, static_cast<uint8_t>('B' ^ '0' ^ kEtx)};
    memcpy(tx_, brk, sizeof(brk));
    tx_len_ = sizeof(brk);
  } else {
    char z = (baud_for_char(ident_z_) != 0) ? ident_z_ : '0';
    if (baud_for_char(max_z_) != 0 && max_z_ < z) z = max_z_;
    const char ack[7] = {static_cast<char>(kAckChar), '0', z, '0', '\r', '\n', 0};
    set_tx_(ack);
    baud_ = baud_for_char(z);
  }
  state_ = State::kSendAck;
  state_ms_ = now_ms;
  return kNone;
}

uint8_t IecReader::handle_data_line_() {
  buf_[len_] = 0;
  char *open = strchr(buf_, '(');
  if (!open) return kNone;  // e.g. "!" or an empty line
  char *close = strchr(open, ')');
  if (!close) return kNone;
  memset(&line_, 0, sizeof(line_));
  copy_trim(line_.obis, sizeof(line_.obis), buf_, static_cast<size_t>(open - buf_));
  char *star = static_cast<char *>(memchr(open, '*', static_cast<size_t>(close - open)));
  const char *vend = star ? star : close;
  copy_trim(line_.value, sizeof(line_.value), open + 1, static_cast<size_t>(vend - open - 1));
  if (star) copy_trim(line_.unit, sizeof(line_.unit), star + 1, static_cast<size_t>(close - star - 1));
  if (close[1] == '(') {
    char *close2 = strchr(close + 2, ')');
    if (close2) copy_trim(line_.extra, sizeof(line_.extra), close + 2, static_cast<size_t>(close2 - close - 2));
  }
  if (strcmp(line_.obis, "0.0.0") == 0) {  // device address = serial number
    strncpy(serial_, line_.value, sizeof(serial_) - 1);
    serial_[sizeof(serial_) - 1] = 0;
  }
  return kLine;
}

uint8_t IecReader::push(uint8_t byte, uint32_t now_ms) {
  if (state_ == State::kIdle) return kNone;
  const char c = static_cast<char>(byte & 0x7F);
  last_rx_ms_ = now_ms;

  switch (state_) {
    case State::kWaitIdent:
      if (c == '/') {
        len_ = 0;
        buf_[len_++] = c;
      } else if (len_ > 0) {
        if (c == '\n') {
          // valid: "/" + 3 letters + baud rate code (an echo of "/?!" is filtered out here)
          if (len_ >= 5 && is_letter(buf_[1]) && is_letter(buf_[2]) && is_letter(buf_[3])) return finish_ident_(now_ms);
          len_ = 0;
        } else if (c != '\r') {
          if (len_ < sizeof(buf_) - 1)
            buf_[len_++] = c;
          else
            len_ = 0;
        }
      }
      return kNone;

    case State::kSendAck:
      return kNone;  // the meter should be silent now

    case State::kWaitStx:
      if (byte == kStx || c == kStx) {
        state_ = State::kData;
        bcc_ = 0;
        len_ = 0;
      }
      return kNone;

    case State::kData:
      bcc_ ^= static_cast<uint8_t>(c);
      if (c == kEtx) {
        state_ = State::kWaitBcc;
        return kNone;
      }
      if (c == '\n') {
        const uint8_t r = (len_ > 0) ? static_cast<uint8_t>(handle_data_line_() | kRawLine) : static_cast<uint8_t>(kNone);
        len_ = 0;
        return r;
      }
      if (c == '\r') return kNone;
      if (len_ >= sizeof(buf_) - 1) return fail_(Error::kFormat);
      buf_[len_++] = c;
      return kNone;

    case State::kWaitBcc:
      if (static_cast<uint8_t>(c) != (bcc_ & 0x7F)) return fail_(Error::kBcc);
      state_ = State::kIdle;
      return kDone;

    case State::kIdle:
      break;
  }
  return kNone;
}

uint8_t IecReader::poll(uint32_t now_ms) {
  if (state_ == State::kIdle) return kNone;
  if (static_cast<uint32_t>(now_ms - start_ms_) > total_timeout_ms_) return fail_(Error::kTimeout);
  const uint32_t in_state = now_ms - state_ms_;
  switch (state_) {
    case State::kWaitIdent:
      if (in_state > kIdentTimeoutMs) return fail_(Error::kNoIdent);
      break;
    case State::kSendAck:
      if (in_state < kReplyDelayMs) break;
      if (mode_ == Mode::kIdentOnly) {
        state_ = State::kIdle;
        return kSend | kDone;
      }
      state_ = State::kWaitStx;
      state_ms_ = now_ms;
      return static_cast<uint8_t>(kSend | (baud_ != 300 ? kSetSerial : 0));
    case State::kWaitStx:
      if (in_state > kStxTimeoutMs) return fail_(Error::kTimeout);
      break;
    case State::kData:
    case State::kWaitBcc:
      if (static_cast<uint32_t>(now_ms - last_rx_ms_) > kCharTimeoutMs) return fail_(Error::kTimeout);
      break;
    case State::kIdle:
      break;
  }
  return kNone;
}

}  // namespace amis

#endif  // LIBAMIS_NO_IEC
