# SickNode escalation handoff (spec v4)

## The invariant

Queue discipline across the loop escalation L1 → L2 → L3:

> **The handoff must carry momentum forward, NOT restart cold.** When the
> combo loop (L2 / Bay 2) escalates to the exploratory/cold loop (L3 /
> Loop 3), the cold loop inherits the incumbent best byte count (the bar
> to beat), the full scan report, and the list of already-tried candidates
> with their byte counts. It starts informed. A "stop and turn" —
> discarding this state at the handoff — is a SPEC VIOLATION, not a
> simplification.

The receiving loop MUST:

1. **Skip every candidate in the tried table** — no redoing work the
   conductor already ran.
2. **Use `incumbent_bytes` as its bar** — only a strictly better total
   displaces the incumbent.

## The struct

```c
typedef struct {
    int mode;           /* WB mode id (12..24); 12 = raw sentinel */
    uint64_t param;     /* transform param (columnar period); routed: block_size */
    size_t bytes;       /* actual total bytes of that candidate's full run,
                           or (size_t)-1 when the candidate failed/declined */
} wb_tried_t;
#define WB_TRIED_MAX 16
typedef struct {
    wb_scan_t scan;                 /* full scan report, inherited */
    size_t incumbent_bytes;         /* the bar to beat */
    int incumbent_mode;             /* WB mode id of the incumbent */
    uint64_t incumbent_param;
    wb_tried_t tried[WB_TRIED_MAX]; /* every candidate the conductor ran */
    int ntried;
    int routed_built;
    size_t routed_bytes;            /* (size_t)-1 when the routed run failed */
    size_t block_size;
    size_t nblocks;
} wb_handoff_t;
```

The P1 conductor (`wb_encode_full`) populates this on **every** run:

- `scan` — the whole-file `wb_scan` report (entropy, H1, alphabet
  utilization, E8/E9 density, record period + columnar gain, per-width
  smoothness, tar/homogeneous/bwt-friendly flags).
- `tried` — one entry per whole-file candidate the conductor actually ran
  (seat order), plus one entry for the routed candidate
  (`mode=24, param=block_size`) whenever routing was attempted
  (`nblocks >= 2`). Failed/declined candidates are recorded with
  `bytes = (size_t)-1` so the next loop skips them too.
- `incumbent_*` — the true minimum by actual total bytes over everything
  the conductor ran.

P1 does not implement L3. The struct and its serialization exist now so
P2/P3 plug into them instead of retrofitting.

## Text serialization

`wb_handoff_write()` emits NUL-terminated text; `wb_handoff_read()`
parses it back (fail-closed: any malformed input returns -1, never a
partial handoff). The format is line-oriented `key=value`, versioned by
the magic first line, so a future cold-loop stage — possibly a separate
process — can inherit the handoff without linking the workbench.

```
# sicknode handoff v1
incumbent bytes=280380 mode=12 param=0
scan size=1048576 entropy=4.5020 h1=3.4126 alpha=0.3086 e8e9_per_mb=0.00 period=0 period_conf=0.0000 smooth8=0 smooth16=0 smooth24=0 smooth32=0 is_tar=0 homogeneous=1 bwt_friendly=1
tried mode=12 param=0 bytes=280380
tried mode=22 param=0 bytes=312004
tried mode=23 param=0 bytes=301222
tried mode=20 param=0 bytes=290111
tried mode=24 param=262144 bytes=281102
routed built=1 bytes=281102 block_size=262144 nblocks=4
```

- `incumbent` — the bar. `mode` is the WB mode id (`12` = raw sentinel,
  `13`–`23` = transforms, `24` = routed).
- `scan` — the full scan report, same fields as the `wbscan` line.
- `tried` — one line per candidate the conductor ran, in seat order;
  `bytes=X` marks a candidate that failed or declined (still skipped by
  the next loop — it was tried).
- `routed` — whether the routed candidate was built, its bytes (`X` when
  the routed run failed), and the block geometry it used.

## CLI export

`npcc wbc` writes the handoff text to the path in `NPCC_WB_HANDOFF` when
the variable is set (opt-in; nothing is written otherwise):

```
NPCC_WB_HANDOFF=/tmp/run.handoff npcc wbc IN OUT
```

## How a future L3 uses it

1. Read the handoff (`wb_handoff_read` or parse the text).
2. Seed its best-known with `incumbent_bytes` / `incumbent_mode`.
3. Build its candidate queue **excluding** every `(mode, param)` in
   `tried` — including the `X` rows.
4. Explore only untried transforms/params; any new full run that beats
   `incumbent_bytes` becomes the new incumbent.
5. When L3 finishes, it emits an updated handoff (new tried rows
   appended, incumbent refreshed) for whatever comes next.

## See also

- `ROUTING.md` — the hot loop, the routed frame, and the audit dump the
  bench worker uses to validate routing predictions.
- `MIXER_LOG.tsv` — the append-only Tier-3 training signal (scan features
  → winning transform pairs), written by every `wbc` run.
