#!/bin/bash
# Sideloaded NATIVE_FREECAD edition release, after the device build of the FreeCAD session
# (deps.sh, build_python.py, build_occt.py, session/build.py, stage_runtime.py and
# session/package.py for iphoneos, as .github/workflows/release.yml runs them): an unsigned IPA
# plus the complete corresponding source for it (GPL-3.0-or-later as a whole: FreeCAD is
# LGPL-2.1-or-later, the 3D part generator port GPL-3.0-or-later). Publishing is a separate step.
#   release_native.sh <corvcad-git-checkout> <freecad-ios-root> <output-dir>
# <freecad-ios-root> is the build root (build/freecad-ios): source-ios/FreeCAD, deps/ with vcpkg
# and its downloads, python/, occt/, package/device. VERSION=<x.y.z> sets the app's version.
# vcpkg's downloads must hold every dependency's source: restore them together with any binary
# cache, never a binary cache alone.
set -euo pipefail
REPO=$(cd "$1" && pwd); R=$(cd "$2" && pwd); OUT=$3
REV=${VERSION:-$(git -C "$REPO" rev-parse --short=12 HEAD)}
lock() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d$2)" "$REPO/integrations/freecad_core/$1"; }
FREECAD_COMMIT=$(lock upstream.json "['source']['commit']"); FREECAD_TAG=$(lock upstream.json "['source']['tag']")
VCPKG_COMMIT=$(lock ios/inputs.json "['vcpkg']['commit']"); PYTHON=$(lock ios/inputs.json "['python']['version']")
mkdir -p "$OUT/source" && OUT=$(cd "$OUT" && pwd)
[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || { echo "CorvCAD checkout has local changes"; exit 1; }
[ -f "$R/package/device/libcorvcad_freecad.a" ] || { echo "no device package in $R/package/device"; exit 1; }

(cd "$REPO/app" && xcodegen generate -q)
xcodebuild build -project "$REPO/app/CorvCAD.xcodeproj" -scheme CorvCAD -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath "$OUT/DerivedData" \
  NATIVE_FREECAD=YES FREECAD_PACKAGE="$R/package" CODE_SIGNING_ALLOWED=NO ${VERSION:+MARKETING_VERSION=$VERSION} \
  > "$OUT/xcodebuild.log" 2>&1 || { grep -E 'error:' "$OUT/xcodebuild.log" | sort -u | head -40; exit 1; }
rm -rf "$OUT/Payload" && mkdir "$OUT/Payload"
cp -R "$OUT/DerivedData/Build/Products/Release-iphoneos/CorvCAD.app" "$OUT/Payload/"
(cd "$OUT" && rm -f "CorvCAD-FreeCAD-$REV-unsigned.ipa" && zip -qr "CorvCAD-FreeCAD-$REV-unsigned.ipa" Payload)

# Corresponding source: CorvCAD (FreeCAD's patches, the session, the part generator port and these
# scripts), pristine FreeCAD at the pinned commit, CPython and OCCT as the pinned release archives,
# and vcpkg's recipes with the source archives it downloaded for every dependency.
S=$OUT/source
git -C "$REPO" archive --prefix="corvcad/" -o "$S/corvcad-$REV.tar.gz" HEAD
[ "$(git -C "$R/source-ios/FreeCAD" rev-parse HEAD)" = "$FREECAD_COMMIT" ] || { echo "FreeCAD checkout is not at the pin"; exit 1; }
git -C "$R/source-ios/FreeCAD" archive --prefix="FreeCAD/" -o "$S/FreeCAD-$FREECAD_TAG.tar.gz" "$FREECAD_COMMIT"
cp "$R/python/Python-$PYTHON.tar.xz" "$S/"
cp "$R/occt/OCCT-V7_8_1.tar.gz" "$S/"
git -C "$R/deps/vcpkg" archive --prefix="vcpkg/" -o "$S/vcpkg-$VCPKG_COMMIT.tar.gz" "$VCPKG_COMMIT"
D=${VCPKG_DOWNLOADS:-$R/deps/vcpkg/downloads}
tar -czf "$S/vcpkg-downloads.tar.gz" -C "$(dirname "$D")" --exclude "$(basename "$D")/tools" \
  --exclude "$(basename "$D")/*.part" "$(basename "$D")"
cat > "$S/README.txt" <<EOF
Corresponding source for CorvCAD-FreeCAD-$REV-unsigned.ipa (GPL-3.0-or-later as a whole).

corvcad-$REV.tar.gz         CorvCAD at the release commit, including integrations/freecad_core (FreeCAD
                            patches, the mechanical session, build scripts, licence audit) and
                            integrations/parts3d (the port of KiCad's 3D model generator).
FreeCAD-$FREECAD_TAG.tar.gz       FreeCAD at $FREECAD_COMMIT, unmodified; prepare_ios.py and
                            session/build.py apply CorvCAD's patches.
Python-$PYTHON.tar.xz      The pinned CPython release (SHA-256 in ios/inputs.json), built by build_python.py.
OCCT-V7_8_1.tar.gz          The pinned Open CASCADE release (SHA-256 in ios/build_occt.py).
vcpkg-*.tar.gz              vcpkg at its pinned baseline (port recipes and patches), used by deps.sh.
vcpkg-downloads.tar.gz      The dependency source archives vcpkg downloaded for this build.

Build order for iphoneos (triplet arm64-ios), from the corvcad directory:
  bash integrations/freecad_core/ios/deps.sh build/freecad-ios/deps arm64-ios
  python3 integrations/freecad_core/ios/dependency_inventory.py build/freecad-ios/deps/installed arm64-ios \\
    build/freecad-ios/deps/inventory-arm64-ios.json --verify-target
  python3 integrations/freecad_core/ios/build_python.py --sdk iphoneos --deps build/freecad-ios/deps/installed/arm64-ios
  python3 integrations/freecad_core/ios/build_occt.py --sdk iphoneos
  python3 integrations/freecad_core/session/build.py --sdk iphoneos
  python3 integrations/freecad_core/ios/stage_runtime.py ... (see .github/workflows/release.yml)
  python3 integrations/freecad_core/session/package.py --sdk iphoneos --output build/freecad-ios/package/device
  VERSION=<version> bash integrations/freecad_core/release_native.sh . build/freecad-ios <output>
Placing vcpkg-downloads' files in vcpkg/downloads lets vcpkg build without network access.
EOF
(cd "$S" && shasum -a 256 ./* > SHA256SUMS)
echo "ipa: $OUT/CorvCAD-FreeCAD-$REV-unsigned.ipa"
du -sh "$OUT"/*.ipa "$S"/*
