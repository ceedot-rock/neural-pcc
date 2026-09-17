#!/bin/bash
# bench_one.sh — single-file SickNode workbench benchmark worker (SPEC v2, rev P1).
# Usage: bench_one.sh FILE BIN OUTDIR FULL
#   FULL: 0 = conductor (two-family exact minimum: whole-file raw+top-3 vs
#          routed 256KB per-block, framing overhead included)
#         1 = NPCC_WB_FULL=1 full battery (audit: per-candidate bytes)
#
# Parsing contract (verified against src/main.c + src/workbench.c):
#   * wbc stdout machine line:
#       file=IN raw=N packed=M winner=W seats=a>b>c full=0|1 routed=0|1 enc=Ss
#     (only stdout line starting with "file="; winner name in {raw} U
#     transform names U {routed}; seats = conductor seat order)
#   * frame header byte 0 of OUT (ground truth for outer mode):
#       24            -> routed
#       13..23        -> outer transform (13 columnar ... 23 shuffle)
#       otherwise     -> RAW win: the file is a bare inner frame, and byte 0
#                        is the INNER mode id (0 lz, 1 bwt, 3 xz, 5 col,
#                        6 tr, 7 lzm2, 8 blk). Inner ids are all < 13, so this
#                        never collides with the outer range (and the decoder
#                        wb_decode_frame uses exactly this rule).
#     For transform wins (13..23) the inner blob starts at 5+meta_len
#     (meta_len = u32 LE at bytes 1..4); its first byte is the inner mode.
#   * per-candidate exact bytes come from the escalation handoff export
#     (NPCC_WB_HANDOFF): "tried mode=M param=P bytes=N" lines (bytes=X when
#     the candidate failed/declined). Nothing in stdout/stderr prints per-
#     candidate totals; the handoff is the only source. Runner-up margin is
#     computed from the sorted tried-byte list (loser - winner).
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
handoff="$OUT/$f$suffix.handoff"

if [ ! -f "$src" ]; then
  echo "$f -1 -1 ?:-1 -1 FAIL ? rc=-1 drc=-1" > "$res"
  exit 0
fi
raw=$(stat -c%s "$src")
sha0=$(sha256sum "$src" | cut -d' ' -f1)

# Per-file timeout, scaled with size: 900 + (bytes/1e6)*400 s, minimum 3600.
to=$(awk -v b="$raw" 'BEGIN{ t=900+b/1e6*400; if(t<3600) t=3600; printf "%d", t }')

t0=$(date +%s%N)
if [ "$FULL" = "1" ]; then
  NPCC_WB_FULL=1 NPCC_VERBOSE=1 NPCC_WB_HANDOFF="$handoff" nice -n 10 timeout "$to" "$BIN" wbc "$src" "$out" >"$log" 2>&1
else
  NPCC_VERBOSE=1 NPCC_WB_HANDOFF="$handoff" nice -n 10 timeout "$to" "$BIN" wbc "$src" "$out" >"$log" 2>&1
fi
rc=$?
t1=$(date +%s%N)
secs=$(awk "BEGIN{printf \"%.1f\", ($t1-$t0)/1e9}")

if [ $rc -ne 0 ]; then
  echo "$f $raw -1 ?:-1 $secs FAIL ? rc=$rc drc=-1 to=$to" > "$res"
  echo "INCIDENT encode rc=$rc (timeout ${to}s or crash)" >>"$log"
  exit 0
fi
if [ ! -f "$out" ]; then
  echo "$f $raw -1 ?:-1 $secs FAIL ? rc=$rc drc=-1 to=$to" > "$res"
  echo "INCIDENT output file missing after rc=0" >>"$log"
  exit 0
fi
bytes=$(stat -c%s "$out")

# --- wbc machine line (stdout; the only '^file=' line) ---
wbcline=$(grep -aE '^file=' "$log" | tail -1)
w_packed=$(printf '%s\n' "$wbcline" | grep -oE 'packed=[0-9]+' | head -1 | cut -d= -f2)
w_winner=$(printf '%s\n' "$wbcline" | grep -oE 'winner=[A-Za-z0-9_]+' | head -1 | cut -d= -f2)
w_seats=$(printf '%s\n' "$wbcline" | grep -oE 'seats=[A-Za-z0-9_>]+' | head -1 | cut -d= -f2)
w_full=$(printf '%s\n' "$wbcline" | grep -oE 'full=[01]' | head -1 | cut -d= -f2)
w_routed=$(printf '%s\n' "$wbcline" | grep -oE 'routed=[01]' | head -1 | cut -d= -f2)
w_enc=$(printf '%s\n' "$wbcline" | grep -oE 'enc=[0-9.]+s' | head -1 | cut -d= -f2 | tr -d 's')
if [ -z "$wbcline" ]; then
  echo "INCIDENT no wbc machine line in stdout" >>"$log"
fi
if [ -n "$w_packed" ] && [ "$w_packed" != "$bytes" ]; then
  echo "INCIDENT wbc packed=$w_packed != output file size $bytes" >>"$log"
fi

# --- winner outer mode id from the frame header byte (ground truth) ---
modenum=$(od -An -tu1 -N1 "$out" | tr -d ' ')
case "$modenum" in
  24) modename="routed";;
  13) modename="columnar";; 14) modename="delta8";; 15) modename="delta16";;
  16) modename="delta24";; 17) modename="delta32";; 18) modename="xor16";;
  19) modename="xor32";; 20) modename="exe";; 21) modename="img2d";;
  22) modename="bitplane";; 23) modename="shuffle";;
  *)  modename="raw";;   # bare inner frame: byte0 = inner mode id (0..8)
esac

# --- inner mode of the winning whole-file path (ground truth from frame) ---
case "$modenum" in
  0) inner="lz";; 1) inner="bwt";; 3) inner="xz";; 5) inner="col";;
  6) inner="tr";; 7) inner="lzm2";; 8) inner="blk";;
  24) inner="per-block";;
  *)
    # transform frame 13..23: inner blob starts at 5+meta_len (u32 LE @1..4)
    mlen=$(od -An -tu4 -j1 -N4 --endian=little "$out" 2>/dev/null | tr -d ' ')
    if [ -n "$mlen" ] && [ "$mlen" -ge 8 ] 2>/dev/null; then
      ib=$(od -An -tu1 -j$((5 + mlen)) -N1 "$out" 2>/dev/null | tr -d ' ')
      case "$ib" in
        0) inner="lz";; 1) inner="bwt";; 3) inner="xz";; 5) inner="col";;
        6) inner="tr";; 7) inner="lzm2";; 8) inner="blk";; *) inner="ib$ib";;
      esac
    else
      inner="?"; echo "INCIDENT meta_len=$mlen unparseable for mode $modenum" >>"$log"
    fi
    ;;
esac

# --- cross-checks: stdout winner vs header; handoff incumbent vs file size ---
if [ -n "$w_winner" ] && [ "${w_winner,,}" != "${modename,,}" ]; then
  echo "INCIDENT stdout winner='$w_winner' disagrees with header mode $modenum:$modename" >>"$log"
fi

# --- runner-up margin from the handoff tried table ---
margin="?"
if [ -f "$handoff" ]; then
  inc_line=$(grep -aE '^incumbent ' "$handoff" | head -1)
  inc_bytes=$(printf '%s\n' "$inc_line" | grep -oE 'bytes=[0-9]+' | cut -d= -f2)
  inc_mode=$(printf '%s\n' "$inc_line" | grep -oE 'mode=[0-9]+' | cut -d= -f2)
  if [ -n "$inc_bytes" ] && [ "$inc_bytes" != "$bytes" ]; then
    echo "INCIDENT handoff incumbent_bytes=$inc_bytes != output file size $bytes" >>"$log"
  fi
  case "$inc_mode" in
    12) exp_name="raw";; 24) exp_name="routed";;
    13) exp_name="columnar";; 14) exp_name="delta8";; 15) exp_name="delta16";;
    16) exp_name="delta24";; 17) exp_name="delta32";; 18) exp_name="xor16";;
    19) exp_name="xor32";; 20) exp_name="exe";; 21) exp_name="img2d";;
    22) exp_name="bitplane";; 23) exp_name="shuffle";; *) exp_name="?";;
  esac
  if [ -n "$w_winner" ] && [ "${exp_name,,}" != "${w_winner,,}" ] && [ "$exp_name" != "?" ]; then
    echo "INCIDENT handoff incumbent mode=$inc_mode ($exp_name) disagrees with wbc winner='$w_winner'" >>"$log"
  fi
  # margin = second-smallest tried bytes - smallest tried bytes (ties -> 0)
  tb=$(grep -aE '^tried ' "$handoff" | grep -oE 'bytes=[0-9]+' | cut -d= -f2 | sort -n)
  n1=$(printf '%s\n' "$tb" | sed -n 1p)
  n2=$(printf '%s\n' "$tb" | sed -n 2p)
  if [ -n "$n1" ] && [ -n "$n2" ]; then
    margin=$((n2 - n1))
  elif [ -n "$n1" ]; then
    margin=0
  fi
  # also log whole-family best vs routed total for color
  wb_best=$(grep -aE '^tried ' "$handoff" | grep -avE 'mode=24 ' | grep -oE 'bytes=[0-9]+' | cut -d= -f2 | sort -n | head -1)
  rt_b=$(grep -aE '^tried mode=24 ' "$handoff" | grep -oE 'bytes=[0-9]+' | cut -d= -f2 | head -1)
  echo "families whole_best=${wb_best:-?} routed=${rt_b:-?}" >>"$log"
else
  echo "INCIDENT handoff file missing: $handoff" >>"$log"
fi

# Decode + SHA-256 verify — this is what makes the byte count reportable.
# wbd usage: wbd ENCODED ORIG_BYTES OUTPUT (orig is a decimal byte count,
# not a path — passing the path fails with "wbd: bad orig", rc=2).
timeout 900 "$BIN" wbd "$out" "$raw" "$dec" >>"$log" 2>&1
drc=$?
shaok="no"
if [ $drc -eq 0 ] && [ -f "$dec" ]; then
  sha1=$(sha256sum "$dec" | cut -d' ' -f1)
  [ "$sha0" = "$sha1" ] && shaok="yes"
else
  echo "INCIDENT decode rc=$drc" >>"$log"
fi

echo "$f $raw $bytes $modenum:$modename $secs $shaok $margin rc=$rc drc=$drc inner=$inner w=$w_winner seats=$w_seats full=$w_full routedflag=$w_routed enc=$w_enc to=$to" > "$res"
exit 0
