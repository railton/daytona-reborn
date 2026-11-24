#!/bin/sh
# Cross-compiles the Windows build (daytona.exe) on Linux, in Docker, with
# llvm-mingw (docker/windows-cross): the same compiler scripts/windows-build.ps1
# uses on Windows. Run scripts/docker-build.sh first: this reuses the game's
# code it recompiled from your ROM set (build-daytona/gen), the same on every
# system. Result: build-windows/daytona.exe and build-windows/lang/; copy them
# to a Windows PC with your ROM set. For you only: never share the build.
set -e
cd "$(dirname "$0")/.."
IMAGE=daytona-windows-cross
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" docker/windows-cross
fi
if [ ! -f build-daytona/gen/daytona/gen_table.cpp ]; then
    echo "docker-build-windows: run scripts/docker-build.sh first (build-daytona/gen is the game recompiled from your ROM set)" >&2
    exit 1
fi
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/src" -w /src "$IMAGE" sh -c '
    [ -f build-windows/CMakeCache.txt ] || cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-cross.cmake -DM2_ROMSET=daytona -DM2_GEN_ROOT=/src/build-daytona/gen
    cmake --build build-windows --target daytona daytona_lang'
echo
echo "Windows build: build-windows/daytona.exe (with build-windows/lang/)"
