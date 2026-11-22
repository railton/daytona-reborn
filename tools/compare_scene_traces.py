#!/usr/bin/env python3
"""Compare two M2_OBJECT_TRACE CSVs tick by tick over an explicit window.

Diagnostic only, no identity/cell attachment, no interpolation.
Columns: tick (or frame), pool, oba, obc, mode, matrix0..matrix11, source,
command_word, opcode, tpa, tha. Extra columns are ignored.
Comparison key = pool,oba,obc,mode,source,opcode,tpa,tha + 12 exact matrix
values (-0 == +0, finite only). command_word is NOT in the key.
Per tick present on both sides:
  * multiset_different_ticks: key multisets differ (rows added/removed/duplicated)
  * rank_permutation_ticks: among mutually unique keys (count==1 on both sides)
    the relative order differs (pair inversions counted separately)
  * command_word_change_rows: command_word differs, mutually unique keys only
Ticks inside the window present in only one trace are reported, never compared.
Refused (exit 1): no common tick in the window (including an empty window on
one side), duplicate column names, and a header holding both 'tick' and
'frame' (ambiguous time axis; rename one).
Excluded rows are split into disjoint, verifiable categories per side, so that
rows_x = mutually_unique_rows + rows_absent_other_side_x
         + rows_duplicated_present_both_x + rows_unique_here_duplicated_other_x
(rows_absent_other_side counts rows whose key is missing on the other side,
duplicated or not; duplicate_key_rows_x counts every row of a key repeated on
side x, absent or not, hence overlaps rows_absent_other_side_x).
rows_excluded_total = sum of the three excluded categories over both sides.
Duplicates: keys repeated on a side are excluded from rank/command_word checks
(only multiset equality sees them); counts are reported in the result.
rank_changed_rows is a rank among mutually unique rows, so a pure insertion
of a non-shared row shifts nothing.
"""
from __future__ import annotations
import csv
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

from check_scene_draw_order import MATRIX, count_inversions, geometry_of, pose_of

REQUIRED = ('pool', 'oba', 'obc', 'mode') + MATRIX + (
    'source', 'command_word', 'opcode', 'tpa', 'tha')


def _key(row):
    arguments = tuple(row[name] for name in ('opcode', 'tpa', 'tha'))
    return geometry_of(row) + arguments + pose_of(row)


def load(path, start, end):
    """Return {tick: [(key, command_word), ...]} in CSV row order, window inclusive."""
    ticks = defaultdict(list)
    with Path(path).open(encoding='utf-8', newline='') as fh:
        rd = csv.DictReader(fh, strict=True)
        names = rd.fieldnames or []
        dups = sorted(n for n, count in Counter(names).items() if count > 1)
        if dups:
            raise ValueError('duplicate column name(s): ' + ','.join(dups))
        if 'tick' in names and 'frame' in names:
            raise ValueError("ambiguous time axis: both 'tick' and 'frame' columns")
        tcol = 'tick' if 'tick' in names else 'frame' if 'frame' in names else None
        missing = ([] if tcol else ['tick']) + [c for c in REQUIRED if c not in names]
        if missing:
            raise ValueError('missing column(s): ' + ','.join(missing))
        for r in rd:
            if None in r or any(r[c] is None or r[c] == '' for c in (tcol,) + REQUIRED):
                raise ValueError('malformed row')
            t = int(r[tcol])
            if start <= t <= end:
                ticks[t].append((_key(r), r['command_word']))
    return ticks


def compare(a, b, start, end):
    if start > end:
        raise ValueError('window start > end')
    common_ticks = a.keys() & b.keys()
    if not common_ticks:
        raise ValueError('no common tick in window (a: %d ticks, b: %d ticks)'
                         % (len(a), len(b)))
    result = {'window': [start, end], 'ticks_a': len(a), 'ticks_b': len(b),
           'ticks_only_in_a': sorted(set(a) - set(b)),
           'ticks_only_in_b': sorted(set(b) - set(a)),
           'ticks_compared': 0, 'rows_a': 0, 'rows_b': 0,
           'multiset_different_ticks': 0, 'rank_permutation_ticks': 0,
           'rank_pair_inversions': 0, 'rank_changed_rows': 0,
           'mutually_unique_rows': 0, 'rows_excluded_total': 0,
           'rows_absent_other_side_a': 0, 'rows_absent_other_side_b': 0,
           'rows_duplicated_present_both_a': 0, 'rows_duplicated_present_both_b': 0,
           'rows_unique_here_duplicated_other_a': 0, 'rows_unique_here_duplicated_other_b': 0,
           'command_word_change_rows': 0,
           'duplicate_key_rows_a': 0, 'duplicate_key_rows_b': 0,
           'ticks_with_duplicates': 0, 'ticks_not_compared': 0,
           'limits': ('rank and command_word cover mutually unique keys only; '
                      'duplicated keys are visible only through multiset counts; '
                      'ticks present on one side only are not compared'),
           'identity_asserted': False, 'interpolation_allowed': False}
    result['ticks_not_compared'] = len(result['ticks_only_in_a']) + len(result['ticks_only_in_b'])
    for t in sorted(common_ticks):
        ra, rb = a[t], b[t]
        result['ticks_compared'] += 1
        result['rows_a'] += len(ra)
        result['rows_b'] += len(rb)
        ca = Counter(k for k, _ in ra)
        cb = Counter(k for k, _ in rb)
        if ca != cb:
            result['multiset_different_ticks'] += 1
        da = sum(c for c in ca.values() if c > 1)
        db = sum(c for c in cb.values() if c > 1)
        result['duplicate_key_rows_a'] += da
        result['duplicate_key_rows_b'] += db
        result['ticks_with_duplicates'] += bool(da or db)
        uniq = {k for k in ca if ca[k] == 1 and cb.get(k) == 1}
        result['rows_excluded_total'] += (len(ra) + len(rb)) - 2 * len(uniq)
        for side, c, o in (('a', ca, cb), ('b', cb, ca)):
            for k, n in c.items():
                if o.get(k, 0) == 0:
                    result['rows_absent_other_side_' + side] += n
                elif n > 1:
                    result['rows_duplicated_present_both_' + side] += n
                elif o[k] > 1:
                    result['rows_unique_here_duplicated_other_' + side] += n
        result['mutually_unique_rows'] += len(uniq)
        pa = [(k, cw) for k, cw in ra if k in uniq]
        ub = [(k, cw) for k, cw in rb if k in uniq]
        pb = {k: (i, cw) for i, (k, cw) in enumerate(ub)}
        order_b = []
        for i, (k, cw) in enumerate(pa):
            result['command_word_change_rows'] += cw != pb[k][1]
            result['rank_changed_rows'] += i != pb[k][0]
            order_b.append(pb[k][0])
        inv = count_inversions(order_b)
        result['rank_pair_inversions'] += inv
        result['rank_permutation_ticks'] += inv > 0
    return result


def main(argv):
    if len(argv) != 5:
        print('usage: compare_scene_traces.py A.csv B.csv START_TICK END_TICK', file=sys.stderr)
        return 2
    try:
        s, e = int(argv[3]), int(argv[4])
        print(json.dumps(compare(load(argv[1], s, e), load(argv[2], s, e), s, e),
                         indent=2, sort_keys=True))
    except (OSError, UnicodeError, csv.Error, ValueError) as exc:
        print('invalid input (' + type(exc).__name__ + '): ' + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
