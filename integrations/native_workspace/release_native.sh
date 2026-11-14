#!/bin/bash
# Sideloaded combined (NATIVE_KICAD + NATIVE_FREECAD) edition release, after the workspace device
# build (.github/workflows/combined.yml: deps.py, wx_build.py, kicad_build.py, the FreeCAD session
# against the same prefix, compose.py): an unsigned IPA plus the complete corresponding source for
# it (GPL-3.0-or-later as a whole). Publishing is a separate step.
#   release_native.sh <corvcad-git-checkout> <build-dir> <kicad-checkout> <output-dir>
# <build-dir> holds native-workspace/ (deps/iphoneos with vcpkg, link/mixed-link.rsp), freecad-ios/
# (source-ios/FreeCAD, python/, occt/, package/device; deps -> the workspace prefix), wx-ios/wx.tar.bz2
# and ngspice/. <kicad-checkout> is a fetch.py --full-source checkout. VCPKG_DOWNLOADS, when set, is
# where vcpkg downloaded every dependency's source; VERSION=<x.y.z> sets the app's version.
set -euo pipefail
REPO=$(cd "$1" && pwd); B=$(cd "$2" && pwd); K=$(cd "$3" && pwd); OUT=$4
REV=${VERSION:-$(git -C "$REPO" rev-parse --short=12 HEAD)}
NAME=CorvCAD-Combined-$REV-unsigned.ipa
lock() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d$2)" "$REPO/integrations/$1"; }
FREECAD_COMMIT=$(lock freecad_core/upstream.json "['source']['commit']"); FREECAD_TAG=$(lock freecad_core/upstream.json "['source']['tag']")
PYTHON=$(lock freecad_core/ios/inputs.json "['python']['version']"); VCPKG_COMMIT=$(lock native_workspace/inputs.json "['vcpkg']['commit']")
KICAD_COMMIT=$(lock kicad_core/upstream.json "['schematic_source']['commit']"); KICAD_TAG=$(lock kicad_core/upstream.json "['schematic_source']['tag']")
WX=$(lock kicad_core/upstream.json "['wxwidgets']['version']"); NG=$(lock kicad_core/upstream.json "['ngspice']['version']")
W=$B/native-workspace/deps/iphoneos
mkdir -p "$OUT/source" && OUT=$(cd "$OUT" && pwd)
[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || { echo "CorvCAD checkout has local changes"; exit 1; }
[ -s "$B/native-workspace/link/mixed-link.rsp" ] || { echo "no combined link (compose.py)"; exit 1; }
python3 "$REPO/integrations/kicad_core/audit.py" verify --kicad "$K"

(cd "$REPO/app" && xcodegen generate -q)
xcodebuild build -project "$REPO/app/CorvCAD.xcodeproj" -scheme CorvCAD -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath "$OUT/DerivedData" \
  NATIVE_KICAD=YES NATIVE_FREECAD=YES NATIVE_WORKSPACE_LINK="$B/native-workspace/link/mixed-link.rsp" \
  FREECAD_PACKAGE="$B/freecad-ios/package" CODE_SIGNING_ALLOWED=NO ${VERSION:+MARKETING_VERSION=$VERSION} \
  > "$OUT/xcodebuild.log" 2>&1 || { grep -E 'error:' "$OUT/xcodebuild.log" | sort -u | head -40; exit 1; }
rm -rf "$OUT/Payload" && mkdir "$OUT/Payload"
cp -R "$OUT/DerivedData/Build/Products/Release-iphoneos/CorvCAD.app" "$OUT/Payload/"
(cd "$OUT" && rm -f "$NAME" && zip -qr "$NAME" Payload)

# Corresponding source: CorvCAD (both engines' patches, stand-ins, sessions, the workspace recipe and
# these scripts), pristine KiCad and FreeCAD at their pins, the pinned wx, ngspice, CPython and OCCT
# release archives, and vcpkg's recipes with the source archives it downloaded for every dependency.
S=$OUT/source
git -C "$REPO" archive --prefix="corvcad/" -o "$S/corvcad-$REV.tar.gz" HEAD
git -C "$K" archive --prefix="kicad/" -o "$S/kicad-$KICAD_TAG.tar.gz" "$KICAD_COMMIT"
[ "$(git -C "$B/freecad-ios/source-ios/FreeCAD" rev-parse HEAD)" = "$FREECAD_COMMIT" ] || { echo "FreeCAD checkout is not at the pin"; exit 1; }
git -C "$B/freecad-ios/source-ios/FreeCAD" archive --prefix="FreeCAD/" -o "$S/FreeCAD-$FREECAD_TAG.tar.gz" "$FREECAD_COMMIT"
cp "$B/wx-ios/wx.tar.bz2" "$S/wxWidgets-$WX.tar.bz2"
cp "$B/ngspice/ngspice-$NG.tar.gz" "$S/"
cp "$B/freecad-ios/python/Python-$PYTHON.tar.xz" "$S/"
cp "$B/freecad-ios/occt/OCCT-V7_8_1.tar.gz" "$S/"
git -C "$W/vcpkg" archive --prefix="vcpkg/" -o "$S/vcpkg-$VCPKG_COMMIT.tar.gz" "$VCPKG_COMMIT"
D=${VCPKG_DOWNLOADS:-$W/vcpkg/downloads}
tar -czf "$S/vcpkg-downloads.tar.gz" -C "$(dirname "$D")" --exclude "$(basename "$D")/tools" \
  --exclude "$(basename "$D")/*.part" "$(basename "$D")"
cat > "$S/README.txt" <<EOF
Corresponding source for $NAME (GPL-3.0-or-later as a whole).

corvcad-$REV.tar.gz          CorvCAD at the release commit: integrations/kicad_core (KiCad patches,
                             headless replacements, gate 1 inventory), integrations/freecad_core
                             (FreeCAD patches, mechanical session, licence audit), integrations/parts3d,
                             and integrations/native_workspace (the shared-dependency recipe).
kicad-$KICAD_TAG.tar.gz          KiCad at $KICAD_COMMIT; fetch.py applies the patches in upstream.json order.
FreeCAD-$FREECAD_TAG.tar.gz        FreeCAD at $FREECAD_COMMIT, unmodified; prepare_ios.py applies CorvCAD's patches.
wxWidgets-$WX.tar.bz2      The pinned wx release (SHA-256 in kicad_core/upstream.json).
ngspice-$NG.tar.gz           The pinned ngspice release (SHA-256 in kicad_core/upstream.json), built by ngspice_ios.sh.
Python-$PYTHON.tar.xz        The pinned CPython release (SHA-256 in freecad_core/ios/inputs.json).
OCCT-V7_8_1.tar.gz           The pinned Open CASCADE release (SHA-256 in freecad_core/ios/build_occt.py).
vcpkg-*.tar.gz               vcpkg at its pinned baseline (port recipes and patches), used by deps.py.
vcpkg-downloads.tar.gz       The dependency source archives vcpkg downloaded for this build.

Build order for iphoneos (arm64): the steps of .github/workflows/combined.yml, in order:
fetch.py, ngspice_ios.sh (mac, then device), wx's source tarball, native_workspace/deps.py,
wx_build.py --system-regex, the probe CMake configure with NGSPICE_BUILD, kicad_build.py,
build_python.py --system-expat and build_occt.py, session/build.py, stage_runtime.py,
session/package.py, compose.py, then this script. Placing vcpkg-downloads' files in
vcpkg/downloads lets vcpkg build without network access.
EOF
(cd "$S" && shasum -a 256 ./* > SHA256SUMS)
echo "ipa: $OUT/$NAME"
du -sh "$OUT"/*.ipa "$S"/*
