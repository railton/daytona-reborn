#!/usr/bin/env python3
'''Synthetic validation oracle for presentation digest CSV evidence.

Integral interval means and medians are reported exactly. Fractional
intervals and rates use floating-point values.

The report is derived only from the supplied present_ns and digest rows. It
does not measure a display or renderer and has no runtime hook.

CSV contract:
- Header exactly present_ns,digest.
- One row per actual present, in order.
- present_ns is a strictly increasing decimal unsigned 64-bit integer
  (0..18446744073709551615), matching native nanoseconds.
- digest is a non-empty string.

Only adjacent equal digests count as repeated presentations. Non-adjacent
recurrence is not a duplicate. Content changes are adjacent digest
transitions, not counts of globally unique images.
'''
from __future__ import annotations

import argparse
import csv
import io
import json
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence, Tuple, Union

_HEADER = ('present_ns', 'digest')
_DECIMAL_UNSIGNED = re.compile(r'[0-9]+')
_UINT64_MAX = (1 << 64) - 1
_UINT64_MAX_TEXT = str(_UINT64_MAX)
_NL = chr(10)


class PresentationDigestError(ValueError):
    '''Raised when the CSV does not satisfy the digest stream contract.'''


@dataclass(frozen=True)
class Presentation:
    present_ns: int
    digest: str


def _row_error(row_number: int, message: str) -> PresentationDigestError:
    return PresentationDigestError(f'row {row_number}: {message}')


def _parse_present_ns(row_number: int, raw: str) -> int:
    '''Parse one unsigned 64-bit decimal nanosecond timestamp.'''
    text = raw.strip()
    if not _DECIMAL_UNSIGNED.fullmatch(text):
        raise _row_error(
            row_number,
            'present_ns must be a decimal unsigned 64-bit integer '
            f'in range 0..{_UINT64_MAX}, got {raw!r}',
        )

    significant = text.lstrip('0')
    if not significant:
        return 0
    if (
        len(significant) > len(_UINT64_MAX_TEXT)
        or (
            len(significant) == len(_UINT64_MAX_TEXT)
            and significant > _UINT64_MAX_TEXT
        )
    ):
        raise _row_error(
            row_number,
            'present_ns is outside the decimal unsigned 64-bit range '
            f'0..{_UINT64_MAX}: {raw!r}',
        )
    return int(significant, 10)


def parse_presentation_digest_csv(text: str) -> Tuple[Presentation, ...]:
    '''Parse and validate CSV text, returning ordered presentations.'''
    if not isinstance(text, str):
        raise TypeError('csv text must be a string')

    reader = csv.reader(io.StringIO(text, newline=''), strict=True)
    try:
        header = next(reader)
    except StopIteration as exc:
        raise PresentationDigestError(
            'header: missing; expected present_ns,digest'
        ) from exc
    except csv.Error as exc:
        raise PresentationDigestError(f'header: malformed CSV ({exc})') from exc

    if tuple(header) != _HEADER:
        raise PresentationDigestError(
            'header: expected present_ns,digest, got ' + ','.join(header)
        )

    presentations = []
    previous_ns = None
    try:
        for row_number, row in enumerate(reader, start=2):
            if not row:
                raise _row_error(row_number, 'blank line')
            if len(row) != 2:
                raise _row_error(
                    row_number, f'expected 2 columns, got {len(row)}'
                )

            present_ns = _parse_present_ns(row_number, row[0])

            if previous_ns is not None and present_ns <= previous_ns:
                raise _row_error(
                    row_number,
                    'present_ns must be strictly increasing '
                    f'({present_ns} after {previous_ns})',
                )

            digest = row[1]
            if not digest.strip():
                raise _row_error(row_number, 'digest must be non-empty')

            presentations.append(Presentation(present_ns, digest))
            previous_ns = present_ns
    except csv.Error as exc:
        raise PresentationDigestError(
            f'row near line {reader.line_num}: malformed CSV ({exc})'
        ) from exc

    return tuple(presentations)


def summarize_presentations(
    presentations: Iterable[Presentation],
) -> dict[str, object]:
    '''Compute presentation counts and interval-derived rates.

    content_changes counts adjacent digest transitions, not globally unique
    images. Every rate is None when there is no interval to measure.
    '''
    items = tuple(presentations)
    for index in range(1, len(items)):
        if items[index].present_ns <= items[index - 1].present_ns:
            raise PresentationDigestError(
                f'presentation {index}: timestamps must be strictly increasing'
            )

    content_changes = 0
    repeated_presentations = 0
    for previous, current in zip(items, items[1:]):
        if current.digest == previous.digest:
            repeated_presentations += 1
        else:
            content_changes += 1

    if items:
        elapsed_ns = items[-1].present_ns - items[0].present_ns
    else:
        elapsed_ns = 0

    intervals = tuple(
        items[index].present_ns - items[index - 1].present_ns
        for index in range(1, len(items))
    )
    interval_count = len(intervals)

    if interval_count:
        sample_rate_hz = interval_count * 1_000_000_000.0 / elapsed_ns
        content_change_rate_hz = (
            content_changes * 1_000_000_000.0 / elapsed_ns
        )
        repeated_presentations_per_second = (
            repeated_presentations * 1_000_000_000.0 / elapsed_ns
        )
        mean_interval_ns = statistics.mean(intervals)
        ordered = sorted(intervals)
        middle = interval_count // 2
        median_interval_ns = (ordered[middle] if interval_count % 2
                              else statistics.mean(ordered[middle - 1:middle + 1]))
        min_interval_ns = min(intervals)
        max_interval_ns = max(intervals)
    else:
        sample_rate_hz = None
        content_change_rate_hz = None
        repeated_presentations_per_second = None
        mean_interval_ns = None
        median_interval_ns = None
        min_interval_ns = None
        max_interval_ns = None

    return {
        'presentations': len(items),
        'interval_count': interval_count,
        'content_changes': content_changes,
        'repeated_presentations': repeated_presentations,
        'elapsed_ns': elapsed_ns,
        'sample_rate_hz': sample_rate_hz,
        'content_change_rate_hz': content_change_rate_hz,
        'repeated_presentations_per_second': (
            repeated_presentations_per_second
        ),
        'mean_interval_ns': mean_interval_ns,
        'median_interval_ns': median_interval_ns,
        'min_interval_ns': min_interval_ns,
        'max_interval_ns': max_interval_ns,
    }


def analyze_presentation_digest_csv(text: str) -> dict[str, object]:
    '''Validate CSV text and return the derived report.'''
    return summarize_presentations(parse_presentation_digest_csv(text))


def check_presentation_digest_stream(text: str) -> dict[str, object]:
    '''Validate CSV text and return the derived report.'''
    return analyze_presentation_digest_csv(text)


def check_presentation_digest_file(
    csv_path: Union[str, Path],
) -> dict[str, object]:
    '''Read and validate one CSV file.'''
    text = Path(csv_path).read_text(encoding='utf-8-sig')
    return analyze_presentation_digest_csv(text)


def _csv_text(*rows: str) -> str:
    return _NL.join(('present_ns,digest',) + rows) + _NL


def _expect_rejected(text: str, label: str) -> None:
    try:
        analyze_presentation_digest_csv(text)
    except PresentationDigestError:
        return
    raise AssertionError(f'expected rejection: {label}')


def run_self_tests() -> None:
    '''Exercise counts, rates, boundaries, and malformed input rejection.'''
    repeated_then_new = _csv_text(
        '1000000000,alpha',
        '1008333333,alpha',
        '1016666666,beta',
        '1025000000,alpha',
    )
    report = analyze_presentation_digest_csv(repeated_then_new)
    assert report['presentations'] == 4
    assert report['interval_count'] == 3
    assert report['content_changes'] == 2
    assert report['repeated_presentations'] == 1
    assert report['elapsed_ns'] == 25_000_000
    assert report['sample_rate_hz'] is not None
    assert abs(report['sample_rate_hz'] - 120.0) < 1e-9
    assert report['content_change_rate_hz'] is not None
    assert abs(report['content_change_rate_hz'] - 80.0) < 1e-9
    assert report['repeated_presentations_per_second'] is not None
    assert (
        abs(report['repeated_presentations_per_second'] - 40.0) < 1e-9
    )
    assert report['min_interval_ns'] == 8_333_333
    assert report['max_interval_ns'] == 8_333_334

    header_only = analyze_presentation_digest_csv(
        'present_ns,digest' + _NL
    )
    assert header_only['presentations'] == 0
    assert header_only['interval_count'] == 0
    assert header_only['content_changes'] == 0
    assert header_only['repeated_presentations'] == 0
    assert header_only['elapsed_ns'] == 0
    for key in (
        'sample_rate_hz',
        'content_change_rate_hz',
        'repeated_presentations_per_second',
        'mean_interval_ns',
        'median_interval_ns',
        'min_interval_ns',
        'max_interval_ns',
    ):
        assert header_only[key] is None

    single_zero = analyze_presentation_digest_csv(_csv_text('0,alpha'))
    assert single_zero['presentations'] == 1
    assert single_zero['interval_count'] == 0
    assert single_zero['elapsed_ns'] == 0
    for key in (
        'sample_rate_hz',
        'content_change_rate_hz',
        'repeated_presentations_per_second',
    ):
        assert single_zero[key] is None

    boundary = analyze_presentation_digest_csv(
        _csv_text('0,alpha', f'{_UINT64_MAX},beta')
    )
    assert boundary['presentations'] == 2
    assert boundary['interval_count'] == 1
    assert boundary['elapsed_ns'] == _UINT64_MAX
    assert boundary['mean_interval_ns'] == _UINT64_MAX
    assert boundary['median_interval_ns'] == _UINT64_MAX

    exact_large_interval = (1 << 53) + 1
    large_even = analyze_presentation_digest_csv(_csv_text(
        '0,alpha', f'{exact_large_interval},beta', f'{2 * exact_large_interval},alpha',
    ))
    assert large_even['mean_interval_ns'] == exact_large_interval
    assert large_even['median_interval_ns'] == exact_large_interval
    assert boundary['content_changes'] == 1
    assert boundary['repeated_presentations'] == 0

    recurrence = _csv_text(
        '0,alpha',
        '8333333,beta',
        '16666666,alpha',
    )
    report = analyze_presentation_digest_csv(recurrence)
    assert report['content_changes'] == 2
    assert report['repeated_presentations'] == 0

    _expect_rejected('', 'empty input')
    _expect_rejected(
        'present_ns,digest,extra' + _NL + '0,alpha',
        'extra header column',
    )
    _expect_rejected(
        'timestamp,digest' + _NL + '0,alpha',
        'wrong header',
    )
    _expect_rejected(_csv_text('0'), 'missing digest column')
    _expect_rejected(_csv_text('0,alpha,extra'), 'extra row column')
    _expect_rejected(_csv_text('0.5,alpha'), 'non-integer timestamp')
    _expect_rejected(_csv_text('alpha,0'), 'non-integer timestamp text')
    _expect_rejected(_csv_text('-1,alpha'), 'negative timestamp')
    _expect_rejected(
        _csv_text(f'{_UINT64_MAX + 1},alpha'),
        'timestamp above uint64 max',
    )
    _expect_rejected(_csv_text('0,'), 'empty digest')
    _expect_rejected(_csv_text('0,   '), 'blank digest')
    _expect_rejected(
        _csv_text('10,alpha', '10,beta'),
        'equal timestamps',
    )
    _expect_rejected(
        _csv_text('10,alpha', '9,beta'),
        'decreasing timestamps',
    )
    _expect_rejected(
        'present_ns,digest' + _NL + _NL + '0,alpha',
        'blank row',
    )
    _expect_rejected(
        'present_ns,digest' + _NL + '0,' + chr(34) + 'unterminated',
        'malformed CSV',
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description='Validate a synthetic presentation digest CSV and print JSON.'
    )
    parser.add_argument('csv_path', nargs='?', help='input CSV path')
    parser.add_argument(
        '--self-test',
        action='store_true',
        help='run the built-in synthetic checks',
    )
    args = parser.parse_args(argv)

    if args.self_test:
        if args.csv_path is not None:
            parser.error('csv_path cannot be combined with --self-test')
        run_self_tests()
        print('self-tests passed')
        return 0

    if args.csv_path is None:
        parser.error('csv_path is required unless --self-test is used')

    try:
        report = check_presentation_digest_file(args.csv_path)
    except (OSError, UnicodeError, PresentationDigestError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2

    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())