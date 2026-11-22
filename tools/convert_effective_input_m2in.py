"""Convert an effective cabinet input CSV to a private M2IN v1 stream.

Strict Rev A ("daytona") plan; MAME 0.289 model2.cpp (daytona_gearbox_r, lines
1478-1493, 1571-1605).  The m2trace header layout is preserved byte for byte.
Validated with the Lua codec and synthetic headers. Live MAME timing and
input field compatibility remain to be verified.

Plan (the header must match it, anything else is rejected):
  :STEER/:ACCEL/:BRAKE  one analog field each, mask 0xFF, defaults 0x80/0x20/0x20,
                        accepted values 0x20..0xE0.
  :GEARS 0x01,02,04,08,10  active-high selectors (default 0), neutral=0x01,
                        gear1..4 = 0x02..0x10.  Native IN1 bits 4..6 are
                        0,2,1,6,5 -> 0x01,0x02,0x04,0x08,0x10; 3/4/7 fail.
                        Exactly one selector is raised per tick.
  :IN0 / :IN1           known buttons (IN0 0x01,04,08,10,20,40,80 and IN1 0x01)
                        are active-low single-bit fields (default=mask).  Any
                        other field present in the header is written at its
                        default (idle); the CSV must be idle on every bit that
                        is not a known button, and on in2.  A pressed button
                        without a field in the header is unrepresentable.
  :IN1 0x70 may never be a field (computed gear bits).
"""

import os
import struct
import sys
from collections import namedtuple
from pathlib import Path

GAME = "daytona"
GEAR_PORT = ":GEARS"
GEAR_MASKS = (0x01, 0x02, 0x04, 0x08, 0x10)  # neutral, gear1..gear4
GEAR_VALUES = (0, 2, 1, 6, 5)  # native IN1 bits 4..6
GEAR_SHIFT, GEAR_FIELD = 4, 0x70
ANALOG = {":STEER": (0, 0x80), ":ACCEL": (1, 0x20), ":BRAKE": (2, 0x20)}  # port -> (column, default)
ANALOG_MIN, ANALOG_MAX = 0x20, 0xE0
DIGITAL_PORTS = {":IN0": 3, ":IN1": 4}
BUTTONS = {(":IN0", m) for m in (0x01, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80)} | {(":IN1", 0x01)}

Field = namedtuple("Field", "port mask defvalue analog")


class M2InError(ValueError):
    pass


def default_read_trace():
    from convert_player_input_trace import read_trace  # ordinary sibling import
    return read_trace


def _take(blob, pos, fmt):
    size = struct.calcsize(fmt)
    if pos + size > len(blob):
        raise M2InError("truncated M2IN header")
    return struct.unpack_from(fmt, blob, pos), pos + size


def _take_str(blob, pos):
    (n,), pos = _take(blob, pos, "<H")
    if pos + n > len(blob):
        raise M2InError("truncated M2IN header")
    try:
        text = blob[pos:pos + n].decode("utf-8")
    except UnicodeDecodeError:
        raise M2InError("malformed string in M2IN header") from None
    return text, pos + n


def read_header(blob):
    """Parse an M2IN v1 header. Returns (header_bytes, game, fields)."""
    if blob[:4] != b"M2IN":
        raise M2InError("not an M2IN stream")
    (version,), pos = _take(blob, 4, "<I")
    if version != 1:
        raise M2InError("unsupported M2IN version")
    game, pos = _take_str(blob, pos)
    (nfields,), pos = _take(blob, pos, "<H")
    if nfields == 0:
        raise M2InError("M2IN header has no fields")
    fields = []
    for _ in range(nfields):
        port, pos = _take_str(blob, pos)
        (mask, defv, analog), pos = _take(blob, pos, "<IIB")
        if analog > 1:
            raise M2InError("invalid analog flag")
        fields.append(Field(port, mask, defv, bool(analog)))
    if (len(blob) - pos) % (4 * nfields):
        raise M2InError("M2IN body is not whole frames")
    return blob[:pos], game, fields


def validate_layout(game, fields):
    """Strict Rev A layout check. Returns a plan for the row mapper."""
    if game != GAME:
        raise M2InError("game must be exactly 'daytona'")
    seen = set()
    analog, digital, gears = {}, {":IN0": {}, ":IN1": {}}, {}
    for f in fields:
        key = (f.port, f.mask)
        if key in seen:
            raise M2InError("duplicate field")
        seen.add(key)
        if f.mask == 0 or f.mask > 0xFFFFFFFF or f.defvalue & ~f.mask:
            raise M2InError("invalid field mask/default")
        if f.port in ANALOG:
            if not f.analog or f.port in analog:
                raise M2InError("analog port needs exactly one analog field")
            if f.mask != 0xFF or f.defvalue != ANALOG[f.port][1]:
                raise M2InError("analog field is not the Rev A mask/default")
            analog[f.port] = f
        elif f.port in DIGITAL_PORTS:
            if f.analog or f.mask > 0xFF:
                raise M2InError("IN0/IN1 fields must be digital bytes")
            if f.port == ":IN1" and f.mask & GEAR_FIELD:
                raise M2InError("computed IN1 gear bits must not be a field")
            if (f.port, f.mask) in BUTTONS and f.defvalue != f.mask:
                raise M2InError("button must be active-low")
            digital[f.port][f.mask] = f
        elif f.port == GEAR_PORT:
            if f.analog or f.mask not in GEAR_MASKS or f.defvalue != 0:
                raise M2InError("gear selector must be active-high with default 0")
            gears[f.mask] = f
        else:
            raise M2InError("unknown port in header")
    if set(analog) != set(ANALOG):
        raise M2InError("missing analog field")
    if set(gears) != set(GEAR_MASKS):
        raise M2InError("missing gear selector field")
    for port, flds in digital.items():
        acc = 0
        for mask in flds:
            if acc & mask:
                raise M2InError("overlapping fields on " + port)
            acc |= mask
    return {"digital": digital}


def map_row(row, fields, plan):
    """One CSV row (steer, accel, brake, in0, in1, in2) -> field-ordered values."""
    if len(row) != 6 or any(type(v) is not int or not 0 <= v <= 255 for v in row):
        raise M2InError("malformed effective row")
    for port, (col, _) in ANALOG.items():
        if not ANALOG_MIN <= row[col] <= ANALOG_MAX:
            raise M2InError("analog value outside 0x20..0xE0")
    in0, in1, in2 = row[3], row[4], row[5]
    if in2 != 0xFF:
        raise M2InError("unmappable nondefault in2")
    buttons0 = sum(m for p, m in BUTTONS if p == ":IN0")
    buttons1 = sum(m for p, m in BUTTONS if p == ":IN1")
    if in0 & ~buttons0 & 0xFF != ~buttons0 & 0xFF:
        raise M2InError("unmappable nondefault in0 bits")
    if in1 & ~buttons1 & ~GEAR_FIELD & 0xFF != ~buttons1 & ~GEAR_FIELD & 0xFF:
        raise M2InError("unmappable nondefault in1 bits")
    gear_bits = (in1 & GEAR_FIELD) >> GEAR_SHIFT
    if gear_bits not in GEAR_VALUES:
        raise M2InError("unknown or nonrepresentable gear bits")
    selected = GEAR_MASKS[GEAR_VALUES.index(gear_bits)]
    byte = {":IN0": in0, ":IN1": in1}
    out = []
    for f in fields:
        if f.port in ANALOG:
            out.append(row[ANALOG[f.port][0]])
        elif f.port == GEAR_PORT:
            out.append(f.mask if f.mask == selected else 0)
        elif (f.port, f.mask) in BUTTONS:
            out.append(f.mask if byte[f.port] & f.mask else 0)
        else:
            out.append(f.defvalue)  # unused field: idle
    for port, mask in BUTTONS:  # pressed button without a field
        if not byte[port] & mask and mask not in plan["digital"][port]:
            raise M2InError("pressed button has no field in header")
    return out


def build_stream(header, fields, plan, rows):
    if not rows:
        raise M2InError("no frames to convert")
    pack = struct.Struct("<%dI" % len(fields)).pack
    return header + b"".join(pack(*map_row(r, fields, plan)) for r in rows)


def convert(trace_path, header_path, out_path, read_trace=None):
    """Convert CSV + m2trace header/recording into a NEW M2IN file (exclusive)."""
    trace_path, header_path, out_path = Path(trace_path), Path(header_path), Path(out_path)
    read_trace = read_trace or default_read_trace()
    rows = read_trace(trace_path)
    header, game, fields = read_header(header_path.read_bytes())
    plan = validate_layout(game, fields)
    data = build_stream(header, fields, plan, rows)
    created = None
    try:
        with out_path.open("xb") as out:
            created = os.fstat(out.fileno())
            out.write(data)
    except OSError:
        if created is not None:
            try:
                if os.path.samestat(created, out_path.lstat()):
                    out_path.unlink()
            except OSError:
                pass
        raise
    return len(rows)


def main(argv):
    args = list(argv[1:])
    if len(args) != 3:
        print("usage: convert_effective_input_m2in.py EFFECTIVE.csv HEADER.m2in OUT.m2in", file=sys.stderr)
        return 2
    try:
        convert(*args)
    except M2InError as error:
        print(f"cannot convert effective input: {error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, ImportError) as error:
        # never echo paths: OSError/ValueError text may carry them
        print(f"cannot convert effective input ({type(error).__name__})", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
