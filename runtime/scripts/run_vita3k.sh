#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Install and run build-vita/pokerogue.vpk in the Vita3K emulator (headless), save a screenshot every
# 15 s to .cache/vita3k-shots/, and print the app's log. Crash/compatibility checks only: emulator
# speed says nothing about the real Vita.
# Needs your own libshacccg.suprx in .cache/private/ (never commit or share it).
set -euo pipefail
cd "$(dirname "$0")/.."
SECONDS_TO_RUN=${1:-120}
V3K=../phase0/.cache/vita3k          # shared emulator install (see ../phase0/scripts/run_vita3k.sh)
[ -d "$V3K/squashfs-root" ] || { echo "run ../phase0/scripts/run_vita3k.sh once to install Vita3K"; exit 1; }
FS="$V3K/home/.local/share/Vita3K/Vita3K"
mkdir -p "$FS/ur0/data" .cache/vita3k-shots
[ -f .cache/private/libshacccg.suprx ] && cp .cache/private/libshacccg.suprx "$FS/ur0/data/"
rm -f "$FS/ux0/data/pokerogue/log.txt" .cache/vita3k-shots/*.png
docker build -q -t pkrb-vita3k -f ../phase0/docker/vita3k.Dockerfile ../phase0/docker >/dev/null
docker rm -f pkr-vita3k >/dev/null 2>&1 || true
docker run --rm --init --name pkr-vita3k ${DOCKER_ARGS:-} -e XDG_RUNTIME_DIR=/tmp -v "$(cd "$V3K" && pwd)":/v -v "$(cd "$V3K/home" && pwd)":/root \
  -v "$PWD/build-vita":/vpk -v "$PWD/.cache/vita3k-shots":/shots pkrb-vita3k bash -c "
    Xvfb :7 -screen 0 1280x800x24 >/dev/null 2>&1 &
    export DISPLAY=:7; sleep 2
    (cd /v && timeout $SECONDS_TO_RUN squashfs-root/AppRun --frontend sdl -B OpenGL /vpk/pokerogue.vpk > /shots/vita3k.log 2>&1) &
    EMU=\$!
    for i in \$(seq 15 15 $SECONDS_TO_RUN); do sleep 15; xwd -root -silent | convert xwd:- /shots/t\$i.png 2>/dev/null; done
    wait \$EMU" || true
cat "$FS/ux0/data/pokerogue/log.txt" 2>/dev/null || { echo "(no app log)"; grep -E '\|E\||\|C\|' .cache/vita3k-shots/vita3k.log | tail -20; }
