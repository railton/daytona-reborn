#!/usr/bin/env python3
'''Synthetic tests for tools/check_scene_draw_order.py (stdlib only).'''
from __future__ import annotations

import csv
from itertools import combinations
from random import Random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_scene_draw_order as tool  # noqa: E402

FIELDS = ['frame', 'pool', 'oba', 'obc', 'mode'] + [f'matrix{i}' for i in range(12)] + ['vertices', 'source', 'command_word']


def row(frame, oba, tx, cw, vertices=10, ty=0):
    r = {'frame': frame, 'pool': 0, 'oba': oba, 'obc': 1, 'mode': 0,
         'vertices': vertices, 'source': 'ROM', 'command_word': cw}
    for i, v in enumerate([1, 0, 0, 0, 1, 0, 0, 0, 1, tx, ty, 0]):
        r[f'matrix{i}'] = v
    return r


def run(rows):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 't.csv'
        with p.open('w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
        return tool.summarize(tool.load(p))


class DrawOrder(unittest.TestCase):
    def test_swap_two_identical_meshes_distinct_poses(self):
        out = run([row(0, 1, 1, 'A'), row(0, 1, 2, 'A'),
                   row(1, 1, 2, 'A'), row(1, 1, 1, 'A')])
        self.assertEqual(out['matched'], 2)
        self.assertEqual(out['global_rank_changes'], 2)
        self.assertEqual(out['geometry_rank_changes'], 2)
        self.assertEqual(out['global_pair_inversions'], 1)
        self.assertEqual(out['geometry_pair_inversions'], 1)
        self.assertEqual(out['command_word_changes'], 0)
        self.assertFalse(out['identity_asserted'])
        self.assertFalse(out['interpolation_allowed'])

    def test_command_word_change_counted(self):
        out = run([row(0, 1, 1, 'A'), row(1, 1, 1, 'B')])
        self.assertEqual(out['command_word_changes'], 1)
        self.assertEqual(out['global_rank_changes'], 0)

    def test_disappearance(self):
        out = run([row(0, 1, 1, 'A'), row(0, 2, 2, 'A'), row(1, 1, 1, 'A')])
        self.assertEqual(out['matched'], 1)
        self.assertEqual(out['unmatched_disappeared'], 1)

    def test_equal_pose_ambiguous_excluded(self):
        out = run([row(0, 1, 5, 'A'), row(0, 1, 5, 'A'), row(1, 1, 5, 'A'), row(1, 1, 5, 'A')])
        self.assertEqual(out['matched'], 0)
        self.assertEqual(out['ambiguous_excluded'], 4)
        self.assertEqual(out['global_rank_changes'], 0)

    def test_ambiguous_on_one_side_only_is_excluded(self):
        out = run([row(0, 1, 5, 'A'), row(1, 1, 5, 'A'), row(1, 1, 5, 'A')])
        self.assertEqual(out['matched'], 0)
        self.assertEqual(out['ambiguous_excluded'], 3)

    def test_tick_jump_not_compared(self):
        out = run([row(0, 1, 1, 'A'), row(2, 1, 1, 'B')])
        self.assertEqual(out['adjacent_pairs'], 0)
        self.assertEqual(out['skipped_non_adjacent'], 1)
        self.assertEqual(out['matched'], 0)
        self.assertEqual(out['command_word_changes'], 0)

    def test_different_oba_same_count_and_pose_do_not_join(self):
        out = run([row(0, 0x100, 1, 'A'), row(1, 0x900, 1, 'A')])
        self.assertEqual(out['matched'], 0)
        self.assertEqual(out['unmatched_disappeared'], 1)
        self.assertEqual(out['unmatched_appeared'], 1)

    def test_command_word_is_not_geometry(self):
        out = run([row(0, 0x100, 1, 'A'), row(1, 0x100, 1, 'Z')])
        self.assertEqual(out['matched'], 1)
        self.assertEqual(out['command_word_changes'], 1)

    def test_vertices_ignored(self):
        out = run([row(0, 0x100, 1, 'A', vertices=10), row(1, 0x100, 1, 'A', vertices=99)])
        self.assertEqual(out['matched'], 1)

    def test_source_disambiguates(self):
        a = row(0, 0x100, 1, 'A')
        b = row(1, 0x100, 1, 'A')
        b['source'] = 'RAM0'
        self.assertEqual(run([a, b])['matched'], 0)

    def test_global_rank_only_change(self):
        out = run([row(0, 0x100, 1, 'A'), row(0, 0x200, 2, 'A'),
                   row(1, 0x300, 3, 'A'), row(1, 0x100, 1, 'A'), row(1, 0x200, 2, 'A')])
        self.assertEqual(out['matched'], 2)
        self.assertEqual(out['global_rank_changes'], 2)
        self.assertEqual(out['geometry_rank_changes'], 0)
        self.assertEqual(out['global_pair_inversions'], 0)
        self.assertEqual(out['geometry_pair_inversions'], 0)

    def test_insertion_within_geometry_is_not_a_reversal(self):
        out = run([row(0, 1, 1, 'A'), row(0, 1, 2, 'A'),
                   row(1, 1, 9, 'A'), row(1, 1, 1, 'A'), row(1, 1, 2, 'A')])
        self.assertEqual(out['global_rank_changes'], 2)
        self.assertEqual(out['geometry_rank_changes'], 2)
        self.assertEqual(out['global_pair_inversions'], 0)
        self.assertEqual(out['geometry_pair_inversions'], 0)

    def test_swap_across_geometries_is_only_global(self):
        out = run([row(0, 1, 1, 'A'), row(0, 2, 2, 'A'),
                   row(1, 2, 2, 'A'), row(1, 1, 1, 'A')])
        self.assertEqual(out['global_pair_inversions'], 1)
        self.assertEqual(out['geometry_pair_inversions'], 0)

    def test_ambiguous_rows_do_not_contribute_pairs(self):
        out = run([row(0, 1, 9, 'A'), row(0, 1, 1, 'A'), row(0, 1, 2, 'A'),
                   row(1, 1, 2, 'A'), row(1, 1, 1, 'A'),
                   row(1, 1, 9, 'A'), row(1, 1, 9, 'A')])
        self.assertEqual(out['ambiguous_excluded'], 3)
        self.assertEqual(out['matched'], 2)
        self.assertEqual(out['global_pair_inversions'], 1)
        self.assertEqual(out['geometry_pair_inversions'], 1)

    def test_pair_counter_against_quadratic_oracle(self):
        random = Random(105)
        for length in range(50):
            ranks = [random.randrange(20) for _ in range(length)]
            expected = sum(a > b for a, b in combinations(ranks, 2))
            self.assertEqual(tool.count_inversions(ranks), expected)

    def test_negative_zero_equals_positive_zero(self):
        out = run([row(0, 0x100, 0, 'A', ty=0), row(1, 0x100, 0, 'A', ty='-0.0')])
        self.assertEqual(out['matched'], 1)

    def test_non_finite_rejected(self):
        for bad in ('nan', 'inf', '-inf'):
            with self.assertRaises(ValueError):
                run([row(0, 0x100, 1, 'A', ty=bad)])

    def test_missing_column_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'bad.csv'
            p.write_text('frame,pool\n0,0\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                tool.load(p)


if __name__ == '__main__':
    unittest.main()
