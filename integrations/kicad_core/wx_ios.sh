#!/bin/bash
# Build wxWidgets (version and checksum pinned in upstream.json) as static libraries for the
# iOS simulator (default) or an arm64 iPadOS device, for probe/ios_sweep.sh and the coming
# iPadOS slice build. macOS with Xcode.
#   [SIM_ARCH=x86_64|arm64] wx_ios.sh <work-dir> [sim|device]     e.g. wx_ios.sh build/wx-ios device
# The build lands in <work-dir>/build-sim or <work-dir>/build-device. The simulator slice is
# x86_64 (Intel Macs) unless SIM_ARCH=arm64 (Apple silicon runners).
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd)
case ${2:-sim}-${SIM_ARCH:-x86_64} in
  sim-x86_64) SDKNAME=iphonesimulator; ARCH=x86_64; HOST=x86_64-apple-darwin ;;
  # arm64 alone would mean the device: the triple names the simulator platform.
  sim-arm64)  SDKNAME=iphonesimulator; ARCH=arm64;  HOST="aarch64-apple-darwin --build=x86_64-apple-darwin"
              TRIPLE="-target arm64-apple-ios17.0-simulator" ;;
  # --build keeps configure in cross mode on an arm64 Mac, where host and build would match.
  device-*)   SDKNAME=iphoneos;        ARCH=arm64;  HOST="aarch64-apple-darwin --build=x86_64-apple-darwin" ;;
  *) echo "unknown target: ${2:-sim} with SIM_ARCH ${SIM_ARCH:-x86_64} (sim or device; x86_64 or arm64)"; exit 2 ;;
esac
LOCK=$(cd "$(dirname "$0")" && pwd)/upstream.json
read -r VERSION URL SHA <<< "$(python3 -c "
import json, sys
w = json.load(open(sys.argv[1]))['wxwidgets']
print(w['version'], w['url'], w['sha256'])" "$LOCK")"

cd "$W"
if [ ! -d "wxWidgets-$VERSION" ]; then
  curl -fsSL -o wx.tar.bz2 "$URL"
  echo "$SHA  wx.tar.bz2" | shasum -a 256 -c -
  tar xjf wx.tar.bz2
fi
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
D=build-${2:-sim}
mkdir -p $D && cd $D
"../wxWidgets-$VERSION/configure" --with-osx_iphone --disable-shared --disable-sys-libs \
  --with-macosx-sdk="$SDK" --with-macosx-version-min=17.0 --enable-macosx_arch=$ARCH \
  --host=$HOST CC="xcrun --sdk $SDKNAME clang -arch $ARCH ${TRIPLE:-}" CXX="xcrun --sdk $SDKNAME clang++ -arch $ARCH ${TRIPLE:-}" \
  > ../configure-$D.log 2>&1 || { tail -20 ../configure-$D.log; exit 1; }
make -j"${JOBS:-$(sysctl -n hw.ncpu)}" > ../make-$D.log 2>&1 || { tail -20 ../make-$D.log; exit 1; }
echo "wxWidgets $VERSION for $SDKNAME ($ARCH): $W/$D"
