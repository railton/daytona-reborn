#!/usr/bin/env python3
'''Verify offline rendering of a synthetic MSC2 triangle at known poses.

With the optional timing test executable, render positions sampled by the C++
timing code and verify that both guarded cases match the latest pose.
'''

import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

WIDTH = 992
HEIGHT = 768
MAGIC = 0x3243534D
NEWLINE = bytes([10])
CONTEXT = b'MSC2_CONTEXT 1 50 496 384 10 20 3 -5' + NEWLINE
IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
# Keep the triangle clear of the left scissor edge at every translation.
POSITIONS = {'start': 1, 'half': 3, 'end': 5}
GUARDED_POSITIONS = {'cut_midpoint': 5, 'cross_session_midpoint': 5}
EXPECTED_RGB = (55, 121, 177)
EXPECTED_PIXELS = 162
SHIFT_HALF = 40
SHIFT_END = 80
DRAW_FIELDS = {'meshes': 1, 'instances': 1, 'draws': 1, 'skipped_primitives': 0, 'skipped_instances': 0}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def triangle_fixture(translation):
    data = bytearray(struct.pack('<4I', MAGIC, 1, 1, 1))
    data += struct.pack('<IIIBI', 7, 0, 0, 0, 1)
    data += struct.pack('<4IBi2h4hI', 1, 0, 0, 0, 0, 0, 248, 192, 257, 169, 496, 512, 3)
    data += struct.pack('<5f', -1.0, -1.0, 10.0, 0.0, 0.0)
    data += struct.pack('<5f', 1.0, -1.0, 10.0, 1.0, 0.0)
    data += struct.pack('<5f', 0.0, 1.0, 10.0, 0.5, 1.0)
    data += struct.pack('<I14f', 7, *IDENTITY, *translation, 100.0, 100.0)
    return bytes(data)


def empty_fixture():
    return struct.pack('<4I', MAGIC, 1, 0, 0)


def read_ppm(path):
    magic, dimensions, maximum, raster = path.read_bytes().split(NEWLINE, 3)
    check(magic == b'P6' and maximum == b'255', 'expected binary PPM')
    check(tuple(map(int, dimensions.split())) == (WIDTH, HEIGHT), 'bad dimensions')
    check(len(raster) == WIDTH * HEIGHT * 3, 'incomplete PPM payload')
    return raster


def parse_log(text):
    fields = {}
    for token in text.split():
        if '=' in token:
            key, value = token.split('=', 1)
            if value.isdigit():
                fields[key] = int(value)
    return fields


def coloured_map(raster, background):
    found = {}
    for index in range(WIDTH * HEIGHT):
        pixel = bytes(raster[index * 3:index * 3 + 3])
        if pixel != background:
            found[(index % WIDTH, index // WIDTH)] = pixel
    return found


def check_exact_shift(base, other, dx, label):
    shifted = {(x + dx, y): c for (x, y), c in base.items()}
    check(len(other) == len(base), f'{label}: coloured pixel count {len(other)} != {len(base)}')
    missing = shifted.keys() - other.keys()
    extra = other.keys() - shifted.keys()
    check(not missing and not extra, f'{label}: not an exact {dx}px translation (missing={len(missing)}, extra={len(extra)})')
    wrong = [p for p in shifted if shifted[p] != other[p]]
    check(not wrong, f'{label}: {len(wrong)} pixels changed colour')

def unique_json_object(pairs):
    obj = dict(pairs)
    check(len(obj) == len(pairs), 'duplicate timing JSON key')
    return obj


def load_timing_positions(timing_exe):
    result = subprocess.run([str(timing_exe), '--emit-synthetic-positions'],
                            capture_output=True, text=True, timeout=45)
    check(result.returncode == 0, f'timing executable exited {result.returncode}: {result.stderr[-500:]}')
    check(not result.stderr.strip(), 'timing executable wrote to stderr')
    data = json.loads(result.stdout, object_pairs_hook=unique_json_object)
    check(isinstance(data, dict) and data.keys() == {'continuous', *GUARDED_POSITIONS},
          'unexpected timing JSON keys')
    continuous = data['continuous']
    check(isinstance(continuous, list) and len(continuous) == len(POSITIONS),
          'expected three continuous timing positions')
    positions = dict(zip(POSITIONS, continuous))
    positions.update({name: data[name] for name in GUARDED_POSITIONS})
    for name, expected in (POSITIONS | GUARDED_POSITIONS).items():
        value = positions[name]
        check(type(value) in (int, float) and value == expected,
              f'{name}: unexpected timing position {value!r}, expected {expected}')
    return positions


def main():
    check(len(sys.argv) in (5, 6),
          'usage: check_scene_intermediate_gpu.py EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR [TIMING_TEST_EXE]')
    exe, vs, ps, output = [Path(arg).expanduser().resolve() for arg in sys.argv[1:5]]
    positions = POSITIONS
    if len(sys.argv) == 6:
        positions = load_timing_positions(Path(sys.argv[5]).expanduser().resolve())
    output.mkdir(exist_ok=False)
    context = output / 'frame.context'
    context.write_bytes(CONTEXT)

    logs = {}

    def render(label, data):
        snapshot = output / (label + '.msc2')
        snapshot.write_bytes(data)
        image = output / (label + '.ppm')
        result = subprocess.run([str(exe), str(snapshot), str(vs), str(ps), str(image), str(context)],
                                capture_output=True, text=True, timeout=45)
        (output / (label + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        check(result.returncode == 0, label + ': ' + result.stderr[-500:])
        logs[label] = parse_log(result.stdout)
        return read_ppm(image)

    empty = render('empty', empty_fixture())
    background = empty[:3]
    check(empty == background * (WIDTH * HEIGHT), 'empty scene is not a uniform background')

    images = {name: render(name, triangle_fixture((x, 0.0, 0.0))) for name, x in positions.items()}
    digests = {name: hashlib.sha256(image).hexdigest() for name, image in images.items()}
    check(len({digests[name] for name in POSITIONS}) == 3,
          'start, half and end outputs must have distinct digests')
    if len(sys.argv) == 6:
        for name in GUARDED_POSITIONS:
            check(images[name] == images['end'], f'{name}: image differs from the latest endpoint')
            print(f'{name}: matches end sha256={digests[name]}')

    for name in images:
        for key, value in DRAW_FIELDS.items():
            check(logs[name].get(key) == value, f'{name}: {key}={logs[name].get(key)}, expected {value}')
    maps = {name: coloured_map(images[name], background) for name in POSITIONS}
    for name, found in maps.items():
        check(len(found) == EXPECTED_PIXELS, f'{name}: {len(found)} coloured pixels, expected {EXPECTED_PIXELS}')
        check(set(found.values()) == {bytes(EXPECTED_RGB)}, f'{name}: colours {sorted(set(found.values()))} != {EXPECTED_RGB}')
    check_exact_shift(maps['start'], maps['half'], SHIFT_HALF, 'half')
    check_exact_shift(maps['start'], maps['end'], SHIFT_END, 'end')

    # Negative control: a non-midpoint translation must be rejected by the exact-shift check.
    mutant = (positions['half'] + 0.25, 0.0, 0.0)
    mutant_map = coloured_map(render('mutant', triangle_fixture(mutant)), background)
    try:
        check_exact_shift(maps['start'], mutant_map, SHIFT_HALF, 'mutant')
    except AssertionError as error:
        print('negative control rejected as expected:', error)
    else:
        raise AssertionError('negative control (half + 0.25) was NOT rejected: check is not discriminating')

    for name in POSITIONS:
        mask = maps[name]
        count = len(mask)
        cx = sum(x for x, _ in mask) / count
        cy = sum(y for _, y in mask) / count
        print(f'{name}: pixels={count} centroid=({cx:.2f},{cy:.2f}) sha256={digests[name]}')
    for middle in ('end', 'empty'):
        pair = subprocess.run([str(exe), '--verify-pair',
                               str(output / 'start.msc2'), str(context),
                               str(output / f'{middle}.msc2'), str(context),
                               str(vs), str(ps), str(output / 'start.ppm'),
                               str(output / f'{middle}.ppm')],
                              capture_output=True, text=True, timeout=45)
        (output / f'pair-{middle}.log').write_text(pair.stdout + pair.stderr, encoding='utf-8')
        check(pair.returncode == 0 and 'A-B-A verified' in pair.stdout,
              f'A-{middle}-A comparison failed: {pair.stderr[-500:]}')
    print('scene intermediate GPU checks passed')


if __name__ == '__main__':
    main()
