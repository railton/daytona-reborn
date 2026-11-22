"""Protocol and persistence tests use fake local processes, never paid calls."""
import contextlib
import io
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from team_rpc import JsonLines, RpcError, acp_turn, codex_turn
import team_queue as tq

FAKE = r'''
import json,sys,time
mode=sys.argv[1]
if mode=='no-read': time.sleep(20)
def send(v): print(json.dumps(v),flush=True)
for line in sys.stdin:
 m=json.loads(line)
 if 'method' not in m: continue
 rid=m['id']
 if mode=='silent': time.sleep(20)
 elif mode=='collision':
  send({'id':rid,'method':'session/request_permission','params':{}})
  answer=json.loads(sys.stdin.readline())
  send({'id':rid,'result':answer['result']})
 elif mode=='stderr':
  sys.stderr.write('x'*200000+'\n');sys.stderr.flush()
  send({'id':rid,'result':{'ok':True}})
 elif mode in ['acp','cancelled']:
  send({'method':'session/update','params':{'sessionId':'s','update':{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'peer answer'}}}})
  send({'id':rid,'result':{'stopReason':'end_turn' if mode=='acp' else 'cancelled'}})
 elif mode=='codex':
  send({'method':'item/completed','params':{'threadId':'th','turnId':'tu','item':{'type':'agentMessage','text':'review complete'}}})
  send({'method':'turn/completed','params':{'threadId':'th','turn':{'id':'tu','status':'completed'}}})
  send({'id':rid,'result':{'turn':{'id':'tu'}}})
 elif mode=='child':
  import subprocess
  child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'])
  send({'id':rid,'result':{'pid':child.pid}})
  time.sleep(30)
'''


class TransportTests(unittest.TestCase):
    def rpc(self, mode):
        return JsonLines([sys.executable, "-u", "-c", FAKE, mode], Path.cwd())

    def test_server_request_id_collision_is_denied_then_response_read(self):
        with self.rpc("collision") as rpc:
            result = rpc.request("initialize", {}, time.monotonic() + 5)
            self.assertEqual(result, {"outcome": {"outcome": "cancelled"}})

    def test_stderr_is_drained(self):
        with self.rpc("stderr") as rpc:
            self.assertTrue(rpc.request("initialize", {}, time.monotonic() + 5)["ok"])

    def test_acp_requires_explicit_completion(self):
        with self.rpc("acp") as rpc:
            self.assertEqual(acp_turn(rpc, "s", "prompt", time.monotonic() + 5), "peer answer")
        with self.rpc("cancelled") as rpc:
            with self.assertRaises(RpcError):
                acp_turn(rpc, "s", "prompt", time.monotonic() + 5)

    def test_codex_completion_before_response(self):
        with self.rpc("codex") as rpc:
            self.assertEqual(codex_turn(rpc, "th", "prompt", time.monotonic() + 5), "review complete")

    def test_deadline_and_process_cleanup(self):
        with self.rpc("silent") as rpc:
            proc = rpc.proc
            with self.assertRaises(TimeoutError):
                rpc.request("initialize", {}, time.monotonic() + 0.1)
        self.assertIsNotNone(proc.poll())

    def test_backend_not_reading_stdin_cannot_block_worker(self):
        before = time.monotonic()
        with self.rpc("no-read") as rpc:
            proc = rpc.proc
            with self.assertRaises(TimeoutError):
                rpc.request("initialize", {"text": "x" * 200_000}, time.monotonic() + 0.2)
        self.assertIsNotNone(proc.poll())
        self.assertLess(time.monotonic() - before, 8)

    @unittest.skipUnless(os.name == "nt", "Windows Job Object")
    def test_job_object_terminates_owned_descendant(self):
        with self.rpc("child") as rpc:
            pid = rpc.request("initialize", {}, time.monotonic() + 5)["pid"]
            self.assertTrue(tq.process_alive(pid))
        self.assertFalse(tq.process_alive(pid))


class QueueTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name)
        self.db = tq.database(self.path / "queue.sqlite")

    def tearDown(self):
        self.db.close()
        self.tmp.cleanup()

    def manifest(self):
        return {"max_submissions": 2, "tasks": [
            {"id": "a", "backend": "peer", "cwd": str(self.path), "prompt": "implement"},
            {"id": "b", "backend": "peer", "cwd": str(self.path), "prompt": "review", "after": ["a"]}]}

    def test_init_is_idempotent_and_rejects_changed_manifest(self):
        m = self.manifest(); tq.init(self.db, m); tq.init(self.db, m)
        self.assertEqual(self.db.execute("SELECT count(*) FROM tasks").fetchone()[0], 2)
        m["tasks"][0]["prompt"] = "different"
        with self.assertRaises(ValueError): tq.init(self.db, m)

    def test_invalid_dependency_and_cycle_rejected(self):
        m = self.manifest(); m["tasks"][0]["after"] = ["b"]
        with self.assertRaises(ValueError): tq.init(self.db, m)

    def test_claim_is_exclusive_and_results_chain_without_repetition(self):
        tq.init(self.db, self.manifest())
        self.assertEqual(tq.claim(self.db)["id"], "a")
        other = tq.database(self.path / "queue.sqlite")
        try: self.assertIsNone(tq.claim(other))
        finally: other.close()
        tq.finish(self.db, "a", answer="patch")
        self.assertEqual(tq.claim(self.db)["id"], "b")
        tq.finish(self.db, "b", answer="review")
        self.assertIsNone(tq.claim(self.db))
        self.assertEqual(self.db.execute("SELECT value FROM meta WHERE key='submissions'").fetchone()[0], "2")

    def test_failure_circuit_breaker(self):
        tq.init(self.db, self.manifest()); tq.claim(self.db)
        tq.finish(self.db, "a", error="quota")
        self.assertIsNone(tq.claim(self.db))
        self.assertFalse(tq.run(self.db, {}, 2, 60, self.path / "STOP"))

    def test_backend_and_stop_path_are_pinned(self):
        tq.init(self.db, self.manifest()); (self.path / "STOP").touch()
        tq.run(self.db, {"peer": {"billing": "existing-account"}}, 2, 60, self.path / "STOP")
        with self.assertRaises(ValueError):
            tq.run(self.db, {"peer": {"billing": "different"}}, 2, 60, self.path / "STOP")
        with self.assertRaises(ValueError):
            tq.run(self.db, {"peer": {"billing": "existing-account"}}, 2, 60, self.path / "different-stop")

    def test_interrupted_worker_never_resubmitted(self):
        tq.init(self.db, self.manifest()); tq.claim(self.db)
        with mock.patch.object(tq, "process_alive", return_value=False): tq.reconcile(self.db)
        self.assertEqual(self.db.execute("SELECT state FROM tasks WHERE id='a'").fetchone()[0], "interrupted")
        self.assertIsNone(tq.claim(self.db))

    def test_stop_file_prevents_submission(self):
        tq.init(self.db, self.manifest()); (self.path / "STOP").touch()
        tq.run(self.db, {}, 2, 60, self.path / "STOP")
        self.assertEqual(self.db.execute("SELECT value FROM meta WHERE key='submissions'").fetchone()[0], "0")

    def test_real_worker_crash_after_claim_is_not_redispatched(self):
        tq.init(self.db, self.manifest())
        script = ("import sys,os; sys.path.insert(0,sys.argv[1]); import team_queue as q; "
                  "db=q.database(sys.argv[2]); assert q.claim(db)['id']=='a'; os._exit(23)")
        result = subprocess.run([sys.executable, "-c", script,
            str(Path(tq.__file__).parent), str(self.path / "queue.sqlite")], timeout=10)
        self.assertEqual(result.returncode, 23)
        with mock.patch.object(tq, "execute") as dispatch:
            self.assertFalse(tq.run(self.db, {}, 2, 10, self.path / "STOP"))
            dispatch.assert_not_called()
        self.assertEqual(self.db.execute("SELECT state FROM tasks WHERE id='a'").fetchone()[0], "interrupted")
        self.assertEqual(self.db.execute("SELECT value FROM meta WHERE key='submissions'").fetchone()[0], "1")

    def test_worker_passes_prior_result_and_stops_when_complete(self):
        tq.init(self.db, self.manifest()); prompts = []
        def fake(task, cfg, prompt, deadline):
            prompts.append(prompt); return "proposal-" + task["id"], "session", []
        with mock.patch.object(tq, "execute", side_effect=fake), contextlib.redirect_stdout(io.StringIO()):
            tq.run(self.db, {"peer": {}}, 2, 60, self.path / "STOP")
            tq.run(self.db, {"peer": {}}, 2, 60, self.path / "STOP")
        self.assertEqual(len(prompts), 2)
        self.assertIn("proposal-a", prompts[1])


if __name__ == "__main__":
    unittest.main()
