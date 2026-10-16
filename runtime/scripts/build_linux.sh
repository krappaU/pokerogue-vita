#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build the desktop runtime in Docker (output: build-linux/pokerogue-rt).
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -q -t pkr-rt-linux -f docker/linux.Dockerfile docker >/dev/null
# PKR_PROFILE_RATE=1000 scripts/build_linux.sh: a build for detailed profiles (see CMakeLists.txt).
docker run --rm -e PKR_COMMIT="$(scripts/build_id.sh)" -e PKR_PROFILE_RATE="${PKR_PROFILE_RATE:-}" -v "$PWD":/w -w /w pkr-rt-linux bash -c \
  'cmake -S . -B build-linux -DPKR_COMMIT="$PKR_COMMIT" -DPKR_PROFILE_RATE="$PKR_PROFILE_RATE" >/dev/null && cmake --build build-linux -j4 2>&1 | grep -E "error|warning" ; test -x build-linux/pokerogue-rt'
