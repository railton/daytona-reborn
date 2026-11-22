import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.validate_race_lifecycle import main, validate


def journal(*rows):
    return "\n".join(json.dumps(dict(event=e, frame=f, status=s)) for e, f, s in rows) + "\n"


BEGIN = ("begin", 0, "starting")
START = ("transition", 837, "racing")
RETURN = ("transition", 7741, "finished")
END = ("end", 20000, "finished")


class LifecycleTests(unittest.TestCase):
    def test_startup_can_be_cancelled_before_first_tick(self):
        text = journal(BEGIN, ('transition', 0, 'cancelled'), ('end', 0, 'cancelled'))
        for requirement in ('started', 'returned'):
            report = validate(text, require=requirement)
            self.assertTrue(report['valid'])
            self.assertFalse(report['passed'])
            self.assertEqual(report['end_frame'], 0)
        for state in ('racing', 'finished', 'timed_out', 'wrong_course', 'wrong_mode', 'wrong_transmission'):
            with self.subTest(state=state), self.assertRaises(ValueError):
                validate(journal(BEGIN, ('transition', 0, state), ('end', 0, state)))

    def test_complete_cycle_does_not_claim_victory_or_parity(self):
        report = validate(journal(BEGIN, START, RETURN, END), expect_frames=20000)
        self.assertTrue(report["passed"])
        self.assertEqual(report["start_frame"], 837)
        self.assertEqual(report["return_to_attract_frame"], 7741)
        self.assertFalse(report["race_victory_verified"])
        self.assertFalse(report["arcade_parity_verified"])

    def test_early_exit_is_not_return_to_attract(self):
        text = journal(BEGIN, START, ("end", 1400, "racing"))
        self.assertTrue(validate(text, require="started")["passed"])
        self.assertEqual(validate(text)["failures"], ["no_return_to_attract"])

    def test_no_start_and_terminal_failure_are_not_success(self):
        for state in ("wrong_course", "wrong_mode", "wrong_transmission", "timed_out", "cancelled"):
            with self.subTest(state=state):
                report = validate(journal(BEGIN, ("transition", 100, state), ("end", 110, state)))
                self.assertFalse(report["passed"])
                self.assertIn("race_not_started", report["failures"])
        self.assertFalse(validate(journal(BEGIN, ("end", 0, "starting")))["passed"])

    def test_last_tick_transition_can_equal_end_tick(self):
        self.assertTrue(validate(journal(BEGIN, START, RETURN, ("end", 7741, "finished")))["passed"])

    def test_expected_length_mismatch(self):
        self.assertEqual(validate(journal(BEGIN, START, RETURN, END), expect_frames=19999)["failures"],
                         ["unexpected_end_frame"])

    def test_interrupted_and_reordered_journals_are_invalid(self):
        cases = [(), (BEGIN,), (BEGIN, START), (BEGIN, RETURN, END),
                 (BEGIN, START, START, END),
                 (BEGIN, START, ("transition", 837, "finished"), END),
                 (BEGIN, START, ("end", 800, "racing")),
                 (BEGIN, START, END), (BEGIN, START, RETURN, START, END),
                 (BEGIN, START, ("end", 1000, "racing"), END),
                 (BEGIN, ("begin", 1, "starting"), END),
                 (("begin", 1, "starting"), START, RETURN, END)]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                validate(journal(*rows))

    def test_strict_json_schema(self):
        text = journal(BEGIN, START, RETURN, END)
        bad_lines = ['{}', '[]', 'null', '{', '',
                     '{"event":"begin","frame":0,"frame":1,"status":"starting"}',
                     '{"event":"begin","frame":true,"status":"starting"}',
                     '{"event":"begin","frame":0.0,"status":"starting"}',
                     '{"event":"begin","frame":-1,"status":"starting"}',
                     '{"event":"begin","frame":0,"status":[]}',
                     '{"event":"begin","frame":0,"status":"unknown"}']
        for line in bad_lines:
            with self.subTest(line=line), self.assertRaises(ValueError):
                validate(line + "\n" + text)

    def test_invalid_requirements(self):
        for kwargs in ({"require": "won"}, {"expect_frames": -1}, {"expect_frames": True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                validate(journal(BEGIN, START, RETURN, END), **kwargs)

    def test_excessively_nested_json_is_malformed_evidence(self):
        depth = max(10000, sys.getrecursionlimit() + 100)
        text = '[' * depth + '0' + ']' * depth
        with self.assertRaises(ValueError):
            validate(text)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'nested.jsonl'
            path.write_text(text, encoding='utf-8')
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(main([str(path)]), 2)
            self.assertFalse(json.loads(output.getvalue())['valid'])

    def test_startup_failure_after_racing_is_not_a_native_transition(self):
        # RaceSession::observe maps startup errors only while Starting;
        # RaceSession::cancel also rejects cancellation after Racing.
        for state in ('timed_out', 'wrong_course', 'wrong_mode', 'wrong_transmission', 'cancelled'):
            with self.subTest(state=state), self.assertRaises(ValueError):
                validate(journal(BEGIN, START, ('transition', 900, state), ('end', 1000, state)))

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "race.jsonl"
            for text, expected in [(journal(BEGIN, START, RETURN, END), 0),
                                   (journal(BEGIN, START, ("end", 1400, "racing")), 1),
                                   (journal(BEGIN, START), 2)]:
                path.write_text(text, encoding="utf-8")
                output = io.StringIO()
                with redirect_stdout(output):
                    self.assertEqual(main([str(path)]), expected)
                self.assertEqual(json.loads(output.getvalue())["passed"], expected == 0)
            path.unlink()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(path)]), 2)


if __name__ == "__main__":
    unittest.main()
