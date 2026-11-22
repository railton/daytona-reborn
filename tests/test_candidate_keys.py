import unittest

from tools.audit_candidate_keys import audit

KEYS = (
    "total_rows",
    "frames",
    "missing_candidates",
    "rows_in_same_frame_collisions",
    "candidate_geometry_reuse",
    "shared_links",
    "changed_row_order",
    "labelled_links",
    "false_label_links",
)
G1, G2, G3 = (0, 0, 1, 1), (1, 1, 2, 2), (2, 2, 3, 3)


def row(frame, geometry, candidate, label=None):
    return {"frame": frame, "geometry": geometry, "candidate": candidate, "label": label}


class CandidateKeyAuditTests(unittest.TestCase):
    def test_permuted_labelled_objects(self):
        rows = [
            row(0, G1, (1,), "a"), row(0, G2, (2,), "b"), row(0, G3, (3,), "c"),
            row(1, G3, (3,), "c"), row(1, G1, (1,), "a"), row(1, G2, (2,), "b"),
            row(2, G2, (2,), "b"), row(2, G3, (3,), "c"), row(2, G1, (1,), "a"),
        ]
        result = audit(rows)
        self.assertEqual(tuple(result[key] for key in KEYS), (9, 3, 0, 0, 0, 6, 6, 6, 0))
        self.assertIs(result["persistent_identity_proven"], False)

    def test_false_label_for_same_key(self):
        result = audit([row(0, G1, (1,), "a"), row(1, G1, (1,), "b")])
        self.assertEqual(
            (result["shared_links"], result["labelled_links"], result["false_label_links"]),
            (1, 1, 1),
        )

    def test_same_geometry_replacement_is_not_a_link(self):
        result = audit([row(0, G1, (1,), "a"), row(1, G1, (2,), "b")])
        self.assertEqual((result["shared_links"], result["false_label_links"]), (0, 0))

    def test_candidate_recycling_counts_geometry_reuse_only(self):
        rows = [row(0, G1, (7,), "a"), row(1, G2, (7,), "a"), row(2, G3, (7,), "a")]
        result = audit(rows)
        self.assertEqual((result["shared_links"], result["candidate_geometry_reuse"]), (0, 1))

    def test_duplicate_key_is_ambiguous_from_both_frames(self):
        rows = [row(0, G1, (1,), "a"), row(0, G1, (1,), "b"), row(1, G1, (1,), "a")]
        result = audit(rows)
        self.assertEqual(
            (result["rows_in_same_frame_collisions"], result["shared_links"], result["labelled_links"]),
            (2, 0, 0),
        )

    def test_duplicate_only_in_later_frame_excludes_both_links(self):
        result = audit([
            row(0, G1, (1,), "a"),
            row(1, G1, (1,), "a"), row(1, G1, (1,), "b"),
            row(2, G1, (1,), "b"),
        ])
        self.assertEqual(
            tuple(result[key] for key in (
                "rows_in_same_frame_collisions", "shared_links",
                "labelled_links", "false_label_links")),
            (2, 0, 0, 0),
        )

    def test_stationary_row_positions_do_not_establish_identity(self):
        result = audit([
            row(0, G1, (1,), "a"), row(0, G2, (2,), "b"),
            row(1, G1, (1,), "replacement"), row(1, G2, (2,), "b"),
        ])
        self.assertEqual(
            tuple(result[key] for key in (
                "shared_links", "changed_row_order", "labelled_links", "false_label_links")),
            (2, 0, 2, 1),
        )
        self.assertIs(result["persistent_identity_proven"], False)

    def test_none_candidates_are_counted_and_excluded(self):
        rows = [row(0, G1, None), row(0, G1, None, "x"), row(1, G1, (1,))]
        result = audit(rows)
        self.assertEqual(
            (result["missing_candidates"], result["rows_in_same_frame_collisions"], result["shared_links"]),
            (2, 0, 0),
        )

    def test_gap_frames_are_not_compared(self):
        result = audit([row(0, G1, (1,), "a"), row(2, G1, (1,), "a")])
        self.assertEqual((result["frames"], result["shared_links"]), (2, 0))

    def test_missing_labels_do_not_confirm_identity(self):
        rows = [row(0, G1, (1,)), row(1, G1, (1,)), row(2, G1, (1,), "a")]
        result = audit(rows)
        self.assertEqual(
            (result["shared_links"], result["labelled_links"], result["false_label_links"]),
            (2, 0, 0),
        )
        self.assertIs(result["persistent_identity_proven"], False)

    def test_empty_input_has_aggregate_shape(self):
        self.assertEqual(
            audit([]),
            {
                "total_rows": 0,
                "frames": 0,
                "missing_candidates": 0,
                "rows_in_same_frame_collisions": 0,
                "candidate_geometry_reuse": 0,
                "shared_links": 0,
                "changed_row_order": 0,
                "labelled_links": 0,
                "false_label_links": 0,
                "persistent_identity_proven": False,
            },
        )

    def test_inputs_are_not_mutated(self):
        rows = [row(0, G1, (1,), "a")]
        expected = [dict(item) for item in rows]
        audit(rows)
        self.assertEqual(rows, expected)

    def test_invalid_rows_are_rejected(self):
        invalid = [
            [],
            {"geometry": G1, "candidate": (1,)},
            {"frame": True, "geometry": G1, "candidate": (1,)},
            {"frame": -1, "geometry": G1, "candidate": (1,)},
            {"frame": 0, "geometry": list(G1), "candidate": (1,)},
            {"frame": 0, "geometry": (0, 0, 1), "candidate": (1,)},
            {"frame": 0, "geometry": (0, 0, 1, True), "candidate": (1,)},
            {"frame": 0, "geometry": G1},
            {"frame": 0, "geometry": G1, "candidate": [1]},
            {"frame": 0, "geometry": G1, "candidate": (1, -1)},
            {"frame": 0, "geometry": G1, "candidate": (True,)},
            {"frame": 0, "geometry": G1, "candidate": (1,), "label": ""},
            {"frame": 0, "geometry": G1, "candidate": (1,), "label": 3},
        ]
        for bad in invalid:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                audit([bad])
        for bad in (None, 3):
            with self.assertRaises(ValueError):
                audit(bad)
