# SickNode hot loop: per-block routing (spec v2)

## Design

The hot loop routes; it doesn't judge. For each block it runs a cheap
deterministic probe per scan-narrowed candidate and records the predicted
transform. **Only full exact runs determine winners** — the conductor
re-runs every routed block through the full `tnssrc_encode_inner()`
7-mode selector and takes the true minimum by actual total bytes over
the whole-file candidates plus the routed candidate.

```
input ──► wb_scan (file) ──► wb_route (per block) ──► routing map
                                                        │
whole-file cands (raw + mixer top-3, full inner) ──┐    │
routed blocks (map transform + full inner each) ───┴──► min bytes wins
```

## Block size

The block size is a **parameter**, never a hardcoded constant in the
routing/encode path:

- `wb_route(in, n, block_size, rep, &routes, &nblocks)`
- `wb_encode_routed(in, n, block_size, routes, nblocks, &out, &on)`
- `nblocks = ceil(n / block_size)` (derived, not stored independently)

P1 default: **256 KiB** (`WB_HOT_BLOCK`; `wb_hot_block_size()` reads
`NPCC_WB_BLOCKKB`, default 256, clamped to 4–16384). The routed
conductor candidate is built only when the input spans **≥ 2 blocks**;
a single block is just the whole file and routing would add pure
overhead.

## Per-block light scan

Each block gets a cheap feature pass (`wb_block_features`, ~ms):

- byte entropy, H1 (conditional entropy), alphabet utilization,
  E8/E9 site density — all block-local,
- per-width subtract-delta smoothness (8/16/24/32) — block-local,
- record period, period confidence, tar flag, homogeneity,
  bwt-friendly — inherited file-global (record structure is global).

## Candidate narrowing (mixer, per block)

The Bay-0 mixer scores candidates from the block's features; the hot
loop probes **raw + the top-3 boarding outer transforms** by block-local
mixer score (ties → lower mode id, deterministic). This is the
"scan-narrowed top candidates" set. Narrowing is scheduling, not
elimination at the file level — the conductor still runs its own
whole-file shortlist exactly.

## The probe

**Engineering choice (measured):** the probe encodes the transformed
block with inner mode **BWT (1) only**.

Measured on the first 256 KiB of `mozilla`: BWT packs in ~0.6 s vs
LZM2 (7) at ~2.2 s, and the transform choice dominates the ranking, so
the 3.7× slower second mode is reserved for tie-breaks. Spec v2
suggested `{1,7}`; the P1 probe set is `{1}` on measured cost grounds.

**Probe scheduling (spec v3b)** follows the mixer inner-mode scores:
BWT probes first (stage 1 — the fast discriminator; the probe ranks
transforms, not inner modes, and BWT's 3.7× speed advantage dominates).
When the top-2 BWT probes agree within 5% **and** the block actually
compressed **and** the block is ≥ 64 KiB, both finalists are re-probed
with the stage-2 inner mode — the highest-scoring mixer inner mode
other than BWT (usually LZM2, sometimes LZ on noise) — and
`min(stage1, stage2)` wins the block. The audit dump records the
tie-break flag and the stage-2 mode (`1(m7)`) so the bench worker can
measure whether it earns its cost.

Probe output per block (`wb_block_route_t`):

- `mode` / `param` — the probe-winning transform,
- `probe_bytes` — the winner's deciding probe bytes
  (`(size_t)-1` when every candidate declined),
- `cand_modes[]` / `cand_bytes[]` — every probed candidate and its BWT
  probe bytes (`(size_t)-1` = declined/failed),
- `lzm2_tiebreak` — whether the LZM2 tie-break ran,
- the block's light-scan features (for Tier-3 logging).

The probe **estimates**; it never sees the full inner selector. A probe
miss costs bytes on that block, and the audit below exists to quantify
exactly that.

## Routing audit dump

`wb_route_audit(tag, block_size, routes, nblocks)` returns malloc'd
NUL-terminated text (caller frees). `npcc wbroute IN [AUDIT]` runs
scan + route, prints a one-line summary, and writes the dump to `AUDIT`
or stdout.

```
# sicknode routing audit v1
# tag=/tmp/d1m.bin block_size=262144 nblocks=4
# per-block: idx mode param probe_bytes tiebreak | cand_mode:bytes,...
0 raw 0 78774 0 4 | raw:78774 bitplane:203539 shuffle:160155 delta8:86595
1 raw 0 75116 0 4 | raw:75116 bitplane:202794 shuffle:157521 delta8:81925
```

- Header comment lines (`#`) carry the format version, tag, block size,
  and block count.
- One line per block: index, winning mode/param, deciding probe bytes
  (`X` when every candidate declined), tie-break flag, candidate count,
  then `mode:bytes` per probed candidate (`X` = declined/failed).
- Deterministic: same input, same block size → same dump.

**Bench-worker audit recipe:** for each file, run `wbroute` to get the
predicted per-block transform, then independently run every transform ×
full `tnssrc_encode_inner()` per block to get the exact per-block winner;
compare predicted vs exact per block (top-1 agreement) and sum the byte
delta. The dump's `cand_mode:bytes` columns make the probe's ranking
auditable without re-running probes.

## Routed frame and block table (block-table addendum)

```
[24][block_size u32 LE][nblocks u32 LE][table][block frames...]
```

- The block table is sized **dynamically**: 16 bytes × `nblocks`, where
  `nblocks = ceil(orig / block_size)` is derived from the frame's own
  `block_size` field and the caller's `orig` at decode time. There is no
  fixed-size table anywhere; a 1 MiB block size does not appear in the
  code path.
- Table entry: `[offset u64 LE][len u64 LE]`, offset from frame start.
  (Mode/param/transformed-length live inside each block's own sub-frame,
  so per-block decode reuses the exact whole-file frame parser —
  `[mode][meta_len u32][meta][inner blob]`, or a bare inner frame for raw
  blocks.)
- Encoder: the assembled total must beat `|input|` (global never-expand);
  there is no per-block cap — a block's frame is the exact inner encoding
  of its routed transform, and the routed candidate only survives when the
  whole assembly wins. A block whose routed transform fails exact encoding
  fails the routed candidate — the map is then unreliable for this input
  and the conductor falls back to whole-file candidates.
- Decoder (`wb_decode_routed`) validates **before allocating or using**:
  header present; `block_size ≥ 1`; `nblocks ≥ 1`; the table
  (16 × nblocks) fits inside the frame (this rejects absurd `nblocks`
  before the table is touched); `nblocks` is consistent with `orig`
  (`(nblocks-1)*block_size < orig ≤ nblocks*block_size`); each entry's
  offset/len lies inside the frame at/after the table; entries are in
  order and non-overlapping; each block frame decodes to exactly its
  expected original length (`block_size`, except the last block).
  Corrupt frames fail closed with `-1`; no partial output is returned.

## CLI output formats (spec v3 Tier-2)

`wbscan` — single machine-parseable line:

```
file=PATH size=N entropy=H h1=H1 alpha=A e8e9_per_mb=D period=P period_conf=C smooth8=a smooth16=b smooth24=c smooth32=d is_tar=t homogeneous=g bwt_friendly=f seats=raw>...>... scan=Ss
```

`wbc` — single machine-parseable line: seated candidates in order plus
the final winner:

```
file=IN raw=N packed=M winner=W seats=raw>...>... full=0|1 routed=0|1 enc=Ss
```

`winner` is `raw`, a transform name, or `routed`. `seats` is the
conductor's seat order (top-3 + raw, or the full battery).

`wbroute` — summary line plus the audit dump:

```
file=IN size=N block_size=B nblocks=K routes=raw:120,delta8:45,... route=Ss
```

## Tier-3 logging

Every `wbc` run appends to `MIXER_LOG.tsv` (`NPCC_MIXER_LOG` overrides
the default `~/workspace/tnssrc-workbench/MIXER_LOG.tsv`): one `whole`
row (file scan features, seat order, exact overall winner) and one row
per block (block features, probe ranking, block transform). Append-only;
no learning happens in P1 — the log is the future training signal. With
256 KiB blocks each file contributes ~4× the rows per MiB.

Row semantics (honest labeling): `whole` rows are whole-file training
pairs — scan features → the exact winning transform by actual bytes.
Numeric block rows are **router** training pairs — the `winner` column
is the router's *predicted* transform for that block (not a per-block
exact bakeoff), and `packed_bytes` is the exact bytes of that block's
full-inner run under the predicted transform. The routed candidate's
aggregate outcome lives in the escalation handoff (`HANDOFF.md`), not
in this log.

## Escalation handoff

When the combo loop (L2) escalates to the cold loop (L3), the handoff
carries momentum forward: incumbent bytes, full scan report, and the
tried-candidate table. See `HANDOFF.md`. `npcc wbc` exports it to the
path in `NPCC_WB_HANDOFF`.

## Performance notes

Routing costs ~1–3 s per 256 KiB block (light scan + up to 4 BWT
probes + occasional LZM2 tie-breaks). The routed conductor candidate
then runs the **full** 7-mode inner selector per block, so `wbc` on
large files is dominated by the routed path (e.g. ~170 s for 1 MiB of
text, mostly LZM2 per block). This is the spec'd cost of exact
per-block minima; `NPCC_WB_BLOCKKB` scales the block count for
experiments.
