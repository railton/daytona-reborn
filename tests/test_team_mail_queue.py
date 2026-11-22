from contextlib import closing
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import team_mail_queue as queue
import team_project as project


class MailQueueTest(unittest.TestCase):
    def exercise(self, outcome, actor='grok_bot'):
        with tempfile.TemporaryDirectory(prefix='daytona-mail-queue-') as temporary:
            root = Path(temporary).resolve()
            with closing(project.database(root / 'project.sqlite')) as db:
                tasks = [dict(id=name, agent=actor, worktree=str(root), prompt='synthetic review',
                              payload=dict(kind='mailbox', title=name), after=after, timeout=5)
                         for name, after in [('first', []), ('next', ['first'])]]
                project.initialize(db, [dict(id='test', description='serial mailbox proof')], tasks=tasks)
                calls = []
                status_reads = 0

                def fake(sender, operation, args):
                    nonlocal status_reads
                    if operation == 'task_create':
                        if calls:
                            first = db.execute("select state from tasks where id='first'").fetchone()[0]
                            self.assertEqual(first, 'completed')
                        self.assertEqual(args['assigned_to'], actor)
                        name = args['title']
                        calls.append(name)
                        if outcome == 'uncertain':
                            raise RuntimeError('transport closed during submission')
                        return dict(task_id=name, message_id='message-' + name)
                    status_reads += 1
                    if outcome == 'defer' and status_reads == 2:
                        return {'tasks': [dict(id='manual', assigned_to=actor, state='running')], 'messages': []}
                    if outcome == 'transient' and status_reads == 3:
                        raise sqlite3.OperationalError('database is locked')
                    state = 'blocked' if outcome == 'blocked' else 'done'
                    records = [dict(id=calls[-1], assigned_to=actor, state=state, result='fixture result')] if calls else []
                    return {'tasks': records, 'messages': []}

                result = queue.run(db, fake, lambda: True, 2, poll_interval=0, actor=actor)
                states = [row[0] for row in db.execute('select state from tasks order by seq')]
                leases = db.execute('select count(*) from leases').fetchone()[0]
                budget = db.execute('select coalesce(sum(calls),0) from budget').fetchone()[0]
                return result, calls, states, leases, budget

    def test_successor_runs_after_delivery(self):
        self.assertEqual(self.exercise('transient', actor='grok_build'),
                         (2, ['first', 'next'], ['completed', 'completed'], 0, 2))

    def test_deepseek_mailbox_and_acp_use_distinct_reservations(self):
        with tempfile.TemporaryDirectory(prefix='daytona-transport-') as temporary:
            root = Path(temporary).resolve()
            with closing(project.database(root / 'project.sqlite')) as db:
                tasks = [
                    dict(id=name, agent='deepseek', worktree=str(root), prompt='test transport',
                         payload=dict(kind=kind, title=name))
                    for name, kind in [('code-first', 'code'), ('mail-second', 'mailbox')]
                ]
                project.initialize(db, [dict(id='test', description='transport selection')], tasks=tasks)
                deliveries = []

                def invoke(sender, operation, args):
                    if operation == 'task_create':
                        deliveries.append(args['title'])
                        self.assertEqual(args['assigned_to'], 'deepseek')
                        self.assertIsNone(project.claim(db, 'deepseek', mailbox_task=False))
                        return dict(task_id='mail', message_id='message-mail')
                    records = [dict(id='mail', assigned_to='deepseek', state='done',
                                    result='fixture result')] if deliveries else []
                    return dict(tasks=records, messages=[])

                self.assertEqual(queue.run(db, invoke, lambda: True, 1, poll_interval=0,
                                           actor='deepseek'), 1)
                self.assertEqual(deliveries, ['mail-second'])
                self.assertEqual(db.execute('select sum(calls) from budget').fetchone()[0], 1)
                self.assertFalse(db.execute('select 1 from leases').fetchone())
                code = project.claim(db, 'deepseek', mailbox_task=False)
                self.assertEqual(code['id'], 'code-first')
                project.finish(db, code['id'], {'state': 'tested'}, 'completed',
                               claim_token=code['claim_token'])

    def test_known_failure_does_not_start_successor(self):
        self.assertEqual(self.exercise('blocked'), (0, ['first'], ['blocked', 'blocked'], 0, 1))

    def test_uncertain_submission_keeps_lease(self):
        self.assertEqual(self.exercise('uncertain'), (0, ['first'], ['interrupted', 'blocked'], 1, 1))

    def test_busy_mailbox_defers_without_submitting(self):
        self.assertEqual(self.exercise('defer'), (0, [], ['pending', 'pending'], 0, 0))


    def test_late_result_releases_exact_reservation_and_runs_successor(self):
        actor = 'grok_bot'
        with tempfile.TemporaryDirectory(prefix='daytona-late-mail-') as temporary:
            root = Path(temporary)
            with closing(project.database(root / 'project.sqlite')) as db:
                project.initialize(db, [dict(id='test', description='late delivery')],
                                   tasks=[dict(id=name, agent=actor, worktree=str(root),
                                               prompt='synthetic result', after=after,
                                               payload=dict(kind='mailbox', title=name))
                                          for name, after in [('first', []), ('next', ['first'])]])
                calls, records = [], []

                def invoke(sender, operation, args):
                    if operation == 'task_create':
                        name = args['title']
                        calls.append(name)
                        return dict(task_id=name, message_id='message-' + name)
                    return dict(tasks=records, messages=[])

                with patch.object(queue, 'wait_for_result', side_effect=TimeoutError('late')):
                    self.assertEqual(queue.run(db, invoke, lambda: True, 2), 0)
                self.assertEqual(calls, ['first'])

                returned = dict(id='first', message_id='message-first', assigned_to=actor,
                                owner=actor, state='done', result='late fixture result')
                for incorrect in (dict(message_id='other'), dict(owner='grok_build'),
                                  dict(assigned_to='grok_build'), dict(state='review'),
                                  dict(result='')):
                    with self.subTest(incorrect=incorrect):
                        records[:] = [{**returned, **incorrect}]
                        self.assertEqual(queue.run(db, invoke, lambda: True, 2), 0)
                        self.assertEqual(calls, ['first'])
                        self.assertEqual(db.execute('select task from leases').fetchone()[0], 'first')

                records[:] = [returned]
                (root / 'STOP').touch()
                self.assertEqual(queue.run(db, invoke, lambda: True, 2), 0)
                self.assertEqual(db.execute('select task from leases').fetchone()[0], 'first')
                (root / 'STOP').unlink()

                with patch.object(queue, 'wait_for_result', return_value=('done', 'next fixture result')):
                    self.assertEqual(queue.run(db, invoke, lambda: True, 2), 1)
                self.assertEqual(calls, ['first', 'next'])
                rows = db.execute('select state, result from tasks order by seq').fetchall()
                self.assertEqual([row['state'] for row in rows], ['completed', 'completed'])
                self.assertIn('late fixture result', rows[0]['result'])
                self.assertIn('interruption', rows[0]['result'])
                self.assertFalse(db.execute('select 1 from leases').fetchone())
                self.assertEqual(db.execute('select sum(calls) from budget').fetchone()[0], 2)
                self.assertEqual(db.execute("select task from events where event='mailbox_late_result'").fetchone()[0], 'first')


if __name__ == '__main__':
    unittest.main()
