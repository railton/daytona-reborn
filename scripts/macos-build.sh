#!/bin/bash
# Builds the game on macOS (Apple Silicon or Intel) with Apple's command line
# tools and portable CMake and Ninja: no Homebrew, no administrator rights.
# Start it with "Build Daytona.command" (double-click) in the project folder,
# or:
#
#   bash scripts/macos-build.sh [--update] [--no-shortcut]
#
# 1. Checks Apple's command line tools (compiler, SDK, git, python3): if they
#    are missing, macOS shows its own dialog to install them; run the build
#    again when it finishes.
# 2. Downloads into .tools/ (git-ignored), each file checked against its
#    pinned SHA-256: CMake and Ninja. Later runs reuse them.
# 3. Asks for your ROM set (Revision A, MAME's daytona) the first time and
#    copies it to roms/daytona.zip (or .7z).
# 4. Fetches the pinned libraries (scripts/setup.py), recompiles the game's
#    code from your ROM set and builds it in build-daytona/
#    (scripts/recompile.py). 10 to 40 minutes the first time.
# 5. Puts a "Daytona (recomp)" app on the desktop that starts the game.
#
# --update: in a git clone, fetches the latest source first (git pull).
# Everything is logged to macos-build.log. The build holds code derived from
# your ROM set: it is for you only, never share it (rules.md).

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 1
exec > >(tee -a "$root/macos-build.log") 2>&1

update=0
shortcut=1
for a in "$@"; do
    case "$a" in
    --update) update=1 ;;
    --no-shortcut) shortcut=0 ;;
    *) echo "macos-build: unknown option $a (--update, --no-shortcut)"; exit 2 ;;
    esac
done

say() { printf '\n== %s\n' "$1"; }
fail() {
    printf '\nBuild stopped: %s\nThe whole log is in macos-build.log.\n' "$1"
    exit 1
}

[ "$(uname -s)" = Darwin ] || fail "this script is for macOS (Linux: scripts/docker-build.sh; Windows: Build Daytona.bat)."

say "Apple's command line tools"
if ! xcode-select -p >/dev/null 2>&1; then
    echo "They are not installed: macOS opens a window to install them (free, from Apple)."
    xcode-select --install >/dev/null 2>&1 || true
    fail "install the command line tools, then run \"Build Daytona.command\" again."
fi
for tool in clang++ git python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is missing from the command line tools; reinstall them (xcode-select --install)."
done
echo "   $(xcode-select -p), $(python3 --version 2>&1)"

# The tools, pinned (update URL and SHA-256 together):
#   name | folder | url | sha256 | folder inside the archive | where its executables are
tools_dir="$root/.tools"
downloads="$tools_dir/downloads"
tools=(
    "CMake 4.5.1|cmake|https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-macos-universal.tar.gz|e1fb288543e4eb2a410e385ecebc64323bda51f0205782f2b094ccd5d572ca2d|cmake-4.5.1-macos-universal|CMake.app/Contents/bin"
    "Ninja 1.13.2|ninja|https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip|c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b||"
)

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

get_tool() {
    IFS='|' read -r name dir url sha inner bin <<<"$1"
    local dest="$tools_dir/$dir" stamp="$tools_dir/$dir/.sha256"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$sha" ]; then
        echo "   $name: ready"
    else
        mkdir -p "$downloads"
        local archive
        archive="$downloads/$(basename "$url")"
        if [ ! -f "$archive" ] || [ "$(sha256 "$archive")" != "$sha" ]; then
            echo "   $name: downloading..."
            curl -fL --retry 3 -o "$archive" "$url" || fail "$name: the download failed (internet connection?)."
            local got
            got="$(sha256 "$archive")"
            if [ "$got" != "$sha" ]; then
                rm -f "$archive"
                fail "$name: the download's SHA-256 is $got, expected $sha. Nothing was installed; run the build again."
            fi
        fi
        echo "   $name: unpacking..."
        rm -rf "$dest" "$tools_dir/unpack-$dir"
        mkdir -p "$tools_dir/unpack-$dir"
        case "$archive" in
        *.zip) unzip -q "$archive" -d "$tools_dir/unpack-$dir" ;;
        *) tar -xzf "$archive" -C "$tools_dir/unpack-$dir" ;;
        esac || fail "$name: could not unpack $archive."
        if [ -n "$inner" ]; then
            mv "$tools_dir/unpack-$dir/$inner" "$dest"
            rm -rf "$tools_dir/unpack-$dir"
        else
            mv "$tools_dir/unpack-$dir" "$dest"
        fi
        echo "$sha" >"$stamp"
    fi
    PATH="$dest${bin:+/$bin}:$PATH"
}

say "Tools (portable, in .tools/)"
for t in "${tools[@]}"; do get_tool "$t"; done
chmod +x "$tools_dir/ninja/ninja" 2>/dev/null
export PATH

if [ "$update" = 1 ]; then
    say "Fetching the latest source"
    if [ -d "$root/.git" ]; then
        git pull --ff-only || fail "git pull failed (local changes?). The build was not started."
    else
        echo "This folder is not a git clone: download the new release instead."
    fi
fi

say "Your ROM set"
mkdir -p "$root/roms"
have=""
for f in daytona.zip daytona.7z; do [ -f "$root/roms/$f" ] && have="$f" && break; done
if [ -z "$have" ]; then
    echo "Choose your Daytona USA ROM set: Revision A, 1994 (MAME's \"daytona\", a .zip or .7z)."
    chosen="$(osascript -e 'POSIX path of (choose file with prompt "Daytona USA ROM set (Revision A, MAME daytona): a .zip or .7z" of type {"zip", "7z", "public.zip-archive", "org.7-zip.7-zip-archive"})' 2>/dev/null)"
    [ -n "$chosen" ] || fail "no ROM set chosen: the game cannot be built without it."
    ext="${chosen##*.}"
    ext="$(printf '%s' "$ext" | tr '[:upper:]' '[:lower:]')"
    cp "$chosen" "$root/roms/daytona.$ext" || fail "could not copy $chosen."
    echo "   copied to roms/daytona.$ext"
else
    echo "   roms/$have"
fi

say "Libraries (pinned, into extern/)"
python3 scripts/setup.py --no-build || fail "fetching the libraries failed (see above)."

build_dir=build-daytona
say "Recompiling the game from your ROM set and building it (10 to 40 minutes the first time)"
if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DM2_ROMSET=daytona ||
        fail "CMake could not configure the build (see above)."
fi
python3 scripts/recompile.py --set daytona --build-dir "$build_dir"
status=$?
[ "$status" = 3 ] && fail "your ROM set was rejected: the \"m2import:\" line above names the file that is missing or wrong. It must be MAME's \"daytona\" (Revision A, 1994)."
[ "$status" = 0 ] || fail "the build failed (the errors are above, and in macos-build.log)."

game="$root/$build_dir/daytona"
[ -x "$game" ] || fail "the build finished without $game."

if [ "$shortcut" = 1 ]; then
    # A small app on the desktop that starts the game where it was built
    # (run this again if the project folder moves).
    app="$HOME/Desktop/Daytona (recomp).app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS"
    cat >"$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>start</string>
    <key>CFBundleIdentifier</key><string>local.daytona-recomp.start</string>
    <key>CFBundleName</key><string>Daytona (recomp)</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
    printf '#!/bin/sh\nexec "%s" "$@"\n' "$game" >"$app/Contents/MacOS/start"
    chmod +x "$app/Contents/MacOS/start"
fi

say "Done"
echo "The game: $game"
[ "$shortcut" = 1 ] && echo "A \"Daytona (recomp)\" app is on your desktop."
echo "To update later: run \"Build Daytona.command\" again (with --update in a git clone)."
