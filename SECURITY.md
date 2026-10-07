# Security Policy

Neural-PCC (TNSSRC) is a lossless compression codec. A bug that corrupts data
silently, produces output that does not decode to the exact input, or breaks
the never-expand guarantee is a security issue, not a normal bug.

## Reporting a vulnerability

Please do not open a public issue for security problems.

- Use GitHub's private vulnerability reporting on this repository
  (Security tab, "Report a vulnerability"), or
- Email: corey@slidphilabs.com with the subject line `Neural-PCC security`

Include the affected file or pathway, steps or inputs to reproduce, and what
you expected versus what happened.

You can expect an acknowledgement within 3 business days. We will keep you
updated while we investigate and credit you in the release notes unless you
prefer to stay anonymous.

## In scope

- Roundtrip failure: any input where decompress(compress(x)) != x
- Silent corruption: a decoder that returns success on a wrong output
- Never-expand violation: compressed output larger than the input
- Format-version acceptance bugs (accepting or rejecting the wrong versions)
- The `npcc` CLI, the C library, the Python package, and the HTTP API server

## Out of scope

- Host xz/gzip/bzip2 skins (retired, not in this tree)
- Operator deployments we do not run
- Social engineering, spam, or denial-of-service against hosted demos
