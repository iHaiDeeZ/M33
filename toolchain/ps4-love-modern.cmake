# love-ps4's toolchain (PacBrew ps4.cmake: ld.lld 12 called directly, OpenOrbis CRT and C
# library) with one change: clang 21 + libc++ 21 instead of clang 12 + libc++ 12, because
# Dolphin needs C++23. libc++ 21 is built by build-libcxx.sh into PS4_SYSROOT together with the
# libc-overlay headers (FreeBSD values for the musl headers' clock ids, mmap flags and signals).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=/mnt/j/DolphinPS4/toolchain/ps4-love-modern.cmake ...
#   include(ps4-love-style.cmake) in the project, as with the plain PacBrew toolchain.

if (DEFINED ENV{OPENORBIS})
    set(OPENORBIS $ENV{OPENORBIS})
else ()
    set(OPENORBIS /opt/pacbrew/ps4/openorbis)
endif ()
include(${OPENORBIS}/cmake/ps4.cmake)

if (DEFINED ENV{PS4_SYSROOT})
    set(PS4_SYSROOT $ENV{PS4_SYSROOT})
else ()
    set(PS4_SYSROOT $ENV{HOME}/m33-build/sysroot)
endif ()
if (NOT DEFINED CLANG_VER)
    set(CLANG_VER 21)
endif ()
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OPENORBIS PS4_SYSROOT CLANG_VER)

find_program(PS4_MODERN_CC clang-${CLANG_VER} REQUIRED)
find_program(PS4_MODERN_CXX clang++-${CLANG_VER} REQUIRED)
set(CMAKE_C_COMPILER ${PS4_MODERN_CC} CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER ${PS4_MODERN_CXX} CACHE PATH "" FORCE)
set(CMAKE_ASM_COMPILER ${PS4_MODERN_CC} CACHE PATH "" FORCE)

# Same flags as ps4.cmake, plus the libc overlay ahead of the OpenOrbis C headers.
set(_ps4_c_flags
  "-target x86_64-pc-freebsd12-elf \
   -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ \
   -DPS4 -D__BSD_VISIBLE -D_BSD_SOURCE \
   -fPIC -funwind-tables \
   -isysroot ${OPENORBIS} -isystem ${PS4_SYSROOT}/libc-overlay -isystem ${OPENORBIS}/include \
   -I${OPENORBIS}/usr/include")
set(CMAKE_ASM_FLAGS_INIT "${_ps4_c_flags}")
set(CMAKE_C_FLAGS_INIT "${_ps4_c_flags}")
# libc++ 21 headers replace the OpenOrbis libc++ 12 ones and must come before the C headers.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${PS4_SYSROOT}/include/c++/v1 ${_ps4_c_flags}")

set(CMAKE_CXX_STANDARD_LIBRARIES
    "${CMAKE_C_STANDARD_LIBRARIES} ${PS4_SYSROOT}/lib/libc++.a ${PS4_SYSROOT}/lib/libc++abi.a ${OPENORBIS}/lib/libunwind.a")
