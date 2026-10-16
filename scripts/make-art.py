#!/usr/bin/env python3
"""M33's home screen art: sce_sys/icon0.png (512x512, the tile) and sce_sys/pic1.png (1920x1080, the
background behind it and the boot splash). "M33" in white Michroma letters with a violet-to-cyan
glow over a deep indigo field with a faint PSP-shaped outline, "MARCH33" and "PSP EMULATOR FOR PS4"
below. Needs Pillow:  py scripts/make-art.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
FONT = os.path.join(REPO, "art", "Michroma-Regular.ttf")
OUT = os.path.join(REPO, "sce_sys")

BG_IN = (38, 22, 84)
BG_OUT = (6, 4, 18)
VIOLET = (160, 92, 255)
CYAN = (64, 220, 255)
WHITE = (255, 255, 255)


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def background(w, h, cx, cy):
    """Radial indigo glow around (cx, cy) fading to near black."""
    img = Image.new("RGB", (w, h), BG_OUT)
    px = img.load()
    reach = math.hypot(max(cx, w - cx), max(cy, h - cy))
    for y in range(h):
        for x in range(w):
            t = min(1.0, math.hypot(x - cx, y - cy) / reach)
            px[x, y] = lerp(BG_IN, BG_OUT, t ** 0.8)
    return img


def gradient_text(size, text, font, track, top, bottom):
    """Text with a vertical top->bottom color gradient, on a transparent layer."""
    mask = Image.new("L", size, 0)
    d = ImageDraw.Draw(mask)
    x = 0
    for ch in text:
        d.text((x, 0), ch, font=font, fill=255)
        x += font.getlength(ch) + track
    bbox = mask.getbbox()
    mask = mask.crop(bbox)
    w, h = mask.size
    grad = Image.new("RGB", (w, h))
    gp = grad.load()
    for y in range(h):
        c = lerp(top, bottom, y / max(1, h - 1))
        for x in range(w):
            gp[x, y] = c
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    layer.paste(grad, (0, 0), mask)
    return layer, mask


def spaced_text(draw, xy, text, font, track, fill, anchor_center=True):
    widths = [font.getlength(ch) for ch in text]
    total = sum(widths) + track * (len(text) - 1)
    x, y = xy
    if anchor_center:
        x -= total / 2
    for ch, cw in zip(text, widths):
        draw.text((x, y), ch, font=font, fill=fill)
        x += cw + track
    return total


def psp_outline(draw, cx, cy, w, color, width, details=True):
    """A PSP-1000 silhouette: a stadium body (with details: the screen, D-pad and face buttons)."""
    h = w * 0.42
    r = h / 2
    box = [cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2]
    draw.rounded_rectangle(box, radius=r, outline=color, width=width)
    if not details:
        return
    sw, sh = w * 0.52, h * 0.66
    draw.rounded_rectangle([cx - sw / 2, cy - sh / 2, cx + sw / 2, cy + sh / 2], radius=width * 2, outline=color, width=width)
    # D-pad and face buttons
    dx = cx - w * 0.375
    s = h * 0.09
    draw.rectangle([dx - s, cy - s * 3, dx + s, cy + s * 3], outline=color, width=max(1, width // 2))
    draw.rectangle([dx - s * 3, cy - s, dx + s * 3, cy + s], outline=color, width=max(1, width // 2))
    bx = cx + w * 0.375
    for ox, oy in ((0, -2.4), (0, 2.4), (-2.4, 0), (2.4, 0)):
        draw.ellipse([bx + ox * s - s, cy + oy * s - s, bx + ox * s + s, cy + oy * s + s], outline=color, width=max(1, width // 2))


def logo(canvas, cx, cy, size):
    """The M33 mark centered on (cx, cy): glow, then the gradient letters."""
    font = ImageFont.truetype(FONT, size)
    layer, mask = gradient_text((int(size * 4), int(size * 1.6)), "M33", font, int(size * 0.06), WHITE, (214, 226, 255))
    w, h = layer.size
    x, y = int(cx - w / 2), int(cy - h / 2)
    glow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    tint = Image.new("RGBA", (w, h), VIOLET + (255,))
    glow.paste(tint, (x, y), mask)
    glow2 = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    glow2.paste(Image.new("RGBA", (w, h), CYAN + (255,)), (x, y + int(size * 0.05)), mask)
    for g, r in ((glow, size * 0.28), (glow2, size * 0.12)):
        canvas.alpha_composite(g.filter(ImageFilter.GaussianBlur(r)))
    canvas.alpha_composite(layer, (x, y))
    return w, h


def accent_line(draw, cx, y, half, thickness):
    for i in range(int(-half), int(half)):
        t = (i + half) / (2 * half)
        c = lerp(VIOLET, CYAN, t)
        a = 1.0 - abs(t - 0.5) * 2
        c = lerp(BG_OUT, c, 0.25 + 0.75 * a)
        draw.line([(cx + i, y), (cx + i, y + thickness)], fill=c)


def icon():
    W = H = 512
    img = background(W, H, W / 2, H * 0.42).convert("RGBA")
    d = ImageDraw.Draw(img)
    psp_outline(d, W / 2, H * 0.40, W * 0.90, (88, 70, 150, 255), 3, details=False)
    logo(img, W / 2, H * 0.39, 116)
    d = ImageDraw.Draw(img)
    accent_line(d, W / 2, int(H * 0.66), W * 0.30, 3)
    sub = ImageFont.truetype(FONT, 34)
    spaced_text(d, (W / 2, H * 0.71), "MARCH33", sub, 10, (226, 232, 255))
    small = ImageFont.truetype(FONT, 17)
    spaced_text(d, (W / 2, H * 0.83), "PSP EMULATOR", small, 5, (150, 160, 210))
    img.convert("RGB").save(os.path.join(OUT, "icon0.png"), optimize=True)


def picture():
    W, H = 1920, 1080
    # rendered at half size then scaled: the gradient is per pixel
    small = background(W // 2, H // 2, W * 0.33 / 2, H * 0.48 / 2).resize((W, H), Image.BICUBIC).convert("RGBA")
    img = small
    d = ImageDraw.Draw(img)
    # a faint perspective grid towards the bottom
    horizon = H * 0.78
    for i in range(-14, 15):
        x0 = W / 2 + i * 120
        d.line([(W / 2 + i * 22, horizon), (x0 * 1.0 + i * 260, H)], fill=(44, 30, 96, 255), width=1)
    for k in range(1, 9):
        y = horizon + (H - horizon) * (k / 8) ** 1.8
        d.line([(0, y), (W, y)], fill=(40, 28, 88, 255), width=1)
    psp_outline(d, W * 0.33, H * 0.40, 900, (70, 56, 128, 255), 4, details=False)
    logo(img, W * 0.33, H * 0.39, 200)
    d = ImageDraw.Draw(img)
    accent_line(d, W * 0.33, int(H * 0.60), 300, 4)
    sub = ImageFont.truetype(FONT, 52)
    spaced_text(d, (W * 0.33, H * 0.625), "MARCH33", sub, 16, (226, 232, 255))
    small_font = ImageFont.truetype(FONT, 24)
    spaced_text(d, (W * 0.33, H * 0.705), "PSP EMULATOR FOR PS4  ·  POWERED BY PPSSPP", small_font, 6, (150, 160, 210))
    img.convert("RGB").save(os.path.join(OUT, "pic1.png"), optimize=True)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    icon()
    picture()
    print("wrote sce_sys/icon0.png and sce_sys/pic1.png")
