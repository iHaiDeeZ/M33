#!/usr/bin/env bash
# Package the PS4 PPSSPP build (after configure-ppsspp.sh + build-ppsspp.sh) as M33 (March33),
# MRCH00033 (IV0000-MRCH00033_00-MARCH33PSPEMU000.pkg).
#   M33_VERSION=00.01 package-m33.sh
# Older builds of this app are deleted from out/. The console gets the package from Windows with
# scripts/upload-to-console.sh (uploads sent from WSL were lost on the console, see Resonance).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
BUILD="$PS4_BUILD_ROOT/build/ppsspp"
SRC="${PPSSPP_SRC:-$PS4_BUILD_ROOT/src/ppsspp}"
EBOOT="$BUILD/PPSSPPPS4_eboot/eboot.bin"
VERSION="${M33_VERSION:-00.01}"

[ -f "$EBOOT" ] || { echo "missing $EBOOT: build first" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9][0-9]\.[0-9][0-9]$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
STAGE="$PS4_BUILD_ROOT/build/m33-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$EBOOT" "$STAGE/eboot.bin"
cp "$HERE/../sce_sys/icon0.png" "$HERE/../sce_sys/pic1.png" "$STAGE/sce_sys/"
echo "$VERSION" > "$STAGE/version.txt"
# PPSSPP's assets (UI images, fonts, flash0, languages, shaders, themes, compat.ini, ...), without
# the web debugger and upload pages
mkdir -p "$STAGE/assets"
tar -C "$SRC/assets" --exclude=debugger --exclude=upload --exclude=.git -cf - . | tar -C "$STAGE/assets" -xf -
# M33's branding over them: its files (assets/: the March33 theme, the menus' background), then its
# images in the UI atlas and its name in the texts (scripts/brand-stage.py; brand/ from make-brand.py)
cp -r "$HERE/../assets/." "$STAGE/assets/"
python3 "$HERE/brand-stage.py" "$STAGE/assets" "$HERE/.."
# The startup clip (scripts/blender/startup_anim.py + encode.py), played instead of the drawn opening.
# It carries the user's own PSP startup video (Sony's): bundled in the package on purpose, kept out of
# git (.gitignore: brand/startup/). M33_STARTUP_CLIP= (empty) packages without it.
CLIP="${M33_STARTUP_CLIP-$HERE/../brand/startup/m33-startup-preview.mp4}"
if [ -n "$CLIP" ] && [ -f "$CLIP" ]; then
    cp "$CLIP" "$STAGE/assets/startup.mp4"
    echo "startup clip: $(basename "$CLIP") ($(stat -c %s "$CLIP") bytes)"
fi
# PkgTool's pkg_build crashes ("Sequence contains no elements") on empty folders.
find "$STAGE" -type d -empty -delete
# Vulkan (RADV on GNM) runs under the plain homebrew identity (4.5 GiB of direct memory).
PKG="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR=/nonexistent "$HERE/make-pkg.sh" "$STAGE" MRCH00033 "M33" "$VERSION" MARCH33PSPEMU "$PS4_BUILD_ROOT/out" | tail -1)"
ls -la "$PKG"
# a copy next to the repo (E:\PS4 Projects\March33\out), easy to find from Windows; *.pkg is ignored by git
mkdir -p "$HERE/../out"
NAME="M33-v$VERSION.pkg"
cp "$PKG" "$HERE/../out/$NAME"
find "$HERE/../out" -maxdepth 1 -name '*.pkg' ! -name "$NAME" -print -delete | sed 's|^|removed old |'
# Keep the symbols of every packaged build: crash logs must be symbolized against the exact binary.
mkdir -p "$PS4_BUILD_ROOT/oelf"
cp "$BUILD/PPSSPPPS4_eboot/PPSSPPPS4.oelf" "$PS4_BUILD_ROOT/oelf/v$VERSION.oelf"
cp "$BUILD/PPSSPPPS4" "$PS4_BUILD_ROOT/oelf/v$VERSION.elf"
echo "now, from Windows (Git Bash): scripts/upload-to-console.sh out/$NAME"
