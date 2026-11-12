#!/bin/bash
# Run the board probe and compare with the installed KiCad:
#  - model: the probe's report vs integrations/kicad_native/bridge.py `inspect` (KiCad's own
#    pcbnew Python module) on the original board;
#  - save round trip: KiCad's report on the probe-saved board must match the original's, and
#    `kicad-cli pcb drc` must report the same violation count for both;
#  - DRC: the probe's in-process violation list vs `kicad-cli pcb drc` on the original (both
#    without schematic parity, kicad-cli's default).
#   compare_pcb.sh <probe-binary> <in.kicad_pcb> <work-dir>
set -u
PROBE=$1; IN=$2; W=$3
APP=${KICAD_APP:-/Applications/KiCad/KiCad.app}
CLI=$APP/Contents/MacOS/kicad-cli
PY=$APP/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
BRIDGE=$(cd "$(dirname "$0")/../../kicad_native" && pwd)/bridge.py
rm -rf "$W" && mkdir -p "$W/probe"
export KICAD_CONFIG_HOME="$W/config"
cp -R "$(dirname "$IN")"/. "$W/probe/"
SAVED="$W/probe/$(basename "$IN")"

echo "probe: $("$PROBE" "$IN" "$W/probe" 2>"$W/probe.err" || echo "FAILED (see $W/probe.err)")"
"$PY" "$BRIDGE" inspect "$IN" "$W/kicad.json" > "$W/bridge.log" 2>&1
"$PY" "$BRIDGE" inspect "$SAVED" "$W/kicad-saved.json" >> "$W/bridge.log" 2>&1
for f in "$IN" "$SAVED"; do
  "$CLI" pcb drc --format json -o "$W/drc-$(basename "$(dirname "$f")").json" "$f" > /dev/null 2>&1
done

/usr/bin/python3 - "$W" <<'PY'
import json, math, sys, pathlib
w = pathlib.Path(sys.argv[1])
def load(name):
    try:
        data = json.loads((w / name).read_text())
    except Exception as error:
        return f"unreadable ({error.__class__.__name__})"
    data.pop("kicad_version", None)
    return data
def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, (int, float)) and isinstance(b, (int, float)) and math.isclose(a, b, abs_tol=1e-6)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b
probe, kicad, saved = load("probe/board.json"), load("kicad.json"), load("kicad-saved.json")
print("model:", "identical to KiCad's pcbnew module" if same(probe, kicad) else "DIFFERS (compare probe/board.json with kicad.json)")
print("save round trip:", "KiCad reads the probe-saved board identically" if same(kicad, saved) else "DIFFERS (compare kicad.json with kicad-saved.json)")
def count(p):
    try:
        data = json.loads(p.read_text())
        return len(data.get("violations", [])) + len(data.get("unconnected_items", []))
    except Exception as error:
        return f"unreadable ({error.__class__.__name__})"
print("drc:", ", ".join(f"{p.stem}={count(p)}" for p in sorted(w.glob("drc-*.json"))))
# The probe's in-process DRC (probe-drc.json) must list the same violations and unconnected
# items as kicad-cli on the original: type, severity, item UUIDs and positions.
import collections
def items(p):
    try:
        data = json.loads(p.read_text())
    except Exception as error:
        return f"unreadable ({error.__class__.__name__})"
    return collections.Counter(
        (key, v["type"], v["severity"],
         tuple(sorted((i.get("uuid"), round(i["pos"]["x"], 4), round(i["pos"]["y"], 4)) for i in v.get("items", []))))
        for key in ("violations", "unconnected_items") for v in data.get(key, []))
original = next((p for p in w.glob("drc-*.json") if p.stem != "drc-probe"), None)
cli, probe = (items(original) if original else "missing"), items(w / "probe" / "probe-drc.json")
if isinstance(cli, collections.Counter) and isinstance(probe, collections.Counter):
    if cli == probe:
        print(f"drc: probe list identical to kicad-cli ({sum(cli.values())} items)")
    else:
        print(f"drc: probe {sum(probe.values())} vs kicad-cli {sum(cli.values())}")
        for item in list((cli - probe).elements())[:5]: print("  only kicad-cli:", item)
        for item in list((probe - cli).elements())[:5]: print("  only probe:", item)
else:
    print(f"drc: kicad-cli {cli}, probe {probe}")
PY
