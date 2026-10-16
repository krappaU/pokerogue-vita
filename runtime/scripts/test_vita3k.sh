#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Run test pages (tests/<name>/, default: all) in Vita3K with build-vita/pokerogue-<VERSION>.vpk,
# headless in the pkrb-vita3k image. With your own .cache/private/libshacccg.suprx the pages run on
# the emulated Vita; without it only the boot is checked (tests/run_vita3k_page.sh).
# Logs and screenshots in .cache/test-vita3k/<name>/. Build first (build_vita.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
tests/fetch_test_deps.sh >/dev/null
scripts/get_vita3k.sh
docker build -q -t pkrb-vita3k -f docker/vita3k.Dockerfile docker >/dev/null
TESTS=("$@")
if [ ${#TESTS[@]} -eq 0 ]; then
  for d in tests/*/; do TESTS+=("$(basename "$d")"); done
fi
docker run --rm --init -v "$PWD":/w -w /w pkrb-vita3k bash -c \
  'rc=0; for t in "$@"; do tests/run_vita3k_page.sh "$t" || rc=1; done; exit $rc' _ "${TESTS[@]}"
