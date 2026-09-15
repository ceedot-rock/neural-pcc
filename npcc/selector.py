"""PCC-style selector. Never expand vs |x|. Ties by PRIORITY."""
from __future__ import annotations
import hashlib
from dataclasses import dataclass
from .format import PATHWAY, PRIORITY, pack
from . import pathways as P


@dataclass
class Budget:
    allow_neural: bool = True
    allow_search: bool = True
    max_prog_len: int = 256


def compress(x: bytes, budget: Budget | None = None) -> bytes:
    budget = budget or Budget()
    candidates: list[tuple[int, int, bytes]] = []

    def consider(pid: PATHWAY, payload: bytes | None, model_hash: bytes = b""):
        if payload is None:
            return
        blob = pack(int(pid), len(x), payload, model_hash=model_hash)
        if len(blob) < len(x):
            candidates.append((len(blob), PRIORITY[pid], blob))

    consider(PATHWAY.TRU8, P.tru8_compress(x))
    consider(PATHWAY.ZECK, P.zeck_compress(x))
    consider(PATHWAY.LZCM, P.lzcm_compress(x))
    if budget.allow_neural:
        consider(
            PATHWAY.NEURAL_AR,
            P.neural_ar_compress(x),
            model_hash=hashlib.sha256(P.MODEL_AR_ID).digest(),
        )
        if budget.allow_search:
            consider(PATHWAY.NEURAL_PROG, P.neural_prog_search(x, budget.max_prog_len))
    if not candidates:
        return pack(int(PATHWAY.RAW), len(x), P.raw_payload(x))
    candidates.sort(key=lambda t: (t[0], t[1]))
    return candidates[0][2]
