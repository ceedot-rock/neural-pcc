/* Neural-PCC C engine. Laws in cuni/ are proven; LZ/AC/AR live here. Proprietary. */
#include "npcc.h"
#include "tnssrc.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MAGIC "NPCC"
#define VERSION 1
#define FLAG_HAS_MODEL 1
#define FREQ_BITS 12
#define FREQ_SUM (1 << FREQ_BITS)
#define CTX 8
#define ZECK_MAX_LEN 8
#define LZ_W 4096
#define LZ_MIN 3
#define LZ_MAX 255
#define LZ_HASH 16
#define LZ_HSIZE (1u << LZ_HASH)

enum { PRI_PROG = 0, PRI_AR = 1, PRI_TRU8 = 2, PRI_ZECK = 3, PRI_LZ = 4, PRI_RAW = 255 };

static const char *PATH_NAME[] = {"RAW", "TRU8", "ZECK", "LZCM", "TNSSRC", "NEURAL_PROG"};

void npcc_budget_full(NpccBudget *b) {
    b->allow_neural = 1;
    b->allow_search = 1;
    b->max_prog_len = 256;
}
void npcc_budget_ar(NpccBudget *b) {
    b->allow_neural = 1;
    b->allow_search = 0;
    b->max_prog_len = 256;
}
void npcc_budget_classical(NpccBudget *b) {
    b->allow_neural = 0;
    b->allow_search = 0;
    b->max_prog_len = 256;
}

const char *npcc_pathway_name(int id) {
    if (id < 0 || id > 5) return "?";
    return PATH_NAME[id];
}

const char *npcc_strerror(int rc) {
    switch (rc) {
    case NPCC_OK: return "ok";
    case NPCC_ERR_NOMEM: return "nomem";
    case NPCC_ERR_FORMAT: return "format";
    case NPCC_ERR_CRC: return "crc";
    case NPCC_ERR_PATHWAY: return "pathway";
    case NPCC_ERR_MODEL: return "model";
    case NPCC_ERR_LEN: return "length";
    case NPCC_ERR_IO: return "io";
    case NPCC_ERR_PROG: return "prog";
    default: return "error";
    }
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static int buf_reserve(Buf *b, size_t need) {
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}
static int buf_push(Buf *b, uint8_t x) {
    if (buf_reserve(b, b->n + 1)) return -1;
    b->p[b->n++] = x;
    return 0;
}
static int buf_app(Buf *b, const uint8_t *p, size_t n) {
    if (buf_reserve(b, b->n + n)) return -1;
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}
static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static int uleb(Buf *b, uint64_t n) {
    while (n >= 0x80) {
        if (buf_push(b, (uint8_t)((n & 0x7f) | 0x80))) return -1;
        n >>= 7;
    }
    return buf_push(b, (uint8_t)n);
}
static int read_uleb(const uint8_t *p, size_t n, size_t *i, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    while (*i < n) {
        uint8_t c = p[(*i)++];
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return 0;
        }
        shift += 7;
        if (shift > 63) return -1;
    }
    return -1;
}

/* SHA-256 of model id tiny-ar-v1 */
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
static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t nblk = ((len + 9 + 63) / 64);
    size_t L = nblk * 64;
    uint8_t *blk = calloc(1, L);
    if (!blk) return;
    memcpy(blk, msg, len);
    blk[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) blk[L - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < L; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)blk[off + 4 * i] << 24) | ((uint32_t)blk[off + 4 * i + 1] << 16) |
                   ((uint32_t)blk[off + 4 * i + 2] << 8) | blk[off + 4 * i + 3];
        }
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
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(blk);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static int pack_blob(int pid, uint64_t orig, const uint8_t *payload, size_t pn,
                     const uint8_t *mhash, size_t hn, Buf *out) {
    uint16_t flags = 0;
    if (hn) flags |= FLAG_HAS_MODEL;
    Buf head = {0};
    if (buf_app(&head, (const uint8_t *)MAGIC, 4)) goto nomem;
    uint8_t ver[2] = {(uint8_t)(VERSION), (uint8_t)(VERSION >> 8)};
    if (buf_app(&head, ver, 2) || buf_push(&head, (uint8_t)pid)) goto nomem;
    uint8_t fl[2] = {(uint8_t)flags, (uint8_t)(flags >> 8)};
    if (buf_app(&head, fl, 2) || uleb(&head, orig) || uleb(&head, pn) || buf_push(&head, (uint8_t)hn))
        goto nomem;
    if (hn && buf_app(&head, mhash, hn)) goto nomem;
    if (uleb(&head, 0)) goto nomem;
    uint32_t crc = crc32(0, head.p, (uInt)head.n);
    crc = crc32(crc, payload, (uInt)pn);
    if (buf_app(out, head.p, head.n) || buf_app(out, payload, pn)) goto nomem;
    uint8_t c[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    buf_free(&head);
    return buf_app(out, c, 4);
nomem:
    buf_free(&head);
    return -1;
}

typedef struct {
    int pid, pri;
    Buf blob;
} Cand;

static int consider(Cand *best, int have, int pid, int pri, uint64_t orig, const uint8_t *payload,
                    size_t pn, const uint8_t *mh, size_t hn) {
    if (!payload) return have;
    Buf blob = {0};
    if (pack_blob(pid, orig, payload, pn, mh, hn, &blob)) {
        buf_free(&blob);
        return have;
    }
    if (blob.n >= orig) {
        buf_free(&blob);
        return have;
    }
    if (!have || blob.n < best->blob.n || (blob.n == best->blob.n && pri < best->pri)) {
        buf_free(&best->blob);
        best->pid = pid;
        best->pri = pri;
        best->blob = blob;
        return 1;
    }
    buf_free(&blob);
    return have;
}

/* ---- rans/ac ---- */
static void normalize_freqs(const uint64_t *counts, int n, int *freqs) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += counts[i];
    if (total == 0) {
        int base = FREQ_SUM / n;
        for (int i = 0; i < n; i++) freqs[i] = base;
        freqs[n - 1] = FREQ_SUM - base * (n - 1);
        return;
    }
    int present[256], np = 0;
    for (int i = 0; i < n; i++) {
        freqs[i] = 0;
        if (counts[i] > 0) present[np++] = i;
    }
    int acc = 0;
    for (int k = 0; k < np; k++) {
        int i = present[k];
        int f = (int)((counts[i] * FREQ_SUM) / total);
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
static void cdf_from(const int *freqs, int n, int *cdf) {
    cdf[0] = 0;
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += freqs[i];
        cdf[i + 1] = s;
    }
}

typedef struct {
    uint8_t *p;
    size_t nbits, capbits;
} Bits;
static int bits_push(Bits *b, int bit) {
    if (b->nbits + 1 > b->capbits) {
        size_t cap = b->capbits ? b->capbits * 2 : 256;
        while (cap < b->nbits + 1) cap *= 2;
        uint8_t *p = realloc(b->p, (cap + 7) / 8);
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

typedef void (*FreqFn)(size_t i, const uint8_t *ctx, int ctxn, const uint64_t *counts, int *freqs,
                       void *u);

static int ac_out_bit(Bits *bits, int *pending, int b) {
    if (bits_push(bits, b & 1)) return -1;
    while (*pending) {
        if (bits_push(bits, (b & 1) ^ 1)) return -1;
        (*pending)--;
    }
    return 0;
}

static int ac_encode2(const uint8_t *x, size_t n, FreqFn ff, void *u, Buf *out) {
    uint32_t low = 0, high = 0xFFFFFFFFu;
    int pending = 0;
    Bits bits = {0};
    uint64_t counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 1;
    uint8_t ctx[CTX];
    int ctxn = 0;
    for (size_t i = 0; i < n; i++) {
        int freqs[256], cdf[257];
        ff(i, ctx, ctxn, counts, freqs, u);
        cdf_from(freqs, 256, cdf);
        uint64_t span = (uint64_t)high - low + 1;
        uint32_t sym = x[i];
        high = (uint32_t)(low + (span * (uint64_t)cdf[sym + 1]) / FREQ_SUM - 1);
        low = (uint32_t)(low + (span * (uint64_t)cdf[sym]) / FREQ_SUM);
        for (;;) {
            if (high < 0x80000000u) {
                if (ac_out_bit(&bits, &pending, 0)) goto nomem;
            } else if (low >= 0x80000000u) {
                if (ac_out_bit(&bits, &pending, 1)) goto nomem;
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
        if (ctxn < CTX)
            ctx[ctxn++] = (uint8_t)sym;
        else {
            memmove(ctx, ctx + 1, CTX - 1);
            ctx[CTX - 1] = (uint8_t)sym;
        }
    }
    pending++;
    if (ac_out_bit(&bits, &pending, low >= 0x40000000u ? 1 : 0)) goto nomem;
    size_t nbytes = (bits.nbits + 7) / 8;
    int rc = buf_app(out, bits.p, nbytes);
    free(bits.p);
    return rc;
nomem:
    free(bits.p);
    return -1;
}

static int ac_decode2(const uint8_t *p, size_t pn, size_t n, FreqFn ff, void *u, uint8_t *out) {
    size_t bi = 0;
    uint32_t value = 0;
    for (int k = 0; k < 32; k++) value = (value << 1) | (uint32_t)getbit(p, pn, &bi);
    uint32_t low = 0, high = 0xFFFFFFFFu;
    uint64_t counts[256];
    for (int i = 0; i < 256; i++) counts[i] = 1;
    uint8_t ctx[CTX];
    int ctxn = 0;
    for (size_t i = 0; i < n; i++) {
        int freqs[256], cdf[257];
        ff(i, ctx, ctxn, counts, freqs, u);
        cdf_from(freqs, 256, cdf);
        uint64_t span = (uint64_t)high - low + 1;
        uint64_t scaled = ((uint64_t)(value - low + 1) * FREQ_SUM - 1) / span;
        int lo = 0, hi = 256;
        while (lo + 1 < hi) {
            int mid = (lo + hi) / 2;
            if ((uint64_t)cdf[mid] <= scaled)
                lo = mid;
            else
                hi = mid;
        }
        int s = lo;
        high = (uint32_t)(low + (span * (uint64_t)cdf[s + 1]) / FREQ_SUM - 1);
        low = (uint32_t)(low + (span * (uint64_t)cdf[s]) / FREQ_SUM);
        for (;;) {
            if (high < 0x80000000u) {
                /* */
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
        if (ctxn < CTX)
            ctx[ctxn++] = (uint8_t)s;
        else {
            memmove(ctx, ctx + 1, CTX - 1);
            ctx[CTX - 1] = (uint8_t)s;
        }
    }
    return 0;
}

static void freq_adaptive(size_t i, const uint8_t *ctx, int ctxn, const uint64_t *counts, int *freqs,
                          void *u) {
    (void)i;
    (void)ctx;
    (void)ctxn;
    (void)u;
    normalize_freqs(counts, 256, freqs);
}

static int ar_ready;
static uint8_t MODEL_HASH[32];

static void ar_init(void) {
    if (ar_ready) return;
    sha256((const uint8_t *)TNSSRC_MODEL_ID, sizeof TNSSRC_MODEL_ID - 1, MODEL_HASH);
    ar_ready = 1;
}

/* ---- pathways ---- */
static int tru8_compress(const uint8_t *x, size_t n, Buf *p) {
    if (!n) return 0;
    uint8_t b0 = x[0];
    for (size_t i = 1; i < n; i++)
        if (x[i] != b0) return 0;
    if (buf_push(p, 1) || buf_push(p, b0)) return -1;
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (uint8_t)(n >> (8 * i));
    if (buf_app(p, le, 8)) return -1;
    return 1;
}
static int tru8_decompress(const uint8_t *p, size_t pn, uint8_t **out, size_t *on) {
    if (pn != 10 || p[0] != 1) return NPCC_ERR_FORMAT;
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n |= (uint64_t)p[2 + i] << (8 * i);
    uint8_t *y = malloc((size_t)n ? (size_t)n : 1);
    if (!y) return NPCC_ERR_NOMEM;
    memset(y, p[1], (size_t)n);
    *out = y;
    *on = (size_t)n;
    return 0;
}

static int zeck_compress(const uint8_t *x, size_t n, Buf *p) {
    if (!n) return 0;
    size_t i = 0;
    while (i < n && x[i] == 0) i++;
    if (n - i > ZECK_MAX_LEN) return 0;
    uint64_t v = 0;
    for (; i < n; i++) v = (v << 8) | x[i];
    if (v == 0) {
        if (buf_push(p, 0)) return -1;
        return 1;
    }
    uint64_t fib[96];
    fib[0] = 1;
    fib[1] = 2;
    int k = 2;
    while (fib[k - 1] < v && k < 96) {
        uint64_t nxt = fib[k - 1] + fib[k - 2];
        if (nxt < fib[k - 1]) break;
        fib[k++] = nxt;
    }
    int bits[96];
    int nb = 0;
    uint64_t rest = v;
    for (int j = k - 1; j >= 0; j--) {
        if (fib[j] <= rest) {
            bits[nb++] = 1;
            rest -= fib[j];
        } else
            bits[nb++] = 0;
    }
    if (rest != 0) return 0;
    uint8_t le[4] = {(uint8_t)nb, (uint8_t)(nb >> 8), (uint8_t)(nb >> 16), (uint8_t)(nb >> 24)};
    if (buf_app(p, le, 4)) return -1;
    uint8_t acc = 0;
    int t = 0;
    for (int j = 0; j < nb; j++) {
        acc = (uint8_t)((acc << 1) | bits[j]);
        t++;
        if (t == 8) {
            if (buf_push(p, acc)) return -1;
            acc = 0;
            t = 0;
        }
    }
    if (t && buf_push(p, (uint8_t)(acc << (8 - t)))) return -1;
    return 1;
}
static int zeck_decompress(const uint8_t *p, size_t pn, size_t orig, uint8_t **out, size_t *on) {
    if (!pn) return NPCC_ERR_FORMAT;
    uint8_t *y = calloc(orig ? orig : 1, 1);
    if (!y) return NPCC_ERR_NOMEM;
    if (pn == 1 && p[0] == 0) {
        *out = y;
        *on = orig;
        return 0;
    }
    if (pn < 4) {
        free(y);
        return NPCC_ERR_FORMAT;
    }
    uint32_t bitcount = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                        ((uint32_t)p[3] << 24);
    int bits[512];
    int nb = 0;
    for (size_t i = 4; i < pn && nb < (int)bitcount; i++) {
        for (int b = 7; b >= 0 && nb < (int)bitcount; b--) bits[nb++] = (p[i] >> b) & 1;
    }
    uint64_t fib[96];
    fib[0] = 1;
    fib[1] = 2;
    int fk = 2;
    while (fk < nb && fk < 96) {
        fib[fk] = fib[fk - 1] + fib[fk - 2];
        fk++;
    }
    uint64_t v = 0;
    for (int j = 0; j < nb; j++)
        if (bits[j]) v += fib[nb - 1 - j];
    for (int j = (int)orig - 1; j >= 0; j--) {
        y[j] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    *out = y;
    *on = orig;
    return 0;
}

static int lzcm_tags(const uint8_t *x, size_t n, Buf *tags) {
    uint32_t *head = malloc(LZ_HSIZE * sizeof(uint32_t));
    uint32_t *prev = malloc(n * sizeof(uint32_t));
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, LZ_HSIZE * sizeof(uint32_t));
    size_t i = 0;
    while (i < n) {
        int best = 0, best_off = 0;
        if (i + LZ_MIN <= n) {
            uint32_t h = (uint32_t)x[i] | ((uint32_t)x[i + 1] << 8) | ((uint32_t)x[i + 2] << 16);
            h ^= h >> 16;
            h &= LZ_HSIZE - 1;
            int nc = 0;
            uint32_t cand[64];
            for (uint32_t pos = head[h]; pos != 0xFFFFFFFFu && nc < 64; pos = prev[pos]) {
                if (pos >= i) continue;
                if (pos + 2 >= n) continue;
                if (x[pos] == x[i] && x[pos + 1] == x[i + 1] && x[pos + 2] == x[i + 2])
                    cand[nc++] = pos;
            }
            for (int c = 0; c < nc; c++) {
                uint32_t pos = cand[c];
                if (i - pos > LZ_W) continue;
                int lim = LZ_MAX;
                if ((size_t)lim > n - i) lim = (int)(n - i);
                if ((size_t)lim > i - pos) lim = (int)(i - pos);
                int L = 0;
                while (L < lim && x[pos + L] == x[i + L]) L++;
                if (L > best) {
                    best = L;
                    best_off = (int)(i - pos);
                }
            }
        }
        if (best >= LZ_MIN) {
            if (buf_push(tags, 1) || buf_push(tags, (uint8_t)best) ||
                buf_push(tags, (uint8_t)best_off) || buf_push(tags, (uint8_t)(best_off >> 8))) {
                free(head);
                free(prev);
                return -1;
            }
            size_t end = i + (size_t)best;
            while (i < end) {
                if (i + 3 <= n) {
                    uint32_t h =
                        (uint32_t)x[i] | ((uint32_t)x[i + 1] << 8) | ((uint32_t)x[i + 2] << 16);
                    h ^= h >> 16;
                    h &= LZ_HSIZE - 1;
                    prev[i] = head[h];
                    head[h] = (uint32_t)i;
                }
                i++;
            }
        } else {
            if (buf_push(tags, 0) || buf_push(tags, x[i])) {
                free(head);
                free(prev);
                return -1;
            }
            if (i + 3 <= n) {
                uint32_t h = (uint32_t)x[i] | ((uint32_t)x[i + 1] << 8) | ((uint32_t)x[i + 2] << 16);
                h ^= h >> 16;
                h &= LZ_HSIZE - 1;
                prev[i] = head[h];
                head[h] = (uint32_t)i;
            }
            i++;
        }
    }
    free(head);
    free(prev);
    return 1;
}

static int lzcm_untags(const uint8_t *p, size_t pn, size_t orig, uint8_t **out, size_t *on) {
    Buf y = {0};
    size_t i = 0;
    while (i < pn) {
        uint8_t kind = p[i++];
        if (kind == 0) {
            if (i >= pn || buf_push(&y, p[i++])) {
                buf_free(&y);
                return NPCC_ERR_FORMAT;
            }
        } else if (kind == 1) {
            if (i + 3 > pn) {
                buf_free(&y);
                return NPCC_ERR_FORMAT;
            }
            int length = p[i++];
            int off = p[i] | (p[i + 1] << 8);
            i += 2;
            if (off == 0 || (size_t)off > y.n) {
                buf_free(&y);
                return NPCC_ERR_FORMAT;
            }
            for (int k = 0; k < length; k++) {
                if (buf_push(&y, y.p[y.n - (size_t)off])) {
                    buf_free(&y);
                    return NPCC_ERR_NOMEM;
                }
            }
        } else {
            buf_free(&y);
            return NPCC_ERR_FORMAT;
        }
    }
    if (orig && y.n != orig) {
        buf_free(&y);
        return NPCC_ERR_LEN;
    }
    *out = y.p;
    *on = y.n;
    return 0;
}

static int lzcm_compress(const uint8_t *x, size_t n, Buf *p) {
    if (n < 16) return 0;
    Buf tags = {0};
    if (lzcm_tags(x, n, &tags) <= 0) {
        buf_free(&tags);
        return 0;
    }
    Buf coded = {0};
    if (ac_encode2(tags.p, tags.n, freq_adaptive, NULL, &coded)) {
        buf_free(&tags);
        buf_free(&coded);
        return -1;
    }
    if (4 + coded.n < tags.n) {
        if (buf_push(p, 0xA1)) goto fail;
        uint8_t le[4] = {(uint8_t)tags.n, (uint8_t)(tags.n >> 8), (uint8_t)(tags.n >> 16),
                         (uint8_t)(tags.n >> 24)};
        if (buf_app(p, le, 4) || buf_app(p, coded.p, coded.n)) goto fail;
    } else {
        if (buf_push(p, 0xA0) || buf_app(p, tags.p, tags.n)) goto fail;
    }
    buf_free(&tags);
    buf_free(&coded);
    return 1;
fail:
    buf_free(&tags);
    buf_free(&coded);
    return -1;
}

static int lzcm_decompress(const uint8_t *p, size_t pn, size_t orig, uint8_t **out, size_t *on) {
    if (!pn) return NPCC_ERR_FORMAT;
    if (p[0] == 0xA0) return lzcm_untags(p + 1, pn - 1, orig, out, on);
    if (p[0] == 0xA1) {
        if (pn < 5) return NPCC_ERR_FORMAT;
        size_t nsym = (size_t)p[1] | ((size_t)p[2] << 8) | ((size_t)p[3] << 16) | ((size_t)p[4] << 24);
        uint8_t *tags = malloc(nsym ? nsym : 1);
        if (!tags) return NPCC_ERR_NOMEM;
        if (ac_decode2(p + 5, pn - 5, nsym, freq_adaptive, NULL, tags)) {
            free(tags);
            return NPCC_ERR_FORMAT;
        }
        int rc = lzcm_untags(tags, nsym, orig, out, on);
        free(tags);
        return rc;
    }
    return lzcm_untags(p, pn, orig, out, on);
}

static int neural_ar_compress(const uint8_t *x, size_t n, Buf *p) {
    if (!n) return 0;
    ar_init();
    uint8_t *c = NULL;
    size_t cn = 0;
    if (tnssrc_encode(x, n, &c, &cn)) return -1;
    int rc = buf_app(p, c, cn);
    free(c);
    return rc ? -1 : 1;
}
static int neural_ar_decompress(const uint8_t *p, size_t pn, size_t orig, uint8_t **out, size_t *on) {
    if (tnssrc_decode(p, pn, orig, out, on)) return NPCC_ERR_FORMAT;
    return 0;
}

static int prog_run(const uint8_t *prog, size_t pn, Buf *out, size_t limit) {
    size_t i = 0, steps = 0;
    while (i < pn) {
        uint8_t op = prog[i++];
        if (op == 0) break;
        if (op == 1) {
            if (i >= pn || buf_push(out, prog[i++])) return -1;
            steps++;
        } else if (op == 2) {
            if (i + 3 > pn) return -1;
            uint8_t b = prog[i++];
            uint16_t cnt = (uint16_t)(prog[i] | (prog[i + 1] << 8));
            i += 2;
            if (steps + cnt > limit) return -1;
            for (uint16_t k = 0; k < cnt; k++)
                if (buf_push(out, b)) return -1;
            steps += cnt;
        } else if (op == 3) {
            if (i + 3 > pn) return -1;
            uint16_t off = (uint16_t)(prog[i] | (prog[i + 1] << 8));
            uint8_t ln = prog[i + 2];
            i += 3;
            if (off == 0 || off > out->n) return -1;
            if (steps + ln > limit) return -1;
            for (uint8_t k = 0; k < ln; k++)
                if (buf_push(out, out->p[out->n - off])) return -1;
            steps += ln;
        } else
            return -1;
        if (steps > limit) return -1;
    }
    return 0;
}

static int neural_prog_search(const uint8_t *x, size_t n, int max_prog, Buf *best) {
    int have = 0;
    if (n && n <= 65535) {
        int same = 1;
        for (size_t i = 1; i < n; i++)
            if (x[i] != x[0]) {
                same = 0;
                break;
            }
        if (same) {
            uint8_t prog[6] = {2, x[0], (uint8_t)n, (uint8_t)(n >> 8), 0};
            Buf y = {0};
            if (prog_run(prog, 5, &y, 1000000) == 0 && y.n == n && memcmp(y.p, x, n) == 0) {
                uint8_t payload[6] = {1, 2, x[0], (uint8_t)n, (uint8_t)(n >> 8), 0};
                if ((int)sizeof payload <= max_prog) {
                    buf_free(best);
                    best->p = NULL;
                    best->n = best->cap = 0;
                    buf_app(best, payload, 6);
                    have = 1;
                }
            }
            buf_free(&y);
        }
    }
    if (n >= 8 && n <= 255) {
        int ok = 1;
        for (size_t i = 0; i < n; i++)
            if (x[i] != x[i % 2]) {
                ok = 0;
                break;
            }
        if (ok) {
            uint8_t prog[9] = {1, x[0], 1, x[1], 3, 2, 0, (uint8_t)(n - 2), 0};
            Buf y = {0};
            if (prog_run(prog, 9, &y, 1000000) == 0 && y.n == n && memcmp(y.p, x, n) == 0) {
                uint8_t payload[10] = {1, 1, x[0], 1, x[1], 3, 2, 0, (uint8_t)(n - 2), 0};
                if (10 <= max_prog && (!have || 10 < best->n)) {
                    buf_free(best);
                    best->p = NULL;
                    best->n = best->cap = 0;
                    buf_app(best, payload, 10);
                    have = 1;
                }
            }
            buf_free(&y);
        }
    }
    return have;
}

static int neural_prog_decompress(const uint8_t *p, size_t pn, size_t orig, uint8_t **out, size_t *on) {
    if (!pn || p[0] != 1) return NPCC_ERR_FORMAT;
    Buf y = {0};
    if (prog_run(p + 1, pn - 1, &y, 1000000)) {
        buf_free(&y);
        return NPCC_ERR_PROG;
    }
    if (y.n != orig) {
        buf_free(&y);
        return NPCC_ERR_LEN;
    }
    *out = y.p;
    *on = y.n;
    return 0;
}

int npcc_compress(const uint8_t *in, size_t n, const NpccBudget *b, uint8_t **out, size_t *out_n) {
    NpccBudget def;
    if (!b) {
        npcc_budget_full(&def);
        b = &def;
    }
    Cand best = {0};
    int have = 0;
    Buf p = {0};
    if (tru8_compress(in, n, &p) > 0)
        have = consider(&best, have, NPCC_TRU8, PRI_TRU8, n, p.p, p.n, NULL, 0);
    buf_free(&p);
    p = (Buf){0};
    int z = zeck_compress(in, n, &p);
    if (z > 0) have = consider(&best, have, NPCC_ZECK, PRI_ZECK, n, p.p, p.n, NULL, 0);
    buf_free(&p);
    if (!b->allow_neural) {
        p = (Buf){0};
        int l = lzcm_compress(in, n, &p);
        if (l > 0) have = consider(&best, have, NPCC_LZCM, PRI_LZ, n, p.p, p.n, NULL, 0);
        buf_free(&p);
    }
    if (b->allow_neural) {
        ar_init();
        p = (Buf){0};
        int a = neural_ar_compress(in, n, &p);
        if (a > 0)
            have = consider(&best, have, NPCC_NEURAL_AR, PRI_AR, n, p.p, p.n, MODEL_HASH, 32);
        buf_free(&p);
        if (b->allow_search) {
            Buf prog = {0};
            if (neural_prog_search(in, n, b->max_prog_len, &prog))
                have = consider(&best, have, NPCC_NEURAL_PROG, PRI_PROG, n, prog.p, prog.n, NULL, 0);
            buf_free(&prog);
        }
    }
    if (!have) {
        Buf blob = {0};
        if (pack_blob(NPCC_RAW, n, in, n, NULL, 0, &blob)) return NPCC_ERR_NOMEM;
        *out = blob.p;
        *out_n = blob.n;
        return NPCC_OK;
    }
    *out = best.blob.p;
    *out_n = best.blob.n;
    return NPCC_OK;
}

typedef struct {
    int version, pathway, flags;
    uint64_t orig, plen;
    const uint8_t *payload;
    const uint8_t *mhash;
    size_t hlen;
} Head;

static int unpack(const uint8_t *in, size_t n, Head *h) {
    if (n < 11 || memcmp(in, MAGIC, 4) != 0) return NPCC_ERR_FORMAT;
    uint16_t ver = (uint16_t)(in[4] | (in[5] << 8));
    if (ver != VERSION) return NPCC_ERR_FORMAT;
    h->version = ver;
    h->pathway = in[6];
    h->flags = in[7] | (in[8] << 8);
    size_t i = 9;
    if (read_uleb(in, n, &i, &h->orig) || read_uleb(in, n, &i, &h->plen)) return NPCC_ERR_FORMAT;
    if (i >= n) return NPCC_ERR_FORMAT;
    h->hlen = in[i++];
    if (i + h->hlen > n) return NPCC_ERR_FORMAT;
    h->mhash = in + i;
    i += h->hlen;
    uint64_t alen;
    if (read_uleb(in, n, &i, &alen)) return NPCC_ERR_FORMAT;
    i += (size_t)alen;
    if (i + h->plen + 4 != n) return NPCC_ERR_FORMAT;
    h->payload = in + i;
    i += (size_t)h->plen;
    uint32_t crc_got = (uint32_t)in[i] | ((uint32_t)in[i + 1] << 8) | ((uint32_t)in[i + 2] << 16) |
                       ((uint32_t)in[i + 3] << 24);
    uint32_t crc = crc32(0, in, (uInt)(i - h->plen));
    crc = crc32(crc, h->payload, (uInt)h->plen);
    if (crc != crc_got) return NPCC_ERR_CRC;
    return 0;
}

int npcc_peek(const uint8_t *in, size_t n, int *pathway, uint64_t *orig_len) {
    Head h;
    int rc = unpack(in, n, &h);
    if (rc) return rc;
    if (pathway) *pathway = h.pathway;
    if (orig_len) *orig_len = h.orig;
    return 0;
}

int npcc_decompress(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n) {
    Head h;
    int rc = unpack(in, n, &h);
    if (rc) return rc;
    uint8_t *y = NULL;
    size_t yn = 0;
    switch (h.pathway) {
    case NPCC_RAW:
        y = malloc(h.orig ? (size_t)h.orig : 1);
        if (!y) return NPCC_ERR_NOMEM;
        memcpy(y, h.payload, (size_t)h.orig);
        yn = (size_t)h.orig;
        break;
    case NPCC_TRU8:
        rc = tru8_decompress(h.payload, (size_t)h.plen, &y, &yn);
        break;
    case NPCC_ZECK:
        rc = zeck_decompress(h.payload, (size_t)h.plen, (size_t)h.orig, &y, &yn);
        break;
    case NPCC_LZCM:
        rc = lzcm_decompress(h.payload, (size_t)h.plen, (size_t)h.orig, &y, &yn);
        break;
    case NPCC_NEURAL_AR: {
        ar_init();
        if (h.hlen && (h.hlen != 32 || memcmp(h.mhash, MODEL_HASH, 32) != 0)) return NPCC_ERR_MODEL;
        rc = neural_ar_decompress(h.payload, (size_t)h.plen, (size_t)h.orig, &y, &yn);
        break;
    }
    case NPCC_NEURAL_PROG:
        rc = neural_prog_decompress(h.payload, (size_t)h.plen, (size_t)h.orig, &y, &yn);
        break;
    default:
        return NPCC_ERR_PATHWAY;
    }
    if (rc) return rc;
    if (yn != (size_t)h.orig) {
        free(y);
        return NPCC_ERR_LEN;
    }
    *out = y;
    *out_n = yn;
    return 0;
}


