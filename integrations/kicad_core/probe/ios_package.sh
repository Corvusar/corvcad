#!/bin/bash
# Package KiCad for the app (sideloaded edition): per SDK, two static libraries from the
# objects ios_sweep.sh compiled and ios_link.sh proved.
#   libcorvcad_kicad.a       the measured native closure (measure.sh: the session and the board
#                            checks over both slices), kicad_session.cpp and kicad_board.cpp; the
#                            app force-loads it, which matches
#                            ios_link.sh linking these as objects (DRC providers and board
#                            generators register through static objects).
#   libcorvcad_kicad_deps.a  wx (base, xml, core), the vcpkg libraries and, when the probe build
#                            has NGSPICE_BUILD, ngspice; linked on demand.
# The engine's route planner is not included: the app compiles it already.
#   ios_package.sh <build-dir> <wx-ios-dir> <ios-deps-installed-dir> [output-dir]
# <wx-ios-dir> holds build-sim/ and build-device/; <ios-deps-installed-dir> holds
# x64-ios-simulator/ (arm64-ios-simulator/ with SIM_ARCH=arm64) and arm64-ios/. Output defaults
# to <build-dir>/package/{sim,device}.
set -eu
B=$(cd "$1" && pwd); WX=$(cd "$2" && pwd); DEPS=$(cd "$3" && pwd); OUT=${4:-$B/package}
HERE=$(cd "$(dirname "$0")" && pwd); K=$(sed -n 's/^KICAD_SOURCE:[A-Z]*=//p' "$B/CMakeCache.txt")
# With NGSPICE_BUILD (the probe build's ngspice_ios.sh work directory), KiCad's simulator is in the
# closure and the deps archive carries that SDK's libngspice.a (built with the same SIM_ARCH).
NG=$(sed -n 's/^NGSPICE_BUILD:[A-Z]*=//p' "$B/CMakeCache.txt")
sed -E 's|^.*integrations/kicad_core/||; s|/|__|g; s|$|.o|' "$B/native.closure.txt" > "$B/ios_native.units"
# TARGETS="device" packages only the arm64 device slice (a device-only release build).
for t in ${TARGETS:-sim device}; do
  case $t in
    sim)    S=;        WXB=$WX/build-sim
            case ${SIM_ARCH:-x86_64} in arm64) PREFIX=$DEPS/arm64-ios-simulator ;; *) PREFIX=$DEPS/x64-ios-simulator ;; esac ;;
    device) S=-device; WXB=$WX/build-device; PREFIX=$DEPS/arm64-ios ;;
  esac
  mkdir -p "$OUT/$t" && rm -f "$OUT/$t"/*.a
  ls "$B/ios_sweep$S/obj" | while read -r o; do
    if grep -qxF "$(echo "$o" | sed -E 's/^CMakeFiles__[a-z_]+\.dir__//')" "$B/ios_native.units" \
       || [ "$o" = session__kicad_session.cpp.o ] || [ "$o" = session__kicad_board.cpp.o ]; then
      echo "$B/ios_sweep$S/obj/$o"
    fi
  done > "$OUT/$t/objects.txt"
  # wx-config lists its own archives by path and the bundled codec/regex/expat ones as -lwx*.
  WXLIBS=$("$WXB/wx-config" --libs base,xml,core | tr ' ' '\n' \
    | sed -nE "s|^(/.*\.a)$|\1|p; s|^-l(wx.*)|$WXB/lib/lib\1.a|p")
  NGLIB=
  if [ -n "$NG" ]; then
    NGLIB=$NG/build-$t/libngspice.a
    [ -f "$NGLIB" ] || { echo "no $NGLIB: run ngspice_ios.sh $NG $t"; exit 1; }
  else
    # spice_simulator.cpp stand-in: without -dead_strip (a debug dylib) KIBIS still references it.
    case $t in sim) TARGET=${SIM_ARCH:-x86_64}-apple-ios17.0-simulator; SDK=iphonesimulator ;; device) TARGET=arm64-apple-ios17.0; SDK=iphoneos ;; esac
    xcrun --sdk $SDK clang++ -std=c++20 -target $TARGET -isysroot "$(xcrun --sdk $SDK --show-sdk-path)" -O2 \
      -I"$K/eeschema/sim" -c "$HERE/../headless/simulator_stub.cpp" -o "$OUT/$t/simulator_stub.o"
    echo "$OUT/$t/simulator_stub.o" >> "$OUT/$t/objects.txt"
  fi
  # shellcheck disable=SC2046
  libtool -static -no_warning_for_no_symbols -o "$OUT/$t/libcorvcad_kicad.a" $(cat "$OUT/$t/objects.txt")
  # shellcheck disable=SC2086
  libtool -static -no_warning_for_no_symbols -o "$OUT/$t/libcorvcad_kicad_deps.a" $WXLIBS "$PREFIX"/lib/*.a $NGLIB 2> "$OUT/$t/deps.log"
  echo "$t: $(wc -l < "$OUT/$t/objects.txt") objects, $(du -h "$OUT/$t"/libcorvcad_kicad.a | cut -f1) + $(du -h "$OUT/$t"/libcorvcad_kicad_deps.a | cut -f1) deps"
done
