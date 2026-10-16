# SPDX-License-Identifier: AGPL-3.0-only
# Headless runner for the Vita3K emulator (crash checks only: its speed says nothing about real hardware).
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      xvfb xauth x11-apps imagemagick mesa-vulkan-drivers libvulkan1 libgl1 libegl1 libgtk-3-0 libsdl2-2.0-0 >/dev/null \
 && rm -rf /var/lib/apt/lists/*
