#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# PokeRogue's own shaders (AGPL-3.0-only, src/pipelines/glsl of a pinned commit, hash-checked) into
# third_party/pokerogue/ (not committed): tests/shaders compares js/webgl.js's rewrites with them,
# and the dev build's shader benchmark (bench/) draws them.
set -euo pipefail
cd "$(dirname "$0")/.."
POKEROGUE=0816f506d12b91b3f62ef6d840954fefb921685a # 2026-10-20
mkdir -p third_party/pokerogue
while read -r sum name; do
  f=third_party/pokerogue/$name
  [ -f "$f" ] || curl -sfL "https://raw.githubusercontent.com/pagefaultgames/pokerogue/$POKEROGUE/src/pipelines/glsl/$name" -o "$f"
  echo "$sum  $f" | sha256sum -c --quiet
done <<'LIST'
f5c93b669da3a2366525f9909f735b79a4c8402b7b60b7b5f7747e83532c9f16 sprite-frag-shader.frag
b16ec24efe382b3e27817533976b20828ab1cd4d8d813f308135855be78e4613 field-sprite-frag-shader.frag
e04bee4512705fcea568017b5ee0b9ab5fbb8b98be941a7375cb6b636a7e8f7f sprite-shader.vert
LIST
