"""Independent numeric and mesh checks; stdlib only for contract tests."""
from collections import Counter
import math

from inputs import area, fastening_geometry, thickness


def close(actual, expected, tolerance=1e-6):
    if abs(actual - expected) > tolerance:
        raise AssertionError(f"Expected {expected:.12g}, got {actual:.12g} (tolerance {tolerance})")


def expected(data, settings, cutouts=(), vents=(), fastening=None, sketches=()):
    """Analytic tray and lid. Each wall cutout or vent slot removes width x (height up to the rim)
    x wall: it lies wholly in the wall slab (validate_openings keeps openings off the corners, the
    bosses and each other). A lid slot removes width x length x lid thickness."""
    x0, y0, x1, y1 = data["bounds"]
    s = settings
    width = x1-x0+2*(s["margin"]+s["wall"])
    depth = y1-y0+2*(s["margin"]+s["wall"])
    height = s["floor"]+s["standoff_height"]+s["board_clearance"]+data["board_thickness"]
    inner_width, inner_depth = width-2*s["wall"], depth-2*s["wall"]
    tray_volume = width*depth*height - inner_width*inner_depth*(height-s["floor"])
    tray_volume += len(data["mounting_holes"])*math.pi*(s["standoff_diameter"]/2)**2*s["standoff_height"]
    tray_volume -= sum(math.pi*(h["diameter"]/2)**2*(s["floor"]+s["standoff_height"])
                       for h in data["mounting_holes"])
    tray_volume -= sum(c["width"] * (min(c["bottom"]+c["height"], height) - c["bottom"]) * s["wall"]
                       for c in cutouts)
    tray_volume -= sum(v["count"] * v["width"] * (min(v["across"]+v["length"], height) - v["across"]) * s["wall"]
                       for v in vents if v["face"] != "lid")
    lid_slots = sum(v["count"] * v["width"] * v["length"] for v in vents if v["face"] == "lid")
    if fastening:
        # Four corner posts standing in the empty cavity, a blind hole in each, lid holes above.
        g = fastening_geometry(data, settings, fastening)
        tray_volume += 4 * (g["post"]**2 * (height - s["floor"]) - math.pi * (g["hole"]/2)**2 * g["depth"])
        lid_slots += 4 * math.pi * (g["clear"]/2)**2
    x, y = x0-s["margin"]-s["wall"], y0-s["margin"]-s["wall"]
    gap = s["lid_gap"]
    lid_volume = ((width-2*gap)*(depth-2*gap) - lid_slots)*s["lid_thickness"]
    # Sketches: a cut removes area x depth from its lid or wall slab (validate_openings keeps it
    # inside the slab and off every other opening); an extrusion adds area x depth outside the face.
    reach = {face: 0.0 for face in ("lid", "front", "back", "left", "right")}
    for sk in sketches:
        cut = sk["operation"] == "cut"
        amount = sum(map(area, sk["shapes"])) * (thickness(s, sk["face"]) if sk["depth"] is None else sk["depth"])
        if sk["face"] == "lid":
            lid_volume += -amount if cut else amount
        else:
            tray_volume += -amount if cut else amount
        if not cut:
            reach[sk["face"]] = max(reach[sk["face"]], sk["depth"])
    return {"Tray": {"bounds": [x - reach["left"], y - reach["front"], 0,
                                x + width + reach["right"], y + depth + reach["back"], height], "volume": tray_volume},
            "Lid": {"bounds": [x+gap, y+gap, height+gap, x+width-gap, y+depth-gap,
                                height+gap+s["lid_thickness"]+reach["lid"]],
                    "volume": lid_volume}}


def manifold(points, faces):
    """Require welded, consistently wound triangles, each edge shared twice.

    Mesh exports duplicate vertices. Weld at 1e-6 mm for STL float rounding;
    reject degenerate triangles. Closed edge incidence alone does not imply a
    valid solid, so the FreeCAD harness also checks solidity, self-intersection and volume.
    """
    vertices = [tuple(round(float(c), 6) for c in p) for p in points]
    if any(len(p) != 3 or any(not math.isfinite(c) for c in p) for p in vertices):
        raise AssertionError("Non-finite or malformed STL vertex")
    edges, directed = Counter(), Counter()
    volume = 0.0
    for face in faces:
        a, b, c = (vertices[i] for i in face)
        ab = [b[i]-a[i] for i in range(3)]
        ac = [c[i]-a[i] for i in range(3)]
        cross = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        if sum(v*v for v in cross) < 1e-20:
            raise AssertionError("Degenerate STL triangle")
        for u, v in ((a, b), (b, c), (c, a)):
            edges[tuple(sorted((u, v)))] += 1
            directed[(u, v)] += 1
        volume += (a[0]*(b[1]*c[2]-b[2]*c[1]) + a[1]*(b[2]*c[0]-b[0]*c[2]) +
                   a[2]*(b[0]*c[1]-b[1]*c[0])) / 6
    if not faces or any(n != 2 for n in edges.values()):
        raise AssertionError("STL has open or nonmanifold edges")
    if any(directed[(v, u)] != n for (u, v), n in directed.items()):
        raise AssertionError("STL has inconsistent winding")
    if volume <= 0:
        raise AssertionError("STL has non-positive oriented volume")
    return {"triangles": len(faces), "edges": len(edges), "signed_volume_mm3": volume}
