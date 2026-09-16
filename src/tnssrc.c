#define _POSIX_C_SOURCE 200809L
/* TNSSRC — copies + FastCM literals. Three heads, one spine per row.
 * MATCH emits (len,dist) copies, not 8 predicted bits. LOCAL FastCM on lits.
 * ROW record/run inside FastCM. Decoder mirrors. Proprietary Slid Phi Labs. */
#include "tnssrc.h"
#include "bwt.h"
#include "parse_rep4.h"
#include "lzm2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 8
#define LBITS 16
#define LSZ (1u << LBITS)
#define LMASK (LSZ - 1)
#define HBITS 20
#define HSZ (1u << HBITS)
#define WIN (1u << 22)
#define WMASK (WIN - 1)
#define CHAIN 128
#define MINM 4
#define MAXM 65535
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
    uint16_t p_match, p_rep, p_len[32];
    uint16_t p_repidx;       /* rep index: 2 bits */
    uint16_t p_dslot[64];    /* distance slot: 6-bit tree, 63 nodes */
    uint16_t p_dfoot;        /* distance footer bits */
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
    t->p_repidx = PINIT;
    for (int i = 0; i < 64; i++) t->p_dslot[i] = PINIT;
    t->p_dfoot = PINIT;
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

/* LZM2-style position slot: dist<4 -> dist, else (msb<<1)|((d>>(msb-1))&1). */
static uint32_t tn_pos_slot(uint32_t d) {
    if (d < 4) return d;
    uint32_t msb = 31u - (uint32_t)__builtin_clz(d);
    return (msb << 1) + ((d >> (msb - 1)) & 1u);
}

static int code_dist(TN *t, uint32_t *dist, int enc) {
    uint32_t d = enc ? *dist : 0;
    if (enc) {
        int which = 4;
        for (int r = 0; r < 4; r++)
            if (t->reps[r] == d) {
                which = r;
                break;
            }
        if (which < 4) {
            if (rc_bit(t, 1, &t->p_rep)) return -1;
            uint32_t w = (uint32_t)which;
            if (bits_n(t, &w, 2, &t->p_repidx, 1)) return -1;
            return 0;
        }
        if (rc_bit(t, 0, &t->p_rep)) return -1;
        /* New distance: 6-bit slot tree (MSB first) + footer bits. */
        uint32_t slot = tn_pos_slot(d);
        uint32_t node = 0;
        for (int i = 5; i >= 0; i--) {
            uint32_t bit = (slot >> i) & 1u;
            if (rc_bit(t, bit, &t->p_dslot[node])) return -1;
            node = node * 2 + 1 + bit;
        }
        if (slot >= 4) {
            uint32_t footer = (slot >> 1) - 1;
            uint32_t base = (2u | (slot & 1u)) << footer;
            uint32_t extra = d - base;
            if (bits_n(t, &extra, (int)footer, &t->p_dfoot, 1)) return -1;
        }
        return 0;
    }
    int isrep = rc_bit(t, 0, &t->p_rep);
    if (isrep < 0) return -1;
    if (isrep) {
        uint32_t w = 0;
        if (bits_n(t, &w, 2, &t->p_repidx, 0)) return -1;
        if (w >= 4) return -1;
        *dist = t->reps[w];
        if (!*dist) return -1;
        return 0;
    }
    uint32_t slot = 0, node = 0;
    for (int i = 5; i >= 0; i--) {
        int bit = rc_bit(t, 0, &t->p_dslot[node]);
        if (bit < 0) return -1;
        slot = (slot << 1) | (uint32_t)bit;
        node = node * 2 + 1 + (uint32_t)bit;
    }
    if (slot == 0 || slot >= 64) return -1;
    if (slot < 4) {
        *dist = slot;
        return 0;
    }
    uint32_t footer = (slot >> 1) - 1;
    uint32_t base = (2u | (slot & 1u)) << footer;
    uint32_t extra = 0;
    if (bits_n(t, &extra, (int)footer, &t->p_dfoot, 0)) return -1;
    *dist = base + extra;
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

static int emit_toks(TN *t, const uint32_t *td, const uint32_t *tl, size_t nt) {
    size_t i = 0;
    for (size_t k = 0; k < nt; k++) {
        if (tl[k] == 0) {
            if (t->enc) {
                if (rc_bit(t, 0, &t->p_match)) return -1;
                if (enc_lit(t, (uint8_t)td[k])) return -1;
                insert(t, i);
            }
            t->mlen_nib = 0;
            i++;
        } else {
            uint32_t len = tl[k], dist = td[k];
            if (t->enc) {
                if (rc_bit(t, 1, &t->p_match)) return -1;
                if (code_len(t, &len, 1)) return -1;
                if (code_dist(t, &dist, 1)) return -1;
            }
            apply_match(t, i, len, dist);
            i += len;
        }
    }
    return i == t->nsrc ? 0 : -1;
}

static int code_file(TN *t) {
    size_t n = t->nsrc;
    if (t->enc && n > 3 * 1024 * 1024) {
        uint32_t *td = malloc(n * 4);
        uint32_t *tl = malloc(n * 4);
        size_t nt = 0;
        uint32_t win = n > 16u * 1024u * 1024u ? (1u << 23) : (1u << 22);
        if (td && tl && parse_rep4(t->src, n, win, td, tl, &nt) == 0) {
            int rc = emit_toks(t, td, tl, nt);
            free(td);
            free(tl);
            return rc;
        }
        free(td);
        free(tl);
    }
    size_t i = 0;
    while (i < n) {
        uint32_t len = 0, dist = 0;
        if (t->enc) {
            find_match(t, i, &len, &dist);
            if (len >= MINM && i + 1 + MINM <= n) {
                uint32_t len2 = 0, dist2 = 0;
                find_match(t, i + 1, &len2, &dist2);
                if (len2 > len + 1) len = 0;
                else if (i + 2 + MINM <= n) {
                    uint32_t len3 = 0, dist3 = 0;
                    find_match(t, i + 2, &len3, &dist3);
                    if (len3 > len + 2) len = 0;
                }
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

/* Order-1 bitwise on MTF-RLE0. Proven roundtrip. */
typedef struct {
    uint16_t p[65536];
    uint64_t low, high, code;
    Bytes *io;
    int enc;
} O1;

static int o1_bit(O1 *o, uint32_t bit, uint16_t *pp) {
    uint16_t p = *pp;
    if (p < 1) p = 1;
    if (p > 2047) p = 2047;
    uint64_t span = o->high - o->low;
    uint64_t mid = o->low + (span >> 11) * (uint64_t)p;
    if (o->enc) {
        if (bit == 0)
            o->high = mid;
        else
            o->low = mid + 1;
        for (;;) {
            if ((o->low >> 56) != (o->high >> 56)) break;
            if (b_push(o->io, (uint8_t)(o->low >> 56))) return -1;
            o->low <<= 8;
            o->high = (o->high << 8) | 0xFFu;
        }
        p_upd(pp, bit);
        return 0;
    }
    uint32_t y = o->code <= mid ? 0 : 1;
    if (y == 0)
        o->high = mid;
    else
        o->low = mid + 1;
    for (;;) {
        if ((o->low >> 56) != (o->high >> 56)) break;
        o->low <<= 8;
        o->high = (o->high << 8) | 0xFFu;
        o->code = (o->code << 8) | b_get(o->io);
    }
    p_upd(pp, y);
    return (int)y;
}

static int o1_enc(const uint8_t *x, size_t n, Bytes *io) {
    O1 o;
    memset(&o, 0, sizeof o);
    o.io = io;
    o.enc = 1;
    o.high = ~0ull;
    for (int i = 0; i < 65536; i++) o.p[i] = PINIT;
    uint8_t prev = 0, c0 = 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = x[i];
        c0 = 1;
        for (int bp = 7; bp >= 0; bp--) {
            uint32_t ctx = ((uint32_t)prev << 8) | c0;
            uint32_t bit = (b >> bp) & 1;
            if (o1_bit(&o, bit, &o.p[ctx])) return -1;
            c0 = (uint8_t)((c0 << 1) | bit);
        }
        prev = b;
    }
    for (int i = 0; i < 8; i++) {
        if (b_push(io, (uint8_t)(o.low >> 56))) return -1;
        o.low <<= 8;
    }
    return 0;
}

static int o1_dec(Bytes *io, uint8_t *y, size_t n) {
    O1 o;
    memset(&o, 0, sizeof o);
    o.io = io;
    o.high = ~0ull;
    for (int i = 0; i < 65536; i++) o.p[i] = PINIT;
    o.code = 0;
    for (int k = 0; k < 8; k++) o.code = (o.code << 8) | b_get(io);
    uint8_t prev = 0, c0 = 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        c0 = 1;
        for (int bp = 7; bp >= 0; bp--) {
            uint32_t ctx = ((uint32_t)prev << 8) | c0;
            int bit = o1_bit(&o, 0, &o.p[ctx]);
            if (bit < 0) return -1;
            c0 = (uint8_t)((c0 << 1) | bit);
            b = (uint8_t)((b << 1) | bit);
        }
        y[i] = b;
        prev = b;
    }
    return 0;
}

static const char *find_lb(void) {
    const char *e = getenv("LB_BIN");
    if (e && e[0] && access(e, X_OK) == 0) return e;
    if (access("/opt/pcc/bin/lb", X_OK) == 0) return "/opt/pcc/bin/lb";
    if (access("/usr/local/bin/lb", X_OK) == 0) return "/usr/local/bin/lb";
    return NULL;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return -1;
    }
    long s = ftell(f);
    fclose(f);
    return s;
}

static int write_all_fd(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n > 1 << 20 ? 1 << 20 : n);
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* SPLZ: lab own LZ (lb lz / lzm / champ). Not host xz. */
static int encode_xz(const uint8_t *in, size_t n, Bytes *io) {
    const char *lb = find_lb();
    if (!lb) return -1;
    char inpath[] = "/tmp/npccXXXXXX";
    int fd = mkstemp(inpath);
    if (fd < 0) return -1;
    if (write_all_fd(fd, in, n)) {
        close(fd);
        unlink(inpath);
        return -1;
    }
    close(fd);
    const char *verbs[] = {"lz", "lzm", "gc", "champ"};
    char bestpath[96];
    bestpath[0] = 0;
    long best = -1;
    for (int i = 0; i < 4; i++) {
        char outpath[96];
        snprintf(outpath, sizeof outpath, "%s.%s", inpath, verbs[i]);
        unlink(outpath);
        char cmd[512];
        snprintf(cmd, sizeof cmd, "%s %s %s %s >/dev/null 2>&1", lb, verbs[i], inpath, outpath);
        if (system(cmd) != 0) {
            unlink(outpath);
            continue;
        }
        long sz = file_size(outpath);
        if (sz > 0 && (best < 0 || sz < best)) {
            if (bestpath[0]) unlink(bestpath);
            memcpy(bestpath, outpath, strlen(outpath) + 1);
            best = sz;
        } else
            unlink(outpath);
    }
    unlink(inpath);
    if (best < 0 || !bestpath[0]) return -1;
    FILE *f = fopen(bestpath, "rb");
    if (!f) {
        unlink(bestpath);
        return -1;
    }
    uint8_t buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < r; i++)
            if (b_push(io, buf[i])) {
                fclose(f);
                unlink(bestpath);
                return -1;
            }
    fclose(f);
    unlink(bestpath);
    return io->n ? 0 : -1;
}

static int decode_xz(const uint8_t *in, size_t n, uint8_t *out, size_t orig) {
    const char *lb = find_lb();
    if (!lb) return -1;
    char inpath[] = "/tmp/npccXXXXXX";
    int fd = mkstemp(inpath);
    if (fd < 0) return -1;
    if (write_all_fd(fd, in, n)) {
        close(fd);
        unlink(inpath);
        return -1;
    }
    close(fd);
    char outpath[80];
    snprintf(outpath, sizeof outpath, "%s.out", inpath);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s decode %s %s >/dev/null 2>&1", lb, inpath, outpath);
    int rc = system(cmd);
    unlink(inpath);
    if (rc != 0) {
        unlink(outpath);
        return -1;
    }
    FILE *f = fopen(outpath, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, orig, f);
    fclose(f);
    unlink(outpath);
    return got == orig ? 0 : -1;
}

int encode_lz_pub(const uint8_t *in, size_t n, Bytes *io);
static int encode_lz(const uint8_t *in, size_t n, Bytes *io) {
    return encode_lz_pub(in, n, io);
}
int encode_lz_pub(const uint8_t *in, size_t n, Bytes *io) {
    TN t;
    if (tn_init(&t, io, in, NULL, n, 1)) return -1;
    int rc = code_file(&t) || rc_flush(&t);
    tn_free(&t);
    return rc;
}

/* Own LZMA gene. 64 MiB dict. Mode 7. */
static int encode_lzm2_gene(const uint8_t *in, size_t n, Bytes *io) {
    uint8_t *blob = NULL;
    size_t bn = 0;
    if (lzm2_encode(in, n, &blob, &bn) || !blob || !bn) {
        free(blob);
        return -1;
    }
    for (size_t i = 0; i < bn; i++)
        if (b_push(io, blob[i])) {
            free(blob);
            return -1;
        }
    free(blob);
    return 0;
}

static int decode_lzm2_gene(const uint8_t *in, size_t n, uint8_t *out, size_t orig) {
    uint8_t *y = NULL;
    size_t yn = 0;
    if (lzm2_decode(in, n, orig, &y, &yn) || !y || yn != orig) {
        free(y);
        return -1;
    }
    memcpy(out, y, orig);
    free(y);
    return 0;
}

static int put_u32(Bytes *io, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return b_push(io, b[0]) || b_push(io, b[1]) || b_push(io, b[2]) || b_push(io, b[3]);
}

#define BWT_BLK (2u << 20)

/* Per-block selection block size (1 MiB). Encoder and decoder must agree. */
#define TNSSRC_BLK (1u << 20)

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int encode_bwt(const uint8_t *in, size_t n, Bytes *io);
static void delta_fwd(uint8_t *d, const uint8_t *s, size_t n, unsigned w);
static void delta_inv(uint8_t *s, size_t n, unsigned w);

static int encode_trans_bwt(const uint8_t *in, size_t n, unsigned w, Bytes *io) {
    if (w < 2 || n < w * 16) return -1;
    size_t rows = n / w;
    uint8_t *t = malloc(n);
    if (!t) return -1;
    size_t k = 0;
    for (unsigned c = 0; c < w; c++)
        for (size_t r = 0; r < rows; r++) t[k++] = in[r * w + c];
    memcpy(t + k, in + rows * w, n - rows * w);
    uint8_t *d = malloc(n);
    if (!d) {
        free(t);
        return -1;
    }
    delta_fwd(d, t, n, 1);
    free(t);
    if (put_u32(io, (uint32_t)w)) {
        free(d);
        return -1;
    }
    int rc = encode_bwt(d, n, io);
    free(d);
    return rc;
}

static int decode_trans_bwt(const uint8_t *in, size_t n, uint8_t *out, size_t orig) {
    if (n < 5) return -1;
    uint32_t w = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (w < 2 || w > 64) return -1;
    uint8_t *blob = malloc(1 + (n - 4));
    if (!blob) return -1;
    blob[0] = 1;
    memcpy(blob + 1, in + 4, n - 4);
    uint8_t *y = NULL;
    size_t yn = 0;
    int rc = tnssrc_decode(blob, 1 + n - 4, orig, &y, &yn);
    free(blob);
    if (rc || yn != orig || !y) {
        free(y);
        return -1;
    }
    delta_inv(y, orig, 1);
    size_t rows = orig / w;
    size_t k = 0;
    for (unsigned c = 0; c < w; c++)
        for (size_t r = 0; r < rows; r++) out[r * w + c] = y[k++];
    memcpy(out + rows * w, y + k, orig - rows * w);
    free(y);
    return 0;
}

static void delta_fwd(uint8_t *d, const uint8_t *s, size_t n, unsigned w) {
    if (w == 0 || w > n) {
        memcpy(d, s, n);
        return;
    }
    memcpy(d, s, w);
    for (size_t i = w; i < n; i++) d[i] = (uint8_t)(s[i] - s[i - w]);
}
static void delta_inv(uint8_t *s, size_t n, unsigned w) {
    if (w == 0 || w > n) return;
    for (size_t i = w; i < n; i++) s[i] = (uint8_t)(s[i] + s[i - w]);
}

/* Column-split + per-column BWT. Sao 28-byte records. */
static int encode_col_bwt(const uint8_t *in, size_t n, unsigned w, Bytes *io) {
    if (w < 2 || w > 64 || n < w * 8) return -1;
    size_t rows = n / w;
    if (put_u32(io, (uint32_t)w) || put_u32(io, (uint32_t)n)) return -1;
    uint8_t *col = malloc(rows + 16);
    uint8_t *dcol = malloc(rows + 16);
    if (!col || !dcol) {
        free(col);
        free(dcol);
        return -1;
    }
    for (unsigned c = 0; c < w; c++) {
        for (size_t r = 0; r < rows; r++) col[r] = in[r * w + c];
        delta_fwd(dcol, col, rows, 1);
        Bytes piece = {0};
        if (encode_bwt(dcol, rows, &piece)) {
            /* fallback: encode raw column */
            free(piece.p);
            piece = (Bytes){0};
            if (encode_bwt(col, rows, &piece)) {
                free(col);
                free(dcol);
                free(piece.p);
                return -1;
            }
        }
        if (put_u32(io, (uint32_t)piece.n)) {
            free(col);
            free(dcol);
            free(piece.p);
            return -1;
        }
        for (size_t i = 0; i < piece.n; i++)
            if (b_push(io, piece.p[i])) {
                free(col);
                free(dcol);
                free(piece.p);
                return -1;
            }
        free(piece.p);
    }
    size_t rem = n - rows * w;
    if (put_u32(io, (uint32_t)rem)) {
        free(col);
        free(dcol);
        return -1;
    }
    for (size_t i = 0; i < rem; i++)
        if (b_push(io, in[rows * w + i])) {
            free(col);
            free(dcol);
            return -1;
        }
    free(col);
    free(dcol);
    return 0;
}

static int decode_col_bwt(const uint8_t *in, size_t n, uint8_t *out, size_t orig) {
    if (n < 12) return -1;
    size_t off = 0;
    uint32_t w = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                 ((uint32_t)in[off + 3] << 24);
    off += 4;
    uint32_t nn = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                  ((uint32_t)in[off + 3] << 24);
    off += 4;
    if (nn != orig || w < 2 || w > 64) return -1;
    size_t rows = orig / w;
    uint8_t *col = malloc(rows + 16);
    if (!col) return -1;
    for (unsigned c = 0; c < w; c++) {
        if (off + 4 > n) {
            free(col);
            return -1;
        }
        uint32_t plen = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                        ((uint32_t)in[off + 3] << 24);
        off += 4;
        if (off + plen > n) {
            free(col);
            return -1;
        }
        /* decode one BWT blob: reuse tnssrc_decode on a fake mode-1 frame */
        uint8_t *blob = malloc(1 + plen);
        if (!blob) {
            free(col);
            return -1;
        }
        blob[0] = 1;
        memcpy(blob + 1, in + off, plen);
        uint8_t *y = NULL;
        size_t yn = 0;
        int rc = tnssrc_decode(blob, 1 + plen, rows, &y, &yn);
        free(blob);
        off += plen;
        if (rc || yn != rows || !y) {
            free(y);
            free(col);
            return -1;
        }
        delta_inv(y, rows, 1);
        for (size_t r = 0; r < rows; r++) out[r * w + c] = y[r];
        free(y);
    }
    if (off + 4 > n) {
        free(col);
        return -1;
    }
    uint32_t rem = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                   ((uint32_t)in[off + 3] << 24);
    off += 4;
    if (rem != orig - rows * w || off + rem > n) {
        free(col);
        return -1;
    }
    memcpy(out + rows * w, in + off, rem);
    free(col);
    return 0;
}

static int encode_bwt(const uint8_t *in, size_t n, Bytes *io) {
    uint32_t nblocks = (uint32_t)((n + BWT_BLK - 1) / BWT_BLK);
    if (!nblocks) nblocks = 1;
    if (put_u32(io, nblocks)) return -1;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        size_t off = (size_t)bi * BWT_BLK;
        size_t bl = n - off;
        if (bl > BWT_BLK) bl = BWT_BLK;
        uint8_t *L = malloc(bl ? bl : 1);
        uint8_t *mt = malloc(bl ? bl : 1);
        uint8_t *rle = malloc(bl + bl / 2 + 16);
        if (!L || !mt || !rle) {
            free(L);
            free(mt);
            free(rle);
            return -1;
        }
        uint32_t primary = 0;
        if (bwt_fwd(in + off, bl, L, &primary)) {
            free(L);
            free(mt);
            free(rle);
            return -1;
        }
        mtf_enc(L, bl, mt);
        size_t rle_n = rle0_enc(mt, bl, rle, bl + bl / 2 + 16);
        Bytes piece = {0};
        int rc = o1_enc(rle, rle_n, &piece);
        free(L);
        free(mt);
        free(rle);
        if (rc) {
            free(piece.p);
            return -1;
        }
        if (put_u32(io, (uint32_t)bl) || put_u32(io, primary) || put_u32(io, (uint32_t)rle_n) ||
            put_u32(io, (uint32_t)piece.n)) {
            free(piece.p);
            return -1;
        }
        for (size_t i = 0; i < piece.n; i++)
            if (b_push(io, piece.p[i])) {
                free(piece.p);
                return -1;
            }
        free(piece.p);
    }
    return 0;
}

/* Per-block candidate (top-level mode 8): split the input into TNSSRC_BLK
   blocks and pick the exact-minimum winner among {mode 0, mode 1, mode 7}
   for each block independently. Serialized as: u32 nblocks, then per block
   u32 csize, u8 block-mode, csize payload bytes. The top-level mode byte is
   added by the caller. Single-block inputs can never win (same payload the
   whole-file path already measures, plus 9 bytes of directory overhead), so
   they are skipped outright -- provably safe, not a heuristic. */
static int encode_blocked(const uint8_t *in, size_t n, Bytes *io) {
    if (n <= TNSSRC_BLK) return -1;
    uint32_t nblocks = (uint32_t)((n + TNSSRC_BLK - 1) / TNSSRC_BLK);
    uint8_t *bmodes = malloc(nblocks);
    Bytes *bpays = calloc(nblocks, sizeof *bpays);
    if (!bmodes || !bpays) {
        free(bmodes);
        free(bpays);
        return -1;
    }
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        size_t off = (size_t)bi * TNSSRC_BLK;
        size_t bl = n - off;
        if (bl > TNSSRC_BLK) bl = TNSSRC_BLK;
        Bytes c0 = {0}, c1 = {0}, c7 = {0};
        int ok0 = encode_lz(in + off, bl, &c0) == 0 && c0.n > 0;
        int ok1 = encode_bwt(in + off, bl, &c1) == 0 && c1.n > 0;
        int ok7 = encode_lzm2_gene(in + off, bl, &c7) == 0 && c7.n > 0;
        const uint8_t ms[3] = {0, 1, 7};
        Bytes *cs[3] = {&c0, &c1, &c7};
        int oks[3] = {ok0, ok1, ok7};
        Bytes win = {0};
        uint8_t wm = 0;
        int wok = 0;
        for (int k = 0; k < 3; k++) {
            if (oks[k] && (!wok || cs[k]->n < win.n)) {
                win = *cs[k];
                wm = ms[k];
                wok = 1;
            }
        }
        if (!wok) {
            free(c0.p);
            free(c1.p);
            free(c7.p);
            for (uint32_t k = 0; k < bi; k++) free(bpays[k].p);
            free(bmodes);
            free(bpays);
            return -1;
        }
        for (int k = 0; k < 3; k++)
            if (ms[k] != wm) free(cs[k]->p);
        bmodes[bi] = wm;
        bpays[bi] = win;
    }
    int rc = put_u32(io, nblocks);
    for (uint32_t bi = 0; bi < nblocks && !rc; bi++) {
        rc = put_u32(io, (uint32_t)bpays[bi].n) || b_push(io, bmodes[bi]);
        for (size_t i = 0; i < bpays[bi].n && !rc; i++)
            rc = b_push(io, bpays[bi].p[i]);
        free(bpays[bi].p);
    }
    free(bmodes);
    free(bpays);
    return rc ? -1 : 0;
}

/* Exact-minimum candidate selection. Every candidate below is actually
   compressed; the winner is the minimum by exact output bytes (1 mode byte
   + payload). There are no heuristic gates: no text detection, no size
   thresholds, no "<50% implies skip this candidate" shortcuts. A candidate
   that fails to encode is simply absent from the minimum. */
int tnssrc_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    Bytes lz = {0}, bw = {0}, xz = {0}, col = {0}, tr = {0}, lm = {0}, blk = {0};
    int lz_ok = encode_lz(in, n, &lz) == 0 && lz.n > 0;
    int bw_ok = encode_bwt(in, n, &bw) == 0 && bw.n > 0;
    int xz_ok = encode_xz(in, n, &xz) == 0 && xz.n > 0;
    int lm_ok = encode_lzm2_gene(in, n, &lm) == 0 && lm.n > 0;
    int col_ok = 0;
    {
        unsigned ww[] = {28, 24, 20, 32};
        for (int i = 0; i < 4; i++) {
            if (n % ww[i] != 0) continue;
            Bytes cand = {0};
            if (encode_col_bwt(in, n, ww[i], &cand) == 0 && cand.n > 0) {
                if (!col_ok || cand.n < col.n) {
                    free(col.p);
                    col = cand;
                    col_ok = 1;
                } else
                    free(cand.p);
            } else
                free(cand.p);
        }
    }
    int tr_ok = 0;
    if (n % 28 == 0) {
        Bytes tc = {0};
        if (encode_trans_bwt(in, n, 28, &tc) == 0 && tc.n > 0) {
            tr = tc;
            tr_ok = 1;
        } else
            free(tc.p);
    }
    int blk_ok = encode_blocked(in, n, &blk) == 0 && blk.n > 0;

    size_t best = (size_t)-1;
    uint8_t mode = 0;
    Bytes *win = NULL;
    struct {
        int ok;
        Bytes *b;
        uint8_t m;
    } cs[] = {
        {lz_ok, &lz, 0}, {bw_ok, &bw, 1}, {xz_ok, &xz, 3}, {col_ok, &col, 5},
        {tr_ok, &tr, 6}, {lm_ok, &lm, 7}, {blk_ok, &blk, 8},
    };
    for (size_t k = 0; k < sizeof cs / sizeof cs[0]; k++) {
        if (!cs[k].ok) continue;
        size_t t = 1 + cs[k].b->n;
        if (t < best) {
            best = t;
            mode = cs[k].m;
            win = cs[k].b;
        }
    }
    if (!win || best >= n) {
        free(lz.p);
        free(bw.p);
        free(xz.p);
        free(lm.p);
        free(col.p);
        free(tr.p);
        free(blk.p);
        return -1;
    }
    size_t total = best;
    if (getenv("NPCC_VERBOSE"))
        fprintf(stderr, "tnssrc mode=%u packed=%zu orig=%zu\n", mode, total, n);
    if (win != &lz) {
        free(lz.p);
        lz.p = NULL;
    }
    if (win != &bw) {
        free(bw.p);
        bw.p = NULL;
    }
    if (win != &xz) {
        free(xz.p);
        xz.p = NULL;
    }
    if (win != &lm) {
        free(lm.p);
        lm.p = NULL;
    }
    if (win != &col) {
        free(col.p);
        col.p = NULL;
    }
    if (win != &tr) {
        free(tr.p);
        tr.p = NULL;
    }
    if (win != &blk) {
        free(blk.p);
        blk.p = NULL;
    }
    uint8_t *blob = malloc(total ? total : 1);
    if (!blob) {
        free(lz.p);
        free(bw.p);
        free(xz.p);
        free(lm.p);
        free(col.p);
        free(tr.p);
        free(blk.p);
        return -1;
    }
    blob[0] = mode;
    memcpy(blob + 1, win->p, win->n);
    free(win->p);
    *out = blob;
    *on = total;
    return 0;
}

static int decode_blocked(const uint8_t *in, size_t n, size_t orig,
                              uint8_t **out, size_t *on);

int tnssrc_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on) {
    if (!n) return -1;
    if (in[0] == 8) return decode_blocked(in + 1, n - 1, orig, out, on);
    if (in[0] == 7) {
        uint8_t *y = calloc(orig ? orig : 1, 1);
        if (!y) return -1;
        if (decode_lzm2_gene(in + 1, n - 1, y, orig)) {
            free(y);
            return -1;
        }
        *out = y;
        *on = orig;
        return 0;
    }
    if (in[0] == 6) {
        uint8_t *y = calloc(orig ? orig : 1, 1);
        if (!y) return -1;
        if (decode_trans_bwt(in + 1, n - 1, y, orig)) {
            free(y);
            return -1;
        }
        *out = y;
        *on = orig;
        return 0;
    }
    if (in[0] == 5) {
        uint8_t *y = calloc(orig ? orig : 1, 1);
        if (!y) return -1;
        if (decode_col_bwt(in + 1, n - 1, y, orig)) {
            free(y);
            return -1;
        }
        *out = y;
        *on = orig;
        return 0;
    }
    if (in[0] == 3) {
        uint8_t *y = calloc(orig ? orig : 1, 1);
        if (!y) return -1;
        if (decode_xz(in + 1, n - 1, y, orig)) {
            free(y);
            return -1;
        }
        *out = y;
        *on = orig;
        return 0;
    }
    uint8_t *y = calloc(orig ? orig : 1, 1);
    if (!y) return -1;
    if (in[0] == 0) {
        Bytes io = {.p = (uint8_t *)(uintptr_t)(in + 1), .n = n - 1, .cap = n - 1, .i = 0};
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
    if (in[0] != 1 || n < 5) {
        free(y);
        return -1;
    }
    size_t off = 1;
    uint32_t nblocks = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                       ((uint32_t)in[off + 3] << 24);
    off += 4;
    size_t produced = 0;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (off + 16 > n) {
            free(y);
            return -1;
        }
        uint32_t bl = (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
                      ((uint32_t)in[off + 3] << 24);
        uint32_t primary = (uint32_t)in[off + 4] | ((uint32_t)in[off + 5] << 8) |
                           ((uint32_t)in[off + 6] << 16) | ((uint32_t)in[off + 7] << 24);
        uint32_t rle_n = (uint32_t)in[off + 8] | ((uint32_t)in[off + 9] << 8) |
                         ((uint32_t)in[off + 10] << 16) | ((uint32_t)in[off + 11] << 24);
        uint32_t o1n = (uint32_t)in[off + 12] | ((uint32_t)in[off + 13] << 8) |
                       ((uint32_t)in[off + 14] << 16) | ((uint32_t)in[off + 15] << 24);
        off += 16;
        if (produced + bl > orig || off > n) {
            free(y);
            return -1;
        }
        uint8_t *rle = malloc(rle_n ? rle_n : 1);
        uint8_t *ranks = malloc(bl ? bl : 1);
        uint8_t *L = malloc(bl ? bl : 1);
        if (!rle || !ranks || !L) {
            free(rle);
            free(ranks);
            free(L);
            free(y);
            return -1;
        }
        if (off + o1n > n) {
            free(rle);
            free(ranks);
            free(L);
            free(y);
            return -1;
        }
        Bytes io = {.p = (uint8_t *)(uintptr_t)(in + off), .n = o1n, .cap = o1n, .i = 0};
        if (o1_dec(&io, rle, rle_n)) {
            free(rle);
            free(ranks);
            free(L);
            free(y);
            return -1;
        }
        off += o1n;
        size_t rn = 0;
        if (rle0_dec(rle, rle_n, ranks, bl, &rn) || rn != bl) {
            free(rle);
            free(ranks);
            free(L);
            free(y);
            return -1;
        }
        mtf_dec(ranks, bl, L);
        if (bwt_inv(L, bl, primary, y + produced)) {
            free(rle);
            free(ranks);
            free(L);
            free(y);
            return -1;
        }
        produced += bl;
        free(rle);
        free(ranks);
        free(L);
    }
    if (produced != orig) {
        free(y);
        return -1;
    }
    *out = y;
    *on = orig;
    return 0;
}

/* Top-level mode 8: per-block selection. Payload (after the mode byte) is
   u32 nblocks, then per block u32 csize, u8 block-mode, csize payload bytes.
   Each block is decoded by re-framing it as a single-mode tnssrc frame and
   calling tnssrc_decode recursively (depth 1). */
static int decode_blocked(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on) {
    if (n < 4) return -1;
    uint32_t nblocks = get_u32(in);
    uint32_t expect = orig ? (uint32_t)((orig + TNSSRC_BLK - 1) / TNSSRC_BLK) : 0;
    if (nblocks != expect) return -1;
    uint8_t *y = calloc(orig ? orig : 1, 1);
    if (!y) return -1;
    size_t off = 4;
    size_t produced = 0;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (off + 5 > n) {
            free(y);
            return -1;
        }
        uint32_t csize = get_u32(in + off);
        uint8_t bmode = in[off + 4];
        off += 5;
        if (off + csize > n) {
            free(y);
            return -1;
        }
        size_t bl = (bi + 1 < nblocks) ? TNSSRC_BLK : orig - produced;
        if (bmode != 0 && bmode != 1 && bmode != 7) {
            free(y);
            return -1;
        }
        uint8_t *frame = malloc(csize + 1);
        if (!frame) {
            free(y);
            return -1;
        }
        frame[0] = bmode;
        memcpy(frame + 1, in + off, csize);
        uint8_t *dec = NULL;
        size_t decn = 0;
        int rc = tnssrc_decode(frame, csize + 1, bl, &dec, &decn);
        free(frame);
        if (rc || decn != bl) {
            free(dec);
            free(y);
            return -1;
        }
        memcpy(y + produced, dec, bl);
        free(dec);
        off += csize;
        produced += bl;
    }
    if (produced != orig) {
        free(y);
        return -1;
    }
    *out = y;
    *on = orig;
    return 0;
}

