from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

_START = re.compile(r"^m2run: PC solo race started at frame (\d+)\s*$")
_SUM = re.compile(
    r"^m2run: (\d+) frames, (\d+) i960 instructions \(all native\), "
    r"(\d+) TGP instructions, (\d+) interrupts, (\d+) bytes to the sound board; "
    r"(\d+(?:\.\d+)?) s \((\d+(?:\.\d+)?) frames/s\)\s*$"
)
_HASH = re.compile(r"^last screen hash ([0-9a-fA-F]+)\s*$")
_DRIVE = re.compile(r"^drive board: (\d+) commands \((.*)\)\s*$")
_BUCKET = re.compile(r"^([0-9a-fA-F]+-): (\d+)$")
_SOUND = re.compile(
    r"^sound board: (\d+) 68000 instructions \(all native\), (\d+) command bytes received\s*$"
)
_FIELDS = (
    "startup_frame", "frames", "i960", "tgp", "interrupts", "sound_bytes",
    "screen_hash", "drive_total", "m68k", "sound_cmd_bytes",
)


def parse_log(text):
    if not isinstance(text, str):
        raise ValueError("log text must be a string")
    got = {"start": [], "summary": [], "hash": [], "drive": [], "sound": []}
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        matched = False
        for key, cre in (
            ("start", _START), ("summary", _SUM), ("hash", _HASH),
            ("drive", _DRIVE), ("sound", _SOUND),
        ):
            found = cre.match(line)
            if found:
                got[key].append(found)
                matched = True
                break
        if not matched:
            raise ValueError("unrecognized log line")
    for key, rows in got.items():
        if len(rows) != 1:
            raise ValueError(f"expected exactly one {key}, found {len(rows)}")
    summary, drive, sound = got["summary"][0], got["drive"][0], got["sound"][0]
    buckets = {}
    histogram = drive.group(2).strip()
    if histogram:
        for part in histogram.split(","):
            piece = part.strip()
            bucket = _BUCKET.match(piece)
            if not bucket:
                raise ValueError("malformed drive histogram")
            name = bucket.group(1).lower()
            if name in buckets:
                raise ValueError("duplicate drive bucket")
            buckets[name] = int(bucket.group(2))
    total = int(drive.group(1))
    if sum(buckets.values()) != total:
        raise ValueError("drive histogram does not sum to command total")
    return {
        "startup_frame": int(got["start"][0].group(1)),
        "frames": int(summary.group(1)),
        "i960": int(summary.group(2)),
        "tgp": int(summary.group(3)),
        "interrupts": int(summary.group(4)),
        "sound_bytes": int(summary.group(5)),
        "elapsed_s": float(summary.group(6)),
        "fps": float(summary.group(7)),
        "screen_hash": got["hash"][0].group(1).lower(),
        "drive_total": total,
        "drive": buckets,
        "m68k": int(sound.group(1)),
        "sound_cmd_bytes": int(sound.group(2)),
    }


def _path(value):
    if isinstance(value, Path):
        return value
    if isinstance(value, str):
        return Path(value)
    raise ValueError("expected a path")


def _read_log(path):
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"missing log: {path}") from exc
    return parse_log(text)


def _hashes(directory):
    try:
        if not directory.is_dir():
            raise ValueError(f"missing frames directory: {directory}")
        entries = list(directory.iterdir())
    except OSError as exc:
        raise ValueError(f"missing frames directory: {directory}") from exc
    names = []
    for path in entries:
        try:
            is_sample = path.is_file() and path.name.startswith("run_") and path.suffix == ".rgb"
        except OSError as exc:
            raise ValueError(f"missing sample: {path}") from exc
        if is_sample:
            names.append(path)
    if not names:
        raise ValueError(f"empty capture set: {directory}")
    out = {}
    for path in names:
        try:
            size = path.stat().st_size
        except OSError as exc:
            raise ValueError(f"missing sample: {path}") from exc
        if size == 0:
            raise ValueError(f"zero-byte capture: {path}")
        digest = hashlib.sha256()
        try:
            with path.open("rb") as handle:
                for chunk in iter(lambda: handle.read(65536), b""):
                    digest.update(chunk)
        except OSError as exc:
            raise ValueError(f"missing sample: {path}") from exc
        out[path.name] = digest.hexdigest()
    return out


def compare(before_log, after_log, before_frames, after_frames):
    before, after = _read_log(_path(before_log)), _read_log(_path(after_log))
    bframes, aframes = _hashes(_path(before_frames)), _hashes(_path(after_frames))
    diffs = []
    for name in _FIELDS:
        if before[name] != after[name]:
            diffs.append({"name": name, "before": before[name], "after": after[name]})
    if before["drive"] != after["drive"]:
        diffs.append({"name": "drive", "before": before["drive"], "after": after["drive"]})
    for name in sorted(set(bframes) | set(aframes)):
        bh, ah = bframes.get(name), aframes.get(name)
        if bh != ah:
            diffs.append({"name": "frame:" + name, "before": bh, "after": ah})
    return {"equal": not diffs, "differences": diffs}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Compare m2run startup logs and RGB captures")
    parser.add_argument("before_log")
    parser.add_argument("after_log")
    parser.add_argument("--before-frames", required=True)
    parser.add_argument("--after-frames", required=True)
    args = parser.parse_args(argv)
    try:
        report = compare(args.before_log, args.after_log, args.before_frames, args.after_frames)
    except ValueError as exc:
        json.dump({"equal": False, "differences": [], "error": str(exc)}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout)
    sys.stdout.write("\n")
    return 0 if report["equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
