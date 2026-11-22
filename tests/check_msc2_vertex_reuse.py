#!/usr/bin/env python3
"""Exercise the CPU diagnostic with two instances sharing a mesh range."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from check_msc2_context_shot import check
from check_msc2_geometry_material_isolation import scene


def main():
    executable = Path(sys.argv[1]).resolve()

    def run(*args):
        return subprocess.run([executable, *args], capture_output=True, text=True, timeout=15)

    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        first = folder / 'first.msc2'
        second = folder / 'second.msc2'
        context = folder / 'scene.context'
        first.write_bytes(scene((0, 0.0)))
        pair = bytearray(first.read_bytes())
        struct.pack_into('<I', pair, 12, 2)  # Two instances, one mesh.
        instance = bytearray(pair[-struct.calcsize('<I14f'):])
        struct.pack_into('<f', instance, struct.calcsize('<I9f'), 2.0)
        second.write_bytes(pair + instance)
        context.write_bytes(b'MSC2_CONTEXT 1 50 496 384 0 0 0 0\n')

        result = run('--vertex-reuse-diagnostic', first, context, second, context)
        check(result.returncode == 0, f'shared range rejected: {result.stderr}')
        check('draws 2 reused 0 new; potential bytes 120 reused 0 new' in result.stdout,
              f'incorrect per-draw totals: {result.stdout}')
        for args in (('--vertex-reuse-diagnostic', first), ('--self-test-vertex-reuse', 'extra')):
            result = run(*args)
            check(result.returncode == 2 and 'usage:' in result.stderr,
                  f'{args[0]} accepted an invalid argument count: {result.stdout} {result.stderr}')


if __name__ == '__main__':
    main()
