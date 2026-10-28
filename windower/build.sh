#!/bin/bash
# Build portfix.dll for Windower 4 with mingw-w64 (i686).
#
# Usage: ./build.sh [install]   - "install" also copies the addon into Windower4/addons/portfix
set -euo pipefail
cd "$(dirname "$0")"
CORE=../core

WINDOWER_DIR="${WINDOWER_DIR:-$HOME/Games/final-fantasy-xi-online/drive_c/Windower4}"
CXX="${CXX:-i686-w64-mingw32-g++}"
OUT=build

mkdir -p "$OUT/portfix"
"$CXX" -std=c++17 -O2 -Wall -shared -static -static-libgcc -static-libstdc++ -I "$CORE" \
    src/portfix.cpp "$CORE"/*.cpp -o "$OUT/portfix/portfix.dll" -lwinhttp -lversion -Wl,--enable-stdcall-fixup -s
cp portfix/portfix.lua "$OUT/portfix/"

echo "Built $OUT/portfix"
if [ "${1:-}" = install ]; then
    if ps -eo args= | grep -q "[P]layOnlineViewer.pol\.exe"; then echo "pol.exe is running (the dll may be loaded) - not installing"; exit 1; fi
    mkdir -p "$WINDOWER_DIR/addons/portfix"
    cp "$OUT/portfix/portfix.dll" "$OUT/portfix/portfix.lua" "$WINDOWER_DIR/addons/portfix/"
    echo "Installed to $WINDOWER_DIR/addons/portfix"
fi
