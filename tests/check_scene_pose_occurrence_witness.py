'''Synthetic witness that pose links do not identify an occulted object.

Two histories share the same matrices. The tracked object is absent for one
tick, then a slot returns at its old pose. Continuation keeps a fixture
label; replacement changes it. Labels here are synthetic and local to this
file. Nothing in this test reads a producer, and it does not claim that
producer tokens exist.

candidate_links skips a key when both sides have fewer than two entries, so
a second far instance of the same key stays in the group.
'''

from __future__ import annotations

from pathlib import Path
import sys

import numpy as np


KEY = ("0", "0", "0")
TOOLS = Path(__file__).resolve().parents[1] / "tools"
TRANSLATION_TOL = 0.05
LINEAR_TOL = 0.001
TRACKED_X = 0.0
FAR_X = 100.0


def _load_candidate_links(tools_dir):
    '''Import candidate_links from the explicit tools directory.'''
    module_path = Path(tools_dir) / 'scene_pose_candidates.py'
    if not module_path.is_file():
        raise SystemExit('scene_pose_candidates.py not found in %s' % tools_dir)
    sys.path.insert(0, str(module_path.parent))
    sys.modules.pop('scene_pose_candidates', None)
    from scene_pose_candidates import candidate_links
    return candidate_links


def _pose(x):
    '''Identity rotation, translation on x, affine bottom row.'''
    pose = np.eye(4, dtype=np.float64)
    pose[0, 3] = float(x)
    return pose


def _clone(frames):
    return {
        tick: [pose.copy() for pose in group]
        for tick, group in frames.items()
    }


def _links(candidate_links, frames, previous, current):
    '''Pose-only helper. Identity delta, production tolerances.'''
    return candidate_links(
        {KEY: frames[previous]},
        {KEY: frames[current]},
        np.eye(4, dtype=np.float64),
        translation_tol=TRANSLATION_TOL,
        linear_tol=LINEAR_TOL,
    )


def occurrence_bridge_ok(source_token, target_token):
    '''Accept a labeled bridge only when both synthetic labels match.

    None is a missing label, not an identity, even when both sides are None.
    '''
    if source_token is None or target_token is None:
        return False
    return source_token == target_token


def _decisions(links, source_tokens, target_tokens):
    '''Apply the labeled oracle to pose links. Tokens are not pose inputs.'''
    return [
        (
            key,
            source,
            target,
            occurrence_bridge_ok(source_tokens[source], target_tokens[target]),
        )
        for key, source, target in links
    ]


def _require(label, actual, expected):
    print('%s %r' % (label, actual))
    if actual != expected:
        raise AssertionError((label, actual, expected))


def _pose_history():
    '''Shared matrices: tracked, one-tick gap, return, then a steady tick.'''
    tracked = _pose(TRACKED_X)
    far = _pose(FAR_X)
    return {
        0: [tracked, far],
        1: [far.copy()],
        2: [tracked.copy(), far.copy()],
        3: [tracked.copy(), far.copy()],
    }


def run(candidate_links):
    '''Compare continuation and replacement without feeding labels to poses.'''
    separation = abs(FAR_X - TRACKED_X)
    print('separation %s' % separation)
    if separation <= TRANSLATION_TOL:
        raise AssertionError('far instance is inside the translation tolerance')

    continuation_poses = _pose_history()
    replacement_poses = _clone(continuation_poses)
    # Synthetic fixture labels only. Not read from a producer.
    continuation_tokens = {
        0: ('same-occurrence', 'far-steady'),
        1: ('far-steady',),
        2: ('same-occurrence', 'far-steady'),
        3: ('same-occurrence', 'far-steady'),
    }
    replacement_tokens = {
        0: ('same-occurrence', 'far-steady'),
        1: ('far-steady',),
        2: ('replacement', 'far-steady'),
        3: ('replacement', 'far-steady'),
    }
    none_tokens = {
        0: ('same-occurrence', 'far-steady'),
        2: (None, 'far-steady'),
    }

    singleton = _links(
        candidate_links,
        {0: [_pose(TRACKED_X)], 1: [_pose(TRACKED_X)]},
        0,
        1,
    )
    _require('singleton-skipped', singleton, [])

    spans = (
        ('into-gap', 0, 1, [(KEY, 1, 0)]),
        ('out-of-gap', 1, 2, [(KEY, 0, 1)]),
        ('bridge', 0, 2, [(KEY, 0, 0), (KEY, 1, 1)]),
        ('steady', 2, 3, [(KEY, 0, 0), (KEY, 1, 1)]),
    )
    pose_outputs = {}
    for label, previous, current, expected in spans:
        continued = _links(candidate_links, continuation_poses, previous, current)
        replaced = _links(candidate_links, replacement_poses, previous, current)
        _require('continuation-%s' % label, continued, expected)
        _require('replacement-%s' % label, replaced, expected)
        if continued != replaced:
            raise AssertionError((label, continued, replaced))
        pose_outputs[label] = continued

    bridge = pose_outputs['bridge']
    steady = pose_outputs['steady']
    continuation_bridge = _decisions(
        bridge, continuation_tokens[0], continuation_tokens[2]
    )
    replacement_bridge = _decisions(
        bridge, replacement_tokens[0], replacement_tokens[2]
    )
    none_bridge = _decisions(bridge, none_tokens[0], none_tokens[2])
    continuation_steady = _decisions(
        steady, continuation_tokens[2], continuation_tokens[3]
    )
    replacement_steady = _decisions(
        steady, replacement_tokens[2], replacement_tokens[3]
    )
    _require('continuation-bridge-oracle', continuation_bridge, [
        (KEY, 0, 0, True),
        (KEY, 1, 1, True),
    ])
    _require('replacement-bridge-oracle', replacement_bridge, [
        (KEY, 0, 0, False),
        (KEY, 1, 1, True),
    ])
    _require('none-bridge-oracle', none_bridge, [
        (KEY, 0, 0, False),
        (KEY, 1, 1, True),
    ])
    _require('continuation-steady-oracle', continuation_steady, [
        (KEY, 0, 0, True),
        (KEY, 1, 1, True),
    ])
    _require('replacement-steady-oracle', replacement_steady, [
        (KEY, 0, 0, True),
        (KEY, 1, 1, True),
    ])
    if occurrence_bridge_ok(None, None):
        raise AssertionError('two missing labels were treated as one occurrence')
    if occurrence_bridge_ok('same-occurrence', 'replacement'):
        raise AssertionError('a changed label bridged the occultation')
    print('producer-tokens none')


def main():
    print('tools %s' % TOOLS)
    candidate_links = _load_candidate_links(TOOLS)
    run(candidate_links)
    print('check_scene_pose_occurrence_witness: ok')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
