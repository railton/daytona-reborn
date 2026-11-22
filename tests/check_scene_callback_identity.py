#!/usr/bin/env python3
"""Synthetic witness (no ROM): callback/incarnation identity.

Claim: a visible history (slot, callback, bit state, order, mesh) can be
identical between a CONTINUATION (same object lives on) and a REPLACEMENT
(object freed, a new one allocated in the same slot). Incarnation tokens live
only in the synthetic oracle. A tracker must refuse a link when tokens are
missing or ambiguous. Includes a shared-mesh permutation witness.

Usage: python check_scene_callback_identity.py            normal test (bad CLI args: exit 2)
       python check_scene_callback_identity.py --mutant slot_callback|order   mutant control
Exit 0 = expected outcome (normal: all checks pass; mutant: mutant is KILLED).
"""
import sys

# An observation: visible fields + oracle-only 'token' (None = missing).
def obs(slot, cb, bit, order, mesh, token):
    return {"slot": slot, "cb": cb, "bit": bit, "order": order, "mesh": mesh, "token": token}

def visible(o):
    return (o["slot"], o["cb"], o["bit"], o["order"], o["mesh"])

def visible_history(frames):
    return [sorted(visible(o) for o in f) for f in frames]

# ---- scenarios -------------------------------------------------------------
# Continuation: token T1 persists in slot 3 over frames 0..2.
CONT = [[obs(3, "cbA", 1, 0, "m0", "T1")] for _ in range(3)]
# Replacement: T1 in frames 0..1, freed, T2 allocated in slot 3 at frame 2.
REPL = [[obs(3, "cbA", 1, 0, "m0", "T1")],
        [obs(3, "cbA", 1, 0, "m0", "T1")],
        [obs(3, "cbA", 1, 0, "m0", "T2")]]
# Shared-mesh permutation: slots 1,2 share mesh m7, same callback and bit.
# Identity-preserving: T1 in slot1, T2 in slot2 throughout.
PERM_KEEP = [[obs(1, "cbB", 0, 0, "m7", "T1"), obs(2, "cbB", 0, 1, "m7", "T2")]
             for _ in range(2)]
# Permuted incarnations: tokens swap slots between frames; visible stays equal.
PERM_SWAP = [[obs(1, "cbB", 0, 0, "m7", "T1"), obs(2, "cbB", 0, 1, "m7", "T2")],
             [obs(1, "cbB", 0, 0, "m7", "T2"), obs(2, "cbB", 0, 1, "m7", "T1")]]

# Rank change with shared mesh: render ranks (order) swap between frames while
# each incarnation keeps its slot (oracle tokens T1/T2 stay in slots 1/2).
PERM_RANK = [[obs(1, "cbB", 0, 0, "m7", "T1"), obs(2, "cbB", 0, 1, "m7", "T2")],
             [obs(1, "cbB", 0, 1, "m7", "T1"), obs(2, "cbB", 0, 0, "m7", "T2")]]

# Checks that decide whether a mutant is killed (both must fail for a mutant).
DECISIVE = ("replacement breaks link at step 1", "perm swap: tokens cross slots")

# ---- trackers --------------------------------------------------------------
def link_token(prev, cur):
    """Link prev->cur only via tokens. Refuse if missing or ambiguous."""
    toks = [o["token"] for o in prev + cur]
    if any(t is None for t in toks):
        return "REFUSED"
    if len({o["token"] for o in prev}) != len(prev) or len({o["token"] for o in cur}) != len(cur):
        return "REFUSED"
    return sorted((p["slot"], c["slot"]) for p in prev for c in cur if p["token"] == c["token"])

def link_slot_callback(prev, cur):  # mutant: key = (slot, callback)
    return sorted((p["slot"], c["slot"]) for p in prev for c in cur
                  if (p["slot"], p["cb"]) == (c["slot"], c["cb"]))

def link_order(prev, cur):  # mutant: key = order
    return sorted((p["slot"], c["slot"]) for p in prev for c in cur if p["order"] == c["order"])

def chain(frames, linker):
    return [linker(frames[i], frames[i + 1]) for i in range(len(frames) - 1)]

def run(linker):
    """Return list of (name, ok) checks for the given linker."""
    res = []
    res.append(("visible history identical CONT==REPL", visible_history(CONT) == visible_history(REPL)))
    res.append(("visible history identical PERM_KEEP==PERM_SWAP", visible_history(PERM_KEEP) == visible_history(PERM_SWAP)))
    res.append(("continuation links at every step", chain(CONT, linker) == [[(3, 3)], [(3, 3)]]))
    res.append(("replacement breaks link at step 1", chain(REPL, linker) == [[(3, 3)], []]))
    res.append(("perm keep: identity links", chain(PERM_KEEP, linker)[0] == [(1, 1), (2, 2)]))
    res.append(("perm swap: tokens cross slots", chain(PERM_SWAP, linker)[0] == [(1, 2), (2, 1)]))
    res.append(("rank change, shared mesh: slots keep identity",
                visible_history(PERM_RANK)[0] != visible_history(PERM_RANK)[1]
                and chain(PERM_RANK, linker)[0] == [(1, 1), (2, 2)]))
    # missing / ambiguous tokens must be refused (only meaningful for token tracker)
    missing = [[obs(3, "cbA", 1, 0, "m0", None)], [obs(3, "cbA", 1, 0, "m0", "T1")]]
    ambig = [[obs(1, "cbB", 0, 0, "m7", "T1"), obs(2, "cbB", 0, 1, "m7", "T1")],
             [obs(1, "cbB", 0, 0, "m7", "T1")]]
    res.append(("missing token refused", linker(*missing) == "REFUSED"))
    res.append(("ambiguous token refused", linker(*ambig) == "REFUSED"))
    return res

LINKERS = {"slot_callback": link_slot_callback, "order": link_order}

def parse_args(argv):
    """Return mutant name or None. Exit 2 on a missing or unknown mutant / argument."""
    args = argv[1:]
    if not args:
        return None
    if args[0] != "--mutant" or len(args) != 2:
        sys.stderr.write("usage: check_scene_callback_identity.py [--mutant slot_callback|order]\n")
        sys.exit(2)
    if args[1] not in LINKERS:
        sys.stderr.write("unknown mutant %r (expected slot_callback|order)\n" % args[1])
        sys.exit(2)
    return args[1]

def main():
    mutant = parse_args(sys.argv)
    linker = LINKERS[mutant] if mutant else link_token
    res = run(linker)
    for name, ok in res:
        print(("PASS " if ok else "FAIL ") + name)
    failed = [n for n, ok in res if not ok]
    if mutant is not None:
        status = dict(res)
        # A mutant is KILLED only if BOTH decisive assertions fail,
        # not through an incidental failure such as a missing token.
        killed = all(not status[n] for n in DECISIVE)
        print("RESULT: mutant %s %s (decisive failing: %d/%d, total failing: %d)" % (
            mutant, "KILLED" if killed else "SURVIVED",
            sum(1 for n in DECISIVE if not status[n]), len(DECISIVE), len(failed)))
        return 0 if killed else 1
    if mutant is None:
        print("RESULT: %s" % ("OK" if not failed else "BROKEN"))
        return 0 if not failed else 1

if __name__ == "__main__":
    sys.exit(main())
