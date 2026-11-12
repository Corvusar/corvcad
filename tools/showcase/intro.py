"""The showcase intro: an electron (a glowing blue orb) wakes in the dark on a real KiCad board,
runs through a chain of its parts along their actual copper and lands on one each bar. Every
arrival sends a wave of light through the copper, a shock ring and sparks; arcs jump to nearby pads
on the music's attacks. On the phrase accent the camera rises and the whole board powers on in
rings, one per beat, and settles into the iPad frame where the app footage begins.

Everything that pulses is driven by the music: the bar grid for arrivals, the attack (`hit`) and
bass (`low`) envelopes for light, lens and camera (analyze.py).

  intro.py <board.json> <music/analysis.json> <out.mp4|frames-dir> [--stills t1,t2,...] [--jobs N]
"""
import json, math, os, subprocess, sys
from multiprocessing import Pool
import numpy as np
import skia

from layout import W, H, FPS, SCREEN, frame_bezel, beats_of, INTRO_END

board = json.load(open(sys.argv[1]))
music = json.load(open(sys.argv[2]))
OUT = sys.argv[3]
STILLS = [float(v) for v in sys.argv[sys.argv.index("--stills") + 1].split(",")] if "--stills" in sys.argv else None
JOBS = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else max(1, (os.cpu_count() or 2) - 2)

beats, downbeats = beats_of(music)
phrases = music["phrases"]
IGNITE = phrases[0]                                    # the first accent
REVEAL = min(phrases, key=lambda t: abs(t - 17.2))    # the camera starts to rise
SETTLE = min(phrases, key=lambda t: abs(t - 25.2))    # the board flies into the iPad frame
END = INTRO_END

# --- The route, re-timed so the orb reaches a part on each bar --------------------------------
pts = np.array(board["route"]["points"], dtype=float)
seg = np.r_[0, np.cumsum(np.hypot(*np.diff(pts, axis=0).T))]
stop_idx = [s["index"] for s in board["route"]["stops"]]
stop_s = seg[stop_idx]
arrivals = [d for d in downbeats if IGNITE - 0.01 <= d <= REVEAL + 0.01]
n_close = len(arrivals)                              # stops reached in the close-up
# After the reveal the orb keeps going, a stop per bar, until SETTLE.
late = [d for d in downbeats if REVEAL + 0.01 < d <= SETTLE + 0.01]
stop_t = np.array(arrivals + late)[: len(stop_s)]
stop_s = stop_s[: len(stop_t)]

def ease(u):
    u = min(max(u, 0.0), 1.0)
    return u * u * (3 - 2 * u)

def orb_s(t):
    """Arc length at time t: eased per leg, so the orb lands on each part on its bar."""
    if t <= stop_t[0]:
        return stop_s[0]
    if t >= stop_t[-1]:
        return stop_s[-1]
    k = np.searchsorted(stop_t, t) - 1
    u = (t - stop_t[k]) / (stop_t[k + 1] - stop_t[k])
    u = 0.3 * u + 0.7 * ease(u)
    return stop_s[k] + (stop_s[k + 1] - stop_s[k]) * u

def at_s(s):
    k = min(max(np.searchsorted(seg, s) - 1, 0), len(seg) - 2)
    u = 0 if seg[k + 1] == seg[k] else (s - seg[k]) / (seg[k + 1] - seg[k])
    return pts[k] + (pts[k + 1] - pts[k]) * u

# Smoothed heading (the chase camera turns the board so travel points up the screen).
TT = np.arange(0, END + 1 / FPS, 1 / FPS)
pos = np.array([at_s(orb_s(t)) for t in TT])
ahead = np.array([at_s(orb_s(t + 0.5)) for t in TT])
raw = np.arctan2(ahead[:, 0] - pos[:, 0], -(ahead[:, 1] - pos[:, 1]))
heading = np.unwrap(raw)
for _ in range(3):  # three passes of a one-second moving average
    k = FPS
    heading = np.convolve(np.r_[[heading[0]] * k, heading, [heading[-1]] * k], np.ones(k) / k, "same")[k:-k]
cam = pos.copy()
for i in range(1, len(cam)):  # the camera trails the orb a little
    cam[i] = cam[i - 1] + (pos[i] - cam[i - 1]) * 0.12

# --- The music's envelopes, per frame: a fast attack follower and a slower bass follower ------
def follow(values, decay):
    out, level = np.zeros(len(TT)), 0.0
    k = math.exp(-1 / (FPS * decay))
    for i in range(len(TT)):
        level = max(values[min(i, len(values) - 1)], level * k)
        out[i] = level
    return np.clip(out, 0, 1.2)

HIT = follow(music["hit"], 0.07)
LOW = follow(music["low"], 0.16)

def since(times, t):
    """Seconds since the last of `times` at or before t (inf before the first)."""
    k = np.searchsorted(times, t + 1e-6) - 1
    return math.inf if k < 0 else t - times[k]

RING_BEATS = np.array([b for b in beats if REVEAL - 0.01 <= b <= SETTLE + 0.01])

# --- Lighting times: parts on the route at arrival; the rest in rings after the reveal -------
fps_ = board["footprints"]
light_at = np.full(len(fps_), np.inf)
refs = [f["ref"] for f in fps_]
for n, (t, s) in enumerate(zip(stop_t, board["route"]["stops"])):
    light_at[refs.index(s["ref"])] = min(light_at[refs.index(s["ref"])], t)
reveal_pos = at_s(orb_s(REVEAL))
centres = np.array([[(f["box"][0] + f["box"][2]) / 2, (f["box"][1] + f["box"][3]) / 2] for f in fps_])
dist = np.hypot(*(centres - reveal_pos).T)
REACH = dist.max() + 1e-6
ring = np.minimum((dist / REACH * len(RING_BEATS)).astype(int), len(RING_BEATS) - 1)
for i in range(len(fps_)):
    light_at[i] = min(light_at[i], RING_BEATS[ring[i]])

def intensity(t, t0):
    if t < t0:
        return 0.0
    d = t - t0
    return min(1.0, d / 0.05) * (0.6 + 1.1 * math.exp(-d / 0.3))

# --- Drawing -----------------------------------------------------------------------------------
def paint(color, alpha=1.0, stroke=None, blur=0.0, plus=False):
    p = skia.Paint(AntiAlias=True)
    p.setColor(skia.Color4f(color[0], color[1], color[2], min(max(alpha, 0.0), 1.0)))
    if stroke is not None:
        p.setStyle(skia.Paint.kStroke_Style); p.setStrokeWidth(stroke); p.setStrokeCap(skia.Paint.kRound_Cap)
    if blur:
        p.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, blur))
    if plus:
        p.setBlendMode(skia.BlendMode.kPlus)
    return p

def rgb(h):
    return ((h >> 16) & 255) / 255, ((h >> 8) & 255) / 255, (h & 255) / 255

def col(c, a):
    return skia.Color4f(c[0], c[1], c[2], min(max(a, 0.0), 1.0))

SUB, ZONE, CU_F, CU_B, PAD, SILK = rgb(0x041a14), rgb(0x0a2a21), rgb(0x16473a), rgb(0x0c2b24), rgb(0x4a4128), rgb(0x2c403a)
GOLD, CYAN, ORB, WHITE = rgb(0xffc861), rgb(0x9ff8ff), rgb(0x2fa8ff), (1, 1, 1)
LIVE = rgb(0x35c9a4)   # copper once the board is powered

# Paths built once, in board millimetres.
def lines_path(rows):
    p = skia.Path()
    for r in rows:
        p.moveTo(r[0], r[1]); p.lineTo(r[2], r[3])
    return p

tracks_by = {}
for t in board["tracks"]:
    tracks_by.setdefault((t[5], round(t[4], 3)), []).append(t)
track_paths = [(layer, w, lines_path(rows)) for (layer, w), rows in tracks_by.items()]
zone_paths = []
for z in board["zones"]:
    p = skia.Path(); p.addPoly([skia.Point(*q) for q in z["pts"]], True); zone_paths.append((z["layer"], p))
x0, y0, x1, y1 = board["bounds"]
board_rect = skia.Rect.MakeLTRB(x0, y0, x1, y1)

def pad_shape(p, grow=0.0):
    x, y, w, h, r, shape = p[0], p[1], p[2] + grow, p[3] + grow, p[4], p[5]
    path = skia.Path()
    rect = skia.Rect.MakeXYWH(-w / 2, -h / 2, w, h)
    if shape in ("circle", "oval"):
        path.addRRect(skia.RRect.MakeRectXY(rect, min(w, h) / 2, min(w, h) / 2))
    elif shape == "roundrect":
        path.addRRect(skia.RRect.MakeRectXY(rect, min(w, h) * 0.25, min(w, h) * 0.25))
    else:
        path.addRect(rect)
    m = skia.Matrix(); m.setTranslate(x, y); m.preRotate(-r)
    path.transform(m)
    return path

pads_of = [[] for _ in fps_]
for p in board["pads"]:
    pads_of[p[7]].append(p)
pad_paths = [skia.Path() for _ in fps_]
for i, ps in enumerate(pads_of):
    for p in ps:
        pad_paths[i].addPath(pad_shape(p))
all_pads = skia.Path()
for p in pad_paths:
    all_pads.addPath(p)
PAD_XY = np.array([[p[0], p[1]] for p in board["pads"]])
silk_paths = [skia.Path() for _ in fps_]
for s in board["silk"]:
    silk_paths[s[4]].moveTo(s[0], s[1]); silk_paths[s[4]].lineTo(s[2], s[3])
all_silk = skia.Path()
for p in silk_paths:
    all_silk.addPath(p)
vias = skia.Path()
for v in board["vias"]:
    vias.addCircle(v[0], v[1], v[2] / 2)

rng = np.random.default_rng(3)
motes = np.c_[rng.uniform(x0, x1, 420), rng.uniform(y0, y1, 420), rng.uniform(0.4, 3.0, 420), rng.uniform(0, 6.28, 420)]
grain = [rng.normal(0, 1, (H // 2, W // 2)).astype(np.float32) for _ in range(6)]
NOISE = rng.uniform(0, 1, (4096, 4))            # per-spark angle, speed, size, phase
BURST = rng.uniform(0, 1, (len(stop_t), 18, 3))  # per-arrival sparks: angle, speed, length

def camera(t, i):
    """World (mm) -> screen matrix. Close: chase cam, tilted, punched by each arrival. Then a rise
    to a top-down full view, then into the iPad screen rectangle."""
    full_scale = min(W * 0.9 / (x1 - x0), H * 0.8 / (y1 - y0))
    rise = 0.0 if t < REVEAL else min(1.0, (t - REVEAL) / (SETTLE - REVEAL))
    r = ease(rise) if rise < 1 else 1.0
    arrive = math.exp(-since(stop_t, t) / 0.3)
    close_scale = W / 26.0 * (1 + 0.045 * arrive + 0.015 * LOW[i])   # 26 mm across the screen
    scale = math.exp(math.log(close_scale) * (1 - r) + math.log(full_scale) * r)
    scale *= 1 + 0.012 * r * math.exp(-since(RING_BEATS, t) / 0.18)
    centre = cam[i] * (1 - r) + np.array([(x0 + x1) / 2, (y0 + y1) / 2]) * r
    shake = arrive * (1 - r)
    angle = heading[i] * (1 - r) + math.radians(0.7) * shake * math.sin(t * 47)
    tilt = math.radians(58) * (1 - r)
    anchor = np.array([W / 2, H * 0.66]) * (1 - r) + np.array([W / 2, H / 2]) * r
    anchor = anchor + np.array([7 * math.sin(t * 61), 5 * math.cos(t * 53)]) * shake
    if t > SETTLE:  # into the iPad screen
        u = ease(min(1.0, (t - SETTLE) / (END - SETTLE - 0.15)))
        sx, sy, sw, sh = SCREEN
        target = min(sw * 0.94 / (x1 - x0), sh * 0.8 / (y1 - y0))
        scale = math.exp(math.log(full_scale) * (1 - u) + math.log(target) * u)
        anchor = np.array([W / 2, H / 2]) * (1 - u) + np.array([sx + sw / 2, sy + sh / 2]) * u
    m = skia.Matrix()
    m.setTranslate(-centre[0], -centre[1])
    m.postRotate(-math.degrees(angle))
    m.postScale(scale, scale)
    # Perspective: forward (up the screen) recedes. k sets the strength.
    if tilt > 1e-4:
        k = math.sin(tilt) / (H * 0.9)
        persp = skia.Matrix.MakeAll(1, 0, 0, 0, math.cos(tilt), 0, 0, -k, 1)
        m.postConcat(persp)
    m.postTranslate(anchor[0], anchor[1])
    return m, scale, r

def ring_shader(centre, radius, width, colour, alpha):
    """A wavefront: transparent, then `colour` over the last `width` mm before `radius`."""
    outer = radius + 0.5
    return skia.GradientShader.MakeRadial(skia.Point(*centre), outer,
        [col(colour, 0), col(colour, 0), col(colour, alpha), col(WHITE, alpha * 0.6), col(colour, 0)],
        [0.0, max(0.0, (radius - width) / outer), max(0.001, (radius - 0.25 * width) / outer), radius / outer, 1.0])

def copper(c, shader, grow, px_mm, layers=("F.Cu", "B.Cu")):
    """The tracks (and pads) stroked with a shader, additive: light running through the copper."""
    p = skia.Paint(AntiAlias=True); p.setShader(shader); p.setBlendMode(skia.BlendMode.kPlus)
    p.setStyle(skia.Paint.kStroke_Style); p.setStrokeCap(skia.Paint.kRound_Cap)
    for layer, w, path in track_paths:
        if layer in layers:
            p.setStrokeWidth(max(w * grow, 1.3 * px_mm)); c.drawPath(path, p)
    p.setStyle(skia.Paint.kFill_Style)
    c.drawPath(all_pads, p)

def scene(t, i):
    surface = skia.Surface(W, H)
    c = surface.getCanvas()
    c.clear(skia.Color4f(0.004, 0.008, 0.012, 1))
    if t < IGNITE - 0.3:
        return surface
    hit, low = float(HIT[i]), float(LOW[i])
    m, scale, rise = camera(t, i)
    lit = [intensity(t, light_at[k]) for k in range(len(fps_))]
    # Darkness: the board is unseen except near the orb until the reveal lifts it.
    dark = 0.9 * (1 - rise) if t < REVEAL else 0.9 * (1 - rise) ** 2
    wake = min(1.0, max(0.0, t - IGNITE) / 2.5)
    px_mm = 1.0 / max(scale, 1e-6)
    c.save(); c.concat(m)
    c.drawRect(board_rect, paint(SUB))
    for layer, p in zone_paths:
        c.drawPath(p, paint(ZONE if layer == "F.Cu" else CU_B, 0.9))
    for layer, w, p in track_paths:
        if layer != "F.Cu":
            c.drawPath(p, paint(CU_B, 0.8, stroke=w))
    for layer, w, p in track_paths:
        if layer == "F.Cu":
            c.drawPath(p, paint(CU_F, 1.0, stroke=w))
    c.drawPath(vias, paint(PAD, 0.9))
    c.drawPath(all_pads, paint(PAD))
    c.drawPath(all_silk, paint(SILK, 0.9, stroke=0.15))
    c.restore()
    # Darkness overlay with a light pool around the orb.
    orb = at_s(orb_s(t))
    ox, oy = m.mapXY(*orb)
    if dark > 0.001:
        pool = 560 * (0.6 + 0.4 * wake) * (1 + 0.2 * low + 0.25 * hit)
        shader = skia.GradientShader.MakeRadial(skia.Point(ox, oy), pool,
            [skia.Color4f(0, 0, 0, 0), skia.Color4f(0, 0, 0, dark * 0.75), skia.Color4f(0, 0, 0, dark)], [0.0, 0.45, 1.0])
        p = skia.Paint(); p.setShader(shader); c.drawPaint(p)
    c.save(); c.concat(m)
    # The board powers on: live copper spreads from the reveal point with the rings.
    if t >= REVEAL:
        reach = REACH * 1.1 * min(1.0, (t - REVEAL) / (SETTLE - REVEAL - 0.5))
        if reach > 1:
            shader = skia.GradientShader.MakeRadial(skia.Point(*reveal_pos), reach + 10,
                [col(LIVE, 0.55), col(LIVE, 0.55), col(LIVE, 0)], [0.0, reach / (reach + 10), 1.0])
            copper(c, shader, 1.0, px_mm, layers=("F.Cu",))
        for b in RING_BEATS:      # a wavefront per beat
            age = t - b
            if 0 <= age < 1.1:
                copper(c, ring_shader(reveal_pos, age * 190 + 4, 12, CYAN, 0.5 * (1 - age / 1.1) ** 1.5), 1.1, px_mm)
    # Each arrival: a wave of light through the copper around the part, and a shock ring.
    for j, t0 in enumerate(stop_t):
        age = t - t0
        if not 0 <= age < 1.5:
            continue
        centre = at_s(stop_s[j])
        fade = (1 - age / 1.5) ** 1.5
        copper(c, ring_shader(centre, age * 46 + 1.5, 6, CYAN, 0.5 * fade), 1.0, px_mm)
        if age < 0.9:
            ringfade = (1 - age / 0.9) ** 2
            c.drawCircle(centre[0], centre[1], 2 + age * 34, paint(CYAN, 0.8 * ringfade, stroke=max(0.22, 2 * px_mm)))
            c.drawCircle(centre[0], centre[1], 2 + age * 34, paint(ORB, 0.5 * ringfade, stroke=max(0.9, 7 * px_mm), blur=6 * px_mm, plus=True))
    # Lit parts: pads gold, outlines cyan, each with a glow that breathes with the attacks.
    for k, v in enumerate(lit):
        if v <= 0.01:
            continue
        a = min(1.0, v * (0.82 + 0.35 * hit))
        c.drawPath(pad_paths[k], paint(GOLD, a * 0.55, blur=6 * px_mm, plus=True))
        c.drawPath(pad_paths[k], paint(GOLD, a))
        c.drawPath(silk_paths[k], paint(CYAN, a * 0.9, stroke=max(0.18, 1.6 * px_mm)))
        c.drawPath(silk_paths[k], paint(CYAN, a * 0.5, stroke=max(0.18, 1.6 * px_mm), blur=5 * px_mm, plus=True))
    # The energised lane: the route so far, bright near the orb, with a pulse running up it per beat.
    s_now = orb_s(t)
    lane = skia.Path(); first = True
    for s in np.linspace(0, s_now, max(2, int(s_now / 0.5))):
        q = at_s(s)
        (lane.moveTo if first else lane.lineTo)(*q); first = False
    c.drawPath(lane, paint(ORB, 0.4, stroke=max(0.25, 2 * px_mm)))
    c.drawPath(lane, paint(ORB, 0.3 + 0.2 * hit, stroke=max(0.25, 2 * px_mm), blur=8 * px_mm, plus=True))
    hot = skia.Path(); first = True
    for s in np.linspace(max(0, s_now - 18), s_now, 60):
        q = at_s(s)
        (hot.moveTo if first else hot.lineTo)(*q); first = False
    c.drawPath(hot, paint(ORB, 0.9, stroke=max(0.3, 3 * px_mm), blur=3 * px_mm, plus=True))
    beat_age = since(beats, t)
    if beat_age < 0.45 and s_now > 2:
        head = s_now * min(1.0, beat_age / 0.4)
        pulse = skia.Path(); first = True
        for s in np.linspace(max(0, head - 7), head, 14):
            q = at_s(s)
            (pulse.moveTo if first else pulse.lineTo)(*q); first = False
        c.drawPath(pulse, paint(WHITE, 0.8 * (1 - beat_age / 0.45), stroke=max(0.3, 2.4 * px_mm), blur=2 * px_mm, plus=True))
    # Arcs from the orb to nearby pads on the attacks.
    if hit > 0.42 and t > IGNITE and rise < 0.6:
        d = np.hypot(*(PAD_XY - orb).T)
        near = np.flatnonzero((d > 2.5) & (d < 13))
        slot = int(t * 8)                       # the choice holds for an eighth of a second
        for n in range(2 if hit > 0.8 else 1):
            if len(near) == 0:
                break
            target = PAD_XY[near[int(NOISE[(slot * 7 + n * 131) % 4096, 0] * len(near))]]
            along = target - orb
            normal = np.array([-along[1], along[0]]) / (np.hypot(*along) + 1e-6)
            flick = int(t * FPS / 2)
            arc = skia.Path(); arc.moveTo(*orb)
            for q in range(1, 7):
                jitter = (NOISE[(flick * 13 + q * 29 + n * 7) % 4096, 1] - 0.5) * 0.28 * np.hypot(*along) * math.sin(math.pi * q / 7)
                arc.lineTo(*(orb + along * q / 7 + normal * jitter))
            arc.lineTo(*target)
            a = min(1.0, hit) * (1 - rise)
            c.drawPath(arc, paint(ORB, 0.8 * a, stroke=max(0.5, 5 * px_mm), blur=4 * px_mm, plus=True))
            c.drawPath(arc, paint(WHITE, 0.95 * a, stroke=max(0.1, 1.3 * px_mm), plus=True))
            c.drawCircle(target[0], target[1], 1.1, paint(CYAN, 0.8 * a, blur=0.8, plus=True))
    c.restore()
    # Arrival sparks, thrown outwards and slowing (screen space, so they stay crisp).
    for j, t0 in enumerate(stop_t):
        age = t - t0
        if not 0 <= age < 0.75:
            continue
        centre = at_s(stop_s[j])
        cx, cy = m.mapXY(*centre)
        flash = math.exp(-age / 0.1)
        c.drawCircle(cx, cy, 190 * (1 - 0.5 * rise), paint(CYAN, 0.28 * flash, blur=70, plus=True))
        for angle, speed, length in BURST[j]:
            direction = np.array([math.cos(angle * 6.283), math.sin(angle * 6.283)])
            travel = (14 + 30 * speed) * (1 - math.exp(-age * 4.5)) / 4.5 * 3.2
            a_ = centre + direction * travel
            b_ = centre + direction * max(0.0, travel - (1.2 + 2.6 * length) * (1 - age / 0.75))
            (ax, ay), (bx, by) = m.mapXY(*a_), m.mapXY(*b_)
            fade = (1 - age / 0.75) ** 1.2
            c.drawLine(ax, ay, bx, by, paint(GOLD, 0.9 * fade, stroke=2.2, plus=True))
            c.drawLine(ax, ay, bx, by, paint(WHITE, 0.6 * fade, stroke=5, blur=3, plus=True))
    # Dust motes drifting above the board (close-up only).
    if rise < 0.9:
        mp = skia.Paint(AntiAlias=True)
        for x, y, z, ph in motes:
            if abs(x - orb[0]) > 40 or abs(y - orb[1]) > 40:
                continue
            sx, sy = m.mapXY(x + 0.6 * math.sin(t * 0.3 + ph), y + 0.6 * math.cos(t * 0.23 + ph))
            if not (0 <= sx < W and 0 <= sy < H):
                continue
            d = math.hypot(sx - ox, sy - oy)
            a = 0.4 * max(0, 1 - d / 700) * (1 - rise) * wake * (0.7 + 0.6 * hit)
            mp.setColor(skia.Color4f(0.6, 0.85, 1.0, min(1.0, a))); c.drawCircle(sx, sy, z * (1.2 - 0.6 * rise), mp)
    # The orb's spark tail: embers shed along its path, drifting and dying.
    if t > IGNITE:
        first_spark = int((t - 0.55) / 0.02) + 1
        for n in range(max(first_spark, int(IGNITE / 0.02) + 1), int(t / 0.02) + 1):
            born = n * 0.02
            age = t - born
            angle, speed, size, _ = NOISE[n % 4096]
            origin = at_s(orb_s(born))
            p_ = origin + np.array([math.cos(angle * 6.283), math.sin(angle * 6.283)]) * (0.8 + 3.5 * speed) * age
            sx, sy = m.mapXY(*p_)
            fade = max(0.0, 1 - age / 0.55) ** 1.4 * (1 - 0.7 * rise)   # age can round a hair past 0.55
            c.drawCircle(sx, sy, (1.2 + 2.4 * size) * (1 - 0.4 * rise), paint(CYAN, 0.85 * fade, blur=1.5, plus=True))
    # The orb: core, halo, starburst, anamorphic streak and lens ghosts.
    if t < IGNITE:      # sputtering before it catches
        size = 0.22 * abs(math.sin(t * 70)) * max(0.0, (t - (IGNITE - 0.3)) / 0.3)
    else:
        flick = 1.0 if t > IGNITE + 0.5 else 0.5 + 0.5 * abs(math.sin((t - IGNITE) * 40))
        grow = min(1.0, (t - IGNITE) / 0.35)
        size = (1.0 + 0.3 * low + 0.3 * hit) * grow * flick * (1 - 0.55 * rise)
    if size > 0.001:
        c.drawCircle(ox, oy, 150 * size, paint(ORB, 0.22, blur=60 * size, plus=True))
        c.drawCircle(ox, oy, 55 * size, paint(ORB, 0.6, blur=22 * size, plus=True))
        c.drawCircle(ox, oy, 16 * size, paint(WHITE, 0.95, blur=5 * size, plus=True))
        c.drawCircle(ox, oy, 6 * size, paint(WHITE, 1.0))
        for half, thick, colour, a in ((640, 2.0, ORB, 0.5), (300, 1.0, WHITE, 0.6)):
            half *= size * (1 + 0.5 * hit)
            streak = skia.Paint(AntiAlias=True); streak.setBlendMode(skia.BlendMode.kPlus)
            streak.setShader(skia.GradientShader.MakeLinear([skia.Point(ox - half, oy), skia.Point(ox + half, oy)],
                [col(colour, 0), col(colour, a), col(colour, 0)]))
            c.drawRect(skia.Rect.MakeLTRB(ox - half, oy - thick, ox + half, oy + thick), streak)
        c.save(); c.translate(ox, oy); c.rotate(math.degrees(t * 0.35))
        for k in range(6):   # starburst
            reach_ = (90 + 110 * hit) * size
            ray = skia.Paint(AntiAlias=True); ray.setBlendMode(skia.BlendMode.kPlus)
            ray.setShader(skia.GradientShader.MakeLinear([skia.Point(0, 0), skia.Point(reach_, 0)],
                [col(CYAN, 0.35 * (0.4 + hit)), col(CYAN, 0)]))
            c.drawRect(skia.Rect.MakeLTRB(0, -0.9, reach_, 0.9), ray)
            c.rotate(60)
        c.restore()
        vx, vy = W / 2 - ox, H / 2 - oy
        for k, radius, a, colour in ((0.55, 22, 0.12, ORB), (1.2, 54, 0.07, CYAN), (1.65, 12, 0.14, GOLD), (2.15, 84, 0.05, ORB)):
            c.drawCircle(ox + vx * k, oy + vy * k, radius * size, paint(colour, a * (0.5 + 0.8 * hit) * (1 - rise), blur=radius * 0.3, plus=True))
    # Horizon fog while tilted, and a vignette.
    if rise < 1:
        fog = skia.Paint(); fog.setShader(skia.GradientShader.MakeLinear([skia.Point(0, 0), skia.Point(0, H * 0.55)],
            [skia.Color4f(0.004, 0.01, 0.016, 0.95 * (1 - rise)), skia.Color4f(0.004, 0.01, 0.016, 0)]))
        c.drawRect(skia.Rect.MakeWH(W, H * 0.55), fog)
    vig = skia.Paint(); vig.setShader(skia.GradientShader.MakeRadial(skia.Point(W / 2, H / 2), W * 0.75,
        [skia.Color4f(0, 0, 0, 0), skia.Color4f(0, 0, 0, 0.75)], [0.55, 1.0]))
    c.drawPaint(vig)
    # The iPad frame fades in around the board as it lands.
    if t > SETTLE:
        frame_bezel(c, min(1.0, (t - SETTLE) / (END - SETTLE)))
    return surface

# --- The lens: depth of field, chromatic aberration, bloom, exposure --------------------------
def channel(r, g, b):
    return skia.ColorFilters.Matrix([r, 0, 0, 0, 0,  0, g, 0, 0, 0,  0, 0, b, 0, 0,  0, 0, 0, 1, 0])

RED, GREEN, BLUE = channel(1, 0, 0), channel(0, 1, 0), channel(0, 0, 1)
BRIGHT = skia.ColorFilters.Matrix([1.9, 0, 0, 0, -0.5,  0, 1.9, 0, 0, -0.5,  0, 0, 1.9, 0, -0.5,  0, 0, 0, 1, 0])
LINEAR = skia.SamplingOptions(skia.FilterMode.kLinear)

def lens(surface, t, i):
    hit, low = float(HIT[i]), float(LOW[i])
    rise = 0.0 if t < REVEAL else min(1.0, (t - REVEAL) / (SETTLE - REVEAL))
    image = surface.makeImageSnapshot()
    # Tilt-shift: the far board (top) and the nearest strip (bottom) fall out of focus while tilted.
    if rise < 1 and t > IGNITE - 0.3:
        c = surface.getCanvas()
        for top, bottom, sigma, strength in ((0.0, 0.46, 7, 1.0), (1.0, 0.8, 4, 0.8)):
            c.saveLayer()
            p = skia.Paint(); p.setImageFilter(skia.ImageFilters.Blur(sigma, sigma))
            c.drawImage(image, 0, 0, LINEAR, p)
            mask = skia.Paint(); mask.setBlendMode(skia.BlendMode.kDstIn)
            mask.setShader(skia.GradientShader.MakeLinear([skia.Point(0, H * top), skia.Point(0, H * bottom)],
                [skia.Color4f(0, 0, 0, strength * (1 - ease(rise))), skia.Color4f(0, 0, 0, 0)]))
            c.drawPaint(mask)
            c.restore()
        image = surface.makeImageSnapshot()
    out = skia.Surface(W, H)
    c = out.getCanvas(); c.clear(skia.Color4f(0, 0, 0, 1))
    # Chromatic aberration: red and blue split outwards with the arrivals and attacks.
    arrive = math.exp(-since(stop_t, t) / 0.25)
    split = 0.0012 + 0.006 * min(1.0, 0.8 * arrive + 0.45 * hit) + 0.01 * math.exp(-max(0.0, t - REVEAL) / 0.3) * (t >= REVEAL)
    for scale, channel_filter in ((1 + split, RED), (1.0, GREEN), (1 - split, BLUE)):
        p = skia.Paint(); p.setColorFilter(channel_filter); p.setBlendMode(skia.BlendMode.kPlus)
        c.save(); c.translate(W / 2, H / 2); c.scale(scale, scale); c.translate(-W / 2, -H / 2)
        c.drawImage(image, 0, 0, LINEAR, p)
        c.restore()
    # Bloom: the bright parts, blurred tight and wide, added back; it swells with the bass.
    small = skia.Surface(W // 4, H // 4)
    sc = small.getCanvas(); sc.scale(0.25, 0.25)
    p = skia.Paint(); p.setColorFilter(BRIGHT)
    sc.drawImage(image, 0, 0, LINEAR, p)
    bright = small.makeImageSnapshot()
    c.save(); c.scale(4, 4)
    for sigma, alpha in ((2.5, 0.4), (9, 0.25 + 0.25 * low + 0.15 * hit)):
        p = skia.Paint(); p.setImageFilter(skia.ImageFilters.Blur(sigma, sigma)); p.setBlendMode(skia.BlendMode.kPlus)
        p.setAlphaf(min(1.0, alpha))
        c.drawImage(bright, 0, 0, LINEAR, p)
    c.restore()
    # Exposure: a flash on the reveal, a lift into the cut.
    flash = 0.24 * math.exp(-(t - REVEAL) / 0.2) if t >= REVEAL else 0.0
    flash += 0.5 * max(0.0, (t - (END - 0.1)) / 0.1)
    if flash > 0.003:
        c.drawPaint(paint((0.72, 0.9, 1.0), min(1.0, flash), plus=True))
    return out

def render(i):
    t = float(TT[i])
    img = lens(scene(t, i), t, i).makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType)
    rgb_ = img[:, :, :3].astype(np.float32)
    rgb_ += np.repeat(np.repeat(grain[i % len(grain)], 2, 0), 2, 1)[:, :, None] * 3.0
    return np.clip(rgb_, 0, 255).astype(np.uint8)

def render_bytes(i):
    return render(i).tobytes()

if __name__ == "__main__":
    if STILLS:
        from PIL import Image
        for t in STILLS:
            Image.fromarray(render(min(int(round(t * FPS)), len(TT) - 1))).save(f"{OUT}/intro-{t:05.2f}.png")
            print("still", t)
        sys.exit()
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                           "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-preset", "slow", "-crf", "12", "-pix_fmt", "yuv420p", OUT],
                          stdin=subprocess.PIPE)
    frames = [i for i, t in enumerate(TT) if t < END]
    with Pool(JOBS) as pool:
        for i, data in enumerate(pool.imap(render_bytes, frames, chunksize=4)):
            ff.stdin.write(data)
            if i % 120 == 0:
                print(f"{TT[i]:6.2f}s", flush=True)
    ff.stdin.close(); ff.wait()
    print("intro done", OUT, "stops", len(stop_t), "close stops", n_close, "reveal", REVEAL, "settle", SETTLE)
