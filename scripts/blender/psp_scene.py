# M33 icon concepts, rendered in Blender (4.5 LTS, Cycles): a PSP-1000-style handheld in piano
# black, modelled here from primitives (no Sony assets or logos), M33's opening on its screen.
#
#   blender -b -P scripts/blender/psp_scene.py -- <out dir> <screen.png> <font.ttf> [samples] [size] [shots...]
#
# Scene units: the handheld is 1.70 x 0.74 x 0.23 (10x its size in metres), lying flat, front +Z.
import math
import os
import sys

import bmesh
import bpy
from mathutils import Euler, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
OUT, SCREEN, FONT = argv[0], argv[1], argv[2]
SAMPLES = int(argv[3]) if len(argv) > 3 else 160
SIZE = int(argv[4]) if len(argv) > 4 else 1024
SHOTS = argv[5:] or ["hero", "front", "float", "macro"]

W, H, T = 1.70, 0.74, 0.20      # body width, height, thickness
TOP = T                          # the front face is at z = T

# ---------- scene ----------

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.cycles.max_bounces = 10
scene.cycles.glossy_bounces = 6
scene.render.resolution_x = SIZE
scene.render.resolution_y = SIZE
scene.render.film_transparent = False
scene.view_settings.view_transform = "AgX"
scene.view_settings.look = "AgX - Medium High Contrast"
scene.render.image_settings.file_format = "PNG"

world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.004, 0.003, 0.008, 1)
bg.inputs["Strength"].default_value = 1.0


# ---------- materials ----------

def principled(name, color, rough, metallic=0.0, coat=0.0, coat_rough=0.03, spec=0.5):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*color, 1)
    p.inputs["Roughness"].default_value = rough
    p.inputs["Metallic"].default_value = metallic
    p.inputs["Coat Weight"].default_value = coat
    p.inputs["Coat Roughness"].default_value = coat_rough
    p.inputs["Specular IOR Level"].default_value = spec
    return m


def add_micro_texture(m, scale=180.0, rough_var=0.06, bump=0.015):
    """Real surfaces aren't perfect: roughness that varies a little, and a faint orange-peel bump."""
    nt = m.node_tree
    p = nt.nodes["Principled BSDF"]
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = scale
    noise.inputs["Detail"].default_value = 6
    ramp = nt.nodes.new("ShaderNodeMapRange")
    base = p.inputs["Roughness"].default_value
    ramp.inputs["To Min"].default_value = max(0.0, base - rough_var / 2)
    ramp.inputs["To Max"].default_value = base + rough_var / 2
    nt.links.new(noise.outputs["Fac"], ramp.inputs["Value"])
    nt.links.new(ramp.outputs["Result"], p.inputs["Roughness"])
    b = nt.nodes.new("ShaderNodeBump")
    b.inputs["Strength"].default_value = bump
    b.inputs["Distance"].default_value = 0.002
    nt.links.new(noise.outputs["Fac"], b.inputs["Height"])
    nt.links.new(b.outputs["Normal"], p.inputs["Normal"])
    nt.links.new(b.outputs["Normal"], p.inputs["Coat Normal"])


M_BLACK = principled("PianoBlack", (0.006, 0.006, 0.008), 0.12, coat=1.0, coat_rough=0.015)
add_micro_texture(M_BLACK, scale=260, rough_var=0.05, bump=0.01)
M_SIDE = principled("SideGunmetal", (0.10, 0.10, 0.115), 0.28, metallic=0.85)
add_micro_texture(M_SIDE, scale=900, rough_var=0.08, bump=0.02)
M_RECESS = principled("RecessSatin", (0.004, 0.004, 0.005), 0.35)
M_KEY = principled("KeyGloss", (0.012, 0.012, 0.015), 0.10, coat=1.0, coat_rough=0.02)
add_micro_texture(M_KEY, scale=400, rough_var=0.04, bump=0.008)
M_NUB = principled("NubRubber", (0.03, 0.03, 0.035), 0.55)
add_micro_texture(M_NUB, scale=1500, rough_var=0.15, bump=0.08)
M_SILVER = principled("SilverPrint", (0.55, 0.56, 0.60), 0.25, metallic=1.0)
M_GREY_PRINT = principled("GreyPrint", (0.30, 0.31, 0.34), 0.4)
M_HOLE = principled("Hole", (0.0, 0.0, 0.0), 0.9)
M_GLASS = principled("ScreenBezel", (0.0, 0.0, 0.0), 0.04, coat=1.0, coat_rough=0.0, spec=0.6)
M_CLEAR = principled("ClearGlass", (1.0, 1.0, 1.0), 0.0, spec=0.5)
M_CLEAR.node_tree.nodes["Principled BSDF"].inputs["Transmission Weight"].default_value = 1.0
M_CLEAR.node_tree.nodes["Principled BSDF"].inputs["IOR"].default_value = 1.5
M_FLOOR = principled("Floor", (0.004, 0.004, 0.006), 0.22, spec=0.35)
add_micro_texture(M_FLOOR, scale=40, rough_var=0.10, bump=0.0)
SYMBOL_COLORS = {"tri": (0.10, 0.75, 0.50), "cir": (0.95, 0.18, 0.22), "crs": (0.30, 0.48, 1.0), "sqr": (0.95, 0.35, 0.72)}
M_SYMBOL = {k: principled("Symbol_" + k, c, 0.3) for k, c in SYMBOL_COLORS.items()}


def lcd_material():
    m = bpy.data.materials.new("LCD")
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(SCREEN)
    em.inputs["Strength"].default_value = 2.2
    nt.links.new(tex.outputs["Color"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return m


def emitter(name, color, strength):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1)
    em.inputs["Strength"].default_value = strength
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return m


M_LCD = lcd_material()
M_LED = emitter("LED", (0.25, 1.0, 0.45), 6.0)


# ---------- geometry helpers ----------

ROOT = bpy.data.objects.new("Handheld", None)
scene.collection.objects.link(ROOT)


def link(obj):
    scene.collection.objects.link(obj)
    obj.parent = ROOT
    return obj


def rounded_rect_pts(w, h, r, n=24):
    pts = []
    for cx, cy, a0 in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180), (w / 2 - r, -h / 2 + r, 270)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
    return pts


def prism(name, pts, z0, z1, mat, side_mat=None, bevel=0.0, segments=4, smooth=True):
    """An extrusion of a 2D outline from z0 to z1, optionally bevelled."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bottom = [bm.verts.new((x, y, z0)) for x, y in pts]
    top = [bm.verts.new((x, y, z1)) for x, y in pts]
    bm.faces.new(list(reversed(bottom)))
    bm.faces.new(top)
    n = len(pts)
    for i in range(n):
        f = bm.faces.new((bottom[i], bottom[(i + 1) % n], top[(i + 1) % n], top[i]))
        f.material_index = 1 if side_mat else 0
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    me.materials.append(mat)
    if side_mat:
        me.materials.append(side_mat)
    link(obj)
    if bevel > 0:
        b = obj.modifiers.new("Bevel", "BEVEL")
        b.width = bevel
        b.segments = segments
        b.limit_method = "ANGLE"
        b.angle_limit = math.radians(40)
        b.harden_normals = True
        obj.data.shade_smooth() if hasattr(obj.data, "shade_smooth") else None
    if smooth:
        for poly in obj.data.polygons:
            poly.use_smooth = True
    return obj


def cylinder(name, x, y, r, z0, z1, mat, bevel=0.0, verts=96):
    pts = [(x + math.cos(2 * math.pi * i / verts) * r, y + math.sin(2 * math.pi * i / verts) * r) for i in range(verts)]
    return prism(name, pts, z0, z1, mat, bevel=bevel, segments=3)


def cut(target, cutter):
    m = target.modifiers.new("Cut_" + cutter.name, "BOOLEAN")
    m.operation = "DIFFERENCE"
    m.solver = "EXACT"
    m.object = cutter
    cutter.hide_render = True
    cutter.hide_viewport = True
    # booleans before the bevel: the recess rims get rounded too
    while target.modifiers.find(m.name) > 0:
        bpy.ops.object.modifier_move_up({"object": target}, modifier=m.name) if False else None
        break
    return m


def text(name, s, x, y, z, size, mat, extrude=0.0015, align="CENTER", spacing=1.0):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = s
    cu.font = bpy.data.fonts.load(FONT)
    cu.size = size
    cu.extrude = extrude
    cu.align_x = align
    cu.align_y = "CENTER"
    cu.space_character = spacing
    obj = bpy.data.objects.new(name, cu)
    obj.location = (x, y, z)
    obj.data.materials.append(mat)
    return link(obj)


# ---------- the handheld ----------

body_pts = rounded_rect_pts(W, H, 0.335, n=40)
body = prism("Body", body_pts, 0.0, T, M_BLACK, side_mat=M_BLACK, bevel=0.0, smooth=True)

# recesses in the face: the D-pad well, the face button well, the nub ring, the screen bed
DPAD = (-0.655, 0.085)
FACE = (0.655, 0.085)
NUB = (-0.66, -0.195)
recess_cutters = []
for nm, (cx, cy), r, depth in (("DpadWell", DPAD, 0.152, 0.014), ("FaceWell", FACE, 0.158, 0.014), ("NubWell", NUB, 0.088, 0.012)):
    c = cylinder(nm + "Cutter", cx, cy, r, TOP - depth, TOP + 0.1, M_RECESS)
    recess_cutters.append((nm, c, cx, cy, r, depth))
for nm, c, *_ in recess_cutters:
    mod = body.modifiers.new("Cut_" + nm, "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = c
    c.hide_render = True
    c.hide_viewport = True
bev = body.modifiers.new("Bevel", "BEVEL")
bev.width = 0.045
bev.segments = 10
bev.limit_method = "ANGLE"
bev.angle_limit = math.radians(40)
bev.use_clamp_overlap = True
bev.harden_normals = True
bev.profile = 0.55
wn = body.modifiers.new("WeightedNormal", "WEIGHTED_NORMAL")
wn.keep_sharp = True
for poly in body.data.polygons:
    poly.use_smooth = True
# the recess floors: satin, so the wells read darker than the lacquer
for nm, c, cx, cy, r, depth in recess_cutters:
    cylinder(nm + "Floor", cx, cy, r - 0.002, TOP - depth - 0.001, TOP - depth + 0.0005, M_RECESS)

# the silver seam: a thin bright ring around the side, halfway down
seam = prism("Seam", rounded_rect_pts(W + 0.003, H + 0.003, 0.3365, n=40), T * 0.40, T * 0.43, M_SILVER)

# the screen: glass over a lit LCD
GLASS = (0.0, 0.045, 1.03, 0.575)
glass = prism("Bezel", rounded_rect_pts(GLASS[2], GLASS[3], 0.035), TOP - 0.004, TOP + 0.004, M_GLASS, bevel=0.003, segments=3)
glass.location = (GLASS[0], GLASS[1], 0)
lw, lh = 0.905, 0.905 * 9 / 16
bpy.ops.mesh.primitive_plane_add(size=1)
lcd = bpy.context.active_object
lcd.name = "LCD"
lcd.scale = (lw, lh, 1)
lcd.location = (0.0, 0.045, TOP + 0.0045)
lcd.data.materials.append(M_LCD)
lcd.parent = ROOT
lcd.location.z = TOP + 0.0042
# the cover glass over bezel and LCD: a clear sheet, so the reflections sit on top of the picture
bpy.ops.mesh.primitive_plane_add(size=1)
cover = bpy.context.active_object
cover.name = "CoverGlass"
cover.scale = (GLASS[2] - 0.01, GLASS[3] - 0.01, 1)
cover.location = (GLASS[0], GLASS[1], TOP + 0.0055)
cover.data.materials.append(M_CLEAR)
cover.parent = ROOT

# D-pad: four separate keys pointing out from the centre
for i, ang in enumerate((90, 0, 270, 180)):
    a = math.radians(ang)
    pts = []
    for (px, py) in ((0.105, 0.0), (0.032, 0.052), (0.032, -0.052)):
        pts.append((DPAD[0] + px * math.cos(a) - py * math.sin(a), DPAD[1] + px * math.sin(a) + py * math.cos(a)))
    # round the wedge's corners by sampling a fillet
    k = prism("DpadKey%d" % i, pts, TOP - 0.014, TOP + 0.010, M_KEY, bevel=0.012, segments=6)
    # the arrow printed on the key
    tip = [(DPAD[0] + (0.090) * math.cos(a), DPAD[1] + 0.090 * math.sin(a))]
    ax = DPAD[0] + 0.072 * math.cos(a)
    ay = DPAD[1] + 0.072 * math.sin(a)
    arr = [(ax + 0.016 * math.cos(a), ay + 0.016 * math.sin(a)),
           (ax - 0.010 * math.cos(a) - 0.014 * math.sin(a), ay - 0.010 * math.sin(a) + 0.014 * math.cos(a)),
           (ax - 0.010 * math.cos(a) + 0.014 * math.sin(a), ay - 0.010 * math.sin(a) - 0.014 * math.cos(a))]
    prism("DpadArrow%d" % i, arr, TOP + 0.0095, TOP + 0.0105, M_GREY_PRINT)

# the analog nub: a slightly domed rubber disc
nub = cylinder("Nub", NUB[0], NUB[1], 0.066, TOP - 0.012, TOP + 0.008, M_NUB, bevel=0.010)

# face buttons with their symbols
for kind, (ox, oy) in (("tri", (0, 1)), ("cir", (1, 0)), ("crs", (0, -1)), ("sqr", (-1, 0))):
    bx, by = FACE[0] + ox * 0.088, FACE[1] + oy * 0.088
    cylinder("Button_" + kind, bx, by, 0.046, TOP - 0.014, TOP + 0.012, M_KEY, bevel=0.010)
    z0, z1, s, w = TOP + 0.0115, TOP + 0.0125, 0.020, 0.0035
    mat = M_SYMBOL[kind]
    if kind == "cir":
        ring = [(bx + math.cos(2 * math.pi * i / 64) * s, by + math.sin(2 * math.pi * i / 64) * s) for i in range(64)]
        o = prism("Sym_" + kind, ring, z0, z1, mat)
        inner = cylinder("SymHole_" + kind, bx, by, s - w, z0 - 0.01, z1 + 0.01, mat)
        bm_ = o.modifiers.new("hole", "BOOLEAN")
        bm_.operation = "DIFFERENCE"
        bm_.object = inner
        inner.hide_render = True
    elif kind == "sqr":
        outer = [(bx - s * 0.85, by - s * 0.85), (bx + s * 0.85, by - s * 0.85), (bx + s * 0.85, by + s * 0.85), (bx - s * 0.85, by + s * 0.85)]
        o = prism("Sym_" + kind, outer, z0, z1, mat)
        hole = prism("SymHole_" + kind, [(bx - s * 0.85 + w, by - s * 0.85 + w), (bx + s * 0.85 - w, by - s * 0.85 + w), (bx + s * 0.85 - w, by + s * 0.85 - w), (bx - s * 0.85 + w, by + s * 0.85 - w)], z0 - 0.01, z1 + 0.01, mat)
        bm_ = o.modifiers.new("hole", "BOOLEAN")
        bm_.operation = "DIFFERENCE"
        bm_.object = hole
        hole.hide_render = True
    elif kind == "tri":
        tri = [(bx, by + s * 1.0), (bx - s * 0.95, by - s * 0.65), (bx + s * 0.95, by - s * 0.65)]
        o = prism("Sym_" + kind, tri, z0, z1, mat)
        tri_in = [(bx, by + s * 1.0 - w * 2.0), (bx - s * 0.95 + w * 1.7, by - s * 0.65 + w), (bx + s * 0.95 - w * 1.7, by - s * 0.65 + w)]
        hole = prism("SymHole_" + kind, tri_in, z0 - 0.01, z1 + 0.01, mat)
        bm_ = o.modifiers.new("hole", "BOOLEAN")
        bm_.operation = "DIFFERENCE"
        bm_.object = hole
        hole.hide_render = True
    else:
        for sgn in (1, -1):
            a = math.radians(45 * sgn)
            L = s * 1.05
            pts = [(-L, -w / 2), (L, -w / 2), (L, w / 2), (-L, w / 2)]
            pts = [(bx + x * math.cos(a) - y * math.sin(a), by + x * math.sin(a) + y * math.cos(a)) for x, y in pts]
            prism("Sym_crs%d" % sgn, pts, z0, z1, mat)

# the row under the screen: HOME, volume, display, sound, SELECT, START
ROW_Y = -0.292
for nm, x, w, label in (("Home", -0.40, 0.075, "HOME"), ("VolDown", -0.235, 0.048, ""), ("VolUp", -0.17, 0.048, ""),
                        ("Display", -0.04, 0.048, ""), ("Sound", 0.025, 0.048, ""), ("Select", 0.215, 0.062, "SELECT"),
                        ("Start", 0.335, 0.062, "START")):
    prism(nm, rounded_rect_pts(w, 0.024, 0.0119, n=12), TOP - 0.004, TOP + 0.007, M_KEY, bevel=0.004, segments=4).location = (x, ROW_Y, 0)
    if label:
        text("Label" + nm, label, x, ROW_Y - 0.034, TOP + 0.0002, 0.017, M_GREY_PRINT, extrude=0.0002, spacing=1.25)

# shoulder buttons on the top edge
for sx in (-1, 1):
    sh = prism("Shoulder%d" % sx, rounded_rect_pts(0.40, 0.10, 0.045), 0.04, T - 0.05, M_KEY, bevel=0.02, segments=6)
    sh.location = (sx * 0.52, H / 2 - 0.045, 0)

# speaker holes and status lights
for sx in (-1, 1):
    for i in range(5):
        for j in range(3):
            x = sx * 0.80 + (i - 2) * 0.014 + (j % 2) * 0.007
            y = -0.16 + (j - 1) * 0.014
            cylinder("Speaker", x, y, 0.0042, TOP - 0.03, TOP + 0.0003, M_HOLE, verts=16)
for i, mat in enumerate((M_LED, M_RECESS)):
    cylinder("LED%d" % i, 0.52 + i * 0.035, 0.30, 0.007, TOP - 0.002, TOP + 0.0015, mat, verts=24)

# M33 printed in silver above the D-pad
text("PrintM33", "M33", DPAD[0], 0.30, TOP + 0.0002, 0.055, M_SILVER, extrude=0.0003, spacing=1.15)


# ---------- the studio ----------

bpy.ops.mesh.primitive_plane_add(size=60, location=(0, 0, -0.0005))
floor = bpy.context.active_object
floor.name = "Floor"
floor.data.materials.append(M_FLOOR)


def area_light(name, loc, rot, size, energy, color=(1, 1, 1), size_y=None):
    ld = bpy.data.lights.new(name, "AREA")
    ld.energy = energy
    ld.color = color
    ld.shape = "RECTANGLE" if size_y else "SQUARE"
    ld.size = size
    if size_y:
        ld.size_y = size_y
    o = bpy.data.objects.new(name, ld)
    o.location = loc
    o.rotation_euler = rot
    scene.collection.objects.link(o)
    return o


def strip(name, loc, rot, w, h, strength):
    """A softbox only reflections see: long highlights on the lacquer."""
    bpy.ops.mesh.primitive_plane_add(size=1, location=loc, rotation=rot)
    o = bpy.context.active_object
    o.name = name
    o.scale = (w, h, 1)
    o.data.materials.append(emitter(name + "Mat", (1, 1, 1), strength))
    o.visible_camera = False
    o.visible_shadow = False
    return o


def look_at(obj, target):
    d = Vector(target) - obj.location
    obj.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


cam_data = bpy.data.cameras.new("Camera")
cam = bpy.data.objects.new("Camera", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam

key = area_light("Key", (0.0, -1.6, 5.0), (0, 0, 0), 3.0, 320)
fill = area_light("Fill", (-4.0, -3.0, 2.0), (0, 0, 0), 2.5, 70)
rim_v = area_light("RimViolet", (-3.0, 2.6, 2.4), (0, 0, 0), 1.2, 520, color=(0.62, 0.36, 1.0))
rim_c = area_light("RimCyan", (3.4, 4.2, 3.0), (0, 0, 0), 2.4, 260, color=(0.25, 0.86, 1.0))
for l in (key, fill, rim_v, rim_c):
    look_at(l, (0, 0, 0.1))
strip_a = strip("StripA", (-1.2, -0.6, 3.6), (math.radians(-12), math.radians(-18), 0), 4.0, 0.22, 4.5)
strip_b = strip("StripB", (1.4, 0.4, 3.4), (math.radians(14), math.radians(20), math.radians(8)), 4.0, 0.14, 5.0)


def shot(name, root_loc, root_rot, cam_loc, target, lens, fstop=None, focus=None, cutout=False, res=None, shift=(0, 0)):
    # cutout: no floor or backdrop, a transparent background (the handheld alone, for compositing)
    scene.render.film_transparent = cutout
    floor.hide_render = cutout
    scene.render.resolution_x, scene.render.resolution_y = res if res else (SIZE, SIZE)
    cam_data.shift_x, cam_data.shift_y = shift
    ROOT.location = root_loc
    ROOT.rotation_euler = Euler([math.radians(v) for v in root_rot])
    cam.location = cam_loc
    look_at(cam, target)
    cam_data.lens = lens
    cam_data.dof.use_dof = fstop is not None
    if fstop:
        cam_data.dof.aperture_fstop = fstop
        cam_data.dof.focus_distance = focus if focus else (Vector(cam_loc) - Vector(target)).length
    scene.render.filepath = os.path.join(OUT, name + ".png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath, flush=True)


SHOT_DEFS = {
    # lying on the glossy floor, seen low from the front left, the screen in focus
    "hero": dict(root_loc=(0, 0, 0), root_rot=(0, 0, -12), cam_loc=(-0.95, -2.35, 1.75), target=(0.05, 0.02, 0.12), lens=50, fstop=4.0),
    # straight down, like a flat lay, rim light raking across
    # standing upright, facing the camera square on, over its reflection
    "front": dict(root_loc=(0, 0, 0.385), root_rot=(84, 0, 0), cam_loc=(0.0, -3.6, 0.75), target=(0.0, 0.0, 0.40), lens=55, fstop=8.0),
    # standing up and turned, floating over its reflection
    "float": dict(root_loc=(0, 0, 0.62), root_rot=(72, 0, 16), cam_loc=(-0.7, -3.4, 1.0), target=(0.0, 0.0, 0.60), lens=60, fstop=5.6),
    # close on the right side: face buttons and the screen's edge, shallow focus
    "macro": dict(root_loc=(0, 0, 0), root_rot=(0, 0, -24), cam_loc=(1.35, -1.65, 0.95), target=(0.45, 0.06, 0.15), lens=85, fstop=2.0),
    # the chosen icon: the macro shot alone on transparency (square, and 16:9 for the home screen)
    "macro_cut": dict(root_loc=(0, 0, 0), root_rot=(0, 0, -24), cam_loc=(1.35, -1.65, 0.95), target=(0.45, 0.06, 0.15), lens=85, fstop=2.0, cutout=True),
    "macro_wide": dict(root_loc=(0, 0, 0), root_rot=(0, 0, -24), cam_loc=(1.55, -1.95, 1.05), target=(0.45, 0.06, 0.15), lens=85, fstop=2.0, cutout=True, res=(1920, 1080), shift=(0.17, 0.0)),
}

os.makedirs(OUT, exist_ok=True)
for s in SHOTS:
    if s in SHOT_DEFS:  # (startup_anim.py builds the scene with "none")
        shot(s, **SHOT_DEFS[s])
