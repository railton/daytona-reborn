#!/bin/sh
# Build the Linux game in Docker: nothing but Docker is needed on the host.
#
#   scripts/docker-build.sh [--no-appimage] [setup.py options]
#
# Builds the build image (docker/linux-build, tag daytona-builder) the first
# time, then runs python3 scripts/setup.py in it with the project folder
# mounted at /src, as your own user, so every file it writes is yours. Put
# your ROM set at roms/daytona.zip (Revision A) and/or roms/daytona93.zip
# first. The game is then also packed as build-daytona/Daytona-x86_64.AppImage
# (scripts/make-appimage.sh: one file that runs on Ubuntu 24.04 and newer, with
# roms/, cars/ and textures/ beside it); --no-appimage leaves it out. To rebuild
# the image (e.g. after the Dockerfile changes):
#   docker rmi daytona-builder
# See docs/BUILDING.md.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
IMAGE=daytona-builder

# --no-appimage is ours; every other option goes to setup.py
APPIMAGE=1
for arg do
    shift
    if [ "$arg" = "--no-appimage" ]; then APPIMAGE=""; else set -- "$@" "$arg"; fi
done

if ! command -v docker >/dev/null 2>&1; then
    echo "docker-build: docker not found; install Docker first (https://docs.docker.com/engine/install/)" >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== Building the $IMAGE image (once)"
    docker build -t "$IMAGE" docker/linux-build
fi

# Run as the calling user (the one behind sudo, if run with sudo).
USER_ID=$(id -u)
GROUP_ID=$(id -g)
if [ "$USER_ID" -eq 0 ] && [ -n "${SUDO_UID:-}" ]; then
    USER_ID=$SUDO_UID
    GROUP_ID=${SUDO_GID:-$SUDO_UID}
fi

# A terminal: -it, so Ctrl-C stops the build.
TTY=""
[ -t 0 ] && [ -t 1 ] && TTY="-it"

# :z lets the container use the folder on SELinux systems (Fedora); ignored elsewhere.
# shellcheck disable=SC2086
docker run --rm $TTY --init \
    -u "$USER_ID:$GROUP_ID" -e HOME=/tmp \
    -v "$ROOT:/src:z" -w /src \
    "$IMAGE" python3 scripts/setup.py "$@"

# the AppImage of the Revision A build; a failure here leaves the game itself built
if [ -n "$APPIMAGE" ] && [ -x build-daytona/daytona ]; then
    echo "== Packing the AppImage"
    if ! bash scripts/make-appimage.sh build-daytona build-daytona/Daytona-x86_64.AppImage; then
        echo "docker-build: the AppImage could not be made (the game is built; scripts/make-appimage.sh to try again)" >&2
    fi
fi

found=""
for exe in build-daytona/Daytona-x86_64.AppImage build-daytona/daytona build/daytona; do
    if [ -x "$exe" ]; then
        [ -z "$found" ] && printf '\n== The game is built. Start it from %s with:\n\n' "$ROOT"
        printf '    %s\n' "$exe"
        found=1
    fi
done
if [ -z "$found" ]; then
    printf '\n== No game was built: put your ROM set at roms/daytona.zip (or roms/daytona93.zip) and run %s again.\n' "$0"
fi
