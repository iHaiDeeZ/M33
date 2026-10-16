# CMake toolchain for the third-party libraries (scripts/build-deps.sh): clang 21 + libc++ 21
# targeting PS4 homebrew; configure checks link real PS4 executables (PS4_TRY_LINK). From PSChrome.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=.../toolchain/ps4-deps.cmake ...

cmake_minimum_required(VERSION 3.20)

if (DEFINED ENV{OPENORBIS})
    set(OPENORBIS $ENV{OPENORBIS})
else ()
    set(OPENORBIS /opt/pacbrew/ps4/openorbis)
endif ()
if (DEFINED ENV{PS4_SYSROOT})
    set(PS4_SYSROOT $ENV{PS4_SYSROOT})
else ()
    set(PS4_SYSROOT $ENV{HOME}/m33-build/sysroot)
endif ()
if (NOT DEFINED CLANG_VER)
    set(CLANG_VER 21)
endif ()

# Forward to try_compile projects.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OPENORBIS PS4_SYSROOT CLANG_VER PS4_LIBC_FIRST)

set(PS4 TRUE)
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 12)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING TRUE)

set(PS4_TARGET x86_64-pc-freebsd12-elf)
find_program(CMAKE_C_COMPILER clang-${CLANG_VER} REQUIRED)
find_program(CMAKE_CXX_COMPILER clang++-${CLANG_VER} REQUIRED)
find_program(CMAKE_LINKER ld.lld-${CLANG_VER} REQUIRED)
find_program(CMAKE_AR llvm-ar-${CLANG_VER} REQUIRED)
find_program(CMAKE_RANLIB llvm-ranlib-${CLANG_VER} REQUIRED)
find_program(CMAKE_STRIP llvm-strip-${CLANG_VER})
find_program(CMAKE_OBJCOPY llvm-objcopy-${CLANG_VER})
set(CMAKE_C_COMPILER_TARGET ${PS4_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${PS4_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${PS4_TARGET})

set(_ps4_flags "-D_GNU_SOURCE -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -fPIC -funwind-tables -march=btver2 \
 --sysroot=${OPENORBIS} -isystem ${PS4_SYSROOT}/libc-overlay -isystem ${OPENORBIS}/include -isystem ${PS4_SYSROOT}/include")
set(CMAKE_C_FLAGS_INIT "${_ps4_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_ps4_flags}")
# libc++ 21 headers must come before the C headers they wrap.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${PS4_SYSROOT}/include/c++/v1 ${_ps4_flags}")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "PS4 homebrew links statically")
# Library builds (build-deps.sh) pass -DPS4_TRY_LINK=ON so configure checks
# link a real PS4 executable: compile-only checks report every function as
# present.
if (NOT PS4_TRY_LINK)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif ()

# Our sysroot first, so its libc++ wins over the OpenOrbis libc++ 12.
set(CMAKE_FIND_ROOT_PATH ${PS4_SYSROOT} ${OPENORBIS}/usr ${OPENORBIS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link through the clang driver (so -Wl,... options from projects like WebKit
# work) but with lld, the OpenOrbis linker script and CRT, and no host libs.
set(PS4_LINK_DIRS "-L${PS4_SYSROOT}/lib -L${OPENORBIS}/usr/lib -L${OPENORBIS}/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-fuse-ld=lld -nostdlib -Wl,--no-dynamic-linker -Wl,-m,elf_x86_64 -Wl,-pie -Wl,--eh-frame-hdr -Wl,--script,${CMAKE_CURRENT_LIST_DIR}/ps4-link.x ${PS4_LINK_DIRS}")
# Several functions (open, sysconf, nanosleep, ...) exist both in the
# OpenOrbis libc.a and the PS4 kernel library; the first one listed wins.
if (PS4_LIBC_FIRST)
    set(CMAKE_C_STANDARD_LIBRARIES "-lc -lkernel -lSceLibcInternal ${OPENORBIS}/lib/libclang_rt.builtins-x86_64.a")
else ()
    set(CMAKE_C_STANDARD_LIBRARIES "-lkernel -lc -lSceLibcInternal ${OPENORBIS}/lib/libclang_rt.builtins-x86_64.a")
endif ()
# Runtime fixes (malloc) must win over the OpenOrbis
# libc.a; its getaddrinfo loads libSceNet through libSceSysmodule.
set(CMAKE_C_STANDARD_LIBRARIES "${PS4_SYSROOT}/lib/libdolphinps4_rt.a ${CMAKE_C_STANDARD_LIBRARIES} -lSceNet -lSceSysmodule")
set(CMAKE_CXX_STANDARD_LIBRARIES "${PS4_SYSROOT}/lib/libc++.a ${PS4_SYSROOT}/lib/libc++abi.a ${OPENORBIS}/lib/libunwind.a ${CMAKE_C_STANDARD_LIBRARIES}")

set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> -o <TARGET> -Wl,--start-group ${OPENORBIS}/lib/crt1.o ${OPENORBIS}/lib/crti.o <OBJECTS> <LINK_LIBRARIES> ${OPENORBIS}/lib/crtn.o -Wl,--end-group")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> -o <TARGET> -Wl,--start-group ${OPENORBIS}/lib/crt1.o ${OPENORBIS}/lib/crti.o <OBJECTS> <LINK_LIBRARIES> ${OPENORBIS}/lib/crtn.o -Wl,--end-group")

