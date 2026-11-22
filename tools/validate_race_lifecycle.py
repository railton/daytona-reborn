"""Validate m2run --race-status-log evidence without mistaking exit for finish.

This checks the native session contract, not race victory or MAME parity.
Exit codes: 0 requirement met, 1 valid evidence but unmet requirement,
2 missing/malformed evidence. No ROM or generated assets are needed.
"""
import argparse
import json
from pathlib import Path
import sys


TRANSITIONS = {
    "starting": {"racing", "wrong_course", "wrong_mode", "wrong_transmission", "timed_out", "cancelled"},
    "racing": {"finished"},
    "finished": set(), "wrong_course": set(), "wrong_mode": set(),
    "wrong_transmission": set(), "timed_out": set(),
    "cancelled": set(),
}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def validate(text, *, require="returned", expect_frames=None):
    if require not in {"started", "returned"}:
        raise ValueError("require must be started or returned")
    if expect_frames is not None and (type(expect_frames) is not int or expect_frames < 0):
        raise ValueError("expect_frames must be a nonnegative integer")
    events = []
    for number, line in enumerate(text.splitlines(), 1):
        try:
            event = json.loads(line, object_pairs_hook=_unique_object)
        except (ValueError, RecursionError) as exc:
            raise ValueError(f"line {number}: invalid JSON ({exc})") from exc
        if not isinstance(event, dict) or set(event) != {"event", "frame", "status"}:
            raise ValueError(f"line {number}: expected event, frame and status fields")
        if type(event["frame"]) is not int or event["frame"] < 0:
            raise ValueError(f"line {number}: frame must be a nonnegative integer")
        if not isinstance(event["status"], str) or event["status"] not in TRANSITIONS:
            raise ValueError(f"line {number}: unknown native race status")
        events.append(event)
    if len(events) < 2 or events[0] != {"event": "begin", "frame": 0, "status": "starting"}:
        raise ValueError("expected begin at frame zero with an accepted native start")
    if events[-1]["event"] != "end":
        raise ValueError("missing end record; execution may have been interrupted")
    previous = events[0]
    for event in events[1:-1]:
        if event["event"] != "transition":
            raise ValueError("only transitions are allowed between begin and end")
        immediate_cancel = (previous == events[0] and event["frame"] == 0
                            and event["status"] == "cancelled")
        if event["frame"] <= previous["frame"] and not immediate_cancel:
            raise ValueError("transition frames must strictly increase")
        if event["status"] not in TRANSITIONS[previous["status"]]:
            raise ValueError(f"illegal transition {previous['status']} -> {event['status']}")
        previous = event
    end = events[-1]
    if end["frame"] < previous["frame"] or end["status"] != previous["status"]:
        raise ValueError("end must preserve the last observed state and frame order")
    start = next((e["frame"] for e in events[1:-1] if e["status"] == "racing"), None)
    returned = next((e["frame"] for e in events[1:-1] if e["status"] == "finished"), None)
    failures = []
    if start is None:
        failures.append("race_not_started")
    if require == "returned" and returned is None:
        failures.append("no_return_to_attract")
    if expect_frames is not None and end["frame"] != expect_frames:
        failures.append("unexpected_end_frame")
    return {"valid": True, "passed": not failures, "require": require,
            "failures": failures, "start_frame": start, "return_to_attract_frame": returned,
            "end_frame": end["frame"], "end_status": end["status"],
            "race_victory_verified": False, "arcade_parity_verified": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    parser.add_argument("--require", choices=("started", "returned"), default="returned")
    parser.add_argument("--expect-frames", type=int)
    args = parser.parse_args(argv)
    try:
        report = validate(args.journal.read_text(encoding="utf-8"),
                          require=args.require, expect_frames=args.expect_frames)
    except (OSError, UnicodeError, ValueError) as exc:
        print(json.dumps({"valid": False, "passed": False, "error": str(exc)}))
        return 2
    print(json.dumps(report))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
