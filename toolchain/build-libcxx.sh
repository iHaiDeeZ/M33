#!/usr/bin/env bash
# Build libc++ / libc++abi 21 for the PS4 (static) into $PS4_SYSROOT.
set -euo pipefail
source "$(dirname "$0")/env.sh"
need "$PS4_CC" "$PS4_CXX" cmake ninja

"$(dirname "$0")/prepare-sysroot.sh"
LLVM_VER=21.1.6
SRC="$PSB/src/llvm-project-$LLVM_VER.src"
BUILD="$PSB/build/libcxx"

# Only the runtimes subtrees are needed. Copy them from a local LLVM tree when
# one exists (PSChrome's), otherwise download the release. Our own copy, since
# the libc++ sources are patched in place below.
if [ ! -d "$SRC/libcxx" ]; then
    mkdir -p "$PSB/src"
    LOCAL="${LLVM_LOCAL_SRC:-$HOME/pschrome-build/src/llvm-project-$LLVM_VER.src}"
    if [ -d "$LOCAL/libcxx" ]; then
        mkdir -p "$SRC"
        for d in runtimes libcxx libcxxabi libunwind libc cmake llvm/cmake llvm/utils/lit llvm/utils/llvm-lit third-party; do
            [ -e "$LOCAL/$d" ] || continue
            mkdir -p "$SRC/$(dirname "$d")"
            cp -a "$LOCAL/$d" "$SRC/$d"
        done
        # PSChrome's tree already has the __FreeBSD__ -> musl rewrite below.
        [ -f "$LOCAL/.ps4-patched" ] && touch "$SRC/.ps4-patched"
    else
        curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"             | tar -xJ -C "$PSB/src" "llvm-project-$LLVM_VER.src/"{runtimes,libcxx,libcxxabi,libunwind,libc,cmake,llvm/cmake,llvm/utils/lit,llvm/utils/llvm-lit,third-party}
    fi
fi
# OpenOrbis pairs a FreeBSD target triple with musl: make libc++ treat PS4 as
# musl wherever it would pick a FreeBSD-specific path (same idea as the
# OpenOrbis patch to libc++ 12).
if [ ! -f "$SRC/.ps4-patched" ]; then
    grep -rl '__FreeBSD__' "$SRC/libcxx/include" "$SRC/libcxx/src" | xargs sed -i -E \
        -e 's/defined\(__FreeBSD__\)/(defined(__FreeBSD__) \&\& !defined(__PS4__))/g' \
        -e 's/^(#[ ]*)ifdef __FreeBSD__/\1if defined(__FreeBSD__) \&\& !defined(__PS4__)/'
    touch "$SRC/.ps4-patched"
fi
# musl's headers carry Linux syscall numbers; the PS4 kernel is FreeBSD, so
# never issue SYS_gettid (only used for guard deadlock diagnostics).
sed -i -E 's/^#elif defined\(SYS_gettid\) && _LIBCPP_HAS_THREAD_API_PTHREAD$/#elif defined(SYS_gettid) \&\& _LIBCPP_HAS_THREAD_API_PTHREAD \&\& !defined(__PS4__)/' \
    "$SRC/libcxxabi/src/cxa_guard_impl.h"
# The musl headers ship <sys/sendfile.h> with Linux's signature, but the PS4
# kernel implements FreeBSD's sendfile: use the portable copy path instead.
sed -i -E 's/^#if __has_include\(<sys\/sendfile.h>\)$/#if __has_include(<sys\/sendfile.h>) \&\& !defined(__PS4__)/'     "$SRC/libcxx/src/filesystem/operations.cpp"
# musl gets its locale support through the Linux path.
sed -i -E 's/^#  elif defined\(__linux__\)$/#  elif defined(__linux__) || defined(__PS4__)/' \
    "$SRC/libcxx/include/__locale_dir/locale_base_api.h"

cmake -G Ninja -S "$SRC/runtimes" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PS4_SYSROOT" \
    -DCMAKE_SYSTEM_NAME=FreeBSD \
    -DCMAKE_C_COMPILER="$PS4_CC" -DCMAKE_CXX_COMPILER="$PS4_CXX" \
    -DCMAKE_AR="$(command -v "$PS4_AR")" -DCMAKE_RANLIB="$(command -v "$PS4_RANLIB")" \
    -DCMAKE_C_COMPILER_TARGET="$PS4_TARGET" -DCMAKE_CXX_COMPILER_TARGET="$PS4_TARGET" \
    -DCMAKE_C_FLAGS="$PS4_BASE_FLAGS" -DCMAKE_CXX_FLAGS="$PS4_BASE_FLAGS" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_HAS_MUSL_LIBC=ON \
    -DLIBCXX_HAS_PTHREAD_API=ON \
    -DLIBCXX_ENABLE_FILESYSTEM=ON \
    -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
    -DLIBCXX_USE_COMPILER_RT=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXXABI_HAS_PTHREAD_API=ON \
    -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF

ninja -C "$BUILD" -j"$JOBS" cxx cxxabi
ninja -C "$BUILD" install-cxx install-cxxabi
echo "libc++ installed to $PS4_SYSROOT"
ls -la "$PS4_SYSROOT/lib"
