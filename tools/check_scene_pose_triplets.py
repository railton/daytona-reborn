'''Check adjacent scene-pose candidate chains against independent two-tick fits.'''

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_scene_camera_motion import (  # noqa: E402
    LINEAR_TOL,
    TRANS_TOL,
    _is_inlier,
    _load_frames,
)
from scene_pose_candidates import candidate_links  # noqa: E402


def fit_delta(previous, current):
    '''Fit a camera delta from evenly spaced single-pose anchors.'''
    unique = sorted(
        key
        for key in previous.keys() & current.keys()
        if len(previous[key]) == len(current[key]) == 1
    )
    anchors = [(previous[key][0], current[key][0]) for key in unique[::2]]
    if len(anchors) < 3:
        return None

    def support(delta):
        return sum(
            _is_inlier(delta, old, new, LINEAR_TOL, TRANS_TOL)
            for old, new in anchors
        )

    return max(
        (new @ np.linalg.inv(old) for old, new in anchors), key=support
    )


def compare(first, second, skip):
    '''Compare a composed chain with an independent direct two-tick link.'''
    continuation = {(key, middle): end for key, middle, end in second}
    direct = {(key, start): end for key, start, end in skip}
    direct_targets = {(key, end) for (key, _), end in direct.items()}
    counts = Counter(composed=0, matching=0, conflicting=0, no_direct=0)
    for key, start, middle in first:
        end = continuation.get((key, middle))
        if end is None:
            continue
        counts['composed'] += 1
        independent = direct.get((key, start))
        if independent == end:
            counts['matching'] += 1
        elif independent is not None or (key, end) in direct_targets:
            counts['conflicting'] += 1
        else:
            counts['no_direct'] += 1
    return counts


def check(frames):
    '''Return the five triplet-chain aggregates for loaded frames.'''
    pairs = {}
    for tick, previous in sorted(frames.items()):
        current = frames.get(tick + 1)
        if current is None:
            continue
        delta = fit_delta(previous, current)
        if delta is not None:
            pairs[tick] = candidate_links(previous, current, delta)

    stats = Counter(
        triplets=0, composed=0, matching=0, conflicting=0, no_direct=0
    )
    for tick, first in pairs.items():
        if tick + 1 not in pairs:
            continue
        previous, current = frames[tick], frames[tick + 2]
        delta = fit_delta(previous, current)
        if delta is None:
            continue
        direct = candidate_links(previous, current, delta)
        stats['triplets'] += 1
        stats.update(compare(first, pairs[tick + 1], direct))
    return stats


def _self_test():
    conflict = compare([('k', 0, 1)], [('k', 1, 2)], [('k', 0, 0)])
    assert conflict['composed'] == 1 and conflict['conflicting'] == 1, conflict
    claimed_target = compare([('k', 0, 1)], [('k', 1, 2)], iter([('k', 3, 2)]))
    assert claimed_target['conflicting'] == 1 and claimed_target['no_direct'] == 0, claimed_target
    missing = compare([('k', 0, 1)], [('k', 1, 2)], [])
    assert missing['composed'] == 1 and missing['no_direct'] == 1, missing
    matched = compare([('k', 0, 1)], [('k', 1, 2)], [('k', 0, 2)])
    assert matched['matching'] == 1 and matched['conflicting'] == 0, matched


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Compare scene-pose chains against two-tick fits.'
    )
    parser.add_argument('trace', nargs='?', help='objects.csv trace to inspect')
    parser.add_argument('--start', type=int, help='first frame inclusive')
    parser.add_argument('--end', type=int, help='last frame inclusive')
    parser.add_argument(
        '--self-test', action='store_true', help='run synthetic link checks'
    )
    args = parser.parse_args(argv)
    if args.self_test:
        _self_test()
        print('check_scene_pose_triplets: self-test passed')
        return 0
    if args.trace is None or args.start is None or args.end is None:
        parser.error('TRACE, --start and --end are required')
    if args.start > args.end:
        parser.error('--start must not exceed --end')
    frames = _load_frames(args.trace, args.start, args.end)
    print(json.dumps(check(frames), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
