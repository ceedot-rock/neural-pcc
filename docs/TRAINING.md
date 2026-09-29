# riser training — status: not in this tree

The shipped C riser (`src/riser.c`, `model_init`) generates its weights from a
portable LCG at startup (seed `0x52495331`). This is the **untrained
riser-v1**. There is no trained riser-v2 in this repo:

- `src/riser_weights.h` was never committed.
- Nothing includes the dead `src/riser_bind.inc` (it `#include`s the missing
  header).
- `train/train_riser.py` is a stub that prints a pointer to a session
  artifact; no training code ships here.

What the lab-note riser-v2 was (not reproducible from this tree): lab corpus
(zeros, runs, fox, xml-ish, logs, json, ramps), 314,260 bytes. SGD 4000 steps
x batch 32, lr 0.03. CE loss 4.86 → 3.07 (uniform is 5.55). The plan was
int16-quantized tables bound in `model_init` via memcpy from `RISER_W*` —
not implemented here. This is not Silesia/PCC training; the adaptive mix
still does most of the coding.
