#!/usr/bin/env python3
"""M33's home screen art from the Blender renders (scripts/blender/psp_scene.py: macro_cut, macro_wide):
the handheld close up, on black, with the M33 lettering in the open space on the right.

  sce_sys/icon0.png              512x512, the PS4 home screen tile
  sce_sys/pic1.png               1920x1080, the background behind it and the boot splash
  assets/ui_images/icon.png      72x72, the app's icon inside the UI (main screen, logo)

  py scripts/make-icon.py <macro_cut.png> <macro_wide.png>
Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
FONT = os.path.join(REPO, "art", "Michroma-Regular.ttf")
VIOLET = (160, 92, 255)
CYAN = (64, 220, 255)


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def tracked(draw, x, y, text, font, track, fill, align="left"):
    widths = [font.getlength(c) for c in text]
    total = sum(widths) + track * (len(text) - 1)
    if align == "right":
        x -= total
    elif align == "center":
        x -= total / 2
    for c, w in zip(text, widths):
        draw.text((x, y), c, font=font, fill=fill, anchor="ls")
        x += w + track
    return total


def accent(img, x0, x1, y, h):
    """The brand line: violet to cyan, fading at both ends."""
    d = ImageDraw.Draw(img)
    for x in range(int(x0), int(x1)):
        t = (x - x0) / max(1, x1 - x0)
        a = int(255 * (0.25 + 0.75 * (1 - abs(t * 2 - 1))))
        d.line([(x, y), (x, y + h)], fill=lerp(VIOLET, CYAN, t) + (a,))


def lettering(canvas, right, top, big, show_tag=True):
    """M33 over the accent line, MARCH33 and the tagline, right-aligned at `right`, with a soft glow."""
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    f_big = ImageFont.truetype(FONT, big)
    w = tracked(d, right, top + big, "M33", f_big, big * 0.06, (244, 246, 255, 255), "right")
    glow = layer.filter(ImageFilter.GaussianBlur(big * 0.18))
    tint = Image.new("RGBA", canvas.size, VIOLET + (0,))
    tint.putalpha(glow.split()[3].point(lambda v: int(v * 0.55)))
    canvas.alpha_composite(tint)
    canvas.alpha_composite(layer)
    y = top + big + big * 0.22
    accent(canvas, right - w, right, int(y), max(2, big // 40))
    if show_tag:
        d = ImageDraw.Draw(canvas)
        f_mid = ImageFont.truetype(FONT, max(10, int(big * 0.21)))
        tracked(d, right, y + big * 0.42, "MARCH33", f_mid, big * 0.105, (226, 230, 255, 255), "right")
        f_small = ImageFont.truetype(FONT, max(8, int(big * 0.105)))
        tracked(d, right, y + big * 0.66, "PSP EMULATOR FOR PS4", f_small, big * 0.035, (150, 158, 210, 255), "right")


def on_black(render, size):
    bg = Image.new("RGBA", size, (0, 0, 0, 255))
    bg.alpha_composite(render.resize(size, Image.LANCZOS))
    return bg


def tile(cut):
    img = on_black(cut, (1024, 1024))
    lettering(img, 960, 70, 150, show_tag=False)
    d = ImageDraw.Draw(img)
    f_mid = ImageFont.truetype(FONT, 30)
    tracked(d, 960, 70 + 150 + 33 + 62, "MARCH33", f_mid, 15, (226, 230, 255, 255), "right")
    return img.resize((512, 512), Image.LANCZOS).convert("RGB")


def background(wide):
    img = on_black(wide, (1920, 1080))
    lettering(img, 1800, 330, 230)
    return img.convert("RGB")


def small_icon(cut):
    """72x72 for the UI: the face buttons and screen corner, rounded like PPSSPP's own icon."""
    img = on_black(cut, (1024, 1024))
    crop = img.crop((180, 160, 900, 880)).resize((72, 72), Image.LANCZOS)
    mask = Image.new("L", (72 * 4, 72 * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 72 * 4 - 1, 72 * 4 - 1], radius=16 * 4, fill=255)
    out = Image.new("RGBA", (72, 72), (0, 0, 0, 0))
    out.paste(crop, (0, 0), mask.resize((72, 72), Image.LANCZOS))
    return out


if __name__ == "__main__":
    cut = Image.open(sys.argv[1]).convert("RGBA")
    wide = Image.open(sys.argv[2]).convert("RGBA")
    tile(cut).save(os.path.join(REPO, "sce_sys", "icon0.png"), optimize=True)
    background(wide).save(os.path.join(REPO, "sce_sys", "pic1.png"), optimize=True)
    os.makedirs(os.path.join(REPO, "assets", "ui_images"), exist_ok=True)
    small_icon(cut).save(os.path.join(REPO, "assets", "ui_images", "icon.png"), optimize=True)
    print("wrote sce_sys/icon0.png, sce_sys/pic1.png, assets/ui_images/icon.png")
