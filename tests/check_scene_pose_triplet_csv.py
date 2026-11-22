#!/usr/bin/env python3
# Regression test for tools/check_scene_pose_triplets.py.

from __future__ import annotations

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools' / 'check_scene_pose_triplets.py'

EXPECTED = {
    'triplets': 1,
    'composed': 2,
    'matching': 2,
    'conflicting': 0,
    'no_direct': 0,
}

FIELDS = [
    'frame',
    'source',
    'vertices',
    'oba',
    'obc',
    'mode',
    *[f'matrix{i}' for i in range(12)],
]


def _rom_matrix_values(x):
    # matrix0..matrix11 form the loader's affine block: identity linear
    # part with translation (x, 0, -10).
    return [
        1.0, 0.0, 0.0, 0.0,
        1.0, 0.0, 0.0, 0.0,
        1.0, x, 0.0, -10.0,
    ]


def _row(frame, oba, obc, mode, x):
    row = {
        'frame': str(frame),
        'source': 'ROM',
        'vertices': '6',
        'oba': oba,
        'obc': obc,
        'mode': mode,
    }
    for index, value in enumerate(_rom_matrix_values(x)):
        row[f'matrix{index}'] = str(value)
    return row


def _write_trace(path):
    rows = []
    for tick in (10, 11, 12):
        for index in range(6):
            x = 100.0 + 10.0 * index + tick
            rows.append(_row(tick, 'anchor', f'a{index}', 'singleton', x))

        repeated = [
            _row(tick, 'repeat', 'key', 'repeated', float(tick)),
            _row(tick, 'repeat', 'key', 'repeated', float(tick + 10)),
        ]
        if tick == 11:
            repeated.reverse()
        rows.extend(repeated)

    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def _run_tool(trace):
    return subprocess.run(
        [
            sys.executable,
            str(TOOL),
            str(trace),
            '--start',
            '10',
            '--end',
            '12',
        ],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
    )


def test_scene_pose_triplet_csv():
    with tempfile.TemporaryDirectory() as temp_dir:
        trace = Path(temp_dir) / 'scene_pose_triplets.csv'
        _write_trace(trace)
        completed = _run_tool(trace)

    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert set(report) == set(EXPECTED), report
    assert report == EXPECTED, report


if __name__ == '__main__':
    test_scene_pose_triplet_csv()
    print('check_scene_pose_triplet_csv: ok')
