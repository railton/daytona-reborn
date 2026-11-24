# Cross-compiles the Windows build on Linux with llvm-mingw (Clang, lld and
# the MinGW-w64 headers and libraries, targeting x86_64-w64-mingw32): the same
# toolchain scripts/windows-build.ps1 uses on Windows. Built by
# scripts/docker-build-windows.sh.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build python3 git ca-certificates curl xz-utils \
    && rm -rf /var/lib/apt/lists/*
ARG LLVM_MINGW=llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64
ARG LLVM_MINGW_SHA256=e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701
RUN curl -fsSL -o /tmp/llvm-mingw.tar.xz \
        https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/${LLVM_MINGW}.tar.xz \
    && echo "${LLVM_MINGW_SHA256}  /tmp/llvm-mingw.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/llvm-mingw.tar.xz -C /opt && mv /opt/${LLVM_MINGW} /opt/llvm-mingw && rm /tmp/llvm-mingw.tar.xz
ENV PATH=/opt/llvm-mingw/bin:$PATH
RUN git config --system --add safe.directory '*'
WORKDIR /src
