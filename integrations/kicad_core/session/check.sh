#!/bin/bash
# Run abi_test against both backends on one project and compare (port gate 3):
#  - per backend: abort and undo restore the first snapshot, redo restores the edited one;
#  - native vs compatible: the unedited snapshots match, ignoring item order. On a shared
#    sheet they differ by design: native reports each instance's reference, compatible the
#    file's single Reference text;
#  - each saved copy loads in kicad-cli; prints ERC violation counts and netlist net counts.
#    abi_test's edits leave a moved symbol and a dangling wire and label, so the copies add
#    pin_not_connected, unconnected_wire_endpoint and label_dangling violations.
#   check.sh <native-abi_test> <compat-abi_test> <project.kicad_pro|.kicad_sch> <work-dir>
set -u
NATIVE=$1; COMPAT=$2; IN=$3; W=$4
CLI=${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
rm -rf "$W" && mkdir -p "$W"
export KICAD_CONFIG_HOME="$W/config"
"$NATIVE" "$IN" "$W/native" > "$W/native.log" 2>&1 || { echo "native: FAILED"; tail -5 "$W/native.log"; }
"$COMPAT" "$IN" "$W/compat" > "$W/compat.log" 2>&1 || { echo "compat: FAILED"; tail -5 "$W/compat.log"; }

SCH="$(basename "${IN%.*}").kicad_sch"
for f in "${IN%.*}.kicad_sch" "$W/native/copy/$SCH" "$W/compat/copy/$SCH"; do
  [ -f "$f" ] || continue
  name=$(echo "$f" | sed -E "s|^$W/||; s|/copy/.*||; s|.*/.*|original|")
  "$CLI" sch erc --format json -o "$W/erc-$name.json" "$f" > /dev/null 2>&1
  "$CLI" sch export netlist --format kicadsexpr -o "$W/net-$name.net" "$f" > /dev/null 2>&1
done

python3 - "$W" <<'PY'
import json, re, sys, pathlib
w = pathlib.Path(sys.argv[1])
def load(backend, name):
    data = json.loads((w / backend / name).read_text())
    data.pop("backend")
    for sheet in data["sheets"]:
        for value in sheet.values():
            if isinstance(value, list):
                value.sort(key=lambda item: item["id"] if isinstance(item, dict) else item)
    return data
def compare(a, b, what):
    if a == b:
        print(f"{what}: same")
        return
    print(f"{what}: DIFFER")
    for sa, sb in zip(a["sheets"], b["sheets"]):
        for key in sa:
            if sa[key] != sb.get(key):
                left = {json.dumps(i, sort_keys=True) for i in sa[key]} if isinstance(sa[key], list) else {repr(sa[key])}
                right = {json.dumps(i, sort_keys=True) for i in sb[key]} if isinstance(sb.get(key), list) else {repr(sb.get(key))}
                for line in sorted(left - right)[:3]: print(f"  {sa['path']} {key} < {line}")
                for line in sorted(right - left)[:3]: print(f"  {sa['path']} {key} > {line}")
for backend in ("native", "compat"):
    if not (w / backend / "redone.json").exists():
        continue
    compare(load(backend, "before.json"), load(backend, "aborted.json"), f"{backend} abort")
    compare(load(backend, "before.json"), load(backend, "undone.json"), f"{backend} undo")
    compare(load(backend, "after.json"), load(backend, "redone.json"), f"{backend} redo")
if all((w / b / "before.json").exists() for b in ("native", "compat")):
    compare(load("native", "before.json"), load("compat", "before.json"), "native vs compat")
# After the edits, new items have backend-specific UUIDs; compare everything else.
def content(backend):
    data = load(backend, "after.json")
    for sheet in data["sheets"]:
        for key, value in sheet.items():
            if isinstance(value, list) and value and isinstance(value[0], dict):
                sheet[key] = sorted(json.dumps({k: v for k, v in i.items() if k not in ("id", "uuid")}, sort_keys=True)
                                    for i in value)
    return data
if all((w / b / "after.json").exists() for b in ("native", "compat")):
    a, b = content("native"), content("compat")
    print("native vs compat after edits (ignoring UUIDs):", "same" if a == b else "DIFFER")
    if a != b:
        for sa, sb in zip(a["sheets"], b["sheets"]):
            for key in sa:
                if sa[key] != sb.get(key) and isinstance(sa[key], list):
                    for line in sorted(set(sa[key]) - set(sb[key]))[:3]: print(f"  {sa['path']} {key} native {line}")
                    for line in sorted(set(sb[key]) - set(sa[key]))[:3]: print(f"  {sa['path']} {key} compat {line}")
for report in sorted(w.glob("erc-*.json")):
    try:
        data = json.loads(report.read_text())
        count = sum(len(s.get("violations", [])) for s in data.get("sheets", []))
    except Exception as error:
        count = f"unreadable ({error.__class__.__name__})"
    net = w / ("net-" + report.stem[4:] + ".net")
    nets = len(re.findall(r"\(net\s", net.read_text())) if net.exists() else "missing"
    print(f"{report.stem[4:]}: erc {count}, nets {nets}")

# In-process netlist and ERC (version 3) after undo vs kicad-cli on the original, and after
# redo vs kicad-cli on the saved copy. Header metadata, timestamps and kicad-cli's footprint
# link check (outside the schematic slice) are left out, as in probe/compare.sh.
def netlist(p):
    text = re.sub(r" *\(", "\n(", re.sub(r"\s+", " ", p.read_text()))
    return [re.sub(r'\(tstamps? "[^"]*"\)', "", re.sub(r" +\)", ")", line)).strip()
            for line in text.split("\n")
            if not re.match(r"\((source|date|tool|textvar|comment|company|rev|title|sheetpath)\b", line)]
def violations(p):
    data = json.loads(p.read_text())
    return sorted((s.get("uuid_path"), v["type"], v["severity"],
                   sorted((i.get("uuid"), round(i["pos"]["x"], 4), round(i["pos"]["y"], 4)) for i in v.get("items", [])))
                  for s in data.get("sheets", []) for v in s.get("violations", [])
                  if v["type"] != "footprint_link_issues")
for state, reference in (("undone", "original"), ("redone", "native")):
    mine, cli = w / "native" / f"{state}.net", w / f"net-{reference}.net"
    if mine.exists() and cli.exists():
        print(f"native {state} netlist vs kicad-cli on {reference}:", "same" if netlist(mine) == netlist(cli) else "DIFFER")
    mine, cli = w / "native" / f"{state}-erc.json", w / f"erc-{reference}.json"
    if mine.exists() and cli.exists():
        a, b = violations(mine), violations(cli)
        print(f"native {state} ERC vs kicad-cli on {reference}:", f"same ({len(a)})" if a == b else f"DIFFER ({len(a)} vs {len(b)})")
PY
