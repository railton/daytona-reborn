"""Join synthetic GPU generations, then exercise the real digest CSV CLI."""

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_presentation_generation_join import join_presentations


def main():
    samples = [(0, 1), (8333333, 1), (16666666, 2), (25000000, 3)]
    digests = {3: "A", 1: "A", 2: "B"}
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        missing = root / "missing.csv"
        for incomplete in ({1: "A", 3: "A"}, {1: "A", 2: " ", 3: "A"}):
            try:
                rows = join_presentations(samples, incomplete)
            except ValueError as e:
                assert "generation 2" in str(e), e
            else:
                missing.write_text(str(rows), encoding="utf-8")
            assert not missing.exists()
        trace = root / "trace.csv"
        rows = join_presentations(samples, digests)
        assert [digest for _, digest in rows] == ["A", "A", "B", "A"], rows
        with trace.open("w", encoding="ascii", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("present_ns", "digest"))
            writer.writerows(rows)
        call = subprocess.run([sys.executable, "-B", str(ROOT / "tools/check_presentation_digest_stream.py"),
                               str(trace)], check=True, capture_output=True, text=True, timeout=20)
        report = json.loads(call.stdout)
        expected = {"interval_count": 3, "elapsed_ns": 25000000,
                    "sample_rate_hz": 120.0, "content_change_rate_hz": 80.0,
                    "repeated_presentations_per_second": 40.0}
        assert {key: report[key] for key in expected} == expected, report
    print("generation join and real digest CLI: ok")


if __name__ == "__main__":
    main()
