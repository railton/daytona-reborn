import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import team_project as project
import team_worker as worker


class WorkerTests(unittest.TestCase):
    def test_completed_cycle_automatically_materializes_next_after_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); path=root/'state.sqlite'; calls=[]
            def task(name,agent,after=()):
                return dict(id=name,agent=agent,worktree=str(root),prompt='code or review',
                    after=list(after),payload=dict(cwd=str(root),backend=agent,kind=name))
            cycles=[dict(id='one',goal_id='game',phase='prototype',tasks=[task('code','deepseek'),task('review','grok',['code'])]),
                    dict(id='two',goal_id='game',phase='next feature',tasks=[task('code','grok'),task('review','deepseek',['code'])])]
            def perform(spec,backend,out):
                calls.append((spec['backend'],spec['kind']))
                live = json.loads((root/'team-status.json').read_text())
                self.assertEqual(live['agents'][spec['backend']]['state'], 'working')
                self.assertIsNotNone(live['agents'][spec['backend']]['task'])
                return dict(state='tested' if spec['kind']=='code' else 'reviewed',session='fake',
                            packet=dict(idle=True,summary='local fixture'))
            with contextlib.closing(project.database(path)) as db:
                project.initialize(db,[dict(id='game',description='finish game')],cycles=cycles)
                with mock.patch.object(worker,'perform',side_effect=perform),contextlib.redirect_stdout(io.StringIO()):
                    first=worker.run(db,{'deepseek':{},'grok':{}},root,['deepseek','grok'],2,30,origin='scheduled')
                self.assertEqual(len(calls),2)
                self.assertTrue(any(t['id']=='two/code' and t['state']=='pending' for t in first['tasks']))
            # Different connection/worker run, durable project persists.
            with contextlib.closing(project.database(path)) as db:
                with mock.patch.object(worker,'perform',side_effect=perform),contextlib.redirect_stdout(io.StringIO()):
                    last=worker.run(db,{'deepseek':{},'grok':{}},root,['deepseek','grok'],2,30)
                    worker.run(db,{'deepseek':{},'grok':{}},root,['deepseek','grok'],2,30)
                self.assertEqual(len(calls),4)
                self.assertEqual(last['state'],'needs_planning')
                self.assertIsNone(last['acceptance'])
                self.assertEqual(db.execute("select count(*) from events where event='agent_idle'").fetchone()[0],4)
                wakes = [json.loads(row[0]) for row in db.execute(
                    "select detail from events where event='worker_started' order by rowid")]
                self.assertEqual([wake['origin'] for wake in wakes],
                    ['scheduled', 'interactive', 'interactive'])
                rests = [json.loads(row[0]) for row in db.execute(
                    "select detail from events where event='worker_idle' order by rowid")]
                self.assertEqual(len(rests), len(wakes))
                self.assertEqual(rests[-1]['state'], 'needs_planning')
                idle = json.loads((root/'team-status.json').read_text())
                self.assertTrue(all(a['state']=='waiting' and a['reason']=='open_goals'
                    for a in idle['agents'].values()))

    def test_acp_worker_leaves_mailbox_task_pending(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tasks = [
                dict(id=name, agent='deepseek', worktree=str(root), prompt='test transport',
                     payload=dict(cwd=str(root), backend='deepseek', kind=kind))
                for name, kind in [('mail-first', 'mailbox'), ('code-second', 'code')]
            ]
            with contextlib.closing(project.database(root / 'db')) as db:
                project.initialize(db, [dict(id='game', description='transport selection')], tasks)
                result = dict(state='tested', session='fake', packet=dict(idle=True, summary='tested'))
                with mock.patch.object(worker, 'perform', return_value=result) as perform:
                    with contextlib.redirect_stdout(io.StringIO()):
                        worker.run(db, {'deepseek': {}}, root, ['deepseek'], 2, 30)
                self.assertEqual(perform.call_count, 1)
                self.assertEqual(perform.call_args.args[0]['kind'], 'code')
                states = [row[0] for row in db.execute('select state from tasks order by seq')]
                self.assertEqual(states, ['pending', 'completed'])
                self.assertEqual(db.execute('select sum(calls) from budget').fetchone()[0], 1)
                self.assertFalse(db.execute('select 1 from leases').fetchone())

    def test_review_objection_blocks_next_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            with contextlib.closing(project.database(root/'db')) as db:
                project.initialize(db,[dict(id='game',description='finish')],[dict(id='review',agent='grok',worktree=str(root),prompt='review',payload=dict(cwd=str(root),backend='grok'))])
                result=dict(state='changes_requested',session='fake',packet=dict(idle=True,summary='counterexample'))
                with mock.patch.object(worker,'perform',return_value=result),contextlib.redirect_stdout(io.StringIO()):
                    final=worker.run(db,{'grok':{}},root,['grok'],2,30)
                self.assertEqual(final['state'],'blocked')


if __name__=='__main__': unittest.main()
