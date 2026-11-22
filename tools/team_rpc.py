"""Bounded JSON-lines transports for local ACP and Codex app-server clients.

No shell command execution is approved. Prompts carry explicit source excerpts;
remote requests for tools, credentials, or user input are denied, never retried.
This is a review transport, not a sandbox for arbitrary third-party software.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time


class RpcError(RuntimeError):
    pass


class WindowsJob:
    """Closing the worker (including a crash) kills only its owned child tree."""
    def __init__(self, process):
        import ctypes as c
        from ctypes import wintypes as w
        class Basic(c.Structure):
            _fields_ = [("process_time", c.c_longlong), ("job_time", c.c_longlong),
                ("flags", w.DWORD), ("min_ws", c.c_size_t), ("max_ws", c.c_size_t),
                ("active", w.DWORD), ("affinity", c.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]
        class Counters(c.Structure):
            _fields_ = [(name, c.c_ulonglong) for name in
                        ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]
        class Extended(c.Structure):
            _fields_ = [("basic", Basic), ("io", Counters),
                ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                ("peak_process_memory", c.c_size_t), ("peak_job_memory", c.c_size_t)]
        self.api = c.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
        self.api.CreateJobObjectW.restype = w.HANDLE
        self.api.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
        self.api.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        self.api.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError(c.get_last_error(), "CreateJobObjectW")
        limits = Extended(); limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE
        if (not self.api.SetInformationJobObject(self.handle, 9, c.byref(limits), c.sizeof(limits))
                or not self.api.AssignProcessToJobObject(self.handle, int(process._handle))):
            error = c.get_last_error()
            self.close()
            raise OSError(error, "Cannot bound backend lifetime with a Windows Job Object")

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


class JsonLines:
    def __init__(self, argv, cwd, env=None, max_bytes=4_000_000):
        self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8", errors="replace",
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.job = None
        if os.name == "nt":
            try:
                self.job = WindowsJob(self.proc)
            except BaseException:
                self.proc.kill(); self.proc.wait()
                raise
        self.events = []
        self.errors = []
        self.inbox = queue.Queue(maxsize=4096)
        self.next_id = 0
        self.max_bytes = max_bytes
        self.bytes = 0
        self.failed = None
        self.closed = False
        self.write_thread = None
        self.read_thread = threading.Thread(target=self._read, daemon=True)
        self.error_thread = threading.Thread(target=self._stderr, daemon=True)
        self.read_thread.start(); self.error_thread.start()

    def _read(self):
        try:
            while line := self.proc.stdout.readline(self.max_bytes + 1):
                self.bytes += len(line.encode("utf-8"))
                if self.bytes > self.max_bytes:
                    self.failed = "protocol output limit exceeded"
                    break
                try:
                    msg = json.loads(line)
                    if isinstance(msg, dict):
                        self.inbox.put(msg, timeout=1)
                except json.JSONDecodeError:
                    self.failed = "non-JSON protocol output"
                    break
                except queue.Full:
                    self.failed = "protocol queue limit exceeded"
                    break
        finally:
            try:
                self.inbox.put(None, timeout=1)
            except queue.Full:
                pass

    def _stderr(self):
        # Always drain stderr; retain a bounded tail locally, never credentials.
        while line := self.proc.stderr.readline(4096):
            self.errors.append(line[-2000:])
            if len(self.errors) > 32:
                del self.errors[0]

    def send(self, msg, deadline=None):
        if self.proc.poll() is not None:
            raise RpcError("process exited before request")
        remaining = 5 if deadline is None else min(5, deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError("RPC deadline exceeded before write")
        errors = []
        def write():
            try:
                self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
                self.proc.stdin.flush()
            except (OSError, ValueError) as exc:
                errors.append(exc)
        self.write_thread = threading.Thread(target=write, daemon=True)
        self.write_thread.start()
        self.write_thread.join(timeout=remaining)
        if self.write_thread.is_alive():
            raise TimeoutError("RPC input pipe stalled")
        if errors:
            raise RpcError("RPC input pipe closed") from errors[0]

    def request(self, method, params, deadline):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}, deadline)
        while True:
            msg = self.receive(deadline)
            # A server request may reuse our numeric id: method distinguishes it.
            if "method" not in msg and msg.get("id") == rid:
                if "error" in msg:
                    raise RpcError(str(msg["error"])[:1000])
                if "result" not in msg:
                    raise RpcError("response missing result")
                return msg["result"]

    def receive(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("RPC deadline exceeded")
        try:
            msg = self.inbox.get(timeout=remaining)
        except queue.Empty:
            raise TimeoutError("RPC deadline exceeded") from None
        if msg is None:
            raise RpcError(self.failed or "protocol closed before completion")
        self.events.append(msg)
        if "method" in msg and "id" in msg:
            self.deny(msg)
        return msg

    def deny(self, msg):
        if msg["method"] == "session/request_permission":
            result = {"outcome": {"outcome": "cancelled"}}
        elif msg["method"].endswith("requestApproval"):
            result = {"decision": "decline"}
        else:
            self.send({"jsonrpc": "2.0", "id": msg["id"],
                       "error": {"code": -32601, "message": "Client tools are disabled for this task"}})
            return
        self.send({"jsonrpc": "2.0", "id": msg["id"], "result": result})

    def close(self):
        if self.closed:
            return
        self.closed = True
        # A stalled writer holds the pipe lock. Kill the owned process before
        # closing stdin, otherwise cleanup itself could block on that lock.
        if self.write_thread and self.write_thread.is_alive():
            if self.job:
                self.job.close()
            else:
                self.proc.kill()
            self.proc.wait(timeout=5)
            self.write_thread.join(timeout=2)
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=3)
            except (subprocess.TimeoutExpired, OSError):
                # Only the owned process tree, never an existing user session.
                if self.job:
                    self.job.close()
                else:
                    self.proc.kill()
                self.proc.wait(timeout=5)
        if self.job:
            self.job.close()
        self.read_thread.join(timeout=2)
        self.error_thread.join(timeout=2)
        for stream in (self.proc.stdout, self.proc.stderr):
            stream.close()
        self.proc.stdin.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def acp_initialize(rpc, cwd, deadline):
    hello = rpc.request("initialize", {"protocolVersion": 1,
        "clientCapabilities": {}, "clientInfo": {"name": "daytona-team", "version": "0.1"}}, deadline)
    session = rpc.request("session/new", {"cwd": str(Path(cwd).resolve()), "mcpServers": []}, deadline)
    if not session.get("sessionId"):
        raise RpcError("session/new returned no session ID")
    return hello, session


def acp_turn(rpc, session_id, prompt, deadline):
    begin = len(rpc.events)
    result = rpc.request("session/prompt", {"sessionId": session_id,
        "prompt": [{"type": "text", "text": prompt}]}, deadline)
    if result.get("stopReason") != "end_turn":
        raise RpcError("ACP did not finish normally: " + str(result.get("stopReason")))
    pieces = []
    for msg in rpc.events[begin:]:
        params = msg.get("params", {})
        if msg.get("method") != "session/update" or params.get("sessionId") != session_id:
            continue
        update = params.get("update", {})
        if update.get("sessionUpdate") == "agent_message_chunk":
            content = update.get("content", {})
            if content.get("type") == "text":
                pieces.append(content.get("text", ""))
    text = "".join(pieces).strip()
    if not text:
        raise RpcError("completed turn has no answer")
    return text


def codex_initialize(rpc, cwd, deadline):
    hello = rpc.request("initialize", {"clientInfo": {"name": "daytona-team",
        "title": "Daytona team", "version": "0.1"}, "capabilities": {}}, deadline)
    rpc.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    account = rpc.request("account/read", {}, deadline).get("account") or {}
    if account.get("type") != "chatgpt":
        raise RpcError("Only the existing ChatGPT account is authorized; API-key billing refused")
    thread = rpc.request("thread/start", {"cwd": str(Path(cwd).resolve()),
        "sandbox": "read-only", "approvalPolicy": "never", "ephemeral": True,
        "developerInstructions": "This is a bounded peer review of supplied text. Do not call tools, browse, spawn agents, change files, or contact any service. Return the requested review only."}, deadline)
    return hello, thread["thread"]["id"]


def codex_turn(rpc, thread_id, prompt, deadline):
    begin = len(rpc.events)
    response = rpc.request("turn/start", {"threadId": thread_id,
        "input": [{"type": "text", "text": prompt, "text_elements": []}]}, deadline)
    turn_id = response["turn"]["id"]
    # Completion can arrive before the turn/start reply. Scan buffered events too.
    cursor = begin
    while True:
        while cursor < len(rpc.events):
            msg = rpc.events[cursor]
            cursor += 1
            params = msg.get("params", {})
            if (msg.get("method") == "turn/completed" and params.get("threadId") == thread_id
                    and params.get("turn", {}).get("id") == turn_id):
                if params["turn"].get("status") != "completed":
                    raise RpcError("Codex turn did not complete: " + str(params["turn"].get("status")))
                pieces = []
                for event in rpc.events[begin:]:
                    p = event.get("params", {})
                    if (event.get("method") == "item/completed" and p.get("threadId") == thread_id
                            and p.get("turnId") == turn_id and p.get("item", {}).get("type") == "agentMessage"):
                        pieces.append(p["item"].get("text", ""))
                answer = "\n".join(pieces).strip()
                if not answer:
                    raise RpcError("completed Codex turn has no answer")
                return answer
        rpc.receive(deadline)
