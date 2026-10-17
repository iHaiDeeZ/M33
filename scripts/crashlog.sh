#!/usr/bin/env bash
# Fetch /data/March33/{boot-trace,crash,hang}.log and PPSSPP's ppsspp.log from the console
# and symbolize the crash and hang addresses against the ELF (default: the newest packaged
# version's, from oelf/).
#   crashlog.sh [elf]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
ELF="${1:-$(ls -t "$PS4_BUILD_ROOT"/oelf/v*.elf 2>/dev/null | head -1)}"
HOST="ftp://${PS4_HOST:-192.168.0.212}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/logs"
mkdir -p "$OUT"
for f in boot-trace.log crash.log hang.log ppsspp.log; do
    curl -s --max-time 30 -o "$OUT/$(basename "$f")" "$HOST/data/March33/$f" || rm -f "$OUT/$(basename "$f")"
done
# symbolize against the build that ran (boot-trace.log: "M33 XX.YY starting")
if [ -z "${1:-}" ]; then
    ran="$(grep -oE "M33 [0-9]{2}\.[0-9]{2} starting" "$OUT/boot-trace.log" 2>/dev/null | head -1 | cut -d' ' -f2 || true)"
    [ -n "$ran" ] && [ -f "$PS4_BUILD_ROOT/oelf/v$ran.elf" ] && ELF="$PS4_BUILD_ROOT/oelf/v$ran.elf"
fi
echo "== symbols from $(basename "$ELF")"
symbolize() {
    llvm-symbolizer-21 --obj="$ELF" --functions=linkage --demangle --no-inlines "$1" | head -2 | paste -sd' '
}
# A thread stack dump (ps4_dump_thread_stacks: hang.log, the end of a crash) from stdin, symbolized.
print_thread_dump() {
    while IFS= read -r line; do
        case "$line" in
        "-- "*)
            echo "$line"
            rip="$(echo "$line" | grep -oE "rip elf 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" || true)"
            [ -n "$rip" ] && echo "     rip  $(symbolize "$rip")"
            ;;
        *"stack:"*)
            for addr in $(echo "$line" | grep -oE "0x[0-9a-f]+" | head -14); do
                echo "     $addr  $(symbolize "$addr")"
            done
            ;;
        *) echo "$line" ;;
        esac
    done
}
# screenshots taken from PPSSPP
echo "== screenshots: $(curl -s --max-time 30 --list-only "$HOST/data/March33/screenshots/" | tr '
' ' ')"
echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
echo "== ppsspp.log (last 60 lines)"; tail -60 "$OUT/ppsspp.log" 2>/dev/null || echo "(none)"
# crash.log collects every run's crashes: only the last one, and only if this run crashed
if [ -s "$OUT/crash.log" ] && grep -q "crashed" "$OUT/boot-trace.log" 2>/dev/null; then
    echo "== crash.log (last crash)"
    last="$(awk '/^=== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/crash.log")"
    # the crashing thread (before the other threads' dump)
    crashed="$(printf '%s\n' "$last" | sed '/^==== /,$d')"
    printf '%s\n' "$crashed" | grep -v "^  \[rsp"
    echo "== symbolized"
    printf '%s\n' "$crashed" | grep -oE "(rip 0x[0-9a-f]+ elf |^  \[rsp\+0x[0-9a-f]+\] )0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" |
        while read -r addr; do
            printf '%s  ' "$addr"
            symbolize "$addr"
        done
    # every other thread at the crash (ps4_dump_thread_stacks), symbolized like hang.log
    if printf '%s\n' "$last" | grep -q "^==== "; then
        echo "== the other threads at the crash"
        printf '%s\n' "$last" | sed -n '/^==== /,$p' | print_thread_dump
    fi
    # the emulated side (ps4_crash_guest_info: the PSP CPU registers) follows in crash.log
    printf '%s
' "$last" | sed -n '/^== PSP CPU at the crash/,$p'
else
    echo "== crash.log: no crash in this run"
fi
# hang.log: the watchdog's stack dumps of every named thread
if [ -s "$OUT/hang.log" ]; then
    echo "== hang.log (last dump, symbolized; stacks may include stale return addresses)"
    awk '/^==== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/hang.log" | print_thread_dump
fi
