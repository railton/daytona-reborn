# Online race timing

Measured on 2026-11-23 against the running local Docker relay, starting from
`a1b1028`. Two linked headless Revision A cabinets ran at `--realtime`, with
ordinary scripted Start, steering and pedal inputs. No live guest RAM or
generated code was changed. The observed milestone is entry into mode 22
at `0x5010a0`, not a screenshot-based measurement of the green light.

## Superseded: no frame sync online, a start barrier

The measurements below were taken without network latency. With latency
added to the local relay's outgoing packets (`tc netem`), frame sync held
for the whole session slows every frame to the ring's round trip, and a
pause stops everyone:

| Frame sync | 0 ms | 20 ms | 40 ms |
| --- | ---: | ---: | ---: |
| On, all session (2 cabinets) | 57.5 frames/s | 22.1 | 11.7 |
| Off (2 cabinets) | 57.5 | | 57.5 |

- A 5 s stall of one cabinet (`M2_TEST_STALL_AT/MS`, as the game's Esc
  pause): with frame sync on, the other cabinet froze about 3 s and both
  lost the link; the stalled one froze another 3 s after it resumed. With
  frame sync off both carried on, the link up.
- Frame sync held only from the entry to the start (tried first) deadlocked
  2 cabinets at 0 and 20 ms: the MASTER went straight to the selection and
  waited for its token while the SLAVE was still in attract.
- Off, the comm board fetched received data only once a frame: a guest
  waiting for the other cabinets inside a frame waited out its own timeout
  (6.6 s at frame 2510 entering the selection, 0 ms of latency). `read_fg`
  now polls the transport (at most once a millisecond).

So Online runs without frame sync and lines up the start itself
(`rt::online::StartBarrier`, `src/runtime/online_settings.h`): each game,
entering mode 21 (the last before the race), sends "arrived" and holds
(`M2Board::link_idle_tick` keeps its comm board answering) until every game
of the ring has arrived. `m2run --relay` does the same
(`M2_TEST_RELAY_BARRIER=0` turns it off, `M2_TEST_RELAY_SYNC=on` forces
frame sync).

2 realtime cabinets through a relay in Europe from Brazil (round trip ~229
ms; the second through a local proxy adding 40 ms each way), scripted
Start, course, gearbox confirmations, race entry (first mode 22) on the
same host clock:

| Run | Start to race (MASTER) | Race entry, MASTER after SLAVE | Barrier wait |
| --- | ---: | ---: | ---: |
| No barrier, before the `read_fg` fix | | 10.5 s | |
| No barrier | 21.5 s | 0.02 s | |
| No barrier (again) | 39.4 s | 6.1 s | |
| Barrier | 28.6 s | 0.28 s | 2.7 s (SLAVE) |
| Barrier, 0 ms (local relay) | 20.9 s | 0.03 s | 0.03 s |

The selection's exchanges vary with the route (the same steps took 3 to 7 s,
the pre-race mode 20 from 5.8 to 10.8 s), hence the barrier. With 4
cabinets on the same route (two direct, two +80 ms) the game's network check
at boot failed in every run (with or without the barrier): the link came up
around frame 500, the cabinets stayed in mode 0, and two of them restarted
the link every ~825 frames.

### Rooms of 4 to 8 over long routes (the link preset and the paced flag)

The boot failure: each SLAVE raises its ready flag in its own slot at a
fixed frame (about 510) and only if the link is up by then. The FF/FE
numbering waits 232 frames, then a trip round the ring and one hop per
cabinet down: with 4 cabinets at ~250 ms a hop it came up too late, and
nothing numbers the ring again after the game restarts the board. Online
games now preset the ring from the relay's room (`CommBoard::preset_ring`,
ids as the numbering gives them: `tests/test_comm_board.cpp` checks a ring
of 3).

The slow motion: the game's vblank interrupt reads the link flag (fg bit 7)
and keeps it as a reference; its main loop then spins until the flag
differs, and runs that frame's logic. Each received block flips the flag:
a block landing in the vblank tick (before the reference) or two in one
frame (an even count) left nothing to wait for, so the frame's logic tick
was lost. Paced (`CommBoard::set_paced`), a flip is held and shown at the
game's first fg read after the reference, at most once a frame, with blocks
in reserve against frames with no arrival. The game's own tick counter
(byte 0x0a of its block, the trace's `tick`), share of the frames, 4
cabinets through the local relay:

| Condition | Unpaced | Paced, no reserve | Reserve 1 | Reserve 2 |
| --- | --- | --- | --- | --- |
| 0 ms, race | 88-100% | 100% | | |
| 100 +- 15 ms, selection | ~25% (no Start seen) | 82-95% | 91-96% | 96-97% |
| 100 +- 15 ms, race | | 80-96% | 92-99% | 98-100% |

Through the relay in Europe from Brazil, 4 cabinets (two +80 ms), defaults
(preset, paced with 2 in reserve, barrier): selection 97%, race 98-100%,
race entry 29 ms apart; unpaced on the same route 53-81% in the race.
Further headless runs with the defaults: 8 cabinets at 100 +- 15 ms
(93-100%, entry 24 ms apart); 3 cabinets, two races in a row (Expert, then
Advanced chosen by the MASTER: all followed), one pausing 5 s in the first
race, the same standings on all three at each race's end; a player leaving
mid-race (the others carry on, the relay breaks the ring); link play on a
local network (unpaced) up through a race.

## Results

| Test | Master race frame | Slave race frame | Slave wall time minus master |
| --- | ---: | ---: | ---: |
| Previous asynchronous mode, normal pacing | 6498 | 6499 | +13 ms |
| Previous asynchronous mode, master delayed 22 ms per frame | 5115 | 5769 | **-6926 ms** |
| Existing frame sync, normal pacing | 3963 | 3964 | +22 ms |
| Existing frame sync, master delayed 22 ms per frame | 3963 | 3964 | +27 ms |
| Final version-5 default, master delayed 22 ms per frame | 3963 | 3964 | +27 ms |

The delay starts at frame 2500, including each frame's processing time in
addition to the injected sleep. It models a persistently slower client,
not packet latency. With eight synchronized cabinets, all entered mode 22
within a 24 ms wall-clock window (frames 3961 through 3964). Frame numbers
are local; the comm board aligns its ring clock, not boot frame counters.

The normal asynchronous run also spent about 6.1 s between the selection
transition records at frames 2509 and 2511 and missed the scripted course
confirmation, eventually selecting
Beginner by timeout. Its ordinary race-start gap alone does not expose the
problem. The deliberately slower master reproduces a slave entering the
race several seconds first. One earlier delayed run has no recovered process
exit status after a tooling session restart and is excluded from the comparison.

State transitions and common-host monotonic timestamps are preserved in
`evidence/online-frame-sync/transitions.csv`; finite-run summaries are in
`evidence/online-frame-sync/runs.txt`. Link values: 0 off, 1 waiting,
2 up, 3 lost. A client finishing its finite run disconnects; a peer may
therefore report a lost link at shutdown. This is distinct from a loss
before either client finishes.

## Change and scope

The desktop Online path previously passed `false` to `M2Board::set_link`.
It now passes `true`, and `m2run --relay` uses the same default. This reuses
the existing comm-board token exchange and wait; it does not add a new
server protocol or alter the emulated board implementation. LAN retains
its existing configurable frame-sync setting. Online compatibility version
5 prevents mixing these clients with version 4 in one room.

This follows the design document's **Audio, inputs, force feedback, link
play / Link play** section: optional synchronization is already provided by
the board; the application chooses it for Online. No new ROM/MAME parity
claim is made. The controlled symptom is reproduced, but there is no
client trace from the user's original session to prove its exact cause.

Frame sync can slow the whole room to its slowest member and adds waiting
for the network ring. Its existing 3-second timeout remains. These are
same-host relay measurements, not validation over a high-latency Internet
connection, and do not prove literal simultaneous presentation on monitors.

Validation: desktop and headless builds succeeded; CTest ran 66 tests with
no failures and skipped the two Lua tests; `go test ./...` passed. The final
default was rerun for 4500 frames without `--link-sync`: both clients exited
successfully and remained linked through entry into the race. The two- and
eight-player comparison runs each covered 6500 frames.

## Reproduce

Build `m2run`, then use a fresh diagnostic room and two separate terminals:

```sh
M2_ONLINE_TRACE=/tmp/master.csv M2_TEST_FRAME_DELAY_MS=22 \
  build-daytona/m2run build-daytona/rom_cache/daytona 6500 --realtime \
  --frame-skip 2 --relay 127.0.0.1 --relay-create timing-test:2 \
  --relay-name master --inputs /tmp/timing-inputs.txt

M2_ONLINE_TRACE=/tmp/slave.csv \
  build-daytona/m2run build-daytona/rom_cache/daytona 6500 --realtime \
  --frame-skip 2 --relay 127.0.0.1 --relay-join timing-test --relay-name slave
```

Contents of `/tmp/timing-inputs.txt`:

```text
frames 6500
2500-2510 start=1
2600-2800 steer=0xe0
2740-2759 accel=0xe0
```

To hold frame sync on for the whole session (as the measurements below this note did; see "Superseded" above), set `M2_TEST_RELAY_SYNC=on` on **all** processes; `M2_TEST_RELAY_BARRIER=0` turns the start barrier off.
For normal pacing, omit `M2_TEST_FRAME_DELAY_MS`. Fault injection is only in
the headless tool; `M2_TEST_FRAME_DELAY_FROM` optionally changes its default
starting frame of 2500. The trace records state/link transitions and a
heartbeat every 60 frames. Compare the first mode-22 row's `wall_ms`; these
monotonic timestamps are comparable only on the same host.
