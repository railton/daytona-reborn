#!/usr/bin/env python3
'''Estimate shared camera motion between adjacent ROM object frames.'''

import argparse
import csv
import json
import math

import numpy as np

LINEAR_TOL = 0.001
TRANS_TOL = 0.05


def _affine(a, t):
    m = np.eye(4, dtype=float)
    m[:3, :3] = a
    m[:3, 3] = t
    return m


def _matrix_from_values(vals):
    try:
        a = [float(v) for v in vals]
        if len(a) != 12 or not all(math.isfinite(v) for v in a):
            return None
    except (TypeError, ValueError):
        return None
    m = np.array(
        [
            [a[0], a[3], a[6], a[9]],
            [a[1], a[4], a[7], a[10]],
            [a[2], a[5], a[8], a[11]],
            [0.0, 0.0, 0.0, 1.0],
        ],
        dtype=float,
    )
    if abs(float(m[2, 3])) <= 0.01:
        return None
    try:
        if np.linalg.matrix_rank(m) < 4:
            return None
    except np.linalg.LinAlgError:
        return None
    return m


def _text(row, name):
    value = row.get(name, '')
    return '' if value is None else str(value).strip()


def _matrix_from_row(row):
    if _text(row, 'source').upper() != 'ROM':
        return None
    try:
        vertices = float(_text(row, 'vertices'))
    except (TypeError, ValueError):
        return None
    if not math.isfinite(vertices) or vertices <= 0:
        return None
    values = []
    for i in range(12):
        value = _text(row, 'matrix%d' % i)
        if value == '':
            return None
        try:
            values.append(float(value))
        except (TypeError, ValueError):
            return None
    return _matrix_from_values(values)


def _frame_number(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number != int(number):
        return None
    return int(number)


def _geometry_key(row):
    return (_text(row, 'oba'), _text(row, 'obc'), _text(row, 'mode'))


def _is_inlier(delta, prev, nxt, linear_tol, trans_tol):
    try:
        predicted = delta @ prev
        linear_error = float(np.max(np.abs(predicted[:3, :3] - nxt[:3, :3])))
        trans_error = float(np.linalg.norm(predicted[:3, 3] - nxt[:3, 3]))
    except (TypeError, ValueError, np.linalg.LinAlgError):
        return False
    return (
        math.isfinite(linear_error)
        and math.isfinite(trans_error)
        and linear_error <= linear_tol
        and trans_error <= trans_tol
    )


def _analyze_frames(frames, linear_tol=LINEAR_TOL, trans_tol=TRANS_TOL):
    pairs = 0
    omitted = 0
    fit_inliers = 0
    holdout_inliers = 0
    fit_keys_total = 0
    holdout_keys_total = 0
    histogram = {}
    for frame in sorted(frames):
        if frame + 1 not in frames:
            continue
        left = frames[frame]
        right = frames[frame + 1]
        common = [
            key
            for key in sorted(set(left) & set(right))
            if len(left[key]) == 1 and len(right[key]) == 1
        ]
        fit = common[0::2]
        holdout = common[1::2]
        if len(fit) < 3:
            omitted += 1
            continue
        best = None
        for key in fit:
            prev = left[key][0]
            nxt = right[key][0]
            try:
                candidate = nxt @ np.linalg.inv(prev)
            except np.linalg.LinAlgError:
                continue
            if not np.isfinite(candidate).all():
                continue
            support = sum(
                1
                for other in fit
                if _is_inlier(
                    candidate,
                    left[other][0],
                    right[other][0],
                    linear_tol,
                    trans_tol,
                )
            )
            if (
                best is None
                or support > best[0]
                or (support == best[0] and key < best[1])
            ):
                best = (support, key, candidate)
        if best is None:
            omitted += 1
            continue
        support, _, delta = best
        hold_support = sum(
            1
            for key in holdout
            if _is_inlier(
                delta,
                left[key][0],
                right[key][0],
                linear_tol,
                trans_tol,
            )
        )
        pairs += 1
        fit_inliers += support
        holdout_inliers += hold_support
        fit_keys_total += len(fit)
        holdout_keys_total += len(holdout)
        histogram[support] = histogram.get(support, 0) + 1
    return {
        'pairs': pairs,
        'pairs_considered': pairs + omitted,
        'pairs_omitted': omitted,
        'frames_eligible': len(frames),
        'fit_keys': fit_keys_total,
        'holdout_keys': holdout_keys_total,
        'fit_inliers': fit_inliers,
        'holdout_inliers': holdout_inliers,
        'fit_support_histogram': {
            str(key): histogram[key] for key in sorted(histogram)
        },
    }


def _load_frames(path, start, end):
    frames = {}
    with open(path, newline='', encoding='utf-8-sig') as stream:
        for raw in csv.DictReader(stream):
            row = {}
            for name, value in raw.items():
                if name is not None:
                    row[name.strip()] = value
            frame = _frame_number(row.get('frame'))
            if frame is None or frame < start or frame > end:
                continue
            matrix = _matrix_from_row(row)
            if matrix is None:
                continue
            key = _geometry_key(row)
            frames.setdefault(frame, {}).setdefault(key, []).append(matrix)
    return frames


def _check(condition, message):
    if not condition:
        raise AssertionError(message)


def _run_self_test():
    delta = _affine(
        np.array(
            [
                [0.94, 0.17, -0.05],
                [0.02, 1.06, 0.12],
                [-0.03, -0.08, 0.98],
            ],
            dtype=float,
        ),
        np.array([0.55, -0.30, 1.25], dtype=float),
    )
    frames = {0: {}, 1: {}}
    for i in range(6):
        linear = np.array(
            [
                [1.0, 0.03 * i, 0.02],
                [0.0, 1.0 + 0.01 * i, -0.02],
                [0.01 * i, 0.0, 1.0],
            ],
            dtype=float,
        )
        translation = np.array(
            [0.15 * i, -0.10 * i, -6.0 + 0.25 * i], dtype=float
        )
        prev = _affine(linear, translation)
        key = ('g', 'g', 'a%d' % i)
        frames[0][key] = [prev]
        frames[1][key] = [delta @ prev]

    outlier_key = ('g', 'g', 'a0_out')
    outlier_prev = frames[0][('g', 'g', 'a0')][0]
    outlier_next = delta @ outlier_prev
    outlier_next[:3, 3] += np.array([0.0, 0.0, 1.0], dtype=float)
    frames[0][outlier_key] = [outlier_prev]
    frames[1][outlier_key] = [outlier_next]

    keys = sorted(set(frames[0]) & set(frames[1]))
    _check(outlier_key in keys[1::2], 'outlier must be in holdout')
    _check(
        not _is_inlier(delta, outlier_prev, outlier_next, LINEAR_TOL, TRANS_TOL),
        'outlier must not be an inlier',
    )

    _check(
        _matrix_from_values(
            [
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0,
                0.0, 0.0, 5.0, 1.0,
            ]
        )
        is None,
        'singular matrix must be rejected',
    )
    invalid = [1.0] * 12
    invalid[3] = float('inf')
    _check(_matrix_from_values(invalid) is None, 'invalid matrix must be rejected')
    _check(
        _matrix_from_values(['bad'] * 12) is None,
        'non-numeric matrix must be rejected',
    )

    report = _analyze_frames(frames, LINEAR_TOL, TRANS_TOL)
    _check(report['pairs'] == 1, 'expected one analyzed pair')
    _check(report['pairs_omitted'] == 0, 'pair must not be omitted')
    _check(report['frames_eligible'] == 2, 'expected two eligible frames')
    _check(report['fit_keys'] == 4, 'expected four fit keys')
    _check(report['holdout_keys'] == 3, 'expected three holdout keys')
    _check(report['fit_inliers'] == 4, 'all fit keys must be inliers')
    _check(report['holdout_inliers'] == 2, 'only anchor holdout keys must be inliers')
    _check(report['fit_support_histogram'] == {'4': 1}, 'unexpected fit histogram')


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Analyze shared camera motion in ROM object CSV frames.'
    )
    parser.add_argument('trace', nargs='?', help='object CSV trace path')
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--end', type=int, default=None)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--linear-tol', type=float, default=LINEAR_TOL)
    parser.add_argument('--translation-tol', type=float, default=TRANS_TOL)
    args = parser.parse_args(argv)

    if args.self_test:
        _run_self_test()
        print('SELF-TEST OK')
        return 0

    if not args.trace:
        parser.error('TRACE is required unless --self-test is used')
    end = args.end if args.end is not None else 2 ** 63 - 1
    if args.start > end:
        parser.error('--start must not exceed --end')
    if (
        not math.isfinite(args.linear_tol)
        or not math.isfinite(args.translation_tol)
        or args.linear_tol <= 0
        or args.translation_tol <= 0
    ):
        parser.error('tolerances must be positive and finite')
    frames = _load_frames(args.trace, args.start, end)
    report = _analyze_frames(frames, args.linear_tol, args.translation_tol)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
