/* LZM2 — lab LZMA gene. 64 MiB dict, hash4+hash3, 4-rep, model-priced optimum.
 * Own C. Not host xz. Proprietary Slid Phi Labs. */
#include "lzm2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LC 3
#define LP 0
#define PB 2
#define LIT_STATES (1u << LC)
#define POS_STATES (1u << PB)
#define LIT_SIZE 0x300
#define STATES 12
#define HASH4_BITS 20
#define HASH4 (1u << HASH4_BITS)
#define HASH3_BITS 16
#define HASH3 (1u << HASH3_BITS)
#define WIN (1u << 26)
#define CHAIN4 96
#define CHAIN3 32
#define MIN_NEW 3
#define MAX_LEN 273
#define MAX_PAIRS 16
#define PINIT 1024
#define kTopValue (1u << 24)
#define kBitModelTotal 2048
#define kNumMoveBits 5

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static int bpush(Buf *b, uint8_t x) {
    if (b->n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = realloc(b->p, c);
        if (!p) return -1;
        b->p = p;
        b->cap = c;
    }
    b->p[b->n++] = x;
    return 0;
}

/* LZMA range coder: 32-bit range, 64-bit low, ShiftLow carry. */
typedef struct {
    uint64_t low;
    uint32_t range;
    uint32_t cache;
    uint64_t cache_size;
    uint32_t code;
    Buf *io;
    const uint8_t *in;
    size_t inn, ii;
    int enc;
    int err;
} RC;

static void rc_init(RC *r, Buf *io, int enc) {
    memset(r, 0, sizeof *r);
    r->io = io;
    r->enc = enc;
    r->range = 0xFFFFFFFFu;
    r->cache_size = 1;
    r->cache = 0;
}

static uint8_t rc_get(RC *r) {
    if (r->ii >= r->inn) return 0;
    return r->in[r->ii++];
}

static void shift_low(RC *r) {
    if ((uint32_t)r->low < 0xFF000000u || (uint32_t)(r->low >> 32) != 0) {
        uint8_t temp = (uint8_t)r->cache;
        do {
            if (bpush(r->io, (uint8_t)(temp + (uint8_t)(r->low >> 32)))) r->err = 1;
            temp = 0xFF;
        } while (--r->cache_size != 0);
        r->cache = (uint32_t)((uint32_t)r->low >> 24);
    }
    r->cache_size++;
    r->low = ((uint32_t)r->low) << 8;
}

static void rc_norm_enc(RC *r) {
    while (r->range < kTopValue) {
        shift_low(r);
        r->range <<= 8;
    }
}

static void rc_norm_dec(RC *r) {
    int guard = 0;
    while (r->range < kTopValue) {
        r->code = (r->code << 8) | rc_get(r);
        r->range <<= 8;
        if (++guard > 8) break;
    }
}

static void p_upd(uint16_t *p, uint32_t bit) {
    uint16_t pr = *p;
    if (pr < 1) pr = 1;
    if (pr > 2047) pr = 2047;
    if (bit == 0)
        pr = (uint16_t)(pr + ((kBitModelTotal - pr) >> kNumMoveBits));
    else
        pr = (uint16_t)(pr - (pr >> kNumMoveBits));
    if (pr < 1) pr = 1;
    if (pr > 2047) pr = 2047;
    *p = pr;
}

static void rc_bit(RC *r, uint32_t bit, uint16_t *p) {
    uint16_t pr = *p;
    if (pr < 1) pr = 1;
    if (pr > 2047) pr = 2047;
    uint32_t bound = (r->range >> 11) * (uint32_t)pr;
    if (bit == 0)
        r->range = bound;
    else {
        r->low += bound;
        r->range -= bound;
    }
    p_upd(p, bit);
    rc_norm_enc(r);
}

static uint32_t rc_bit_get(RC *r, uint16_t *p) {
    uint16_t pr = *p;
    if (pr < 1) pr = 1;
    if (pr > 2047) pr = 2047;
    uint32_t bound = (r->range >> 11) * (uint32_t)pr;
    uint32_t bit;
    if (r->code < bound) {
        r->range = bound;
        bit = 0;
    } else {
        r->code -= bound;
        r->range -= bound;
        bit = 1;
    }
    p_upd(p, bit);
    rc_norm_dec(r);
    return bit;
}

static void rc_direct(RC *r, uint32_t bit) {
    r->range >>= 1;
    if (bit)
        r->low += r->range;
    rc_norm_enc(r);
}

static uint32_t rc_direct_get(RC *r) {
    r->range >>= 1;
    uint32_t bit = 0;
    if (r->code >= r->range) {
        r->code -= r->range;
        bit = 1;
    }
    rc_norm_dec(r);
    return bit;
}

static void rc_flush(RC *r) {
    int i;
    for (i = 0; i < 5; i++) shift_low(r);
}

static void rc_init_dec(RC *r, const uint8_t *in, size_t n) {
    int i;
    r->in = in;
    r->inn = n;
    r->ii = 0;
    r->code = 0;
    r->range = 0xFFFFFFFFu;
    for (i = 0; i < 5; i++) r->code = (r->code << 8) | rc_get(r);
}

typedef struct {
    uint16_t is_match[STATES][POS_STATES];
    uint16_t is_rep[STATES];
    uint16_t is_rep_g0[STATES];
    uint16_t is_rep_g1[STATES];
    uint16_t is_rep_g2[STATES];
    uint16_t is_rep0_long[STATES][POS_STATES];
    uint16_t *lit;
    uint16_t len_choice, len_choice2;
    uint16_t len_low[POS_STATES][8];
    uint16_t len_mid[POS_STATES][8];
    uint16_t len_high[256];
    uint16_t rlen_choice, rlen_choice2;
    uint16_t rlen_low[POS_STATES][8];
    uint16_t rlen_mid[POS_STATES][8];
    uint16_t rlen_high[256];
    uint16_t pos_slot[4][64];
    uint16_t spec[10 * 64];
    uint16_t align[16];
} Model;

static int model_init(Model *m) {
    int i, j;
    memset(m, 0, sizeof *m);
    m->lit = malloc(LIT_STATES * LIT_SIZE * sizeof(uint16_t));
    if (!m->lit) return -1;
    for (i = 0; i < STATES; i++) {
        m->is_rep[i] = m->is_rep_g0[i] = m->is_rep_g1[i] = m->is_rep_g2[i] = PINIT;
        for (j = 0; j < (int)POS_STATES; j++) {
            m->is_match[i][j] = PINIT;
            m->is_rep0_long[i][j] = PINIT;
        }
    }
    for (i = 0; i < (int)(LIT_STATES * LIT_SIZE); i++) m->lit[i] = PINIT;
    m->len_choice = m->len_choice2 = m->rlen_choice = m->rlen_choice2 = PINIT;
    for (i = 0; i < (int)POS_STATES; i++)
        for (j = 0; j < 8; j++) {
            m->len_low[i][j] = m->len_mid[i][j] = PINIT;
            m->rlen_low[i][j] = m->rlen_mid[i][j] = PINIT;
        }
    for (i = 0; i < 256; i++) m->len_high[i] = m->rlen_high[i] = PINIT;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 64; j++) m->pos_slot[i][j] = PINIT;
    for (i = 0; i < 10 * 64; i++) m->spec[i] = PINIT;
    for (i = 0; i < 16; i++) m->align[i] = PINIT;
    return 0;
}

static void model_free(Model *m) {
    free(m->lit);
    m->lit = NULL;
}

static uint32_t hash3(const uint8_t *d, size_t i) {
    uint32_t v = d[i] | ((uint32_t)d[i + 1] << 8) | ((uint32_t)d[i + 2] << 16);
    return (v * 0x9E3779B1u) >> (32 - HASH3_BITS);
}

static uint32_t hash4(const uint8_t *d, size_t i) {
    uint32_t v = d[i] | ((uint32_t)d[i + 1] << 8) | ((uint32_t)d[i + 2] << 16) | ((uint32_t)d[i + 3] << 24);
    return (v * 0x9E3779B1u) >> (32 - HASH4_BITS);
}

static size_t match_len(const uint8_t *d, size_t a, size_t b, size_t cap, size_t n) {
    size_t max = cap;
    if (n - a < max) max = n - a;
    if (n - b < max) max = n - b;
    size_t k = 0;
    while (k + 8 <= max) {
        uint64_t x, y;
        memcpy(&x, d + a + k, 8);
        memcpy(&y, d + b + k, 8);
        if (x != y) break;
        k += 8;
    }
    while (k < max && d[a + k] == d[b + k]) k++;
    return k;
}

static size_t pos_state(size_t i) { return i & (POS_STATES - 1); }
static size_t lit_index(uint8_t prev) { return ((size_t)prev >> (8 - LC)) * LIT_SIZE; }
static size_t next_lit(size_t s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }
static size_t next_match(size_t s) { return s < 7 ? 7 : 10; }
static size_t next_rep(size_t s) { return s < 7 ? 8 : 11; }
static size_t next_short(size_t s) { return s < 7 ? 9 : 11; }

static uint32_t pos_slot(uint32_t dist) {
    if (dist < 4) return dist;
    uint32_t msb = 31u - (uint32_t)__builtin_clz(dist);
    return (msb << 1) + ((dist >> (msb - 1)) & 1);
}

static void enc_tree(RC *e, uint32_t v, uint32_t bits, uint16_t *probs) {
    uint32_t ctx = 1;
    uint32_t i;
    for (i = bits; i-- > 0;) {
        uint32_t bit = (v >> i) & 1;
        rc_bit(e, bit, &probs[ctx]);
        ctx = (ctx << 1) | bit;
    }
}

static uint32_t dec_tree(RC *d, uint32_t bits, uint16_t *probs) {
    uint32_t ctx = 1, i;
    for (i = 0; i < bits; i++) {
        uint32_t bit = rc_bit_get(d, &probs[ctx]);
        ctx = (ctx << 1) | bit;
    }
    return ctx - (1u << bits);
}

static void enc_tree_rev(RC *e, uint32_t v, uint32_t bits, uint16_t *probs) {
    uint32_t ctx = 1, i;
    for (i = 0; i < bits; i++) {
        uint32_t bit = (v >> i) & 1;
        rc_bit(e, bit, &probs[ctx]);
        ctx = (ctx << 1) | bit;
    }
}

static uint32_t dec_tree_rev(RC *d, uint32_t bits, uint16_t *probs) {
    uint32_t ctx = 1, v = 0, i;
    for (i = 0; i < bits; i++) {
        uint32_t bit = rc_bit_get(d, &probs[ctx]);
        v |= bit << i;
        ctx = (ctx << 1) | bit;
    }
    return v;
}

static void enc_len(RC *e, uint32_t len, size_t ps, uint16_t *ch, uint16_t *ch2, uint16_t low[][8],
                    uint16_t mid[][8], uint16_t *high) {
    uint32_t l = len - 2;
    if (l < 8) {
        rc_bit(e, 0, ch);
        enc_tree(e, l, 3, low[ps]);
    } else {
        rc_bit(e, 1, ch);
        if (l < 16) {
            rc_bit(e, 0, ch2);
            enc_tree(e, l - 8, 3, mid[ps]);
        } else {
            rc_bit(e, 1, ch2);
            uint32_t h = l - 16;
            if (h > 255) h = 255;
            enc_tree(e, h, 8, high);
        }
    }
}

static uint32_t dec_len(RC *d, size_t ps, uint16_t *ch, uint16_t *ch2, uint16_t low[][8],
                        uint16_t mid[][8], uint16_t *high) {
    if (rc_bit_get(d, ch) == 0) return 2 + dec_tree(d, 3, low[ps]);
    if (rc_bit_get(d, ch2) == 0) return 10 + dec_tree(d, 3, mid[ps]);
    return 18 + dec_tree(d, 8, high);
}

static void enc_lit(RC *e, Model *m, uint8_t symbol, uint8_t prev, uint8_t match_byte, size_t state) {
    size_t off = lit_index(prev);
    uint16_t *probs = m->lit + off;
    if (state >= 7) {
        uint32_t ctx = 1;
        uint8_t mb = match_byte;
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t match_bit = mb >> 7;
            mb <<= 1;
            uint32_t bit = (symbol >> i) & 1;
            uint32_t idx = ((1 + match_bit) << 8) + ctx;
            rc_bit(e, bit, &probs[idx]);
            ctx = (ctx << 1) | bit;
            if (bit != match_bit) {
                for (i--; i >= 0; i--) {
                    bit = (symbol >> i) & 1;
                    rc_bit(e, bit, &probs[ctx]);
                    ctx = (ctx << 1) | bit;
                }
                return;
            }
        }
        return;
    }
    {
        uint32_t ctx = 1;
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t bit = (symbol >> i) & 1;
            rc_bit(e, bit, &probs[ctx]);
            ctx = (ctx << 1) | bit;
        }
    }
}

static uint8_t dec_lit(RC *d, Model *m, uint8_t prev, uint8_t match_byte, size_t state) {
    size_t off = lit_index(prev);
    uint16_t *probs = m->lit + off;
    if (state >= 7) {
        uint32_t ctx = 1;
        uint8_t mb = match_byte;
        int k;
        for (k = 0; k < 8; k++) {
            uint32_t match_bit = mb >> 7;
            mb <<= 1;
            uint32_t idx = ((1 + match_bit) << 8) + ctx;
            uint32_t bit = rc_bit_get(d, &probs[idx]);
            ctx = (ctx << 1) | bit;
            if (bit != match_bit) {
                while (ctx < 256) {
                    uint32_t b2 = rc_bit_get(d, &probs[ctx]);
                    ctx = (ctx << 1) | b2;
                }
                return (uint8_t)ctx;
            }
        }
        return (uint8_t)ctx;
    }
    {
        uint32_t ctx = 1;
        while (ctx < 256) {
            uint32_t bit = rc_bit_get(d, &probs[ctx]);
            ctx = (ctx << 1) | bit;
        }
        return (uint8_t)ctx;
    }
}

static void enc_dist(RC *e, Model *m, uint32_t dist, uint32_t len) {
    uint32_t slot = pos_slot(dist);
    size_t ls = (len - 2) > 3 ? 3 : (size_t)(len - 2);
    enc_tree(e, slot, 6, m->pos_slot[ls]);
    if (slot >= 4) {
        uint32_t footer = (slot >> 1) - 1;
        uint32_t base = (2 | (slot & 1)) << footer;
        uint32_t extra = dist - base;
        if (slot < 14) {
            enc_tree_rev(e, extra, footer, m->spec + (size_t)(slot - 4) * 64);
        } else {
            uint32_t direct = footer - 4;
            uint32_t i;
            for (i = direct; i-- > 0;) rc_direct(e, (extra >> (i + 4)) & 1);
            enc_tree_rev(e, extra & 15, 4, m->align);
        }
    }
}

static uint32_t dec_dist(RC *d, Model *m, uint32_t len) {
    size_t ls = (len - 2) > 3 ? 3 : (size_t)(len - 2);
    uint32_t slot = dec_tree(d, 6, m->pos_slot[ls]);
    if (slot < 4) return slot;
    uint32_t footer = (slot >> 1) - 1;
    uint32_t base = (2 | (slot & 1)) << footer;
    uint32_t extra;
    if (slot < 14) {
        extra = dec_tree_rev(d, footer, m->spec + (size_t)(slot - 4) * 64);
    } else {
        uint32_t direct = footer - 4;
        uint32_t v = 0, i;
        for (i = direct; i-- > 0;) v |= rc_direct_get(d) << (i + 4);
        extra = v | dec_tree_rev(d, 4, m->align);
    }
    return base + extra;
}

static void shift_rep(uint32_t r[4], uint32_t dist) {
    r[3] = r[2];
    r[2] = r[1];
    r[1] = r[0];
    r[0] = dist;
}

static void use_rep(uint32_t r[4], int k) {
    uint32_t d = r[k];
    if (k == 0) return;
    if (k == 1) {
        r[1] = r[0];
        r[0] = d;
    } else if (k == 2) {
        r[2] = r[1];
        r[1] = r[0];
        r[0] = d;
    } else {
        r[3] = r[2];
        r[2] = r[1];
        r[1] = r[0];
        r[0] = d;
    }
}

static void insert(const uint8_t *d, size_t n, size_t i, uint32_t *h4, uint32_t *p4, uint32_t *h3,
                   uint32_t *p3, size_t win) {
    if (i + 2 < n) {
        uint32_t h = hash3(d, i);
        p3[i % win] = h3[h];
        h3[h] = (uint32_t)i;
    }
    if (i + 3 < n) {
        uint32_t h = hash4(d, i);
        p4[i % win] = h4[h];
        h4[h] = (uint32_t)i;
    }
}

static int too_far(size_t ml, uint32_t dist, int which) {
    if (which >= 0) return 0;
    if (ml < MIN_NEW) return 1;
    if (ml == 3 && dist > 4096) return 1;
    if (ml == 4 && dist > 65536u) return 1;
    return 0;
}

#define NOPT 4096
#define PRICE_INF 0x3FFFFFFFu

typedef struct {
    uint32_t price, prev, len, dist;
    int which;
    uint32_t reps[4];
    uint8_t state;
} Opt;

static uint32_t ProbPrices[128];

static void init_prob_prices(void) {
    int i;
    ProbPrices[0] = 18 * 16;
    for (i = 1; i < 128; i++)
        ProbPrices[i] = (uint32_t)(16.0 * log2(128.0 / (double)i) + 0.5);
}

static uint32_t bit_price(uint16_t pr, uint32_t bit) {
    uint32_t p = pr;
    if (p < 1) p = 1;
    if (p > 2047) p = 2047;
    if (bit) p = 2048 - p;
    return ProbPrices[p >> 4];
}

static uint32_t tree_price(uint16_t *probs, uint32_t bits, uint32_t v) {
    uint32_t ctx = 1, p = 0, i;
    for (i = bits; i-- > 0;) {
        uint32_t bit = (v >> i) & 1;
        p += bit_price(probs[ctx], bit);
        ctx = (ctx << 1) | bit;
    }
    return p;
}

static uint32_t tree_price_rev(uint16_t *probs, uint32_t bits, uint32_t v) {
    uint32_t ctx = 1, p = 0, i;
    for (i = 0; i < bits; i++) {
        uint32_t bit = (v >> i) & 1;
        p += bit_price(probs[ctx], bit);
        ctx = (ctx << 1) | bit;
    }
    return p;
}

static uint32_t price_len_enc(uint16_t *ch, uint16_t *ch2, uint16_t low[][8], uint16_t mid[][8],
                              uint16_t *high, size_t ps, uint32_t len) {
    uint32_t l = len - 2;
    if (l < 8) return bit_price(*ch, 0) + tree_price(low[ps], 3, l);
    {
        uint32_t p = bit_price(*ch, 1);
        if (l < 16) return p + bit_price(*ch2, 0) + tree_price(mid[ps], 3, l - 8);
        if (l > 16 + 255) l = 16 + 255;
        return p + bit_price(*ch2, 1) + tree_price(high, 8, l - 16);
    }
}

static uint32_t price_dist_m(Model *m, uint32_t dist0, uint32_t len) {
    uint32_t slot = pos_slot(dist0);
    size_t ls = (len - 2) > 3 ? 3 : (size_t)(len - 2);
    uint32_t p = tree_price(m->pos_slot[ls], 6, slot);
    if (slot >= 4) {
        uint32_t footer = (slot >> 1) - 1;
        uint32_t base = (2 | (slot & 1)) << footer;
        uint32_t extra = dist0 - base;
        if (slot < 14)
            p += tree_price_rev(m->spec + (size_t)(slot - 4) * 64, footer, extra);
        else
            p += (footer - 4) * 16 + tree_price_rev(m->align, 4, extra & 15);
    }
    return p;
}

static uint32_t price_rep_index(Model *m, size_t state, int which) {
    if (which == 0) return bit_price(m->is_rep_g0[state], 0);
    if (which == 1) return bit_price(m->is_rep_g0[state], 1) + bit_price(m->is_rep_g1[state], 0);
    if (which == 2)
        return bit_price(m->is_rep_g0[state], 1) + bit_price(m->is_rep_g1[state], 1) +
               bit_price(m->is_rep_g2[state], 0);
    return bit_price(m->is_rep_g0[state], 1) + bit_price(m->is_rep_g1[state], 1) +
           bit_price(m->is_rep_g2[state], 1);
}

static uint32_t lit_price(Model *m, uint8_t symbol, uint8_t prev, uint8_t match_byte, size_t state) {
    size_t off = lit_index(prev);
    uint16_t *probs = m->lit + off;
    uint32_t ctx = 1, p = 0;
    if (state >= 7) {
        uint8_t mb = match_byte;
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t match_bit = mb >> 7;
            mb <<= 1;
            uint32_t bit = (symbol >> i) & 1;
            uint32_t idx = ((1 + match_bit) << 8) + ctx;
            p += bit_price(probs[idx], bit);
            ctx = (ctx << 1) | bit;
            if (bit != match_bit) {
                for (i--; i >= 0; i--) {
                    bit = (symbol >> i) & 1;
                    p += bit_price(probs[ctx], bit);
                    ctx = (ctx << 1) | bit;
                }
                return p;
            }
        }
        return p;
    }
    {
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t bit = (symbol >> i) & 1;
            p += bit_price(probs[ctx], bit);
            ctx = (ctx << 1) | bit;
        }
    }
    return p;
}

static int get_matches(const uint8_t *d, size_t n, size_t i, uint32_t *h4, uint32_t *p4, uint32_t *h3,
                       uint32_t *p3, size_t win, uint32_t *olens, uint32_t *odists) {
    size_t cap = MAX_LEN;
    if (n - i < cap) cap = n - i;
    int np = 0;
    size_t max_l = 0;
    if (i + 4 <= n) {
        uint32_t h = hash4(d, i);
        uint32_t p = h4[h];
        int walked = 0;
        while (p != 0xFFFFFFFFu && walked < CHAIN4) {
            size_t j = p;
            if (i > j && i - j <= win) {
                size_t l = match_len(d, j, i, cap, n);
                uint32_t dist = (uint32_t)(i - j);
                if (l >= 4 && l > max_l && !too_far(l, dist, -1)) {
                    if (np < MAX_PAIRS) {
                        olens[np] = (uint32_t)l;
                        odists[np] = dist;
                        np++;
                    } else {
                        olens[np - 1] = (uint32_t)l;
                        odists[np - 1] = dist;
                    }
                    max_l = l;
                    if (l >= cap) break;
                }
            }
            p = p4[j % win];
            walked++;
        }
    }
    if (max_l < 4 && i + MIN_NEW <= n) {
        uint32_t h = hash3(d, i);
        uint32_t p = h3[h];
        int walked = 0;
        while (p != 0xFFFFFFFFu && walked < CHAIN3) {
            size_t j = p;
            if (i > j && i - j <= win) {
                size_t l = match_len(d, j, i, cap, n);
                uint32_t dist = (uint32_t)(i - j);
                if (l >= MIN_NEW && l > max_l && !too_far(l, dist, -1)) {
                    if (np < MAX_PAIRS) {
                        olens[np] = (uint32_t)l;
                        odists[np] = dist;
                        np++;
                    }
                    max_l = l;
                }
            }
            p = p3[j % win];
            walked++;
        }
    }
    return np;
}

static int relax(Opt *o, uint32_t dest, uint32_t price, uint32_t prev, uint32_t len, uint32_t dist,
                 int which, const uint32_t reps[4], uint8_t st) {
    if (price >= o[dest].price) return 0;
    o[dest].price = price;
    o[dest].prev = prev;
    o[dest].len = len;
    o[dest].dist = dist;
    o[dest].which = which;
    memcpy(o[dest].reps, reps, 4 * sizeof(uint32_t));
    o[dest].state = st;
    return 1;
}

int lzm2_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    if (n < 32 || n > 0xFFFFFFFFu) return -1;
    size_t win = n < WIN ? n : WIN;
    if (win < 256) win = n;
    uint32_t *h4 = malloc(HASH4 * 4);
    uint32_t *p4 = malloc(win * 4);
    uint32_t *h3 = malloc(HASH3 * 4);
    uint32_t *p3 = malloc(win * 4);
    if (!h4 || !p4 || !h3 || !p3) {
        free(h4);
        free(p4);
        free(h3);
        free(p3);
        return -1;
    }
    memset(h4, 0xFF, HASH4 * 4);
    memset(p4, 0xFF, win * 4);
    memset(h3, 0xFF, HASH3 * 4);
    memset(p3, 0xFF, win * 4);
    Model m;
    if (model_init(&m)) {
        free(h4);
        free(p4);
        free(h3);
        free(p3);
        return -1;
    }
    Buf payload = {0};
    RC e;
    rc_init(&e, &payload, 1);
    init_prob_prices();
    uint32_t reps[4] = {1, 1, 1, 1};
    size_t state = 0, i = 0;
    Opt *opt = malloc(NOPT * sizeof(Opt));
    uint32_t *plens = malloc(NOPT * MAX_PAIRS * 4);
    uint32_t *pdists = malloc(NOPT * MAX_PAIRS * 4);
    uint8_t *npair = malloc(NOPT);
    uint32_t *path = malloc(NOPT * 4);
    if (!opt || !plens || !pdists || !npair || !path) {
        free(opt);
        free(plens);
        free(pdists);
        free(npair);
        free(path);
        model_free(&m);
        free(h4);
        free(p4);
        free(h3);
        free(p3);
        return -1;
    }
    while (i < n) {
        size_t lim = n - i;
        if (lim > NOPT - 1) lim = NOPT - 1;
        size_t k;
        for (k = 0; k < lim; k++) {
            insert(in, n, i + k, h4, p4, h3, p3, win);
            npair[k] = (uint8_t)get_matches(in, n, i + k, h4, p4, h3, p3, win, plens + k * MAX_PAIRS,
                                            pdists + k * MAX_PAIRS);
        }
        for (k = 0; k <= lim; k++) opt[k].price = PRICE_INF;
        opt[0].price = 0;
        opt[0].prev = 0;
        opt[0].len = 0;
        opt[0].dist = 0;
        opt[0].which = -2;
        memcpy(opt[0].reps, reps, 16);
        opt[0].state = (uint8_t)state;
        for (k = 0; k < lim; k++) {
            if (opt[k].price == PRICE_INF) continue;
            uint32_t r[4];
            memcpy(r, opt[k].reps, 16);
            uint8_t st = opt[k].state;
            size_t pos = i + k;
            size_t ps = pos_state(pos);
            uint32_t pm0 = bit_price(m.is_match[st][ps], 0);
            uint32_t pm1 = bit_price(m.is_match[st][ps], 1);
            uint32_t pr0 = bit_price(m.is_rep[st], 0);
            uint32_t pr1 = bit_price(m.is_rep[st], 1);
            uint8_t prevb = pos > 0 ? in[pos - 1] : 0;
            uint8_t mb = pos >= r[0] ? in[pos - r[0]] : 0;
            uint32_t plit = opt[k].price + pm0 + lit_price(&m, in[pos], prevb, mb, st);
            relax(opt, (uint32_t)k + 1, plit, (uint32_t)k, 0, 0, -2, r, (uint8_t)next_lit(st));
            if (pos >= r[0] && in[pos] == in[pos - r[0]]) {
                uint32_t psr = opt[k].price + pm1 + pr1 + bit_price(m.is_rep_g0[st], 0) +
                               bit_price(m.is_rep0_long[st][ps], 0);
                relax(opt, (uint32_t)k + 1, psr, (uint32_t)k, 1, r[0], 0, r, (uint8_t)next_short(st));
            }
            int rk;
            for (rk = 0; rk < 4; rk++) {
                uint32_t dist = r[rk];
                if (!dist || dist > pos) continue;
                size_t cap = MAX_LEN;
                if (n - pos < cap) cap = n - pos;
                if (lim - k < cap) cap = lim - k;
                if (cap < 2) continue;
                if (in[pos] != in[pos - dist]) continue;
                if (cap > 1 && in[pos + 1] != in[pos - dist + 1]) continue;
                size_t Lfull = match_len(in, pos - dist, pos, cap, n);
                if (Lfull < 2) continue;
                uint32_t nr[4];
                memcpy(nr, r, 16);
                use_rep(nr, rk);
                uint32_t base = opt[k].price + pm1 + pr1 + price_rep_index(&m, st, rk);
                uint32_t cand[6];
                int nc = 0, ci;
                cand[nc++] = (uint32_t)Lfull;
                if (Lfull > 2) cand[nc++] = (uint32_t)Lfull - 1;
                if (Lfull > 4) cand[nc++] = 3;
                if (Lfull > 8) cand[nc++] = 8;
                if (Lfull > 16) cand[nc++] = 16;
                if (Lfull > 32) cand[nc++] = 32;
                for (ci = 0; ci < nc; ci++) {
                    uint32_t L = cand[ci];
                    if (L < 2 || L > (uint32_t)Lfull) continue;
                    uint32_t pr = base;
                    if (rk == 0)
                        pr += bit_price(m.is_rep0_long[st][ps], 1);
                    pr += price_len_enc(&m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                                        m.rlen_high, ps, L);
                    relax(opt, (uint32_t)k + L, pr, (uint32_t)k, L, dist, rk, nr, (uint8_t)next_rep(st));
                }
            }
            {
                int t, npp = npair[k];
                uint32_t prev_len = MIN_NEW - 1;
                uint32_t *lens = plens + k * MAX_PAIRS;
                uint32_t *dists = pdists + k * MAX_PAIRS;
                for (t = 0; t < npp; t++) {
                    uint32_t L = lens[t], dist = dists[t];
                    if (L < MIN_NEW || !dist) continue;
                    if (k + L > lim) L = (uint32_t)(lim - k);
                    if (L < MIN_NEW) continue;
                    uint32_t nr[4];
                    memcpy(nr, r, 16);
                    shift_rep(nr, dist);
                    uint32_t lens_try[4];
                    int ntry = 0;
                    if (L > prev_len) lens_try[ntry++] = L;
                    if (L > 1 && L - 1 > prev_len) lens_try[ntry++] = L - 1;
                    {
                        int ti;
                        for (ti = 0; ti < ntry; ti++) {
                            uint32_t len = lens_try[ti];
                            if (len < MIN_NEW) continue;
                            uint32_t pr = opt[k].price + pm1 + pr0 +
                                          price_len_enc(&m.len_choice, &m.len_choice2, m.len_low, m.len_mid,
                                                        m.len_high, ps, len) +
                                          price_dist_m(&m, dist - 1, len);
                            relax(opt, k + len, pr, (uint32_t)k, len, dist, -1, nr,
                                  (uint8_t)next_match(st));
                        }
                    }
                    prev_len = L;
                }
            }
        }
        if (opt[lim].price == PRICE_INF) {
            for (k = 1; k <= lim; k++)
                if (opt[k].price == PRICE_INF)
                    relax(opt, (uint32_t)k, opt[k - 1].price + 9 * 16, (uint32_t)k - 1, 0, 0, -2,
                          opt[k - 1].reps, (uint8_t)next_lit(opt[k - 1].state));
        }
        uint32_t np = 0, x = (uint32_t)lim;
        while (x) {
            path[np++] = x;
            x = opt[x].prev;
            if (np >= NOPT) break;
        }
        uint32_t t;
        for (t = np; t-- > 0;) {
            uint32_t dest = path[t];
            uint32_t src = opt[dest].prev;
            uint32_t ml = opt[dest].len;
            uint32_t dist = opt[dest].dist;
            size_t pos = i + src;
            size_t ps = pos_state(pos);
            if (ml == 0) {
                rc_bit(&e, 0, &m.is_match[state][ps]);
                uint8_t prevb = pos > 0 ? in[pos - 1] : 0;
                uint8_t mb = pos >= reps[0] ? in[pos - reps[0]] : 0;
                enc_lit(&e, &m, in[pos], prevb, mb, state);
                state = next_lit(state);
            } else {
                int w = -1, rk;
                for (rk = 0; rk < 4; rk++)
                    if (reps[rk] == dist) {
                        w = rk;
                        break;
                    }
                if (ml == 1) w = 0;
                rc_bit(&e, 1, &m.is_match[state][ps]);
                if (w >= 0) {
                    rc_bit(&e, 1, &m.is_rep[state]);
                    if (w == 0) {
                        rc_bit(&e, 0, &m.is_rep_g0[state]);
                        if (ml == 1) {
                            rc_bit(&e, 0, &m.is_rep0_long[state][ps]);
                            state = next_short(state);
                        } else {
                            rc_bit(&e, 1, &m.is_rep0_long[state][ps]);
                            enc_len(&e, ml, ps, &m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                                    m.rlen_high);
                            state = next_rep(state);
                        }
                    } else if (w == 1) {
                        rc_bit(&e, 1, &m.is_rep_g0[state]);
                        rc_bit(&e, 0, &m.is_rep_g1[state]);
                        enc_len(&e, ml, ps, &m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                                m.rlen_high);
                        state = next_rep(state);
                    } else if (w == 2) {
                        rc_bit(&e, 1, &m.is_rep_g0[state]);
                        rc_bit(&e, 1, &m.is_rep_g1[state]);
                        rc_bit(&e, 0, &m.is_rep_g2[state]);
                        enc_len(&e, ml, ps, &m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                                m.rlen_high);
                        state = next_rep(state);
                    } else {
                        rc_bit(&e, 1, &m.is_rep_g0[state]);
                        rc_bit(&e, 1, &m.is_rep_g1[state]);
                        rc_bit(&e, 1, &m.is_rep_g2[state]);
                        enc_len(&e, ml, ps, &m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                                m.rlen_high);
                        state = next_rep(state);
                    }
                    use_rep(reps, w);
                } else {
                    rc_bit(&e, 0, &m.is_rep[state]);
                    enc_len(&e, ml, ps, &m.len_choice, &m.len_choice2, m.len_low, m.len_mid, m.len_high);
                    enc_dist(&e, &m, dist - 1, ml);
                    shift_rep(reps, dist);
                    state = next_match(state);
                }
            }
        }
        i += lim;
        if (e.err) break;
    }
    free(opt);
    free(plens);
    free(pdists);
    free(npair);
    free(path);
    if (e.err) {
        model_free(&m);
        free(h4);
        free(p4);
        free(h3);
        free(p3);
        free(payload.p);
        return -1;
    }
    rc_flush(&e);
    model_free(&m);
    free(h4);
    free(p4);
    free(h3);
    free(p3);
    if (e.err || !payload.p) {
        free(payload.p);
        return -1;
    }
    size_t total = 4 + 1 + 1 + 4 + payload.n;
    uint8_t *blob = malloc(total);
    if (!blob) {
        free(payload.p);
        return -1;
    }
    memcpy(blob, LZM2_MAGIC, 4);
    blob[4] = 1;
    blob[5] = (uint8_t)(((LC & 0xf) << 4) | ((LP & 3) << 2) | (PB & 3));
    blob[6] = (uint8_t)n;
    blob[7] = (uint8_t)(n >> 8);
    blob[8] = (uint8_t)(n >> 16);
    blob[9] = (uint8_t)(n >> 24);
    memcpy(blob + 10, payload.p, payload.n);
    free(payload.p);
    *out = blob;
    *on = total;
    return 0;
}

int lzm2_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on) {
    if (n < 10 || memcmp(in, LZM2_MAGIC, 4) != 0) return -1;
    size_t nn = (size_t)in[6] | ((size_t)in[7] << 8) | ((size_t)in[8] << 16) | ((size_t)in[9] << 24);
    if (orig && nn != orig) return -1;
    if (!nn) return -1;
    uint8_t *y = malloc(nn);
    if (!y) return -1;
    Model m;
    if (model_init(&m)) {
        free(y);
        return -1;
    }
    Buf dummy = {0};
    RC d;
    rc_init(&d, &dummy, 0);
    rc_init_dec(&d, in + 10, n - 10);
    uint32_t reps[4] = {1, 1, 1, 1};
    size_t state = 0, i = 0;
    while (i < nn) {
        size_t ps = pos_state(i);
        if (rc_bit_get(&d, &m.is_match[state][ps]) == 0) {
            uint8_t prev = i > 0 ? y[i - 1] : 0;
            uint8_t mb = i >= reps[0] ? y[i - reps[0]] : 0;
            y[i] = dec_lit(&d, &m, prev, mb, state);
            state = next_lit(state);
            i++;
            continue;
        }
        if (rc_bit_get(&d, &m.is_rep[state]) == 1) {
            int which;
            if (rc_bit_get(&d, &m.is_rep_g0[state]) == 0)
                which = 0;
            else if (rc_bit_get(&d, &m.is_rep_g1[state]) == 0)
                which = 1;
            else if (rc_bit_get(&d, &m.is_rep_g2[state]) == 0)
                which = 2;
            else
                which = 3;
            uint32_t len;
            if (which == 0 && rc_bit_get(&d, &m.is_rep0_long[state][ps]) == 0) {
                state = next_short(state);
                len = 1;
            } else {
                len = dec_len(&d, ps, &m.rlen_choice, &m.rlen_choice2, m.rlen_low, m.rlen_mid,
                              m.rlen_high);
                state = next_rep(state);
            }
            uint32_t dist = reps[which];
            if (!dist || dist > i || i + len > nn) {
                model_free(&m);
                free(y);
                return -1;
            }
            use_rep(reps, which);
            uint32_t t;
            for (t = 0; t < len; t++) {
                y[i] = y[i - dist];
                i++;
            }
        } else {
            uint32_t len = dec_len(&d, ps, &m.len_choice, &m.len_choice2, m.len_low, m.len_mid,
                                   m.len_high);
            uint32_t dist0 = dec_dist(&d, &m, len);
            uint32_t dist = dist0 + 1;
            if (!dist || dist > i || i + len > nn) {
                model_free(&m);
                free(y);
                return -1;
            }
            shift_rep(reps, dist);
            state = next_match(state);
            uint32_t t;
            for (t = 0; t < len; t++) {
                y[i] = y[i - dist];
                i++;
            }
        }
    }
    model_free(&m);
    *out = y;
    *on = nn;
    return 0;
}
