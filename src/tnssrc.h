#ifndef TNSSRC_H
#define TNSSRC_H

#include <stddef.h>
#include <stdint.h>

#define TNSSRC_MODEL_ID "tnssrc-v1"

/* TriNeural Shared Spine Row Compression.
 * Compressor 2. Three heads, one spine per 8-bit row. Online. Bit-exact. */
int tnssrc_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int tnssrc_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on);

#endif
