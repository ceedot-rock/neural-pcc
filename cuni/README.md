# Neural-PCC CuNi laws

Source of truth also lives in `cuni/examples/compressors/npcc-*.cuni`.

Each file encodes and decodes. `cuni check` emit+runs the catalog; stdout must match.

```
cuni check . --timeout 180
cuni bank paste npcc-tru8.cuni --from cuni --to c
```

LZ / AC / tiny-AR are the C engine (`../src/codec.c`), not laws.
