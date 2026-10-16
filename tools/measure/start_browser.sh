#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Start headless Chrome plus the asset-proxy server for measuring the real game.
# Usage: tools/measure/start_browser.sh <path to built pokerogue dist/> [asset cache dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DIST="$(cd "$1" && pwd)"
CACHE="${2:-$HERE/../.cache/assets}"
mkdir -p "$CACHE" "$HERE/../.cache/shots"
docker build -q -t pkr-chrome "$HERE" >/dev/null
docker rm -f pkr-browse >/dev/null 2>&1 || true
docker run -d --name pkr-browse -v "$HERE":/tools -v "$(cd "$HERE/../.cache/shots" && pwd)":/shots \
  -v "$DIST":/dist:ro -v "$(cd "$CACHE" && pwd)":/cache pkr-chrome bash -c '
    node /tools/serve.js /dist /cache 8000 &
    chromium --headless=new --no-sandbox --remote-debugging-port=9222 --remote-debugging-address=127.0.0.1 \
      --use-angle=swiftshader --enable-unsafe-swiftshader --ignore-gpu-blocklist \
      --autoplay-policy=no-user-gesture-required --window-size=1280,720 about:blank 2>/dev/null &
    wait' >/dev/null
echo "Started. Next: docker exec pkr-browse node /tools/run.js   (screenshots land in tools/.cache/shots)"
echo "Stop it when done (software rendering is CPU-heavy): docker rm -f pkr-browse"
