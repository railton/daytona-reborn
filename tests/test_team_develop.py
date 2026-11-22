import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from team_develop import apply_packet, digest, packet, safe_path, perform


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()

    def test_packet_requires_explicit_idle(self):
        with self.assertRaises(ValueError): packet('{"files":[]}')
        self.assertTrue(packet('```json\n{"idle":true}\n```')['idle'])

    def test_paths_cannot_escape_or_use_windows_streams(self):
        for path in ('../x','/x','C:/x','foo:stream','a\\b','.git/config','a/../b'):
            with self.subTest(path=path),self.assertRaises(ValueError): safe_path(self.root,path)

    def test_validate_all_before_writing(self):
        value={'files':[{'path':'first.py','content':'ok'},{'path':'../bad','content':'bad'}]}
        with self.assertRaises(ValueError): apply_packet(self.root,{'first.py':None},value)
        self.assertFalse((self.root/'first.py').exists())

    def test_missing_file_and_duplicate_case_fail(self):
        with self.assertRaises(ValueError):
            apply_packet(self.root,{'a':None,'b':None},{'files':[{'path':'a','content':'x'}]})
        with self.assertRaises(ValueError):
            apply_packet(self.root,{'a':None,'A':None},{'files':[{'path':'a','content':'x'},{'path':'A','content':'y'}]})

    def test_content_changed_after_assignment_is_preserved(self):
        path=self.root/'a'; path.write_text('existing',encoding='utf-8')
        with self.assertRaises(ValueError):
            apply_packet(self.root,{'a':None},{'files':[{'path':'a','content':'overwrite'}]})
        self.assertEqual(path.read_text(),'existing')

    def test_exact_files_delivered_and_hashed(self):
        value={'files':[{'path':'tools/a.py','content':'print(1)\n'}]}
        hashes=apply_packet(self.root,{'tools/a.py':None},value)
        self.assertEqual(hashes['tools/a.py'],digest(self.root/'tools/a.py'))

    def test_broker_supplies_reserved_paths_when_prompt_omits_them(self):
        expected = {'tools/helper.py': None, 'tests/test_helper.py': None}
        spec = dict(cwd=str(self.root), base='test-base', kind='code',
                    expected=expected, prompt='Implement a helper.', tests=[])
        def fake_execute(spec, backend, context, deadline):
            self.assertIn('Assigned output paths (return exactly these, no others): ' +
                          json.dumps(sorted(expected), ensure_ascii=False), context)
            return json.dumps(dict(idle=True, files=[
                dict(path=name, content='# test fixture\n') for name in expected])), 'local-test', []
        with patch('team_develop.run_command', return_value={'exit': 0, 'stdout': 'test-base'}), \
             patch('team_develop.execute', side_effect=fake_execute):
            result = perform(spec, {}, self.root / 'artifacts')
        self.assertEqual(result['state'], 'tested')
        self.assertEqual(set(result['files']), set(expected))

    def test_wrong_reserved_path_rejects_whole_transport_delivery(self):
        spec = dict(cwd=str(self.root), base='test-base', kind='code',
                    expected={'tools/helper.py': None, 'tests/test_helper.py': None},
                    prompt='Implement a helper.', tests=[])
        answer = json.dumps(dict(idle=True, files=[
            dict(path='tools/helper.py', content='good'),
            dict(path='tests/other.py', content='wrong reserved path')]))
        with patch('team_develop.run_command', return_value={'exit': 0, 'stdout': 'test-base'}), \
             patch('team_develop.execute', return_value=(answer, 'local-test', [])):
            with self.assertRaisesRegex(ValueError, 'unexpected'):
                perform(spec, {}, self.root / 'artifacts')
        self.assertFalse((self.root / 'tools/helper.py').exists())
        self.assertFalse((self.root / 'tests/other.py').exists())
        self.assertTrue((self.root / 'artifacts/delivery.json').exists())


if __name__=='__main__': unittest.main()
