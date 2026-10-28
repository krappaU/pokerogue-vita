#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Download and unpack the Vita3K emulator into .cache/vita3k/squashfs-root (once per pinned build).
# Pinned to a numbered build of Vita3K/Vita3K-builds, checked against its hash: the rolling
# "continuous" build changes under us, and build 4236 (2026-10-27) can't create its OpenGL context
# under Xvfb's software GL (GLXBadFBConfig), which failed every CI run. To move the pin, check that
# scripts/test_vita3k.sh passes with the new build, then update both values.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=4201
SHA256=b8f3e5e2920cc6c4b12a1f89ff9f3c40c11a0dcbc3e30212ec69993a9e419df7
V3K=.cache/vita3k
[ -x "$V3K/squashfs-root/AppRun" ] && [ "$(cat "$V3K/build.txt" 2>/dev/null)" = "$BUILD" ] && exit 0
rm -rf "$V3K"
mkdir -p "$V3K"
curl -sfL -o "$V3K/Vita3K.AppImage" "https://github.com/Vita3K/Vita3K-builds/releases/download/$BUILD/Vita3K-x86_64.AppImage"
echo "$SHA256  $V3K/Vita3K.AppImage" | sha256sum -c --quiet
chmod +x "$V3K/Vita3K.AppImage"
(cd "$V3K" && ./Vita3K.AppImage --appimage-extract >/dev/null)
echo "$BUILD" > "$V3K/build.txt"
