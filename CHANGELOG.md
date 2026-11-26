# Changelog

## 0.4.0 – 2026-11-26

Fixes from an external review of 0.3.1 (findings R1–R33).

### Security (with a key set)

OMS security mode 5 encrypts but has no MAC, and 0.3.1 trusted too much of the frame:

- Bytes after the encrypted part were parsed as plain text, so a 1.8.0 record could simply be appended (R1).
  Only the decrypted bytes are parsed now.
- The CBC IV comes from the cleartext header, and a changed ciphertext block flips bits in the next plaintext
  block, so the decrypted values could be changed without the key (R2). New `set_header_check()` (on by
  default): the header must have the values Netz OÖ fixes (identification 0, 0x4C2D, 0x01, 0x0E) and the 80
  decrypted bytes the record layout of the AMIS (Netz OÖ table 2), else the new `Error::kHeader`.
- Older frames were accepted again (R3). New `set_replay_check()` (on by default): the meter time (UTC) must be
  newer than that of the last accepted reading, else the new `Error::kReplay`; `reset_replay_check()`.

### Breaking changes

- With a key set, frames that do not look like an AMIS are rejected (see above); turn the checks off for other
  meters. The test vectors now use the header of a real AMIS (identification 0).
- `Reading::kTime` is set for every well-formed time record, also one the meter flags invalid; check
  `time.valid`. Before, such a frame failed with `kIncomplete` under `set_required_fields(kAll)` (R15).
- `IecReader`: the request is sent 4 times by default instead of 3, the last one 15 s after the first, so it
  reliably reaches the meter after it has left the M-Bus session (R3-5).
- `LIBAMIS_MAX_FRAME` and `LIBAMIS_NO_STATS` are part of the symbol names of `Reader` (inline namespace): a
  translation unit built with other values fails to link instead of corrupting memory (R4). Set them as
  build flags; `LIBAMIS_MAX_FRAME` must be a plain number.

### Fixes

- `Reader`:
  - Guard against writing past the frame buffer if `reset()` runs concurrently; `Reader` and `IecReader` are
    documented as not thread-safe (R5).
  - `power_balance_w()` computes in 64 bits and saturates (no signed overflow, R13).
  - `DateTime::to_unix_utc()` no longer overflows on targets with 16-bit `int` (R14).
  - `DateTime::decode_type_i()` rejects days that do not exist (Feb 30, Apr 31, Feb 29 outside leap years, R16).
  - A second time record no longer leaves a half-written time behind (R26).
  - `online()` no longer reports a reading as fresh again after 49.7 days without readings (R12).
  - `last_plaintext()` is empty after errors before or during decryption (R27).
- `IecReader`:
  - An STX after a byte that looked like the echo of our acknowledgement is no longer swallowed (R20).
  - Without acknowledgement (`set_send_ack(false)`) an error telegram ends the readout with `kMeterError`
    instead of sending the request again (R19).
  - The identification needs a valid baud rate character, and a pause of more than 1.5 s inside it starts over:
    M-Bus noise can no longer pass as an identification (R3-4).
  - A refused acknowledgement whose error telegram lost its first character (block check fails, no data lines)
    repeats the acknowledgement instead of failing with `kBcc` (R3-1).
  - An error telegram after data lines ends the readout (`kMeterError`) instead of silently reading everything
    again (R3-2).
  - `begin()` clears `raw_line_truncated()` (R28).

### New

- `IecReader::verified()`: the block came with STX and its block check character matched (R7, R18).
- `IecReader::serial_from_data()`: whether `serial_number()` is 0.0.0 or the identification (R29).

### Examples, docs, CI

- The examples follow the UART error rule (ESP-IDF event queue, Arduino-ESP32 `onReceiveError()` with a flag,
  ESP8266 `hasOverrun()`), set `kAll` and use pins that do not collide with flash, PSRAM or console pins on C3,
  C6 and S3 (R8, R22). The ESP8266 example explains `Serial.swap()` against boot and crash output on UART0 (R2-1).
- README: IEC pattern fixed (handle the actions of every `push()` call, lines are provisional until `kDone`,
  R6, R18), the E5 timing for backlogs and network bridges (R2-2, R2-4), `abort()` (R21), the request count
  (R31), the ESP-IDF component folder name (R25). Header: inter-byte timeout semantics (R2-3, R11), the limits of
  `line()` (R3-3), unsupported record types (R17).
- CI: sanitizer findings are fatal (R10), the fuzzers run for 30 s per target (R23, now also with header and
  replay checks off, primary address and inter-byte timeout), `tools/gen_sbox.py` checks all 256 entries of
  both AES tables (R30), the PlatformIO platforms are pinned (R32).
- Tests for all error codes, addresses 0xFE and foreign addresses, the setters and the new checks (R24).

## 0.3.1 – 2026-11-25

### Security

- `Reader`: with a key set, an unencrypted frame (security mode 0) is rejected with the new
  `Error::kNotEncrypted`. Before, anyone with access to the optical port could feed in made-up values without
  the key. Without a key, unencrypted frames are still read.

### RAM

- On the ESP8266 two small tables no longer live in RAM (50 bytes): the AES round constants are computed, and
  `IecReader::baud_for_char()` no longer compiles to a lookup table.
- `-DLIBAMIS_NO_STATS` leaves out the counters (`Stats`, 44 bytes per `Reader`); `stats()` is not available then.

### Repository

- `SECURITY.md` (private vulnerability reporting), Dependabot for the GitHub Actions, actions pinned to commit
  SHAs, PlatformIO version pinned in CI, workflow token read-only.

### Tests and docs

- libFuzzer targets for `Reader` and `IecReader` in `test/fuzz/` (`make -C test fuzz`, needs clang); 5.7 million
  inputs under ASan and UBSan without a finding.
- Header: the key stays in RAM as AES round keys; the shared `error_str()` buffer on the ESP8266 is not
  thread-safe.

## 0.3.0 – 2026-11-25

### Breaking changes

- `IecReader::begin()` during a running readout no longer aborts it: it changes nothing and returns `kNone`
  (check `active()` first). `IecReader::Error::kBusy` and its text are gone.
- `IecReader::set_address()` returns `bool`: only digits, letters and spaces, at most 32 characters (manual
  5.2.8.1); an invalid address is rejected and the previous one kept (before, it was cut to 32 characters).
- `Reading::meter_id` is now `Reading::header_id`: the M-Bus header carries the secondary address of the reading
  head, fixed by Netz OÖ 2.4.2.6 (0 on every AMIS), not the meter number. Comments and docs corrected.

### New

- `Reader::push_error()` and `Stats::line_errors`: report a UART parity, framing or overrun error, the frame in
  progress is dropped (no `E5`). README and header describe the integration rule (check the UART error flag
  before a block goes to `push()`). Not usable with `Serial.hasRxError()` on the ESP8266 Arduino core, which
  also reports the RX timeout.
- `Reader::set_required_fields()` and `Error::kIncomplete`: a frame without a required value is reported as an
  error instead of a reading with zeros (default: no requirement). `Reading::kAll` names all 10 fields.
- `IecReader::raw_line_truncated()` and `truncated_lines()`: a data line longer than 95 characters no longer ends
  the readout with `Error::kFormat`; it is cut off, reported as raw line only, counted, and the readout goes on.
- `DateTime::raw`: the 6 bytes of the timestamp as sent (M-Bus type I / CP48).

### Changed

- `Reader`: a frame counts as repetition only with the same access number **and** an unchanged FCB (when FCV is
  set), per EN 13757-2. A SND_NKE to 0xFF also resets the repetition state, without `E5`.
- `Reader`: default inter-byte timeout 500 ms instead of 200 ms (`now_ms` is the processing time, a stalled main
  loop dropped frames). Docs: the `E5` to a data frame belongs into the M-Bus reply window of about 85 ms.

### Docs and build

- README, header and protocol.md: identification takes about 10–15 s at a real meter, the billing data set about
  7 min; note on inconsistent historic values if a cumulation falls into the readout (manual 5.2.12.3);
  `DateTime::to_unix_utc()` only applies if `valid`.
- CMake: `target_compile_features(amis PUBLIC cxx_std_17)` instead of a global standard; the test target is only
  built when libamis is the top-level project.
- CI also runs on pushes to `main`.

### Fixes

- `IecReader`: the total timeout (`set_total_timeout_ms()`, default 120 s) ended a readout with the default
  settings in the middle of the billing data set (about 7 min at 300 Bd). It now only applies until the data
  starts; while data arrives, the 2 s character timeout guards the readout.
- `IecReader`: a non-printable character while waiting for the identification drops what was received so far;
  noise after a random `/` no longer keeps the request from being repeated.

## 0.2.3 – 2026-11-25

Fixes in `IecReader` from a code review:

- A repeated acknowledgement after a switch to a faster baud rate (`set_max_baud_char('6')`) was sent at the new
  baud rate, which the meter does not understand. Now the UART is switched back to 300 Bd first (`kSetSerial`
  alone), and the acknowledgement is repeated for 300 Bd (`ACK 0 0 0`).
- Data without STX is only taken after a repeated acknowledgement (as the real TD-3511 sends it) or without
  acknowledgement; an optical echo of our own acknowledgement is ignored.
- An error telegram ended by CR LF no longer hides the data lines after it; if data follows, the error is dropped.
- The request is not repeated (and the reader does not give up) while an identification is arriving.
- After the last acknowledgement the reader waits at least 2.5 s for data, even with a short `set_ack_retry_ms()`.

## 0.2.2 – 2026-11-24

`IecReader` adapted to a real TD-3511 (behaviour captured with a RAM trace, see docs/protocol.md):

- The request is repeated every 5 s (`set_request_interval_ms()`, before 2 s): the meter only answers after it has
  left the M-Bus session, about 10 s after the last `E5`. After a broken answer the request is sent again.
- The acknowledgement follows the identification after 700 ms (`set_reply_delay_ms()`).
- If the meter answers the acknowledgement with an error telegram or sends no data, the acknowledgement is sent
  again 7 s later, up to 3 times (`set_ack_tries()`, `set_ack_retry_ms()`), like the original firmware. A real
  TD-3511 always refuses the first one with `ER0007`.
- Data without STX is accepted (the meter sends it that way after the repeated acknowledgement); the block check
  character is not checked then.
- Error telegrams are recognised with and without brackets (`ER0007`), and they are no longer reported as data
  lines (`kRawLine`).
- Waiting for the data after the acknowledgement: 7 s per acknowledgement; without acknowledgement
  (`set_send_ack(false)`, manual fig. 5-1) 12 s.

## 0.2.1 – 2026-11-24

Fixes in `IecReader` after checking against the Siemens TD-351x manual (D23-510-1.02, chapter 5.2):

- **Baud rate:** the meter only switches if the acknowledgement repeats the code from its identification
  (always “6” on the TD-351x). The old default (`'5'`, 9600) made libamis switch while the meter stayed at
  300 baud, so a readout failed. Now only the meter's own code or `'0'` is used; the default is `'0'` (300 baud).
- The request `/?!` is repeated (default 3 times, `set_request_tries()`), because the meter only wakes up its
  IEC mode with the first received character.
- Error telegrams `STX (ERxxxx) ETX BCC` end the readout with `Error::kMeterError`, code in `meter_error()`.
  A line directly before ETX (without CR LF) is now processed.
- A missing block check character after ETX is accepted (allowed by the manual).
- `begin()` now only returns `kSetSerial`; the request follows as `kSend` from the next `poll()`. Before, both came
  together, and the documented order (send first, then switch) sent the first request at the old baud rate.

## 0.2.0 – 2026-11-24

- `IecReader`: `kRawLine` and `raw_line()` give every data line exactly as received (also the final `!`),
  e.g. to store the billing data set as a file.
- Less RAM, mainly on the ESP8266:
  - the payload is decrypted in place in the frame buffer (no second 261-byte buffer);
    `last_plaintext()` is therefore only valid until the next `push()`
  - `-DLIBAMIS_MAX_FRAME=101..261` sets the frame buffer size (default 261); the AMIS needs 101,
    longer frames are dropped safely
  - error texts and lookup tables are stored in flash on the ESP8266 (`error_str()` copies the text into a
    shared 40-byte buffer there)
  - `amis::Reader` shrinks from 848 to 588 bytes, or to 452 bytes with `LIBAMIS_MAX_FRAME=128` (ESP8266)

## 0.1.0 – 2026-11-24

First release.

- `amis::Reader`: M-Bus slave for the AMIS customer interface. Frame detection with resynchronisation,
  `E5` acknowledgement of SND_NKE and SND_UD, AES-128-CBC (OMS mode 5), wrong-key detection, decoding of all
  10 values from the Netz OÖ specification, detection of repeated frames, statistics.
- `amis::Aes128`: own AES-128 implementation, S-boxes in flash on ESP8266.
- `amis::DateTime`: M-Bus type I with daylight saving time, conversion to UTC (CET/CEST).
- `amis::IecReader` (optional, `-DLIBAMIS_NO_IEC` leaves it out): identification or billing data set via
  IEC 62056-21 mode C, readout only.
- Host tests (79 checks): example from the Netz OÖ specification, NIST vectors, frames encrypted independently
  with Python, simulated meter for the IEC part.
- Main header `amis.h`, alternatively `libamis.h` (Arduino naming convention).
- Packaging for PlatformIO, Arduino and ESP-IDF; examples for ESP8266 (Arduino), ESP32 (Arduino) and ESP-IDF.
- CI: tests with GCC and Clang plus sanitizers, CMake build, builds for ESP8266, ESP32, ESP32-S3, ESP32-C3
  and ESP32-C6.
