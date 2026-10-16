#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Fetch pinned third-party sources into third_party/ (not committed).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party
clone() { # name url tag
  [ -d "third_party/$1" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "third_party/$1"
}
clone quickjs https://github.com/quickjs-ng/quickjs v0.17.0
# Our patches (patches/quickjs-*.patch): a stack sampler for the built-in profiler.
git -C third_party/quickjs checkout -q -- .
for p in patches/quickjs-*.patch; do git -C third_party/quickjs apply "$PWD/$p"; done
clone plutovg https://github.com/sammycage/plutovg v1.3.3
mkdir -p third_party/stb
STB=f58f558c120e9b32c217290b80bad1a0729fbb2c   # nothings/stb, 2025-05-26 (stb_image 2.30)
for f in stb_image.h stb_image_write.h; do
  [ -f "third_party/stb/$f" ] || curl -sfL "https://raw.githubusercontent.com/nothings/stb/$STB/$f" -o "third_party/stb/$f"
done
ls third_party

# CA certificates for HTTPS on the Vita (Mozilla's bundle as published by curl; MPL 2.0).
[ -f third_party/cacert.pem ] || curl -sfL https://curl.se/ca/cacert-2025-09-09.pem -o third_party/cacert.pem \
  || curl -sfL https://curl.se/ca/cacert.pem -o third_party/cacert.pem

# 8x8 bitmap font for on-screen status messages (public domain, Daniel Hepper)
[ -f third_party/font8x8_basic.h ] || curl -sfL https://raw.githubusercontent.com/dhepper/font8x8/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h -o third_party/font8x8_basic.h

# miniz (MIT): unpacks the offline game's zip on the console.
if [ ! -f third_party/miniz/miniz.c ]; then
  mkdir -p third_party/miniz
  curl -sfL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip -o third_party/miniz/miniz.zip
  (cd third_party/miniz && unzip -oq miniz.zip miniz.c miniz.h && rm miniz.zip)
fi

# Audio decoders: minimp3 (MP3) and minimp4 (MP4/M4A demuxing) by lieff, CC0 / public domain.
# AAC (the game's .m4a cries) is decoded with faad2 (GPL-2.0-or-later): VitaSDK's package on the
# Vita, Debian's libfaad-dev on the desktop build.
MINIMP3=ea99364f61c14656440e8d77e9c233ccf3124633
MINIMP4=2a3014a7753cfcba4442adeb469431967238fe6a
mkdir -p third_party/minimp3
for f in minimp3.h minimp3_ex.h; do
  [ -f "third_party/minimp3/$f" ] || curl -sfL "https://raw.githubusercontent.com/lieff/minimp3/$MINIMP3/$f" -o "third_party/minimp3/$f"
done
[ -f third_party/minimp3/minimp4.h ] || curl -sfL "https://raw.githubusercontent.com/lieff/minimp4/$MINIMP4/minimp4.h" -o third_party/minimp3/minimp4.h

# QR codes for links the game opens (shown on screen to scan): Nayuki's QR-Code-generator, MIT.
QRCODEGEN=3c6d0b3cefb4e049dc337e82237c9644399716a8
mkdir -p third_party/qrcodegen
for f in qrcodegen.c qrcodegen.h; do
  [ -f "third_party/qrcodegen/$f" ] || curl -sfL "https://raw.githubusercontent.com/nayuki/QR-Code-generator/$QRCODEGEN/c/$f" -o "third_party/qrcodegen/$f"
done
