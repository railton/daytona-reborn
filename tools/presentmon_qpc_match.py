'''Match relative source timestamps to PresentMon ETW return events.

For each source timestamp, the inclusive QPC interval is::

    offset_low  + ns * frequency / 1e9 - max_lag_us * frequency / 1e6
    offset_high + ns * frequency / 1e9 + early_us  * frequency / 1e6

Exactly one ETW event must fall in each interval, and the selected ETW
indices must be consecutive.  All matching arithmetic uses Fraction.
'''

from __future__ import annotations

from bisect import bisect_left, bisect_right
from fractions import Fraction
from typing import Sequence

__all__ = ['match_returns', 'match_contiguous_returns']

_NS_PER_SECOND = 1_000_000_000
_US_PER_SECOND = 1_000_000


def _require_nonempty(name: str, values: object) -> None:
    try:
        length = len(values)  # type: ignore[arg-type]
    except TypeError as exc:
        raise ValueError(f'{name} must be a nonempty sequence') from exc
    if length == 0:
        raise ValueError(f'{name} must not be empty')


def _validate_source_ns(source_ns: Sequence[int]) -> None:
    _require_nonempty('source_ns', source_ns)
    previous = None
    for index, value in enumerate(source_ns):
        if type(value) is not int:
            raise ValueError(f'source_ns[{index}] must be an int')
        if value < 0:
            raise ValueError(f'source_ns[{index}] must be nonnegative')
        if previous is not None and value <= previous:
            raise ValueError('source_ns must be strictly increasing')
        previous = value


def _validate_etw_return_qpc(etw_return_qpc: Sequence[Fraction]) -> None:
    _require_nonempty('etw_return_qpc', etw_return_qpc)
    previous = None
    for index, value in enumerate(etw_return_qpc):
        if not isinstance(value, Fraction):
            raise ValueError(f'etw_return_qpc[{index}] must be a Fraction')
        if previous is not None and value <= previous:
            raise ValueError('etw_return_qpc must be strictly increasing')
        previous = value


def _validate_configuration(
    frequency: int,
    offset_low: Fraction,
    offset_high: Fraction,
    max_lag_us: int,
    early_us: int,
) -> None:
    if type(frequency) is not int or frequency <= 0:
        raise ValueError('frequency must be a positive int')
    if not isinstance(offset_low, Fraction) or not isinstance(offset_high, Fraction):
        raise ValueError('offset_low and offset_high must be Fractions')
    if offset_low > offset_high:
        raise ValueError('offset_low must not exceed offset_high')
    if type(max_lag_us) is not int or max_lag_us < 0:
        raise ValueError('max_lag_us must be a nonnegative int')
    if type(early_us) is not int or early_us < 0:
        raise ValueError('early_us must be a nonnegative int')


def _return_windows(source_ns, etw_return_qpc, frequency, offset_low,
                    offset_high, max_lag_us, early_us):
    _validate_source_ns(source_ns)
    _validate_etw_return_qpc(etw_return_qpc)
    _validate_configuration(
        frequency, offset_low, offset_high, max_lag_us, early_us
    )

    ticks_per_ns = Fraction(frequency, _NS_PER_SECOND)
    lag_ticks = Fraction(max_lag_us * frequency, _US_PER_SECOND)
    early_ticks = Fraction(early_us * frequency, _US_PER_SECOND)

    for ns in source_ns:
        earliest = ns * ticks_per_ns + offset_low
        latest = ns * ticks_per_ns + offset_high
        yield (
            bisect_left(etw_return_qpc, earliest - lag_ticks),
            bisect_right(etw_return_qpc, latest + early_ticks),
            latest - lag_ticks,
            earliest + early_ticks,
        )


def match_returns(
    source_ns: Sequence[int],
    etw_return_qpc: Sequence[Fraction],
    frequency: int,
    offset_low: Fraction,
    offset_high: Fraction,
    *,
    max_lag_us: int = 2000,
    early_us: int = 5,
) -> list[int]:
    '''Return consecutive ETW indices matched to source timestamps.

    Parameters are validated eagerly.  A ValueError is raised for empty or
    improperly ordered inputs, invalid configuration, an interval with no
    eligible event, an interval with multiple eligible events, or a
    non-consecutive sequence of matched ETW indices.
    '''
    windows = _return_windows(
        source_ns, etw_return_qpc, frequency, offset_low, offset_high,
        max_lag_us, early_us,
    )
    matched: list[int] = []
    previous_index = None
    for source_index, (first, after_last, _, _) in enumerate(windows):
        eligible = after_last - first
        if eligible == 0:
            raise ValueError(
                f'no ETW return event is eligible for source index {source_index}'
            )
        if eligible != 1:
            raise ValueError(
                f'multiple ETW return events are eligible for source index {source_index}'
            )
        if previous_index is not None and first != previous_index + 1:
            raise ValueError('matched ETW return indices must be consecutive')

        matched.append(first)
        previous_index = first

    return matched

def match_contiguous_returns(
    source_ns: Sequence[int],
    etw_return_qpc: Sequence[Fraction],
    frequency: int,
    offset_low: Fraction,
    offset_high: Fraction,
    *,
    max_lag_us: int = 2000,
    early_us: int = 5,
) -> list[int]:
    """Match one complete, consecutive ETW sequence with stable clock bounds."""
    windows = list(_return_windows(
        source_ns, etw_return_qpc, frequency, offset_low, offset_high,
        max_lag_us, early_us,
    ))
    # Row i permits the same start indices as its candidate indices minus i.
    start = max(first - i for i, (first, _, _, _) in enumerate(windows))
    stop = min(after_last - i for i, (_, after_last, _, _) in enumerate(windows))
    if start >= stop:
        raise ValueError("no contiguous ETW return sequence")
    if start + 1 != stop:
        raise ValueError("multiple contiguous ETW return sequences")

    indices = list(range(start, start + len(windows)))
    for index, (_, _, stable_low, stable_high) in zip(indices, windows):
        if not stable_low <= etw_return_qpc[index] <= stable_high:
            raise ValueError("match depends on the uncertain QPC offset")
    return indices
