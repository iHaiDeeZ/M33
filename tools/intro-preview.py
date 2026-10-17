# M33 tools: renders the opening as a GIF from the images tools/svg-raster.c extracted (out/<ID>.rgba).
# Mirrors ps4/PS4Brand.cpp (M33_DrawIntro) in the PPSSPP patch: keep the two in step.
# Mirrors ps4/PS4Brand.cpp M33_DrawIntro with PIL, to preview the timing and look as a GIF.
import math
from PIL import Image, ImageDraw, ImageFilter

W, H = 960, 540
FPS = 20
SECONDS = 4.2
PI = math.pi


def load(id_):
    data = open(f"out/{id_}.rgba", "rb").read()
    hdr, rest = data.split(b"\n", 1)
    w, h = map(int, hdr.split())
    return Image.frombytes("RGBA", (w, h), rest[:w * h * 4])


IMG = {k: load(k) for k in ("I_M33_INTRO", "I_M33_MARCH", "I_M33_TAGLINE")}


def smooth(a, b, t):
    x = min(1, max(0, (t - a) / (b - a)))
    return x * x * (3 - 2 * x)


def ease(x):
    x = min(1, max(0, x))
    return 1 - (1 - x) ** 3


def bump(a, b, t):
    return 0 if t <= a or t >= b else math.sin((t - a) / (b - a) * PI)


def brand(u):
    u = min(1, max(0, u))
    return (int(160 + (64 - 160) * u), int(92 + (220 - 92) * u), 255)


def stadium(half, r, s):
    arc = PI * r
    if s < half: return s, -r
    s -= half
    if s < arc:
        a = -PI / 2 + s / r; return half + math.cos(a) * r, math.sin(a) * r
    s -= arc
    if s < 2 * half: return half - s, r
    s -= 2 * half
    if s < arc:
        a = PI / 2 + s / r; return -half + math.cos(a) * r, math.sin(a) * r
    s -= arc
    return -half + s, -r


def paste(canvas, img, cx, cy, width, tint, alpha):
    h = int(width * img.height / img.width)
    im = img.resize((max(1, int(width)), max(1, h)), Image.LANCZOS)
    r, g, b, a = im.split()
    col = Image.new("RGBA", im.size, tint + (0,))
    col.putalpha(a.point(lambda v: int(v * alpha)))
    canvas.alpha_composite(col, (int(cx - im.width / 2), int(cy - im.height / 2)))
    return im


def frame(t):
    fade = 1 - smooth(SECONDS - 0.6, SECONDS, t)
    bg_in = smooth(0, 0.5, t)
    cx, cy = W / 2, H * 0.42
    c = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    # gradient field
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for y in range(H):
        u = y / H
        col = tuple(int(a + (b - a) * u) for a, b in zip((20, 12, 51), (5, 4, 16)))
        gd.line([(0, y), (W, y)], fill=col + (int(255 * bg_in * fade),))
    c.alpha_composite(grad)
    # glow discs
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gl = ImageDraw.Draw(glow)
    g = bg_in * fade * (0.85 + 0.15 * bump(1.9, 2.7, t))
    for i in range(10):
        r = H * (0.12 + 0.05 * i)
        gl.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(70, 38, 160, int(255 * 0.045 * g)))
        c.alpha_composite(glow); glow = Image.new("RGBA", (W, H), (0, 0, 0, 0)); gl = ImageDraw.Draw(glow)
    # grid
    grid = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gr = ImageDraw.Draw(grid)
    horizon = H * 0.80
    ga = int(255 * 0.16 * bg_in * fade)
    for i in range(-16, 17):
        gr.line([(cx + i * W * 0.0135, horizon), (cx + i * W * 0.16, H)], fill=(120, 90, 255, ga))
    drift = (t * 0.35) % 1
    for k in range(9):
        f = (k + drift) / 9
        y = horizon + (H - horizon) * f * f
        gr.line([(0, y), (W, y)], fill=(120, 90, 255, int(ga * min(1, f * 3))))
    c.alpha_composite(grid)
    # ring
    ring_w = min(W * 0.60, H * 1.25)
    ring_h = ring_w * 0.42
    r = ring_h / 2
    half = (ring_w - ring_h) / 2
    per = 4 * half + 2 * PI * r
    p = ease(smooth(0.25, 1.35, t))
    S = 2  # supersample the strokes
    if p > 0:
        rl = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
        rd = ImageDraw.Draw(rl)
        reach = p * per / 2
        for side in (-1, 1):
            px, py = stadium(half, r, 0)
            for i in range(1, 181):
                x, y = stadium(half, r, reach * i / 180)
                bx = cx + side * x
                col = brand((bx - (cx - ring_w / 2)) / ring_w)
                rd.line([((cx + side * px) * S, (cy + py) * S), (bx * S, (cy + y) * S)], fill=col + (int(255 * 0.95 * fade),), width=3 * S)
                px, py = x, y
        rl = rl.resize((W, H), Image.LANCZOS)
        soft = rl.filter(ImageFilter.GaussianBlur(4))
        c.alpha_composite(soft); c.alpha_composite(rl)
        if p < 0.999:
            x, y = stadium(half, r, reach)
            tips = ImageDraw.Draw(c)
            for side in (-1, 1):
                tx, ty = cx + side * x, cy + y
                tips.ellipse([tx - 4, ty - 4, tx + 4, ty + 4], fill=(255, 255, 255, int(230 * fade)))
    # logo
    la = smooth(0.75, 1.55, t) * fade
    if la > 0:
        zoom = 1.12 - 0.12 * ease(smooth(0.75, 1.55, t))
        lw = ring_w * 0.70 * zoom
        pulse = 1 + 0.8 * bump(1.9, 2.7, t)
        gl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        paste(gl, IMG["I_M33_INTRO"], cx, cy, lw, (160, 92, 255), min(1, 0.6 * la * pulse))
        c.alpha_composite(gl.filter(ImageFilter.GaussianBlur(9 * pulse)))
        gl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        paste(gl, IMG["I_M33_INTRO"], cx, cy + 2, lw, (64, 220, 255), min(1, 0.5 * la * pulse))
        c.alpha_composite(gl.filter(ImageFilter.GaussianBlur(3 * pulse)))
        paste(c, IMG["I_M33_INTRO"], cx, cy, lw, (240, 244, 255), la)
    # accent line
    line_y = cy + r + H * 0.05
    lp = ease(smooth(1.3, 1.9, t))
    if lp > 0:
        hw = ring_w * 0.33 * lp
        d = ImageDraw.Draw(c)
        for i in range(48):
            u = i / 48
            x1 = cx - hw + 2 * hw * u
            edge = 1 - abs(u * 2 - 1)
            d.rectangle([x1, line_y, x1 + 2 * hw / 48 + 0.5, line_y + 2], fill=brand(u) + (int(255 * (0.25 + 0.75 * edge) * fade),))
    ma = smooth(1.6, 2.2, t) * fade
    if ma > 0:
        rise = (1 - ease(smooth(1.6, 2.2, t))) * 7
        paste(c, IMG["I_M33_MARCH"], cx, line_y + H * 0.055 + rise, ring_w * 0.44, (226, 230, 255), ma)
    ta = smooth(1.9, 2.5, t) * fade
    if ta > 0:
        paste(c, IMG["I_M33_TAGLINE"], cx, line_y + H * 0.115, ring_w * 0.54, (154, 164, 216), ta)
    ca = smooth(2.2, 2.7, t) * fade
    if ca > 0:
        d = ImageDraw.Draw(c)
        d.text((cx, H - H * 0.07), "Based on PPSSPP by Henrik Rydgård · Free Software under GPL 2.0+", fill=(140, 144, 190, int(255 * ca)), anchor="ms")
    return c.convert("RGB")


frames = [frame(i / FPS) for i in range(int(SECONDS * FPS) + 1)]
frames[0].save("m33-intro-preview.gif", save_all=True, append_images=frames[1:], duration=int(1000 / FPS), loop=0)
frame(2.6).save("m33-intro-still.png")
print("frames", len(frames))
