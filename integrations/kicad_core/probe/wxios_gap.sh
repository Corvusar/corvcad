#!/bin/bash
# wx symbols the headless closure needs that a wxWidgets iOS build does not define, per unit.
#   wxios_gap.sh <build-dir> <wx-ios-lib-dir>
set -u
B=$1; L=$2
T=$(mktemp -d)
for f in "$L"/libwx_baseu*.a "$L"/libwx_osx_iphoneu_core*.a; do nm -gU "$f" 2>/dev/null | awk 'NF==3 {print $3}'; done | sort -u > "$T/ios.txt"
# Everything any member of the closure (or a headless replacement) defines itself.
while read -r u; do
  m="$B/stage/$(echo "$u" | sed 's|/|__|g').o"; [ -f "$m" ] || m=$(ls "$B"/stage_headless/*"$(basename "$u")".o 2>/dev/null | head -1)
  [ -f "$m" ] && echo "$m"
done < "$B/probe_headless.closure.txt" > "$T/members.txt"
xargs nm -gU < "$T/members.txt" 2>/dev/null | awk 'NF==3 {print $3}' | sort -u > "$T/own.txt"
while read -r m; do
  nm -u "$m" | awk '{print $NF}' | grep -E 'wx|WX' | sort -u | comm -23 - "$T/ios.txt" | comm -23 - "$T/own.txt" > "$T/miss.txt"
  [ -s "$T/miss.txt" ] || continue
  echo "$(basename "$m" .o | sed 's|__|/|g'): $(wc -l < "$T/miss.txt" | tr -d ' ') | $(c++filt < "$T/miss.txt" | sed -E 's/^(typeinfo for |vtable for |non-virtual thunk to )//; s/(::|\().*//' | sort -u | head -10 | tr '\n' ' ')"
done < "$T/members.txt"
rm -rf "$T"
