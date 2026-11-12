"""Compare CorvCAD's portable schematic connectivity with KiCad's netlist (desktop/CI only).

For every single-sheet project under the given folders (root .kicad_sch without child
sheets), runs `corvcad_eda_cli nets` and `kicad-cli sch export netlist` and compares the nets
that contain component pins: the pin groups and each net's name. Exits 1 on any difference.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from bridge import find_cli


def kicad_nets(cli: str, schematic: Path) -> dict[frozenset[str], str]:
    with tempfile.TemporaryDirectory() as folder:
        output = Path(folder) / "net.xml"
        subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadxml", "--output", str(output),
                        str(schematic)], check=True, capture_output=True)
        root = ET.parse(output).getroot()
    nets = {}
    for net in root.findall("./nets/net"):
        pins = frozenset(f"{n.get('ref')}.{n.get('pin')}" for n in net.findall("node")
                         if not n.get("ref", "#").startswith("#"))
        if pins:
            nets[pins] = net.get("name")
    return nets


def engine_nets(engine: str, schematic: Path) -> dict[frozenset[str], str]:
    result = subprocess.run([engine, "nets", str(schematic)], check=True, capture_output=True, text=True)
    nets = {}
    for line in result.stdout.splitlines():
        name, _, pins = line.partition("\t")
        nets[frozenset(pins.split())] = name
    return nets


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", help="path to corvcad_eda_cli")
    parser.add_argument("folders", nargs="+", type=Path)
    args = parser.parse_args(argv)
    cli = find_cli()
    checked = failed = 0
    for folder in args.folders:
        for project in sorted(folder.rglob("*.kicad_pro")):
            schematic = project.with_suffix(".kicad_sch")
            if not schematic.is_file() or re.search(r"\(sheet\s", schematic.read_text(encoding="utf-8", errors="replace")):
                continue
            try:
                expected, actual = kicad_nets(cli, schematic), engine_nets(args.engine, schematic)
            except subprocess.CalledProcessError as error:
                print(f"ERROR {schematic}: {error.stderr or error}")
                failed += 1
                continue
            checked += 1
            missing, extra = expected.keys() - actual.keys(), actual.keys() - expected.keys()
            names = [(expected[p], actual[p]) for p in expected.keys() & actual.keys() if expected[p] != actual[p]]
            if missing or extra or names:
                failed += 1
                print(f"DIFF {schematic}: {len(expected)} KiCad nets")
                for pins in sorted(map(sorted, missing))[:5]:
                    print(f"  KiCad only: {expected[frozenset(pins)]} {' '.join(pins)}")
                for pins in sorted(map(sorted, extra))[:5]:
                    print(f"  CorvCAD only: {actual[frozenset(pins)]} {' '.join(pins)}")
                for kicad, corvcad in sorted(names)[:5]:
                    print(f"  name: KiCad {kicad} / CorvCAD {corvcad}")
            else:
                print(f"OK {schematic}: {len(expected)} nets")
    print(f"{checked} schematics checked, {failed} with differences")
    return 1 if failed or not checked else 0


if __name__ == "__main__":
    raise SystemExit(main())
