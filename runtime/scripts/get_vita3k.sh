#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Download and unpack the Vita3K emulator into .cache/vita3k/squashfs-root (once per pinned build).
# Pinned to a numbered build of Vita3K/Vita3K-builds, checked against its hash: the rolling
# "continuous" build changes under us, and a build can break CI (4236 couldn't create its OpenGL
# context under Xvfb's software GL: GLXBadFBConfig). Build 4358 since 2026-11-16, the newest that
# passed CI then. To move the pin, check that scripts/test_vita3k.sh passes with the new build,
# then update both values.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=4358
SHA256=cac43477f45f312292bc31ef1ff0aa6cdcab6b64c7dd76df81cb5d5ef7997e5a
V3K=.cache/vita3k
[ -x "$V3K/squashfs-root/AppRun" ] && [ "$(cat "$V3K/build.txt" 2>/dev/null)" = "$BUILD" ] && exit 0
rm -rf "$V3K"
mkdir -p "$V3K"
curl -sfL -o "$V3K/Vita3K.AppImage" "https://github.com/Vita3K/Vita3K-builds/releases/download/$BUILD/Vita3K-x86_64.AppImage"
echo "$SHA256  $V3K/Vita3K.AppImage" | sha256sum -c --quiet
chmod +x "$V3K/Vita3K.AppImage"
(cd "$V3K" && ./Vita3K.AppImage --appimage-extract >/dev/null)
echo "$BUILD" > "$V3K/build.txt"
