# The startup clip's placeholder for the handheld's screen, for the public package (the user's own
# PSP startup video, Sony's, stays out of it): M33's own boot - a violet glow blooming on a dark
# screen, the M33 wordmark fading in with "March33" under it, a light sweep, a soft synthesized
# chime. Same length and rate as the video it stands in for (130 frames at 30 fps), so
# startup_anim.py's timeline doesn't change.
#
#   python3 scripts/blender/placeholder_screen.py <out dir> <font.ttf>
#   -> <out dir>/video_frames/v_0001.png ... and <out dir>/chime.wav
#   then blender -b -P scripts/blender/placeholder_video.py -- <out dir>  (the .mp4 the scripts take)
import math
import os
import struct
import sys
import wave

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT, FONT = sys.argv[1], sys.argv[2]
W, H, FPS, N = 1280, 720, 30, 130
ACCENT = (155, 123, 255)
frames_dir = os.path.join(OUT, "video_frames")
os.makedirs(frames_dir, exist_ok=True)

big = ImageFont.truetype(FONT, 190)
small = ImageFont.truetype(FONT, 34)


def smooth(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def text_size(font, text):
    box = font.getbbox(text)
    return box[2] - box[0], box[3] - box[1], box


for i in range(N):
    t = i / FPS
    img = Image.new("RGB", (W, H), (4, 6, 14))
    # the glow: a violet bloom from the middle, growing then settling
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    g = ImageDraw.Draw(glow)
    bloom = smooth(0.0, 0.9, t)
    r = 120 + 520 * bloom
    for k in range(14, 0, -1):
        rr = r * k / 14.0
        a = (1.0 - k / 14.0) * 0.55 * bloom * (0.85 + 0.15 * math.sin(t * 2.2))
        col = tuple(int(c * a) for c in ACCENT)
        g.ellipse((W / 2 - rr * 1.6, H / 2 - rr, W / 2 + rr * 1.6, H / 2 + rr), fill=col)
    glow = glow.filter(ImageFilter.GaussianBlur(40))
    img = ImageChops.add(img, glow)
    d = ImageDraw.Draw(img)
    # the wordmark, fading and rising a little into place
    word = smooth(0.7, 1.6, t)
    if word > 0:
        tw, th, box = text_size(big, "M33")
        x = (W - tw) / 2 - box[0]
        y = (H - th) / 2 - box[1] - 40 + 18 * (1 - word)
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        ld.text((x, y), "M33", font=big, fill=(255, 255, 255, int(255 * word)))
        # the light sweep across the letters
        sweep = smooth(1.6, 2.6, t)
        if 0 < sweep < 1:
            mask = Image.new("L", (W, H), 0)
            md = ImageDraw.Draw(mask)
            sx = -200 + (W + 400) * sweep
            md.polygon([(sx, 0), (sx + 90, 0), (sx - 60, H), (sx - 150, H)], fill=200)
            mask = mask.filter(ImageFilter.GaussianBlur(30))
            shine = Image.new("RGBA", (W, H), (210, 200, 255, 0))
            shine.putalpha(Image.composite(mask, Image.new("L", (W, H), 0), layer.getchannel("A")))
            layer = Image.alpha_composite(layer, shine)
        img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")
        d = ImageDraw.Draw(img)
        sub = smooth(1.3, 2.1, t)
        if sub > 0:
            sw, sh, sbox = text_size(small, "MARCH33")
            col = tuple(int(c * sub) for c in ACCENT)
            d.text(((W - sw) / 2 - sbox[0], y + box[3] + 34 - sbox[1]), "MARCH33", font=small, fill=col)
    img.save(os.path.join(frames_dir, "v_%04d.png" % (i + 1)))

# the chime: two soft bell notes (E5, then B5 over it), a little stereo spread
RATE = 48000
dur = N / FPS
samples = []
notes = [(659.25, 0.25, 0.0), (987.77, 0.95, 0.15)]  # (Hz, start s, pan)
for n in range(int(dur * RATE)):
    t = n / RATE
    left = right = 0.0
    for f, start, pan in notes:
        if t < start:
            continue
        u = t - start
        env = (1 - math.exp(-u * 60)) * math.exp(-u * 1.6)
        v = env * (math.sin(2 * math.pi * f * u) * 0.6 + math.sin(2 * math.pi * f * 2.01 * u) * 0.18 + math.sin(2 * math.pi * f * 3.0 * u) * 0.07)
        left += v * (1 - pan)
        right += v * (1 + pan)
    fade = min(1.0, (dur - t) / 0.6)
    samples.append((int(max(-1, min(1, left * 0.32 * fade)) * 32767), int(max(-1, min(1, right * 0.32 * fade)) * 32767)))
with wave.open(os.path.join(OUT, "chime.wav"), "wb") as wf:
    wf.setnchannels(2)
    wf.setsampwidth(2)
    wf.setframerate(RATE)
    wf.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in samples))
print("placeholder:", N, "frames,", round(dur, 2), "s")
