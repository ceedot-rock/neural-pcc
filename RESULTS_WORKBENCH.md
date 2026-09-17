# SickNode workbench — RESULTS (P1)

Workbench: `~/workspace/tnssrc-workbench`, branch `workbench`.
Binary: `bin/npcc` (build verified by P1 BUILD worker; coordinator-verified).
Corpus: `~/workspace/helix-match-exp/corpus/full` (211,938,580 bytes, 12 Silesia files).

Baselines (exact, SHA-256-verified, from prior lanes):
- Selector-only: **47,748,655** bytes
- Frontend-arm: **47,214,554** bytes (sao −534,451 columnar; ooffice −165,035 exe;
  mr −21,367 d16; x-ray d16 LOST; mozilla exe LOST)

Exactness rule: every byte count below comes from a real
encode → decode → SHA-256-verify. No estimates anywhere.

---

## P1 — Bay-1 12-file benchmark (256KB routed blocks)

**COMPLETE 2026-09-17** — 12/12 files, all SHA-256 verified, zero incidents.

- Total: **47,586,335** bytes
- Δ vs selector-only (47,748,655): **−162,320** (better)
- Δ vs frontend-arm (47,214,554): **+371,781** (worse)
- Raw corpus: 211,938,580 bytes (verified sum) → ratio 0.2245

### Per-file table

| file | raw bytes | winner bytes | ratio | winning path | inner | runner-up | margin | enc wall s | sha |
|---|---|---|---|---|---|---|---|---|---|
| dickens | 10,192,446 | 2,696,558 | 0.2645 | raw | bwt | delta8 | 114,092 | 1209.7 | yes |
| mozilla | 51,220,480 | 13,614,189 | 0.2658 | raw | lzm2 | bitplane | 184,151 | 4771.7 | yes |
| mr | 9,970,564 | 2,412,678 | 0.2420 | raw | bwt | delta16 | 32,153 | 840.4 | yes |
| nci | 33,553,445 | 1,659,012 | 0.0494 | raw | lzm2 | bitplane | 176,038 | 2399.7 | yes |
| ooffice | 6,152,192 | 2,310,113 | 0.3755 | **exe** | lzm2 | raw | 161,663 | 369.9 | yes |
| osdb | 10,085,684 | 2,611,874 | 0.2590 | raw | bwt | bitplane | 546,822 | 771.0 | yes |
| reymont | 6,627,202 | 1,181,543 | 0.1783 | raw | bwt | bitplane | 57,770 | 676.4 | yes |
| samba | 21,606,400 | 3,843,260 | 0.1779 | raw | lzm2 | bitplane | 484,739 | 1955.3 | yes |
| sao | 7,251,944 | 4,508,993 | 0.6218 | raw | lzm2 | columnar | 126,797 | 538.8 | yes |
| webster | 41,458,703 | 8,321,733 | 0.2007 | raw | bwt | bitplane | 701,201 | 3867.8 | yes |
| x-ray | 8,474,240 | 4,006,922 | 0.4729 | raw | bwt | delta16 | 155,131 | 503.3 | yes |
| xml | 5,345,280 | 419,460 | 0.0785 | raw | bwt | bitplane | 58,039 | 444.3 | yes |

"winning path": whole-file `transform+inner` (on-wire header byte 20=exe,
1=bwt-inner, 7=lzm2-inner; all raw wins confirmed via Bug-#1 parse fix).
Runner-up margin = second-best tried bytes − winner bytes, from the
escalation handoff tried table. All `full=0` (conductor), `routedflag=1`
(routed candidate built but lost on all 12 files).

**Key observations:**
- ooffice is the ONLY file where a frontend arm (exe) wins whole-file.
  The exe transform was seated top-3 on mozilla too but lost to raw.
- sao: columnar seated 2nd but raw won by 126,797. The frontend baseline's
  −942,958 columnar gain (Op 13) did NOT reproduce in the workbench —
  the integrated columnar arm underperforms the standalone frontend.
- x-ray: delta16 seated 2nd but raw won by 155,131. Aware V6's 3,837,303
  not realized; workbench delta16 ≠ Aware pathway.
- dickens (2,696,558) and webster (8,321,733) both beat their verified
  baselines by exactly 55 bytes — the workbench parse improvements.
- nci (1,659,012) beats exact-selector (1,671,141) by 12,129 bytes.
- Inner modes: bwt wins 7/12 files (dickens, mr, osdb, reymont, webster,
  x-ray, xml), lzm2 wins 5/12 (mozilla, nci, ooffice, samba, sao).
  Matches the exact-selector 7/5 split.

### MIXER_LOG.tsv sanity

_pending (audit.sh Tier 3)_

### INCIDENTS

- **6 VM reboots** total (3 during Bay-1 at 2026-09-17 ~00:26, ~01:39,
  ~02:50 UTC; 1 during initial FULL at ~04:19; 2 during FULL audit at
  ~04:20, ~05:32; plus an earlier pre-Bay restart 2026-09-16 ~23:12 UTC).
  Each reboot killed the running wbc processes. The resume logic in
  bench_wb.sh preserved all completed .result files; the run was
  relaunched and completed 12/12 Bay-1 with no data loss. No matrix jobs
  were affected (none were running during the reboots). This is an
  infrastructure stability issue, not a benchmark defect.
- No encode/decode/SHA incidents. No timeouts. No handoff mismatches.

---

## P1 — Full-battery + routing audit

**STATUS: PARTIAL** — Bay-1 complete (12/12). FULL battery 2/12 (xml, reymont).
Routing audit dimension-2 complete (66 slices). Dimensions 1 & 3 pending
full FULL battery.

### COST GATE

**PASS.** After 2 FULL files (xml: 1088.6s/5.3MB, reymont: 1172.0s/6.6MB):
- Rate: 0.0001888 s/byte
- Projected 12-file (211,938,580 bytes): 40,018s = 11.1h single-thread
- At 1.4 effective CPUs: **~7.9h wall** (< 36h gate)
- Both FULL winners MATCH Bay-1 winners (xml: 419,460; reymont: 1,181,543).
  FULL seats longer (9 and 6 vs 4) but winner unchanged.

### Dimension 2: Hot-loop routing vs sampled per-block exact winners

**Result: 58/66 top-1 agreement (87.9%). Total miss cost: 33,749 bytes.**

Sampling: up to 6 blocks per file (0, 25%, 50%, 75%, last, plus one
"interesting" block where prediction differs from neighbors or tiebreak
fired). Each 256KB slice run through FULL battery (NPCC_WB_FULL=1); the
slice tried-table minimum (excluding routed) is the per-block true winner.

Per-file route summaries (from `npcc wbroute`):
| file | nblocks | router prediction |
|---|---|---|
| dickens | 39 | raw:39 |
| mozilla | 196 | raw:185, bitplane:1, shuffle:10 |
| mr | 39 | raw:14, delta16:19, img2d:6 |
| nci | 128 | raw:128 |
| ooffice | 24 | raw:2, delta16:1, exe:21 |
| osdb | 39 | raw:39 |
| reymont | 26 | raw:26 |
| samba | 83 | raw:80, shuffle:3 |
| sao | 28 | raw:25, columnar:3 |
| webster | 159 | raw:159 |
| x-ray | 33 | raw:31, columnar:1, delta16:1 |
| xml | 21 | raw:21 |

Miss details (8 misses):
- **sao** (2 misses, cost 21,111B): blocks 3 and 27 predicted columnar,
  true winner raw. The router over-predicts columnar on sao; the
  integrated columnar arm underperforms (cf. frontend baseline −943KB
  not reproduced).
- **x-ray** (5 misses, cost 12,629B): blocks 0,8,16,24 predicted
  delta16/raw, true winner **columnar**; block 32 predicted columnar,
  true winner shuffle. The mixer's Bay-1 seats for x-ray were
  raw>delta16>delta32>img2d — **columnar was not seated**, yet it wins
  4/5 sampled blocks exactly. This is a mixer blind spot: the columnar
  transform is competitive per-block on x-ray but never gets a seat.
- **mozilla** (1 miss, cost 9B): negligible.

**Key finding:** The hot-loop router is 87.9% accurate top-1, but the
misses are systematic: (1) sao columnar over-prediction (router sees
structure the arm can't exploit), (2) x-ray columnar under-seating
(exact winner missed by top-3). The 33.7KB total miss cost is small
relative to file sizes, but the x-ray blind spot suggests the mixer
should seat columnar for x-ray-like inputs.

### Dimensions 1 & 3: Mixer top-pick and top-3 accuracy vs FULL

**PENDING** — requires FULL battery handoffs for all 12 files (currently
2/12: xml, reymont). For both completed files:
- Dimension 1 (seats[1] vs FULL winner): xml pick=bitplane, true=raw → MISS;
  reymont pick=bitplane, true=raw → MISS. (Expected: when raw wins,
  the top non-raw pick cannot match.)
- Dimension 3 (true winner in seats[1..3]): xml true=raw (seats[0]) → HIT;
  reymont true=raw → HIT. 2/2 so far.

MATRIX.md cross-check: `~/workspace/tnssrc-matrix/MATRIX.md` not present
(matrix worker has not landed it yet; dir is read-only, never written).

### Tier 3: MIXER_LOG.tsv sanity

_pending (run audit.sh when FULL completes)_

### INCIDENTS (audit phase)

- **2 additional VM reboots** during FULL battery (2026-09-17 ~04:20,
  ~05:32 UTC), total **6 reboots** across Bay-1 + audit. Resume logic
  preserved all completions.
- Heavy CPU contention from other workers' approved jobs (pcc-weights-bench
  zstd/xz, model-weights benchmark). FULL battery processes throttled to
  ~30-40% CPU (vs ~70% when box is free). Not killed (not mine to kill).
