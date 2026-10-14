# SPDX-License-Identifier: AGPL-3.0-only
# Desktop (Linux) build + headless run environment for the runtime: SDL2, OpenGL ES 2 via Mesa (llvmpipe),
# libcurl, and a virtual X server for running without a display.
FROM debian:bookworm
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential cmake pkg-config git ca-certificates \
      libsdl2-dev libcurl4-openssl-dev libgles2-mesa-dev libegl1-mesa-dev \
      xvfb xauth >/dev/null \
 && rm -rf /var/lib/apt/lists/*
