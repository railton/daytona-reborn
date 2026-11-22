#!/usr/bin/env python3
"""D3D12 regression for two CPU submissions before a shared fence wait.

The synthetic scenes exercise geometry reuse, exact readback, cancellation,
and three separately injected failures. The event order describes CPU calls.
"""
import re
import sys
from pathlib import Path

import check_msc2_vertex_arena as f

FAULTS = {'cancel-b': (1, 120), 'fence-a': (1, 60), 'fence-b': (2, 120)}
EVENTS = 'events=submit A,submit B,wait A+B'
EXPECTED = {'aba': (60, 60, 0), 'abort-b': (0, 60, 0)}


def main():
    if len(sys.argv) != 6:
        print('usage: check_msc2_two_submission.py BASE_EXE CANDIDATE_EXE VS.dxil PS.dxil FRESH_OUTPUT_DIR',
              file=sys.stderr)
        return 2
    base, cand, vs, ps, out = (Path(a).expanduser().resolve() for a in sys.argv[1:])
    for p in (base, cand, vs, ps):
        if not p.is_file():
            print('missing file: %s' % p, file=sys.stderr)
            return 2
    try:
        out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print('output directory must not exist: %s' % out, file=sys.stderr)
        return 2
    try:
        ctx = out / 'scene.context'
        ctx.write_bytes(f.CONTEXT)
        files = {}
        for name, data in (('a', f.scene_a()), ('b', f.scene_b())):
            snap = out / (name + '.msc2')
            snap.write_bytes(data)
            ref = out / (name + '.ppm')
            r = f.run([base, snap, vs, ps, ref, ctx], 'ref-' + name, out)
            f.check(r.returncode == 0 and ref.is_file(), 'BASE_EXE reference %s failed: %s' % (name, r.stderr[-300:]))
            files[name] = (snap, ref)
        ra, rb = f.read_ppm(files['a'][1]), f.read_ppm(files['b'][1])
        f.check(ra != rb, 'A and B references must differ')
        f.check(sum(f.mask(ra)) > 0 and sum(f.mask(rb)) > 0, 'references must show silhouettes')

        def argv(flag, ref_a, ref_b):
            return [cand, flag, files['a'][0], ctx, files['b'][0], ctx, vs, ps, ref_a, ref_b]

        for label, flag in (('aba', '--two-in-flight-aba'), ('abort-b', '--two-in-flight-abort-b')):
            r = f.run(argv(flag, files['a'][1], files['b'][1]), label, out)
            f.check(r.returncode == 0, '%s failed (%d): %s' % (flag, r.returncode, r.stderr[-300:]))
            f.check('two-in-flight A-B verified' in r.stdout, '%s: no verified report: %r' % (flag, r.stdout[-300:]))
            f.check(EVENTS in r.stdout, '%s: wrong event order: %r' % (flag, r.stdout[-300:]))
            m = re.search(r'uploaded A=(\d+) B=(\d+) round2=(\d+)', r.stdout)
            f.check(m is not None, '%s: no uploaded report' % flag)
            got = tuple(int(x) for x in m.groups())
            f.check(got == EXPECTED[label], '%s: uploads %s, expected %s' % (flag, got, EXPECTED[label]))
            f.check(r.stdout.strip().endswith(' target_allocs=6'), '%s: expected six target allocations' % flag)
            if label == 'abort-b':
                m = re.search(r'aborted_used_after_a=(\d+)', r.stdout)
                f.check(m is not None and int(m.group(1)) == 60, 'abort-b: B was not rolled back to A (used_after_a)')
                f.check('after-injected-B-failure' in r.stdout, 'abort-b: missing injected-failure marker')
            print('%s: %s' % (flag, r.stdout.strip()))

        for case, (submits, index_bytes) in FAULTS.items():
            r = f.run([cand, '--two-in-flight-fault', case, files['a'][0], ctx, files['b'][0], ctx, vs, ps],
                      'fault-' + case, out)
            f.check(r.returncode == 0, 'fault %s failed (%d): %s' % (case, r.returncode, r.stderr[-300:]))
            lines = [x for x in r.stdout.splitlines() if x.strip()]
            f.check(len(lines) == 1, 'fault %s: expected exactly one stdout line, got %d: %r' % (case, len(lines), r.stdout[-300:]))
            tail = 'fault=%s submits=%d idle_fallbacks=1 index_bytes=%d reuse_refused=1' % (case, submits, index_bytes)
            line = lines[0].rstrip()
            f.check(line == 'direct3d12 ' + tail, 'fault %s: line is not exactly the contract: %r' % (case, line))
            print('--two-in-flight-fault %s: %s' % (case, line))

        r = f.run(argv('--two-in-flight-aba', files['b'][1], files['a'][1]), 'swapped', out)
        f.check(r.returncode == 2, 'swapped references: expected exit code 2, got %d' % r.returncode)
        f.check(f.SWAP_MESSAGE in r.stderr, 'swapped references: no explicit message, stderr=%r' % r.stderr[-300:])
        print('swapped references rejected: exit 2, "%s"' % r.stderr.strip())
        print('MSC2 two-submission GPU checks passed')
        return 0
    except f.CheckFailed as error:
        print('FAIL: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
