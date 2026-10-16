#!/usr/bin/env bash
# Configure PPSSPP for PS4 (M33): the PS4 frontend (ps4/), Vulkan on DolphinPS4's RADV port, no
# OpenGL / SDL / X11, FFmpeg 7 from the sysroot. Run through ~/m33 (no space in the path).
#   configure-ppsspp.sh [extra cmake args...]
#
# pthreads are in libkernel, but CMake's probe programs don't link: FindThreads (armips) is told.
# musl's libc.a has getauxval, but it's a Linux system call the PS4's FreeBSD kernel lacks.
# zstd from the sysroot: the bundled one's dictionary builder needs qsort_r, which that libc lacks.
# libzip and libpng from the sysroot: found as config packages (imported targets), but PPSSPP links
# with LIBZIP_LIBRARY / PNG_LIBRARIES, which those don't set.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${PPSSPP_SRC:-$PS4_BUILD_ROOT/src/ppsspp}"
BUILD="$PS4_BUILD_ROOT/build/ppsspp"
export PKG_CONFIG_LIBDIR="$PS4_BUILD_ROOT/sysroot/lib/pkgconfig"
unset PKG_CONFIG_PATH
ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-m33.cmake" \
    "-DCMAKE_PROJECT_PPSSPP_INCLUDE=$HERE/../toolchain/ps4-love-style.cmake" \
    -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" \
    -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" \
    -DUSE_FFMPEG=ON -DUSE_SYSTEM_FFMPEG=ON -DFFMPEG_DIR="$PS4_BUILD_ROOT/sysroot" \
    -DUSE_SYSTEM_LIBZIP=ON -DLIBZIP_LIBRARY="$PS4_BUILD_ROOT/sysroot/lib/libzip.a;$PS4_BUILD_ROOT/sysroot/lib/libz.a" \
    -DUSE_SYSTEM_LIBPNG=ON -DPNG_LIBRARIES="$PS4_BUILD_ROOT/sysroot/lib/libpng16.a;$PS4_BUILD_ROOT/sysroot/lib/libz.a" \
    -DUSE_SYSTEM_ZSTD=ON -DUSE_SYSTEM_SNAPPY=OFF -DUSE_SYSTEM_LIBCHDR=OFF \
    -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_CCACHE=OFF \
    -DHEADLESS=OFF -DUNITTEST=OFF -DATLAS_TOOL=OFF \
    -DCMAKE_HAVE_LIBC_PTHREAD=1 -DCMAKE_THREAD_LIBS_INIT= -DThreads_FOUND=TRUE \
    -DHAVE_GETAUXVAL=0 -DHAVE_ELF_AUX_INFO=0 \
    "$@"
