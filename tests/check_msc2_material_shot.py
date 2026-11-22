#!/usr/bin/env python3
"""Check palette, texture, transparency and sidecar pairing on Direct3D 12."""

import struct
import subprocess
import sys
from pathlib import Path

from check_msc2_context_shot import IDENTITY, MAGIC, check, pixel, read_ppm


BACKGROUND = (10, 15, 23)
# mode, tile, swap sheets, sample pixel, expected colour
SAMPLES = (
    (2, 0, False, (190, 380), (255, 0, 169)),
    (2, 1, False, (390, 380), (0, 255, 84)),
    (3, 0, False, (590, 380), (255, 0, 169)),
    (3, 1, False, (790, 380), BACKGROUND),
    (0, 0, False, (190, 560), (0, 84, 255)),
    (2, 1, True, (390, 560), (255, 0, 169)),
    (2, 2, False, (590, 560), (0, 255, 84)),
    (1, 0, False, (790, 560), BACKGROUND),
    (2, 0, False, (390, 690), BACKGROUND),
)


def scene():
    data = bytearray(struct.pack('<4I', MAGIC, 1, 1, 1))
    data += struct.pack('<IIIBI', 7, 0, 0, 0, len(SAMPLES))
    for mode, tile, swapped, (cx, cy), _ in SAMPLES:
        flags = (mode << 13) | 0xc0 | (0x8000 if cy == 690 else 0)
        data += struct.pack(
            '<4IBi2h4hI', flags, 0, tile | (int(swapped) << 12), 7 << 6,
            255 if mode else 32, 10000, 248, 192, 0, 0, 495, 384, 3,
        )
        points = ((cx - 50, cy + 40, 3), (cx + 50, cy + 40, 5), (cx, cy - 60, 7))
        u = (0, 248, 248) if tile == 2 else (64, 64, 64)
        for (x, y, z), value in zip(points, u):
            data += struct.pack('<5f', (x - 496) * z / 200, (384 - y) * z / 200, z, value, 64)
    data += struct.pack('<I14f', 7, *IDENTITY, 100, 100)
    return data


def materials(snapshot):
    fingerprint = 14695981039346656037
    for byte in snapshot:
        fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
    header = struct.pack('<IIQQQ', 0x544d324d, 1, 50, len(snapshot), fingerprint)
    palette = bytearray(0x800)
    struct.pack_into('<H', palette, 7 * 2, 1 | (2 << 5) | (3 << 10))
    translation = bytearray(0xc000)
    for luma, values in ((63, (255, 64, 191)), (31, (64, 255, 127)), (8, (64, 127, 255))):
        for channel, value in enumerate(values):
            index = channel * 0x2000 + ((channel + 1) << 8) + luma
            struct.pack_into('<H', translation, index * 2, 0xab00 | value)
    luma = bytearray(0x20000)
    luma[40 * 4] = 64
    luma[120 * 4] = 32
    texture = bytearray(b'\x55' * 0x400000)
    # Tile x=32 in sheet zero contains transparent nibble 15.
    for row in range(16):
        start = row * 1024 + 32
        texture[start:start + 32] = b'\xff' * 32
        # Tile x=64 is split at u=16 to distinguish perspective from
        # affine interpolation on the triangle with unequal depths.
        start = row * 1024 + 80
        texture[start:start + 16] = b'\xff' * 16
    return header + palette + translation + luma + texture


def main():
    check(len(sys.argv) == 5, 'usage: check_msc2_material_shot.py EXE VS PS FRESH_OUTPUT_DIR')
    exe, vs, ps, out = (Path(arg).resolve() for arg in sys.argv[1:])
    out.mkdir()
    snapshot = out / 'scene.msc2'
    snapshot.write_bytes(scene())
    context = out / 'scene.context'
    context.write_text('MSC2_CONTEXT 1 50 496 384 0 0 0 0\n', encoding='ascii')
    good = materials(snapshot.read_bytes())
    material = out / 'scene.m2mat'
    material.write_bytes(good)

    def render(name, material_path, frame_context=context):
        path = out / f'{name}.ppm'
        command = [str(p) for p in (exe, snapshot, vs, ps, path, frame_context, material_path)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=45)
        (out / f'{name}.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        return run, path

    run, image = render('textured', material)
    check(run.returncode == 0 and 'direct3d12' in run.stdout, run.stderr)
    check('omitted_mode1=1' in run.stdout, 'missing translucent-solid exclusion')
    raster = read_ppm(image)
    check(pixel(raster, 0, 0) == BACKGROUND, 'clear colour differs')
    for mode, tile, swapped, sample, expected in SAMPLES:
        actual = pixel(raster, *sample)
        check(actual == expected, f'mode {mode}, tile {tile}, swapped {swapped}: {actual} != {expected}')

    # At (580.5, 560.5), affine u is about 18.5 and perspective u is
    # about 13.1. At (605.5, 560.5), perspective u is above 16.
    check(pixel(raster, 580, 560) == (255, 0, 169), 'UV interpolation was not perspective-correct')
    check(pixel(raster, 605, 560) == (0, 255, 84), 'split texture did not reach its second half')
    check(pixel(raster, 391, 690) == (255, 0, 169), 'checker uses the doubled output pixel grid')

    bad_frame = out / 'bad-frame.context'
    bad_frame.write_text('MSC2_CONTEXT 1 51 496 384 0 0 0 0\n', encoding='ascii')
    bad_hash = out / 'bad-hash.m2mat'
    bad_hash.write_bytes(good[:24] + bytes([good[24] ^ 1]) + good[25:])
    truncated = out / 'truncated.m2mat'
    truncated.write_bytes(good[:-1])
    bad_version = out / 'bad-version.m2mat'
    bad_version.write_bytes(good[:4] + struct.pack('<I', 2) + good[8:])
    for name, path, frame_context, message in (
        ('frame', material, bad_frame, 'frame does not match'),
        ('hash', bad_hash, context, 'hash mismatch'),
        ('length', truncated, context, 'unexpected input length'),
        ('version', bad_version, context, 'unsupported version'),
    ):
        run, image = render(name, path, frame_context)
        check(run.returncode != 0 and message in run.stderr and not image.exists(), name + ' accepted')
    print('MSC2 material GPU pixel and pairing checks passed')


if __name__ == '__main__':
    main()
