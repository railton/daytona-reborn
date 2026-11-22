import csv
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from presentmon_qpc_match import match_returns


class Matching(unittest.TestCase):
    def test_exact_contract(self):
        ns = [0, 8_333_333, 16_666_666, 25_000_000]
        returns = [Fraction(980) + Fraction(stamp, 100) for stamp in ns]
        offsets = Fraction(999), Fraction(1001)
        args = 10_000_000, *offsets
        self.assertEqual(match_returns(ns, returns, *args), [0, 1, 2, 3])
        with self.assertRaisesRegex(ValueError, "multiple"):
            match_returns(ns, [*returns[:2], returns[1] + 10, *returns[2:]], *args)
        with self.assertRaisesRegex(ValueError, "no ETW"):
            match_returns(ns, [*returns[:1], *returns[2:]], *args)
        with self.assertRaisesRegex(ValueError, "consecutive"):
            match_returns(ns, [*returns[:1], Fraction(30_000), *returns[1:]], *args)
        with self.assertRaisesRegex(ValueError, "strictly increasing"):
            match_returns([0, 0], returns, *args)
        with self.assertRaisesRegex(ValueError, "strictly increasing"):
            match_returns(ns, returns[::-1], *args)
        with self.assertRaisesRegex(ValueError, "frequency"):
            match_returns(ns, returns, 0, *offsets)
        with self.assertRaisesRegex(ValueError, "must not exceed"):
            match_returns(ns, returns, 10_000_000, *offsets[::-1])

    def test_cli_and_dropped_event(self):
        tool = TOOLS / "check_presentmon_source_pair.py"
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, etw, log = (root / name for name in ("source.csv", "etw.csv", "run.log"))
            source.write_text("present_ns,digest\n0,a\n10000000,b\n20000000,a\n30000000,c\n")
            calibration = (
                "daytona: present clock begin ticks_ns=0 counter_min=999 counter_max=1001 frequency=10000000\n"
                "daytona: present clock end ticks_ns=30000000 counter_min=300999 counter_max=301001 frequency=10000000\n"
            )
            summary = "daytona: hardware present trace samples=4 generations=4 (run ended)\n"
            log.write_text(calibration + summary)
            rows = [(3, "0x2", 980 + i * 100_000, 0, int(i == 1), 1)
                    for i in range(4)]

            def run(events, *options):
                with etw.open("w", newline="") as file:
                    writer = csv.writer(file)
                    writer.writerow(("ProcessID", "SwapChainAddress", "QPCTime",
                                     "msInPresentAPI", "Dropped", "msUntilDisplayed"))
                    writer.writerows(events)
                return subprocess.run([sys.executable, "-B", str(tool), str(source),
                                       str(etw), str(log), *options], capture_output=True, text=True,
                                      timeout=10)

            good = run(rows)
            self.assertEqual(good.returncode, 0, good.stderr)
            report = json.loads(good.stdout)
            self.assertEqual(report["etw_displayed_events"], 3)
            self.assertEqual(report["dropped_in_segment"], 1)
            self.assertEqual(report["changed_source_on_displayed_events"], 1)
            self.assertAlmostEqual(report["displayed_events_per_second"], 2 / 0.03)

            clustered = [rows[0], (3, "0x2", 990, 0, 0, 1), *rows[1:]]
            ambiguous = run(clustered)
            self.assertEqual(ambiguous.returncode, 1)
            self.assertIn("multiple", ambiguous.stderr)
            global_match = run(clustered, "--contiguous")
            self.assertEqual(global_match.returncode, 0, global_match.stderr)
            recovered = json.loads(global_match.stdout)
            self.assertEqual(recovered["match_policy"], "contiguous")
            self.assertEqual(recovered["unique_matched_events"], 4)
            self.assertEqual(recovered["dropped_in_segment"], 1)

            unstable = run([(3, "0x2", 1050, 0, 0, 1), *rows[1:]])
            self.assertEqual(unstable.returncode, 1)
            self.assertIn("uncertain QPC offset", unstable.stderr)

            log.write_text(calibration + summary.replace("samples=4", "samples=5"))
            self.assertIn("sample count", run(rows).stderr)

            log.write_text(calibration + summary + "daytona: incomplete hardware present trace\n")
            self.assertIn("did not finish successfully", run(rows).stderr)


if __name__ == "__main__":
    unittest.main()
