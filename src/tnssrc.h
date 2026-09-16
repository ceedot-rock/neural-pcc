#ifndef TNSSRC_H
#define TNSSRC_H

#include <stddef.h>
#include <stdint.h>

#define TNSSRC_MODEL_ID "tnssrc-v1"

/* TriNeural Shared Spine Row Compression.
 * Compressor 2. Three heads, one spine per 8-bit row. Online. Bit-exact. */
int tnssrc_encode(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int tnssrc_decode(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on);

/* Inner exact-minimum selection (modes 0/1/3/5/6/7 + 8 per-block).
 * The public tnssrc_encode/tnssrc_decode wrap these with the front-end
 * transform arms (outer modes 9/10/11); raw output is byte-identical. */
int tnssrc_encode_inner(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int tnssrc_decode_inner(const uint8_t *in, size_t n, size_t orig, uint8_t **out, size_t *on);

#endif
