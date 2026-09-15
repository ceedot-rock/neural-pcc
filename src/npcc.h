#ifndef NPCC_H
#define NPCC_H

#include <stddef.h>
#include <stdint.h>

#define NPCC_OK 0
#define NPCC_ERR_NOMEM 1
#define NPCC_ERR_FORMAT 2
#define NPCC_ERR_CRC 3
#define NPCC_ERR_PATHWAY 4
#define NPCC_ERR_MODEL 5
#define NPCC_ERR_LEN 6
#define NPCC_ERR_IO 7
#define NPCC_ERR_PROG 8

enum {
    NPCC_RAW = 0,
    NPCC_TRU8 = 1,
    NPCC_ZECK = 2,
    NPCC_LZCM = 3,
    NPCC_NEURAL_AR = 4,
    NPCC_NEURAL_PROG = 5
};

typedef struct {
    int allow_neural;
    int allow_search;
    int max_prog_len;
} NpccBudget;

void npcc_budget_full(NpccBudget *b);
void npcc_budget_ar(NpccBudget *b);
void npcc_budget_classical(NpccBudget *b);

int npcc_compress(const uint8_t *in, size_t n, const NpccBudget *b,
                  uint8_t **out, size_t *out_n);
int npcc_decompress(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n);
int npcc_peek(const uint8_t *in, size_t n, int *pathway, uint64_t *orig_len);
const char *npcc_pathway_name(int id);
const char *npcc_strerror(int rc);

#endif
