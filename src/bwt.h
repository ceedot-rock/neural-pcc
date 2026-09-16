#ifndef NPCC_BWT_H
#define NPCC_BWT_H

#include <stddef.h>
#include <stdint.h>

int bwt_fwd(const uint8_t *s, size_t n, uint8_t *L, uint32_t *primary);
int bwt_inv(const uint8_t *L, size_t n, uint32_t primary, uint8_t *out);
void mtf_enc(const uint8_t *in, size_t n, uint8_t *out);
void mtf_dec(const uint8_t *in, size_t n, uint8_t *out);
size_t rle0_enc(const uint8_t *ranks, size_t n, uint8_t *out, size_t cap);
int rle0_dec(const uint8_t *in, size_t n, uint8_t *ranks, size_t cap, size_t *outn);

#endif
