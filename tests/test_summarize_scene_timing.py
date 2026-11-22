import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.summarize_scene_timing import HEADER, summarize

HEADER_TEXT = ','.join(HEADER)
NL = chr(10)
FIELDS = ('n', 'median_ns', 'p95_ns', 'p99_ns', 'max_ns', 'total_ns',
          'max_input_polygons', 'max_instances')


def make_trace(rows):
    lines = [HEADER_TEXT]
    lines.extend(','.join(str(value) for value in row) for row in rows)
    return NL.join(lines) + NL


def values(group):
    return tuple(group[field] for field in FIELDS)


class SummaryTests(unittest.TestCase):
    def run_rows(self, rows, **kwargs):
        return summarize(io.StringIO(make_trace(rows)), **kwargs)

    def test_phase_split_and_percentiles(self):
        rows = [(0, 10, 2, 1, 5), (5, 0, 5, 3, 6), (9, 30, 4, 2, 8),
                (10, 20, 3, 3, 9), (20, 40, 1, 1, 9)]
        whole = self.run_rows(rows)
        self.assertEqual(values(whole), (5, 20, 40, 40, 40, 100, 5, 3))
        self.assertEqual(whole['resident_meshes_at_end'], 9)
        self.assertIsNone(whole['startup'])
        self.assertIsNone(whole['racing'])

        split = self.run_rows(rows, entry_frame=10)
        self.assertEqual(values(split['startup']), (3, 10, 30, 30, 30, 40, 5, 3))
        self.assertEqual(values(split['racing']), (2, 30, 40, 40, 40, 60, 3, 3))
        self.assertEqual(split['resident_meshes_at_end'], 9)

    def test_nearest_rank_and_even_count(self):
        rows = [(i, i * 10, 1, 1, i) for i in range(100)]
        result = self.run_rows(rows, expected_rows=100)
        self.assertEqual(values(result), (100, 495, 940, 980, 990, 49500, 1, 1))

    def test_zero_durations_and_empty_phases(self):
        zero = self.run_rows([(0, 0, 0, 0, 0)], entry_frame=0)
        self.assertIsNone(zero['startup'])
        self.assertEqual(values(zero['racing']), (1, 0, 0, 0, 0, 0, 0, 0))

        one = self.run_rows([(0, 0, 0, 0, 0)], entry_frame=1)
        self.assertEqual(values(one['startup']), (1, 0, 0, 0, 0, 0, 0, 0))
        self.assertIsNone(one['racing'])

    def test_rejects_malformed_rows(self):
        bad = [(1, 2, 3, 4), (0, -1, 1, 1, 1), (0, '+1', 1, 1, 1),
               (0, '1.0', 1, 1, 1), (0, ' 1', 1, 1, 1), (0, '', 1, 1, 1),
               (0, '１', 1, 1, 1), (0, 0, 1, 2, 1)]
        for row in bad:
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.run_rows([row])

    def test_rejects_bad_sequences_and_limits(self):
        cases = [
            [(1, 0, 1, 1, 1), (1, 1, 1, 1, 2)],
            [(2, 0, 1, 1, 1), (1, 1, 1, 1, 2)],
            [(0, 1, 1, 1, 3), (1, 1, 1, 1, 2)],
            [(1 << 64, 0, 1, 1, 1)],
            [(0, 1 << 63, 1, 1, 1)],
            [(0, 0, 1 << 64, 1, 1)],
            [(0, 0, 1, 1, 1 << 64)],
        ]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                self.run_rows(rows)

    def test_rejects_empty_bad_header_and_bad_csv(self):
        bad = ['', HEADER_TEXT + NL,
               'frame,elapsed_ns,input_polygons,instances' + NL + '0,0,0,0,0' + NL,
               HEADER_TEXT + NL + '0,1,1,1,' + chr(34) + '2' + NL]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                summarize(io.StringIO(text))

    def test_rejects_count_mismatch_and_bad_arguments(self):
        text = make_trace([(0, 0, 0, 0, 0)])
        with self.assertRaises(ValueError):
            summarize(io.StringIO(text), expected_rows=2)
        for kwargs in ({'entry_frame': -1}, {'entry_frame': True},
                       {'expected_rows': 0}, {'expected_rows': True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                summarize(io.StringIO(text), **kwargs)


SCRIPT = ROOT / 'tools' / 'summarize_scene_timing.py'


class CliTests(unittest.TestCase):
    def test_invalid_input_exits_2_without_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'bad.csv'
            path.write_text(HEADER_TEXT + NL + '0,1,2' + NL, encoding='utf-8')
            proc = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                  capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 2)
        self.assertEqual(proc.stdout, '')
        self.assertTrue(proc.stderr)

    def test_valid_input_prints_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'trace.csv'
            path.write_text(make_trace([(0, 5, 1, 1, 1)]), encoding='utf-8')
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(path), '--entry-frame', '0'],
                capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        result = json.loads(proc.stdout)
        self.assertIsNone(result['startup'])
        self.assertEqual(result['racing']['max_ns'], 5)
