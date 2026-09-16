/* SickNode workbench tests: transform roundtrips, scan discovery, conductor. */
#include "workbench.h"
#include "tnssrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* deterministic LCG (no libc rand dependency) */
static uint32_t rng_state = 0x243F6A88u;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static int rt(int mode, uint64_t param, const uint8_t *in, size_t n, const char *tag) {
    uint8_t *t = NULL, *m = NULL;
    size_t tn = 0, mn = 0;
    if (wb_apply(mode, param, in, n, &t, &tn, &m, &mn)) {
        printf("FAIL: %s: apply declined\n", tag);
        return -1;
    }
    /* meta[0..8) must equal tn (frame contract) */
    uint64_t mtn = 0;
    for (int i = 0; i < 8; i++) mtn |= (uint64_t)m[i] << (8 * i);
    if (mtn != tn) {
        printf("FAIL: %s: meta tn %llu != %zu\n", tag, (unsigned long long)mtn, tn);
        free(t);
        free(m);
        return -1;
    }
    uint8_t *out = malloc(n ? n : 1);
    int rc = wb_invert(mode, param, t, tn, m, mn, out, n);
    int ok = rc == 0 && memcmp(out, in, n) == 0;
    if (!ok) printf("FAIL: %s: invert mismatch (rc=%d)\n", tag, rc);
    free(t);
    free(m);
    free(out);
    return ok ? 0 : -1;
}

/* strongly periodic record data: byte c of each record = c*37, except
 * lane 0 which carries the record counter */
static uint8_t *mk_records(size_t period, size_t nrec) {
    size_t n = period * nrec;
    uint8_t *in = malloc(n);
    for (size_t r = 0; r < nrec; r++)
        for (size_t c = 0; c < period; c++)
            in[r * period + c] = (uint8_t)(c == 0 ? r & 0xFF : (c * 37) & 0xFF);
    return in;
}

static void test_columnar(void) {
    uint8_t *a = mk_records(28, 400);
    CHECK(rt(WB_MODE_COLUMNAR, 28, a, 28 * 400, "columnar p28") == 0, "columnar p28");
    /* decline: param 0 / param > n */
    {
        uint8_t *t, *m;
        size_t tn, mn;
        CHECK(wb_apply(WB_MODE_COLUMNAR, 0, a, 28 * 400, &t, &tn, &m, &mn) != 0,
              "columnar declines param 0");
    }
    free(a);
    uint8_t *b = mk_records(37, 300);
    CHECK(rt(WB_MODE_COLUMNAR, 37, b, 37 * 300, "columnar p37") == 0, "columnar p37");
    /* non-multiple length: trailing bytes ride along raw */
    CHECK(rt(WB_MODE_COLUMNAR, 37, b, 37 * 300 + 11, "columnar p37+tail") == 0,
          "columnar p37+tail");
    free(b);
    printf("columnar tests done\n");
}

static void test_delta_xor(void) {
    size_t n = 70000;
    uint8_t *ramp = malloc(n);
    for (size_t i = 0; i < n / 2; i++) {
        uint16_t v = (uint16_t)((i * 3) & 0xFFFF);
        ramp[2 * i] = (uint8_t)v;
        ramp[2 * i + 1] = (uint8_t)(v >> 8);
    }
    CHECK(rt(WB_MODE_DELTA8, 0, ramp, n, "delta8 ramp") == 0, "delta8 ramp");
    CHECK(rt(WB_MODE_DELTA16, 0, ramp, n, "delta16 ramp") == 0, "delta16 ramp");
    CHECK(rt(WB_MODE_XOR16, 0, ramp, n, "xor16 ramp") == 0, "xor16 ramp");
    CHECK(rt(WB_MODE_XOR32, 0, ramp, n, "xor32 ramp") == 0, "xor32 ramp");
    free(ramp);
    size_t n3 = 69999; /* multiple of 3 */
    uint8_t *t3 = malloc(n3);
    for (size_t i = 0; i < n3; i++) t3[i] = (uint8_t)(i * 37 + 11);
    CHECK(rt(WB_MODE_DELTA24, 0, t3, n3, "delta24") == 0, "delta24");
    free(t3);
    size_t n4 = 70000;
    uint8_t *t4 = malloc(n4);
    for (size_t i = 0; i < n4 / 4; i++) {
        uint32_t v = (uint32_t)(i * 7919 + 13);
        t4[4 * i] = (uint8_t)v;
        t4[4 * i + 1] = (uint8_t)(v >> 8);
        t4[4 * i + 2] = (uint8_t)(v >> 16);
        t4[4 * i + 3] = (uint8_t)(v >> 24);
    }
    CHECK(rt(WB_MODE_DELTA32, 0, t4, n4, "delta32") == 0, "delta32");
    free(t4);
    /* odd-length delta8 still inverts (first byte raw) */
    uint8_t odd[999];
    for (int i = 0; i < 999; i++) odd[i] = (uint8_t)rnd();
    CHECK(rt(WB_MODE_DELTA8, 0, odd, 999, "delta8 odd") == 0, "delta8 odd");
    printf("delta/xor tests done\n");
}

static void test_exe(void) {
    size_t n = 70000;
    uint8_t *in = calloc(1, n);
    for (int k = 0; k < 300; k++) {
        size_t i = 100 + (size_t)k * 200;
        in[i] = 0xE8;
        int32_t rel = (int32_t)(5000 - k * 17);
        in[i + 1] = (uint8_t)rel;
        in[i + 2] = (uint8_t)(rel >> 8);
        in[i + 3] = (uint8_t)(rel >> 16);
        in[i + 4] = (uint8_t)(rel >> 24);
    }
    in[60000] = 0xE9;
    {
        int32_t rel = -123456;
        in[60001] = (uint8_t)rel;
        in[60002] = (uint8_t)(rel >> 8);
        in[60003] = (uint8_t)(rel >> 16);
        in[60004] = (uint8_t)(rel >> 24);
    }
    CHECK(rt(WB_MODE_EXE, 0, in, n, "exe") == 0, "exe roundtrip");
    free(in);
    printf("exe tests done\n");
}

static void test_img2d(void) {
    /* 1024x64 synthetic gradient image */
    size_t W = 64, rows = 1024, n = W * rows;
    uint8_t *in = malloc(n);
    for (size_t r = 0; r < rows; r++)
        for (size_t c = 0; c < W; c++)
            in[r * W + c] = (uint8_t)((r * 3 + c * 5 + ((r * c) & 15)) & 0xFF);
    CHECK(rt(WB_MODE_IMG2D, 0, in, n, "img2d") == 0, "img2d roundtrip");
    /* width discovery must have found 64: meta[8..12) = W u32 */
    {
        uint8_t *t, *m;
        size_t tn, mn;
        if (!wb_apply(WB_MODE_IMG2D, 0, in, n, &t, &tn, &m, &mn)) {
            uint32_t wfound = (uint32_t)m[8] | ((uint32_t)m[9] << 8) |
                              ((uint32_t)m[10] << 16) | ((uint32_t)m[11] << 24);
            CHECK(wfound == 64, "img2d width discovery = 64");
            free(t);
            free(m);
        } else {
            CHECK(0, "img2d unexpectedly declined");
        }
    }
    free(in);
    /* noise declines (no clear width) */
    {
        uint8_t *nz = malloc(n);
        for (size_t i = 0; i < n; i++) nz[i] = (uint8_t)rnd();
        uint8_t *t, *m;
        size_t tn, mn;
        CHECK(wb_apply(WB_MODE_IMG2D, 0, nz, n, &t, &tn, &m, &mn) != 0,
              "img2d declines noise");
        free(nz);
    }
    printf("img2d tests done\n");
}

static void test_bitplane_shuffle_raw(void) {
    size_t n = 4096;
    uint8_t *in = malloc(n);
    for (size_t i = 0; i < n; i++) in[i] = (uint8_t)rnd();
    CHECK(rt(WB_MODE_BITPLANE, 0, in, n, "bitplane random") == 0, "bitplane random");
    CHECK(rt(WB_MODE_SHUFFLE, 0, in, n, "shuffle n%4==0") == 0, "shuffle n%4==0");
    CHECK(rt(WB_MODE_RAW, 0, in, n, "raw") == 0, "raw");
    free(in);
    /* shuffle s=2 path (n even, not divisible by 4) */
    size_t n2 = 4098;
    uint8_t *in2 = malloc(n2);
    for (size_t i = 0; i < n2; i++) in2[i] = (uint8_t)rnd();
    CHECK(rt(WB_MODE_SHUFFLE, 0, in2, n2, "shuffle s=2") == 0, "shuffle s=2");
    free(in2);
    /* shuffle declines on odd n */
    {
        uint8_t *t, *m;
        size_t tn, mn;
        uint8_t odd[101];
        for (int i = 0; i < 101; i++) odd[i] = (uint8_t)i;
        CHECK(wb_apply(WB_MODE_SHUFFLE, 0, odd, 101, &t, &tn, &m, &mn) != 0,
              "shuffle declines odd n");
    }
    /* bitplane with non-multiple-of-8 length */
    {
        uint8_t nb[1003];
        for (int i = 0; i < 1003; i++) nb[i] = (uint8_t)(i * 13 + 7);
        CHECK(rt(WB_MODE_BITPLANE, 0, nb, 1003, "bitplane unaligned") == 0,
              "bitplane unaligned");
    }
    printf("bitplane/shuffle/raw tests done\n");
}

static void test_scan(void) {
    /* period 28 discovery */
    uint8_t *rec = mk_records(28, 400);
    wb_scan_t rep;
    CHECK(wb_scan(rec, 28 * 400, &rep) == 0, "scan rc");
    CHECK(rep.record_period == 28, "scan finds period 28");
    CHECK(rep.period_conf > 0.0, "scan period confidence > 0");
    CHECK(rep.size == 28 * 400, "scan size");
    free(rec);
    /* period 37 discovery */
    uint8_t *rec37 = mk_records(37, 300);
    CHECK(wb_scan(rec37, 37 * 300, &rep) == 0, "scan37 rc");
    CHECK(rep.record_period == 37, "scan finds period 37");
    free(rec37);
    /* smooth16 on aperiodic u16 ramp (quadratic term kills periodicity),
     * and not on noise */
    size_t n = 70000;
    uint8_t *ramp = malloc(n);
    for (size_t i = 0; i < n / 2; i++) {
        uint16_t v = (uint16_t)((i * 3 + (i * i >> 12)) & 0xFFFF);
        ramp[2 * i] = (uint8_t)v;
        ramp[2 * i + 1] = (uint8_t)(v >> 8);
    }
    CHECK(wb_scan(ramp, n, &rep) == 0, "scan ramp rc");
    CHECK(rep.smooth16, "scan smooth16 on ramp");
    CHECK(rep.record_period == 0, "scan no period on ramp");
    free(ramp);
    uint8_t *nz = malloc(n);
    for (size_t i = 0; i < n; i++) nz[i] = (uint8_t)rnd();
    CHECK(wb_scan(nz, n, &rep) == 0, "scan noise rc");
    CHECK(!rep.smooth16, "scan no smooth16 on noise");
    CHECK(rep.record_period == 0, "scan no period on noise");
    CHECK(rep.entropy > 7.9, "scan high entropy on noise");
    free(nz);
    /* tar sniff */
    {
        uint8_t blk[1024];
        memset(blk, 0, sizeof blk);
        memcpy(blk + 257, "ustar", 5);
        CHECK(wb_scan(blk, sizeof blk, &rep) == 0, "scan tar rc");
        CHECK(rep.is_tar, "scan tar detected");
    }
    printf("scan tests done\n");
}

static void test_bay1(void) {
    uint8_t *rec = mk_records(28, 400);
    wb_scan_t rep;
    wb_scan(rec, 28 * 400, &rep);
    wb_candidate_t cands[13];
    int nc = wb_bay1(&rep, cands, 0);
    CHECK(nc >= 2, "bay1 candidate count");
    CHECK(cands[0].mode == WB_MODE_RAW, "bay1 cands[0] is RAW");
    CHECK(nc <= 4, "bay1 shortlist length");
    /* columnar must be the top prediction on record data */
    CHECK(nc > 1 && cands[1].mode == WB_MODE_COLUMNAR, "bay1 predicts columnar");
    CHECK(nc > 1 && cands[1].param == 28, "bay1 columnar param = period");
    free(rec);
    /* full mode boards everything structural incl. delta24 */
    {
        size_t n = 70002; /* %2==0, %3==0, >=64KiB */
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 37 + 11);
        wb_scan(buf, n, &rep);
        int nf = wb_bay1(&rep, cands, 1);
        int seen_d24 = 0;
        for (int i = 0; i < nf; i++)
            if (cands[i].mode == WB_MODE_DELTA24) seen_d24 = 1;
        CHECK(seen_d24, "bay1 full boards delta24");
        CHECK(nf > nc || nf >= 4, "bay1 full >= bay1 short");
        free(buf);
    }
    printf("bay1 tests done\n");
}

static void test_conductor(void) {
    /* small synthetic file: period-37 record head (the inner selector's
     * fixed {28,24,20,32} widths cannot exploit it) + u16 ramp tail */
    size_t nrec = 37 * 800;
    size_t nramp = 24576;
    size_t n = nrec + nramp;
    uint8_t *in = malloc(n);
    uint8_t *rec = mk_records(37, 800);
    memcpy(in, rec, nrec);
    free(rec);
    for (size_t i = 0; i < nramp / 2; i++) {
        uint16_t v = (uint16_t)((i * 5 + 1000) & 0xFFFF);
        in[nrec + 2 * i] = (uint8_t)v;
        in[nrec + 2 * i + 1] = (uint8_t)(v >> 8);
    }
    uint8_t *enc = NULL, *dec = NULL;
    size_t en = 0, dn = 0;
    CHECK(wb_encode(in, n, &enc, &en, 0) == 0, "wb_encode rc");
    CHECK(en < n, "wb_encode shrinks structured input");
    CHECK(wb_decode(enc, en, n, &dec, &dn) == 0, "wb_decode rc");
    CHECK(dn == n && memcmp(dec, in, n) == 0, "conductor roundtrip");
    /* a transform must win the exact minimum: a real outer frame */
    if (en > 0)
        printf("conductor winner mode=%d size=%zu\n", enc[0], en);
    CHECK(en > 0 && enc[0] >= WB_MODE_COLUMNAR && enc[0] <= WB_MODE_SHUFFLE,
          "conductor picks transform frame");
    free(enc);
    free(dec);
    /* full-battery roundtrip on the same file */
    CHECK(wb_encode(in, n, &enc, &en, 1) == 0, "wb_encode full rc");
    CHECK(wb_decode(enc, en, n, &dec, &dn) == 0, "wb_decode full rc");
    CHECK(dn == n && memcmp(dec, in, n) == 0, "conductor full roundtrip");
    free(enc);
    free(dec);
    free(in);

    /* RAW byte-identity on constant data: no transform can beat raw here
     * (every transform output has identical inner cost + frame overhead),
     * so the raw path must win and be byte-identical to tnssrc_encode_inner. */
    {
        size_t cn = 8192;
        uint8_t *c = calloc(1, cn);
        uint8_t *a = NULL, *b = NULL;
        size_t an = 0, bn = 0;
        int rca = wb_encode(c, cn, &a, &an, 0);
        int rcb = tnssrc_encode_inner(c, cn, &b, &bn);
        CHECK(rca == 0 && rcb == 0, "raw identity: both encode");
        if (rca == 0 && rcb == 0) {
            int is_raw_frame = a[0] < WB_MODE_COLUMNAR || a[0] > WB_MODE_SHUFFLE;
            CHECK(is_raw_frame, "raw wins on constant data");
            if (is_raw_frame)
                CHECK(an == bn && memcmp(a, b, an) == 0, "raw byte-identical");
        }
        free(a);
        a = NULL;
        rca = wb_encode(c, cn, &a, &an, 1);
        if (rca == 0 && rcb == 0) {
            int is_raw_frame = a[0] < WB_MODE_COLUMNAR || a[0] > WB_MODE_SHUFFLE;
            CHECK(is_raw_frame && an == bn && memcmp(a, b, an) == 0,
                  "raw byte-identical (full)");
        } else {
            CHECK(0, "raw identity full: both encode");
        }
        free(a);
        free(b);
        free(c);
    }
    /* decode rejects garbage */
    {
        uint8_t g[64];
        for (int i = 0; i < 64; i++) g[i] = (uint8_t)rnd();
        uint8_t *o = NULL;
        size_t on = 0;
        CHECK(wb_decode(g, 64, 1000, &o, &on) != 0, "decode rejects garbage");
        CHECK(wb_decode(g, 0, 1000, &o, &on) != 0, "decode rejects empty");
    }
    printf("conductor tests done\n");
}

int main(void) {
    test_columnar();
    test_delta_xor();
    test_exe();
    test_img2d();
    test_bitplane_shuffle_raw();
    test_scan();
    test_bay1();
    test_conductor();
    if (fails == 0) printf("workbench: all tests passed\n");
    else printf("workbench: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}
