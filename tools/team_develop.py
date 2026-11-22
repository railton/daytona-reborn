"""Code delivery broker: peers author files, explicit paths and tests govern application.

No model-selected shell commands are executed. Existing account ACP transports
return a file packet; the broker validates the whole packet before writing into
the assigned worktree. A successful generation is not integration approval.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import time

from team_queue import execute


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def safe_path(root, name):
    if not isinstance(name, str) or "\\" in name or ":" in name:
        raise ValueError("paths must be relative POSIX file names")
    relative = PurePosixPath(name)
    if relative.is_absolute() or any(p in ("..", ".git", ".codex", ".agents") for p in relative.parts):
        raise ValueError("unsafe delivery path")
    if not relative.parts or relative.as_posix() != name:
        raise ValueError("noncanonical delivery path")
    target = root.joinpath(*relative.parts)
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError("delivery escapes worktree")
    current = target
    while current != root:
        if current.is_symlink() or (hasattr(current, "is_junction") and current.is_junction()):
            raise ValueError("linked delivery path")
        current = current.parent
    return target


def packet(text):
    text = text.strip()
    if text.startswith("```json\n") and text.endswith("```"):
        text = text[8:-3]
    value = json.loads(text)
    if not isinstance(value, dict) or value.get("idle") is not True:
        raise ValueError("peer must explicitly report idle after delivery")
    return value


def apply_packet(root, expected, value):
    files = value.get("files")
    if not isinstance(files, list) or not files or len(files) > 8:
        raise ValueError("delivery requires 1..8 files")
    staged = {}
    for item in files:
        name, content = item["path"], item["content"]
        if name not in expected or name.casefold() in staged or not isinstance(content, str):
            raise ValueError("unexpected, duplicate or non-text file")
        path = safe_path(root, name)
        if digest(path) != expected[name]:
            raise ValueError("file changed since task assignment: " + name)
        if len(content.encode("utf-8")) > 120_000 or "\x00" in content:
            raise ValueError("invalid file size or binary content")
        staged[name.casefold()] = (name, path, content)
    if {v[0] for v in staged.values()} != set(expected):
        raise ValueError("delivery omitted assigned files")
    # Validate every file before the first mutation. A crash mid-apply is NOT
    # retried automatically: the task lease remains uncertain for reconciliation.
    for _, path, content in staged.values():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    return {name: digest(path) for name, path, _ in staged.values()}


def run_command(argv, cwd, timeout=180):
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout)
    return {"argv": argv, "exit": result.returncode,
            "stdout": result.stdout[-24000:], "stderr": result.stderr[-8000:]}


def perform(spec, backend, artifact_dir):
    root = Path(spec["cwd"]).resolve()
    artifacts = Path(artifact_dir).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    head = run_command(["git", "rev-parse", "HEAD"], root)
    if head["exit"] or head["stdout"].strip() != spec["base"]:
        raise ValueError("worktree is not at its assigned base")
    if spec["kind"] == "code":
        for name, baseline in spec["expected"].items():
            if digest(safe_path(root, name)) != baseline:
                raise ValueError("assignment baseline changed before dispatch")
    context = spec["prompt"]
    if spec["kind"] == "code":
        # The reservation is authoritative even when a hand-written prompt
        # forgets the filenames. Never make the peer infer its output paths.
        context += "\nAssigned output paths (return exactly these, no others): " + json.dumps(
            sorted(spec["expected"]), ensure_ascii=False)
    if spec["kind"] == "review":
        for name in spec["review_files"]:
            path = safe_path(Path(spec["review_root"]).resolve(), name)
            context += "\nFILE " + name + "\n" + path.read_text(encoding="utf-8")
    context += ("\nReturn only a JSON object, no markdown. Finish with idle:true and next_work "
                "as a short suggestion. No tools, browsing, shell commands or purchases. "
                "Your code will be written into your assigned worktree by the file broker, "
                "then tested independently. Do not claim you ran tests. ")
    if spec["kind"] == "code":
        context += "Schema: {summary:string, files:[{path:string,content:string}], idle:true, next_work:string}."
    else:
        context += "Schema: {summary:string, approved:boolean, findings:[string], idle:true, next_work:string}. Review the supplied implementation; give concrete counterexamples."
    answer, session, trace = execute(spec, backend, context,
        time.monotonic() + min(600, spec.get("timeout", 480)))
    # Completed answers only. No authentication packets or thought streams.
    (artifacts / "answer.txt").write_text(answer, encoding="utf-8")
    result = {"session": session, "protocol": trace, "packet": packet(answer),
              "base": spec["base"], "state": "delivered"}
    (artifacts / "delivery.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    if spec["kind"] == "code":
        result["files"] = apply_packet(root, spec["expected"], result["packet"])
        result["tests"] = []
        for argv in spec["tests"]:
            check = run_command(argv, root)
            result["tests"].append(check)
            if check["exit"]:
                result["state"] = "tests_failed"
                break
        else:
            result["state"] = "tested"
    else:
        result["state"] = "reviewed" if result["packet"].get("approved") is True else "changes_requested"
    (artifacts / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("spec"); ap.add_argument("backends"); ap.add_argument("artifacts")
    args = ap.parse_args()
    spec = json.loads(Path(args.spec).read_text(encoding="utf-8"))
    configs = json.loads(Path(args.backends).read_text(encoding="utf-8"))
    result = perform(spec, configs[spec["backend"]], args.artifacts)
    print(json.dumps({"state": result["state"], "session": result["session"],
                      "idle": result["packet"]["idle"]}))
    return 0 if result["state"] in {"tested", "reviewed"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
