#!/usr/bin/env bash
# Configure + build with logs (~/m33-build/logs/{configure,build}.log) and an error summary.
#   rebuild-ppsspp.sh [clean]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ "${1:-}" = clean ] && rm -rf "$HOME/m33-build/build/ppsspp"
bash "$HERE/logrun.sh" configure bash "$HERE/configure-ppsspp.sh" || { tail -40 "$HOME/m33-build/logs/configure.log"; exit 1; }
bash "$HERE/logrun.sh" build bash "$HERE/build-ppsspp.sh" -k 0
L="$HOME/m33-build/logs/build.log"
tail -1 "$L"; echo "errors: $(grep -c 'error:' "$L")"
grep "error:" "$L" | sed 's/.*src\/ppsspp\///' | sort | uniq -c | sort -rn | head -${TOP:-60}
