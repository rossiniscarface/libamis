# Security policy

libamis parses data from the optical interface of the meter, i.e. input that anyone with physical access to
the reading head can control. Memory safety bugs in the parsers (`amis::Reader`, `amis::IecReader`) and ways to
get made-up values accepted with a key set are treated as security issues.

## What libamis protects against

With a key set, someone at the optical port without the key cannot get other values accepted: unencrypted
frames are rejected, only the decrypted bytes are parsed, the header that forms the IV and the record layout of the
decrypted data are checked, and the meter time must move forward (no replays). OMS security mode 5 has no MAC,
so these checks are what makes a forged frame fail. What libamis cannot prevent: someone blocking the meter (no
readings, `online()` goes false), and anyone who knows the key. The key stays in RAM as AES round keys.

## Reporting a vulnerability

Please report it privately via GitHub: **Security → Report a vulnerability** in this repository
(private vulnerability reporting). Do not open a public issue for it. Please include the libamis version, the
platform and, if possible, the input bytes that trigger the problem.

## Supported versions

Only the latest release gets fixes.

