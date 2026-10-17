# Writes every frame of a video as PNG (<out>/v_0001.png ...) with Blender's video editor.
#   blender -b -P scripts/blender/extract_frames.py -- <video> <out dir>
import sys

import bpy

src, out = sys.argv[sys.argv.index("--") + 1:][:2]
sc = bpy.context.scene
sc.sequence_editor_create()
strip = sc.sequence_editor.strips.new_movie("v", src, channel=1, frame_start=1)
w, h = strip.elements[0].orig_width, strip.elements[0].orig_height
sc.render.resolution_x, sc.render.resolution_y = w, h
sc.render.resolution_percentage = 100
sc.render.fps = int(round(strip.fps)) if hasattr(strip, "fps") else 30
sc.frame_start, sc.frame_end = 1, strip.frame_final_end - 1
sc.render.image_settings.file_format = "PNG"
sc.render.filepath = out + "/v_"
bpy.ops.render.render(animation=True)
