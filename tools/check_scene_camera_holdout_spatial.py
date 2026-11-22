"""Measure held-out camera fit support on either side of view-space X."""

import argparse
import json
from pathlib import Path

import numpy as np

from analyze_scene_camera_motion import LINEAR_TOL, TRANS_TOL, _is_inlier, _load_frames


def _compare_pair(previous, current):
    keys = [key for key in sorted(previous.keys() & current.keys())
            if len(previous[key]) == len(current[key]) == 1]
    fit_keys, holdout_keys = keys[::2], keys[1::2]
    if len(fit_keys) < 3:
        return None

    def matches(delta, key):
        return _is_inlier(delta, previous[key][0], current[key][0], LINEAR_TOL, TRANS_TOL)

    best_delta = None
    best_score = -1
    for key in fit_keys:
        try:
            delta = current[key][0] @ np.linalg.inv(previous[key][0])
        except np.linalg.LinAlgError:
            continue
        if not np.isfinite(delta).all():
            continue
        score = sum(matches(delta, anchor) for anchor in fit_keys)
        if score > best_score:
            best_delta, best_score = delta, score
    if best_delta is None:
        return None

    sides = {"negative": {"total": 0, "inliers": 0},
             "nonnegative": {"total": 0, "inliers": 0}}
    for key in holdout_keys:
        x = previous[key][0][0, 3]
        side = sides["negative" if x < 0 else "nonnegative"]
        side["total"] += 1
        side["inliers"] += int(matches(best_delta, key))
    return sides


def analyze(frames):
    report = {"pairs": 0, "both_eligible": 0, "both_supported": 0,
              "negative": {"total": 0, "inliers": 0},
              "nonnegative": {"total": 0, "inliers": 0}}
    for tick, previous in sorted(frames.items()):
        current = frames.get(tick + 1)
        if current is None:
            continue
        sides = _compare_pair(previous, current)
        if sides is None:
            continue
        report["pairs"] += 1
        report["both_eligible"] += int(all(v["total"] for v in sides.values()))
        report["both_supported"] += int(all(v["inliers"] for v in sides.values()))
        for sign, side in sides.items():
            for metric, count in side.items():
                report[sign][metric] += count
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--start", type=int, required=True)
    parser.add_argument("--end", type=int, required=True)
    args = parser.parse_args()
    if args.end < args.start:
        parser.error("--end must be >= --start")
    print(json.dumps(analyze(_load_frames(args.trace, args.start, args.end)), sort_keys=True))


if __name__ == "__main__":
    main()
