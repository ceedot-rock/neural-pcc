from __future__ import annotations
import hashlib
from .format import PATHWAY, unpack
from .selector import Budget, compress as select_compress
from . import pathways as P


def compress(x: bytes, budget: Budget | None = None) -> bytes:
    return select_compress(x, budget)


def decompress(blob: bytes) -> bytes:
    h = unpack(blob)
    pid = h["pathway_id"]
    p = h["payload"]
    n = h["original_len"]
    if pid == PATHWAY.RAW:
        y = p
    elif pid == PATHWAY.TRU8:
        y = P.tru8_decompress(p)
    elif pid == PATHWAY.ZECK:
        y = P.zeck_decompress(p, n)
    elif pid == PATHWAY.LZCM:
        y = P.lzcm_decompress(p, n)
    elif pid == PATHWAY.NEURAL_AR:
        need = hashlib.sha256(P.MODEL_AR_ID).digest()
        if h["model_hash"] and h["model_hash"] != need:
            raise ValueError("model hash mismatch")
        y = P.neural_ar_decompress(p, n)
    elif pid == PATHWAY.NEURAL_PROG:
        y = P.neural_prog_decompress(p, n)
    else:
        raise ValueError(f"unknown pathway {pid}")
    if len(y) != n:
        raise ValueError("original_len mismatch")
    return y
