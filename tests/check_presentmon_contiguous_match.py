"""Exercise global matching through the public timestamp API."""

from fractions import Fraction
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from presentmon_qpc_match import match_contiguous_returns, match_returns


class ContiguousMatchTests(unittest.TestCase):
    def match(self, source, events, lag=2, early=2, low=0, high=0):
        return match_contiguous_returns(
            source, list(map(Fraction, events)), 1_000_000,
            Fraction(low), Fraction(high), max_lag_us=lag, early_us=early,
        )

    def test_strict_rejection_can_be_global_success(self):
        source = [0, 10_000, 20_000]
        events = [0, 1, 10, 20]
        with self.assertRaisesRegex(ValueError, "multiple ETW"):
            match_returns(source, list(map(Fraction, events)), 1_000_000,
                          Fraction(0), Fraction(0), max_lag_us=2, early_us=2)
        self.assertEqual(self.match(source, events), [1, 2, 3])

    def test_multiple_global_starts_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "multiple contiguous"):
            self.match([0, 1000], [0, 1, 2])

    def test_extra_event_between_matches_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "no contiguous"):
            self.match([0, 10_000], [0, 5, 10], lag=1, early=1)

    def test_endpoint_instability_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "uncertain"):
            self.match([0], [2], lag=0, early=1, low=0, high=2)

    def test_clean_match_at_both_endpoints(self):
        self.assertEqual(self.match([0], [1], lag=0, early=1, low=0, high=1), [0])


if __name__ == "__main__":
    unittest.main()
