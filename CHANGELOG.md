# Changelog

## Unreleased

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
