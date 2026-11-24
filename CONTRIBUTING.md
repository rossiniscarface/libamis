# Contributing

Bug reports, captures from real meters and pull requests are welcome.

## Rules for code

- **No hardware access** in the library: bytes come in via `push()`, the library says what to do (send `E5`,
  change the baud rate). Time is passed in as `now_ms`.
- **No heap, no exceptions, no RTTI, no Arduino dependency.** C++17. Must build with `-Wall -Wextra -Werror`
  on the ESP8266 Arduino core (xtensa-gcc 10.3) and on ESP32 (Arduino and ESP-IDF).
- **Clean room:** do not copy code from other AMIS projects (several are GPL or have no license). Protocol facts
  (byte sequences, tables) are fine; please cite the source.
- As M-Bus slave the reading head may **only send `E5`** (Netz OÖ specification section 2.3.10). The IEC 62056-21
  readout is not covered by that specification; it stays a separate, optional class with a note.
- Code comments and documentation are in English.

## Before a pull request

```bash
make -C test            # host tests, must report 0 failed
make -C test sanitize   # ASan/UBSan
pio run -d ci           # microcontroller builds (needs PlatformIO)
```

Add a test to `test/test_main.cpp` for every behaviour change. If you change `test/gen_vectors.py`, regenerate
`test/vectors.h` (`python3 test/gen_vectors.py`, needs the `cryptography` package).

CI runs for every pull request and for release tags.

## Meter captures

Raw captures from a real meter help most. Please **never post your AES key**; a capture of the encrypted frames
alone does not reveal your readings.
