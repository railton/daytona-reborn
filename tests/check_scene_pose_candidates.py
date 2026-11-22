"""Cross-check repeated-geometry links with the published mapping API."""

from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from scene_pose_candidates import candidate_links


KEY = (1, 2, 3)
CAMERA = np.eye(4)
CAMERA[1, 3] = 0.25


def pose(x):
    matrix = np.eye(4)
    matrix[:3, 3] = (x, 0, 10)
    return matrix


def verify(label, old, new, expected, delta=CAMERA):
    actual = candidate_links({KEY: old}, {KEY: new}, delta)
    assert len(actual) == len(set(actual))
    assert set(actual) == {(KEY, a, b) for a, b in expected}, (label, actual)


def main():
    old = [pose(x) for x in (0, 8, 20)]
    verify('reordered', old, [CAMERA @ pose(x) for x in (20, 0, 8)],
           {(0, 1), (1, 2), (2, 0)})
    verify('distant insertion', old, [CAMERA @ pose(x) for x in (20, 100, 0, 8)],
           {(0, 2), (1, 3), (2, 0)})
    verify('ambiguity', [pose(0), pose(10)], [CAMERA @ pose(x) for x in (.02, .03, 10)],
           {(1, 2)})
    verify('nonfinite row', [pose(0), pose(10)],
           [np.full((4, 4), np.nan), CAMERA @ pose(10), CAMERA @ pose(0)],
           {(0, 2), (1, 1)})
    # In-tolerance replacement is observationally indistinguishable from continuity.
    verify('replacement hypothesis', [pose(0), pose(10)],
           [CAMERA @ pose(.03), CAMERA @ pose(10)], {(0, 0), (1, 1)})
    verify('singletons', [pose(0)], [CAMERA @ pose(0)], set())

    # A finite but non-affine bottom row is not a usable pose entry, and
    # dropping it must preserve the original indices of the remaining rows.
    projective = np.eye(4)
    projective[3, 0] = 0.0001
    translated = np.eye(4)
    translated[0, 3] = 10.0
    verify('non-affine entry', [projective, translated],
           [np.eye(4), translated], {(1, 1)}, delta=np.eye(4))

    # The same matrix is unusable as the camera delta, so nothing links.
    assert candidate_links({KEY: [projective, translated]},
                           {KEY: [np.eye(4), translated]},
                           projective) == []

    # Affine scale and shear stay eligible; only the bottom row is checked.
    scaled = np.eye(4)
    scaled[:3, :3] *= 2.0
    verify('affine scale', [pose(0), pose(10)],
           [scaled @ pose(0), scaled @ pose(10)], {(0, 0), (1, 1)},
           delta=scaled)

    sheared = np.eye(4)
    sheared[0, 1] = 0.5
    verify('affine shear', [pose(0), pose(10)],
           [sheared @ pose(0), sheared @ pose(10)], {(0, 0), (1, 1)},
           delta=sheared)

    print('Repeated-geometry controls passed')


if __name__ == '__main__':
    main()
