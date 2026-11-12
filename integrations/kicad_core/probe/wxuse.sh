#!/bin/bash
# Which wx GUI libraries does each translation unit of a probe's closure still need?
# Prints "<unit>: <lib>(<n>) ..." with the classes it uses, so the iPad slice can be cut to
# wxBase.   wxuse.sh <build-dir> [closure-file]
set -u
B=${1:-build/kicad-probe}
CLOSURE=${2:-$B/probe_headless.closure.txt}
WXLIB=${WXLIB:-$(wx-config --prefix 2>/dev/null || echo /usr/local)/lib}
T=$(mktemp -d)
# Exported symbols of every non-base wx library, tagged with the library name.
for f in "$WXLIB"/libwx_osx_cocoau_*-3.*.dylib; do
  lib=$(basename "$f" | sed -E 's/libwx_osx_cocoau_([a-z]+)-.*/\1/')
  nm -gU "$f" | awk -v l="$lib" '{print $3 "\t" l}'
done | sort -u -k1,1 > "$T/gui.tsv"
while read -r unit; do
  m="$B/stage/$(echo "$unit" | sed 's|/|__|g').o"
  [ -f "$m" ] || m=$(ls "$B"/stage_headless/*"$(basename "$unit")".o 2>/dev/null | head -1)
  [ -f "$m" ] || continue
  nm -u "$m" | awk '{print $NF}' | sort -u | join -t $'\t' - "$T/gui.tsv" > "$T/hits.tsv"
  [ -s "$T/hits.tsv" ] || continue
  libs=$(cut -f2 "$T/hits.tsv" | sort | uniq -c | awk '{printf "%s(%s) ", $2, $1}')
  classes=$(cut -f1 "$T/hits.tsv" | c++filt | sed -E 's/^(typeinfo for |vtable for )//; s/(::|\().*//' | sort -u | head -12 | tr '\n' ' ')
  echo "$unit: $libs| $classes"
done < "$CLOSURE"
rm -rf "$T"
