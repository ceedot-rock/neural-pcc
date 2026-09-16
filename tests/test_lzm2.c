#include "lzm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int roundtrip(const uint8_t *x, size_t n, const char *tag) {
    uint8_t *c = NULL, *y = NULL;
    size_t cn = 0, yn = 0;
    if (lzm2_encode(x, n, &c, &cn)) {
        fprintf(stderr, "encode fail %s n=%zu\n", tag, n);
        return 1;
    }
    if (lzm2_decode(c, cn, n, &y, &yn) || yn != n || memcmp(y, x, n) != 0) {
        fprintf(stderr, "roundtrip fail %s n=%zu packed=%zu yn=%zu\n", tag, n, cn, yn);
        free(c);
        free(y);
        return 1;
    }
    printf("%-16s %6zu -> %6zu\n", tag, n, cn);
    free(c);
    free(y);
    return 0;
}

int main(void) {
    int fail = 0;
    uint8_t zeros[256];
    memset(zeros, 0, sizeof zeros);
    fail |= roundtrip(zeros, sizeof zeros, "zeros");

    uint8_t as[400];
    memset(as, 'A', sizeof as);
    fail |= roundtrip(as, sizeof as, "AAAA");

    uint8_t ab[800];
    for (int i = 0; i < 800; i++) ab[i] = (uint8_t)("ab"[i & 1]);
    fail |= roundtrip(ab, sizeof ab, "ab-repeat");

    const char *fox = "The quick brown fox jumps over the lazy dog. ";
    size_t fl = strlen(fox);
    uint8_t foxr[45 * 20];
    for (int i = 0; i < 20; i++) memcpy(foxr + i * fl, fox, fl);
    fail |= roundtrip(foxr, sizeof foxr, "fox");

    uint8_t ramp[1024];
    for (int i = 0; i < 1024; i++) ramp[i] = (uint8_t)(i & 255);
    fail |= roundtrip(ramp, sizeof ramp, "ramp");

    uint8_t ff00[600];
    for (int i = 0; i < 600; i++) ff00[i] = (uint8_t)((i & 1) ? 0 : 0xff);
    fail |= roundtrip(ff00, sizeof ff00, "ff00");

    uint8_t *mix = malloc(4096);
    if (!mix) return 1;
    for (int i = 0; i < 4096; i++) mix[i] = (uint8_t)((i * 73u + (i >> 3)) & 255);
    memcpy(mix + 200, mix, 180);
    memcpy(mix + 800, mix + 100, 400);
    memcpy(mix + 2000, mix + 50, 900);
    fail |= roundtrip(mix, 4096, "mix4k");
    free(mix);

    uint8_t *big = malloc(65536);
    if (!big) return 1;
    for (int i = 0; i < 65536; i++) big[i] = (uint8_t)((i * 131u) ^ (i >> 5));
    memmove(big + 1000, big, 3000);
    memmove(big + 20000, big + 500, 8000);
    fail |= roundtrip(big, 65536, "mix64k");
    free(big);

    if (fail) return 1;
    puts("lzm2 ok");
    return 0;
}
