#include "npcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const uint8_t *x, size_t n) {
    uint8_t *c = NULL, *y = NULL;
    size_t cn = 0, yn = 0;
    int rc = npcc_compress(x, n, NULL, &c, &cn);
    if (rc) {
        fprintf(stderr, "compress %zu: %s\n", n, npcc_strerror(rc));
        return 1;
    }
    rc = npcc_decompress(c, cn, &y, &yn);
    if (rc || yn != n || memcmp(y, x, n) != 0) {
        int path = -1;
        npcc_peek(c, cn, &path, NULL);
        fprintf(stderr, "roundtrip fail n=%zu packed=%zu path=%s rc=%s\n", n, cn,
                npcc_pathway_name(path), npcc_strerror(rc));
        free(c);
        free(y);
        return 1;
    }
    int path = -1;
    npcc_peek(c, cn, &path, NULL);
    printf("%6zu -> %6zu  path=%s\n", n, cn, npcc_pathway_name(path));
    free(c);
    free(y);
    return 0;
}

int main(void) {
    int fail = 0;
    fail |= check((const uint8_t *)"", 0);
    uint8_t z1[1] = {0};
    fail |= check(z1, 1);
    uint8_t z100[100];
    memset(z100, 0, 100);
    fail |= check(z100, 100);
    uint8_t *z2000 = calloc(2000, 1);
    fail |= check(z2000, 2000);
    uint8_t a500[500];
    memset(a500, 'A', 500);
    fail |= check(a500, 500);
    uint8_t ab[800];
    for (int i = 0; i < 800; i++) ab[i] = (uint8_t)("ab"[i & 1]);
    fail |= check(ab, 800);
    const char *fox = "The quick brown fox jumps over the lazy dog. ";
    uint8_t foxr[900];
    size_t fl = strlen(fox);
    for (int i = 0; i < 20; i++) memcpy(foxr + i * fl, fox, fl);
    fail |= check(foxr, 20 * fl);
    uint8_t r256[256];
    for (int i = 0; i < 256; i++) r256[i] = (uint8_t)i;
    fail |= check(r256, 256);
    uint8_t r1024[1024];
    for (int i = 0; i < 1024; i++) r1024[i] = (uint8_t)(i & 255);
    fail |= check(r1024, 1024);
    uint8_t ff00[600];
    for (int i = 0; i < 600; i++) ff00[i] = (uint8_t)((i & 1) ? 0 : 0xff);
    fail |= check(ff00, 600);
    free(z2000);
    if (fail) return 1;
    puts("ok");
    return 0;
}
