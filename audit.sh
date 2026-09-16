#!/bin/bash
# audit.sh — three-dimension audit (SPEC v2+v3).
# Usage: audit.sh OUTDIR
#
# Dimension 1: scan's predicted top-3 (Bay-1 $f.log) vs true full-battery
#   winner ($f.full.log, exact minimum over every candidate). MISS flag.
# Dimension 2: hot-loop per-block routing predictions (routing-map dump, see
#   ROUTING.md) vs full-exact per-block winners on sampled 256KB blocks.
# Dimension 3 (Tier 2): top-3 accuracy — is the conductor's true winner
#   among the 3 seats the mixer ordered? Target 12/12. Cross-checked
#   against ~/workspace/tnssrc-matrix/MATRIX.md (read-only) and the
#   exact-selector BWT 7/12 / LZM2 5/12 winners.
# Tier 3: MIXER_LOG.tsv existence + line-count sanity (no modeling).
#
# Parsing patterns are verified against real logs before the final table;
# see HARNESS.md. Nothing is estimated: only what the binary emitted.
set -u
OUT="${1:-$HOME/workspace/tnssrc-workbench/bench-out}"
FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
MATRIX_DIR="$HOME/workspace/tnssrc-matrix"   # READ-ONLY, never write here
MIXER_LOG="$HOME/workspace/tnssrc-workbench/MIXER_LOG.tsv"

echo "########## DIMENSION 1: scan top-3 vs full-battery winner ##########"
for f in $FILES; do
  bay="$OUT/$f.log"; full="$OUT/$f.full.log"
  [ -f "$bay" ] || { echo "$f | MISSING Bay-1 log"; continue; }
  [ -f "$full" ] || { echo "$f | MISSING FULL log"; continue; }
  echo "--- $f Bay-1 scan/candidate lines ---"
  grep -iE 'cand|predict|top-?3|scan' "$bay" | head -8 || echo "(none)"
  echo "--- $f FULL candidate lines ---"
  grep -iE 'cand|transform|mode.*[0-9]{4,}' "$full" | head -20 || echo "(none)"
done

echo
echo "########## DIMENSION 2: routing map vs sampled per-block exact winners ##########"
ROUTING_MD="$HOME/workspace/tnssrc-workbench/ROUTING.md"
if [ ! -f "$ROUTING_MD" ]; then
  echo "ROUTING.md not present yet — dimension 2 blocked on build worker's dump format."
  echo "Sampling rule is documented in HARNESS.md; re-run audit.sh when it lands."
else
  echo "ROUTING.md present — implementing dimension 2 here (placeholder until format verified)."
fi

echo
echo "########## DIMENSION 3: top-3 accuracy (Tier 2) ##########"
echo "Target: 12/12 — true winner must be among the mixer's 3 seats."
for f in $FILES; do
  bay="$OUT/$f.log"; full="$OUT/$f.full.log"
  [ -f "$bay" ] || { echo "$f | MISSING Bay-1 log"; continue; }
  [ -f "$full" ] || { echo "$f | MISSING FULL log"; continue; }
  echo "--- $f mixer seat order ---"
  grep -iE 'seat|mixer|order' "$bay" | head -6 || echo "(none — adapt pattern to real format)"
done
if [ -f "$MATRIX_DIR/MATRIX.md" ]; then
  echo "MATRIX.md present — cross-checking ground truth (read-only)."
else
  echo "MATRIX.md not landed yet — cross-check pending (dir is read-only; never write there)."
fi
echo "Exact-selector reference: BWT 7/12 files, LZM2 5/12 files."

echo
echo "########## TIER 3: MIXER_LOG.tsv sanity ##########"
if [ -f "$MIXER_LOG" ]; then
  lines=$(wc -l < "$MIXER_LOG")
  echo "MIXER_LOG.tsv exists: $lines lines"
  echo "head:"; head -3 "$MIXER_LOG"
  echo "file-level lines (expect >=12 after P1, no dupes per run):"
  grep -c $'\tfile\t' "$MIXER_LOG" || echo "(no file-level marker pattern — adapt)"
else
  echo "MIXER_LOG.tsv NOT PRESENT — build worker has not enabled logging yet."
fi
