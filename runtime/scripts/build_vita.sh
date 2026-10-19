#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build the Vita .vpk in the official VitaSDK Docker image (output: build-vita/pokerogue-<VERSION>.vpk).
# PKR_DEV=1: the dev build (build-vita-dev/pokerogue-<VERSION>-dev.vpk) and the updater app that
# installs test builds (build-vita-dev/pokerogue-updater.vpk).
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build_vita_curl.sh >/dev/null
./scripts/build_vita_vitagl.sh >/dev/null
if [ "${PKR_DEV:-}" = 1 ]; then dir=build-vita-dev dev=ON suffix=-dev; else dir=build-vita dev=OFF suffix=; fi
docker run --rm -e PKR_COMMIT="$(scripts/build_id.sh)" -e DIR="$dir" -e DEV="$dev" -v "$PWD":/w -w /w vitasdk/vitasdk:latest sh -c '
  cmake -S . -B "$DIR" -DVITA=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DPKR_COMMIT="$PKR_COMMIT" -DPKR_DEV="$DEV" >/dev/null &&
  cmake --build "$DIR" -j4 2>&1 | grep -E "error|undefined reference|warning: implicit" | sort -u | head -60; true'
ls -la "$dir/pokerogue-$(cat VERSION)$suffix.vpk"
if [ "${PKR_DEV:-}" = 1 ]; then ls -la "$dir/pokerogue-updater.vpk"; fi # the updater app (updater/updater.c)
