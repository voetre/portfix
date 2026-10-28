#!/bin/bash
# Build portfix.dll with clang-cl against the MSVC CRT + Windows SDK splatted by xwin, so the plugin uses the
# same C++ ABI as Ashita itself.
#
# One-time setup:
#   xwin --accept-license --arch x86 --cache-dir ~/.cache/xwin splat --output ~/.xwin
#
# Usage: ./build.sh [install]   - "install" also copies the dll into Ashita/plugins
set -euo pipefail
cd "$(dirname "$0")"
CORE=../core

XWIN="${XWIN:-$HOME/.xwin}"
ASHITA_DIR="${ASHITA_DIR:-$HOME/Games/final-fantasy-xi-online/drive_c/Ashita}"
LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-21/bin}"
OUT=build

# Build against a copy of the Ashita SDK: some of its DirectX headers end in a DOS EOF (0x1A) byte that clang
# rejects, so strip it from the copy instead of touching the Ashita install.
if [ ! -d "$OUT/sdk" ] || [ "$ASHITA_DIR/plugins/sdk/Ashita.h" -nt "$OUT/sdk/Ashita.h" ]; then
    rm -rf "$OUT/sdk"
    mkdir -p "$OUT"
    cp -r "$ASHITA_DIR/plugins/sdk" "$OUT/sdk"
    find "$OUT/sdk" \( -name '*.h' -o -name '*.inl' \) -exec sed -i 's/\x1a//g' {} +
fi

mkdir -p "$OUT/libalias"
# The SDK source asks for Psapi.lib; xwin only ships lower/upper-case names.
ln -sf "$XWIN/sdk/lib/um/x86/psapi.lib" "$OUT/libalias/Psapi.lib"

PATH="$LLVM_BIN:$PATH" clang-cl --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc /MT /LD \
    -Wno-unused-function -fuse-ld=lld-link \
    /imsvc "$XWIN/crt/include" /imsvc "$XWIN/sdk/include/ucrt" \
    /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared" \
    /I "$OUT/sdk" /I "$OUT/sdk/d3d8/includes" /I "$CORE" /Fo"$OUT/" src/*.cpp "$CORE"/*.cpp \
    /link /def:src/portfix.def /libpath:"$OUT/libalias" /libpath:"$XWIN/crt/lib/x86" \
    /libpath:"$XWIN/sdk/lib/um/x86" /libpath:"$XWIN/sdk/lib/ucrt/x86" user32.lib winhttp.lib version.lib /out:"$OUT/portfix.dll"

echo "Built $OUT/portfix.dll"
if [ "${1:-}" = install ]; then
    if ps -eo args= | grep -q "[P]layOnlineViewer.pol\.exe"; then echo "pol.exe is running (the dll may be loaded) - not installing"; exit 1; fi
    cp "$OUT/portfix.dll" "$ASHITA_DIR/plugins/portfix.dll"
    echo "Installed to $ASHITA_DIR/plugins/portfix.dll"
fi
