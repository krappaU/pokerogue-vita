#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Run the desktop runtime headless (virtual X display) in Docker against the live game.
# Extra arguments go to pokerogue-rt (e.g. --frames 600, --control .cache/control.txt).
# Data (HTTP cache, localStorage, log.txt) is kept in .cache/data.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .cache/data
# One instance per container name (PKR_CONTAINER, default pkr-rt; a second checkout can use its
# own), replacing any previous run: stopping the docker client with timeout/Ctrl-C does not stop
# the container.
NAME=${PKR_CONTAINER:-pkr-rt}
docker rm -f "$NAME" >/dev/null 2>&1 || true
exec docker run --rm --init --name "$NAME" ${DOCKER_ARGS:-} -v "$PWD":/w -w /w pkr-rt-linux \
  xvfb-run -a -s "-screen 0 960x544x24" build-linux/pokerogue-rt --js js --data .cache/data "$@"
