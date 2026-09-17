# SickNode bench harness (P1 BENCH worker) — SPEC v2

Harness for the TNSSRC transform workbench benchmark. Modeled on
`~/workspace/tnssrc-frontend/bench12.sh` (encode → decode → SHA-256 verify)
and the matrix `run.sh` `force` pattern (timeout 3600, incident codes in logs).

## SPEC v2 conductor model

`npcc wbc IN OUT` runs the conductor: the true minimum over full exact runs
from TWO candidate families —
  (a) whole-file: raw + scan-predicted top-3 transforms
  (b) routed: per-block 256KB encode with ALL framing overhead counted
      (mode bytes, block table)
Bay 1 is the 256KB hot loop: it routes (per-block predicted transform via
fast probes), it doesn't judge. The winner's outer mode id is 24
(`WB_MODE_ROUTED`) for family (b) — verified from the frame header when the
binary lands (see "Winner parsing" below).
P1 runs 256KB only. 1MB/4MB block exploration is deferred to cold-loop.

## CLI contract with `npcc` (build worker's spec)

| Command | Meaning |
|---|---|
| `npcc wbc IN OUT` | Bay-1 encode: raw + scan-predicted top-3 transforms, exact-minimum winner, never-expand guard |
| `NPCC_WB_FULL=1 npcc wbc IN OUT` | FULL battery: every structurally-boarding transform, exact-minimum winner |
| `npcc wbd IN ORIG OUT` | decode (ORIG = original input path, OUT = decoded output) |
| `npcc wbscan FILE` | structure report |

## Scripts

- `bench_one.sh FILE BIN OUTDIR FULL` — single-file worker. Encodes under
  `timeout 3600` with `nice -n 10`, records wall seconds, winning bytes,
  winning outer mode id (parsed from stdout — see "winner parsing" below),
  then decodes and SHA-256-verifies. Writes `$OUTDIR/$f[.full].result` and
  `$OUTDIR/$f[.full].log`. Every failure mode (timeout, missing output,
  decode failure, SHA mismatch) is recorded in the `.result` line; nothing
  is silently skipped.
- `bench_wb.sh [BIN] [OUTDIR] [FULL]` — drives `bench_one.sh` over the 12
  Silesia files with `xargs -P2` (2 vCPU box, shared with the matrix job —
  never more than -P2). `FULL=1` writes `.full` suffixed files into the same
  OUTDIR so Bay-1 and full-battery runs never clobber each other.
- `audit.sh` — builds the prediction-vs-winner audit from the Bay-1 and
  FULL logs (see below).

## The standing constraint (scheduling vs selection)

Mixer/scout ordering is a SCHEDULING decision (what runs first while the rest
queues), never a selection decision. The conductor takes the true minimum
over full exact runs of every seated candidate — no heuristic gate ever
skips or selects a path. The tiers:

- Tier 1: hand-tuned structural scoring (the only model that ships).
- Tier 2: top-3 accuracy measurement — the scout's job is to seat the true
  winner among the 3, not to pick it.
- Tier 3: promotion-mirror log (`MIXER_LOG.tsv`, scan-features → winner
  pairs, file-level + per-block lines in audit mode) — append-only
  accumulation until the corpus grows. With 12 files anything trained
  overfits; never build or fit a classifier on it.

## Winner parsing (P1 fix notes — verified against src/main.c + src/workbench.c)

`bench_one.sh` reads the winning outer mode id from **the frame header byte
itself** (byte 0 of the output file). This is ground truth, independent of
stdout. The rule, exactly as the decoder (`wb_decode_frame`,
`src/workbench.c:1769`) applies it:

- `24` → routed (SPEC v2 frame; outer dispatch at `workbench.c:1901`)
- `13`–`23` → transform outer frame (`[mode u8][meta_len u32 LE][meta][inner
  blob]`, inner mode byte at offset `5+meta_len`): 13 columnar, 14 delta8,
  15 delta16, 16 delta24, 17 delta32, 18 xor16, 19 xor32, 20 exe, 21 img2d,
  22 bitplane, 23 shuffle
- anything else → RAW win: the file is a **bare inner frame**, and byte 0
  is the INNER mode id (0 lz, 1 bwt, 3 xz, 5 col, 6 tr, 7 lzm2, 8 blk).
  Inner ids are all < 13, so they never collide with the outer range.
  (Bug #1, fixed 2026-09-16: the old harness expected byte 0 == 12 for a
  raw win, but 12 is the candidate-list sentinel, never on the wire —
  every raw win was mis-parsed.)

The wbc machine line is the only `^file=` line on stdout
(`src/main.c:462`):
`file=IN raw=N packed=M winner=W seats=a>b>c full=0|1 routed=0|1 enc=Ss`.
The harness cross-checks all three: stdout winner vs header byte vs
handoff incumbent; any disagreement is an INCIDENT.

Per-candidate exact bytes come ONLY from the escalation handoff export
(`NPCC_WB_HANDOFF=path`, `src/workbench.c:1506`–`1527`): `incumbent
bytes=N mode=M param=P` plus `tried mode=M param=P bytes=N`
(`bytes=X` when the candidate failed/declined). (Bug #2, fixed
2026-09-16: the old harness parsed per-candidate bytes from stdout, but
stdout prints no per-candidate totals — the handoff is the only source.
The harness sets `NPCC_WB_HANDOFF` per run.) Runner-up margin =
second-smallest tried bytes − smallest tried bytes, from the sorted
tried table.

(Bug #3, fixed 2026-09-16: the decode step called
`wbd OUT SRC_PATH DEC`, but `wbd`'s second argument is the original
size as a decimal byte count (`src/main.c` `cmd_wbd`), not a path —
every decode failed with `wbd: bad orig`, rc=2. Fixed to
`wbd "$out" "$raw" "$dec"`. Verified: path form → rc=2, size form →
rc=0 with SHA-256-identical output.)

## Audit protocol (deliverable, not a footnote)

Dimension 1 — scan top-3 vs full battery (whole-file family):
1. Bay-1: `bench_wb.sh BIN OUT 0`. From each `$f.log`, extract scan's
   predicted top-3 candidates (order matters).
2. FULL: `bench_wb.sh BIN OUT 1`. From each `$f.full.log`, extract every
   candidate's total packed bytes; the true winner is the exact minimum.
3. `audit.sh` emits the per-file table: predicted top-3 vs true winner,
   plus a MISS flag on every file where the predicted winner differs from
   the true winner.

Dimension 2 — hot-loop routing vs full-exact per-block winners:
1. The build worker documents its routing-map dump format in `ROUTING.md`
   (read it when it lands). The dump gives, per 256KB block, the hot loop's
   predicted transform.
2. Sampled basis: for each file, extract up to 6 blocks (start, 25%, 50%,
   75%, end, plus one flagged heterogeneous/edge block from the routing map)
   with `dd bs=262144`, then run the FULL battery on each 256KB slice —
   on a single block the full battery's exact minimum IS the per-block true
   winner (framing overhead of a 1-block routed frame is documented as an
   approximation). Compare against the routing map's prediction; MISS on
   every sampled block where the prediction is not the exact minimum.
3. Full per-block full-battery audit on every file is deliberately skipped
   (too slow); the sampling rule above is the documented judgment call.
   ~72 slice encodes worst case, each on 256KB — minutes, not hours.

If `wbc` stdout does not print per-candidate totals, the fallback is
`NPCC_VERBOSE=1` (already set) and then reading the build worker's source
for the candidate log format. Do not estimate or reconstruct candidate
bytes — the audit only reports what the binary actually emitted.

## Dimension 3 — top-3 accuracy (Tier 2)

The scout's job is not to pick THE winner; it is to get the true winner into
the 3 seats. For each of the 12 files:
1. From the Bay-1 log: the mixer's seated order (the 3 candidates, in order).
2. From the FULL battery log: the true winner (exact minimum).
3. HIT if the true winner is among the 3 seats, else MISS.

Target: 12/12. A MISS is a first-class finding in RESULTS_WORKBENCH.md,
same status as a prediction-miss.

Ground-truth cross-checks (all READ-ONLY, never touch the matrix dir):
- `~/workspace/tnssrc-matrix/MATRIX.md` when it lands: the full per-mode
  byte matrix confirms which whole-file outer candidate is truly minimal.
- Exact-selector per-file winners (BWT 7/12, LZM2 5/12): the inner mode of
  each conductor winner should be consistent with the exact-selector's
  BWT/LZM2 assignment for that file; any divergence is flagged.

## Tier 3 — promotion-mirror log check (logging only)

The build worker makes `wbc` append scan-features → winner pairs to
`~/workspace/tnssrc-workbench/MIXER_LOG.tsv` (file-level lines always,
per-block lines in audit mode). The harness verifies:
- the file exists and grows after both the P1 and FULL runs;
- line-count sanity: ≥12 file-level lines after P1; after the FULL run,
  per-block lines ≈ Σ ceil(filesize/256KB) across the 12 files
  (mozilla is the big one; small files contribute 1–4 lines each);
- each line parses (tab-separated, sane field counts, numeric byte fields);
- no duplicate file-level lines for the same run (append-only, but one run
  must not double-log the same file).

No classifier is built or fitted on this log — under a 12-file corpus any
model overfits. This is accumulation for a later corpus, nothing more.

## Exactness rule

Every byte count in RESULTS_WORKBENCH.md comes from a real
encode + decode + SHA-256 verify. No estimates anywhere.
