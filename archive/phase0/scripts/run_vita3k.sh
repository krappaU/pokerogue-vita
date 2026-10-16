#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Install and run build-vita/pokerogue_bench.vpk in Vita3K under a virtual X server, then print the
# app's log. Use this to check the app runs without crashing; emulator timings are not representative.
set -euo pipefail
cd "$(dirname "$0")/.."
CACHE=.cache/vita3k
mkdir -p "$CACHE/home"
if [ ! -d "$CACHE/squashfs-root" ]; then
  curl -sfL -o "$CACHE/Vita3K.AppImage" https://github.com/Vita3K/Vita3K/releases/download/continuous/Vita3K-x86_64.AppImage
  chmod +x "$CACHE/Vita3K.AppImage"
  docker build -q -t pkrb-vita3k -f docker/vita3k.Dockerfile docker >/dev/null
  docker run --rm -v "$PWD/$CACHE":/v -w /v pkrb-vita3k ./Vita3K.AppImage --appimage-extract >/dev/null
fi
LOG="$CACHE/home/.local/share/Vita3K/Vita3K/ux0/data/pokerogue-bench/log.txt"
rm -f "$LOG"
# The app waits for START at the end, so let it run for a fixed time (about 90 s is enough).
docker run --rm -e XDG_RUNTIME_DIR=/tmp -v "$PWD/$CACHE":/v -v "$PWD/$CACHE/home":/root -v "$PWD/build-vita":/vpk pkrb-vita3k \
  bash -c "cd /v && xvfb-run -a -s '-screen 0 1280x720x24' timeout ${VITA3K_SECONDS:-150} squashfs-root/AppRun --frontend sdl -B OpenGL /vpk/pokerogue_bench.vpk >/dev/null 2>&1" || true
cat "$LOG"
