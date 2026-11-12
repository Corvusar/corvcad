#!/bin/bash
# Compile every translation unit of the headless closure for the iOS simulator against a
# wxWidgets iOS build, using the probe's own compile commands, and group failures by their
# first error. Third-party headers come from <ios-deps-prefix> (ios_deps.sh) when given, and
# every desktop include path is dropped; without it they come from Homebrew, which is enough
# to see what fails in KiCad or wx-iOS. The probe's main() and the native session (gate 3)
# are compiled too, for ios_link.sh, and the native board checks (gate 5) for ios_package.sh.
#   [IOS_TARGET=sim|device] [SIM_ARCH=x86_64|arm64] ios_sweep.sh <build-dir> <wx-ios-build-dir> [jobs] [ios-deps-prefix]
# Objects land in <build-dir>/ios_sweep (simulator, x86_64 unless SIM_ARCH=arm64) or
# <build-dir>/ios_sweep-device (arm64).
set -u
B=$(cd "$1" && pwd); WXB=$(cd "$2" && pwd); JOBS=${3:-$(sysctl -n hw.ncpu)}; PREFIX=${4:+$(cd "$4" && pwd)}
STANDIN=$(cd "$(dirname "$0")/../headless/ios" && pwd)
case ${IOS_TARGET:-sim} in
  sim)    SDKNAME=iphonesimulator; TARGET=${SIM_ARCH:-x86_64}-apple-ios17.0-simulator; OUT=$B/ios_sweep ;;
  device) SDKNAME=iphoneos;        TARGET=arm64-apple-ios17.0;            OUT=$B/ios_sweep-device ;;
  *) echo "unknown IOS_TARGET: $IOS_TARGET (sim or device)"; exit 2 ;;
esac
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
# ONLY=<regex> recompiles just the matching objects of an existing sweep (for example
# ONLY=kicad_session after a session edit) and keeps the rest.
if [ -z "${ONLY:-}" ]; then rm -rf "$OUT"; else rm -f "$OUT/failed.txt"; fi
mkdir -p "$OUT/obj" "$OUT/log"
if [ -n "$PREFIX" ]; then
  # KiCad includes <freetype2/ft2build.h>, Homebrew's layout; vcpkg puts ft2build.h at the root.
  mkdir -p "$OUT/extra" && ln -s "$PREFIX/include" "$OUT/extra/freetype2"
  DEPS="-I$PREFIX/include -I$OUT/extra"
  # The IPC API code must come from the protoc matching the iOS libprotobuf (vcpkg's host tool).
  K=$(sed -n 's/^KICAD_SOURCE:[A-Z]*=//p' "$B/CMakeCache.txt")
  mkdir -p "$OUT/gen/cpp/api"
  ( cd "$B/generated/cpp/api" && find . -name '*.pb.cc' | sed 's|^\./||; s|\.pb\.cc$|.proto|' ) \
    | xargs "$(ls -d "$PREFIX"/../*-osx | head -1)/tools/protobuf/protoc" --cpp_out="$OUT/gen/cpp/api" --proto_path="$K/api/proto"
  STRIP="s# -(I|isystem ?)(/usr/local|/opt/homebrew|/Library/Frameworks/Python)[^ ]*##g; s#$B/generated/cpp#$OUT/gen/cpp#g"
else
  DEPS="$(pkg-config --cflags freetype2 harfbuzz fontconfig protobuf libzstd cairo pixman-1 2>/dev/null) -I/usr/local/opt/glm/include -I/usr/local/opt/boost/include"
  # FreeType and HarfBuzz are included as <freetype2/...> and <harfbuzz/...>, so their parents too.
  DEPS="$DEPS -I$(pkg-config --variable=includedir freetype2) -I$(pkg-config --variable=includedir harfbuzz)"
  STRIP='s#^##'
fi
EXTRA="$("$WXB/wx-config" --cxxflags) $DEPS -isysroot $SDK -target $TARGET"

cd "$B"
ninja -t targets all | grep -oE '^[^:]*(kicad_candidates|kicad_headless|kicad_pcb_candidates|kicad_pcb_headless|probe_main|pcb_probe_main|session_main|board_main)\.dir/[^:]*\.o' | sort -u > "$OUT/objects.txt"
# The headless board closure (decoupled from the editor) and the native (session plus board
# checks) closure are swept with the schematic one.
cat "$B/probe_headless.closure.txt" "$B/session.closure.txt" "$B/pcb_probe_headless.closure.txt" "$B/native.closure.txt" 2> /dev/null | sort -u | while read -r unit; do
  key=$(echo "$unit" | sed 's|^.*/||')
  grep -F "/$unit.o" "$OUT/objects.txt" || grep -E "headless/(pcbnew/)?$key\.o" "$OUT/objects.txt"
done | sort -u > "$OUT/todo.txt"
grep -E '^CMakeFiles/(probe|pcb_probe|session|board)_main\.dir/' "$OUT/objects.txt" >> "$OUT/todo.txt"
[ -z "${ONLY:-}" ] || { grep -E "$ONLY" "$OUT/todo.txt" > "$OUT/todo.only"; mv "$OUT/todo.only" "$OUT/todo.txt"; }

compile() {
  obj=$1; name=$(echo "$obj" | sed -E 's|^.*kicad-core/kicad/||; s|^.*integrations/kicad_core/||; s|/|__|g')
  cmd=$(ninja -t commands "$obj" | tail -1 | sed -E \
    's# -I/usr/local/lib/wx/include/[^ ]*##g; s# -I/usr/local/include/wx-3\.[0-9]+##g; s# -D__WXMAC__##g; s# -D__WXOSX__##g; s# -D__WXOSX_COCOA__##g; s# -MD -MT [^ ]* -MF [^ ]*##; s# -o [^ ]*##' \
    | sed -E "$STRIP")
  # iOS stand-ins for editor-only headers go first on the include path.
  cmd=$(echo "$cmd" | sed -E "s#^([^ ]+) #\1 -I$STANDIN #")
  eval "$cmd $EXTRA -o $OUT/obj/$name" > "$OUT/log/$name.log" 2>&1 || echo "$name" >> "$OUT/failed.txt"
}
export -f compile; export OUT EXTRA STANDIN STRIP
xargs -P "$JOBS" -I{} bash -c 'compile "$@"' _ {} < "$OUT/todo.txt"

echo "units: $(wc -l < "$OUT/todo.txt"), failed: $( [ -f "$OUT/failed.txt" ] && wc -l < "$OUT/failed.txt" || echo 0)"
[ -f "$OUT/failed.txt" ] || exit 0
# First error per failing unit, grouped by the header or file it points at.
for f in $(sort "$OUT/failed.txt"); do
  grep -m1 'error:' "$OUT/log/$f.log" | sed -E 's|^.*kicad-core/kicad/||; s|^/[^:]*/||' | cut -c1-150 | sed "s|^|$f: |"
done > "$OUT/first_errors.txt"
sed -E 's/^[^:]*: ([^:]*):.*/\1/' "$OUT/first_errors.txt" | sort | uniq -c | sort -rn | head -30
