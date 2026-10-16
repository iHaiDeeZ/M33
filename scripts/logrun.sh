#!/usr/bin/env bash
# logrun.sh <log name> <command...>: run a command with its output in ~/m33-build/logs/<name>.log
# and its exit status appended as the last line (for long runs started from Windows).
name="$1"; shift
log="$HOME/m33-build/logs/$name.log"
mkdir -p "$(dirname "$log")"
"$@" > "$log" 2>&1
rc=$?
echo "exit=$rc" >> "$log"
exit $rc
