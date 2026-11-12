"""The showcase intro: an electron (a glowing blue orb) wakes in the dark on a real KiCad board
(the KiCad 'video' demo), runs through a chain of its parts along their actual copper, lights each
part as it arrives on a downbeat, then the camera rises and the whole board lights in rings, one per
beat, and settles into the iPad frame where the app footage begins.

  intro.py <board.json> <music/analysis.json> <out.mp4|frames-dir> [--stills t1,t2,...]
"""
import json, math, subprocess, sys
import numpy as np
import skia

from layout import W, H, FPS, SCREEN, frame_bezel, beats_of, INTRO_END

board = json.load(open(sys.argv[1]))
music = json.load(open(sys.argv[2]))
OUT = sys.argv[3]
STILLS = [float(v) for v in sys.argv[sys.argv.index("--stills") + 1].split(",")] if "--stills" in sys.argv else None

beats, downbeats = beats_of(music)
IGNITE = beats[0]                                    # first note
REVEAL = min(downbeats, key=lambda t: abs(t - 19.0)) # the camera starts to rise
SETTLE = min(downbeats, key=lambda t: abs(t - 25.5)) # the board flies into the iPad frame
END = INTRO_END

# --- The route, re-timed so the orb reaches a part on each downbeat --------------------------
pts = np.array(board["route"]["points"], dtype=float)
seg = np.r_[0, np.cumsum(np.hypot(*np.diff(pts, axis=0).T))]
stop_idx = [s["index"] for s in board["route"]["stops"]]
stop_s = seg[stop_idx]
arrivals = [IGNITE] + [d for d in downbeats if IGNITE + 0.6 < d <= REVEAL]
n_close = len(arrivals)                              # stops reached in the close-up
# After the reveal the orb keeps going, faster, through the remaining stops until SETTLE.
rest = len(stop_s) - n_close
late = list(np.linspace(REVEAL, SETTLE, rest + 1)[1:]) if rest > 0 else []
stop_t = np.array(arrivals + late)[: len(stop_s)]
stop_s = stop_s[: len(stop_t)]

def ease(u):
    return u * u * (3 - 2 * u)

def orb_s(t):
    """Arc length at time t: eased per leg, so the orb lands on each part on its beat."""
    if t <= stop_t[0]:
        return stop_s[0]
    if t >= stop_t[-1]:
        return stop_s[-1]
    k = np.searchsorted(stop_t, t) - 1
    u = (t - stop_t[k]) / (stop_t[k + 1] - stop_t[k])
    u = 0.45 * u + 0.55 * ease(u)
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

# --- Lighting times: parts on the route at arrival; the rest in rings after the reveal -------
fps_ = board["footprints"]
light_at = np.full(len(fps_), np.inf)
refs = [f["ref"] for f in fps_]
for n, (t, s) in enumerate(zip(stop_t, board["route"]["stops"])):
    t = t + (beats[1] - beats[0]) * 2 if n == 0 else t  # the first part waits two beats in the dark
    light_at[refs.index(s["ref"])] = min(light_at[refs.index(s["ref"])], t)
reveal_pos = at_s(orb_s(REVEAL))
ring_beats = [b for b in beats if REVEAL <= b <= SETTLE]
centres = np.array([[(f["box"][0] + f["box"][2]) / 2, (f["box"][1] + f["box"][3]) / 2] for f in fps_])
dist = np.hypot(*(centres - reveal_pos).T)
ring = np.minimum((dist / (dist.max() + 1e-6) * len(ring_beats)).astype(int), len(ring_beats) - 1)
for i in range(len(fps_)):
    light_at[i] = min(light_at[i], ring_beats[ring[i]])

def intensity(t, t0):
    if t < t0:
        return 0.0
    d = t - t0
    return min(1.0, d / 0.08) * (0.62 + 0.9 * math.exp(-d / 0.35))

# --- Beat envelope (for pulses) ----------------------------------------------------------------
B = np.array(beats)
def kick(t):
    k = np.searchsorted(B, t) - 1
    return 0.0 if k < 0 else math.exp(-(t - B[k]) / 0.12)

# --- Drawing -----------------------------------------------------------------------------------
def paint(color, alpha=1.0, stroke=None, blur=0.0, plus=False):
    p = skia.Paint(AntiAlias=True)
    p.setColor(skia.Color4f(color[0], color[1], color[2], alpha))
    if stroke is not None:
        p.setStyle(skia.Paint.kStroke_Style); p.setStrokeWidth(stroke); p.setStrokeCap(skia.Paint.kRound_Cap)
    if blur:
        p.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, blur))
    if plus:
        p.setBlendMode(skia.BlendMode.kPlus)
    return p

def rgb(h):
    return ((h >> 16) & 255) / 255, ((h >> 8) & 255) / 255, (h & 255) / 255

SUB, ZONE, CU_F, CU_B, PAD, SILK = rgb(0x041a14), rgb(0x0a2a21), rgb(0x16473a), rgb(0x0c2b24), rgb(0x4a4128), rgb(0x2c403a)
GOLD, CYAN, ORB, WHITE = rgb(0xffc861), rgb(0x9ff8ff), rgb(0x2fa8ff), (1, 1, 1)

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
silk_paths = [skia.Path() for _ in fps_]
for s in board["silk"]:
    silk_paths[s[4]].moveTo(s[0], s[1]); silk_paths[s[4]].lineTo(s[2], s[3])
all_silk = skia.Path()
for p in silk_paths:
    all_silk.addPath(p)
vias = skia.Path()
for v in board["vias"]:
    vias.addCircle(v[0], v[1], v[2] / 2)
route_path = skia.Path()
route_path.moveTo(*pts[0])
for q in pts[1:]:
    route_path.lineTo(*q)

rng = np.random.default_rng(3)
motes = np.c_[rng.uniform(x0, x1, 420), rng.uniform(y0, y1, 420), rng.uniform(0.4, 3.0, 420), rng.uniform(0, 6.28, 420)]
grain = [rng.normal(0, 1, (H // 2, W // 2)).astype(np.float32) for _ in range(6)]

def camera(t, i):
    """World (mm) -> screen matrix. Close: chase cam, tilted. Then rise to a top-down full view,
    then fly into the iPad screen rectangle."""
    full_scale = min(W * 0.9 / (x1 - x0), H * 0.8 / (y1 - y0))
    rise = 0.0 if t < REVEAL else min(1.0, (t - REVEAL) / (SETTLE - REVEAL))
    r = ease(rise) if rise < 1 else 1.0
    close_scale = W / 26.0                               # 26 mm across the screen
    scale = math.exp(math.log(close_scale) * (1 - r) + math.log(full_scale) * r)
    centre = cam[i] * (1 - r) + np.array([(x0 + x1) / 2, (y0 + y1) / 2]) * r
    angle = heading[i] * (1 - r)
    tilt = math.radians(58) * (1 - r)
    anchor = np.array([W / 2, H * 0.66]) * (1 - r) + np.array([W / 2, H / 2]) * r
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

def frame(t, i):
    surface = skia.Surface(W, H)
    c = surface.getCanvas()
    c.clear(skia.Color4f(0.004, 0.008, 0.012, 1))
    if t < IGNITE - 0.05:
        return surface
    m, scale, rise = camera(t, i)
    lit = [intensity(t, light_at[k]) for k in range(len(fps_))]
    k_env = kick(t)
    # Darkness: the board is unseen except near the orb until the reveal lifts it.
    dark = 0.93 * (1 - rise) if t < REVEAL else 0.93 * (1 - rise) ** 2
    wake = min(1.0, (t - IGNITE) / 2.5)
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
        pool = 520 * (0.6 + 0.4 * wake) * (1 + 0.15 * k_env)
        shader = skia.GradientShader.MakeRadial(skia.Point(ox, oy), pool,
            [skia.Color4f(0, 0, 0, 0), skia.Color4f(0, 0, 0, dark * 0.75), skia.Color4f(0, 0, 0, dark)], [0.0, 0.45, 1.0])
        p = skia.Paint(); p.setShader(shader); c.drawPaint(p)
    # Lit parts: pads gold, outlines cyan, each with a glow.
    c.save(); c.concat(m)
    px_mm = 1.0 / max(scale, 1e-6)
    for k, v in enumerate(lit):
        if v <= 0.01:
            continue
        a = min(1.0, v)
        c.drawPath(pad_paths[k], paint(GOLD, a * 0.55, blur=6 * px_mm, plus=True))
        c.drawPath(pad_paths[k], paint(GOLD, a))
        c.drawPath(silk_paths[k], paint(CYAN, a * 0.9, stroke=max(0.18, 1.6 * px_mm)))
        c.drawPath(silk_paths[k], paint(CYAN, a * 0.5, stroke=max(0.18, 1.6 * px_mm), blur=5 * px_mm, plus=True))
    # The energised lane: the route so far, bright near the orb.
    s_now = orb_s(t)
    trail = skia.Path(); trail.moveTo(*at_s(max(0, s_now - 0.01)))
    lane = skia.Path(); first = True
    for s in np.linspace(0, s_now, max(2, int(s_now / 0.5))):
        q = at_s(s)
        (lane.moveTo if first else lane.lineTo)(*q); first = False
    c.drawPath(lane, paint(ORB, 0.35, stroke=max(0.25, 2 * px_mm)))
    c.drawPath(lane, paint(ORB, 0.25, stroke=max(0.25, 2 * px_mm), blur=8 * px_mm, plus=True))
    hot = skia.Path(); first = True
    for s in np.linspace(max(0, s_now - 18), s_now, 60):
        q = at_s(s)
        (hot.moveTo if first else hot.lineTo)(*q); first = False
    c.drawPath(hot, paint(ORB, 0.9, stroke=max(0.3, 3 * px_mm), blur=3 * px_mm, plus=True))
    c.restore()
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
            a = 0.35 * max(0, 1 - d / 700) * (1 - rise) * wake
            mp.setColor(skia.Color4f(0.6, 0.85, 1.0, a)); c.drawCircle(sx, sy, z * (1.2 - 0.6 * rise), mp)
    # The orb.
    flick = 1.0 if t > IGNITE + 0.5 else 0.5 + 0.5 * abs(math.sin((t - IGNITE) * 40))
    grow = min(1.0, (t - IGNITE) / 0.35)
    size = (1.0 + 0.22 * k_env) * grow * flick * (1 - 0.55 * rise)
    if size > 0:
        c.drawCircle(ox, oy, 150 * size, paint(ORB, 0.20, blur=60 * size, plus=True))
        c.drawCircle(ox, oy, 55 * size, paint(ORB, 0.55, blur=22 * size, plus=True))
        c.drawCircle(ox, oy, 16 * size, paint(WHITE, 0.95, blur=5 * size, plus=True))
        c.drawCircle(ox, oy, 6 * size, paint(WHITE, 1.0))
        streak = skia.Paint(AntiAlias=True); streak.setBlendMode(skia.BlendMode.kPlus)
        streak.setShader(skia.GradientShader.MakeLinear([skia.Point(ox - 420 * size, oy), skia.Point(ox + 420 * size, oy)],
            [skia.Color4f(*ORB, 0), skia.Color4f(*ORB, 0.45), skia.Color4f(*ORB, 0)]))
        c.drawRect(skia.Rect.MakeLTRB(ox - 420 * size, oy - 1.5, ox + 420 * size, oy + 1.5), streak)
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

def to_array(surface):
    img = surface.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType)
    rgb_ = img[:, :, :3].astype(np.float32)
    g = grain[int(rgb_.sum()) % len(grain)]
    rgb_ += np.repeat(np.repeat(g, 2, 0), 2, 1)[:, :, None] * 3.0
    return np.clip(rgb_, 0, 255).astype(np.uint8)

if STILLS:
    from PIL import Image
    for t in STILLS:
        i = min(int(t * FPS), len(TT) - 1)
        Image.fromarray(to_array(frame(t, i))).save(f"{OUT}/intro-{t:05.2f}.png")
        print("still", t)
    sys.exit()

ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                       "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-preset", "slow", "-crf", "12", "-pix_fmt", "yuv420p", OUT],
                      stdin=subprocess.PIPE)
for i, t in enumerate(TT):
    if t >= END:
        break
    ff.stdin.write(to_array(frame(t, i)).tobytes())
    if i % 120 == 0:
        print(f"{t:6.2f}s", flush=True)
ff.stdin.close(); ff.wait()
print("intro done", OUT, "stops", len(stop_t), "close stops", n_close, "reveal", REVEAL, "settle", SETTLE)
