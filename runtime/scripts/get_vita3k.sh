#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Download and unpack the Vita3K emulator into .cache/vita3k/squashfs-root (once). Vita3K only
# publishes rolling "continuous" builds; the version in use is in the emulator's log.
set -euo pipefail
cd "$(dirname "$0")/.."
V3K=.cache/vita3k
[ -x "$V3K/squashfs-root/AppRun" ] && exit 0
mkdir -p "$V3K"
curl -sfL -o "$V3K/Vita3K.AppImage" https://github.com/Vita3K/Vita3K/releases/download/continuous/Vita3K-x86_64.AppImage
chmod +x "$V3K/Vita3K.AppImage"
(cd "$V3K" && ./Vita3K.AppImage --appimage-extract >/dev/null)
