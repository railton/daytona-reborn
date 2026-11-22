import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import convert_effective_input_m2in as m

CSV_HEAD = "tick,steer,accel,brake,in0,in1,in2\n"


def s16(text):
    raw = text.encode()
    return struct.pack("<H", len(raw)) + raw


def header(game="daytona", fields=None, version=1, magic=b"M2IN"):
    fields = LAYOUT if fields is None else fields
    out = magic + struct.pack("<I", version) + s16(game) + struct.pack("<H", len(fields))
    for port, mask, defv, analog in fields:
        out += s16(port) + struct.pack("<IIB", mask, defv, analog)
    return out


# Sorted like the m2trace recorder. IN0 0x02 and IN1 0x0E/0x80 are uncovered (no field).
LAYOUT = [
    (":ACCEL", 0xFF, 0x20, 1),
    (":BRAKE", 0xFF, 0x20, 1),
    (":GEARS", 0x01, 0, 0), (":GEARS", 0x02, 0, 0), (":GEARS", 0x04, 0, 0),
    (":GEARS", 0x08, 0, 0), (":GEARS", 0x10, 0, 0),
    (":IN0", 0x01, 0x01, 0), (":IN0", 0x04, 0x04, 0), (":IN0", 0x08, 0x08, 0), (":IN0", 0x10, 0x10, 0),
    (":IN0", 0x20, 0x20, 0), (":IN0", 0x40, 0x40, 0), (":IN0", 0x80, 0x80, 0),
    (":IN1", 0x01, 0x01, 0),
    (":STEER", 0xFF, 0x80, 1),
]
NFIELDS = len(LAYOUT)
IDLE = (0x80, 0x20, 0x20, 0xFF, 0x8F, 0xFF)


def row(**kw):
    r = list(IDLE)
    for k, v in kw.items():
        r["steer accel brake in0 in1 in2".split().index(k)] = v
    return tuple(r)


def in1_with_gear(bits, rest=0x8F):
    return (rest & ~0x70) | (bits << 4)


def frame(*values):
    return struct.pack("<%dI" % len(values), *values)


def convert_rows(rows, hdr=None):
    blob = header() if hdr is None else hdr
    head, game, fields = m.read_header(blob)
    plan = m.validate_layout(game, fields)
    return m.build_stream(head, fields, plan, rows), head


def vals(data, head):
    return struct.unpack_from("<%dI" % NFIELDS, data, len(head))


class Packing(unittest.TestCase):
    def test_exact_packing_start_gear1(self):
        r = (0x80, 0x20, 0xE0, 0xEF, in1_with_gear(2), 0xFF)
        data, head = convert_rows([r])
        expect = frame(0x20, 0xE0,
                       0, 2, 0, 0, 0,
                       0x01, 0x04, 0x08, 0x00, 0x20, 0x40, 0x80,
                       0x01,
                       0x80)
        self.assertEqual(data, head + expect)

    def test_header_preserved_and_frames(self):
        data, head = convert_rows([IDLE] * 3)
        self.assertEqual(head, header())
        self.assertEqual(len(data), len(head) + 3 * 4 * NFIELDS)

    def test_recording_with_frames_uses_header_only(self):
        rec = header() + frame(*[0] * NFIELDS) * 2
        data, head = convert_rows([IDLE], hdr=rec)
        self.assertEqual(head, header())
        self.assertEqual(len(data), len(head) + 4 * NFIELDS)

    def test_analog_range_edges(self):
        for v in (0x20, 0xE0):
            data, head = convert_rows([row(steer=v, accel=v, brake=v)])
            self.assertEqual((vals(data, head)[15], vals(data, head)[0], vals(data, head)[1]), (v, v, v))
        for col in ("steer", "accel", "brake"):
            for v in (0x1F, 0xE1, 0, 255):
                with self.assertRaises(m.M2InError):
                    convert_rows([row(**{col: v})])

    def test_buttons_active_low(self):
        fields = m.read_header(header())[2]
        for bit in (0x01, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80):
            data, head = convert_rows([row(in0=0xFF & ~bit)])
            for f, v in zip(fields, vals(data, head)):
                if f.port == ":IN0":
                    self.assertEqual(v, 0 if f.mask == bit else f.mask)
        data, head = convert_rows([row(in1=in1_with_gear(0, 0x8E))])
        self.assertEqual(vals(data, head)[14], 0)


class Gears(unittest.TestCase):
    def sel(self, bits):
        data, head = convert_rows([row(in1=in1_with_gear(bits))])
        return list(vals(data, head)[2:7])

    def test_native_codes_map_to_selectors_neutral_is_0x01(self):
        for sel_mask, bits in zip(m.GEAR_MASKS, (0, 2, 1, 6, 5)):
            want = [m_ if m_ == sel_mask else 0 for m_ in m.GEAR_MASKS]
            self.assertEqual(self.sel(bits), want)
        self.assertEqual(self.sel(0), [1, 0, 0, 0, 0])  # no neutral override

    def test_unrepresentable_codes_fail(self):
        for bits in (3, 4, 7):
            with self.assertRaises(m.M2InError):
                self.sel(bits)

    def test_no_in1_gear_field(self):
        fields = m.read_header(header())[2]
        self.assertFalse([f for f in fields if f.port == ":IN1" and f.mask & 0x70])

    def test_gear_must_be_active_high_default_zero(self):
        for mask in m.GEAR_MASKS:
            layout = [f if f[:2] != (":GEARS", mask) else (":GEARS", mask, mask, 0) for f in LAYOUT]
            with self.assertRaises(m.M2InError):
                convert_rows([IDLE], hdr=header(fields=layout))


class Layouts(unittest.TestCase):
    def bad(self, **kw):
        with self.assertRaises(m.M2InError):
            convert_rows([IDLE], hdr=header(**kw))

    def repl(self, key, new):
        return [new if f[:len(key)] == key else f for f in LAYOUT]

    def test_game_version_magic(self):
        for game in ("daytona2", "Daytona", "daytonam", "", "daytona "):
            self.bad(game=game)
        self.bad(version=2)
        self.bad(magic=b"M2TR")

    def test_truncated_malformed(self):
        blob = header()
        for cut in (3, 7, 12, 30, len(blob) - 1):
            with self.assertRaises(m.M2InError):
                convert_rows([IDLE], hdr=blob[:cut])
        with self.assertRaises(m.M2InError):
            convert_rows([IDLE], hdr=blob + b"\0\0")
        self.bad(fields=[])
        self.bad(fields=self.repl((":STEER",), (":STEER", 0xFF, 0x80, 2)))

    def test_incomplete_layouts(self):
        self.bad(fields=[f for f in LAYOUT if f[:2] != (":GEARS", 0x01)])  # missing neutral selector
        self.bad(fields=[f for f in LAYOUT if f[0] != ":BRAKE"])
        self.bad(fields=[f for f in LAYOUT if f[0] != ":STEER"])
        self.bad(fields=[f for f in LAYOUT if f[0] != ":GEARS"])

    def test_relabelled_layouts(self):
        self.bad(fields=self.repl((":STEER",), (":WHEEL", 0xFF, 0x80, 1)))
        self.bad(fields=self.repl((":GEARS", 0x02), (":GEAR", 0x02, 0, 0)))
        self.bad(fields=self.repl((":GEARS", 0x20), (":GEARS", 0x20, 0, 0)) + [(":GEARS", 0x20, 0, 0)])
        self.bad(fields=LAYOUT + [(":IN2", 0x01, 1, 0)])
        self.bad(fields=self.repl((":ACCEL",), (":ACCEL", 0xFF, 0x20, 0)))  # not analog

    def test_rev_a_analog_strict(self):
        self.bad(fields=self.repl((":STEER",), (":STEER", 0x0FF0, 0x800, 1)))
        self.bad(fields=self.repl((":STEER",), (":STEER", 0xFF, 0x00, 1)))
        self.bad(fields=self.repl((":ACCEL",), (":ACCEL", 0xFF, 0x00, 1)))
        self.bad(fields=self.repl((":BRAKE",), (":BRAKE", 0x0F, 0x02, 1)))

    def test_unrepresentable_layouts(self):
        self.bad(fields=LAYOUT + [(":IN1", 0x70, 0x70, 0)])  # computed gear bits
        self.bad(fields=LAYOUT + [(":IN0", 0x03, 0x03, 0)])  # overlap
        self.bad(fields=LAYOUT + [(":STEER", 0x0F, 0, 1)])  # second analog
        self.bad(fields=LAYOUT + [LAYOUT[0]])  # duplicate
        self.bad(fields=LAYOUT + [(":IN0", 0x02, 0x01, 0)])  # default outside mask
        self.bad(fields=LAYOUT + [(":IN0", 0x100, 0x100, 0)])  # not a byte

    def test_known_button_polarity(self):
        self.bad(fields=self.repl((":IN0", 0x10), (":IN0", 0x10, 0x00, 0)))
        self.bad(fields=self.repl((":IN1", 0x01), (":IN1", 0x01, 0x00, 0)))

    def test_unused_header_field_stays_idle(self):
        for extra in ((":IN0", 0x02, 0x00, 0), (":IN0", 0x02, 0x02, 0), (":IN1", 0x80, 0x00, 0)):
            layout = LAYOUT + [extra]
            data, head = convert_rows([IDLE], hdr=header(fields=layout))
            self.assertEqual(struct.unpack_from("<I", data, len(data) - 4)[0], extra[2])
            with self.assertRaises(m.M2InError):  # a press on it is still refused
                convert_rows([row(in0=0xFD)], hdr=header(fields=layout))

    def test_press_without_field_rejected_idle_ok(self):
        layout = [f for f in LAYOUT if f[:2] != (":IN0", 0x10)]
        convert_rows([IDLE], hdr=header(fields=layout))
        with self.assertRaises(m.M2InError):
            convert_rows([row(in0=0xEF)], hdr=header(fields=layout))

    def test_multibit_unknown_field_idle(self):
        layout = [f for f in LAYOUT if f[:2] not in ((":IN0", 0x40), (":IN0", 0x80))] + [(":IN0", 0xC0, 0xC0, 0)]
        convert_rows([IDLE], hdr=header(fields=layout))
        with self.assertRaises(m.M2InError):  # press on both: buttons with no field
            convert_rows([row(in0=0xBF)], hdr=header(fields=layout))


class Rows(unittest.TestCase):
    def test_uncovered_and_in2(self):
        for r in (row(in0=0xFD), row(in1=0x8D), row(in1=0x0F), row(in2=0xFE)):
            with self.assertRaises(m.M2InError):
                convert_rows([r])

    def test_malformed_rows_empty(self):
        for r in ((0x80, 0x20, 0x20, 0xFF, 0xAF), IDLE[:5] + (256,), IDLE[:5] + (True,), IDLE[:5] + (-1,)):
            with self.assertRaises(m.M2InError):
                convert_rows([r])
        with self.assertRaises(m.M2InError):
            convert_rows([])


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.hdr = self.dir / "head.m2in"
        self.hdr.write_bytes(header())

    def write_csv(self, rows, name="eff.csv", end=True):
        text = CSV_HEAD + "".join(f"{i},{','.join(map(str, r))}\n" for i, r in enumerate(rows))
        if end:
            text += f"#end,{len(rows)}\n"
        path = self.dir / name
        path.write_text(text, encoding="ascii", newline="")
        return path

    def test_end_to_end_real_read_trace(self):
        rows = [(128, 32, 224, 0xEF, in1_with_gear(2), 255), (127, 33, 0x20, 255, in1_with_gear(5), 255)]
        out = self.dir / "out.m2in"
        self.assertEqual(m.convert(self.write_csv(rows), self.hdr, out), 2)
        self.assertEqual(out.read_bytes(), convert_rows(rows)[0])

    def test_collision_preserved(self):
        out = self.dir / "out.m2in"
        out.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            m.convert(self.write_csv([IDLE]), self.hdr, out)
        self.assertEqual(out.read_bytes(), b"keep")

    def test_failures_leave_no_output(self):
        out = self.dir / "out.m2in"
        with self.assertRaises(m.M2InError):
            m.convert(self.write_csv([row(in1=in1_with_gear(7))]), self.hdr, out)
        self.assertFalse(out.exists())
        with self.assertRaises(ValueError):
            m.convert(self.write_csv([IDLE], end=False), self.hdr, out)
        self.assertFalse(out.exists())
        wrong = self.dir / "wrong.m2in"
        wrong.write_bytes(header(game="daytona2"))
        with self.assertRaises(m.M2InError):
            m.convert(self.write_csv([IDLE]), wrong, out)
        self.assertFalse(out.exists())

    def run_cli(self, *args):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = m.main(["x", *map(str, args)])
        return code, err.getvalue()

    def test_cli_codes_and_no_path_leak(self):
        out = self.dir / "cli_secret_name.m2in"
        csv = self.write_csv([IDLE], name="csv_secret_name.csv")
        self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(self.run_cli(csv, self.hdr, out)[0], 0)
        cases = [
            self.run_cli(csv, self.hdr, out),                                # collision
            self.run_cli(self.dir / "missing_secret.csv", self.hdr, self.dir / "o2"),
            self.run_cli(csv, self.dir / "missing_hdr_secret", self.dir / "o3"),
            self.run_cli(self.write_csv([IDLE], end=False), self.hdr, self.dir / "o4"),
            self.run_cli(csv, self.hdr, self.dir / "nodir_secret" / "o5"),
        ]
        for code, text in cases:
            self.assertEqual(code, 1)
            self.assertNotIn("secret", text)
            self.assertNotIn(str(self.dir), text)
            self.assertNotIn("Users", text)


if __name__ == "__main__":
    unittest.main()
