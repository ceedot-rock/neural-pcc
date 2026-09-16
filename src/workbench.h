#ifndef WORKBENCH_H
#define WORKBENCH_H
#include <stddef.h>
#include <stdint.h>
#define WB_MODE_RAW      12
#define WB_MODE_COLUMNAR 13
#define WB_MODE_DELTA8   14
#define WB_MODE_DELTA16  15
#define WB_MODE_DELTA24  16
#define WB_MODE_DELTA32  17
#define WB_MODE_XOR16    18
#define WB_MODE_XOR32    19
#define WB_MODE_EXE      20
#define WB_MODE_IMG2D    21
#define WB_MODE_BITPLANE 22
#define WB_MODE_SHUFFLE  23
#define WB_MODE_ROUTED   24   /* hot-loop routed per-block frame (spec v2) */
#define WB_HOT_BLOCK (256u * 1024u)  /* default hot-loop block size (spec v2) */
typedef struct {
    double entropy;      /* H0 byte entropy */
    double h1;           /* first-order conditional entropy H(Xn|Xn-1), sampled */
    double alpha_util;   /* alphabet utilization: distinct bytes / 256 */
    double e8e9_per_mb;
    size_t record_period;   /* 0 = none discovered */
    double period_conf;     /* columnar gain at record_period (0..1) */
    int smooth8;           /* w=8-bit subtract-delta lowers byte entropy >5% */
    int smooth16;          /* u16 subtract-delta lowers byte entropy >5% */
    int smooth24;          /* u24 subtract-delta lowers byte entropy >5% */
    int smooth32;
    int is_tar;
    int homogeneous;     /* low block-to-block entropy variance (256KB windows) */
    int bwt_friendly;    /* BWT beats LZ on a 128KB sample */
    size_t size;
} wb_scan_t;
int wb_scan(const uint8_t *in, size_t n, wb_scan_t *rep);
typedef struct { int mode; const char *name; uint64_t param; } wb_candidate_t;
int wb_bay1(const wb_scan_t *rep, wb_candidate_t *cands, int full);
int wb_apply(int mode, uint64_t param, const uint8_t *in, size_t n,
             uint8_t **tp, size_t *tn, uint8_t **mp, size_t *mn);
int wb_invert(int mode, uint64_t param, const uint8_t *t, size_t tn,
              const uint8_t *meta, size_t mn, uint8_t *out, size_t n);
int wb_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on, int full);
int wb_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on);

/* ---- Bay-0 mixer (Tier-1 hand-tuned scoring, spec v3) ----
 * Scores outer transforms AND inner modes. ORDERING IS SCHEDULING ONLY:
 * it decides what the conductor tries first and what the hot loop probes
 * first; it NEVER eliminates a candidate and NEVER decides the winner. */
typedef struct {
    int is_inner;      /* 0 = outer WB transform, 1 = inner mode */
    int mode;          /* WB mode id (12..23) or inner mode id (0/1/3/5/6/7/8) */
    uint64_t param;    /* outer transform param (columnar period) */
    double score;      /* [0,100]; higher = earlier seat / earlier probe */
    const char *name;
} wb_mixer_score_t;
#define WB_MIXER_MAX 20
int wb_mixer_scores(const wb_scan_t *rep, wb_mixer_score_t *out);

/* ---- Hot loop: per-block routing (spec v2) ----
 * block_size is a parameter (P1 default 256KB); never hardcoded in the
 * routing/encode path. */
typedef struct {
    int mode;               /* probe-winning transform (WB_MODE_RAW..23) */
    uint64_t param;
    size_t probe_bytes;     /* winner's deciding probe bytes */
    int ncands;
    int cand_modes[12];
    size_t cand_bytes[12];  /* deciding probe bytes; (size_t)-1 = declined/failed */
    int stage2_tiebreak;    /* nonzero when the stage-2 tie-break ran */
    int stage2_mode;        /* inner mode used for the tie-break (mixer-chosen) */
    double entropy, h1;     /* per-block light features (Tier-3 logging) */
    double alpha_util;
    double e8e9_per_mb;
    int smooth8, smooth16, smooth24, smooth32;
} wb_block_route_t;
int wb_route(const uint8_t *in, size_t n, size_t block_size,
             const wb_scan_t *rep,
             wb_block_route_t **routes_out, size_t *nblocks_out);
/* Audit dump for the bench worker (format documented in ROUTING.md).
 * Returns malloc'd NUL-terminated text; caller frees. */
char *wb_route_audit(const char *tag, size_t block_size,
                     const wb_block_route_t *routes, size_t nblocks);
int wb_encode_routed(const uint8_t *in, size_t n, size_t block_size,
                     const wb_block_route_t *routes, size_t nblocks,
                     uint8_t **out, size_t *on);
size_t wb_hot_block_size(void);  /* NPCC_WB_BLOCKKB env, default 256KB */

/* ---- Tier-3 mixer logging (spec v3): append-only, no learning ----
 * One TSV row per call; creates the file with a header row if missing.
 * The log path is NPCC_MIXER_LOG or ~/workspace/tnssrc-workbench/MIXER_LOG.tsv. */
/* ---- Escalation handoff: L1 -> L2 -> L3 (spec v4) ----
 *
 * STANDING INVARIANT: any loop escalation (L1->L2, L2->L3) passes the
 * handoff forward. The cold loop inherits the incumbent best byte count
 * (the bar to beat), the full scan report, and the list of already-tried
 * candidates with their byte counts. It starts informed: a "stop and
 * turn" that discards this state at the handoff is a SPEC VIOLATION,
 * not a simplification.
 *
 * The receiving loop MUST:
 *   (1) skip every candidate in the tried table (no redoing work), and
 *   (2) use incumbent_bytes as its bar (only a strictly better total
 *       displaces the incumbent).
 *
 * P1 (this build) populates the handoff in the conductor on every run;
 * L3 is not implemented yet, but the struct + text serialization exist
 * now so P2/P3 plug into them instead of retrofitting. Format documented
 * in HANDOFF.md. */
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
/* Text serialization (HANDOFF.md). wb_handoff_write returns malloc'd
 * NUL-terminated text; caller frees. wb_handoff_read parses it back. */
int wb_handoff_write(const wb_handoff_t *h, char **out);
int wb_handoff_read(const char *s, wb_handoff_t *h);

/* Rich result for wb_encode_full: everything the CLI needs for
 * machine-parseable output and Tier-3 logging, without re-running the
 * scan, the shortlist, or the routing. */
typedef struct {
    uint8_t *out;
    size_t on;
    int winner_mode;            /* WB_MODE_RAW / 13..23 / WB_MODE_ROUTED */
    wb_scan_t rep;              /* whole-file scan report */
    wb_candidate_t seats[13];
    int nseats;
    size_t whole_best_bytes;
    int whole_best_mode;
    int routed_built;
    size_t routed_bytes;
    wb_block_route_t *routes;   /* nblocks entries, NULL unless routed_built */
    size_t nblocks;
    size_t block_size;
    size_t *routed_block_bytes; /* per-block framed lengths, NULL unless built */
    wb_handoff_t handoff;       /* escalation handoff, populated every run */
} wb_result_t;
int wb_encode_full(const uint8_t *in, size_t n, int full, wb_result_t *res);
void wb_result_free(wb_result_t *res);

const char *wb_mixer_log_path(void);
int wb_mixer_log_row(const char *file, const char *block, size_t size,
                     double entropy, double h1, double alpha_util,
                     double e8e9_per_mb, size_t period, double period_conf,
                     int smooth8, int smooth16, int smooth24, int smooth32,
                     int is_tar, int homogeneous, int bwt_friendly,
                     const char *seats, const char *winner, size_t packed_bytes);
#endif
