from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from convert_player_input_trace import convert

TRACE = """tick,steer,accel,brake,in0,in1,in2
0,128,32,32,255,143,255
1,120,224,32,255,175,255
#end,2
"""


class ConvertPlayerInputTraceTests(unittest.TestCase):
    def test_preserves_all_ports_and_merges_repeated_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "trace.csv"
            dest = Path(tmp) / "inputs.txt"
            source.write_bytes(TRACE.encode("ascii"))
            convert(source, dest)
            self.assertEqual(dest.read_text().splitlines(), [
                "frames 2", "0 steer=128", "1 steer=120", "0 accel=32", "1 accel=224",
                "0-1 brake=32", "0-1 in0=255", "0 in1=143", "1 in1=175", "0-1 in2=255",
            ])
            with self.assertRaises(FileExistsError):
                convert(source, dest)

    def test_malformed_source_does_not_create_a_script(self):
        malformed = (
            TRACE[:-1], TRACE.replace("#end,2\n", ""), TRACE.replace("#end,2", "#end,3"),
            TRACE + "2,0,0,0,0,0,0\n", TRACE.replace("1,120,", "3,120,"),
            TRACE.replace("1,120,", "1,256,"), TRACE.replace("\n", "\r\n"),
        )
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "trace.csv"
            dest = Path(tmp) / "inputs.txt"
            for bad in malformed:
                with self.subTest(trace=bad):
                    source.write_bytes(bad.encode("ascii"))
                    with self.assertRaises(ValueError):
                        convert(source, dest)
                    self.assertFalse(dest.exists())

    def test_failed_write_clears_the_partial_script(self):
        def interrupted(_rows):
            yield "frames 2\n"
            raise OSError("injected output failure")

        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "trace.csv"
            dest = Path(tmp) / "inputs.txt"
            source.write_bytes(TRACE.encode("ascii"))
            with patch("convert_player_input_trace.script_for", interrupted):
                with self.assertRaisesRegex(OSError, "injected output failure"):
                    convert(source, dest)
            self.assertFalse(dest.exists())
            convert(source, dest)
            self.assertTrue(dest.is_file())


if __name__ == "__main__":
    unittest.main()
