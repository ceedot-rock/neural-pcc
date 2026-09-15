# AIP — Auto Intelligent Protocol (private lab)

Proprietary. Slid Phi Labs. Neural coding riser, not a public SKU.

## Verbs

- `quik.build` — rise raw bytes through a real fixed-point MLP + adaptive mix, wrap AIP1.
- `neat.destruct` — replay the same \(p_\theta\), emit original bytes, drop the frame.

Never expand vs `|x|` after the frame is formed: if the neural payload + header is not strictly shorter, the frame stores RAW (`kind=0`) so destruct is still exact.

## Frame

```
magic     4  AIP1
version   1  = 1
kind      1  0=raw identity  1=neural-ac riser-v1
orig_len  4  little-endian u32
pay_len   4  little-endian u32
model     8  first 8 bytes of sha256("riser-v1") if kind=1, else zeros
payload   pay_len
crc32     4  IEEE of everything before crc
```

Decoder refuses kind=1 if the model stamp does not match.

## Model

`riser-v1`: CTX=8 bytes → 16 ReLU hidden → 256 logits (Q12 weights from a portable LCG).
Mixed 1:7 with Laplace-adaptive byte counts. Arithmetic coder is decoder-mirrored.
This is a lab neural occupant, not a 7B transformer and not TNSSRC-v1 CM.
