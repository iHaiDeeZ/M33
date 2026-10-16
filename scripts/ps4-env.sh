# Source to build like love-ps4: PacBrew OpenOrbis toolchain (clang 12, ld.lld, create-fself).
PS4_BUILD_ROOT="${PS4_BUILD_ROOT:-$HOME/m33-build}"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
# shellcheck disable=SC1091
source "$OPENORBIS/ps4vars.sh"
# PacBrew's ld.lld links against libxml2.so.2; newer distros only ship libxml2.so.16 (same C
# API), so give it a private compat link if needed (from love-ps4's build.sh).
if ldd "$OPENORBIS/bin/ld.lld" | grep -q "libxml2.so.2 => not found"; then
    compat=$(ls /usr/lib/x86_64-linux-gnu/libxml2.so.* 2>/dev/null | grep -E 'libxml2\.so\.[0-9]+$' | head -1)
    if [ -n "$compat" ]; then
        mkdir -p "$PS4_BUILD_ROOT/host-compat"
        ln -sf "$compat" "$PS4_BUILD_ROOT/host-compat/libxml2.so.2"
        export LD_LIBRARY_PATH="$PS4_BUILD_ROOT/host-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
fi
# ps4vars.sh exports CFLAGS/CXXFLAGS/LDFLAGS for makefile projects; CMake would prepend them to
# the toolchain's own flags (and the CXXFLAGS break <cmath>), so clear them for CMake.
ps4_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
}
