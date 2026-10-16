#!/usr/bin/env bash
# Build libdolphinps4_rt.a: replacements for broken OpenOrbis runtime pieces
# (currently malloc, from PSChrome). Linked ahead of the OpenOrbis libc.a.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"

OBJ="$PSB/build/runtime"
mkdir -p "$OBJ" "$PS4_SYSROOT/lib"
"$HERE/bin/ps4-cc" -std=c11 -O2 -Wall -Wextra -c "$HERE/runtime/ps4_malloc.c" -o "$OBJ/ps4_malloc.o"
rm -f "$PS4_SYSROOT/lib/libdolphinps4_rt.a"
"$PS4_AR" rcs "$PS4_SYSROOT/lib/libdolphinps4_rt.a" "$OBJ/ps4_malloc.o"
echo "built $PS4_SYSROOT/lib/libdolphinps4_rt.a"
