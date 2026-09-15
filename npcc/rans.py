"""Freq tables summing to FREQ_SUM. Used by AC and optional ANS."""
from __future__ import annotations

FREQ_BITS = 12
FREQ_SUM = 1 << FREQ_BITS
RANS_L = 1 << 16


def normalize_freqs(counts: list[int]) -> list[int]:
    n = len(counts)
    total = sum(counts)
    if total == 0:
        base = FREQ_SUM // n
        freqs = [base] * n
        freqs[-1] = FREQ_SUM - base * (n - 1)
        return freqs
    present = [i for i, c in enumerate(counts) if c > 0]
    freqs = [0] * n
    acc = 0
    for i in present:
        f = max(1, (counts[i] * FREQ_SUM) // total)
        freqs[i] = f
        acc += f
    if acc > FREQ_SUM:
        while acc > FREQ_SUM:
            j = max(present, key=lambda k: freqs[k])
            if freqs[j] <= 1:
                break
            freqs[j] -= 1
            acc -= 1
    elif acc < FREQ_SUM:
        j = max(present, key=lambda k: counts[k])
        freqs[j] += FREQ_SUM - acc
    return freqs


def cdf_from_freqs(freqs: list[int]) -> list[int]:
    cdf = [0]
    s = 0
    for f in freqs:
        s += f
        cdf.append(s)
    if s != FREQ_SUM:
        raise ValueError("freqs must sum to FREQ_SUM")
    return cdf
