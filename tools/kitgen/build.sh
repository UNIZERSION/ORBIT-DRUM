#!/usr/bin/env bash
# Builds tools/kitgen with zig and regenerates the built-in kits:
#   sdcard/ORBIT/KITS/{808_STYLE,909_STYLE}/*.wav + kit.txt
#   firmware/main/core/kits_808.c, kits_909.c (packed by kitpack.py)
#   host/out/kit_demo_808.wav, kit_demo_909.wav
# Run from anywhere (Git Bash): bash tools/kitgen/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/host/.zig-cache/global" ZIG_LOCAL_CACHE_DIR="$ROOT/host/.zig-cache/local"
mkdir -p tools/kitgen/bin host/out sdcard/ORBIT/KITS/808_STYLE sdcard/ORBIT/KITS/909_STYLE
zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Wdouble-promotion -Wshadow \
    tools/kitgen/kitgen.cpp tools/kitgen/voices808.cpp tools/kitgen/voices909.cpp \
    -o tools/kitgen/bin/kitgen.exe
rm -f sdcard/ORBIT/KITS/808_STYLE/*.wav sdcard/ORBIT/KITS/909_STYLE/*.wav
./tools/kitgen/bin/kitgen.exe "$ROOT"
# 0.4.9: the flash copies are packed losslessly (KPK1, unpacked into PSRAM on first use by core/kits.cpp)
python -I tools/kitgen/kitpack.py
