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

- **4 VM reboots** during Bay-1 (2026-09-17 ~00:26, ~01:39, ~02:50 UTC, plus
  one earlier). Each reboot killed the running wbc processes. The resume
  logic in bench_wb.sh preserved all completed .result files; the run was
  relaunched 4 times and completed 12/12 with no data loss. No matrix jobs
  were affected (none were running during the reboots). This is an
  infrastructure stability issue, not a benchmark defect.
- No encode/decode/SHA incidents. No timeouts. No handoff mismatches.

---

## P1 — Full-battery + routing audit

PENDING.
