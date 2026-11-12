"""Shared layout of the showcase: output size, the iPad frame the app footage sits in, and the
beat grid."""
import skia

W, H, FPS = 1920, 1080, 60
# The iPad screen (landscape, 2388 x 1668 simulator pixels) at 900 px tall, centred: the left
# margin (316 px) holds the captions.
SCREEN_H = 900
SCREEN_W = round(SCREEN_H * 2388 / 1668)
SCREEN = ((W - SCREEN_W) // 2, (H - SCREEN_H) // 2, SCREEN_W, SCREEN_H)
INTRO_END = 27.2  # the cut into the app: the bar on the music's accent at 27.2 s


def beats_of(music):
    """Beats and bars (analyze.py fits the grid over the whole track)."""
    return list(music["beats"]), list(music["downbeats"])


def envelope(music, key):
    """A function t -> the music's `hit` (attacks) or `low` (bass) level at t, 0..1.5."""
    values, fps = music[key], music["fps"]
    return lambda t: values[min(max(int(t * fps), 0), len(values) - 1)]


def frame_bezel(c, alpha, glow_gain=1.0):
    """The iPad's bezel and a soft glow around SCREEN, drawn over whatever is beneath."""
    x, y, w, h = SCREEN
    outer = skia.RRect.MakeRectXY(skia.Rect.MakeXYWH(x - 14, y - 14, w + 28, h + 28), 38, 38)
    inner = skia.RRect.MakeRectXY(skia.Rect.MakeXYWH(x, y, w, h), 24, 24)
    glow = skia.Paint(AntiAlias=True)
    glow.setColor(skia.Color4f(0.18, 0.62, 1.0, min(1.0, 0.16 * alpha * glow_gain)))
    glow.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, 40))
    c.save()
    c.clipRRect(inner, skia.ClipOp.kDifference, True)  # the glow stays outside the screen
    c.drawRRect(outer, glow)
    c.restore()
    body = skia.Paint(AntiAlias=True)
    body.setColor(skia.Color4f(0.035, 0.037, 0.042, alpha))
    c.drawDRRect(outer, inner, body)
    edge = skia.Paint(AntiAlias=True)
    edge.setStyle(skia.Paint.kStroke_Style); edge.setStrokeWidth(1.5)
    edge.setColor(skia.Color4f(1, 1, 1, 0.16 * alpha))
    c.drawRRect(outer, edge)
