#!/usr/bin/env python3
"""Build the private replay metadata description (local only, opt-in).

Produces the JSON object expected by validate_player_replay_metadata.py:
course, transmission, frames, script_sha256, rom_set_sha256, nvram_sha256,
build_sha256. Inputs are explicit files; the output holds only digests and
the explicit course, gearbox, frame count and optional race mode. It never includes raw data or paths.

Digests:
  script_sha256   sha256 of the converted script bytes.
  build_sha256    sha256 of the headless executable bytes.
  rom_set_sha256  sha256 over the ROM images sorted by logical name (UTF-8
                  byte order); per image: u32be len(name), name, u64be size,
                  raw sha256 of the content. Prefixed by b'daytona-romset-v1\0'.
  nvram_sha256    same framing over the fixed parts 'backup_ram' and 'eeprom'
                  (both required), prefix b'daytona-nvram-v1\0'.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

_NAME = re.compile(r'\A[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z')
ROM_PREFIX = b'daytona-romset-v1\0'
NVRAM_PREFIX = b'daytona-nvram-v1\0'
_FRAMES_LINE = re.compile(rb'\Aframes[ \t]+([0-9]{1,12})[ \t]*\Z')


class MetaError(Exception):
    """Error naming only a field/argument, never a value or a path."""


def file_digest(path, field):
    h = hashlib.sha256()
    size = 0
    try:
        with open(path, 'rb') as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b''):
                h.update(chunk)
                size += len(chunk)
    except OSError:
        raise MetaError('unreadable:' + field)
    return h.digest(), size


def script_digest(path, frames):
    """One read: sha256 of the script bytes and check of its 'frames N' line.

    Like the player's loader, a line starting with 'frames' (outside '#'
    lines) is the length line; exactly one is required and it must equal
    --frames. Errors name only the field.
    """
    try:
        with open(path, 'rb') as fh:
            data = fh.read()
    except OSError:
        raise MetaError('unreadable:script_sha256')
    declared = []
    for line in data.split(b'\n'):
        line = line.rstrip(b'\r')
        if line.startswith(b'frames'):
            m = _FRAMES_LINE.match(line)
            if not m:
                raise MetaError('invalid:script_frames')
            declared.append(int(m.group(1)))
    if not declared:
        raise MetaError('missing:script_frames')
    if len(declared) != 1:
        raise MetaError('invalid:script_frames')
    if declared[0] != frames:
        raise MetaError('mismatch:frames')
    return hashlib.sha256(data).digest()


def write_exclusive(path, text):
    """Create path (never replace an existing file) and write text.

    A file this call created and could not finish is removed; a pre-existing
    file, including one that is also an input, is never touched.
    """
    try:
        fh = open(path, 'x', encoding='utf-8', newline='\n')
    except FileExistsError:
        raise MetaError('exists:out')
    except OSError:
        raise MetaError('unwritable:out')
    try:
        with fh:
            fh.write(text)
    except OSError:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise MetaError('unwritable:out')


def framed_digest(prefix, items):
    """items: list of (name, raw_digest, size); sorted by name bytes."""
    h = hashlib.sha256(prefix)
    for name, raw, size in sorted(items, key=lambda i: i[0].encode('utf-8')):
        nb = name.encode('utf-8')
        h.update(struct.pack('>I', len(nb)) + nb + struct.pack('>Q', size) + raw)
    return h.hexdigest()


def parse_named(specs, field):
    """specs: list of 'NAME=PATH'. Returns list of (name, path)."""
    out, seen = [], set()
    for s in specs:
        name, sep, path = s.partition('=')
        if not sep or not path or not _NAME.match(name):
            raise MetaError('invalid_name:' + field)
        if name in seen:
            raise MetaError('duplicate_name:' + field)
        seen.add(name)
        out.append((name, path))
    if not out:
        raise MetaError('empty:' + field)
    return out


def build(course, transmission, frames, script, exe, roms, eeprom, backup_ram, *, race_mode=None):
    if type(course) is not int or not 1 <= course <= 3:
        raise MetaError('invalid:course')
    if transmission not in ('automatic', 'manual'):
        raise MetaError('invalid:transmission')
    if race_mode is not None and race_mode not in ('normal', 'time-attack'):
        raise MetaError('invalid:race_mode')
    if type(frames) is not int or frames <= 0:
        raise MetaError('invalid:frames')
    script_raw = script_digest(script, frames)
    named = parse_named(roms, 'rom')
    items = []
    for name, path in named:
        raw, size = file_digest(path, 'rom_set_sha256')
        items.append((name, raw, size))
    nv = []
    for name, path in (('backup_ram', backup_ram), ('eeprom', eeprom)):
        raw, size = file_digest(path, 'nvram_sha256')
        nv.append((name, raw, size))
    metadata = {
        'course': course,
        'transmission': transmission,
        'frames': frames,
        'script_sha256': script_raw.hex(),
        'rom_set_sha256': framed_digest(ROM_PREFIX, items),
        'nvram_sha256': framed_digest(NVRAM_PREFIX, nv),
        'build_sha256': file_digest(exe, 'build_sha256')[0].hex(),
    }
    if race_mode is not None:
        metadata['race_mode'] = race_mode
    return metadata


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--course', required=True, type=int, help='1..3')
    p.add_argument('--transmission', required=True, choices=('automatic', 'manual'))
    p.add_argument('--race-mode', choices=('normal', 'time-attack'))
    p.add_argument('--frames', required=True, type=int)
    p.add_argument('--script', required=True, help='converted script file')
    p.add_argument('--exe', required=True, help='headless executable')
    p.add_argument('--rom', action='append', default=[], metavar='NAME=PATH',
                   help='explicit ROM image, repeatable; NAME is a logical label')
    p.add_argument('--eeprom', required=True, help='initial EEPROM file')
    p.add_argument('--backup-ram', required=True, help='initial backup RAM file')
    p.add_argument('--out', required=True, help='output JSON file')
    args = p.parse_args(argv)
    try:
        meta = build(args.course, args.transmission, args.frames, args.script,
                     args.exe, args.rom, args.eeprom, args.backup_ram, race_mode=args.race_mode)
        write_exclusive(args.out, json.dumps(meta, sort_keys=True) + '\n')
    except MetaError as e:
        print(json.dumps({'ok': False, 'error': str(e)}, sort_keys=True))
        return 1
    except OSError:
        print(json.dumps({'ok': False, 'error': 'unwritable:out'}, sort_keys=True))
        return 1
    print(json.dumps({'ok': True, 'fields': sorted(meta)}, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
