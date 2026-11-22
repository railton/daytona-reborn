"""Persistent, bounded project state; no model, shell or network execution.

Use database(path), initialize(db, goals, tasks, cycles=...), then claim(db,
agent) before each external call and finish(db, id, result, status). One claim
reserves one daily call; the worker must make at most one call for that claim.
Cycles are finite, authorized templates, not generated prompts. Completion of
tasks never validates goals or supplies human acceptance.

An interrupted dispatch is uncertain. Its agent/worktree leases remain held
until explicit recover(..., execution_stopped=True) creates a different task.
The caller must establish that the old execution cannot still write files.
STOP prevents new claims; the worker must recheck it immediately before launch
and during execution. It does not discard the result of an already claimed job.
"""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import time
import uuid


TERMINAL = {"completed", "failed", "interrupted", "blocked"}
FAILURES = {"failed", "interrupted", "blocked"}


def _json(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False)


def database(path):
    db = sqlite3.connect(path, timeout=30, isolation_level=None)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA journal_mode=WAL")
    if db.execute("SELECT 1 FROM sqlite_master WHERE name='tasks'").fetchone():
        fields = {r[1] for r in db.execute("PRAGMA table_info(tasks)")}
        if "claim_token" not in fields or "resolved_by" not in fields:
            db.close()
            raise ValueError("not a project-state database; use a separate database")
    db.executescript("""
      CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS goals(id TEXT PRIMARY KEY, description TEXT NOT NULL,
        evidence TEXT, validated_by TEXT);
      CREATE TABLE IF NOT EXISTS cycles(seq INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL,
        goal_id TEXT NOT NULL REFERENCES goals(id), phase TEXT NOT NULL,
        templates TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'backlog');
      CREATE TABLE IF NOT EXISTS tasks(seq INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL,
        agent TEXT NOT NULL, worktree TEXT NOT NULL, prompt TEXT NOT NULL,
        payload TEXT NOT NULL, deps TEXT NOT NULL, timeout REAL NOT NULL,
        cycle TEXT REFERENCES cycles(id), state TEXT NOT NULL DEFAULT 'pending',
        owner INTEGER, owner_agent TEXT, claim_token TEXT, result TEXT,
        created REAL NOT NULL, started REAL, ended REAL,
        recovery_of TEXT REFERENCES tasks(id), resolved_by TEXT REFERENCES tasks(id),
        blocked_reason TEXT);
      CREATE TABLE IF NOT EXISTS leases(task TEXT PRIMARY KEY REFERENCES tasks(id),
        agent TEXT UNIQUE NOT NULL, worktree TEXT UNIQUE NOT NULL);
      CREATE TABLE IF NOT EXISTS budget(day TEXT PRIMARY KEY, calls INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY, at REAL NOT NULL,
        task TEXT, event TEXT NOT NULL, detail TEXT NOT NULL);
    """)
    return db


@contextmanager
def _transaction(db, *, immediate=True):
    db.execute("BEGIN IMMEDIATE" if immediate else "BEGIN")
    try:
        yield
        db.execute("COMMIT")
    except BaseException:
        db.execute("ROLLBACK")
        raise


def _event(db, task, event, detail=None):
    db.execute("INSERT INTO events(at,task,event,detail) VALUES(?,?,?,?)",
               (time.time(), task, event, _json(detail or {})))


def _meta(db):
    values = dict(db.execute("SELECT key,value FROM meta"))
    if values.get("schema") != "project-state-v1":
        raise ValueError("project has not been initialized")
    return values


def _id(value):
    if not isinstance(value, str) or not value.strip() or len(value) > 200:
        raise ValueError("nonempty identifier of at most 200 characters required")
    return value


def _task(spec):
    worktree = Path(spec["worktree"])
    if not worktree.is_absolute() or not worktree.is_dir():
        raise ValueError("task requires an existing absolute worktree")
    prompt = spec["prompt"]
    if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 100_000:
        raise ValueError("task requires a bounded nonempty prompt")
    payload = spec.get("payload", {})
    if not isinstance(payload, dict):
        raise ValueError("payload must be a JSON object")
    _json(payload)
    deps = spec.get("after", [])
    if not isinstance(deps, list) or len(deps) != len(set(deps)):
        raise ValueError("after must be a list of distinct task ids")
    timeout = spec.get("timeout", 300)
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 1 <= timeout <= 1800:
        raise ValueError("task timeout must be 1..1800 seconds")
    return dict(id=_id(spec["id"]), agent=_id(spec["agent"]),
                worktree=os.path.normcase(str(worktree.resolve())), prompt=prompt,
                payload=payload, after=[_id(d) for d in deps], timeout=timeout,
                cycle=spec.get("cycle"))


def _insert(db, task, recovery_of=None):
    db.execute("INSERT INTO tasks(id,agent,worktree,prompt,payload,deps,timeout,cycle,created,recovery_of) "
               "VALUES(?,?,?,?,?,?,?,?,?,?)", (task["id"], task["agent"], task["worktree"],
               task["prompt"], _json(task["payload"]), _json(task["after"]), task["timeout"],
               task.get("cycle"), time.time(), recovery_of))


def _validate_graph(graph):
    done, visiting = set(), set()
    def visit(node):
        if node not in graph:
            raise ValueError("unknown dependency: " + node)
        if node in visiting:
            raise ValueError("dependency cycle: " + node)
        if node in done:
            return
        visiting.add(node)
        for dep in graph[node]:
            visit(dep)
        visiting.remove(node)
        done.add(node)
    for node in graph:
        visit(node)


def _expanded(cycle):
    templates = json.loads(cycle["templates"])
    local = {t["id"]: cycle["id"] + "/" + t["id"] for t in templates}
    return [dict(t, id=local[t["id"]], cycle=cycle["id"],
                 after=[local.get(d, d) for d in t["after"]]) for t in templates]


def _enqueue(db, tasks, cycles):
    graph = {r["id"]: json.loads(r["deps"]) for r in db.execute("SELECT id,deps FROM tasks")}
    reserved = set(graph)
    # Include unmaterialized tasks so future dependencies can be validated.
    for cycle in db.execute("SELECT * FROM cycles WHERE state='backlog'"):
        for task in _expanded(cycle):
            graph[task["id"]] = task["after"]
            reserved.add(task["id"])
    direct = [_task(t) for t in tasks]
    direct_available = {r[0] for r in db.execute("SELECT id FROM tasks")} | {t["id"] for t in direct}
    if any(not set(t["after"]) <= direct_available for t in direct):
        raise ValueError("direct tasks cannot depend on unmaterialized cycles")
    active_cycles = {r[0] for r in db.execute("SELECT id FROM cycles WHERE state='active'")}
    if any(t["cycle"] is not None and t["cycle"] not in active_cycles for t in direct):
        raise ValueError("direct tasks can only join an already active cycle")
    prepared = []
    goal_ids = {r[0] for r in db.execute("SELECT id FROM goals")}
    for c in cycles:
        cid, gid = _id(c["id"]), _id(c["goal_id"])
        if gid not in goal_ids:
            raise ValueError("unknown cycle goal")
        templates = [_task(t) for t in c["tasks"]]
        if not templates or len({t["id"] for t in templates}) != len(templates):
            raise ValueError("cycle requires distinct, nonempty templates")
        if "/" in cid or any("/" in t["id"] for t in templates):
            raise ValueError("cycle and local template ids cannot contain '/' separators")
        item = dict(id=cid, goal_id=gid, phase=_id(c.get("phase", cid)), templates=_json(templates))
        prepared.append(item)
    for task in direct + [t for c in prepared for t in _expanded(c)]:
        if task["id"] in reserved:
            raise ValueError("duplicate task id: " + task["id"])
        reserved.add(task["id"])
        graph[task["id"]] = task["after"]
    _validate_graph(graph)
    # A cycle may not depend on later cycles: planning is deliberately ordered.
    available = {r[0] for r in db.execute("SELECT id FROM tasks")} | {t["id"] for t in direct}
    all_cycles = list(db.execute("SELECT * FROM cycles WHERE state='backlog' ORDER BY seq")) + prepared
    for c in all_cycles:
        expanded = _expanded(c)
        available.update(t["id"] for t in expanded)
        if any(not set(t["after"]) <= available for t in expanded):
            raise ValueError("cycle depends on a later cycle")
    for c in prepared:
        db.execute("INSERT INTO cycles(id,goal_id,phase,templates) VALUES(?,?,?,?)",
                   (c["id"], c["goal_id"], c["phase"], c["templates"]))
    for t in direct:
        _insert(db, t)
    _event(db, None, "enqueued", {"tasks": [t["id"] for t in direct], "cycles": [c["id"] for c in prepared]})


def _validate_daily_limit(limit):
    if limit is not None and (isinstance(limit, bool) or not isinstance(limit, int) or limit < 1):
        raise ValueError("daily_limit must be a positive integer or None")


def set_daily_limit(db, limit, *, reason):
    """Audited coordinator policy change; counters and execution guards remain."""
    _validate_daily_limit(limit)
    if not isinstance(reason, str) or not reason.strip():
        raise ValueError("policy change requires a reason")
    with _transaction(db):
        meta = _meta(db)
        if db.execute("SELECT 1 FROM leases").fetchone():
            raise RuntimeError("cannot change policy while a task holds a lease")
        previous = json.loads(meta["daily_limit"])
        db.execute("UPDATE meta SET value=? WHERE key='daily_limit'", (_json(limit),))
        _event(db, None, "daily_limit_changed", {"previous": previous, "limit": limit, "reason": reason})


def initialize(db, goals, tasks=(), *, cycles=(), daily_limit=None, stop_file=None):
    """Idempotent only for the same initial manifest; use enqueue for new work."""
    goals, tasks, cycles = list(goals), list(tasks), list(cycles)
    _validate_daily_limit(daily_limit)
    if not goals:
        raise ValueError("at least one explicit project goal is required")
    path = db.execute("PRAGMA database_list").fetchone()[2]
    stop = str(Path(stop_file or Path(path).parent / "STOP").resolve())
    manifest = dict(goals=goals, tasks=tasks, cycles=cycles, daily_limit=daily_limit, stop_file=stop)
    digest = hashlib.sha256(_json(manifest).encode()).hexdigest()
    with _transaction(db):
        old = db.execute("SELECT value FROM meta WHERE key='manifest'").fetchone()
        if old:
            if old[0] != digest:
                raise ValueError("initial manifest changed; use enqueue")
            return
        db.executemany("INSERT INTO meta VALUES(?,?)", [("schema", "project-state-v1"),
            ("manifest", digest), ("daily_limit", _json(daily_limit)), ("stop_file", stop),
            ("acceptance", "null")])
        for goal in goals:
            description = goal["description"]
            if not isinstance(description, str) or not description.strip() or len(description) > 10_000:
                raise ValueError("goal requires a bounded description")
            db.execute("INSERT INTO goals(id,description) VALUES(?,?)",
                       (_id(goal["id"]), description))
        _enqueue(db, tasks, cycles)
        _event(db, None, "initialized", {"manifest": digest})


def enqueue(db, tasks=(), *, cycles=()):
    with _transaction(db):
        _meta(db)
        _enqueue(db, list(tasks), list(cycles))
        db.execute("UPDATE meta SET value='null' WHERE key='acceptance'")
        db.execute("UPDATE goals SET evidence=NULL,validated_by=NULL")
        _event(db, None, "acceptance_invalidated", {"reason": "scope changed"})


def _rows(db):
    return {r["id"]: dict(r) for r in db.execute("SELECT * FROM tasks ORDER BY seq")}


def _complete(tasks, ident, seen=None):
    row = tasks[ident]
    if row["state"] == "completed":
        return True
    if row["resolved_by"]:
        seen = set() if seen is None else seen
        if ident in seen:
            raise RuntimeError("corrupt recovery chain")
        seen.add(ident)
        return _complete(tasks, row["resolved_by"], seen)
    return False


def _failures(tasks):
    return [r for r in tasks.values() if r["state"] in FAILURES
            and not r["resolved_by"] and r["blocked_reason"] != "dependency"]


def _dependency_view(tasks):
    """Resolve derived blocks to a fixed point without changing stored failures.

    Clear stale derived blocks first, then propagate actual failures. Each pass
    only adds blocks; at most one block per eligible task can be added, so this
    is bounded and independent of insertion order. No work is dispatched here.
    """
    eligible = [r for r in tasks.values()
                if r["state"] == "pending" or r["blocked_reason"] == "dependency"]
    for row in eligible:
        row["state"], row["blocked_reason"] = "pending", None
    for _ in range(len(eligible) + 1):
        changed = False
        for row in eligible:
            if row["state"] == "blocked":
                continue
            deps = json.loads(row["deps"])
            blocked = any(not _complete(tasks, d) and tasks[d]["state"] in FAILURES for d in deps)
            if blocked:
                row["state"], row["blocked_reason"] = "blocked", "dependency"
                changed = True
        if not changed:
            return
    raise RuntimeError("dependency propagation did not converge")


def _reconcile_dependencies(db):
    """Persist the dependency view without planning any new cycle."""
    tasks = _rows(db)
    before = {ident: (r["state"], r["blocked_reason"]) for ident, r in tasks.items()}
    _dependency_view(tasks)
    for ident, row in tasks.items():
        if before[ident] != (row["state"], row["blocked_reason"]):
            db.execute("UPDATE tasks SET state=?,blocked_reason=? WHERE id=?",
                       (row["state"], row["blocked_reason"], ident))


def _plan(db):
    _reconcile_dependencies(db)
    tasks = _rows(db)
    if _failures(tasks):
        return []
    for c in db.execute("SELECT * FROM cycles ORDER BY seq").fetchall():
        members = [t for t in tasks.values() if t["cycle"] == c["id"]]
        if c["state"] == "active":
            if not members or not all(_complete(tasks, t["id"]) for t in members):
                return []
            db.execute("UPDATE cycles SET state='completed' WHERE id=?", (c["id"],))
            _event(db, None, "cycle_completed", {"cycle": c["id"]})
        elif c["state"] == "backlog":
            expanded = _expanded(c)
            for t in expanded:
                _insert(db, t)
            db.execute("UPDATE cycles SET state='active' WHERE id=?", (c["id"],))
            _event(db, None, "cycle_planned", {"cycle": c["id"], "tasks": [t["id"] for t in expanded]})
            return [t["id"] for t in expanded]
    return []


def plan_next_cycle(db):
    with _transaction(db):
        meta = _meta(db)
        if Path(meta["stop_file"]).exists():
            return []
        return _plan(db)


def _day():
    return datetime.fromtimestamp(time.time(), timezone.utc).date().isoformat()


def claim(db, agent, *, isolate_failures=False, mailbox_task=None):
    """Reserve budget and leases for an eligible task on the selected transport."""
    _id(agent)
    with _transaction(db):
        meta = _meta(db)
        if Path(meta["stop_file"]).exists():
            return None
        _plan(db)
        tasks = _rows(db)
        failures = _failures(tasks)
        if ((failures and not isolate_failures) or
                db.execute("SELECT 1 FROM leases WHERE agent=?", (agent,)).fetchone()):
            return None
        day = _day()
        used = db.execute("SELECT calls FROM budget WHERE day=?", (day,)).fetchone()
        limit = json.loads(meta["daily_limit"])
        if limit is not None and used and used[0] >= limit:
            return None
        for row in tasks.values():
            if row["state"] != "pending" or row["agent"] not in (agent, "*"):
                continue
            if not all(_complete(tasks, d) for d in json.loads(row["deps"])):
                continue
            if mailbox_task is not None:
                is_mailbox = json.loads(row["payload"]).get("kind") == "mailbox"
                if is_mailbox != mailbox_task:
                    continue
            # In rolling mode, quarantine the failed actor and checkout.
            # An independent peer can claim work in another checkout.
            if any(failed["worktree"] == row["worktree"] or
                   (failed["owner_agent"] or failed["agent"]) in (agent, "*")
                   for failed in failures):
                continue
            if db.execute("SELECT 1 FROM leases WHERE worktree=?", (row["worktree"],)).fetchone():
                continue
            token = uuid.uuid4().hex
            db.execute("INSERT INTO leases VALUES(?,?,?)", (row["id"], agent, row["worktree"]))
            db.execute("UPDATE tasks SET state='running',owner=?,owner_agent=?,claim_token=?,started=? WHERE id=?",
                       (os.getpid(), agent, token, time.time(), row["id"]))
            db.execute("INSERT INTO budget VALUES(?,1) ON CONFLICT(day) DO UPDATE SET calls=calls+1", (day,))
            _event(db, row["id"], "claimed", {"agent": agent, "call_reserved": True})
            return _public(dict(db.execute("SELECT * FROM tasks WHERE id=?", (row["id"],)).fetchone()))
        return None


def _public(row):
    for key in ("payload", "result"):
        row[key] = json.loads(row[key]) if row[key] is not None else None
    row["after"] = json.loads(row.pop("deps"))
    return row


def finish(db, task_id, result, status, *, claim_token=None):
    if status not in TERMINAL:
        raise ValueError("finish requires completed, failed, interrupted or blocked")
    encoded = _json(result)
    if len(encoded) > 1_000_000:
        raise ValueError("result exceeds storage limit")
    with _transaction(db):
        meta = _meta(db)
        task = db.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
        if not task or task["state"] != "running" or task["owner"] != os.getpid():
            raise RuntimeError("task is not owned by this worker")
        if claim_token is not None and claim_token != task["claim_token"]:
            raise RuntimeError("claim token mismatch")
        db.execute("UPDATE tasks SET state=?,result=?,ended=?,blocked_reason=? WHERE id=?",
                   (status, encoded, time.time(), "worker" if status == "blocked" else None, task_id))
        if status != "interrupted":
            db.execute("DELETE FROM leases WHERE task=?", (task_id,))
        _event(db, task_id, status, {"agent": task["owner_agent"]})
        _event(db, task_id, "agent_idle", {"agent": task["owner_agent"], "status": status,
                                          "quarantined": status == "interrupted"})
        if not Path(meta["stop_file"]).exists():
            _plan(db)


def _alive(pid):
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE
        if not handle:
            return ctypes.get_last_error() != 87  # access denied is not proof of death
        try:
            # A terminated process can return259, also named STILL_ACTIVE.
            # Only a signaled process handle proves termination; a zero-time
            # wait never blocks, and errors/uncertainty retain the lease.
            return kernel.WaitForSingleObject(handle, 0) != 0  # WAIT_OBJECT_0
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def reconcile(db):
    """Mark demonstrably dead workers interrupted; never retry or release leases."""
    interrupted = []
    with _transaction(db):
        meta = _meta(db)
        for task in db.execute("SELECT * FROM tasks WHERE state='running'").fetchall():
            if not _alive(task["owner"]):
                db.execute("UPDATE tasks SET state='interrupted',ended=?,result=? WHERE id=?",
                           (time.time(), _json({"reason": "worker exited; dispatch outcome uncertain"}), task["id"]))
                _event(db, task["id"], "interrupted", {"reason": "worker exited", "outcome_uncertain": True})
                _event(db, task["id"], "agent_idle", {"agent": task["owner_agent"], "quarantined": True})
                interrupted.append(task["id"])
        if not Path(meta["stop_file"]).exists():
            _plan(db)
    return interrupted


def recover(db, task_id, replacement, reason, *, execution_stopped=False, local_evidence=None):
    """Explicit reconciliation only; a new id/prompt and stopped execution required."""
    if execution_stopped is not True or not isinstance(reason, str) or not reason.strip():
        raise ValueError("recovery requires a reason and verified stopped execution")
    new = _task(replacement)
    with _transaction(db):
        meta = _meta(db)
        old = db.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
        if not old or old["state"] not in FAILURES or old["resolved_by"] or old["blocked_reason"] == "dependency":
            raise ValueError("task has no unresolved execution failure")
        # An attributed local code repair can preserve the failed delivery and
        # unblock its independent review without pretending to call a model.
        # Evidence is the coordinator's attestation, not an executed test here.
        if local_evidence is not None:
            prior = json.loads(old["result"] or "{}")
            checks = local_evidence.get("tests", [])
            if (json.loads(old["payload"]).get("kind") != "code" or
                prior.get("state") != "tests_failed" or old["state"] != "blocked" or
                new["agent"] != "coordinator" or new["after"] or
                new["worktree"] != old["worktree"] or
                not local_evidence.get("validator") or not local_evidence.get("hashes") or
                not checks or any(c.get("exit") != 0 or not c.get("argv") for c in checks) or
                db.execute("SELECT 1 FROM leases WHERE worktree=?", (old["worktree"],)).fetchone()):
                raise ValueError("local repair needs stopped failed code, successful tests and hashes")
        if new["id"] == task_id or new["prompt"] == old["prompt"]:
            raise ValueError("recovery must be a differentiated task, not a blind resubmission")
        new["cycle"] = old["cycle"]
        graph = {r["id"]: json.loads(r["deps"]) + ([r["resolved_by"]] if r["resolved_by"] else [])
                 for r in db.execute("SELECT id,deps,resolved_by FROM tasks")}
        reserved = {t["id"] for c in db.execute("SELECT * FROM cycles WHERE state='backlog'") for t in _expanded(c)}
        if new["id"] in graph or new["id"] in reserved:
            raise ValueError("recovery id already exists")
        graph[new["id"]] = new["after"]
        graph[task_id] = graph[task_id] + [new["id"]]  # resolution must not introduce a cycle
        _validate_graph(graph)
        _insert(db, new, recovery_of=task_id)
        db.execute("UPDATE tasks SET resolved_by=? WHERE id=?", (new["id"], task_id))
        db.execute("DELETE FROM leases WHERE task=?", (task_id,))
        db.execute("UPDATE meta SET value='null' WHERE key='acceptance'")
        _event(db, task_id, "recovery", {"replacement": new["id"], "reason": reason, "execution_stopped": True})
        if local_evidence is not None:
            db.execute("UPDATE tasks SET state='completed',result=?,ended=?,owner_agent='coordinator' WHERE id=?",
                (_json(dict(state="local_verified", evidence=local_evidence)), time.time(), new["id"]))
            _event(db, new["id"], "local_repair_verified", local_evidence)
            _event(db, new["id"], "agent_idle", {"agent": "coordinator", "status": "completed"})
            if not Path(meta["stop_file"]).exists():
                # The verified local repair resolves the failure immediately;
                # its dependants must not wait for the next claim/plan pass.
                _reconcile_dependencies(db)
    return new["id"]


def validate_goal(db, goal_id, evidence, *, validated_by="coordinator"):
    """Record an external validation assertion, never infer it from task results."""
    if not evidence or not validated_by:
        raise ValueError("nonempty validation evidence and validator required")
    with _transaction(db):
        _meta(db)
        if db.execute("UPDATE goals SET evidence=?,validated_by=? WHERE id=?",
                      (_json(evidence), validated_by, goal_id)).rowcount != 1:
            raise ValueError("unknown goal")
        db.execute("UPDATE meta SET value='null' WHERE key='acceptance'")
        _event(db, None, "goal_validated", {"goal": goal_id, "validated_by": validated_by, "evidence": evidence})


def _ready_for_acceptance(db):
    tasks = _rows(db)
    return (all(_complete(tasks, t) for t in tasks) and
            not db.execute("SELECT 1 FROM cycles WHERE state!='completed'").fetchone() and
            not db.execute("SELECT 1 FROM goals WHERE evidence IS NULL").fetchone())


def accept(db, accepted_by, evidence, *, human=False):
    """Only an explicitly attributed human acceptance can complete the project."""
    if human is not True or not accepted_by or not evidence:
        raise ValueError("explicit human acceptance and evidence required")
    with _transaction(db):
        _meta(db)
        if not _ready_for_acceptance(db):
            raise ValueError("goals lack validation or project work remains")
        value = dict(human=True, accepted_by=accepted_by, evidence=evidence, at=time.time())
        db.execute("UPDATE meta SET value=? WHERE key='acceptance'", (_json(value),))
        _event(db, None, "human_acceptance", value)


def status(db):
    """Read-only snapshot. Empty work queues never imply goal completion."""
    with _transaction(db, immediate=False):
        return _status(db)


def _status(db):
    meta = _meta(db)
    tasks = _rows(db)
    stopped = Path(meta["stop_file"]).exists()
    if not stopped:
        # STOP removal is external to SQLite. Project the current dependency
        # view on read, without dispatch, planning or writes; claim persists it.
        _dependency_view(tasks)
    goals = [dict(r) for r in db.execute("SELECT * FROM goals ORDER BY id")]
    for g in goals:
        g["evidence"] = json.loads(g["evidence"]) if g["evidence"] else None
    cycles = [dict(r) for r in db.execute("SELECT id,goal_id,phase,state FROM cycles ORDER BY seq")]
    used = db.execute("SELECT calls FROM budget WHERE day=?", (_day(),)).fetchone()
    calls, limit = (used[0] if used else 0), json.loads(meta["daily_limit"])
    acceptance = json.loads(meta["acceptance"])
    reason = None
    if acceptance and acceptance.get("human") is True and _ready_for_acceptance(db):
        state = "completed"
    elif stopped:
        state, reason = "stopped", "STOP"
    elif _failures(tasks):
        state, reason = "blocked", "execution_failure"
    elif any(t["state"] == "running" for t in tasks.values()):
        state = "active"
    elif limit is not None and calls >= limit and (any(t["state"] == "pending" for t in tasks.values()) or
                             any(c["state"] == "backlog" for c in cycles)):
        state, reason = "blocked", "daily_budget"
    elif any(t["state"] == "pending" for t in tasks.values()) or any(c["state"] == "backlog" for c in cycles):
        state = "active"
    else:
        state, reason = "needs_planning", "human_acceptance" if _ready_for_acceptance(db) else "open_goals"
    return dict(state=state, reason=reason, goals=goals, cycles=cycles,
                tasks=[_public(t) for t in tasks.values()],
                leases=[dict(r) for r in db.execute("SELECT * FROM leases ORDER BY agent")],
                budget=dict(day=_day(), calls=calls, limit=limit, remaining=None if limit is None else max(0, limit-calls)),
                acceptance=acceptance)
