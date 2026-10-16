#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Fetch third-party inputs for the Phase 0 benchmark:
#  - QuickJS-ng source (pinned tag)
#  - Phaser 3.90.0 (same version PokeRogue uses)
#  - The current live PokeRogue startup chunks (for the compile-time test)
set -euo pipefail
cd "$(dirname "$0")/.."
QJS_TAG=v0.17.0
PHASER_VER=3.90.0

mkdir -p third_party data/pokerogue
if [ ! -d third_party/quickjs ]; then
  git clone -q --depth 1 --branch "$QJS_TAG" https://github.com/quickjs-ng/quickjs third_party/quickjs
fi
curl -sfL "https://cdn.jsdelivr.net/npm/phaser@${PHASER_VER}/dist/phaser.min.js" -o data/phaser.min.js

UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36'
rm -f data/pokerogue/*.js
curl -sfL -A "$UA" https://pokerogue.net/ -o data/pokerogue/index.html
for f in $(grep -oE '\./assets/[A-Za-z0-9_-]+\.js' data/pokerogue/index.html | sort -u); do
  curl -sfL -A "$UA" "https://pokerogue.net/${f#./}" -o "data/pokerogue/$(basename "$f")"
done
# Static imports of the entry chunk (the __vite__mapDeps list) are preloaded at startup too.
for f in $(grep -ohE '"\./[A-Za-z0-9_-]+\.js"' data/pokerogue/*.js | tr -d '"' | sort -u); do
  [ -f "data/pokerogue/$(basename "$f")" ] || curl -sfL -A "$UA" "https://pokerogue.net/assets/${f#./}" -o "data/pokerogue/$(basename "$f")"
done
ls data/pokerogue/*.js | sed 's#.*/##' > data/pokerogue/files.txt
du -sh data/pokerogue data/phaser.min.js

# On-screen text output for the Vita build (from vitasdk/samples)
mkdir -p third_party/debugScreen
for f in debugScreen.c debugScreen.h debugScreen_custom.h debugScreenFont.c; do
  [ -f third_party/debugScreen/$f ] || curl -sfL "https://raw.githubusercontent.com/vitasdk/samples/master/common/$f" -o third_party/debugScreen/$f
done
