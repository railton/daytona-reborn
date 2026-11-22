from itertools import groupby
from pathlib import Path
import os
import sys

FIELDS = ("steer", "accel", "brake", "in0", "in1", "in2")
HEADER = "tick," + ",".join(FIELDS) + "\n"


def parse_decimal(value, maximum):
    if not value.isdecimal():
        raise ValueError("invalid decimal")
    number = int(value)
    if number > maximum:
        raise ValueError("value outside trace range")
    return number


def read_trace(source):
    with source.open(encoding="ascii", newline="") as file:
        if file.readline() != HEADER:
            raise ValueError("invalid player input header")
        rows = []
        for line in file:
            if not line.endswith("\n"):
                raise ValueError("truncated line")
            values = line[:-1].split(",")
            if values[0] == "#end":
                if len(values) != 2 or parse_decimal(values[1], 2**64 - 1) != len(rows) or file.read(1):
                    raise ValueError("invalid end marker")
                return rows
            if len(values) != 7 or parse_decimal(values[0], 2**64 - 1) != len(rows):
                raise ValueError("invalid tick or sample")
            rows.append(tuple(parse_decimal(value, 255) for value in values[1:]))
    raise ValueError("missing end marker")


def script_for(rows):
    yield f"frames {len(rows)}\n"
    for index, field in enumerate(FIELDS):
        cursor = 0
        for value, run in groupby(row[index] for row in rows):
            next_tick = cursor + sum(1 for _ in run)
            tick_range = str(cursor) if next_tick == cursor + 1 else f"{cursor}-{next_tick-1}"
            yield f"{tick_range} {field}={value}\n"
            cursor = next_tick


def convert(source, dest):
    rows = read_trace(source)
    created = None
    try:
        with dest.open("x", encoding="ascii", newline="\n") as out:
            created = os.fstat(out.fileno())
            out.writelines(script_for(rows))
    except OSError:
        if created is not None:
            try:
                if os.path.samestat(created, dest.lstat()):
                    dest.unlink()
            except OSError:
                pass  # Keep the original output error.
        raise


def main(argv):
    if len(argv) != 3:
        print("usage: convert_player_input_trace.py TRACE.csv SCRIPT.txt", file=sys.stderr)
        return 2
    try:
        convert(Path(argv[1]), Path(argv[2]))
    except (OSError, ValueError) as error:
        print(f"cannot convert player input trace ({type(error).__name__})", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
