#!/usr/bin/env python3
"""Cut the showcase: intro + simulator takes + captions + finale, on the music's beat grid.

  compose.py <music dir> <takes dir> <intro.mp4> <out.mp4> [--from S --to S] [--stills t1,t2]

Each segment maps a take's span between two SHOWCASE-MARKs into a window of the music; the speed
follows from the two lengths and is shown as a "xN" badge when the footage runs faster than 1.5x.
"""
import json, math, subprocess, sys
from pathlib import Path
import numpy as np
import skia

from layout import W, H, FPS, SCREEN, frame_bezel, INTRO_END, beats_of, envelope

music_dir, takes_dir, intro_path, out_path = map(Path, sys.argv[1:5])
args = sys.argv[5:]
opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
music = json.load(open(music_dir / "analysis.json"))
DUR = music["duration"]
_beats, _down = beats_of(music)
BEATS = np.array(_beats)
DOWN = np.array(_down)
snap = lambda t: float(DOWN[np.argmin(abs(DOWN - t))])  # cuts land on bars
HIT, LOW = envelope(music, "hit"), envelope(music, "low")  # the music's attacks and bass, 0..1.5
SIM_W, SIM_H = 2388, 1668  # landscape simulator pixels; window points are half

def marks(take):
    data = json.load(open(takes_dir / f"{take}.json"))
    return {m["name"]: m["t"] for m in data["marks"]}, data.get("viewports", {})

# --- The edit: (music start, music end, take, from mark, to mark, caption, focus) ---------------
# Focus: None, or a function (u 0..1) -> (x, y, w, h) in simulator pixels to fill the screen.
CAP = {}
def cap(act, title, sub):
    return {"act": act, "title": title, "sub": sub}

S = snap
EDL = [
    (INTRO_END, S(31.2), "testA_SchematicToBoard", "root", "sheet",
     cap("01 — SCHEMATIC", "KiCad's own schematic engine.", "KiCad 10.0.6 · hierarchical sheets"), None),
    (S(31.2), S(41.2), "testA_SchematicToBoard", "sheet", "opamp",
     cap("01 — SCHEMATIC", "Parts from KiCad's libraries.", "Symbol picker · footprint chooser"), None),
    (S(41.2), S(49.2), "testA_SchematicToBoard", "opamp", "power",
     cap("01 — SCHEMATIC", "An input buffer, in place.", "Op-amp · gain network · decoupling"), "stage"),
    (S(49.2), S(55.2), "testA_SchematicToBoard", "power", "labels",
     cap("01 — SCHEMATIC", "Wires that route themselves.", "Right angles · junctions · net labels"), "stage"),
    (S(55.2), S(61.2), "testA_SchematicToBoard", "labels", "annotated",
     cap("01 — SCHEMATIC", "Connectivity, live.", "Net highlight · annotation by KiCad"), None),
    (S(61.2), S(67.2), "testA_SchematicToBoard", "annotated", "erc",
     cap("01 — SCHEMATIC", "Checked by KiCad's ERC.", "Electrical rules, on the device"), None),
    (S(67.2), S(73.2), "testA_SchematicToBoard", "erc", "pcb",
     cap("02 — BOARD", "Same project. Same files.", "One tap from schematic to board"), None),
    (S(73.2), S(79.2), "testA_SchematicToBoard", "pcb", "updateReport",
     cap("02 — BOARD", "Update PCB from Schematic.", "KiCad's own netlist transfer"), None),
    (S(79.2), S(89.2), "testA_SchematicToBoard", "updateReport", "placed",
     cap("02 — BOARD", "Placed by hand.", "Hold, move, drop"), "strip"),
    (S(89.2), S(97.2), "testA_SchematicToBoard", "placed", "routed",
     cap("02 — BOARD", "KiCad's interactive router.", "Walk-around · shove · vias"), "strip"),
    (S(97.2), S(103.2), "testA_SchematicToBoard", "routed", "end",
     cap("02 — BOARD", "DRC and zone fill, by KiCad.", "Design rules on the device"), None),
    (S(103.2), S(109.2), "testE_Parts3D", "generating", "generated",
     cap("03 — 3D", "Every part, generated.", "KiCad's 3D model generator, ported to C++"), None),
    (S(109.2), S(117.2), "testE_Parts3D", "generated", "end",
     cap("03 — 3D", "1,058 packages. No model files.", "Exact solids on Open CASCADE"), None),
    (S(117.2), S(125.2), "testB_Simulation", "schematic", "plotFull",
     cap("04 — SIMULATION", "ngspice, inside.", "Transient · AC · DC · operating point"), None),
    (S(125.2), S(137.2), "testB_Simulation", "electrify", "end",
     cap("04 — SIMULATION", "Current, made visible.", "Simulated voltages on the copper"), None),
    (S(137.2), S(147.2), "testF_Enclosure", "ready", "cutouts",
     cap("05 — ENCLOSURE", "FreeCAD's own modeller.", "FreeCAD 1.1.4 · PartDesign timeline"), None),
    (S(147.2), S(155.2), "testF_Enclosure", "cutouts", "screws",
     cap("05 — ENCLOSURE", "Shaped around the board.", "Connector cutouts · vents · screw posts"), None),
    (S(155.2), S(163.2), "testF_Enclosure", "screws", "sketch",
     cap("05 — ENCLOSURE", "Nothing collides.", "Part heights against the lid"), None),
    (S(163.2), S(175.2), "testF_Enclosure", "sketch", "sketchCut",
     cap("05 — ENCLOSURE", "Sketches, solved by FreeCAD.", "Sketcher constraints · arcs · pockets"), None),
    (S(175.2), S(183.2), "testF_Enclosure", "sketchCut", "end",
     cap("05 — ENCLOSURE", "A true parametric body.", "Every step editable · undo included"), None),
    (S(183.2), S(189.2), "testD_Toolbox", "gerbers", "gerbersZoom",
     cap("06 — BENCH", "The bench comes along.", "Gerber viewer"), None),
    (S(189.2), S(195.2), "testD_Toolbox", "calculator", "end",
     cap("06 — BENCH", "The bench comes along.", "PCB calculator · KiCad's formulas"), None),
    (S(195.2), S(201.2), "testC_FabricateKiCad", "fabricate", "end",
     cap("07 — FABRICATE", "Ready for the fab.", "Gerbers · drill · placement · PDF · BOM"), None),
    (S(201.2), S(207.2), "testG_FabricateFreeCAD", "fabricate", "end",
     cap("07 — FABRICATE", "Ready for the shop.", "Exact STEP · STL · the FreeCAD document"), None),
    (S(207.2), S(215.2), "testH_Finale", "open", "end", None, None),
]
FINALE = S(215.2)

# --- Fonts and colours ------------------------------------------------------------------------
def font(name, size, weight=None):
    mgr = skia.FontMgr()
    tf = mgr.matchFamilyStyle(name, skia.FontStyle(weight or skia.FontStyle.kNormal_Weight, skia.FontStyle.kNormal_Width,
                                                   skia.FontStyle.kUpright_Slant)) or skia.Typeface.MakeDefault()
    f = skia.Font(tf, size); f.setSubpixel(True); f.setEdging(skia.Font.Edging.kSubpixelAntiAlias)
    return f

installed = lambda *names: next((n for n in names if skia.FontMgr().matchFamilyStyle(n, skia.FontStyle())), names[-1])
MONO_FAMILY = installed("SF Mono", "Menlo", "Consolas")  # Consolas: rendering on Windows
DISPLAY_FAMILY = installed("SF Pro Display", "Helvetica Neue", "Segoe UI")
MONO = lambda s: font(MONO_FAMILY, s)
DISPLAY = lambda s, w=skia.FontStyle.kLight_Weight: font(DISPLAY_FAMILY, s, w)
GOLD = skia.Color4f(0.91, 0.77, 0.55, 1)
CYAN = skia.Color4f(0.62, 0.97, 1.0, 1)
STEEL = skia.Color4f(0.62, 0.66, 0.72, 1)
WHITE = skia.Color4f(0.95, 0.96, 0.98, 1)

def text(c, s, x, y, f, color, alpha=1.0, tracking=0.0, align="left"):
    p = skia.Paint(AntiAlias=True); p.setColor(skia.Color4f(color.fR, color.fG, color.fB, color.fA * alpha))
    if tracking:
        width = sum(f.measureText(ch) + tracking for ch in s) - tracking
        x0 = x - (width if align == "right" else width / 2 if align == "center" else 0)
        for ch in s:
            c.drawString(ch, x0, y, f, p); x0 += f.measureText(ch) + tracking
    else:
        width = f.measureText(s)
        x0 = x - (width if align == "right" else width / 2 if align == "center" else 0)
        c.drawString(s, x0, y, f, p)

def ease(u):
    u = min(max(u, 0.0), 1.0)
    return u * u * (3 - 2 * u)

def kick(t):
    """The music's pulse at t, 0..1: its attacks, held up a little by the bass."""
    return min(1.0, 0.7 * HIT(t) + 0.4 * LOW(t))

# --- Background: void, faint grid, slow ember glow --------------------------------------------
bg_surface = skia.Surface(W, H)
bc = bg_surface.getCanvas()
bc.clear(skia.Color4f(0.012, 0.012, 0.016, 1))
gp = skia.Paint(AntiAlias=True); gp.setColor(skia.Color4f(1, 1, 1, 0.025)); gp.setStrokeWidth(1)
for x in range(0, W, 48):
    bc.drawLine(x, 0, x, H, gp)
for y in range(0, H, 48):
    bc.drawLine(0, y, W, y, gp)
BACKGROUND = bg_surface.makeImageSnapshot()

# --- Sources -------------------------------------------------------------------------------------
_frame_times = {}
def frame_times(path):
    """The take's frame timestamps. The simulator records a frame only when the screen changes, so
    a still screen leaves a gap of seconds; a seek has to start at the frame on screen at that
    moment, not at the next one stored."""
    if path not in _frame_times:
        out = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "packet=pts_time", "-of", "csv=p=0", str(path)], text=True)
        _frame_times[path] = np.sort(np.array([float(v) for v in out.replace(",", " ").split() if v != "N/A"]))
    return _frame_times[path]

class Reader:
    """Frames of one segment at 60 fps, scaled to the screen size. A focus push-in eases from its first
    to its second rect over the first `push` of the segment; `done` is how far into the segment the
    reader starts (stills). The push-in is cropped here, per frame: ffmpeg's crop evaluates its
    size only once."""
    def __init__(self, path, start, length, speed, focus, done=0.0, push=1.0):
        sx, sy, sw, sh = SCREEN
        self.size = (sw, sh)
        self.focus, self.done, self.push, self.i = focus, done, push, 0
        times = frame_times(path)
        held = float(times[max(0, np.searchsorted(times, start, side="right") - 1)])  # on screen at `start`
        seek = max(0.0, held - 0.002)
        lead = start - seek
        probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                                     "stream=width,height", "-of", "json", str(path)]))["streams"][0]
        rotate = "transpose=2," if probe["height"] > probe["width"] else ""
        self.n = n = max(1, round(length / speed * FPS))
        size = f"{SIM_W}:{SIM_H}" if focus else f"{sw}:{sh}:flags=lanczos"
        self.decoded = (SIM_W, SIM_H) if focus else (sw, sh)
        if focus:
            self.surface = skia.Surface(sw, sh)
        # The held frame is clamped to time 0, the rest follow at the segment's speed; fps fills the gaps.
        vf = f"{rotate}scale={size},setpts='max((T-{lead:.4f})/{speed:.5f},0)/TB',fps={FPS}"
        self.proc = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-ss", f"{seek:.4f}", "-t", f"{lead + length + 1:.3f}",
                                      "-i", str(path), "-vf", vf, "-frames:v", str(n), "-f", "rawvideo",
                                      "-pix_fmt", "rgba", "-"], stdout=subprocess.PIPE)
        self.last = None
    def frame(self):
        dw, dh = self.decoded
        raw = self.proc.stdout.read(dw * dh * 4)
        if len(raw) == dw * dh * 4:
            img = np.frombuffer(raw, np.uint8).reshape(dh, dw, 4)
            if self.focus:
                (ax, ay, aw, ah), (bx, by, bw, bh) = self.focus
                e = ease((self.done + (1 - self.done) * self.i / self.n) / self.push)
                src = skia.Rect.MakeXYWH(ax + (bx - ax) * e, ay + (by - ay) * e, aw + (bw - aw) * e, ah + (bh - ah) * e)
                image = skia.Image.fromarray(np.ascontiguousarray(img), colorType=skia.kRGBA_8888_ColorType)
                sw, sh = self.size
                self.surface.getCanvas().drawImageRect(image, src, skia.Rect.MakeWH(sw, sh),
                                                       skia.SamplingOptions(skia.CubicResampler.Mitchell()))
                img = self.surface.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType)
            self.last = img
        self.i += 1
        return self.last

def focus_rect(name, take, vports):
    """Push-in targets in simulator pixels: the input buffer on the root sheet, the board corner."""
    def box(view, x0, y0, x1, y1, pad=1.25):
        if not view:
            return None
        mx, my, s, left, top = view
        cx, cy = (left + ((x0 + x1) / 2 - mx) * s) * 2, (top + ((y0 + y1) / 2 - my) * s) * 2
        w = max((x1 - x0) * s * 2 * pad, (y1 - y0) * s * 2 * pad * SIM_W / SIM_H, SIM_W / 3.2)
        h = w * SIM_H / SIM_W
        return (max(0, min(SIM_W - w, cx - w / 2)), max(0, min(SIM_H - h, cy - h / 2)), w, h)
    if name == "stage":
        target = box(vports.get("zoomed") or vports.get("back"), 222.7, 177.2, 310.7, 223.2)
    elif name == "strip":
        target = box(vports.get("placed") or vports.get("pcb"), 185, 126, 205, 146)
    else:
        target = None
    return target

# --- Captions --------------------------------------------------------------------------------------
def caption(c, cap_, t, t0, t1):
    if not cap_:
        return
    sx, sy, sw, sh = SCREEN
    a = ease((t - t0) / 0.35) * (1 - ease((t - (t1 - 0.25)) / 0.25))
    x = 34
    text(c, cap_["act"], x, 118, MONO(13), GOLD, a, tracking=3.2)
    p = skia.Paint(AntiAlias=True); p.setColor(skia.Color4f(0.91, 0.77, 0.55, 0.8 * a))
    c.drawRect(skia.Rect.MakeXYWH(x, 132, 26 * ease((t - t0) / 0.5), 1.5), p)
    # Title wraps to the left margin's width.
    words, lines, line = cap_["title"].split(), [], ""
    f = DISPLAY(30)
    for wd in words:
        trial = (line + " " + wd).strip()
        if f.measureText(trial) > sx - 2 * x and line:
            lines.append(line); line = wd
        else:
            line = trial
    lines.append(line)
    for i, ln in enumerate(lines):
        text(c, ln, x, 186 + i * 38, f, WHITE, a)
    y = 186 + len(lines) * 38 + 14
    mono, room = MONO(11), sx - 14 - x - 12   # up to the bezel
    for part in cap_["sub"].split(" · "):
        line = ""
        for wd in part.upper().split() + [None]:   # wrapped to the margin, like the title
            trial = (line + " " + wd).strip() if wd else None
            if wd is None or (line and sum(mono.measureText(ch) + 1.6 for ch in trial) > room):
                text(c, line, x, y, mono, STEEL, a * 0.9, tracking=1.6)
                y += 18 if wd else 22
                line = wd or ""
            else:
                line = trial

def badge(c, speed, t, t0, t1):
    if speed < 1.5:
        return
    a = ease((t - t0) / 0.3) * (1 - ease((t - (t1 - 0.2)) / 0.2))
    sx, sy, sw, sh = SCREEN
    text(c, f"×{speed:.0f}" if speed >= 3 else f"×{speed:.1f}", sx + sw - 18, sy + sh - 22, MONO(14), CYAN, a * 0.85,
         tracking=1.5, align="right")

def draw_screen(c, img, alpha=1.0):
    sx, sy, sw, sh = SCREEN
    if img is None:
        return
    image = skia.Image.fromarray(np.ascontiguousarray(img), colorType=skia.kRGBA_8888_ColorType)
    c.save()
    c.clipRRect(skia.RRect.MakeRectXY(skia.Rect.MakeXYWH(sx, sy, sw, sh), 24, 24), skia.ClipOp.kIntersect, True)
    p = skia.Paint(); p.setAlphaf(alpha)
    c.drawImageRect(image, skia.Rect.MakeXYWH(sx, sy, sw, sh), skia.SamplingOptions(skia.FilterMode.kLinear), p)
    c.restore()

# --- The finale: the orb escapes, a drawing sketches itself, the cliffhanger ------------------------
def orb(c, x, y, size, alpha=1.0):
    for r, a_, blur, col in ((150, 0.2, 60, (0.18, 0.66, 1.0)), (55, 0.55, 22, (0.18, 0.66, 1.0)), (16, 0.95, 5, (1, 1, 1))):
        p = skia.Paint(AntiAlias=True); p.setColor(skia.Color4f(*col, a_ * alpha)); p.setBlendMode(skia.BlendMode.kPlus)
        p.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, blur * size))
        c.drawCircle(x, y, r * size, p)
    p = skia.Paint(AntiAlias=True); p.setColor(skia.Color4f(1, 1, 1, alpha)); c.drawCircle(x, y, 6 * size, p)

def drawing(c, u, alpha):
    """A front and side elevation of the enclosure with dimensions, stroked in as u goes 0..1."""
    lines = []
    fw, th, lh = 322 * 1.0, 18, 6   # front view: 322 x 24 mm (tray 18, lid 6)
    k = 1.2
    ox, oy = (W - (fw + 117) * k - 90) / 2, 430  # both views centred
    def rect(x, y, w, h):
        return [(x, y, x + w, y), (x + w, y, x + w, y + h), (x + w, y + h, x, y + h), (x, y + h, x, y)]
    lines += rect(ox, oy, fw * k, (th + lh) * k * 3)
    lines += [(ox, oy + lh * k * 3, ox + fw * k, oy + lh * k * 3)]
    lines += rect(ox + fw * k + 90, oy, 117 * k, (th + lh) * k * 3)
    lines += [(ox + fw * k + 90, oy + lh * k * 3, ox + fw * k + 90 + 117 * k, oy + lh * k * 3)]
    dims = [((ox, oy + 140, ox + fw * k, oy + 140), "322"), ((ox + fw * k + 90, oy + 140, ox + fw * k + 90 + 117 * k, oy + 140), "117"),
            ((ox - 40, oy, ox - 40, oy + (th + lh) * k * 3), "24")]
    total = len(lines) + len(dims)
    p = skia.Paint(AntiAlias=True); p.setStyle(skia.Paint.kStroke_Style); p.setStrokeWidth(1.6)
    p.setColor(skia.Color4f(0.62, 0.97, 1.0, 0.85 * alpha))
    for i, (x0, y0, x1, y1) in enumerate(lines):
        v = ease(u * total - i)
        if v > 0:
            c.drawLine(x0, y0, x0 + (x1 - x0) * v, y0 + (y1 - y0) * v, p)
    q = skia.Paint(AntiAlias=True); q.setStrokeWidth(1.0); q.setColor(skia.Color4f(0.91, 0.77, 0.55, 0.8 * alpha))
    for j, ((x0, y0, x1, y1), label) in enumerate(dims):
        v = ease(u * total - len(lines) - j)
        if v <= 0:
            continue
        c.drawLine(x0, y0, x0 + (x1 - x0) * v, y0 + (y1 - y0) * v, q)
        for (ax, ay) in ((x0, y0), (x1, y1)):
            if x0 == x1:
                c.drawLine(ax - 8, ay, ax + 8, ay, q)
            else:
                c.drawLine(ax, ay - 8, ax, ay + 8, q)
        if v > 0.9:
            text(c, label, (x0 + x1) / 2 + (18 if x0 == x1 else 0), (y0 + y1) / 2 - 8 if x0 != x1 else (y0 + y1) / 2,
                 MONO(13), GOLD, alpha * (v - 0.9) * 10, tracking=1.2, align="center")

def finale(c, t, last_frame):
    """FINALE..end: the closed enclosure fades into the dark; the orb slips out of a vent and
    rises; the title; then a drawing of the enclosure sketches itself (what comes next)."""
    u = t - FINALE
    sx, sy, sw, sh = SCREEN
    fade = 1 - ease(u / 2.2)
    if fade > 0:
        draw_screen(c, last_frame, fade)
        frame_bezel(c, fade)
    hit = float(BEATS[np.searchsorted(BEATS, FINALE + 1.0)])
    ox, oy = sx + sw * 0.52, sy + sh * 0.46
    # The title holds for six seconds, then the next card for seven: both long enough to read.
    draw_t = DUR - 9.2
    if t > hit:
        v = ease((t - hit) / 2.6)
        x, y = ox + (W / 2 - ox) * v, oy + (H * 0.42 - oy) * v
        drift = ease((t - (draw_t - 1.2)) / 1.4)
        size = (0.55 + 0.35 * v) * (1 + 0.25 * kick(t)) * (1 - drift)
        orb(c, x, y - 260 * drift, max(size, 0.0001), 1 - drift)
    title_t = hit + 2.0
    if t > title_t:
        a = ease((t - title_t) / 0.8) * (1 - ease((t - (draw_t - 1.0)) / 0.8))
        text(c, "CorvCAD", W / 2, H * 0.62, DISPLAY(78, skia.FontStyle.kExtraLight_Weight), WHITE, a, tracking=2, align="center")
        text(c, "FROM THE FIRST WIRE TO THE BOX THAT HOLDS IT", W / 2, H * 0.62 + 48, MONO(14), STEEL, a, tracking=4,
             align="center")
    if t > draw_t:
        v = (t - draw_t) / 2.4
        a = 1 - ease((t - (DUR - 1.4)) / 1.2)
        drawing(c, min(v, 1.0), a)
        if v > 0.3:
            b = ease((v - 0.3) / 0.4) * a
            text(c, "NEXT", W / 2, 300, MONO(14), GOLD, b, tracking=6, align="center")
            text(c, "Drawings. Slicing. Beyond the iPad.", W / 2, 352, DISPLAY(34), WHITE, b, align="center")
            text(c, "GITHUB.COM/CORVUSAR/CORVCAD", W / 2, H - 120, MONO(14), STEEL, b, tracking=3, align="center")

# --- Main loop -----------------------------------------------------------------------------------
def main():
    start = float(opt("--from", 0)); stop = float(opt("--to", DUR))
    stills = [float(v) for v in opt("--stills").split(",")] if opt("--stills") else None
    times = stills or [i / FPS for i in range(int(start * FPS), int(stop * FPS))]
    takes = {}
    def take(name):
        if name not in takes:
            takes[name] = marks(name)
        return takes[name]
    intro = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-ss", f"{min(start, INTRO_END):.3f}", "-i", str(intro_path),
                              "-f", "rawvideo", "-pix_fmt", "rgba", "-"], stdout=subprocess.PIPE) if start < INTRO_END and not stills else None
    if stills:
        from PIL import Image
    else:
        enc = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}",
                                "-r", str(FPS), "-i", "-", "-i", str(music_dir / "Discovery.mp3"),
                                "-ss", f"{start:.3f}", "-t", f"{stop - start:.3f}", "-map", "0:v", "-map", "1:a",
                                "-c:v", "libx264", "-preset", "slow", "-crf", "15", "-pix_fmt", "yuv420p",
                                "-c:a", "aac", "-b:a", "320k", "-movflags", "+faststart", str(out_path)], stdin=subprocess.PIPE)
    reader, current, last_frame = None, None, None
    intro_still = None
    for t in times:
        surface = skia.Surface(W, H)
        c = surface.getCanvas()
        if t < INTRO_END:
            if intro:
                raw = intro.stdout.read(W * H * 4)
                frame = np.frombuffer(raw, np.uint8).reshape(H, W, 4) if len(raw) == W * H * 4 else None
            else:
                frame = None
                if stills:
                    raw = subprocess.check_output(["ffmpeg", "-loglevel", "error", "-ss", f"{t:.3f}", "-i", str(intro_path),
                                                   "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "-"])
                    frame = np.frombuffer(raw, np.uint8).reshape(H, W, 4)
            if frame is not None:
                c.drawImage(skia.Image.fromarray(np.ascontiguousarray(frame), colorType=skia.kRGBA_8888_ColorType), 0, 0)
        else:
            c.drawImage(BACKGROUND, 0, 0)
            seg = next((s for s in EDL if s[0] <= t < s[1]), None)
            if seg is None and t >= FINALE:
                finale(c, t, last_frame)
            elif seg:
                t0, t1, take_name, a, b, cap_, focus_name = seg
                m, vports = take(take_name)
                src_a = m.get(a, 0.0)
                src_b = m.get(b) or max(m.values())
                length = max(0.5, src_b - src_a)
                speed = length / (t1 - t0)
                if current is not seg or stills:
                    target = focus_rect(focus_name, take_name, vports) if focus_name else None
                    # One push-in per run of segments with the same focus: the next ones start zoomed.
                    before = EDL[EDL.index(seg) - 1] if EDL.index(seg) else None
                    zoomed = before is not None and before[2] == take_name and before[6] == focus_name
                    focus = ((target if zoomed else (0, 0, SIM_W, SIM_H)), target) if target else None
                    off = (t - t0) * speed if stills else 0
                    reader = Reader(takes_dir / f"{take_name}.mp4", src_a + off, length - off, speed, focus, off / length,
                                    push=min(1.0, 2.0 / (t1 - t0)))  # the push-in takes one bar
                    current = seg
                img = reader.frame()
                last_frame = img if img is not None else last_frame
                # Section flash on the cut, and a gentle pulse on the beat.
                draw_screen(c, img)
                frame_bezel(c, 1.0, 1 + 1.6 * kick(t))  # the glow breathes with the music
                cut = t - t0
                if cut < 0.25 and (EDL.index(seg) == 0 or seg[5] and EDL[EDL.index(seg) - 1][5] and
                                    seg[5]["act"] != EDL[EDL.index(seg) - 1][5]["act"]):
                    p = skia.Paint(); p.setColor(skia.Color4f(1, 1, 1, 0.55 * (1 - cut / 0.25))); p.setBlendMode(skia.BlendMode.kPlus)
                    c.drawRect(skia.Rect.MakeXYWH(*SCREEN), p)
                caption(c, cap_, t, t0, t1)
                badge(c, speed, t, t0, t1)
                if t0 == EDL[0][0] and t - t0 < 8:
                    text(c, "RECORDED IN THE IPAD SIMULATOR · SPED-UP SEGMENTS MARKED", W / 2, H - 22, MONO(11), STEEL,
                         ease((t - t0) / 0.5) * (1 - ease((t - t0 - 7) / 1.0)) * 0.8, tracking=2, align="center")
        rgba = surface.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType)
        if stills:
            Image.fromarray(rgba[:, :, :3]).save(str(out_path.parent / f"still-{t:06.2f}.png"))
            print("still", t, flush=True)
        else:
            enc.stdin.write(rgba.tobytes())
            if int(t * FPS) % 600 == 0:
                print(f"{t:6.1f}s", flush=True)
    if not stills:
        enc.stdin.close(); enc.wait()
        print("done", out_path)

main()
