import csv
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = PROJECT_ROOT / 'tools'
sys.path.insert(0, str(TOOLS_DIR))
import audit_writer_trace as trace_audit

HEADER = trace_audit.HEADER
MASK32 = 0xFFFFFFFF
UINT64_MAX = 0xFFFFFFFFFFFFFFFF


def make_row(
    *,
    frame=0,
    command_word=0,
    argument_index=0,
    buffer_word=None,
    unknown_mask=MASK32,
    i960_mask=0,
    tgp_mask=0,
    port_mask=0,
    last_mask=0,
    writes=0,
):
    if buffer_word is None:
        buffer_word = (command_word + argument_index) & 0x7FFF
    return {
        'frame': str(frame),
        'command_word': str(command_word),
        'argument_index': str(argument_index),
        'buffer_word': str(buffer_word),
        'unknown_mask': str(unknown_mask),
        'i960_mask': str(i960_mask),
        'tgp_mask': str(tgp_mask),
        'port_mask': str(port_mask),
        'last_mask': str(last_mask),
        'writes': str(writes),
    }


def make_group(frame, command_word, length, **overrides):
    rows = []
    for argument_index in range(length):
        rows.append(
            make_row(
                frame=frame,
                command_word=command_word,
                argument_index=argument_index,
                **overrides,
            )
        )
    return rows


def rows_for_args(frame, command_word, args):
    return [
        make_row(
            frame=frame,
            command_word=command_word,
            argument_index=argument_index,
        )
        for argument_index in args
    ]


def csv_text(rows, header=HEADER):
    output = io.StringIO(newline='')
    writer = csv.DictWriter(
        output,
        fieldnames=list(header),
        lineterminator=chr(10),
    )
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue()


def run_cli(path):
    return subprocess.run(
        [
            sys.executable,
            str(PROJECT_ROOT / 'tools' / 'audit_writer_trace.py'),
            str(path),
        ],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
        timeout=30,
    )


class AuditRowsTests(unittest.TestCase):
    def test_header_is_exact(self):
        self.assertEqual(
            HEADER,
            (
                'frame',
                'command_word',
                'argument_index',
                'buffer_word',
                'unknown_mask',
                'i960_mask',
                'tgp_mask',
                'port_mask',
                'last_mask',
                'writes',
            ),
        )

    def test_valid_three_and_five_row_groups_and_reused_slot(self):
        rows = (
            make_group(10, 5, 3)
            + make_group(10, 5, 5)
            + make_group(12, 6, 3)
        )
        self.assertEqual(
            trace_audit.audit_rows(rows),
            {
                'total_rows': 11,
                'group_count': 3,
                'frame_count': 2,
                'rows_with_origin': 11,
                'mixed_origin_count': 0,
            },
        )

    def test_buffer_word_wraps_at_32767(self):
        rows = make_group(1, 32767, 3)
        self.assertEqual(
            [row['buffer_word'] for row in rows],
            ['32767', '0', '1'],
        )
        result = trace_audit.audit_rows(rows)
        self.assertEqual(result['total_rows'], 3)
        self.assertEqual(result['group_count'], 1)
        self.assertEqual(result['frame_count'], 1)

    def test_mixed_ownership_counts_each_nonzero_mask(self):
        rows = [
            make_row(
                frame=1,
                command_word=2,
                argument_index=0,
                unknown_mask=0xFFFFFFF8,
                i960_mask=0x1,
                tgp_mask=0x2,
                port_mask=0x4,
                last_mask=0x1,
                writes=1,
            ),
            make_row(frame=1, command_word=2, argument_index=1),
            make_row(
                frame=1,
                command_word=2,
                argument_index=2,
                last_mask=0x80000000,
                writes=2,
            ),
        ]
        result = trace_audit.audit_rows(rows)
        self.assertEqual(result['total_rows'], 3)
        self.assertEqual(result['group_count'], 1)
        self.assertEqual(result['frame_count'], 1)
        self.assertEqual(result['rows_with_origin'], 6)
        self.assertEqual(result['mixed_origin_count'], 1)

    def test_maximum_counters(self):
        rows = make_group(
            UINT64_MAX,
            32767,
            3,
            writes=UINT64_MAX,
            last_mask=MASK32,
        )
        self.assertEqual(
            [row['buffer_word'] for row in rows],
            ['32767', '0', '1'],
        )
        self.assertEqual(
            trace_audit.audit_rows(rows),
            {
                'total_rows': 3,
                'group_count': 1,
                'frame_count': 1,
                'rows_with_origin': 3,
                'mixed_origin_count': 0,
            },
        )

    def test_numeric_fields_are_strict_unsigned_decimal(self):
        base = make_row()
        cases = (
            ('negative command word', 'command_word', '-1'),
            ('signed argument index', 'argument_index', '+1'),
            ('decimal argument index', 'argument_index', '1.0'),
            ('exponent frame', 'frame', '1e3'),
            ('hex buffer word', 'buffer_word', '0x1'),
            ('empty frame', 'frame', ''),
            ('command word overflow', 'command_word', '32768'),
            ('frame overflow', 'frame', str(UINT64_MAX + 1)),
            ('writes overflow', 'writes', str(UINT64_MAX + 1)),
            ('mask overflow', 'i960_mask', str(1 << 32)),
            ('negative writes', 'writes', '-0'),
        )
        for label, field, value in cases:
            bad = dict(base)
            bad[field] = value
            with self.subTest(case=label):
                with self.assertRaises(ValueError):
                    trace_audit.audit_rows([bad])

    def test_partition_must_be_disjoint_and_complete(self):
        cases = (
            make_row(unknown_mask=MASK32, i960_mask=1),
            make_row(unknown_mask=MASK32 - 1),
        )
        for row in cases:
            with self.subTest(row=row):
                with self.assertRaisesRegex(ValueError, 'ownership'):
                    trace_audit.audit_rows([row])

    def test_buffer_word_must_match_offset_sum(self):
        row = make_row(command_word=9, buffer_word=8)
        with self.assertRaisesRegex(ValueError, 'buffer_word'):
            trace_audit.audit_rows([row])

    def test_groups_require_exact_contiguous_sequences(self):
        bad_sequences = (
            (),
            (0, 0),
            (1,),
            (0, 2),
            (0, 1),
            (0, 1, 3),
            (0, 1, 2, 4),
            (0, 1, 2, 3),
            (0, 1, 2, 3, 3),
            (0, 1, 2, 4, 3),
            (0, 1, 2, 3, 4, 1),
            (0, 1, 2, 3, 4, 0),
        )
        for sequence in bad_sequences:
            with self.subTest(sequence=sequence):
                with self.assertRaises(ValueError):
                    trace_audit.audit_rows(
                        rows_for_args(1, 4, sequence)
                    )

    def test_groups_require_same_frame_and_command(self):
        cases = (
            [
                make_row(frame=1, command_word=1, argument_index=0),
                make_row(frame=1, command_word=2, argument_index=1),
            ],
            [
                make_row(frame=1, command_word=1, argument_index=0),
                make_row(frame=2, command_word=1, argument_index=1),
            ],
        )
        for rows in cases:
            with self.subTest(rows=rows):
                with self.assertRaisesRegex(ValueError, 'group'):
                    trace_audit.audit_rows(rows)

    def test_frames_must_be_nondecreasing(self):
        rows = make_group(10, 1, 3) + make_group(9, 2, 3)
        with self.assertRaisesRegex(ValueError, 'frame'):
            trace_audit.audit_rows(rows)

    def test_fresh_and_rewritten_to_unknown_states(self):
        fresh = make_group(1, 0, 3)
        rewritten = make_group(
            1,
            0,
            3,
            writes=9,
            last_mask=0x1,
        )
        for label, rows in (
            ('fresh', fresh),
            ('rewritten', rewritten),
        ):
            with self.subTest(state=label):
                result = trace_audit.audit_rows(rows)
                self.assertEqual(result['total_rows'], 3)
                self.assertEqual(result['group_count'], 1)
                self.assertEqual(result['rows_with_origin'], 3)
                self.assertEqual(result['mixed_origin_count'], 0)

    def test_writes_consistency_rejections(self):
        cases = (
            make_row(writes=0, last_mask=1),
            make_row(
                writes=0,
                unknown_mask=MASK32 - 1,
                i960_mask=1,
            ),
            make_row(writes=3, last_mask=0),
            make_row(
                writes=3,
                unknown_mask=0,
                i960_mask=MASK32,
                last_mask=0,
            ),
        )
        for row in cases:
            with self.subTest(row=row):
                with self.assertRaisesRegex(ValueError, 'writes'):
                    trace_audit.audit_rows([row])


class CliTests(unittest.TestCase):
    def test_cli_prints_json_for_valid_input(self):
        rows = make_group(1, 0, 3) + make_group(2, 7, 5)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace.csv'
            path.write_text(csv_text(rows), encoding='utf-8')
            completed = run_cli(path)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, '')
        self.assertEqual(
            json.loads(completed.stdout),
            trace_audit.audit_rows(rows),
        )

    def test_cli_rejects_malformed_headers(self):
        bad_headers = (
            list(HEADER[:-1]),
            list(HEADER) + ['extra'],
            list(reversed(HEADER)),
            list(HEADER[:-1]) + ['frame'],
            [],
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace.csv'
            for header in bad_headers:
                text = (
                    ''
                    if not header
                    else ','.join(header) + chr(10)
                )
                path.write_text(text, encoding='utf-8')
                completed = run_cli(path)
                with self.subTest(header=header):
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(completed.stdout, '')
                    lowered = completed.stderr.lower()
                    self.assertIn('error:', lowered)
                    self.assertIn('header', lowered)
                    self.assertEqual(
                        len(completed.stderr.strip().splitlines()),
                        1,
                    )

    def test_cli_rejects_invalid_row(self):
        rows = make_group(1, 0, 3)
        rows[1]['buffer_word'] = '999'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace.csv'
            path.write_text(csv_text(rows), encoding='utf-8')
            completed = run_cli(path)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, '')
        lowered = completed.stderr.lower()
        self.assertIn('error:', lowered)
        self.assertIn('buffer_word', lowered)
        self.assertEqual(
            len(completed.stderr.strip().splitlines()),
            1,
        )


if __name__ == '__main__':
    unittest.main()
