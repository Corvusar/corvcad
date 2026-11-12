"""Independently check the UI-authored LED Switch project with desktop KiCad."""
import argparse
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="Directory containing matching KiCad project files")
    parser.add_argument("output", type=Path, help="New report directory")
    parser.add_argument("--symbols", required=True, type=Path, help="Pinned packed symbols directory")
    parser.add_argument("--footprints", required=True, type=Path, help="Pinned packed footprints directory")
    parser.add_argument("--cli", default="/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    args = parser.parse_args()
    project = args.project.resolve()
    schematic = project / (project.name + ".kicad_sch")
    board = project / (project.name + ".kicad_pcb")
    if not schematic.is_file() or not board.is_file():
        parser.error("Project directory must contain its matching schematic and board")
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    config = output / "kicad-config" / "10.0"
    config.mkdir(parents=True)
    shutil.copy2(args.symbols / "sym-lib-table", config / "sym-lib-table")
    shutil.copy2(args.footprints / "fp-lib-table", config / "fp-lib-table")
    env = dict(os.environ, KICAD_CONFIG_HOME=str(config.parent),
               KICAD10_SYMBOL_DIR=str(args.symbols.resolve()),
               KICAD10_FOOTPRINT_DIR=str(args.footprints.resolve()))
    version = subprocess.check_output([args.cli, "version"], text=True).strip()
    commands = [
        ["sch", "erc", "--exit-code-violations", "--format", "json", "--output", str(output / "erc.json"), str(schematic)],
        ["pcb", "drc", "--schematic-parity", "--exit-code-violations", "--format", "json", "--output", str(output / "drc.json"), str(board)],
        ["sch", "export", "netlist", "--format", "kicadxml", "--output", str(output / "netlist.xml"), str(schematic)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run([args.cli, *command], env=env, text=True, capture_output=True)
        (output / f"check-{index + 1}.log").write_text(result.stdout + result.stderr)
        result.check_returncode()
    netlist = ET.parse(output / "netlist.xml").getroot()
    components = {c.attrib["ref"]: c.findtext("value") for c in netlist.findall("./components/comp")}
    nets = [{(n.attrib["ref"], n.attrib["pin"]) for n in net.findall("node")} for net in netlist.findall("./nets/net")]
    expected = [
        {("J1", "1"), ("R2", "1")}, {("R2", "2"), ("D1", "2")},
        {("D1", "1"), ("Q1", "1")}, {("J2", "1"), ("R1", "1")},
        {("R1", "2"), ("Q1", "2")}, {("J1", "2"), ("J2", "2"), ("Q1", "3")},
    ]
    if set(components) != {"J1", "J2", "R1", "R2", "D1", "Q1"}:
        raise RuntimeError(f"Unexpected component references: {components}")
    if {frozenset(n) for n in nets} != {frozenset(n) for n in expected}:
        raise RuntimeError(f"Unexpected electrical connectivity: {nets}")
    (output / "connectivity.json").write_text(json.dumps({
        "kicad_cli": version, "components": components, "nets": [sorted(n) for n in nets],
    }, indent=2))
    print("ERC/DRC/parity passed; six components, six nets and thirteen pins verified.")


if __name__ == "__main__":
    main()
