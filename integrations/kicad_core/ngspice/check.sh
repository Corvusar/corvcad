#!/bin/bash
# SPICE phase S1 check: KiCad 10.0.6's simulation demos, exported to SPICE netlists by kicad-cli,
# run by the ngspice executable (the reference) and by probe.c linked with the static library
# from ngspice_ios.sh, on macOS and in the iOS simulator; compare.py compares the result vectors.
# Both are built with the same options (XSPICE; no OSDI or ADMS), so a demo that needs those
# fails in both and is reported as such, not as a mismatch. The executable loads the code models
# as plug-ins (.cm), as desktop KiCad does; the library has them linked in.
#   [ONLY=<demo-name regex>] [TARGETS="mac sim"] [SESSION=<native program>] check.sh <ngspice-work-dir> <kicad-source> <out-dir> [simulator]
# SESSION (phase S2): also runs each demo through KiCad's simulator in the native session
# (probe/native_link.sh's corvcad_kicad_native simulate, cc_sch_session_simulate) on a copy of
# its project, against the same reference. TARGETS picks the probe runs (empty: none). KEEP=1
# keeps the results (normally deleted after each demo).
# <ngspice-work-dir> must hold ngspice_ios.sh's mac and sim builds. Raw files are deleted after
# each demo (one demo's reference is over 2 GB); logs stay in <out-dir>/<demo>.
set -u
N=$(cd "$1" && pwd); K=$(cd "$2" && pwd); O=$(mkdir -p "$3" && cd "$3" && pwd)
SIM=${4:-CorvCAD iPad Pro 11}
HERE=$(cd "$(dirname "$0")" && pwd)
CLI=${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
INC=$(ls -d "$N"/ngspice-*/src/include)
REF=$N/build-cli/src/ngspice
# SETUP, per demo below: the project's compatibility mode, set before the netlist loads.
# On Apple platforms ngspice compares a result's size with the free memory pages only, which
# swing with the system's caches, so the same demo runs or is refused from one run to the next.
# This check is about the results; the app's memory limit is S2's (os_proc_available_memory).
MEMCHECK="set no_mem_check"
LARGE=1000000000  # ASCII raw files above this are not parsed here (compare.py holds them in memory)

xcrun clang -O2 -I"$INC" -o "$O/probe-mac" "$HERE/probe.c" "$N/build-mac/libngspice.a" -lc++ || exit 1
xcrun --sdk iphonesimulator clang -O2 -target x86_64-apple-ios17.0-simulator -I"$INC" \
  -o "$O/probe-sim" "$HERE/probe.c" "$N/build-sim/libngspice.a" -lc++ || exit 1
# The device build only links here: running it needs a signed app on an iPad (phase S4).
if [ -f "$N/build-device/libngspice.a" ]; then
  xcrun --sdk iphoneos clang -O2 -target arm64-apple-ios17.0 -I"$INC" \
    -o "$O/probe-device" "$HERE/probe.c" "$N/build-device/libngspice.a" -lc++ || exit 1
  echo "device probe links: $(lipo -archs "$O/probe-device"), $(stat -f %z "$O/probe-device") bytes"
fi
xcrun simctl boot "$SIM" 2> /dev/null

pass=0; fail=0; both=0; kicad=0; memory=0; sensitive=0
while read -r pro; do
  sch=${pro%.kicad_pro}.kicad_sch
  name=$(basename "${pro%.kicad_pro}")
  [[ $name =~ ${ONLY:-.} ]] || continue
  d=$O/$name; rm -rf "$d"; mkdir -p "$d"
  # Code models open data files relative to the netlist (d_state's state file, say).
  find "$(dirname "$pro")" -maxdepth 1 -type f ! -name '*.kicad_*' ! -name '*.wbk' ! -name sym-lib-table \
    -exec cp {} "$d/" \;
  "$CLI" sch export netlist --format spice -o "$d/$name.cir" "$sch" > "$d/export.log" 2>&1 \
    || { echo "$name: kicad-cli cannot export it: $(head -1 "$d/export.log")"; kicad=$((kicad+1)); continue; }
  # Most demos keep their analysis in the simulator workbook (.wbk), not the schematic: add its
  # first tab's commands before .end, as KiCad's simulator does for that tab.
  analysis='^\.(op|dc|ac|tran|noise|tf|pz|sp|disto)( |$)'
  wbk=${pro%.kicad_pro}.wbk
  if [ -f "$wbk" ]; then
    python3 -c '
import json, re, sys
cir, wbk, pattern = sys.argv[1:]
tabs = json.load(open(wbk)).get("tabs", [])
commands = tabs[0]["commands"] if tabs else []
text = open(cir).read()
if not re.search(pattern, text, re.M | re.I):
    lines = [c for c in commands if not c.startswith(".kicad")]
    open(cir, "w").write(text.replace("\n.end\n", "\n" + "".join(l + "\n" for l in lines) + ".end\n"))
# The session gets the tab as KiCad stores it, with its option lines.
open(cir[:-4] + ".command", "w").write("\n".join(commands))' "$d/$name.cir" "$wbk" "$analysis"
  fi
  # The project's compatibility mode (NGSPICE_COMPATIBILITY_MODE, as NGSPICE::GetSettingCommands
  # sends it); KiCad's default is LTspice and PSpice.
  SETUP=$(python3 -c '
import json, sys
mode = json.load(open(sys.argv[1])).get("schematic", {}).get("ngspice", {}).get("model_mode", 4)
print({1: "unset ngbehavior", 2: "set ngbehavior=psa", 3: "set ngbehavior=lta",
       4: "set ngbehavior=ltpsa", 5: "set ngbehavior=hsa"}.get(mode, "echo user configuration"))' "$pro")
  grep -qiE "$analysis" "$d/$name.cir" \
    || { echo "$name: no analysis in kicad-cli's export or the workbook"; kicad=$((kicad+1)); continue; }
  # The executable reads its setup from .spiceinit in its working directory.
  { echo "$SETUP"; for cm in "$N"/build-cli/src/xspice/icm/*/*.cm; do echo "codemodel $cm"; done; } > "$d/.spiceinit"
  ( cd "$d" && SPICE_ASCIIRAWFILE=1 "$REF" -b -r "$d/ref.raw" "$d/$name.cir" > "$d/ref.log" 2>&1 )
  r=$?; [ -s "$d/ref.raw" ] || r=1
  # ngspice seeds noise sources from the process id: this demo's noise nets, and the currents
  # and power of the sources and resistors on them (V8, V9, I8, I9; R8, R9, R17, R18).
  ignore=(); [ "$name" != v_i_sources ] || ignore=(--ignore 'trnoise|trrandom|^(v8|v9|i8|i9|r8|r9|r17|r18)[#:]')
  # power_supply: the transformer secondary's reference node is tied to ground only through
  # R3 = 1 GOhm, so ngspice resolves it to about ABSTOL x 1 GOhm = 1 mV; it carries 0.2 mV of
  # rounding noise in one build and 0 V in another. That node and R3's current are left out.
  [ "$name" != power_supply ] || ignore=(--ignore '^(net-_t1-ab_|r3#branch)$')
  if [ $r -eq 0 ] && [ "$(stat -f %z "$d/ref.raw")" -gt $LARGE ]; then
    echo "$name: result too large for this check: $(grep -m1 'No. Points' "$d/ref.raw" | tr -s ' ') of $(grep -m1 'No. Variables' "$d/ref.raw" | sed 's/.*: *//') vectors"
    memory=$((memory+1)); rm -f "$d"/*.raw "$d/session.txt"; continue
  fi
  for t in ${TARGETS-mac sim} ${SESSION:+session}; do
    # The reference: the executable on kicad-cli's netlist, run in the demo's folder.
    ref=$d/ref.raw; reflog=$d/ref.log; cir=$d/$name.cir; rundir=$d; rr=$r; tag=
    case $t in
      mac) "$O/probe-mac" "$d/$name.cir" "$d/$t" "$SETUP" "$MEMCHECK" > "$d/$t.log" 2> "$d/$t.err" ;;
      sim) xcrun simctl spawn "$SIM" "$O/probe-sim" "$d/$name.cir" "$d/$t" "$SETUP" "$MEMCHECK" > "$d/$t.log" 2> "$d/$t.err" ;;
      # KiCad's simulator through the native session on a copy of the demo's project, with its
      # workbook's first tab (analysis and options) when it has one. KiCad's own netlist can
      # differ from kicad-cli's (the tab's current and power probes), so the session's reference
      # is the executable on the session's netlist (cc_sch_session_spice_netlist).
      session)
        rm -rf "$d/project"; cp -R "$(dirname "$pro")" "$d/project"; cp "$d/.spiceinit" "$d/project/"
        command=$(cat "$d/$name.command" 2>/dev/null)
        "$SESSION" sim-netlist "$d/project/$name.kicad_pro" "$d/session.cir" "$command" > /dev/null 2> "$d/session-netlist.err"
        ( cd "$d/project" && SPICE_ASCIIRAWFILE=1 "$REF" -b -r "$d/session-ref.raw" "$d/session.cir" > "$d/session-ref.log" 2>&1 )
        rr=$?; [ -s "$d/session-ref.raw" ] || rr=1
        ref=$d/session-ref.raw; reflog=$d/session-ref.log; cir=$d/session.cir; rundir=$d/project; tag=-session
        "$SESSION" simulate "$d/project/$name.kicad_pro" "$d/session.txt" "$command" > "$d/$t.log" 2> "$d/$t.err" ;;
    esac
    p=$?
    if [ "$t" = session ]; then
      results=("$d/session.txt")
      # For information: the session against kicad-cli's netlist.
      [ $p -ne 0 ] || [ $r -ne 0 ] \
        || echo "  $name (session against kicad-cli's netlist): $(python3 "$HERE/compare.py" ${ignore[@]+"${ignore[@]}"} "$d/ref.raw" "${results[@]}")"
    else
      results=("$d/$t".*.raw)
    fi
    if [ $rr -ne 0 ] && [ $p -ne 0 ]; then
      echo "$name ($t): fails in both: $(grep -m1 -E '^Error' "$reflog")"
      both=$((both+1))
    elif [ $rr -ne 0 ] || [ $p -ne 0 ]; then
      echo "$name ($t): reference $rr, probe $p"; fail=$((fail+1))
    elif result=$(python3 "$HERE/compare.py" ${ignore[@]+"${ignore[@]}"} "$ref" "${results[@]}"); then
      echo "$name ($t): $result, $(cat "$d/$t.log")"; pass=$((pass+1))
    elif [[ $result == *interpolated* ]]; then
      # A long switching run on another time-step path. Control: how far the executable drifts
      # from itself with RELTOL nudged by 0.1%. Within 1.5 times that, the difference is the
      # circuit's own sensitivity to rounding; it is counted apart, not as agreement.
      if [ ! -s "$d/control$tag.txt" ]; then
        sed 's/^\.end$/.options reltol=0.000999\
.end/' "$cir" > "$d/nudged$tag.cir"
        ( cd "$rundir" && SPICE_ASCIIRAWFILE=1 "$REF" -b -r "$d/nudged$tag.raw" "$d/nudged$tag.cir" > "$d/nudged$tag.log" 2>&1 )
        python3 "$HERE/compare.py" ${ignore[@]+"${ignore[@]}"} "$ref" "$d/nudged$tag.raw" > "$d/control$tag.txt"
      fi
      rms() { sed -E 's/.*largest RMS ([^,]+).*/\1/' <<< "$1"; }
      control=$(rms "$(cat "$d/control$tag.txt")"); own=$(rms "$result")
      if python3 -c "import sys; sys.exit(not float('$own') <= 1.5 * float('$control'))"; then
        echo "$name ($t): within the circuit's own sensitivity: RMS $own against $control for the executable with RELTOL nudged by 0.1%"
        sensitive=$((sensitive+1))
      else
        echo "$name ($t): $result; the executable with RELTOL nudged by 0.1%: RMS $control"; fail=$((fail+1))
      fi
    else
      echo "$name ($t): $result"; fail=$((fail+1))
    fi
  done
  [ -n "${KEEP:-}" ] || rm -f "$d"/*.raw "$d/session.txt"
done < <(find "$K/demos/simulation" -name '*.kicad_pro' | sort)
echo "ngspice check: $pass agree, $sensitive within the circuit's own sensitivity, $fail differ, $both fail in both builds, $memory too large for this check, $kicad not runnable from kicad-cli's export"
[ $fail -eq 0 ]
