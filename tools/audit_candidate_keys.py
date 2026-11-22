"""Audit exact candidate keys against independent labels."""
from collections import Counter, defaultdict
from collections.abc import Mapping

__all__ = ["audit"]


def _uint(value, field):
    if type(value) is not int or value < 0:
        raise ValueError(f"{field} must be a nonnegative int")
    return value


def _ints(value, field, size=None):
    if type(value) is not tuple or (size is not None and len(value) != size):
        raise ValueError(f"{field} must be a tuple of nonnegative ints")
    if any(type(item) is not int or item < 0 for item in value):
        raise ValueError(f"{field} must contain only nonnegative ints")
    return value


def _validated(row):
    if not isinstance(row, Mapping):
        raise ValueError("each row must be a mapping")
    if any(field not in row for field in ("frame", "geometry", "candidate")):
        raise ValueError("row is missing a required field")
    label = row.get("label")
    if label is not None and (type(label) is not str or not label):
        raise ValueError("label must be a nonempty string or None")
    candidate = row["candidate"]
    return (
        _uint(row["frame"], "frame"),
        _ints(row["geometry"], "geometry", 4),
        None if candidate is None else _ints(candidate, "candidate"),
        label,
    )


def audit(rows):
    """Return aggregate candidate-key counters; identity is never proven."""
    try:
        iterator = iter(rows)
    except TypeError as exc:
        raise ValueError("rows must be iterable") from exc

    by_frame = defaultdict(list)
    for raw in iterator:
        frame, geometry, candidate, label = _validated(raw)
        by_frame[frame].append((len(by_frame[frame]), (geometry, candidate), label))

    frames = sorted(by_frame)
    missing = collisions = 0
    geometries = defaultdict(set)
    unique = {}
    for frame in frames:
        rows_here = by_frame[frame]
        keyed = [entry for entry in rows_here if entry[1][1] is not None]
        missing += len(rows_here) - len(keyed)
        for _, (geometry, candidate), _ in keyed:
            geometries[candidate].add(geometry)
        counts = Counter(key for _, key, _ in keyed)
        collisions += sum(count for count in counts.values() if count > 1)
        unique[frame] = {
            key: (index, label)
            for index, key, label in keyed
            if counts[key] == 1
        }

    shared = changed = labelled = false = 0
    for previous, current in zip(frames, frames[1:]):
        if current != previous + 1:
            continue
        for key in unique[previous].keys() & unique[current].keys():
            old_index, old_label = unique[previous][key]
            new_index, new_label = unique[current][key]
            shared += 1
            changed += old_index != new_index
            if old_label is not None and new_label is not None:
                labelled += 1
                false += old_label != new_label

    return {
        "total_rows": sum(len(items) for items in by_frame.values()),
        "frames": len(frames),
        "missing_candidates": missing,
        "rows_in_same_frame_collisions": collisions,
        "candidate_geometry_reuse": sum(len(g) > 1 for g in geometries.values()),
        "shared_links": shared,
        "changed_row_order": changed,
        "labelled_links": labelled,
        "false_label_links": false,
        "persistent_identity_proven": False,
    }
