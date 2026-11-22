#!/usr/bin/env python3
"""Quick validation viewer for M2_MESH_DUMP CSV: screen-space polygons.

Not the final renderer: it checks that object extraction produces coherent
triangles. Uses Pillow only.
"""
from __future__ import annotations

import argparse
import colorsys
import csv
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 496, 384
CX, CY = W / 2, H / 2


def color_for(key: str):
    h = (hash(key) & 0xffff) / 65535.0
    r, g, b = colorsys.hsv_to_rgb(h, 0.55, 0.95)
    return int(r * 255), int(g * 255), int(b * 255)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--wire", action="store_true")
    a = ap.parse_args()

    polys = defaultdict(list)
    meta = {}
    for r in csv.DictReader(a.csv.open(encoding="utf-8")):
        polys[r["poly"]].append(r)
        meta[r["poly"]] = r

    img = Image.new("RGB", (W, H), (8, 10, 16))
    d = ImageDraw.Draw(img, "RGBA")
    items = []
    for key, vs in polys.items():
        pts = []
        zs = []
        for r in vs:
            sx, sy, sz = float(r["sx"]), float(r["sy"]), float(r["sz"])
            if sz <= 1e-6:
                continue
            pts.append((CX + sx / sz * a.scale, CY - sy / sz * a.scale))
            zs.append(sz)
        if len(pts) >= 3:
            items.append((sum(zs) / len(zs), pts, meta[key]))
    items.sort(key=lambda x: -x[0])
    for z, pts, m in items:
        col = color_for(m["oba"])
        if a.wire:
            d.line(pts + [pts[0]], fill=col + (200,), width=1)
        else:
            shade = max(0.25, min(1.0, 1.0 - z / 1200.0))
            d.polygon(pts, fill=(int(col[0] * shade), int(col[1] * shade), int(col[2] * shade), 220),
                      outline=(0, 0, 0, 80))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(a.out)
    print(f"{len(items)} polygons -> {a.out}")


if __name__ == "__main__":
    main()

