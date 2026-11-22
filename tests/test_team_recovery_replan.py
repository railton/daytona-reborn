"""Regression: attributed local repair unblocks its review before the next claim."""
import json
from pathlib import Path
import shutil
import sys
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import team_project as project


class LocalRepairReplanTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[1] / "build-team-project-tests"
        scratch.mkdir(exist_ok=True)
        self.root = scratch / ("local-repair-replan-" + uuid.uuid4().hex)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.a, self.b = self.root / "a", self.root / "b"
        self.a.mkdir()
        self.b.mkdir()
        self.path = self.root / "project.sqlite"
        self.db = project.database(self.path)
        self.addCleanup(self.db.close)
        self.goals = [{"id": "game", "description": "Evidence-validated game"}]

    def task(self, ident, agent="one", worktree=None, **extra):
        return dict(id=ident, agent=agent, worktree=str(worktree or self.a),
                    prompt="Perform task " + ident, **extra)

    def by_id(self, ident):
        for task in project.status(self.db)["tasks"]:
            if task["id"] == ident:
                return task
        self.fail("missing task " + ident)

    def events(self, name):
        return [dict(row) for row in self.db.execute(
            "SELECT * FROM events WHERE event=?", (name,))]

    def evidence(self):
        return dict(
            validator="local coordinator",
            hashes={"src/game.py": "sha256:" + "a" * 64},
            tests=[dict(argv=["python", "-m", "unittest"], exit=0)],
        )

    def fail_code_and_block_review(self):
        project.initialize(
            self.db, self.goals,
            [self.task("code", payload={"kind": "code"}),
             self.task("review", agent="reviewer", worktree=self.b, after=["code"])],
            daily_limit=2,
        )
        claimed = project.claim(self.db, "one")
        self.assertEqual(claimed["id"], "code")
        project.finish(self.db, claimed["id"], {"state": "tests_failed"}, "blocked",
                       claim_token=claimed["claim_token"])
        snapshot = project.status(self.db)
        self.assertEqual(snapshot["state"], "blocked")
        self.assertEqual(snapshot["reason"], "execution_failure")
        self.assertEqual(self.by_id("review")["state"], "blocked")
        self.assertEqual(self.by_id("review")["blocked_reason"], "dependency")
        self.assertEqual(snapshot["budget"]["calls"], 1)

    def coordinator_repair(self, evidence):
        return project.recover(
            self.db, "code", self.task("local-fix", agent="coordinator"),
            "attributed local code repair after stopped failed execution",
            execution_stopped=True, local_evidence=evidence,
        )

    def test_local_repair_reconciles_review_before_any_claim(self):
        self.fail_code_and_block_review()
        evidence = self.evidence()
        self.assertEqual(self.coordinator_repair(evidence), "local-fix")

        snapshot = project.status(self.db)
        self.assertEqual(snapshot["state"], "active")
        self.assertIsNone(snapshot["reason"])
        review = self.by_id("review")
        self.assertEqual(review["state"], "pending")
        self.assertIsNone(review["blocked_reason"])
        self.assertEqual(review["after"], ["code"])
        code = self.by_id("code")
        self.assertEqual(code["state"], "blocked")
        self.assertEqual(code["result"]["state"], "tests_failed")
        self.assertEqual(code["resolved_by"], "local-fix")
        fix = self.by_id("local-fix")
        self.assertEqual(fix["state"], "completed")
        self.assertEqual(fix["recovery_of"], "code")
        self.assertEqual(fix["result"], {"state": "local_verified", "evidence": evidence})
        self.assertEqual(snapshot["budget"]["calls"], 1)
        self.assertEqual(snapshot["leases"], [])
        verified = self.events("local_repair_verified")
        self.assertEqual(len(verified), 1)
        self.assertEqual(json.loads(verified[0]["detail"]), evidence)

        review_claim = project.claim(self.db, "reviewer")
        self.assertEqual(review_claim["id"], "review")
        self.assertEqual(review_claim["after"], ["code"])
        self.assertTrue(review_claim["claim_token"])
        self.assertEqual(project.status(self.db)["budget"]["calls"], 2)
        self.assertEqual(project.status(self.db)["leases"][0]["task"], "review")

    def test_stop_prevents_immediate_replanning_until_removed(self):
        self.fail_code_and_block_review()
        (self.root / "STOP").touch()
        self.assertEqual(self.coordinator_repair(self.evidence()), "local-fix")

        snapshot = project.status(self.db)
        self.assertEqual(snapshot["state"], "stopped")
        self.assertEqual(snapshot["reason"], "STOP")
        review = self.by_id("review")
        self.assertEqual(review["state"], "blocked")
        self.assertEqual(review["blocked_reason"], "dependency")
        self.assertIsNone(project.claim(self.db, "reviewer"))
        self.assertEqual(project.plan_next_cycle(self.db), [])
        self.assertEqual(snapshot["budget"]["calls"], 1)
        self.assertEqual(len(snapshot["tasks"]), 3)
        self.assertEqual(self.by_id("local-fix")["state"], "completed")

        (self.root / "STOP").unlink()
        before_read = self.db.total_changes
        resumed = project.status(self.db)
        self.assertEqual(resumed["state"], "active")
        self.assertEqual(self.by_id("review")["state"], "pending")
        self.assertEqual(self.db.total_changes, before_read, "status remains read-only")
        self.assertEqual(resumed["budget"]["calls"], 1)
        self.assertEqual(project.claim(self.db, "reviewer")["id"], "review")
        self.assertEqual(project.status(self.db)["budget"]["calls"], 2)

    def test_local_repair_clears_transitive_blocks_independent_of_row_order(self):
        for reverse in (False, True):
            with self.subTest(reverse=reverse):
                path = self.root / ("chain-reverse.sqlite" if reverse else "chain.sqlite")
                db = project.database(path)
                try:
                    tasks = [self.task("code", payload={"kind": "code"}),
                             self.task("review", agent="reviewer", worktree=self.b, after=["code"]),
                             self.task("audit", after=["review"]),
                             self.task("publish", after=["audit"])]
                    project.initialize(db, self.goals, list(reversed(tasks)) if reverse else tasks)
                    claimed = project.claim(db, "one")
                    self.assertEqual(claimed["id"], "code")
                    project.finish(db, "code", {"state": "tests_failed"}, "blocked",
                                   claim_token=claimed["claim_token"])
                    failed = {r["id"]: r for r in project.status(db)["tasks"]}
                    for ident in ("review", "audit", "publish"):
                        self.assertEqual(failed[ident]["blocked_reason"], "dependency")
                    project.recover(db, "code", self.task("fix", agent="coordinator"),
                                    "local verified chain repair", execution_stopped=True,
                                    local_evidence=self.evidence())
                    # Inspect persisted rows before any claim/status can heal them.
                    for ident in ("review", "audit", "publish"):
                        row = db.execute("SELECT state,blocked_reason FROM tasks WHERE id=?", (ident,)).fetchone()
                        self.assertEqual(tuple(row), ("pending", None))
                    self.assertEqual(project.status(db)["state"], "active")
                    self.assertIsNone(project.claim(db, "one"), "later dependants still wait for review completion")
                finally:
                    db.close()


if __name__ == "__main__":
    unittest.main()
