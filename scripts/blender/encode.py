# The startup animation's MP4: the rendered frames (startup_anim.py), the video's sound starting
# when its picture does on the handheld's screen, a fade in from black and out to black.
#
#   blender -b -P scripts/blender/encode.py -- <frames dir> <video.mp4 (its sound)> <out.mp4> [video start s]
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
FRAMES, SOUND, OUT = args[0], args[1], args[2]
VIDEO_START = float(args[3]) if len(args) > 3 else 3.0
FPS = 60

files = sorted(f for f in os.listdir(FRAMES) if f.startswith("f_") and f.endswith(".png"))
sc = bpy.context.scene
sc.render.fps = FPS
first = bpy.data.images.load(os.path.join(FRAMES, files[0]))
sc.render.resolution_x, sc.render.resolution_y = first.size
sc.render.resolution_percentage = 100
sc.sequence_editor_create()
seq = sc.sequence_editor.strips
pics = seq.new_image("frames", os.path.join(FRAMES, files[0]), channel=1, frame_start=1)
for f in files[1:]:
    pics.elements.append(f)
n = len(files)
sc.frame_start, sc.frame_end = 1, n

snd = seq.new_sound("chime", SOUND, channel=2, frame_start=int(VIDEO_START * FPS) + 1)
snd.volume = 1.0

# fades: 0.4 s in from black, 0.7 s out
pics.blend_alpha = 0.0
pics.keyframe_insert("blend_alpha", frame=1)
pics.blend_alpha = 1.0
pics.keyframe_insert("blend_alpha", frame=int(0.4 * FPS))
pics.keyframe_insert("blend_alpha", frame=n - int(0.7 * FPS))
pics.blend_alpha = 0.0
pics.keyframe_insert("blend_alpha", frame=n)

r = sc.render
r.image_settings.file_format = "FFMPEG"
r.ffmpeg.format = "MPEG4"
r.ffmpeg.codec = "H264"
r.ffmpeg.constant_rate_factor = "HIGH"
r.ffmpeg.ffmpeg_preset = "GOOD"
r.ffmpeg.gopsize = FPS
r.ffmpeg.audio_codec = "AAC"
r.ffmpeg.audio_bitrate = 192
r.ffmpeg.audio_channels = "STEREO"
r.ffmpeg.audio_mixrate = 48000
r.filepath = OUT
bpy.ops.render.render(animation=True)
print("ENCODED", OUT, n, "frames", flush=True)
