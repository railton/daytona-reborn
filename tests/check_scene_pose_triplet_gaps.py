'''Gap controls for the real scene-pose triplet checker.'''

from __future__ import annotations

import math
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

from check_scene_pose_triplets import check  # noqa: E402

REPEATED_KEY = ('repeat', 'shared', 'two-poses')
ANCHORS = tuple(('anchor', 'a%d' % index, 'single') for index in range(6))

GAP_EXPECTED = {
    'triplets': 0,
    'composed': 0,
    'matching': 0,
    'conflicting': 0,
    'no_direct': 0,
}
FULL_EXPECTED = {
    'triplets': 2,
    'composed': 4,
    'matching': 4,
    'conflicting': 0,
    'no_direct': 0,
}


def _pose(x, y, z, yaw):
    cosine = math.cos(yaw)
    sine = math.sin(yaw)
    matrix = np.eye(4, dtype=np.float64)
    matrix[:3, :3] = (
        (cosine, -sine, 0.0),
        (sine, cosine, 0.0),
        (0.0, 0.0, 1.0),
    )
    matrix[:3, 3] = (x, y, z)
    return matrix


def _camera_delta():
    angle = 0.08
    cosine = math.cos(angle)
    sine = math.sin(angle)
    return np.array(
        [
            [cosine, -sine, 0.0, 0.23],
            [sine, cosine, 0.0, -0.11],
            [0.0, 0.0, 1.0, 0.07],
            [0.0, 0.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )


_ANCHOR_POSES = tuple(
    _pose(
        0.25 * index - 0.8,
        0.6 - 0.35 * index,
        -6.0 + 0.15 * index,
        0.05 * index,
    )
    for index in range(6)
)
_POSE_A = _pose(1.6, 0.3, -5.5, 0.35)
_POSE_B = _pose(-1.2, -0.9, -7.0, -0.25)


def _frames(ticks):
    delta = _camera_delta()
    frames = {}
    for tick in ticks:
        camera = np.linalg.matrix_power(delta, tick - 10)
        view = {
            key: [camera @ pose]
            for key, pose in zip(ANCHORS, _ANCHOR_POSES)
        }
        repeated = [_POSE_A, _POSE_B]
        if tick == 11:
            repeated.reverse()
        view[REPEATED_KEY] = [camera @ pose for pose in repeated]
        frames[tick] = view
    return frames


def test_scene_pose_triplet_gaps():
    frames = _frames((10, 11, 12, 13))

    # Ticks 10 and 12 retain a valid two-tick pose fit: a gap-bridging
    # implementation would form (10, 12, 13).  The real checker keys pairs
    # by tick + 1, so removing tick 11 must leave no triplet at all.
    gap = {tick: frames[tick] for tick in (10, 12, 13)}
    gap_counts = dict(check(gap))
    assert gap_counts == GAP_EXPECTED, gap_counts
    assert gap_counts['triplets'] == 0

    # Adding the missing tick 11 fills both adjacent windows.
    full_counts = dict(check(frames))
    assert full_counts == FULL_EXPECTED, full_counts
    assert full_counts['triplets'] == 2


def main():
    test_scene_pose_triplet_gaps()
    print('check_scene_pose_triplet_gaps: ok')


if __name__ == '__main__':
    main()
