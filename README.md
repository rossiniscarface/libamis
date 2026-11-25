# libamis

[![test](https://github.com/rossiniscarface/libamis/actions/workflows/test.yml/badge.svg?event=push)](https://github.com/rossiniscarface/libamis/actions/workflows/test.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Reader core for the **Siemens AMIS smart meter** (TD-3511 / TD-3512) used by **Netz Oberösterreich** (Upper
Austria), as a small, portable C++17 library under the **MIT license**.

The library handles everything that makes the AMIS special, but it does **not touch any hardware** itself:

- M-Bus over the optical customer interface (9600 baud, 8E1), frame detection with resynchronisation
- **`E5` acknowledgement** of the search request (SND_NKE to address 240) and of every valid data frame
  (without it the meter stops sending)
- **AES-128-CBC decryption** (OMS security mode 5, IV from meter address and access number) with the key from
  the Netz OÖ customer portal, detection of a wrong key
- Decoding of the data records: meter clock, 1.8.0, 2.8.0, 3.8.1, 4.8.1, 1.7.0, 2.7.0, 3.7.0, 4.7.0, 1.128.0
- Repeated frames (lost acknowledgement) are detected and not reported twice

No heap, no exceptions, no RTTI, no Arduino dependency, C++17. Small RAM footprint: on the ESP8266 tables and
texts stay in flash, and `-DLIBAMIS_MAX_FRAME=128` shrinks the frame buffer to what the AMIS needs. Builds for **ESP8266**, **ESP32** (ESP32, S3, C3,
C6, …), other microcontrollers and the PC.

**Status:** version 0.2.3. Tested on the PC (including the example telegram from the Netz OÖ specification) and
built for ESP8266, ESP32, ESP32-S3, ESP32-C3 and ESP32-C6. On an ESP8266 it runs inside a
[fork of the ESP8266 firmware](https://github.com/rossiniscarface/amis_smartmeter_reader/tree/libamis), checked
against a simulated meter and **tested at a real TD-3511**: M-Bus values, serial number and the billing data set. Protocol details: [docs/protocol.md](docs/protocol.md), changes: [CHANGELOG.md](CHANGELOG.md).

## Usage

```cpp
#include <amis.h>

amis::Reader reader;
reader.set_key_hex("00112233445566778899AABBCCDDEEFF");   // your own key from the Netz OÖ portal

// for every byte from the reading head:
uint8_t st = reader.push(byte, millis());
if (st & amis::Reader::kSendAck) uart_write(amis::Reader::kAck);   // send 0xE5 immediately
if (st & amis::Reader::kReading) {
  const amis::Reading &r = reader.reading();
  // r.energy_import_wh (1.8.0), r.power_import_w (1.7.0), r.power_balance_w(), r.time, ...
}
if (st & amis::Reader::kError) log(amis::error_str(reader.last_error()));
```

Feed the bytes **one at a time**. That way exactly one acknowledgement goes out per frame, even if two frames are
waiting in the receive buffer.

| Field in `Reading` | OBIS | Unit |
|---|---|---|
| `energy_import_wh` / `energy_export_wh` | 1.8.0 / 2.8.0 | Wh |
| `reactive_energy_import_varh` / `reactive_energy_export_varh` | 3.8.1 / 4.8.1 | varh |
| `power_import_w` / `power_export_w` | 1.7.0 / 2.7.0 | W |
| `reactive_power_import_var` / `reactive_power_export_var` | 3.7.0 / 4.7.0 | var |
| `inkasso` | 1.128.0 | Wh (signed, prepayment register) |
| `time` | meter clock (local time with DST bit), `to_unix_utc()` | |
| `present` | which fields were in the frame | |

More: `online(now_ms)`, `stats()` (frames, acknowledgements, errors), `last_plaintext()` (for debugging).

## Optional: serial number and billing data set (IEC 62056-21)

> **Note:** The Netz OÖ specification only describes the M-Bus protocol and limits the answers of the M-Bus
> slave to `E5` (section 2.3.10). It does not cover an IEC 62056-21 readout; Netz OÖ uses the optical port as its
> service interface as well (section 4). Use at your own responsibility. During a readout and up to about 1 min
> afterwards the meter sends no M-Bus data. Nothing is sent until `begin()` is called; with `-DLIBAMIS_NO_IEC`
> this part is not compiled.

`amis::IecReader` only reads in mode C and never programs anything. Two variants:

- `Mode::kIdentOnly`: send `/?!`, receive the identification (manufacturer “SAT”, part or serial number), then
  break. Takes about 1 s.
- `Mode::kReadout`: complete billing data set (among others 0.0.0 serial number, tariff registers 1.8.1–1.8.6,
  1.6.0/2.6.0 quarter-hour maximum demand current and previous periods, voltages/currents 32.7/31.7 …,
  historic values). By default everything runs at 300 baud and takes about 5–6 min. The meter only switches
  to a faster rate if the acknowledgement repeats the code from its identification (TD-351x: “6” = 19200),
  so `set_max_baud_char('6')` is the only faster option.

The request is repeated up to 3 times, 5 s apart: the meter only wakes up its IEC mode with the first received
character, and a real TD-3511 answers only after it has given up the M-Bus session (about 10 s). It refuses the
first acknowledgement with `ER0007` and sends the data after the repeated one, without STX; libamis handles both
(`set_ack_tries()`, `set_ack_retry_ms()`). An error telegram that remains (`ERxxxx`, with or without brackets)
ends the readout with `Error::kMeterError` and `meter_error()`. Details: [docs/protocol.md](docs/protocol.md).

```cpp
amis::IecReader iec;
uint8_t a = iec.begin(amis::IecReader::Mode::kIdentOnly, millis());   // only kSetSerial: 300 Bd 7E1
// loop: a |= iec.push(byte, millis()) for every byte, a |= iec.poll(millis()); always send first, then switch
if (a & amis::IecReader::kSend)      { size_t n; const uint8_t *d = iec.tx(&n); uart_write(d, n); uart_flush(); }
if (a & amis::IecReader::kSetSerial) uart_config(iec.baud(), 7E1);   // switch only AFTER sending
if (a & amis::IecReader::kLine)      use(iec.line());                // obis / value / unit / extra
if (a & amis::IecReader::kRawLine)   save(iec.raw_line());           // line as received (also "!")
if (a & (amis::IecReader::kDone | amis::IecReader::kFailed)) {
  uart_config(9600, 8E1); reader.reset();                            // back to M-Bus
  // iec.serial_number(), iec.identification(), iec.error()
}
```

During the readout the meter sends no M-Bus data. Afterwards it can take up to about 1 min until values arrive
again (search mode).

## Integration

- **PlatformIO:** `lib_deps = https://github.com/rossiniscarface/libamis.git#v0.2.3` (see `library.json`)
- **Arduino IDE:** copy the folder to `libraries/`, `#include <amis.h>`; examples in
  [`examples/esp8266_arduino`](examples/esp8266_arduino) and [`examples/esp32_arduino`](examples/esp32_arduino)
- **ESP-IDF:** put the folder under `components/` (the `CMakeLists.txt` detects ESP-IDF); example in
  [`examples/esp_idf`](examples/esp_idf)
- **ESPHome:** via an external component that uses this library (planned)
- **PC:** `cmake -B build && cmake --build build && ctest --test-dir build`

## Tests

```bash
make -C test            # host tests
make -C test sanitize   # with address and UB sanitizer
pio run -d ci           # build for ESP8266, ESP32, ESP32-S3, ESP32-C3 (Arduino), ESP32 and ESP32-C6 (ESP-IDF)
```

Covered: **the example telegram from the Netz OÖ specification** (key, frame and all expected values), AES with
the NIST test vectors (FIPS-197, SP 800-38A), date/time with values from real frames, and complete frames that
`test/gen_vectors.py` encrypts **independently of the library** with Python `cryptography`. Also search
request, noise bytes, repetition, two frames back to back, wrong or missing key, broken checksum and gaps in the
middle of a frame. The IEC part is tested against a simulated meter (echo, parity bit, reply delay, baud rate
selection, BCC, break, timeout). CI runs the tests with GCC and Clang and builds for all targets above.

## Important per Netz OÖ

- As M-Bus slave the reading head may **only answer with `E5`** (section 2.3.10). Other information or commands
  from the M-Bus slave to the meter are not allowed and would be discarded.
- The search request must be acknowledged within **0.5 s**. After **10 missing acknowledgements** the meter falls
  back to search mode (then about 1 min until the next search request).
- The interface only works with the **terminal cover closed** (symbol “S1” on the display off).
- Netz OÖ also uses the interface for service; do not glue the reading head to the meter.

## Background and sources

Written from scratch after EN 13757-2/-3 (M-Bus), the OMS specification (security mode 5, IV layout) and the
public [Netz OÖ specification of the AMIS customer interface](https://www.netzooe.at/2021-11-05-spezifikation-ks-amis.pdf)
(2021-11-05, German: address 240, CI 5B, table of DIF/VIF codes, example telegram). No code was taken from other
AMIS projects; the AES implementation is independent, and `tools/gen_sbox.py` derives the S-boxes from their
mathematical definition.

Related projects: [mgerhard74/amis_smartmeter_reader](https://github.com/mgerhard74/amis_smartmeter_reader)
(ESP8266 firmware, GPL-2.0), [andyboeh/esphome-amis](https://github.com/andyboeh/esphome-amis) (ESPHome),
[vzlogger](https://github.com/volkszaehler/vzlogger) (Linux, GPL-3.0).

## License

MIT, see [LICENSE](LICENSE). Not affiliated with Netz Oberösterreich or Siemens.
