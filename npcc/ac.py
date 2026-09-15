"""Bit-level arithmetic coder with pending bits."""
from __future__ import annotations
from . import rans


def encode_with(x: bytes, freqs_at) -> bytes:
    low, high, pending = 0, 0xFFFFFFFF, 0
    bits: list[int] = []

    def out_bit(b: int):
        nonlocal pending
        bits.append(b & 1)
        while pending:
            bits.append((b & 1) ^ 1)
            pending -= 1

    counts = [1] * 256
    ctx = bytearray()
    for i, sym in enumerate(x):
        freqs = freqs_at(i, bytes(ctx), counts)
        cdf = rans.cdf_from_freqs(freqs)
        total = rans.FREQ_SUM
        span = high - low + 1
        high = low + (span * cdf[sym + 1]) // total - 1
        low = low + (span * cdf[sym]) // total
        while True:
            if high < 0x80000000:
                out_bit(0)
            elif low >= 0x80000000:
                out_bit(1)
                low -= 0x80000000
                high -= 0x80000000
            elif low >= 0x40000000 and high < 0xC0000000:
                pending += 1
                low -= 0x40000000
                high -= 0x40000000
            else:
                break
            low = (low << 1) & 0xFFFFFFFF
            high = ((high << 1) | 1) & 0xFFFFFFFF
        counts[sym] += 1
        ctx.append(sym)
        if len(ctx) > 8:
            del ctx[0]
    pending += 1
    out_bit(1 if low >= 0x40000000 else 0)
    out, acc, k = bytearray(), 0, 0
    for b in bits:
        acc = (acc << 1) | b
        k += 1
        if k == 8:
            out.append(acc)
            acc, k = 0, 0
    if k:
        out.append(acc << (8 - k))
    return bytes(out)


def decode_with(p: bytes, n: int, freqs_at) -> bytes:
    bits = [(byte >> i) & 1 for byte in p for i in range(7, -1, -1)]
    bits.extend([1] * 64)
    bi = 0

    def getbit():
        nonlocal bi
        b = bits[bi]
        bi += 1
        return b

    value = 0
    for _ in range(32):
        value = (value << 1) | getbit()
    low, high = 0, 0xFFFFFFFF
    counts = [1] * 256
    ctx = bytearray()
    out = bytearray()
    for i in range(n):
        freqs = freqs_at(i, bytes(ctx), counts)
        cdf = rans.cdf_from_freqs(freqs)
        total = rans.FREQ_SUM
        span = high - low + 1
        scaled = ((value - low + 1) * total - 1) // span
        lo_s, hi_s = 0, 256
        while lo_s + 1 < hi_s:
            mid = (lo_s + hi_s) // 2
            if cdf[mid] <= scaled:
                lo_s = mid
            else:
                hi_s = mid
        s = lo_s
        high = low + (span * cdf[s + 1]) // total - 1
        low = low + (span * cdf[s]) // total
        while True:
            if high < 0x80000000:
                pass
            elif low >= 0x80000000:
                low -= 0x80000000
                high -= 0x80000000
                value -= 0x80000000
            elif low >= 0x40000000 and high < 0xC0000000:
                low -= 0x40000000
                high -= 0x40000000
                value -= 0x40000000
            else:
                break
            low = (low << 1) & 0xFFFFFFFF
            high = ((high << 1) | 1) & 0xFFFFFFFF
            value = ((value << 1) | getbit()) & 0xFFFFFFFF
        out.append(s)
        counts[s] += 1
        ctx.append(s)
        if len(ctx) > 8:
            del ctx[0]
    return bytes(out)


def encode(x: bytes) -> bytes:
    return encode_with(x, lambda i, ctx, c: rans.normalize_freqs(c))


def decode(p: bytes, n: int) -> bytes:
    return decode_with(p, n, lambda i, ctx, c: rans.normalize_freqs(c))
