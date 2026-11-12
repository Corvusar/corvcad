"""Executed by stock FreeCADCmd, configured by run.py through environment.

Writes a success report only after all checks pass. FreeCADCmd sometimes exits
zero after script exceptions, so the outer runner requires this report as well.
"""
import hashlib
import copy
import json
import os
from pathlib import Path
import platform
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import FreeCAD as App
import Import
import Mesh
import Part

from checks import close, expected, manifold
from inputs import validate
from model import build, recompute


def bounds(shape):
    b = shape.BoundBox
    return [b.XMin, b.YMin, b.ZMin, b.XMax, b.YMax, b.ZMax]


def check_shape(shape, target):
    if shape.isNull() or not shape.isValid() or len(shape.Solids) != 1 or not shape.isClosed():
        raise AssertionError("Expected one valid, closed B-rep solid")
    for a, b in zip(bounds(shape), target["bounds"]):
        close(a, b)
    close(shape.Volume, target["volume"], max(1e-5, target["volume"]*1e-8))
    return {"bounds_mm": bounds(shape), "volume_mm3": shape.Volume, "valid_solids": 1}


def check_bores(shape, data, settings):
    for hole in data["mounting_holes"]:
        radius = hole["diameter"]/2
        x, y = hole["x"], hole["y"]
        depth = settings["floor"]+settings["standoff_height"]
        # Shrink the probe to avoid counting coincident cylinder surfaces as overlap.
        probe = Part.makeCylinder(radius-0.01, depth, App.Vector(x, y, 0))
        close(shape.common(probe).Volume, 0, 1e-6)
        # An empty cavity would also pass the probe. Require material around the
        # bore, in both the boss and the floor, at four independent azimuths.
        for z in (settings["floor"]/2, settings["floor"]+settings["standoff_height"]/2):
            for dx, dy in ((radius+0.2, 0), (0, radius+0.2), (-radius-0.2, 0), (0, -radius-0.2)):
                if not shape.isInside(App.Vector(x+dx, y+dy, z), 1e-7, False):
                    raise AssertionError(f"Missing boss/floor material around {hole['reference']}")


def check_document(doc, data, settings):
    recompute(doc)
    targets = expected(data, settings)
    result = {name: check_shape(doc.getObject(name).Shape, target) for name, target in targets.items()}
    check_bores(doc.Tray.Shape, data, settings)
    for obj in doc.Objects:
        if obj.TypeId == "Sketcher::SketchObject" and obj.FullyConstrained is False:
            raise AssertionError(f"Underconstrained sketch: {obj.Name}")
    if doc.Tray.Shape.common(doc.Lid.Shape).Volume > 1e-7:
        raise AssertionError("Tray and lid overlap")
    return result


def mesh_export(shape, path):
    vertices, faces = shape.tessellate(0.05)
    mesh = Mesh.Mesh([[vertices[i] for i in face] for face in faces])
    mesh.write(str(path))
    loaded = Mesh.Mesh(str(path))
    if not loaded.isSolid() or loaded.hasNonManifolds() or loaded.hasSelfIntersections():
        raise AssertionError("STL reload did not produce a valid closed mesh")
    points, triangles = loaded.Topology
    report = manifold(points, triangles)
    close(report["signed_volume_mm3"], shape.Volume, max(0.1, shape.Volume*0.005))
    for actual, target in zip(bounds(loaded), bounds(shape)):
        close(actual, target, 1e-4)
    return report


def model_regressions(data):
    """Model paths absent from the two real fixtures: no mounts and mixed bores.

    Negative board origins and a hole at XY zero guard FreeCAD's spreadsheet
    quantity parsing and sketch datum behaviour. These are synthetic test cases,
    never presented as imported boards.
    """
    results = {}
    for name, holes in (("no_mounts", []), ("mixed_bores_and_negative_origin", [
            {"reference": "H1", "x": 0, "y": 0, "diameter": 3.2},
            {"reference": "H2", "x": 15, "y": 10, "diameter": 2.5}])):
        sample = copy.deepcopy(data)
        sample["bounds"] = [-30, -20, 30, 20]
        sample["mounting_holes"] = holes
        doc = build(sample)
        results[name] = check_document(doc, sample, validate(sample))
        App.closeDocument(doc.Name)
    return results


def main():
    started = time.monotonic()
    data_path = Path(os.environ["CORVCAD_FREECAD_INPUT"])
    output = Path(os.environ["CORVCAD_FREECAD_OUTPUT"])
    lock = json.loads(Path(__file__).with_name("upstream.json").read_text())
    version = App.Version()
    if version[:3] != lock["source"]["tag"].split(".") or version[-1] != lock["source"]["commit"]:
        raise RuntimeError(f"Expected pinned FreeCAD {lock['source']['tag']} / {lock['source']['commit']}, got {version}")
    data = json.loads(data_path.read_text(encoding="utf-8"))
    settings = validate(data)
    doc = build(data, settings)
    baseline = check_document(doc, data, settings)
    persisted = output / "enclosure.FCStd"
    doc.saveAs(str(persisted))
    App.closeDocument(doc.Name)
    doc = App.openDocument(str(persisted))
    for obj in doc.Objects:
        obj.touch()
    check_document(doc, data, settings)

    # Each independent parameter must change a recomputed solid, then restore.
    changes = {"wall": 2.6, "floor": 2.7, "standoff_height": 5.1, "standoff_diameter": 6.8,
               "margin": 4.0, "board_clearance": 3.1, "lid_thickness": 2.6, "lid_gap": 0.5}
    recomputes = []
    for key, value in changes.items():
        candidate = {**settings, key: value}
        validate(data, candidate)
        alias = "".join(word.title() for word in key.split("_"))
        cell = doc.Parameters.getCellFromAlias(alias)
        doc.Parameters.set(cell, f"{value} mm")
        measured = check_document(doc, data, candidate)
        recomputes.append({"parameter": key, "value_mm": value, "parts": measured})
        doc.Parameters.set(cell, f"{settings[key]} mm")
        check_document(doc, data, settings)

    # Re-save the restored document; source user board is never touched.
    doc.save()
    meshes = {name: mesh_export(doc.getObject(name).Shape, output / f"{name.lower()}.stl")
              for name in ("Tray", "Lid")}
    step = output / "enclosure.step"
    Import.export([doc.Tray, doc.Lid], str(step))
    imported = Part.Shape()
    imported.read(str(step))
    if not imported.isValid() or len(imported.Solids) != 2:
        raise AssertionError("STEP must reload with exactly two valid solids")
    solids = sorted(imported.Solids, key=lambda s: s.BoundBox.ZMin)
    step_parts = {name: check_shape(solid, expected(data, settings)[name])
                  for name, solid in zip(("Tray", "Lid"), solids)}
    check_bores(solids[0], data, settings)
    for name in ("Tray", "Lid"):
        path = output / f"{name.lower()}.step"
        Import.export([doc.getObject(name)], str(path))
        separate = Part.Shape()
        separate.read(str(path))
        check_shape(separate, expected(data, settings)[name])
        if name == "Tray":
            check_bores(separate, data, settings)
    files = {p.name: {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in (persisted, step, output / "tray.step", output / "lid.step",
                       output / "tray.stl", output / "lid.stl")}
    report = {"status": "passed", "gate": "F1-desktop-model", "platform": platform.platform(),
              "freecad_version": version, "occt_version": Part.OCC_VERSION,
              "input_sha256": hashlib.sha256(data_path.read_bytes()).hexdigest(),
              "board_source": data["source"], "settings_mm": settings,
              "tools_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                               for name in ("fetch.py", "prepare.py", "inputs.py", "model.py", "checks.py", "proof.py", "run.py", "upstream.json")},
              "parts": baseline, "bore_count": len(data["mounting_holes"]),
              "save_reopen_recompute": "passed", "parameter_recomputes": recomputes,
              "stl": meshes, "step_reimport": step_parts, "files": files,
              "synthetic_model_regressions": model_regressions(data),
              "elapsed_seconds": time.monotonic()-started}
    (output / "report.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    App.closeDocument(doc.Name)
    print(f"F1 passed: {data['source']['name']} -> {output}")


# FreeCADCmd executes files in its console namespace, whose __name__ varies by
# build. This is an executable script, never a library imported by the runner.
main()
