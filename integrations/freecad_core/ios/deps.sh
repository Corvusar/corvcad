#!/bin/bash
# Static, headless dependency investigation. No app/release packaging.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
ROOT=$(cd "$1" && pwd)
TRIPLET=${2:?arm64-ios, arm64-ios-simulator or x64-ios-simulator}
case "$TRIPLET" in arm64-ios|arm64-ios-simulator|x64-ios-simulator) ;; *) exit 2 ;; esac
HOST_TRIPLET=$(python3 "$HERE/target_config.py" host-triplet)
PIN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["vcpkg"]["commit"])' "$HERE/inputs.json")
if [ ! -d "$ROOT/vcpkg" ]; then
  git clone -q https://github.com/microsoft/vcpkg.git "$ROOT/vcpkg"
fi
[ -z "$(git -C "$ROOT/vcpkg" status --porcelain --untracked-files=all)" ] || { echo 'vcpkg has local changes'; exit 1; }
git -C "$ROOT/vcpkg" checkout -q --detach "$PIN"
[ "$(git -C "$ROOT/vcpkg" rev-parse HEAD)" = "$PIN" ]
[ -x "$ROOT/vcpkg/vcpkg" ] || "$ROOT/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
"$ROOT/vcpkg/vcpkg" install --x-manifest-root="$HERE" --overlay-triplets="$HERE/triplets" \
  --triplet "$TRIPLET" --host-triplet "$HOST_TRIPLET" --x-install-root="$ROOT/installed"
"$ROOT/vcpkg/vcpkg" list --x-install-root="$ROOT/installed" > "$ROOT/packages-$TRIPLET.txt"
