'''Falsify index-as-identity after a pose permutation and a disappearance.

candidate_links returns mutually unique pose hypotheses. It does not return
a persistent object id. A unique link is therefore a pose correspondence.
This fixture keeps that boundary observable: crossed indices, a missing
successor, equal-pose ambiguity, and a same-geometry newcomer at the old pose.
'''

from __future__ import annotations

import math
from pathlib import Path
import sys

import numpy as np


KEY = ("0", "0", "0")
TRANSLATION_TOL = 0.05
LINEAR_TOL = 0.001


def _tools_directory(argv):
    '''Sibling tools by default; one optional directory overrides it.'''
    if len(argv) > 2:
        raise SystemExit(
            'usage: check_scene_pose_permutation_gap.py [tools_dir]'
        )
    if len(argv) == 2:
        return Path(argv[1])
    return Path(__file__).resolve().parents[1] / 'tools'


def _load_candidate_links(tools_dir):
    '''Import candidate_links from an explicit tools directory.'''
    module_path = Path(tools_dir) / 'scene_pose_candidates.py'
    if not module_path.is_file():
        raise SystemExit('scene_pose_candidates.py not found in %s' % tools_dir)
    sys.path.insert(0, str(module_path.parent))
    sys.modules.pop('scene_pose_candidates', None)
    from scene_pose_candidates import candidate_links
    return candidate_links


def _pose(x):
    '''Identity rotation, translation on x, affine bottom row.'''
    pose = np.eye(4, dtype=np.float64)
    pose[0, 3] = float(x)
    return pose


def _x(pose):
    return float(pose[0, 3])


def _links(candidate_links, previous, current):
    '''Call the direct helper. Identity delta, production tolerances.'''
    return candidate_links(
        {KEY: previous},
        {KEY: current},
        np.eye(4, dtype=np.float64),
        translation_tol=TRANSLATION_TOL,
        linear_tol=LINEAR_TOL,
    )


def _require(label, actual, expected):
    print('%s %r' % (label, actual))
    if actual != expected:
        raise AssertionError((label, actual, expected))


def run(candidate_links):
    '''Exercise permutation, disappearance, ambiguity, and reappearance.'''
    pose_a = _pose(0.0)
    pose_b = _pose(10.0)
    # Same geometry key and the same pose as B, but a later list entry.
    pose_newcomer = _pose(10.0)

    tick0 = [pose_a, pose_b]
    tick1 = [pose_b, pose_a]
    tick2 = [pose_a]
    tick3 = [pose_newcomer, pose_a]

    crossed = _links(candidate_links, tick0, tick1)
    _require('crossed', crossed, [(KEY, 0, 1), (KEY, 1, 0)])

    disappeared = _links(candidate_links, tick1, tick2)
    _require('disappearance', disappeared, [(KEY, 1, 0)])
    linked_previous = {previous for _key, previous, _current in disappeared}
    if 0 in linked_previous:
        raise AssertionError(('disappeared pose still linked', disappeared))

    # Slot 0 is B before the gap and A after it. The midpoint matches neither.
    midpoint = 0.5 * (_x(tick1[0]) + _x(tick2[0]))
    distance_a = abs(midpoint - _x(pose_a))
    distance_b = abs(midpoint - _x(pose_b))
    print(
        'index0 midpoint x=%s distance_a=%s distance_b=%s'
        % (midpoint, distance_a, distance_b)
    )
    if midpoint != 5.0 or distance_a != 5.0 or distance_b != 5.0:
        raise AssertionError((midpoint, distance_a, distance_b))
    if distance_a <= TRANSLATION_TOL or distance_b <= TRANSLATION_TOL:
        raise AssertionError('midpoint fell inside the production tolerance')

    confirmed = disappeared[0]
    confirmed_midpoint = 0.5 * (
        _x(tick1[confirmed[1]]) + _x(tick2[confirmed[2]])
    )
    print('confirmed midpoint x=%s' % confirmed_midpoint)
    if confirmed_midpoint != 0.0:
        raise AssertionError(confirmed_midpoint)

    equal_pair = _links(candidate_links, [pose_a, pose_a], [pose_a, pose_a])
    equal_drop = _links(candidate_links, [pose_a, pose_a], [pose_a])
    _require('equal-pose', equal_pair, [])
    _require('equal-pose-disappearance', equal_drop, [])

    # Skipping the gap links the newcomer exactly as continued B would.
    # The tuples carry no object id, so this is not observed object identity.
    reappeared = _links(candidate_links, tick1, tick3)
    continued = _links(candidate_links, tick1, [pose_b, pose_a])
    adjacent = _links(candidate_links, tick2, tick3)
    _require('reappearance', reappeared, [(KEY, 0, 0), (KEY, 1, 1)])
    _require('continued-pose', continued, [(KEY, 0, 0), (KEY, 1, 1)])
    _require('adjacent-after-gap', adjacent, [(KEY, 0, 1)])
    if reappeared != continued:
        raise AssertionError((reappeared, continued))
    newcomer_index = 0
    if any(current == newcomer_index for _key, _prev, current in adjacent):
        raise AssertionError(('newcomer linked across the empty frame', adjacent))
    if not any(current == newcomer_index for _key, _prev, current in reappeared):
        raise AssertionError(('old pose was not relinked', reappeared))
    if not math.isfinite(midpoint):
        raise AssertionError(midpoint)


def main(argv=None):
    if argv is None:
        argv = sys.argv
    tools_dir = _tools_directory(argv)
    print('tools %s' % tools_dir)
    candidate_links = _load_candidate_links(tools_dir)
    run(candidate_links)
    print('check_scene_pose_permutation_gap: ok')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
