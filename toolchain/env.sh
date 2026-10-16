# Shared settings for the modern PS4 toolchain (clang 21 + libc++ 21 on top
# of the OpenOrbis C runtime). Source this from the other scripts.

export PSB="${PSB:-$HOME/m33-build}"          # build tree lives in WSL, not on J:
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PS4_SYSROOT="${PS4_SYSROOT:-$PSB/sysroot}"                    # our libc++, ICU, ... install here
export PS4_TARGET="x86_64-pc-freebsd12-elf"
export CLANG_VER="${CLANG_VER:-21}"
export PS4_CC="clang-$CLANG_VER"
export PS4_CXX="clang++-$CLANG_VER"
export PS4_LD="ld.lld-$CLANG_VER"
export PS4_AR="llvm-ar-$CLANG_VER"
export PS4_RANLIB="llvm-ranlib-$CLANG_VER"

# The OpenOrbis C library is musl; __PS4__ tells our patched libc++ not to
# take its FreeBSD code paths.
export PS4_BASE_FLAGS="--target=$PS4_TARGET -D_GNU_SOURCE -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -fPIC -funwind-tables \
 --sysroot=$OPENORBIS -isystem $PS4_SYSROOT/libc-overlay -isystem $OPENORBIS/include"
export PS4_CXX_FLAGS="-nostdinc++ -isystem $PS4_SYSROOT/include/c++/v1"

export JOBS="${JOBS:-$(nproc)}"

need() {
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t (see toolchain/README.md)" >&2; exit 1; }
    done
}
