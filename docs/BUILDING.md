# Building the game (Windows, macOS and Linux)

This project contains no game data and there is no ready-made download.
Everyone builds the game on their own computer, from their own ROM set: the
build recompiles the arcade's program to native code. These steps take you
from nothing to the game's home menu, with the **Online** mode.

Official, more detailed guide (macOS too, and every option):
[getting-started.md](getting-started.md).

## What you need

- **Your own ROM set** in MAME format, a `.zip` or `.7z`:
  - **`daytona`** (Daytona USA Revision A, 1994): the one for the PC home
    menu and **Online** play. It contains `epr-16722a.12`, `epr-16723a.13`,
    `epr-16724a.6` and `epr-16725a.7`.
  - **`daytona93`** (Deluxe '93): optional, built as a separate game, without
    Online play.

  Other Daytona sets (`daytonas`, `daytonat`, `daytonase`, ...) are rejected,
  even when renamed.
- **Git** to clone the code (Linux `sudo apt install git`, or your
  distribution's equivalent). On Windows it is optional: download the code
  as a zip instead (below), `Build Daytona.bat` brings its own.
- A 64-bit computer, an internet connection for the first build and about
  10 GB of free disk space. The first build takes from several minutes to
  half an hour or more.

## Get the code

The game with the Online mode is
[railton/daytona-reborn](https://github.com/railton/daytona-reborn):

    git clone https://github.com/railton/daytona-reborn.git
    cd daytona-reborn

Already cloned? From the project folder:

    git pull

Without Git: on the GitHub page, **Code > Download ZIP** (or the source zip
of a release), and unzip it. Every command below runs from this project
folder.

Three ways to build, all from your own ROM set:

| You are on | Use | You install |
| --- | --- | --- |
| Windows 10/11 | [`Build Daytona.bat`](#windows-build-daytonabat-recommended) | nothing (portable tools, no administrator) |
| Linux | [`scripts/docker-build.sh`](#linux-with-docker-recommended-on-linux) | Docker |
| Linux, for your own Windows PC | [`scripts/docker-build-windows.sh`](#windows-build-made-on-linux) | Docker |

Windows with Visual Studio (`setup.ps1`) and Linux without Docker
(`setup.sh`) remain, further down.

## Put your ROM set in place

The project has a `roms` folder: copy your set into it, named exactly after
the set: `roms/daytona.zip` (or `roms/daytona.7z`), and optionally
`roms/daytona93.zip`. Do this before building: without a ROM set only the
tools are built. Git ignores everything in `roms/` but its README, so a ROM
set is never uploaded. `Build Daytona.bat` asks for the file and copies it
there itself.

Linux:

    cp /path/to/daytona.zip roms/daytona.zip

The game finds `roms/daytona.zip` on its own the first time it starts.

On Windows, check the file is not `daytona.zip.zip` (Explorer can hide the
extension).

## Windows: Build Daytona.bat (recommended)

Windows 10 (1803 or later) or 11, 64-bit. Nothing to install, no
administrator rights.

1. **Double-click `Build Daytona.bat`** in the project folder (if Windows
   shows "Windows protected your PC", **More info > Run anyway**: it is a
   script from this project, `scripts\windows-build.ps1`).
2. The first time, it downloads its tools into `.tools\` in the project
   folder, about 330 MB, each file checked against its pinned SHA-256:
   llvm-mingw (Clang for Windows), CMake, Ninja, Python (embeddable) and
   MinGit. They are not installed on Windows: delete the project folder and
   they are gone.
3. If `roms\daytona.zip` is not there yet, a window asks for your ROM set and
   copies it there.
4. It fetches the pinned libraries, recompiles the game from your ROM set
   and builds it: 10 to 40 minutes the first time, much less afterwards.
5. It ends with `== Done`, the game at `build-daytona\daytona.exe`, and a
   **Daytona (recomp)** shortcut on your desktop. The game finds your ROM set
   by itself.

**Updating:** download the new version (or, in a git clone, run
`powershell -ExecutionPolicy Bypass -File scripts\windows-build.ps1 -Update`,
which runs `git pull` first), then double-click `Build Daytona.bat` again.

**If it stops**, it says why in red; everything it printed is in
`windows-build.log` in the project folder. A download whose checksum does
not match is deleted and nothing is unpacked: run it again. To start over,
delete `build-daytona` (and `.tools` to download the tools again).

## macOS: Build Daytona.command (recommended)

macOS on Apple Silicon or Intel. No Homebrew, no administrator rights.

1. **Double-click `Build Daytona.command`** in the project folder. The first
   time macOS may refuse a script that came from the internet: **right-click
   it, Open, then Open again**. A Terminal window shows the build.
2. If Apple's **command line tools** are missing (the compiler, the macOS
   SDK, git and python3), macOS opens its own window to install them, free:
   accept, wait for it to finish, then double-click `Build Daytona.command`
   again.
3. The first time, it downloads CMake and Ninja into `.tools/` in the
   project folder, about 90 MB, each file checked against its pinned
   SHA-256. Nothing is installed on macOS: delete the project folder and
   they are gone.
4. If `roms/daytona.zip` is not there yet, a window asks for your ROM set and
   copies it there.
5. It fetches the pinned libraries, recompiles the game from your ROM set
   and builds it: 10 to 40 minutes the first time, much less afterwards.
6. It ends with `== Done`, the game at `build-daytona/daytona`, and a
   **Daytona (recomp)** app on your desktop that starts it (build again if
   you move the project folder).

**Updating:** download the new version (or, in a git clone, run
`bash scripts/macos-build.sh --update`, which runs `git pull` first), then
double-click `Build Daytona.command` again.

**If it stops**, it says why; everything it printed is in `macos-build.log`
in the project folder. To start over, delete `build-daytona` (and `.tools`
to download the tools again).

## Windows with Visual Studio (setup.ps1)

The original Windows path, Windows 10 or 11, if you prefer Microsoft's
build tools.

1. In PowerShell, in the project folder:

       powershell -ExecutionPolicy Bypass -File setup.ps1

   It installs, with winget, whatever is missing: Git, CMake, Ninja,
   Python 3.12 and the Visual Studio 2022 Build Tools with the C++ workload
   and its Clang tools (a large download). If you already have Visual Studio
   2022, it adds the C++ and Clang tools to it instead: **Windows asks for
   permission** (the Visual Studio Installer needs administrator rights).
   Then it downloads the pinned libraries into `extern/`, builds, recompiles
   the game from your ROM set and runs the tests. The game is built with
   Clang (`Compiler: Clang` in the output).

   To build with Microsoft's compiler instead of Clang:

       powershell -ExecutionPolicy Bypass -File setup.ps1 --msvc

2. It is finished when it prints `== Done` and the command to start the game.
   For Revision A that is normally:

       build-daytona\Release\daytona.exe

   Use the exact path setup printed: it is `build-daytona\daytona.exe` if the
   build used Ninja (only when run from a Visual Studio developer prompt).
   `build\Release\daytona.exe` is Deluxe '93, if you have that set too. The
   `build` folder is always made (it holds the tools and tests), even with
   only `daytona.zip`.

**Updating** (a new version of the branch):

    git pull
    powershell -ExecutionPolicy Bypass -File setup.ps1

Always run setup after pulling: updates can change the recompiled code, and
setup redoes it from your ROM set. It is much quicker after the first time.

**Clean rebuild** (after a failed build, or to start over): delete the
build folders, then run setup again. `roms/` and `extern/` are kept.

    Remove-Item -Recurse -Force build, build-daytona

**Common problems** (details in
[getting-started.md, Troubleshooting](getting-started.md#troubleshooting)):

- *Python "is not on PATH yet"*: close PowerShell, open a new one and run the
  same command again.
- *"The Clang tools for Visual Studio 2022 are not installed"*: in the Visual
  Studio Installer, Modify > Individual components, tick "C++ Clang Compiler
  for Windows" and "MSBuild support for LLVM (clang-cl) toolset", then run
  setup again. Or use `setup.ps1 --msvc`.
- *winget not found*: install "App Installer" from the Microsoft Store.
- *`No ROM set: the tools are built, the game is not`*: the file is not at
  `roms\daytona.zip` under exactly that name. Setup lists any archives it
  found in `roms\` under other names.
- *`m2import: missing ...`* then *`your daytona ROM set ... was rejected`*:
  the file is not the Revision A set, or is incomplete.
- *`your ROM set was accepted, but recompiling or building the game failed`*:
  your ROM is fine. `git pull`, run setup again; if it still fails, do a
  clean rebuild, and report the lines containing `error`.
- The command prompt comes back straight away when the game starts: normal
  for a windowed program. Started from Explorer, the game writes its messages
  to `daytona.log` in its settings folder
  (`%APPDATA%\daytona-recomp\daytona-pc-solo\`).

## Linux with Docker (recommended on Linux)

Nothing is installed on your system except Docker: the compiler and
libraries stay in a container. The game is built in Ubuntu 24.04, so it runs
on that and on any newer distribution.

1. Install Docker Engine
   (<https://docs.docker.com/engine/install/>) and make sure `docker run
   hello-world` works without `sudo` (add yourself to the `docker` group and
   log in again). Running the script with `sudo` also works.
2. With your ROM set in `roms/`, from the project folder:

       ./scripts/docker-build.sh

   The first time it builds the `daytona-builder` image (from
   `docker/linux-build/Dockerfile`), then runs the same build as everyone
   else (`python3 scripts/setup.py`) inside it. Any option is passed on to
   `setup.py`. It ends by printing where the game is.
3. Start the game:

       build-daytona/daytona

   (`build/daytona` is Deluxe '93, if you have that set too.)

The build runs as your own user, so every file in `build/`,
`build-daytona/` and `extern/` belongs to you, not to root: you can delete
them normally.

**What the game needs to run** (a normal desktop already has it): a
graphical session (X11 or Wayland), sound (PipeWire, PulseAudio or ALSA),
and Vulkan drivers for your graphics card; the game draws through Vulkan on
Linux. If it closes at start mentioning the GPU or Vulkan, install them (on
Debian/Ubuntu `sudo apt install libvulkan1 mesa-vulkan-drivers`; NVIDIA's
own driver brings its own). If **Browse...** opens no file window, type the
full path of your ROM set in the box beside it and press Enter (or install
`zenity`).

**Updating:**

    git pull
    ./scripts/docker-build.sh

**Clean rebuild:** `rm -rf build build-daytona`, then run the script again.
To rebuild the image too (after `docker/linux-build/Dockerfile` changes):
`docker rmi daytona-builder`.

Do not mix Docker and native builds in the same folder: the build folders
remember the paths they were configured with (`/src` in the container). To
switch, delete `build` and `build-daytona` first.

## Windows build made on Linux

To test on your own Windows PC (a dual boot, another computer) without
building there: the Windows game is cross-compiled in Docker with the same
compiler `Build Daytona.bat` uses (llvm-mingw, `docker/windows-cross`).

1. Build the Linux game first (`./scripts/docker-build.sh`, above): the
   Windows build reuses the game's code it recompiled from your ROM set
   (`build-daytona/gen/`), which is the same on every system.
2. Then:

       ./scripts/docker-build-windows.sh

   The first time it builds the `daytona-windows-cross` image (about 85 MB
   of llvm-mingw, checked against its SHA-256).
3. Copy `build-windows/daytona.exe` and the `build-windows/lang/` folder to
   the Windows PC, with your ROM set beside them, start `daytona.exe` and
   choose the ROM set in **Settings > Game > Browse...**.

The `.exe` needs nothing else: the C++ runtime is linked in, it only uses
Windows' own DLLs. It is still a build from your ROM set: for your own PC
only (below).

## Linux without Docker

    ./setup.sh

It installs the compiler and libraries with your package manager (apt, dnf,
pacman or zypper; it asks for your `sudo` password), then runs the same
`scripts/setup.py`. The game is `build-daytona/daytona`. Details and fixes:
[getting-started.md](getting-started.md).

## First run

The Revision A game opens on its **home menu**: Play, the three courses,
game mode and gearbox, **Settings**, **Quit** and **Online**.

1. The game finds `roms/daytona.zip` in the project by itself. Otherwise
   (the game copied elsewhere) it opens on **Your ROM set**: click **Choose
   the ROM set...** (or type its full path and press Enter). Nothing else
   opens until the set is verified; a refused set lists the files that are
   missing or wrong. Later, **Settings > Game > Change...** chooses another. **Language**, at the top right, switches the interface between
   English, French and Portuguese (Português). **Back** returns to the home
   menu. The choice is remembered.
2. **Play** starts a solo race. In the game, **Esc** brings the menu back.
3. **Online** (below Settings and Quit) opens the Online screen: create a
   room on a daytona-relay server and send the invite code, or paste a code
   to join a friend's room. If it asks you to choose the ROM set first, do
   step 1. How rooms, invite codes and the server work:
   [server/daytona-relay/README.md](../server/daytona-relay/README.md).

**Two copies on one computer** (to try Online alone):

    build-daytona/daytona --profile 2

(`build-daytona\daytona.exe --profile 2` on Windows; `build-daytona\Release\`
with `setup.ps1`.) A profile is a
separate settings and saves folder, so choose the ROM set once in that
copy's Settings too. Settings live in `~/.local/share/daytona-recomp/` on
Linux and `%APPDATA%\daytona-recomp\` on Windows (`daytona-pc-solo` for the
normal copy, `daytona-2` for `--profile 2`); delete a folder to reset it.

## Never share builds

Do not send the game, its `.exe`, or the `build`, `build-daytona`,
`build-windows` or `roms` folders to anyone, or upload them anywhere: they contain code generated from
your ROM set, and the ROMs themselves. Share only the link to the code
(and this guide). Each player builds the game from their own ROM set.
