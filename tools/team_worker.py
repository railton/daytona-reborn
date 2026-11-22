'''Bounded project worker. Finishing a cycle plans the next, not project success.'''
import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import time
from datetime import datetime, timedelta, timezone

import team_project as project
from team_develop import perform


def _is_wildcard_task(task):
    '''Match the exact wildcard accepted by team_project.claim().'''
    return task.get('agent') == '*'


def _task_owner(task):
    '''Running work is attributed to the worker that claimed it.'''
    owner = task.get('owner_agent')
    if owner:
        return owner
    if _is_wildcard_task(task):
        return None
    return task.get('agent')


def _is_waiting_for(task, agent):
    '''Pending wildcard work is visible to every configured worker.'''
    if task.get('owner_agent'):
        return task['owner_agent'] == agent
    if _is_wildcard_task(task):
        return True
    return _task_owner(task) == agent


def write_live_status(db, artifacts, agents):
    '''Small advisory snapshot; SQLite remains authoritative across workers.'''
    snapshot = project.status(db)
    states = {}
    for agent in agents:
        running = next((t for t in snapshot['tasks']
                        if t.get('state') == 'running' and _task_owner(t) == agent), None)
        waiting = [t for t in snapshot['tasks']
                   if t.get('state') in {'pending', 'blocked'}
                   and not t.get('resolved_by')
                   and _is_waiting_for(t, agent)]
        states[agent] = dict(
            state='working' if running else 'waiting',
            task=running['id'] if running else None,
            assigned=[t['id'] for t in waiting],
            reason=None if running else snapshot.get('reason') or ('dependency_or_dispatch' if waiting else 'needs_assignment'))
    now = datetime.now(timezone.utc)
    value = dict(observed_utc=now.isoformat(), project=snapshot['state'], agents=states,
                 unattributed_running=[t['id'] for t in snapshot['tasks']
                     if t.get('state') == 'running' and _task_owner(t) not in agents],
                 budget=snapshot['budget'],
                 budget_resets_utc=((now.date()+timedelta(days=1)).isoformat()+'T00:00:00Z'
                                    if snapshot['budget']['limit'] is not None else None))
    target = Path(artifacts)/'team-status.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f'team-status.{os.getpid()}.tmp')
    temporary.write_text(json.dumps(value,indent=2),encoding='utf-8')
    temporary.replace(target)
    return value


def run(db, backends, artifacts, agents, max_tasks=4, seconds=1800, origin='interactive',
        *, isolate_failures=False):
    if not 1 <= max_tasks <= 12 or not 1 <= seconds <= 3600:
        raise ValueError('worker must be bounded')
    deadline = time.monotonic() + seconds
    status_agents = list(backends) or list(agents)
    status_agents = list(dict.fromkeys(status_agents))
    config_hash=hashlib.sha256(json.dumps(backends,sort_keys=True).encode()).hexdigest()
    db.execute('BEGIN IMMEDIATE')
    try:
        old=db.execute("SELECT value FROM meta WHERE key='worker_backends'").fetchone()
        if old and old[0]!=config_hash:
            raise ValueError('Backend configuration changed; coordinator review required')
        db.execute("INSERT OR IGNORE INTO meta VALUES('worker_backends',?)",(config_hash,))
        db.execute('COMMIT')
    except BaseException:
        db.execute('ROLLBACK')
        raise
    project.reconcile(db)
    db.execute("INSERT INTO events(at,task,event,detail) VALUES(?,NULL,'worker_started',?)",
        (time.time(),json.dumps(dict(pid=os.getpid(),origin=origin,max_tasks=max_tasks,seconds=seconds))))
    completed = 0
    for _ in range(max_tasks):
        if time.monotonic() >= deadline:
            break
        task = None
        for agent in agents:
            task = project.claim(db, agent, isolate_failures=isolate_failures, mailbox_task=False)
            if task:
                break
        if task is None:
            break
        print(json.dumps({'event':'started', 'task':task['id'], 'agent':task['owner_agent']}), flush=True)
        write_live_status(db, artifacts, status_agents)
        try:
            spec = dict(task['payload'])
            if Path(spec['cwd']).resolve() != Path(task['worktree']).resolve():
                raise ValueError('payload worktree does not match lease')
            if spec['backend'] != task['owner_agent']:
                raise ValueError('payload backend does not match claimed agent')
            spec['timeout'] = max(1, min(spec.get('timeout', 480), deadline-time.monotonic()))
            task_dir = Path(artifacts) / task['id'].replace('/', '_')
            result = perform(spec, backends[spec['backend']], task_dir)
            good = result['state'] in {'tested', 'reviewed'}
            project.finish(db, task['id'], {'state':result['state'], 'session':result['session'],
                'artifact_dir':str(task_dir.resolve()), 'agent_idle':True,
                'summary':result['packet'].get('summary'), 'next_work':result['packet'].get('next_work')},
                'completed' if good else 'blocked', claim_token=task['claim_token'])
            completed += int(good)
            print(json.dumps({'event':'agent_idle', 'task':task['id'], 'state':result['state']}), flush=True)
            if not good:
                break
        except Exception as exc:
            project.finish(db, task['id'], {'error':type(exc).__name__+': '+str(exc)[:2000],
                'agent_idle':True}, 'failed', claim_token=task['claim_token'])
            print(json.dumps({'event':'failed', 'task':task['id'], 'error':str(exc)[:500]}), flush=True)
            break
    snapshot = project.status(db)
    write_live_status(db, artifacts, status_agents)
    db.execute("INSERT INTO events(at,task,event,detail) VALUES(?,NULL,'worker_idle',?)",
        (time.time(),json.dumps(dict(pid=os.getpid(),origin=origin,state=snapshot['state'],reason=snapshot['reason']))))
    (Path(artifacts)/'status.json').write_text(json.dumps(snapshot,indent=2),encoding='utf-8')
    print(json.dumps({'project':snapshot['state'],'reason':snapshot['reason'],'completed_this_run':completed}),flush=True)
    return snapshot


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('db'); ap.add_argument('backends'); ap.add_argument('artifacts')
    ap.add_argument('--agents',nargs='+',default=['deepseek','grok'])
    ap.add_argument('--max-tasks',type=int,default=4)
    ap.add_argument('--seconds',type=int,default=1800)
    ap.add_argument('--origin',choices=['interactive','scheduled'],default='interactive')
    ap.add_argument('--isolate-failures', action='store_true',
                    help='allow independent actors in different worktrees to proceed')
    a=ap.parse_args()
    Path(a.artifacts).mkdir(parents=True,exist_ok=True)
    with closing(project.database(a.db)) as db:
        result=run(db,json.loads(Path(a.backends).read_text(encoding='utf-8')),a.artifacts,a.agents,a.max_tasks,a.seconds,a.origin, isolate_failures=a.isolate_failures)
    return 1 if result['state']=='blocked' else 0


if __name__=='__main__':
    raise SystemExit(main())
