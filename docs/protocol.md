# Protocol of the AMIS customer interface

A summary of what libamis implements, with sources. Section numbers refer to the
**Netz OÖ specification “AMIS-Zähler Kundenschnittstelle”, version 2021-11-05**
([PDF, German](https://www.netzooe.at/2021-11-05-spezifikation-ks-amis.pdf)) or to the **Siemens AMIS TD-351x
user manual D23-510-1.02** (service interface).

## 1. Physical layer

- Optical interface per IEC 62056-21 (infrared), magnetic reading head, top left on the meter.
- **9600 baud, 8 data bits, even parity, 1 stop bit (8E1)** (sections 2.3.2–2.3.4).
- Works only with the **terminal cover closed**. With the cover open “S1” is lit on the display and
  communication pauses (section 2.1).

## 2. Roles and sequence (sections 2.1–2.2)

The **meter is the M-Bus master**; the reading head is an M-Bus slave with the fixed primary address **240 (0xF0)**.

```
meter                                    reading head
  |-- SND_NKE  10 40 F0 30 16 ----------->|   about once a minute (search mode)
  |<---------------------------------- E5 |   within 0.5 s (section 2.3.7)
  |-- SND_UD  68 5F 5F 68 53 F0 5B ... -->|   every second, aligned to the full second
  |<---------------------------------- E5 |   acknowledge every frame (M-Bus reply window, about 85 ms)
  |-- SND_UD  68 5F 5F 68 73 F0 5B ... -->|   FCB bit toggles (C = 53 / 73)
  |<---------------------------------- E5 |
  ...  after 10 missing acknowledgements -> back to search mode
```

Timing of the acknowledgement: 0.5 s applies to the search request (section 2.3.7). For SND_UD the M-Bus reply
window applies (EN 13757-2): at the earliest 11 bit times, at the latest 330 bit times + 50 ms after the end of
the frame, about 85 ms at 9600 Bd. A later `E5` counts as missing and the master repeats the frame. libamis
drops a frame if more than 500 ms pass between two of its bytes (`set_interbyte_timeout_ms()`); a frame takes
about 116 ms (11 bits per character), the next one follows about 885 ms later.

**As M-Bus slave the reading head may only send `E5`** (section 2.3.10). According to Netz OÖ other information
or commands from the M-Bus slave are not allowed and are discarded by the M-Bus master. The reading head must also cope with gaps in the one-second rhythm
(section 2.3.8).

## 3. Telegrams

### Short frame (search request)

`10 C A CS 16` with C = 0x40 (SND_NKE), A = 0xF0, CS = (C + A) mod 256.

### Long frame (meter data), 101 bytes

| Offset | Content | Value on the AMIS |
|---|---|---|
| 0–3 | start, L, L, start | `68 5F 5F 68` (L = 95) |
| 4 | C field | `53` / `73` (SND_UD, FCB toggles) |
| 5 | A field | `F0` (240) |
| 6 | CI | `5B` (data to device, 12-byte header) |
| 7–10 | identification number | `00 00 00 00` |
| 11–12 | manufacturer ID | `2D 4C` (“SAM”) |
| 13 | version | `01` |
| 14 | medium | `0E` (bus/system component) |
| 15 | access number | counts up |
| 16 | status | `00` |
| 17–18 | configuration word | `50 05` → 0x0550 |
| 19–98 | encrypted payload | 80 bytes = 5 AES blocks |
| 99 | checksum | sum of bytes 4–98 mod 256 |
| 100 | stop | `16` |

Bytes 7–14 are the secondary address of the **reading head** (terminal device), not of the meter: Netz OÖ
fixes them for every terminal device (section 2.4.2.6). So `Reading::header_id` is always 0 on a real AMIS. The
meter number only comes from the IEC identification (`/SAT6<identification>`, Siemens manual 5.2.7) or from
register 0.0.0 of the billing data set, see `IecReader`.

Configuration word (OMS): bits 8–12 = security mode (**5** = AES-128-CBC), bits 4–7 = number of encrypted
blocks (**5**).

## 4. Decryption (sections 2.4.2.5–2.4.2.7, OMS Vol. 2)

- AES-128-CBC, key from the Netz OÖ customer portal (16 bytes, given as 32 hex digits).
- **IV** = manufacturer ID (2) | identification number (4) | version | medium | 8 × access number.
  Example from the specification: `2D 4C 00 00 00 00 01 0E 0D 0D 0D 0D 0D 0D 0D 0D`.
- The plaintext starts and ends with the filler bytes `2F 2F`. If `2F 2F` is missing at the start, the key is
  wrong.
- With a key set, libamis only accepts security mode 5; an unencrypted frame (mode 0) is reported as
  `Error::kNotEncrypted`. Otherwise anyone with access to the optical port could feed in made-up values without
  knowing the key.

## 5. Data records (section 2.4.2.8, table 2)

| Value | OBIS | DIF(E)/VIF(E) | Unit | Type |
|---|---|---|---|---|
| date + time | 0.9.1 + 0.9.2 | `06 6D` | M-Bus type I (CP48) | 6 bytes |
| energy A+ | 1.8.0 | `04 03` | Wh | uint32 |
| energy A− | 2.8.0 | `04 83 3C` | Wh | uint32 |
| energy R+ | 3.8.1 | `84 10 FB 82 73` | varh | uint32 |
| energy R− | 4.8.1 | `84 10 FB 82 F3 3C` | varh | uint32 |
| active power P+ | 1.7.0 | `04 2B` | W | uint32 |
| active power P− | 2.7.0 | `04 AB 3C` | W | uint32 |
| reactive power Q+ | 3.7.0 | `04 FB 14` | var | uint32 |
| reactive power Q− | 4.7.0 | `04 FB 94 3C` | var | uint32 |
| prepayment register (“Inkasso”) | 1.128.0 | `04 83 FF 04` | Wh | int32 |

All values are little endian. libamis parses the records generically (DIF, DIFE, VIF, VIFE, length from the DIF)
and maps them using this table. Unknown records are skipped.

### Date/time (M-Bus type I, 6 bytes, local time)

| Byte | Bits | Content |
|---|---|---|
| 0 | 0–5 / 6 | second / daylight saving time |
| 1 | 0–5 / 7 | minute / “invalid” |
| 2 | 0–4 / 5–7 | hour / weekday (1 = Monday) |
| 3 | 0–4 / 5–7 | day / year bits 0–2 |
| 4 | 0–3 / 4–7 | month / year bits 3–6 |
| 5 | 0–5 | calendar week |

Year = 2000 + 7-bit value. Example from the specification: `5F 0C 48 C1 17 1B` = Tue 2014-07-01 08:12:31,
daylight saving time, week 27.

## 6. Example from the specification (section 2.4.2.9)

Key `00112233445566778899AABBCCDDEEFF`, telegram:

```
68 5F 5F 68 53 F0 5B 00 00 00 00 2D 4C 01 0E 0D 00 50 05 3F D0 FE B7 26
76 0C C7 AA F0 B5 2B 41 F0 C5 41 BD 63 06 DC D8 B9 1B 3D A2 31 1E F1
3D 25 14 D0 96 00 82 16 1E FE C4 B6 CB 1E 0B 33 28 BE 61 77 DC A5 94
C1 28 00 24 A8 35 F1 D6 55 BA 71 82 B2 56 E9 4B D3 3A C0 A6 B0 8D A4
67 81 EB 4E 91 E0 12 16
```

Result: 2014-07-01 08:12:31, 1.8.0 = 684544 Wh, 2.8.0 = 129412 Wh, 3.8.1 = 357918 varh, 4.8.1 = 81446 varh,
1.7.0 = 0 W, 2.7.0 = 117 W, 3.7.0 = 0 var, 4.7.0 = 0 var, 1.128.0 = 20 Wh. This is a fixed test in
`test/test_main.cpp`.

## 7. Quarter-hour values

The customer interface does not deliver quarter-hour values. They can be computed exactly though: the meter
produces its register values aligned to the full second and sends its clock along. The difference of 1.8.0
between two quarter-hour boundaries (meter clock), times 4, is the mean power of that quarter hour in W. This
matches the value in the meter's load profile. A quarter hour should be discarded if no valid reading arrived
shortly after a boundary. libamis does not implement this yet.

## 8. Optional: service interface per IEC 62056-21 (Siemens manual section 5.2)

**Not covered by the Netz OÖ specification.** It only describes the M-Bus protocol; section 2.3.10 limits the
answers of the M-Bus slave to `E5`, and section 4 says that Netz OÖ also uses the optical port as its service
interface. It neither allows nor forbids an IEC 62056-21 readout by the customer. libamis offers it as a separate
class (`amis::IecReader`), use at your own responsibility. While it runs, the customer interface (M-Bus) pauses.

- Always starts at **300 baud, 7E1**.
- Request `/?` [device address] `!` CR LF. The device address is the serial number; without an address every
  meter answers. The optical port normally runs at the “web” baud rate and only switches to IEC 62056-21 with
  the first received character, so the request has to be **repeated** (manual 5.2.12.2). The manual says to wait
  1500–2200 ms before repeating (5.2.4); libamis waits 5 s and sends it up to 3 times (see below why).
- Identification `/XXXZ<identification>` CR LF: XXX = “SAT”, Z = baud rate code (TD-351x always reports
  “6” = 19200 for compatibility, 5.2.1), identification = part number, e.g. 2351102623000045 for
  G23-511--.02/623-000045 (5.2.7), at most 16 characters (5.2.8.2).
- Reply at the earliest 200 ms, at the latest 1500 ms after a telegram. Less than 1500 ms between two characters.
- Data readout: `ACK 0 Z 0` CR LF. The meter switches to the Z baud rate **only if Z equals the code from its
  identification**; with any other Z (or a broken or missing acknowledgement) it stays at 300 baud (5.2.1).
  So only “0” or the meter's own code make sense; libamis never sends anything in between. The meter sends
  `STX` data lines `!` CR LF `ETX BCC`. BCC = XOR over everything after STX up to and including ETX
  (ISO 1155); the BCC may also be missing (5.2.8).
- Without acknowledgement the meter sends the data at 300 baud after the timeout. Abort with break
  `SOH B 0 ETX BCC` (5.2.8.12); a few data bytes may still follow a break (5.2.12.4).
- Errors come as `STX (ERxxxx) ETX BCC`, e.g. ER0001 unknown command, ER0002 error in data field, ER0005 cannot
  be executed now, ER0007 security level too low, ER0009 communication locked (5.2.11).
- After a readout the port stays at 300 baud for 15 s before it returns to the web baud rate (5.2.12.5); the
  inactivity timeout is 10 s instead of the 60–120 s of the standard (5.2.13.1).
- Data lines: `OBIS(value*unit)` and optionally `(timestamp)`. The billing data set contains among others
  F.F, 0.0.0 (serial number), 1.128.0, 1.8.0–1.8.6 and 2.8.0–2.8.6 with previous periods (`.Vw`), 1.6.0/2.6.0
  (maximum demand), 3.8.1/4.8.1, 1.7.0–4.7.0, voltages/currents/angles (32.7, 52.7, 72.7, 31.7, 51.7, 71.7,
  81.7.x), frequency 14.7 and C.x status registers.
### Behaviour of a real TD-3511 (Netz OÖ, captured 2026-11-24)

Measured with a RAM trace on a reader at a real meter; it differs from the manual in several points:

| Step | Reader → | ← Meter |
|---|---|---|
| 0 s | `/?!` | nothing useful: the meter keeps sending its M-Bus frames for about 10 s (no more `E5`) |
| 5 s | `/?!` | (M-Bus frames) |
| 10 s | `/?!` | `/SAT6TEST000000000001` CR LF, about 0.4 s later |
| +0.7 s | `ACK 0 0 0` CR LF | `STX ER0007 ETX BCC` — **without brackets** |
| +7 s | `ACK 0 0 0` CR LF (again) | the billing data set, **without STX**, starting with `F.F(`, **without the final `!` line**, ending with `ETX` and one more byte |

- The meter only answers the request after it has given up the M-Bus session (10 missing `E5`, then search
  mode), i.e. about 10 s after the reader stopped acknowledging. Requests must therefore be spread over more than
  10 s.
- The first acknowledgement is always refused with `ER0007`, the repeated one 7 s later is accepted. The original
  ESP8266 firmware works the same way without noticing: it ignores everything but `F.F(` and repeats the
  acknowledgement after 7 s.
- Without STX there is no defined start for the block check character, so libamis does not check it then.
  libamis only takes data without STX after a repeated acknowledgement (or in the mode without acknowledgement),
  and it ignores an optical echo of its own acknowledgement.
- In most readouts at that meter the first character of one to four lines was missing (per 12 KB, e.g.
  `.60.4.2(…)` instead of `C.60.4.2(…)`). It never reached the UART: no parity, framing or overrun error at the
  ESP8266, no software buffer lost, independent of when the file is written to flash, and the original firmware
  1.6.8 loses characters the same way. With the same data from a simulated meter nothing is lost. It always hits
  the first character of a line, so the cause is most likely in the meter or its optical transmitter.
- The whole data set (about 12 KB, 379 lines) takes about 7 minutes at 300 baud; no M-Bus data during that time.
- If a cumulation (end of a billing period) falls into the readout, the historic value numbers of the tariff
  registers can be inconsistent (manual 5.2.12.3); then the data set should be read again.

- Codes for Z: 0 = 300, 1 = 600, 2 = 1200, 3 = 2400, 4 = 4800, 5 = 9600, 6 = 19200, 9 = 115200.
- libamis **never** uses programming or manufacturer specific modes (`ACK 0 Z 1/6/7`).

## 9. Sources

- Netz OÖ: *Technische Spezifikation AMIS-Zähler Kundenschnittstelle*, 2021-11-05 (German)
- Siemens: *AMIS TD-351x Benutzerhandbuch*, D23-510-1.02, 2008-12-04 (German; chapter 5.2 is the IEC 62056-21
  service interface, chapter 6 is about the M-Bus extension module, not the customer interface; e.g. in the repository
  [mgerhard74/amis_smartmeter_reader](https://github.com/mgerhard74/amis_smartmeter_reader) under `Docs/`)
- EN 13757-2/-3 (M-Bus), OMS Specification Vol. 2 Issue 3.0.1, IEC 62056-21
- FIPS-197 (AES), NIST SP 800-38A (CBC test vectors)
