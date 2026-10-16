# Build setup copied from love-ps4 (J:\Mari0\love-ps4\platform\ps4), which runs on the test
# console. Include from a project configured with PacBrew's toolchain file:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=/opt/pacbrew/ps4/openorbis/cmake/ps4.cmake ...
#   include(<this file>)
#
# Provides ps4_add_eboot(<target>) and PS4_RUNTIME_SOURCES (heap + thread-exit fixes, crash log).

set(PS4_RUNTIME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_dlmalloc.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crashlog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_sampler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_string.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_random.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_net.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_netfs.cpp)

# Generate code for the PS4's CPU (AMD Jaguar: SSE4.2, AVX, BMI1, F16C, MOVBE; no AVX2) rather
# than baseline x86-64 (SSE2).
add_compile_options(-march=btver2)

# System libc heap: system modules (Piglet: program links, shader compiles, command buffers)
# allocate from it, and OpenOrbis' crt1.o leaves its size at the ~12.7 MiB default, which
# Dolphin exhausts within minutes. Link a copy of crt1.o declaring an explicit sceLibcHeapSize.
set(PS4_LIBC_HEAP_MIB 64 CACHE STRING "System libc heap size (MiB) declared in crt1.o")
set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-heap${PS4_LIBC_HEAP_MIB}.o)
execute_process(
  COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/patch-crt1.sh ${OPENORBIS}/lib/crt1.o ${PS4_CRT1}
          ${PS4_LIBC_HEAP_MIB}
  RESULT_VARIABLE _ps4_crt1_result)
if(NOT _ps4_crt1_result EQUAL 0)
  message(FATAL_ERROR "patch-crt1.sh failed: ${_ps4_crt1_result}")
endif()
# That had no effect on hardware. Instead (PS4_MALLOC_REPLACE, default ON) link port/ps4_crt1.S,
# a copy of crt1.o whose libc malloc-replacement table sends every malloc of the process -
# system modules included - to our heap (ps4_replace_* in port/ps4_runtime.cpp).
option(PS4_MALLOC_REPLACE "Route all mallocs, system modules included, to the app heap" ON)
if(PS4_MALLOC_REPLACE)
  set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-malloc-replace.o)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -target x86_64-pc-freebsd12-elf -c
            ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crt1.S -o ${PS4_CRT1}
    RESULT_VARIABLE _ps4_crt1_result)
  if(NOT _ps4_crt1_result EQUAL 0)
    message(FATAL_ERROR "assembling port/ps4_crt1.S failed: ${_ps4_crt1_result}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crt1.S)
endif()
foreach(_lang C CXX)
  string(REPLACE "${OPENORBIS}/lib/crt1.o" "${PS4_CRT1}" CMAKE_${_lang}_LINK_EXECUTABLE
                 "${CMAKE_${_lang}_LINK_EXECUTABLE}")
endforeach()

# The toolchain's linker script only collects plain .init_array, so prioritized constructors
# (.init_array.NNN - e.g. libc++'s iostream setup) end up in an orphan section the loader never
# runs. Link with a copy that includes them, in priority order.
set(PS4_LINK_SCRIPT_IN ${OPENORBIS}/link.x)
set(PS4_LINK_SCRIPT ${CMAKE_BINARY_DIR}/ps4-link.x)
file(READ ${PS4_LINK_SCRIPT_IN} PS4_LINK_SCRIPT_TEXT)
string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(.init_array);" PS4_INIT_ARRAY_POS)
if (PS4_INIT_ARRAY_POS EQUAL -1)
    message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(.init_array);' rule to patch")
endif ()
string(REPLACE "*(.init_array);"
    "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))); KEEP(*(.init_array));"
    PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
# It also only collects plain .data/.tdata/.tbss. Per-symbol sections (.data.<name>: C++ template
# statics and inline variables, libc's stdin/stdout/stderr FILEs, the C++ personality pointer,
# all of Mesa with -fdata-sections) became orphans placed past the end of the data segment's
# file image, so they started out as zeros on the console.
foreach (rule ".data" ".tdata" ".tbss")
    string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(${rule})" PS4_RULE_POS)
    if (PS4_RULE_POS EQUAL -1)
        message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(${rule})' rule to patch")
    endif ()
    string(REPLACE "*(${rule})" "*(${rule} ${rule}.*)" PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
endforeach ()
file(WRITE ${PS4_LINK_SCRIPT} "${PS4_LINK_SCRIPT_TEXT}")
string(REPLACE "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ps4-link.x")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${PS4_LINK_SCRIPT}")
endif ()

# The libc heap can't initialize on retail consoles; route the malloc family to
# port/ps4_runtime.cpp. ps4.cmake links with ld.lld directly, so these are raw linker flags.
foreach (fn malloc free calloc realloc memalign __memalign)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=${fn}")
endforeach ()
# The kernel's pthread_once writes 16 bytes into the headers' 4-byte pthread_once_t
# (port/ps4_runtime.cpp has a replacement).
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=pthread_once")
# Large anonymous mmaps go to the system flexible pool, keeping regular flexible memory for Piglet.
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=mmap")
# CLOCK_MONOTONIC from the time stamp counter instead of a system call (port/ps4_runtime.cpp).
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=clock_gettime")
# New threads on all of the app's cores, not just their creator's (port/ps4_runtime.cpp).
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=pthread_create")
# Host names through the PS4's resolver: the C library has no DNS on the PS4 (port/ps4_net.cpp,
# from Crema; network shares and curl).
# (M33: gethostbyname is defined there outright, see the note in it.)
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=getaddrinfo")
# The network game library: SMB / NFS shares read under /net (port/ps4_netfs.cpp, from Crema): open
# and close (port/ps4_crashlog.cpp) and the reads below. _read and _readv are the C library's stdio
# reads. (not lstat / fstatat: the PS4 has no fstatat)
# On the app only (ps4_add_eboot): CMake's probe programs (PPSSPP's architecture checks) would
# fail to link without these files' __wrap_ functions.
set(PS4_NETFS_WRAPS --wrap=open --wrap=close)
foreach (fn read _read readv _readv pread lseek fstat stat getdents)
    list(APPEND PS4_NETFS_WRAPS --wrap=${fn})
endforeach ()

# Program authority ID: the system Piglet (OpenGL ES) only gives a display to processes with a
# system authority ID. This is RetroArch for PS4's value, as used by love-ps4.
set(PS4_PAID "0x3100000000000002" CACHE STRING "Program authority ID of eboot.bin")
set(PS4_AUTH_INFO "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

# Mesa's RADV Vulkan driver on GNM (scripts/build-mesa.sh), linked into <target>. Whole archives,
# as Mesa's own shared-library link does: the dispatch tables refer to entry points through weak
# symbols, which don't pull archive members (they'd silently be NULL). libvulkan_radeon.a
# already contains the Vulkan runtime and WSI objects.
set(PS4_MESA_BUILD "$ENV{HOME}/m33-build/build/mesa" CACHE PATH "Mesa PS4 build directory")
function(ps4_link_mesa target)
    set(libs
        src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a
        src/amd/compiler/libaco.a src/amd/addrlib/libaddrlib.a src/vulkan/util/libvulkan_util.a
        src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
        src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
        src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libparson.a
        src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
    list(TRANSFORM libs PREPEND "${PS4_MESA_BUILD}/")
    # ps4.cmake links with ld.lld directly: raw linker flags.
    # Plain signature: PPSSPP links its app target without keywords, and CMake forbids mixing them.
    target_link_libraries(${target} --whole-archive ${libs} --no-whole-archive)
endfunction()

# ELF -> <binary dir>/<target>_eboot/eboot.bin (fake SELF), then check the result: PacBrew's
# create-fself (Nov 2024) predates OpenOrbis create-fself 626d440, and depending on the size of
# .data.rel.ro it emits the data PT_LOAD twice, which the console rejects at launch
# ("CE-41839-5", LNC 0x80aa001a).
function(ps4_add_eboot target)
    target_link_options(${target} PRIVATE ${PS4_NETFS_WRAPS})
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_eboot)
    add_custom_command(
        OUTPUT ${out_dir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OPENORBIS}
            ${OPENORBIS}/bin/create-fself -in=$<TARGET_FILE:${target}>
            -out=${out_dir}/${target}.oelf --eboot ${out_dir}/eboot.bin
            --paid ${PS4_PAID} --authinfo ${PS4_AUTH_INFO}
        COMMAND ${CMAKE_COMMAND} -DOELF=${out_dir}/${target}.oelf
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check-oelf.cmake
        DEPENDS ${target}
        VERBATIM)
    add_custom_target(${target}_eboot ALL DEPENDS ${out_dir}/eboot.bin)
endfunction()
