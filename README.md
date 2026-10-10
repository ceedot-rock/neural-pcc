> **This repo has moved into the verse.** Development continues at
> [ceedot-rock/PCCVerse](https://github.com/ceedot-rock/PCCVerse), in folder neural-pcc/.
> This copy is archived and read-only - history preserved, nothing lost.

# TNSSRC (Neural-PCC)

[![Audited checks](https://github.com/ceedot-rock/neural-pcc/actions/workflows/audited-checks.yml/badge.svg)](https://github.com/ceedot-rock/neural-pcc/actions/workflows/audited-checks.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

**TNSSRC** (TriNeural Shared Spine Row Compression) is a local lossless compression
engine from Slid Phi Labs — for anyone who compresses files on their own
machine and wants lab-owned math instead of a system codec. It is written in
plain C, it never emits output larger than its input, and its public result is
verifiable: submitted to Matt Mahoney's Silesia benchmark as TNSSRC 0.1.0 at
43,724,575 bytes — decode+SHA-256 verified on all 12 Silesia files on
2026-09-18. Dual-licensed AGPL-3.0-or-later or a written commercial grant.

**License:** AGPL-3.0-or-later **or** a written commercial grant ([LICENSE](LICENSE), [COMMERCIAL.md](COMMERCIAL.md)). Visibility is not a grant to ship TNSSRC in a closed product.

**Compressor 2.** PCC is compressor 1. TriNeural Shared Spine Row Compression. Own C. Not host xz/gzip/bzip2. Separate from Dial A / PCC daily.

## Install (CLI-first)

```
make
sudo make install          # PREFIX=/usr/local → /usr/local/bin/npcc
# or: make install PREFIX=$HOME/.local
npcc --help
npcc --version
```

Uninstall: `sudo make uninstall` (same `PREFIX`).

## Usage

```
make
./bin/npcc c IN.bin OUT.npcc
./bin/npcc d OUT.npcc OUT.bin
./bin/npcc bench IN.bin ar
make test
```

`ar` / `full` run TNSSRC. Pathway laws in `cuni/` (CuNi: same stdout on every catalog seat, or refuse). Never expand vs `|x|`.

## Claim lock

Lab Silesia 12: packed **43,724,575** / 211,938,580. DECODE_OK 12/12
(decode + SHA-256 verified 2026-09-18). Beats PCC pcc-0.12.1 (51,498,645).

**Never claim:** #1, OSCB, or Fast MB/s. Speed is not this seat’s claim.

## Silesia 12

| file | raw | TNSSRC (verified) | PCC |
|---|---:|---:|---:|
| dickens | 10,192,446 | 2,538,324 | 2,738,073 |
| mozilla | 51,220,480 | 13,304,103 | 15,970,087 |
| mr | 9,970,564 | 2,111,392 | 2,537,416 |
| nci | 33,553,445 | 1,156,452 | 1,582,235 |
| ooffice | 6,152,192 | 2,310,168 | 2,670,536 |
| osdb | 10,085,684 | 2,424,253 | 2,761,563 |
| reymont | 6,627,202 | 1,096,419 | 1,217,202 |
| samba | 21,606,400 | 3,748,880 | 4,156,202 |
| sao | 7,251,944 | 3,659,859 | 4,992,917 |
| webster | 41,458,703 | 7,272,548 | 8,169,156 |
| x-ray | 8,474,240 | 3,682,106 | 4,260,093 |
| xml | 5,345,280 | 420,071 | 443,165 |
| **total** | **211,938,580** | **43,724,575** | **51,498,645** |

This supersedes the earlier 48,541,366 measurement (unverified — do not
cite). The xz-9 total of 48,795,480 previously quoted here is unattributed
(no per-file breakdown, xz version, flags, command, or date were recorded),
so it is not repeated as a claim. The superseded run is kept for the record
in `bench/silesia12-superseded-48.54M.txt`.

Contact: corey@slidphilabs.com

## From the same lab

- **pulsar** — free local best-path compressor (GPLv3 demo, not TNSSRC): https://github.com/ceedot-rock/pulsar-best
- **TRUSTREAM** — lossless compression for live data streams in 4 KiB tiles: https://github.com/ceedot-rock/trustream
- **AwLPay** — multi-rail agent payments (USDC x402 on Base and Solana, PayPal sandbox bridge): https://github.com/ceedot-rock/awlpay
- **agenTill** — drop-in payment box that turns any online product into a storefront agents can buy from: https://github.com/ceedot-rock/agenTill
- **ExactOdds** — provably-fair game math, byte-identical rules across five languages: https://github.com/ceedot-rock/exactodds
- **Chamber** — two-key JSON sealing for secrets: https://github.com/ceedot-rock/json-chamber-sdk
- Lab site: https://www.slidphilabs.com
