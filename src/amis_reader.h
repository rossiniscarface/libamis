// SPDX-License-Identifier: MIT
// libamis - reader core for the Siemens AMIS smart meter (TD-3511/TD-3512, Netz Oberoesterreich).
//
// On the optical customer interface the meter is the M-Bus master (9600 baud, 8E1):
//   * about once a minute it looks for a reading head with SND_NKE (10 40 F0 30 16) -> reply E5
//   * then it sends one SND_UD long frame per second with AES-128-CBC encrypted data
//     (OMS security mode 5) -> every valid frame is acknowledged with E5.
// Per the Netz OOe specification ("AMIS-Zaehler Kundenschnittstelle", 2021-11-05): E5 to the search request
// within 0.5 s (2.3.7); for SND_UD the M-Bus reply window applies (EN 13757-2: 11 bit times up to 330 bit times
// + 50 ms after the frame, about 85 ms at 9600 Bd), so call push() soon after the bytes arrive and send E5
// right away. After 10 missing acknowledgements the meter falls back to search mode. As M-Bus slave the
// reading head may ONLY answer with E5 (section 2.3.10). The interface only works with the terminal cover closed.
//
// The class does NOT touch hardware. The caller feeds received bytes with push() and gets back what to do
// (send E5, new reading, error). No heap, no exceptions, no Arduino - runs as is on ESP8266 (Arduino),
// ESP32 (ESPHome/ESP-IDF) and on a PC.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "amis_aes128.h"

// Size of the frame buffer. 261 = largest M-Bus long frame (L = 255). The AMIS sends 101-byte frames, so
// -DLIBAMIS_MAX_FRAME=128 saves RAM; longer frames are then dropped (resynchronisation).
#ifndef LIBAMIS_MAX_FRAME
#define LIBAMIS_MAX_FRAME 261
#endif

namespace amis {

// Date/time of the meter (M-Bus type I, 6 bytes, local time with DST bit)
struct DateTime {
  uint16_t year = 0;   // 2000..2127
  uint8_t month = 0;   // 1..12
  uint8_t day = 0;     // 1..31
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 0;  // 1 = Monday .. 7 = Sunday, 0 = not given
  uint8_t week = 0;     // calendar week, 0 = not given
  bool dst = false;     // daylight saving time
  bool valid = false;

  // Decodes 6 bytes of M-Bus type I. Returns false for implausible values.
  bool decode_type_i(const uint8_t b[6]);
  // Seconds since 1970 (UTC), assuming the meter runs on CET/CEST (Austria).
  int64_t to_unix_utc() const;
};

// One data set as sent by the meter every second. Units as sent by the meter: Wh, varh, W, var.
struct Reading {
  DateTime time;
  uint32_t energy_import_wh = 0;            // 1.8.0  active energy import A+
  uint32_t energy_export_wh = 0;            // 2.8.0  active energy export A-
  uint32_t reactive_energy_import_varh = 0;  // 3.8.1  reactive energy R+ (tariff 1)
  uint32_t reactive_energy_export_varh = 0;  // 4.8.1  reactive energy R- (tariff 1)
  uint32_t power_import_w = 0;              // 1.7.0  active power import P+
  uint32_t power_export_w = 0;              // 2.7.0  active power export P-
  uint32_t reactive_power_import_var = 0;    // 3.7.0  reactive power Q+
  uint32_t reactive_power_export_var = 0;    // 4.7.0  reactive power Q-
  int32_t inkasso = 0;                       // 1.128.0 prepayment ("Inkasso") register in Wh (signed)

  // Which fields were present in the last frame (bits from Field)
  enum Field : uint16_t {
    kTime = 1 << 0,
    kEnergyImport = 1 << 1,
    kEnergyExport = 1 << 2,
    kReactiveEnergyImport = 1 << 3,
    kReactiveEnergyExport = 1 << 4,
    kPowerImport = 1 << 5,
    kPowerExport = 1 << 6,
    kReactivePowerImport = 1 << 7,
    kReactivePowerExport = 1 << 8,
    kInkasso = 1 << 9,
    kAll = 0x3FF,  // all of the above
  };
  uint16_t present = 0;
  bool has(Field f) const { return (present & f) != 0; }

  // Net power import - export in W (positive = drawing from the grid)
  int32_t power_balance_w() const {
    return static_cast<int32_t>(power_import_w) - static_cast<int32_t>(power_export_w);
  }

  // From the M-Bus header: the secondary address of the reading head (terminal device), fixed by Netz OOe
  // 2.4.2.6 and the same for every meter. It is NOT the meter number: that only comes from the IEC
  // identification or register 0.0.0 of the billing data set (IecReader).
  uint32_t header_id = 0;       // identification number (BCD, as sent; 0 on the AMIS)
  uint16_t manufacturer = 0;    // manufacturer code (0x4C2D = "SAM" on the AMIS)
  uint8_t version = 0;          // 0x01 on the AMIS
  uint8_t medium = 0;           // 0x0E (bus/system component) on the AMIS
  uint8_t access_number = 0;    // running counter, +1 per frame
};

enum class Error : uint8_t {
  kNone = 0,
  kChecksum,                // wrong checksum or stop byte
  kFrameFormat,             // implausible long frame header
  kUnsupportedCi,           // unknown CI field
  kUnsupportedEncryption,   // security mode other than 0 or 5
  kNoKey,                   // encrypted, but no key set
  kWrongKey,                // no 2F 2F after decryption -> wrong key
  kRecordFormat,            // data records not parseable
  kNoData,                  // frame without any known value
  kIncomplete,              // a value required with set_required_fields() is missing
};
// Short English text for an error. On the ESP8266 the texts live in flash and are copied into one shared
// buffer, so the result is only valid until the next error_str() call.
const char *error_str(Error e);

struct Stats {
  uint32_t bytes = 0;
  uint32_t short_frames = 0;    // valid short frames (SND_NKE)
  uint32_t long_frames = 0;     // valid long frames
  uint32_t readings = 0;        // successfully decoded
  uint32_t duplicates = 0;      // repeated frame (same access number)
  uint32_t acks = 0;            // E5 requested
  uint32_t checksum_errors = 0;
  uint32_t key_errors = 0;
  uint32_t other_errors = 0;
  uint32_t resyncs = 0;         // byte gap or garbage -> frame search restarted
  uint32_t line_errors = 0;     // UART errors reported with push_error()
};

class Reader {
 public:
  static constexpr uint8_t kAck = 0xE5;
  static constexpr size_t kMaxFrame = LIBAMIS_MAX_FRAME;
  static_assert(kMaxFrame >= 101 && kMaxFrame <= 261, "LIBAMIS_MAX_FRAME must be 101..261");

  // Return value of push(): bits that may be set together
  enum Status : uint8_t {
    kNothing = 0,
    kSendAck = 1 << 0,   // send Reader::kAck (0xE5) to the meter now
    kReading = 1 << 1,   // reading() holds new values
    kError = 1 << 2,     // last_error() tells why
  };

  Reader();

  // Key from the Netz OOe customer portal, 32 hex digits (spaces, colons, dashes allowed)
  bool set_key_hex(const char *hex);
  void set_key(const uint8_t key[16]);
  bool has_key() const { return has_key_; }

  // M-Bus primary address of the reading head (the AMIS looks for 240 = 0xF0)
  void set_primary_address(uint8_t addr) { address_ = addr; }
  // A longer gap inside a frame discards the frame (default 500 ms). now_ms is the processing time, not the
  // receive time: a 101-byte frame takes about 116 ms, the next one follows about 885 ms later, so 500 ms
  // tolerates a stalled main loop and still separates two frames.
  void set_interbyte_timeout_ms(uint32_t ms) { interbyte_timeout_ms_ = ms; }

  // Fields (Reading::Field bits) every frame must contain; if one is missing, push() reports kError with
  // Error::kIncomplete instead of kReading. Default 0 = no requirement. The AMIS always sends all 10 values
  // (Netz OOe table 2), so Reading::kAll is a good choice.
  void set_required_fields(uint16_t mask) { required_ = mask; }

  // Process one received byte. now_ms = monotonic time in ms (millis(), esp_timer, ...).
  // On kSendAck acknowledge immediately (call byte by byte so exactly one E5 goes out per frame).
  uint8_t push(uint8_t byte, uint32_t now_ms);

  // Report a UART line error (parity, framing or overrun) seen while receiving. Drops the frame in progress:
  // no E5, no reading. Counted in Stats::line_errors.
  // M-Bus (EN 13757-2, format FT 1.2) relies on the even parity of every character together with the checksum:
  // the 8-bit sum alone misses two bit errors that cancel out. So read the UART in blocks, check its error
  // flag BEFORE the block goes to push(), and on an error drop the block and call push_error() instead.
  // ESP8266 Arduino: Serial.hasRxError() / Serial.hasOverrun(); ESP-IDF: events UART_PARITY_ERR,
  // UART_FRAME_ERR, UART_FIFO_OVF; Arduino-ESP32: HardwareSerial::onReceiveError().
  void push_error(uint32_t now_ms);

  const Reading &reading() const { return reading_; }
  Error last_error() const { return last_error_; }
  const Stats &stats() const { return stats_; }
  // Is the last valid reading younger than max_age_ms?
  bool online(uint32_t now_ms, uint32_t max_age_ms = 5000) const;

  // Decrypted payload of the last long frame (for debugging). It is decrypted in place in the frame buffer,
  // so it is only valid until the next call of push().
  const uint8_t *last_plaintext(size_t *len) const {
    if (len) *len = plain_len_;
    return buf_ + plain_off_;
  }

  // Reset the frame search (e.g. after changing the baud rate)
  void reset();

 private:
  uint8_t feed_(uint8_t byte);
  uint8_t handle_short_();
  uint8_t handle_long_();
  uint8_t decode_application_(uint8_t *app, size_t len);
  bool parse_records_(const uint8_t *p, size_t len, Reading &out);
  uint8_t fail_(Error e);
  uint8_t restart_from_(size_t skip);

  Aes128 aes_;
  bool has_key_ = false;
  uint8_t address_ = 0xF0;
  uint16_t required_ = 0;
  uint32_t interbyte_timeout_ms_ = 500;

  uint8_t buf_[kMaxFrame];
  size_t len_ = 0;
  size_t expected_ = 0;
  uint32_t last_byte_ms_ = 0;

  size_t plain_off_ = 0;  // plaintext = buf_ + plain_off_ (decrypted in place)
  size_t plain_len_ = 0;

  Reading reading_;
  bool have_reading_ = false;
  uint32_t last_reading_ms_ = 0;
  uint32_t now_ms_ = 0;
  bool have_access_ = false;
  uint8_t last_access_ = 0;

  Error last_error_ = Error::kNone;
  Stats stats_;
};

}  // namespace amis
