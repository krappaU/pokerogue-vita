#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Run the desktop runtime headless (virtual X display) in Docker against the live game.
# Extra arguments go to pokerogue-rt (e.g. --frames 600, --control .cache/control.txt).
# Data (HTTP cache, localStorage, log.txt) is kept in .cache/data.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .cache/data
exec docker run --rm --init ${DOCKER_ARGS:-} -v "$PWD":/w -w /w pkr-rt-linux \
  xvfb-run -a -s "-screen 0 960x544x24" build-linux/pokerogue-rt --js js --data .cache/data "$@"
