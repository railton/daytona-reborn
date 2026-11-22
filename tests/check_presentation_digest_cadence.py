"""Exercise the real presentation digest CLI with synthetic cadence data."""

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile


TOOL = Path(__file__).resolve().parents[1] / "tools/check_presentation_digest_stream.py"


def invoke(path, rows):
    with path.open("w", encoding="ascii", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("present_ns", "digest"))
        writer.writerows(rows)
    return subprocess.run([sys.executable, "-B", str(TOOL), str(path)],
                          capture_output=True, text=True, timeout=20)


def main():
    rows = [(i * 1_000_000_000 // 120, i * 5752 // 12000) for i in range(1201)]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cadence.csv"
        run = invoke(path, rows)
        assert run.returncode == 0, run.stderr
        actual = json.loads(run.stdout)
        expected = {"presentations": 1201, "interval_count": 1200,
                    "elapsed_ns": 10_000_000_000, "sample_rate_hz": 120.0,
                    "content_changes": 575, "repeated_presentations": 625,
                    "content_change_rate_hz": 57.5, "repeated_presentations_per_second": 62.5}
        assert {key: actual[key] for key in expected} == expected, actual

        # Shift one interior stamp. Endpoints and digests stay identical.
        clustered = rows.copy()
        stamp, digest = clustered[1]
        clustered[1] = (stamp + 7_000_000, digest)
        run = invoke(path, clustered)
        assert run.returncode == 0, run.stderr
        burst = json.loads(run.stdout)
        assert {key: burst[key] for key in expected} == expected, burst
        assert burst["mean_interval_ns"] == actual["mean_interval_ns"]
        assert actual["min_interval_ns"] == 8_333_333
        assert actual["max_interval_ns"] == 8_333_334
        assert burst["min_interval_ns"] == 1_333_333
        assert burst["max_interval_ns"] == 15_333_333

        for bad in ([rows[0], rows[0]], [rows[1], rows[0]]):
            failure = invoke(path, bad)
            assert failure.returncode == 2 and "strictly increasing" in failure.stderr, failure
    print("synthetic presentation cadence: 120.0 submitted, 57.5 changed, 62.5 repeated per second")


if __name__ == "__main__":
    main()
