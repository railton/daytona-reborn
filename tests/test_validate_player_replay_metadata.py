import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import validate_player_replay_metadata as v

H = lambda c: c * 64


def base():
    return {'course': 1, 'transmission': 'automatic', 'frames': 600,
            'script_sha256': H('a'), 'rom_set_sha256': H('b'),
            'nvram_sha256': H('c'), 'build_sha256': H('d')}


class ReplayMetadataTests(unittest.TestCase):
    def test_same_bytes_other_course(self):
        b = base(); b['course'] = 2
        r = v.compare(base(), b)
        self.assertFalse(r['ok']); self.assertEqual(r['differing'], ['course'])

    def test_same_journal_other_gearbox(self):
        b = base(); b['transmission'] = 'manual'
        r = v.compare(base(), b)
        self.assertFalse(r['ok']); self.assertEqual(r['differing'], ['transmission'])

    def test_same_fingerprints_other_race_mode(self):
        normal = dict(base(), race_mode='normal')
        time_attack = dict(base(), race_mode='time-attack')
        self.assertTrue(v.compare(base(), normal)['ok'])
        self.assertTrue(v.compare(time_attack, time_attack)['ok'])
        self.assertEqual(v.compare(normal, time_attack)['differing'], ['race_mode'])
        self.assertEqual(v.compare(base(), time_attack)['differing'], ['race_mode'])

    def test_invalid_race_mode(self):
        for invalid in (None, 'unknown', 1):
            bad = dict(base(), race_mode=invalid)
            self.assertEqual(v.compare(base(), bad)['invalid_b'], ['race_mode'])

    def test_invalid_digest(self):
        b = base(); b['rom_set_sha256'] = 'xyz'
        r = v.compare(base(), b)
        self.assertFalse(r['ok']); self.assertEqual(r['invalid_b'], ['rom_set_sha256'])
        self.assertNotIn('xyz', json.dumps(r))

    def test_full_identical_capture(self):
        b = base(); b['build_sha256'] = H('D')
        r = v.compare(base(), b)
        self.assertTrue(r['ok']); self.assertEqual(r['differing'], [])

    def test_missing_field(self):
        a = base(); del a['nvram_sha256']
        r = v.compare(a, base())
        self.assertFalse(r['ok']); self.assertEqual(r['missing_a'], ['nvram_sha256'])

    def test_invalid_course_frames_bool(self):
        b = base(); b['course'] = 4; b['frames'] = 0
        self.assertEqual(v.compare(base(), b)['invalid_b'], ['course', 'frames'])
        b = base(); b['frames'] = True
        self.assertEqual(v.compare(base(), b)['invalid_b'], ['frames'])

    def test_cli_exit_codes_no_values(self):
        with tempfile.TemporaryDirectory() as d:
            pa, pb = Path(d, 'a.json'), Path(d, 'b.json')
            pa.write_text(json.dumps(base())); b = base(); b['frames'] = 7
            pb.write_text(json.dumps(b))
            tool = str(ROOT / 'tools' / 'validate_player_replay_metadata.py')
            r = subprocess.run([sys.executable, tool, '--expected', str(pa), '--actual', str(pb)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn('frames', r.stdout); self.assertNotIn(H('a'), r.stdout)
            r = subprocess.run([sys.executable, tool, '--expected', str(pa), '--actual', str(pa)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)

    def _cli(self, a_text, b_text):
        with tempfile.TemporaryDirectory() as d:
            pa, pb = Path(d, 'a.json'), Path(d, 'b.json')
            for pth, t in ((pa, a_text), (pb, b_text)):
                if t is not None:
                    pth.write_text(t)
            tool = str(ROOT / 'tools' / 'validate_player_replay_metadata.py')
            return subprocess.run([sys.executable, tool, '--expected', str(pa), '--actual', str(pb)],
                                  capture_output=True, text=True)

    def test_cli_duplicate_key_rejected(self):
        good = json.dumps(base())
        dup = good[:-1] + ', "course": 1}'
        r = self._cli(good, dup)
        self.assertEqual(r.returncode, 1)
        out = json.loads(r.stdout)
        self.assertFalse(out['ok']); self.assertTrue(out['unreadable_b']); self.assertFalse(out['unreadable_a'])
        self.assertNotIn('Traceback', r.stderr); self.assertNotIn(H('a'), r.stdout)
        # same duplicate on side A, even with identical values
        r = self._cli(dup, good)
        self.assertEqual(r.returncode, 1); self.assertTrue(json.loads(r.stdout)['unreadable_a'])

    def test_cli_malformed_missing_and_deep(self):
        good = json.dumps(base())
        r = self._cli(good, '{"course": ')
        self.assertEqual(r.returncode, 1); self.assertTrue(json.loads(r.stdout)['unreadable_b'])
        r = self._cli(None, good)
        self.assertEqual(r.returncode, 1); self.assertTrue(json.loads(r.stdout)['unreadable_a'])
        r = self._cli(good, '[' * 200000)
        self.assertEqual(r.returncode, 1); self.assertTrue(json.loads(r.stdout)['unreadable_b'])
        self.assertNotIn('Traceback', r.stderr)

    def test_cli_valid_output_has_no_unreadable_keys(self):
        r = self._cli(json.dumps(base()), json.dumps(base()))
        self.assertEqual(r.returncode, 0); self.assertNotIn('unreadable', r.stdout)


if __name__ == '__main__':
    unittest.main()