# Contributing to Neural-PCC (TNSSRC)

Thanks for helping make compressor 2 provable.

## Ground rules

- **Never expand.** Compressed output must never be larger than the input.
- **CLI-first.** Features must work through the `npcc` CLI, not just the Python API.
- **Decode + SHA-256.** Every byte-count claim must be verified by a real
  decode followed by a SHA-256 comparison against the original.
- **Claim lock.** Do not touch a verified figure in the README unless you can
  reproduce it end to end. See the README's Claim lock section.
- Pathway laws in `cuni/` follow CuNi: same stdout on every catalog seat, or refuse.

## Quick checks

```sh
make                # builds ./bin/npcc
make test           # C test binaries: test_npcc, test_riser, test_lzm2
python3 tests/test_npcc.py   # Python compress/decompress roundtrip
python3 -m npcc serve 127.0.0.1 8080   # HTTP API: /health, /service, /about,
                                       # /endpoints, POST /api/compress, POST /api/decompress
```

CI runs all of these on every pull request (`.github/workflows/audited-checks.yml`).

## Changing the codec or adding a pathway

1. Make the change in `src/` (and `cuni/` for new pathway laws).
2. Keep the never-expand law: add or extend a test that fails if any input grows.
3. Run `make test` and `python3 tests/test_npcc.py` — both must be green.
4. If you touch a benchmark figure, decode + SHA-256 verify it and record the
   exact command, corpus, and date.
5. Open a pull request using the template. Use the PR template's checklist.

## Licensing

Neural-PCC is dual-licensed (AGPL-3.0-or-later or the Slid Phi Labs Commercial
License). See `LICENSE`, `LICENSE.AGPL-3.0`, `LICENSE.COMMERCIAL`, and
`COMMERCIAL.md`. By contributing you agree your contribution may be distributed
under both. Do not change the license of this tree in a pull request.
