#!/usr/bin/env python3
"""Verify the optional System-24 layers in the Direct3D 12 snapshot viewer."""

import struct
import subprocess
import sys
from pathlib import Path

from check_msc2_context_shot import check, pixel, read_ppm
from check_msc2_material_shot import materials, scene


def layer_data(snapshot):
    digest = 14695981039346656037
    for byte in snapshot:
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    header = struct.pack('<IIQQQ', 0x594c324d, 1, 50, len(snapshot), digest)
    background = struct.pack('<I', 0xff2a6120) * (496 * 384)
    foreground = bytearray(496 * 384 * 4)
    struct.pack_into('<I', foreground, (190 * 496 + 95) * 4, 0xff000000)
    return header + background + foreground


def main():
    check(len(sys.argv) == 5, 'usage: check_msc2_layer_shot.py EXE VS PS FRESH_OUTPUT_DIR')
    exe, vs, ps, out = (Path(arg).resolve() for arg in sys.argv[1:])
    out.mkdir()
    snapshot = out / 'scene.msc2'
    snapshot.write_bytes(scene())
    material = out / 'scene.m2mat'
    material.write_bytes(materials(snapshot.read_bytes()))
    context = out / 'scene.context'
    context.write_text('MSC2_CONTEXT 1 50 496 384 0 0 0 0\n', encoding='ascii')
    good = layer_data(snapshot.read_bytes())

    def render(name, data):
        layers = out / f'{name}.m2lay'
        layers.write_bytes(data)
        output = out / f'{name}.ppm'
        args = [str(p) for p in (exe, snapshot, vs, ps, output, context, material, layers)]
        run = subprocess.run(args, capture_output=True, text=True, timeout=45)
        (out / f'{name}.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        return run, output

    run, output = render('layered', good)
    check(run.returncode == 0 and 'layered' in run.stdout and 'direct3d12' in run.stdout, run.stderr)
    rgb = read_ppm(output)
    check(pixel(rgb, 0, 0) == (42, 97, 32), 'background failed')
    check(pixel(rgb, 190, 380) == (0, 0, 0), 'opaque black HUD failed')
    check(pixel(rgb, 192, 380) == (255, 0, 169), 'adjacent textured pixel failed')

    stale_frame = good[:8] + struct.pack('<Q', 51) + good[16:]
    wrong_hash = good[:24] + bytes([good[24] ^ 1]) + good[25:]
    for name, data, reason in (
        ('stale-frame', stale_frame, 'layer frame does not match'),
        ('wrong-snapshot', wrong_hash, 'hash mismatch'),
        ('truncated', good[:-1], 'unexpected input length'),
    ):
        run, output = render(name, data)
        check(run.returncode != 0 and reason in run.stderr and not output.exists(), name + ' accepted')
    print('MSC2 layer GPU pixel and pairing checks passed')


if __name__ == '__main__':
    main()
