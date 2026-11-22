"""Dispatch prepared agent tasks serially through the shared mailbox."""

import argparse
from datetime import datetime, timezone
import json
import os
import sqlite3
from pathlib import Path
import sys
import time

import team_project as project


MAIL_ACTORS = ('grok_bot', 'grok_build', 'deepseek')
ACTIVE = {'pending', 'running', 'assigned'}


def mailbox_active(status, actor):
    return any(task['assigned_to'] == actor and task['state'] in ACTIVE
               for task in status['tasks'])


def wait_for_result(invoke, receipt, deadline, poll_interval):
    while time.monotonic() < deadline:
        try:
            status = invoke('codex', 'mailbox_status', {})
        except sqlite3.OperationalError as exc:
            if not any(word in str(exc).lower() for word in ('locked', 'busy')):
                raise
            time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
            continue
        for task in status['tasks']:
            if task['id'] == receipt['task_id'] and task['state'] in {'done', 'review', 'blocked'}:
                return task['state'], task.get('result')
        for message in status['messages']:
            if message['id'] == receipt['message_id'] and message['state'] == 'blocked':
                raise RuntimeError(message.get('error') or 'mailbox delivery blocked')
        time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
    raise TimeoutError('mailbox result uncertain at the task deadline')


def defer_unsubmitted(db, task, reason):
    with project._transaction(db):
        claim = db.execute(
            "select started from tasks where id=? and state='running' and owner=? and claim_token=?",
            (task['id'], os.getpid(), task['claim_token']),
        ).fetchone()
        if claim is None:
            raise RuntimeError('mailbox reservation has changed')
        if db.execute("select 1 from events where task=? and event='mailbox_dispatch_attempt'",
                      (task['id'],)).fetchone():
            raise RuntimeError('attempted mailbox dispatch must be reconciled')
        db.execute("update tasks set state='pending',owner=NULL,owner_agent=NULL,claim_token=NULL,started=NULL where id=?",
                   (task['id'],))
        db.execute('delete from leases where task=?', (task['id'],))
        day = datetime.fromtimestamp(claim['started'], timezone.utc).date().isoformat()
        db.execute('update budget set calls=calls-1 where day=? and calls>0', (day,))
        project._event(db, task['id'], 'mailbox_deferred', {'reason': reason})


def reconcile_late_result(db, status, actor):
    """Settle only a finished external task with an exact journaled receipt."""
    finished = {task['id']: task for task in status['tasks']
                if task['assigned_to'] == actor and task['state'] == 'done'
                and task.get('owner') == actor and task.get('result')}
    if not finished:
        return
    with project._transaction(db):
        if Path(project._meta(db)['stop_file']).exists():
            return
        entry = db.execute(
            "select t.id, t.result, e.detail from tasks t "
            "join leases l on l.task=t.id and l.agent=t.owner_agent and l.worktree=t.worktree "
            "join events e on e.task=t.id and e.event='mailbox_dispatched' "
            "where t.state='interrupted' and t.resolved_by is null and t.owner_agent=? "
            "order by e.seq desc limit 1", (actor,),
        ).fetchone()
        if entry is None:
            return
        receipt = json.loads(entry['detail'])
        reply = finished.get(receipt['task_id'])
        if reply is None or reply.get('message_id') != receipt['message_id']:
            return
        result = dict(receipt=receipt, state='done', summary=reply['result'],
                      interruption=json.loads(entry['result'] or '{}'))
        db.execute("update tasks set state='completed',result=?,ended=? where id=?",
                   (project._json(result), time.time(), entry['id']))
        db.execute('delete from leases where task=?', (entry['id'],))
        project._event(db, entry['id'], 'mailbox_late_result', receipt)
        project._reconcile_dependencies(db)


def run(db, invoke, enabled, max_tasks, poll_interval=3, actor='grok_bot'):
    if actor not in MAIL_ACTORS:
        raise ValueError('unsupported mailbox actor')
    completed = 0
    for _ in range(max_tasks):
        if not enabled():
            break
        status = invoke('codex', 'mailbox_status', {})
        reconcile_late_result(db, status, actor)
        if mailbox_active(status, actor):
            break
        task = project.claim(db, actor, isolate_failures=True, mailbox_task=True)
        if task is None:
            break
        result = {}
        state = 'blocked'
        attempted = False
        try:
            spec = task['payload']
            if spec.get('kind') != 'mailbox':
                raise ValueError('invalid mailbox task payload')
            if not enabled() or mailbox_active(invoke('codex', 'mailbox_status', {}), actor):
                defer_unsubmitted(db, task, 'paused or another mailbox task active')
                break
            deadline = time.monotonic() + task['timeout']
            with project._transaction(db):
                project._event(db, task['id'], 'mailbox_dispatch_attempt', {'actor': actor})
            attempted = True
            receipt = invoke('codex', 'task_create', {
                'title': spec['title'], 'description': task['prompt'], 'assigned_to': actor,
            })
            result['receipt'] = receipt
            with project._transaction(db):
                project._event(db, task['id'], 'mailbox_dispatched', receipt)
            print(json.dumps({'event': 'mailbox_dispatched', 'task': task['id'], **receipt}), flush=True)
            mail_state, answer = wait_for_result(invoke, receipt, deadline, poll_interval)
            result.update(state=mail_state, summary=answer)
            if mail_state in {'done', 'review'} and answer:
                state = 'completed'
            elif not answer:
                result['error'] = 'mailbox task settled without a result'
        except Exception as exc:
            result['error'] = str(exc)
            if attempted:
                state = 'interrupted'
        project.finish(db, task['id'], result, state, claim_token=task['claim_token'])
        print(json.dumps({'event': 'mailbox_task_finished', 'task': task['id'], 'state': state}), flush=True)
        if state != 'completed':
            break
        completed += 1
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database', type=Path)
    parser.add_argument('mailbox', type=Path)
    parser.add_argument('--max-tasks', type=int, required=True)
    parser.add_argument('--actor', choices=MAIL_ACTORS, default='grok_bot')
    args = parser.parse_args()
    if args.max_tasks < 1:
        parser.error('--max-tasks must be positive')
    directory = args.mailbox.resolve()
    sys.path.insert(0, str(directory))
    from mailbox import invoke

    def enabled():
        if (directory / 'STOP').exists():
            return False
        config = json.loads((directory / 'config.json').read_text(encoding='utf-8-sig'))
        actor_config = config.get('actors', {}).get(args.actor, {})
        return config.get('enabled') is True and actor_config.get('enabled') is True

    with project.database(args.database) as db:
        project.reconcile(db)
        run(db, invoke, enabled, args.max_tasks, actor=args.actor)


if __name__ == '__main__':
    main()
