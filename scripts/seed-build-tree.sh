#!/usr/bin/env bash
# One-time: create ~/m33-build from copies of Resonance's build tree (libc++ 21 sysroot with the
# libc overlay and prebuilt libraries, and the RADV/GNM Mesa build) plus rpcs3's FFmpeg 7.1.1 (the
# decoders PSP games use: H.264 for PSMF movies, ATRAC3/3+, MP3, AAC), so this port never writes
# into ~/resonance-build or ~/rpcs3-build.
set -euo pipefail
RES="${RES:-$HOME/resonance-build}"
RPC="${RPC:-$HOME/rpcs3-build}"
PSB="${PSB:-$HOME/m33-build}"
mkdir -p "$PSB/src" "$PSB/build" "$PSB/out" "$PSB/logs" "$PSB/oelf"
[ -d "$PSB/sysroot" ] || cp -a "$RES/sysroot" "$PSB/sysroot"
[ -d "$PSB/host-compat" ] || cp -a "$RES/host-compat" "$PSB/host-compat"
# Mesa: the whole build directory (its static libraries are thin archives)
[ -d "$PSB/build/mesa" ] || cp -a "$RES/build/mesa" "$PSB/build/mesa"
# FFmpeg from rpcs3's sysroot (scripts/build-deps.sh ffmpeg there)
if [ ! -f "$PSB/sysroot/lib/libavcodec.a" ]; then
    for l in avcodec avformat avutil swresample swscale; do
        cp -a "$RPC/sysroot/lib/lib$l.a" "$PSB/sysroot/lib/"
        cp -a "$RPC/sysroot/include/lib$l" "$PSB/sysroot/include/"
        [ -f "$RPC/sysroot/lib/pkgconfig/lib$l.pc" ] && sed "s|$RPC|$PSB|g" "$RPC/sysroot/lib/pkgconfig/lib$l.pc" > "$PSB/sysroot/lib/pkgconfig/lib$l.pc"
    done
fi
# The PS4's signal context layout in the libc overlay (PPSSPP's fault handler reads it)
PSB="$PSB" bash "$(dirname "$0")/../toolchain/m33-overlay.sh"
# Space-free path to this repo: toolchain flags (linker script paths) break on "PS4 Projects".
REPO="$(cd "$(dirname "$0")/.." && pwd)"
[ -e "$HOME/m33" ] || ln -s "$REPO" "$HOME/m33"
du -sh "$PSB"/* 2>/dev/null
find "$PSB/build/mesa" -name '*.a' | wc -l
