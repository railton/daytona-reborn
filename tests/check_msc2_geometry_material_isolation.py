#!/usr/bin/env python3
"""Check that identical vertex payloads retain separate draw state in D3D12."""

import struct
import subprocess
import sys
from pathlib import Path

from check_msc2_context_shot import HEIGHT, IDENTITY, MAGIC, WIDTH, check, read_ppm
from check_msc2_material_shot import materials

BACKGROUND = bytes((10, 15, 23))
PINK = bytes((255, 0, 169))
GREEN = bytes((0, 255, 84))
VERTICES = b''.join(
    struct.pack('<5f', (x - 496) * z / 200, (384 - y) * z / 200, z, 64, 64)
    for x, y, z in ((350, 440, 3), (450, 440, 5), (400, 340, 7))
)


def scene(*draws):
    data = bytearray(struct.pack('<4I', MAGIC, 1, len(draws), len(draws)))
    for mesh, (tile, _) in enumerate(draws):
        data += struct.pack('<IIIBI', mesh, 0, 0, 0, 1)
        data += struct.pack('<4IBi2h4hI', 0x40C0, 0, tile, 7 << 6, 255, 10000,
                            248, 192, 0, 0, 495, 384, 3)
        data += VERTICES
    for mesh, (_, dx) in enumerate(draws):
        matrix = (*IDENTITY[:9], dx, 0.0, 0.0)
        data += struct.pack('<I14f', mesh, *matrix, 100, 100)
    return bytes(data)


def coloured_pixels(rgb):
    return {i: rgb[i * 3:i * 3 + 3] for i in range(WIDTH * HEIGHT)
            if rgb[i * 3:i * 3 + 3] != BACKGROUND}


def centre_x(pixels):
    return sum(i % WIDTH for i in pixels) / len(pixels)


def check_second(first, second):
    check(second and set(second.values()) == {GREEN}, 'second draw has the wrong material')
    check(centre_x(second) - centre_x(first) > 40, 'second draw did not move right')
    check(not first.keys() & second.keys(), 'the two colour masks overlap')


def main():
    check(len(sys.argv) == 5,
          'usage: check_msc2_geometry_material_isolation.py EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR')
    exe, vs, ps, out = (Path(arg).resolve() for arg in sys.argv[1:])
    out.mkdir()
    context = out / 'scene.context'
    context.write_bytes(b'MSC2_CONTEXT 1 50 496 384 0 0 0 0\n')

    def render(label, data, expected_draws):
        snapshot, companion, image = (out / (label + suffix) for suffix in ('.msc2', '.m2mat', '.ppm'))
        snapshot.write_bytes(data)
        companion.write_bytes(materials(data))
        result = subprocess.run([str(arg) for arg in (exe, snapshot, vs, ps, image, context, companion)],
                                capture_output=True, text=True, timeout=45)
        (out / (label + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        check(result.returncode == 0, f'{label}: {result.stderr[-500:]}')
        check(f'draws={expected_draws}' in result.stdout.split(), f'{label}: unexpected draw count')
        return coloured_pixels(read_ppm(image))

    first = render('first', scene((0, 0.0)), 1)
    second = render('second', scene((1, 2.0)), 1)
    check(first and set(first.values()) == {PINK}, 'first draw has the wrong material')
    check_second(first, second)
    pair = render('pair', scene((0, 0.0), (1, 2.0)), 2)
    check(pair == first | second, 'combined draw does not preserve both complete colour masks')

    for name, tile, dx, expected in (
        ('wrong-material', 0, 2.0, 'second draw has the wrong material'),
        ('wrong-transform', 1, 0.0, 'second draw did not move right'),
    ):
        mutant = render(name, scene((tile, dx)), 1)
        try:
            check_second(first, mutant)
        except AssertionError as error:
            check(str(error) == expected, f'{name}: wrong failure: {error}')
        else:
            raise AssertionError(f'{name}: wrong draw state was accepted')
    print(f'MSC2 geometry/material isolation passed ({len(first)} and {len(second)} pixels)')


if __name__ == '__main__':
    main()
