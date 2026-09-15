# riser-v2 training (private)

Lab corpus (zeros, runs, fox, xml-ish, logs, json, ramps), 314,260 bytes.
SGD 4000 steps x batch 32, lr 0.03.
CE loss 4.86 → 3.07 (uniform is 5.55).

Quantized to int16 tables in `src/riser_weights.h`.
W1 is small after Q map (feat was /255 in float). B2 carries ASCII prior (space, digits, letters).

This is not Silesia/PCC training. Adaptive mix still does most of the coding.
Bind tables in `model_init` with memcpy from RISER_W*.
