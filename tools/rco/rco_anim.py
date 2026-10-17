import struct
import subprocess
import sys

path = sys.argv[1]
d = open(path, "rb").read()
plen, ulen, _ = struct.unpack_from("<3I", d, 0xA4)
subprocess.run([__import__("os").path.expanduser("~/ext/lzrc/lzrc"), path, "0xB0", str(plen), str(ulen), "/tmp/t.tree"], check=True, capture_output=True)
t = open("/tmp/t.tree", "rb").read()
B = 0xA4
names = ["pMain", "pVSMX", "pText", "pSound", "pModel", "pImg", "pUnk", "pFont", "pObj", "pAnim", "pTextData",
         "lTextData", "pLabelData", "lLabelData", "pEventData", "lEventData"]
h = dict(zip(names, struct.unpack_from("<16I", d, 0x10)))


def label(lo):
    i = h["pLabelData"] - B + lo
    return t[i:t.index(b"\0", i)].decode("latin-1")


def ev(lo):
    i = h["pEventData"] - B + lo
    return t[i:t.index(b"\0", i)].decode("latin-1")


def entry_label(off):
    return label(struct.unpack_from("<I", t, off - B + 4)[0])


def ref(rtype, val):
    if rtype == 0xFFFF:
        return "-"
    if rtype == 0x400:
        return "event:" + ev(val)
    if rtype in (0x402, 0x403, 0x407, 0x408, 0x409):
        try:
            return entry_label(val)
        except Exception:
            return "?%x" % val
    if rtype == 0x401:
        return "text#%d" % val
    return "%x:%x" % (rtype, val)


ANIM = {2: ("MoveTo", "rfiFFF"), 3: ("Recolour", "rfiFFFF"), 4: ("Rotate", "rfiFFF"), 5: ("Resize", "rfiFFF"),
        6: ("Fade", "rfiF"), 7: ("Delay", "f"), 8: ("FireEvent", "r"), 9: ("Lock", "i"), 10: ("Unlock", "i"),
        11: ("SlideOut", "rfiFFFF")}
PLANE = "fffffffffffffiree i"


def attrs(off, spec):
    out = []
    o = off
    for c in spec:
        if c == " ":
            continue
        if c in "fF":
            out.append(round(struct.unpack_from("<f", t, o)[0], 3)); o += 4
        elif c == "i":
            out.append(struct.unpack_from("<i", t, o)[0]); o += 4
        else:  # ref / event / image
            rt, rv = struct.unpack_from("<II", t, o)
            out.append(ref(rt, rv)); o += 8
    return out


def walk(off, depth, kind, want=None):
    i = off - B
    typeId, _, lo, ehs, esz, nsub, nxt, prv, par = struct.unpack_from("<HHIIIIIII", t, i)
    typ, idx = typeId >> 8, typeId & 0xFF
    name = label(lo) if lo != 0xFFFFFFFF else ""
    show = want is None or depth == 0 or any(w in name for w in want) or depth > 1
    line = "  " * depth + "%s id%d %s" % (["?", "main", "vsmx", "text", "img", "model", "sound", "font", "obj", "anim"][typ] if typ < 10 else typ, idx, name)
    if kind == "anim" and idx in ANIM:
        nm, spec = ANIM[idx]
        line = "  " * depth + "%-10s %s" % (nm, attrs(i + 0x28, spec))
    if kind == "obj" and idx == 2:
        a = attrs(i + 0x28, PLANE)
        line += "  pos(%s,%s,%s) rgba(%s,%s,%s,%s) size(%s,%s) scale(%s,%s) image=%s" % (a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[10], a[11], a[15])
    if show:
        print(line)
    c = off + (esz if esz else 0x28)
    for _ in range(nsub):
        ci = c - B
        cnext = struct.unpack_from("<I", t, ci + 20)[0]
        walk(c, depth + 1, kind, want if depth == 0 else None)
        c += cnext
        if not cnext:
            break


want = sys.argv[2:] or None
print("== objects")
walk(h["pObj"], 0, "obj", want)
print("== anims")
walk(h["pAnim"], 0, "anim", want)
