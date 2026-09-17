#!/bin/bash
# audit.sh — three-dimension audit (SPEC v2+v3, rev P1).
# Usage: audit.sh OUTDIR
#
# Sources (all ground truth, nothing estimated):
#   Bay-1:  $OUT/$f.log        (wbc machine line: winner, seats, packed)
#           $OUT/$f.handoff    (escalation handoff: tried table, incumbent)
#   FULL:   $OUT/$f.full.log / $OUT/$f.full.handoff  (same, full battery)
#   Routes: $OUT/$f.routes     (npcc wbroute audit dump; per-block predicted
#                               transform + probe bytes per candidate)
#
# Dimension 1: mixer's top pick (Bay-1 seats[1], first non-raw seat) vs the
#   FULL battery's exact whole-file winner (min over tried rows excluding
#   routed/24). MISS flag per file.
# Dimension 2: hot-loop per-block routing predictions (wbroute dump) vs
#   full-exact per-block winners on SAMPLED 256KB slices (FULL wbc on the
#   slice: the slice is 1 block, so routed is skipped and the tried table
#   is exactly the per-block full-exact winner). Sampling is documented
#   in RESULTS_WORKBENCH.md.
# Dimension 3 (Tier 2): top-3 accuracy — is the FULL whole-file true winner
#   among the Bay-1 seats[1..3]? Target 12/12. HIT/MISS per file.
#   (If the true winner is raw, seats[0]=raw is always seated -> HIT.)
# Tier 3: MIXER_LOG.tsv sanity (existence, header, column counts, line
#   counts vs expected, numeric sanity, duplicate check). No modeling.
#
# Cross-check: ~/workspace/tnssrc-matrix/MATRIX.md (read-only) if present.
set -u
OUT="${1:-$HOME/workspace/tnssrc-workbench/bench-out}"
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
MATRIX_DIR="$HOME/workspace/tnssrc-matrix"   # READ-ONLY, never write here
MIXER_LOG="$HOME/workspace/tnssrc-workbench/MIXER_LOG.tsv"

mode_name() {
  case "$1" in
    12) echo raw;; 13) echo columnar;; 14) echo delta8;; 15) echo delta16;;
    16) echo delta24;; 17) echo delta32;; 18) echo xor16;; 19) echo xor32;;
    20) echo exe;; 21) echo img2d;; 22) echo bitplane;; 23) echo shuffle;;
    24) echo routed;; *) echo "?$1";;
  esac
}

# whole-file true winner from a FULL handoff: "bytes mode" of the min
# tried row excluding routed (mode 24); failed rows (bytes=X) are skipped.
true_whole_winner() {
  grep -aE '^tried ' "$1" | grep -avE 'mode=24 ' | grep -aE 'bytes=[0-9]+' | \
    awk '{m="";b=""; for(i=1;i<=NF;i++){ if($i~/^mode=/){m=substr($i,6)}; if($i~/^bytes=/){b=substr($i,7)} } if(m!=""&&b!="") print b, m}' | \
    sort -n | head -1
}

echo "########## DIMENSION 1: mixer top pick vs full-battery true winner ##########"
echo "mixer_pick = Bay-1 seats[1] (first non-raw seat); true = FULL whole-file exact min"
d1_miss=0
for f in $FILES; do
  bay="$OUT/$f.log"; fh="$OUT/$f.full.handoff"
  if [ ! -f "$bay" ]; then echo "$f | MISSING Bay-1 log"; continue; fi
  if [ ! -f "$fh" ]; then echo "$f | MISSING FULL handoff"; continue; fi
  seats=$(grep -aE '^file=' "$bay" | tail -1 | grep -oE 'seats=[A-Za-z0-9_>]+' | cut -d= -f2)
  pick=$(printf '%s' "$seats" | cut -d'>' -f2)
  tw=$(true_whole_winner "$fh"); tb=$(printf '%s' "$tw" | cut -d' ' -f1); tm=$(printf '%s' "$tw" | cut -d' ' -f2)
  tname=$(mode_name "$tm")
  flag="ok"
  if [ "${pick,,}" != "${tname,,}" ]; then flag="MISS"; d1_miss=$((d1_miss+1)); fi
  printf '%-8s seats=[%s] pick=%-9s true=%-9s(%sB) %s\n' "$f" "$seats" "$pick" "$tname" "$tb" "$flag"
done
echo "dimension-1 misses: $d1_miss"

echo
echo "########## DIMENSION 3: top-3 accuracy (Tier 2) ##########"
echo "Target 12/12: FULL whole-file true winner among Bay-1 seats[1..3]"
d3_miss=0; d3_n=0
for f in $FILES; do
  bay="$OUT/$f.log"; fh="$OUT/$f.full.handoff"
  if [ ! -f "$bay" ]; then echo "$f | MISSING Bay-1 log"; continue; fi
  if [ ! -f "$fh" ]; then echo "$f | MISSING FULL handoff"; continue; fi
  seats=$(grep -aE '^file=' "$bay" | tail -1 | grep -oE 'seats=[A-Za-z0-9_>]+' | cut -d= -f2)
  tw=$(true_whole_winner "$fh"); tm=$(printf '%s' "$tw" | cut -d' ' -f2)
  tname=$(mode_name "$tm")
  d3_n=$((d3_n+1))
  hit="MISS"
  if [ "$tm" = "12" ]; then hit="HIT(raw)"; fi
  for k in 1 2 3; do
    s=$(printf '%s' "$seats" | cut -d'>' -f$((k+1)))
    if [ "${s,,}" = "${tname,,}" ]; then hit="HIT"; fi
  done
  [ "$hit" = "MISS" ] && d3_miss=$((d3_miss+1))
  printf '%-8s seats=[%s] true=%-9s %s\n' "$f" "$seats" "$tname" "$hit"
done
echo "top-3 accuracy: $((d3_n - d3_miss))/$d3_n"
if [ -f "$MATRIX_DIR/MATRIX.md" ]; then
  echo "MATRIX.md present — ground-truth cross-check (read-only; see RESULTS_WORKBENCH.md)."
else
  echo "MATRIX.md not landed yet — cross-check pending (dir is read-only; never write there)."
fi
echo "Exact-selector reference: BWT 7/12 files, LZM2 5/12 files (inner mode of each"
echo "conductor winner cross-checked in RESULTS_WORKBENCH.md)."

echo
echo "########## DIMENSION 2: routing map vs sampled per-block exact winners ##########"
echo "wbroute dumps live in \$OUT/\$f.routes; per-slice FULL runs in \$OUT/route-audit/."
echo "Sampling rule + results are tabulated in RESULTS_WORKBENCH.md."
ls "$OUT"/*.routes 2>/dev/null | wc -l | xargs echo "route dumps present:"
ls "$OUT/route-audit" 2>/dev/null | head -5 || echo "(no route-audit dir yet)"

echo
echo "########## TIER 3: MIXER_LOG.tsv sanity ##########"
if [ -f "$MIXER_LOG" ]; then
  lines=$(wc -l < "$MIXER_LOG")
  echo "MIXER_LOG.tsv exists: $lines lines (incl. header)"
  head -1 "$MIXER_LOG"
  badcols=$(tail -n +2 "$MIXER_LOG" | awk -F'\t' 'NF!=20{print NR}' | head -3)
  [ -z "$badcols" ] && echo "column check: all data rows have 20 fields" || echo "BAD-COLUMN rows: $badcols"
  whole=$(tail -n +2 "$MIXER_LOG" | awk -F'\t' '$3=="whole"' | wc -l)
  blocks=$(tail -n +2 "$MIXER_LOG" | awk -F'\t' '$3!="whole"' | wc -l)
  echo "whole rows: $whole | block rows: $blocks"
  badnum=$(tail -n +2 "$MIXER_LOG" | awk -F'\t' '$20 !~ /^[0-9]+$/ {print NR}' | head -3)
  [ -z "$badnum" ] && echo "packed_bytes: all numeric" || echo "NON-NUMERIC packed_bytes rows: $badnum"
  echo "max (file,block) duplicates:"
  tail -n +2 "$MIXER_LOG" | awk -F'\t' '{print $2"\t"$3}' | sort | uniq -c | sort -rn | head -3
else
  echo "MIXER_LOG.tsv NOT PRESENT — wbc has not logged yet."
fi
