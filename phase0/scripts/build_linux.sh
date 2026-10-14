#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build and run the desktop Linux benchmark (plus the same benchmark under Node/V8 for reference).
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -q -t pkrb-linux -f docker/linux.Dockerfile docker >/dev/null
ln -sfn ../js data/js
docker run --rm -v "$PWD":/w -w /w pkrb-linux bash -c '
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release >/dev/null &&
  cmake --build build-linux -j4 2>&1 | grep -E "error" || true
  ./build-linux/pokerogue_bench'
