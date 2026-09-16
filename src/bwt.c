/* Cyclic suffix array (doubling) + BWT, MTF, bzip-style RLE0. */
#include "bwt.h"

#include <stdlib.h>
#include <string.h>

int bwt_fwd(const uint8_t *s, size_t n, uint8_t *L, uint32_t *primary) {
    if (!n) {
        *primary = 0;
        return 0;
    }
    int N = (int)n;
    int *p = malloc((size_t)N * sizeof(int));
    int *c = malloc((size_t)N * sizeof(int));
    int *pn = malloc((size_t)N * sizeof(int));
    int *cn = malloc((size_t)N * sizeof(int));
    int cntn = N > 256 ? N : 256;
    int *cnt = calloc((size_t)cntn, sizeof(int));
    if (!p || !c || !pn || !cn || !cnt) {
        free(p);
        free(c);
        free(pn);
        free(cn);
        free(cnt);
        return -1;
    }
    memset(cnt, 0, 256 * sizeof(int));
    for (int i = 0; i < N; i++) cnt[s[i]]++;
    for (int i = 1; i < 256; i++) cnt[i] += cnt[i - 1];
    for (int i = N - 1; i >= 0; i--) p[--cnt[s[i]]] = i;
    c[p[0]] = 0;
    int classes = 1;
    for (int i = 1; i < N; i++) {
        if (s[p[i]] != s[p[i - 1]]) classes++;
        c[p[i]] = classes - 1;
    }
    for (int h = 0; (1 << h) < N; h++) {
        for (int i = 0; i < N; i++) {
            pn[i] = p[i] - (1 << h);
            if (pn[i] < 0) pn[i] += N;
        }
        memset(cnt, 0, (size_t)classes * sizeof(int));
        for (int i = 0; i < N; i++) cnt[c[pn[i]]]++;
        for (int i = 1; i < classes; i++) cnt[i] += cnt[i - 1];
        for (int i = N - 1; i >= 0; i--) p[--cnt[c[pn[i]]]] = pn[i];
        cn[p[0]] = 0;
        classes = 1;
        int step = 1 << h;
        for (int i = 1; i < N; i++) {
            int i0 = p[i] + step;
            if (i0 >= N) i0 -= N;
            int i1 = p[i - 1] + step;
            if (i1 >= N) i1 -= N;
            int a0 = c[p[i]], b0 = c[i0];
            int a1 = c[p[i - 1]], b1 = c[i1];
            if (a0 != a1 || b0 != b1) classes++;
            cn[p[i]] = classes - 1;
        }
        int *sw = c;
        c = cn;
        cn = sw;
        if (classes == N) break;
    }
    *primary = 0;
    for (int i = 0; i < N; i++) {
        int j = p[i];
        L[i] = s[j ? j - 1 : N - 1];
        if (j == 0) *primary = (uint32_t)i;
    }
    free(p);
    free(c);
    free(pn);
    free(cn);
    free(cnt);
    return 0;
}

int bwt_inv(const uint8_t *L, size_t n, uint32_t primary, uint8_t *out) {
    if (!n) return 0;
    if (primary >= n) return -1;
    int N = (int)n;
    int C[256];
    memset(C, 0, sizeof C);
    for (int i = 0; i < N; i++) C[L[i]]++;
    int sum = 0;
    for (int i = 0; i < 256; i++) {
        int t = C[i];
        C[i] = sum;
        sum += t;
    }
    int *lf = malloc((size_t)N * sizeof(int));
    if (!lf) return -1;
    int occ[256];
    memset(occ, 0, sizeof occ);
    for (int i = 0; i < N; i++) {
        unsigned b = L[i];
        lf[i] = C[b] + occ[b];
        occ[b]++;
    }
    /* row `primary` is the original string; F[primary] is s[0], L[primary] is s[n-1] */
    int t = (int)primary;
    for (int i = N - 1; i >= 0; i--) {
        out[i] = L[t];
        t = lf[t];
    }
    free(lf);
    return 0;
}

void mtf_enc(const uint8_t *in, size_t n, uint8_t *out) {
    int nxt[256], prv[256], head = 0;
    for (int i = 0; i < 256; i++) {
        nxt[i] = i + 1;
        prv[i] = i - 1;
    }
    nxt[255] = -1;
    prv[0] = -1;
    for (size_t i = 0; i < n; i++) {
        int b = in[i], k = 0, x = head;
        while (x != b) {
            x = nxt[x];
            k++;
        }
        out[i] = (uint8_t)k;
        if (x != head) {
            if (nxt[x] >= 0) prv[nxt[x]] = prv[x];
            if (prv[x] >= 0) nxt[prv[x]] = nxt[x];
            prv[head] = x;
            nxt[x] = head;
            prv[x] = -1;
            head = x;
        }
    }
}

void mtf_dec(const uint8_t *in, size_t n, uint8_t *out) {
    int nxt[256], prv[256], head = 0;
    for (int i = 0; i < 256; i++) {
        nxt[i] = i + 1;
        prv[i] = i - 1;
    }
    nxt[255] = -1;
    prv[0] = -1;
    for (size_t i = 0; i < n; i++) {
        int k = in[i], x = head;
        for (int j = 0; j < k; j++) x = nxt[x];
        out[i] = (uint8_t)x;
        if (x != head) {
            if (nxt[x] >= 0) prv[nxt[x]] = prv[x];
            if (prv[x] >= 0) nxt[prv[x]] = nxt[x];
            prv[head] = x;
            nxt[x] = head;
            prv[x] = -1;
            head = x;
        }
    }
}

static void put_sym(uint16_t s, uint8_t **o, uint8_t *end) {
    if (*o >= end) return;
    if (s < 255) {
        *(*o)++ = (uint8_t)s;
    } else if (*o + 1 < end) {
        *(*o)++ = 255;
        *(*o)++ = (uint8_t)(s - 255);
    }
}

static void flush_zeros(uint32_t z, uint8_t **o, uint8_t *end) {
    while (z > 0 && *o < end) {
        if (z & 1)
            put_sym(0, o, end);
        else
            put_sym(1, o, end);
        z = (z - 1) >> 1;
    }
}

size_t rle0_enc(const uint8_t *ranks, size_t n, uint8_t *out, size_t cap) {
    uint8_t *o = out, *end = out + cap;
    uint32_t zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (ranks[i] == 0)
            zeros++;
        else {
            if (zeros) {
                flush_zeros(zeros, &o, end);
                zeros = 0;
            }
            put_sym((uint16_t)ranks[i] + 1, &o, end);
        }
    }
    if (zeros) flush_zeros(zeros, &o, end);
    return (size_t)(o - out);
}

int rle0_dec(const uint8_t *in, size_t n, uint8_t *ranks, size_t cap, size_t *outn) {
    size_t o = 0, i = 0;
    uint32_t run_pow = 0, pending = 0;
    while (i < n && o < cap) {
        uint16_t s;
        if (in[i] < 255) {
            s = in[i++];
        } else {
            if (i + 1 >= n) break;
            s = 255 + in[i + 1];
            i += 2;
        }
        if (s == 0 || s == 1) {
            pending += (uint32_t)(s + 1) << run_pow;
            run_pow++;
        } else {
            while (pending && o < cap) {
                ranks[o++] = 0;
                pending--;
            }
            run_pow = 0;
            pending = 0;
            if (o < cap) ranks[o++] = (uint8_t)(s - 1);
        }
    }
    while (pending && o < cap) {
        ranks[o++] = 0;
        pending--;
    }
    *outn = o;
    return 0;
}
