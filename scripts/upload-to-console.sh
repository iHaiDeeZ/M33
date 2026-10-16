#!/usr/bin/env bash
# Puts an M33 package in the console's /data/pkg and removes this app's older packages there
# (M33-v*, IV0000-MRCH00033*; other apps' are left alone).
#   scripts/upload-to-console.sh out/M33-vXX.YY.pkg
# Run it from Windows (Git Bash), not from WSL: on 2026-10-12 every upload sent from inside WSL
# (even through Windows' curl.exe) was undone on the console a minute later (the old package back,
# the new one gone), while the same steps from Windows stayed.
set -euo pipefail
PKG="${1:?package path}"
NAME="$(basename "$PKG")"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
SIZE="$(stat -c %s "$PKG")"
export MSYS_NO_PATHCONV=1

listing() { curl -sS --list-only "$HOST/data/pkg/" | tr -d '\r'; }
present() { curl -sS "$HOST/data/pkg/" | tr -d '\r' | awk -v n="$NAME" -v s="$SIZE" '$NF == n && $5 == s { f = 1 } END { exit !f }'; }
# the new package stays in the listing for 30 s
settled() { for _ in 1 2 3; do sleep 10; present || return 1; done; }

for try in 1 2 3; do
    curl -sS -T "$PKG" "$HOST/data/pkg/"
    echo "uploaded $NAME"
    settled && break
    echo "console: $NAME didn't stay, uploading again"
done
for old in $(listing | grep -E '^(M33-v[0-9.]+|IV0000-MRCH00033_00-[A-Z0-9]+)[.]pkg$' | grep -vx "$NAME"); do
    if curl -sS "$HOST/" -Q "DELE /data/pkg/$old" > /dev/null; then echo "console: removed old $old"; else echo "console: could not remove $old"; fi
    sleep 15
done
settled || { echo "console: $NAME went missing after the cleanup, uploading again"; curl -sS -T "$PKG" "$HOST/data/pkg/"; settled; }
listing | grep -E '^(M33-v|IV0000-MRCH00033)' | sed 's/^/console has: /'
