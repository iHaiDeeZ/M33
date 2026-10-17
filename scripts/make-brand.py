#!/usr/bin/env python3
"""M33's brand inside the app, from the Michroma font (art/Michroma-Regular.ttf):

- brand/m33-ui.svg: vector images for PPSSPP's UI atlas (assets/ui_images/images.svg), merged in
  by scripts/brand-stage.py when packaging: I_LOGO (the M33 wordmark next to the icon on the main
  screen), I_ICON (the app badge), and the intro's I_M33_INTRO (big wordmark), I_M33_MARCH
  ("MARCH33") and I_M33_TAGLINE. Letters are glyph outlines (nanosvg has no text), white, so the
  app can tint them; the icon carries its own colors.
- assets/m33_background.jpg: the menus' background (the home screen art, without the lettering).

Coordinates are images.svg units (1 unit = 3.78 px at 1x). The images sit in a band below the
existing ones (y >= BAND_Y), which brand-stage.py makes room for.
Needs Pillow and fontTools:  py scripts/make-brand.py
"""
import math
import os

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
FONT = os.path.join(REPO, "art", "Michroma-Regular.ttf")

BAND_Y = 200.0       # below PPSSPP's images (its canvas is 193.2 units tall)
CANVAS_W = 200.0     # brand-stage.py widens the canvas to this
CANVAS_H = 200.0 + 112.0

VIOLET = "#A05CFF"
CYAN = "#40DCFF"


class Font:
    def __init__(self, path):
        self.tt = TTFont(path)
        self.glyphs = self.tt.getGlyphSet()
        self.cmap = self.tt.getBestCmap()
        self.upem = self.tt["head"].unitsPerEm
        self.cap = getattr(self.tt["OS/2"], "sCapHeight", 0) or int(self.upem * 0.7)
        self.hmtx = self.tt["hmtx"]

    def advance(self, ch):
        return self.hmtx[self.cmap[ord(ch)]][0]

    def outline(self, ch):
        pen = SVGPathPen(self.glyphs)
        self.glyphs[self.cmap[ord(ch)]].draw(pen)
        return pen.getCommands()

    def width(self, text, track):
        """Width in font units, `track` extra font units between letters."""
        return sum(self.advance(c) for c in text) + track * (len(text) - 1)

    def text(self, text, x, y, cap_height, track_em=0.0, fill="#FFFFFF", center=False):
        """SVG paths for `text`, baseline at y, capitals `cap_height` units tall."""
        s = cap_height / self.cap
        track = track_em * self.upem
        if center:
            x -= self.width(text, track) * s / 2
        out = []
        pen_x = 0.0
        for ch in text:
            if ch != " ":
                d = self.outline(ch)
                out.append(f'<path d="{d}" fill="{fill}" transform="translate({x + pen_x * s:.4f} {y:.4f}) scale({s:.6f} {-s:.6f})"/>')
            pen_x += self.advance(ch) + track
        return out, self.width(text, track) * s


def stadium(x, y, w, h, stroke, color, width):
    r = h / 2
    return f'<rect x="{x:.3f}" y="{y:.3f}" width="{w:.3f}" height="{h:.3f}" rx="{r:.3f}" ry="{r:.3f}" fill="none" stroke="{color}" stroke-width="{width:.3f}"/>'


def ui_svg(font):
    parts = [
        "<!-- M33 brand images for PPSSPP's UI atlas (scripts/make-brand.py; merged by scripts/brand-stage.py) -->",
        # nanosvg reads gradient coordinates as percentages of the shape (0/1 alone were ignored)
        "<defs>",
        '<linearGradient id="m33IconBg" x1="0%" y1="0%" x2="0%" y2="100%">'
        '<stop offset="0" stop-color="#2E1A6E"/><stop offset="1" stop-color="#0B0820"/></linearGradient>',
        '<linearGradient id="m33Ring" x1="0%" y1="0%" x2="100%" y2="0%">'
        f'<stop offset="0" stop-color="{VIOLET}"/><stop offset="1" stop-color="{CYAN}"/></linearGradient>',
        '<linearGradient id="m33Letters" x1="0%" y1="0%" x2="0%" y2="100%">'
        '<stop offset="0" stop-color="#FFFFFF"/><stop offset="1" stop-color="#D6E2FF"/></linearGradient>',
        "</defs>",
    ]

    # I_ICON: 19 x 19 units (72 px at 1x, as PPSSPP's icon.png): a rounded badge, the stadium ring
    # (a PSP's outline) and M33.
    x0, y0, size = 2.0, BAND_Y + 2.0, 19.0
    icon = [
        f'<rect x="{x0}" y="{y0}" width="{size}" height="{size}" rx="4.2" ry="4.2" fill="url(#m33IconBg)"/>',
        stadium(x0 + 1.5, y0 + 5.3, size - 3.0, 8.4, 0, "url(#m33Ring)", 0.6),
    ]
    # Michroma is wide: capitals 3 units tall make M33 about 12 wide, inside the ring
    letters, _ = font.text("M33", x0 + size / 2, y0 + 5.3 + 4.2 + 1.5, 3.0, track_em=0.03, fill="url(#m33Letters)", center=True)
    icon += letters
    parts.append('<g id="I_ICON">' + "".join(icon) + "</g>")

    # I_LOGO: the header wordmark, about PPSSPP's logo's height (41 px = 10.8 units)
    letters, w = font.text("M33", 26.0, BAND_Y + 12.0, 9.0, track_em=0.06, fill="url(#m33Letters)")
    parts.append('<g id="I_LOGO">' + "".join(letters) + "</g>")

    # The intro's images, big (scaled down when drawn): white, tinted at run time.
    letters, _ = font.text("M33", 4.0, BAND_Y + 62.0, 44.0, track_em=0.06)
    parts.append('<g id="I_M33_INTRO">' + "".join(letters) + "</g>")
    letters, _ = font.text("MARCH33", 4.0, BAND_Y + 80.0, 9.0, track_em=0.55)
    parts.append('<g id="I_M33_MARCH">' + "".join(letters) + "</g>")
    letters, _ = font.text("PSP EMULATOR FOR PS4", 4.0, BAND_Y + 96.0, 5.0, track_em=0.30)
    parts.append('<g id="I_M33_TAGLINE">' + "".join(letters) + "</g>")
    return "\n".join(parts) + "\n"


def background():
    """The menus' background: deep indigo, a soft glow, a faint perspective grid and ring."""
    W, H = 1920, 1080
    w, h = W // 2, H // 2
    img = Image.new("RGB", (w, h))
    px = img.load()
    cx, cy = w * 0.5, h * 0.42
    reach = math.hypot(w * 0.62, h * 0.7)
    inner, outer = (34, 20, 78), (5, 4, 15)
    for y in range(h):
        for x in range(w):
            t = min(1.0, math.hypot(x - cx, (y - cy) * 1.25) / reach) ** 0.85
            px[x, y] = tuple(int(a + (b - a) * t) for a, b in zip(inner, outer))
    img = img.resize((W, H), Image.BICUBIC).convert("RGBA")
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    horizon = H * 0.80
    for i in range(-16, 17):
        d.line([(W / 2 + i * 26, horizon), (W / 2 + i * 300, H)], fill=(120, 90, 255, 34), width=1)
    for k in range(1, 9):
        y = horizon + (H - horizon) * (k / 8) ** 1.8
        d.line([(0, y), (W, y)], fill=(120, 90, 255, 30), width=1)
    ring = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rw, rh = 1500, 630
    rd.rounded_rectangle([(W - rw) / 2, H * 0.42 - rh / 2, (W + rw) / 2, H * 0.42 + rh / 2], radius=rh / 2,
                         outline=(150, 110, 255, 40), width=3)
    ring = ring.filter(ImageFilter.GaussianBlur(1.2))
    img = Image.alpha_composite(Image.alpha_composite(img, layer), ring)
    out = os.path.join(REPO, "assets", "m33_background.jpg")
    img.convert("RGB").save(out, quality=90)
    return out


def preview(font_path):
    """brand/preview-icon.png: I_ICON as drawn at 4x (a rough PIL rendering, for review)."""
    from PIL import ImageFont
    S = 4 * 72
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (S, S))
    gp = grad.load()
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(a + (b - a) * t) for a, b in zip((46, 26, 110), (11, 8, 32)))
        for x in range(S):
            gp[x, y] = c + (255,)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=S * 4.2 / 19, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    k = S / 19
    d.rounded_rectangle([1.6 * k, 5.2 * k, (19 - 1.6) * k, (5.2 + 8.6) * k], radius=4.3 * k, outline=(130, 120, 255), width=int(0.55 * k))
    f = ImageFont.truetype(font_path, int(5.6 * k))
    d.text((S / 2, 11.5 * k), "M33", font=f, fill=(244, 246, 255), anchor="ms")
    out = os.path.join(REPO, "brand", "preview-icon.png")
    img.save(out)
    return out


if __name__ == "__main__":
    font = Font(FONT)
    os.makedirs(os.path.join(REPO, "brand"), exist_ok=True)
    os.makedirs(os.path.join(REPO, "assets"), exist_ok=True)
    with open(os.path.join(REPO, "brand", "m33-ui.svg"), "w", encoding="utf-8", newline="\n") as f:
        f.write(ui_svg(font))
    print("wrote brand/m33-ui.svg")
    print("wrote", os.path.relpath(background(), REPO))
    print("wrote", os.path.relpath(preview(FONT), REPO))
