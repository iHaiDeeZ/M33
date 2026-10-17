import struct
import sys

from PIL import Image


def unswizzle(data, width_bytes, height):
    # PSP "faster" order: 16-byte x 8-row blocks
    out = bytearray(len(data))
    bw = width_bytes // 16
    i = 0
    for by in range(0, height, 8):
        for bx in range(bw):
            for y in range(8):
                if by + y >= height:
                    i += 16
                    continue
                dst = (by + y) * width_bytes + bx * 16
                out[dst:dst + 16] = data[i:i + 16]
                i += 16
    return bytes(out)


def decode(path):
    d = open(path, "rb").read()
    assert d[:11] == b"MIG.00.1PSP", d[:16]
    pal = None
    img = None

    def walk(off, end):
        nonlocal pal, img
        while off < end:
            bid, _, size, nxt, dat = struct.unpack_from("<HHIII", d, off)
            if bid in (2, 3):
                walk(off + dat, off + size)
            elif bid in (4, 5):
                b = off + 0x10
                hlen, _, fmt, order, w, h, bpp, pbw, pbh = struct.unpack_from("<HHHHHHHHH", d, b)
                poff = struct.unpack_from("<I", d, b + 0x1C)[0]
                pix = d[b + poff:]
                info = (fmt, order, w, h, pix)
                if bid == 4:
                    img = info
                else:
                    pal = info
            off += size if size else end
    walk(0x10, len(d))
    fmt, order, w, h, pix = img
    bits = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8}[fmt]
    stride_px = (w + (128 // bits) - 1) // (128 // bits) * (128 // bits)
    wb = stride_px * bits // 8
    hh = (h + 7) // 8 * 8 if order == 1 else h
    raw = pix[:wb * hh]
    if order == 1:
        raw = unswizzle(raw, wb, hh)

    def color(fmt_, v):
        if fmt_ == 3:
            return (v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24)
        if fmt_ == 2:
            return tuple(((v >> s) & 15) * 17 for s in (0, 4, 8, 12))
        if fmt_ == 1:
            return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31, 255 if v >> 15 else 0)
        return ((v & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, ((v >> 11) & 31) * 255 // 31, 255)

    palette = None
    if fmt in (4, 5):
        pf, _, pw, ph, pp = pal
        pbits = 32 if pf == 3 else 16
        palette = [color(pf, int.from_bytes(pp[i * pbits // 8:(i + 1) * pbits // 8], "little")) for i in range(pw * ph)]
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        row = raw[y * wb:(y + 1) * wb]
        for x in range(w):
            if fmt == 4:
                v = (row[x // 2] >> (4 * (x & 1))) & 15
                px[x, y] = palette[v]
            elif fmt == 5:
                px[x, y] = palette[row[x]]
            else:
                n = bits // 8
                px[x, y] = color(fmt, int.from_bytes(row[x * n:(x + 1) * n], "little"))
    print(path, "fmt", fmt, "order", order, w, h)
    return im


for p in sys.argv[1:]:
    im = decode(p)
    im.save(p.rsplit(".", 1)[0] + ".png")
