#!/bin/bash
# Link the iOS objects from ios_sweep.sh (headless schematic closure plus the probe's main) into
# an iOS simulator executable, plus a wrapper that runs it in a booted simulator through
# simctl spawn, so compare.sh can drive it like the desktop probe.
#   [IOS_TARGET=sim|device] [PROGRAM=probe|session] ios_link.sh <build-dir> <wx-ios-build-dir> <ios-deps-prefix> [simulator]
# IOS_TARGET=device links the arm64 objects (ios_sweep-device) into corvcad_kicad_probe_ios-device
# and stops there: running it needs a signed app on a physical iPad.
# PROGRAM=session links the native session with abi_test.c (gate 3) instead of the probe, for
# session/check.sh. PROGRAM=pcb_probe links the headless board probe, for compare_pcb.sh.
# PROGRAM=native links the app's native closure with session/board_test.c (ngspice/check.sh's
# SESSION in the simulator).
set -u
B=$(cd "$1" && pwd); WXB=$(cd "$2" && pwd); PREFIX=$(cd "$3" && pwd); shift 3
case ${IOS_TARGET:-sim} in
  sim)    SDKNAME=iphonesimulator; TARGET=${SIM_ARCH:-x86_64}-apple-ios17.0-simulator; S= ;;
  device) SDKNAME=iphoneos;        TARGET=arm64-apple-ios17.0;            S=-device ;;
  *) echo "unknown IOS_TARGET: $IOS_TARGET (sim or device)"; exit 2 ;;
esac
# ios_sweep.sh sweeps the union of the closures and names objects by source path
# (pcbnew__board.cpp.o, headless__confirm.cpp.o). Each program links the objects of its own
# measured desktop closure plus its main: linked as objects, others' static initialisers
# (board DRC providers in the schematic probe, say) would stay.
case ${PROGRAM:-probe} in
  probe)     CLOSURE=probe_headless;     MAIN='^CMakeFiles__probe_main\.' ;;
  session)   CLOSURE=session;            MAIN='^(session__(kicad_session|abi_test)\.|CMakeFiles__session_main\.)' ;;
  pcb_probe) CLOSURE=pcb_probe_headless; MAIN='^CMakeFiles__pcb_probe_main\.' ;;
  # What the NATIVE_KICAD app links (the session and the board checks) with session/board_test.c
  # as main, as native_link.sh links it on the desktop.
  native)    CLOSURE=native;             MAIN='^(session__(kicad_session|kicad_board|board_test)\.|CMakeFiles__session_main\.)' ;;
  *) echo "unknown PROGRAM: $PROGRAM (probe, session, pcb_probe or native)"; exit 2 ;;
esac
sed -E 's|^.*integrations/kicad_core/||; s|/|__|g; s|$|.o|' "$B/$CLOSURE.closure.txt" > "$B/ios_$CLOSURE.units"
objects() {
  # Generated sources are named CMakeFiles__<library>.dir__generated__...; the closure has generated/...
  ls "$B"/ios_sweep$S/obj/ | while read -r o; do
    if grep -qxF "$(echo "$o" | sed -E 's/^CMakeFiles__[a-z_]+\.dir__//')" "$B/ios_$CLOSURE.units" \
       || echo "$o" | grep -qE "$MAIN"; then
      echo "$B/ios_sweep$S/obj/$o"
    fi
  done
}
N=${PROGRAM:-probe}_ios$S
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
EXE=$B/corvcad_kicad_$N
# KiCad's simulator, when the probe build has NGSPICE_BUILD (ngspice_ios.sh's libraries).
NG=$(sed -n 's/^NGSPICE_BUILD:[A-Z]*=//p' "$B/CMakeCache.txt")
NGLIB=; [ -z "$NG" ] || NGLIB=$NG/build-${IOS_TARGET:-sim}/libngspice.a
# shellcheck disable=SC2046,SC2086
xcrun --sdk $SDKNAME clang++ -target $TARGET -isysroot "$SDK" \
  -o "$EXE" $(objects) \
  $("$WXB/wx-config" --libs base,xml,core) "$PREFIX"/lib/*.a $NGLIB \
  -framework Foundation -framework CoreText -framework CFNetwork -framework Security \
  -Wl,-dead_strip -Wl,-map,"$B/$N.map" > "$B/$N.link.log" 2>&1
status=$?
grep -E '^  "' "$B/$N.link.log" | sort -u > "$B/$N.unresolved.txt"
echo "corvcad_kicad_$N: link status $status, unresolved symbols $(wc -l < "$B/$N.unresolved.txt")"
[ $status -eq 0 ] || { grep -m5 -E 'error|ld:' "$B/$N.link.log"; exit 1; }
ls -l "$EXE" | awk '{print "size: " $5 " bytes"}'
[ -z "$S" ] || { lipo -info "$EXE"; exit 0; }
SIM=${1:-CorvCAD iPad Pro 11}
xcrun simctl boot "$SIM" 2> /dev/null
printf '#!/bin/bash\nexec xcrun simctl spawn "%s" "%s" "$@"\n' "$SIM" "$EXE" > "$EXE.sim"
chmod +x "$EXE.sim"
echo "run in $SIM: $EXE.sim <args>"
