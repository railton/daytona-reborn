# Online race timing

Measured on 2026-11-23 against the running local Docker relay, starting from
`a1b1028`. Two linked headless Revision A cabinets ran at `--realtime`, with
ordinary scripted Start, steering and pedal inputs. No live guest RAM or
generated code was changed. The observed milestone is entry into mode 22
at `0x5010a0`, not a screenshot-based measurement of the green light.

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

For the former behavior, set `M2_TEST_RELAY_ASYNC=1` on **both** processes.
For normal pacing, omit `M2_TEST_FRAME_DELAY_MS`. Fault injection is only in
the headless tool; `M2_TEST_FRAME_DELAY_FROM` optionally changes its default
starting frame of 2500. The trace records state/link transitions and a
heartbeat every 60 frames. Compare the first mode-22 row's `wall_ms`; these
monotonic timestamps are comparable only on the same host.
