# Building the game (Windows and Linux)

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
- **Git**: Windows `winget install Git.Git` (then open a new PowerShell);
  Linux `sudo apt install git` (or your distribution's equivalent).
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

Every command below runs from this project folder.

## Put your ROM set in place

Make a `roms` folder in the project and copy your set into it, named exactly
after the set: `roms/daytona.zip` (or `roms/daytona.7z`), and optionally
`roms/daytona93.zip`. Do this before building: without a ROM set only the
tools are built. Git ignores `roms/`, so it is never uploaded.

Windows (PowerShell):

    New-Item -ItemType Directory -Path roms -Force
    Copy-Item C:\path\to\daytona.zip roms\daytona.zip

Linux:

    mkdir -p roms
    cp /path/to/daytona.zip roms/daytona.zip

On Windows, check the file is not `daytona.zip.zip` (Explorer can hide the
extension).

## Windows

The project's supported Windows path, Windows 10 or 11.

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

## Linux without Docker

    ./setup.sh

It installs the compiler and libraries with your package manager (apt, dnf,
pacman or zypper; it asks for your `sudo` password), then runs the same
`scripts/setup.py`. The game is `build-daytona/daytona`. Details and fixes:
[getting-started.md](getting-started.md).

## First run

The Revision A game opens on its **home menu**: Play, the three courses,
game mode and gearbox, **Settings**, **Quit** and **Online**.

1. The first time, open **Settings**. On the **Game** tab, click
   **Browse...** and choose your `roms/daytona.zip` (or type its path and
   press Enter), and wait for the line under it to say all files are
   verified. **Language**, at the top right, switches the interface between
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

(`build-daytona\Release\daytona.exe --profile 2` on Windows.) A profile is a
separate settings and saves folder, so choose the ROM set once in that
copy's Settings too. Settings live in `~/.local/share/daytona-recomp/` on
Linux and `%APPDATA%\daytona-recomp\` on Windows (`daytona-pc-solo` for the
normal copy, `daytona-2` for `--profile 2`); delete a folder to reset it.

## Never share builds

Do not send the game, its `.exe`, or the `build`, `build-daytona` or `roms`
folders to anyone, or upload them anywhere: they contain code generated from
your ROM set, and the ROMs themselves. Share only the link to the code
(and this guide). Each player builds the game from their own ROM set.
