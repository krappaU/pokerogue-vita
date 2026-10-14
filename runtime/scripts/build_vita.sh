#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build the Vita .vpk in the official VitaSDK Docker image (output: build-vita/pokerogue.vpk).
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build_vita_curl.sh >/dev/null
docker run --rm -v "$PWD":/w -w /w vitasdk/vitasdk:latest sh -c '
  cmake -S . -B build-vita -DVITA=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null &&
  cmake --build build-vita -j4 2>&1 | grep -E "error|undefined reference|warning: implicit" | sort -u | head -60; true'
ls -la build-vita/*.vpk
