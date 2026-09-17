#!/bin/bash
# bench_wb.sh — SickNode workbench 12-file benchmark harness.
# Usage: bench_wb.sh [BIN] [OUTDIR] [FULL]
#   BIN    default $HOME/workspace/tnssrc-workbench/bin/npcc
#   OUTDIR default $HOME/workspace/tnssrc-workbench/bench-out
#   FULL   0 = Bay-1 (scan-predicted top-3)   1 = full battery (NPCC_WB_FULL=1)
#
# 2 vCPU box shared with the matrix benchmark: xargs -P2 max, nice -n 10.
# Per-file encode timeout 3600s. Results in OUTDIR/*.result, logs in *.log.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HOME/workspace/tnssrc-workbench/bin/npcc}"
OUT="${2:-$HOME/workspace/tnssrc-workbench/bench-out}"
FULL="${3:-0}"
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"

[ -x "$BIN" ] || { echo "no binary: $BIN" >&2; exit 1; }
mkdir -p "$OUT"
ts=$(date +%Y%m%d-%H%M%S)
echo "== bench_wb.sh FULL=$FULL start $ts bin=$BIN out=$OUT" | tee "$OUT/run-$ts.log"

# Resume: skip files that already have a non-FAIL result (Bay-1 and FULL=1
# runs share OUTDIR via the .full suffix, so resume keys off the suffix).
todo=""
for f in $FILES; do
  sfx=""; [ "$FULL" = "1" ] && sfx=".full"
  r="$OUT/$f$sfx.result"
  if [ -f "$r" ] && ! grep -qE 'FAIL|MISSING' "$r"; then
    echo "resume: $f already done ($(cat "$r" | cut -c1-80))" | tee -a "$OUT/run-$ts.log"
  else
    todo="$todo $f"
  fi
done

printf '%s\n' $todo | xargs -r -P2 -I{} bash "$HERE/bench_one.sh" {} "$BIN" "$OUT" "$FULL"

echo
echo "== summary =="
printf '%-8s %12s %12s %-16s %10s %6s %10s %s\n' file raw winner_bytes mode enc_s sha margin rc
for f in $FILES; do
  sfx=""; [ "$FULL" = "1" ] && sfx=".full"
  r="$OUT/$f$sfx.result"
  if [ -f "$r" ]; then
    # fields: f raw bytes mode secs shaok margin rc= drc=
    set -- $(cat "$r")
    printf '%-8s %12s %12s %-16s %10s %6s %10s %s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8 $9"
  else
    printf '%-8s %s\n' "$f" "MISSING_RESULT"
  fi
done | tee -a "$OUT/run-$ts.log"
echo "== done $(date +%Y%m%d-%H%M%S) ==" | tee -a "$OUT/run-$ts.log"
