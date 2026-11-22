'''Strict, offline audit of M2_WRITE_TRACE CSV rows.

The audit never creates persistent IDs, stable slot IDs, or mesh labels.
It streams rows in encounter order and groups them by argument sequence.
'''

import csv
import json
import sys
from collections.abc import Mapping

HEADER = (
    'frame',
    'command_word',
    'argument_index',
    'buffer_word',
    'unknown_mask',
    'i960_mask',
    'tgp_mask',
    'port_mask',
    'last_mask',
    'writes',
)
_REQUIRED_FIELDS = frozenset(HEADER)
_OFFSET_MAX = 0x7FFF
_OFFSET_MASK = 0x7FFF
_MASK32 = 0xFFFFFFFF
_UINT64_MAX = 0xFFFFFFFFFFFFFFFF


def _parse_uint(row, name, row_number, maximum):
    value = row[name]
    if (
        not isinstance(value, str)
        or not value
        or not value.isascii()
        or not value.isdecimal()
    ):
        raise ValueError(
            f'row {row_number}: {name}: expected unsigned decimal'
        )
    try:
        number = int(value, 10)
    except ValueError:
        raise ValueError(
            f'row {row_number}: {name}: expected unsigned decimal'
        ) from None
    if number > maximum:
        raise ValueError(f'row {row_number}: {name}: out of range')
    return number


def _parse_row(raw, row_number):
    if not isinstance(raw, Mapping):
        raise ValueError(f'row {row_number}: expected a mapping')
    if set(raw.keys()) != _REQUIRED_FIELDS:
        raise ValueError(f'row {row_number}: expected exact header fields')

    frame = _parse_uint(raw, 'frame', row_number, _UINT64_MAX)
    command_word = _parse_uint(raw, 'command_word', row_number, _OFFSET_MAX)
    argument_index = _parse_uint(
        raw, 'argument_index', row_number, _OFFSET_MAX
    )
    buffer_word = _parse_uint(raw, 'buffer_word', row_number, _OFFSET_MAX)
    unknown_mask = _parse_uint(raw, 'unknown_mask', row_number, _MASK32)
    i960_mask = _parse_uint(raw, 'i960_mask', row_number, _MASK32)
    tgp_mask = _parse_uint(raw, 'tgp_mask', row_number, _MASK32)
    port_mask = _parse_uint(raw, 'port_mask', row_number, _MASK32)
    last_mask = _parse_uint(raw, 'last_mask', row_number, _MASK32)
    writes = _parse_uint(raw, 'writes', row_number, _UINT64_MAX)

    if buffer_word != ((command_word + argument_index) & _OFFSET_MASK):
        raise ValueError(f'row {row_number}: buffer_word mismatch')

    if (
        (unknown_mask & i960_mask)
        or (unknown_mask & tgp_mask)
        or (unknown_mask & port_mask)
        or (i960_mask & tgp_mask)
        or (i960_mask & port_mask)
        or (tgp_mask & port_mask)
        or ((unknown_mask | i960_mask | tgp_mask | port_mask) != _MASK32)
    ):
        raise ValueError(
            f'row {row_number}: ownership masks must be disjoint and cover 32 bits'
        )

    if writes == 0:
        if unknown_mask != _MASK32 or last_mask != 0:
            raise ValueError(
                f'row {row_number}: writes=0 requires unknown_mask=FFFFFFFF and last_mask=0'
            )
    elif last_mask == 0:
        raise ValueError(
            f'row {row_number}: positive writes requires a nonzero last_mask'
        )

    return (
        frame,
        command_word,
        argument_index,
        unknown_mask,
        i960_mask,
        tgp_mask,
        port_mask,
        last_mask,
        writes,
    )


def audit_rows(rows):
    '''Audit an iterable of M2_WRITE_TRACE row mappings.

    rows_with_origin counts every nonzero ownership-mask entry, so a mixed row
    contributes more than once. mixed_origin_count counts rows with two or more
    nonzero ownership masks. The returned aggregate is JSON-safe and contains
    only counts. Invalid input raises ValueError.
    '''
    total_rows = 0
    group_count = 0
    frame_count = 0
    rows_with_origin = 0
    mixed_origin_count = 0
    last_frame = None
    group_length = 0
    group_frame = 0
    group_command_word = 0

    for row_number, raw in enumerate(rows, start=1):
        (
            frame,
            command_word,
            argument_index,
            unknown_mask,
            i960_mask,
            tgp_mask,
            port_mask,
            last_mask,
            writes,
        ) = _parse_row(raw, row_number)

        if last_frame is not None and frame < last_frame:
            raise ValueError(
                f'row {row_number}: frame decreased from {last_frame} to {frame}'
            )
        if last_frame is None or frame != last_frame:
            frame_count += 1
        last_frame = frame
        total_rows += 1

        nonzero_masks = 0
        for mask in (unknown_mask, i960_mask, tgp_mask, port_mask):
            if mask:
                nonzero_masks += 1
        rows_with_origin += nonzero_masks
        if nonzero_masks > 1:
            mixed_origin_count += 1

        if group_length == 0:
            if argument_index != 0:
                raise ValueError(
                    f'row {row_number}: group must start with argument_index 0'
                )
            group_length = 1
            group_frame = frame
            group_command_word = command_word
            continue

        same_slot = (
            frame == group_frame and command_word == group_command_word
        )
        if same_slot and argument_index == group_length:
            group_length += 1
            if group_length == 5:
                group_count += 1
                group_length = 0
            continue

        if group_length == 3 and argument_index == 0:
            group_count += 1
            group_length = 1
            group_frame = frame
            group_command_word = command_word
            continue

        if not same_slot:
            raise ValueError(
                f'row {row_number}: group changed frame/command before completion'
            )
        if group_length == 4:
            raise ValueError(f'row {row_number}: truncated 5-row group')
        if group_length in (1, 2):
            raise ValueError(
                f'row {row_number}: expected argument_index {group_length}, '
                f'got {argument_index}'
            )
        raise ValueError(
            f'row {row_number}: expected argument_index 3 or a new group at 0, '
            f'got {argument_index}'
        )

    if total_rows == 0:
        raise ValueError('empty input: no trace rows')

    if group_length:
        if group_length == 3:
            group_count += 1
        else:
            raise ValueError(
                f'truncated group at end of input (length {group_length}; '
                f'expected 3 or 5)'
            )

    return {
        'total_rows': total_rows,
        'group_count': group_count,
        'frame_count': frame_count,
        'rows_with_origin': rows_with_origin,
        'mixed_origin_count': mixed_origin_count,
    }


def main(argv=None):
    '''Run the strict DictReader CLI.'''
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 1:
        print('usage: audit_writer_trace.py TRACE.csv', file=sys.stderr)
        return 2

    try:
        with open(args[0], 'r', encoding='utf-8', newline='') as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None or tuple(reader.fieldnames) != HEADER:
                raise ValueError(
                    'invalid header: expected ' + ','.join(HEADER)
                )
            result = audit_rows(reader)
    except (OSError, UnicodeError, csv.Error, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1

    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
