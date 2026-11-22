"""Exact writer/port event-key join. Frame is never an identity."""

import contextlib
import csv
import io
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import audit_writer_trace as audit
import join_writer_observations as join


UINT64_MAX = str((1 << 64) - 1)
UINT32_MAX = str((1 << 32) - 1)

# Independent of join.PORT_COLUMNS. Lock the two schemas together in tests.
PORT_HEADER = (
    "sequence",
    "frame",
    "address",
    "lane_mask",
    "data_masked",
    "buffer_cursor",
    "cpu_state_present",
    "observed_ip",
    "observed_count",
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r5",
    "r6",
    "r7",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "r13",
    "r14",
    "r15",
    "r16",
    "r17",
    "r18",
    "r19",
    "r20",
    "r21",
    "r22",
    "r23",
    "r24",
    "r25",
    "r26",
    "r27",
    "r28",
    "r29",
    "r30",
    "r31",
    "buffer_provenance_available",
    "buffer_write_observed",
    "buffer_word_written",
    "buffer_write_count",
)


def writer(word, writes, mask="4294967295", frame="1"):
    return {
        "frame": str(frame),
        "port_mask": str(mask),
        "writes": str(writes),
        "buffer_word": str(word),
    }


def port(sequence, word, count, observed="1", provenance="1", frame="9",
         present="0"):
    row = {name: "0" for name in PORT_HEADER}
    row.update({
        "sequence": str(sequence),
        "frame": str(frame),
        "cpu_state_present": present,
        "buffer_provenance_available": provenance,
        "buffer_write_observed": observed,
        "buffer_word_written": str(word),
        "buffer_write_count": str(count),
    })
    return row


def valid_writer_row(frame, command, argument, writes, origin):
    """Build one audit-legal writer row. origin is 'port', 'unknown', or 'i960'."""
    word = (int(command) + int(argument)) & 0x7FFF
    row = {name: "0" for name in audit.HEADER}
    row["frame"] = str(frame)
    row["command_word"] = str(command)
    row["argument_index"] = str(argument)
    row["buffer_word"] = str(word)
    row["writes"] = str(writes)
    if int(writes) == 0:
        row["unknown_mask"] = UINT32_MAX
        row["last_mask"] = "0"
        return row
    row["last_mask"] = "1"
    if origin == "port":
        row["port_mask"] = UINT32_MAX
    elif origin == "unknown":
        row["unknown_mask"] = UINT32_MAX
    else:
        row["i960_mask"] = UINT32_MAX
    return row


def write_csv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        output = csv.DictWriter(handle, fieldnames=list(header), extrasaction="ignore")
        output.writeheader()
        for row in rows:
            output.writerow(row)


class JoinRowsTest(unittest.TestCase):
    def test_port_schema_is_the_literal_45(self):
        self.assertEqual(len(PORT_HEADER), 45)
        self.assertEqual(len(set(PORT_HEADER)), 45)
        self.assertEqual(join.PORT_COLUMNS, PORT_HEADER)

    def test_exact_match_ignores_row_order(self):
        writers = [writer(4, 2, frame=3), writer(1, 7, frame=8)]
        ports = [port(0, 1, 7, frame=1), port(1, 4, 2, frame=90)]
        result = join.join_rows(writers, ports)
        self.assertEqual(result["matches"], {"4:2": 1, "1:7": 0})
        self.assertEqual(result["summary"]["matched_writer_rows"], 2)
        self.assertEqual(result["summary"]["unmatched_writer_rows"], 0)
        self.assertEqual(result["summary"]["port_rows"], 2)

    def test_same_slot_different_count_does_not_match(self):
        result = join.join_rows([writer(5, 2)], [port(0, 5, 1), port(1, 5, 3)])
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["unmatched_writer_rows"], 1)

    def test_repeated_target_key_keeps_multiplicity(self):
        writers = [writer(6, 4, frame=1), writer(6, 4, frame=2), writer(6, 4, frame=9)]
        result = join.join_rows(writers, [port(0, 6, 4, frame=100)])
        self.assertEqual(result["matches"], {"6:4": 0})
        self.assertEqual(result["summary"]["matched_writer_rows"], 3)
        self.assertEqual(result["summary"]["distinct_target_keys"], 1)
        self.assertEqual(result["summary"]["eligible_writer_rows"], 3)

    def test_reads_before_overwrite_keep_the_previous_version(self):
        writers = [writer(100, 1, frame=10), writer(100, 1, frame=11),
                   writer(100, 2, frame=11)]
        ports = [port(0, 100, 1, frame=10), port(1, 100, 2, frame=11)]
        result = join.join_rows(writers, ports)
        self.assertEqual(result["matches"], {"100:1": 0, "100:2": 1})
        self.assertEqual(result["summary"]["matched_writer_rows"], 3)
        self.assertEqual(result["summary"]["distinct_target_keys"], 2)
        self.assertEqual(result["summary"]["ambiguous_writer_rows"], 0)

    def test_missing_event_is_unmatched(self):
        result = join.join_rows([writer(2, 1)], [port(0, 0, 0, observed="0")])
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["unmatched_writer_rows"], 1)
        self.assertEqual(result["summary"]["matched_writer_rows"], 0)

    def test_duplicate_port_key_is_ambiguous(self):
        ports = [port(0, 3, 5, frame=1), port(1, 3, 5, frame=2)]
        result = join.join_rows([writer(3, 5), writer(3, 5, frame=4)], ports)
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["ambiguous_writer_rows"], 2)
        self.assertEqual(result["summary"]["matched_writer_rows"], 0)

    def test_third_port_duplicate_stays_ambiguous(self):
        ports = [port(index, 3, 5) for index in range(3)]
        result = join.join_rows([writer(3, 5)], ports)
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["ambiguous_writer_rows"], 1)
        self.assertEqual(result["summary"]["port_rows"], 3)

    def test_other_origins_are_ineligible(self):
        writers = [
            writer(1, 1, mask="1"),
            writer(1, 0),
            writer(2, 4),
        ]
        result = join.join_rows(writers, [port(0, 1, 1), port(1, 2, 4)])
        self.assertEqual(result["summary"]["ineligible_writer_rows"], 2)
        self.assertEqual(result["summary"]["eligible_writer_rows"], 1)
        self.assertEqual(result["summary"]["distinct_target_keys"], 1)
        self.assertEqual(result["matches"], {"2:4": 1})

    def test_uint64_max_counter(self):
        result = join.join_rows(
            [writer(32767, UINT64_MAX)],
            [port(0, 32767, UINT64_MAX)],
        )
        self.assertEqual(result["matches"], {f"32767:{UINT64_MAX}": 0})

    def test_leading_zeros_match_numeric_key(self):
        result = join.join_rows(
            [writer("01", "02")],
            [port("00", "01", "02")],
        )
        self.assertEqual(result["matches"], {"1:2": 0})

    def test_frame_alone_does_not_match(self):
        writers = [writer(1, 1, frame=7)]
        ports = [port(0, 2, 1, frame=7)]
        result = join.join_rows(writers, ports)
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["unmatched_writer_rows"], 1)

    def test_no_write_requires_zero_word_and_count(self):
        result = join.join_rows(
            [],
            [port(0, 0, 0, observed="0", provenance="1"),
             port(1, 0, 0, observed="0", provenance="0")],
        )
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["port_rows"], 2)

    def test_malformed_observed_write_flags_count_seq_and_word(self):
        cases = [
            [port(0, 1, 1, provenance="0")],
            [port(0, 1, 0)],
            [port(0, 1, 1, observed="2")],
            [port(0, 4, "-1")],
            [port(0, 4, "1.5")],
            [port(1, 1, 1)],
            [port(0, 1, 1), port(2, 1, 1)],
            [port(0, 32768, 1)],
            [port(0, "+1", 1)],
            [port(0, 1, UINT64_MAX + "0")],
            [port(0, 3, 1, observed="0")],
        ]
        for rows in cases:
            with self.assertRaises(ValueError):
                join.join_rows([], rows)

    def test_malformed_writer_fields(self):
        for row in (writer("-1", 1), writer(1, "+2"), writer(1, "1.0"), {"writes": "1"}):
            with self.assertRaises(ValueError):
                join.join_rows([row], [])

    def test_cpu_state_unknown_is_not_inferred(self):
        absent = port(0, 8, 2, present="0")
        result = join.join_rows([writer(8, 2, frame=0)], [absent])
        self.assertEqual(result["matches"], {"8:2": 0})
        self.assertNotIn("observed_ip", result["summary"])
        present = port(0, 8, 2, present="1")
        present["observed_ip"] = "16"
        present["observed_count"] = "3"
        present["r0"] = "99"
        result = join.join_rows([writer(8, 2)], [present])
        self.assertEqual(result["matches"], {"8:2": 0})
        self.assertNotIn("99", str(result))
        self.assertNotIn("observed_ip", result["summary"])

    def test_absent_cpu_rejects_nonzero_regs_ip_and_count(self):
        probes = {
            "observed_ip": "1",
            "observed_count": "2",
            "r0": "3",
            "r31": "4",
        }
        for field, raw in probes.items():
            row = port(0, 1, 1, present="0")
            row[field] = raw
            with self.assertRaises(ValueError) as caught:
                join.join_rows([], [row])
            self.assertEqual(str(caught.exception), "malformed " + field)
            self.assertNotIn(raw, str(caught.exception))

    def test_stream_sequence_reorder_is_rejected(self):
        later = port(0, 8, 1)
        earlier = port(1, 9, 1)
        with self.assertRaises(ValueError) as caught:
            join.join_rows([], [earlier, later])
        self.assertEqual(str(caught.exception), "malformed sequence")
        kept = join.join_rows(
            [writer(8, 1), writer(9, 1)],
            [port(0, 9, 1), port(1, 8, 1)],
        )
        self.assertEqual(kept["matches"], {"9:1": 0, "8:1": 1})

    def test_iterator_inputs_are_single_pass(self):
        seen = {"writers": 0, "ports": 0}

        def writers():
            seen["writers"] += 1
            yield writer(4, 2)
            yield writer(1, 7)

        def ports():
            seen["ports"] += 1
            yield port(0, 1, 7)
            yield port(1, 4, 2)

        writer_stream = writers()
        port_stream = ports()
        result = join.join_rows(writer_stream, port_stream)
        self.assertEqual(result["matches"], {"4:2": 1, "1:7": 0})
        self.assertEqual(seen, {"writers": 1, "ports": 1})
        with self.assertRaises(StopIteration):
            next(writer_stream)
        with self.assertRaises(StopIteration):
            next(port_stream)

    def test_collect_matches_can_be_disabled(self):
        result = join.join_rows(
            [writer(1, 1), writer(1, 1)],
            [port(0, 1, 1)],
            collect_matches=False,
        )
        self.assertEqual(result["matches"], {})
        self.assertEqual(result["summary"]["matched_writer_rows"], 2)
        self.assertEqual(result["summary"]["distinct_target_keys"], 1)

    def test_non_target_port_keys_are_not_retained(self):
        result = join.join_rows(
            [writer(1, 1)],
            [port(0, 1, 1), port(1, 9, 9), port(2, 9, 9)],
        )
        self.assertEqual(result["matches"], {"1:1": 0})
        self.assertEqual(result["summary"]["ambiguous_writer_rows"], 0)

    def test_every_port_field_rejects_bad_tokens_without_echo(self):
        uint64 = ("frame", "observed_count", "buffer_write_count")
        uint32 = ("address", "lane_mask", "observed_ip", "buffer_cursor", "data_masked") + tuple(
            f"r{index}" for index in range(32)
        )
        flags = (
            "cpu_state_present",
            "buffer_provenance_available",
            "buffer_write_observed",
        )
        samples = {name: "nope" for name in uint64 + uint32 + flags}
        samples["sequence"] = "nope"
        samples["buffer_word_written"] = "nope"
        samples["address"] = str((1 << 32))
        samples["frame"] = "١٢"
        for field, raw in samples.items():
            row = port(0, 1, 1, present="1")
            row[field] = raw
            with self.assertRaises(ValueError) as caught:
                join.join_rows([], [row])
            message = str(caught.exception)
            self.assertEqual(message, "malformed " + field)
            self.assertNotIn(raw, message)

    def test_data_is_uint32_not_boolean(self):
        row = port(0, 1, 1)
        row["lane_mask"] = UINT32_MAX
        for value in ("2", "2147483648", UINT32_MAX):
            row["data_masked"] = value
            self.assertEqual(join.join_rows([], [row])["summary"]["port_rows"], 1)

    def test_cursor_rejects_more_than_uint32(self):
        row = port(0, 1, 1)
        row["buffer_cursor"] = str(1 << 32)
        with self.assertRaisesRegex(ValueError, "malformed buffer_cursor"):
            join.join_rows([], [row])

    def test_data_outside_lane_mask_is_rejected(self):
        row = port(0, 1, 1)
        row["lane_mask"] = "0"
        row["data_masked"] = "1"
        with self.assertRaisesRegex(ValueError, "malformed data_masked"):
            join.join_rows([], [row])

    def test_bounds_accept_max_and_ascii_decimal(self):
        row = port(0, 1, 1, present="1")
        row["address"] = UINT32_MAX
        row["lane_mask"] = UINT32_MAX
        row["observed_ip"] = UINT32_MAX
        row["r15"] = UINT32_MAX
        row["frame"] = UINT64_MAX
        row["buffer_cursor"] = UINT32_MAX
        row["observed_count"] = UINT64_MAX
        row["data_masked"] = "1"
        result = join.join_rows([], [row])
        self.assertEqual(result["summary"]["port_rows"], 1)
        blank = port(0, 1, 1)
        blank["frame"] = ""
        with self.assertRaises(ValueError) as caught:
            join.join_rows([], [blank])
        self.assertEqual(str(caught.exception), "malformed frame")

    def test_missing_and_extra_port_fields(self):
        missing = port(0, 1, 1)
        del missing["r8"]
        extra = port(0, 1, 1)
        extra["secret"] = "raw-secret"
        foreign = port(0, 1, 1)
        foreign["address"] = "raw-secret"
        for row in (missing, extra, foreign, ["sequence"]):
            with self.assertRaises(ValueError) as caught:
                join.join_rows([], [row])
            self.assertNotIn("raw-secret", str(caught.exception))
            self.assertTrue(str(caught.exception).startswith("malformed "))


class CliTest(unittest.TestCase):
    def test_summary_only_and_audits_writer_first(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            row = {name: "0" for name in audit.HEADER}
            row["port_mask"] = "4294967295"
            row["writes"] = "4"
            row["buffer_word"] = "1"
            write_csv(writer_path, audit.HEADER, [row])
            write_csv(port_path, PORT_HEADER, [port(0, 1, 4, frame=3)])
            seen = {}

            def audit_rows(rows):
                self.assertNotIsInstance(rows, list)
                words = []
                for item in rows:
                    words.append(item["buffer_word"])
                seen["words"] = words
                return {
                    "total_rows": len(words),
                    "group_count": 1,
                    "frame_count": 1,
                    "rows_with_origin": len(words),
                    "mixed_origin_count": 0,
                }

            stdout = io.StringIO()
            with patch("audit_writer_trace.audit_rows", side_effect=audit_rows):
                with contextlib.redirect_stdout(stdout):
                    code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(code, 0)
            self.assertEqual(seen["words"], ["1"])
            text = stdout.getvalue()
            self.assertTrue(text.startswith("SUMMARY "))
            self.assertIn("matched_writer_rows=1", text)
            self.assertNotIn("matches", text)
            for column in ("r0", "observed_ip", "buffer_word"):
                self.assertNotIn(column + "=", text)

    def test_malformed_port_header_is_nonzero(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            writer_path.write_text(",".join(audit.HEADER) + "\n", encoding="utf-8")
            port_path.write_text("sequence,frame\n0,1\n", encoding="utf-8")
            stderr = io.StringIO()
            with patch(
                "audit_writer_trace.audit_rows",
                return_value={"total_rows": 0},
            ) as audit_rows:
                with contextlib.redirect_stderr(stderr):
                    code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(code, 1)
            self.assertEqual(stderr.getvalue().strip(), "malformed port header")
            self.assertEqual(audit_rows.call_count, 1)
            self.assertNotIsInstance(audit_rows.call_args.args[0], list)

    def test_unmocked_three_row_direct_header_group(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            rows = [
                valid_writer_row(4, 10, 0, 4, "port"),
                valid_writer_row(4, 10, 1, 0, "unknown"),
                valid_writer_row(4, 10, 2, 9, "port"),
            ]
            self.assertEqual(list(rows[0].keys()), list(audit.HEADER))
            write_csv(writer_path, audit.HEADER, rows)
            write_csv(
                port_path,
                PORT_HEADER,
                [
                    port(0, 12, 9, frame=1),
                    port(1, 0, 0, observed="0", frame=2),
                    port(2, 10, 4, frame=8),
                ],
            )
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(stderr.getvalue(), "")
            self.assertEqual(code, 0)
            self.assertEqual(
                stdout.getvalue().strip(),
                "SUMMARY eligible_writer_rows=2 ineligible_writer_rows=1 "
                "distinct_target_keys=2 matched_writer_rows=2 "
                "ambiguous_writer_rows=0 unmatched_writer_rows=0 port_rows=3",
            )

    def test_writer_header_missing_extra_duplicate_and_reorder(self):
        base = list(audit.HEADER)
        variants = [
            base[:-1],
            base + ["extra"],
            base[:3] + ["frame"] + base[4:],
            list(reversed(base)),
        ]
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            port_path.write_text(",".join(PORT_HEADER) + "\n", encoding="utf-8")
            for names in variants:
                writer_path.write_text(",".join(names) + "\n1\n", encoding="utf-8")
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    code = join.main([str(writer_path), str(port_path)])
                self.assertEqual(code, 1)
                self.assertEqual(stderr.getvalue().strip(), "malformed writer header")

    def test_port_header_missing_extra_duplicate_and_reorder(self):
        base = list(PORT_HEADER)
        variants = [
            base[:-1],
            base + ["extra"],
            base[:4] + ["sequence"] + base[5:],
            list(reversed(base)),
        ]
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            write_csv(
                writer_path,
                audit.HEADER,
                [
                    valid_writer_row(1, 2, 0, 1, "port"),
                    valid_writer_row(1, 2, 1, 0, "unknown"),
                    valid_writer_row(1, 2, 2, 1, "i960"),
                ],
            )
            for names in variants:
                port_path.write_text(",".join(names) + "\n", encoding="utf-8")
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    code = join.main([str(writer_path), str(port_path)])
                self.assertEqual(code, 1)
                self.assertEqual(stderr.getvalue().strip(), "malformed port header")

    def test_corrupt_port_row_and_cpu_do_not_echo_cells(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "port.csv"
            write_csv(
                writer_path,
                audit.HEADER,
                [
                    valid_writer_row(1, 2, 0, 1, "port"),
                    valid_writer_row(1, 2, 1, 0, "unknown"),
                    valid_writer_row(1, 2, 2, 0, "unknown"),
                ],
            )
            header = ",".join(PORT_HEADER)
            secret_row = ",".join(["0"] * 45) + ",SECRETTOKEN"
            port_path.write_text(header + "\n" + secret_row + "\n", encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(code, 1)
            self.assertNotIn("SECRETTOKEN", stderr.getvalue())
            self.assertTrue(stderr.getvalue().strip().startswith("malformed "))

            bad = port(0, 1, 1, present="0")
            bad["r3"] = "424242"
            bad["observed_ip"] = "0"
            write_csv(port_path, PORT_HEADER, [bad])
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(code, 1)
            self.assertEqual(stderr.getvalue().strip(), "malformed r3")
            self.assertNotIn("424242", stderr.getvalue())

    def test_audit_finishes_before_port_is_opened(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            writer_path = root / "writer.csv"
            port_path = root / "not-a-file"
            port_path.mkdir()
            write_csv(
                writer_path,
                audit.HEADER,
                [valid_writer_row(1, 2, 0, 4, "port")],
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(writer_path), str(port_path)])
            self.assertEqual(code, 1)
            self.assertIn("invalid trace input", stderr.getvalue())
            self.assertNotIn("malformed port header", stderr.getvalue())

    def test_oserror_unicode_and_csv_errors(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "missing.csv"
            other = root / "other.csv"
            other.write_text(",".join(PORT_HEADER) + "\n", encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(missing), str(other)])
            self.assertEqual(code, 1)
            self.assertNotEqual(stderr.getvalue(), "")

            writer_path = root / "bad.csv"
            writer_path.write_bytes(b"\xff\xfe not utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(writer_path), str(other)])
            self.assertEqual(code, 1)

            nul_path = root / "nul.csv"
            nul_path.write_bytes(b"frame\0,command_word\n")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = join.main([str(nul_path), str(other)])
            self.assertEqual(code, 1)
            self.assertNotIn("\0", stderr.getvalue())

    def test_usage(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = join.main([])
        self.assertEqual(code, 2)
        self.assertIn("usage:", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
