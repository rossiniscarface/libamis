// SPDX-License-Identifier: MIT
// libamis - OPTIONAL: readout via the service interface per IEC 62056-21 (mode C, data readout only).
//
// Note: The Netz OOe customer interface specification (2021-11-05) only describes the M-Bus protocol and
// limits the answers of the M-Bus slave to E5 (section 2.3.10). It does not cover an IEC 62056-21 readout;
// Netz OOe uses the optical port as its service interface as well (section 4). Use at your own
// responsibility. While a readout runs, the meter sends no M-Bus data (up to about 1 min afterwards, until
// it has found the M-Bus slave again). Nothing is sent until begin() is called. With -DLIBAMIS_NO_IEC this
// part is not compiled.
//
// Sequence (Siemens TD-351x user manual D23-510-1.02, section 5.2):
//   300 Bd 7E1:  -> "/?!" CR LF            <- "/SAT6<identification>" CR LF
//   kIdentOnly:  -> break (SOH B0 ETX BCC), done. Yields manufacturer and identification (part/serial number).
//   kReadout:    -> ACK "0" Z "0" CR LF (Z = baud rate), then switch to Z baud
//                <- STX "0.0.0(...)" CR LF ... "!" CR LF ETX BCC  (billing data set, line by line)
// Afterwards switch back to 9600 8E1 and reset the M-Bus reader. During the readout the meter sends no
// M-Bus data; it may take up to about 1 min until it sends again (search mode).
//
// The class does NOT touch hardware. It says what to do (send, change baud rate) and gets received
// bytes via push(). Timing via poll(now_ms).
#pragma once

#ifndef LIBAMIS_NO_IEC

#include <stddef.h>
#include <stdint.h>

namespace amis {

// One data line, e.g. 1.8.0(001234.567*kWh) or 1.6.0(01.234*kW)(2611231445)
struct IecLine {
  char obis[20];
  char value[32];
  char unit[8];
  char extra[32];  // content of a second bracket (e.g. timestamp), else empty
};

class IecReader {
 public:
  enum class Mode : uint8_t { kIdentOnly, kReadout };

  enum Action : uint8_t {
    kNone = 0,
    kSend = 1 << 0,       // send tx() (then flush before changing the baud rate!)
    kSetSerial = 1 << 1,  // switch the UART to baud() with 7E1 (AFTER sending tx())
    kLine = 1 << 2,       // line() holds a new data line
    kDone = 1 << 3,       // done -> UART back to 9600 8E1, amis::Reader::reset()
    kFailed = 1 << 4,     // aborted, error() tells why -> also back to 9600 8E1
    kRawLine = 1 << 5,    // raw_line() holds a data line as received (every non-empty line, also "!")
  };

  // kMeterError: the meter answered with an error telegram (ERxxxx), see meter_error()
  // kFormat is no longer reported since 0.3.0 (an overlong line is truncated, see raw_line_truncated())
  enum class Error : uint8_t { kNone, kNoIdent, kTimeout, kBcc, kFormat, kMeterError };

  // Highest wanted data baud rate as IEC code ('0'=300 ... '5'=9600, '6'=19200, '9'=115200).
  // The meter only switches if the acknowledgement repeats exactly the code from its identification
  // (Siemens manual 5.2.1; the TD-351x always reports '6' = 19200). So the code of the meter is used if it is
  // not higher than this value, otherwise everything stays at 300 Bd. Default '0': always 300 Bd (proven,
  // the billing data set then takes about 7 min). With '6' the TD-351x sends the data at 19200 Bd.
  void set_max_baud_char(char z) { max_z_ = z; }
  // How often the request is sent if the meter does not answer (default 3). The TD-351x only wakes up its
  // IEC mode with the first received character, so the request usually has to be repeated (manual 5.2.12.2).
  void set_request_tries(uint8_t n) { request_tries_ = n ? n : 1; }
  // Pause between two requests without answer (default 5000 ms). A real TD-351x did not answer requests
  // 2 s apart (the 1500..2200 ms of the manual), but does with 5 s like the original firmware.
  void set_request_interval_ms(uint32_t ms) { request_interval_ms_ = ms; }
  // Delay between the identification and our acknowledgement or break (default 700 ms; the manual allows
  // 200..1500 ms, a real TD-351x answered ER0007 to an acknowledgement after 300 ms).
  void set_reply_delay_ms(uint32_t ms) { reply_delay_ms_ = ms; }
  // How often the acknowledgement is sent if the meter answers it with an error telegram or sends no data
  // (default 3), and the time between two acknowledgements (default 7000 ms, like the original firmware).
  // After a switch to a faster baud rate a repetition first returns kSetSerial alone (back to 300 Bd) and is then
  // sent as ACK 0 0 0 at 300 Bd.
  void set_ack_tries(uint8_t n) { ack_tries_ = n ? n : 1; }
  void set_ack_retry_ms(uint32_t ms) { ack_retry_ms_ = ms; }
  // Readout without acknowledgement (manual fig. 5-1): after the identification nothing is sent, the meter sends
  // the data at 300 Bd by itself after its timeout.
  void set_send_ack(bool on) { send_ack_ = on; }
  // Device address for the request ("/?<address>!"), empty = general address. At most 32 characters, only
  // digits, letters and spaces (manual 5.2.8.1); otherwise returns false and keeps the previous address.
  bool set_address(const char *addr);
  // Limit the time from begin() until the data starts (default 120 s). It does not apply while data arrives:
  // then the readout ends if no character comes for 2 s.
  void set_total_timeout_ms(uint32_t ms) { total_timeout_ms_ = ms; }

  // Starts the readout. Returns kSetSerial (switch to 300 Bd 7E1 now); the request "/?!" follows as kSend from
  // the next poll(). Rule for all actions: first send tx(), then switch the baud rate.
  // While a readout runs (active()), begin() changes nothing and returns kNone: check active() first.
  uint8_t begin(Mode mode, uint32_t now_ms);
  // Received byte (the parity bit is ignored)
  uint8_t push(uint8_t byte, uint32_t now_ms);
  // Call regularly (e.g. in loop()): delayed sending, timeouts
  uint8_t poll(uint32_t now_ms);
  void abort();

  bool active() const { return state_ != State::kIdle; }
  const uint8_t *tx(size_t *len) const {
    *len = tx_len_;
    return tx_;
  }
  uint32_t baud() const { return baud_; }
  const IecLine &line() const { return line_; }
  // Last data line exactly as received, without CR LF. Only valid right after push() returned kRawLine.
  const char *raw_line() const { return buf_; }
  // The last raw line was longer than 95 characters and is cut off (it is not reported as kLine). Such a line
  // does not end the readout; truncated_lines() counts them since begin().
  bool raw_line_truncated() const { return raw_truncated_; }
  uint16_t truncated_lines() const { return truncated_lines_; }
  Error error() const { return error_; }

  // Result of the identification, e.g. "SAT" / '6' / "2351102623000045"
  const char *manufacturer() const { return manufacturer_; }
  char baud_char() const { return ident_z_; }
  const char *identification() const { return ident_; }
  // Error code of an error telegram from the meter, e.g. "ER0007" (manual 5.2.11), else empty
  const char *meter_error() const { return meter_error_; }
  // Serial number: 0.0.0 from the data set (device address = serial number), else the identification
  const char *serial_number() const { return serial_[0] ? serial_ : ident_; }

  static uint32_t baud_for_char(char z);

 private:
  enum class State : uint8_t { kIdle, kWaitIdent, kSendAck, kWaitStx, kData, kWaitBcc };
  uint8_t fail_(Error e);
  uint8_t finish_ident_(uint32_t now_ms);
  uint8_t handle_data_line_();
  uint8_t end_of_data_();
  uint8_t data_line_();
  uint8_t ack_due_(uint32_t now_ms);
  void set_tx_(const char *s);

  State state_ = State::kIdle;
  Mode mode_ = Mode::kReadout;
  Error error_ = Error::kNone;
  char max_z_ = '0';
  uint8_t request_tries_ = 3;
  uint32_t request_interval_ms_ = 5000;
  uint32_t reply_delay_ms_ = 700;
  uint8_t ack_tries_ = 3;
  uint32_t ack_retry_ms_ = 7000;
  uint32_t ack_ms_ = 0;
  uint32_t wait_ms_ = 0;
  bool send_ack_ = true;
  bool no_stx_ = false;     // data block started without STX (no block check)
  bool uart_fast_ = false;  // the caller's UART is at a data baud rate above 300 Bd
  bool skip_echo_ = false;  // ignoring an echo of our acknowledgement up to LF
  bool err_line_ = false;   // the last line was an error telegram
  bool truncated_ = false;  // the current data line is longer than buf_
  bool raw_truncated_ = false;  // the last raw line was truncated
  uint16_t truncated_lines_ = 0;
  uint8_t acks_sent_ = 0;
  uint8_t requests_sent_ = 0;
  char meter_error_[9] = {0};
  char address_[33] = {0};
  uint32_t total_timeout_ms_ = 120000;
  uint32_t start_ms_ = 0;
  uint32_t last_rx_ms_ = 0;
  uint32_t state_ms_ = 0;
  uint32_t baud_ = 300;

  uint8_t tx_[40];
  size_t tx_len_ = 0;

  char buf_[96];
  size_t len_ = 0;
  uint8_t bcc_ = 0;

  char manufacturer_[4] = {0};
  char ident_z_ = 0;
  char ident_[33] = {0};
  char serial_[33] = {0};
  IecLine line_;
};

// Short English text for an error. On the ESP8266 the texts live in flash and are copied into one shared
// buffer, so the result is only valid until the next error_str() call.
const char *error_str(IecReader::Error e);

}  // namespace amis

#endif  // LIBAMIS_NO_IEC
