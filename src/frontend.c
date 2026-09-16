/* Front-end transform arms for TNSSRC. See frontend.h.
 * Ported designs (reimplemented in C):
 *  - sao: SAO star-catalog 28-byte record columnar split (from PCCX Op 13).
 *  - d16: little-endian u16 left-delta (from PCCX Op 14).
 *  - exe: x86/x64 E8/E9 rel32 -> absolute normalization (from PCCX Op 15).
 * All transforms are exactly invertible and deterministic. Proprietary. */
#include "frontend.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} FEBuf;

static int fe_push(FEBuf *b, uint8_t x) {
    if (b->n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    b->p[b->n++] = x;
    return 0;
}
static int fe_app(FEBuf *b, const uint8_t *p, size_t n) {
    if (n == 0) return 0;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}
static int fe_u32(FEBuf *b, uint32_t v) {
    uint8_t tmp[4];
    wr32le(tmp, v);
    return fe_app(b, tmp, 4);
}
/* unsigned LEB128 */
static int fe_uleb(FEBuf *b, uint64_t v) {
    do {
        uint8_t c = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (fe_push(b, v ? (uint8_t)(c | 0x80) : c)) return -1;
    } while (v);
    return 0;
}
static int fe_rd_uleb(const uint8_t *p, size_t n, size_t *i, uint64_t *out) {
    uint64_t v = 0;
    int sh = 0;
    while (*i < n) {
        uint8_t c = p[(*i)++];
        v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) {
            *out = v;
            return 0;
        }
        sh += 7;
        if (sh > 63) return -1;
    }
    return -1;
}

/* ---------------- SAO: 28-byte record columnar ---------------- */
#define SAO_REC 28
#define SAO_NCOL 7

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Validates the 28-byte record layout from record 0. Returns record count
 * r (including header record) or 0 when the layout does not match. */
static size_t sao_sniff(const uint8_t *in, size_t n, uint32_t *nrec_out) {
    if (n < 2 * SAO_REC || n % SAO_REC != 0) return 0;
    if (rd32le(in + 6 * 4) != SAO_REC) return 0;
    uint32_t nrec = rd32le(in + 2 * 4);
    if (nrec == 0) return 0;
    if ((uint64_t)(nrec + 1) * SAO_REC != n) return 0;
    *nrec_out = nrec;
    return (size_t)nrec + 1;
}

int fe_sao_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn) {
    uint32_t nrec = 0;
    size_t r = sao_sniff(in, n, &nrec);
    if (!r) return -1;
    FEBuf t = {0}, m = {0};
    uint8_t tn_le[8];
    wr64le(tn_le, 0); /* patched at the end */
    if (fe_app(&m, tn_le, 8) || fe_u32(&m, nrec)) goto fail;
    /* column c section encodings; dict info for cols 4..6 goes to meta */
    for (int c = 0; c < SAO_NCOL; c++) {
        if (c == 1) {
            /* sorted coordinate column -> wrapping deltas */
            uint32_t prev = 0;
            for (size_t rec = 0; rec < r; rec++) {
                uint32_t v = rd32le(in + rec * SAO_REC + (size_t)c * 4);
                uint32_t dv = rec == 0 ? v : v - prev;
                uint8_t tmp[4];
                wr32le(tmp, dv);
                if (fe_app(&t, tmp, 4)) goto fail;
                prev = v;
            }
        } else if (c >= 4 && c <= 6) {
            /* low-cardinality -> dictionary + ranks */
            uint32_t *uniq = malloc(r * sizeof(uint32_t));
            if (!uniq) goto fail;
            for (size_t rec = 0; rec < r; rec++)
                uniq[rec] = rd32le(in + rec * SAO_REC + (size_t)c * 4);
            qsort(uniq, r, sizeof(uint32_t), cmp_u32);
            size_t nd = 0;
            for (size_t k = 0; k < r; k++)
                if (k == 0 || uniq[k] != uniq[k - 1]) uniq[nd++] = uniq[k];
            if (nd > 65536) {
                /* degenerate: raw column, marker 0 */
                free(uniq);
                if (fe_u32(&m, 0)) goto fail;
                for (size_t rec = 0; rec < r; rec++) {
                    uint8_t tmp[4];
                    memcpy(tmp, in + rec * SAO_REC + (size_t)c * 4, 4);
                    if (fe_app(&t, tmp, 4)) goto fail;
                }
            } else {
                if (fe_u32(&m, (uint32_t)nd)) {
                    free(uniq);
                    goto fail;
                }
                for (size_t k = 0; k < nd; k++) {
                    uint8_t tmp[4];
                    wr32le(tmp, uniq[k]);
                    if (fe_app(&m, tmp, 4)) {
                        free(uniq);
                        goto fail;
                    }
                }
                int wide = nd > 256;
                for (size_t rec = 0; rec < r; rec++) {
                    uint32_t v = rd32le(in + rec * SAO_REC + (size_t)c * 4);
                    /* binary search in sorted uniq */
                    size_t lo = 0, hi = nd;
                    while (lo < hi) {
                        size_t mid = lo + (hi - lo) / 2;
                        if (uniq[mid] < v)
                            lo = mid + 1;
                        else
                            hi = mid;
                    }
                    if (lo >= nd || uniq[lo] != v) {
                        free(uniq);
                        goto fail; /* cannot happen */
                    }
                    if (wide) {
                        uint8_t tmp[2] = {(uint8_t)lo, (uint8_t)(lo >> 8)};
                        if (fe_app(&t, tmp, 2)) {
                            free(uniq);
                            goto fail;
                        }
                    } else {
                        if (fe_push(&t, (uint8_t)lo)) {
                            free(uniq);
                            goto fail;
                        }
                    }
                }
                free(uniq);
            }
        } else {
            /* raw u32 column bytes */
            for (size_t rec = 0; rec < r; rec++) {
                uint8_t tmp[4];
                memcpy(tmp, in + rec * SAO_REC + (size_t)c * 4, 4);
                if (fe_app(&t, tmp, 4)) goto fail;
            }
        }
    }
    wr64le(m.p, (uint64_t)t.n);
    *tp = t.p;
    *tn = t.n;
    *mp = m.p;
    *mn = m.n;
    return 0;
fail:
    free(t.p);
    free(m.p);
    return -1;
}

int fe_sao_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n) {
    if (mn < 12) return -1;
    if (rd64le(meta) != tn) return -1;
    uint32_t nrec = rd32le(meta + 8);
    size_t r = (size_t)nrec + 1;
    if (r * SAO_REC != n) return -1;
    size_t mi = 12;
    /* parse dicts for cols 4..6 first (they precede column data layout) */
    uint32_t *dicts[3] = {0, 0, 0};
    size_t nds[3] = {0, 0, 0};
    int rawcol[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        if (mi + 4 > mn) goto derr;
        uint32_t nd = rd32le(meta + mi);
        mi += 4;
        if (nd == 0) {
            rawcol[k] = 1;
            continue;
        }
        if (nd > 65536 || mi + (size_t)nd * 4 > mn) goto derr;
        dicts[k] = malloc(nd * sizeof(uint32_t));
        if (!dicts[k]) goto derr;
        for (uint32_t j = 0; j < nd; j++) {
            dicts[k][j] = rd32le(meta + mi);
            mi += 4;
        }
        nds[k] = nd;
    }
    if (mi != mn) goto derr;
    /* walk the transformed stream section by section */
    size_t ti = 0;
    for (int c = 0; c < SAO_NCOL; c++) {
        if (c == 1) {
            if (ti + r * 4 > tn) goto derr;
            uint32_t prev = 0;
            for (size_t rec = 0; rec < r; rec++) {
                uint32_t dv = rd32le(t + ti);
                ti += 4;
                uint32_t v = rec == 0 ? dv : prev + dv;
                wr32le(out + rec * SAO_REC + (size_t)c * 4, v);
                prev = v;
            }
        } else if (c >= 4 && c <= 6) {
            int k = c - 4;
            if (rawcol[k]) {
                if (ti + r * 4 > tn) goto derr;
                for (size_t rec = 0; rec < r; rec++) {
                    memcpy(out + rec * SAO_REC + (size_t)c * 4, t + ti, 4);
                    ti += 4;
                }
            } else {
                int wide = nds[k] > 256;
                size_t need = wide ? r * 2 : r;
                if (ti + need > tn) goto derr;
                for (size_t rec = 0; rec < r; rec++) {
                    uint32_t rank;
                    if (wide) {
                        rank = (uint32_t)t[ti] | ((uint32_t)t[ti + 1] << 8);
                        ti += 2;
                    } else {
                        rank = t[ti++];
                    }
                    if (rank >= nds[k]) goto derr;
                    wr32le(out + rec * SAO_REC + (size_t)c * 4, dicts[k][rank]);
                }
            }
        } else {
            if (ti + r * 4 > tn) goto derr;
            for (size_t rec = 0; rec < r; rec++) {
                memcpy(out + rec * SAO_REC + (size_t)c * 4, t + ti, 4);
                ti += 4;
            }
        }
    }
    for (int k = 0; k < 3; k++) free(dicts[k]);
    return ti == tn ? 0 : -1;
derr:
    for (int k = 0; k < 3; k++) free(dicts[k]);
    return -1;
}

/* ---------------- D16: u16 LE left-delta ---------------- */
int fe_d16_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn) {
    if (n < 65536 || (n & 1)) return -1;
    uint8_t *t = malloc(n ? n : 1);
    uint8_t *m = malloc(8);
    if (!t || !m) {
        free(t);
        free(m);
        return -1;
    }
    size_t nw = n / 2;
    uint16_t prev = (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    t[0] = in[0];
    t[1] = in[1];
    for (size_t i = 1; i < nw; i++) {
        uint16_t v = (uint16_t)((uint16_t)in[2 * i] | ((uint16_t)in[2 * i + 1] << 8));
        uint16_t d = (uint16_t)(v - prev);
        t[2 * i] = (uint8_t)d;
        t[2 * i + 1] = (uint8_t)(d >> 8);
        prev = v;
    }
    wr64le(m, (uint64_t)n);
    *tp = t;
    *tn = n;
    *mp = m;
    *mn = 8;
    return 0;
}

int fe_d16_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n) {
    if (mn < 8 || rd64le(meta) != tn || tn != n || (n & 1) || tn < 2) return -1;
    size_t nw = n / 2;
    uint16_t prev = (uint16_t)((uint16_t)t[0] | ((uint16_t)t[1] << 8));
    out[0] = t[0];
    out[1] = t[1];
    for (size_t i = 1; i < nw; i++) {
        uint16_t d = (uint16_t)((uint16_t)t[2 * i] | ((uint16_t)t[2 * i + 1] << 8));
        uint16_t v = (uint16_t)(prev + d);
        out[2 * i] = (uint8_t)v;
        out[2 * i + 1] = (uint8_t)(v >> 8);
        prev = v;
    }
    return 0;
}

/* ---------------- EXE: E8/E9 rel32 -> absolute ---------------- */
#define EXE_REL_LIMIT (16 * 1024 * 1024)
#define EXE_MIN_SITES 128
#define EXE_MIN_LEN 65536

int fe_exe_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn) {
    if (n < EXE_MIN_LEN) return -1;
    /* collect sites (ascending by construction) */
    uint32_t *sites = NULL;
    size_t ns = 0, ncap = 0;
    for (size_t i = 0; i + 5 <= n; i++) {
        uint8_t b = in[i];
        if (b == 0xE8 || b == 0xE9) {
            int32_t rel = (int32_t)rd32le(in + i + 1);
            if (rel >= -EXE_REL_LIMIT && rel < EXE_REL_LIMIT) {
                if (ns == ncap) {
                    size_t nnc = ncap ? ncap * 2 : 4096;
                    uint32_t *nn = realloc(sites, nnc * sizeof(uint32_t));
                    if (!nn) {
                        free(sites);
                        return -1;
                    }
                    sites = nn;
                    ncap = nnc;
                }
                sites[ns++] = (uint32_t)i;
            }
        }
    }
    if (ns < EXE_MIN_SITES) {
        free(sites);
        return -1;
    }
    uint8_t *t = malloc(n ? n : 1);
    FEBuf m = {0};
    if (!t) {
        free(sites);
        return -1;
    }
    memcpy(t, in, n);
    for (size_t k = 0; k < ns; k++) {
        size_t i = sites[k];
        uint32_t cur = rd32le(t + i + 1);
        uint32_t abs = cur + (uint32_t)(i + 5);
        uint8_t tmp[4];
        wr32le(tmp, abs);
        memcpy(t + i + 1, tmp, 4);
    }
    uint8_t tn_le[8];
    wr64le(tn_le, (uint64_t)n);
    int ok = fe_app(&m, tn_le, 8);
    uint8_t ns_le[4];
    wr32le(ns_le, (uint32_t)ns);
    ok |= fe_app(&m, ns_le, 4);
    uint32_t prev = 0;
    for (size_t k = 0; k < ns && !ok; k++) {
        ok |= fe_uleb(&m, (uint64_t)(sites[k] - prev));
        prev = sites[k];
    }
    free(sites);
    if (ok) {
        free(t);
        free(m.p);
        return -1;
    }
    *tp = t;
    *tn = n;
    *mp = m.p;
    *mn = m.n;
    return 0;
}

int fe_exe_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n) {
    if (mn < 12 || rd64le(meta) != tn || tn != n) return -1;
    uint32_t ns = rd32le(meta + 8);
    if (ns < EXE_MIN_SITES) return -1;
    uint32_t *sites = malloc((size_t)ns * sizeof(uint32_t));
    if (!sites) return -1;
    size_t mi = 12;
    uint32_t prev = 0;
    for (uint32_t k = 0; k < ns; k++) {
        uint64_t d = 0;
        if (fe_rd_uleb(meta, mn, &mi, &d)) {
            free(sites);
            return -1;
        }
        if (d > 0xFFFFFFFFu - prev) {
            free(sites);
            return -1;
        }
        prev += (uint32_t)d;
        sites[k] = prev;
    }
    if (mi != mn) {
        free(sites);
        return -1;
    }
    memcpy(out, t, n);
    /* invert in exact reverse order of the forward pass */
    for (uint32_t k = ns; k-- > 0;) {
        size_t i = sites[k];
        if (i + 5 > n) {
            free(sites);
            return -1;
        }
        uint32_t abs = rd32le(out + i + 1);
        uint32_t rel = abs - (uint32_t)(i + 5);
        uint8_t tmp[4];
        wr32le(tmp, rel);
        memcpy(out + i + 1, tmp, 4);
    }
    free(sites);
    return 0;
}
