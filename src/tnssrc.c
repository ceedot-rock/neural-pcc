/* TNSSRC — copies + FastCM literals. Three heads, one spine per row.
 * MATCH emits (len,dist) copies, not 8 predicted bits. LOCAL FastCM on lits.
 * ROW record/run inside FastCM. Decoder mirrors. Proprietary Slid Phi Labs. */
#include "tnssrc.h"

#include <stdlib.h>
#include <string.h>

#define N 8
#define LBITS 16
#define LSZ (1u << LBITS)
#define LMASK (LSZ - 1)
#define HBITS 20
#define HSZ (1u << HBITS)
#define WIN (1u << 23)
#define WMASK (WIN - 1)
#define CHAIN 32
#define MINM 4
#define MAXM 273
#define PINIT 1024
#define LR 0.04f

typedef struct {
    uint8_t *p;
    size_t n, cap, i;
} Bytes;

static int b_push(Bytes *b, uint8_t x) {
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
static uint8_t b_get(Bytes *b) {
    if (b->i >= b->n) return 0;
    return b->p[b->i++];
}

static uint32_t hash4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t v = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
    return (v * 0x9E3779B1u) >> (32 - HBITS);
}

static size_t match_len(const uint8_t *s, size_t i, size_t j, size_t cap, size_t n) {
    size_t max = cap;
    if (i >= n || j >= n) return 0;
    if (n - i < max) max = n - i;
    if (n - j < max) max = n - j;
    size_t k = 0;
    while (k + 8 <= max) {
        uint64_t a, b;
        memcpy(&a, s + i + k, 8);
        memcpy(&b, s + j + k, 8);
        if (a != b) break;
        k += 8;
    }
    while (k < max && s[i + k] == s[j + k]) k++;
    return k;
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
        for (size_t i = w; i < lim; i++, tot++)
            if (s[i] == s[i - w]) hits++;
        if (tot && hits * 3 > tot && hits > best) {
            best = hits;
            best_w = w;
        }
    }
    return best_w;
}

typedef struct {
    uint16_t t[N * LSZ];
    float w[N];
    uint16_t sse[64];
    uint16_t p_match, p_rep, p_len[32], p_dist[3];
    uint32_t *mhead, *mprev;
    uint32_t reps[4];
    uint8_t last[4], run, mlen_nib;
    uint32_t c0, bpos, word;
    size_t rec_w, rec_i, nsrc;
    const uint8_t *src;
    uint8_t *dst;
    uint64_t low, high, code;
    Bytes *io;
    int enc;
} TN;

static uint16_t clamp11(int p) {
    if (p < 1) return 1;
    if (p > 2047) return 2047;
    return (uint16_t)p;
}
static void p_upd(uint16_t *p, uint32_t bit) {
    int x = *p;
    if (bit == 0)
        x += (2048 - x) >> 5;
    else
        x -= x >> 5;
    *p = clamp11(x);
}

static int tn_init(TN *t, Bytes *io, const uint8_t *src, uint8_t *dst, size_t n, int enc) {
    memset(t, 0, sizeof *t);
    t->mhead = malloc(HSZ * 4);
    t->mprev = malloc(WIN * 4);
    if (!t->mhead || !t->mprev) {
        free(t->mhead);
        free(t->mprev);
        return -1;
    }
    memset(t->mhead, 0xFF, HSZ * 4);
    memset(t->mprev, 0xFF, WIN * 4);
    for (int i = 0; i < N * (int)LSZ; i++) t->t[i] = PINIT;
    t->w[0] = 0.45f;
    t->w[1] = 0.35f;
    t->w[2] = 0.22f;
    t->w[3] = 0.28f;
    t->w[4] = 0.20f;
    t->w[5] = 0.18f;
    t->w[6] = 0.25f;
    t->w[7] = 0.15f;
    for (int i = 0; i < 64; i++) t->sse[i] = PINIT;
    t->p_match = PINIT;
    t->p_rep = PINIT;
    for (int i = 0; i < 32; i++) t->p_len[i] = PINIT;
    for (int i = 0; i < 3; i++) t->p_dist[i] = PINIT;
    t->c0 = 1;
    t->src = src;
    t->dst = dst;
    t->nsrc = n;
    t->io = io;
    t->enc = enc;
    t->low = 0;
    t->high = ~0ull;
    t->rec_w = 0;
    return 0;
}
static void tn_free(TN *t) {
    free(t->mhead);
    free(t->mprev);
    t->mhead = t->mprev = NULL;
}

static void insert(TN *t, size_t i) {
    if (i + 3 >= t->nsrc) return;
    const uint8_t *s = t->enc ? t->src : t->dst;
    uint32_t h = hash4(s[i], s[i + 1], s[i + 2], s[i + 3]);
    t->mprev[i & WMASK] = t->mhead[h];
    t->mhead[h] = (uint32_t)i;
}

static void find_match(TN *t, size_t i, uint32_t *olen, uint32_t *odist) {
    const uint8_t *s = t->enc ? t->src : t->dst;
    size_t n = t->nsrc;
    *olen = 0;
    *odist = 0;
    if (i + MINM > n) return;
    uint32_t best = 0, bestd = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t d = t->reps[r];
        if (!d || i < d) continue;
        uint32_t L = (uint32_t)match_len(s, i, i - d, MAXM, n);
        if (L > best) {
            best = L;
            bestd = d;
        }
    }
    if (i + 4 > n) {
        if (best >= MINM) {
            *olen = best;
            *odist = bestd;
        }
        return;
    }
    uint32_t h = hash4(s[i], s[i + 1], s[i + 2], s[i + 3]);
    uint32_t p = t->mhead[h];
    int chain = 0;
    while (p != 0xFFFFFFFFu && chain++ < CHAIN) {
        if (p >= i || i - p > WIN) break;
        uint32_t L = (uint32_t)match_len(s, i, p, MAXM, n);
        uint32_t d = (uint32_t)(i - p);
        if (L > best) {
            best = L;
            bestd = d;
        }
        uint32_t nxt = t->mprev[p & WMASK];
        if (nxt >= p) break;
        p = nxt;
    }
    if (best >= MINM) {
        *olen = best;
        *odist = bestd;
    }
}

static int rc_bit(TN *t, uint32_t bit, uint16_t *pp) {
    uint16_t p = *pp;
    if (p < 1) p = 1;
    if (p > 2047) p = 2047;
    uint64_t span = t->high - t->low;
    uint64_t mid = t->low + (span >> 11) * (uint64_t)p;
    if (t->enc) {
        if (bit == 0)
            t->high = mid;
        else
            t->low = mid + 1;
        for (;;) {
            if ((t->low >> 56) != (t->high >> 56)) break;
            if (b_push(t->io, (uint8_t)(t->low >> 56))) return -1;
            t->low <<= 8;
            t->high = (t->high << 8) | 0xFFu;
        }
        p_upd(pp, bit);
        return 0;
    }
    uint32_t y = t->code <= mid ? 0 : 1;
    if (y == 0)
        t->high = mid;
    else
        t->low = mid + 1;
    for (;;) {
        if ((t->low >> 56) != (t->high >> 56)) break;
        t->low <<= 8;
        t->high = (t->high << 8) | 0xFFu;
        t->code = (t->code << 8) | b_get(t->io);
    }
    p_upd(pp, y);
    return (int)y;
}

static int rc_flush(TN *t) {
    for (int i = 0; i < 8; i++) {
        if (b_push(t->io, (uint8_t)(t->low >> 56))) return -1;
        t->low <<= 8;
    }
    return 0;
}

static void loc_ctx(TN *t, size_t h[N]) {
    size_t n = t->bpos, c = t->c0;
    size_t a = t->last[0], b = t->last[1], d = t->last[2], e = t->last[3];
    h[0] = (c + n * 257 + a * 4099) & LMASK;
    h[1] = (c * 13 + ((a << 8) | b) * 17 + n) & LMASK;
    h[2] = ((t->mlen_nib) * 16 + n + c * 3) & LMASK;
    h[3] = (c * 29 + a * 251 + b * 57 + d * 13 + n) & LMASK;
    h[4] = (t->word ^ (c << 3) ^ n) & LMASK;
    h[5] = (c + ((a ^ d) << 8) + (e << 4) + n * 13) & LMASK;
    size_t col = t->rec_w == 0 ? a : (t->rec_i % t->rec_w) + (a << 6);
    h[6] = (c * 17 + col * 4099 + n) & LMASK;
    h[7] = ((t->run < 63 ? t->run : 63) * 8 + n + (a << 3)) & LMASK;
}

static uint16_t loc_p(TN *t, size_t h[N], float st[N]) {
    float s = 0;
    for (int i = 0; i < N; i++) {
        uint16_t p = t->t[i * LSZ + h[i]];
        if (p < 1) p = 1;
        if (p > 2047) p = 2047;
        st[i] = (float)p - 1024.f;
        s += t->w[i] * st[i];
    }
    int q = (int)(1024.f + s / (float)N);
    if (q < 64) q = 64;
    if (q > 1984) q = 1984;
    int si = q >> 5;
    int q2 = (q + t->sse[si] + 1) / 2;
    if (q2 < 64) q2 = 64;
    if (q2 > 1984) q2 = 1984;
    return (uint16_t)q2;
}

static void loc_upd(TN *t, size_t h[N], float st[N], uint16_t q, uint32_t bit) {
    float err = (1.f - (float)bit) - (q / 2048.f);
    int si = q >> 5;
    int sp = t->sse[si];
    if (bit == 0)
        sp += (2048 - sp) >> 4;
    else
        sp -= sp >> 4;
    t->sse[si] = clamp11(sp);
    int train = err > 0.02f || err < -0.02f;
    for (int i = 0; i < N; i++) {
        if (train) {
            t->w[i] += LR * err * (st[i] / 1024.f);
            if (t->w[i] < -2.f) t->w[i] = -2.f;
            if (t->w[i] > 2.f) t->w[i] = 2.f;
        }
        int slot = (int)(i * LSZ + h[i]);
        int tp = t->t[slot];
        if (bit == 0)
            tp += (2048 - tp) >> 5;
        else
            tp -= tp >> 5;
        t->t[slot] = clamp11(tp);
    }
}

static void commit_byte(TN *t, uint8_t b) {
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
    t->c0 = 1;
    t->bpos = 0;
    if (t->rec_i == 4096) {
        const uint8_t *s = t->enc ? t->src : t->dst;
        t->rec_w = rec_width(s, 4096);
    }
}

static int lit_bit(TN *t, uint32_t bit) {
    size_t h[N];
    float st[N];
    loc_ctx(t, h);
    uint16_t q = loc_p(t, h, st);
    uint16_t pq = q;
    int y;
    if (t->enc) {
        if (rc_bit(t, bit, &pq)) return -1;
        y = (int)bit;
    } else {
        y = rc_bit(t, 0, &pq);
        if (y < 0) return -1;
    }
    loc_upd(t, h, st, q, (uint32_t)y);
    t->c0 = (t->c0 << 1) | (uint32_t)y;
    t->bpos++;
    return y;
}

static int enc_lit(TN *t, uint8_t b) {
    for (int bp = 7; bp >= 0; bp--) {
        if (lit_bit(t, (b >> bp) & 1) < 0) return -1;
    }
    commit_byte(t, b);
    return 0;
}
static int dec_lit(TN *t, uint8_t *b) {
    uint8_t v = 0;
    for (int bp = 7; bp >= 0; bp--) {
        int y = lit_bit(t, 0);
        if (y < 0) return -1;
        v = (uint8_t)((v << 1) | y);
    }
    *b = v;
    commit_byte(t, v);
    return 0;
}

static int bits_n(TN *t, uint32_t *v, int n, uint16_t *model, int enc_val) {
    uint32_t x = enc_val ? *v : 0;
    for (int i = 0; i < n; i++) {
        uint32_t bit = enc_val ? ((x >> i) & 1) : 0;
        if (t->enc) {
            if (rc_bit(t, bit, model)) return -1;
        } else {
            int y = rc_bit(t, 0, model);
            if (y < 0) return -1;
            if (y) x |= 1u << i;
        }
    }
    if (!t->enc) *v = x;
    return 0;
}

static int code_len(TN *t, uint32_t *len, int enc) {
    uint32_t extra = enc ? (*len - MINM) : 0;
    if (enc) {
        uint32_t n = extra < 15 ? extra : 15;
        if (bits_n(t, &n, 4, &t->p_len[0], 1)) return -1;
        if (extra >= 15) {
            uint32_t e = extra - 15;
            uint32_t b = e < 255 ? e : 255;
            if (bits_n(t, &b, 8, &t->p_len[1], 1)) return -1;
            if (e >= 255) {
                uint32_t hi = e - 255;
                if (bits_n(t, &hi, 16, &t->p_len[2], 1)) return -1;
            }
        }
        return 0;
    }
    uint32_t n = 0;
    if (bits_n(t, &n, 4, &t->p_len[0], 0)) return -1;
    if (n < 15) {
        *len = MINM + n;
        return 0;
    }
    uint32_t b = 0;
    if (bits_n(t, &b, 8, &t->p_len[1], 0)) return -1;
    if (b < 255) {
        *len = MINM + 15 + b;
        return 0;
    }
    uint32_t hi = 0;
    if (bits_n(t, &hi, 16, &t->p_len[2], 0)) return -1;
    *len = MINM + 15 + 255 + hi;
    if (*len > MAXM) *len = MAXM;
    return 0;
}

static int code_dist(TN *t, uint32_t *dist, int enc) {
    uint32_t d = enc ? *dist : 0;
    int which = 3;
    if (enc) {
        for (int r = 0; r < 3; r++)
            if (t->reps[r] == d) {
                which = r;
                break;
            }
        if (which < 3) {
            if (rc_bit(t, 1, &t->p_rep)) return -1;
            uint32_t w = (uint32_t)which;
            if (bits_n(t, &w, 2, &t->p_dist[0], 1)) return -1;
            return 0;
        }
        if (rc_bit(t, 0, &t->p_rep)) return -1;
        uint32_t dm = d - 1;
        if (dm < 256) {
            if (rc_bit(t, 0, &t->p_dist[1])) return -1;
            if (bits_n(t, &dm, 8, &t->p_dist[2], 1)) return -1;
        } else if (dm < 65536) {
            if (rc_bit(t, 1, &t->p_dist[1])) return -1;
            if (rc_bit(t, 0, &t->p_dist[0])) return -1;
            if (bits_n(t, &dm, 16, &t->p_dist[2], 1)) return -1;
        } else {
            if (rc_bit(t, 1, &t->p_dist[1])) return -1;
            if (rc_bit(t, 1, &t->p_dist[0])) return -1;
            if (bits_n(t, &dm, 24, &t->p_dist[2], 1)) return -1;
        }
        return 0;
    }
    int isrep = rc_bit(t, 0, &t->p_rep);
    if (isrep < 0) return -1;
    if (isrep) {
        uint32_t w = 0;
        if (bits_n(t, &w, 2, &t->p_dist[0], 0)) return -1;
        *dist = t->reps[w & 3];
        if (!*dist) return -1;
        return 0;
    }
    int b1 = rc_bit(t, 0, &t->p_dist[1]);
    if (b1 < 0) return -1;
    uint32_t dm = 0;
    if (b1 == 0) {
        if (bits_n(t, &dm, 8, &t->p_dist[2], 0)) return -1;
    } else {
        int b0 = rc_bit(t, 0, &t->p_dist[0]);
        if (b0 < 0) return -1;
        if (b0 == 0) {
            if (bits_n(t, &dm, 16, &t->p_dist[2], 0)) return -1;
        } else if (bits_n(t, &dm, 24, &t->p_dist[2], 0))
            return -1;
    }
    *dist = dm + 1;
    return 0;
}

static void bump_reps(TN *t, uint32_t d) {
    if (t->reps[0] == d) return;
    t->reps[3] = t->reps[2];
    t->reps[2] = t->reps[1];
    t->reps[1] = t->reps[0];
    t->reps[0] = d;
}

static void apply_match(TN *t, size_t i, uint32_t len, uint32_t dist) {
    uint8_t *s = t->enc ? (uint8_t *)(uintptr_t)t->src : t->dst;
    for (uint32_t k = 0; k < len; k++) {
        uint8_t b = s[i + k - dist];
        if (!t->enc) t->dst[i + k] = b;
        insert(t, i + k);
        commit_byte(t, b);
    }
    t->mlen_nib = (uint8_t)(len < 15 ? len : 15);
    bump_reps(t, dist);
}

static int code_file(TN *t) {
    size_t n = t->nsrc;
    size_t i = 0;
    while (i < n) {
        uint32_t len = 0, dist = 0;
        if (t->enc) {
            find_match(t, i, &len, &dist);
            if (len >= MINM && i + 1 + MINM <= n) {
                uint32_t len2 = 0, dist2 = 0;
                find_match(t, i + 1, &len2, &dist2);
                if (len2 > len) len = 0;
            }
        }
        uint32_t is_m = t->enc ? (len >= MINM) : 0;
        if (t->enc) {
            if (rc_bit(t, is_m, &t->p_match)) return -1;
        } else {
            int y = rc_bit(t, 0, &t->p_match);
            if (y < 0) return -1;
            is_m = (uint32_t)y;
        }
        if (is_m) {
            if (t->enc) {
                if (code_len(t, &len, 1)) return -1;
                if (code_dist(t, &dist, 1)) return -1;
            } else {
                if (code_len(t, &len, 0)) return -1;
                if (code_dist(t, &dist, 0)) return -1;
                if (!dist || dist > i || i + len > n) return -1;
            }
            apply_match(t, i, len, dist);
            i += len;
        } else {
            if (t->enc) {
                if (enc_lit(t, t->src[i])) return -1;
                insert(t, i);
            } else {
                uint8_t b;
                if (dec_lit(t, &b)) return -1;
                t->dst[i] = b;
                insert(t, i);
            }
            t->mlen_nib = 0;
            i++;
        }
    }
    return 0;
}

int tnssrc_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    Bytes io = {0};
    TN t;
    if (tn_init(&t, &io, in, NULL, n, 1)) return -1;
    if (code_file(&t) || rc_flush(&t)) {
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
    if (tn_init(&t, &io, y, y, orig, 0)) {
        free(y);
        return -1;
    }
    t.code = 0;
    for (int k = 0; k < 8; k++) t.code = (t.code << 8) | b_get(&io);
    if (code_file(&t)) {
        tn_free(&t);
        free(y);
        return -1;
    }
    tn_free(&t);
    *out = y;
    *on = orig;
    return 0;
}
