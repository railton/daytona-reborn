# High-resolution textures (experimental)

The game's textures, upscaled 4x by AI models, drawn in place of the
originals where they are magnified on screen. The pack is made on your
computer from your own ROM set and stays there: it is made of the game's
textures, so it is never shared nor committed (rules.md 6, 22).

The race only: the select screens, the HUD and the screens after a race are
drawn by the 2D tilemap layers, which the pack does not cover (a session
through every screen finds 410 textures, 4 more than the attract loop and a
race on each course).

## How it looks in the game

- A texture is found by the hash of its texels at its place in texture RAM,
  checked each time texture RAM changes (2 to 5 ms); one the pack misses is
  drawn as before. With no pack the game is unchanged.
- The upscaled tones go through the game's own luma and colour tables:
  lighting, palettes and the cars' colours stay the game's.
- Used at the finest level of detail only, where the texture is magnified;
  further away the game's own mip levels are drawn.
- Cut-out textures (trees, crowds, signs, the cars' numbers and decals) keep
  an upscaled opacity: crisp outlines, smooth with MSAA.
- Measured at 3840 x 2160, MSAA 4x, the attract race (3600 frames), with the
  pack of 406 textures (87 MB): 294 in use, 15 to 24 % of each frame's
  pixels changed, the renderer's CPU time per frame 0.16 -> 0.30 ms.

## The models

Each texture is upscaled by the model best at its kind
(`scripts/hires_classes.csv` lists every texture the game's sessions use;
another is sorted by a rule on its tones):

| Kind | Model | Author, licence |
| --- | --- | --- |
| Graphic: lettering, numbers, logos, LED panels, stripes, flat areas, and every cut-out's opacity | [4xTextureDAT2_otf](https://openmodeldb.info/models/4x-TextureDAT2-otf) | Philip Hofmann (Phhofm), CC BY 4.0 |
| Organic: rock, foliage, bark, cobbles, concrete, crowds | [4x-PBRify_UpscalerV4](https://openmodeldb.info/models/4x-PBRify-UpscalerV4) | Kim2091, CC0 1.0 |
| Road: asphalt, lane lines, edges | [RealESRGAN_x4plus](https://github.com/xinntao/Real-ESRGAN) | Xintao Wang et al., BSD-3-Clause |

The models are not part of this repository: `scripts/hires_textures.py
models` downloads them and checks each one's SHA-256. They run on PyTorch
through [spandrel](https://github.com/chaiNNer-org/spandrel) (MIT).

## Making a pack

You need the game built (`m2texcap` is built with it), Python 3, and
either Docker (Linux) or Python with PyTorch, spandrel, safetensors, numpy
and Pillow. A graphics card with CUDA makes the upscaling fast (under 2
minutes on a GeForce RTX 5090); the CPU works too, much slower.

1. Capture the textures the game uses (about 75 s; the folder holds the
   game's textures):

       build-daytona/m2texcap build-daytona/rom_cache/daytona CAPTURE \
         --attract 9000 --course 1 4000 --course 2 4000 --course 3 4000 \
         --inputs scripts/inputs/race_to_end.txt 20000

2. Download the models, upscale and write the pack. With Docker (Linux,
   NVIDIA graphics card):

       docker build -t daytona-hires docker/hires-textures
       docker run --rm --gpus all -u $(id -u):$(id -g) -e HOME=/tmp \
         -v "$PWD:/src:ro" -v CAPTURE:/capture -v MODELS:/models -v OUT:/out \
         -w /src daytona-hires \
         python3 scripts/hires_textures.py all /capture /models /out/hires.m2tex

   Without Docker: `python3 scripts/hires_textures.py all CAPTURE MODELS
   hires.m2tex`.

3. Start the game with the pack, the renderer on the graphics card:

       M2_HIRES_TEXTURES=hires.m2tex build-daytona/daytona

   The log says how many textures the pack holds and how many are in use.
   `m2gpushot --hires-textures hires.m2tex` draws headless frames with it.

## The pack file

Little endian: `M2HT`, version 1, the number of entries, the scale (4); per
entry: sheet, x, y, width, height (texels), flags (1: cut-out), the hash
(FNV-1a over the size and the texels), the data offset; then the data: each
entry's tones (width x 4 by height x 4, 0..255 for texels 0..15), and a
cut-out's opacities after them (`src/app/gpu/hires_textures.h`).
