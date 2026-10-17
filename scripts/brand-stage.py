#!/usr/bin/env python3
"""M33's branding applied to a staged copy of PPSSPP's assets (scripts/package-m33.sh), so the
PPSSPP sources stay as upstream has them:

- ui_images/images.svg: PPSSPP's I_LOGO is renamed (I_LOGO_PPSSPP), the canvas grows by the band
  brand/m33-ui.svg is drawn in (scripts/make-brand.py), and that band is added: M33's I_LOGO,
  I_ICON and the intro's images. (SVG images win over ui_images/*.png in the atlas.)
- lang/*.ini: "PPSSPP" in the UI's texts becomes "M33", except where the text is about the PPSSPP
  project itself (its team, site, forums, Gold, credits).

  brand-stage.py <stage>/assets <repo>
"""
import os
import re
import sys

UNIT_PX = 380 / 100.54165  # images.svg's pixels per unit

# Texts that stay about PPSSPP: the project, its people and its services.
KEEP = re.compile(r"ppsspp\.org|PPSSPP (team|project|Gold|Forums|forums)|PPSSPP_|PPSSPPWindows|"
                  r"regular/Gold|Multiplayer Wiki|Check out PPSSPP|Share PPSSPP|translating PPSSPP|"
                  r"translated PPSSPP|PPSSPP uses to identify|PPSSPP won't use", re.I)


def brand_svg(assets, repo):
    path = os.path.join(assets, "ui_images", "images.svg")
    s = open(path, encoding="utf-8").read()
    frag = open(os.path.join(repo, "brand", "m33-ui.svg"), encoding="utf-8").read()
    if "I_M33_INTRO" in s:
        return
    n = s.count('id="I_LOGO"')
    if n != 1:
        sys.exit(f"images.svg: expected one I_LOGO, found {n}")
    s = s.replace('id="I_LOGO"', 'id="I_LOGO_PPSSPP"')
    # M33's I_ICON wins over ui_images/icon.png only if the SVG has none of its own
    if 'id="I_ICON"' in s:
        sys.exit("images.svg: has an I_ICON of its own")
    m = re.search(r'<svg\b[^>]*>', s)
    root = m.group(0)
    vb = re.search(r'viewBox="([^"]+)"', root).group(1).split()
    w0, h0 = float(vb[2]), float(vb[3])
    fw = float(re.search(r"CANVAS_W = ([0-9.]+)", open(os.path.join(repo, "scripts", "make-brand.py")).read()).group(1))
    fh = 312.0
    w1, h1 = max(w0, fw), max(h0, fh)
    new_root = re.sub(r'viewBox="[^"]+"', f'viewBox="0 0 {w1:g} {h1:g}"', root)
    new_root = re.sub(r'\bwidth="[^"]+"', f'width="{w1 * UNIT_PX:.0f}"', new_root, count=1)
    new_root = re.sub(r'\bheight="[^"]+"', f'height="{h1 * UNIT_PX:.0f}"', new_root, count=1)
    s = s.replace(root, new_root, 1)
    end = s.rindex("</svg>")
    s = s[:end] + frag + s[end:]
    open(path, "w", encoding="utf-8", newline="\n").write(s)
    print(f"images.svg: M33 images added, canvas {w0:g}x{h0:g} -> {w1:g}x{h1:g} units")


def brand_lang(assets):
    changed = 0
    lang = os.path.join(assets, "lang")
    for name in sorted(os.listdir(lang)):
        if not name.endswith(".ini"):
            continue
        path = os.path.join(lang, name)
        lines = open(path, encoding="utf-8").read().split("\n")
        out = []
        for line in lines:
            if "=" in line and not line.lstrip().startswith(("#", ";", "[")):
                key, value = line.split("=", 1)
                if "PPSSPP" in value and not KEEP.search(line):
                    value = value.replace("PPSSPP", "M33")
                    changed += 1
                line = key + "=" + value
            out.append(line)
        open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print(f"lang: {changed} texts now say M33")


if __name__ == "__main__":
    assets, repo = sys.argv[1], sys.argv[2]
    brand_svg(assets, repo)
    brand_lang(assets)
