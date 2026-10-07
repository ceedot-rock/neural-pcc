## What changed

<!-- One or two sentences. -->

## Claim impact

<!-- Any benchmark figure touched (e.g. Silesia 12 total), or "none". -->

## Checks

- [ ] `make test` passes (C test binaries: test_npcc, test_riser, test_lzm2)
- [ ] `python3 tests/test_npcc.py` passes (Python roundtrip)
- [ ] Any claim figure changed is decode + SHA-256 verified per the README Claim lock
- [ ] Never-expand law holds: compressed output is never larger than the input
- [ ] New pathway laws added to `cuni/` follow the CuNi rule (same stdout on every catalog seat, or refuse)
- [ ] License unchanged: this tree stays AGPL-3.0-or-later OR commercial (see LICENSE)
