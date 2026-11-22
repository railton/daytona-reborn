"""State-engine tests, including real process exit/restart and concurrent claims."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import team_project as project


class ProjectTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[1] / "build-team-project-tests"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.a, self.b = self.root / "a", self.root / "b"
        self.a.mkdir()
        self.b.mkdir()
        self.path = self.root / "project.sqlite"
        self.db = project.database(self.path)
        self.addCleanup(self.db.close)
        self.goals = [{"id": "game", "description": "Evidence-validated modern game with human acceptance"}]

    def task(self, ident, agent="one", worktree=None, **extra):
        return dict(id=ident, agent=agent, worktree=str(worktree or self.a),
                    prompt="Perform task " + ident, **extra)

    def initialize(self, tasks=(), **kwargs):
        project.initialize(self.db, self.goals, tasks, **kwargs)

    def finish(self, task, state="completed", result=None):
        project.finish(self.db, task["id"], result or {"state": "tested", "agent_idle": True},
                       state, claim_token=task["claim_token"])

    def child(self, source, *args):
        prelude = "import sys; sys.path.insert(0, sys.argv[1]); import team_project as p; "
        return subprocess.run([sys.executable, "-c", prelude + source,
                               str(Path(project.__file__).parent), str(self.path), *args],
                              capture_output=True, text=True, timeout=20)

    def events(self, name):
        return [dict(r) for r in self.db.execute("SELECT * FROM events WHERE event=?", (name,))]

    def test_empty_queue_is_needs_planning_never_completion(self):
        self.initialize()
        self.assertIsNone(project.claim(self.db, "one"))
        self.assertEqual(project.status(self.db)["state"], "needs_planning")
        self.assertEqual(project.status(self.db)["budget"]["calls"], 0)
        with self.assertRaises(ValueError):
            project.accept(self.db, "human", "confirmed", human=True)

    def test_local_repair_keeps_failure_and_budget_then_requires_review(self):
        self.initialize([self.task('code', payload={'kind': 'code'}),
                         self.task('review', agent='reviewer', after=['code'])], daily_limit=2)
        self.finish(project.claim(self.db, 'one'), 'blocked', {'state': 'tests_failed'})
        repair = self.task('local-fix', agent='coordinator')
        evidence = dict(validator='local coordinator', hashes={'source.py': 'verified-hash'},
                        tests=[dict(argv=['python', 'test.py'], exit=0)])
        for bad in ({}, {**evidence, 'tests': [dict(argv=['test'], exit=1)]}):
            with self.assertRaises(ValueError):
                project.recover(self.db, 'code', repair, 'fixed fixture',
                    execution_stopped=True, local_evidence=bad)
        project.recover(self.db, 'code', repair, 'fixed fixture',
            execution_stopped=True, local_evidence=evidence)
        state = project.status(self.db)
        self.assertEqual(state['budget']['calls'], 1)
        self.assertEqual(state['tasks'][0]['state'], 'blocked')
        self.assertEqual(state['tasks'][0]['result']['state'], 'tests_failed')
        self.assertEqual(state['tasks'][-1]['result']['state'], 'local_verified')
        self.assertEqual(project.claim(self.db, 'reviewer')['id'], 'review')
        self.assertEqual(project.status(self.db)['budget']['calls'], 2)

    def test_local_repair_cannot_approve_a_review(self):
        self.initialize([self.task('review', payload={'kind': 'review'})])
        self.finish(project.claim(self.db, 'one'), 'blocked', {'state': 'changes_requested'})
        with self.assertRaises(ValueError):
            project.recover(self.db, 'review', self.task('bypass', agent='coordinator'),
                'not an independent review', execution_stopped=True,
                local_evidence=dict(validator='coordinator', hashes={'f': 'h'},
                                    tests=[dict(argv=['test'], exit=0)]))
        self.assertEqual(len(project.status(self.db)['tasks']), 1)

    def test_two_cycles_materialize_automatically_and_payload_survives(self):
        payload = {"cwd": str(self.a), "expected": ["sample.cpp"], "tests": [["python", "-V"]],
                   "nested": {"unicode": "révision", "enabled": True}}
        cycles = [dict(id="c1", goal_id="game", phase="implement", tasks=[
            self.task("code", payload=payload), self.task("review", "two", self.b, after=["code"])]),
            dict(id="c2", goal_id="game", phase="improve", tasks=[self.task("next", "two", self.b)])]
        self.initialize(cycles=cycles)
        self.assertEqual(project.status(self.db)["tasks"], [])
        self.assertIsNone(project.claim(self.db, "two"))  # review waits for code
        first = project.claim(self.db, "one")
        self.assertEqual(first["id"], "c1/code")
        self.assertEqual(first["payload"], payload)
        self.assertEqual(first["state"], "running")
        self.assertIsNone(project.claim(self.db, "two"))
        self.finish(first)
        review = project.claim(self.db, "two")
        self.assertEqual(review["id"], "c1/review")
        self.assertEqual(review["after"], ["c1/code"])
        self.finish(review, result={"state": "reviewed", "agent_idle": True})
        snapshot = project.status(self.db)
        self.assertEqual([c["state"] for c in snapshot["cycles"]], ["completed", "active"])
        self.assertEqual(snapshot["tasks"][-1]["id"], "c2/next")
        self.finish(project.claim(self.db, "two"))
        self.assertEqual(project.status(self.db)["state"], "needs_planning")
        self.assertEqual(project.status(self.db)["budget"]["calls"], 3)
        self.assertEqual(len(self.events("cycle_planned")), 2)
        self.assertEqual(len(self.events("cycle_completed")), 2)
        self.assertEqual(len(self.events("agent_idle")), 3)

    def test_completion_requires_validated_goals_and_explicit_human(self):
        self.initialize([self.task("code")])
        self.finish(project.claim(self.db, "one"), result={"goal_completed": True, "human": True})
        self.assertEqual(project.status(self.db)["state"], "needs_planning")
        with self.assertRaises(ValueError):
            project.accept(self.db, "operator", "looks good", human=True)
        project.validate_goal(self.db, "game", {"report": "checked-report", "sha256": "a" * 64})
        self.assertEqual(project.status(self.db)["reason"], "human_acceptance")
        with self.assertRaises(ValueError):
            project.accept(self.db, "operator", "looks good")
        project.accept(self.db, "operator", {"confirmation": "explicit approval"}, human=True)
        self.assertEqual(project.status(self.db)["state"], "completed")
        project.enqueue(self.db, [self.task("more")])
        self.assertIsNone(project.status(self.db)["acceptance"])
        self.assertIsNone(project.status(self.db)["goals"][0]["evidence"])

    def test_rolling_claim_isolates_actor_and_worktree_failures(self):
        self.initialize([
            self.task("first", agent="one", worktree=self.a),
            self.task("same-actor", agent="one", worktree=self.b),
            self.task("same-worktree", agent="two", worktree=self.a),
            self.task("dependent", agent="two", worktree=self.b, after=["first"]),
            self.task("independent", agent="two", worktree=self.b),
        ])
        first = project.claim(self.db, "one")
        self.finish(first, "interrupted", {"error": "uncertain delivery"})
        self.assertIsNone(project.claim(self.db, "one", isolate_failures=True))
        self.assertIsNone(project.claim(self.db, "two"))
        peer = project.claim(self.db, "two", isolate_failures=True)
        self.assertEqual(peer["id"], "independent")
        self.finish(peer)
        self.assertIsNone(project.claim(self.db, "two", isolate_failures=True))
        self.assertEqual(self.db.execute("select task from leases").fetchone()[0], "first")

    def test_rolling_known_failure_quarantines_actor_and_worktree_without_lease(self):
        self.initialize([
            self.task("first", agent="one", worktree=self.a),
            self.task("same-actor", agent="one", worktree=self.b),
            self.task("same-worktree", agent="two", worktree=self.a),
            self.task("dependent", agent="two", worktree=self.b, after=["first"]),
            self.task("transitive", agent="two", worktree=self.b, after=["dependent"]),
            self.task("independent", agent="two", worktree=self.b),
        ])
        self.finish(project.claim(self.db, "one"), "blocked", {"error": "known failure"})
        self.assertFalse(self.db.execute("select * from leases").fetchall())
        self.assertIsNone(project.claim(self.db, "one", isolate_failures=True))
        peer = project.claim(self.db, "two", isolate_failures=True)
        self.assertEqual(peer["id"], "independent")
        self.finish(peer)
        self.assertIsNone(project.claim(self.db, "two", isolate_failures=True))

    def test_failure_stops_dispatch_and_never_retries(self):
        self.initialize([self.task("broken"), self.task("dependent", "two", self.b, after=["broken"]),
                         self.task("independent", "two", self.b)])
        self.finish(project.claim(self.db, "one"), "failed", {"state": "changes_requested"})
        for _ in range(3):
            project.reconcile(self.db)
            self.assertIsNone(project.claim(self.db, "one"))
            self.assertIsNone(project.claim(self.db, "two"))
        snap = project.status(self.db)
        self.assertEqual(snap["state"], "blocked")
        self.assertEqual(snap["budget"]["calls"], 1)
        self.assertEqual(snap["tasks"][1]["state"], "blocked")

    def test_controlled_recovery_differentiates_and_unblocks_dependencies(self):
        self.initialize([self.task("old"), self.task("after", "two", self.b, after=["old"])])
        self.finish(project.claim(self.db, "one"), "interrupted")
        replacement = self.task("investigate")
        with self.assertRaises(ValueError):
            project.recover(self.db, "old", replacement, "review partial outputs")
        duplicate = dict(replacement, prompt="Perform task old")
        with self.assertRaises(ValueError):
            project.recover(self.db, "old", duplicate, "blind retry", execution_stopped=True)
        cyclic = dict(replacement, after=["after"])
        with self.assertRaises(ValueError):
            project.recover(self.db, "old", cyclic, "would deadlock", execution_stopped=True)
        self.assertEqual(len(project.status(self.db)["leases"]), 1)
        project.recover(self.db, "old", replacement, "verified worker and external execution stopped; inspect partial output",
                        execution_stopped=True)
        self.assertEqual(len(self.events("recovery")), 1)
        task = project.claim(self.db, "one")
        self.assertEqual(task["id"], "investigate")
        self.assertEqual(task["recovery_of"], "old")
        self.finish(task)
        self.assertEqual(project.claim(self.db, "two")["id"], "after")

    def test_restart_after_real_process_exit_keeps_uncertain_call_quarantined(self):
        self.initialize([self.task("dispatch")])
        run = self.child("import os,json; d=p.database(sys.argv[2]); "
                         "print(json.dumps(p.claim(d,'one')),flush=True); os._exit(7)")
        self.assertEqual(run.returncode, 7, run.stderr)
        self.assertEqual(json.loads(run.stdout)["id"], "dispatch")
        self.db.close()
        self.db = project.database(self.path)
        self.addCleanup(self.db.close)
        self.assertEqual(project.reconcile(self.db), ["dispatch"])
        self.assertEqual(project.reconcile(self.db), [])
        self.assertIsNone(project.claim(self.db, "one"))
        snap = project.status(self.db)
        self.assertEqual(snap["tasks"][0]["state"], "interrupted")
        self.assertEqual(len(snap["leases"]), 1)
        self.assertEqual(snap["budget"]["calls"], 1)
        self.assertEqual(len(self.events("agent_idle")), 1)
        late = self.child("d=p.database(sys.argv[2]); p.finish(d,'dispatch',{},'completed')")
        self.assertNotEqual(late.returncode, 0)

    def test_restart_continues_next_cycle_without_repeating_completed_call(self):
        cycles = [dict(id="first", phase="one", goal_id="game", tasks=[self.task("job")]),
                  dict(id="second", phase="two", goal_id="game", tasks=[self.task("job")])]
        self.initialize(cycles=cycles)
        source = ("import json; d=p.database(sys.argv[2]); t=p.claim(d,'one'); "
                  "p.finish(d,t['id'],{'state':'tested'},'completed',claim_token=t['claim_token']); "
                  "print(json.dumps({'id':t['id'],'state':p.status(d)['state']}))")
        first = self.child(source)
        second = self.child(source)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(json.loads(first.stdout)["id"], "first/job")
        self.assertEqual(json.loads(second.stdout), {"id": "second/job", "state": "needs_planning"})
        self.assertEqual(project.status(self.db)["budget"]["calls"], 2)

    def _race(self, agents):
        gate = self.root / "go"
        source = ("import sys,time,json; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                  "import team_project as p; d=p.database(sys.argv[2]); gate=Path(sys.argv[4]); "
                  "exec('while not gate.exists(): time.sleep(0.01)'); "
                  "t=p.claim(d,sys.argv[3]); print(json.dumps(t['id'] if t else None))")
        processes = [subprocess.Popen([sys.executable, "-c", source, str(Path(project.__file__).parent),
                                       str(self.path), agent, str(gate)], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) for agent in agents]
        try:
            gate.touch()
            output = [p.communicate(timeout=20) for p in processes]
            for proc, (_, err) in zip(processes, output):
                self.assertEqual(proc.returncode, 0, err)
            return [json.loads(out) for out, _ in output]
        finally:
            for proc in processes:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()

    def test_concurrent_claims_allow_one_lease_per_worktree(self):
        self.initialize([self.task("a", "one"), self.task("b", "two")])
        results = self._race(["one", "two"])
        self.assertEqual(sum(r is not None for r in results), 1)
        self.assertEqual(len(project.status(self.db)["leases"]), 1)
        self.assertEqual(project.status(self.db)["budget"]["calls"], 1)

    def test_concurrent_claims_allow_one_lease_per_agent(self):
        self.initialize([self.task("a"), self.task("b", worktree=self.b)])
        results = self._race(["one", "one"])
        self.assertEqual(sum(r is not None for r in results), 1)
        self.assertEqual(len(project.status(self.db)["leases"]), 1)

    def test_concurrent_claims_respect_daily_budget(self):
        self.initialize([self.task("a", "one"), self.task("b", "two", self.b)], daily_limit=1)
        results = self._race(["one", "two"])
        self.assertEqual(sum(r is not None for r in results), 1)
        self.assertEqual(project.status(self.db)["budget"]["calls"], 1)

    def test_daily_budget_resets_in_utc_and_stop_blocks_planning(self):
        self.initialize([self.task("a"), self.task("b")], daily_limit=1)
        with patch.object(project.time, "time", return_value=1_700_000_000):
            self.finish(project.claim(self.db, "one"))
            self.assertIsNone(project.claim(self.db, "one"))
            self.assertEqual(project.status(self.db)["reason"], "daily_budget")
        (self.root / "STOP").touch()
        with patch.object(project.time, "time", return_value=1_700_086_400):
            self.assertIsNone(project.claim(self.db, "one"))
            self.assertEqual(project.status(self.db)["state"], "stopped")
            self.assertEqual(project.status(self.db)["budget"]["calls"], 0)
            (self.root / "STOP").unlink()
            self.assertEqual(project.claim(self.db, "one")["id"], "b")

    def test_stop_does_not_cancel_a_claim_or_lose_its_result(self):
        self.initialize(cycles=[dict(id="c1", goal_id="game", tasks=[self.task("a")]),
                                dict(id="c2", goal_id="game", tasks=[self.task("b")])])
        task = project.claim(self.db, "one")
        (self.root / "STOP").touch()
        self.finish(task)
        self.assertEqual(len(project.status(self.db)["tasks"]), 1)
        self.assertEqual(project.plan_next_cycle(self.db), [])
        self.assertEqual(project.reconcile(self.db), [])
        self.assertEqual(len(project.status(self.db)["tasks"]), 1)
        (self.root / "STOP").unlink()
        self.assertEqual(project.claim(self.db, "one")["id"], "c2/b")

    def test_no_daily_cap_keeps_accounting_beyond_twelve_and_stop(self):
        self.initialize([self.task(str(i)) for i in range(14)])
        for i in range(13):
            task = project.claim(self.db, "one")
            self.assertEqual(task["id"], str(i))
            self.finish(task)
        state = project.status(self.db)
        self.assertEqual(state["budget"]["calls"], 13)
        self.assertIsNone(state["budget"]["limit"])
        self.assertIsNone(state["budget"]["remaining"])
        self.assertEqual(state["state"], "active")
        (self.root / "STOP").touch()
        self.assertIsNone(project.claim(self.db, "one"))

    def test_remove_daily_cap_is_audited_without_resetting_calls(self):
        self.initialize([self.task("a"), self.task("b")], daily_limit=1)
        task = project.claim(self.db, "one")
        with self.assertRaises(RuntimeError):
            project.set_daily_limit(self.db, None, reason="active lease")
        self.finish(task)
        self.assertIsNone(project.claim(self.db, "one"))
        project.set_daily_limit(self.db, None, reason="User rejects arbitrary account-call ceiling")
        self.assertEqual(project.status(self.db)["budget"]["calls"], 1)
        self.assertEqual(len(self.events("daily_limit_changed")), 1)
        self.finish(project.claim(self.db, "one"), "blocked", {"error": "account quota"})
        self.assertEqual(project.status(self.db)["reason"], "execution_failure")
        self.assertIsNone(project.claim(self.db, "one"))

    def test_invalid_policy_change_preserves_previous_policy(self):
        self.initialize(daily_limit=12)
        for bad in (0, -1, True, 1.5, "none"):
            with self.assertRaises(ValueError):
                project.set_daily_limit(self.db, bad, reason="invalid")
        with self.assertRaises(ValueError):
            project.set_daily_limit(self.db, None, reason="")
        self.assertEqual(project.status(self.db)["budget"]["limit"], 12)

    def test_validation_rollback_and_initialize_idempotence(self):
        with self.assertRaises(ValueError):
            self.initialize(daily_limit=0)
        with self.assertRaises(ValueError):
            self.initialize([self.task("bad", after=["missing"])])
        self.assertEqual(self.db.execute("SELECT count(*) FROM goals").fetchone()[0], 0)
        initial = [self.task("initial")]
        self.initialize(initial)
        self.initialize(initial)
        self.assertEqual(len(project.status(self.db)["tasks"]), 1)
        with self.assertRaises(ValueError):
            self.initialize([self.task("changed")])
        with self.assertRaises(ValueError):
            project.enqueue(self.db, [self.task("a", after=["b"]), self.task("b", after=["a"])])
        self.assertEqual(len(project.status(self.db)["tasks"]), 1)

    def test_finish_checks_owner_token_and_terminal_state(self):
        self.initialize([self.task("one")])
        task = project.claim(self.db, "one")
        with self.assertRaises(RuntimeError):
            project.finish(self.db, task["id"], {}, "completed", claim_token="wrong")
        self.finish(task)
        with self.assertRaises(RuntimeError):
            self.finish(task)
        self.assertEqual(len(self.events("agent_idle")), 1)

    def test_recovery_chain_cannot_depend_on_its_own_ancestor(self):
        self.initialize([self.task("original")])
        self.finish(project.claim(self.db, "one"), "failed")
        project.recover(self.db, "original", self.task("repair"), "inspect failure", execution_stopped=True)
        self.finish(project.claim(self.db, "one"), "failed")
        with self.assertRaises(ValueError):
            project.recover(self.db, "repair", self.task("second", after=["original"]),
                            "would create a resolution deadlock", execution_stopped=True)
        self.assertEqual(len(project.status(self.db)["tasks"]), 2)

    def test_recovery_cannot_steal_a_future_template_id(self):
        self.initialize(cycles=[dict(id="c1", goal_id="game", tasks=[self.task("a")]),
                                dict(id="c2", goal_id="game", tasks=[self.task("b")])])
        self.finish(project.claim(self.db, "one"), "failed")
        with self.assertRaises(ValueError):
            project.recover(self.db, "c1/a", self.task("c2/b"), "id reserved in backlog", execution_stopped=True)
        self.assertEqual(len(project.status(self.db)["tasks"]), 1)

    def test_invalid_future_cycle_dependencies_are_rejected_atomically(self):
        with self.assertRaises(ValueError):
            self.initialize(cycles=[dict(id="c1", goal_id="game", tasks=[self.task("a", after=["c2/b"])]),
                                    dict(id="c2", goal_id="game", tasks=[self.task("b")])])
        self.assertEqual(self.db.execute("SELECT count(*) FROM cycles").fetchone()[0], 0)
        self.initialize(cycles=[dict(id="c1", goal_id="game", tasks=[self.task("a")])])
        with self.assertRaises(ValueError):
            project.enqueue(self.db, [self.task("outside", after=["c1/a"])])
        with self.assertRaises(ValueError):
            project.enqueue(self.db, [self.task("outside", cycle="c1")])
        self.assertEqual(len(project.status(self.db)["tasks"]), 0)


if __name__ == "__main__":
    unittest.main()
