"""Minimal ACP client for Grok Build (`grok agent stdio`).

Read-only by default: read/search/fetch/think tool calls are allowed, edits and
shell execution are rejected. Use --allow-write only in an isolated worktree.
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

GROK = Path(os.environ.get("GROK_BIN", str(
    Path.home() / "AppData/Local/Microsoft/WinGet/Packages/xAI.GrokBuild_Microsoft.Winget.Source_8wekyb3d8bbwe/grok.exe")))
if not GROK.is_file():
    found = shutil.which("grok")
    if found:
        GROK = Path(found)
READ_KINDS = {"read", "search", "fetch", "think", "other"}
READ_COMMANDS = ("rg ", "rg.exe ", "grep ", "git status", "git log", "git diff", "git show",
                 "ls ", "dir ", "cat ", "type ", "head ", "tail ", "wc ", "find ", "where ", "pwd")


def read_only_command(command: str) -> bool:
    c = command.strip().lower()
    if any(x in c for x in (">", "set-content", "out-file", "remove-", "del ", "rmdir",
                            "mkdir", "new-item", "copy ", "move ", "git commit", "git push",
                            "git checkout", "git switch", "git reset", "git clean")):
        return False
    return any(c.startswith(p) or (" " + p) in c for p in READ_COMMANDS)


class Acp:
    def __init__(self, cwd: Path, allow_write: bool):
        self.cwd = cwd
        self.allow_write = allow_write
        self.p = subprocess.Popen(
            [str(GROK), "agent", "stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.q: queue.Queue = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.next_id = 1

    def _reader(self):
        assert self.p.stdout is not None
        for line in self.p.stdout:
            try:
                self.q.put(json.loads(line))
            except json.JSONDecodeError:
                pass

    def send(self, method: str, params: dict, request_id: int | None = None):
        assert self.p.stdin is not None
        rid = request_id if request_id is not None else self.next_id
        if request_id is None:
            self.next_id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        return rid

    def _permission(self, msg: dict):
        params = msg.get("params", {})
        tool = params.get("toolCall", {})
        kind = str(tool.get("kind", "")).lower()
        title = str(tool.get("title", ""))
        options = params.get("options", [])
        allow = kind in READ_KINDS or (not kind and "trust" in title.lower())
        if not self.allow_write and kind in {"edit", "execute", "delete", "move"}:
            allow = False
        chosen = None
        for opt in options:
            k = str(opt.get("kind", "")).lower()
            if allow and ("allow" in k or "allow" in str(opt.get("name", "")).lower()):
                chosen = opt
                break
            if not allow and ("reject" in k or "deny" in k or "reject" in str(opt.get("name", "")).lower()):
                chosen = opt
                break
        if chosen is None and options:
            chosen = options[0] if allow else options[-1]
        print(f"[permission] {kind or '?'} {title!r} -> {chosen.get('name') if chosen else 'none'}", file=sys.stderr)
        if chosen:
            self.send("session/request_permission", {}, None)  # placeholder, replaced below
            # The helper above is not used; send the response directly.

    def wait(self, want_id: int, timeout: float = 180.0):
        end = time.time() + timeout
        events = []
        while time.time() < end:
            try:
                msg = self.q.get(timeout=1)
            except queue.Empty:
                if self.p.poll() is not None:
                    break
                continue
            if msg.get("method") == "session/request_permission":
                self._answer_permission(msg)
            events.append(msg)
            if msg.get("id") == want_id:
                return events
        return events

    def _answer_permission(self, msg: dict):
        params = msg.get("params", {})
        tool = params.get("toolCall", {})
        kind = str(tool.get("kind", "")).lower()
        title = str(tool.get("title", ""))
        options = params.get("options", [])
        raw = tool.get("rawInput") or tool.get("input") or {}
        command = str(raw.get("command", "")) if isinstance(raw, dict) else ""
        allow = kind in READ_KINDS or (not kind and "trust" in title.lower())
        if self.allow_write:
            allow = True
        elif kind == "execute":
            allow = read_only_command(command)
        if not self.allow_write and kind in {"edit", "delete", "move"}:
            allow = False
        print(f"[permission] kind={kind!r} title={title!r} cmd={command[:300]!r} allow={allow}", file=sys.stderr)
        chosen = None
        for opt in options:
            k = str(opt.get("kind", "")).lower()
            n = str(opt.get("name", "")).lower()
            if allow and ("allow" in k or "allow" in n):
                chosen = opt
                break
            if not allow and ("reject" in k or "deny" in k or "reject" in n):
                chosen = opt
                break
        if chosen is None and options:
            chosen = options[0] if allow else options[-1]
        print(f"[permission] {kind or '?'} {title!r} -> {chosen.get('name') if chosen else 'none'}", file=sys.stderr)
        if chosen:
            assert self.p.stdin is not None
            self.p.stdin.write(json.dumps({
                "jsonrpc": "2.0", "id": msg["id"],
                "result": {"outcome": {"outcome": "selected", "optionId": chosen["optionId"]}},
            }) + "\n")
            self.p.stdin.flush()

    def close(self):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cwd", type=Path, required=True)
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--allow-write", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    cwd = a.cwd.resolve()
    if not GROK.is_file():
        raise SystemExit(f"Grok CLI missing: {GROK}")
    acp = Acp(cwd, a.allow_write)
    try:
        acp.send("initialize", {"protocolVersion": 1, "clientCapabilities": {},
                                "clientInfo": {"name": "dsh-acp-bridge", "version": "0.1"}})
        acp.wait(1, 20)
        sid_msg = acp.wait(acp.send("session/new", {"cwd": str(cwd), "mcpServers": []}), 30)
        sid = None
        for m in sid_msg:
            if m.get("id") == 2 and isinstance(m.get("result"), dict):
                sid = m["result"].get("sessionId")
        if not sid:
            raise SystemExit("ACP session/new failed (auth?)")
        acp.send("session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": a.prompt}]})
        events = acp.wait(3, a.timeout)
        text = []
        result = None
        for m in events:
            if m.get("method") == "session/update":
                u = m.get("params", {}).get("update", {})
                if u.get("sessionUpdate") == "agent_message_chunk":
                    c = u.get("content", {})
                    if isinstance(c, dict) and c.get("type") == "text":
                        text.append(c.get("text", ""))
            elif m.get("id") == 3:
                result = m.get("result")
        out = {"sessionId": sid, "text": "".join(text), "result": result}
        print(json.dumps(out, ensure_ascii=False, indent=2) if a.json else out["text"])
        return 0 if result and result.get("stopReason") == "end_turn" else 1
    finally:
        acp.close()


if __name__ == "__main__":
    sys.exit(main())

