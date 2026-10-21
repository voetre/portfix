#!/bin/bash
# Build the release archives into dist/.
# Usage: tools/package.sh <version>
#   dist/portfix-ashita-<version>.zip: plugins/portfix.dll, extracted into the Ashita folder.
#   dist/portfix-windower-<version>.zip: addons/portfix/, extracted into the Windower folder.
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:?usage: tools/package.sh <version>}"
DIST=dist
rm -rf "$DIST/stage"
mkdir -p "$DIST/stage/ashita/plugins" "$DIST/stage/windower/addons/portfix"

ashita/build.sh
cp ashita/build/portfix.dll "$DIST/stage/ashita/plugins/"
(cd "$DIST/stage/ashita" && rm -f "../../portfix-ashita-$VER.zip" && zip -qr "../../portfix-ashita-$VER.zip" .)

windower/build.sh
cp windower/build/portfix/portfix.dll windower/build/portfix/portfix.lua "$DIST/stage/windower/addons/portfix/"
(cd "$DIST/stage/windower" && rm -f "../../portfix-windower-$VER.zip" && zip -qr "../../portfix-windower-$VER.zip" .)

rm -rf "$DIST/stage"
ls -l "$DIST"
