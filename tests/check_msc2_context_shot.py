#!/usr/bin/env python3
'''Black-box checks for m2sceneshot MSC2 context snapshots.'''

import struct
import subprocess
import sys
from pathlib import Path

WIDTH = 992
HEIGHT = 768
MAGIC = 0x3243534D
COLOR = (55, 121, 177)
NEWLINE = bytes([10])
CONTEXT = b'MSC2_CONTEXT 1 50 496 384 10 20 3 -5' + NEWLINE
BAD_CONTEXT = b'MSC2_CONTEXT 1 50 496 384 10 20 3' + NEWLINE
BAD_WIDTH = b'MSC2_CONTEXT 1 50 495 384 10 20 3 -5' + NEWLINE
IDENTITY = (
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0,
    0.0, 0.0, 0.0,
)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def triangle_fixture():
    data = bytearray(struct.pack('<4I', MAGIC, 1, 1, 1))
    data += struct.pack('<IIIBI', 7, 0, 0, 0, 1)
    data += struct.pack(
        '<4IBi2h4hI',
        1, 0, 0, 0, 0, 0, 248, 192, 257, 169, 496, 512, 3,
    )
    data += struct.pack('<5f', -1.0, -1.0, 10.0, 0.0, 0.0)
    data += struct.pack('<5f', 1.0, -1.0, 10.0, 1.0, 0.0)
    data += struct.pack('<5f', 0.0, 1.0, 10.0, 0.5, 1.0)
    data += struct.pack('<I14f', 7, *IDENTITY, 100.0, 100.0)
    return bytes(data)


def empty_fixture():
    return struct.pack('<4I', MAGIC, 1, 0, 0)


def read_ppm(path):
    magic, dimensions, maximum, raster = path.read_bytes().split(NEWLINE, 3)
    check(magic == b'P6' and maximum == b'255', 'expected binary PPM')
    check(tuple(map(int, dimensions.split())) == (WIDTH, HEIGHT), 'bad dimensions')
    check(len(raster) == WIDTH * HEIGHT * 3, 'incomplete PPM payload')
    return raster


def pixel(raster, x, y):
    offset = (y * WIDTH + x) * 3
    return tuple(raster[offset:offset + 3])


def main():
    check(len(sys.argv) == 5,
          'usage: check_msc2_context_shot.py EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR')
    exe, vs, ps, output = [Path(arg).expanduser().resolve() for arg in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    triangle = output / 'triangle.msc2'
    triangle.write_bytes(triangle_fixture())
    empty = output / 'empty.msc2'
    empty.write_bytes(empty_fixture())

    def render(label, snapshot, context=None, expected_failure=False):
        image = output / (label + '.ppm')
        argv = [str(exe), str(snapshot), str(vs), str(ps), str(image)]
        if context is not None:
            context_path = output / (label + '.context')
            context_path.write_bytes(context)
            argv.append(str(context_path))
        result = subprocess.run(argv, capture_output=True, text=True, timeout=45)
        (output / (label + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if expected_failure:
            check(result.returncode != 0 and not image.exists(), label + ' should fail before output')
            return None
        check(result.returncode == 0, label + ': ' + result.stderr[-500:])
        return read_ppm(image)

    background = pixel(render('empty', empty, CONTEXT), 0, 0)
    aligned = render('aligned', triangle, CONTEXT)
    check(pixel(aligned, 522, 421) == COLOR, 'shifted triangle or inclusive bottom edge is wrong')
    for x, y in ((519, 421), (522, 422), (496, 384)):
        check(pixel(aligned, x, y) == background, f'expected background at {(x, y)}')
    legacy = render('legacy', triangle)
    check(pixel(legacy, 496, 384) == COLOR, 'legacy projection changed')
    render('bad-context', triangle, BAD_CONTEXT, expected_failure=True)
    render('bad-width', triangle, BAD_WIDTH, expected_failure=True)
    print('MSC2 context GPU pixel checks passed')


if __name__ == '__main__':
    main()
