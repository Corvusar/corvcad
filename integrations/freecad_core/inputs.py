"""Dependency-free validation of the experimental mechanical input contract."""
import math

DEFAULTS = dict(margin=3.0, wall=2.0, floor=2.0, board_clearance=2.0,
                standoff_height=4.0, standoff_diameter=6.0, lid_thickness=2.0, lid_gap=0.25)


def validate(data, settings=None):
    if data.get("schema") != 1 or data.get("units") != "mm":
        raise ValueError("Expected F1 schema 1 in millimetres")
    settings = {**DEFAULTS, **(settings or {})}
    if settings.keys() != DEFAULTS.keys():
        raise ValueError("Unknown enclosure setting")
    values = [*data["bounds"], data["board_thickness"], *settings.values()]
    for hole in data["mounting_holes"]:
        values += [hole["x"], hole["y"], hole["diameter"]]
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in values):
        raise ValueError("All dimensions must be finite numbers")
    if len(data["bounds"]) != 4:
        raise ValueError("Expected four board bounds")
    x0, y0, x1, y1 = data["bounds"]
    if x1 <= x0 or y1 <= y0 or data["board_thickness"] <= 0:
        raise ValueError("Empty board geometry")
    holes_present = bool(data["mounting_holes"])
    for key, value in settings.items():
        # No mounting holes (a mechanical-only box): no standoffs, so their height may be 0.
        zero_ok = ("margin", "board_clearance", "lid_gap") + (() if holes_present else ("standoff_height",))
        if value < 0 or (key not in zero_ok and value == 0):
            raise ValueError(f"Invalid dimension: {key}")
    if 2 * settings["lid_gap"] >= min(x1-x0, y1-y0) + 2*(settings["margin"]+settings["wall"]):
        raise ValueError("Lid gap consumes the lid")
    holes = data["mounting_holes"]
    for i, hole in enumerate(holes):
        r = settings["standoff_diameter"] / 2
        if hole["diameter"] <= 0 or hole["diameter"] + 1 > 2*r:
            raise ValueError("Bore must leave at least 0.5 mm radial boss wall")
        if not (x0 <= hole["x"] <= x1 and y0 <= hole["y"] <= y1):
            raise ValueError("Mounting hole outside board bounds")
        if (hole["x"]-r <= x0-settings["margin"] or hole["x"]+r >= x1+settings["margin"] or
                hole["y"]-r <= y0-settings["margin"] or hole["y"]+r >= y1+settings["margin"]):
            raise ValueError("F1 requires bosses wholly inside the cavity")
        for other in holes[:i]:
            if math.hypot(hole["x"]-other["x"], hole["y"]-other["y"]) <= 2*r:
                raise ValueError("Overlapping bosses are outside F1")
    return settings


SIDES = ("front", "back", "left", "right")  # y min, y max, x min, x max
CUTOUT_KEYS = ("side", "center", "width", "bottom", "height")
MAX_CUTOUTS = 12


def tray_height(data, settings):
    return settings["floor"] + settings["standoff_height"] + settings["board_clearance"] + data["board_thickness"]


def validate_cutouts(data, settings, cutouts):
    """Wall cutouts: a box through one side wall, within the cavity's span along that wall (never
    a corner), from `bottom` up, open at the top when it reaches the tray's rim. Millimetres, in
    the board's coordinates; `center` runs along the wall (x for front/back, y for left/right)."""
    if not isinstance(cutouts, (list, tuple)) or len(cutouts) > MAX_CUTOUTS:
        raise ValueError(f"Expected at most {MAX_CUTOUTS} cutouts")
    x0, y0, x1, y1 = data["bounds"]
    margin, height = settings["margin"], tray_height(data, settings)
    result = []
    for cutout in cutouts:
        if not isinstance(cutout, dict) or set(cutout) != set(CUTOUT_KEYS) or cutout["side"] not in SIDES:
            raise ValueError("Expected a cutout with side (front, back, left or right), center, width, bottom and height")
        numbers = [cutout[key] for key in CUTOUT_KEYS[1:]]
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in numbers):
            raise ValueError("Cutout dimensions must be finite numbers")
        center, width, bottom, tall = (float(v) for v in numbers)
        low, high = (x0, x1) if cutout["side"] in ("front", "back") else (y0, y1)
        if width < 1 or tall < 1:
            raise ValueError("A cutout must be at least 1 mm wide and high")
        if center - width/2 < low - margin or center + width/2 > high + margin:
            raise ValueError("A cutout must stay within the cavity's span along its wall")
        if bottom < 0 or bottom > height - 1:
            raise ValueError("A cutout must start between the tray's bottom and 1 mm below its rim")
        result.append({"side": cutout["side"], "center": center, "width": width, "bottom": bottom, "height": tall})
    return result


VENT_FACES = ("lid",) + SIDES
VENT_KEYS = ("face", "center", "across", "count", "width", "length", "pitch")
MAX_VENTS = 6


def vent_slots(vent):
    """The slots' (low, high) along the pattern: x for the lid, front and back, y for the sides."""
    span = (vent["count"] - 1) * vent["pitch"]
    first = vent["center"] - span / 2
    return [(first + k * vent["pitch"] - vent["width"] / 2, first + k * vent["pitch"] + vent["width"] / 2)
            for k in range(vent["count"])]


def validate_vents(data, settings, vents):
    """Vent slot rows. On a side wall: vertical slots through the wall, `across` their bottom and
    `length` their height (open at the rim when they reach it), within the cavity's span along the
    wall. On the lid: slots through the lid, `center` the row's x and `across` its y, `length`
    along y, at least 1 mm inside the lid's edge. `count` slots `pitch` apart (centre to centre)."""
    if not isinstance(vents, (list, tuple)) or len(vents) > MAX_VENTS:
        raise ValueError(f"Expected at most {MAX_VENTS} vent rows")
    x0, y0, x1, y1 = data["bounds"]
    s = settings
    margin, height, outer = s["margin"], tray_height(data, s), s["margin"] + s["wall"]
    result = []
    for vent in vents:
        if not isinstance(vent, dict) or set(vent) != set(VENT_KEYS) or vent["face"] not in VENT_FACES:
            raise ValueError("Expected a vent row with face (lid, front, back, left or right), center, across, count, width, length and pitch")
        count = vent["count"]
        if isinstance(count, bool) or not isinstance(count, (int, float)) or count != int(count) or not 1 <= count <= 40:
            raise ValueError("A vent row has 1 to 40 slots")
        numbers = [vent[key] for key in ("center", "across", "width", "length", "pitch")]
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in numbers):
            raise ValueError("Vent dimensions must be finite numbers")
        item = {"face": vent["face"], "count": int(count),
                **{key: float(vent[key]) for key in ("center", "across", "width", "length", "pitch")}}
        if item["width"] < 1 or item["length"] < 1:
            raise ValueError("Vent slots must be at least 1 mm wide and long")
        if item["count"] > 1 and item["pitch"] < item["width"] + 1:
            raise ValueError("Vent slots need at least 1 mm of material between them")
        slots = vent_slots(item)
        low, high = slots[0][0], slots[-1][1]
        if item["face"] == "lid":
            gap = s["lid_gap"] + 1
            if (low < x0 - outer + gap or high > x1 + outer - gap or
                    item["across"] - item["length"]/2 < y0 - outer + gap or item["across"] + item["length"]/2 > y1 + outer - gap):
                raise ValueError("Lid vents must stay at least 1 mm inside the lid's edge")
        else:
            along = (x0, x1) if item["face"] in ("front", "back") else (y0, y1)
            if low < along[0] - margin or high > along[1] + margin:
                raise ValueError("Wall vents must stay within the cavity's span along their wall")
            if item["across"] < 0 or item["across"] > height - 1:
                raise ValueError("Wall vents must start between the tray's bottom and 1 mm below its rim")
        result.append(item)
    return result


# Typical diameters, mm: pilot for a self-tapping screw in plastic, lid clearance, heat-set insert.
SCREWS = {"M2": dict(pilot=1.6, clear=2.4, insert=3.2),
          "M2.5": dict(pilot=2.0, clear=2.9, insert=3.6),
          "M3": dict(pilot=2.5, clear=3.4, insert=4.0)}


def fastening_geometry(data, settings, fastening):
    """Square corner posts (side `post`) from the floor to the rim in the cavity's corners, a hole
    of `hole` diameter and `depth` down from the rim in each, and `clear` holes through the lid
    above them; `centers` are the four posts' centres."""
    size = SCREWS[fastening["screw"]]
    hole = size["insert" if fastening["insert"] else "pilot"]
    post = max(6.0, math.ceil((hole + 3.2) * 2) / 2)
    x0, y0, x1, y1 = data["bounds"]
    m = settings["margin"]
    low, high = (x0 - m + post/2, y0 - m + post/2), (x1 + m - post/2, y1 + m - post/2)
    return {"post": post, "hole": hole, "clear": size["clear"],
            "depth": min(8.0, tray_height(data, settings) - settings["floor"] - 0.5),
            "centers": [(x, y) for y in (low[1], high[1]) for x in (low[0], high[0])]}


def validate_fastening(data, settings, fastening):
    """None, or lid screws {"screw": M2, M2.5 or M3, "insert": heat-set insert holes or pilots}."""
    if fastening is None:
        return None
    if (not isinstance(fastening, dict) or set(fastening) != {"screw", "insert"} or fastening["screw"] not in SCREWS
            or not isinstance(fastening["insert"], bool)):
        raise ValueError("Expected lid screws as screw (M2, M2.5 or M3) and insert (true or false)")
    item = {"screw": fastening["screw"], "insert": fastening["insert"]}
    g = fastening_geometry(data, settings, item)
    if settings["margin"] < g["post"]:
        raise ValueError(f"Lid screws need a board margin of at least {g['post']:g} mm for their corner posts")
    if g["depth"] < 3:
        raise ValueError("The tray is too shallow for lid screws")
    r = settings["standoff_diameter"] / 2
    for hole in data["mounting_holes"]:
        for cx, cy in g["centers"]:
            nearest = (min(max(hole["x"], cx - g["post"]/2), cx + g["post"]/2),
                       min(max(hole["y"], cy - g["post"]/2), cy + g["post"]/2))
            if math.hypot(hole["x"] - nearest[0], hole["y"] - nearest[1]) <= r:
                raise ValueError("A corner post would meet a standoff")
    return item


SKETCH_FACES = VENT_FACES
OPERATIONS = ("cut", "extrude")
MAX_SKETCHES, MAX_SHAPES, MAX_POINTS = 8, 12, 40


def _numbers(values):
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in values):
        raise ValueError("Sketch dimensions must be finite numbers")
    return [float(v) for v in values]


def _crosses(p, q, r, s):
    """Whether segments pq and rs share a point (collinear overlaps included)."""
    def side(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])

    def on(a, b, c):
        return min(a[0], b[0]) - 1e-9 <= c[0] <= max(a[0], b[0]) + 1e-9 and min(a[1], b[1]) - 1e-9 <= c[1] <= max(a[1], b[1]) + 1e-9
    d = side(p, q, r), side(p, q, s), side(r, s, p), side(r, s, q)
    if ((d[0] > 0) != (d[1] > 0) and abs(d[0]) > 1e-12 and abs(d[1]) > 1e-12 and
            (d[2] > 0) != (d[3] > 0) and abs(d[2]) > 1e-12 and abs(d[3]) > 1e-12):
        return True
    return any(abs(v) <= 1e-12 and on(a, b, c) for v, (a, b, c) in zip(d, ((p, q, r), (p, q, s), (r, s, p), (r, s, q))))


def _shape(shape):
    """A closed profile in face coordinates: rect (x, y its low corner), circle (x, y its centre) or
    polygon (points, a simple closed loop of 3 to 40 corners). A corner [u, v, bulge] bends the edge
    to the next corner into an arc: bulge = tan(sweep / 4), positive counterclockwise (DXF's
    convention), at most a half circle; [u, v] is a straight edge."""
    kind = shape.get("kind") if isinstance(shape, dict) else None
    keys = {"rect": {"kind", "x", "y", "width", "height"}, "circle": {"kind", "x", "y", "diameter"},
            "polygon": {"kind", "points"}}.get(kind)
    if keys is None or set(shape) != keys:
        raise ValueError("Expected a sketch shape: rect (x, y, width, height), circle (x, y, diameter) or polygon (points)")
    if kind == "rect":
        x, y, w, h = _numbers([shape["x"], shape["y"], shape["width"], shape["height"]])
        if w < 0.5 or h < 0.5:
            raise ValueError("A sketch rectangle must be at least 0.5 mm wide and high")
        return {"kind": kind, "x": x, "y": y, "width": w, "height": h}
    if kind == "circle":
        x, y, d = _numbers([shape["x"], shape["y"], shape["diameter"]])
        if d < 0.5:
            raise ValueError("A sketch circle must be at least 0.5 mm across")
        return {"kind": kind, "x": x, "y": y, "diameter": d}
    points = shape["points"]
    if not isinstance(points, list) or not 3 <= len(points) <= MAX_POINTS or any(
            not isinstance(p, (list, tuple)) or len(p) not in (2, 3) for p in points):
        raise ValueError(f"A sketch polygon has 3 to {MAX_POINTS} corners")
    points = [p[:2] if len(p) == 2 or p[2] == 0 else p for p in map(_numbers, points)]
    if any(len(p) == 3 and abs(p[2]) > 1 for p in points):
        raise ValueError("A sketch arc is at most a half circle (bulge -1 to 1)")
    n = len(points)
    if any(math.hypot(points[i][0]-points[i-1][0], points[i][1]-points[i-1][1]) < 0.5 for i in range(n)):
        raise ValueError("Sketch polygon edges must be at least 0.5 mm long")
    result = {"kind": kind, "points": points}
    loop = flatten(result)
    m = len(loop)
    for i in range(m):
        for j in range(i + 2, m):
            if (j + 1) % m != i and _crosses(loop[i], loop[(i+1) % m], loop[j], loop[(j+1) % m]):
                raise ValueError("A sketch polygon must not cross itself")
    if area(result) < 0.25:
        raise ValueError("A sketch polygon must enclose at least 0.25 mm²")
    return result


def area(shape):
    if shape["kind"] == "rect":
        return shape["width"] * shape["height"]
    if shape["kind"] == "circle":
        return math.pi * (shape["diameter"] / 2) ** 2
    p = shape["points"]
    signed = sum(p[i-1][0]*p[i][1] - p[i][0]*p[i-1][1] for i in range(len(p))) / 2
    # Each arc adds its circular segment, signed like the shoelace term: r² (sweep - sin sweep) / 2.
    for i, a in enumerate(p):
        if len(a) == 3:
            _, r, _, sweep = arc(a, p[(i+1) % len(p)])
            signed += r * r * (sweep - math.sin(sweep)) / 2
    return abs(signed)


def arc(a, b):
    """The arc from corner a = [u, v, bulge] to b: (centre, radius, start angle, sweep)."""
    bulge = a[2]
    dx, dy = b[0] - a[0], b[1] - a[1]
    chord = math.hypot(dx, dy)
    offset = chord * (1 - bulge * bulge) / (4 * bulge)  # centre from the chord's middle, to its left
    cx, cy = (a[0] + b[0]) / 2 - dy / chord * offset, (a[1] + b[1]) / 2 + dx / chord * offset
    return (cx, cy), chord * (1 + bulge * bulge) / (4 * abs(bulge)), math.atan2(a[1] - cy, a[0] - cx), 4 * math.atan(bulge)


def flatten(shape, step=math.pi / 32):
    """A polygon's corners with each arc replaced by chords at most `step` radians apart; for the
    crossing and inside-point checks only. The model's geometry stays exact."""
    p = shape["points"]
    result = []
    for i, a in enumerate(p):
        result.append((a[0], a[1]))
        if len(a) == 3:
            (cx, cy), r, start, sweep = arc(a, p[(i+1) % len(p)])
            count = max(2, math.ceil(abs(sweep) / step))
            result += [(cx + r * math.cos(start + sweep * k / count), cy + r * math.sin(start + sweep * k / count))
                       for k in range(1, count)]
    return result


def extent(shape):
    """(u0, v0, u1, v1): the shape's box in face coordinates."""
    if shape["kind"] == "rect":
        return shape["x"], shape["y"], shape["x"] + shape["width"], shape["y"] + shape["height"]
    if shape["kind"] == "circle":
        r = shape["diameter"] / 2
        return shape["x"] - r, shape["y"] - r, shape["x"] + r, shape["y"] + r
    points = shape["points"]
    us, vs = [p[0] for p in points], [p[1] for p in points]
    for i, a in enumerate(points):  # an arc reaches past its ends where it crosses an axis direction
        if len(a) == 3:
            (cx, cy), r, start, sweep = arc(a, points[(i+1) % len(points)])
            for k in range(4):
                angle = k * math.pi / 2
                if (angle - start if sweep > 0 else start - angle) % (2 * math.pi) < abs(sweep):
                    us.append(cx + r * math.cos(angle))
                    vs.append(cy + r * math.sin(angle))
    return min(us), min(vs), max(us), max(vs)


def thickness(settings, face):
    return settings["lid_thickness"] if face == "lid" else settings["wall"]


def validate_sketches(data, settings, sketches):
    """Sketches on the lid's top face or a side wall's outer face, each extruded outward or cut in
    by `depth` (null for a cut: through the lid or wall). Face coordinates (u, v): the lid's are
    board x, y; front and back walls' x, z; left and right walls' y, z (z up from the tray's base).
    A cut stays within the cavity's span along its wall (never a corner) or 1 mm inside the lid's
    edge; an extrusion within its face. Both keep below the tray's rim."""
    if not isinstance(sketches, (list, tuple)) or len(sketches) > MAX_SKETCHES:
        raise ValueError(f"Expected at most {MAX_SKETCHES} sketches")
    x0, y0, x1, y1 = data["bounds"]
    s = settings
    margin, height, outer = s["margin"], tray_height(data, s), s["margin"] + s["wall"]
    result = []
    for sketch in sketches:
        if (not isinstance(sketch, dict) or set(sketch) != {"face", "operation", "depth", "shapes"} or
                sketch["face"] not in SKETCH_FACES or sketch["operation"] not in OPERATIONS):
            raise ValueError("Expected a sketch with face (lid, front, back, left or right), operation (cut or extrude), depth and shapes")
        face, cut = sketch["face"], sketch["operation"] == "cut"
        depth = sketch["depth"]
        if depth is None and not cut:
            raise ValueError("An extrusion needs a depth")
        if depth is not None:
            (depth,) = _numbers([depth])
            if depth < 0.2 or depth > (thickness(s, face) if cut else 50):
                raise ValueError("A cut is at least 0.2 mm deep and at most through its face"
                                 if cut else "An extrusion is 0.2 to 50 mm deep")
        if not isinstance(sketch["shapes"], list) or not 1 <= len(sketch["shapes"]) <= MAX_SHAPES:
            raise ValueError(f"A sketch has 1 to {MAX_SHAPES} shapes")
        shapes = [_shape(shape) for shape in sketch["shapes"]]
        if face == "lid":
            inset = s["lid_gap"] + (1 if cut else 0)
            low, high = (x0 - outer + inset, y0 - outer + inset), (x1 + outer - inset, y1 + outer - inset)
        else:
            along = (x0, x1) if face in ("front", "back") else (y0, y1)
            spare = margin if cut else outer
            low, high = (along[0] - spare, 0), (along[1] + spare, height)
        for shape in shapes:
            u0, v0, u1, v1 = extent(shape)
            if u0 < low[0] - 1e-9 or v0 < low[1] - 1e-9 or u1 > high[0] + 1e-9 or v1 > high[1] + 1e-9:
                raise ValueError("A sketch cut must stay within the cavity's span on its wall, or 1 mm inside the lid's edge"
                                 if cut else "A sketch extrusion must stay within its face")
        result.append({"face": face, "operation": sketch["operation"], "depth": depth, "shapes": shapes})
    return result


def _rect(box):
    u0, u1, v0, v1 = box
    return {"kind": "rect", "x": u0, "y": v0, "width": u1 - u0, "height": v1 - v0}


def validate_openings(data, settings, cutouts, vents, fastening=None, sketches=(), separate=None):
    """Cutouts, vents, lid screws and sketches together: no two openings or sketch shapes may
    overlap on one face, and lid vents and lid sketches keep 1 mm from the lid's screw holes.
    Openings are kept apart by their boxes. A sketch shape must not touch anything: with
    `separate(a, b)` (exact, from the FreeCAD model: whether two shapes are apart) where their boxes
    meet, else by its box. Returns (cutouts, vents, fastening, sketches)."""
    cutouts = validate_cutouts(data, settings, cutouts)
    vents = validate_vents(data, settings, vents)
    fastening = validate_fastening(data, settings, fastening)
    sketches = validate_sketches(data, settings, sketches)

    def meet(a, b, touching):
        return (min(a[1], b[1]) > max(a[0], b[0]) or touching and min(a[1], b[1]) >= max(a[0], b[0])) and \
               (min(a[3], b[3]) > max(a[2], b[2]) or touching and min(a[3], b[3]) >= max(a[2], b[2]))
    if fastening:
        g = fastening_geometry(data, settings, fastening)
        holes = [{"kind": "circle", "x": cx, "y": cy, "diameter": g["clear"] + 2} for cx, cy in g["centers"]]
        lid = [((low, high, v["across"] - v["length"]/2, v["across"] + v["length"]/2), None, "Lid vents")
               for v in vents if v["face"] == "lid" for low, high in vent_slots(v)]
        lid += [((u0, u1, v0, v1), shape, "Lid sketches") for sk in sketches if sk["face"] == "lid"
                for shape in sk["shapes"] for u0, v0, u1, v1 in [extent(shape)]]
        for box, shape, what in lid:
            for hole in holes:
                u0, v0, u1, v1 = extent(hole)
                if meet(box, (u0, u1, v0, v1), shape is not None) and (shape is None or separate is None or not separate(shape, hole)):
                    raise ValueError(f"{what} must keep 1 mm from the lid's screw holes")
    height = tray_height(data, settings)
    items = {face: [] for face in VENT_FACES}  # (box (u0, u1, v0, v1), sketch shape or None)
    for c in cutouts:
        items[c["side"]].append(((c["center"] - c["width"]/2, c["center"] + c["width"]/2,
                                  c["bottom"], min(c["bottom"] + c["height"], height)), None))
    for v in vents:
        if v["face"] != "lid":
            items[v["face"]] += [((a, b, v["across"], min(v["across"] + v["length"], height)), None) for a, b in vent_slots(v)]
        else:
            items["lid"] += [((a, b, v["across"] - v["length"]/2, v["across"] + v["length"]/2), None) for a, b in vent_slots(v)]
    for sk in sketches:
        items[sk["face"]] += [((u0, u1, v0, v1), shape) for shape in sk["shapes"] for u0, v0, u1, v1 in [extent(shape)]]
    for face, entries in items.items():
        for i, (a, shape_a) in enumerate(entries):
            for b, shape_b in entries[:i]:
                sketched = shape_a is not None or shape_b is not None
                if meet(a, b, sketched) and not (sketched and separate is not None and
                                                 separate(shape_a or _rect(a), shape_b or _rect(b))):
                    raise ValueError("Openings and sketches on the lid overlap" if face == "lid"
                                     else f"Openings and sketches on the {face} wall overlap")
    return cutouts, vents, fastening, sketches
