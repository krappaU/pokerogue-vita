#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Run the test pages (tests/<name>/, default: all) against build-linux/pokerogue-rt, headless in the
# pkr-rt-linux image. Logs and screenshots in .cache/test/<name>/. Build first (build_linux.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
tests/fetch_test_deps.sh >/dev/null
docker build -q -t pkr-rt-linux -f docker/linux.Dockerfile docker >/dev/null
TESTS=("$@")
if [ ${#TESTS[@]} -eq 0 ]; then
  for d in tests/*/; do TESTS+=("$(basename "$d")"); done
fi
docker run --rm --init -v "$PWD":/w -w /w pkr-rt-linux bash -c \
  'rc=0; for t in "$@"; do tests/run_page.sh "$t" || rc=1; done; exit $rc' _ "${TESTS[@]}"
