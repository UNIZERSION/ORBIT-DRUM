#!/usr/bin/env bash
# Builds and runs every host (PC) test against the REAL module implementations and prints
# PASS/FAIL per test. Exit code 0 only when every selected test passes.
#
#   bash tools/run_host_tests.sh                 # all tests
#   bash tools/run_host_tests.sh seq engine      # only these (names below, without "test_")
#   bash tools/run_host_tests.sh -j 4            # parallel builds (default: 4); runs are serial
#   bash tools/run_host_tests.sh --no-build      # run the existing host/bin/*.exe again
#
# Tests: core_basics seq engine sampleio kits app widgets render_scenes
#        shot_chrome shot_sample shot_seqmix shot_browser_dlg ui_shots hal_logic bt_protocol
# (shot_* = the screen modules' own tests, host/shots/<module>_*.png; ui_shots = host/render_ui.cpp: the
#  whole UI with the real App, host/shots/ui_*.png named after reference/shots + interaction smoke test;
#  hal_logic = the portable decision logic of the Tab5 HAL, firmware/main/hal/hal_logic.h;
#  bt_protocol = the Pico Bluetooth bridge client, firmware/main/hal/bt_protocol.h + the host hal::bt*)
# Logs: host/out/test_logs/<name>.build.log and <name>.run.log
# Needs zig (0.17) on PATH and, for persist.cpp, ESP-IDF's cJSON source (read-only use):
# $IDF_PATH/components/json/cJSON or $HOME/esp/v5.5.2/esp-idf/components/json/cJSON.
# The only test-only code linked is host/tests/stubs_for_app.cpp (records ui::toast for
# test_app, which has no LVGL) and host/tests/fake_app.cpp (minimal App for test_widgets).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
export ZIG_GLOBAL_CACHE_DIR="$ROOT/host/.zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$ROOT/host/.zig-cache/local"

ALL="core_basics seq engine sampleio kits app widgets render_scenes shot_chrome shot_sample shot_seqmix shot_browser_dlg ui_shots hal_logic bt_protocol acid"
JOBS=4
BUILD=1
SEL=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --no-build) BUILD=0; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) SEL="$SEL $1"; shift ;;
    esac
done
[[ -z "$SEL" ]] && SEL="$ALL"

T=x86_64-windows-gnu
C=firmware/main/core
U=firmware/main/ui
LV=firmware/managed_components/lvgl__lvgl
J="${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}/components/json/cJSON"
BIN=host/bin
LOGS=host/out/test_logs
mkdir -p "$BIN" "$LOGS" host/shots   # (not in git: a fresh clone has none of them)
CXX="zig c++ -target $T -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost"
BASE="$C/params.cpp $C/project.cpp $C/sample.cpp host/platform_host.cpp"
KITOBJ="$BIN/kits_808.o $BIN/kits_909.o"
# UI programs: LVGL host display + real App + host HAL
UICXX="$CXX -DLV_CONF_INCLUDE_SIMPLE -I$LV -I$J"
WKIT="$U/app.cpp $U/theme.cpp $U/ui_scale.cpp $U/widgets.cpp $U/widgets_knob.cpp $U/widgets_input.cpp $U/widgets_overlay.cpp"
UILINK="host/hal_host.cpp host/platform_host.cpp host/lvgl_host.cpp $BIN/cJSON.o $KITOBJ host/lib/liborbit_assets.a host/lib/liblvgl.a"

# One build command per test (each test file's header carries the same line for manual use).
build_cmd() {
    case "$1" in
        core_basics) echo "$CXX host/tests/test_core_basics.cpp $BASE $C/settings.cpp -o $BIN/test_core_basics.exe" ;;
        seq) echo "$CXX host/tests/test_seq.cpp $C/seq.cpp $BASE -o $BIN/test_seq.exe" ;;
        engine) echo "$CXX host/tests/test_engine.cpp $C/engine.cpp $C/voice.cpp $C/fx.cpp $C/seq.cpp $C/acid.cpp $BASE -o $BIN/test_engine_seq.exe" ;;
        sampleio) echo "$CXX -g -fsanitize=undefined -I$J host/tests/test_sampleio.cpp $C/wav.cpp $C/sample_ops.cpp $C/slice.cpp $C/library.cpp $C/persist.cpp $BASE $BIN/cJSON.o host/testdata/utf8/utf8.rc -o $BIN/test_sampleio.exe" ;;
        kits) echo "$CXX host/tests/test_kits.cpp $C/kits.cpp $KITOBJ -o $BIN/test_kits.exe" ;;
        app) echo "$CXX -I$J -DHAL_HOST_NO_LVGL host/tests/test_app.cpp host/tests/stubs_for_app.cpp $U/app.cpp $(ls $C/*.cpp | tr '\n' ' ') host/hal_host.cpp host/platform_host.cpp $BIN/cJSON.o $KITOBJ -o $BIN/test_app.exe" ;;
        widgets) echo "$CXX -fno-exceptions -fno-rtti -DLV_CONF_INCLUDE_SIMPLE -I$LV host/tests/test_widgets.cpp host/tests/fake_app.cpp host/lvgl_host.cpp $U/theme.cpp $U/ui_scale.cpp $U/widgets.cpp $U/widgets_knob.cpp $U/widgets_input.cpp $U/widgets_overlay.cpp $C/params.cpp $C/settings.cpp host/lib/liborbit_assets.a host/lib/liblvgl.a -o $BIN/test_widgets.exe" ;;
        render_scenes) echo "$CXX host/tests/render_scenes.cpp $C/engine.cpp $C/voice.cpp $C/fx.cpp $C/seq.cpp $C/acid.cpp $C/kits.cpp $BASE $KITOBJ -o $BIN/render_scenes.exe" ;;
        shot_chrome) echo "$UICXX -DORBIT_HOST host/tests/shot_chrome.cpp $U/ui.cpp $U/ui_bg.cpp $U/scr_top.cpp $U/scr_pads.cpp $U/scr_catpanel.cpp $U/scr_macros.cpp $U/scr_timeline.cpp $WKIT $(ls $C/*.cpp | tr '\n' ' ') $UILINK -o $BIN/shot_chrome.exe" ;;
        shot_sample) echo "$UICXX host/tests/shot_sample.cpp $U/view_sample.cpp $U/wave_view.cpp $U/dlg_sample.cpp $WKIT $(ls $C/*.cpp | tr '\n' ' ') $UILINK -o $BIN/shot_sample.exe" ;;
        shot_seqmix) echo "$UICXX host/tests/shot_seqmix.cpp $U/view_sequence.cpp $U/step_grid.cpp $U/view_mixer.cpp $U/view_fx.cpp $WKIT $(ls $C/*.cpp | tr '\n' ' ') $UILINK -o $BIN/shot_seqmix.exe" ;;
        shot_browser_dlg) echo "$UICXX host/tests/shot_browser_dlg.cpp $U/view_browser.cpp $U/dlg_settings.cpp $U/dlg_project.cpp $U/dlg_help.cpp $U/wave_view.cpp $WKIT $(ls $C/*.cpp | tr '\n' ' ') $UILINK -o $BIN/shot_browser_dlg.exe" ;;
        ui_shots) echo "$UICXX -DORBIT_HOST host/render_ui.cpp $(ls $U/*.cpp | tr '\n' ' ') $(ls $C/*.cpp | tr '\n' ' ') $UILINK -o $BIN/render_ui.exe" ;;
        hal_logic) echo "$CXX host/tests/test_hal_logic.cpp -o $BIN/test_hal_logic.exe" ;;
        bt_protocol) echo "$CXX -DHAL_HOST_NO_LVGL host/tests/test_bt_protocol.cpp host/hal_host.cpp $C/settings.cpp -o $BIN/test_bt_protocol.exe" ;;
        acid) echo "$CXX host/tests/test_acid.cpp $C/acid.cpp -o $BIN/test_acid.exe" ;;
        *) return 1 ;;
    esac
}
exe_of() {
    case "$1" in
        engine) echo "$BIN/test_engine_seq.exe" ;;
        render_scenes) echo "$BIN/render_scenes.exe" ;;
        ui_shots) echo "$BIN/render_ui.exe" ;;
        shot_*) echo "$BIN/$1.exe" ;;
        *) echo "$BIN/test_$1.exe" ;;
    esac
}

for t in $SEL; do
    build_cmd "$t" >/dev/null || { echo "unknown test: $t (known: $ALL)"; exit 2; }
done

declare -A RES
if [[ $BUILD == 1 ]]; then
    # Shared objects, rebuilt only when stale: cJSON (persist.cpp) and the two kit tables
    # (megabytes of C; compiled as C11 like the firmware does).
    if [[ ! -f "$BIN/cJSON.o" || "$J/cJSON.c" -nt "$BIN/cJSON.o" ]]; then
        echo "building cJSON.o"
        zig cc -target $T -O2 -c "$J/cJSON.c" -o "$BIN/cJSON.o" || { echo "cJSON build FAILED"; exit 1; }
    fi
    for k in 808 909; do
        if [[ ! -f "$BIN/kits_$k.o" || "$C/kits_$k.c" -nt "$BIN/kits_$k.o" ]]; then
            echo "building kits_$k.o"
            zig cc -target $T -std=c11 -O1 -g0 -c "$C/kits_$k.c" -o "$BIN/kits_$k.o" || { echo "kits_$k build FAILED"; exit 1; }
        fi
    done
    if [[ " $SEL " == *" widgets "* || " $SEL " == *" ui_shots "* || " $SEL " == *" shot_"* ]]; then
        bash host/build_lvgl.sh > "$LOGS/lvgl.build.log" 2>&1 || { echo "build_lvgl.sh FAILED (see $LOGS/lvgl.build.log)"; RES[widgets]=BUILD; }
    fi
    echo "building:$SEL (-j $JOBS)"
    running=0
    for t in $SEL; do
        [[ -n "${RES[$t]:-}" ]] && continue
        (
            cmd="$(build_cmd "$t")"
            { echo "\$ $cmd"; eval "$cmd"; } > "$LOGS/$t.build.log" 2>&1
            echo $? > "$LOGS/$t.build.rc"
        ) &
        running=$((running + 1))
        if [[ $running -ge $JOBS ]]; then wait -n; running=$((running - 1)); fi
    done
    wait
    for t in $SEL; do
        [[ -n "${RES[$t]:-}" ]] && continue
        rc=$(cat "$LOGS/$t.build.rc" 2>/dev/null || echo 1)
        if [[ "$rc" != 0 ]]; then
            RES[$t]=BUILD
        elif grep -q "warning:" "$LOGS/$t.build.log"; then
            echo "  note: $t built with warnings (see $LOGS/$t.build.log)"
        fi
    done
fi

fails=0
for t in $SEL; do
    if [[ "${RES[$t]:-}" == BUILD ]]; then
        printf 'FAIL  %-14s build failed, see %s\n' "$t" "$LOGS/$t.build.log"
        fails=$((fails + 1))
        continue
    fi
    exe="$(exe_of "$t")"
    if [[ ! -x "$exe" ]]; then
        printf 'FAIL  %-14s %s missing\n' "$t" "$exe"
        fails=$((fails + 1))
        continue
    fi
    start=$(date +%s)
    "$exe" > "$LOGS/$t.run.log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    last="$(grep -v '^[[:space:]]*$' "$LOGS/$t.run.log" | tail -n 1 | tr -d '\r')"
    if [[ $rc == 0 && "$last" == *passed* && "$last" != *FAILED* ]]; then
        printf 'PASS  %-14s %3ss  %s\n' "$t" "$secs" "$last"
    else
        printf 'FAIL  %-14s %3ss  exit %s: %s (see %s)\n' "$t" "$secs" "$rc" "$last" "$LOGS/$t.run.log"
        fails=$((fails + 1))
    fi
done

n=$(echo $SEL | wc -w)
if [[ $fails == 0 ]]; then
    echo "host tests: all $n passed"
    exit 0
fi
echo "host tests: $fails of $n FAILED"
exit 1
