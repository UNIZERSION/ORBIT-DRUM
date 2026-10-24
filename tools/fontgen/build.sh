#!/usr/bin/env bash
# Builds tools/fontgen and regenerates every UI font into firmware/main/ui/fonts/orbit_*.c.
#   bash tools/fontgen/build.sh              # tool + fonts
#   bash tools/fontgen/build.sh --tool-only  # just the generator
# Swapping the face (e.g. Inter later): set TEXT_TTF (and drop the synthetic weights by setting
# W_MEDIUM/W_SEMI/W_BOLD to 0 and pointing MEDIUM_TTF/SEMI_TTF/BOLD_TTF at the real weights).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/host/.zig-cache/global" ZIG_LOCAL_CACHE_DIR="$ROOT/host/.zig-cache/local"
BIN=tools/fontgen/bin
FG=$BIN/fontgen.exe
mkdir -p "$BIN"
if [[ ! -f "$FG" || tools/fontgen/fontgen.cpp -nt "$FG" ]]; then
    zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -w \
        -Ifirmware/managed_components/lvgl__lvgl/src/libs/tiny_ttf \
        tools/fontgen/fontgen.cpp -o "$FG"
    echo "built $FG"
fi
[[ "${1:-}" == "--tool-only" ]] && exit 0

# ---- faces (see README.md for why these were chosen)
TEXT_TTF=${TEXT_TTF:-tools/fonts/Lato-Regular.ttf}
MEDIUM_TTF=${MEDIUM_TTF:-$TEXT_TTF}
SEMI_TTF=${SEMI_TTF:-$TEXT_TTF}
BOLD_TTF=${BOLD_TTF:-$TEXT_TTF}
MONO_TTF=${MONO_TTF:-tools/fonts/ShareTechMono-Regular.ttf}
FALLBACK_TTF=${FALLBACK_TTF:-tools/fonts/DejaVuSans.ttf}   # symbols + rare Latin Ext-A letters
# Synthetic emboldening per side in em (0 = use the face as is).
W_REGULAR=${W_REGULAR:-0.004}   # Lato is a touch lighter than Segoe UI at 11-14 px
W_MEDIUM=${W_MEDIUM:-0.012}
W_SEMI=${W_SEMI:-0.022}
W_BOLD=${W_BOLD:-0.045}
W_MONO=${W_MONO:-0.025}       # Share Tech Mono is lighter than Consolas

OUT=firmware/main/ui/fonts
mkdir -p "$OUT"
gen() {   # gen NAME TTF SIZE WEIGHT [extra]
    local name=$1 ttf=$2 size=$3 w=$4
    shift 4
    "$FG" --ttf "$ttf" --fallback "$FALLBACK_TTF" --size "$size" --weight "$w" --kern --tnum \
        --name "orbit_font_$name" --out "$OUT/orbit_font_$name.c" "$@"
}
gen r12 "$TEXT_TTF" 12 "$W_REGULAR"
gen r13 "$TEXT_TTF" 13 "$W_REGULAR"
gen r14 "$TEXT_TTF" 14 "$W_REGULAR"
gen m12 "$MEDIUM_TTF" 12 "$W_MEDIUM"
gen m13 "$MEDIUM_TTF" 13 "$W_MEDIUM"
gen m14 "$MEDIUM_TTF" 14 "$W_MEDIUM"
gen m16 "$MEDIUM_TTF" 16 "$W_MEDIUM"
gen m20 "$MEDIUM_TTF" 20 "$W_MEDIUM"
gen m24 "$MEDIUM_TTF" 24 "$W_MEDIUM"
gen s11 "$SEMI_TTF" 11 "$W_SEMI"
gen s13 "$SEMI_TTF" 13 "$W_SEMI"
gen s15 "$SEMI_TTF" 15 "$W_SEMI"
gen b34 "$BOLD_TTF" 34 "$W_BOLD"
gen mono12 "$MONO_TTF" 12 "$W_MONO" --track -0.05   # keep the face's 0.54 em pitch
echo "fonts written to $OUT"
