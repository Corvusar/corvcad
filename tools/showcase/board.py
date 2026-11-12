"""Parse a KiCad 10 .kicad_pcb into flat geometry for the showcase renderer, and choose the orb's
route: the net whose longest continuous F.Cu/B.Cu track path passes the most footprints.
  board.py <board.kicad_pcb> <out.json>"""
import json, math, re, sys
from collections import defaultdict


def parse(text):
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|\(|\)|[^\s()"]+', text)
    stack, cur = [], []
    for t in tokens:
        if t == "(":
            stack.append(cur); cur = []
        elif t == ")":
            done = cur; cur = stack.pop(); cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def kids(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def kid(node, name):
    k = kids(node, name)
    return k[0] if k else None


def xy(node):
    return float(node[1]), float(node[2])


def rot(p, deg):
    a = math.radians(deg)
    return p[0] * math.cos(a) + p[1] * math.sin(a), -p[0] * math.sin(a) + p[1] * math.cos(a)


def layer_of(node):
    l = kid(node, "layer")
    return l[1] if l else ""


board = parse(open(sys.argv[1], encoding="utf-8").read())
out = {"outline": [], "tracks": [], "vias": [], "pads": [], "footprints": [], "zones": [], "silk": []}

for g in [c for c in board if isinstance(c, list) and c and c[0] in ("gr_line", "gr_arc", "gr_rect")]:
    if layer_of(g) != "Edge.Cuts":
        continue
    if g[0] == "gr_rect":
        (x0, y0), (x1, y1) = xy(kid(g, "start")), xy(kid(g, "end"))
        out["outline"] += [[x0, y0, x1, y0], [x1, y0, x1, y1], [x1, y1, x0, y1], [x0, y1, x0, y0]]
    elif g[0] == "gr_line":
        out["outline"].append([*xy(kid(g, "start")), *xy(kid(g, "end"))])
    else:
        s, m, e = xy(kid(g, "start")), xy(kid(g, "mid")), xy(kid(g, "end"))
        out["outline"] += [[*s, *m], [*m, *e]]

nets = {}
for n in kids(board, "net"):
    nets[n[1]] = n[2] if len(n) > 2 else ""

def net_of(node):
    n = kid(node, "net")
    return n[1] if n else ""

for s in kids(board, "segment"):
    out["tracks"].append([*xy(kid(s, "start")), *xy(kid(s, "end")), float(kid(s, "width")[1]), layer_of(s), net_of(s)])
for v in kids(board, "via"):
    out["vias"].append([*xy(kid(v, "at")), float(kid(v, "size")[1]), net_of(v)])
for z in kids(board, "zone"):
    for fp in kids(z, "filled_polygon"):
        pts = [xy(p) for p in kid(fp, "pts")[1:] if p[0] == "xy"]
        out["zones"].append({"layer": layer_of(fp), "pts": pts})

for f in kids(board, "footprint"):
    at = kid(f, "at"); fx, fy = xy(at); frot = float(at[3]) if len(at) > 3 else 0.0
    side = "B" if layer_of(f).startswith("B.") else "F"
    ref = next((p[2] for p in kids(f, "property") if p[1] == "Reference"), "")
    def place(p):
        r = rot(p, frot)
        return fx + r[0], fy + r[1]
    index = len(out["footprints"])
    xs, ys = [], []
    for p in kids(f, "pad"):
        pat = kid(p, "at"); lx, ly = xy(pat); prot = float(pat[3]) if len(pat) > 3 else frot
        size = kid(p, "size"); w = float(size[1]); h = float(size[2]) if len(size) > 2 else w
        layers = " ".join(kid(p, "layers")[1:]) if kid(p, "layers") else ""
        ax, ay = place((lx, ly))
        out["pads"].append([ax, ay, w, h, prot, p[3], side if "*.Cu" not in layers else "*", index, net_of(p)])
        xs += [ax - w / 2, ax + w / 2]; ys += [ay - h / 2, ay + h / 2]
    for g in kids(f, "fp_line") + kids(f, "fp_rect") + kids(f, "fp_circle") + kids(f, "fp_arc"):
        l = layer_of(g)
        if not l.endswith(("SilkS", "Fab")):
            continue
        kind = "silk" if l.endswith("SilkS") else "fab"
        if g[0] in ("fp_line", "fp_arc"):
            a, b = place(xy(kid(g, "start"))), place(xy(kid(g, "end")))
            out["silk"].append([*a, *b, index, kind])
        elif g[0] == "fp_rect":
            (x0, y0), (x1, y1) = xy(kid(g, "start")), xy(kid(g, "end"))
            c = [place(q) for q in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
            for i in range(4):
                out["silk"].append([*c[i], *c[(i + 1) % 4], index, kind])
        else:
            c, e = xy(kid(g, "center")), xy(kid(g, "end")); r = math.dist(c, e)
            pts = [place((c[0] + r * math.cos(t), c[1] + r * math.sin(t))) for t in [i * math.pi / 8 for i in range(17)]]
            for i in range(16):
                out["silk"].append([*pts[i], *pts[i + 1], index, kind])
    if xs:
        out["footprints"].append({"ref": ref, "x": fx, "y": fy, "side": side,
                                  "box": [min(xs), min(ys), max(xs), max(ys)]})
    else:
        out["footprints"].append({"ref": ref, "x": fx, "y": fy, "side": side, "box": [fx, fy, fx, fy]})

# The orb's route: current through a chain of parts. It enters a footprint at one pad, leaves by
# another, and follows that net's real copper (tracks on any layer, joined at shared endpoints) to
# a pad of the next footprint. Searched with seeded random greedy walks; the longest chain wins.
import heapq, random

key = lambda x, y: (round(x, 3), round(y, 3))
graphs = defaultdict(lambda: defaultdict(list))
for x0, y0, x1, y1, w, layer, net in out["tracks"]:
    if net:
        a, b = key(x0, y0), key(x1, y1)
        d = math.dist(a, b)
        graphs[net][a].append((b, d)); graphs[net][b].append((a, d))
pads_by_net = defaultdict(list)
for i, p in enumerate(out["pads"]):
    if p[8]:
        pads_by_net[p[8]].append(i)
attach = {}
for i, p in enumerate(out["pads"]):
    g = graphs.get(p[8])
    if not g:
        continue
    r = max(p[2], p[3]) / 2 + 0.05
    near = [n for n in g if abs(n[0] - p[0]) <= r and abs(n[1] - p[1]) <= r]
    if near:
        attach[i] = min(near, key=lambda n: math.dist(n, (p[0], p[1])))

def copper_path(net, a, b):
    g, dist, prev, todo = graphs[net], {a: 0.0}, {a: None}, [(0.0, a)]
    while todo:
        d, n = heapq.heappop(todo)
        if n == b:
            break
        if d > dist[n]:
            continue
        for m, w in g[n]:
            if d + w < dist.get(m, 1e18):
                dist[m] = d + w; prev[m] = n; heapq.heappush(todo, (d + w, m))
    if b not in dist:
        return None, 0
    path, n = [], b
    while n is not None:
        path.append(n); n = prev[n]
    return path[::-1], dist[b]

pads_of = defaultdict(list)
for i, p in enumerate(out["pads"]):
    pads_of[p[7]].append(i)
refs = [f["ref"] for f in out["footprints"]]

def walk(rng):
    starts = [i for i, f in enumerate(out["footprints"]) if f["ref"].startswith(("J", "P")) and f["side"] == "F"]
    comp = rng.choice(starts)
    entry = rng.choice([i for i in pads_of[comp] if i in attach] or [None])
    if entry is None:
        return None
    visited, legs, total = {comp}, [], 0.0
    for _ in range(40):
        options = []
        for exit_pad in pads_of[comp]:
            net = out["pads"][exit_pad][8]
            if exit_pad not in attach or len(pads_by_net[net]) > 24:
                continue  # skip supply and ground planes: too many branches to read as one path
            for target in pads_by_net[net]:
                fp = out["pads"][target][7]
                if fp in visited or target not in attach:
                    continue
                path, length = copper_path(net, attach[exit_pad], attach[target])
                if path and 8 < length < 70:
                    options.append((length, exit_pad, target, path))
        if not options:
            break
        length, exit_pad, target, path = rng.choice(options)
        legs.append({"through": refs[comp], "entry": entry, "exit": exit_pad, "target": target,
                     "net": nets.get(out["pads"][exit_pad][8], ""), "path": [list(p) for p in path]})
        total += length
        comp, entry = out["pads"][target][7], target
        visited.add(comp)
    return legs, total, comp

best = None
rng = random.Random(7)
for _ in range(400):
    r = walk(rng)
    if r and (best is None or (len(r[0]), r[1]) > (len(best[0]), best[1])):
        best = r
legs, total, last = best
points, stops = [], []
for leg in legs:
    entry, exit_ = out["pads"][leg["entry"]], out["pads"][leg["exit"]]
    stops.append({"ref": leg["through"], "index": len(points)})
    points += [[entry[0], entry[1]], [exit_[0], exit_[1]]] + leg["path"]
target = out["pads"][legs[-1]["target"]]
stops.append({"ref": refs[last], "index": len(points)})
points.append([target[0], target[1]])
out["route"] = {"length_mm": round(total, 1), "stops": stops, "points": points,
                "nets": [l["net"] for l in legs]}
length, touched, path = total, len(stops), points
xs = [v for s in out["outline"] for v in (s[0], s[2])]; ys = [v for s in out["outline"] for v in (s[1], s[3])]
out["bounds"] = [min(xs), min(ys), max(xs), max(ys)]
json.dump(out, open(sys.argv[2], "w"))
print(f"bounds {out['bounds']}, {len(out['tracks'])} tracks, {len(out['pads'])} pads, {len(out['footprints'])} footprints, "
      f"{len(out['zones'])} zone polygons; route {length:.1f} mm through {touched} footprints, {len(path)} points:")
print(" -> ".join(s["ref"] for s in out["route"]["stops"]))
print("nets:", out["route"]["nets"])
