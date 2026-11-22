"""No-mock regression for the generated hook CLI, using synthetic files."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def invoke(tool, manifest, generated, expected, **fields):
    run = subprocess.run([sys.executable, "-B", str(tool), "--manifest", str(manifest),
                          "--generated", str(generated)], capture_output=True, text=True, timeout=15)
    assert run.returncode == expected and not run.stderr, (run.returncode, run.stdout, run.stderr)
    report = json.loads(run.stdout)
    assert report["ok"] == (expected == 0), report
    assert all(report[key] == value for key, value in fields.items()), report


def main():
    tool = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (
        Path(__file__).resolve().parents[1] / "tools/check_generated_scenery_hooks.py"
    )
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        manifest = root / "daytona93_hooks.txt"
        manifest.write_text("0000000A scenery_cost\n0xa draw_list # generator ignores the rest\n", encoding="ascii")
        generated = root / "gen"
        invoke(tool, manifest, generated, 3, error="missing_generated")
        generated.mkdir()
        invoke(tool, manifest, generated, 4, error="missing_chunks")
        chunk = generated / "chunk_000.cpp"
        call = "  rt::hook_draw_list(c);\n"
        cases = (
            ("unknown else", "#if FLAG\n#else\n" + call + "#endif\n", 6, "unresolved_conditional"),
            ("unknown elif", "#if FLAG\n#elif 1\n" + call + "#endif\n", 6, "unresolved_conditional"),
            ("unknown dead elif", "#if FLAG\n#elif 0\n#else\n" + call + "#endif\n", 6, "unresolved_conditional"),
            ("known elif", "#if 0\n#elif 1\n" + call + "#endif\n", 0, None),
            ("dead final else", "#if FLAG\n#elif 1\n#else\n" + call + "#endif\n" + call, 0, None),
            ("malformed", "#endif\n" + call, 8, "invalid_generated"),
            ("double else", "#if 0\n#else\n#else\n" + call + "#endif\n", 8, "invalid_generated"),
            ("elifdef", "#if 0\n#elifdef FLAG\n" + call + "#endif\n", 8, "invalid_generated"),
            ("elifndef", "#if 1\n#elifndef FLAG\n" + call + "#endif\n", 8, "invalid_generated"),
            ("continuation", "#if 0\\\n || 1\n" + call + "#endif\n", 8, "invalid_generated"),
            ("comment continuation", "// comment \\\n" + call, 8, "invalid_generated"),
            ("comment elifdef", "// #elifdef FLAG\n" + call, 0, None),
            ("disabled", "#if 0\n" + call + "#endif\n", 5, "count_mismatch"),
            ("quoted comment marker", 'const char *marker = "/*";\n' + call, 0, None),
        )
        for label, source, status, error in cases:
            chunk.write_text(source, encoding="ascii")
            fields = {"error": error} if error else {"counts": {"draw_list": 1}}
            invoke(tool, manifest, generated, status, **fields)
        chunk.write_bytes(b"\xff\n")
        invoke(tool, manifest, generated, 8, error="unreadable_generated")
        manifest.write_text("0000000A scenery_cost\n", encoding="ascii")
        invoke(tool, manifest, root / "absent", 7, error="profile_minimum", hook="draw_list")
        manifest.write_text("xyz draw_list\n", encoding="ascii")
        invoke(tool, manifest, generated, 1, error="invalid_manifest",
               detail="line 1: expected ADDRESS NAME")
        manifest.write_text("  # misleading\n0xa draw_list\n", encoding="ascii")
        invoke(tool, manifest, generated, 1, error="invalid_manifest")
        manifest.write_text("0xa draw_list\n", encoding="utf-8-sig")
        invoke(tool, manifest, generated, 1, error="invalid_manifest")
        manifest.unlink()
        invoke(tool, manifest, generated, 1, error="unreadable_manifest")
    print("generated hook CLI: 22 synthetic controls passed")


if __name__ == "__main__":
    main()
