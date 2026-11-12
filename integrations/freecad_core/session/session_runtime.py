"""Persistent FreeCAD enclosure sessions behind CorvCAD's mechanical C ABI.

MIT. Uses the unchanged F1 model, input validation and numeric/mesh checks.
This module requires a real FreeCAD interpreter; it has no mesh-only fallback.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile

# Source-tree use. A packaged runtime puts this module beside the F1 helpers.
_source_helpers = Path(__file__).resolve().parent.parent
if (_source_helpers / "model.py").is_file():
    sys.path.insert(0, str(_source_helpers))

import FreeCAD as App
import Import
import Mesh
import Part

from checks import close as numeric_close, expected, manifold
from inputs import DEFAULTS, MAX_CUTOUTS, extent, fastening_geometry, flatten, thickness, tray_height, validate, vent_slots
from inputs import _shape as sketch_shape

_KEEP = object()  # set_openings: leave this kind of opening as it is
import model

_OPEN_PATHS = set()


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _mechanical_identity(data):
    name = data["source"].get("name")
    if (not isinstance(name, str) or not name or "/" in name or "\\" in name or
            not (name.lower().endswith(".kicad_pcb") or name == "corvcad.project.json")):
        # corvcad.project.json: a mechanical-only project's box (bounds, height as thickness, no holes).
        raise ValueError("Expected a project-local board filename")
    return {"board_name": name, "schema": data["schema"], "units": data["units"],
            "bounds": data["bounds"], "board_thickness": data["board_thickness"],
            "mounting_holes": data["mounting_holes"]}


def _hash(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular file: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _alias(name):
    return "".join(word.title() for word in name.split("_"))


def _bounds(shape):
    box = shape.BoundBox
    return (box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax)


def _shape_check(shape, target):
    if shape.isNull() or not shape.isValid() or not shape.isClosed() or len(shape.Solids) != 1:
        raise ValueError("Expected one valid closed exact solid")
    if not math.isfinite(shape.Volume) or shape.Volume <= 0:
        raise ValueError("Invalid exact solid volume")
    if any(not math.isfinite(value) for value in _bounds(shape)):
        raise ValueError("Non-finite exact solid bounds")
    for actual, wanted in zip(_bounds(shape), target["bounds"]):
        numeric_close(actual, wanted)
    numeric_close(shape.Volume, target["volume"], max(1e-5, target["volume"] * 1e-8))


def _signature(doc):
    """Model-owned structure; custom objects/properties remain outside it."""
    result = {}
    for obj in doc.Objects:
        profile = getattr(obj, "Profile", None)
        if isinstance(profile, (tuple, list)):
            profile = profile[0] if profile else None
        result[obj.Name] = {
            "type": obj.TypeId,
            "expressions": tuple((str(a), str(b)) for a, b in getattr(obj, "ExpressionEngine", ())),
            "profile": getattr(profile, "Name", None),
            "tip": getattr(getattr(obj, "Tip", None), "Name", None),
            "constraints": getattr(obj, "ConstraintCount", None),
        }
    return result


def _build(data, settings, cutouts=(), vents=(), screws=None, sketches=()):
    before = set(App.listDocuments())
    try:
        return model.build(data, settings, cutouts, vents, screws, sketches)
    except Exception:
        for name in set(App.listDocuments()) - before:
            App.closeDocument(name)
        raise


def _inside(shape):
    """A point inside a sketch shape: the middle of the first span a horizontal line through the
    polygon's middle (nudged off its corners) crosses; arcs as fine chords."""
    if shape["kind"] == "rect":
        return shape["x"] + shape["width"] / 2, shape["y"] + shape["height"] / 2
    if shape["kind"] == "circle":
        return shape["x"], shape["y"]
    points = flatten(shape, math.pi / 256)
    u0, v0, u1, v1 = extent(shape)
    v = (v0 + v1) / 2
    while any(abs(p[1] - v) < 1e-6 for p in points):
        v += (v1 - v0) * 1e-3
    crossings = sorted(a[0] + (v - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
                       for a, b in zip(points, points[1:] + points[:1]) if (a[1] < v) != (b[1] < v))
    return (crossings[0] + crossings[1]) / 2, v


def solve_edge(points, edge, length):
    """Sets straight polygon edge `edge` to `length` with FreeCAD's Sketcher (model.polygon's
    constraints, arcs keeping their sweeps): every corner is held except those tied to the edge's
    end (by horizontal or vertical lines) and not to its start. Returns the corners with the
    solved arcs' bulges."""
    import Sketcher
    if len(points[edge]) == 3:
        raise ValueError("Set an arc's height, not its length")
    n = len(points)
    doc = App.newDocument("CorvCADSketchSolve", hidden=True)
    try:
        sketch = doc.addObject("Sketcher::SketchObject", "Solve")
        ends = model.polygon(sketch, points, edge=edge)
        sketch.addConstraint(Sketcher.Constraint("Distance", ends[edge][0], length))
        if sketch.solve() != 0:
            raise ValueError("FreeCAD's Sketcher found no polygon with that edge length")
        result = []
        for g, start, _ in ends:
            corner = sketch.getPoint(g, start)
            result.append([float(corner.x), float(corner.y)])
            if len(points[len(result) - 1]) == 3:
                arc = sketch.Geometry[g]
                bulge = math.tan((arc.LastParameter - arc.FirstParameter) / 4)  # a half circle may solve a hair over 1
                result[-1].append(min(bulge, 1.0) * (1 if start == 1 else -1))
    finally:
        App.closeDocument(doc.Name)
    end = (edge + 1) % n
    numeric_close(math.hypot(result[end][0] - result[edge][0], result[end][1] - result[edge][1]), length)
    return sketch_shape({"kind": "polygon", "points": result})["points"]

class Session:
    def __init__(self, document_path, input_json, initial_settings):
        self._doc = None
        self._owned_path = None
        self._undo, self._redo, self._exports = [], [], {}
        raw_path = Path(document_path).absolute()
        if raw_path.is_symlink():
            raise ValueError("Document symlinks are not writable session paths")
        self._path = raw_path.resolve()
        if self._path.suffix.lower() != ".fcstd" or not self._path.parent.is_dir():
            raise ValueError("Expected an FCStd path in an existing directory")
        self._data = json.loads(input_json)
        if not isinstance(self._data, dict) or not isinstance(self._data.get("source"), dict):
            raise ValueError("Expected F1 board source provenance")
        if not re.fullmatch(r"[0-9a-f]{64}", str(self._data["source"].get("sha256", ""))):
            raise ValueError("Expected a lowercase board SHA256")
        self._input = _canonical(self._data)
        self._identity = _mechanical_identity(self._data)
        self._file_hash = _hash(self._path) if self._path.exists() else None
        key = os.path.normcase(str(self._path))
        if key in _OPEN_PATHS or any(
                doc.FileName and os.path.normcase(str(Path(doc.FileName).resolve())) == key
                for doc in App.listDocuments().values()):
            raise ValueError("Enclosure document is already open")
        _OPEN_PATHS.add(key)
        self._owned_path = key
        template = None
        try:
            if self._file_hash is None:
                self._settings = validate(self._data, initial_settings)
                self._cutouts, self._vents, self._screws, self._sketches = [], [], None, []
                self._doc = _build(self._data, self._settings)
                self._signature = _signature(self._doc)
            else:
                self._doc = App.openDocument(str(self._path))
                if self._file_hash != _hash(self._path):
                    raise ValueError("Document changed while opening")
                self._verify_provenance()
                self._settings = validate(self._data, self._read_settings())
                self._cutouts, self._vents, self._screws, self._sketches = self._read_openings()
                template = _build(self._data, self._settings, *self._state()[1:])
                self._signature = _signature(template)
                actual = _signature(self._doc)
                if any(actual.get(name) != value for name, value in self._signature.items()):
                    raise ValueError("Partial or incompatible enclosure model structure")
            self._doc.UndoMode = 1
            self._check_document()
        except Exception:
            self.close()
            raise
        finally:
            if template is not None:
                App.closeDocument(template.Name)

    def _require(self):
        if self._doc is None:
            raise ValueError("Mechanical session is closed")
        return self._doc

    def _verify_provenance(self):
        provenance = self._require().getObject("InputProvenance")
        if provenance is None:
            raise ValueError("Board provenance mismatch; rebuild the enclosure explicitly")
        if getattr(provenance, "Scope", "") != "F1 rectangular prototype; plain through bores; no lid fastening":
            raise ValueError("Unrecognized enclosure model provenance")
        schema = getattr(provenance, "CorvCADSessionSchema", None)
        stored = getattr(provenance, "CorvCADBoardInput", None)
        if (schema is None) != (stored is None) or schema not in (None, "1"):
            raise ValueError("Partial or unsupported mechanical session metadata")
        if stored is None:
            # Legacy F1 files do not retain board identity/geometry metadata;
            # their matching whole-board hash is required for safe adoption.
            if getattr(provenance, "BoardSHA256", None) != self._data["source"]["sha256"]:
                raise ValueError("Legacy board provenance mismatch; validate and save with the original board first")
        else:
            snapshot = json.loads(stored)
            if snapshot["source"]["sha256"] != getattr(provenance, "BoardSHA256", None):
                raise ValueError("Inconsistent saved board provenance")
            if _mechanical_identity(snapshot) != self._identity:
                raise ValueError("Board mechanical input mismatch; rebuild the enclosure explicitly")

    def _read_value(self, alias):
        sheet = self._require().getObject("Parameters")
        if sheet is None or sheet.TypeId != "Spreadsheet::Sheet":
            raise ValueError("Missing enclosure Parameters spreadsheet")
        cell = sheet.getCellFromAlias(alias)
        if not cell:
            raise ValueError("Missing enclosure parameter alias: " + alias)
        value = sheet.getPropertyByName(cell)
        # Quantities must have length dimensions; a plain scalar is not mm.
        if not isinstance(value, App.Units.Quantity) or value.Unit != App.Units.Quantity("1 mm").Unit:
            raise ValueError("Expected millimetre parameter: " + alias)
        return float(value.Value)

    def _read_settings(self):
        return {name: self._read_value(_alias(name)) for name in DEFAULTS}

    def _read_openings(self):
        """The document's wall cutouts (sides in InputProvenance.CorvCADCutouts, dimensions in the
        Parameters spreadsheet) and vent rows (InputProvenance.CorvCADVents); absent: none."""
        provenance = self._require().InputProvenance
        sides = json.loads(getattr(provenance, "CorvCADCutouts", "") or "[]")
        vents = json.loads(getattr(provenance, "CorvCADVents", "") or "[]")
        screws = json.loads(getattr(provenance, "CorvCADScrews", "") or "null")
        sketches = json.loads(getattr(provenance, "CorvCADSketches", "") or "[]")
        if not isinstance(sides, list) or len(sides) > MAX_CUTOUTS or not isinstance(vents, list):
            raise ValueError("Unsupported enclosure opening metadata")
        cutouts = [{"side": side, **{field.lower(): self._read_value(f"Cutout{index}{field}")
                                     for field in model.CUTOUT_FIELDS}}
                   for index, side in enumerate(sides, 1)]
        return model.openings(self._data, self._settings, cutouts, vents, screws, sketches)

    def _sync_openings(self, cutouts, vents, screws, sketches):
        """Replaces the cutout, vent and lid screw features and cells in place, in a fresh build's
        order; other objects stay."""
        doc = self._require()
        tray, sheet = doc.Tray, doc.Parameters
        for body in (tray, doc.Lid):
            for obj in reversed([o for o in body.Group if o.Name.startswith(("Cutout", "Vent", "ScrewPost", "ScrewHole", "LidScrewHole", "UserSketch")) and
                                 o.TypeId in ("PartDesign::SubtractiveBox", "PartDesign::LinearPattern",
                                              "PartDesign::AdditiveBox", "PartDesign::SubtractiveCylinder",
                                              "PartDesign::Pad", "PartDesign::Pocket", "Sketcher::SketchObject")]):
                body.removeObject(obj)
                doc.removeObject(obj.Name)
        for index in range(1, MAX_CUTOUTS + 1):
            for k in range(len(model.CUTOUT_FIELDS)):
                row = model.CUTOUT_ROW + 4 * (index - 1) + k
                if sheet.getAlias(f"B{row}"):
                    sheet.setAlias(f"B{row}", "")
                sheet.clear(f"A{row}")
                sheet.clear(f"B{row}")
        for index, item in enumerate(cutouts, 1):
            model.cutout_cells(sheet, index, item)
        model.recompute(doc)
        for index, item in enumerate(cutouts, 1):
            model.cutout(tray, index, item["side"])
        for body in (tray, doc.Lid):
            body.Tip = [o for o in body.Group if o.isDerivedFrom("PartDesign::Feature")][-1]
        for index, item in enumerate(vents, 1):
            model.vent(doc.Lid if item["face"] == "lid" else tray, index, item)
        if screws:
            model.fastening(tray, doc.Lid, self._data, self._settings, screws)
        for index, item in enumerate(sketches, 1):
            model.sketch_feature(doc.Lid if item["face"] == "lid" else tray, index, item)
        provenance = doc.InputProvenance
        for name, value in (("CorvCADCutouts", [item["side"] for item in cutouts]), ("CorvCADVents", vents),
                            ("CorvCADScrews", screws), ("CorvCADSketches", sketches)):
            if name not in provenance.PropertiesList:
                provenance.addProperty("App::PropertyString", name)
            setattr(provenance, name, json.dumps(value, sort_keys=True))

    def _openings(self):
        """(body, point) in the middle of every cutout and vent slot, halfway through its wall or
        the lid: each must be empty."""
        x0, y0, x1, y1 = self._data["bounds"]
        s = self._settings
        outer, height = s["margin"] + s["wall"], tray_height(self._data, s)
        wall = {"front": y0 - outer + s["wall"]/2, "back": y1 + outer - s["wall"]/2,
                "left": x0 - outer + s["wall"]/2, "right": x1 + outer - s["wall"]/2}

        def on_wall(side, along, bottom, top):
            z = (bottom + min(top, height)) / 2
            return ("Tray", App.Vector(along, wall[side], z) if side in ("front", "back") else App.Vector(wall[side], along, z))
        points = [on_wall(c["side"], c["center"], c["bottom"], c["bottom"] + c["height"]) for c in self._cutouts]
        for v in self._vents:
            for low, high in vent_slots(v):
                if v["face"] == "lid":
                    points.append(("Lid", App.Vector((low+high)/2, v["across"], height + s["lid_gap"] + s["lid_thickness"]/2)))
                else:
                    points.append(on_wall(v["face"], (low+high)/2, v["across"], v["across"] + v["length"]))
        if self._screws:
            g = fastening_geometry(self._data, s, self._screws)
            for cx, cy in g["centers"]:
                points += [("Tray", App.Vector(cx, cy, height - g["depth"]/2)),
                           ("Lid", App.Vector(cx, cy, height + s["lid_gap"] + s["lid_thickness"]/2))]
        return points + [point for point, cut in self._sketch_points() if cut]

    def _sketch_points(self):
        """((body, point), cut) inside every sketch shape, halfway through its cut or extrusion."""
        x0, y0, x1, y1 = self._data["bounds"]
        s = self._settings
        outer, top = s["margin"] + s["wall"], tray_height(self._data, s) + s["lid_gap"] + s["lid_thickness"]
        result = []
        for sk in self._sketches:
            cut = sk["operation"] == "cut"
            w = (thickness(s, sk["face"]) if sk["depth"] is None else sk["depth"]) / 2 * (1 if cut else -1)
            for shape in sk["shapes"]:
                u, v = _inside(shape)
                point = {"lid": (u, v, top - w), "front": (u, y0 - outer + w, v), "back": (u, y1 + outer - w, v),
                         "left": (x0 - outer + w, u, v), "right": (x1 + outer - w, u, v)}[sk["face"]]
                result.append((("Lid" if sk["face"] == "lid" else "Tray", App.Vector(*point)), cut))
        return result

    def _posts(self):
        """Points in each lid screw post beside its hole, 1 mm below the rim: each must be solid."""
        if not self._screws:
            return []
        g = fastening_geometry(self._data, self._settings, self._screws)
        z = tray_height(self._data, self._settings) - 1
        return [App.Vector(cx + (g["hole"]/2 + g["post"]/2) / 2, cy, z) for cx, cy in g["centers"]]

    def _check_document(self):
        doc = self._require()
        self._verify_provenance()
        model.recompute(doc)
        actual = _signature(doc)
        if any(actual.get(name) != value for name, value in self._signature.items()):
            raise ValueError("Model-owned structure was modified")
        if self._read_settings() != self._settings:
            raise ValueError("Enclosure parameters changed outside this session")
        if self._read_openings() != self._state()[1:]:
            raise ValueError("Enclosure openings changed outside this session")
        x0, y0, x1, y1 = self._data["bounds"]
        board = {"XMin": x0, "YMin": y0, "BoardWidth": x1-x0, "BoardDepth": y1-y0,
                 "BoardThickness": self._data["board_thickness"]}
        board.update({f"Bore{i+1}": hole["diameter"] for i, hole in enumerate(self._data["mounting_holes"])})
        for name, value in board.items():
            numeric_close(self._read_value(name), value)
        targets = expected(self._data, *self._state())
        for name in ("Tray", "Lid"):
            _shape_check(doc.getObject(name).Shape, targets[name])
        for body, point in self._openings():
            if doc.getObject(body).Shape.isInside(point, 1e-7, True):
                raise ValueError("An opening or screw hole is not open")
        for point in self._posts():
            if not doc.Tray.Shape.isInside(point, 1e-7, False):
                raise ValueError("A lid screw post is missing")
        for (body, point), cut in self._sketch_points():
            if not cut and not doc.getObject(body).Shape.isInside(point, 1e-7, False):
                raise ValueError("A sketch extrusion is missing")
        for obj in doc.Objects:
            if obj.Name in self._signature and obj.TypeId == "Sketcher::SketchObject" and not obj.FullyConstrained:
                raise ValueError("Underconstrained enclosure sketch: " + obj.Name)
        tray = doc.Tray.Shape
        for hole in self._data["mounting_holes"]:
            radius = hole["diameter"] / 2
            x, y = hole["x"], hole["y"]
            height = self._settings["floor"] + self._settings["standoff_height"]
            numeric_close(tray.common(Part.makeCylinder(radius-min(0.01, radius/100), height, App.Vector(x, y, 0))).Volume, 0)
            for z in (self._settings["floor"]/2, self._settings["floor"]+self._settings["standoff_height"]/2):
                for dx, dy in ((radius+0.2, 0), (0, radius+0.2), (-radius-0.2, 0), (0, -radius-0.2)):
                    if not tray.isInside(App.Vector(x+dx, y+dy, z), 1e-7, False):
                        raise ValueError("Missing enclosure bore surround")
        if tray.common(doc.Lid.Shape).Volume > 1e-7:
            raise ValueError("Tray and lid overlap")

    def get_settings(self):
        self._require()
        return dict(self._settings)

    def _state(self):
        return (dict(self._settings), [dict(c) for c in self._cutouts], [dict(v) for v in self._vents],
                dict(self._screws) if self._screws else None, json.loads(json.dumps(self._sketches)))

    def _apply(self, settings, cutouts=None, vents=None, screws=_KEEP, sketches=None):
        self._check_document()
        candidate = validate(self._data, settings)
        openings = model.openings(
            self._data, candidate, self._cutouts if cutouts is None else cutouts,
            self._vents if vents is None else vents, self._screws if screws is _KEEP else screws,
            self._sketches if sketches is None else sketches)
        # Cutout dimensions live in spreadsheet cells written with 12 significant digits
        # (model.cutout_cells): keep them at that precision, or the read-back differs
        # (13.000600000000006 -> 13.0006) and the document looks changed outside the session.
        openings = ([{k: float(f"{v:.12g}") if isinstance(v, float) else v for k, v in c.items()}
                     for c in openings[0]], *openings[1:])
        previous = (self._settings, self._cutouts, self._vents, self._screws, self._sketches, self._signature)
        # Changed openings change the structure: a fresh build is the reference for it. So does a
        # settings change that moves the lid screws' hole depth (min(8 mm, tray height - floor -
        # 0.5 mm)), which the screw holes carry as a number, not an expression.
        depth = lambda s: fastening_geometry(self._data, s, openings[2])["depth"] if openings[2] else None
        changed = (openings != (self._cutouts, self._vents, self._screws, self._sketches)
                   or depth(candidate) != depth(self._settings))
        template = _build(self._data, candidate, *openings) if changed else None
        doc = self._require()
        doc.openTransaction("CorvCAD enclosure")
        try:
            for name, value in candidate.items():
                doc.Parameters.set(doc.Parameters.getCellFromAlias(_alias(name)), f"={value:.17g} mm")
            self._settings = candidate
            if template is not None:
                self._sync_openings(*openings)
                self._cutouts, self._vents, self._screws, self._sketches = openings
                self._signature = _signature(template)
            self._check_document()
            doc.commitTransaction()
        except Exception:
            doc.abortTransaction()
            self._settings, self._cutouts, self._vents, self._screws, self._sketches, self._signature = previous
            model.recompute(doc)
            self._check_document()
            raise
        finally:
            if template is not None:
                App.closeDocument(template.Name)

    def set_parameter(self, name, value):
        self._require()
        if name not in DEFAULTS:
            raise ValueError("Unknown enclosure setting: " + str(name))
        candidate = validate(self._data, {**self._settings, name: value})
        if candidate == self._settings:
            return
        previous = self._state()
        self._apply(candidate)
        self._undo.append(previous)
        self._redo.clear()

    def set_openings(self, cutouts=None, vents=None, screws=_KEEP, settings=None, sketches=None):
        """One undoable step: openings and sketches, and any settings they need (lid screws raise
        the margin)."""
        self._require()
        candidate = validate(self._data, {**self._settings, **(settings or {})})
        openings = model.openings(
            self._data, candidate, self._cutouts if cutouts is None else cutouts,
            self._vents if vents is None else vents, self._screws if screws is _KEEP else screws,
            self._sketches if sketches is None else sketches)
        if (candidate, *openings) == self._state():
            return
        previous = self._state()
        self._apply(candidate, *openings)
        self._undo.append(previous)
        self._redo.clear()

    def set_cutouts(self, cutouts):
        self.set_openings(cutouts=cutouts)

    def set_vents(self, vents):
        self.set_openings(vents=vents)

    def recompute(self):
        self._check_document()

    def can_undo(self):
        self._require()
        return bool(self._undo)

    def can_redo(self):
        self._require()
        return bool(self._redo)

    def undo(self):
        self._require()
        if not self._undo:
            raise ValueError("No enclosure parameter change to undo")
        previous = self._state()
        self._apply(*self._undo[-1])
        self._undo.pop()
        self._redo.append(previous)

    def redo(self):
        self._require()
        if not self._redo:
            raise ValueError("No enclosure parameter change to redo")
        previous = self._state()
        self._apply(*self._redo[-1])
        self._redo.pop()
        self._undo.append(previous)

    def _part(self, part):
        self._require()
        if isinstance(part, bool) or not isinstance(part, int) or part not in (0, 1):
            raise ValueError("Expected part 0 (tray) or 1 (lid)")
        return self._doc.Tray if part == 0 else self._doc.Lid

    def _mesh(self, part):
        self._check_document()
        shape = self._part(part).Shape
        vertices, faces = shape.tessellate(0.05)
        points = [tuple(float(v) for v in point) for point in vertices]
        report = manifold(points, faces)
        numeric_close(report["signed_volume_mm3"], shape.Volume, max(0.1, shape.Volume * 0.005))
        return points, faces

    def triangles(self, part):
        points, faces = self._mesh(part)
        return [tuple(coordinate for index in face for coordinate in points[index]) for face in faces]

    def _publish(self, path, writer, expected_hash):
        path = Path(path).absolute()
        if path.is_symlink():
            raise ValueError("Refusing to overwrite a symlink")
        path = path.resolve()
        if not path.parent.is_dir():
            raise ValueError("Output directory does not exist")
        if path.exists() and (expected_hash is None or _hash(path) != expected_hash):
            raise ValueError("Refusing unknown or externally modified output: " + str(path))
        if not path.exists() and expected_hash is not None:
            raise ValueError("Previously owned output was removed externally")
        descriptor, temporary = tempfile.mkstemp(prefix=".corvcad-", suffix=path.suffix, dir=path.parent)
        os.close(descriptor)
        temporary = Path(temporary)
        # FreeCAD makes timestamped backup files when saveCopy replaces an
        # existing file. Give it this unique, absent temporary destination.
        temporary.unlink()
        try:
            writer(temporary)
            if not temporary.is_file() or temporary.stat().st_size == 0:
                raise ValueError("Empty mechanical output")
            with temporary.open("r+b") as stream:
                os.fsync(stream.fileno())
            if expected_hash is None:
                # Atomic create without replacing a file created during export.
                os.link(temporary, path)
            else:
                if _hash(path) != expected_hash:
                    raise ValueError("Output changed during mechanical write")
                os.replace(temporary, path)
            return path, _hash(path)
        finally:
            temporary.unlink(missing_ok=True)

    def save(self):
        self._check_document()
        provenance = self._doc.InputProvenance
        names = ("BoardSHA256", "CorvCADSessionSchema", "CorvCADBoardInput")
        previous = {name: getattr(provenance, name) for name in names if name in provenance.PropertiesList}

        def writer(path):
            # Retain the last validated whole-board snapshot only on explicit
            # save. SHA-only electronics revisions do not invalidate geometry.
            for name, value in zip(names, (self._data["source"]["sha256"], "1", self._input)):
                if name not in provenance.PropertiesList:
                    provenance.addProperty("App::PropertyString", name)
                setattr(provenance, name, value)
            self._doc.saveCopy(str(path))
        try:
            _, self._file_hash = self._publish(self._path, writer, self._file_hash)
        except Exception:
            for name in names:
                if name in previous:
                    setattr(provenance, name, previous[name])
                elif name in provenance.PropertiesList:
                    provenance.removeProperty(name)
            raise

    def _export(self, part, path, kind):
        self._check_document()
        body = self._part(part)
        path = Path(path).absolute()
        if path.is_symlink() or path.resolve() == self._path:
            raise ValueError("Export path conflicts with document or symlink")
        path = path.resolve()
        suffixes = {"step": (".step", ".stp"), "stl": (".stl",)}
        if path.suffix.lower() not in suffixes[kind]:
            raise ValueError("Wrong mechanical export extension")
        previous = self._exports.get(path)
        if previous and previous[0] != (part, kind):
            raise ValueError("Output belongs to another part or format")

        def writer(temporary):
            if kind == "step":
                Import.export([body], str(temporary))
                imported = Part.Shape()
                imported.read(str(temporary))
                _shape_check(imported, expected(self._data, *self._state())[body.Name])
            else:
                points, faces = self._mesh(part)
                mesh = Mesh.Mesh([[App.Vector(*points[index]) for index in face] for face in faces])
                mesh.write(str(temporary))
                loaded = Mesh.Mesh(str(temporary))
                if not loaded.isSolid() or loaded.hasNonManifolds() or loaded.hasSelfIntersections():
                    raise ValueError("STL reload is not a valid closed mesh")
                manifold(*loaded.Topology)
        _, checksum = self._publish(path, writer, previous[1] if previous else None)
        self._exports[path] = ((part, kind), checksum)

    # cc_mechanical_call_json: method name -> handler taking the request's JSON object.
    _JSON_METHODS = ("features", "openings", "set_openings", "solve_sketch")

    def _json_openings(self):
        _, cutouts, vents, screws, sketches = self._state()
        posts = {f"{screw}{'-insert' if insert else ''}":
                 fastening_geometry(self._data, self._settings, {"screw": screw, "insert": insert})["post"]
                 for screw in ("M2", "M2.5", "M3") for insert in (False, True)}
        return {"cutouts": cutouts, "vents": vents, "screws": screws, "screw_posts": posts, "sketches": sketches,
                "tray_height": tray_height(self._data, self._settings)}

    def _json_set_openings(self, cutouts=None, vents=None, settings=None, sketches=None, **rest):
        """`screws`: absent keeps them, null removes them."""
        if set(rest) - {"screws"}:
            raise ValueError("Unknown opening request: " + ", ".join(sorted(set(rest) - {"screws"})))
        self.set_openings(cutouts, vents, rest.get("screws", _KEEP), settings, sketches)
        return self._json_openings()

    def _json_solve_sketch(self, points, edge, length):
        """A polygon edge set to `length` by FreeCAD's Sketcher: the corners keep their horizontal
        and vertical edges, and only the corners tied to the edge's end may move."""
        self._require()
        shape = sketch_shape({"kind": "polygon", "points": points})
        if isinstance(edge, bool) or not isinstance(edge, int) or not 0 <= edge < len(shape["points"]):
            raise ValueError("Expected an edge of the polygon")
        if isinstance(length, bool) or not isinstance(length, (int, float)) or not 0.5 <= length <= 1000:
            raise ValueError("An edge is 0.5 to 1000 mm long")
        return {"points": solve_edge(shape["points"], edge, float(length))}

    def call_json(self, method, arguments):
        self._require()
        if method not in self._JSON_METHODS:
            raise ValueError("Unknown mechanical JSON method: " + str(method))
        request = json.loads(arguments)
        if not isinstance(request, dict):
            raise ValueError("Expected a JSON object of arguments")
        return json.dumps(getattr(self, "_json_" + method)(**request), sort_keys=True, allow_nan=False)

    def _json_features(self):
        """The bodies' PartDesign features in tree order, with what drives them."""
        doc = self._require()
        features = []
        for body in (doc.Tray, doc.Lid):
            for obj in body.Group:
                if not obj.TypeId.startswith("PartDesign::") or obj.TypeId == "PartDesign::Body":
                    continue
                profile = getattr(obj, "Profile", None)
                if isinstance(profile, (tuple, list)):
                    profile = profile[0] if profile else None
                features.append({"body": body.Name, "name": obj.Name, "type": obj.TypeId.split("::", 1)[1],
                                 "label": obj.Label, "suppressed": bool(getattr(obj, "Suppressed", False)),
                                 "profile": getattr(profile, "Name", None),
                                 "expressions": {str(a): str(b) for a, b in getattr(obj, "ExpressionEngine", ())}})
        return {"features": features}

    def export_step(self, part, path):
        self._export(part, path, "step")

    def export_stl(self, part, path):
        self._export(part, path, "stl")

    def close(self):
        try:
            if self._doc is not None:
                name = self._doc.Name
                self._doc = None
                App.closeDocument(name)
        finally:
            if self._owned_path is not None:
                _OPEN_PATHS.discard(self._owned_path)
                self._owned_path = None
