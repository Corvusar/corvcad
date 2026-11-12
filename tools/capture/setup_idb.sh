#!/bin/bash
# Local Intel macOS capture tooling; separate from the CorvCAD app build.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:-"$ROOT/build/capture-idb"}
COMMIT=36ba98251ba558bca2cc706da503b9ba5095c21d
[ "$(uname -m)" = x86_64 ] || { echo 'This capture patch is for the Intel iMac.' >&2; exit 1; }
[ ! -e "$DEST" ] || { echo "Choose a new tooling directory: $DEST already exists." >&2; exit 1; }
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin https://github.com/facebook/idb.git
git -C "$DEST" fetch --depth 1 origin "$COMMIT"
git -C "$DEST" checkout --detach FETCH_HEAD
git -C "$DEST" apply "$ROOT/tools/capture/idb-intel.patch"
(cd "$DEST" && ./build.sh build sim-video)
printf 'Capture executable: %s/Build/Products/Release/sim-video\n' "$DEST"
