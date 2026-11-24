# Changelog

## Unreleased

- `IecReader`: `kRawLine` and `raw_line()` give every data line exactly as received (also the final `!`),
  e.g. to store the billing data set as a file.

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
