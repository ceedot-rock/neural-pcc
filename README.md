# TNSSRC (Neural-PCC)

**License:** AGPL-3.0-or-later **or** a written commercial grant ([LICENSE](LICENSE), [COMMERCIAL.md](COMMERCIAL.md)). Visibility is not a grant to ship TNSSRC in a closed product.

**Compressor 2.** PCC is compressor 1. TriNeural Shared Spine Row Compression. Own C. Not host xz/gzip/bzip2. Separate from Dial A / PCC daily.

## Install (CLI-first)

```
make
sudo make install          # PREFIX=/usr/local → /usr/local/bin/npcc
# or: make install PREFIX=$HOME/.local
npcc --help
npcc --version
```

Uninstall: `sudo make uninstall` (same `PREFIX`).

## Usage

```
make
./bin/npcc c IN.bin OUT.npcc
./bin/npcc d OUT.npcc OUT.bin
./bin/npcc bench IN.bin ar
make test
```

`ar` / `full` run TNSSRC. Pathway laws in `cuni/` (CuNi: same stdout on every catalog seat, or refuse). Never expand vs `|x|`.

## Claim lock

Lab Silesia 12: packed **48.54M (48,541,366)** / 211,938,580. Beats xz-9 (48,795,480). DECODE_OK 12/12. Beats PCC pcc-0.12.1 (51,498,645).

**Never claim:** #1, OSCB, or Fast MB/s. Speed is not this seat’s claim.

## Silesia 12

| file | raw | TNSSRC | PCC | xz-9 |
|---|---:|---:|---:|---:|
| dickens | 10,192,446 | 2,696,613 | 2,738,073 | |
| mozilla | 51,220,480 | 13,754,644 | 15,970,087 | |
| mr | 9,970,564 | 2,412,733 | 2,537,416 | |
| nci | 33,553,445 | 1,678,352 | 1,582,235 | |
| ooffice | 6,152,192 | 2,484,820 | 2,670,536 | |
| osdb | 10,085,684 | 2,611,929 | 2,761,563 | |
| reymont | 6,627,202 | 1,181,597 | 1,217,202 | |
| samba | 21,606,400 | 4,454,141 | 4,156,202 | |
| sao | 7,251,944 | 4,518,258 | 4,992,917 | |
| webster | 41,458,703 | 8,321,788 | 8,169,156 | |
| x-ray | 8,474,240 | 4,006,977 | 4,260,093 | |
| xml | 5,345,280 | 419,514 | 443,165 | |
| **total** | **211,938,580** | **48,541,366** | **51,498,645** | **48,795,480** |

See `bench/silesia12-48.54M.txt`.

Contact: corey@slidphilabs.com
