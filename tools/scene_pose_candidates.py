'''Pose-correspondence hypotheses for repeated scene geometry.'''

from __future__ import annotations

import argparse
import math
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

__all__ = ['candidate_links']


def _affine_matrix(value: Any) -> Optional[np.ndarray]:
    '''Return a finite homogeneous affine matrix, or None.'''
    if np.iscomplexobj(value):
        return None
    try:
        matrix = np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError, OverflowError):
        return None
    if matrix.shape != (4, 4) or not np.isfinite(matrix).all():
        return None
    if not np.array_equal(matrix[3], (0.0, 0.0, 0.0, 1.0)):
        return None
    return matrix


def _finite_tolerance(value: Any) -> Optional[float]:
    '''Return a finite non-negative tolerance, or None when invalid.'''
    try:
        tolerance = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    if not math.isfinite(tolerance) or tolerance < 0.0:
        return None
    return tolerance


def _valid_matrices(group: Any) -> List[Tuple[int, np.ndarray]]:
    '''Collect valid matrices while preserving their original positions.'''
    try:
        values = list(group)
    except TypeError:
        return []
    valid: List[Tuple[int, np.ndarray]] = []
    for index, value in enumerate(values):
        matrix = _affine_matrix(value)
        if matrix is not None:
            valid.append((index, matrix))
    return valid


def _within_tolerances(
    predicted: np.ndarray,
    observed: np.ndarray,
    translation_tol: float,
    linear_tol: float,
) -> bool:
    '''Test the translation and 3x3 linear parts against both tolerances.'''
    translation_error = float(
        np.linalg.norm(predicted[:3, 3] - observed[:3, 3])
    )
    linear_error = float(np.max(np.abs(predicted[:3, :3] - observed[:3, :3])))
    return (
        math.isfinite(translation_error)
        and math.isfinite(linear_error)
        and translation_error <= translation_tol
        and linear_error <= linear_tol
    )


def candidate_links(
    previous: Any,
    current: Any,
    camera_delta: Any,
    translation_tol: float = 0.05,
    linear_tol: float = 0.001,
) -> List[Tuple[Tuple[int, int, int], int, int]]:
    '''Return mutually unique pose hypotheses for repeated geometry groups.

    Only keys present in both maps are considered, and only when at least
    one side has more than one entry.  For each key, every valid previous
    matrix is transformed by ``camera_delta`` and compared with every valid
    current matrix.  A pair is emitted only when it is the sole candidate
    for its previous entry and the sole candidate for its current entry.
    Non-finite and projective matrices are ignored.  Links are ordered by
    key and input position, independent of mapping insertion order.
    '''
    delta = _affine_matrix(camera_delta)
    trans_tol = _finite_tolerance(translation_tol)
    lin_tol = _finite_tolerance(linear_tol)
    if delta is None or trans_tol is None or lin_tol is None:
        return []

    try:
        common_keys = set(previous.keys()) & set(current.keys())
    except (AttributeError, TypeError):
        return []

    links: List[Tuple[Tuple[int, int, int], int, int]] = []
    with np.errstate(over='ignore', invalid='ignore'):
        for key in sorted(common_keys):
            try:
                prev_values = list(previous[key])
                curr_values = list(current[key])
            except (KeyError, TypeError):
                continue
            if len(prev_values) < 2 and len(curr_values) < 2:
                continue

            prev_matrices = _valid_matrices(prev_values)
            curr_matrices = _valid_matrices(curr_values)
            if not prev_matrices or not curr_matrices:
                continue

            by_previous: Dict[int, List[int]] = {
                index: [] for index, _ in prev_matrices
            }
            by_current: Dict[int, List[int]] = {
                index: [] for index, _ in curr_matrices
            }

            for prev_index, prev_matrix in prev_matrices:
                predicted = delta @ prev_matrix
                for curr_index, curr_matrix in curr_matrices:
                    if _within_tolerances(
                        predicted, curr_matrix, trans_tol, lin_tol
                    ):
                        by_previous[prev_index].append(curr_index)
                        by_current[curr_index].append(prev_index)

            for prev_index in by_previous:
                curr_matches = by_previous[prev_index]
                if len(curr_matches) != 1:
                    continue
                curr_index = curr_matches[0]
                prev_matches = by_current[curr_index]
                if len(prev_matches) == 1 and prev_matches[0] == prev_index:
                    links.append((key, prev_index, curr_index))

    return links


def _pose(x: float, y: float, z: float, yaw: float) -> np.ndarray:
    '''Build a simple 4x4 model-view pose for the self-test.'''
    cosine = math.cos(yaw)
    sine = math.sin(yaw)
    pose = np.eye(4, dtype=np.float64)
    pose[:3, :3] = (
        (cosine, -sine, 0.0),
        (sine, cosine, 0.0),
        (0.0, 0.0, 1.0),
    )
    pose[:3, 3] = (x, y, z)
    return pose


def _self_test() -> None:
    '''Check nonidentity motion, reordered poses, and ambiguity handling.'''
    key = (0, 0, 0)
    delta = np.array(
        [
            [0.0, -1.0, 0.0, 1.0],
            [1.0, 0.0, 0.0, -2.0],
            [0.0, 0.0, 1.0, 0.5],
            [0.0, 0.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )
    pose_a = _pose(0.0, 0.0, 0.0, 0.0)
    pose_b = _pose(3.0, -1.0, 2.0, 0.4)

    # Nonidentity camera motion with the two current poses reordered.
    previous = {key: [pose_a, pose_b]}
    current = {key: [delta @ pose_b, delta @ pose_a]}
    assert candidate_links(previous, current, delta) == [
        (key, 0, 1),
        (key, 1, 0),
    ]

    # Two equal-geometry poses on each side are not uniquely matchable.
    duplicate_previous = {key: [pose_a, pose_a]}
    duplicate_current = {key: [delta @ pose_a, delta @ pose_a]}
    assert candidate_links(duplicate_previous, duplicate_current, delta) == []

    # A single previous pose matches two current poses within tolerance.
    shifted = (delta @ pose_a).copy()
    shifted[0, 3] += 0.01
    ambiguous_previous = {key: [pose_a]}
    ambiguous_current = {key: [delta @ pose_a, shifted]}
    assert candidate_links(ambiguous_previous, ambiguous_current, delta) == []

    # Invalid matrices produce no link but do not hide a valid unique one.
    invalid_pose = pose_a.copy()
    invalid_pose[0, 0] = np.nan
    mixed_previous = {key: [invalid_pose, pose_a]}
    mixed_current = {key: [delta @ pose_a]}
    assert candidate_links(mixed_previous, mixed_current, delta) == [
        (key, 1, 0)
    ]


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description='Generate mutually unique scene pose hypotheses.'
    )
    parser.add_argument(
        '--self-test',
        action='store_true',
        help='run the built-in behavioral checks',
    )
    args = parser.parse_args(argv)
    if not args.self_test:
        parser.error('nothing to do; pass --self-test')
    _self_test()
    print('scene_pose_candidates: self-test passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
