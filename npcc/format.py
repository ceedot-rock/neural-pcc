"""Frozen .npcc bitstream layout. Version 1."""
from __future__ import annotations
import struct
import zlib
from enum import IntEnum

MAGIC = b"NPCC"
VERSION = 1


class PATHWAY(IntEnum):
    RAW = 0
    TRU8 = 1
    ZECK = 2
    LZCM = 3
    NEURAL_AR = 4
    NEURAL_PROG = 5


PRIORITY = {
    PATHWAY.NEURAL_PROG: 0,
    PATHWAY.NEURAL_AR: 1,
    PATHWAY.TRU8: 2,
    PATHWAY.ZECK: 3,
    PATHWAY.LZCM: 4,
    PATHWAY.RAW: 255,
}


class FLAG:
    HAS_MODEL = 1 << 0
    HAS_ADAPTER = 1 << 1
    HAS_RESIDUAL = 1 << 2


def uleb128(n: int) -> bytes:
    if n < 0:
        raise ValueError("uleb128 unsigned")
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_uleb128(buf: bytes, i: int) -> tuple[int, int]:
    shift = 0
    n = 0
    while True:
        if i >= len(buf):
            raise ValueError("truncated uleb128")
        b = buf[i]
        i += 1
        n |= (b & 0x7F) << shift
        if not (b & 0x80):
            return n, i
        shift += 7
        if shift > 63:
            raise ValueError("uleb128 too long")


def pack(
    pathway_id: int,
    original_len: int,
    payload: bytes,
    *,
    flags: int = 0,
    model_hash: bytes = b"",
    adapter: bytes = b"",
) -> bytes:
    if model_hash:
        flags |= FLAG.HAS_MODEL
    if adapter:
        flags |= FLAG.HAS_ADAPTER
    if len(model_hash) > 255:
        raise ValueError("model hash too long")
    head = bytearray()
    head += MAGIC
    head += struct.pack("<H", VERSION)
    head += struct.pack("<B", pathway_id)
    head += struct.pack("<H", flags)
    head += uleb128(original_len)
    head += uleb128(len(payload))
    head += struct.pack("<B", len(model_hash))
    head += model_hash
    head += uleb128(len(adapter))
    head += adapter
    crc = zlib.crc32(bytes(head) + payload) & 0xFFFFFFFF
    return bytes(head) + payload + struct.pack("<I", crc)


def unpack(blob: bytes) -> dict:
    if len(blob) < 11:
        raise ValueError("truncated npcc")
    if blob[:4] != MAGIC:
        raise ValueError("bad magic")
    version = struct.unpack_from("<H", blob, 4)[0]
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    pathway_id = blob[6]
    flags = struct.unpack_from("<H", blob, 7)[0]
    i = 9
    original_len, i = read_uleb128(blob, i)
    payload_len, i = read_uleb128(blob, i)
    hlen = blob[i]
    i += 1
    model_hash = blob[i : i + hlen]
    i += hlen
    alen, i = read_uleb128(blob, i)
    adapter = blob[i : i + alen]
    i += alen
    payload = blob[i : i + payload_len]
    i += payload_len
    if i + 4 != len(blob):
        raise ValueError("length mismatch")
    crc_got = struct.unpack_from("<I", blob, i)[0]
    header = blob[: i - payload_len]
    crc_need = zlib.crc32(header + payload) & 0xFFFFFFFF
    if crc_got != crc_need:
        raise ValueError("crc mismatch")
    return {
        "version": version,
        "pathway_id": pathway_id,
        "flags": flags,
        "original_len": original_len,
        "payload": payload,
        "model_hash": model_hash,
        "adapter": adapter,
    }
