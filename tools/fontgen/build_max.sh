#!/usr/bin/env bash
# Larger UI fonts for maximized panels (0.3.2): the same faces and synthetic weights as build.sh, at the
# sizes of the ladder in firmware/main/ui/ui_scale.cpp (kLadder). A maximized panel is laid out natively at
# 1.3-2x, so its text must be rasterised at that size (scaling the 12-24 px fonts up as an image blurs it).
# Only writes the ladder fonts; the base fonts (build.sh) are left as they are.
#   bash tools/fontgen/build_max.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
FG=tools/fontgen/bin/fontgen.exe
[[ -f "$FG" ]] || bash tools/fontgen/build.sh --tool-only

TEXT_TTF=tools/fonts/Lato-Regular.ttf
MONO_TTF=tools/fonts/ShareTechMono-Regular.ttf
FALLBACK_TTF=tools/fonts/DejaVuSans.ttf
W_REGULAR=0.004
W_MEDIUM=0.012
W_SEMI=0.022
W_MONO=0.025

OUT=firmware/main/ui/fonts
gen() {   # gen NAME TTF SIZE WEIGHT [extra]
    local name=$1 ttf=$2 size=$3 w=$4
    shift 4
    "$FG" --ttf "$ttf" --fallback "$FALLBACK_TTF" --size "$size" --weight "$w" --kern --tnum \
        --name "orbit_font_$name" --out "$OUT/orbit_font_$name.c" "$@"
}
for s in 16 18 20 22 24 26 28; do gen "r$s" "$TEXT_TTF" "$s" "$W_REGULAR"; done
for s in 18 22 26 28 32 36 40 44 48; do gen "m$s" "$TEXT_TTF" "$s" "$W_MEDIUM"; done
for s in 16 18 20 22 24 26 28 30; do gen "s$s" "$TEXT_TTF" "$s" "$W_SEMI"; done
for s in 16 18 20 24; do gen "mono$s" "$MONO_TTF" "$s" "$W_MONO" --track -0.05; done
echo "ladder fonts written to $OUT"
