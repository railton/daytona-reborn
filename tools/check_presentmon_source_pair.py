"""Correlate Daytona source textures with one PresentMon v1 ETW swapchain."""

import argparse
import csv
from dataclasses import dataclass
from fractions import Fraction
import json
from pathlib import Path
import re
import statistics

from check_presentation_digest_stream import parse_presentation_digest_csv
from presentmon_qpc_match import match_contiguous_returns, match_returns


CLOCK = re.compile(
    r"daytona: present clock (begin|end) ticks_ns=(\d+)"
    r" counter_min=(\d+) counter_max=(\d+) frequency=(\d+)"
)
TRACE_SUMMARY = re.compile(
    r"^daytona: hardware present trace samples=(\d+) generations=\d+ "
    r"\((?:run ended|image size changed|left eligible race)\)$",
    re.MULTILINE,
)
ETW_COLUMNS = {
    "ProcessID", "SwapChainAddress", "QPCTime", "msInPresentAPI",
    "Dropped", "msUntilDisplayed",
}


@dataclass(frozen=True)
class Calibration:
    begin_ns: int
    end_ns: int
    frequency: int
    offset_low: Fraction
    offset_high: Fraction


def calibrate(log):
    matches = [CLOCK.fullmatch(line) for line in log.splitlines()
               if line.startswith("daytona: present clock ")]
    if (len(matches) != 2 or not all(matches)
            or [match[1] for match in matches] != ["begin", "end"]):
        raise ValueError("expected one well-formed begin and end calibration")
    begin, end = [tuple(map(int, match.groups()[1:])) for match in matches]
    if begin[0] >= end[0] or begin[3] <= 0 or begin[3] != end[3]:
        raise ValueError("invalid clock interval or QPC frequency")
    frequency = begin[3]
    bounds = []
    for ns, low, high, _ in (begin, end):
        if low > high:
            raise ValueError("inverted QPC calibration bracket")
        ticks = Fraction(ns * frequency, 1_000_000_000)
        bounds.append((low - ticks, high - ticks))
    low = max(bound[0] for bound in bounds)
    high = min(bound[1] for bound in bounds)
    if low > high:
        raise ValueError("QPC calibration brackets do not intersect")
    return Calibration(begin[0], end[0], frequency, low, high)


def read_events(path, frequency):
    with path.open(newline="", encoding="utf-8-sig") as file:
        reader = csv.DictReader(file, strict=True)
        names = reader.fieldnames or []
        if len(set(names)) != len(names) or not ETW_COLUMNS <= set(names):
            raise ValueError("expected PresentMon v1 CSV with QPC and display fields")
        identity = None
        returns = []
        display_times = []
        for row in reader:
            if None in row or any(row[key] is None for key in ETW_COLUMNS):
                raise ValueError(f"malformed PresentMon row {reader.line_num}")
            process, swapchain = int(row["ProcessID"]), int(row["SwapChainAddress"], 16)
            qpc = int(row["QPCTime"])
            duration_ms = Fraction(row["msInPresentAPI"])
            dropped = row["Dropped"]
            if min(process, swapchain, qpc) <= 0 or duration_ms < 0 or dropped not in ("0", "1"):
                raise ValueError(f"invalid PresentMon row {reader.line_num}")
            if identity is None:
                identity = process, swapchain
            elif identity != (process, swapchain):
                raise ValueError("expected one process and one swapchain")
            returns.append(qpc + duration_ms * frequency / 1000)
            display_time = None
            if dropped == "0":
                delay_ms = Fraction(row["msUntilDisplayed"])
                if delay_ms <= 0:
                    raise ValueError(f"invalid displayed time at row {reader.line_num}")
                display_time = Fraction(qpc, frequency) + delay_ms / 1000
            display_times.append(display_time)
    return returns, display_times


def analyze(source, etw_path, clock, *, contiguous=False):
    ns = [sample.present_ns for sample in source]
    if not ns or ns[0] != clock.begin_ns or ns[-1] > clock.end_ns:
        raise ValueError("source trace does not span its calibrated segment")
    frequency = clock.frequency
    returns, display_times = read_events(etw_path, frequency)
    match = match_contiguous_returns if contiguous else match_returns
    indices = match(ns, returns, frequency, clock.offset_low, clock.offset_high)
    # The selected event must remain eligible at both clock-offset extremes.
    for stamp, index in zip(ns, indices):
        ticks = Fraction(stamp * frequency, 1_000_000_000)
        low = clock.offset_high + ticks - Fraction(2000 * frequency, 1_000_000)
        high = clock.offset_low + ticks + Fraction(5 * frequency, 1_000_000)
        if not low <= returns[index] <= high:
            raise ValueError("match depends on the uncertain QPC offset")
    midpoint = (clock.offset_low + clock.offset_high) / 2
    to_us = Fraction(1_000_000, frequency)
    residuals = [float((midpoint + Fraction(stamp * frequency, 1_000_000_000)
                        - returns[index]) * to_us) for stamp, index in zip(ns, indices)]
    displayed = [(display_times[index], sample.digest)
                 for sample, index in zip(source, indices) if display_times[index] is not None]
    if len(displayed) < 2 or any(a[0] >= b[0] for a, b in zip(displayed, displayed[1:])):
        raise ValueError("expected at least two strictly increasing displayed times")
    elapsed = displayed[-1][0] - displayed[0][0]
    changes = sum(a[1] != b[1] for a, b in zip(displayed, displayed[1:]))
    return {
        "source_samples": len(source),
        "etw_rows": len(returns),
        "unique_matched_events": len(indices),
        "calibration_width_us": float((clock.offset_high - clock.offset_low) * to_us),
        "min_post_present_us": min(residuals),
        "median_post_present_us": statistics.median(residuals),
        "max_post_present_us": max(residuals),
        "etw_displayed_events": len(displayed),
        "dropped_in_segment": len(indices) - len(displayed),
        "displayed_elapsed_s": float(elapsed),
        "displayed_events_per_second": float((len(displayed) - 1) / elapsed),
        "changed_source_on_displayed_events": changes,
        "source_changes_per_displayed_second": float(changes / elapsed),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_csv", type=Path)
    parser.add_argument("presentmon_v1_csv", type=Path)
    parser.add_argument("daytona_log", type=Path)
    parser.add_argument("--contiguous", action="store_true",
                        help="require one complete consecutive ETW alignment")
    args = parser.parse_args()
    try:
        source = parse_presentation_digest_csv(args.source_csv.read_text(encoding="utf-8"))
        log = args.daytona_log.read_text(encoding="utf-8")
        if "daytona: incomplete hardware present trace" in log:
            raise ValueError("the hardware trace did not finish successfully")
        counts = TRACE_SUMMARY.findall(log)
        if len(counts) != 1 or int(counts[0]) != len(source):
            raise ValueError("source sample count differs from the trace summary")
        clock = calibrate(log)
        report = analyze(source, args.presentmon_v1_csv, clock, contiguous=args.contiguous)
        if args.contiguous:
            report["match_policy"] = "contiguous"
    except (OSError, ValueError, ZeroDivisionError, csv.Error) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
