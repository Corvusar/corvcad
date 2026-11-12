#!/bin/bash
# Sideloaded NATIVE_KICAD edition release, on the macOS build host that holds the pinned iOS
# dependency builds: an unsigned IPA plus the complete corresponding source for it
# (GPL-3.0-or-later; see docs/history/KICAD_GATE1_HANDOFF.md). Publishing both is a separate, manual step.
#   release_native.sh <corvcad-git-checkout> <build-dir> <kicad-checkout> <output-dir>
# <build-dir> holds kicad-probe/ (probe build + ios_sweep-device objects), wx-ios/ (wx_ios.sh)
# and ios-deps/ (ios_deps.sh). <kicad-checkout> is the fetch.py checkout with the patch series.
set -euo pipefail
REPO=$(cd "$1" && pwd); B=$(cd "$2" && pwd); K=$(cd "$3" && pwd); OUT=$4
REV=$(git -C "$REPO" rev-parse --short=12 HEAD)
lock() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['$1']['$2'])" "$REPO/integrations/kicad_core/upstream.json"; }
mkdir -p "$OUT/source" && OUT=$(cd "$OUT" && pwd)

# The KiCad tree must match the audited inventory before anything ships.
python3 "$REPO/integrations/kicad_core/audit.py" verify --kicad "$K"

bash "$REPO/integrations/kicad_core/probe/ios_package.sh" "$B/kicad-probe" "$B/wx-ios" "$B/ios-deps/installed"
(cd "$REPO/app" && xcodegen generate -q)
xcodebuild build -project "$REPO/app/CorvCAD.xcodeproj" -scheme CorvCAD -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath "$OUT/DerivedData" \
  NATIVE_KICAD=YES KICAD_PACKAGE="$B/kicad-probe/package" CODE_SIGNING_ALLOWED=NO > "$OUT/xcodebuild.log" 2>&1
rm -rf "$OUT/Payload" && mkdir "$OUT/Payload"
cp -R "$OUT/DerivedData/Build/Products/Release-iphoneos/CorvCAD.app" "$OUT/Payload/"
(cd "$OUT" && rm -f "CorvCAD-KiCad-$REV-unsigned.ipa" && zip -qr "CorvCAD-KiCad-$REV-unsigned.ipa" Payload)

# Corresponding source: CorvCAD (with the KiCad patches, headless replacements and these
# scripts), pristine KiCad at the pinned commit, the pinned wx tarball, and vcpkg's port
# recipes and downloaded source archives for every dependency.
S=$OUT/source
git -C "$REPO" archive --prefix="corvcad/" -o "$S/corvcad-$REV.tar.gz" HEAD
git -C "$K" archive --prefix="kicad/" -o "$S/kicad-$(lock schematic_source tag).tar.gz" "$(lock schematic_source commit)"
cp "$B/wx-ios/wx.tar.bz2" "$S/wxWidgets-$(lock wxwidgets version).tar.bz2"
git -C "$B/ios-deps/vcpkg" archive --prefix="vcpkg/" -o "$S/vcpkg-$(lock vcpkg commit).tar.gz" "$(lock vcpkg commit)"
tar -czf "$S/vcpkg-downloads.tar.gz" -C "$B/ios-deps/vcpkg" downloads
# ngspice, when the build has it (the probe build's NGSPICE_BUILD): the pinned release tarball.
NG=$(sed -n 's/^NGSPICE_BUILD:[A-Z]*=//p' "$B/kicad-probe/CMakeCache.txt")
NGNOTE=
if [ -n "$NG" ]; then
  cp "$NG/ngspice-$(lock ngspice version).tar.gz" "$S/"
  NGNOTE="ngspice-*.tar.gz       The pinned ngspice release (SHA-256 in upstream.json), built by ngspice_ios.sh."
fi
cat > "$S/README.txt" <<EOF
Corresponding source for CorvCAD-KiCad-$REV-unsigned.ipa (GPL-3.0-or-later as a whole).

corvcad-$REV.tar.gz     CorvCAD at $REV, including integrations/kicad_core (patches, headless
                        replacements, build scripts) and the gate 1 inventory.
kicad-*.tar.gz          KiCad at $(lock schematic_source commit); apply the patches with
                        corvcad/integrations/kicad_core/fetch.py or git apply, in upstream.json order.
wxWidgets-*.tar.bz2     The pinned wx release (SHA-256 in upstream.json), built by wx_ios.sh.
vcpkg-*.tar.gz          vcpkg at its pinned baseline (port recipes and patches), used by ios_deps.sh.
vcpkg-downloads.tar.gz  The dependency source archives that vcpkg downloaded for this build.
$NGNOTE

Build order: fetch.py, wx_ios.sh, ios_deps.sh, ngspice_ios.sh (mac, then device) when the build
has the simulator, the probe CMake build (docs/KICAD_CORE_PORT.md; NGSPICE_BUILD for the
simulator), probe/ios_sweep.sh (IOS_TARGET=device), then release_native.sh.
EOF
(cd "$S" && shasum -a 256 ./* > SHA256SUMS)
echo "ipa: $OUT/CorvCAD-KiCad-$REV-unsigned.ipa"
du -sh "$OUT"/*.ipa "$S"/*
