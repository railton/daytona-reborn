#!/usr/bin/env python3
"""Make a high-resolution texture pack (src/app/gpu/hires_textures.h) from the
textures tools/m2texcap captured, upscaled 4x by AI models, each texture by
the model best at its kind:

  graphic  lettering, numbers, logos, LED panels, arrows, stripes, gradients
           and flat areas: 4xTextureDAT2_otf (Phhofm, CC BY 4.0)
  organic  rock, foliage, bark, cobbles, concrete, crowds:
           4x-PBRify_UpscalerV4 (Kim2091, CC0)
  road     the asphalt, its lane lines and edges:
           RealESRGAN_x4plus (Xintao Wang et al., BSD-3-Clause)

A texture's kind comes from scripts/hires_classes.csv (its hash, the same for
every copy of the ROM set), else from a rule on its tones (kind_of). A cut-out
texture (texel 15 transparent) has its transparent texels filled from their
neighbours before upscaling, so no model blends the hole's tone into its
edges, and its opacity upscaled by the graphic model, for crisp outlines.

  hires_textures.py models MODELS_DIR                 # download the models (pinned SHA-256)
  hires_textures.py upscale CAPTURE_DIR MODELS_DIR    # CAPTURE_DIR/out: one 4x PNG per texture
  hires_textures.py pack CAPTURE_DIR PACK             # CAPTURE_DIR/out -> PACK
  hires_textures.py all CAPTURE_DIR MODELS_DIR PACK

upscale needs Python 3 with PyTorch, spandrel and Pillow (docker/hires-textures
has them; a CUDA graphics card makes it fast, the CPU works too, slowly).
models and pack need Python 3 (pack with Pillow).

Everything made here is made of the game's textures, from the player's own
ROM set: it stays on their computer, never shared nor committed (rules.md 6,
22). The game uses a pack with M2_HIRES_TEXTURES=PACK; m2gpushot with
--hires-textures PACK.
"""

import csv
import hashlib
import os
import struct
import sys
import time
import urllib.request

SCALE = 4  # m2.hlsl kHiresScale
PAD = 8    # texels wrapped around a texture before upscaling: textures repeat

MODELS = {
    "graphic": ("4xTextureDAT2_otf.safetensors",
                "https://github.com/Phhofm/models/releases/download/4xTextureDAT2_otf/4xTextureDAT2_otf.safetensors",
                "5851f91a4c95d803c919dd279a9353e32b212495cf54c5fbc868eed23b9f068b"),
    "organic": ("4x-PBRify_UpscalerV4.pth",
                "https://github.com/Kim2091/Kim2091-Models/releases/download/4x-PBRify_UpscalerV4/4x-PBRify_UpscalerV4.pth",
                "82db39e31e01da6816a9814c0553a8dcdb7bd2d655353e67075c9295068cb789"),
    "road": ("RealESRGAN_x4plus.pth",
             "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
             "4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1"),
}

HERE = os.path.dirname(os.path.abspath(__file__))


TRANSPARENT = 255  # texel 15, times 17 (tools/m2texcap)


def rows(capture):
    with open(os.path.join(capture, "textures.csv")) as f:
        return list(csv.DictReader(f))


def fill(a, holes):
    """The tones of a cut-out texture (2D numpy array) with its holes filled
    from their neighbours, repeating (textures wrap), outwards from the opaque
    texels; None when there is nothing opaque."""
    import numpy as np
    if holes.all():
        return None
    a = a.astype(np.float32)
    known = ~holes
    while not known.all():
        total = np.zeros_like(a)
        count = np.zeros_like(a)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            total += np.roll(np.where(known, a, 0), (dy, dx), (0, 1))
            count += np.roll(known, (dy, dx), (0, 1))
        grow = ~known & (count > 0)
        a[grow] = total[grow] / count[grow]
        known |= grow
    return a


def read_pgm(path):
    """tools/m2texcap's PGM (P5, 8 bits): width, height, tones."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    if parts[0] != b"P5" or parts[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit PGM")
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][:w * h]


def kind_of(w, h, tones):
    """A texture missing from hires_classes.csv: graphic when a few tones, or
    two of them, fill it, it is high contrast, or most texels equal their
    right-hand neighbour (lettering, gradients, flat areas); else organic.
    Agrees with the table on 113 of its 127 graphic and organic textures."""
    hist = [0] * 256
    for t in tones:
        hist[t] += 1
    n = len(tones)
    used = sum(1 for v in hist if v)
    top2 = sum(sorted(hist)[-2:]) / n
    mean = sum(i * v for i, v in enumerate(hist)) / n
    std = (sum(v * (i - mean) ** 2 for i, v in enumerate(hist)) / n) ** 0.5
    changes = sum(1 for y in range(h) for x in range(w - 1) if tones[y * w + x] != tones[y * w + x + 1])
    changing = changes / (h * (w - 1))
    return "graphic" if used <= 5 or top2 >= 0.6 or std >= 80 or changing < 0.5 else "organic"


def classes():
    with open(os.path.join(HERE, "hires_classes.csv")) as f:
        return {r["hash"]: r["class"] for r in csv.DictReader(f)}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def models(directory):
    os.makedirs(directory, exist_ok=True)
    for name, url, digest in MODELS.values():
        path = os.path.join(directory, name)
        if not os.path.exists(path) or sha256(path) != digest:
            print(f"hires_textures: downloading {name}")
            urllib.request.urlretrieve(url, path + ".part")
            if sha256(path + ".part") != digest:
                os.remove(path + ".part")
                sys.exit(f"hires_textures: {name}: unexpected SHA-256, not used")
            os.replace(path + ".part", path)
    print(f"hires_textures: models in {directory}")


def load_model(path):
    import torch
    from spandrel import ModelLoader
    if path.endswith(".safetensors"):
        from safetensors.torch import load_file
        state = load_file(path)
    else:
        state = torch.load(path, map_location="cpu", weights_only=True)
    model = ModelLoader().load_from_state_dict(state).eval()
    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = model.to(device)
    half = device == "cuda" and model.supports_half
    if half:
        model.model.half()
    return model, device, half


def run_model(model, device, half, a):
    """A 2D array of 0..1 values upscaled SCALE times, padded by wrapping."""
    import numpy as np
    import torch
    h, w = a.shape
    a = np.pad(a.astype(np.float32), PAD, mode="wrap")
    x = torch.from_numpy(a)[None, None].expand(1, 3, -1, -1).contiguous().to(device)
    with torch.inference_mode():
        y = model(x.half() if half else x)
    y = y[0, :, PAD * SCALE:(PAD + h) * SCALE, PAD * SCALE:(PAD + w) * SCALE].float().mean(0)
    return (y.clamp(0, 1).cpu().numpy() * 255.0 + 0.5).astype(np.uint8)


def upscale(capture, directory):
    import numpy as np
    import torch
    from PIL import Image
    table = classes()
    out = os.path.join(capture, "out")
    os.makedirs(out, exist_ok=True)
    work = {kind: [] for kind in MODELS}  # per model: (file, tones or opacities, is an opacity)
    for r in rows(capture):
        w, h, tones = read_pgm(os.path.join(capture, r["file"]))
        a = np.frombuffer(tones, dtype=np.uint8).reshape(h, w)
        if r["cutout"] == "1":
            holes = a == TRANSPARENT
            filled = fill(a, holes)
            if filled is None:
                continue
            kind = table.get(r["hash"]) or kind_of(w, h, bytes(filled.round().astype(np.uint8).ravel()))
            work[kind].append((r["file"], filled / 255.0, False))
            work["graphic"].append((r["file"], (~holes).astype(np.float32), True))
        else:
            kind = table.get(r["hash"]) or kind_of(w, h, tones)
            work[kind].append((r["file"], a / 255.0, False))
    for kind, items in work.items():
        if not items:
            continue
        name, _, digest = MODELS[kind]
        path = os.path.join(directory, name)
        if not os.path.exists(path) or sha256(path) != digest:
            sys.exit(f"hires_textures: {path}: missing or changed (hires_textures.py models {directory})")
        model, device, half = load_model(path)
        start = time.time()
        for file, a, opacity in items:
            suffix = "_a.png" if opacity else ".png"
            Image.fromarray(run_model(model, device, half, a), "L").save(os.path.join(out, file[:-4] + suffix))
        print(f"hires_textures: {len(items)} {kind} images with {name} ({device}), {time.time() - start:.0f} s")
        del model
        if device == "cuda":
            torch.cuda.empty_cache()


def pack(capture, path):
    from PIL import Image
    entries, data = [], bytearray()
    for r in rows(capture):
        w, h = int(r["w"]), int(r["h"])
        src = os.path.join(capture, "out", r["file"][:-4] + ".png")
        cutout = r["cutout"] == "1"
        planes = [src] + ([src[:-4] + "_a.png"] if cutout else [])
        if not all(os.path.exists(p) for p in planes):
            print(f"hires_textures: {r['file']}: not upscaled, left out")
            continue
        entries.append((int(r["sheet"]), int(r["x"]), int(r["y"]), w, h, int(cutout), int(r["hash"], 16), len(data)))
        for p in planes:
            im = Image.open(p).convert("L")
            if im.size != (w * SCALE, h * SCALE):
                im = im.resize((w * SCALE, h * SCALE), Image.LANCZOS)
            data += im.tobytes()
    with open(path, "wb") as f:
        f.write(b"M2HT" + struct.pack("<III", 1, len(entries), SCALE))
        for e in entries:
            f.write(struct.pack("<IIIIIIQQ", *e))
        f.write(data)
    cutouts = sum(e[5] for e in entries)
    print(f"hires_textures: {len(entries)} textures ({cutouts} cut-out), {len(data) / 1e6:.1f} MB, in {path}")


def main():
    a = sys.argv[1:]
    if a[:1] == ["models"] and len(a) == 2:
        models(a[1])
    elif a[:1] == ["upscale"] and len(a) == 3:
        upscale(a[1], a[2])
    elif a[:1] == ["pack"] and len(a) == 3:
        pack(a[1], a[2])
    elif a[:1] == ["all"] and len(a) == 4:
        models(a[2])
        upscale(a[1], a[2])
        pack(a[1], a[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
