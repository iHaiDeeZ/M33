import os
import struct
import subprocess
import sys
import zlib

LZRC = os.path.expanduser("~/ext/lzrc/lzrc")


def lzrc(path, off, plen, ulen):
    out = "/tmp/lzrc.out"
    r = subprocess.run([LZRC, path, str(off), str(plen), str(ulen), out], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return open(out, "rb").read()


def parse(path, dump_dir=None):
    d = open(path, "rb").read()
    hdr = struct.unpack_from("<4I", d, 0)
    comp = hdr[3] >> 4
    names = ["pMain", "pVSMX", "pText", "pSound", "pModel", "pImg", "pUnk", "pFont", "pObj", "pAnim",
             "pTextData", "lTextData", "pLabelData", "lLabelData", "pEventData", "lEventData",
             "pTextPtrs", "lTextPtrs", "pImgPtrs", "lImgPtrs", "pModelPtrs", "lModelPtrs", "pSoundPtrs",
             "lSoundPtrs", "pObjPtrs", "lObjPtrs", "pAnimPtrs", "lAnimPtrs", "pImgData", "lImgData",
             "pSoundData", "lSoundData", "pModelData", "lModelData"]
    vals = struct.unpack_from("<%dI" % len(names), d, 0x10)
    h = dict(zip(names, vals))
    if comp:
        plen, ulen, _ = struct.unpack_from("<3I", d, 0xA4)
        tree = lzrc(path, 0xB0, plen, ulen)
        print(path, "tree", plen, "->", None if tree is None else len(tree))
        if tree is None:
            return
        base = 0xA4
    else:
        tree = d
        base = 0

    def T(off):
        return off - base

    def label(lo):
        p = h["pLabelData"]
        if p == 0xFFFFFFFF:
            return "?"
        i = T(p) + lo
        if 0 <= i < len(tree):
            return tree[i:tree.index(b"\0", i)].decode("latin-1")
        return "?"

    if h["pImg"] == 0xFFFFFFFF:
        print("  no images")
        return
    t = T(h["pImg"])
    typeId, _, lab, ehs, esz, nsub, nxt, prv, par = struct.unpack_from("<HHIIIIIII", tree, t)
    print("  image table: %d images, img data at 0x%x (0x%x)" % (nsub, h["pImgData"], h["lImgData"]))
    e = t + 0x28
    for i in range(nsub):
        typeId, _, lab, ehs, esz, ns, nxt, prv, par = struct.unpack_from("<HHIIIIIII", tree, e)
        fmt, cmp_, sp, off = struct.unpack_from("<HHII", tree, e + 0x28)
        su = struct.unpack_from("<I", tree, e + 0x34)[0] if cmp_ else sp
        name = label(lab)
        print("   %-40s fmt %d comp %d %6d -> %6d at 0x%x" % (name, fmt, cmp_, sp, su, off))
        if dump_dir:
            raw = d[h["pImgData"] + off: h["pImgData"] + off + sp]
            if cmp_ == 2:
                tmp = "/tmp/img.bin"
                open(tmp, "wb").write(raw)
                raw = lzrc(tmp, 0, sp, su)
            elif cmp_ == 1:
                raw = zlib.decompress(raw)
            if raw:
                ext = ["png", "jpg", "tif", "gif", "bmp", "gim"][fmt] if fmt < 6 else "bin"
                open(os.path.join(dump_dir, "%s.%s" % (name, ext)), "wb").write(raw)
        e += nxt if nxt else 0x28 + 0x10


for p in sys.argv[2:]:
    parse(p, sys.argv[1] if sys.argv[1] != "-" else None)
