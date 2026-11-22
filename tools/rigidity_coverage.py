"""Exact worst-frame coverage clique. Stdlib only."""

from fractions import Fraction

_MAX_NODES = 22
_MAX_VISITS = 100_000


class ExactSearchLimit(ValueError):
    """No exact answer was produced within the explicit diagnostic budget."""



def best_coverage_clique(nodes, edges, weights_by_frame):
    """Return the sorted nodes of the optimal clique.

    Maximizes the minimum over frames of
    sum(weight[f][v] for v in clique) / sum(weight[f][v] for v in nodes).
    A frame whose vertex weights sum to zero contributes coverage 0.
    Scores compare as fractions.Fraction. Remaining ties favor higher total
    raw weight across frames, then higher cardinality, then the
    lexicographically smallest sorted tuple. The empty set and every
    singleton are candidates.
    """
    ordered = _nodes(nodes)
    frames = _frames(weights_by_frame, ordered)
    adj = _adjacency(edges, ordered)
    if len(ordered) > _MAX_NODES:
        raise ExactSearchLimit(
            "graph exceeds exact search bound of %d nodes" % _MAX_NODES
        )

    n = len(ordered)
    fcount = len(frames)
    vertex_w = [[frames[f][v] for f in range(fcount)] for v in ordered]
    vertex_raw = [sum(row) for row in vertex_w]
    totals = [0] * fcount
    for row in vertex_w:
        for index, weight in enumerate(row):
            totals[index] += weight

    best = None
    visits = 0

    def coverage(weights):
        return min(
            Fraction(0)
            if totals[index] == 0
            else Fraction(weights[index], totals[index])
            for index in range(fcount)
        )

    def consider(chosen, weights, raw_sum):
        nonlocal best
        key = (coverage(weights), raw_sum, len(chosen), tuple(chosen))
        if best is None or _beats(key, best):
            best = key

    def rec(start, chosen, weights, raw_sum):
        nonlocal visits
        visits += 1
        if visits > _MAX_VISITS:
            raise ExactSearchLimit("exact search exceeded %d visits" % _MAX_VISITS)
        consider(chosen, weights, raw_sum)
        candidates = [
            index
            for index in range(start, n)
            if all(ordered[index] in adj[node] for node in chosen)
        ]
        if not candidates:
            return
        bound_weights = weights[:]
        bound_raw = raw_sum
        for index in candidates:
            for frame in range(fcount):
                bound_weights[frame] += vertex_w[index][frame]
            bound_raw += vertex_raw[index]
        bound_key = (
            coverage(bound_weights),
            bound_raw,
            len(chosen) + len(candidates),
        )
        # Weights were validated nonnegative, so this bound cannot exclude
        # a better score, raw total, or cardinality. Equal bounds still search.
        if best is not None and bound_key < best[:3]:
            return
        for index in candidates:
            chosen.append(ordered[index])
            for frame in range(fcount):
                weights[frame] += vertex_w[index][frame]
            rec(index + 1, chosen, weights, raw_sum + vertex_raw[index])
            for frame in range(fcount):
                weights[frame] -= vertex_w[index][frame]
            chosen.pop()

    rec(0, [], [0] * fcount, 0)
    return list(best[3])


def _beats(key, best):
    if key[0] != best[0]:
        return key[0] > best[0]
    if key[1] != best[1]:
        return key[1] > best[1]
    if key[2] != best[2]:
        return key[2] > best[2]
    return key[3] < best[3]


def _nodes(nodes):
    if isinstance(nodes, (str, bytes)):
        raise ValueError("nodes must be an iterable of keys, not a string")
    try:
        sequence = list(nodes)
    except TypeError as exc:
        raise ValueError("nodes must be an iterable of hashable keys") from exc
    try:
        if len(set(sequence)) != len(sequence):
            raise ValueError("duplicate nodes")
    except TypeError as exc:
        raise ValueError("nodes must be hashable") from exc
    try:
        return sorted(sequence)
    except TypeError as exc:
        raise ValueError("nodes must be mutually comparable") from exc


def _frames(weights_by_frame, ordered):
    if isinstance(weights_by_frame, (str, bytes, dict)):
        raise ValueError("weights_by_frame must be a nonempty sequence of dicts")
    try:
        frames = list(weights_by_frame)
    except TypeError as exc:
        raise ValueError("weights_by_frame must be a nonempty sequence of dicts") from exc
    if not frames:
        raise ValueError("weights_by_frame must be nonempty")
    needed = set(ordered)
    parsed_frames = []
    for frame_index, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise ValueError("frame %d must be a dict" % frame_index)
        if set(frame) != needed:
            raise ValueError("frame %d keys do not match nodes" % frame_index)
        parsed_frames.append(
            {
                node: _as_weight(frame[node], node, frame_index)
                for node in ordered
            }
        )
    return parsed_frames


def _as_weight(value, node, frame_index):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(
            "non-integer weight for %r in frame %d" % (node, frame_index)
        )
    if value < 0:
        raise ValueError(
            "negative weight for %r in frame %d" % (node, frame_index)
        )
    return value


def _adjacency(edges, ordered):
    if not isinstance(edges, dict):
        raise ValueError("edges must be a dict of adjacency sets")
    known = set(ordered)
    adjacency = {node: set() for node in ordered}
    for node, neighbors in edges.items():
        if node not in known:
            raise ValueError("edge endpoint not in nodes: %r" % (node,))
        if isinstance(neighbors, (str, bytes)):
            raise ValueError("adjacency of %r must be a set of nodes" % (node,))
        try:
            neighbor_list = list(neighbors)
        except TypeError as exc:
            raise ValueError("adjacency of %r must be iterable" % (node,)) from exc
        for other in neighbor_list:
            if other == node:
                continue
            if other not in known:
                raise ValueError("edge endpoint not in nodes: %r" % (other,))
            adjacency[node].add(other)
            adjacency[other].add(node)
    return adjacency
