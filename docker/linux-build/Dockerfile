# Build environment for the Linux game: scripts/docker-build.sh builds this
# image (tag daytona-builder) and runs scripts/setup.py in it, with the
# project folder mounted at /src. Nothing but Docker is needed on the host.
#
# Why Ubuntu 24.04: a Linux program only runs on systems whose C library
# (glibc) and C++ library (libstdc++) are at least as new as the ones it was
# built against. 24.04 ships older ones than the current desktop distributions,
# so the game built here runs on 24.04 and on everything newer (Ubuntu 25.x
# and 26.04, current Fedora, Arch, ...). SDL 3 is built statically with the
# game and loads X11, Wayland, ALSA, PulseAudio/PipeWire and Vulkan from the
# host at run time, so the binary itself links only libc and libstdc++.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# The packages setup.sh installs on Debian/Ubuntu, minus the ones only needed
# to run the game (libvulkan1, mesa-vulkan-drivers, zenity): the toolchain,
# and the development headers SDL 3 is built against (window, input, audio).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build python3 python3-pip git clang pkg-config ca-certificates \
        libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
        libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libegl-dev \
        libwayland-dev libdecor-0-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*

# The project folder is mounted from the host and owned by the host user, not
# by the user the container runs as: let git work in it (and in extern/).
RUN git config --system --add safe.directory '*'

WORKDIR /src
