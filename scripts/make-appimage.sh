#!/usr/bin/env bash
# The Linux game as one AppImage: scripts/make-appimage.sh [BUILD_DIR] [OUT.AppImage]
# (defaults build-daytona, build-daytona/Daytona-x86_64.AppImage). Run after
# scripts/docker-build.sh; Docker only, nothing installed on the host.
#
# The image holds the game, m2texcap and the AI textures kit (hires/), and the
# translations (lang/). The game looks for roms/, cars/ and textures/ beside the
# .AppImage file (the image itself is read-only), then as before. It is built
# from your own ROM set: never share it (rules.md 22).
#
# The game links only the C and C++ libraries (SDL 3 is static, loading X11,
# Wayland, audio and Vulkan from the system), so nothing else is bundled; built
# on Ubuntu 24.04, it runs there and on everything newer.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=$(realpath "${1:-$repo/build-daytona}")
out=$(realpath -m "${2:-$build/Daytona-x86_64.AppImage}")

# the packing tools, pinned by version and SHA-256
tools="$build/appimage-tools"
mkdir -p "$tools"
fetch() { # URL FILE SHA256
    if [[ ! -f "$2" ]] || ! echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --retry 3 -o "$2.part" "$1"
        echo "$3  $2.part" | sha256sum -c --quiet -
        mv "$2.part" "$2"
    fi
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
    "$tools/appimagetool" ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
    "$tools/runtime-x86_64" 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
chmod +x "$tools/appimagetool"

# the AppDir
appdir="$build/AppDir"
rm -rf "${appdir:?}"
mkdir -p "$appdir/usr/bin"
for f in daytona m2texcap; do [[ -f "$build/$f" ]] && cp "$build/$f" "$appdir/usr/bin/"; done
for d in lang hires; do [[ -d "$build/$d" ]] && cp -r "$build/$d" "$appdir/usr/bin/"; done
[[ -x "$appdir/usr/bin/daytona" ]] || { echo "no game in $build: run scripts/docker-build.sh first" >&2; exit 1; }
cp "$repo/platform/linux/daytona.desktop" "$repo/platform/linux/daytona.png" "$appdir/"
cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
# the game, with the working folder beside the .AppImage (roms/, cars/, textures/ are there)
here=$(dirname "$(readlink -f "$0")")
[ -n "$APPIMAGE" ] && cd "$(dirname "$APPIMAGE")"
exec "$here/usr/bin/daytona" "$@"
EOF
chmod +x "$appdir/AppRun"

# packed in a container (appimagetool is itself an AppImage: extracted, not mounted, no FUSE needed)
docker build -q -t daytona-appimage "$repo/docker/appimage" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e APPIMAGE_EXTRACT_AND_RUN=1 -e ARCH=x86_64 \
    -v "$build:/b" -v "$(dirname "$out"):/o" -w /tmp daytona-appimage \
    /b/appimage-tools/appimagetool --runtime-file /b/appimage-tools/runtime-x86_64 -n /b/AppDir "/o/$(basename "$out")"
rm -rf "${appdir:?}"
ls -la "$out"
