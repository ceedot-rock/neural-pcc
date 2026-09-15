"""Independent compressors. Payload or None."""
from __future__ import annotations
import struct
from . import ac, rans, tiny_ar

MODEL_AR_ID = tiny_ar.MODEL_ID


def raw_payload(x: bytes) -> bytes:
    return x


def tru8_compress(x: bytes) -> bytes | None:
    if not x:
        return None
    b0 = x[0]
    if all(b == b0 for b in x):
        return b"\x01" + bytes([b0]) + struct.pack("<Q", len(x))
    return None


def tru8_decompress(p: bytes) -> bytes:
    if len(p) != 10 or p[0] != 1:
        raise ValueError("bad tru8")
    return bytes([p[1]]) * struct.unpack_from("<Q", p, 2)[0]


def _fibs_upto(n: int) -> list[int]:
    f = [1, 2]
    while f[-1] < n:
        f.append(f[-1] + f[-2])
    return f


def zeck_compress(x: bytes) -> bytes | None:
    if not x:
        return None
    n = int.from_bytes(x, "big")
    if n == 0:
        return b"\x00"
    fibs = _fibs_upto(n)
    bits = []
    rest = n
    for f in reversed(fibs):
        if f <= rest:
            bits.append(1)
            rest -= f
        else:
            bits.append(0)
    if rest != 0:
        return None
    bitcount = len(bits)
    out = bytearray(struct.pack("<I", bitcount))
    acc = k = 0
    for b in bits:
        acc = (acc << 1) | b
        k += 1
        if k == 8:
            out.append(acc)
            acc = k = 0
    if k:
        out.append(acc << (8 - k))
    return bytes(out)


def zeck_decompress(p: bytes, original_len: int) -> bytes:
    if not p:
        raise ValueError("empty zeck")
    if p == b"\x00":
        return b"\x00" * original_len
    bitcount = struct.unpack_from("<I", p, 0)[0]
    bits = []
    for byte in p[4:]:
        for i in range(7, -1, -1):
            bits.append((byte >> i) & 1)
            if len(bits) == bitcount:
                break
    fibs = [1, 2]
    while len(fibs) < bitcount:
        fibs.append(fibs[-1] + fibs[-2])
    n = 0
    for bit, f in zip(bits, reversed(fibs[:bitcount])):
        if bit:
            n += f
    raw = n.to_bytes((n.bit_length() + 7) // 8 or 1, "big")
    if len(raw) < original_len:
        raw = b"\x00" * (original_len - len(raw)) + raw
    if len(raw) != original_len:
        raise ValueError("zeck length")
    return raw


def _lzcm_tags(x: bytes) -> bytes:
    w, min_m, max_m = 4096, 3, 255
    out = bytearray()
    i = 0
    table: dict[bytes, list[int]] = {}
    while i < len(x):
        best = best_off = 0
        if i + min_m <= len(x):
            key = x[i : i + 3]
            for pos in reversed(table.get(key, [])[-64:]):
                if i - pos > w or pos >= i:
                    continue
                L = 0
                lim = min(max_m, len(x) - i, i - pos)
                while L < lim and x[pos + L] == x[i + L]:
                    L += 1
                if L > best:
                    best, best_off = L, i - pos
        if best >= min_m:
            out.append(1)
            out.append(best)
            out += struct.pack("<H", best_off)
            end = i + best
            while i < end:
                if i + 3 <= len(x):
                    table.setdefault(x[i : i + 3], []).append(i)
                i += 1
        else:
            out.append(0)
            out.append(x[i])
            if i + 3 <= len(x):
                table.setdefault(x[i : i + 3], []).append(i)
            i += 1
    return bytes(out)


def _lzcm_untags(p: bytes, original_len: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(p):
        kind = p[i]
        i += 1
        if kind == 0:
            out.append(p[i])
            i += 1
        elif kind == 1:
            length = p[i]
            off = struct.unpack_from("<H", p, i + 1)[0]
            i += 3
            if off == 0 or off > len(out):
                raise ValueError("bad lz offset")
            for _ in range(length):
                out.append(out[-off])
        else:
            raise ValueError("bad lz tag")
    if original_len and len(out) != original_len:
        raise ValueError("lz length")
    return bytes(out)


def lzcm_compress(x: bytes) -> bytes | None:
    if len(x) < 16:
        return None
    tags = _lzcm_tags(x)
    coded = ac.encode(tags)
    blob = len(tags).to_bytes(4, "little") + coded
    if len(blob) < len(tags):
        return b"\xA1" + blob
    return b"\xA0" + tags


def lzcm_decompress(p: bytes, original_len: int) -> bytes:
    if not p:
        raise ValueError("empty lzcm")
    if p[0] == 0xA0:
        tags = p[1:]
    elif p[0] == 0xA1:
        nsym = int.from_bytes(p[1:5], "little")
        tags = ac.decode(p[5:], nsym)
    else:
        tags = p
    return _lzcm_untags(tags, original_len)


def neural_ar_compress(x: bytes) -> bytes | None:
    if not x:
        return None
    return ac.encode_with(x, lambda i, ctx, c: tiny_ar.mixed_freqs(ctx, c))


def neural_ar_decompress(p: bytes, original_len: int) -> bytes:
    return ac.decode_with(p, original_len, lambda i, ctx, c: tiny_ar.mixed_freqs(ctx, c))


LANG_ID = b"\x01"


def prog_encode(program: bytes) -> bytes:
    return LANG_ID + program


def prog_run(program: bytes, limit: int = 1_000_000) -> bytes:
    out = bytearray()
    i = 0
    steps = 0
    while i < len(program):
        op = program[i]
        i += 1
        if op == 0:
            break
        if op == 1:
            out.append(program[i])
            i += 1
            steps += 1
        elif op == 2:
            b = program[i]
            cnt = struct.unpack_from("<H", program, i + 1)[0]
            i += 3
            if steps + cnt > limit:
                raise ValueError("time limit")
            out.extend(bytes([b]) * cnt)
            steps += cnt
        elif op == 3:
            off = struct.unpack_from("<H", program, i)[0]
            ln = program[i + 2]
            i += 3
            if off == 0 or off > len(out):
                raise ValueError("bad cpy")
            if steps + ln > limit:
                raise ValueError("time limit")
            for _ in range(ln):
                out.append(out[-off])
            steps += ln
        else:
            raise ValueError("bad op")
        if steps > limit:
            raise ValueError("time limit")
    return bytes(out)


def neural_prog_search(x: bytes, max_prog: int = 256) -> bytes | None:
    candidates: list[bytes] = []
    if x and all(b == x[0] for b in x) and len(x) <= 65535:
        candidates.append(bytes([2, x[0]]) + struct.pack("<H", len(x)) + bytes([0]))
    if len(x) >= 8:
        pat = x[:2]
        if pat * (len(x) // 2) + pat[: len(x) % 2] == x and len(x) <= 255:
            prog = bytes([1, pat[0], 1, pat[1], 3]) + struct.pack("<H", 2) + bytes([len(x) - 2, 0])
            candidates.append(prog)
    best = None
    for prog in candidates:
        if len(prog) > max_prog:
            continue
        try:
            y = prog_run(prog)
        except ValueError:
            continue
        if y == x:
            payload = prog_encode(prog)
            if best is None or len(payload) < len(best):
                best = payload
    return best


def neural_prog_decompress(p: bytes, original_len: int) -> bytes:
    if not p or p[0] != 1:
        raise ValueError("bad lang")
    y = prog_run(p[1:])
    if len(y) != original_len:
        raise ValueError("prog length")
    return y
