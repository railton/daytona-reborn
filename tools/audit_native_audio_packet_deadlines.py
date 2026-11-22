#!/usr/bin/env python3
'''Diagnostic packet-deadline audit; the reanchor is not a production policy.'''
import argparse
import csv
import io
import json
import sys

HEADER = ['tick', 'origin', 'cursor']
UART_FIRST = 16

def parse_rows(text):
    try:
        return _parse_rows(text)
    except csv.Error as exc:
        raise ValueError('CSV error: %s' % exc) from None

def _parse_rows(text):
    reader = csv.DictReader(io.StringIO(text, newline=''), strict=True)
    if reader.fieldnames != HEADER:
        raise ValueError('header must be exactly tick,origin,cursor')
    rows = []
    prev = None
    for line, row in enumerate(reader, 2):
        if None in row or any(row.get(k) is None or not row[k].strip() for k in HEADER):
            raise ValueError('line %d: malformed row' % line)
        try:
            vals = tuple(int(row[k]) for k in HEADER)
        except ValueError:
            raise ValueError('line %d: values must be integers' % line) from None
        if prev is not None:
            if vals[0] <= prev[0]:
                raise ValueError('line %d: tick must strictly increase' % line)
            if vals[1] < prev[1]:
                raise ValueError('line %d: origin must not decrease' % line)
            if vals[2] < prev[2]:
                raise ValueError('line %d: cursor must not decrease' % line)
        rows.append(vals)
        prev = vals
    if not rows:
        raise ValueError('no data rows')
    return rows

def audit(rows, lead):
    fixed_late = would_late = reanchors = offset = max_correction = 0
    for _tick, origin, cursor in rows:
        fixed_completion = origin + lead + UART_FIRST
        if cursor > fixed_completion:
            fixed_late += 1
        active_completion = fixed_completion + offset
        if cursor > active_completion:
            correction = cursor - active_completion
            would_late += 1
            reanchors += 1
            offset += correction
            max_correction = max(max_correction, correction)
    return {
        'reanchors': reanchors,
        'final_lead': lead + offset,
        'max_correction': max_correction,
        'fixed_late': fixed_late,
        'would_late': would_late,
    }

FIXTURE = '''tick,origin,cursor
1,0,600
2,16,620
3,32,620
4,48,760
5,64,760
'''

def self_test():
    want = {
        'reanchors': 3,
        'final_lead': 696,
        'max_correction': 108,
        'fixed_late': 5,
        'would_late': 3,
    }
    got = audit(parse_rows(FIXTURE), 512)
    assert got == want, (got, want)
    nl = chr(10)
    invalid = (
        nl.join(('tick,origin', '1,0')),
        nl.join(('tick,origin,cursor', '1,0,10', '1,1,11')),
        nl.join(('tick,origin,cursor', '1,0,10', '0,1,11')),
        nl.join(('tick,origin,cursor', '1,5,10', '2,4,11')),
        nl.join(('tick,origin,cursor', '1,0,10', '2,1,9')),
        nl.join(('tick,origin,cursor', '1,x,10')),
        nl.join(('tick,origin,cursor', '1,0')),
        nl.join(('tick,origin,cursor', '')),
    )
    for text in invalid:
        try:
            parse_rows(text)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted invalid CSV: %r' % text)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv_path', nargs='?')
    parser.add_argument('--lead', type=int)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        print(json.dumps({'self_test': 'ok'}, separators=(',', ':')))
        return 0
    if args.csv_path is None or args.lead is None:
        parser.error('CSV path and --lead are required')
    if args.lead < 0:
        parser.error('--lead must be nonnegative')
    try:
        with open(args.csv_path, encoding='utf-8-sig', newline='') as handle:
            rows = parse_rows(handle.read())
        result = audit(rows, args.lead)
    except (OSError, ValueError) as exc:
        print('error: %s' % exc, file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True, separators=(',', ':')))
    return 0

if __name__ == '__main__':
    sys.exit(main())