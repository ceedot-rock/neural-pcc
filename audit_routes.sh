#!/bin/bash
# audit_routes.sh — Dimension 2: hot-loop routing predictions vs sampled
# per-block exact winners (SPEC v2+v3, rev P1).
# Usage: audit_routes.sh OUTDIR
# For each file: read $f.routes, select up to 6 blocks (0, 25%, 50%, 75%,
# last, plus one "interesting" block where the prediction differs from its
# neighbors or a stage-2 tiebreak fired), extract via dd, run the FULL
# battery (NPCC_WB_FULL=1) on the slice. On a single 256KB slice the tried
# table's exact minimum IS the per-block true winner. Compare vs the route
# dump's predicted mode; report HIT/MISS and the byte cost of misses
# (predicted-mode tried bytes - true-min bytes, from the slice handoff).
set -u
OUT="${1:-$HOME/workspace/tnssrc-workbench/bench-out}"
BIN="$HOME/workspace/tnssrc-workbench/bin/npcc"
CORPUS="$HOME/workspace/helix-match-exp/corpus/full"
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
RAD="$OUT/route-audit"
mkdir -p "$RAD"

mode_name() {
  case "$1" in
    12) echo raw;; 13) echo columnar;; 14) echo delta8;; 15) echo delta16;;
    16) echo delta24;; 17) echo delta32;; 18) echo xor16;; 19) echo xor32;;
    20) echo exe;; 21) echo img2d;; 22) echo bitplane;; 23) echo shuffle;;
    24) echo routed;; *) echo "?$1";;
  esac
}
mode_id() {
  case "$1" in
    raw) echo 12;; columnar) echo 13;; delta8) echo 14;; delta16) echo 15;;
    delta24) echo 16;; delta32) echo 17;; xor16) echo 18;; xor32) echo 19;;
    exe) echo 20;; img2d) echo 21;; bitplane) echo 22;; shuffle) echo 23;;
    *) echo -1;;
  esac
}

total_hit=0; total_n=0; total_cost=0
for f in $FILES; do
  rf="$OUT/$f.routes"
  [ -f "$rf" ] || { echo "$f: no routes file, skip"; continue; }
  nblocks=$(grep -a "^# tag=" "$rf" | grep -oE "nblocks=[0-9]+" | cut -d= -f2)
  [ -n "$nblocks" ] || { echo "$f: no nblocks, skip"; continue; }
  # block selection: 0, quartiles, last, plus one "interesting" block
  # (first block whose predicted mode differs from block 0, or tiebreak=1)
  q1=$((nblocks/4)); q2=$((nblocks/2)); q3=$((3*nblocks/4)); last=$((nblocks-1))
  interesting=""
  m0=$(awk 'NR>3{print $2; exit}' "$rf")
  interesting=$(awk -v m0="$m0" 'NR>3 && ($2!=m0 || $5~/^1/) {print $1; exit}' "$rf")
  sel="$((0)) $q1 $q2 $q3 $last $interesting"
  # dedupe, drop empties/out-of-range
  blocks=$(for b in $sel; do [ -n "$b" ] && [ "$b" -ge 0 ] 2>/dev/null && [ "$b" -lt "$nblocks" ] 2>/dev/null && echo "$b"; done | sort -nu | tr '\n' ' ')
  echo "== $f: nblocks=$nblocks sampling blocks: $blocks"
  for b in $blocks; do
    pred=$(awk -v b="$b" 'NR>3 && $1==b {print $2}' "$rf")
    slice="$RAD/$f.b$b.slice"
    dd if="$CORPUS/$f" of="$slice" bs=262144 skip="$b" count=1 status=none 2>/dev/null
    [ -f "$slice" ] || { echo "  block $b: slice extract failed"; continue; }
    hh="$RAD/$f.b$b.handoff"
    lg="$RAD/$f.b$b.log"
    NPCC_WB_FULL=1 NPCC_WB_HANDOFF="$hh" nice -n 10 timeout 1200 "$BIN" wbc "$slice" "$slice.wb" >"$lg" 2>&1
    rc=$?
    if [ $rc -ne 0 ] || [ ! -f "$hh" ]; then
      echo "  block $b: FULL wbc rc=$rc, skip"; continue
    fi
    # true winner: min tried bytes excluding routed (not built on 1 block anyway)
    tw=$(grep -aE '^tried ' "$hh" | grep -avE 'mode=24 ' | grep -aE 'bytes=[0-9]+' | \
      awk '{m="";x=""; for(i=1;i<=NF;i++){ if($i~/^mode=/){m=substr($i,6)}; if($i~/^bytes=/){x=substr($i,7)} } if(m!=""&&x!="") print x, m}' | sort -n | head -1)
    tb=$(echo "$tw" | cut -d' ' -f1); tm=$(echo "$tw" | cut -d' ' -f2)
    tname=$(mode_name "$tm")
    # predicted mode's exact bytes from the slice tried table
    pid=$(mode_id "$pred")
    pb=$(grep -aE "^tried mode=$pid " "$hh" | grep -oE 'bytes=[0-9]+' | cut -d= -f2 | head -1)
    [ -z "$pb" ] && pb="X"
    flag="HIT"; cost=0
    if [ "${pred,,}" != "${tname,,}" ]; then
      flag="MISS"
      if [ "$pb" != "X" ] && [ -n "$tb" ]; then cost=$((pb - tb)); total_cost=$((total_cost + cost)); fi
    else
      total_hit=$((total_hit+1))
    fi
    total_n=$((total_n+1))
    printf '  block %-4s pred=%-9s true=%-9s(%sB) pred_bytes=%s %s cost=%s\n' "$b" "$pred" "$tname" "$tb" "$pb" "$flag" "$cost"
  done
done
echo
echo "routing top-1 agreement: $total_hit/$total_n ; total miss cost: ${total_cost}B"
