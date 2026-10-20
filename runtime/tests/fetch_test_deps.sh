#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Test-only files into third_party/test/ (not committed), served next to the test pages by
# tests/run_page.sh and tests/run_vita3k_page.sh. Each is checked against a known hash.
# - Phaser 3.90.0 (MIT), the version PokeRogue uses: tests/phaser/ runs the native fast paths against
#   Phaser's own code. ES module build, from the npm registry.
# - PokeRogue's own shaders (AGPL-3.0-only, src/pipelines/glsl of a pinned commit): tests/shaders/
#   compares them with js/webgl.js's rewrites, pixel for pixel.
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

POKEROGUE=0816f506d12b91b3f62ef6d840954fefb921685a # 2026-10-20
mkdir -p third_party/test/pokerogue
while read -r sum name; do
  f=third_party/test/pokerogue/$name
  [ -f "$f" ] || curl -sfL "https://raw.githubusercontent.com/pagefaultgames/pokerogue/$POKEROGUE/src/pipelines/glsl/$name" -o "$f"
  echo "$sum  $f" | sha256sum -c --quiet
done <<'EOF'
f5c93b669da3a2366525f9909f735b79a4c8402b7b60b7b5f7747e83532c9f16 sprite-frag-shader.frag
b16ec24efe382b3e27817533976b20828ab1cd4d8d813f308135855be78e4613 field-sprite-frag-shader.frag
e04bee4512705fcea568017b5ee0b9ab5fbb8b98be941a7375cb6b636a7e8f7f sprite-shader.vert
EOF
ls -la third_party/test third_party/test/pokerogue
