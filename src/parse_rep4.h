#ifndef PARSE_REP4_H
#define PARSE_REP4_H

#include <stddef.h>
#include <stdint.h>

/* tok_len==0 => literal in tok_dist low 8 bits. Else match. */
int parse_rep4(const uint8_t *data, size_t n, uint32_t window, uint32_t *tok_dist,
               uint32_t *tok_len, size_t *ntok);

#endif
