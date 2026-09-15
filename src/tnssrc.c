/* TNSSRC — TriNeural Shared Spine Row Compression. Compressor 2.
 * LOCAL order maps, MATCH hash-4 chain + 4-rep (4 MiB), ROW record/run.
 * Spine = live match once per 8-bit row. p = P(1) in 1..4095.
 * Proprietary Slid Phi Labs. */
#include "tnssrc.h"

#include <stdlib.h>
#include <string.h>

#define NMAP 8
#define TBITS 18
#define TSZ (1u << TBITS)
#define HBITS 20
#define HSZ (1u << HBITS)
#define WIN (1u << 23)
#define WMASK (WIN - 1)
#define CHAIN 96
#define MINM 4
#define PMAX 4095
#define NHEAD 3
#define HEAD_LOCAL 0
#define HEAD_MATCH 1
#define HEAD_ROW 2
#define NMIX 256

typedef struct {
    uint16_t n0, n1;
} Stat;

typedef struct {
    uint8_t *p;
    size_t n, cap, i;
} Bytes;

static int b_push(Bytes *b, uint8_t x) {
    if (b->n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    b->p[b->n++] = x;
    return 0;
}
static uint8_t b_get(Bytes *b) {
    if (b->i >= b->n) return 0;
    return b->p[b->i++];
}

static uint32_t hash4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t v = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
    return (v * 0x9E3779B1u) >> (32 - HBITS);
}

static size_t rec_width(const uint8_t *s, size_t n) {
    if (n < 128) return 0;
    size_t lim = n < 4096 ? n : 4096, best_w = 0;
    uint32_t best = 0;
    const size_t ws[] = {4, 8, 12, 16, 20, 24, 32, 40, 48, 64};
    for (size_t t = 0; t < sizeof ws / sizeof ws[0]; t++) {
        size_t w = ws[t];
        if (w >= lim / 2) break;
        uint32_t hits = 0, tot = 0;
        for (size_t i = w; i < lim; i++) {
            if (s[i] == s[i - w]) hits++;
            tot++;
        }
        if (tot && hits * 3 > tot && hits > best) {
            best = hits;
            best_w = w;
        }
    }
    return best_w;
}

typedef struct {
    Stat *map;
    uint32_t *mhead, *mprev;
    int32_t w[NMIX][NHEAD];
    uint16_t apm[256][256];
    uint32_t reps[4], live_dist, live_mlen;
    uint8_t expect, last[4], run, c0;
    uint32_t word, x1, x2, x;
    size_t rec_w, rec_i, pos, nsrc;
    const uint8_t *src;
    Bytes *io;
    int enc;
} TN;

static int map_p(const Stat *s) {
    unsigned n0 = s->n0 + 1, n1 = s->n1 + 1;
    unsigned p = (n1 << 12) / (n0 + n1);
    if (p < 1) p = 1;
    if (p > PMAX) p = PMAX;
    return (int)p;
}
static void map_upd(Stat *s, int y) {
    if (y) {
        if (s->n1 < 65535) s->n1++;
    } else if (s->n0 < 65535)
        s->n0++;
    if ((unsigned)s->n0 + s->n1 > 2048u) {
        s->n0 = (uint16_t)((s->n0 >> 1) + 1);
        s->n1 = (uint16_t)((s->n1 >> 1) + 1);
    }
}
static int clamp_p(int p) {
    if (p < 1) return 1;
    if (p > PMAX) return PMAX;
    return p;
}

static int tn_init(TN *t, Bytes *io, const uint8_t *src, size_t n, int enc) {
    memset(t, 0, sizeof *t);
    t->map = calloc((size_t)NMAP * TSZ, sizeof(Stat));
    t->mhead = malloc(HSZ * 4);
    t->mprev = malloc(WIN * 4);
    if (!t->map || !t->mhead || !t->mprev) {
        free(t->map);
        free(t->mhead);
        free(t->mprev);
        return -1;
    }
    memset(t->mhead, 0xFF, HSZ * 4);
    memset(t->mprev, 0xFF, WIN * 4);
    for (int m = 0; m < NMIX; m++) {
        t->w[m][HEAD_LOCAL] = 256;
        t->w[m][HEAD_MATCH] = 256;
        t->w[m][HEAD_ROW] = 128;
    }
    for (int a = 0; a < 256; a++)
        for (int p = 0; p < 256; p++) t->apm[a][p] = (uint16_t)((p << 4) + 8);
    t->c0 = 1;
    t->x1 = 0xFFFFFFFFu;
    t->src = src;
    t->nsrc = n;
    t->io = io;
    t->enc = enc;
    return 0;
}
static void tn_free(TN *t) {
    free(t->map);
    free(t->mhead);
    free(t->mprev);
    t->map = NULL;
    t->mhead = NULL;
    t->mprev = NULL;
}

static void hashes(const TN *t, int bp, uint32_t h[NMAP]) {
    uint32_t c = t->c0, a = t->last[0], b = t->last[1], d = t->last[2], e = t->last[3];
    uint32_t n = (uint32_t)bp;
    h[0] = (c | (n << 9)) & (TSZ - 1);
    h[1] = (c + n * 257 + a * 4099) & (TSZ - 1);
    h[2] = (c * 13 + ((a << 8) | b) * 17 + n) & (TSZ - 1);
    h[3] = (c * 29 + a * 251 + b * 57 + d * 13 + n) & (TSZ - 1);
    h[4] = (t->word ^ (c << 3) ^ n) & (TSZ - 1);
    h[5] = (c + ((a ^ d) << 8) + (e << 4) + n * 13) & (TSZ - 1);
    size_t col = t->rec_w ? (t->rec_i % t->rec_w) : a;
    h[6] = (c * 17 + (uint32_t)col * 4099 + n) & (TSZ - 1);
    h[7] = ((t->run < 63 ? t->run : 63) * 8 + n + (a << 3)) & (TSZ - 1);
}

static void spine_row(TN *t) {
    size_t i = t->pos;
    const uint8_t *s = t->src;
    if (t->live_dist && i >= t->live_dist) {
        t->expect = s[i - t->live_dist];
        return;
    }
    t->live_dist = 0;
    t->live_mlen = 0;
    t->expect = 0;
    if (i < 4) return;
    uint32_t best_m = 0, best_d = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t dist = t->reps[r];
        if (!dist || i < dist + 4) continue;
        size_t j = i - dist;
        if (s[i - 1] != s[j - 1] || s[i - 2] != s[j - 2] || s[i - 3] != s[j - 3] ||
            s[i - 4] != s[j - 4])
            continue;
        uint32_t back = 4;
        while (back < 255 && back < i && back < j && s[i - 1 - back] == s[j - 1 - back]) back++;
        if (back > best_m) {
            best_m = back;
            best_d = dist;
        }
    }
    uint32_t hv = hash4(s[i - 4], s[i - 3], s[i - 2], s[i - 1]);
    uint32_t p = t->mhead[hv];
    int chain = 0;
    while (p != 0xFFFFFFFFu && chain++ < CHAIN) {
        if (p >= i || i - p > WIN) break;
        if (p >= 4 && s[p - 1] == s[i - 1] && s[p - 2] == s[i - 2] && s[p - 3] == s[i - 3] &&
            s[p - 4] == s[i - 4]) {
            uint32_t back = 4;
            while (back < 255 && back < p && back < i && s[i - 1 - back] == s[p - 1 - back])
                back++;
            uint32_t dist = (uint32_t)(i - p);
            if (back > best_m) {
                best_m = back;
                best_d = dist;
            }
        }
        uint32_t nxt = t->mprev[p & WMASK];
        if (nxt >= p) break;
        p = nxt;
    }
    if (best_m >= MINM && best_d) {
        t->live_dist = best_d;
        t->live_mlen = best_m;
        t->expect = s[i - best_d];
    }
}

static int predict(TN *t, int bp, int *tvec, int *mx, uint32_t h[NMAP]) {
    hashes(t, bp, h);
    int pL = 2048;
    for (int o = 3; o >= 0; o--) {
        Stat *s = &t->map[(size_t)o * TSZ + h[o]];
        if ((int)s->n0 + (int)s->n1 >= 1) {
            pL = map_p(s);
            break;
        }
    }
    int pM;
    if (t->live_mlen >= MINM) {
        int yhat = (t->expect >> bp) & 1;
        int conf = 3584 + (int)t->live_mlen * 8;
        if (conf > 4032) conf = 4032;
        pM = yhat ? conf : (4096 - conf);
    } else
        pM = map_p(&t->map[4 * TSZ + h[4]]);
    int pR = (map_p(&t->map[6 * TSZ + h[6]]) + map_p(&t->map[7 * TSZ + h[7]])) / 2;
    pL = clamp_p(pL);
    pM = clamp_p(pM);
    pR = clamp_p(pR);
    tvec[HEAD_LOCAL] = pL;
    tvec[HEAD_MATCH] = pM;
    tvec[HEAD_ROW] = pR;
    int id = (int)((t->last[0] ^ ((uint32_t)bp << 5) ^ (t->last[1] << 1)) & (NMIX - 1));
    *mx = id;
    int wL = t->w[id][0], wM = t->w[id][1], wR = t->w[id][2];
    if (wL < 1) wL = 1;
    if (wM < 1) wM = 1;
    if (wR < 1) wR = 1;
    if (t->live_mlen >= MINM) wM += 2048;
    int p = (int)(((int64_t)wL * pL + (int64_t)wM * pM + (int64_t)wR * pR) / (wL + wM + wR));
    p = clamp_p(p);
    int actx = (t->last[0] ^ (bp << 5)) & 255;
    int slot = p >> 4;
    if (slot > 255) slot = 255;
    return clamp_p((p * 2 + clamp_p(t->apm[actx][slot])) / 3);
}

static void update(TN *t, int y, int p, int bp, const int *tvec, int mx, const uint32_t h[NMAP]) {
    for (int k = 0; k < NMAP; k++) map_upd(&t->map[(size_t)k * TSZ + h[k]], y);
    for (int k = 0; k < NHEAD; k++) {
        int agree = y ? tvec[k] : (4096 - tvec[k]);
        t->w[mx][k] += (agree - 2048) >> 7;
        if (t->w[mx][k] < 1) t->w[mx][k] = 1;
        if (t->w[mx][k] > 8192) t->w[mx][k] = 8192;
    }
    int actx = (t->last[0] ^ (bp << 5)) & 255;
    int slot = p >> 4;
    if (slot > 255) slot = 255;
    int ap = t->apm[actx][slot];
    ap += ((y ? PMAX : 1) - ap) >> 4;
    t->apm[actx][slot] = (uint16_t)clamp_p(ap);
}

static int rc_encode(TN *t, int y, int p) {
    p = clamp_p(p);
    uint32_t xmid = t->x2 + (uint32_t)(((uint64_t)(t->x1 - t->x2) * (uint64_t)p) >> 12);
    if (xmid < t->x2) xmid = t->x2;
    if (xmid >= t->x1) xmid = t->x1 - 1;
    if (y)
        t->x1 = xmid;
    else
        t->x2 = xmid + 1;
    while (((t->x1 ^ t->x2) & 0xFF000000u) == 0) {
        if (b_push(t->io, (uint8_t)(t->x1 >> 24))) return -1;
        t->x1 = (t->x1 << 8) | 255u;
        t->x2 <<= 8;
    }
    return 0;
}
static int rc_flush(TN *t) {
    for (int i = 0; i < 4; i++) {
        if (b_push(t->io, (uint8_t)(t->x1 >> 24))) return -1;
        t->x1 <<= 8;
    }
    return 0;
}
static int rc_decode(TN *t, int p) {
    p = clamp_p(p);
    uint32_t xmid = t->x2 + (uint32_t)(((uint64_t)(t->x1 - t->x2) * (uint64_t)p) >> 12);
    if (xmid < t->x2) xmid = t->x2;
    if (xmid >= t->x1) xmid = t->x1 - 1;
    int y = t->x <= xmid;
    if (y)
        t->x1 = xmid;
    else
        t->x2 = xmid + 1;
    while (((t->x1 ^ t->x2) & 0xFF000000u) == 0) {
        t->x1 = (t->x1 << 8) | 255u;
        t->x2 <<= 8;
        t->x = (t->x << 8) | b_get(t->io);
    }
    return y;
}

static void commit_row(TN *t, uint8_t b) {
    size_t i = t->pos;
    if (t->live_dist && i >= t->live_dist && b == t->src[i - t->live_dist]) {
        t->live_mlen++;
        uint32_t d = t->live_dist;
        if (t->reps[0] != d) {
            t->reps[3] = t->reps[2];
            t->reps[2] = t->reps[1];
            t->reps[1] = t->reps[0];
            t->reps[0] = d;
        }
    } else {
        t->live_dist = 0;
        t->live_mlen = 0;
    }
    if (i >= 4) {
        uint32_t hv = hash4(t->src[i - 4], t->src[i - 3], t->src[i - 2], t->src[i - 1]);
        t->mprev[i & WMASK] = t->mhead[hv];
        t->mhead[hv] = (uint32_t)i;
    }
    if (b == t->last[0])
        t->run = t->run < 255 ? (uint8_t)(t->run + 1) : 255;
    else
        t->run = 0;
    t->last[3] = t->last[2];
    t->last[2] = t->last[1];
    t->last[1] = t->last[0];
    t->last[0] = b;
    if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9'))
        t->word = t->word * 263u + b;
    else
        t->word = 0;
    t->rec_i++;
    t->pos++;
    t->c0 = 1;
    if (t->pos == 4096) t->rec_w = rec_width(t->src, 4096);
}

int tnssrc_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    Bytes io = {0};
    TN t;
    if (tn_init(&t, &io, in, n, 1)) return -1;
    for (size_t i = 0; i < n; i++) {
        spine_row(&t);
        uint8_t b = in[i];
        for (int bp = 7; bp >= 0; bp--) {
            int tvec[NHEAD], mx = 0;
            uint32_t h[NMAP];
            int p = predict(&t, bp, tvec, &mx, h);
            int y = (b >> bp) & 1;
            if (rc_encode(&t, y, p)) {
                tn_free(&t);
                free(io.p);
                return -1;
            }
            update(&t, y, p, bp, tvec, mx, h);
            t.c0 = (uint8_t)((t.c0 << 1) | y);
        }
        commit_row(&t, b);
    }
    if (rc_flush(&t)) {
        tn_free(&t);
        free(io.p);
        return -1;
    }
    tn_free(&t);
    *out = io.p;
    *on = io.n;
    return 0;
}

int tnssrc_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on) {
    uint8_t *y = calloc(orig ? orig : 1, 1);
    if (!y) return -1;
    Bytes io = {.p = (uint8_t *)(uintptr_t)in, .n = n, .cap = n, .i = 0};
    TN t;
    if (tn_init(&t, &io, y, orig, 0)) {
        free(y);
        return -1;
    }
    t.x = 0;
    for (int k = 0; k < 4; k++) t.x = (t.x << 8) | b_get(&io);
    for (size_t i = 0; i < orig; i++) {
        spine_row(&t);
        uint8_t b = 0;
        for (int bp = 7; bp >= 0; bp--) {
            int tvec[NHEAD], mx = 0;
            uint32_t h[NMAP];
            int p = predict(&t, bp, tvec, &mx, h);
            int bit = rc_decode(&t, p);
            update(&t, bit, p, bp, tvec, mx, h);
            t.c0 = (uint8_t)((t.c0 << 1) | bit);
            b = (uint8_t)((b << 1) | bit);
        }
        y[i] = b;
        commit_row(&t, b);
    }
    tn_free(&t);
    *out = y;
    *on = orig;
    return 0;
}
