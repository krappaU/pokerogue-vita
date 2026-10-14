#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Build libcurl for the Vita against VitaSDK's current OpenSSL 1.1. VitaSDK's prebuilt libcurl was
# compiled against OpenSSL 1.0 structure layouts and crashes in TLS code. Same flags as VitaSDK's
# own package recipe, plus: curl's own inet_pton (the Vita's accepts host names such as
# "pokerogue.net" as IPv4 addresses, which disables SNI and breaks certificate checks).
# Output: third_party/vita-curl/{include,lib}
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f third_party/vita-curl/lib/libcurl.a ] && exit 0
VER=8.17.0
mkdir -p third_party/src
[ -d third_party/src/curl-$VER ] || curl -sfL "https://github.com/curl/curl/releases/download/curl-${VER//./_}/curl-$VER.tar.gz" | tar -xz -C third_party/src
docker run --rm -v "$PWD":/w -w /w/third_party/src/curl-$VER vitasdk/vitasdk:latest sh -c '
  rm -rf build-vita && mkdir build-vita && cd build-vita &&
  cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_INSTALL_PREFIX=/w/third_party/vita-curl \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DENABLE_IPV6=OFF \
    -DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_FCNTL_O_NONBLOCK=OFF -DENABLE_THREADED_RESOLVER=OFF -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_OPENSSL=ON \
    -DCURL_CA_BUNDLE=app0:cacert.pem -DCURL_DISABLE_LDAP=ON -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF >/dev/null &&
  sed -i "/HAVE_PIPE2/d" lib/curl_config.h &&
  sed -i "/HAVE_INET_PTON/d" lib/curl_config.h &&
  make -j4 2>&1 | grep -E "error|Error" ; make install >/dev/null'
ls -la third_party/vita-curl/lib/
