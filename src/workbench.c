/* SickNode: structure scan + generalized transform battery + Bay-1 conductor.
 *
 * Bay 1 of the transform workbench for the TNSSRC lossless compressor.
 * wb_scan() cheaply characterizes the input (byte entropy, E8/E9 site
 * density, record-period discovery, u16/u32 delta smoothness, tar sniff).
 * wb_bay1() predicts the top-3 transform candidates from that report using
 * the documented structural scoring below. wb_encode() runs every boarding
 * candidate for real through the proven tnssrc_encode_inner() exact-minimum
 * selector and keeps the true minimum by bytes; in full-battery mode no
 * boarding path is ever skipped by heuristic.
 *
 * Outer frame: [mode u8][meta_len u32 LE][meta][inner blob],
 * meta[0..8) = transformed length (tn) u64 LE.
 *
 * The RAW candidate is the bare tnssrc_encode_inner() output, byte-identical
 * to selector-only output. WB_MODE_RAW (12) is only the candidate-list
 * sentinel; it never appears as a frame mode byte on the wire.
 *
 * All transforms are deterministic and exactly invertible. Proprietary. */
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv for the inner probe path */
#include "workbench.h"
#include <time.h>
#include "tnssrc.h"
#include "frontend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- little-endian helpers ---------------- */
static uint32_t wb_rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t wb_rd64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void wb_wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wb_wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static const char *wb_mode_name(int mode) {
    switch (mode) {
    case WB_MODE_RAW: return "raw";
    case WB_MODE_COLUMNAR: return "columnar";
    case WB_MODE_DELTA8: return "delta8";
    case WB_MODE_DELTA16: return "delta16";
    case WB_MODE_DELTA24: return "delta24";
    case WB_MODE_DELTA32: return "delta32";
    case WB_MODE_XOR16: return "xor16";
    case WB_MODE_XOR32: return "xor32";
    case WB_MODE_EXE: return "exe";
    case WB_MODE_IMG2D: return "img2d";
    case WB_MODE_BITPLANE: return "bitplane";
    case WB_MODE_SHUFFLE: return "shuffle";
    default: return "?";
    }
}

/* ================= wb_scan ================= */

static double wb_hist_entropy(const uint32_t h[256], size_t total) {
    if (!total) return 0.0;
    double e = 0.0;
    for (int i = 0; i < 256; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * (log(p) / log(2.0));
    }
    return e;
}

/* Byte entropy over a strided sample of at most 1MiB. */
static double wb_sample_entropy(const uint8_t *in, size_t n) {
    uint32_t h[256] = {0};
    size_t want = n < 1048576 ? n : 1048576;
    size_t step = n / want;
    if (!step) step = 1;
    size_t total = 0;
    for (size_t i = 0; i < n; i += step) {
        h[in[i]]++;
        total++;
    }
    return wb_hist_entropy(h, total);
}

/* E8/E9 site density per MiB, using the same +/-16MiB rel32 plausibility
 * filter as fe_exe_apply so the count predicts EXE boarding. */
static double wb_e8e9_density(const uint8_t *in, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i + 5 <= n; i++) {
        uint8_t b = in[i];
        if (b == 0xE8 || b == 0xE9) {
            int32_t rel = (int32_t)wb_rd32le(in + i + 1);
            if (rel >= -16777216 && rel < 16777216) c++;
        }
    }
    return n ? (double)c / ((double)n / 1048576.0) : 0.0;
}

/* Record-period discovery via column-entropy gain.
 *
 * Byte-match autocorrelation is blind to real record structure (measured:
 * real sao scores 0.065 at P=28, pure noise level) because record lanes
 * carry varying data. The direct signature of record structure is that
 * splitting the stream into P columns lowers byte entropy:
 *   gain(P) = (H_global - mean_column_H) / H_global   (0..1)
 * measured on a 256KB contiguous, phase-preserving sample (sao: 0.265 at
 * P=28 vs ~0.08 at neighboring periods).
 *
 * We sweep P=2..128 by gain, plus the top byte-autocorrelation peaks above
 * 128 (deduped against smaller divisors), and take the smallest P within
 * 95% of the max gain. Accepted when max gain >= 0.12. period_conf = gain,
 * which is the "autocorrelation peak strength" the Bay-0 mixer scores
 * COLUMNAR against. Deterministic. */
static double wb_gain_at(const uint8_t *s, size_t sn, double gH, size_t P,
                         uint32_t *cnt, uint32_t *ctot) {
    memset(cnt, 0, P * 256 * sizeof(uint32_t));
    memset(ctot, 0, P * sizeof(uint32_t));
    for (size_t c = 0; c < P; c++) {
        uint32_t *h = cnt + c * 256;
        uint32_t t = 0;
        for (size_t i = c; i < sn; i += P) {
            h[s[i]]++;
            t++;
        }
        ctot[c] = t;
    }
    double colH = 0.0;
    for (size_t c = 0; c < P; c++) {
        if (!ctot[c]) continue;
        double e = 0.0;
        int nd = 0;
        uint32_t *h = cnt + c * 256;
        for (int b = 0; b < 256; b++) {
            if (!h[b]) continue;
            nd++;
            double p = (double)h[b] / (double)ctot[c];
            e -= p * (log(p) / log(2.0));
        }
        /* Miller-Madow bias correction: entropy estimated on few samples
         * is biased low, which would inflate the gain of large P (short
         * columns). Correct per column. */
        if (nd > 1) e += (double)(nd - 1) / (2.0 * (double)ctot[c] * log(2.0));
        colH += e;
    }
    colH /= (double)P;
    return (gH - colH) / gH;
}

/* byte-match autocorrelation ratios, used only to propose large-P
 * candidates for the gain test */
static void wb_autocorr_ratios(const uint8_t *in, size_t n, double *ratios) {
    for (size_t P = 2; P <= 4096; P++) ratios[P] = 0.0;
    if (n <= 8192) return;
    const size_t K = 8192;
    size_t step = (n - 4096) / K;
    if (!step) step = 1;
    size_t ntrials = 0;
    for (size_t p = 4096; p < n; p += step) ntrials++;
    if (ntrials < 256) return;
    for (size_t P = 2; P <= 4096; P++) {
        size_t m = 0;
        for (size_t p = 4096; p < n; p += step) m += (in[p] == in[p - P]);
        ratios[P] = (double)m / (double)ntrials;
    }
}

static void wb_discover_period(const uint8_t *in, size_t n, size_t *pp, double *cp) {
    *pp = 0;
    *cp = 0.0;
    if (n < 512) return;
    size_t sn = n < 262144 ? n : 262144;
    const uint8_t *s = in;
    uint32_t gh[256] = {0};
    for (size_t i = 0; i < sn; i++) gh[s[i]]++;
    double gH = wb_hist_entropy(gh, sn);
    if (gH < 0.5) return;

    static double pg[4097];
    for (size_t P = 2; P <= 4096; P++) pg[P] = -1.0;
    static uint32_t cnt[128 * 256];
    static uint32_t ctot[128];
    double best = 0.0;
    size_t bestp = 0;
    for (size_t P = 2; P <= 128; P++) {
        double g = wb_gain_at(s, sn, gH, P, cnt, ctot);
        pg[P] = g;
        if (g > best) {
            best = g;
            bestp = P;
        }
    }
    /* large-P candidates from autocorrelation peaks */
    static double ratios[4097];
    wb_autocorr_ratios(in, n, ratios);
    for (int k = 0; k < 6; k++) {
        size_t bp = 0;
        double br = 0.0;
        for (size_t P = 129; P <= 4096; P++) {
            if (pg[P] >= 0.0) continue; /* already evaluated */
            if (ratios[P] <= br) continue;
            int dup = 0;
            for (size_t Q = 2; Q * Q <= P && !dup; Q++) {
                if (P % Q == 0 && pg[Q] >= 0.0 && ratios[Q] >= 0.9 * ratios[P])
                    dup = 1;
            }
            if (!dup) {
                br = ratios[P];
                bp = P;
            }
        }
        if (!bp || br < 0.05) break;
        uint32_t *lcnt = calloc(bp * 256, sizeof(uint32_t));
        uint32_t *lctot = calloc(bp, sizeof(uint32_t));
        if (!lcnt || !lctot) {
            free(lcnt);
            free(lctot);
            break;
        }
        double g = wb_gain_at(s, sn, gH, bp, lcnt, lctot);
        free(lcnt);
        free(lctot);
        pg[bp] = g;
        /* large-P candidates must CLEARLY beat the small-period hypothesis:
         * fine column splits are otherwise always favored by sub-lane
         * purity (e.g. sao scores higher at 448=16x28 than at its true
         * 28-byte record period). */
        if (g > best + 0.05) {
            best = g;
            bestp = bp;
        }
    }
    if (best < 0.12) return;
    size_t pick = bestp;
    if (bestp <= 128) {
        /* smallest small-P period within 95% of the best gain */
        for (size_t P = 2; P <= 128; P++) {
            if (pg[P] >= 0.95 * best) {
                pick = P;
                break;
            }
        }
    }
    *pp = pick;
    *cp = best;
}


/* u16/u32/u8/u24 subtract-delta smoothness: byte entropy of the wrapping
 * subtract-delta word stream vs the raw bytes, over up to 4 contiguous
 * windows of 16384 ADJACENT words (deltas are only meaningful between
 * neighbors). Smooth when delta entropy drops >5% and raw entropy is
 * non-degenerate (>0.5). */
static int wb_delta_smooth(const uint8_t *in, size_t n, int wbytes) {
    const size_t WORDS = 16384;
    size_t winbytes = WORDS * (size_t)wbytes;
    if (n < winbytes * 2) return 0;
    int nwin = n < winbytes * 4 ? 2 : 4;
    uint32_t hraw[256] = {0}, hdel[256] = {0};
    size_t nraw = 0, ndel = 0;
    for (int w = 0; w < nwin; w++) {
        size_t start = (n - winbytes) * (size_t)w / (size_t)(nwin - 1);
        start -= start % (size_t)wbytes;
        uint64_t prev = 0;
        for (size_t i = 0; i < WORDS; i++) {
            size_t idx = start + i * (size_t)wbytes;
            uint64_t v = 0;
            for (int b = 0; b < wbytes; b++) {
                uint8_t x = in[idx + (size_t)b];
                hraw[x]++;
                nraw++;
                v |= (uint64_t)x << (8 * b);
            }
            if (i) {
                uint64_t d = v - prev; /* wrapping subtract */
                for (int b = 0; b < wbytes; b++) {
                    hdel[(uint8_t)(d >> (8 * b))]++;
                    ndel++;
                }
            }
            prev = v;
        }
    }
    if (!ndel) return 0;
    double h0 = wb_hist_entropy(hraw, nraw);
    double h1 = wb_hist_entropy(hdel, ndel);
    return (h0 > 0.5 && h1 < 0.95 * h0);
}

/* tar sniff: "ustar" magic at offset 257 with a valid version field
 * ("\0\0" or " \0" at 262) in any of the first 32 512-byte blocks. */
static int wb_sniff_tar(const uint8_t *in, size_t n) {
    size_t blocks = n / 512;
    if (blocks > 32) blocks = 32;
    for (size_t b = 0; b < blocks; b++) {
        const uint8_t *h = in + b * 512;
        if (memcmp(h + 257, "ustar", 5) != 0) continue;
        if ((h[262] == 0 && h[263] == 0) || (h[262] == ' ' && h[263] == 0))
            return 1;
    }
    return 0;
}

/* H1: first-order conditional entropy H(Xn|Xn-1) over sampled ADJACENT
 * pairs (pair starts strided, but each pair is two neighboring bytes). */
static double wb_h1_sample(const uint8_t *in, size_t n) {
    if (n < 4096) return 0.0;
    const size_t NP = 131072;
    size_t step = n / NP;
    if (!step) step = 1;
    uint32_t *joint = calloc(65536, sizeof(uint32_t));
    uint32_t marg[256] = {0};
    if (!joint) return 0.0;
    size_t np = 0;
    for (size_t i = 0; i + 1 < n && np < NP; i += step) {
        joint[((size_t)in[i] << 8) | (size_t)in[i + 1]]++;
        marg[in[i]]++;
        np++;
    }
    double hj = 0.0, hm = 0.0;
    for (int k = 0; k < 65536; k++) {
        if (!joint[k]) continue;
        double p = (double)joint[k] / (double)np;
        hj -= p * (log(p) / log(2.0));
    }
    for (int k = 0; k < 256; k++) {
        if (!marg[k]) continue;
        double p = (double)marg[k] / (double)np;
        hm -= p * (log(p) / log(2.0));
    }
    free(joint);
    return hj - hm;
}

/* alphabet utilization: distinct bytes / 256 over a 64KB sample */
static double wb_alpha_util(const uint8_t *in, size_t n) {
    uint8_t seen[256] = {0};
    size_t sn = n < 65536 ? n : 65536;
    size_t step = n / sn;
    if (!step) step = 1;
    int nd = 0;
    for (size_t i = 0, c = 0; i < n && c < sn; i += step, c++) {
        if (!seen[in[i]]) {
            seen[in[i]] = 1;
            nd++;
        }
    }
    return (double)nd / 256.0;
}

/* homogeneous: low block-to-block entropy variance across 256KB windows */
static int wb_homogeneous(const uint8_t *in, size_t n) {
    const size_t W = 262144;
    if (n < 2 * W) return 1;
    size_t nw = n / W;
    if (nw > 16) nw = 16;
    double mn = 1e300, mx = -1e300;
    for (size_t w = 0; w < nw; w++) {
        size_t start = (n - W) * w / (nw - 1);
        uint32_t h[256] = {0};
        size_t sn = 32768, step = W / sn;
        for (size_t i = 0; i < W; i += step) h[in[start + i]]++;
        double e = wb_hist_entropy(h, W / step);
        if (e < mn) mn = e;
        if (e > mx) mx = e;
    }
    return (mx - mn) < 0.75;
}

/* single-candidate inner probe via the NPCC_FORCE_MODE mechanism, with
 * env save/restore so it composes with any ambient setting. Returns
 * packed bytes, or (size_t)-1 on failure. */
static size_t wb_probe_inner(const uint8_t *in, size_t n, int mode) {
    char oldb[64];
    int had = 0;
    const char *oe = getenv("NPCC_FORCE_MODE");
    if (oe) {
        had = 1;
        snprintf(oldb, sizeof oldb, "%s", oe);
    }
    char ms[16];
    snprintf(ms, sizeof ms, "%d", mode);
    setenv("NPCC_FORCE_MODE", ms, 1);
    uint8_t *o = NULL;
    size_t on = 0;
    int rc = tnssrc_encode_inner(in, n, &o, &on);
    free(o);
    if (had) setenv("NPCC_FORCE_MODE", oldb, 1);
    else unsetenv("NPCC_FORCE_MODE");
    return rc ? (size_t)-1 : on;
}

/* bwt_friendly: BWT (inner mode 1) beats LZ (inner mode 0) on a 128KB
 * sample. A cheap structural signal, not a decision. */
static int wb_bwt_friendly(const uint8_t *in, size_t n) {
    size_t sn = n < 131072 ? n : 131072;
    size_t b = wb_probe_inner(in, sn, 1);
    size_t l = wb_probe_inner(in, sn, 0);
    if (b == (size_t)-1 || l == (size_t)-1) return 0;
    return b < l;
}

int wb_scan(const uint8_t *in, size_t n, wb_scan_t *rep) {
    if (!rep) return -1;
    memset(rep, 0, sizeof *rep);
    rep->size = n;
    if (!n || !in) return 0;
    rep->entropy = wb_sample_entropy(in, n);
    rep->e8e9_per_mb = wb_e8e9_density(in, n);
    wb_discover_period(in, n, &rep->record_period, &rep->period_conf);
    rep->h1 = wb_h1_sample(in, n);
    rep->alpha_util = wb_alpha_util(in, n);
    rep->smooth8 = wb_delta_smooth(in, n, 1);
    rep->smooth16 = wb_delta_smooth(in, n, 2);
    rep->smooth24 = wb_delta_smooth(in, n, 3);
    rep->smooth32 = wb_delta_smooth(in, n, 4);
    rep->is_tar = wb_sniff_tar(in, n);
    rep->homogeneous = wb_homogeneous(in, n);
    rep->bwt_friendly = wb_bwt_friendly(in, n);
    return 0;
}

/* ================= transforms =================
 * Every apply() returns 0 with *tp = transformed bytes (tn), *mp = side
 * metadata (mn, meta[0..8) = tn u64 LE); nonzero = decline. Every invert()
 * reconstructs out[0..n) exactly; nonzero on any format error. Caller frees
 * tp/mp. All deterministic. */

/* RAW: identity copy (candidate-list sentinel; wb_encode uses the bare
 * inner output for RAW instead of this frame). */
static int wb_raw_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                        uint8_t **mp, size_t *mn) {
    uint8_t *t = malloc(n ? n : 1);
    uint8_t *m = malloc(8);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    memcpy(t, in, n);
    wb_wr64le(m, (uint64_t)n);
    *tp = t;
    *tn = n;
    *mp = m;
    *mn = 8;
    return 0;
}
static int wb_raw_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                         uint8_t *out, size_t n) {
    if (mn < 8 || wb_rd64le(meta) != tn || tn != n) return -1;
    memcpy(out, t, n);
    return 0;
}

/* COLUMNAR: period P from scan (param). Columns concatenated; trailing
 * partial-record bytes appended raw. meta = [tn u64][P u64]. */
static int wb_columnar_apply(uint64_t param, const uint8_t *in, size_t n,
                             uint8_t **tp, size_t *tn, uint8_t **mp, size_t *mn) {
    if (param < 2 || param > (uint64_t)n) return -1;
    size_t P = (size_t)param;
    size_t nr = n / P, rem = n % P;
    uint8_t *t = malloc(n ? n : 1);
    uint8_t *m = malloc(16);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    for (size_t c = 0; c < P; c++)
        for (size_t r = 0; r < nr; r++) t[c * nr + r] = in[r * P + c];
    memcpy(t + P * nr, in + nr * P, rem);
    wb_wr64le(m, (uint64_t)n);
    wb_wr64le(m + 8, param);
    *tp = t;
    *tn = n;
    *mp = m;
    *mn = 16;
    return 0;
}
static int wb_columnar_invert(uint64_t param, const uint8_t *t, size_t tn,
                              const uint8_t *meta, size_t mn, uint8_t *out, size_t n) {
    if (mn < 16 || wb_rd64le(meta) != tn || tn != n) return -1;
    if (wb_rd64le(meta + 8) != param) return -1;
    if (param < 2 || param > (uint64_t)n) return -1;
    size_t P = (size_t)param;
    size_t nr = n / P, rem = n % P;
    for (size_t c = 0; c < P; c++)
        for (size_t r = 0; r < nr; r++) out[r * P + c] = t[c * nr + r];
    memcpy(out + nr * P, t + P * nr, rem);
    return 0;
}

/* DELTA8/16/24/32: out[i] = in[i]-in[i-w] (wrapping), first w bytes raw.
 * XOR16/32: out[i] = in[i]^in[i-w], first w raw. meta = [tn u64]. */
static int wb_delta_apply(int w, int is_xor, const uint8_t *in, size_t n,
                          uint8_t **tp, size_t *tn, uint8_t **mp, size_t *mn) {
    if (w < 1 || w > 4 || !n) return -1;
    uint8_t *t = malloc(n);
    uint8_t *m = malloc(8);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    size_t k = (size_t)w < n ? (size_t)w : n;
    memcpy(t, in, k);
    for (size_t i = k; i < n; i++)
        t[i] = is_xor ? (uint8_t)(in[i] ^ in[i - (size_t)w])
                      : (uint8_t)(in[i] - in[i - (size_t)w]);
    wb_wr64le(m, (uint64_t)n);
    *tp = t;
    *tn = n;
    *mp = m;
    *mn = 8;
    return 0;
}
static int wb_delta_invert(int w, int is_xor, const uint8_t *t, size_t tn,
                           const uint8_t *meta, size_t mn, uint8_t *out, size_t n) {
    if (w < 1 || w > 4 || mn < 8 || wb_rd64le(meta) != tn || tn != n || !n) return -1;
    size_t k = (size_t)w < n ? (size_t)w : n;
    memcpy(out, t, k);
    for (size_t i = k; i < n; i++)
        out[i] = is_xor ? (uint8_t)(t[i] ^ out[i - (size_t)w])
                        : (uint8_t)(t[i] + out[i - (size_t)w]);
    return 0;
}

/* EXE: E8/E9 rel32 -> absolute normalization; wraps the proven fe_exe
 * pair from frontend.c (file-agnostic; keeps its own >=128-site and
 * >=64KiB structural gates). meta[0..8) = tn = n already. */
static int wb_exe_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                        uint8_t **mp, size_t *mn) {
    return fe_exe_apply(in, n, tp, tn, mp, mn);
}
static int wb_exe_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                         uint8_t *out, size_t n) {
    return fe_exe_invert(t, tn, meta, mn, out, n);
}

/* IMG2D: row width W (even bytes) discovered by min-entropy row-delta scan;
 * per row the best PNG-style filter {None,Sub,Up,Avg,Paeth} wins by min
 * sum-of-abs residuals scored on u16 samples (even byte positions).
 * Output: [filter u8][W filtered bytes] per row, trailing partial row raw.
 * meta = [tn u64][W u32]. Declines when no clear width is found. */
static int wb_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

#define WB_IMG2D_WMIN 4
#define WB_IMG2D_WMAX 2048
#define WB_IMG2D_MINN 65536

static size_t wb_img2d_find_width(const uint8_t *in, size_t n) {
    if (n < WB_IMG2D_MINN) return 0;
    const size_t K = 65536;
    size_t step = (n - WB_IMG2D_WMAX) / K;
    if (!step) step = 1;
    double best = 1e300, second = 1e300;
    size_t bestw = 0;
    for (size_t W = WB_IMG2D_WMIN; W <= WB_IMG2D_WMAX; W += 2) {
        uint64_t sum = 0;
        size_t cnt = 0;
        for (size_t i = WB_IMG2D_WMAX; i < n; i += step) {
            int d = (int)in[i] - (int)in[i - W];
            sum += (uint64_t)(d < 0 ? -d : d);
            cnt++;
        }
        if (!cnt) continue;
        double mean = (double)sum / (double)cnt;
        if (mean < best) {
            second = best;
            best = mean;
            bestw = W;
        } else if (mean < second) {
            second = mean;
        }
    }
    /* accept only a clear, genuinely predictive minimum */
    if (!bestw || best >= 24.0 || best > 0.9 * second) return 0;
    return bestw;
}

static int wb_img2d_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                          uint8_t **mp, size_t *mn) {
    size_t W = wb_img2d_find_width(in, n);
    if (!W) return -1;
    size_t rows = n / W, rem = n % W;
    size_t outn = rows * (W + 1) + rem;
    uint8_t *t = malloc(outn ? outn : 1);
    uint8_t *m = malloc(12);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    size_t o = 0;
    for (size_t r = 0; r < rows; r++) {
        const uint8_t *row = in + r * W;
        const uint8_t *up = r ? in + (r - 1) * W : NULL;
        int bestf = 0;
        uint64_t bestscore = (uint64_t)-1;
        for (int f = 0; f < 5; f++) {
            uint64_t score = 0;
            for (size_t j = 0; j < W; j += 2) { /* u16 samples */
                int b = row[j];
                int left = j ? row[j - 1] : 0;
                int u = up ? up[j] : 0;
                int ul = (up && j) ? up[j - 1] : 0;
                int pred = f == 0 ? 0 : f == 1 ? left
                             : f == 2         ? u
                             : f == 3         ? (left + u) >> 1
                                              : wb_paeth(left, u, ul);
                int s = (b - pred) & 0xFF;
                if (s >= 128) s -= 256;
                score += (uint64_t)(s < 0 ? -s : s);
            }
            if (score < bestscore) {
                bestscore = score;
                bestf = f;
            }
        }
        t[o++] = (uint8_t)bestf;
        for (size_t j = 0; j < W; j++) {
            int b = row[j];
            int left = j ? row[j - 1] : 0;
            int u = up ? up[j] : 0;
            int ul = (up && j) ? up[j - 1] : 0;
            int pred = bestf == 0 ? 0 : bestf == 1 ? left
                          : bestf == 2            ? u
                          : bestf == 3            ? (left + u) >> 1
                                                  : wb_paeth(left, u, ul);
            t[o++] = (uint8_t)(b - pred);
        }
    }
    memcpy(t + o, in + rows * W, rem);
    o += rem;
    wb_wr64le(m, (uint64_t)outn);
    wb_wr32le(m + 8, (uint32_t)W);
    *tp = t;
    *tn = outn;
    *mp = m;
    *mn = 12;
    return 0;
}
static int wb_img2d_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                           uint8_t *out, size_t n) {
    if (mn < 12 || wb_rd64le(meta) != tn) return -1;
    uint32_t W32 = wb_rd32le(meta + 8);
    if (W32 < WB_IMG2D_WMIN || W32 > WB_IMG2D_WMAX || (W32 & 1)) return -1;
    size_t W = W32;
    size_t rows = n / W, rem = n % W;
    if (tn != rows * (W + 1) + rem) return -1;
    size_t o = 0;
    for (size_t r = 0; r < rows; r++) {
        if (o + 1 + W > tn) return -1;
        int f = t[o++];
        if (f < 0 || f > 4) return -1;
        uint8_t *row = out + r * W;
        const uint8_t *up = r ? out + (r - 1) * W : NULL;
        for (size_t j = 0; j < W; j++) {
            int left = j ? row[j - 1] : 0;
            int u = up ? up[j] : 0;
            int ul = (up && j) ? up[j - 1] : 0;
            int pred = f == 0 ? 0 : f == 1 ? left
                         : f == 2         ? u
                         : f == 3         ? (left + u) >> 1
                                          : wb_paeth(left, u, ul);
            row[j] = (uint8_t)(t[o++] + pred);
        }
    }
    if (o + rem != tn) return -1;
    memcpy(out + rows * W, t + o, rem);
    return 0;
}

/* BITPLANE: 8 planes, MSB-first; bits packed MSB-first. meta = [tn u64]. */
static int wb_bitplane_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                             uint8_t **mp, size_t *mn) {
    if (!n) return -1;
    size_t pb = (n + 7) / 8;
    size_t outn = pb * 8;
    uint8_t *t = calloc(outn ? outn : 1, 1);
    uint8_t *m = malloc(8);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    for (size_t p = 0; p < 8; p++) {
        uint8_t *plane = t + p * pb;
        uint8_t bit = (uint8_t)(0x80u >> p);
        for (size_t i = 0; i < n; i++)
            if (in[i] & bit) plane[i / 8] |= (uint8_t)(0x80u >> (i & 7));
    }
    wb_wr64le(m, (uint64_t)outn);
    *tp = t;
    *tn = outn;
    *mp = m;
    *mn = 8;
    return 0;
}
static int wb_bitplane_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                              uint8_t *out, size_t n) {
    if (!n || mn < 8 || wb_rd64le(meta) != tn) return -1;
    size_t pb = (n + 7) / 8;
    if (tn != pb * 8) return -1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        uint8_t m = (uint8_t)(0x80u >> (i & 7));
        for (size_t p = 0; p < 8; p++)
            if (t[p * pb + i / 8] & m) b |= (uint8_t)(0x80u >> p);
        out[i] = b;
    }
    return 0;
}

/* SHUFFLE: interleave stride s=4 when n%4==0 else s=2 when n even
 * (param is ignored; s is derived deterministically from n and stored in
 * meta). meta = [tn u64][s u64]. Declines on odd n. */
static int wb_shuffle_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                            uint8_t **mp, size_t *mn) {
    size_t s = (n % 4 == 0) ? 4 : ((n % 2 == 0 && n >= 2) ? 2 : 0);
    if (!s) return -1;
    size_t g = n / s;
    uint8_t *t = malloc(n);
    uint8_t *m = malloc(16);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    for (size_t k = 0; k < s; k++)
        for (size_t i = 0; i < g; i++) t[k * g + i] = in[i * s + k];
    wb_wr64le(m, (uint64_t)n);
    wb_wr64le(m + 8, (uint64_t)s);
    *tp = t;
    *tn = n;
    *mp = m;
    *mn = 16;
    return 0;
}
static int wb_shuffle_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                             uint8_t *out, size_t n) {
    if (mn < 16 || wb_rd64le(meta) != tn || tn != n) return -1;
    uint64_t s64 = wb_rd64le(meta + 8);
    if ((s64 != 4 && s64 != 2) || n % s64) return -1;
    size_t s = (size_t)s64, g = n / s;
    for (size_t k = 0; k < s; k++)
        for (size_t i = 0; i < g; i++) out[i * s + k] = t[k * g + i];
    return 0;
}

int wb_apply(int mode, uint64_t param, const uint8_t *in, size_t n,
             uint8_t **tp, size_t *tn, uint8_t **mp, size_t *mn) {
    if (!tp || !tn || !mp || !mn || !in) return -1;
    *tp = NULL;
    *tn = 0;
    *mp = NULL;
    *mn = 0;
    switch (mode) {
    case WB_MODE_RAW: return wb_raw_apply(in, n, tp, tn, mp, mn);
    case WB_MODE_COLUMNAR: return wb_columnar_apply(param, in, n, tp, tn, mp, mn);
    case WB_MODE_DELTA8: return wb_delta_apply(1, 0, in, n, tp, tn, mp, mn);
    case WB_MODE_DELTA16: return wb_delta_apply(2, 0, in, n, tp, tn, mp, mn);
    case WB_MODE_DELTA24: return wb_delta_apply(3, 0, in, n, tp, tn, mp, mn);
    case WB_MODE_DELTA32: return wb_delta_apply(4, 0, in, n, tp, tn, mp, mn);
    case WB_MODE_XOR16: return wb_delta_apply(2, 1, in, n, tp, tn, mp, mn);
    case WB_MODE_XOR32: return wb_delta_apply(4, 1, in, n, tp, tn, mp, mn);
    case WB_MODE_EXE: return wb_exe_apply(in, n, tp, tn, mp, mn);
    case WB_MODE_IMG2D: return wb_img2d_apply(in, n, tp, tn, mp, mn);
    case WB_MODE_BITPLANE: return wb_bitplane_apply(in, n, tp, tn, mp, mn);
    case WB_MODE_SHUFFLE: return wb_shuffle_apply(in, n, tp, tn, mp, mn);
    default: return -1;
    }
}

int wb_invert(int mode, uint64_t param, const uint8_t *t, size_t tn,
              const uint8_t *meta, size_t mn, uint8_t *out, size_t n) {
    if (!t || !meta || !out) return -1;
    switch (mode) {
    case WB_MODE_RAW: return wb_raw_invert(t, tn, meta, mn, out, n);
    case WB_MODE_COLUMNAR: return wb_columnar_invert(param, t, tn, meta, mn, out, n);
    case WB_MODE_DELTA8: return wb_delta_invert(1, 0, t, tn, meta, mn, out, n);
    case WB_MODE_DELTA16: return wb_delta_invert(2, 0, t, tn, meta, mn, out, n);
    case WB_MODE_DELTA24: return wb_delta_invert(3, 0, t, tn, meta, mn, out, n);
    case WB_MODE_DELTA32: return wb_delta_invert(4, 0, t, tn, meta, mn, out, n);
    case WB_MODE_XOR16: return wb_delta_invert(2, 1, t, tn, meta, mn, out, n);
    case WB_MODE_XOR32: return wb_delta_invert(4, 1, t, tn, meta, mn, out, n);
    case WB_MODE_EXE: return wb_exe_invert(t, tn, meta, mn, out, n);
    case WB_MODE_IMG2D: return wb_img2d_invert(t, tn, meta, mn, out, n);
    case WB_MODE_BITPLANE: return wb_bitplane_invert(t, tn, meta, mn, out, n);
    case WB_MODE_SHUFFLE: return wb_shuffle_invert(t, tn, meta, mn, out, n);
    default: return -1;
    }
}

/* ================= boarding + Bay 1 ================= */

/* Structural boarding prechecks for the full battery. A candidate only
 * runs when it structurally applies; DELTA8/BITPLANE always board (SHUFFLE
 * additionally needs even n, since an odd length cannot interleave).
 * The 64KiB floors keep tiny-input overhead from dominating; alignment
 * floors match each transform's stride. */
static int wb_boards_n(int mode, const wb_scan_t *rep, size_t n) {
    switch (mode) {
    case WB_MODE_RAW: return 1;
    case WB_MODE_COLUMNAR: return rep->record_period > 0;
    case WB_MODE_DELTA8: return 1;
    case WB_MODE_DELTA16:
    case WB_MODE_XOR16: return (n % 2 == 0) && n >= 65536;
    case WB_MODE_DELTA24: return (n % 3 == 0) && n >= 65536;
    case WB_MODE_DELTA32:
    case WB_MODE_XOR32: return (n % 4 == 0) && n >= 65536;
    case WB_MODE_EXE:
        return n >= 65536 && rep->e8e9_per_mb * (double)n / 1048576.0 >= 128.0;
    case WB_MODE_IMG2D: return rep->smooth16 && n >= 65536;
    case WB_MODE_BITPLANE: return 1;
    case WB_MODE_SHUFFLE: return (n % 2 == 0) && n >= 2;
    default: return 0;
    }
}

/* Boarding gates: a transform boards only when its structural
 * preconditions hold (even length for u16 transforms, enough E8/E9 sites
 * for exe, a discovered width for img2d, ...). wb_apply() can still
 * decline at apply time; the gates prune hopeless seats early. */
static int wb_boards(int mode, const wb_scan_t *rep) {
    return wb_boards_n(mode, rep, rep->size);
}

/* Bay-0 mixer: Tier-1 hand-tuned scoring (spec v3).
 *
 * The mixer scores outer battery transforms AND inner modes from scan
 * features. Scores are in [0,100]; higher = earlier seat / earlier probe.
 *
 *   Outer transforms (seat order):
 *     COLUMNAR : period>0 ? 30 + 70*period_conf : 0  (proportional to the
 *                columnar-gain peak strength at the discovered period)
 *     DELTA8   : 10 + 55*smooth8   (cheap universal decorrelator)
 *     DELTA16  :  5 + 70*smooth16
 *     DELTA24  :  5 + 60*smooth24
 *     DELTA32  :  5 + 70*smooth32
 *     XOR16    : 45*smooth16       (xor trails subtract-delta empirically)
 *     XOR32    : 45*smooth32
 *     EXE      : 90*min(1, e8e9_per_mb/5000)  (proportional to density)
 *     IMG2D    : smooth16 ? 75*(1 - h1/8) : 0 (2-D predictors want smooth
 *                16-bit streams with low conditional entropy)
 *     BITPLANE : entropy>=7.9 ? 10 : 20  (low baseline prior)
 *     SHUFFLE  : entropy>=7.9 ?  5 : 15  (low baseline prior)
 *
 *   Inner modes (hot-loop probe scheduling order):
 *     BWT (1)  : 58*(1-h1/8)*(1-alpha_util)*(1-exen)   (58 ~ 7/12 prior:
 *                BWT won 7/12 whole-file in the exact-selector run; it
 *                thrives on low conditional entropy and small alphabets,
 *                and degrades on exe-normalized code, hence (1-exe))
 *     LZM2 (7) : 42*(1-h1/16)   (42 ~ 5/12 prior; mild H1 preference)
 *     LZ (0)   : 30             (high baseline prior, flat)
 *     others   : 10             (still exact-run by the conductor; probe last)
 *   exen = min(1, e8e9_per_mb/30000).
 *
 * Mapping note (v3): BWT/LZM2/LZ are inner modes, not battery transforms,
 * so their scores never seat outer candidates. They are used for (a) the
 * whole-file candidate order only indirectly (outer scores seat first),
 * and (b) the hot loop's per-block fast-probe scheduling: the probe tries
 * inner modes in mixer-score order (P1: BWT first, LZM2 as a tie-break
 * probe when the top-2 BWT probes agree within 5%; see wb_route).
 *
 * ORDERING IS SCHEDULING ONLY. Under 2-vCPU it decides what runs first
 * while the rest queues; it NEVER eliminates a candidate and NEVER
 * decides the winner. The conductor's exact minimum over real runs is
 * untouched; in full-battery mode every boarding outer path runs.
 *
 * Returns the number of entries filled (<= WB_MIXER_MAX). */
static const char *wb_inner_name(int mode) {
    switch (mode) {
    case 0: return "lz";
    case 1: return "bwt";
    case 3: return "xz";
    case 5: return "colbwt";
    case 6: return "transbwt";
    case 7: return "lzm2";
    case 8: return "blocked";
    default: return "?";
    }
}

int wb_mixer_scores(const wb_scan_t *rep, wb_mixer_score_t *out) {
    if (!rep || !out) return -1;
    int k = 0;
    double exen = rep->e8e9_per_mb / 30000.0;
    if (exen > 1.0) exen = 1.0;
    double h1n = rep->h1 / 8.0;
    if (h1n < 0.0) h1n = 0.0;
    if (h1n > 1.0) h1n = 1.0;
    int hi_ent = rep->entropy >= 7.9;
#define MX(_inner, _mode, _param, _score)                                 \
    do {                                                                  \
        out[k].is_inner = (_inner);                                        \
        out[k].mode = (_mode);                                            \
        out[k].param = (_param);                                           \
        out[k].score = (_score);                                           \
        out[k].name = (_inner) ? wb_inner_name(_mode) : wb_mode_name(_mode); \
        k++;                                                              \
    } while (0)
    double col = 0.0;
    if (rep->record_period > 0) {
        col = 30.0 + 70.0 * rep->period_conf;
        if (col > 100.0) col = 100.0;
    }
    MX(0, WB_MODE_COLUMNAR, rep->record_period, col);
    MX(0, WB_MODE_DELTA8, 0, 10.0 + 55.0 * rep->smooth8);
    MX(0, WB_MODE_DELTA16, 0, 5.0 + 70.0 * rep->smooth16);
    MX(0, WB_MODE_DELTA24, 0, 5.0 + 60.0 * rep->smooth24);
    MX(0, WB_MODE_DELTA32, 0, 5.0 + 70.0 * rep->smooth32);
    MX(0, WB_MODE_XOR16, 0, 45.0 * rep->smooth16);
    MX(0, WB_MODE_XOR32, 0, 45.0 * rep->smooth32);
    MX(0, WB_MODE_EXE, 0, 90.0 * (rep->e8e9_per_mb / 5000.0 > 1.0 ? 1.0 : rep->e8e9_per_mb / 5000.0));
    MX(0, WB_MODE_IMG2D, 0, rep->smooth16 ? 75.0 * (1.0 - h1n) : 0.0);
    MX(0, WB_MODE_BITPLANE, 0, hi_ent ? 10.0 : 20.0);
    MX(0, WB_MODE_SHUFFLE, 0, hi_ent ? 5.0 : 15.0);
    double bwt = 58.0 * (1.0 - h1n) * (1.0 - rep->alpha_util) * (1.0 - exen);
    MX(1, 1, 0, bwt < 0.0 ? 0.0 : bwt);
    MX(1, 7, 0, 42.0 * (1.0 - h1n / 2.0));
    MX(1, 0, 0, 30.0);
    MX(1, 3, 0, 10.0);
    MX(1, 5, 0, 10.0);
    MX(1, 6, 0, 10.0);
    MX(1, 8, 0, 10.0);
#undef MX
    return k;
}

/* Bay-1 shortlist, now seated by the Bay-0 mixer: cands[0] is always RAW;
 * then the top-3 boarding outer transforms by mixer score (ties broken by
 * lower mode id, deterministic). When full != 0, every other boarding
 * candidate is appended in mode-id order, so full mode always runs the
 * complete battery. Scores only order the seats; the exact minimum over
 * real compression runs decides the winner.
 *
 * Returns the candidate count; cands must hold 13 entries. */
int wb_bay1(const wb_scan_t *rep, wb_candidate_t *cands, int full) {
    if (!rep || !cands) return -1;
    int nc = 0;
    cands[nc].mode = WB_MODE_RAW;
    cands[nc].name = wb_mode_name(WB_MODE_RAW);
    cands[nc].param = 0;
    nc++;

    wb_mixer_score_t ms[WB_MIXER_MAX];
    int nm = wb_mixer_scores(rep, ms);
    struct {
        int mode;
        double score;
        uint64_t param;
    } pool[12];
    int np = 0;
    for (int i = 0; i < nm; i++) {
        if (ms[i].is_inner) continue;
        int m = ms[i].mode;
        if (m == WB_MODE_RAW) continue;
        if (!wb_boards(m, rep)) continue;
        if (!full && ms[i].score <= 0.0) continue;
        pool[np].mode = m;
        pool[np].score = ms[i].score;
        pool[np].param = ms[i].param;
        np++;
    }
    /* top-3 by score, ties -> lower mode id */
    int picked[12] = {0};
    for (int k = 0; k < 3 && nc < 13; k++) {
        int bi = -1;
        for (int i = 0; i < np; i++) {
            if (picked[i]) continue;
            if (bi < 0 || pool[i].score > pool[bi].score ||
                (pool[i].score == pool[bi].score && pool[i].mode < pool[bi].mode))
                bi = i;
        }
        if (bi < 0) break;
        picked[bi] = 1;
        cands[nc].mode = pool[bi].mode;
        cands[nc].name = wb_mode_name(pool[bi].mode);
        cands[nc].param = pool[bi].param;
        nc++;
    }
    if (full) {
        /* append every other boarding candidate in mode-id order */
        for (int m = WB_MODE_COLUMNAR; m <= WB_MODE_SHUFFLE && nc < 13; m++) {
            if (!wb_boards(m, rep)) continue;
            int dup = 0;
            for (int i = 0; i < nc; i++)
                if (cands[i].mode == m) {
                    dup = 1;
                    break;
                }
            if (dup) continue;
            uint64_t param = (m == WB_MODE_COLUMNAR) ? rep->record_period : 0;
            cands[nc].mode = m;
            cands[nc].name = wb_mode_name(m);
            cands[nc].param = param;
            nc++;
        }
    }
    return nc;
}

/* ================= hot loop: per-block routing (spec v2) =================
 *
 * The hot loop routes; it does not judge. For each block it runs a cheap
 * deterministic probe per scan-narrowed candidate and records the predicted
 * transform. Only full exact runs (the conductor) determine winners.
 *
 * Probe design (engineering choice, measured): the probe encodes the
 * transformed block with inner mode BWT (1) only. Measured on the first
 * 256KB of mozilla: BWT packs in ~0.6s vs LZM2 (7) at ~2.2s, and the
 * transform choice dominates the ranking, so the 3.7x slower second mode
 * is reserved for tie-breaks. Probe scheduling follows the mixer inner
 * scores (v3b): BWT probes first; when the top-2 BWT probes agree within
 * 5% (and the block actually compressed and is >= 64KB), both finalists
 * are re-probed with LZM2 and the min wins. The audit dump records when
 * the tie-break fired so the bench worker can measure its value.
 *
 * Per-block candidate narrowing: raw + top-3 outer transforms by
 * block-local mixer scores (block entropy/H1/smoothness feed the mixer;
 * record period, tar flag, homogeneity are file-global). */

size_t wb_hot_block_size(void) {
    const char *e = getenv("NPCC_WB_BLOCKKB");
    long kb = e ? atol(e) : 256;
    if (kb < 4) kb = 4;
    if (kb > 16384) kb = 16384;
    return (size_t)kb * 1024u;
}

/* per-block light scan: block-local features for mixer narrowing and
 * Tier-3 logging. File-global structure (period/tar/homogeneity) is
 * inherited from the whole-file report. */
static void wb_block_features(const uint8_t *b, size_t bn,
                              const wb_scan_t *grep, wb_scan_t *brep) {
    memset(brep, 0, sizeof *brep);
    brep->size = bn;
    if (!bn || !b) return;
    brep->entropy = wb_sample_entropy(b, bn);
    brep->h1 = wb_h1_sample(b, bn);
    brep->alpha_util = wb_alpha_util(b, bn);
    size_t c = 0;
    for (size_t i = 0; i + 5 <= bn; i++) {
        uint8_t x = b[i];
        if (x == 0xE8 || x == 0xE9) {
            int32_t rel = (int32_t)wb_rd32le(b + i + 1);
            if (rel >= -16777216 && rel < 16777216) c++;
        }
    }
    brep->e8e9_per_mb = bn ? (double)c / ((double)bn / 1048576.0) : 0.0;
    brep->record_period = grep->record_period;
    brep->period_conf = grep->period_conf;
    brep->smooth8 = wb_delta_smooth(b, bn, 1);
    brep->smooth16 = wb_delta_smooth(b, bn, 2);
    brep->smooth24 = wb_delta_smooth(b, bn, 3);
    brep->smooth32 = wb_delta_smooth(b, bn, 4);
    brep->is_tar = grep->is_tar;
    brep->homogeneous = grep->homogeneous;
    brep->bwt_friendly = grep->bwt_friendly;
}

/* one probe: transform (unless raw) then inner encode with the given mode.
 * Returns packed bytes, or (size_t)-1 when the transform declines or the
 * inner encode fails. Deterministic. */
static size_t wb_probe_candidate_mode(int mode, uint64_t param,
                                      const uint8_t *blk, size_t bn, int inner) {
    const uint8_t *src = blk;
    size_t sn = bn;
    uint8_t *t = NULL, *meta = NULL;
    size_t tn = 0, mn = 0;
    if (mode != WB_MODE_RAW) {
        if (wb_apply(mode, param, blk, bn, &t, &tn, &meta, &mn))
            return (size_t)-1;
        src = t;
        sn = tn;
    }
    size_t pb = wb_probe_inner(src, sn, inner);
    free(t);
    free(meta);
    return pb;
}

int wb_route(const uint8_t *in, size_t n, size_t block_size,
             const wb_scan_t *rep,
             wb_block_route_t **routes_out, size_t *nblocks_out) {
    if (!in || !n || !rep || !routes_out || !nblocks_out) return -1;
    if (block_size < 4096) return -1;
    size_t nblocks = (n + block_size - 1) / block_size;
    if (!nblocks) return -1;
    wb_block_route_t *r = calloc(nblocks, sizeof *r);
    if (!r) return -1;
    for (size_t b = 0; b < nblocks; b++) {
        size_t off = b * block_size;
        size_t bn = n - off > block_size ? block_size : n - off;
        const uint8_t *blk = in + off;
        wb_scan_t brep;
        wb_block_features(blk, bn, rep, &brep);
        wb_block_route_t *rb = &r[b];
        rb->entropy = brep.entropy;
        rb->h1 = brep.h1;
        rb->alpha_util = brep.alpha_util;
        rb->e8e9_per_mb = brep.e8e9_per_mb;
        rb->smooth8 = brep.smooth8;
        rb->smooth16 = brep.smooth16;
        rb->smooth24 = brep.smooth24;
        rb->smooth32 = brep.smooth32;
        /* narrow: raw + top-3 boarding outer transforms by block-local
         * mixer score (ties -> lower mode id, deterministic) */
        wb_mixer_score_t ms[WB_MIXER_MAX];
        int nm = wb_mixer_scores(&brep, ms);
        int cm[4];
        uint64_t cp[4];
        int nc = 0;
        cm[nc] = WB_MODE_RAW;
        cp[nc] = 0;
        nc++;
        int picked[WB_MIXER_MAX] = {0};
        for (int k = 0; k < 3; k++) {
            int bi = -1;
            for (int i = 0; i < nm; i++) {
                if (picked[i] || ms[i].is_inner) continue;
                int m = ms[i].mode;
                if (m == WB_MODE_RAW || ms[i].score <= 0.0) continue;
                if (!wb_boards_n(m, &brep, bn)) continue;
                if (bi < 0 || ms[i].score > ms[bi].score ||
                    (ms[i].score == ms[bi].score && m < ms[bi].mode))
                    bi = i;
            }
            if (bi < 0) break;
            picked[bi] = 1;
            cm[nc] = ms[bi].mode;
            cp[nc] = ms[bi].param;
            nc++;
        }
        /* probe each candidate with BWT (mixer probe order: BWT first) */
        rb->ncands = nc;
        size_t best = (size_t)-1;
        int bi_best = -1, bi_2nd = -1;
        for (int i = 0; i < nc; i++) {
            rb->cand_modes[i] = cm[i];
            size_t pb = wb_probe_candidate_mode(cm[i], cp[i], blk, bn, 1);
            rb->cand_bytes[i] = pb;
            if (pb == (size_t)-1) continue;
            if (pb < best) {
                best = pb;
                bi_2nd = bi_best;
                bi_best = i;
            } else if (bi_2nd < 0 || pb < rb->cand_bytes[bi_2nd]) {
                bi_2nd = i;
            }
        }
        if (bi_best < 0) {
            /* every candidate declined: route raw, no probe bytes */
            rb->mode = WB_MODE_RAW;
            rb->param = 0;
            rb->probe_bytes = (size_t)-1;
            continue;
        }
        /* LZM2 tie-break (v3b scheduling): top-2 BWT probes within 5%,
         * block actually compressed, block >= 64KB. Both finalists get
         * the LZM2 probe; the min(BWT, LZM2) wins the block. */
        rb->lzm2_tiebreak = 0;
        if (bi_2nd >= 0 && rb->cand_bytes[bi_2nd] <= best + best / 20 &&
            best < bn && bn >= 65536) {
            size_t lz0 = wb_probe_candidate_mode(cm[bi_best], cp[bi_best], blk, bn, 7);
            size_t lz1 = wb_probe_candidate_mode(cm[bi_2nd], cp[bi_2nd], blk, bn, 7);
            if (lz0 != (size_t)-1 || lz1 != (size_t)-1) {
                rb->lzm2_tiebreak = 1;
                size_t s0 = (lz0 != (size_t)-1 && lz0 < best) ? lz0 : best;
                size_t b2 = rb->cand_bytes[bi_2nd];
                size_t s1 = (lz1 != (size_t)-1 && lz1 < b2) ? lz1 : b2;
                if (s1 < s0) {
                    bi_best = bi_2nd;
                    best = s1;
                } else {
                    best = s0;
                }
            }
        }
        rb->mode = cm[bi_best];
        rb->param = cp[bi_best];
        rb->probe_bytes = best;
    }
    *routes_out = r;
    *nblocks_out = nblocks;
    return 0;
}

/* Routing audit dump (format documented in ROUTING.md): header comment
 * lines, then one line per block:
 *   idx mode param probe_bytes tiebreak ncands | m0:b0 m1:b1 ...
 * declined candidates print as X. probe_bytes X when every candidate
 * declined. */
char *wb_route_audit(const char *tag, size_t block_size,
                     const wb_block_route_t *routes, size_t nblocks) {
    if (!routes || !nblocks) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    /* append helper */
#define AUD(...)                                                        \
    do {                                                                 \
        int need = snprintf(NULL, 0, __VA_ARGS__);                        \
        if (need < 0) { free(buf); return NULL; }                         \
        while (len + (size_t)need + 1 > cap) {                            \
            cap *= 2;                                                     \
            char *nb = realloc(buf, cap);                                 \
            if (!nb) { free(buf); return NULL; }                           \
            buf = nb;                                                     \
        }                                                                 \
        len += (size_t)snprintf(buf + len, cap - len, __VA_ARGS__);        \
    } while (0)
    AUD("# sicknode routing audit v1\n");
    AUD("# tag=%s block_size=%zu nblocks=%zu\n", tag ? tag : "-",
        block_size, nblocks);
    AUD("# per-block: idx mode param probe_bytes tiebreak | cand_mode:bytes,...\n");
    for (size_t b = 0; b < nblocks; b++) {
        const wb_block_route_t *rb = &routes[b];
        if (rb->probe_bytes == (size_t)-1)
            AUD("%zu %s %llu X %d %d |", b, wb_mode_name(rb->mode),
                (unsigned long long)rb->param, rb->lzm2_tiebreak, rb->ncands);
        else
            AUD("%zu %s %llu %zu %d %d |", b, wb_mode_name(rb->mode),
                (unsigned long long)rb->param, rb->probe_bytes,
                rb->lzm2_tiebreak, rb->ncands);
        for (int i = 0; i < rb->ncands; i++) {
            if (rb->cand_bytes[i] == (size_t)-1)
                AUD(" %s:X", wb_mode_name(rb->cand_modes[i]));
            else
                AUD(" %s:%zu", wb_mode_name(rb->cand_modes[i]),
                    rb->cand_bytes[i]);
        }
        AUD("\n");
    }
#undef AUD
    return buf;
}

/* Encode one candidate (or raw) to a malloc'd frame. Transform candidates
 * wrap as [mode][meta_len u32][meta][inner blob]; raw is the bare inner
 * frame. Never-expand vs cap: returns -1 when the framed total >= cap. */
static int wb_encode_candidate(const uint8_t *in, size_t n, int mode,
                               uint64_t param, size_t cap,
                               uint8_t **out, size_t *on) {
    int is_raw = (mode == WB_MODE_RAW);
    const uint8_t *src = in;
    size_t srcn = n;
    uint8_t *t = NULL, *meta = NULL;
    size_t tn = 0, mn = 0;
    if (!is_raw) {
        if (wb_apply(mode, param, in, n, &t, &tn, &meta, &mn)) return -1;
        src = t;
        srcn = tn;
    }
    uint8_t *payload = NULL;
    size_t payload_n = 0;
    int ok = (srcn == 0) || tnssrc_encode_inner(src, srcn, &payload, &payload_n);
    free(t);
    if (ok) {
        free(meta);
        return -1;
    }
    uint8_t *ob;
    size_t total;
    if (is_raw) {
        ob = payload;
        total = payload_n;
    } else {
        total = 1 + 4 + mn + payload_n;
        if (total >= cap) {
            free(payload);
            free(meta);
            return -1;
        }
        ob = malloc(total ? total : 1);
        if (!ob) {
            free(payload);
            free(meta);
            return -1;
        }
        ob[0] = (uint8_t)mode;
        wb_wr32le(ob + 1, (uint32_t)mn);
        memcpy(ob + 5, meta, mn);
        memcpy(ob + 5 + mn, payload, payload_n);
        free(payload);
    }
    free(meta);
    *out = ob;
    *on = total;
    return 0;
}

/* Routed per-block encode (spec v2 + block-table addendum).
 *
 * Frame: [24][block_size u32 LE][nblocks u32 LE][table][block frames...]
 * The block table is sized DYNAMICALLY: 16 bytes x nblocks, where
 *   nblocks = ceil(orig / block_size)   (derived from the frame's own
 *   block_size field and the caller's orig at decode time; validated
 *   before any allocation).
 * Table entry: [offset u64 LE][len u64 LE], offset from frame start.
 * Each block frame is a whole-file-style frame ([mode][meta][inner] or a
 * bare inner frame for raw blocks), so per-block decode reuses the exact
 * whole-file frame parser.
 *
 * Every block's frame must beat its block (per-block never-expand); the
 * assembled total must beat |input| (global never-expand). A block whose
 * routed transform fails exact encoding fails the routed candidate: the
 * routing map is then unreliable for this input and the conductor falls
 * back to whole-file candidates. block_bytes_out (optional) receives the
 * per-block framed lengths for Tier-3 logging. */
static int wb_encode_routed_ex(const uint8_t *in, size_t n, size_t block_size,
                               const wb_block_route_t *routes, size_t nblocks,
                               uint8_t **out, size_t *on,
                               size_t *block_bytes_out);

int wb_encode_routed(const uint8_t *in, size_t n, size_t block_size,
                     const wb_block_route_t *routes, size_t nblocks,
                     uint8_t **out, size_t *on) {
    return wb_encode_routed_ex(in, n, block_size, routes, nblocks, out, on,
                               NULL);
}

static int wb_encode_routed_ex(const uint8_t *in, size_t n, size_t block_size,
                               const wb_block_route_t *routes, size_t nblocks,
                               uint8_t **out, size_t *on,
                               size_t *block_bytes_out) {
    if (!in || !n || !routes || !nblocks || !block_size || !out || !on)
        return -1;
    if ((n + block_size - 1) / block_size != nblocks) return -1;
    if (block_size > 0xFFFFFFFFu || nblocks > 0xFFFFFFFFu) return -1;
    uint8_t **frames = calloc(nblocks, sizeof *frames);
    size_t *flens = calloc(nblocks, sizeof *flens);
    if (!frames || !flens) {
        free(frames);
        free(flens);
        return -1;
    }
    size_t total = 9 + 16 * nblocks;
    int rc = -1;
    for (size_t b = 0; b < nblocks; b++) {
        size_t off = b * block_size;
        size_t bn = n - off > block_size ? block_size : n - off;
        if (wb_encode_candidate(in + off, bn, routes[b].mode, routes[b].param,
                                bn, &frames[b], &flens[b]))
            goto done;
        total += flens[b];
        if (total >= n) goto done; /* global never-expand vs |input| */
    }
    {
        uint8_t *ob = malloc(total ? total : 1);
        if (!ob) goto done;
        ob[0] = WB_MODE_ROUTED;
        wb_wr32le(ob + 1, (uint32_t)block_size);
        wb_wr32le(ob + 5, (uint32_t)nblocks);
        size_t woff = 9 + 16 * nblocks;
        for (size_t b = 0; b < nblocks; b++) {
            wb_wr64le(ob + 9 + 16 * b, (uint64_t)woff);
            wb_wr64le(ob + 9 + 16 * b + 8, (uint64_t)flens[b]);
            memcpy(ob + woff, frames[b], flens[b]);
            if (block_bytes_out) block_bytes_out[b] = flens[b];
            woff += flens[b];
        }
        *out = ob;
        *on = total;
        rc = 0;
    }
done:
    for (size_t b = 0; b < nblocks; b++) free(frames[b]);
    free(frames);
    free(flens);
    return rc;
}

/* ================= Tier-3 mixer logging (spec v3) =================
 * Append-only TSV. No learning happens here: when the cold loop later
 * finds a surprise winner, the (scan-features -> winning transform) pairs
 * in this log are the training signal. Until the corpus grows, Tier-1
 * hand-tuned scoring stays the model. */

const char *wb_mixer_log_path(void) {
    const char *e = getenv("NPCC_MIXER_LOG");
    if (e && *e) return e;
    static char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof path, "%s/workspace/tnssrc-workbench/MIXER_LOG.tsv",
             home);
    return path;
}

static void wb_tsv_sanitize(char *dst, size_t dn, const char *s) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < dn; i++) {
        char c = s[i];
        dst[j++] = (c == '\t' || c == '\n' || c == '\r') ? '_' : c;
    }
    dst[j] = 0;
}

int wb_mixer_log_row(const char *file, const char *block, size_t size,
                     double entropy, double h1, double alpha_util,
                     double e8e9_per_mb, size_t period, double period_conf,
                     int smooth8, int smooth16, int smooth24, int smooth32,
                     int is_tar, int homogeneous, int bwt_friendly,
                     const char *seats, const char *winner,
                     size_t packed_bytes) {
    const char *path = wb_mixer_log_path();
    FILE *f = fopen(path, "r");
    int exists = (f != NULL);
    if (f) fclose(f);
    f = fopen(path, "a");
    if (!f) return -1;
    if (!exists) {
        fprintf(f, "ts\tfile\tblock\tsize\tentropy\th1\talpha_util\t"
                   "e8e9_per_mb\tperiod\tperiod_conf\t"
                   "smooth8\tsmooth16\tsmooth24\tsmooth32\t"
                   "is_tar\thomogeneous\tbwt_friendly\t"
                   "seats\twinner\tpacked_bytes\n");
    }
    char fs[1024], bs[64], ss[1024], ws[64];
    wb_tsv_sanitize(fs, sizeof fs, file ? file : "-");
    wb_tsv_sanitize(bs, sizeof bs, block ? block : "-");
    wb_tsv_sanitize(ss, sizeof ss, seats ? seats : "-");
    wb_tsv_sanitize(ws, sizeof ws, winner ? winner : "-");
    fprintf(f, "%lld\t%s\t%s\t%zu\t%.4f\t%.4f\t%.4f\t%.2f\t%zu\t%.4f\t"
               "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%zu\n",
            (long long)time(NULL), fs, bs, size, entropy, h1, alpha_util,
            e8e9_per_mb, period, period_conf, smooth8, smooth16, smooth24,
            smooth32, is_tar, homogeneous, bwt_friendly, ss, ws, packed_bytes);
    fclose(f);
    return 0;
}

/* ---- Escalation handoff serialization (spec v4, format in HANDOFF.md) ---- */

int wb_handoff_write(const wb_handoff_t *h, char **out) {
    if (!h || !out) return -1;
    size_t cap = 2048, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
#define HO(...)                                                         \
    do {                                                                 \
        int need = snprintf(NULL, 0, __VA_ARGS__);                        \
        if (need < 0) { free(buf); return -1; }                            \
        while (len + (size_t)need + 1 > cap) {                             \
            cap *= 2;                                                     \
            char *nb = realloc(buf, cap);                                 \
            if (!nb) { free(buf); return -1; }                             \
            buf = nb;                                                     \
        }                                                                 \
        len += (size_t)snprintf(buf + len, cap - len, __VA_ARGS__);        \
    } while (0)
    const wb_scan_t *r = &h->scan;
    HO("# sicknode handoff v1\n");
    HO("incumbent bytes=%zu mode=%d param=%llu\n", h->incumbent_bytes,
       h->incumbent_mode, (unsigned long long)h->incumbent_param);
    HO("scan size=%zu entropy=%.4f h1=%.4f alpha=%.4f e8e9_per_mb=%.2f "
       "period=%zu period_conf=%.4f smooth8=%d smooth16=%d smooth24=%d "
       "smooth32=%d is_tar=%d homogeneous=%d bwt_friendly=%d\n",
       r->size, r->entropy, r->h1, r->alpha_util, r->e8e9_per_mb,
       r->record_period, r->period_conf, r->smooth8, r->smooth16, r->smooth24,
       r->smooth32, r->is_tar, r->homogeneous, r->bwt_friendly);
    for (int i = 0; i < h->ntried; i++) {
        if (h->tried[i].bytes == (size_t)-1)
            HO("tried mode=%d param=%llu bytes=X\n", h->tried[i].mode,
               (unsigned long long)h->tried[i].param);
        else
            HO("tried mode=%d param=%llu bytes=%zu\n", h->tried[i].mode,
               (unsigned long long)h->tried[i].param, h->tried[i].bytes);
    }
    if (h->routed_bytes == (size_t)-1)
        HO("routed built=%d bytes=X block_size=%zu nblocks=%zu\n",
           h->routed_built, h->block_size, h->nblocks);
    else
        HO("routed built=%d bytes=%zu block_size=%zu nblocks=%zu\n",
           h->routed_built, h->routed_bytes, h->block_size, h->nblocks);
#undef HO
    *out = buf;
    return 0;
}

/* Parse the wb_handoff_write format back. Returns 0 on success, -1 on any
 * malformed input (fail closed; never returns a partial handoff). */
int wb_handoff_read(const char *s, wb_handoff_t *h) {
    if (!s || !h) return -1;
    wb_handoff_t t;
    memset(&t, 0, sizeof t);
    t.routed_bytes = (size_t)-1;
    const char magic[] = "# sicknode handoff v1\n";
    if (strncmp(s, magic, sizeof magic - 1)) return -1;
    s += sizeof magic - 1;
    int have_inc = 0, have_scan = 0, have_routed = 0;
    char line[2048];
    while (*s) {
        size_t i = 0;
        while (s[i] && s[i] != '\n' && i + 1 < sizeof line) {
            line[i] = s[i];
            i++;
        }
        line[i] = 0;
        s += i;
        if (*s == '\n') s++;
        if (!strncmp(line, "incumbent ", 10)) {
            unsigned long long param = 0;
            if (sscanf(line + 10, "bytes=%zu mode=%d param=%llu",
                       &t.incumbent_bytes, &t.incumbent_mode,
                       &param) != 3)
                return -1;
            t.incumbent_param = (uint64_t)param;
            have_inc = 1;
        } else if (!strncmp(line, "scan ", 5)) {
            wb_scan_t *r = &t.scan;
            unsigned long long period = 0;
            if (sscanf(line + 5,
                       "size=%zu entropy=%lf h1=%lf alpha=%lf e8e9_per_mb=%lf "
                       "period=%llu period_conf=%lf smooth8=%d smooth16=%d "
                       "smooth24=%d smooth32=%d is_tar=%d homogeneous=%d "
                       "bwt_friendly=%d",
                       &r->size, &r->entropy, &r->h1, &r->alpha_util,
                       &r->e8e9_per_mb, &period, &r->period_conf, &r->smooth8,
                       &r->smooth16, &r->smooth24, &r->smooth32, &r->is_tar,
                       &r->homogeneous, &r->bwt_friendly) != 14)
                return -1;
            r->record_period = (size_t)period;
            have_scan = 1;
        } else if (!strncmp(line, "tried ", 6)) {
            if (t.ntried >= WB_TRIED_MAX) return -1;
            unsigned long long param = 0;
            char bstr[64];
            if (sscanf(line + 6, "mode=%d param=%llu bytes=%63s",
                       &t.tried[t.ntried].mode, &param, bstr) != 3)
                return -1;
            t.tried[t.ntried].param = (uint64_t)param;
            if (!strcmp(bstr, "X")) {
                t.tried[t.ntried].bytes = (size_t)-1;
            } else {
                char *end = NULL;
                unsigned long long b = strtoull(bstr, &end, 10);
                if (!end || *end) return -1;
                t.tried[t.ntried].bytes = (size_t)b;
            }
            t.ntried++;
        } else if (!strncmp(line, "routed ", 7)) {
            char bstr[64];
            if (sscanf(line + 7, "built=%d bytes=%63s block_size=%zu nblocks=%zu",
                       &t.routed_built, bstr, &t.block_size,
                       &t.nblocks) != 4)
                return -1;
            if (!strcmp(bstr, "X")) {
                t.routed_bytes = (size_t)-1;
            } else {
                char *end = NULL;
                unsigned long long b = strtoull(bstr, &end, 10);
                if (!end || *end) return -1;
                t.routed_bytes = (size_t)b;
            }
            have_routed = 1;
        } else if (line[0] == '#' || line[0] == 0) {
            continue;
        } else {
            return -1;
        }
    }
    if (!have_inc || !have_scan || !have_routed) return -1;
    *h = t;
    return 0;
}

/* ================= conductor: wb_encode / wb_decode ================= */

/* Conductor: (a) whole-file candidates (raw + mixer top-3, or the full
 * battery) each through full tnssrc_encode_inner(); (b) the routed
 * per-block candidate (hot loop, parameterized block size, only when the
 * input spans >= 2 blocks). Winner = true minimum by actual total bytes
 * over (a)+(b); never-expand vs |input| always applies. */
int wb_encode_full(const uint8_t *in, size_t n, int full, wb_result_t *res) {
    if (!in || !res) return -1;
    memset(res, 0, sizeof *res);
    res->winner_mode = -1;
    res->whole_best_mode = -1;
    if (wb_scan(in, n, &res->rep)) return -1;
    wb_candidate_t cands[13];
    int nc = wb_bay1(&res->rep, cands, full);
    if (nc < 0) return -1;
    for (int i = 0; i < nc; i++) res->seats[i] = cands[i];
    res->nseats = nc;

    uint8_t *best = NULL;
    size_t best_n = 0;
    int have = 0;
    int best_mode = -1;
    uint64_t best_param = 0;
    wb_tried_t tried[WB_TRIED_MAX];
    int ntried = 0;
    /* (a) whole-file candidates */
    for (int i = 0; i < nc; i++) {
        uint8_t *ob = NULL;
        size_t total = 0;
        int ok = !wb_encode_candidate(in, n, cands[i].mode, cands[i].param, n,
                                      &ob, &total);
        if (ntried < WB_TRIED_MAX) {
            tried[ntried].mode = cands[i].mode;
            tried[ntried].param = cands[i].param;
            tried[ntried].bytes = ok ? total : (size_t)-1;
            ntried++;
        }
        if (!ok) continue;
        if (!have || total < best_n) {
            free(best);
            best = ob;
            best_n = total;
            have = 1;
            best_mode = cands[i].mode;
            best_param = cands[i].param;
        } else {
            free(ob);
        }
    }
    res->whole_best_bytes = have ? best_n : 0;
    res->whole_best_mode = best_mode;
    /* (b) routed per-block candidate (hot loop) */
    size_t bs = wb_hot_block_size();
    size_t nblocks = (n + bs - 1) / bs;
    if (nblocks >= 2) {
        wb_block_route_t *routes = NULL;
        size_t nrb = 0;
        if (wb_route(in, n, bs, &res->rep, &routes, &nrb) == 0 &&
            nrb == nblocks) {
            uint8_t *rob = NULL;
            size_t ron = 0;
            size_t *bb = calloc(nblocks, sizeof *bb);
            int built =
                bb && !wb_encode_routed_ex(in, n, bs, routes, nrb, &rob, &ron,
                                          bb);
            if (ntried < WB_TRIED_MAX) {
                tried[ntried].mode = WB_MODE_ROUTED;
                tried[ntried].param = bs;
                tried[ntried].bytes = built ? ron : (size_t)-1;
                ntried++;
            }
            if (built) {
                res->routed_built = 1;
                res->routed_bytes = ron;
                res->routes = routes;
                res->nblocks = nrb;
                res->block_size = bs;
                res->routed_block_bytes = bb;
                routes = NULL;
                bb = NULL;
                if (ron < n && (!have || ron < best_n)) {
                    free(best);
                    best = rob;
                    best_n = ron;
                    have = 1;
                    best_mode = WB_MODE_ROUTED;
                    best_param = bs;
                } else {
                    free(rob);
                }
            } else {
                free(bb);
            }
            free(routes);
        }
    }
    if (!have) return -1;
    /* escalation handoff (spec v4): populated on every run */
    res->handoff.scan = res->rep;
    res->handoff.incumbent_bytes = best_n;
    res->handoff.incumbent_mode = best_mode;
    res->handoff.incumbent_param = best_param;
    for (int i = 0; i < ntried; i++) res->handoff.tried[i] = tried[i];
    res->handoff.ntried = ntried;
    res->handoff.routed_built = res->routed_built;
    res->handoff.routed_bytes =
        res->routed_built ? res->routed_bytes : (size_t)-1;
    res->handoff.block_size = bs;
    res->handoff.nblocks = nblocks;
    if (getenv("NPCC_VERBOSE"))
        fprintf(stderr, "wb outer mode=%d packed=%zu orig=%zu full=%d\n",
                best_mode, best_n, n, full);
    res->out = best;
    res->on = best_n;
    res->winner_mode = best_mode;
    return 0;
}

void wb_result_free(wb_result_t *res) {
    if (!res) return;
    free(res->out);
    free(res->routes);
    free(res->routed_block_bytes);
    memset(res, 0, sizeof *res);
}

/* Original contract entry point: signature and behavior preserved
 * (raw stays byte-identical); now a thin wrapper over wb_encode_full. */
int wb_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on, int full) {
    if (!in || !out || !on) return -1;
    wb_result_t res;
    if (wb_encode_full(in, n, full, &res)) return -1;
    *out = res.out;
    *on = res.on;
    res.out = NULL;
    wb_result_free(&res);
    return 0;
}

/* one whole-file-style frame: [mode][mn][meta][inner] or bare inner */
static int wb_decode_frame(const uint8_t *in, size_t n, size_t orig,
                           uint8_t **out, size_t *on) {
    if (!in || !out || !on || !n) return -1;
    unsigned mode = in[0];
    if (mode >= WB_MODE_COLUMNAR && mode <= WB_MODE_SHUFFLE) {
        if (n < 13) return -1;
        uint32_t mn = wb_rd32le(in + 1);
        if (mn < 8 || (size_t)mn + 5 > n) return -1;
        const uint8_t *meta = in + 5;
        const uint8_t *iblob = in + 5 + mn;
        size_t iblen = n - 5 - mn;
        uint64_t tn = wb_rd64le(meta);
        if (tn == 0 || tn > orig || iblen < 1) return -1;
        uint64_t param = 0;
        switch (mode) {
        case WB_MODE_COLUMNAR:
            if (mn < 16) return -1;
            param = wb_rd64le(meta + 8);
            break;
        case WB_MODE_IMG2D:
            if (mn < 12) return -1;
            param = wb_rd32le(meta + 8);
            break;
        case WB_MODE_SHUFFLE:
            if (mn < 16) return -1;
            param = wb_rd64le(meta + 8);
            break;
        default:
            break;
        }
        uint8_t *t = NULL;
        size_t tgot = 0;
        if (tnssrc_decode_inner(iblob, iblen, (size_t)tn, &t, &tgot)) return -1;
        if (tgot != (size_t)tn) {
            free(t);
            return -1;
        }
        uint8_t *y = malloc(orig ? orig : 1);
        if (!y) {
            free(t);
            return -1;
        }
        int rc = wb_invert((int)mode, param, t, (size_t)tn, meta, mn, y, orig);
        free(t);
        if (rc) {
            free(y);
            return -1;
        }
        *out = y;
        *on = orig;
        return 0;
    }
    /* RAW: the bare inner frame decodes directly */
    return tnssrc_decode_inner(in, n, orig, out, on);
}

/* Routed frame decode (spec v2 + block-table addendum).
 *
 * The block table is sized DYNAMICALLY from the frame: nblocks and
 * block_size come from the header, and every bound is validated BEFORE
 * any allocation or use:
 *  - header present (9 bytes), block_size >= 1, nblocks >= 1
 *  - table (16 x nblocks) fits inside the frame (this also rejects
 *    absurd nblocks before the table is touched)
 *  - nblocks is consistent with orig: (nblocks-1)*block_size < orig <=
 *    nblocks*block_size  (i.e. nblocks == ceil(orig/block_size))
 *  - each entry's offset/len lies inside the frame, at/after the table,
 *    entries are in order and non-overlapping
 *  - each block frame decodes to exactly its expected original length
 *    (block_size, except the last block)
 * Corrupt frames fail closed with -1; no partial output is returned. */
static int wb_decode_routed(const uint8_t *in, size_t n, size_t orig,
                            uint8_t **out, size_t *on) {
    if (n < 9) return -1;
    uint64_t bs64 = wb_rd32le(in + 1);
    uint64_t nblocks64 = wb_rd32le(in + 5);
    if (bs64 == 0 || nblocks64 == 0) return -1;
    if (nblocks64 > ((uint64_t)n - 9) / 16) return -1; /* table must fit */
    size_t nblocks = (size_t)nblocks64;
    size_t block_size = (size_t)bs64;
    /* nblocks == ceil(orig / block_size) */
    if (orig == 0) return -1;
    if ((nblocks64 - 1) * bs64 >= orig) return -1;
    if (nblocks64 * bs64 < orig) return -1;
    size_t taboff = 9;
    size_t framesoff = 9 + 16 * nblocks;
    uint8_t *y = malloc(orig);
    if (!y) return -1;
    size_t produced = 0;
    uint64_t prev_end = framesoff;
    for (size_t b = 0; b < nblocks; b++) {
        uint64_t boff = wb_rd64le(in + taboff + 16 * b);
        uint64_t blen = wb_rd64le(in + taboff + 16 * b + 8);
        if (boff < framesoff || blen == 0 || blen > n) {
            free(y);
            return -1;
        }
        if (boff < prev_end || boff + blen > n || boff + blen < boff) {
            free(y);
            return -1;
        }
        prev_end = boff + blen;
        size_t exp = (b + 1 < nblocks) ? block_size : orig - produced;
        if (exp == 0 || exp > block_size || produced + exp > orig) {
            free(y);
            return -1;
        }
        uint8_t *piece = NULL;
        size_t piecen = 0;
        if (wb_decode_frame(in + boff, (size_t)blen, exp, &piece, &piecen)) {
            free(y);
            return -1;
        }
        if (piecen != exp) {
            free(piece);
            free(y);
            return -1;
        }
        memcpy(y + produced, piece, exp);
        free(piece);
        produced += exp;
    }
    if (produced != orig) {
        free(y);
        return -1;
    }
    *out = y;
    *on = orig;
    return 0;
}

int wb_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on) {
    if (!in || !out || !on || !n) return -1;
    if (in[0] == WB_MODE_ROUTED) return wb_decode_routed(in, n, orig, out, on);
    return wb_decode_frame(in, n, orig, out, on);
}
