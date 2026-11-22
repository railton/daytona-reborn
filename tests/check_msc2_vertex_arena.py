#!/usr/bin/env python3
"""ROM-free GPU check of the resident vertex arena (m2sceneshot --arena-aba).

usage: check_msc2_vertex_arena.py BASE_EXE ARENA_EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR

Synthetic, untextured MSC2 triangles only (no ROM, no network, standard library).
  case A: two instances sharing ONE mesh (identical vertex bytes), left and right.
  case B: left instance unchanged (same mesh, same range), right instance uses a NEW mesh.
References are rendered by BASE_EXE (single scene, no arena). ARENA_EXE --arena-aba
must reproduce them (A, B, A again) and report effectively submitted bytes 60/60/0.
Masks (pixels != background) prove both silhouettes and the change exist visually.
Permuted references must make ARENA_EXE fail with exit code 2 and an explicit message.
Exit code: 0 pass; 1 check failed; 2 usage/environment error.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

WIDTH, HEIGHT = 992, 768
MAGIC = 0x3243534D
IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)
CONTEXT = b'MSC2_CONTEXT 1 50 496 384 0 0 0 0\n'
TIMEOUT = 60
SUBMITTED = (60, 60, 0)          # expected bytes submitted: A, B, A again
SHIFT = 12.0                     # instance translation, left = -SHIFT, right = +SHIFT
SWAP_MESSAGE = 'differs from its single-scene reference'


class CheckFailed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise CheckFailed(message)


def mesh(mesh_id, points):
    data = struct.pack('<IIIBI', mesh_id, 0, 0, 0, 1)
    data += struct.pack('<4IBi2h4hI', 1, 0, 0, 0, 0, 0, 248, 192, 0, 0, 495, 384, 3)
    for (x, y, z), u in zip(points, (0.0, 1.0, 0.5)):
        data += struct.pack('<5f', x, y, z, u, 0.0)
    return data


def instance(mesh_id, dx):
    m = list(IDENTITY)
    m[9] = dx
    return struct.pack('<I14f', mesh_id, *m, 100.0, 100.0)


def snapshot(meshes, instances):
    return (struct.pack('<4I', MAGIC, 1, len(meshes), len(instances))
            + b''.join(meshes) + b''.join(instances))


SMALL = ((-1.0, -1.0, 10.0), (1.0, -1.0, 10.0), (0.0, 1.0, 10.0))
LARGE = ((-1.5, -1.5, 10.0), (1.5, -1.5, 10.0), (0.0, 2.0, 10.0))


def scene_a():   # one mesh, two instances
    return snapshot([mesh(7, SMALL)], [instance(7, -SHIFT), instance(7, SHIFT)])


def scene_b():   # left range unchanged (mesh 7), right range new (mesh 8)
    # The new range is first in B but follows A's resident range in the arena.
    return snapshot([mesh(7, SMALL), mesh(8, LARGE)], [instance(8, SHIFT), instance(7, -SHIFT)])


def read_ppm(path):
    data = Path(path).read_bytes()
    header = b'P6\n%d %d\n255\n' % (WIDTH, HEIGHT)
    check(data.startswith(header), '%s: bad PPM header' % path)
    raster = data[len(header):]
    check(len(raster) == WIDTH * HEIGHT * 3, '%s: incomplete PPM payload' % path)
    return raster


def mask(raster):
    bg = raster[:3]
    return [raster[i:i + 3] != bg for i in range(0, len(raster), 3)]


def half(m, right):
    out = []
    for y in range(HEIGHT):
        row = m[y * WIDTH:(y + 1) * WIDTH]
        out.append(row[WIDTH // 2:] if right else row[:WIDTH // 2])
    return out


def count(rows):
    return sum(sum(r) for r in rows)


def run(argv, label, out):
    try:
        r = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                           timeout=TIMEOUT, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        raise CheckFailed('%s: timed out after %ds' % (label, TIMEOUT))
    (out / (label + '.log')).write_text(
        'argv: %s\nexit: %d\n--stdout--\n%s\n--stderr--\n%s\n' % (argv, r.returncode, r.stdout, r.stderr),
        encoding='utf-8')
    return r


def main():
    if len(sys.argv) != 6:
        print('usage: check_msc2_vertex_arena.py BASE_EXE ARENA_EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR',
              file=sys.stderr)
        return 2
    base, arena, vs, ps, out = (Path(a).expanduser().resolve() for a in sys.argv[1:])
    for f in (base, arena, vs, ps):
        if not f.is_file():
            print('missing file: %s' % f, file=sys.stderr)
            return 2
    try:
        out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print('output directory must not exist: %s' % out, file=sys.stderr)
        return 2
    try:
        ctx = out / 'scene.context'
        ctx.write_bytes(CONTEXT)
        files = {}
        for name, data in (('a', scene_a()), ('b', scene_b())):
            snap = out / (name + '.msc2')
            snap.write_bytes(data)
            ref = out / (name + '.ppm')
            r = run([base, snap, vs, ps, ref, ctx], 'ref-' + name, out)
            check(r.returncode == 0 and ref.is_file(), 'BASE_EXE reference %s failed: %s' % (name, r.stderr[-300:]))
            files[name] = (snap, ref)
        ra, rb = read_ppm(files['a'][1]), read_ppm(files['b'][1])

        # visual checks on the references (no uniform references, silhouettes, change)
        ma, mb = mask(ra), mask(rb)
        la, lb, rra, rrb = half(ma, False), half(mb, False), half(ma, True), half(mb, True)
        check(count(la) > 0 and count(rra) > 0, 'A must show two silhouettes (left %d, right %d px)'
              % (count(la), count(rra)))
        check(count(lb) > 0 and count(rrb) > 0, 'B must show two silhouettes (left %d, right %d px)'
              % (count(lb), count(rrb)))
        check(sum(ma) < len(ma) // 2 and sum(mb) < len(mb) // 2, 'reference is not a mostly-background scene')
        check(la == lb, 'unchanged left range must render identically in A and B')
        check(rra != rrb and count(rrb) != count(rra), 'right silhouette must change between A and B')
        check(ra != rb, 'A and B references must differ')
        print('references: A left=%d right=%d px; B left=%d right=%d px'
              % (count(la), count(rra), count(lb), count(rrb)))

        # arena run with the correct references
        argv = [arena, '--arena-aba', files['a'][0], ctx, files['b'][0], ctx, vs, ps,
                files['a'][1], files['b'][1]]
        r = run(argv, 'arena', out)
        check(r.returncode == 0, 'ARENA_EXE --arena-aba failed (%d): %s' % (r.returncode, r.stderr[-300:]))
        m = re.search(r'uploaded_bytes A=(\d+) B=(\d+) A2=(\d+)', r.stdout)
        check(m is not None, 'no uploaded_bytes report in arena output: %r' % r.stdout[-300:])
        got = tuple(int(x) for x in m.groups())
        check(got == SUBMITTED, 'submitted bytes %s, expected %s' % (got, SUBMITTED))
        check('arena A-B-A verified' in r.stdout, 'arena did not report A-B-A verified')
        print('arena: %s' % r.stdout.strip())

        # permuted references must fail explicitly with code 2
        argv_swapped = argv[:8] + [files['b'][1], files['a'][1]]
        r = run(argv_swapped, 'arena-swapped', out)
        check(r.returncode == 2, 'swapped references: expected exit code 2, got %d' % r.returncode)
        check(SWAP_MESSAGE in r.stderr, 'swapped references: no explicit message, stderr=%r' % r.stderr[-300:])
        print('swapped references rejected: exit 2, "%s"' % r.stderr.strip())
        print('MSC2 vertex arena GPU checks passed')
        return 0
    except CheckFailed as error:
        print('FAIL: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
