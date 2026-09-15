#include "riser_weights.h"
#include <string.h>
static void riser_load_trained(int16_t *W1, int16_t *B1, int16_t *W2, int16_t *B2) {
    memcpy(W1, RISER_W1, sizeof(RISER_W1));
    memcpy(B1, RISER_B1, sizeof(RISER_B1));
    memcpy(W2, RISER_W2, sizeof(RISER_W2));
    memcpy(B2, RISER_B2, sizeof(RISER_B2));
}
