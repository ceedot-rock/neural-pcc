/* BT4 + hash-4 chain + 4-rep block DP. Champ MATCH parse (lbr1 parse_rep4). */
#include "parse_rep4.h"

#include <stdlib.h>
#include <string.h>

#define MINM 4
#define MAXM 65535
#define INF (0xFFFFFFFFu / 4)
#define HASH18 (1u << 18)
#define HASH20 (1u << 20)

static size_t match_len(const uint8_t *s, size_t i, size_t j, size_t cap, size_t n) {
    size_t max = cap;
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

static uint32_t bits_lit(void) { return 10; }
static uint32_t bits_rep(uint32_t len) {
    uint32_t extra = len - MINM;
    uint32_t lb = extra < 8 ? 4 : extra < 16 ? 6 : extra < 31 ? 10 : 26;
    return 1 + 1 + 2 + lb;
}
static uint32_t bits_new(uint32_t dist, uint32_t len) {
    uint32_t extra = len - MINM;
    uint32_t lb = extra < 8 ? 4 : extra < 16 ? 6 : extra < 31 ? 10 : 26;
    uint32_t slot = 32 - (dist ? __builtin_clz(dist) : 32);
    return 1 + 1 + lb + 5 + (slot ? slot - 1 : 0);
}

static void bump_reps(uint32_t r[4], uint32_t d) {
    if (!d || d == r[0]) return;
    if (d == r[1]) {
        r[1] = r[0];
        r[0] = d;
        return;
    }
    if (d == r[2]) {
        r[2] = r[1];
        r[1] = r[0];
        r[0] = d;
        return;
    }
    r[3] = r[2];
    r[2] = r[1];
    r[1] = r[0];
    r[0] = d;
}

static void consider_dual(uint32_t *bl, uint32_t *bd, uint32_t *al, uint32_t *ad, const uint8_t *s,
                         size_t i, size_t j, size_t n) {
    if (j >= i || i + 4 > n || j + 4 > n) return;
    if (s[j] != s[i] || s[j + 1] != s[i + 1] || s[j + 2] != s[i + 2] || s[j + 3] != s[i + 3])
        return;
    uint32_t L = (uint32_t)match_len(s, i, j, MAXM, n);
    if (L < MINM) return;
    uint32_t dist = (uint32_t)(i - j);
    if (L > *bl) {
        if (*bd && *bd != dist && *bl > *al) {
            *al = *bl;
            *ad = *bd;
        }
        *bl = L;
        *bd = dist;
    } else if (dist != *bd && L > *al) {
        *al = L;
        *ad = dist;
    }
}

static size_t lens_try(uint32_t best, uint32_t *out) {
    if (best < MINM) return 0;
    size_t n = 0;
    out[n++] = best;
    static const uint32_t ks[] = {16384, 8192, 4096, 2048, 1024, 768, 512, 384, 256, 128, 64, 48, 32, 24, 16, 12};
    for (size_t i = 0; i < sizeof ks / sizeof ks[0] && n < 15; i++)
        if (ks[i] < best && ks[i] >= MINM) out[n++] = ks[i];
    uint32_t l = best < 8 ? best : 8;
    while (l >= MINM && n < 16) {
        int seen = 0;
        for (size_t i = 0; i < n; i++)
            if (out[i] == l) seen = 1;
        if (!seen) out[n++] = l;
        if (l == MINM) break;
        l--;
    }
    return n;
}

static void bt4_fill(const uint8_t *s, size_t n, size_t win, uint32_t *best_d, uint32_t *best_l,
                     uint32_t *alt_d, uint32_t *alt_l) {
    int32_t *hash = malloc(HASH18 * sizeof(int32_t));
    int32_t *son = malloc(win * 2 * sizeof(int32_t));
    if (!hash || !son) {
        free(hash);
        free(son);
        return;
    }
    memset(hash, 0xFF, HASH18 * sizeof(int32_t));
    memset(son, 0xFF, win * 2 * sizeof(int32_t));
    const int depth = 16;
    for (size_t pos = 0; pos + MINM <= n; pos++) {
        uint32_t v;
        memcpy(&v, s + pos, 4);
        size_t h = (v * 0x85EBCA6Bu) >> (32 - 18);
        h &= HASH18 - 1;
        int32_t cur = hash[h];
        hash[h] = (int32_t)pos;
        int32_t floor = pos > win ? (int32_t)(pos - win) : -1;
        size_t slot = (pos % win) * 2;
        size_t ptr0 = slot, ptr1 = slot + 1;
        size_t len0 = 0, len1 = 0;
        uint32_t bd = 0, bl = 0, ad = 0, al = 0;
        int zero = (s[pos] | s[pos + 1] | s[pos + 2] | s[pos + 3]) == 0;
        int cap = zero ? 8 : depth;
        int steps = 0;
        for (;;) {
            if (cur <= floor || steps >= cap) {
                son[ptr0] = son[ptr1] = -1;
                break;
            }
            steps++;
            size_t j = (size_t)cur;
            if (j >= pos) {
                son[ptr0] = son[ptr1] = -1;
                break;
            }
            size_t pair = (j % win) * 2;
            size_t len = len0 < len1 ? len0 : len1;
            size_t max = n - pos;
            if (n - j < max) max = n - j;
            if (max > MAXM) max = MAXM;
            while (len + 8 <= max) {
                uint64_t a, b;
                memcpy(&a, s + pos + len, 8);
                memcpy(&b, s + j + len, 8);
                if (a != b) break;
                len += 8;
            }
            while (len < max && s[pos + len] == s[j + len]) len++;
            uint32_t dist = (uint32_t)(pos - j);
            if ((uint32_t)len > bl) {
                if (bd && bd != dist) {
                    ad = bd;
                    al = bl;
                }
                bl = (uint32_t)len;
                bd = dist;
                if (len >= max) {
                    son[ptr0] = son[pair];
                    son[ptr1] = son[pair + 1];
                    break;
                }
            } else if ((uint32_t)len >= MINM && dist != bd && (uint32_t)len > al) {
                al = (uint32_t)len;
                ad = dist;
            }
            if (j + len >= n || s[j + len] < s[pos + len]) {
                son[ptr1] = cur;
                ptr1 = pair + 1;
                cur = son[ptr1];
                len1 = len;
            } else {
                son[ptr0] = cur;
                ptr0 = pair;
                cur = son[ptr0];
                len0 = len;
            }
        }
        best_d[pos] = bd;
        best_l[pos] = bl;
        alt_d[pos] = ad;
        alt_l[pos] = al;
    }
    free(hash);
    free(son);
}

static void chain_improve(const uint8_t *s, size_t n, size_t win, uint32_t *best_d, uint32_t *best_l,
                          uint32_t *alt_d, uint32_t *alt_l) {
    int32_t *head = malloc(HASH20 * sizeof(int32_t));
    int32_t *prevc = malloc(win * sizeof(int32_t));
    if (!head || !prevc) {
        free(head);
        free(prevc);
        return;
    }
    memset(head, 0xFF, HASH20 * sizeof(int32_t));
    memset(prevc, 0xFF, win * sizeof(int32_t));
    for (size_t i = 0; i + MINM <= n; i++) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        size_t h = (v * 0x85EBCA6Bu) >> 12;
        h &= HASH20 - 1;
        int32_t floor = i > win ? (int32_t)(i - win) : -1;
        int zero = (s[i] | s[i + 1] | s[i + 2] | s[i + 3]) == 0;
        int cap = zero ? 8 : 64;
        int32_t p = head[h];
        int steps = 0;
        uint32_t bd = best_d[i], bl = best_l[i], ad = alt_d[i], al = alt_l[i];
        while (p > floor && steps < cap) {
            size_t j = (size_t)p;
            if (j < i) consider_dual(&bl, &bd, &al, &ad, s, i, j, n);
            p = prevc[j % win];
            steps++;
        }
        best_d[i] = bd;
        best_l[i] = bl;
        alt_d[i] = ad;
        alt_l[i] = al;
        if (i + 3 < n) {
            prevc[i % win] = head[h];
            head[h] = (int32_t)i;
        }
    }
    free(head);
    free(prevc);
}

int parse_rep4(const uint8_t *data, size_t n, uint32_t window, uint32_t *tok_dist, uint32_t *tok_len,
               size_t *ntok) {
    if (!n) {
        *ntok = 0;
        return 0;
    }
    size_t win = window;
    if (win < 256) win = 256;
    if (win > n) win = n;
    uint32_t *best_d = calloc(n, 4);
    uint32_t *best_l = calloc(n, 4);
    uint32_t *alt_d = calloc(n, 4);
    uint32_t *alt_l = calloc(n, 4);
    if (!best_d || !best_l || !alt_d || !alt_l) {
        free(best_d);
        free(best_l);
        free(alt_d);
        free(alt_l);
        return -1;
    }
    bt4_fill(data, n, win, best_d, best_l, alt_d, alt_l);
    chain_improve(data, n, win, best_d, best_l, alt_d, alt_l);
    /* HC8: 8-byte fingerprint chain, skip if primary already long. */
    {
        int32_t *h8 = malloc((1u << 18) * sizeof(int32_t));
        int32_t *p8 = malloc(win * sizeof(int32_t));
        if (h8 && p8) {
            memset(h8, 0xFF, (1u << 18) * sizeof(int32_t));
            memset(p8, 0xFF, win * sizeof(int32_t));
            for (size_t i = 0; i + 8 <= n; i++) {
                uint32_t lo, hi;
                memcpy(&lo, data + i, 4);
                memcpy(&hi, data + i + 4, 4);
                uint64_t v = (uint64_t)lo | ((uint64_t)hi << 32);
                size_t h = (v * 0x9E3779B185EBCA77ull) >> (64 - 18);
                h &= (1u << 18) - 1;
                int32_t floor = i > win ? (int32_t)(i - win) : -1;
                if (best_l[i] < 256) {
                    int32_t p = h8[h];
                    int steps = 0, cap = 32;
                    uint32_t bd = best_d[i], bl = best_l[i], ad = alt_d[i], al = alt_l[i];
                    while (p > floor && steps < cap) {
                        size_t j = (size_t)p;
                        if (j < i) consider_dual(&bl, &bd, &al, &ad, data, i, j, n);
                        p = p8[j % win];
                        steps++;
                    }
                    best_d[i] = bd;
                    best_l[i] = bl;
                    alt_d[i] = ad;
                    alt_l[i] = al;
                }
                p8[i % win] = h8[h];
                h8[h] = (int32_t)i;
            }
        }
        free(h8);
        free(p8);
    }
    size_t block = n > 16 * 1024 * 1024 ? 8 * 1024 * 1024 : 4 * 1024 * 1024;
    size_t pos = 0, nt = 0;
    uint32_t file_reps[4] = {0, 0, 0, 0};
    while (pos < n) {
        size_t end = pos + block;
        if (end > n) end = n;
        size_t m = end - pos;
        uint32_t *price = malloc((m + 1) * 4);
        int32_t *come = malloc((m + 1) * 4);
        uint32_t *come_d = calloc(m + 1, 4);
        uint32_t *r0 = malloc((m + 1) * 4);
        uint32_t *r1 = malloc((m + 1) * 4);
        uint32_t *r2 = malloc((m + 1) * 4);
        uint32_t *r3 = malloc((m + 1) * 4);
        if (!price || !come || !come_d || !r0 || !r1 || !r2 || !r3) {
            free(price);
            free(come);
            free(come_d);
            free(r0);
            free(r1);
            free(r2);
            free(r3);
            free(best_d);
            free(best_l);
            free(alt_d);
            free(alt_l);
            return -1;
        }
        for (size_t i = 0; i <= m; i++) {
            price[i] = INF;
            come[i] = -1;
        }
        price[0] = 0;
        r0[0] = file_reps[0];
        r1[0] = file_reps[1];
        r2[0] = file_reps[2];
        r3[0] = file_reps[3];
        uint32_t cand[16];
        for (size_t k = 0; k < m; k++) {
            if (price[k] == INF) continue;
            size_t i = pos + k;
            uint32_t pl = price[k] + bits_lit();
            if (pl < price[k + 1]) {
                price[k + 1] = pl;
                come[k + 1] = -1;
                r0[k + 1] = r0[k];
                r1[k + 1] = r1[k];
                r2[k + 1] = r2[k];
                r3[k + 1] = r3[k];
            }
            uint32_t reps[4] = {r0[k], r1[k], r2[k], r3[k]};
            if (i + MINM <= n) {
                for (int ri = 0; ri < 4; ri++) {
                    uint32_t rd = reps[ri];
                    if (!rd || rd > i) continue;
                    size_t cap = n - i;
                    if (m - k < cap) cap = m - k;
                    if (cap > MAXM) cap = MAXM;
                    uint32_t lr = (uint32_t)match_len(data, i, i - rd, cap, n);
                    size_t nc = lens_try(lr, cand);
                    for (size_t t = 0; t < nc; t++) {
                        uint32_t len = cand[t];
                        size_t j = k + len;
                        if (j > m) continue;
                        uint32_t pm = price[k] + bits_rep(len);
                        int better = pm < price[j] || (pm == price[j] && come[j] >= 0 && (int32_t)len > come[j]);
                        if (better) {
                            price[j] = pm;
                            come[j] = (int32_t)len;
                            come_d[j] = rd;
                            uint32_t nr[4] = {reps[0], reps[1], reps[2], reps[3]};
                            bump_reps(nr, rd);
                            r0[j] = nr[0];
                            r1[j] = nr[1];
                            r2[j] = nr[2];
                            r3[j] = nr[3];
                        }
                    }
                }
            }
            uint32_t ds[2] = {best_d[i], alt_d[i]};
            uint32_t ls[2] = {best_l[i], alt_l[i]};
            for (int ci = 0; ci < 2; ci++) {
                uint32_t dist = ds[ci], bl = ls[ci];
                if (bl < MINM || !dist) continue;
                int is_rep = dist == reps[0] || dist == reps[1] || dist == reps[2] || dist == reps[3];
                uint32_t lim = bl;
                if ((uint32_t)(m - k) < lim) lim = (uint32_t)(m - k);
                size_t nc = lens_try(lim, cand);
                for (size_t ti = 0; ti < nc; ti++) {
                    uint32_t len = cand[ti];
                    size_t j = k + len;
                    if (j > m) continue;
                    uint32_t add = is_rep ? bits_rep(len) : bits_new(dist, len);
                    uint32_t pm = price[k] + add;
                    int better = pm < price[j] || (pm == price[j] && come[j] >= 0 && (int32_t)len > come[j]);
                    if (better) {
                        price[j] = pm;
                        come[j] = (int32_t)len;
                        come_d[j] = dist;
                        uint32_t nr[4] = {reps[0], reps[1], reps[2], reps[3]};
                        bump_reps(nr, dist);
                        r0[j] = nr[0];
                        r1[j] = nr[1];
                        r2[j] = nr[2];
                        r3[j] = nr[3];
                    }
                }
            }
        }
        /* reconstruct */
        size_t k = m;
        size_t local_n = 0;
        uint32_t *ld = malloc((m + 2) * 4);
        uint32_t *ll = malloc((m + 2) * 4);
        if (!ld || !ll) {
            free(ld);
            free(ll);
            free(price);
            free(come);
            free(come_d);
            free(r0);
            free(r1);
            free(r2);
            free(r3);
            free(best_d);
            free(best_l);
            free(alt_d);
            free(alt_l);
            return -1;
        }
        while (k > 0) {
            if (come[k] < 0) {
                ld[local_n] = data[pos + k - 1];
                ll[local_n] = 0;
                local_n++;
                k--;
            } else {
                uint32_t len = (uint32_t)come[k];
                if (!len || len > k) {
                    ld[local_n] = data[pos + k - 1];
                    ll[local_n] = 0;
                    local_n++;
                    k--;
                    continue;
                }
                ld[local_n] = come_d[k];
                ll[local_n] = len;
                local_n++;
                k -= len;
            }
        }
        for (size_t t = local_n; t-- > 0;) {
            tok_dist[nt] = ld[t];
            tok_len[nt] = ll[t];
            if (ll[t] >= MINM) bump_reps(file_reps, ld[t]);
            nt++;
        }
        file_reps[0] = r0[m];
        file_reps[1] = r1[m];
        file_reps[2] = r2[m];
        file_reps[3] = r3[m];
        free(ld);
        free(ll);
        free(price);
        free(come);
        free(come_d);
        free(r0);
        free(r1);
        free(r2);
        free(r3);
        pos = end;
    }
    free(best_d);
    free(best_l);
    free(alt_d);
    free(alt_l);
    *ntok = nt;
    return 0;
}
