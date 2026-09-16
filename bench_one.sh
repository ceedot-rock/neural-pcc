#!/bin/bash
# bench_one.sh — single-file SickNode workbench benchmark worker (SPEC v2).
# Usage: bench_one.sh FILE BIN OUTDIR FULL
#   FULL: 0 = conductor (two-family exact minimum: whole-file raw+top-3 vs
#          routed 256KB per-block, framing overhead included)
#         1 = NPCC_WB_FULL=1 full battery (audit: per-candidate bytes)
#
# The winning outer mode id is verified from the frame header itself:
#   outer frame = [mode u8][meta_len u32 LE][meta][inner blob]
# Byte 0 of the output file IS the mode id — hard ground truth, independent
# of stdout. Cross-checked against the binary's reported winner.
set -u
f="$1"; BIN="$2"; OUT="$3"; FULL="$4"
CORPUS="$HOME/workspace/helix-match-exp/corpus/full"
mkdir -p "$OUT"
src="$CORPUS/$f"

suffix=""
if [ "$FULL" = "1" ]; then suffix=".full"; fi
out="$OUT/$f$suffix.wb"
log="$OUT/$f$suffix.log"
res="$OUT/$f$suffix.result"
dec="$OUT/$f$suffix.out"

if [ ! -f "$src" ]; then
  echo "$f -1 -1 ?:-1 -1 FAIL ? rc=-1 drc=-1" > "$res"
  exit 0
fi
raw=$(stat -c%s "$src")
sha0=$(sha256sum "$src" | cut -d' ' -f1)

t0=$(date +%s%N)
if [ "$FULL" = "1" ]; then
  NPCC_WB_FULL=1 NPCC_VERBOSE=1 nice -n 10 timeout 3600 "$BIN" wbc "$src" "$out" >"$log" 2>&1
else
  NPCC_VERBOSE=1 nice -n 10 timeout 3600 "$BIN" wbc "$src" "$out" >"$log" 2>&1
fi
rc=$?
t1=$(date +%s%N)
secs=$(awk "BEGIN{printf \"%.1f\", ($t1-$t0)/1e9}")

if [ $rc -ne 0 ]; then
  echo "$f $raw -1 ?:-1 $secs FAIL ? rc=$rc drc=-1" > "$res"
  echo "INCIDENT encode rc=$rc (timeout 3600s or crash)" >>"$log"
  exit 0
fi
if [ ! -f "$out" ]; then
  echo "$f $raw -1 ?:-1 $secs FAIL ? rc=$rc drc=-1" > "$res"
  echo "INCIDENT output file missing after rc=0" >>"$log"
  exit 0
fi
bytes=$(stat -c%s "$out")

# Winner mode id from the frame header byte (ground truth).
modenum=$(od -An -tu1 -N1 "$out" | tr -d ' ')
case "$modenum" in
  12) modename="raw";; 13) modename="columnar";; 14) modename="delta8";;
  15) modename="delta16";; 16) modename="delta24";; 17) modename="delta32";;
  18) modename="xor16";; 19) modename="xor32";; 20) modename="exe";;
  21) modename="img2d";; 22) modename="bitplane";; 23) modename="shuffle";;
  24) modename="routed";;   # SPEC v2 WB_MODE_ROUTED — verify when it lands
  *)  modename="UNKNOWN";;
esac
if [ "$modename" = "UNKNOWN" ]; then
  echo "INCIDENT header mode byte=$modenum not in known table (12-24)" >>"$log"
fi

# Cross-check: what did stdout claim the winner was?
stdout_mode=$(grep -oEi '(winner|winning[^:]{0,24}|outer_?mode|best_?(candidate|transform|mode))[^0-9a-z_.-]*[0-9a-z_.-]+' "$log" \
        | tail -1 | grep -oE '[0-9a-zA-Z_.-]+$' || true)
if [ -n "$stdout_mode" ] && [ "${stdout_mode,,}" != "${modename,,}" ] && [ "${stdout_mode,,}" != "${modenum}" ]; then
  echo "INCIDENT stdout winner='$stdout_mode' disagrees with header mode $modenum:$modename" >>"$log"
fi

# Runner-up margin: conductor compares whole-file family vs routed family.
# Parse both totals from the log; margin = loser - winner (bytes).
margin="?"
whole_b=$(grep -oEi 'whole[^0-9]{0,12}[0-9][0-9,]*' "$log" | tail -1 | grep -oE '[0-9][0-9,]*' | tr -d ',' || true)
routed_b=$(grep -oEi 'rout(ed|ing)?[^0-9]{0,12}[0-9][0-9,]*' "$log" | tail -1 | grep -oE '[0-9][0-9,]*' | tr -d ',' || true)
if [ -n "$whole_b" ] && [ -n "$routed_b" ]; then
  if [ "$whole_b" -ge "$routed_b" ]; then margin=$((whole_b - routed_b)); else margin=$((routed_b - whole_b)); fi
fi

# Decode + SHA-256 verify — this is what makes the byte count reportable.
timeout 600 "$BIN" wbd "$out" "$src" "$dec" >>"$log" 2>&1
drc=$?
shaok="no"
if [ $drc -eq 0 ] && [ -f "$dec" ]; then
  sha1=$(sha256sum "$dec" | cut -d' ' -f1)
  [ "$sha0" = "$sha1" ] && shaok="yes"
else
  echo "INCIDENT decode rc=$drc" >>"$log"
fi

echo "$f $raw $bytes $modenum:$modename $secs $shaok $margin rc=$rc drc=$drc" > "$res"
exit 0
