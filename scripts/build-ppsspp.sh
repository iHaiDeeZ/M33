#!/usr/bin/env bash
# Build PPSSPP for PS4 (run scripts/configure-ppsspp.sh first). Extra args go to ninja.
#   build-ppsspp.sh [ninja args...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"  # also puts PacBrew ld.lld's libxml2 compat link on LD_LIBRARY_PATH
ninja -C "$PS4_BUILD_ROOT/build/ppsspp" -j"${JOBS:-12}" "$@"
