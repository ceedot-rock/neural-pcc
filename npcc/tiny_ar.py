"""Deterministic tiny neural AR. Model id tiny-ar-v1."""
from __future__ import annotations
import math
from . import rans

CTX, HID = 8, 32
F = CTX + 1
MODEL_ID = b"tiny-ar-v1"
MIX_NET = 1
MIX_ADP = 7


def _lcg(seed: int):
    x = seed & 0xFFFFFFFF
    while True:
        x = (1664525 * x + 1013904223) & 0xFFFFFFFF
        yield x


def _weights():
    g = _lcg(0x4E415231)
    W1 = [((next(g) / 0xFFFFFFFF) * 2 - 1) * 0.3 for _ in range(F * HID)]
    b1 = [((next(g) / 0xFFFFFFFF) * 2 - 1) * 0.1 for _ in range(HID)]
    W2 = [((next(g) / 0xFFFFFFFF) * 2 - 1) * 0.1 for _ in range(HID * 256)]
    b2 = [((next(g) / 0xFFFFFFFF) * 2 - 1) * 0.05 for _ in range(256)]
    return W1, b1, W2, b2


W1, B1, W2, B2 = _weights()


def logits(ctx: bytes) -> list[float]:
    buf = (bytes(CTX - len(ctx)) + ctx) if len(ctx) < CTX else ctx[-CTX:]
    feat = [buf[i] / 255.0 for i in range(CTX)] + [1.0]
    hid = []
    for h in range(HID):
        s = B1[h]
        row = h * F
        for f in range(F):
            s += feat[f] * W1[row + f]
        hid.append(s if s > 0.0 else 0.0)
    out = []
    for o in range(256):
        s = B2[o]
        row = o * HID
        for h in range(HID):
            s += hid[h] * W2[row + h]
        out.append(s)
    return out


def net_freqs(ctx: bytes) -> list[int]:
    lg = logits(ctx)
    m = max(lg)
    exps, acc = [], 0.0
    for v in lg:
        x = max(-20.0, min(20.0, v - m))
        e = math.exp(x)
        exps.append(e)
        acc += e
    counts = [max(1, int(e / acc * rans.FREQ_SUM)) for e in exps]
    return rans.normalize_freqs(counts)


def mixed_freqs(ctx: bytes, adaptive_counts: list[int]) -> list[int]:
    nf = net_freqs(ctx)
    af = rans.normalize_freqs(adaptive_counts)
    mixed = [MIX_NET * nf[i] + MIX_ADP * af[i] for i in range(256)]
    return rans.normalize_freqs(mixed)
