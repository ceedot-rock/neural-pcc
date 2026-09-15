/* Lab neural coding riser. AIP quik.build / neat.destruct.
 * Fixed-point MLP p_theta mixed with adaptive counts. Proprietary. */
#include "riser.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CTX 8
#define HID 16
#define F (CTX + 1)
#define FREQ_SUM 4096
#define MIX_NET 1
#define MIX_ADP 7
#define KIND_RAW 0
#define KIND_NET 1
#define VER 1

static uint32_t lcg_next(uint32_t *s) {
    *s = 1664525u * (*s) + 1013904223u;
    return *s;
}

static int16_t W1[HID * F], B1[HID], W2[256 * HID], B2[256];
static int ready;
static uint8_t MODEL8[8];

static void sha256_8(const uint8_t *msg, size_t len, uint8_t out8[8]);

static void model_init(void) {
    if (ready) return;
    uint32_t s = 0x52495331u; /* RIS1 */
    for (int i = 0; i < HID * F; i++)
        W1[i] = (int16_t)((int)(lcg_next(&s) % 4915) - 2457); /* ~Q12 * 0.3 */
    for (int i = 0; i < HID; i++)
        B1[i] = (int16_t)((int)(lcg_next(&s) % 1639) - 819);
    for (int i = 0; i < 256 * HID; i++)
        W2[i] = (int16_t)((int)(lcg_next(&s) % 1639) - 819);
    for (int i = 0; i < 256; i++)
        B2[i] = (int16_t)((int)(lcg_next(&s) % 821) - 410);
    sha256_8((const uint8_t *)RISER_MODEL_ID, sizeof RISER_MODEL_ID - 1, MODEL8);
    ready = 1;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha256_8(const uint8_t *msg, size_t len, uint8_t out8[8]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t nblk = (len + 9 + 63) / 64;
    size_t L = nblk * 64;
    uint8_t *blk = calloc(1, L);
    if (!blk) return;
    memcpy(blk, msg, len);
    blk[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) blk[L - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < L; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)blk[off + 4 * i] << 24) | ((uint32_t)blk[off + 4 * i + 1] << 16) |
                   ((uint32_t)blk[off + 4 * i + 2] << 8) | blk[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    free(blk);
    out8[0] = (uint8_t)(h[0] >> 24); out8[1] = (uint8_t)(h[0] >> 16);
    out8[2] = (uint8_t)(h[0] >> 8);  out8[3] = (uint8_t)h[0];
    out8[4] = (uint8_t)(h[1] >> 24); out8[5] = (uint8_t)(h[1] >> 16);
    out8[6] = (uint8_t)(h[1] >> 8);  out8[7] = (uint8_t)h[1];
}

static void net_logits(const uint8_t *ctx, int ctxn, int32_t lg[256]) {
    int32_t feat[F];
    for (int i = 0; i < CTX; i++) {
        uint8_t b = 0;
        if (ctxn >= CTX - i) b = ctx[ctxn - CTX + i];
        else if (i >= CTX - ctxn) b = ctx[i - (CTX - ctxn)];
        feat[i] = (int32_t)b; /* 0..255 */
    }
    feat[CTX] = 256;
    int32_t hid[HID];
    for (int h = 0; h < HID; h++) {
        int32_t s = B1[h] * 16;
        for (int f = 0; f < F; f++) s += (int32_t)W1[h * F + f] * feat[f];
        hid[h] = s > 0 ? (s >> 8) : 0;
    }
    for (int o = 0; o < 256; o++) {
        int32_t s = B2[o] * 16;
        for (int h = 0; h < HID; h++) s += (int32_t)W2[o * HID + h] * hid[h];
        lg[o] = s;
    }
}

static void normalize(const uint32_t *counts, int *freqs) {
    uint64_t tot = 0;
    for (int i = 0; i < 256; i++) tot += counts[i];
    if (!tot) {
        for (int i = 0; i < 256; i++) freqs[i] = FREQ_SUM / 256;
        freqs[255] = FREQ_SUM - (FREQ_SUM / 256) * 255;
        return;
    }
    int acc = 0;
    int present[256], np = 0;
    for (int i = 0; i < 256; i++) {
        freqs[i] = 0;
        if (counts[i]) present[np++] = i;
    }
    for (int k = 0; k < np; k++) {
        int i = present[k];
        int f = (int)((counts[i] * (uint64_t)FREQ_SUM) / tot);
        if (f < 1) f = 1;
        freqs[i] = f;
        acc += f;
    }
    if (acc > FREQ_SUM) {
        while (acc > FREQ_SUM) {
            int j = present[0];
            for (int k = 1; k < np; k++)
                if (freqs[present[k]] > freqs[j]) j = present[k];
            if (freqs[j] <= 1) break;
            freqs[j]--;
            acc--;
        }
    } else if (acc < FREQ_SUM) {
        int j = present[0];
        for (int k = 1; k < np; k++)
            if (counts[present[k]] > counts[j]) j = present[k];
        freqs[j] += FREQ_SUM - acc;
    }
}

static void mixed_freqs(const uint8_t *ctx, int ctxn, const uint32_t *adp, int *freqs) {
    int32_t lg[256];
    net_logits(ctx, ctxn, lg);
    int32_t m = lg[0];
    for (int i = 1; i < 256; i++)
        if (lg[i] > m) m = lg[i];
    uint32_t nc[256];
    uint64_t acc = 0;
    for (int i = 0; i < 256; i++) {
        int32_t d = lg[i] - m;
        /* crude positive map: shift so max=0, use 1 + (d+offset) */
        int32_t w = d + 4096;
        if (w < 1) w = 1;
        nc[i] = (uint32_t)w;
        acc += nc[i];
    }
    int nf[256], af[256];
    normalize(nc, nf);
    normalize(adp, af);
    uint32_t mix[256];
    for (int i = 0; i < 256; i++) mix[i] = (uint32_t)(MIX_NET * nf[i] + MIX_ADP * af[i]);
    normalize(mix, freqs);
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;
static int push(Buf *b, uint8_t x) {
    if (b->n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        uint8_t *p = realloc(b->p, c);
        if (!p) return -1;
        b->p = p;
        b->cap = c;
    }
    b->p[b->n++] = x;
    return 0;
}
static int app(Buf *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (push(b, p[i])) return -1;
    return 0;
}

typedef struct {
    uint8_t *p;
    size_t nbits, capbits;
} Bits;
static int bpush(Bits *b, int bit) {
    if (b->nbits + 1 > b->capbits) {
        size_t cap = b->capbits ? b->capbits * 2 : 256;
        uint8_t *p = realloc(b->p, (cap + 7) / 8 + 1);
        if (!p) return -1;
        b->p = p;
        b->capbits = cap;
    }
    size_t bi = b->nbits / 8;
    int off = 7 - (int)(b->nbits & 7);
    if ((b->nbits & 7) == 0) b->p[bi] = 0;
    if (bit) b->p[bi] |= (uint8_t)(1u << off);
    b->nbits++;
    return 0;
}
static int getbit(const uint8_t *p, size_t n, size_t *bi) {
    if (*bi >= n * 8) {
        (*bi)++;
        return 1;
    }
    size_t byte = *bi / 8;
    int off = 7 - (int)(*bi & 7);
    (*bi)++;
    return (p[byte] >> off) & 1;
}
static int out_bit(Bits *bits, int *pending, int b) {
    if (bpush(bits, b & 1)) return -1;
    while (*pending) {
        if (bpush(bits, (b & 1) ^ 1)) return -1;
        (*pending)--;
    }
    return 0;
}

static int ac_encode(const uint8_t *x, size_t n, Buf *out) {
    model_init();
    uint32_t low = 0, high = 0xFFFFFFFFu;
    int pending = 0;
    Bits bits = {0};
    uint32_t counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 1;
    uint8_t ctx[CTX];
    int ctxn = 0;
    for (size_t i = 0; i < n; i++) {
        int freqs[256], cdf[257];
        mixed_freqs(ctx, ctxn, counts, freqs);
        cdf[0] = 0;
        for (int k = 0; k < 256; k++) cdf[k + 1] = cdf[k] + freqs[k];
        uint64_t span = (uint64_t)high - low + 1;
        uint32_t sym = x[i];
        high = (uint32_t)(low + (span * (uint64_t)cdf[sym + 1]) / FREQ_SUM - 1);
        low = (uint32_t)(low + (span * (uint64_t)cdf[sym]) / FREQ_SUM);
        for (;;) {
            if (high < 0x80000000u) {
                if (out_bit(&bits, &pending, 0)) goto fail;
            } else if (low >= 0x80000000u) {
                if (out_bit(&bits, &pending, 1)) goto fail;
                low -= 0x80000000u;
                high -= 0x80000000u;
            } else if (low >= 0x40000000u && high < 0xC0000000u) {
                pending++;
                low -= 0x40000000u;
                high -= 0x40000000u;
            } else
                break;
            low = (low << 1) & 0xFFFFFFFFu;
            high = ((high << 1) | 1u) & 0xFFFFFFFFu;
        }
        counts[sym]++;
        if (ctxn < CTX) ctx[ctxn++] = (uint8_t)sym;
        else {
            memmove(ctx, ctx + 1, CTX - 1);
            ctx[CTX - 1] = (uint8_t)sym;
        }
    }
    pending++;
    if (out_bit(&bits, &pending, low >= 0x40000000u ? 1 : 0)) goto fail;
    size_t nbytes = (bits.nbits + 7) / 8;
    int rc = app(out, bits.p, nbytes);
    free(bits.p);
    return rc;
fail:
    free(bits.p);
    return -1;
}

static int ac_decode(const uint8_t *p, size_t pn, size_t n, uint8_t *out) {
    model_init();
    size_t bi = 0;
    uint32_t value = 0;
    for (int k = 0; k < 32; k++) value = (value << 1) | (uint32_t)getbit(p, pn, &bi);
    uint32_t low = 0, high = 0xFFFFFFFFu;
    uint32_t counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 1;
    uint8_t ctx[CTX];
    int ctxn = 0;
    for (size_t i = 0; i < n; i++) {
        int freqs[256], cdf[257];
        mixed_freqs(ctx, ctxn, counts, freqs);
        cdf[0] = 0;
        for (int k = 0; k < 256; k++) cdf[k + 1] = cdf[k] + freqs[k];
        uint64_t span = (uint64_t)high - low + 1;
        uint64_t scaled = ((uint64_t)(value - low + 1) * FREQ_SUM - 1) / span;
        int lo = 0, hi = 256;
        while (lo + 1 < hi) {
            int mid = (lo + hi) / 2;
            if ((uint64_t)cdf[mid] <= scaled) lo = mid;
            else hi = mid;
        }
        int s = lo;
        high = (uint32_t)(low + (span * (uint64_t)cdf[s + 1]) / FREQ_SUM - 1);
        low = (uint32_t)(low + (span * (uint64_t)cdf[s]) / FREQ_SUM);
        for (;;) {
            if (high < 0x80000000u) {
            } else if (low >= 0x80000000u) {
                low -= 0x80000000u;
                high -= 0x80000000u;
                value -= 0x80000000u;
            } else if (low >= 0x40000000u && high < 0xC0000000u) {
                low -= 0x40000000u;
                high -= 0x40000000u;
                value -= 0x40000000u;
            } else
                break;
            low = (low << 1) & 0xFFFFFFFFu;
            high = ((high << 1) | 1u) & 0xFFFFFFFFu;
            value = ((value << 1) | (uint32_t)getbit(p, pn, &bi)) & 0xFFFFFFFFu;
        }
        out[i] = (uint8_t)s;
        counts[s]++;
        if (ctxn < CTX) ctx[ctxn++] = (uint8_t)s;
        else {
            memmove(ctx, ctx + 1, CTX - 1);
            ctx[CTX - 1] = (uint8_t)s;
        }
    }
    return 0;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int aip_is_frame(const uint8_t *in, size_t n) {
    return n >= 26 && memcmp(in, AIP_MAGIC, 4) == 0 && in[4] == VER;
}

static int wrap(uint8_t kind, const uint8_t *pay, size_t pn, uint32_t orig, uint8_t **out, size_t *on) {
    model_init();
    size_t hn = 22 + pn + 4;
    uint8_t *f = malloc(hn);
    if (!f) return -1;
    memcpy(f, AIP_MAGIC, 4);
    f[4] = VER;
    f[5] = kind;
    wr32(f + 6, orig);
    wr32(f + 10, (uint32_t)pn);
    if (kind == KIND_NET) memcpy(f + 14, MODEL8, 8);
    else memset(f + 14, 0, 8);
    memcpy(f + 22, pay, pn);
    uint32_t crc = crc32(0, f, (uInt)(22 + pn));
    wr32(f + 22 + pn, crc);
    *out = f;
    *on = hn;
    return 0;
}

int aip_quik_build(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    model_init();
    Buf coded = {0};
    if (n && ac_encode(in, n, &coded)) {
        free(coded.p);
        return -1;
    }
    size_t framed = 26 + coded.n;
    int kind = KIND_NET;
    const uint8_t *pay = coded.p;
    size_t pn = coded.n;
    if (!n || framed >= n) {
        kind = KIND_RAW;
        pay = in;
        pn = n;
    }
    int rc = wrap((uint8_t)kind, pay, pn, (uint32_t)n, out, on);
    free(coded.p);
    return rc;
}

int aip_neat_destruct(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    if (!aip_is_frame(in, n)) return -2;
    uint32_t orig = rd32(in + 6);
    uint32_t pn = rd32(in + 10);
    if (22 + (size_t)pn + 4 != n) return -2;
    uint32_t crc_got = rd32(in + 22 + pn);
    uint32_t crc_need = crc32(0, in, (uInt)(22 + pn));
    if (crc_got != crc_need) return -3;
    uint8_t kind = in[5];
    const uint8_t *pay = in + 22;
    if (kind == KIND_RAW) {
        uint8_t *y = malloc(orig ? orig : 1);
        if (!y) return -1;
        if (pn != orig) {
            free(y);
            return -2;
        }
        memcpy(y, pay, orig);
        *out = y;
        *on = orig;
        return 0;
    }
    if (kind != KIND_NET) return -2;
    model_init();
    if (memcmp(in + 14, MODEL8, 8) != 0) return -4;
    uint8_t *y = malloc(orig ? orig : 1);
    if (!y) return -1;
    if (orig && ac_decode(pay, pn, orig, y)) {
        free(y);
        return -2;
    }
    *out = y;
    *on = orig;
    return 0;
}
