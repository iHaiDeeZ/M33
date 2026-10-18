# The placeholder screen's frames and chime (placeholder_screen.py) as one 30 fps MP4, the form
# startup_anim.py (its frame count and rate) and encode.py (its sound) take a screen video in.
#
#   blender -b -P scripts/blender/placeholder_video.py -- <dir with video_frames/ and chime.wav>
import os
import sys

import bpy

D = sys.argv[sys.argv.index("--") + 1]
FRAMES = os.path.join(D, "video_frames")
files = sorted(f for f in os.listdir(FRAMES) if f.endswith(".png"))
sc = bpy.context.scene
sc.render.fps = 30
sc.render.resolution_x, sc.render.resolution_y = 1280, 720
sc.render.resolution_percentage = 100
sc.sequence_editor_create()
seq = sc.sequence_editor.strips
pics = seq.new_image("frames", os.path.join(FRAMES, files[0]), channel=1, frame_start=1)
for f in files[1:]:
    pics.elements.append(f)
seq.new_sound("chime", os.path.join(D, "chime.wav"), channel=2, frame_start=1)
sc.frame_start, sc.frame_end = 1, len(files)
r = sc.render
r.image_settings.file_format = "FFMPEG"
r.ffmpeg.format = "MPEG4"
r.ffmpeg.codec = "H264"
r.ffmpeg.constant_rate_factor = "HIGH"
r.ffmpeg.audio_codec = "AAC"
r.ffmpeg.audio_bitrate = 192
r.ffmpeg.audio_channels = "STEREO"
r.ffmpeg.audio_mixrate = 48000
r.filepath = os.path.join(D, "placeholder.mp4")
bpy.ops.render.render(animation=True)
print("PLACEHOLDER", r.filepath, len(files), "frames", flush=True)
