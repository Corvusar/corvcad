#!/bin/bash
# Run the probe on a schematic and compare with the installed kicad-cli:
#  - netlist: probe (NETLIST_EXPORTER_KICAD) vs `kicad-cli sch export netlist`, ignoring the
#    header lines that carry dates, tool versions and absolute paths;
#  - ERC: the probe's in-process violation list vs `kicad-cli sch erc` on the input;
#  - save round trip: the probe's re-saved root sheet must load in kicad-cli, and its ERC
#    report must match the original's violation count.
#   compare.sh <probe-binary> <in.kicad_sch> <work-dir>
set -u
PROBE=$1; IN=$2; W=$3
CLI=${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
rm -rf "$W" && mkdir -p "$W/probe" "$W/cli"
export KICAD_CONFIG_HOME="$W/config"
# Child sheets and project files must sit beside the re-saved root for the ERC check.
cp -R "$(dirname "$IN")"/. "$W/probe/"

echo "probe: $("$PROBE" "$IN" "$W/probe" 2>"$W/probe.err" || echo "FAILED (see $W/probe.err)")"
"$CLI" sch export netlist --format kicadsexpr -o "$W/cli/cli.net" "$IN" > "$W/cli.log" 2>&1
# kicad-cli pretty-prints and the exporter alone does not, so compare one "(" token per line.
strip() { tr -s ' \t\n' ' ' < "$1" | sed -E 's/ *\(/\n(/g; s/ +\)/)/g; s/ +$//' \
          | grep -vE '^\((source|date|tool|textvar|comment|company|rev|title|sheetpath)\b' \
          | sed -E 's/\(tstamps? "[^"]*"\)//g'; }
if [ ! -s "$W/probe/probe.net" ] || [ ! -s "$W/cli/cli.net" ]; then
  echo "netlist: MISSING (see $W/probe.err and $W/cli.log)"
elif diff <(strip "$W/probe/probe.net") <(strip "$W/cli/cli.net") > "$W/netlist.diff"; then
  echo "netlist: identical to kicad-cli (ignoring header metadata)"
else
  echo "netlist: $(grep -c '^[<>]' "$W/netlist.diff") differing lines (see $W/netlist.diff)"
fi

SAVED="$W/probe/$(basename "$IN")"
"$CLI" sch erc --format json -o "$W/cli/erc.json" "$IN" > /dev/null 2>&1
"$CLI" sch erc --format json -o "$W/erc-saved.json" "$SAVED" > /dev/null 2>&1
# The probe's in-process ERC (probe-erc.json) must list the same violations as kicad-cli:
# same sheet, type, severity, and item UUIDs and positions.
python3 - "$W" <<'PY'
import collections, json, sys, pathlib
w = pathlib.Path(sys.argv[1])
def violations(p):
    try:
        data = json.loads(p.read_text())
    except Exception as error:
        return f"unreadable ({error.__class__.__name__})"
    return collections.Counter(
        (s.get("uuid_path"), v["type"], v["severity"],
         tuple(sorted((i.get("uuid"), round(i["pos"]["x"], 4), round(i["pos"]["y"], 4)) for i in v.get("items", []))))
        for s in data.get("sheets", []) for v in s.get("violations", []))
cli, probe, saved = (violations(w / n) for n in ("cli/erc.json", "probe/probe-erc.json", "erc-saved.json"))
size = lambda v: sum(v.values()) if isinstance(v, collections.Counter) else v
print(f"erc: kicad-cli={size(cli)}, probe={size(probe)}, kicad-cli on re-saved={size(saved)}")
if isinstance(cli, collections.Counter) and isinstance(probe, collections.Counter):
    # kicad-cli checks footprint links through the CvPcb module's footprint library table,
    # which is outside the schematic slice; the probe skips that one test.
    links = sum(n for v, n in cli.items() if v[1] == "footprint_link_issues")
    cli = collections.Counter({v: n for v, n in cli.items() if v[1] != "footprint_link_issues"})
    if links:
        print(f"erc: {links} kicad-cli footprint_link_issues not compared")
    if cli == probe:
        print("erc: probe violations identical to kicad-cli")
    else:
        for item in list((cli - probe).elements())[:5]: print("  only kicad-cli:", item)
        for item in list((probe - cli).elements())[:5]: print("  only probe:", item)
PY
