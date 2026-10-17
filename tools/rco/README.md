# PSP firmware RCO tools

For looking inside the installed firmware's `flash0:/vsh/resource/*.rco` (how the game boot was worked out).
They use a host build of PPSSPP's own LZRC decoder (`Core/FileSystems/tlzrc.cpp`) as `~/ext/lzrc/lzrc`:

    g++ -O2 -I<ppsspp> -o ~/ext/lzrc/lzrc lzrc_main.cpp <ppsspp>/Core/FileSystems/tlzrc.cpp

(`lzrc_main.cpp`: read `<file> <offset> <packed> <unpacked> <out>`, call `lzrc_decompress`, write the result.)

- `rco_list.py <dump dir|-> <rco>...`: the images, by label (dumped when a directory is given)
- `rco_anim.py <rco> [name filter]...`: the object tree (pages, planes) and the animations' steps
- `gim2png.py <gim>...`: GIM images to PNG

Layouts after RCOMage (rcofile.h, data/*.ini). The RCOs' "RLZ" is LZRC; modes 6 and 7 take the output
position into the literal context, which M33's patched tlzrc.cpp now does too.
