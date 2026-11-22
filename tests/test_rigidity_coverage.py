"""Independent tests for exact worst-frame coverage cliques."""

import unittest
from fractions import Fraction
from itertools import combinations

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from rigidity_coverage import best_coverage_clique


def _link(pairs):
    adjacency = {}
    for left, right in pairs:
        adjacency.setdefault(left, set()).add(right)
        adjacency.setdefault(right, set()).add(left)
    return adjacency


def _beats(key, best):
    if key[0] != best[0]:
        return key[0] > best[0]
    if key[1] != best[1]:
        return key[1] > best[1]
    if key[2] != best[2]:
        return key[2] > best[2]
    return key[3] < best[3]


def exhaustive_oracle(nodes, edges, weights_by_frame):
    """Score every subset independently of the recursive search."""
    ordered = sorted(set(nodes))
    adjacency = {node: set() for node in ordered}
    for node, neighbors in edges.items():
        for other in neighbors:
            if other != node:
                adjacency[node].add(other)
                adjacency[other].add(node)
    frame_count = len(weights_by_frame)
    totals = [
        sum(weights_by_frame[frame][node] for node in ordered)
        for frame in range(frame_count)
    ]
    best = None
    for size in range(len(ordered) + 1):
        for subset in combinations(ordered, size):
            if any(
                right not in adjacency[left]
                for left, right in combinations(subset, 2)
            ):
                continue
            frame_weights = [
                sum(weights_by_frame[frame][node] for node in subset)
                for frame in range(frame_count)
            ]
            score = min(
                Fraction(0)
                if totals[frame] == 0
                else Fraction(frame_weights[frame], totals[frame])
                for frame in range(frame_count)
            )
            raw = sum(frame_weights)
            key = (score, raw, size, subset)
            if best is None or _beats(key, best):
                best = key
    return list(best[3])


class ExactCoverageCliqueTests(unittest.TestCase):
    def test_later_minority_beats_frame0_majority(self):
        nodes = ["a", "b", "c", "d"]
        edges = _link([("a", "b"), ("c", "d")])
        weights = [
            {"a": 9, "b": 9, "c": 2, "d": 2},
            {"a": 1, "b": 1, "c": 9, "d": 9},
        ]
        # Frame 0 alone prefers {a, b}; worst-frame coverage prefers {c, d}.
        self.assertEqual(best_coverage_clique(nodes, edges, weights), ["c", "d"])
        self.assertEqual(
            best_coverage_clique(nodes, edges, weights[:1]), ["a", "b"]
        )

    def test_balanced_pair_beats_frame0_winner(self):
        nodes = ["a", "b", "c", "d", "e", "f"]
        edges = _link([("a", "b"), ("c", "d"), ("e", "f")])
        weights = [
            {"a": 8, "b": 8, "c": 1, "d": 1, "e": 3, "f": 3},
            {"a": 0, "b": 0, "c": 0, "d": 0, "e": 5, "f": 5},
            {"a": 2, "b": 0, "c": 4, "d": 0, "e": 4, "f": 4},
        ]
        found = best_coverage_clique(nodes, edges, weights)
        self.assertEqual(found, ["e", "f"])
        self.assertEqual(found, exhaustive_oracle(nodes, edges, weights))

    def test_tie_is_stable_under_reordered_input(self):
        nodes = ["a", "b", "c", "d"]
        edges = _link([("a", "b"), ("c", "d")])
        weights = [
            {"a": 4, "b": 4, "c": 4, "d": 4},
            {"a": 2, "b": 2, "c": 2, "d": 2},
        ]
        expected = ["a", "b"]
        self.assertEqual(best_coverage_clique(nodes, edges, weights), expected)
        reversed_nodes = ["d", "c", "b", "a"]
        reversed_edges = {"d": {"c"}, "b": {"a"}}
        reversed_weights = [
            {"d": 4, "c": 4, "b": 4, "a": 4},
            {"b": 2, "a": 2, "d": 2, "c": 2},
        ]
        self.assertEqual(
            best_coverage_clique(
                reversed_nodes, reversed_edges, reversed_weights
            ),
            expected,
        )
        swapped_frames = list(reversed(weights))
        self.assertEqual(
            best_coverage_clique(nodes, edges, swapped_frames), expected
        )

    def test_zero_weight_partner_increases_cardinality(self):
        nodes = ["a", "b"]
        edges = _link([("a", "b")])
        weights = [{"a": 3, "b": 0}, {"a": 1, "b": 0}]
        self.assertEqual(best_coverage_clique(nodes, edges, weights), ["a", "b"])

    def test_zero_total_frame_forces_raw_weight_tiebreak(self):
        nodes = ["a", "b", "c"]
        edges = _link([("a", "b"), ("b", "c")])
        weights = [
            {"a": 0, "b": 0, "c": 0},
            {"a": 1, "b": 5, "c": 4},
        ]
        found = best_coverage_clique(nodes, edges, weights)
        self.assertEqual(found, ["b", "c"])
        self.assertEqual(found, exhaustive_oracle(nodes, edges, weights))

    def test_all_zero_weights_return_lexicographic_max_clique(self):
        nodes = ["c", "a", "b"]
        edges = _link([("a", "b"), ("b", "c"), ("a", "c")])
        weights = [{"a": 0, "b": 0, "c": 0}]
        self.assertEqual(
            best_coverage_clique(nodes, edges, weights), ["a", "b", "c"]
        )

    def test_empty_and_singleton_graphs(self):
        weights = [{}]
        self.assertEqual(best_coverage_clique([], {}, weights), [])
        self.assertEqual(
            best_coverage_clique(["z"], {}, [{"z": 4}, {"z": 1}]), ["z"]
        )
        isolated = best_coverage_clique(
            ["a", "b"],
            {},
            [{"a": 1, "b": 100}, {"a": 100, "b": 1}],
        )
        self.assertEqual(isolated, ["a"])

    def test_invalid_weights_raise(self):
        nodes = ["a", "b"]
        edges = _link([("a", "b")])
        with self.assertRaises(ValueError):
            best_coverage_clique(nodes, edges, [])
        with self.assertRaises(ValueError):
            best_coverage_clique(
                nodes, edges, [{"a": -1, "b": 1}]
            )
        with self.assertRaises(ValueError):
            best_coverage_clique(nodes, edges, [{"a": 1}])
        with self.assertRaises(ValueError):
            best_coverage_clique(
                nodes, edges, [{"a": 1.5, "b": 1}]
            )
        with self.assertRaises(ValueError):
            best_coverage_clique(
                nodes, edges, [{"a": True, "b": 1}]
            )
        with self.assertRaises(ValueError):
            best_coverage_clique(nodes, edges, ["ab"])
        with self.assertRaises(ValueError):
            best_coverage_clique(nodes, edges, {"a": 1, "b": 1})

    def test_oracle_on_deterministic_multiframe_graphs(self):
        graphs = [
            (
                ["a", "b", "c", "d"],
                _link([("a", "b"), ("b", "c"), ("c", "d"), ("a", "c")]),
                [
                    {"a": 5, "b": 0, "c": 2, "d": 7},
                    {"a": 1, "b": 4, "c": 0, "d": 1},
                    {"a": 3, "b": 3, "c": 3, "d": 0},
                ],
            ),
            (
                ["a", "b", "c", "d", "e"],
                _link([("a", "b"), ("a", "c"), ("b", "c"), ("c", "d"), ("d", "e")]),
                [
                    {"a": 2, "b": 2, "c": 2, "d": 9, "e": 1},
                    {"a": 6, "b": 0, "c": 5, "d": 0, "e": 4},
                ],
            ),
            (
                [1, 2, 3, 4],
                _link([(1, 2), (2, 3), (3, 4), (4, 1), (1, 3)]),
                [
                    {1: 0, 2: 0, 3: 0, 4: 0},
                    {1: 4, 2: 1, 3: 4, 4: 1},
                    {1: 2, 2: 8, 3: 1, 4: 0},
                ],
            ),
            (
                ["p", "q", "r"],
                _link([("p", "q"), ("q", "r")]),
                [
                    {"p": 7, "q": 1, "r": 7},
                    {"p": 0, "q": 5, "r": 2},
                    {"p": 3, "q": 3, "r": 0},
                ],
            ),
        ]
        for nodes, edges, weights in graphs:
            self.assertEqual(
                best_coverage_clique(list(reversed(nodes)), edges, weights),
                exhaustive_oracle(nodes, edges, weights),
            )


if __name__ == "__main__":
    unittest.main()
