#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Test-only files into third_party/test/ (not committed), served next to the test pages by
# tests/run_page.sh and tests/run_vita3k_page.sh. Each is checked against a known hash.
# - Phaser 3.90.0 (MIT), the version PokeRogue uses: tests/phaser/ runs the native fast paths against
#   Phaser's own code. ES module build, from the npm registry.
# - PokeRogue's own shaders (scripts/fetch_pokerogue_shaders.sh): tests/shaders/ compares them with
#   js/webgl.js's rewrites, pixel for pixel.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party/test

PHASER=3.90.0
PHASER_SHA256=c4e3a8da498ab94aa8859c7a32cb89140d94bfd95d769bee36a98cdef1bea6e6
if [ ! -f third_party/test/phaser.js ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  curl -sfL "https://registry.npmjs.org/phaser/-/phaser-$PHASER.tgz" -o "$tmp/phaser.tgz"
  echo "$PHASER_SHA256  $tmp/phaser.tgz" | sha256sum -c --quiet
  tar -xzf "$tmp/phaser.tgz" -C "$tmp" package/dist/phaser.esm.min.js
  mv "$tmp/package/dist/phaser.esm.min.js" third_party/test/phaser.js
fi

# PokeRogue's shaders: the same pinned copies the dev build's benchmark uses.
scripts/fetch_pokerogue_shaders.sh
mkdir -p third_party/test/pokerogue
cp third_party/pokerogue/* third_party/test/pokerogue/
ls -la third_party/test third_party/test/pokerogue
