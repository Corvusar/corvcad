#!/bin/bash
# Build the headless slice's third-party libraries for iOS with vcpkg (release tag pinned in
# upstream.json). macOS with Xcode.
#   ios_deps.sh <work-dir> [triplet]      e.g. ios_deps.sh build/ios-deps x64-ios-simulator
# Installed headers and static libraries land in <work-dir>/installed/<triplet>.
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd); TRIPLET=${2:-x64-ios-simulator}
HERE=$(cd "$(dirname "$0")" && pwd)
read -r REPO TAG COMMIT <<< "$(python3 -c "
import json, sys
v = json.load(open(sys.argv[1]))['vcpkg']
print(v['repository'], v['tag'], v['commit'])" "$HERE/upstream.json")"
PORTS=$(python3 -c "import json, sys; print(' '.join(json.load(open(sys.argv[1]))['vcpkg']['ports']))" "$HERE/upstream.json")

[ -d "$W/vcpkg" ] || git clone -q "$REPO" "$W/vcpkg"
git -C "$W/vcpkg" fetch -q origin "refs/tags/$TAG:refs/tags/$TAG"
ACTUAL=$(git -C "$W/vcpkg" rev-parse "$TAG^{commit}")
[ "$ACTUAL" = "$COMMIT" ] || { echo "vcpkg tag $TAG is $ACTUAL; expected $COMMIT"; exit 1; }
[ -z "$(git -C "$W/vcpkg" status --porcelain --untracked-files=no)" ] \
  || { echo "vcpkg has local recipe changes; preserve it and use a new work directory"; exit 1; }
git -C "$W/vcpkg" checkout -q --detach "$COMMIT"
[ -x "$W/vcpkg/vcpkg" ] || "$W/vcpkg/bootstrap-vcpkg.sh" -disableMetrics > "$W/bootstrap.log"
# shellcheck disable=SC2086
"$W/vcpkg/vcpkg" install --classic --overlay-triplets="$HERE/triplets" --triplet "$TRIPLET" \
  --x-install-root="$W/installed" $PORTS > "$W/install-$TRIPLET.log" 2>&1 \
  || { tail -30 "$W/install-$TRIPLET.log"; exit 1; }
echo "iOS dependencies ($TRIPLET): $W/installed/$TRIPLET"
