# Neural-PCC (private)

Proprietary. Slid Phi Labs. Not for public repos, papers, or open licenses.

Multi-pathway lossless compressor. PCC outer contract: bit-exact, never expand when a pathway is shorter than `|x|`, shortest candidate wins, ties by pathway priority.

```
PYTHONPATH=. python3 tests/test_npcc.py
PYTHONPATH=. python3 -m npcc c IN.bin OUT.npcc
PYTHONPATH=. python3 -m npcc d OUT.npcc OUT.bin
PYTHONPATH=. python3 -m npcc serve 127.0.0.1 8080
```

POST `/api/compress` and `/api/decompress`. Query `?budget=classical|ar|full`.
