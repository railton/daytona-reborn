import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.compare_startup_runs import compare, main, parse_log

BASE = dict(
    startup=837, frames=1400, i960=212172801, tgp=28230359, interrupts=2093,
    sound_bytes=420, elapsed="5.96", fps=235, screen="8ac3c65c1383c1ee",
    m68k=18301875, cmd=420, drive_total=9,
    buckets="0-: 4, 1-: 1, 2-: 1, 3-: 1, 7-: 1, f-: 1",
)


def render(overrides=None):
    data = dict(BASE)
    if overrides:
        data.update(overrides)
    return (
        f"m2run: PC solo race started at frame {data['startup']}\n"
        f"m2run: {data['frames']} frames, {data['i960']} i960 instructions (all native), "
        f"{data['tgp']} TGP instructions, {data['interrupts']} interrupts, "
        f"{data['sound_bytes']} bytes to the sound board; {data['elapsed']} s ({data['fps']} frames/s)\n"
        f"  last screen hash {data['screen']}\n"
        f"  drive board: {data['drive_total']} commands ({data['buckets']})\n"
        f"  sound board: {data['m68k']} 68000 instructions (all native), {data['cmd']} command bytes received\n"
    )


class CompareStartupRunsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write_case(self, name, text, files):
        side = self.root / name
        frames = side / "frames"
        frames.mkdir(parents=True)
        log = side / "run.log"
        log.write_text(text, encoding="utf-8")
        for filename, payload in files.items():
            (frames / filename).write_bytes(payload)
        return log, frames

    def test_equal_despite_speed_change(self):
        files = {"run_a.rgb": b"rgb-sample", "run_b.rgb": b"\x00\x01"}
        before = self.write_case("before", render(), files)
        after = self.write_case("after", render({"elapsed": "9.50", "fps": "100.5"}), files)
        report = compare(before[0], after[0], before[1], after[1])
        self.assertTrue(report["equal"])
        self.assertEqual(report["differences"], [])
        parsed = parse_log(render())
        self.assertEqual(parsed["startup_frame"], 837)
        self.assertEqual(parsed["screen_hash"], "8ac3c65c1383c1ee")
        self.assertEqual(parsed["elapsed_s"], 5.96)
        self.assertEqual(parsed["fps"], 235.0)

    def test_counter_mismatch(self):
        files = {"run_a.rgb": b"x"}
        before = self.write_case("before", render(), files)
        after = self.write_case("after", render({"interrupts": 2094}), files)
        report = compare(str(before[0]), str(after[0]), str(before[1]), str(after[1]))
        self.assertFalse(report["equal"])
        self.assertIn("interrupts", [item["name"] for item in report["differences"]])

    def test_frame_content_mismatch(self):
        before = self.write_case("before", render(), {"run_a.rgb": b"one"})
        after = self.write_case("after", render(), {"run_a.rgb": b"two"})
        report = compare(before[0], after[0], before[1], after[1])
        self.assertFalse(report["equal"])
        self.assertTrue(any(item["name"] == "frame:run_a.rgb" for item in report["differences"]))

    def test_missing_sample(self):
        before = self.write_case("before", render(), {"run_a.rgb": b"one", "run_b.rgb": b"two"})
        after = self.write_case("after", render(), {"run_a.rgb": b"one"})
        report = compare(before[0], after[0], before[1], after[1])
        self.assertFalse(report["equal"])
        self.assertTrue(any(item["before"] and item["after"] is None for item in report["differences"]))

    def test_empty_capture_set(self):
        before = self.write_case("before", render(), {"run_a.rgb": b"one"})
        after = self.write_case("after", render(), {})
        with self.assertRaises(ValueError):
            compare(before[0], after[0], before[1], after[1])

    def test_truncated_and_duplicate_logs(self):
        with self.assertRaises(ValueError):
            parse_log(render().split("sound board")[0])
        with self.assertRaises(ValueError):
            parse_log(render() + "m2run: PC solo race started at frame 1\n")
        with self.assertRaises(ValueError):
            compare(self.root / "missing.log", self.root / "missing.log", self.root, self.root)

    def test_cli_exit_codes(self):
        files = {"run_a.rgb": b"z"}
        before = self.write_case("before", render(), files)
        changed = self.write_case("changed", render({"i960": 1}), files)
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = main([str(before[0]), str(changed[0]), "--before-frames", str(before[1]), "--after-frames", str(changed[1])])
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(buf.getvalue())["equal"])
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = main([str(self.root / "nope"), str(changed[0]), "--before-frames", str(before[1]), "--after-frames", str(changed[1])])
        self.assertEqual(code, 2)

    def test_zero_command_histogram(self):
        text = render({"drive_total": 0, "buckets": ""})
        self.assertIn("drive board: 0 commands ()", text)
        parsed = parse_log(text)
        self.assertEqual(parsed["drive_total"], 0)
        self.assertEqual(parsed["drive"], {})
        files = {"run_a.rgb": b"rgb-sample"}
        before = self.write_case("before", text, files)
        after = self.write_case("after", text, files)
        report = compare(before[0], after[0], before[1], after[1])
        self.assertTrue(report["equal"])
        self.assertEqual(report["differences"], [])

    def test_drive_histogram_validation(self):
        cases = (
            (render({"buckets": "nope"}), "malformed drive histogram"),
            (render({"buckets": "0-: 4,, 1-: 1, 2-: 1, 3-: 1, 7-: 1, f-: 1"}), "malformed drive histogram"),
            (render({"drive_total": 2, "buckets": "0-: 1, 0-: 1"}), "duplicate drive bucket"),
            (render({"drive_total": 2, "buckets": "A-: 1, a-: 1"}), "duplicate drive bucket"),
            (render({"buckets": "0-: 4, 1-: 1"}), "drive histogram does not sum to command total"),
            (render({"drive_total": 1, "buckets": ""}), "drive histogram does not sum to command total"),
        )
        for text, fragment in cases:
            with self.assertRaises(ValueError) as caught:
                parse_log(text)
            self.assertIn(fragment, str(caught.exception))

    def test_each_compared_field_mismatch(self):
        files = {"run_a.rgb": b"x"}
        cases = (
            ("startup_frame", {"startup": 838}, 837, 838),
            ("frames", {"frames": 1401}, 1400, 1401),
            ("i960", {"i960": 1}, 212172801, 1),
            ("tgp", {"tgp": 1}, 28230359, 1),
            ("interrupts", {"interrupts": 2094}, 2093, 2094),
            ("sound_bytes", {"sound_bytes": 421}, 420, 421),
            ("screen_hash", {"screen": "deadbeef"}, "8ac3c65c1383c1ee", "deadbeef"),
            ("m68k", {"m68k": 1}, 18301875, 1),
            ("sound_cmd_bytes", {"cmd": 421}, 420, 421),
        )
        for name, overrides, before_value, after_value in cases:
            with self.subTest(field=name):
                before = self.write_case(f"before-{name}", render(), files)
                after = self.write_case(f"after-{name}", render(overrides), files)
                report = compare(before[0], after[0], before[1], after[1])
                self.assertFalse(report["equal"])
                matched = [item for item in report["differences"] if item["name"] == name]
                self.assertEqual(matched, [{"name": name, "before": before_value, "after": after_value}])

        same_total = "0-: 3, 1-: 2, 2-: 1, 3-: 1, 7-: 1, f-: 1"
        before = self.write_case("before-drive-histogram", render(), files)
        after = self.write_case("after-drive-histogram", render({"buckets": same_total}), files)
        report = compare(before[0], after[0], before[1], after[1])
        self.assertEqual([item["name"] for item in report["differences"]], ["drive"])

        fewer = "0-: 3, 1-: 1, 2-: 1, 3-: 1, 7-: 1, f-: 1"
        before = self.write_case("before-drive-total", render(), files)
        after = self.write_case("after-drive-total", render({"drive_total": 8, "buckets": fewer}), files)
        report = compare(before[0], after[0], before[1], after[1])
        totals = [item for item in report["differences"] if item["name"] == "drive_total"]
        self.assertEqual(totals, [{"name": "drive_total", "before": 9, "after": 8}])
        self.assertIn("drive", [item["name"] for item in report["differences"]])

    def test_cli_success_and_error_json(self):
        files = {"run_a.rgb": b"z"}
        before = self.write_case("before", render(), files)
        after = self.write_case("after", render({"elapsed": "1.0", "fps": 1}), files)
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = main([
                str(before[0]), str(after[0]),
                "--before-frames", str(before[1]),
                "--after-frames", str(after[1]),
            ])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(buf.getvalue()), {"equal": True, "differences": []})

        buf = io.StringIO()
        with redirect_stdout(buf):
            code = main([
                str(self.root / "nope"), str(after[0]),
                "--before-frames", str(before[1]),
                "--after-frames", str(after[1]),
            ])
        self.assertEqual(code, 2)
        payload = json.loads(buf.getvalue())
        self.assertFalse(payload["equal"])
        self.assertEqual(payload["differences"], [])
        self.assertIn("missing log", payload["error"])

    def test_zero_byte_capture_rejected(self):
        empty = {"run_a.rgb": b"", "run_b.rgb": b""}
        before = self.write_case("before", render(), empty)
        after = self.write_case("after", render(), empty)
        with self.assertRaises(ValueError) as caught:
            compare(before[0], after[0], before[1], after[1])
        self.assertIn("zero-byte capture", str(caught.exception))

    def test_frames_iterdir_oserror_is_valueerror(self):
        files = {"run_a.rgb": b"one"}
        before = self.write_case("before", render(), files)
        after = self.write_case("after", render(), files)
        real_iterdir = Path.iterdir

        def boom(path):
            if path == after[1]:
                raise OSError("unreadable")
            return real_iterdir(path)

        with mock.patch.object(Path, "iterdir", boom):
            with self.assertRaises(ValueError) as caught:
                compare(before[0], after[0], before[1], after[1])
        self.assertIn("missing frames directory", str(caught.exception))
        buf = io.StringIO()
        with mock.patch.object(Path, "iterdir", boom):
            with redirect_stdout(buf):
                code = main([
                    str(before[0]), str(after[0]),
                    "--before-frames", str(before[1]),
                    "--after-frames", str(after[1]),
                ])
        self.assertEqual(code, 2)
        payload = json.loads(buf.getvalue())
        self.assertFalse(payload["equal"])
        self.assertEqual(payload["differences"], [])
        self.assertIn("missing frames directory", payload["error"])

    def test_frames_stat_permission_error_is_json(self):
        before = self.write_case("before", render(), {"run_a.rgb": b"one"})
        after = self.write_case("after", render(), {"run_a.rgb": b"one"})
        buf = io.StringIO()
        with mock.patch.object(Path, "is_dir", side_effect=PermissionError(13, "denied")):
            with redirect_stdout(buf):
                code = main([str(before[0]), str(after[0]),
                    "--before-frames", str(before[1]), "--after-frames", str(after[1])])
        self.assertEqual(code, 2)
        report = json.loads(buf.getvalue())
        self.assertFalse(report["equal"])
        self.assertEqual(report["differences"], [])
        self.assertIn("missing frames directory", report["error"])
