# Security policy

libamis parses data from the optical interface of the meter, i.e. input that anyone with physical access to
the reading head can control. Memory safety bugs in the parsers (`amis::Reader`, `amis::IecReader`) and ways to
get made-up values accepted with a key set are treated as security issues.

## Reporting a vulnerability

Please report it privately via GitHub: **Security → Report a vulnerability** in this repository
(private vulnerability reporting). Do not open a public issue for it. Please include the libamis version, the
platform and, if possible, the input bytes that trigger the problem.

## Supported versions

Only the latest release gets fixes.

