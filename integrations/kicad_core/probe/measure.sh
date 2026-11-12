#!/bin/bash
# Build the candidate objects (keep going past failures), archive what compiled, link each
# probe with a linker map, and report failed sources, unresolved symbols and the measured
# closure.   measure.sh <build-dir>
set -u
B=${1:-build/kicad-probe}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
# Drop objects of sources no longer in the build, or archive() would stage them.
ninja -C "$B" -t cleandead > /dev/null
ninja -C "$B" -k 0 -j "$JOBS" > "$B/compile.log" 2>&1
grep -E '^FAILED: ' "$B/compile.log" | sed -E 's|.*/kicad_(pcb_)?candidates.dir/||; s|\.o$||' | sort -u > "$B/failed_sources.txt"
echo "failed sources:   $(wc -l < "$B/failed_sources.txt")"
# CMake list -> words: "-framework Cocoa" must become two arguments.
FLAGS=$(tr ';' ' ' < "$B/link_flags.txt")

# Archive members are named by basename, and KiCad repeats file names across folders, so
# stage every object under its flattened source path (common__io__x.cpp.o) first.
#   archive <object-library> <stage-dir> <archive>
archive() {
  rm -rf "$B/$2" "$B/$3" && mkdir -p "$B/$2"
  ( cd "$B/CMakeFiles/$1.dir" && find . -name '*.o' | while read -r o; do
      cp "$o" "../../$2/$(echo "${o#./}" | sed -E 's|^.*kicad-core/kicad/||; s|/|__|g')"; done )
  find "$B/$2" -name '*.o' > "$B/$2.list"
  libtool -static -o "$B/$3" -filelist "$B/$2.list" 2> "$B/$3.log"
  echo "$1: $(wc -l < "$B/$2.list") objects compiled"
}

#   link <probe-name> "<probe-object-libraries>" <out-prefix> <archives...>
# SKIP=<regex> leaves matching probe objects out (a second main).
link() {
  local name=$1 libs=$2 out=$3; shift 3
  local probe; probe=$(for lib in $libs; do find "$B/CMakeFiles/$lib.dir" -name '*.o'; done | grep -vE "${SKIP:-^$}")
  [ -n "$probe" ] || { echo "$name did not compile; see $B/compile.log"; return 1; }
  local archives=(); for a in "$@"; do archives+=("$B/$a"); done
  # shellcheck disable=SC2086
  clang++ -o "$B/$name" $probe "${archives[@]}" $FLAGS \
    -framework Cocoa -framework CoreText -framework IOKit -framework SystemConfiguration \
    -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-map,"$B/$out.map" > "$B/$out.link.log" 2>&1
  local status=$?
  grep -E '^  "' "$B/$out.link.log" | sort -u > "$B/$out.unresolved.txt"
  echo "$name: link status $status, unresolved symbols $(wc -l < "$B/$out.unresolved.txt")"
  if [ $status -eq 0 ]; then
    # Object files that contributed to the binary = the measured translation-unit closure.
    LC_ALL=C sed -n '/^# Object files:/,/^# Sections:/p' "$B/$out.map" \
      | grep -oE 'libkicad_[a-z_]+\.a\([^)]+\.o\)|/stage[a-z_]*/[^ ]+\.o' \
      | sed -E 's|libkicad_[a-z_]+\.a\(||; s|^/stage[a-z_]*/||; s|\.o\)?$||; s|__|/|g' | sort -u > "$B/$out.closure.txt"
    echo "$name: closure $(wc -l < "$B/$out.closure.txt") translation units"
  fi
}

archive kicad_candidates stage libkicad_slice.a
archive kicad_pcb_candidates stage_pcb libkicad_pcb.a
archive kicad_headless stage_headless libkicad_headless.a
archive kicad_pcb_headless stage_pcb_headless libkicad_pcb_headless.a

#   gui_report <probe-name> <closure-file>: editor translation units left in a closure.
gui_report() {
  [ -f "$2" ] || return
  python3 - "$(dirname "$0")" "$1" "$2" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import closure
units = [line.strip() for line in open(sys.argv[3]) if line.strip()]
gui = [u for u in units if "headless" not in u and closure.GUI.search(u.replace("/", "__"))]
print(f"{sys.argv[2]}: {len(gui)} GUI translation units" + "".join("\n  " + u for u in gui[:40]))
PY
  # With -dead_strip_dylibs only libraries something still calls stay linked.
  echo "$1: wx libraries $(otool -L "$B/$1" | grep -oE 'libwx_[a-z_]+' | sort -u | tr '\n' ' ')"
}

link corvcad_kicad_probe probe_main probe libkicad_slice.a
# DRC test providers register themselves through static objects nothing references, so an
# archive would drop them (2 of 28 linked); KiCad links every pcbnew object into its kiface.
# Pass them as objects, which the linker always loads.
# Board generators (tuning patterns) register the same way; without them a board that has one
# fails to load ("Cannot create generated object of type 'tuning_pattern'"). Their editor
# halves (patch 0007) stay in the archive, so only a probe that needs them loads them.
DRC_PROVIDERS=$(cd "$B" && ls stage_pcb/*drc_test_provider_*.o stage_pcb/pcbnew__generators__*.o | grep -v '_edit\.cpp\.o$')
# shellcheck disable=SC2086
link corvcad_kicad_pcb_probe pcb_probe_main pcb_probe $DRC_PROVIDERS libkicad_pcb.a libkicad_slice.a
# Same probe with the headless replacements first: ld takes each symbol from the first
# archive that defines it, so editor TUs load only if something still needs them.
link corvcad_kicad_probe_headless probe_main probe_headless libkicad_headless.a libkicad_slice.a
gui_report corvcad_kicad_probe_headless "$B/probe_headless.closure.txt"
# The board probe with the board and common headless replacements first (decoupling in progress).
# shellcheck disable=SC2086
link corvcad_kicad_pcb_probe_headless pcb_probe_main pcb_probe_headless $DRC_PROVIDERS \
  libkicad_pcb_headless.a libkicad_headless.a libkicad_pcb.a libkicad_slice.a
gui_report corvcad_kicad_pcb_probe_headless "$B/pcb_probe_headless.closure.txt"
# Gate 3: the native session with session/abi_test.c; session/check.sh runs it.
link corvcad_kicad_session session_main session libkicad_headless.a libkicad_slice.a
# Gate 5: what the NATIVE_KICAD app links, the session and the board checks (kicad_board.cpp,
# with session/board_test.c as main) over both slices. Its closure is what ios_package.sh
# packages; probe/native_link.sh then links that closure as objects, as the app force-loads it.
# shellcheck disable=SC2086
SKIP='/abi_test\.c\.o$' link corvcad_kicad_native_measure "board_main session_main" native $DRC_PROVIDERS \
  libkicad_pcb_headless.a libkicad_headless.a libkicad_pcb.a libkicad_slice.a
gui_report corvcad_kicad_native_measure "$B/native.closure.txt"
