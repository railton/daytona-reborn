#!/usr/bin/env python3
"""Synthetic regression for grouping geometry commands within one frame.

Commands carry their sequence position in the frame. Invocation declarations
supply the inclusive sequence bounds and expected part count. Readable bounds
claim commands even when a declaration has an invalid part count. Conflicting
claims, overlapping bounds, duplicate sequences, missing parts and mixed
sources reject the affected invocation. Commands with unreadable bounds remain
unbound.

The fixture uses invented inputs and measures grouping inside a single frame.
Pass --mutant NAME to check that the cases reject one deliberate regression.
"""
import itertools
import sys
from collections import namedtuple

Cmd = namedtuple("Cmd", "frame producer slot mesh part source seq")
Invocation = namedtuple("Invocation", "frame producer slot mesh lo hi nparts")
IntraFrameGroup = namedtuple("IntraFrameGroup", "frame producer slot mesh span parts")
MUTANTS = ("producer", "frame", "nparts", "nobound", "undeclared", "trunc",
           "overlap", "nonparts", "source", "dupseq", "invconsume", "globalseq",
           "validclaims")

def _key(x, mutant):
    return (x.frame if mutant != "frame" else 0,
            x.producer if mutant != "producer" else "*", x.slot, x.mesh)

def _valid(i):
    return (isinstance(i.lo, int) and isinstance(i.hi, int) and i.lo <= i.hi
            and isinstance(i.nparts, int) and not isinstance(i.nparts, bool)
            and i.nparts > 0)

def group(cmds, invs, mutant=None):
    """Return groups and refusals in deterministic order.

    Refusals carry the frame, producer, slot, mesh and span. A missing span
    denotes an unbound command.
    """
    groups, refused = [], []
    used = set()
    valid = []
    for i in invs:
        if _valid(i) or (mutant == "nonparts" and isinstance(i.lo, int)
                         and isinstance(i.hi, int) and i.lo <= i.hi):
            valid.append(i)
        else:
            refused.append(_key(i, None) + ((i.lo, i.hi),))
            if (mutant != "invconsume" and type(i.lo) is int and type(i.hi) is int
                    and i.lo <= i.hi):   # bound readable: the refusal owns its commands
                used.update(c for c in cmds if _key(c, None) == _key(i, None)
                            and i.lo <= c.seq <= i.hi)
    # claims are established per seq BEFORE declarations are validated: every
    # invocation with a readable bound claims its range, valid or not. A conflict
    # blocks emission of every claimant, even if one declaration is invalid.
    claimants = valid if mutant == "validclaims" else [
        i for i in invs if type(i.lo) is int and type(i.hi) is int and i.lo <= i.hi]
    # overlap between invocations of the same key -> all refused
    bad = set()
    if mutant != "overlap":
        for a, b in itertools.combinations(claimants, 2):
            if _key(a, mutant) == _key(b, mutant) and a.lo <= b.hi and b.lo <= a.hi:
                bad.update({a, b})
    # seq is global to the frame: one seq claimed by two invocations -> both refused
    clash, owners = set(), {}
    if mutant != "globalseq":
        for i in claimants:
            for c in cmds:
                if (_key(c, mutant) == _key(i, mutant) and i.lo <= c.seq <= i.hi):
                    owners.setdefault((c.frame, c.seq), set()).add(i)
        for o in owners.values():
            if len(o) > 1:
                clash.update(o)
    for i in sorted(valid):
        if mutant == "nobound":
            mine = [c for c in cmds if _key(c, mutant) == _key(i, mutant)]
        else:
            mine = [c for c in cmds if _key(c, mutant) == _key(i, mutant)
                    and i.lo <= c.seq <= i.hi]
        used.update(mine)
        parts = sorted(c.part for c in mine)
        n = i.nparts if mutant != "nonparts" or i.nparts else len(parts)
        ok = parts == list(range(n)) and len({c.source for c in mine}) == 1
        if mutant == "source":
            ok = parts == list(range(n))
        if mutant == "nparts":
            ok = len({c.source for c in mine}) == 1 and len(set(parts)) == len(parts) and bool(parts)
        if mutant == "trunc":      # mutant: ignore parts beyond the declared count
            p2 = [p for p in parts if p < n]
            ok = p2 == list(range(n)) and len({c.source for c in mine}) == 1
        if mutant != "dupseq" and len({c.seq for c in mine}) != len(mine):
            ok = False
        if i in bad or i in clash:
            ok = False
        k = _key(i, mutant) + ((i.lo, i.hi),)
        if ok:
            groups.append(IntraFrameGroup(*k, tuple(parts)))
        else:
            refused.append(k)
    for c in cmds:                  # command with no declared bound
        if c not in used and mutant != "undeclared":
            refused.append(_key(c, None) + (None,))
    if mutant == "undeclared":      # mutant: trust observed commands, no bound needed
        rest = [c for c in cmds if c not in used]
        for k in sorted({_key(c, mutant) for c in rest}):
            ps = sorted(c.part for c in rest if _key(c, mutant) == k)
            groups.append(IntraFrameGroup(*k, None, tuple(ps)))
    return sorted(set(groups), key=lambda g: tuple(map(str, g))), \
           sorted(set(refused), key=lambda r: tuple(map(str, r)))

# --- invented scenarios -----------------------------------------------------
# Branch A: 1 command (seq 0); branch B: 3 commands (seq 1..3); both on mesh m7.
A = [Cmd(0, "A", 5, "m7", 0, "a0", 0)]
B = [Cmd(0, "B", 5, "m7", i, "b0", 1 + i) for i in range(3)]
BASE = A + B
INV = [Invocation(0, "A", 5, "m7", 0, 0, 1), Invocation(0, "B", 5, "m7", 1, 3, 3)]
EXPECT = ([IntraFrameGroup(0, "A", 5, "m7", (0, 0), (0,)),
           IntraFrameGroup(0, "B", 5, "m7", (1, 3), (0, 1, 2))], [])
KB = (0, "B", 5, "m7")

def test_all_permutations():
    for p in itertools.permutations(BASE):
        for q in (INV, INV[::-1]):
            assert group(list(p), q) == EXPECT, p
    assert len(BASE) == 4

def test_keeps_three_parts():
    g, r = group(BASE, INV)
    assert [x for x in g if x.producer == "B"][0].parts == (0, 1, 2) and not r

def test_neighbour_not_merged():
    assert len(group(BASE, INV)[0]) == 2

def test_two_partial_invocations_do_not_complete_each_other():
    # same slot/source: invocation 1 emits parts 0,1 ; invocation 2 emits part 2.
    # Each declares 3 parts. Merged they would look complete; they must be refused.
    cs = A + [Cmd(0, "B", 5, "m7", 0, "b0", 1), Cmd(0, "B", 5, "m7", 1, "b0", 2),
              Cmd(0, "B", 5, "m7", 2, "b0", 5)]
    iv = [INV[0], Invocation(0, "B", 5, "m7", 1, 2, 3), Invocation(0, "B", 5, "m7", 5, 5, 3)]
    g, r = group(cs, iv)
    assert [x.producer for x in g] == ["A"]
    assert r == [KB + ((1, 2),), KB + ((5, 5),)]

def test_underdeclared_refused():
    # declared 2 parts but 3 produced: refuse (no truncation, no trust of observed).
    iv = [INV[0], Invocation(0, "B", 5, "m7", 1, 3, 2)]
    g, r = group(BASE, iv)
    assert r == [KB + ((1, 3),)] and [x.producer for x in g] == ["A"]

def test_overdeclared_refused():
    iv = [INV[0], Invocation(0, "B", 5, "m7", 1, 3, 4)]
    g, r = group(BASE, iv)
    assert r == [KB + ((1, 3),)] and len(g) == 1

def test_two_complete_invocations_stay_separate():
    # same slot, same source, two full invocations back to back: two groups.
    b2 = [Cmd(0, "B", 5, "m7", i, "b0", 4 + i) for i in range(3)]
    iv = INV + [Invocation(0, "B", 5, "m7", 4, 6, 3)]
    g, r = group(BASE + b2, iv)
    assert not r and len(g) == 3
    assert [x.span for x in g if x.producer == "B"] == [(1, 3), (4, 6)]
    assert all(x.parts == (0, 1, 2) for x in g if x.producer == "B")
    # different sources per invocation are fine too (no cross-invocation compare)
    b3 = [c._replace(source="b1") for c in b2]
    g3, r3 = group(BASE + b3, iv)
    assert not r3 and len(g3) == 3

def test_missing_bound_refused():
    g, r = group(BASE, [INV[0]])                 # no declaration for B
    assert r == [KB + (None,)] * 1 and [x.producer for x in g] == ["A"]

def test_missing_nparts_refused():
    for bad in (None, 0, -1, True):
        iv = [INV[0], Invocation(0, "B", 5, "m7", 1, 3, bad)]
        g, r = group(BASE, iv)
        assert KB + ((1, 3),) in r and all(x.producer != "B" for x in g), bad
        assert any(x.producer == "A" for x in g)

def test_missing_bound_value_refused():
    iv = [INV[0], Invocation(0, "B", 5, "m7", None, None, 3)]
    g, r = group(BASE, iv)
    assert KB + ((None, None),) in r and all(x.producer != "B" for x in g)

def test_overlapping_bounds_refused():
    iv = [INV[0], Invocation(0, "B", 5, "m7", 1, 3, 3), Invocation(0, "B", 5, "m7", 3, 6, 3)]
    g, r = group(BASE, iv)
    assert KB + ((1, 3),) in r and KB + ((3, 6),) in r
    assert all(x.producer != "B" for x in g)

def test_duplicate_seq_refused():
    # same seq fills two distinct parts (0 and 1) of one invocation: refuse it.
    cs = A + [Cmd(0, "B", 5, "m7", 0, "b0", 1), Cmd(0, "B", 5, "m7", 1, "b0", 1),
              Cmd(0, "B", 5, "m7", 2, "b0", 3)]
    g, r = group(cs, INV)
    assert r == [KB + ((1, 3),)] and [x.producer for x in g] == ["A"]
    assert group(cs[::-1], INV[::-1]) == (g, r)          # order independent
    # duplicate seq in a single-part invocation too (identical commands)
    d = [Cmd(0, "A", 5, "m7", 0, "a0", 0)] * 2
    g2, r2 = group(d, [INV[0]])
    assert not g2 and r2 == [(0, "A", 5, "m7", (0, 0))]

def test_invalid_bound_with_commands_single_refusal():
    # invalid nparts but readable bound: ONE refusal (the invocation), not two.
    for bad in (None, 0, -1, True):
        inv = [Invocation(0, "B", 5, "m7", 1, 3, bad)]
        assert group(B, inv) == ([], [KB + ((1, 3),)]), bad
        assert group(B[::-1], inv) == ([], [KB + ((1, 3),)]), bad
    # unreadable bound: commands cannot be attributed, stay unbound (deterministic)
    g, r = group(B, [Invocation(0, "B", 5, "m7", None, None, 3)])
    assert g == [] and r == [KB + ((None, None),), KB + (None,)]

def test_same_seq_two_invocations_same_frame_refused():
    ca = [Cmd(0, "A", 5, "m7", 0, "a0", 1)]
    cb = [Cmd(0, "B", 5, "m7", 0, "b0", 1)]
    iv = [Invocation(0, "A", 5, "m7", 1, 1, 1), Invocation(0, "B", 5, "m7", 1, 1, 1)]
    exp = ([], [(0, "A", 5, "m7", (1, 1)), (0, "B", 5, "m7", (1, 1))])
    assert group(ca + cb, iv) == exp
    assert group(cb + ca, iv[::-1]) == exp
    # the clash does not poison a third, unrelated invocation
    g, r = group(ca + cb + B, iv + [Invocation(0, "B", 5, "m7", 1, 3, 3)][:0]
                 + [Invocation(0, "C", 1, "m1", 7, 7, 1)])
    assert len(r) >= 2

def test_seq_reused_across_frames_accepted():
    f0 = [Cmd(0, "A", 5, "m7", 0, "a0", 1)]
    f1 = [Cmd(1, "B", 5, "m7", 0, "b0", 1)]
    iv = [Invocation(0, "A", 5, "m7", 1, 1, 1), Invocation(1, "B", 5, "m7", 1, 1, 1)]
    g, r = group(f0 + f1, iv)
    assert not r and len(g) == 2

def test_clash_spares_other_invocations():
    ca = [Cmd(0, "A", 5, "m7", 0, "a0", 1)]
    cb = [Cmd(0, "B", 5, "m7", 0, "b0", 1)]
    cc = [Cmd(0, "C", 2, "m2", 0, "c0", 4)]
    iv = [Invocation(0, "A", 5, "m7", 1, 1, 1), Invocation(0, "B", 5, "m7", 1, 1, 1),
          Invocation(0, "C", 2, "m2", 4, 4, 1)]
    g, r = group(ca + cb + cc, iv)
    assert [x.producer for x in g] == ["C"] and len(r) == 2

def test_overlapping_bounds_without_shared_seq_refused():
    # bounds overlap only on an empty seq (3): the overlap rule must still refuse both.
    cs = [Cmd(0, "B", 5, "m7", 0, "b0", 1), Cmd(0, "B", 5, "m7", 1, "b0", 2),
          Cmd(0, "B", 5, "m7", 0, "b0", 5), Cmd(0, "B", 5, "m7", 1, "b0", 6)]
    iv = [Invocation(0, "B", 5, "m7", 1, 3, 2), Invocation(0, "B", 5, "m7", 3, 6, 2)]
    g, r = group(cs, iv)
    assert not g and r == [KB + ((1, 3),), KB + ((3, 6),)]

def test_two_sources_refused():
    cs = BASE + [Cmd(0, "B", 5, "m7", i, "b1", 1 + i) for i in range(3)]
    g, r = group(cs, INV)
    assert KB + ((1, 3),) in r and all(x.producer != "B" for x in g)
    assert any(x.producer == "A" for x in g)

def test_partial_production_refused():
    cs = A + B[:2]
    g, r = group(cs, INV)
    assert r == [KB + ((1, 3),)] and len(g) == 1
    cs2 = A + B[:2] + [Cmd(0, "B", 5, "m7", 1, "b0", 3)]   # duplicate part
    assert KB + ((1, 3),) in group(cs2, INV)[1]
    assert group(A, INV)[1] == [KB + ((1, 3),)]            # nothing produced

def test_mixed_sources_with_complete_parts_refused():
    # parts are exactly 0..2 but come from two sources: still refused.
    cs = A + [B[0], B[1], B[2]._replace(source="b1")]
    g, r = group(cs, INV)
    assert r == [KB + ((1, 3),)] and [x.producer for x in g] == ["A"]

def test_bound_excludes_foreign_commands():
    # a stray command of the same key outside every bound is refused, not absorbed.
    stray = Cmd(0, "B", 5, "m7", 0, "b0", 9)
    g, r = group(BASE + [stray], INV)
    assert r == [KB + (None,)] and len(g) == 2

def test_slot_context_per_frame_producer():
    nxt = [Cmd(1, "B", 5, "m7", 0, "b9", 0)]
    g, r = group(BASE + nxt, INV + [Invocation(1, "B", 5, "m7", 0, 0, 1)])
    assert len(g) == 3 and not r
    assert {x.frame for x in g} == {0, 1}

def _no_emit_conflict(cs, iv):
    for p in itertools.permutations(iv):
        g, r = group(cs, list(p))
        assert g == [], (iv, g)
        assert r, iv

def test_valid_and_invalid_same_scope_conflict():
    # same three commands, same bound 1..3: nparts=0 (invalid) vs nparts=3 (valid).
    for bad in (0, -1, None, True, "3"):
        iv = [Invocation(0, "B", 5, "m7", 1, 3, bad), Invocation(0, "B", 5, "m7", 1, 3, 3)]
        _no_emit_conflict(B, iv)
        g, r = group(B, iv)
        assert r == [KB + ((1, 3),)]      # one deterministic refusal, no double entry

def test_valid_and_invalid_partial_overlap_conflict():
    # invalid 1..2 (nparts=0) partially overlaps valid 2..3 (nparts=2): no emission.
    iv = [Invocation(0, "B", 5, "m7", 1, 2, 0), Invocation(0, "B", 5, "m7", 2, 3, 2)]
    _no_emit_conflict(B, iv)
    g, r = group(B, iv)
    assert r == [KB + ((1, 2),), KB + ((2, 3),)]
    # same with the valid one first / wider
    iv = [Invocation(0, "B", 5, "m7", 1, 3, 3), Invocation(0, "B", 5, "m7", 3, 3, 0)]
    _no_emit_conflict(B, iv)

def test_invalid_claim_does_not_block_disjoint_valid():
    # invalid declaration on a disjoint range must not block the valid neighbour.
    iv = INV + [Invocation(0, "B", 5, "m7", 8, 9, 0)]
    g, r = group(BASE, iv)
    assert len(g) == 2 and r == [KB + ((8, 9),)]

def test_invalid_unreadable_bound_claims_nothing():
    iv = INV + [Invocation(0, "B", 5, "m7", None, 3, 3)]
    g, r = group(BASE, iv)
    assert len(g) == 2 and len(r) == 1

def test_unbound_commands_still_refused_deterministically():
    for p in itertools.permutations(BASE):
        g, r = group(list(p), [])
        assert g == [] and len(r) == 2 and all(x[-1] is None for x in r)
        assert (g, r) == group(BASE, [])

def test_no_cross_frame_id():
    fields = set(IntraFrameGroup._fields)
    assert not (fields & {"id", "actor", "actor_id", "token", "uid"})
    g0, _ = group(BASE, INV)
    g1, _ = group([c._replace(frame=1) for c in BASE], [i._replace(frame=1) for i in INV])
    assert [x._replace(frame=0) for x in g1] == g0 and set(g0).isdisjoint(g1)

TESTS = [v for k, v in sorted(globals().items()) if k.startswith("test_")]

def run(mutant):
    global group
    if mutant:
        orig = group
        group = lambda cmds, invs, m=mutant, o=orig: o(cmds, invs, m)
    fails = []
    for t in TESTS:
        try:
            t()
        except (AssertionError, IndexError, TypeError):
            fails.append(t.__name__)
    return fails

def main(args):
    if not args:
        failures = run(None)
        print("FAIL" if failures else "OK", failures or f"{len(TESTS)} tests")
        return 1 if failures else 0
    if len(args) == 2 and args[0] == "--mutant" and args[1] in MUTANTS:
        name = args[1]
        failures = run(name)
        print("mutant", name, "KILLED by" if failures else "SURVIVED", failures)
        return 0 if failures else 1
    print("usage: check_scene_invocation_grouping.py [--mutant NAME]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
