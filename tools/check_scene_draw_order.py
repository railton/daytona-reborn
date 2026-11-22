#!/usr/bin/env python3
'''Summarize draw-order changes between adjacent frames of an object trace.

Input CSV schema: frame,pool,oba,obc,mode,matrix0..matrix11,source,command_word
(extra columns such as vertices are allowed).

Diagnostic only. A row is paired across two adjacent frames (frame numbers
differing by exactly 1) only when its (geometry key, exact 12 model-view
values) tuple occurs exactly once on each side. Ambiguous tuples are excluded
and counted; no occurrence identity is ever asserted and nothing here
authorizes interpolation. Geometry key = pool, oba, obc, mode (canonical, as in
docs/TRACE_SCHEMA.md and analyze-object-trace.py) plus source as a
disambiguator. oba is the ROM geometry address; command_word is the command
offset and is NOT part of the key. vertices is never used. Two different oba
never join, even with equal counts and poses.
Matrix values must be finite; -0 and +0 are numerically equal.
Ranks are the 0-based CSV row order inside a frame; they are ordinal, so an
insertion can shift the global rank without any relative permutation.
Pair inversions count reversals among retained matches, across the whole
frame and separately within each geometry. Unmatched rows cannot form pairs.
'''
from __future__ import annotations

import csv
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

MATRIX = tuple(f'matrix{i}' for i in range(12))
REQUIRED = ('frame', 'pool', 'oba', 'obc', 'mode') + MATRIX + ('source', 'command_word')


def geometry_of(row):
    return (int(row['pool']), int(row['oba']), int(row['obc']),
            int(row['mode']), row['source'])


def pose_of(row):
    vals = []
    for m in MATRIX:
        v = float(row[m])
        if not math.isfinite(v):
            raise ValueError('non-finite matrix value')
        vals.append((v + 0.0).hex())  # -0.0 + 0.0 == +0.0
    return tuple(vals)


def load(path):
    with Path(path).open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader(stream, strict=True)
        missing = [c for c in REQUIRED if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError('missing column(s): ' + ','.join(missing))
        frames = defaultdict(list)
        for r in reader:
            if None in r or any(r[c] is None or r[c] == '' for c in REQUIRED):
                raise ValueError('malformed row')
            pose = pose_of(r)
            geo = geometry_of(r)
            frames[int(r['frame'])].append(
                {'geo': geo, 'pose': pose, 'cw': r['command_word']})
    if not frames:
        raise ValueError('empty trace: no data rows')
    return frames


def _annotate(rows):
    seen = Counter()
    out = []
    for i, r in enumerate(rows):
        out.append(dict(r, rank=i, grank=seen[r['geo']]))
        seen[r['geo']] += 1
    return out


def count_inversions(ranks):
    """Count strictly reversed pairs in O(n log n) using a Fenwick tree."""
    if len(ranks) < 2:
        return 0
    positions = {rank: i for i, rank in enumerate(sorted(set(ranks)), 1)}
    tree = [0] * (len(positions) + 1)
    total = 0
    for seen, rank in enumerate(ranks):
        position = positions[rank]
        prefix_count = 0
        i = position
        while i:
            prefix_count += tree[i]
            i -= i & -i
        total += seen - prefix_count
        i = position
        while i < len(tree):
            tree[i] += 1
            i += i & -i
    return total


def summarize(frames):
    result = {
        'frames': len(frames), 'adjacent_pairs': 0, 'skipped_non_adjacent': 0,
        'rows_compared': 0, 'matched': 0, 'ambiguous_excluded': 0,
        'unmatched_disappeared': 0, 'unmatched_appeared': 0,
        'command_word_changes': 0, 'global_rank_changes': 0,
        'geometry_rank_changes': 0, 'global_pair_inversions': 0,
        'geometry_pair_inversions': 0, 'identity_asserted': False,
        'interpolation_allowed': False,
    }
    ordered = sorted(frames)
    for a, b in zip(ordered, ordered[1:]):
        if b != a + 1:
            result['skipped_non_adjacent'] += 1
            continue
        result['adjacent_pairs'] += 1
        ra, rb = _annotate(frames[a]), _annotate(frames[b])
        ca = Counter((r['geo'], r['pose']) for r in ra)
        cb = Counter((r['geo'], r['pose']) for r in rb)
        result['rows_compared'] += len(ra) + len(rb)
        ia = {(r['geo'], r['pose']): r for r in ra if ca[(r['geo'], r['pose'])] == 1}
        ib = {(r['geo'], r['pose']): r for r in rb if cb[(r['geo'], r['pose'])] == 1}
        for k in set(ca) | set(cb):
            if ca.get(k, 0) > 1 or cb.get(k, 0) > 1:
                result['ambiguous_excluded'] += ca.get(k, 0) + cb.get(k, 0)
            elif k in ia and k not in ib:
                result['unmatched_disappeared'] += 1
            elif k in ib and k not in ia:
                result['unmatched_appeared'] += 1
        global_ranks = []
        geometry_ranks = defaultdict(list)
        # ia preserves frame a's row order. Read frame b's ranks in that order.
        for k, x in ia.items():
            if k not in ib:
                continue
            y = ib[k]
            result['matched'] += 1
            result['command_word_changes'] += x['cw'] != y['cw']
            result['global_rank_changes'] += x['rank'] != y['rank']
            result['geometry_rank_changes'] += x['grank'] != y['grank']
            global_ranks.append(y['rank'])
            geometry_ranks[x['geo']].append(y['grank'])
        result['global_pair_inversions'] += count_inversions(global_ranks)
        result['geometry_pair_inversions'] += sum(
            count_inversions(ranks) for ranks in geometry_ranks.values())
    return result


def main(argv):
    if len(argv) != 2:
        print('usage: check_scene_draw_order.py OBJECT_TRACE.csv', file=sys.stderr)
        return 2
    try:
        print(json.dumps(summarize(load(argv[1])), indent=2, sort_keys=True))
    except (OSError, UnicodeError, csv.Error, ValueError) as exc:
        print('invalid trace input (' + type(exc).__name__ + ')', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
