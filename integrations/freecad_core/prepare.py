#!/usr/bin/env python3
"""Read a board with installed KiCad 10 pcbnew; emit the narrow F1 JSON boundary.

Run with KiCad's Python, separately from FreeCAD's interpreter. No board writes.
Only a rectangular outer loop without slots and circular MountingHole NPTH pads
are supported. This deliberately does not duplicate the portable board parser.
"""
import argparse
import hashlib
import json
from pathlib import Path


def extract(path):
    import pcbnew
    version = pcbnew.GetBuildVersion()
    if not version.startswith("10."):
        raise ValueError(f"F1 requires KiCad 10, got {version}")
    board = pcbnew.LoadBoard(str(path))
    outlines = pcbnew.SHAPE_POLY_SET()
    if not board.GetBoardPolygonOutlines(outlines, False) or outlines.OutlineCount() != 1:
        raise ValueError("F1 requires one closed Edge.Cuts outer loop")
    if outlines.HoleCount(0):
        raise ValueError("Board slots/inner loops are outside F1")
    loop = outlines.COutline(0)
    points = [[pcbnew.ToMM(loop.CPoint(i).x), pcbnew.ToMM(loop.CPoint(i).y)]
              for i in range(loop.PointCount())]
    # Even an arc with only cardinal endpoints must not pass as a rectangle.
    if loop.ArcCount() or len(points) != 4:
        raise ValueError("F1 supports rectangular boards only")
    xs, ys = sorted({p[0] for p in points}), sorted({p[1] for p in points})
    if len(xs) != 2 or len(ys) != 2 or len({tuple(p) for p in points}) != 4:
        raise ValueError("F1 supports axis-aligned rectangular boards only")
    if any(a[0] != b[0] and a[1] != b[1] for a, b in zip(points, points[1:] + points[:1])):
        raise ValueError("Crossed rectangle outline")
    holes = []
    for footprint in board.GetFootprints():
        if "MountingHole" not in footprint.GetFPIDAsString():
            continue
        for pad in footprint.Pads():
            drill = pad.GetDrillSize()
            if (pad.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH or
                    pad.GetDrillShape() != pcbnew.PAD_DRILL_SHAPE_CIRCLE or drill.x != drill.y):
                raise ValueError("F1 supports circular unplated mounting holes only")
            holes.append({"reference": footprint.GetReference(),
                          "x": pcbnew.ToMM(pad.GetPosition().x),
                          "y": pcbnew.ToMM(pad.GetPosition().y),
                          "diameter": pcbnew.ToMM(drill.x)})
    return {"schema": 1, "units": "mm", "coordinate_system": "KiCad XY unchanged; enclosure Z up",
            "source": {"name": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                       "kicad_version": version},
            "bounds": [xs[0], ys[0], xs[1], ys[1]],
            "board_thickness": pcbnew.ToMM(board.GetDesignSettings().GetBoardThickness()),
            "mounting_holes": sorted(holes, key=lambda h: (h["reference"], h["x"], h["y"]))}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = extract(args.board)
    # Exclusive creation keeps this experiment from replacing existing user data.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(data, indent=2) + "\n")
    print(f"Prepared {args.board.name}: {len(data['mounting_holes'])} mounting holes")
