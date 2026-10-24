#!/usr/bin/env bash
# Renders font candidates (weights / faces) with the LVGL host so they can be compared with crops of
# reference/shots (see README.md). Output: tools/fontgen/specimen_out/specimen.png + row offsets.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/host/.zig-cache/global" ZIG_LOCAL_CACHE_DIR="$ROOT/host/.zig-cache/local"
OUT=tools/fontgen/specimen_out
FG=tools/fontgen/bin/fontgen.exe
mkdir -p "$OUT"
rm -f "$OUT"/*.c "$OUT"/specimen_cands.h
bash tools/fontgen/build.sh --tool-only
bash host/build_lvgl.sh
LATO=tools/fonts/Lato-Regular.ttf
MM=tools/fonts/Montserrat-Medium.ttf
MB=tools/fonts/Montserrat-Bold.ttf
DV=tools/fonts/DejaVuSans.ttf
STM=tools/fonts/ShareTechMono-Regular.ttf
PANEL=0xE8EAEB
INK=0x252629
# cand SYM TTF SIZE WEIGHT TEXT LS BG FG [extra fontgen args]
cand() {
    local sym=$1 ttf=$2 size=$3 w=$4 text=$5 ls=$6 bg=$7 fg=$8
    shift 8
    "$FG" --ttf "$ttf" --fallback "$DV" --size "$size" --weight "$w" --name "$sym" --out "$OUT/$sym.c" "$@" 2>/dev/null
    printf 'X(%s, "%s", %s, %s, %s)\n' "$sym" "$text" "$ls" "$bg" "$fg" >> "$OUT/specimen_cands.h"
}
# Rows named *_pick are the weights used by build.sh.
T1="Kick 01  Zażółć"
cand a_lato0        $LATO 21 0     "$T1" 0 $PANEL $INK
cand a_lato12_pick  $LATO 21 0.012 "$T1" 0 $PANEL $INK
cand a_lato20       $LATO 21 0.020 "$T1" 0 $PANEL $INK
cand a_mont_medium  $MM   21 0     "$T1" 0 $PANEL $INK
T2="ATTACK   DECAY   SUSTAIN"
cand b_lato12       $LATO 10 0.012 "$T2" 1 $PANEL 0x343434
cand b_lato22_pick  $LATO 10 0.022 "$T2" 1 $PANEL 0x343434
cand b_lato30       $LATO 10 0.030 "$T2" 1 $PANEL 0x343434
cand b_mont_medium  $MM   10 0     "$T2" 1 $PANEL 0x343434
cand b_mont_bold    $MB   10 0     "$T2" 1 $PANEL 0x343434
T3="PADS   BROWSER"
cand c_lato22_pick  $LATO 12 0.022 "$T3" 1 $PANEL $INK
cand c_lato30       $LATO 12 0.030 "$T3" 1 $PANEL $INK
cand c_mont_medium  $MM   12 0     "$T3" 1 $PANEL $INK
T4="All Samples  Drums  Kick  50 ms"
cand d_lato0        $LATO 11 0     "$T4" 0 $PANEL $INK
cand d_lato4_pick   $LATO 11 0.004 "$T4" 0 $PANEL $INK
cand d_lato8        $LATO 11 0.008 "$T4" 0 $PANEL $INK
cand d_mont_medium  $MM   11 0     "$T4" 0 $PANEL $INK
T5="TAB5"
cand e_lato30       $LATO 30 0.030 "$T5" -2 0xEBECED $INK
cand e_lato45_pick  $LATO 30 0.045 "$T5" -2 0xEBECED $INK
cand e_lato60       $LATO 30 0.060 "$T5" -2 0xEBECED $INK
cand e_mont_bold    $MB   30 0     "$T5" -2 0xEBECED $INK
T6="120 BPM"
cand f_lato4        $LATO 14 0.004 "$T6" 0 0xEBECED $INK
cand f_lato12_pick  $LATO 14 0.012 "$T6" 0 0xEBECED $INK
cand f_mont_medium  $MM   14 0     "$T6" 0 0xEBECED $INK
T7="0.000s — 0.420s   44 100 Hz"
cand g_stm0         $STM  11 0     "$T7" 0 0x1D2022 0xB0B1B2
cand g_stm25_pick   $STM  11 0.025 "$T7" 0 0x1D2022 0xB0B1B2 --track -0.05
cand g_lato_tnum    $LATO 11 0     "$T7" 0 0x1D2022 0xB0B1B2 --tnum
cp tools/fontgen/specimen.cpp "$OUT/"
for c in "$OUT"/[a-g]_*.c; do
    zig cc -target x86_64-windows-gnu -std=c99 -O1 -w -Ihost -Ifirmware/managed_components/lvgl__lvgl \
        -DLV_CONF_INCLUDE_SIMPLE -c "$c" -o "${c%.c}.o"
done
zig c++ -target x86_64-windows-gnu -std=c++17 -O1 -w -Ihost -Ifirmware/managed_components/lvgl__lvgl -I"$OUT" \
    -DLV_CONF_INCLUDE_SIMPLE "$OUT/specimen.cpp" host/lvgl_host.cpp "$OUT"/[a-g]_*.o \
    host/lib/liblvgl.a -o "$OUT/specimen.exe"
"$OUT/specimen.exe" "$OUT/specimen.png" > "$OUT/rows.txt"
cat "$OUT/rows.txt"
# Reference crop (top) + candidate rows, zoomed: compare_<group>.png (rows.txt: x y h name).
R=reference/shots/view-sample.png
P="$OUT/specimen.png"
rows() { awk -v g="$1" -v w="$2" -v z="$3" -v p="$P" '$4 ~ "^"g"_" {printf "%s:%d,%d,%d,%d@%d ", p, $1, $2, w, $3, z}' "$OUT/rows.txt"; }
PT="python -I tools/fontgen/pngtool.py"
$PT "$OUT/compare_a_name21.png"    $R:324,80,200,34@3   $(rows a 200 3)
$PT "$OUT/compare_b_knob10.png"    $R:548,394,230,20@3  $(rows b 230 3)
$PT "$OUT/compare_c_title12.png"   $R:24,80,130,22@3    $(rows c 130 3)
$PT "$OUT/compare_d_body11.png"    $R:60,454,180,20@3   $(rows d 180 3)
$PT "$OUT/compare_e_logo30.png"    $R:16,6,110,48@2     $(rows e 110 2)
$PT "$OUT/compare_f_bpm14.png"     $R:1068,14,90,24@2   $(rows f 90 2)
$PT "$OUT/compare_g_mono11.png"    $R:850,126,190,20@3  $(rows g 190 3)
rm -f "$OUT"/*.c "$OUT"/*.o "$OUT"/specimen.cpp "$OUT"/specimen.exe "$OUT"/*.pdb "$OUT"/specimen_cands.h
echo "specimens: $OUT/compare_*.png"
