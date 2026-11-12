#!/bin/bash
# Desktop proof of the NATIVE_KICAD link (port gate 5): the schematic session, the board checks
# and their measured closure (native.closure.txt, from measure.sh) linked as objects, as the app
# force-loads them, so duplicate symbols between the schematic and board slices or a lost
# static registration show here. The program's main is session/board_test.c;
# session/board_check.py runs it. Builds only those objects when they are missing.
#   [JOBS=n] native_link.sh <build-dir>
set -eu
B=$(cd "$1" && pwd); JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
cd "$B"
ninja -t targets all | grep -oE '^[^:]*\.o' | sort -u > native.targets
# One object per unit (ios_sweep.sh's matching): KiCad and generated sources by path, CorvCAD
# replacements by headless/[pcbnew/]<name>.
while read -r unit; do
  grep -m1 -F "/$unit.o" native.targets || grep -m1 -E "headless/(pcbnew/)?${unit##*/}\.o" native.targets \
    || { echo "no object for $unit" >&2; exit 1; }
done < native.closure.txt > native.objects
grep -E '^CMakeFiles/(session|board)_main\.dir/' native.targets | grep -v '/abi_test\.c\.o$' >> native.objects
echo "objects: $(wc -l < native.objects)"
# shellcheck disable=SC2046
ninja -j "$JOBS" $(cat native.objects) > native.compile.log 2>&1 || { grep -m20 -E 'error:|FAILED' native.compile.log; exit 1; }
FLAGS=$(tr ';' ' ' < link_flags.txt)
# shellcheck disable=SC2046,SC2086
clang++ -o corvcad_kicad_native $(cat native.objects) $FLAGS \
  -framework Cocoa -framework CoreText -framework IOKit -framework SystemConfiguration \
  -Wl,-dead_strip > native.link.log 2>&1 || { grep -m20 -E 'duplicate symbol|Undefined|error' -A3 native.link.log; exit 1; }
ls -l corvcad_kicad_native | awk '{print "corvcad_kicad_native: " $5 " bytes"}'
