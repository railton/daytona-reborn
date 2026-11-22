import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import team_worker


def _snapshot(tasks):
    return {
        'tasks': tasks,
        'state': 'running' if any(t.get('state') == 'running' for t in tasks) else 'pending',
        'reason': None,
        'budget': {'limit': None},
    }


def test_running_wildcard_is_attributed_to_owner_only(tmp_path, monkeypatch):
    tasks = [
        {
            'id': 'T/running-wildcard',
            'agent': '*',
            'owner_agent': 'grok',
            'state': 'running',
            'resolved_by': None,
        },
    ]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))

    team_worker.write_live_status(object(), tmp_path, ['deepseek', 'grok'])

    value = json.loads((tmp_path / 'team-status.json').read_text(encoding='utf-8'))
    assert set(value['agents']) == {'deepseek', 'grok'}
    assert value['agents']['deepseek']['state'] == 'waiting'
    assert value['agents']['deepseek']['task'] is None
    assert value['agents']['grok']['state'] == 'working'
    assert value['agents']['grok']['task'] == 'T/running-wildcard'


def test_pending_wildcard_is_listed_without_marking_agents_working(tmp_path, monkeypatch):
    tasks = [
        {
            'id': 'T/pending-wildcard',
            'agent': '*',
            'owner_agent': None,
            'state': 'pending',
            'resolved_by': None,
        },
    ]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))

    team_worker.write_live_status(object(), tmp_path, ['deepseek', 'grok'])

    value = json.loads((tmp_path / 'team-status.json').read_text(encoding='utf-8'))
    for agent in ('deepseek', 'grok'):
        assert value['agents'][agent]['state'] == 'waiting'
        assert value['agents'][agent]['task'] is None
        assert value['agents'][agent]['assigned'] == ['T/pending-wildcard']


class _FakeCursor:
    def fetchone(self):
        return None


def test_literal_names_are_not_wildcards(tmp_path, monkeypatch):
    tasks = [dict(id='literal-' + name, agent=name, owner_agent=None,
                  state='pending', resolved_by=None)
             for name in ('any', 'all', 'wildcard', ' * ')]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))
    value = team_worker.write_live_status(object(), tmp_path, ['deepseek', 'grok', 'any'])
    assert value['agents']['deepseek']['assigned'] == []
    assert value['agents']['grok']['assigned'] == []
    assert value['agents']['any']['assigned'] == ['literal-any']


def test_missing_owner_is_visible_as_unattributed_running(tmp_path, monkeypatch):
    tasks = [dict(id='orphan', agent='*', owner_agent=None,
                  state='running', resolved_by=None)]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))
    value = team_worker.write_live_status(object(), tmp_path, ['deepseek', 'grok'])
    assert value['unattributed_running'] == ['orphan']


def test_owned_blocked_wildcard_does_not_appear_assigned_to_everyone(tmp_path, monkeypatch):
    tasks = [dict(id='blocked', agent='*', owner_agent='grok',
                  state='blocked', resolved_by=None)]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))
    value = team_worker.write_live_status(object(), tmp_path, ['deepseek', 'grok'])
    assert value['agents']['deepseek']['assigned'] == []
    assert value['agents']['grok']['assigned'] == ['blocked']


class _FakeDB:
    def execute(self, sql, params=()):
        return _FakeCursor()


def test_separately_filtered_workers_keep_all_configured_agents(tmp_path, monkeypatch):
    tasks = [
        {
            'id': 'T/already-running',
            'agent': 'deepseek',
            'owner_agent': 'deepseek',
            'state': 'running',
            'resolved_by': None,
        },
    ]
    monkeypatch.setattr(team_worker.project, 'status', lambda db: _snapshot(tasks))
    monkeypatch.setattr(team_worker.project, 'reconcile', lambda db: None)

    claims = []

    def claim(db, agent):
        claims.append(agent)
        return None

    monkeypatch.setattr(team_worker.project, 'claim', claim)

    backends = {'deepseek': {'name': 'deepseek'}, 'grok': {'name': 'grok'}}
    db = _FakeDB()
    for dispatch_agent in ('deepseek', 'grok'):
        artifacts = tmp_path / dispatch_agent
        team_worker.run(db, backends, str(artifacts), [dispatch_agent], max_tasks=1, seconds=2)
        value = json.loads((artifacts / 'team-status.json').read_text(encoding='utf-8'))
        assert set(value['agents']) == {'deepseek', 'grok'}
        assert value['agents']['deepseek']['state'] == 'working'
        assert value['agents']['deepseek']['task'] == 'T/already-running'
        assert value['agents']['grok']['state'] == 'waiting'
        assert value['agents']['grok']['task'] is None

    assert claims == ['deepseek', 'grok']
