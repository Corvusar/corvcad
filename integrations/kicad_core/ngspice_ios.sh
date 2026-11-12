#!/bin/bash
# Build ngspice (version and checksum pinned in upstream.json) as one static library with its
# shared-library API (sharedspice.h) and XSPICE, without OSDI, OpenMP, readline, FFTW or X11, for
# macOS (the desktop check), the iOS simulator or an arm64 iPadOS device. macOS with Xcode.
#   ngspice_ios.sh <work-dir> [mac|sim|device]
# The library lands in <work-dir>/build-<target>/libngspice.a; the API header is
# <work-dir>/ngspice-<version>/src/include/ngspice/sharedspice.h. The XSPICE code models are
# linked in: call corvcad_ngspice_codemodels() (ngspice/static_codemodels.c) after ngSpice_Init.
# "mac" also builds the plain ngspice executable (<work-dir>/build-cli/src/ngspice) with its
# code-model plug-ins, the reference ngspice/check.sh compares with. Build "mac" first: the iOS
# builds run its code-model generator (cmpp). PACK_ONLY=1 reuses the build trees and only
# repacks libngspice.a (after a change to the CorvCAD files compiled into it).
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd); T=${2:-sim}
HERE=$(cd "$(dirname "$0")" && pwd)
LOCK=$HERE/upstream.json
read -r VERSION URL SHA <<< "$(python3 -c "
import json, sys
n = json.load(open(sys.argv[1]))['ngspice']
print(n['version'], n['url'], n['sha256'])" "$LOCK")"
# Only at make time: in configure, its <stdlib.h> would break the function checks (qsort, strtol).
PREFIX="-include $HERE/ngspice/ios_prefix.h"
case $T in
  mac)    CC="xcrun clang -mmacosx-version-min=13.0"; HOST= ;;
  # The simulator slice is x86_64 (Intel Macs) unless SIM_ARCH=arm64 (Apple silicon), as wx_ios.sh.
  sim)    case ${SIM_ARCH:-x86_64} in
            arm64) CC="xcrun --sdk iphonesimulator clang -target arm64-apple-ios17.0-simulator"
                   HOST="--host=aarch64-apple-darwin --build=x86_64-apple-darwin" ;;
            *)     CC="xcrun --sdk iphonesimulator clang -target x86_64-apple-ios17.0-simulator"; HOST=--host=x86_64-apple-darwin ;;
          esac ;;
  device) CC="xcrun --sdk iphoneos clang -target arm64-apple-ios17.0"; HOST="--host=aarch64-apple-darwin --build=x86_64-apple-darwin" ;;
  *) echo "unknown target: $T (mac, sim or device)"; exit 2 ;;
esac
OPTIONS="--disable-osdi --disable-openmp --without-x --with-readline=no --with-fftw3=no --disable-debug"
# The code models the registration file lists, in ngspice's own order (src/xspice/icm/GNUmakefile.in).
CODEMODELS="spice2poly digital analog xtradev xtraevt table tlines"

cd "$W"
if [ ! -d "ngspice-$VERSION" ]; then
  curl -fsSL -o "ngspice-$VERSION.tar.gz" "$URL"
  echo "$SHA  ngspice-$VERSION.tar.gz" | shasum -a 256 -c -
  tar xzf "ngspice-$VERSION.tar.gz"
fi
grep -q "^CMDIRS = $CODEMODELS\$" "ngspice-$VERSION/src/xspice/icm/GNUmakefile.in" \
  || { echo "ngspice's code-model list changed; update CODEMODELS and static_codemodels.c"; exit 1; }
MAKEVARS=()
if [ "$T" != mac ]; then
  # The generator runs on this Mac while it builds: the simulator build would otherwise make an
  # iOS cmpp, which the Mac can't run.
  CMPP=$W/build-mac/src/xspice/cmpp/cmpp
  [ -x "$CMPP" ] || { echo "build mac first: its $CMPP generates the code models"; exit 1; }
  # src/xspice/icm/GNUmakefile names it twice: CMPP generates, cmpp lists the models.
  MAKEVARS=(CMPP="$CMPP" cmpp="$CMPP")
fi
build() {  # build <dir> <configure options...>
  local D=$1; shift
  rm -rf "$D" && mkdir "$D" && cd "$D"
  # shellcheck disable=SC2086
  "../ngspice-$VERSION/configure" "$@" $HOST CC="$CC" CXX="${CC/clang/clang++}" > ../configure-$D.log 2>&1 \
    || { tail -20 ../configure-$D.log; exit 1; }
  make -j"${JOBS:-$(sysctl -n hw.ncpu)}" CPPFLAGS="$PREFIX" ${MAKEVARS[@]+"${MAKEVARS[@]}"} > ../make-$D.log 2>&1 \
    || { tail -20 ../make-$D.log; exit 1; }
  cd ..
}
# shellcheck disable=SC2086
[ -n "${PACK_ONLY:-}" ] || build build-$T --with-ngshared $OPTIONS
# --with-ngshared links only libngspice.dylib. The static library is the same objects (the
# library's own and the convenience archives it is linked from, in the same order), the code
# models' objects without their plug-in glue (dlmain.o, and dstring.o, which the core has), and
# the registration file.
I=build-$T/src/xspice/icm
# Besides its forwarders into the core, dlmain.c defines helpers the models call
# (fopen_with_path, cm_message_printf, cm_is_inertial). They are compiled from the pinned file
# itself: its includes, and everything from fopen_with_path's comment to the end.
DL=ngspice-$VERSION/src/xspice/icm/dlmain.c
H=$(grep -n '^#include "ngspice/inertial.h"$' "$DL" | cut -d: -f1)
S=$(($(grep -n '^fopen_with_path()$' "$DL" | cut -d: -f1) - 1))
[ "$(sed -n "${S}p" "$DL")" = "/*" ] || { echo "dlmain.c changed: update the helper extraction"; exit 1; }
{ sed -n "1,${H}p" "$DL"; sed -n "${S},\$p" "$DL"; } > "build-$T/dlmain_helpers.c"
mkdir -p "build-$T/corvcad/.deps"
for f in "$HERE/ngspice/static_codemodels.c" "$W/build-$T/dlmain_helpers.c"; do
  o=$(basename "$f" .c)
  # shellcheck disable=SC2086
  $CC -O2 $PREFIX -I"$W/build-$T/src/include" -I"$W/ngspice-$VERSION/src/include" -I"$W/$I" \
    -MD -MF "build-$T/corvcad/.deps/$o.Po" -c "$f" -o "build-$T/corvcad/$o.o"
done
# Usable memory for ngspice's memory check. Without the prefix, which would rename the
# host_statistics call it wraps.
# shellcheck disable=SC2086
$CC -O2 -c "$HERE/ngspice/apple_memory.c" -o "build-$T/corvcad/apple_memory.o"
# ngspice's makefile compiles the shared transmission-line files without dependency records;
# ngspice/inventory.py needs them.
mkdir -p "$I/.deps"
for s in tline_common msline_common; do
  # shellcheck disable=SC2086
  $CC -MM -I"$W/ngspice-$VERSION/src/xspice/tlines" -I"$W/build-$T/src/include" -I"$W/ngspice-$VERSION/src/include" \
    "$W/ngspice-$VERSION/src/xspice/tlines/$s.c" > "$I/.deps/$s.P"
done
MODELS=$(for cm in $CODEMODELS; do find "$I/$cm" -mindepth 2 -name cfunc.o -o -mindepth 2 -name ifspec.o -o -mindepth 2 -name udnfunc.o; done | sort)
LIBADD=$(cd build-$T/src && printf 'print:\n\t@echo $(libngspice_la_LIBADD)\n' | make -s -f Makefile -f - print)
# The inputs, one per line, are kept for ngspice/inventory.py.
# shellcheck disable=SC2086
printf '%s\n' build-$T/src/.libs/libngspice_la-*.o \
  $(echo "$LIBADD" | tr ' ' '\n' | grep '\.la$' | sed -E "s|^(.*/)?([^/]+)\.la\$|build-$T/src/\1.libs/\2.a|") \
  $MODELS "$I/tline_common.o" "$I/msline_common.o" build-$T/corvcad/*.o > build-$T/libngspice.inputs
# shellcheck disable=SC2046
libtool -static -no_warning_for_no_symbols -o build-$T/libngspice.a $(cat build-$T/libngspice.inputs)
N=$(echo "$MODELS" | grep -c cfunc.o || true)
echo "code models: $N devices from $CODEMODELS"
[ "$N" -gt 0 ] || { echo "no code models were compiled"; exit 1; }
# shellcheck disable=SC2086
# The reference stays plain ngspice (no prefix); it streams results to its raw file anyway.
[ "$T" != mac ] || [ -n "${PACK_ONLY:-}" ] || PREFIX= build build-cli $OPTIONS
echo "ngspice $VERSION ($T): $W/build-$T/libngspice.a"
