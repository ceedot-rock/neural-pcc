#ifndef FRONTEND_H
#define FRONTEND_H

#include <stddef.h>
#include <stdint.h>

/* Front-end transform arms for TNSSRC.
 *
 * Each transform maps the raw input to a transformed byte stream that the
 * existing whole-file mode selection (modes 0/1/3/5/6/7) then compresses.
 * Side metadata is stored raw in the outer frame; the decoder reads the
 * transform id first, decodes the inner blob, then inverts.
 *
 * Outer frame: [mode u8][meta_len u32 LE][meta][inner blob]
 * meta:        [tn u64 LE][transform-specific...]   (tn = transformed length)
 *
 * Outer mode ids (inner blob keeps modes 0/1/3/5/6/7/8, where 8 is the
 * per-block exact-selection mode of the host tree; the front-end arms
 * therefore live at 9/10/11 to avoid collision):
 */
#define FE_MODE_SAO 9   /* sao columnar (28-byte records) */
#define FE_MODE_D16 10  /* d16 u16-delta */
#define FE_MODE_EXE 11  /* exe E8/E9 normalization */

/* apply(): returns 0 and fills the transformed-bytes outputs (tp/tn)
 * and side-metadata outputs (mp/mn; first 8 metadata bytes = tn LE)
 * when the transform structurally applies to the input. Returns nonzero
 * to decline. Caller frees the tp and mp buffers. Fully deterministic:
 * no randomness, no nondeterministic iteration. */
int fe_sao_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn);
int fe_d16_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn);
int fe_exe_apply(const uint8_t *in, size_t n, uint8_t **tp, size_t *tn,
                 uint8_t **mp, size_t *mn);

/* invert(): reconstruct out[0..n) from t[0..tn) + meta[0..mn).
 * Returns 0 on success, nonzero on any format error. */
int fe_sao_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n);
int fe_d16_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n);
int fe_exe_invert(const uint8_t *t, size_t tn, const uint8_t *meta, size_t mn,
                  uint8_t *out, size_t n);

#endif
