#!/bin/bash
# Build the release archives into dist/.
# Usage: tools/package.sh <version>
#   dist/portfix-ashita-<version>.zip: plugins/portfix.dll, extracted into the Ashita folder.
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:?usage: tools/package.sh <version>}"
DIST=dist
rm -rf "$DIST/stage"
mkdir -p "$DIST/stage/ashita/plugins"

ashita/build.sh
cp ashita/build/portfix.dll "$DIST/stage/ashita/plugins/"
(cd "$DIST/stage/ashita" && rm -f "../../portfix-ashita-$VER.zip" && zip -qr "../../portfix-ashita-$VER.zip" .)

rm -rf "$DIST/stage"
ls -l "$DIST"
