import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

tool = Path(__file__).resolve().parents[1] / "tools" / "check_scene_camera_holdout_spatial.py"
with tempfile.TemporaryDirectory() as tmp:
    trace = Path(tmp) / "test.csv"
    fields = ["frame", "source", "vertices", "oba", "obc", "mode"]
    fields += [f"matrix{i}" for i in range(12)]
    with trace.open("w", encoding="utf-8", newline="") as stream:
        rows = csv.writer(stream)
        rows.writerow(fields)
        for tick in (10, 11):
            for index, x in enumerate((0, -1, 1, 0, 1, 2, -4, -7)):
                key = min(index, 6)
                if tick == 11 and index == 7:
                    continue
                if tick == 11:
                    x += 5 if index == 5 else 1
                rows.writerow([tick, "ROM", 3, f"{key:02}", "0", "0", 1,0,0,0,1,0,0,0,1,x,0,-10])
    call = subprocess.run([sys.executable, "-B", str(tool), str(trace), "--start", "10", "--end", "11"],
                          check=True, capture_output=True, text=True, timeout=20)
    actual = json.loads(call.stdout)
    assert actual == {"pairs": 1, "both_eligible": 1, "both_supported": 1,
                      "negative": {"total": 1, "inliers": 1},
                      "nonnegative": {"total": 2, "inliers": 1}}, actual
    print("synthetic CSV fixture passed")
