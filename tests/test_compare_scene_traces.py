import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import compare_scene_traces as ct  # noqa: E402

HDR = ['tick','pool','oba','obc','mode'] + [f'matrix{i}' for i in range(12)] + ['source','command_word','opcode','tpa','tha']

def row(tick, oba, cw='c0', m=0.0, src='s', op='1', tpa='0', tha='0', pool=1, obc=0, mode=0):
    return [tick,pool,oba,obc,mode] + [m]*12 + [src,cw,op,tpa,tha]

def write(rows, tmp, name, hdr=HDR):
    p = Path(tmp) / name
    with p.open('w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(hdr)
        writer.writerows(rows)
    return str(p)

class CompareSceneTracesTests(unittest.TestCase):
    def run_cmp(self, ra, rb, s=0, e=10):
        with tempfile.TemporaryDirectory() as d:
            a, b = write(ra, d, 'a.csv'), write(rb, d, 'b.csv')
            return ct.compare(ct.load(a, s, e), ct.load(b, s, e), s, e)

    def check_partition(self, r):
        for s in 'ab':
            self.assertEqual(r['rows_'+s], r['mutually_unique_rows'] + r['rows_absent_other_side_'+s]
                + r['rows_duplicated_present_both_'+s] + r['rows_unique_here_duplicated_other_'+s])
        self.assertEqual(r['rows_excluded_total'], r['rows_a'] + r['rows_b'] - 2*r['mutually_unique_rows'])

    def test_absent_rows_not_labelled_duplicates(self):
        r = self.run_cmp([row(1,10), row(1,11), row(1,12)], [row(1,10)])
        self.assertEqual((r['duplicate_key_rows_a'], r['duplicate_key_rows_b']), (0, 0))
        self.assertEqual((r['rows_absent_other_side_a'], r['rows_absent_other_side_b']), (2, 0))
        self.assertEqual(r['rows_excluded_total'], 2)
        self.check_partition(r)

    def test_symmetric_suppression(self):
        a, b = [row(1,10), row(1,11), row(1,12)], [row(1,10)]
        r1, r2 = self.run_cmp(a, b), self.run_cmp(b, a)
        self.assertEqual((r2['rows_absent_other_side_a'], r2['rows_absent_other_side_b']), (0, 2))
        for k in ('mutually_unique_rows', 'rows_excluded_total', 'multiset_different_ticks'):
            self.assertEqual(r1[k], r2[k])
        self.check_partition(r2)

    def test_duplicate_on_one_side_only(self):
        r = self.run_cmp([row(1,10), row(1,10), row(1,11)], [row(1,10), row(1,11)])
        self.assertEqual((r['duplicate_key_rows_a'], r['duplicate_key_rows_b']), (2, 0))
        self.assertEqual((r['rows_duplicated_present_both_a'], r['rows_duplicated_present_both_b']), (2, 0))
        self.assertEqual((r['rows_unique_here_duplicated_other_a'], r['rows_unique_here_duplicated_other_b']), (0, 1))
        self.assertEqual((r['mutually_unique_rows'], r['rows_excluded_total']), (1, 3))
        self.assertEqual(r['multiset_different_ticks'], 1)
        self.check_partition(r)
        r2 = self.run_cmp([row(1,10), row(1,11)], [row(1,10), row(1,10), row(1,11)])
        self.assertEqual((r2['duplicate_key_rows_a'], r2['duplicate_key_rows_b']), (0, 2))
        self.assertEqual((r2['rows_unique_here_duplicated_other_a'], r2['rows_duplicated_present_both_b']), (1, 2))
        self.check_partition(r2)

    def test_no_identity_no_interpolation_kept(self):
        r = self.run_cmp([row(1,10)], [row(1,10)])
        self.assertIs(r['identity_asserted'], False)
        self.assertIs(r['interpolation_allowed'], False)

    def test_identical(self):
        r = self.run_cmp([row(1,10), row(1,11)], [row(1,10), row(1,11)])
        self.assertEqual((r['multiset_different_ticks'], r['rank_permutation_ticks'], r['command_word_change_rows']), (0,0,0))
        self.assertEqual(r['mutually_unique_rows'], 2)

    def test_permutation_only(self):
        r = self.run_cmp([row(1,10), row(1,11), row(1,12)], [row(1,12), row(1,10), row(1,11)])
        self.assertEqual(r['multiset_different_ticks'], 0)
        self.assertEqual(r['rank_permutation_ticks'], 1)
        self.assertEqual(r['rank_pair_inversions'], 2)

    def test_command_word_only(self):
        r = self.run_cmp([row(1,10,'c0')], [row(1,10,'c9')])
        self.assertEqual(r['command_word_change_rows'], 1)
        self.assertEqual(r['multiset_different_ticks'], 0)

    def test_multiset_and_nonunique_excluded(self):
        r = self.run_cmp([row(1,10), row(1,10), row(1,11,'x')], [row(1,10), row(1,11,'y')])
        self.assertEqual(r['multiset_different_ticks'], 1)
        self.assertEqual(r['mutually_unique_rows'], 1)
        self.assertEqual(r['rows_excluded_total'], 3)
        self.check_partition(r)
        self.assertEqual(r['command_word_change_rows'], 1)

    def test_opcode_tpa_tha_in_key(self):
        for kw in ({'op':'2'}, {'tpa':'5'}, {'tha':'7'}):
            r = self.run_cmp([row(1,10)], [row(1,10, **kw)])
            self.assertEqual(r['multiset_different_ticks'], 1, kw)
            self.assertEqual(r['mutually_unique_rows'], 0)

    def test_missing_ticks_both_sides(self):
        r = self.run_cmp([row(1,10), row(2,10)], [row(1,10), row(3,10)])
        self.assertEqual(r['ticks_only_in_a'], [2])
        self.assertEqual(r['ticks_only_in_b'], [3])
        self.assertEqual(r['ticks_compared'], 1)

    def test_window_filters(self):
        r = self.run_cmp([row(1,10), row(50,10)], [row(1,10)], 0, 10)
        self.assertEqual(r['ticks_only_in_a'], [])

    def test_empty_windows_refused(self):
        with self.assertRaises(ValueError):
            self.run_cmp([row(99,10)], [row(98,10)], 0, 10)

    def test_no_common_tick_refused(self):
        with self.assertRaises(ValueError):
            self.run_cmp([row(1,10)], [row(99,10)])
        with self.assertRaises(ValueError):
            self.run_cmp([row(1,10)], [row(2,10)])

    def test_one_empty_side_refused(self):
        with self.assertRaises(ValueError):
            self.run_cmp([row(1,10)], [row(99,10)])
        with self.assertRaises(ValueError):
            self.run_cmp([], [row(1,10)])

    def test_duplicate_column_names_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            h = HDR + ['oba']
            p = write([row(1,10)+[10]], d, 'd.csv', hdr=h)
            with self.assertRaises(ValueError) as c:
                ct.load(p, 0, 10)
            self.assertIn('duplicate', str(c.exception))

    def test_tick_and_frame_ambiguous_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = write([row(1,10)+[1]], d, 'tf.csv', hdr=HDR + ['frame'])
            with self.assertRaises(ValueError) as c:
                ct.load(p, 0, 10)
            self.assertIn('ambiguous', str(c.exception))

    def test_pure_insertion_no_inversion(self):
        base = [row(1,10), row(1,11), row(1,12)]
        expanded = [row(1,99)] + base[:1] + [row(1,98)] + base[1:]
        for a, b in ((base, expanded), (expanded, base)):
            r = self.run_cmp(a, b)
            self.assertEqual(r['multiset_different_ticks'], 1)
            self.assertEqual((r['rank_permutation_ticks'], r['rank_pair_inversions'], r['rank_changed_rows']), (0, 0, 0))
            self.assertEqual(r['mutually_unique_rows'], 3)

    def test_duplicate_limits_and_uncompared_reported(self):
        r = self.run_cmp([row(1,10), row(1,10), row(1,11), row(2,10)],
                         [row(1,10), row(1,11), row(1,11), row(3,10)])
        self.assertEqual((r['duplicate_key_rows_a'], r['duplicate_key_rows_b']), (2, 2))
        self.assertEqual(r['ticks_with_duplicates'], 1)
        self.assertEqual(r['ticks_not_compared'], 2)
        self.assertIn('unique', r['limits'])

    def test_bad_window(self):
        with self.assertRaises(ValueError):
            ct.compare({1:[]},{1:[]},5,2)

    def test_signed_zero_equal_nonfinite_rejected(self):
        self.assertEqual(self.run_cmp([row(1,10,m=0.0)], [row(1,10,m=-0.0)])['multiset_different_ticks'], 0)
        with tempfile.TemporaryDirectory() as d:
            p = write([row(1,10,m=float('nan'))], d, 'n.csv')
            with self.assertRaises(ValueError):
                ct.load(p, 0, 10)

    def test_missing_column(self):
        with tempfile.TemporaryDirectory() as d:
            p = write([[1]], d, 'm.csv', hdr=['tick'])
            with self.assertRaises(ValueError):
                ct.load(p, 0, 10)

    def test_frame_alias(self):
        with tempfile.TemporaryDirectory() as d:
            h = ['frame'] + HDR[1:]
            p = write([row(1,10)], d, 'f.csv', hdr=h)
            self.assertIn(1, ct.load(p, 0, 10))

if __name__ == '__main__':
    unittest.main()
