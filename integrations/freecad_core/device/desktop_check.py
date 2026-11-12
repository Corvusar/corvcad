"""Independently reopen collected FCStd/STEP/STL in pinned desktop FreeCAD. MIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
CORE = HERE.parent
sys.path.insert(0, str(HERE))  # FreeCADCmd does not reliably add the executed script's directory.


def verify_collection(case):
    from packet import inside, record
    receipt = json.loads((case / "collection.json").read_text())
    if (receipt.get("status") != "collected; independent desktop and slicer/device review pending"
            or receipt.get("f5_accepted") is not False
            or receipt.get("hardware_class") not in ("physical-device", "simulator")):
        raise ValueError("A complete collection is required before desktop validation")
    for relative, expected in receipt["artifacts"].items():
        if record(inside(case, relative)) != expected:
            raise ValueError("Collected artifact hash mismatch: " + relative)
    return receipt


def validate(case, output):
    # Imports remain here: preparation and contract tests require no FreeCAD installation.
    sys.path.insert(0, str(CORE))
    sys.path.insert(0, str(CORE / "session"))
    import FreeCAD as App
    import Mesh
    import Part
    from checks import close, expected, manifold
    from inputs import DEFAULTS
    from session_runtime import Session

    from packet import record, save
    receipt = verify_collection(case)
    lock = json.loads((CORE / "upstream.json").read_text())
    version = App.Version()
    if version[:3] != lock["source"]["tag"].split(".") or version[-1] != lock["source"]["commit"]:
        raise ValueError("Desktop reimport requires the pinned FreeCAD build")
    data = json.loads((case / "input.json").read_text())
    device = json.loads((case / "device-report.json").read_text())
    before = {name: record(case / name) for name in device["files"]}
    session = Session(case / "enclosure.FCStd", json.dumps(data), DEFAULTS)
    try:
        settings = session.get_settings()
        if settings != device["settings_mm"]:
            raise ValueError("Desktop FCStd settings differ from the device report")
    finally:
        session.close()
    targets = expected(data, settings)

    def shape_check(shape, name):
        if shape.isNull() or not shape.isValid() or not shape.isClosed() or len(shape.Solids) != 1:
            raise ValueError("Expected one valid closed exact solid")
        box = shape.BoundBox
        bounds = [box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax]
        for actual, wanted in zip(bounds, targets[name]["bounds"]):
            close(actual, wanted)
        close(shape.Volume, targets[name]["volume"], max(1e-5, targets[name]["volume"]*1e-8))
        if name == "Tray":
            for hole in data["mounting_holes"]:
                radius = hole["diameter"] / 2
                x, y = hole["x"], hole["y"]
                probe = Part.makeCylinder(radius-0.01, settings["floor"]+settings["standoff_height"], App.Vector(x, y, 0))
                close(shape.common(probe).Volume, 0, 1e-6)
                for z in (settings["floor"]/2, settings["floor"]+settings["standoff_height"]/2):
                    for dx, dy in ((radius+0.2, 0), (0, radius+0.2), (-radius-0.2, 0), (0, -radius-0.2)):
                        if not shape.isInside(App.Vector(x+dx, y+dy, z), 1e-7, False):
                            raise ValueError("Missing material around a mounting bore")
        return {"valid_solids": 1, "bounds_mm": bounds, "volume_mm3": shape.Volume}

    document = App.openDocument(str(case / "enclosure.FCStd"))
    try:
        document.recompute()
        fcstd = {name: shape_check(document.getObject(name).Shape, name) for name in targets}
    finally:
        App.closeDocument(document.Name)
    steps, meshes = {}, {}
    for name in targets:
        step = Part.Shape()
        step.read(str(case / (name.lower() + ".step")))
        steps[name] = shape_check(step, name)
        mesh = Mesh.Mesh(str(case / (name.lower() + ".stl")))
        if not mesh.isSolid() or mesh.hasNonManifolds() or mesh.hasSelfIntersections():
            raise ValueError("STL is not a valid closed mesh")
        points, faces = mesh.Topology
        meshes[name] = manifold(points, faces)
        close(meshes[name]["signed_volume_mm3"], targets[name]["volume"], max(0.1, targets[name]["volume"]*0.005))
        box = mesh.BoundBox
        for actual, wanted in zip((box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax), targets[name]["bounds"]):
            close(actual, wanted, 1e-4)
    if before != {name: record(case / name) for name in device["files"]}:
        raise ValueError("Desktop reimport changed collected files")
    report = {"schema": 1, "status": "desktop reimport passed", "f5_accepted": False,
              "hardware_class": receipt["hardware_class"], "freecad_version": version,
              "occt_version": Part.OCC_VERSION, "fcstd": fcstd, "step": steps, "stl": meshes,
              "collection_sha256": record(case / "collection.json")["sha256"],
              "validator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "slicer_review": "pending", "manual_device_ui": "pending"}
    save(output, report)
    return report


def run_host(executable, case, output):
    verify_collection(case)
    if output.exists():
        raise ValueError("Refusing to replace a desktop report")
    environment = dict(os.environ, CORVCAD_F5_CASE=str(case.resolve()), CORVCAD_F5_DESKTOP_REPORT=str(output.resolve()))
    with output.with_suffix(".log").open("xb") as log:
        result = subprocess.run([str(executable), str(Path(__file__).resolve())], env=environment,
                                stdout=log, stderr=subprocess.STDOUT, timeout=600)
    # FreeCADCmd can return zero after script exceptions; success JSON is mandatory.
    if result.returncode or not output.is_file() or json.loads(output.read_text()).get("status") != "desktop reimport passed":
        raise RuntimeError("Desktop validation failed; retain the log and collected files")


if os.environ.get("CORVCAD_F5_CASE") and os.environ.get("CORVCAD_F5_DESKTOP_REPORT"):
    validate(Path(os.environ["CORVCAD_F5_CASE"]), Path(os.environ["CORVCAD_F5_DESKTOP_REPORT"]))
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freecad-command", type=Path, required=True)
    parser.add_argument("--case", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run_host(args.freecad_command, args.case, args.output)
    print("Desktop reimport passed; slicer and physical/UI acceptance remain separate")
