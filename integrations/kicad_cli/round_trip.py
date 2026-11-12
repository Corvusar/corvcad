"""KiCad round trip of a CorvCAD project folder (desktop only).

Runs KiCad's ERC and DRC on the project, re-saves both files with KiCad's own writer
(`kicad-cli ... upgrade --force`) in a copy, and checks that the re-saved project has the
same netlist, footprint positions and violations. With `expected.json` in the project
folder or given by --expected (`{"nets": [["R1.1", "R2.2"], ...]}`), the netlist must contain
exactly those multi-pin nets, so wiring that touches the wrong pin fails. The project is never
changed. Fixture: engine/tests/fixtures/roundtrip (see docs/KICAD_TEST_PROJECT.md).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from bridge import find_cli


def kicad(cli: str, *args: str) -> None:
    result = subprocess.run([cli, *args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"kicad-cli {' '.join(args[:2])} failed: {result.stdout}{result.stderr}")


def nets(cli: str, schematic: Path, output: Path) -> set[frozenset[str]]:
    kicad(cli, "sch", "export", "netlist", "--format", "kicadxml", "--output", str(output), str(schematic))
    found = set()
    for net in ET.parse(output).getroot().findall("./nets/net"):
        pins = frozenset(f"{node.get('ref')}.{node.get('pin')}" for node in net.findall("node"))
        if len(pins) > 1:
            found.add(pins)
    return found


def violations(cli: str, source: Path, output: Path) -> list[str]:
    kind = ("sch", "erc") if source.suffix == ".kicad_sch" else ("pcb", "drc")
    kicad(cli, *kind, "--format", "json", "--severity-all", "--output", str(output), str(source))
    report = json.loads(output.read_text(encoding="utf-8"))
    items = report.get("violations", []) + report.get("unconnected_items", [])
    items += [item for sheet in report.get("sheets", []) for item in sheet.get("violations", [])]
    return sorted(item["type"] for item in items)


def positions(cli: str, board: Path, output: Path) -> str:
    kicad(cli, "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--output", str(output), str(board))
    return output.read_text(encoding="utf-8")


def check(project: Path, work: Path, expected: Path) -> list[str]:
    cli = find_cli()
    stem = next(project.glob("*.kicad_pro")).stem
    original, resaved = work / "original", work / "resaved"
    for copy in (original, resaved):
        shutil.rmtree(copy, ignore_errors=True)
        shutil.copytree(project, copy)
    kicad(cli, "sch", "upgrade", "--force", str(resaved / f"{stem}.kicad_sch"))
    kicad(cli, "pcb", "upgrade", "--force", str(resaved / f"{stem}.kicad_pcb"))

    problems = []
    result = {}
    for name, folder in (("original", original), ("resaved", resaved)):
        sch, pcb = folder / f"{stem}.kicad_sch", folder / f"{stem}.kicad_pcb"
        result[name] = {
            "nets": nets(cli, sch, work / f"{name}.xml"),
            "erc": violations(cli, sch, work / f"{name}-erc.json"),
            "drc": violations(cli, pcb, work / f"{name}-drc.json"),
            "positions": positions(cli, pcb, work / f"{name}-pos.csv"),
        }
    first, second = result["original"], result["resaved"]
    print(f"ERC: {first['erc'] or 'no violations'}")
    print(f"DRC: {first['drc'] or 'no violations'}")
    print("Nets: " + "; ".join(" ".join(sorted(net)) for net in sorted(first["nets"], key=sorted)))
    for key in ("nets", "erc", "drc", "positions"):
        if first[key] != second[key]:
            problems.append(f"KiCad's re-saved copy differs in {key}")
    if expected.is_file():
        wanted = {frozenset(net) for net in json.loads(expected.read_text(encoding="utf-8"))["nets"]}
        if first["nets"] != wanted:
            problems.append(f"netlist differs from expected.json: missing {sorted(map(sorted, wanted - first['nets']))}, "
                            f"unexpected {sorted(map(sorted, first['nets'] - wanted))}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="folder with one .kicad_pro, .kicad_sch and .kicad_pcb")
    parser.add_argument("output", type=Path, help="work folder for copies and reports")
    parser.add_argument("--expected", type=Path, help="expected.json to use instead of the project's")
    args = parser.parse_args(argv)
    project = args.project.resolve()
    try:
        problems = check(project, args.output.resolve(), args.expected or project / "expected.json")
    except (FileNotFoundError, RuntimeError, StopIteration) as error:
        print(error or "no .kicad_pro in the project folder", file=sys.stderr)
        return 1
    for problem in problems:
        print(problem, file=sys.stderr)
    print("round trip ok" if not problems else f"{len(problems)} round-trip problems")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
