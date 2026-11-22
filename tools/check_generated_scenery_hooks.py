"""Check counts of selected Daytona hooks in generated chunk_*.cpp files.

This checks the generator's standalone hook statements, not hook addresses
or invocation order. Manifest duplicates and extra fields follow load_hooks.
Malformed addresses, indented comments and UTF-8 BOMs are rejected.
"""

from collections import Counter
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys


PROFILES = {
    "revision-a": ("daytona_hooks.txt", {"draw_list": 1, "scenery_budget_check": 2, "scenery_cost": 2}),
    "daytona93": ("daytona93_hooks.txt", {"draw_list": 1}),
}
ADDRESS = re.compile(r"(?:0[xX])?[0-9a-fA-F]+")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
CALL = re.compile(r"\s*rt::hook_([A-Za-z_][A-Za-z_0-9]*)\(c\);\s*")
DIRECTIVE = re.compile(r"\s*#\s*([A-Za-z_][A-Za-z_0-9]*)\b(.*)")
COMMENT_OR_LITERAL = re.compile(r"""//|/\*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""")


class UnresolvedHook(Exception):
    def __init__(self, hook, line):
        self.hook = hook
        self.line = line


@dataclass
class Conditional:
    active: bool | None = False
    covered: bool | None = False
    saw_else: bool = False

    def branch(self, condition):
        if self.covered is True or condition is False:
            self.active = False
        elif self.covered is False and condition is True:
            self.active = True
        else:
            self.active = None

        if condition is True:
            self.covered = True
        elif condition is None and self.covered is False:
            self.covered = None


def manifest_counts(text):
    by_address = {}
    for line_number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 2 or not ADDRESS.fullmatch(fields[0]) or not IDENTIFIER.fullmatch(fields[1]):
            raise ValueError(f"line {line_number}: expected ADDRESS NAME")
        address = int(fields[0], 16)
        if address > 0xFFFFFFFF:
            raise ValueError(f"line {line_number}: address exceeds uint32")
        # m2recomp takes the first two fields and the last definition per address.
        by_address[address] = fields[1]
    return Counter(by_address.values())


def strip_comments(line, in_block):
    pieces = []
    cursor = 0
    while cursor < len(line):
        if in_block:
            end = line.find("*/", cursor)
            if end < 0:
                return "".join(pieces), True
            cursor = end + 2
            in_block = False
            continue

        match = COMMENT_OR_LITERAL.search(line, cursor)
        if match is None:
            pieces.append(line[cursor:])
            break
        pieces.append(line[cursor:match.start()])
        token = match[0]
        cursor = match.end()
        if token == "//":
            break
        if token == "/*":
            in_block = True
            pieces.append(" ")
        else:
            pieces.append(token)
    return "".join(pieces), in_block


def literal(expression):
    token = expression.strip()
    if token in ("1", "(1)"):
        return True
    if token in ("0", "(0)"):
        return False
    return None


def update_conditionals(stack, directive, expression):
    if directive in ("elifdef", "elifndef"):
        raise ValueError("unsupported conditional")
    if directive in ("if", "ifdef", "ifndef"):
        frame = Conditional()
        frame.branch(literal(expression) if directive == "if" else None)
        stack.append(frame)
    elif directive in ("elif", "else", "endif"):
        if not stack:
            raise ValueError("unbalanced conditional")
        if directive == "endif":
            stack.pop()
            return
        frame = stack[-1]
        if frame.saw_else:
            raise ValueError("branch after else")
        frame.saw_else = directive == "else"
        frame.branch(True if frame.saw_else else literal(expression))


def generated_counts(text, names):
    counts = Counter()
    stack = []
    in_block = False
    for line_no, raw in enumerate(text.splitlines(), 1):
        if raw.rstrip().endswith("\\"):
            raise ValueError("unsupported line continuation")
        code, in_block = strip_comments(raw, in_block)
        directive = DIRECTIVE.match(code)
        if directive:
            update_conditionals(stack, *directive.groups())
            continue
        call = CALL.fullmatch(code)
        if not call or call[1] not in names:
            continue
        states = [frame.active for frame in stack]
        if False in states:
            continue
        if None in states:
            raise UnresolvedHook(call[1], line_no)
        counts[call[1]] += 1
    if stack or in_block:
        raise ValueError("unbalanced conditional or comment")
    return counts


def emit(exit_code, **fields):
    print(json.dumps({"ok": exit_code == 0, **fields}, separators=(",", ":"), sort_keys=True))
    return exit_code


def main(argv):
    options = dict(zip(argv[::2], argv[1::2]))
    if (len(argv) != 2 * len(options) or not {"--manifest", "--generated"} <= options.keys()
            or options.keys() - {"--manifest", "--generated", "--profile"}):
        return emit(1, error="usage")
    manifest = Path(options["--manifest"])
    generated = Path(options["--generated"])
    profile = options.get("--profile")
    if profile is None:
        profile = next((name for name, (basename, _) in PROFILES.items() if basename == manifest.name), None)
    if profile not in PROFILES:
        return emit(2, error="unsupported_revision", profile=profile)
    try:
        expected = manifest_counts(manifest.read_text(encoding="utf-8"))
    except OSError:
        return emit(1, error="unreadable_manifest")
    except UnicodeError:
        return emit(1, error="invalid_manifest", detail="expected UTF-8 text")
    except ValueError as error:
        return emit(1, error="invalid_manifest", detail=str(error))
    _, minimums = PROFILES[profile]
    for hook, minimum in minimums.items():
        if expected[hook] < minimum:
            return emit(7, error="profile_minimum", profile=profile,
                        hook=hook, manifest=expected[hook], minimum=minimum)
    if not generated.is_dir():
        return emit(3, error="missing_generated")
    chunks = sorted(path for path in generated.glob("chunk_*.cpp") if path.is_file())
    if not chunks:
        return emit(4, error="missing_chunks")

    actual = Counter()
    for chunk in chunks:
        try:
            text = chunk.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            return emit(8, error="unreadable_generated", file=chunk.name)
        try:
            actual.update(generated_counts(text, minimums))
        except UnresolvedHook as e:
            return emit(6, error="unresolved_conditional", file=chunk.name, hook=e.hook, line=e.line)
        except ValueError:
            return emit(8, error="invalid_generated", file=chunk.name)
    for hook in minimums:
        if actual[hook] != expected[hook]:
            return emit(5, error="count_mismatch", hook=hook, manifest=expected[hook], generated=actual[hook])
    return emit(0, profile=profile, counts={hook: actual[hook] for hook in minimums})


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
