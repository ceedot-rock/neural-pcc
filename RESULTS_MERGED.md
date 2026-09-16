# TNSSRC: exact selector + front-end arms — merged verified results

Date: 2026-09-16. Tree: `~/workspace/tnssrc-dev`, branch `parsing-match-buildout`.
Local only, never pushed.

## What was built

**Part 1 — exact-minimum mode selection** (commit `8a9eae6`):
- `tnssrc_encode` runs every candidate for real — modes 0 (LZ), 1 (BWT),
  3 (xz), 5 (col-BWT best of 4 widths), 6 (trans-BWT), 7 (LZM2), 8
  (per-block) — and picks the true minimum by exact output bytes.
- Removed heuristic gates: `is_textish`, `want_lz`/`want_bwt`/`want_xz`/
  `want_lzm2`, `xz_good`/`lm_good` <50% shortcuts, xz verb-loop early break.
- Per-block top-level mode 8: inputs > 1 MiB split into 1 MiB blocks, each
  block picks its own winner among modes 0/1/7.

**Part 2 — front-end transform arms** (ported from the frontend-arms tree,
commit `d5d04b1`, verified there at 47,214,554 bytes / −720,853 vs stock):
- Mode mapping in THIS tree: inner modes 0/1/3/5/6/7/8 (8 = per-block), so
  the arms live at outer modes **9 = sao columnar** (28-byte records),
  **10 = d16 u16-delta**, **11 = exe E8/E9 normalization**.
- Outer frame: `[mode u8][meta_len u32 LE][meta][inner blob]`.
- Every file tries raw + every structurally-applicable transform with a
  REAL run of the full inner exact selection; winner = exact minimum.
- Raw output byte-identical to the pre-port exact-selector encoder
  (verified by byte-compare on synthetic inputs).
- Never-expand: a transform wins only when its total < |input|.

## Correctness

- `make test`: test_npcc, test_riser, test_lzm2, test_frontend all pass.
- test_frontend: 12/12 synthetic cases (sao decline/unsorted/raw-fallback,
  d16 decline/odd/wrapping, exe decline/overlap/giant-rel32).
- Full 12-file run below: every file decoded and SHA-256 verified.

## Results

### Part 1 — OLD gated selector vs NEW exact selector (12/12 SHA-256 verified both runs)

| file | old bytes | new bytes | delta | old mode | new mode |
|---|---|---|---|---|---|
| dickens | 2,696,613 | 2,696,613 | 0 | 1 (BWT) | 1 (BWT) |
| mozilla | 13,754,644 | 13,614,244 | −140,400 | 7 (LZM2) | 7 (LZM2) |
| mr | 2,412,733 | 2,412,733 | 0 | 1 (BWT) | 1 (BWT) |
| nci | 1,671,141 | 1,659,066 | −12,075 | 7 (LZM2) | 7 (LZM2) |
| ooffice | 2,484,820 | 2,471,831 | −12,989 | 7 (LZM2) | 7 (LZM2) |
| osdb | 2,611,929 | 2,611,929 | 0 | 1 (BWT) | 1 (BWT) |
| reymont | 1,181,597 | 1,181,597 | 0 | 1 (BWT) | 1 (BWT) |
| samba | 3,855,393 | 3,843,315 | −12,078 | 7 (LZM2) | 7 (LZM2) |
| sao | 4,518,258 | 4,509,048 | −9,210 | 7 (LZM2) | 7 (LZM2) |
| webster | 8,321,788 | 8,321,788 | 0 | 1 (BWT) | 1 (BWT) |
| x-ray | 4,006,977 | 4,006,977 | 0 | 1 (BWT) | 1 (BWT) |
| xml | 419,514 | 419,514 | 0 | 1 (BWT) | 1 (BWT) |
| **TOTAL** | **47,935,407** | **47,748,655** | **−186,752** | | |

24/24 encodes decoded and SHA-256 verified. No timeouts, no failures.
Per-block mode 8 was tried on every file > 1 MiB and never won a file.

### Part 2 — merged tree (exact selector + sao/d16/exe arms)

(RUNNING — table to be filled)

## Wrong-pick audit

**Finding: the old gated picker chose the correct mode on all 12 files.**
Every file's exact-selector winner is the same mode the old heuristic
gates picked. Zero wrong picks on Silesia — the gates were not costing any
file a worse mode.

The entire −186,752 byte gain comes from the **improved LZM2 gene**
(`src/lzm2.c` / `src/parse_rep4.c` changes in this tree), not from mode
selection: mozilla/nci/ooffice/samba/sao all re-won mode 7 with a better
encoder. Breakdown of the gain by file:

| file | gain | source |
|---|---|---|
| mozilla | −140,400 | better LZM2 encode, same mode 7 |
| ooffice | −12,989 | better LZM2 encode, same mode 7 |
| samba | −12,078 | better LZM2 encode, same mode 7 |
| nci | −12,075 | better LZM2 encode, same mode 7 |
| sao | −9,210 | better LZM2 encode, same mode 7 |

Text files (dickens/mr/osdb/reymont/webster/x-ray/xml) are byte-identical
old-vs-new: BWT was already the true minimum and the gates didn't block it.

## How to reproduce

```
cd ~/workspace/tnssrc-dev && make
~/workspace/tnssrc-bench/bench.sh bin/npcc <outdir> <log> <label>
# per file: npcc c (3600 s timeout), npcc d, SHA-256 vs original,
# mode bytes via ~/workspace/tnssrc-bench/modeof.py
```

## Incidents

- VM reboot mid-run wiped /tmp, killing the first Part-1 benchmark and the
  /tmp binaries. All repo state is on the persistent disk and survived.
  Benchmark harness moved to ~/workspace/tnssrc-bench (persistent) with
  resume support; OLD/NEW binaries rebuilt from git worktrees
  (~/workspace/tnssrc-old @ be24980, ~/workspace/tnssrc-new @ 8a9eae6).
