#ifndef NPCC_RISER_H
#define NPCC_RISER_H

#include <stddef.h>
#include <stdint.h>

#define RISER_MODEL_ID "riser-v1"
#define AIP_MAGIC "AIP1"

/* Auto Intelligent Protocol.
 * quik.build  = neural encode into an AIP BUILD frame
 * neat.destruct = exact restore from AIP (or raw refuse)
 */
int aip_quik_build(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int aip_neat_destruct(const uint8_t *in, size_t n, uint8_t **out, size_t *on);
int aip_is_frame(const uint8_t *in, size_t n);

#endif
