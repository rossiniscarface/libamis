// SPDX-License-Identifier: MIT
// libamis - OPTIONAL: IEC 62056-21 mode C, data readout. See the warning in amis_iec.h.
#include "amis_iec.h"

#ifndef LIBAMIS_NO_IEC

#include <string.h>

#include "amis_pgm.h"

namespace amis {

namespace {
constexpr uint8_t kSoh = 0x01, kStx = 0x02, kEtx = 0x03, kAckChar = 0x06;
constexpr uint32_t kIdentTimeoutMs = 2500;  // the meter answers within 1500 ms
constexpr uint32_t kStxTimeoutMs = 12000;  // the meter sends without acknowledgement after its timeout
constexpr uint32_t kCharTimeoutMs = 2000;   // ta < 1500 ms between two characters
constexpr uint32_t kMinDataWaitMs = 2500;   // wait at least this long for data after the last acknowledgement

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
const char kIecErr5[] AMIS_PROGMEM = "meter reported an error";
const char kIecErrUnknown[] AMIS_PROGMEM = "?";
}  // namespace

const char *error_str(IecReader::Error e) {
  switch (e) {
    case IecReader::Error::kNone: return detail::pgm_str(kIecErr0);
    case IecReader::Error::kNoIdent: return detail::pgm_str(kIecErr1);
    case IecReader::Error::kTimeout: return detail::pgm_str(kIecErr2);
    case IecReader::Error::kBcc: return detail::pgm_str(kIecErr3);
    case IecReader::Error::kFormat: return detail::pgm_str(kIecErr4);
    case IecReader::Error::kMeterError: return detail::pgm_str(kIecErr5);
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

bool IecReader::set_address(const char *addr) {
  if (!addr) addr = "";
  size_t n = 0;
  for (; addr[n]; n++) {
    const char c = addr[n];
    if (n >= 32 || !((c >= '0' && c <= '9') || is_letter(c) || c == ' ')) return false;
  }
  memcpy(address_, addr, n);
  address_[n] = 0;
  return true;
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
  if (active()) return kNone;  // a readout is running: leave it alone
  mode_ = mode;
  error_ = Error::kNone;
  manufacturer_[0] = 0;
  ident_[0] = 0;
  ident_z_ = 0;
  meter_error_[0] = 0;
  requests_sent_ = 0;
  acks_sent_ = 1;
  uart_fast_ = false;
  skip_echo_ = false;
  err_line_ = false;
  truncated_ = false;
  truncated_lines_ = 0;
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
  return kSetSerial;  // the request is sent by the next poll(), after the switch to 300 Bd
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
  } else if (!send_ack_) {
    // Readout without acknowledgement (manual 5.2.2, fig. 5-1): the meter sends the data at 300 Bd by itself
    baud_ = 300;
    state_ = State::kWaitStx;
    state_ms_ = now_ms;
    skip_echo_ = false;
    return kNone;
  } else {
    // The meter only switches if Z equals the code from its identification (manual 5.2.1), so either that
    // code or '0' (300 Bd), never something in between
    char z = '0';
    if (baud_for_char(ident_z_) != 0 && baud_for_char(max_z_) != 0 && ident_z_ <= max_z_) z = ident_z_;
    const char ack[7] = {static_cast<char>(kAckChar), '0', z, '0', '\r', '\n', 0};
    set_tx_(ack);
    baud_ = baud_for_char(z);
  }
  state_ = State::kSendAck;
  state_ms_ = now_ms;
  wait_ms_ = reply_delay_ms_;
  return kNone;
}

uint8_t IecReader::handle_data_line_() {
  buf_[len_] = 0;
  err_line_ = false;
  // Error telegram: "(ERxxxx)" per manual 5.2.11; a real TD-351x sends it without brackets ("ERxxxx")
  {
    const char *e = (buf_[0] == '(') ? buf_ + 1 : buf_;
    if (e[0] == 'E' && e[1] == 'R' && e[2] >= '0' && e[2] <= '9') {
      size_t k = 0;
      while (k < sizeof(meter_error_) - 1 && e[k] && e[k] != ')') {
        meter_error_[k] = e[k];
        k++;
      }
      meter_error_[k] = 0;
      err_line_ = true;
      return kNone;
    }
  }
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

uint8_t IecReader::data_line_() {
  raw_truncated_ = truncated_;
  if (truncated_) {
    // too long for buf_: no parsed line (the values may be cut off), only the raw line, marked as truncated
    truncated_lines_++;
    buf_[len_] = 0;
    err_line_ = false;
    return kRawLine;
  }
  const uint8_t r = handle_data_line_();
  if (err_line_) return r;  // error telegrams are no data lines
  if (r & kLine) meter_error_[0] = 0;  // data follows after all: the error telegram no longer applies
  return static_cast<uint8_t>(r | kRawLine);
}

uint8_t IecReader::ack_due_(uint32_t now_ms) {
  if (uart_fast_) {
    // The UART is still at the data baud rate of the previous acknowledgement, but the meter answers a refused
    // or ignored one at 300 Bd: switch back first and repeat it for 300 Bd (Z = '0', tx_ = ACK 0 Z 0 CR LF)
    uart_fast_ = false;
    baud_ = 300;
    tx_[2] = '0';
    state_ = State::kSendAck;
    state_ms_ = now_ms;
    wait_ms_ = 0;
    return kSetSerial;
  }
  state_ = State::kWaitStx;
  state_ms_ = now_ms;
  ack_ms_ = now_ms;
  skip_echo_ = false;
  if (baud_ == 300) return kSend;
  uart_fast_ = true;
  return static_cast<uint8_t>(kSend | kSetSerial);
}

uint8_t IecReader::end_of_data_() {
  if (meter_error_[0] && acks_sent_ < ack_tries_) {
    // The meter refused the acknowledgement (a real TD-351x answers the first one with ER0007): send it again
    // ack_retry_ms_ after the previous one, still in the same session, like the original firmware does
    acks_sent_++;
    meter_error_[0] = 0;
    len_ = 0;
    state_ = State::kSendAck;
    state_ms_ = ack_ms_;
    wait_ms_ = ack_retry_ms_;
    return kNone;
  }
  if (meter_error_[0]) return fail_(Error::kMeterError);
  state_ = State::kIdle;
  return kDone;
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
        } else if (c == '\r') {
          // end of the identification follows with LF
        } else if (c < 0x20 || c == 0x7F) {
          // not printable: noise (e.g. M-Bus frames at 9600 Bd seen at 300 Bd), no identification. Dropping it
          // also lets poll() repeat the request instead of waiting for an identification that is not coming.
          len_ = 0;
        } else {
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
      // An optical echo of our own acknowledgement (ACK 0 Z 0 CR LF) is no data
      if (c == kAckChar) {
        skip_echo_ = true;
        return kNone;
      }
      if (skip_echo_) {
        if (c == '\n') skip_echo_ = false;
        return kNone;
      }
      if (byte == kStx || c == kStx) {
        state_ = State::kData;
        bcc_ = 0;
        len_ = 0;
        no_stx_ = false;
        truncated_ = false;
        return kNone;
      }
      // A real TD-351x sends the data set after a repeated acknowledgement without STX, starting directly
      // with "F.F(". Take it as data (only then, or without acknowledgement); there is no defined start for
      // the block check character then.
      if (send_ack_ && acks_sent_ < 2) return kNone;
      if (c <= ' ' || c >= 0x7F || c == '/' || c == '!') return kNone;
      state_ = State::kData;
      bcc_ = 0;
      len_ = 0;
      no_stx_ = true;
      truncated_ = false;
      [[fallthrough]];  // process this character as data

    case State::kData:
      bcc_ ^= static_cast<uint8_t>(c);
      if (c == kEtx) {
        // a line without CR LF before ETX, e.g. the error telegram STX (ERxxxx) ETX BCC
        const uint8_t r = (len_ > 0) ? data_line_() : static_cast<uint8_t>(kNone);
        len_ = 0;
        truncated_ = false;
        state_ = State::kWaitBcc;
        return r;
      }
      if (c == '\n') {
        const uint8_t r = (len_ > 0) ? data_line_() : static_cast<uint8_t>(kNone);
        len_ = 0;
        truncated_ = false;
        return r;
      }
      if (c == '\r') return kNone;
      if (len_ >= sizeof(buf_) - 1) {
        truncated_ = true;  // keep reading: drop the rest of this line
        return kNone;
      }
      buf_[len_++] = c;
      return kNone;

    case State::kWaitBcc:
      if (!no_stx_ && static_cast<uint8_t>(c) != (bcc_ & 0x7F)) return fail_(Error::kBcc);
      return end_of_data_();

    case State::kIdle:
      break;
  }
  return kNone;
}

uint8_t IecReader::poll(uint32_t now_ms) {
  if (state_ == State::kIdle) return kNone;
  // The total timeout only covers the time up to the data: the billing data set alone takes about 7 min at
  // 300 Bd. While data arrives, the character timeout guards the readout.
  if (state_ != State::kData && state_ != State::kWaitBcc &&
      static_cast<uint32_t>(now_ms - start_ms_) > total_timeout_ms_)
    return fail_(Error::kTimeout);
  const uint32_t in_state = now_ms - state_ms_;
  switch (state_) {
    case State::kWaitIdent:
      if (requests_sent_ == 0) {  // first request
        requests_sent_ = 1;
        state_ms_ = now_ms;
        return kSend;
      }
      // an identification is arriving right now: neither repeat the request nor give up
      if (len_ > 0 && static_cast<uint32_t>(now_ms - last_rx_ms_) < kCharTimeoutMs) break;
      if (requests_sent_ < request_tries_) {
        if (in_state > request_interval_ms_) {
          // no (complete) answer: send the request again (tx_ still holds it)
          requests_sent_++;
          len_ = 0;
          state_ms_ = now_ms;
          return kSend;
        }
      } else if (in_state > kIdentTimeoutMs) {
        return fail_(Error::kNoIdent);
      }
      break;
    case State::kSendAck:
      if (in_state < wait_ms_) break;
      if (mode_ == Mode::kIdentOnly) {
        state_ = State::kIdle;
        return kSend | kDone;
      }
      return ack_due_(now_ms);
    case State::kWaitStx:
      if (!send_ack_) {
        if (in_state > kStxTimeoutMs) return fail_(Error::kTimeout);
      } else if (acks_sent_ >= ack_tries_) {
        const uint32_t wait = ack_retry_ms_ > kMinDataWaitMs ? ack_retry_ms_ : kMinDataWaitMs;
        if (in_state > wait) return fail_(Error::kTimeout);
      } else if (in_state > ack_retry_ms_) {
        acks_sent_++;  // no data after the acknowledgement: send it again (like the original firmware)
        return ack_due_(now_ms);
      }
      break;
    case State::kData:
      if (static_cast<uint32_t>(now_ms - last_rx_ms_) > kCharTimeoutMs) return fail_(Error::kTimeout);
      break;
    case State::kWaitBcc:
      // the block check character is optional (manual 5.2.8): after ETX without BCC the data is complete
      if (static_cast<uint32_t>(now_ms - last_rx_ms_) > kCharTimeoutMs) return end_of_data_();
      break;
    case State::kIdle:
      break;
  }
  return kNone;
}

}  // namespace amis

#endif  // LIBAMIS_NO_IEC
