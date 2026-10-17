# M33 startup animation: the handheld from psp_scene.py spins in out of the dark, powers on, and
# plays a video on its screen. Renders a PNG sequence (encode.py adds the video's sound and makes
# the MP4).
#
#   blender -b -P scripts/blender/startup_anim.py -- <frames dir> <video.mp4> <font.ttf> <samples> <width> <height> [first last]
#
# Timeline at 60 fps (the video is 30 fps: each of its frames shows for two):
#   0.0 - 2.6 s  a turn and a quarter around the vertical, easing out, lights sweeping the lacquer
#   2.6 s        the power light; 2.85 s the backlight
#   3.0 s        the video starts (its sound too, in encode.py), the camera pushes in slowly
#   video end    + 0.6 s hold; the fade to black is added when encoding
import math
import os
import sys

import bpy
from mathutils import Euler, Vector

args = sys.argv[sys.argv.index("--") + 1:]
FRAMES, VIDEO, FONT = args[0], args[1], args[2]
SAMPLES, WIDTH, HEIGHT = int(args[3]), int(args[4]), int(args[5])
FIRST = int(args[6]) if len(args) > 6 else None
LAST = int(args[7]) if len(args) > 7 else None

FPS = 60
SPIN_END = 2.6
LED_ON = 2.6
BACKLIGHT = 2.85
VIDEO_START = 3.0

# Build the handheld and the studio (psp_scene.py, rendering nothing)
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], "--", FRAMES, os.path.join(here, "..", "..", "brand", "intro-still.png"), FONT, str(SAMPLES), str(WIDTH), "none"]
exec(compile(open(os.path.join(here, "psp_scene.py")).read(), "psp_scene.py", "exec"))

scene = bpy.context.scene
scene.render.fps = FPS
scene.render.resolution_x = WIDTH
scene.render.resolution_y = HEIGHT
scene.render.use_motion_blur = True
scene.render.motion_blur_shutter = 0.5
scene.cycles.seed = 33
scene.cycles.samples = SAMPLES

# --- the video on the screen, at half the scene's rate, dark until it starts ---
clip = bpy.data.movieclips.load(VIDEO)
video_frames = clip.frame_duration
video_fps = clip.fps
step = FPS / video_fps
start_f = int(VIDEO_START * FPS) + 1
end_f = start_f + int(video_frames * step) + int(0.6 * FPS)
scene.frame_start = 1
scene.frame_end = end_f

lcd_mat = bpy.data.materials["LCD"]
nt = lcd_mat.node_tree
tex = next(n for n in nt.nodes if n.type == "TEX_IMAGE")
emission = next(n for n in nt.nodes if n.type == "EMISSION")
# The video's frames, extracted beforehand (extract_frames.py) next to the output: the screen's
# texture is swapped to the right one before every scene frame, each held for `step` frames (a
# movie texture didn't follow the frame offset reliably).
VFRAMES = os.path.join(os.path.dirname(FRAMES.rstrip("/")), "video_frames")
video_imgs = [bpy.data.images.load(os.path.join(VFRAMES, "v_%04d.png" % (i + 1))) for i in range(video_frames)]
tex.image = video_imgs[0]


def show_video_frame(sc, *_):
    f = sc.frame_current
    i = min(video_frames - 1, max(0, int((f - start_f) / step)))
    tex.image = video_imgs[i]


bpy.app.handlers.frame_change_pre.append(show_video_frame)

# the backlight: a faint blue glow first, then the picture at full strength
backlight = nt.nodes.new("ShaderNodeEmission")
backlight.inputs["Color"].default_value = (0.08, 0.12, 0.30, 1)
mix = nt.nodes.new("ShaderNodeMixShader")
out = next(n for n in nt.nodes if n.type == "OUTPUT_MATERIAL")
nt.links.new(backlight.outputs["Emission"], mix.inputs[1])
nt.links.new(emission.outputs["Emission"], mix.inputs[2])
nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])


def key(socket, frame, value, interp="BEZIER"):
    socket.default_value = value
    socket.keyframe_insert("default_value", frame=frame)


f_back = int(BACKLIGHT * FPS) + 1
key(backlight.inputs["Strength"], 1, 0.0)
key(backlight.inputs["Strength"], f_back - 1, 0.0)
key(backlight.inputs["Strength"], f_back + 6, 1.2)
key(mix.inputs["Fac"], 1, 0.0)
key(mix.inputs["Fac"], start_f - 1, 0.0)
key(mix.inputs["Fac"], start_f + 6, 1.0)
key(emission.inputs["Strength"], 1, 2.4)

# the power light
led = bpy.data.materials["LED"].node_tree.nodes
led_em = next(n for n in led if n.type == "EMISSION")
f_led = int(LED_ON * FPS) + 1
key(led_em.inputs["Strength"], 1, 0.0)
key(led_em.inputs["Strength"], f_led - 1, 0.0)
key(led_em.inputs["Strength"], f_led + 4, 8.0)

# --- the handheld: standing, a turn and a quarter around the vertical, easing out ---
ROOT = bpy.data.objects["Handheld"]
REST = (72.0, 0.0, 16.0)      # the Float pose
spin_frames = int(SPIN_END * FPS)
for f in range(1, spin_frames + 2):
    t = min(1.0, (f - 1) / spin_frames)
    e = 1 - (1 - t) ** 3                        # ease out
    z = REST[2] - 450.0 * (1 - e)               # 1.25 turns
    tilt = REST[0] - 10.0 * (1 - e) ** 2        # leans back a little while it spins
    lift = 0.62 + 0.10 * (1 - e) ** 2           # and settles down onto its pose
    ROOT.rotation_euler = Euler([math.radians(tilt), 0, math.radians(z)])
    ROOT.location = (0, 0, lift)
    ROOT.keyframe_insert("rotation_euler", frame=f)
    ROOT.keyframe_insert("location", frame=f)
# a slow breath after it lands
for f in range(spin_frames + 2, end_f + 1, 6):
    s = (f - spin_frames) / FPS
    ROOT.rotation_euler = Euler([math.radians(REST[0]), math.radians(0.6 * math.sin(s * 0.8)), math.radians(REST[2] + 1.2 * math.sin(s * 0.5))])
    ROOT.location = (0, 0, 0.62 + 0.004 * math.sin(s * 1.1))
    ROOT.keyframe_insert("rotation_euler", frame=f)
    ROOT.keyframe_insert("location", frame=f)

# --- the camera: the Float view, pushing in towards the screen once the video plays ---
cam = scene.camera
cam_data = cam.data
cam_data.lens = 60
cam_data.dof.use_dof = True
cam_data.dof.aperture_fstop = 5.6
target = Vector((0.0, 0.0, 0.60))
start_pos = Vector((-0.95, -4.5, 1.2))
end_pos = Vector((-0.62, -3.35, 0.98))
for f in range(1, end_f + 1, 3):
    t = 0.0 if f < start_f else min(1.0, (f - start_f) / (end_f - start_f))
    e = t * t * (3 - 2 * t)
    cam.location = start_pos.lerp(end_pos, e)
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam_data.dof.focus_distance = (cam.location - target).length
    cam.keyframe_insert("location", frame=f)
    cam.keyframe_insert("rotation_euler", frame=f)
    cam_data.dof.keyframe_insert("focus_distance", frame=f)

# --- render the frames ---
os.makedirs(FRAMES, exist_ok=True)
scene.render.image_settings.file_format = "PNG"
scene.render.filepath = os.path.join(FRAMES, "f_")
scene.frame_start = FIRST or 1
scene.frame_end = LAST or end_f
print("ANIM frames", scene.frame_start, "-", scene.frame_end, "of", end_f, "video starts at", start_f, flush=True)
bpy.ops.render.render(animation=True)
print("ANIM done", flush=True)
