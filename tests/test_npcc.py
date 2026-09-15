import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from npcc import PATHWAY, compress, decompress
from npcc.format import unpack


def check(x: bytes) -> bytes:
    c = compress(x)
    y = decompress(c)
    assert y == x, (len(x), len(y), unpack(c)["pathway_id"])
    return c


def test_cases():
    samples = [
        b"",
        b"\x00",
        b"\x00" * 100,
        b"\x00" * 2000,
        b"A" * 500,
        b"ab" * 400,
        b"The quick brown fox jumps over the lazy dog. " * 20,
        bytes(range(256)),
        bytes(range(256)) * 4,
        b"\xff\x00" * 300,
    ]
    for x in samples:
        c = check(x)
        info = unpack(c)
        print(f"{len(x):6d} -> {len(c):6d}  path={PATHWAY(info['pathway_id']).name}")


if __name__ == "__main__":
    test_cases()
    print("ok")
