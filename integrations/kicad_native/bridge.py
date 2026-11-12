r"""Desktop KiCad 10 PCB model adapter (bundled pcbnew Python bindings).

Run with KiCad's Python interpreter, for example on Windows:
  & 'C:\Program Files\KiCad\10.0\bin\python.exe' integrations/kicad_native/bridge.py inspect board.kicad_pcb out.json

This executes KiCad's own board loader/model. It is a reference adapter, not
an iPadOS port, and it never overwrites the input board.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import pcbnew


def mm(value: int) -> float:
    return round(pcbnew.ToMM(value), 6)


def point(value: pcbnew.VECTOR2I) -> list[float]:
    return [mm(value.x), mm(value.y)]


def inspect(board: pcbnew.BOARD) -> dict:
    footprints = []
    for footprint in board.GetFootprints():
        pads = []
        for pad in footprint.Pads():
            pads.append({
                "number": pad.GetNumber(),
                "position_mm": point(pad.GetPosition()),
                "net": pad.GetNetname(),
            })
        footprints.append({
            "reference": footprint.GetReference(),
            "value": footprint.GetValue(),
            "position_mm": point(footprint.GetPosition()),
            "rotation_deg": footprint.GetOrientationDegrees(),
            "pads": sorted(pads, key=lambda item: item["number"]),
        })
    tracks = []
    vias = []
    for item in board.GetTracks():
        if isinstance(item, pcbnew.PCB_VIA):
            vias.append({"position_mm": point(item.GetPosition()),
                         "net": item.GetNetname()})
        else:
            tracks.append({"start_mm": point(item.GetStart()),
                           "end_mm": point(item.GetEnd()),
                           "net": item.GetNetname(),
                           "layer": item.GetLayerName()})
    return {
        "kicad_version": pcbnew.GetBuildVersion(),
        "footprints": sorted(footprints, key=lambda item: item["reference"]),
        "tracks": tracks,
        "vias": vias,
        "zones": len(list(board.Zones())),
    }


def audit_project(schematic_path: Path, board: pcbnew.BOARD) -> dict:
    """Compare KiCad's schematic netlist with KiCad's board model."""
    cli = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli")
    if not cli:
        sibling = Path(sys.executable).with_name("kicad-cli.exe")
        cli = str(sibling) if sibling.is_file() else None
    if not cli:
        raise FileNotFoundError("kicad-cli is required for project audit")
    with tempfile.TemporaryDirectory(prefix="corvcad-netlist-") as directory:
        netlist = Path(directory) / "netlist.xml"
        result = subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadxml",
                                 "--output", str(netlist), str(schematic_path)],
                                text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(f"KiCad netlist export failed: {result.stdout}\n{result.stderr}")
        root = ET.parse(netlist).getroot()

    components = {}
    for comp in root.findall("./components/comp"):
        footprint = comp.findtext("footprint", default="").strip()
        if footprint:
            components[comp.attrib["ref"]] = footprint
    board_footprints = {fp.GetReference(): fp for fp in board.GetFootprints()}
    missing = sorted(set(components) - set(board_footprints))
    board_only = sorted(set(board_footprints) - set(components))
    footprint_mismatches = [
        {"reference": ref, "schematic": fp_id,
         "board": board_footprints[ref].GetFPIDAsString()}
        for ref, fp_id in sorted(components.items())
        if ref in board_footprints and fp_id != board_footprints[ref].GetFPIDAsString()
    ]

    schematic_pins = {}
    for net in root.findall("./nets/net"):
        for node in net.findall("node"):
            ref = node.attrib.get("ref", "")
            pin = node.attrib.get("pin", "")
            if ref in components and pin:
                schematic_pins[(ref, pin)] = net.attrib.get("name", "")
    board_pins = {}
    for ref, footprint in board_footprints.items():
        if ref not in components:
            continue
        for pad in footprint.Pads():
            if pad.GetNumber():
                board_pins[(ref, pad.GetNumber())] = pad.GetNetname()
    missing_pads = sorted(schematic_pins.keys() - board_pins.keys())
    extra_board_net_pads = sorted(
        key for key, name in board_pins.items() if name and key not in schematic_pins
    )
    net_mismatches = [
        {"reference": ref, "pin": pin, "schematic": name,
         "board": board_pins[(ref, pin)]}
        for (ref, pin), name in sorted(schematic_pins.items())
        if (ref, pin) in board_pins and name != board_pins[(ref, pin)]
    ]
    return {
        "kicad_version": pcbnew.GetBuildVersion(),
        "schematic_components_with_footprints": len(components),
        "board_footprints": len(board_footprints),
        "missing_on_board": missing,
        "board_only": board_only,
        "footprint_mismatches": footprint_mismatches,
        "missing_pads": [{"reference": ref, "pin": pin} for ref, pin in missing_pads],
        "extra_board_net_pads": [
            {"reference": ref, "pin": pin, "board": board_pins[(ref, pin)]}
            for ref, pin in extra_board_net_pads
        ],
        "net_mismatches": net_mismatches,
        "consistent": not (missing or footprint_mismatches or missing_pads
                           or extra_board_net_pads or net_mismatches),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    view = sub.add_parser("inspect", help="load with KiCad and write a JSON reference report")
    view.add_argument("board", type=Path)
    view.add_argument("output", type=Path)
    move = sub.add_parser("move", help="use KiCad's own model to move a footprint in a copy")
    move.add_argument("board", type=Path)
    move.add_argument("reference")
    move.add_argument("x_mm", type=float)
    move.add_argument("y_mm", type=float)
    move.add_argument("output", type=Path)
    audit = sub.add_parser("audit", help="compare KiCad schematic netlist to board model")
    audit.add_argument("board", type=Path)
    audit.add_argument("schematic", type=Path)
    audit.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.board.resolve()
    if not source.is_file():
        parser.error(f"Board not found: {source}")
    board = pcbnew.LoadBoard(str(source))
    if board is None:
        parser.error(f"KiCad could not load board: {source}")
    output = args.output.resolve()
    if output == source:
        parser.error("Output must differ from input")
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.command == "audit":
        schematic = args.schematic.resolve()
        if not schematic.is_file():
            parser.error(f"Schematic not found: {schematic}")
        report = audit_project(schematic, board)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"KiCad project audit: {output} (consistent={report['consistent']})")
        if not report["consistent"]:
            raise SystemExit(1)
    elif args.command == "inspect":
        output.write_text(json.dumps(inspect(board), indent=2) + "\n", encoding="utf-8")
        print(f"KiCad board model report: {output}")
    else:
        matches = [fp for fp in board.GetFootprints() if fp.GetReference() == args.reference]
        if len(matches) != 1:
            parser.error(f"Expected one footprint {args.reference}, found {len(matches)}")
        matches[0].SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(args.x_mm),
                                                pcbnew.FromMM(args.y_mm)))
        if not pcbnew.SaveBoard(str(output), board):
            parser.error(f"KiCad could not save board: {output}")
        print(f"KiCad-native board copy: {output}; run DRC before fabrication")


if __name__ == "__main__":
    main()
