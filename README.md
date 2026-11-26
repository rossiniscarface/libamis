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
- Repeated frames (lost acknowledgement: same access number and FCB) are detected and not reported twice
- With a key: checks against forged and replayed frames (header and layout, meter time must move forward)

No heap, no exceptions, no RTTI, no Arduino dependency, C++17. Small RAM footprint: on the ESP8266 tables and
texts stay in flash, and `-DLIBAMIS_MAX_FRAME=128` shrinks the frame buffer to what the AMIS needs; `-DLIBAMIS_NO_STATS` leaves out the
counters of `stats()` (44 bytes per `Reader`). Builds for **ESP8266**, **ESP32** (ESP32, S3, C3,
C6, …), other microcontrollers and the PC.

**Status:** version 0.4.0. Tested on the PC (including the example telegram from the Netz OÖ specification) and
built for ESP8266, ESP32, ESP32-S3, ESP32-C3 and ESP32-C6. On an ESP8266 it runs inside a
[fork of the ESP8266 firmware](https://github.com/rossiniscarface/amis_smartmeter_reader/tree/libamis), checked
against a simulated meter and **tested at a real TD-3511**: M-Bus values, serial number and the billing data set. Protocol details: [docs/protocol.md](docs/protocol.md), changes: [CHANGELOG.md](CHANGELOG.md).

## Usage

```cpp
#include <amis.h>

amis::Reader reader;
reader.set_key_hex("00112233445566778899AABBCCDDEEFF");   // your own key from the Netz OÖ portal
reader.set_required_fields(amis::Reading::kAll);          // optional: only complete frames count

// for every byte from the reading head:
uint8_t st = reader.push(byte, millis());
if (st & amis::Reader::kSendAck) uart_write(amis::Reader::kAck);   // send 0xE5 immediately
if (st & amis::Reader::kReading) {
  const amis::Reading &r = reader.reading();
  if (r.has(amis::Reading::kEnergyImport)) publish(r.energy_import_wh);   // 1.8.0
  // r.power_import_w (1.7.0), r.power_balance_w(), r.time, ... (a missing field stays 0: check has())
}
if (st & amis::Reader::kError) log(amis::error_str(reader.last_error()));
```

Feed the bytes **one at a time**. That way exactly one acknowledgement goes out per frame, even if two frames are
waiting in the receive buffer. But the `E5` must reach the meter within about 85 ms after the end of its frame:
an acknowledgement for a frame that waited longer comes too late (the meter ignores it or counts it for a later
frame). So the `E5` has to come from the device at the reading head, not from a decoder behind a TCP/WLAN bridge,
and the main loop must not block (modem sleep, blocking network calls).

**Protection with a key:** OMS security mode 5 encrypts, but has no MAC, so libamis adds these checks when a key
is set (all on by default):

- An unencrypted frame (security mode 0) is rejected with `Error::kNotEncrypted`; the AMIS always encrypts.
- Only the decrypted bytes are parsed; anything appended after them is ignored.
- `set_header_check()`: the header (it forms the IV) must have the values Netz OÖ fixes for every meter
  (identification 0, manufacturer 0x4C2D, version 0x01, medium 0x0E), and the 80 decrypted bytes must have the
  record layout of the AMIS (Netz OÖ table 2: the same record headers at the same places, in every AES block).
  Otherwise `Error::kHeader`. Turn it off for other meters.
- `set_replay_check()`: the meter time (UTC) must be newer than that of the last accepted reading, else
  `Error::kReplay` (the frame is still acknowledged). If Netz OÖ sets the meter clock back, readings stop until
  the meter time passes the last accepted one; `reset_replay_check()`, `set_key()` or a restart clear it.

Someone at the optical port without the key can then only block the meter, not feed in other values (`online()`
goes false). Without a key none of this applies.

**UART errors:** M-Bus relies on the even parity of every character together with the frame checksum; the 8-bit
sum alone misses two bit errors that cancel out, and such a frame would be acknowledged. So read the UART in
blocks, check its error flag **before** a block goes to `push()`, and on a parity, framing or overrun error drop
the block and call `reader.push_error(millis())` instead (no `E5` for that frame, counted in
`stats().line_errors`). ESP-IDF: the events `UART_PARITY_ERR`, `UART_FRAME_ERR` and `UART_FIFO_OVF`;
Arduino-ESP32: `Serial.onReceiveError()`. **Not** on the ESP8266 Arduino core: there `Serial.hasRxError()` is
also set by the RX timeout interrupt (`UITO` in `cores/esp8266/uart.cpp`), i.e. after nearly every received
block, so it cannot tell a parity error from normal data (tested at a real meter: it discarded valid data). Only
`Serial.hasOverrun()` is usable there. The examples show the pattern for all three platforms.

**Threads:** `Reader` and `IecReader` are not thread-safe. Call all their functions from one task; a UART error
callback in another task (Arduino-ESP32 `onReceiveError()`) should only set a flag.

| Field in `Reading` | OBIS | Unit |
|---|---|---|
| `energy_import_wh` / `energy_export_wh` | 1.8.0 / 2.8.0 | Wh |
| `reactive_energy_import_varh` / `reactive_energy_export_varh` | 3.8.1 / 4.8.1 | varh |
| `power_import_w` / `power_export_w` | 1.7.0 / 2.7.0 | W |
| `reactive_power_import_var` / `reactive_power_export_var` | 3.7.0 / 4.7.0 | var |
| `inkasso` | 1.128.0 | Wh (signed, prepayment register) |
| `time` | meter clock (local time with DST bit), `valid`, `to_unix_utc()`, `raw` (the 6 bytes as sent) | |
| `present` | which fields were in the frame (`kTime` also for a time the meter flags invalid: check `time.valid`) | |
| `header_id`, `manufacturer`, `version`, `medium` | M-Bus header: address of the **reading head**, the same for every AMIS (Netz OÖ 2.4.2.6) | |

The meter number is **not** in the M-Bus frame (`header_id` is 0). It only comes from the IEC identification or
register 0.0.0 of the billing data set (see `IecReader` below).

More: `online(now_ms)`, `stats()` (frames, acknowledgements, errors), `last_plaintext()` (for debugging).

## Optional: serial number and billing data set (IEC 62056-21)

> **Note:** The Netz OÖ specification only describes the M-Bus protocol and limits the answers of the M-Bus
> slave to `E5` (section 2.3.10). It does not cover an IEC 62056-21 readout; Netz OÖ uses the optical port as its
> service interface as well (section 4). Use at your own responsibility. During a readout and up to about 1 min
> afterwards the meter sends no M-Bus data. Nothing is sent until `begin()` is called; with `-DLIBAMIS_NO_IEC`
> this part is not compiled.

`amis::IecReader` only reads in mode C and never programs anything. Two variants:

- `Mode::kIdentOnly`: send `/?!`, receive the identification (manufacturer “SAT” and the identification, per
  the manual the part number; at a TD-351x the same number as the serial number 0.0.0), then break. Takes about 10–15 s at a real meter (it only answers after it has left the M-Bus session).
- `Mode::kReadout`: complete billing data set (among others 0.0.0 serial number, tariff registers 1.8.1–1.8.6,
  1.6.0/2.6.0 quarter-hour maximum demand current and previous periods, voltages/currents 32.7/31.7 …,
  historic values). By default everything runs at 300 baud and takes about 7 min. The meter only switches
  to a faster rate if the acknowledgement repeats the code from its identification (TD-351x: “6” = 19200),
  so `set_max_baud_char('6')` is the only faster option.

The request is sent up to 4 times, 5 s apart: the meter only wakes up its IEC mode with the first received
character, and a real TD-3511 answers only after it has given up the M-Bus session (about 10 s). It refuses the
first acknowledgement with `ER0007` and sends the data after the repeated one, without STX; libamis handles both
(`set_ack_tries()`, `set_ack_retry_ms()`). An error telegram that remains (`ERxxxx`, with or without brackets)
ends the readout with `Error::kMeterError` and `meter_error()`. `set_total_timeout_ms()` (default 120 s) limits
the time until the data starts; while data arrives, the readout only ends if no character comes for 2 s. A data
line longer than 95 characters is cut off (`raw_line_truncated()`, `truncated_lines()`), the readout goes on.
Lines are provisional until `kDone`: with STX the block check character comes at the end (`kFailed` with
`Error::kBcc`: discard the lines), and the TD-351x sends the data without STX, so there is no check at all then;
`verified()` tells whether the block was checked. `serial_from_data()` tells whether `serial_number()` came from
0.0.0 or falls back to the identification. `abort()` stops at once without sending anything: switch back to
9600 8E1 and call `reader.reset()` yourself.
If a cumulation (end of a billing period) falls into the readout, the historic value numbers of the tariff
registers can be inconsistent (manual 5.2.12.3): read again in that case. Details: [docs/protocol.md](docs/protocol.md).

Handle the actions of **every** `push()` and `poll()` call right away: the next byte overwrites `line()` and
`raw_line()`, so do not collect the actions of several bytes and read the line afterwards.

```cpp
amis::IecReader iec;
handle(iec.begin(amis::IecReader::Mode::kIdentOnly, millis()));   // kSetSerial: 300 Bd 7E1

// in the loop: handle(iec.push(byte, millis())) for every received byte, then handle(iec.poll(millis()))
void handle(uint8_t a) {
  if (a & amis::IecReader::kSend)      { size_t n; const uint8_t *d = iec.tx(&n); uart_write(d, n); uart_flush(); }
  if (a & amis::IecReader::kSetSerial) uart_config(iec.baud(), 7E1);   // switch only AFTER sending
  if (a & amis::IecReader::kLine)      keep(iec.line());               // obis / value / unit / extra (provisional)
  if (a & amis::IecReader::kRawLine)   keep_raw(iec.raw_line());       // line as received (also "!")
  if (a & (amis::IecReader::kDone | amis::IecReader::kFailed)) {
    uart_config(9600, 8E1); reader.reset();                            // back to M-Bus
    // kDone: use the kept lines; iec.serial_number(), iec.identification(), iec.verified(); kFailed: iec.error()
  }
}
```

During the readout the meter sends no M-Bus data. Afterwards it can take up to about 1 min until values arrive
again (search mode).

## Integration

- **PlatformIO:** `lib_deps = https://github.com/rossiniscarface/libamis.git#v0.4.0` (see `library.json`).
  Set `LIBAMIS_MAX_FRAME` / `LIBAMIS_NO_STATS` in `build_flags`, never with `#define` in a source file: a
  mismatch fails to link.
- **Arduino IDE:** copy the folder to `libraries/`, `#include <amis.h>`; examples in
  [`examples/esp8266_arduino`](examples/esp8266_arduino) and [`examples/esp32_arduino`](examples/esp32_arduino)
- **ESP-IDF** (5.x): put the folder under `components/`, named **`libamis`** (ESP-IDF names a component after
  its folder, so rename a `libamis-main` or `libamis-0.4.0` download); the `CMakeLists.txt` detects ESP-IDF.
  Example in [`examples/esp_idf`](examples/esp_idf), which uses the repository root as component directory.
- **ESP8266 on UART0:** the boot ROM banner and crash dumps also go out on GPIO1 (TX). The meter ignores them,
  but to keep them away from the head use `Serial.swap()` (head on GPIO15/GPIO13), see the ESP8266 example.
- **ESPHome:** via an external component that uses this library (planned)
- **PC:** `cmake -B build && cmake --build build && ctest --test-dir build`

## Tests

```bash
make -C test            # host tests
make -C test sanitize   # with address and UB sanitizer
make -C test fuzz       # libFuzzer with address and UB sanitizer (clang), FUZZ_TIME=60 s per target
pio run -d ci           # build for ESP8266, ESP32, ESP32-S3, ESP32-C3 (Arduino), ESP32 and ESP32-C6 (ESP-IDF)
```

Covered: **the example telegram from the Netz OÖ specification** (key, frame and all expected values), AES with
the NIST test vectors (FIPS-197, SP 800-38A), date/time with values from real frames, and complete frames that
`test/gen_vectors.py` encrypts **independently of the library** with Python `cryptography`. Also search
request, noise bytes, repetition, two frames back to back, wrong or missing key, broken checksum and gaps in the
middle of a frame. The IEC part is tested against a simulated meter (echo, parity bit, reply delay, baud rate
selection, BCC, break, timeout). CI runs the tests with GCC and Clang and builds for all targets above.

Both parsers take untrusted input (anything that reaches the optical port), so they never write outside their
fixed buffers. `test/fuzz/` has libFuzzer targets for `Reader` (with both frame buffer sizes) and `IecReader`;
before 0.3.1 they ran 5.7 million inputs under ASan and UBSan without a finding. CI runs them for 30 s per target,
with UBSan findings fatal, and checks the AES tables with `tools/gen_sbox.py`.

## Important per Netz OÖ

- As M-Bus slave the reading head may **only answer with `E5`** (section 2.3.10). Other information or commands
  from the M-Bus slave to the meter are not allowed and would be discarded.
- The search request must be acknowledged within **0.5 s**, a data frame within the M-Bus reply window of about
  **85 ms** (330 bit times + 50 ms): call `push()` soon after the bytes arrive and send `E5` right away. After **10 missing acknowledgements** the meter falls
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
