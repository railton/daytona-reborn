#!/usr/bin/env python3
"""Distinguish native depth from the reference priority on Direct3D 12."""

import struct
import subprocess
import sys
from pathlib import Path

from check_msc2_context_shot import IDENTITY, MAGIC, check, pixel, read_ppm


def fixture():
    snapshot = bytearray(struct.pack('<4I', MAGIC, 1, 1, 1))
    snapshot += struct.pack('<IIIBI', 7, 0, 0, 0, 3)
    for z, palette_index, mode in ((2, 0, 0), (1, 0, 1), (4, 1, 0)):
        snapshot += struct.pack(
            '<4IBi2h4hI', mode << 13, 0, 0, palette_index << 6,
            0, 0, 248, 192, 0, 0, 495, 384, 3,
        )
        for x, y in ((-z / 10, -z / 10), (z / 10, -z / 10), (0, z / 10)):
            snapshot += struct.pack('<5f', x, y, z, 0, 0)
    snapshot += struct.pack('<I14f', 7, *IDENTITY, 100, 100)

    digest = 14695981039346656037
    for byte in snapshot:
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    material = bytearray(struct.pack('<IIQQQ', 0x544d324d, 1, 50, len(snapshot), digest))
    material += bytearray(0x800 + 0xc000 + 0x20000 + 2 * 0x200000)
    struct.pack_into('<HH', material, 32, 31, 31 << 10)
    for colour in (31 << 8, 0x4000 + (31 << 8)):
        material[32 + 0x800 + 2 * colour] = 255
    return snapshot, material, digest


def main():
    check(len(sys.argv) == 5, 'usage: check_msc2_priority_probe.py EXE VS PS FRESH_OUTPUT_DIR')
    exe, vs, ps, out = (Path(arg).resolve() for arg in sys.argv[1:])
    out.mkdir()
    snapshot, material, digest = fixture()
    scene = out / 'scene.msc2'
    scene.write_bytes(snapshot)
    context = out / 'scene.context'
    context.write_text('MSC2_CONTEXT 1 50 496 384 0 0 0 0\n', encoding='ascii')
    memory = out / 'scene.m2mat'
    memory.write_bytes(material)
    layers = out / 'scene.m2lay'
    layers.write_bytes(struct.pack('<IIQQQ', 0x594c324d, 1, 50, len(snapshot), digest) +
                       bytes(2 * 496 * 384 * 4))
    priority = struct.pack('<IIQQQ', 0x524f324d, 1, 50, len(snapshot), digest)
    priority += struct.pack('<9I', 1, 500, 1, 1, 499, 0, 1, 500, 2)
    stale = priority[:8] + struct.pack('<Q', 51) + priority[16:]
    bad_hash = priority[:24] + bytes([priority[24] ^ 1]) + priority[25:]
    duplicate = priority[:-4] + struct.pack('<I', 1)

    for name, data, expected, error in (
        ('native', None, (255, 0, 0), None),
        ('priority', priority, (0, 0, 255), None),
        ('short', priority[:-1], None, 'unexpected input length'),
        ('stale', stale, None, 'priority frame does not match'),
        ('hash', bad_hash, None, 'hash mismatch'),
        ('duplicate', duplicate, None, 'duplicate reference priority emission'),
    ):
        image = out / f'{name}.ppm'
        args = [str(path) for path in (exe, scene, vs, ps, image, context, memory)]
        if data is not None:
            path = out / f'{name}.m2ord'
            path.write_bytes(data)
            args.extend((str(layers), str(path)))
        run = subprocess.run(args, capture_output=True, text=True, timeout=45)
        (out / f'{name}.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        if error:
            check(run.returncode != 0 and error in run.stderr and not image.exists(), name + ' accepted')
        else:
            check(run.returncode == 0 and 'direct3d12' in run.stdout and 'omitted_mode1=1' in run.stdout, run.stderr)
            check(pixel(read_ppm(image), 496, 384) == expected, f'{name} selected the wrong polygon')
    print('MSC2 native-depth and reference-priority GPU checks passed')


if __name__ == '__main__':
    main()
