# High-resolution textures (experimental)

The race's textures, upscaled four times by AI models, drawn in place of
the originals where they are magnified on screen. The pack is made **on
your computer, from your own ROM set**, and stays there. It is made of the
game's textures, so it is never shared nor committed (rules.md 6, 22).

This is a feature **in testing**: it may fail or change. When it fails, the
log it writes (below) is what we need to fix it.

## What you need

### Linux (the supported system for now)

| Tool | Why | How to check |
| --- | --- | --- |
| The game built from source (`docs/BUILDING.md`) | the build puts `m2texcap` (the capture) and `hires/` (the script, the textures' kinds, the container's recipe) next to the game | `ls build-daytona/hires build-daytona/m2texcap` |
| **Docker Engine**, usable by your user (the `docker` group) | the upscaling tools (Python, PyTorch, the models' loader) run in a container, so nothing is installed on your system | `docker version` answers without `sudo` |
| An **NVIDIA graphics card** with its driver | the models run on the card's CUDA cores | `nvidia-smi` lists the card |
| The **NVIDIA Container Toolkit** | lets Docker give the card to the container (`docker run --gpus all`) | `docker info` lists an `nvidia` runtime |
| About **15 GB of free disk** while the pack is made | the container's base image (PyTorch with CUDA, 7.6 GB) and its build; the models (0.4 GB) | |
| An Internet connection, the first time | the base image from Docker Hub, the Python packages and the three models | |

Without an NVIDIA card (or without the toolkit), the **processor** can make
the pack: tick "On the processor" in the menu. It takes about 30 minutes on
20 threads (estimated from a 3 % sample); with an RTX 5090 it took 1 min 57 s
in all, the base image already downloaded.

Installing the pieces (Ubuntu/Debian; see each project's page for others):

    # Docker Engine: https://docs.docker.com/engine/install/
    sudo usermod -aG docker $USER     # then log out and back in
    # NVIDIA Container Toolkit:
    # https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html
    sudo nvidia-ctk runtime configure --runtime=docker
    sudo systemctl restart docker

### Windows and macOS

Not yet. On Windows the plan is the build's way: portable tools (Python and
PyTorch) downloaded into the game's folder, checked by SHA-256, and removed
after the pack is made. Until then, a pack can be made by hand with Python
(see "By hand" below).

## Where the pack lives

In the project's `textures/` folder, beside `roms/` and `cars/` (the game
looks beside its build folder, then in the working folder;
[textures/README.md](../textures/README.md)). At each start:

1. `textures/hires.m2tex` is used when it is a valid pack;
2. else the first `.zip` (or `.7z`) there holding a `.m2tex` file is
   checked, unpacked as `hires.m2tex` and used, and "Use the
   high-resolution textures" is turned on (an 84 MB pack is a 38 MB zip,
   unpacked in under a second); a file that is not a valid pack is left
   out, its reason in the log (`daytona: high-resolution textures: ...`);
3. else a pack made before in the game's data folder is moved there;
4. none: the AI textures tab makes one (below), saved there.

Git ignores everything in `textures/` but its README.

## Making the pack from the game

Settings > **AI textures**:

1. **This computer** shows what was found: the graphics card, Docker, the
   NVIDIA Container Toolkit. "Check again" after installing something.
2. **Make the pack** runs, in the background (Cancel stops it):
   1. the capture: `m2texcap` plays the attract loop, a race on each course
      and a session through the select screens, and writes the textures it
      saw (about 50 s);
   2. the tools: Docker builds the upscaling container (`hires/Dockerfile`,
      tagged `daytona-hires-kit`); the first time it downloads the base
      image;
   3. the upscaling: the container downloads the three models (each checked
      against its SHA-256) and upscales every texture with the model for its
      kind;
   4. the pack is checked and saved as `hires.m2tex` in the project's
      `textures/` folder (beside `roms/` and `cars/`);
   5. after a success only, the tools are removed: the captured textures,
      the models, the `daytona-hires-kit` image, and the base image unless it
      was there before. Docker's build cache may keep some layers:
      `docker builder prune` frees them.
3. **The pack**: where it lives, "Use the high-resolution textures"
   (ticked when a pack is made), its size, "Delete the pack" (a `.zip`
   left in `textures/` is unpacked again at the next start). It needs the Hardware renderer
   (Display tab); a texture the pack does not have is drawn as before.

`daytona --make-hires-pack` does the same without the window (the profile's
ROM set; `--profile NAME` for another), printing the progress.

### When it fails

The menu says which step failed and names the log:
`hires-textures.log` in the game's data folder (the pack itself is in `textures/`) (on Linux
`~/.local/share/daytona-recomp/daytona/`, or `daytona-PROFILE/`). It has
every command run, all their output and the exit codes: send it with the
report. After a failure (or Cancel) the tools are kept, so trying again is
quicker, and so are the textures already upscaled: trying again upscales
only the rest (`capture/out/done.txt` says how each was made: the model, the
scale, the version of the script; one made another way is made again).

| The menu says | Usually |
| --- | --- |
| Docker is not installed | install Docker Engine |
| This user cannot use Docker | `sudo usermod -aG docker $USER`, log in again |
| Docker cannot use the graphics card | install the NVIDIA Container Toolkit, configure it, restart Docker |
| No NVIDIA graphics card answered | the driver: `nvidia-smi` must answer; or use the processor |
| The capture failed | the ROM set (Game tab); the log has `m2texcap`'s message |
| The upscaling tools could not be prepared | disk space or the network (Docker Hub, PyPI) |
| The upscaling failed | the log: a model download, the card's memory, the CUDA driver |

## How it looks in the game

- A texture is found by the hash of its texels at its place in texture RAM,
  checked each time texture RAM changes (2 to 5 ms); one the pack misses is
  drawn as before. With no pack the game is unchanged.
- The upscaled tones go through the game's own luma and colour tables:
  lighting, palettes and the cars' colours stay the game's.
- Used at the finest level of detail only, where the texture is magnified;
  further away the game's own mip levels are drawn.
- Cut-out textures (trees, crowds, signs, the cars' numbers and decals) keep
  an upscaled opacity: crisp outlines, smooth with MSAA. The model can fill a
  mask's scattered holes, so a mask drawing more than 1 point more (or less)
  of itself than the game's (the share at least a half, what a render
  without MSAA draws) has its opacities remapped to the game's share: 4 of
  213 here, the worst a road patch's holes, 3.5 points.
- Measured at 3840 x 2160, MSAA 4x, the attract race (3600 frames), with a
  pack of 406 textures (87 MB): 294 in use, 15 to 24 % of each frame's
  pixels changed, the renderer's CPU time per frame 0.16 -> 0.30 ms.
- The race only: the menus, the HUD and the results are the 2D layers (see
  Settings > Enhancements > Smooth menus and HUD for those).

## The models

Each texture is upscaled by the model best at its kind
(`scripts/hires_classes.csv` lists every texture the game's sessions use,
by hash; another is sorted by a rule on its tones):

| Kind | Model | Author, licence |
| --- | --- | --- |
| Graphic: lettering, numbers, logos, LED panels, stripes, flat areas, and every cut-out's opacity | [4xTextureDAT2_otf](https://openmodeldb.info/models/4x-TextureDAT2-otf) | Philip Hofmann (Phhofm), CC BY 4.0 |
| Organic: rock, foliage, bark, cobbles, concrete, crowds | [4x-PBRify_UpscalerV4](https://openmodeldb.info/models/4x-PBRify-UpscalerV4) | Kim2091, CC0 1.0 |
| Road: asphalt, lane lines, edges | [RealESRGAN_x4plus](https://github.com/xinntao/Real-ESRGAN) | Xintao Wang et al., BSD-3-Clause |

The models are not part of this repository: `scripts/hires_textures.py
models` downloads them and checks each one's SHA-256 (THIRD_PARTY.md). They
run on PyTorch through [spandrel](https://github.com/chaiNNer-org/spandrel)
(MIT).

## By hand

With the game built, Python 3 and either Docker or Python with PyTorch,
spandrel, safetensors, numpy and Pillow:

1. Capture the textures (the folder holds the game's textures):

       build-daytona/m2texcap ROM.zip CAPTURE \
         --attract 9000 --course 1 4000 --course 2 4000 --course 3 4000 \
         --inputs scripts/inputs/race_to_end.txt 20000

   Its last line counts the polygons the pack would miss: a texture is found
   at its first place in texture RAM and in its mode (cut-out or not); one
   drawn from another place, or a cut-out one drawn opaque, goes without.
   Measured: 1,324 of 513,179 polygons (0.26 %, 3 textures), none by the
   mode.

2. Download the models, upscale and write the pack. With Docker:

       docker build -t daytona-hires docker/hires-textures
       docker run --rm --gpus all -u $(id -u):$(id -g) -e HOME=/tmp \
         -v "$PWD:/src:ro" -v CAPTURE:/capture -v MODELS:/models -v OUT:/out \
         -w /src daytona-hires \
         python3 scripts/hires_textures.py all /capture /models /out/hires.m2tex

   Without Docker: `python3 scripts/hires_textures.py all CAPTURE MODELS
   hires.m2tex`.

3. Start the game with it: `M2_HIRES_TEXTURES=hires.m2tex build-daytona/daytona`
   (this one wins over the menu's pack); `m2gpushot --hires-textures
   hires.m2tex` draws headless frames with it.

## The pack file

Little endian: `M2HT`, version 1, the number of entries, the scale (4); per
entry: sheet, x, y, width, height (texels), flags (1: cut-out), the hash
(FNV-1a over the size and the texels), the data offset; then the data: each
entry's tones (width x 4 by height x 4, 0..255 for texels 0..15), and a
cut-out's opacities after them (`src/app/gpu/hires_textures.h`). Widths and
heights are powers of two from 32 to 256; a texture's place holds up to four
textures of different sizes (`tests/test_hires.cpp`).
