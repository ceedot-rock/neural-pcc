# Neural-PCC — TNSSRC (private)

Proprietary. Slid Phi Labs. Not for public repos, papers, or open licenses.

**Compressor 2.** PCC is 1. This is TriNeural Shared Spine Row Compression.

C engine. Pathway **laws** are CuNi (`cuni/`): same stdout on every catalog seat, or refuse.

One spine per 8-bit row. Three heads (LOCAL, MATCH, ROW). Online updates, decoder mirrors the encoder. Never expand vs `|x|`.

```
make
./bin/npcc c IN.bin OUT.npcc
./bin/npcc d OUT.npcc OUT.bin
./bin/npcc bench IN.bin full|ar|classical
./bin/npcc serve 127.0.0.1 8080
make test
cuni check cuni --timeout 180
```

`full` / `ar` run TNSSRC. `classical` is TRU8/ZECK/LZCM only.

Silesia snapshots (DECODE_OK, C engine, not official 12-file):

| file | raw | TNSSRC | gzip-9 | PCC pcc-0.12.1 |
|---|---:|---:|---:|---:|
| xml | 5,345,280 | **594,574** | 662,284 | 443,165 |
| dickens | 10,192,446 | **3,786,706** | 3,851,823 | 2,738,073 |

TNSSRC now **beats gzip-9** on both. PCC/champ still ahead. MATCH emits copies (len,dist) + FastCM on literals.
