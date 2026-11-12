"""F1 only: an editable headless PartDesign model, independent of the iPad app.

CorvCAD-authored code, MIT. Uses FreeCAD's public Python API; no upstream source
is copied. Pocket hollows the pad so floor and wall can vary independently.
No generated mesh is ever used to construct a STEP shape.
"""
import FreeCAD as App
import Part
import Sketcher
import PartDesign  # registers the App-side feature types
import Spreadsheet

import math

from inputs import arc, fastening_geometry, validate, validate_openings, vent_slots


def recompute(doc):
    doc.recompute()
    failed = [(o.Name, o.getStatusString()) for o in doc.Objects if "Invalid" in o.State]
    if failed:
        raise RuntimeError(f"FreeCAD recompute failed: {failed}")


def parameters(doc, data, settings):
    sheet = doc.addObject("Spreadsheet::Sheet", "Parameters")
    x0, y0, x1, y1 = data["bounds"]
    entries = dict(XMin=x0, YMin=y0, BoardWidth=x1-x0, BoardDepth=y1-y0,
                   BoardThickness=data["board_thickness"],
                   **{"".join(word.title() for word in key.split("_")): value
                      for key, value in settings.items()})
    for i, hole in enumerate(data["mounting_holes"]):
        entries[f"Bore{i+1}"] = hole["diameter"]
    for row, (alias, value) in enumerate(entries.items(), 1):
        sheet.set(f"A{row}", alias)
        # A leading minus with units can be stored as text without '='.
        sheet.set(f"B{row}", f"={value:.12g} mm")
        sheet.setAlias(f"B{row}", alias)
    recompute(doc)
    return sheet


HEIGHT = "Parameters.Floor + Parameters.StandoffHeight + Parameters.BoardClearance + Parameters.BoardThickness"
OUTER_X = "Parameters.XMin - Parameters.Margin - Parameters.Wall"
OUTER_Y = "Parameters.YMin - Parameters.Margin - Parameters.Wall"
WIDTH = "Parameters.BoardWidth + 2 * (Parameters.Margin + Parameters.Wall)"
DEPTH = "Parameters.BoardDepth + 2 * (Parameters.Margin + Parameters.Wall)"


def rectangle(body, name, width, depth, x, y, z):
    sketch = body.newObject("Sketcher::SketchObject", name)
    points = [(0, 0), (10, 0), (10, 10), (0, 10)]
    for a, b in zip(points, points[1:] + points[:1]):
        sketch.addGeometry(Part.LineSegment(App.Vector(*a, 0), App.Vector(*b, 0)), False)
    for i in range(4):
        sketch.addConstraint(Sketcher.Constraint("Coincident", i, 2, (i+1) % 4, 1))
        sketch.addConstraint(Sketcher.Constraint("Horizontal" if i % 2 == 0 else "Vertical", i))
    sketch.addConstraint(Sketcher.Constraint("Coincident", 0, 1, -1, 1))
    for geometry, expression in ((0, width), (1, depth)):
        index = sketch.addConstraint(Sketcher.Constraint("Distance", geometry, 10.0))
        sketch.setExpression(f"Constraints[{index}]", expression)
    for axis, expression in zip("xyz", (x, y, z)):
        sketch.setExpression(f"Placement.Base.{axis}", expression)
    return sketch


def circles(body, name, data, z, bosses):
    sketch = body.newObject("Sketcher::SketchObject", name)
    sketch.setExpression("Placement.Base.z", z)
    for i, hole in enumerate(data["mounting_holes"]):
        n = sketch.addGeometry(Part.Circle(App.Vector(hole["x"], hole["y"], 0),
                                          App.Vector(0, 0, 1), 1), False)
        for axis, value in (("X", hole["x"]), ("Y", hole["y"])):
            if value == 0:
                sketch.addConstraint(Sketcher.Constraint("Distance" + axis, n, 3, 0.0))
            else:
                sketch.addConstraint(Sketcher.Constraint("Distance" + axis, n, 3, value))
        index = sketch.addConstraint(Sketcher.Constraint("Diameter", n, 2.0))
        sketch.setExpression(f"Constraints[{index}]",
                             "Parameters.StandoffDiameter" if bosses else f"Parameters.Bore{i+1}")
    return sketch


CUTOUT_ROW = 100  # Cutout i's cells: rows 100 + 4 (i - 1) onward, labels in A, values in B.
CUTOUT_FIELDS = ("Center", "Width", "Bottom", "Height")


def cutout_cells(sheet, index, cutout):
    for k, field in enumerate(CUTOUT_FIELDS):
        row = CUTOUT_ROW + 4 * (index - 1) + k
        sheet.set(f"A{row}", f"Cutout{index}{field}")
        sheet.set(f"B{row}", f"={cutout[field.lower()]:.12g} mm")
        sheet.setAlias(f"B{row}", f"Cutout{index}{field}")


def cutout(tray, index, side):
    """A box through one side wall: from 1 mm outside it to its inner face, so it removes exactly
    the wall's thickness wherever the wall moves."""
    box = tray.newObject("PartDesign::SubtractiveBox", f"Cutout{index}")
    if "MapMode" in box.PropertiesList:
        box.MapMode = "Deactivated"
    p = f"Parameters.Cutout{index}"
    along, through = f"{p}Center - {p}Width / 2", "Parameters.Wall + 1 mm"
    if side in ("front", "back"):
        x = along
        y = f"{OUTER_Y} - 1 mm" if side == "front" else f"{OUTER_Y} + {DEPTH} - Parameters.Wall"
        length, width = f"{p}Width", through
    else:
        x = f"{OUTER_X} - 1 mm" if side == "left" else f"{OUTER_X} + {WIDTH} - Parameters.Wall"
        y = along
        length, width = through, f"{p}Width"
    for name, expression in (("Length", length), ("Width", width), ("Height", f"{p}Height"),
                             ("Placement.Base.x", x), ("Placement.Base.y", y), ("Placement.Base.z", f"{p}Bottom")):
        box.setExpression(name, expression)
    box.Refine = True
    recompute(tray.Document)
    return box


def vent(body, index, item):
    """A row of slots: the first slot as a box, repeated `count` times `pitch` apart by a
    LinearPattern. Wall slots run through the wall from `across` up; lid slots through the lid."""
    doc = body.Document
    slot = body.newObject("PartDesign::SubtractiveBox", f"Vent{index}")
    if "MapMode" in slot.PropertiesList:
        slot.MapMode = "Deactivated"
    start = f"{vent_slots(item)[0][0]:.12g} mm"
    width, length = f"{item['width']:.12g} mm", f"{item['length']:.12g} mm"
    through = "Parameters.Wall + 1 mm"
    if item["face"] == "lid":
        values = (("Length", width), ("Width", length), ("Height", "Parameters.LidThickness + 2 mm"),
                  ("Placement.Base.x", start), ("Placement.Base.y", f"{item['across'] - item['length']/2:.12g} mm"),
                  ("Placement.Base.z", f"{HEIGHT} + Parameters.LidGap - 1 mm"))
    elif item["face"] in ("front", "back"):
        y = f"{OUTER_Y} - 1 mm" if item["face"] == "front" else f"{OUTER_Y} + {DEPTH} - Parameters.Wall"
        values = (("Length", width), ("Width", through), ("Height", length),
                  ("Placement.Base.x", start), ("Placement.Base.y", y), ("Placement.Base.z", f"{item['across']:.12g} mm"))
    else:
        x = f"{OUTER_X} - 1 mm" if item["face"] == "left" else f"{OUTER_X} + {WIDTH} - Parameters.Wall"
        values = (("Length", through), ("Width", width), ("Height", length),
                  ("Placement.Base.x", x), ("Placement.Base.y", start), ("Placement.Base.z", f"{item['across']:.12g} mm"))
    for name, expression in values:
        slot.setExpression(name, expression)
    slot.Refine = True
    recompute(doc)
    last = slot
    if item["count"] > 1:
        pattern = body.newObject("PartDesign::LinearPattern", f"Vent{index}Pattern")
        pattern.Originals = [slot]
        axis = "X_Axis" if item["face"] in ("lid", "front", "back") else "Y_Axis"
        pattern.Direction = (next(o for o in body.Origin.OriginFeatures if o.Name.startswith(axis)), [""])
        pattern.Mode = "Extent"
        pattern.setExpression("Length", f"{(item['count'] - 1) * item['pitch']:.12g} mm")
        pattern.Occurrences = item["count"]
        pattern.Refine = True
        last = pattern
    body.Tip = last
    recompute(doc)
    return last


def corners(body, name, original, post):
    """`original` repeated at the cavity's four corners: a 2 x 2 LinearPattern whose extents are
    the cavity's size less one post."""
    pattern = body.newObject("PartDesign::LinearPattern", name)
    pattern.Originals = [original]
    axes = {o.Name[:6]: o for o in body.Origin.OriginFeatures}
    for suffix, axis, span in (("", "X_Axis", "Parameters.BoardWidth"), ("2", "Y_Axis", "Parameters.BoardDepth")):
        setattr(pattern, "Direction" + suffix, (axes[axis], [""]))
        setattr(pattern, "Mode" + suffix, "Extent")
        pattern.setExpression("Length" + suffix, f"{span} + 2 * Parameters.Margin - {post:.12g} mm")
        setattr(pattern, "Occurrences" + suffix, 2)
    pattern.Refine = True
    body.Tip = pattern
    recompute(body.Document)
    return pattern


def primitive(body, kind, name, values):
    obj = body.newObject(f"PartDesign::{kind}", name)
    if "MapMode" in obj.PropertiesList:
        obj.MapMode = "Deactivated"
    for prop, expression in values:
        obj.setExpression(prop, expression)
    obj.Refine = True
    recompute(body.Document)
    return obj


def fastening(tray, lid, data, settings, item):
    """Lid screws: a square post in each cavity corner from the floor to the rim with a blind hole
    from the rim (pilot or heat-set insert), and clearance holes through the lid above."""
    g = fastening_geometry(data, settings, item)
    corner_x, corner_y = "Parameters.XMin - Parameters.Margin", "Parameters.YMin - Parameters.Margin"
    post = primitive(tray, "AdditiveBox", "ScrewPost", (
        ("Length", f"{g['post']:.12g} mm"), ("Width", f"{g['post']:.12g} mm"), ("Height", f"{HEIGHT} - Parameters.Floor"),
        ("Placement.Base.x", corner_x), ("Placement.Base.y", corner_y), ("Placement.Base.z", "Parameters.Floor")))
    corners(tray, "ScrewPosts", post, g["post"])
    centre = (f"{corner_x} + {g['post']/2:.12g} mm", f"{corner_y} + {g['post']/2:.12g} mm")
    hole = primitive(tray, "SubtractiveCylinder", "ScrewHole", (
        ("Radius", f"{g['hole']/2:.12g} mm"), ("Height", f"{g['depth'] + 1:.12g} mm"),
        ("Placement.Base.x", centre[0]), ("Placement.Base.y", centre[1]),
        ("Placement.Base.z", f"{HEIGHT} - {g['depth']:.12g} mm")))
    corners(tray, "ScrewHoles", hole, g["post"])
    lid_hole = primitive(lid, "SubtractiveCylinder", "LidScrewHole", (
        ("Radius", f"{g['clear']/2:.12g} mm"), ("Height", "Parameters.LidThickness + 2 mm"),
        ("Placement.Base.x", centre[0]), ("Placement.Base.y", centre[1]),
        ("Placement.Base.z", f"{HEIGHT} + Parameters.LidGap - 1 mm")))
    corners(lid, "LidScrewHoles", lid_hole, g["post"])


# Sketch planes: local x, y are the face's u, v; `reverse` when the plane's normal points inward.
# Walls' planes are rotations of XY: front/back x->X y->Z (normal -Y), left/right x->Y y->Z (+X).
FACES = {"lid": (App.Rotation(), ("0 mm", "0 mm", f"{HEIGHT} + Parameters.LidGap + Parameters.LidThickness"), False),
         "front": (App.Rotation(App.Vector(1, 0, 0), 90), ("0 mm", OUTER_Y, "0 mm"), False),
         "back": (App.Rotation(App.Vector(1, 0, 0), 90), ("0 mm", f"{OUTER_Y} + {DEPTH}", "0 mm"), True),
         "left": (App.Rotation(App.Vector(1, 1, 1), 120), (OUTER_X, "0 mm", "0 mm"), True),
         "right": (App.Rotation(App.Vector(1, 1, 1), 120), (f"{OUTER_X} + {WIDTH}", "0 mm", "0 mm"), False)}


def _point(sketch, geometry, position, u, v):
    sketch.addConstraint(Sketcher.Constraint("DistanceX", geometry, position, u))
    sketch.addConstraint(Sketcher.Constraint("DistanceY", geometry, position, v))


def profile(sketch, shape):
    """One closed profile, fully constrained: rectangles by corner, width and height; circles by
    centre and diameter; polygons by polygon()."""
    if shape["kind"] == "circle":
        n = sketch.addGeometry(Part.Circle(App.Vector(shape["x"], shape["y"], 0), App.Vector(0, 0, 1), shape["diameter"] / 2), False)
        _point(sketch, n, 3, shape["x"], shape["y"])
        sketch.addConstraint(Sketcher.Constraint("Diameter", n, shape["diameter"]))
        return
    if shape["kind"] == "polygon":
        polygon(sketch, shape["points"])
        return
    x, y, w, h = shape["x"], shape["y"], shape["width"], shape["height"]
    first = polygon(sketch, [(x, y), (x + w, y), (x + w, y + h), (x, y + h)], fix=False)[0][0]
    sketch.addConstraint(Sketcher.Constraint("DistanceX", first, 1, first, 2, w))
    sketch.addConstraint(Sketcher.Constraint("DistanceY", first + 1, 1, first + 1, 2, h))
    _point(sketch, first, 1, x, y)


def polygon(sketch, points, fix=True, edge=None):
    """A closed loop of lines and arcs (corners [u, v] or [u, v, bulge]): coincident corners, the
    horizontal and vertical lines it was drawn with, each arc's sweep, and (`fix`) one x and one
    y distance for each set of corners those lines tie together. With `edge`, the sets holding that
    edge's end corner stay free unless they also hold its start. Returns (geometry, start position,
    end position) per edge: corner i is edge i's start."""
    n = len(points)
    ends = []
    for i, a in enumerate(points):
        b = points[(i+1) % n]
        if len(a) == 3:
            (cx, cy), r, start, sweep = arc(a, b)
            low, high = (start, start + sweep) if sweep > 0 else (start + sweep, start)
            g = sketch.addGeometry(Part.ArcOfCircle(Part.Circle(App.Vector(cx, cy, 0), App.Vector(0, 0, 1), r), low, high), False)
            # Its sweep, not its radius: with both ends held, a radius is degenerate at a half circle.
            sketch.addConstraint(Sketcher.Constraint("Angle", g, abs(sweep)))
            ends.append((g, 1, 2) if sweep > 0 else (g, 2, 1))  # Sketcher arcs run counterclockwise
        else:
            ends.append((sketch.addGeometry(Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False), 1, 2))
    for i in range(n):
        sketch.addConstraint(Sketcher.Constraint("Coincident", ends[i][0], ends[i][2], ends[(i+1) % n][0], ends[(i+1) % n][1]))
    # Union-find over corners sharing an x (vertical line) or a y (horizontal line).
    groups = {"x": list(range(n)), "y": list(range(n))}

    def root(axis, i):
        while groups[axis][i] != i:
            i = groups[axis][i]
        return i
    for i, a in enumerate(points):
        b = points[(i+1) % n]
        if len(a) == 3:
            continue
        for axis, same, kind in (("y", a[1] == b[1], "Horizontal"), ("x", a[0] == b[0], "Vertical")):
            if same:
                sketch.addConstraint(Sketcher.Constraint(kind, ends[i][0]))
                groups[axis][root(axis, (i+1) % n)] = root(axis, i)
    if fix:
        for axis, kind in (("x", "DistanceX"), ("y", "DistanceY")):
            loose = set() if edge is None else {root(axis, (edge + 1) % n)} - {root(axis, edge)}
            for i in range(n):
                if root(axis, i) == i and i not in loose:
                    sketch.addConstraint(Sketcher.Constraint(kind, ends[i][0], ends[i][1], points[i][0 if axis == "x" else 1]))
    return ends


def face(shape):
    """A sketch shape as a planar Part face in face coordinates."""
    if shape["kind"] == "circle":
        return Part.Face(Part.Wire(Part.makeCircle(shape["diameter"] / 2, App.Vector(shape["x"], shape["y"], 0))))
    if shape["kind"] == "rect":
        x, y, w, h = shape["x"], shape["y"], shape["width"], shape["height"]
        points = [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    else:
        points = shape["points"]
    edges = []
    for i, a in enumerate(points):
        b = points[(i+1) % len(points)]
        start, end = App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)
        if len(a) == 3:
            (cx, cy), r, angle, sweep = arc(a, b)
            middle = App.Vector(cx + r * math.cos(angle + sweep / 2), cy + r * math.sin(angle + sweep / 2), 0)
            edges.append(Part.Arc(start, middle, end).toShape())
        else:
            edges.append(Part.LineSegment(start, end).toShape())
    return Part.Face(Part.Wire(edges))


def separate(a, b):
    """Whether two sketch shapes (or opening boxes) are apart: no shared point, exactly."""
    return face(a).distToShape(face(b))[0] > 1e-6


def openings(data, settings, cutouts=(), vents=(), screws=None, sketches=()):
    """validate_openings with sketch shapes kept apart exactly."""
    return validate_openings(data, settings, cutouts, vents, screws, sketches, separate=separate)

def sketch_feature(body, index, item):
    """A user sketch on its face and the Pad (outward) or Pocket (inward, through the slab when
    `depth` is null) made from it."""
    rotation, base, reverse = FACES[item["face"]]
    sketch = body.newObject("Sketcher::SketchObject", f"UserSketch{index}")
    sketch.Placement = App.Placement(App.Vector(), rotation)
    for axis, expression in zip("xyz", base):
        sketch.setExpression(f"Placement.Base.{axis}", expression)
    for shape in item["shapes"]:
        profile(sketch, shape)
    cut = item["operation"] == "cut"
    depth = (("Parameters.LidThickness" if item["face"] == "lid" else "Parameters.Wall")
             if item["depth"] is None else f"{item['depth']:.12g} mm")
    obj = body.newObject("PartDesign::Pocket" if cut else "PartDesign::Pad", f"UserSketch{index}{'Cut' if cut else 'Extrude'}")
    obj.Profile = sketch
    obj.setExpression("Length", depth)
    obj.Reversed = reverse
    obj.Refine = True
    body.Tip = obj
    recompute(body.Document)
    return obj


def feature(body, kind, name, sketch, length):
    obj = body.newObject(f"PartDesign::{kind}", name)
    obj.Profile = sketch
    obj.setExpression("Length", length)
    obj.Refine = True
    recompute(body.Document)
    return obj


def build(data, settings=None, cutouts=(), vents=(), screws=None, sketches=()):
    settings = validate(data, settings)
    cutouts, vents, screws, sketches = openings(data, settings, cutouts, vents, screws, sketches)
    doc = App.newDocument("CorvCADEnclosure")
    sheet = parameters(doc, data, settings)
    for index, item in enumerate(cutouts, 1):
        cutout_cells(sheet, index, item)
    doc.addObject("App::FeaturePython", "InputProvenance")
    doc.InputProvenance.addProperty("App::PropertyString", "BoardSHA256")
    doc.InputProvenance.BoardSHA256 = data["source"]["sha256"]
    doc.InputProvenance.addProperty("App::PropertyString", "Scope")
    doc.InputProvenance.Scope = "F1 rectangular prototype; plain through bores; no lid fastening"

    tray = doc.addObject("PartDesign::Body", "Tray")
    outer = rectangle(tray, "OuterOutline", WIDTH, DEPTH, OUTER_X, OUTER_Y, "0 mm")
    feature(tray, "Pad", "OuterPad", outer, HEIGHT)
    inner = rectangle(tray, "CavityOutline", "Parameters.BoardWidth + 2 * Parameters.Margin",
                      "Parameters.BoardDepth + 2 * Parameters.Margin",
                      "Parameters.XMin - Parameters.Margin", "Parameters.YMin - Parameters.Margin", HEIGHT)
    feature(tray, "Pocket", "Cavity", inner,
            "Parameters.StandoffHeight + Parameters.BoardClearance + Parameters.BoardThickness")
    if data["mounting_holes"]:
        boss_profile = circles(tray, "BossLocations", data, "Parameters.Floor", True)
        feature(tray, "Pad", "BossPad", boss_profile, "Parameters.StandoffHeight")
        # One Hole feature per drill diameter, preserving mixed mounting-hole sizes.
        for i, hole in enumerate(data["mounting_holes"]):
            profile = circles(tray, f"BoreLocation{i+1}", {"mounting_holes": [hole]},
                              "Parameters.Floor + Parameters.StandoffHeight", False)
            profile.setExpression("Constraints[2]", f"Parameters.Bore{i+1}")
            bore = tray.newObject("PartDesign::Hole", f"MountingBore{i+1}")
            bore.Profile = profile
            bore.setExpression("Diameter", f"Parameters.Bore{i+1}")
            bore.DepthType = 1  # Through all: includes floor, plain clearance bore.
            bore.ThreadType = 0
            bore.HoleCutType = 0
            bore.DrillPoint = 0
            bore.Refine = True
            recompute(doc)
    for index, item in enumerate(cutouts, 1):
        cutout(tray, index, item["side"])
    for index, item in enumerate(vents, 1):
        if item["face"] != "lid":
            vent(tray, index, item)

    lid = doc.addObject("PartDesign::Body", "Lid")
    profile = rectangle(lid, "LidOutline", f"{WIDTH} - 2 * Parameters.LidGap",
                        f"{DEPTH} - 2 * Parameters.LidGap", f"{OUTER_X} + Parameters.LidGap",
                        f"{OUTER_Y} + Parameters.LidGap", f"{HEIGHT} + Parameters.LidGap")
    feature(lid, "Pad", "LidPad", profile, "Parameters.LidThickness")
    for index, item in enumerate(vents, 1):
        if item["face"] == "lid":
            vent(lid, index, item)
    if screws:
        fastening(tray, lid, data, settings, screws)
    for index, item in enumerate(sketches, 1):
        sketch_feature(lid if item["face"] == "lid" else tray, index, item)
    recompute(doc)
    # Save only final features as visible for a desktop open. No Gui module needed.
    for body in (tray, lid):
        for obj in body.Group:
            obj.Visibility = obj == body.Tip
    return doc
