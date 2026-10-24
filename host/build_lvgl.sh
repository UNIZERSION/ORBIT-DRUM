#!/usr/bin/env bash
# Host (PC) libraries for UI tests, compiled with zig cc and cached:
#   host/lib/liblvgl.a         LVGL 9.3 (firmware/managed_components/lvgl__lvgl) with host/lv_conf.h
#   host/lib/liborbit_assets.a generated UI fonts (firmware/main/ui/fonts/*.c) + icons (ui/icons/*.c)
# Each is rebuilt only when its inputs are newer (or with -f). Run from anywhere:
#   bash host/build_lvgl.sh [-f]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
LV=firmware/managed_components/lvgl__lvgl
OUT=host/lib
OBJ=$OUT/obj
LIB=$OUT/liblvgl.a
export ZIG_GLOBAL_CACHE_DIR="$ROOT/host/.zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$ROOT/host/.zig-cache/local"
TARGET=${ZIG_TARGET:-x86_64-windows-gnu}
FORCE=0
[[ "${1:-}" == "-f" ]] && FORCE=1
NPROC=$(nproc 2>/dev/null || echo 8)

# ---------------------------------------------------------------- LVGL
if [[ $FORCE == 0 && -f "$LIB" && "$LIB" -nt host/lv_conf.h && "$LIB" -nt host/build_lvgl.sh ]]; then
    echo "liblvgl.a is up to date"
else
    mkdir -p "$OBJ"
    rm -f "$OBJ"/*.o "$LIB"
    # Every LVGL source; the parts disabled in lv_conf.h compile to empty objects.
    find "$LV/src" -name '*.c' | sort > "$OUT/sources.txt"
    N=$(wc -l < "$OUT/sources.txt")
    echo "compiling $N LVGL sources with zig cc ($TARGET)..."
    compile_one() {
        src="$1"
        o="$OBJ/$(echo "${src#*/src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
        zig cc -target "$TARGET" -std=c99 -O2 -g0 -w -DLV_CONF_INCLUDE_SIMPLE \
            -I host -I "$LV" -c "$src" -o "$o"
    }
    export -f compile_one
    export OBJ TARGET LV
    xargs -P "$NPROC" -I{} bash -c 'compile_one "$@"' _ {} < "$OUT/sources.txt"
    zig ar rcs "$LIB" "$OBJ"/*.o
    echo "built $LIB ($(ls "$OBJ"/*.o | wc -l) objects)"
fi

# ---------------------------------------------------------------- UI assets (fonts + icons)
ALIB=$OUT/liborbit_assets.a
AOBJ=$OUT/assets_obj
ASSETS=$(ls firmware/main/ui/fonts/*.c firmware/main/ui/icons/*.c 2>/dev/null || true)
if [[ -z "$ASSETS" ]]; then
    echo "no generated fonts/icons yet (run tools/fontgen/build.sh and tools/icongen/icongen.py)"
    exit 0
fi
stale=$FORCE
[[ -f "$ALIB" ]] || stale=1
if [[ $stale == 0 ]]; then
    for f in $ASSETS firmware/main/ui/icons/icons.h host/lv_conf.h; do
        [[ "$f" -nt "$ALIB" ]] && { stale=1; break; }
    done
fi
if [[ $stale == 0 ]]; then
    echo "liborbit_assets.a is up to date"
    exit 0
fi
mkdir -p "$AOBJ"
rm -f "$AOBJ"/*.o "$ALIB"
compile_asset() {
    src="$1"
    o="$AOBJ/$(basename "${src%.c}").o"
    zig cc -target "$TARGET" -std=c99 -O1 -g0 -w -DLV_CONF_INCLUDE_SIMPLE \
        -I host -I "$LV" -I firmware/main/ui/icons -c "$src" -o "$o"
}
export -f compile_asset
export AOBJ TARGET LV
printf '%s\n' $ASSETS | xargs -P "$NPROC" -I{} bash -c 'compile_asset "$@"' _ {}
zig ar rcs "$ALIB" "$AOBJ"/*.o
echo "built $ALIB ($(ls "$AOBJ"/*.o | wc -l) objects)"
