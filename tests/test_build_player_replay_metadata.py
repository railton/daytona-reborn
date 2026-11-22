import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_player_replay_metadata as b
import validate_player_replay_metadata as v

TOOL = str(ROOT / 'tools' / 'build_player_replay_metadata.py')


class BuildTests(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.d = Path(self._td.name)
        self.files = {'script': b'frames 600\nSCRIPT', 'exe': b'EXE', 'r1': b'ROM-ONE', 'r2': b'ROM-TWO',
                      'ee': b'EEPROM', 'br': b'BACKUP'}
        for k, val in self.files.items():
            (self.d / k).write_bytes(val)

    def tearDown(self):
        self._td.cleanup()

    def P(self, k):
        return str(self.d / k)

    def meta(self, **kw):
        a = dict(course=1, transmission='automatic', frames=600, script=self.P('script'),
                 exe=self.P('exe'), roms=['a=' + self.P('r1'), 'b=' + self.P('r2')],
                 eeprom=self.P('ee'), backup_ram=self.P('br'))
        a.update(kw)
        return b.build(**a)

    def test_valid_for_validator_and_deterministic(self):
        m = self.meta()
        self.assertEqual(v.invalid_fields(m), ([], []))
        self.assertTrue(v.compare(m, self.meta())['ok'])

    def test_no_paths_or_names_in_output(self):
        s = json.dumps(self.meta())
        self.assertNotIn(str(self.d), s)
        self.assertNotIn('ROM-ONE', s)

    def test_rom_order_independent(self):
        m1 = self.meta()
        m2 = self.meta(roms=['b=' + self.P('r2'), 'a=' + self.P('r1')])
        self.assertEqual(m1, m2)

    def test_rom_name_and_content_matter(self):
        base = self.meta()['rom_set_sha256']
        self.assertNotEqual(base, self.meta(roms=['a=' + self.P('r2'), 'b=' + self.P('r1')])['rom_set_sha256'])
        self.assertNotEqual(base, self.meta(roms=['a=' + self.P('r1'), 'c=' + self.P('r2')])['rom_set_sha256'])
        self.assertNotEqual(base, self.meta(roms=['a=' + self.P('r1')])['rom_set_sha256'])

    def test_initial_state_variation(self):
        base = self.meta()
        (self.d / 'ee').write_bytes(b'EEPROM2')
        m = self.meta()
        self.assertEqual(v.compare(base, m)['differing'], ['nvram_sha256'])
        (self.d / 'ee').write_bytes(b'EEPROM')
        (self.d / 'br').write_bytes(b'BACKUP2')
        self.assertEqual(v.compare(base, self.meta())['differing'], ['nvram_sha256'])

    def test_eeprom_backup_not_interchangeable(self):
        a = self.meta()
        c = self.meta(eeprom=self.P('br'), backup_ram=self.P('ee'))
        self.assertNotEqual(a['nvram_sha256'], c['nvram_sha256'])

    def test_script_and_build_digest_changes(self):
        base = self.meta()
        (self.d / 'script').write_bytes(b'frames 600\nSCRIPT2')
        self.assertEqual(v.compare(base, self.meta())['differing'], ['script_sha256'])
        (self.d / 'script').write_bytes(b'frames 600\nSCRIPT')
        (self.d / 'exe').write_bytes(b'EXE2')
        self.assertEqual(v.compare(base, self.meta())['differing'], ['build_sha256'])

    def test_course_and_gearbox_explicit(self):
        base = self.meta()
        self.assertEqual(v.compare(base, self.meta(course=2))['differing'], ['course'])
        self.assertEqual(v.compare(base, self.meta(transmission='manual'))['differing'], ['transmission'])
        for kw in ({'course': 0}, {'course': 4}, {'course': True}, {'transmission': 'x'},
                   {'frames': 0}, {'frames': True}):
            with self.assertRaises(b.MetaError):
                self.meta(**kw)

    def test_invalid_and_duplicate_names(self):
        for bad in (['x'], ['=' + self.P('r1')], ['a='], ['../a=' + self.P('r1')], ['a b=' + self.P('r1')],
                    ['-a=' + self.P('r1')], [('a' * 65) + '=' + self.P('r1')], []):
            with self.assertRaises(b.MetaError):
                self.meta(roms=bad)
        with self.assertRaises(b.MetaError) as cm:
            self.meta(roms=['a=' + self.P('r1'), 'a=' + self.P('r2')])
        self.assertEqual(str(cm.exception), 'duplicate_name:rom')

    def test_unreadable_error_has_no_path(self):
        with self.assertRaises(b.MetaError) as cm:
            self.meta(exe=str(self.d / 'missing'))
        self.assertEqual(str(cm.exception), 'unreadable:build_sha256')
        self.assertNotIn(str(self.d), str(cm.exception))

    def cli(self, *extra, rom=None):
        out = self.d / 'out.json'
        cmd = [sys.executable, TOOL, '--course', '1', '--transmission', 'manual', '--frames', '600',
               '--script', self.P('script'), '--exe', self.P('exe'),
               '--eeprom', self.P('ee'), '--backup-ram', self.P('br'), '--out', str(out)]
        for r in (rom if rom is not None else ['a=' + self.P('r1')]):
            cmd += ['--rom', r]
        return subprocess.run(cmd + list(extra), capture_output=True, text=True), out

    def test_cli_ok_writes_valid_json_without_values(self):
        r, out = self.cli()
        self.assertEqual(r.returncode, 0)
        m = json.loads(out.read_text())
        self.assertEqual(v.invalid_fields(m), ([], []))
        self.assertNotIn(m['script_sha256'], r.stdout)
        self.assertNotIn(str(self.d), r.stdout + r.stderr)

    def test_cli_records_optional_mode(self):
        run, out = self.cli('--race-mode', 'time-attack')
        self.assertEqual(run.returncode, 0)
        metadata = json.loads(out.read_text())
        self.assertEqual(metadata['race_mode'], 'time-attack')
        self.assertNotIn('race_mode', self.meta())
        with self.assertRaisesRegex(b.MetaError, 'invalid:race_mode'):
            self.meta(race_mode='unknown')

    def test_cli_errors_no_traceback_no_output_file(self):
        r, out = self.cli(rom=['a=' + self.P('r1'), 'a=' + self.P('r2')])
        self.assertEqual(r.returncode, 1)
        self.assertEqual(json.loads(r.stdout), {'error': 'duplicate_name:rom', 'ok': False})
        self.assertFalse(out.exists())
        self.assertNotIn('Traceback', r.stderr)

    def test_cli_existing_out_preserved(self):
        out = self.d / 'out.json'
        out.write_bytes(b'KEEP')
        r, _ = self.cli()
        self.assertEqual(r.returncode, 1)
        self.assertEqual(json.loads(r.stdout), {'error': 'exists:out', 'ok': False})
        self.assertEqual(out.read_bytes(), b'KEEP')
        self.assertNotIn(str(self.d), r.stdout + r.stderr)
        self.assertNotIn('"ok": true', r.stdout)

    def test_cli_out_same_as_input_preserved(self):
        for k in ('script', 'exe', 'r1', 'ee', 'br'):
            before = (self.d / k).read_bytes()
            cmd = [sys.executable, TOOL, '--course', '1', '--transmission', 'manual', '--frames', '600',
                   '--script', self.P('script'), '--exe', self.P('exe'), '--rom', 'a=' + self.P('r1'),
                   '--eeprom', self.P('ee'), '--backup-ram', self.P('br'), '--out', self.P(k)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertEqual(json.loads(r.stdout), {'error': 'exists:out', 'ok': False})
            self.assertEqual((self.d / k).read_bytes(), before)

    def test_cli_unwritable_out_not_ok(self):
        out = str(self.d / 'missing_dir' / 'o.json')
        cmd = [sys.executable, TOOL, '--course', '1', '--transmission', 'manual', '--frames', '600',
               '--script', self.P('script'), '--exe', self.P('exe'), '--rom', 'a=' + self.P('r1'),
               '--eeprom', self.P('ee'), '--backup-ram', self.P('br'), '--out', out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(json.loads(r.stdout), {'error': 'unwritable:out', 'ok': False})
        self.assertNotIn(str(self.d), r.stdout + r.stderr)

    def test_script_frames_must_match(self):
        for data, err in ((b'frames 601\nX', 'mismatch:frames'), (b'X\n', 'missing:script_frames'),
                          (b'frames 600\nframes 600\n', 'invalid:script_frames'),
                          (b'frames abc\n', 'invalid:script_frames'),
                          (b'# frames 9\nframes 600\r\nX', None)):
            (self.d / 'script').write_bytes(data)
            if err is None:
                self.assertEqual(self.meta()['frames'], 600)
                continue
            with self.assertRaises(b.MetaError) as cm:
                self.meta()
            self.assertEqual(str(cm.exception), err)
            r, out = self.cli()
            self.assertEqual(r.returncode, 1)
            self.assertEqual(json.loads(r.stdout), {'error': err, 'ok': False})
            self.assertFalse(out.exists())
            self.assertNotIn(str(self.d), r.stdout + r.stderr)

    def test_script_digest_is_of_bytes_read(self):
        import hashlib
        self.assertEqual(self.meta()['script_sha256'],
                         hashlib.sha256(self.files['script']).hexdigest())


if __name__ == '__main__':
    unittest.main()
