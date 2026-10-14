#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build vitaGL for the Vita with the same commit and flags as VitaSDK's package, plus:
#   NO_SPLASHSCREEN  no vitaGL boot logo
#   HAVE_SHARK_LOG   keep the shader compiler's messages (glGetShaderInfoLog)
#   patches/vitagl-*.patch (fail glLinkProgram instead of crashing when a shader did not compile;
#   optional dump of translated shader sources for debugging; GLSL lessThan/lessThanEqual were
#   swapped)
# Output: third_party/vita-vitagl/{include,lib}
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f third_party/vita-vitagl/lib/libvitaGL.a ] && exit 0
COMMIT=2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8   # VitaSDK's vitaGL package, 2026
mkdir -p third_party/src
if [ ! -d third_party/src/vitaGL ]; then
  git clone -q https://github.com/Rinnegatamante/vitaGL third_party/src/vitaGL
  git -C third_party/src/vitaGL -c advice.detachedHead=false checkout -q $COMMIT
fi
git -C third_party/src/vitaGL checkout -q -- .
for p in patches/vitagl-*.patch; do git -C third_party/src/vitaGL apply "$PWD/$p"; done
docker run --rm -v "$PWD":/w -w /w/third_party/src/vitaGL vitasdk/vitasdk:latest sh -c \
  'make clean >/dev/null 2>&1; make NO_DEBUG=1 NO_SPLASHSCREEN=1 HAVE_SHARK_LOG=1 -j4 2>&1 | grep -E " error" ; true'
mkdir -p third_party/vita-vitagl/lib third_party/vita-vitagl/include
cp third_party/src/vitaGL/libvitaGL.a third_party/vita-vitagl/lib/
cp third_party/src/vitaGL/source/vitaGL.h third_party/vita-vitagl/include/
ls -la third_party/vita-vitagl/lib
