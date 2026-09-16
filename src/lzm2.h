#ifndef LZM2_H
#define LZM2_H

#include <stddef.h>
#include <stdint.h>

#define LZM2_MAGIC "LZM2"

int lzm2_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int lzm2_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on);

#endif
