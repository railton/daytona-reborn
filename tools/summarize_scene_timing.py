#!/usr/bin/env python3
'''Aggregate scene timing CSV traces.'''

import argparse
import csv
import json
import re
import statistics
import sys
from collections import namedtuple

HEADER = ('frame', 'elapsed_ns', 'input_polygons', 'instances', 'resident_meshes')
UINT64_MAX = (1 << 64) - 1
INT64_MAX = (1 << 63) - 1
DECIMAL = re.compile(r'[0-9]+')
Row = namedtuple('Row', HEADER)


def _number(text, label, limit):
    if DECIMAL.fullmatch(text) is None:
        raise ValueError(f'{label} must be ASCII decimal digits')
    value = int(text)
    if value > limit:
        raise ValueError(f'{label} exceeds {limit}')
    return value


def _row(fields, line):
    if len(fields) != len(HEADER):
        raise ValueError(f'line {line}: expected {len(HEADER)} fields, got {len(fields)}')
    values = tuple(
        _number(text, name, INT64_MAX if name == 'elapsed_ns' else UINT64_MAX)
        for text, name in zip(fields, HEADER)
    )
    if values[3] > values[2]:
        raise ValueError(f'line {line}: instances exceed input_polygons')
    return Row(*values)


def _group(rows):
    times = sorted(row.elapsed_ns for row in rows)
    n = len(times)

    def rank(percent):
        return times[(percent * n + 99) // 100 - 1]

    return {
        'n': n,
        'median_ns': statistics.median(times),
        'p95_ns': rank(95),
        'p99_ns': rank(99),
        'max_ns': times[-1],
        'total_ns': sum(times),
        'max_input_polygons': max(row.input_polygons for row in rows),
        'max_instances': max(row.instances for row in rows),
    }


def summarize(stream, entry_frame=None, expected_rows=None):
    if entry_frame is not None and (type(entry_frame) is not int or entry_frame < 0):
        raise ValueError('entry_frame must be a nonnegative integer')
    if expected_rows is not None and (type(expected_rows) is not int or expected_rows <= 0):
        raise ValueError('expected_rows must be a positive integer')

    reader = csv.reader(stream, strict=True)
    try:
        header = next(reader)
    except StopIteration:
        raise ValueError('empty trace: missing header') from None
    except csv.Error as exc:
        raise ValueError(f'malformed CSV: {exc}') from exc
    if tuple(header) != HEADER:
        raise ValueError('invalid scene timing header')

    rows = []
    previous = None
    try:
        for fields in reader:
            line = reader.line_num
            row = _row(fields, line)
            if previous is not None:
                if row.frame <= previous.frame:
                    raise ValueError(f'line {line}: frame is not increasing')
                if row.resident_meshes < previous.resident_meshes:
                    raise ValueError(f'line {line}: resident_meshes decreased')
            rows.append(row)
            previous = row
    except csv.Error as exc:
        raise ValueError(f'malformed CSV: {exc}') from exc

    if not rows:
        raise ValueError('empty trace: no data rows')
    if expected_rows is not None and len(rows) != expected_rows:
        raise ValueError(f'expected {expected_rows} rows, got {len(rows)}')

    summary = _group(rows)
    summary['startup'] = None
    summary['racing'] = None
    if entry_frame is not None:
        startup = [row for row in rows if row.frame < entry_frame]
        racing = [row for row in rows if row.frame >= entry_frame]
        if startup:
            summary['startup'] = _group(startup)
        if racing:
            summary['racing'] = _group(racing)
    summary['resident_meshes_at_end'] = rows[-1].resident_meshes
    return summary


def _nonnegative(text):
    if DECIMAL.fullmatch(text) is None:
        raise argparse.ArgumentTypeError('must be a nonnegative integer')
    return int(text)


def _positive(text):
    value = _nonnegative(text)
    if value == 0:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path')
    parser.add_argument('--entry-frame', type=_nonnegative)
    parser.add_argument('--expected-rows', type=_positive)
    args = parser.parse_args(argv)

    try:
        with open(args.path, 'r', encoding='utf-8', newline='') as stream:
            summary = summarize(stream, args.entry_frame, args.expected_rows)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2
    print(json.dumps(summary, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
