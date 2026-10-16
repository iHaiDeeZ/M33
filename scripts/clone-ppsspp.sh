#!/usr/bin/env bash
# Clone upstream PPSSPP (with submodules) into the WSL build tree at the base recorded in
# patches/BASE.txt, and link the repo space-free.
set -euo pipefail
ln -sfn "/mnt/e/PS4 Projects/March33" "$HOME/m33"
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(awk '/^base:/{print $2}' "$HERE/../patches/BASE.txt" 2>/dev/null || true)"
mkdir -p "$HOME/m33-build/src" "$HOME/m33-build/logs"
cd "$HOME/m33-build/src"
[ -d ppsspp ] || git clone https://github.com/hrydgard/ppsspp.git ppsspp
cd ppsspp
if [ -n "$BASE" ]; then
    git fetch -q origin
    git checkout -q "$BASE"
fi
git submodule update --init --recursive
git log -1 --format='%h %ci %s' && git submodule status
