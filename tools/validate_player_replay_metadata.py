#!/usr/bin/env python3
"""Opt-in comparison of two private replay session descriptions.

Each description is a JSON object with: course (1..3), transmission
("automatic"/"manual"), frames (positive int), script_sha256, rom_set_sha256,
nvram_sha256, build_sha256 (64 hex chars, supplied by the caller, local only).
An optional race_mode is normal or time-attack. Older manifests default to normal.
Output only names fields; values are never printed.
"""
import argparse
import json
import re
import sys

DIGEST_FIELDS = ('script_sha256', 'rom_set_sha256', 'nvram_sha256', 'build_sha256')
FIELDS = ('course', 'transmission', 'frames') + DIGEST_FIELDS
_HEX = re.compile(r'\A[0-9a-f]{64}\Z')


def invalid_fields(desc):
    """Return (missing, invalid) field-name lists for one description."""
    if not isinstance(desc, dict):
        return list(FIELDS), []
    missing = [f for f in FIELDS if f not in desc]
    invalid = []
    for f in FIELDS:
        if f in missing:
            continue
        v = desc[f]
        if f == 'course':
            ok = type(v) is int and 1 <= v <= 3
        elif f == 'transmission':
            ok = v in ('automatic', 'manual')
        elif f == 'frames':
            ok = type(v) is int and v > 0
        else:
            ok = isinstance(v, str) and bool(_HEX.match(v.lower()))
        if not ok:
            invalid.append(f)
    if 'race_mode' in desc and desc['race_mode'] not in ('normal', 'time-attack'):
        invalid.append('race_mode')
    return missing, invalid


def _norm(desc, f):
    v = desc[f]
    return v.lower() if f in DIGEST_FIELDS else v


def compare(a, b):
    """Return dict: ok, missing_a/b, invalid_a/b, differing (field names only)."""
    ma, ia = invalid_fields(a)
    mb, ib = invalid_fields(b)
    res = {'missing_a': ma, 'invalid_a': ia, 'missing_b': mb, 'invalid_b': ib, 'differing': []}
    if not (ma or ia or mb or ib):
        res['differing'] = [f for f in FIELDS if _norm(a, f) != _norm(b, f)]
        if a.get('race_mode', 'normal') != b.get('race_mode', 'normal'):
            res['differing'].append('race_mode')
    res['ok'] = not (ma or ia or mb or ib or res['differing'])
    return res


def _no_duplicates(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError('duplicate key')
    return dict(pairs)


_UNREADABLE = object()


def _load(path):
    """Return the parsed JSON, or _UNREADABLE (unreadable, malformed, duplicate key, too deep)."""
    try:
        with open(path, 'r', encoding='utf-8') as fh:
            return json.load(fh, object_pairs_hook=_no_duplicates)
    except (OSError, ValueError, RecursionError, MemoryError):
        return _UNREADABLE


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--expected', required=True, help='JSON description file A')
    p.add_argument('--actual', required=True, help='JSON description file B')
    args = p.parse_args(argv)
    a, b = _load(args.expected), _load(args.actual)
    ua, ub = a is _UNREADABLE, b is _UNREADABLE
    res = compare(None if ua else a, None if ub else b)
    if ua or ub:
        res['unreadable_a'], res['unreadable_b'] = ua, ub
        res['ok'] = False
    print(json.dumps(res, sort_keys=True))
    return 0 if res['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())