#!/usr/bin/env bash
# extract_roster_atlases.sh — ONE-COMMAND batch: extract+pack the COMPLETE offline
# emitter atlas for every extracted character. Pixels come FREE from the disc (GFX1
# LZSS decode loc_8c0354c0, bit-exact) — NO emulator capture, NO build host, NO play.
#
# Per char it produces (ROM-derived, gitignored — scp-only, NEVER git):
#   PLxx_parts.png  PLxx_parts.json  PLxx_asm.json  PLxx_idx.png  PLxx_lut.json  PLxx_preview.png
#
# Usage:
#   tools/extract_roster_atlases.sh                 # all PLxx under dasm_PLDAT/Output
#   tools/extract_roster_atlases.sh PL00 PL17 PL2D  # just these
#   DAT_ROOT=... OUT=... tools/extract_roster_atlases.sh
#
# Then deploy (ROM-derived -> scp only):
#   scp web/test-atlas/chars/PL*_{parts.png,parts.json,asm.json,idx.png,lut.json} \
#       ubuntu@15.204.141.58:/var/www/maplecast/test-atlas/chars/   # rise3 (sudo needed to write)
#
# The client picks each char up lazily: loadAsmChar(char_id) fetches PL{HEX}_parts/_asm
# by char_id (cache-busted), buildEmitterDrawList renders every sprite_id (all poses).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAT_ROOT="${DAT_ROOT:-$HERE/dasm_PLDAT/Output}"
OUT="${OUT:-$HERE/web/test-atlas/chars}"
PY="${PY:-python3}"
mkdir -p "$OUT"

# Most chars use palette body-bank 0. Override per-char here if a char's body row differs.
declare -A BANK
# BANK[PL17]=0   # example override

chars=("$@")
if [ ${#chars[@]} -eq 0 ]; then
  chars=()
  for d in "$DAT_ROOT"/PL*_DAT; do
    [ -d "$d" ] || continue
    b="$(basename "$d")"; chars+=("${b%_DAT}")
  done
fi

echo "roster: ${chars[*]}"
for c in "${chars[@]}"; do
  G1="$DAT_ROOT/${c}_DAT/${c}_DAT_GFX_DATA_00.BIN"
  G2="$DAT_ROOT/${c}_DAT/${c}_DAT_GFX_DATA_01.BIN"
  PAL="$DAT_ROOT/${c}_DAT/${c}_DAT_PALETTE_DATA.BIN"
  if [ ! -f "$G1" ] || [ ! -f "$PAL" ]; then
    echo "  [$c] SKIP (missing GFX/PAL — extract the DAT with dasm_PLDAT first)"; continue
  fi
  bank="${BANK[$c]:-0}"
  echo "  [$c] extracting (bank=$bank)…"
  "$PY" "$HERE/tools/extract_gfx1_atlas.py" \
     --gfx1 "$G1" ${G2:+--gfx2 "$G2"} --pal "$PAL" \
     --char "$c" --bank "$bank" --out "$OUT"
done
echo "done -> $OUT  (scp to prod test-atlas/chars/ — ROM-derived, never git)"
