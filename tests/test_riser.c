#include "riser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rt(const uint8_t *x, size_t n) {
    uint8_t *c = NULL, *y = NULL;
    size_t cn = 0, yn = 0;
    if (aip_quik_build(x, n, &c, &cn)) {
        fprintf(stderr, "build fail n=%zu\n", n);
        return 1;
    }
    if (!aip_is_frame(c, cn)) {
        fprintf(stderr, "not AIP n=%zu packed=%zu\n", n, cn);
        free(c);
        return 1;
    }
    if (aip_neat_destruct(c, cn, &y, &yn) || yn != n || memcmp(y, x, n) != 0) {
        fprintf(stderr, "destruct fail n=%zu packed=%zu yn=%zu\n", n, cn, yn);
        free(c);
        free(y);
        return 1;
    }
    printf("riser %6zu -> %6zu  kind=%u\n", n, cn, (unsigned)c[5]);
    free(c);
    free(y);
    return 0;
}

int main(void) {
    int fail = 0;
    fail |= rt((const uint8_t *)"", 0);
    uint8_t z[200];
    memset(z, 0, sizeof z);
    fail |= rt(z, sizeof z);
    uint8_t a[400];
    memset(a, 'A', sizeof a);
    fail |= rt(a, sizeof a);
    uint8_t r[512];
    for (int i = 0; i < 512; i++) r[i] = (uint8_t)i;
    fail |= rt(r, sizeof r);
    const char *s = "The quick brown fox jumps over the lazy dog. ";
    uint8_t fox[45 * 8];
    for (int i = 0; i < 8; i++) memcpy(fox + i * 45, s, 45);
    fail |= rt(fox, sizeof fox);
    if (fail) return 1;
    puts("riser ok");
    return 0;
}
