"""Join consumed buffer-word versions to completed port writes.

Eligibility is port_mask == 4294967295 and writes > 0. A writer row with
(buffer_word, writes) samples the version left by that write. A later write
may overwrite the word without invalidating the earlier observation. The
same version can be read repeatedly, across frames. This join does not
establish object identity, freshness, or which command caused the write.
Both streams must come from one board lifetime. The schema has no board
identifier with which to verify this precondition.

Frame and object identity are not keys. Port sequence must be the contiguous
encounter index 0, 1, 2, ... in stream order. Rows are never sorted, and a
later sequence value cannot repair an earlier gap. Word order may differ from
sequence order.

join_rows accepts any iterable and does not copy the streams into row lists.
It still checks the writer fields it consumes. Port rows must carry the full
45-column schema. Unknown CPU state is only cpu_state_present == 0, and the
absent registers, instruction pointer, and observed_count must be zero.
Those fields are never inferred.
"""

from __future__ import annotations

import csv
import sys
from collections import Counter

import audit_writer_trace

UINT64_MAX = (1 << 64) - 1
UINT32_MAX = (1 << 32) - 1
WORD_MAX = 32767
TARGET_PORT_MASK = 4294967295

PORT_COLUMNS = (
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

_PORT_FIELDS = frozenset(PORT_COLUMNS)
_UINT64_FIELDS = (
    "frame",
    "observed_count",
    "buffer_write_count",
)
_UINT32_FIELDS = (
    "address",
    "buffer_cursor",
    "data_masked",
    "lane_mask",
    "observed_ip",
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
)
_BOOL_FIELDS = (
    "cpu_state_present",
    "buffer_provenance_available",
    "buffer_write_observed",
)
_ABSENT_CPU_FIELDS = ("observed_ip", "observed_count") + tuple(
    f"r{index}" for index in range(32)
)

_SUMMARY_KEYS = (
    "eligible_writer_rows",
    "ineligible_writer_rows",
    "distinct_target_keys",
    "matched_writer_rows",
    "ambiguous_writer_rows",
    "unmatched_writer_rows",
    "port_rows",
)


class MalformedTrace(ValueError):
    """Safe fixed-label diagnostic, never contains input cell values."""


def _malformed(label):
    raise MalformedTrace("malformed " + label)


def parse_uint(value, limit, label):
    if (
        not isinstance(value, str)
        or not value
        or not value.isascii()
        or not value.isdecimal()
    ):
        _malformed(label)
    number = int(value, 10)
    if number > limit:
        _malformed(label)
    return number


def parse_bit(value, label):
    if value not in ("0", "1"):
        _malformed(label)
    return value == "1"


def _writer_fields(row):
    if not isinstance(row, dict):
        _malformed("writer row")
    try:
        mask_raw = row["port_mask"]
        writes_raw = row["writes"]
        word_raw = row["buffer_word"]
    except KeyError as exc:
        _malformed(exc.args[0])
    mask = parse_uint(mask_raw, UINT32_MAX, "port_mask")
    writes = parse_uint(writes_raw, UINT64_MAX, "writes")
    word = parse_uint(word_raw, WORD_MAX, "buffer_word")
    return mask, writes, word


def _port_event(row, index):
    if not isinstance(row, dict) or set(row) != _PORT_FIELDS:
        _malformed("port row")
    for name in PORT_COLUMNS:
        if row[name] is None:
            _malformed(name)

    sequence = parse_uint(row["sequence"], UINT64_MAX, "sequence")
    if sequence != index:
        _malformed("sequence")

    parsed = {}
    for name in _UINT64_FIELDS:
        parsed[name] = parse_uint(row[name], UINT64_MAX, name)
    for name in _UINT32_FIELDS:
        parsed[name] = parse_uint(row[name], UINT32_MAX, name)
    for name in _BOOL_FIELDS:
        parsed[name] = parse_bit(row[name], name)
    if parsed["data_masked"] & ~parsed["lane_mask"]:
        _malformed("data_masked")
    word = parse_uint(row["buffer_word_written"], WORD_MAX, "buffer_word_written")
    count = parsed["buffer_write_count"]

    if not parsed["cpu_state_present"]:
        for name in _ABSENT_CPU_FIELDS:
            if parsed[name] != 0:
                _malformed(name)

    if not parsed["buffer_write_observed"]:
        if word != 0 or count != 0:
            _malformed("observed write")
        return None
    if not parsed["buffer_provenance_available"] or count == 0:
        _malformed("observed write")
    return word, count


def join_rows(writer_rows, port_rows, collect_matches=True):
    """Return summary counts and the unique matched event-key map.

    Writer rows are prevalidated by audit_writer_trace.audit_rows when they
    come from the CLI. A target key with more than one observed port row is
    ambiguous and is omitted from matches. Port state retained for a target
    key is only the first sequence and a count capped at 2.

    collect_matches=False still counts matches and returns an empty map.
    """
    try:
        writer_iter = iter(writer_rows)
        port_iter = iter(port_rows)
    except TypeError:
        _malformed("rows")

    eligible = Counter()
    ineligible = 0
    for row in writer_iter:
        mask, writes, word = _writer_fields(row)
        if mask == TARGET_PORT_MASK and writes > 0:
            eligible[(word, writes)] += 1
        else:
            ineligible += 1

    tracked = {}
    port_count = 0
    for row in port_iter:
        event = _port_event(row, port_count)
        port_count += 1
        if event is None or event not in eligible:
            continue
        bucket = tracked.get(event)
        if bucket is None:
            tracked[event] = [port_count - 1, 1]
        elif bucket[1] < 2:
            bucket[1] = 2

    matched = ambiguous = unmatched = 0
    matches = {}
    for key, copies in eligible.items():
        found = tracked.get(key)
        if found is None:
            unmatched += copies
        elif found[1] == 1:
            matched += copies
            if collect_matches:
                matches[f"{key[0]}:{key[1]}"] = found[0]
        else:
            ambiguous += copies

    return {
        "summary": {
            "eligible_writer_rows": sum(eligible.values()),
            "ineligible_writer_rows": ineligible,
            "distinct_target_keys": len(eligible),
            "matched_writer_rows": matched,
            "ambiguous_writer_rows": ambiguous,
            "unmatched_writer_rows": unmatched,
            "port_rows": port_count,
        },
        "matches": matches,
    }


def _require_header(reader, expected, label):
    names = reader.fieldnames
    if names is None or tuple(names) != tuple(expected):
        _malformed(label)


def _summary_line(summary):
    body = " ".join(f"{key}={summary[key]}" for key in _SUMMARY_KEYS)
    return f"SUMMARY {body}"


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2:
        print(
            "usage: join_writer_observations.py writer.csv port.csv",
            file=sys.stderr,
        )
        return 2
    writer_path, port_path = args
    try:
        with open(writer_path, encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            _require_header(reader, audit_writer_trace.HEADER, "writer header")
            audit_writer_trace.audit_rows(reader)
        with open(writer_path, encoding="utf-8", newline="") as writer_handle, open(
            port_path, encoding="utf-8", newline=""
        ) as port_handle:
            writer_reader = csv.DictReader(writer_handle)
            port_reader = csv.DictReader(port_handle)
            _require_header(writer_reader, audit_writer_trace.HEADER, "writer header")
            _require_header(port_reader, PORT_COLUMNS, "port header")
            joined = join_rows(writer_reader, port_reader, collect_matches=False)
    except MalformedTrace as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except (OSError, UnicodeError, csv.Error, ValueError) as exc:
        # Auditor/OS/codec messages can contain private cells or paths.
        print("invalid trace input (" + type(exc).__name__ + ")", file=sys.stderr)
        return 1
    print(_summary_line(joined["summary"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
