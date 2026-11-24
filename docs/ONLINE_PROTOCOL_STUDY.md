# Online protocol study (towards 0.13.0)

Six changes to the Online mode's link were implemented behind switches and
measured against 0.12.0 on the same harness, then in combination. Date:
2026-11-24. **0.13.0 adopts all six, on by default**; each switch below,
set to `0`, turns one off again (diagnostics and comparisons).

## The changes

| # | Change | Switch (off) | Where |
| --- | --- | --- | --- |
| 1 | **Star**: the relay decodes each player's blocks and sends each player a block with every other car's latest data (one hop), instead of the ring's data passed from player to player | relay `DAYTONA_STAR=0` | `server/daytona-relay/cmd/daytona-relay/main.go` (`compose`), `linkcodec/` (the game's codec in Go, byte-identical: `TestSameAsTheGame`) |
| 2 | **UDP with repetition**: every game message also goes by UDP, numbered, each datagram repeating the last two; received once, in order, from whichever path is first; TCP stays the backstop | game `M2_PROTO_UDP=0` (hello without `udp=1`) | `cmd/daytona-relay/udp.go`, `src/app/relay_link.cpp` |
| 3 | **Adaptive reserve**: the paced link's reserve grows when frames go without a block, shrinks when there was always one to spare (every 300 frames) | `M2_PROTO_ADAPTIVE=0` (m2run; always on in the game) | `src/runtime/comm_board.cpp` (`vblank`) |
| 4 | **One send a frame**: the block and the token (MASTER), the answer and the token (SLAVE) in one send, at once | `M2_PROTO_COALESCE=0` | `comm_board.cpp` (`forward_token`, `consume`), `LinkTransport::flush_now` |
| 5 | **Rejoin after a drop**: the relay keeps a dropped racer's place 10 s (star mode); the game reconnects with its key (`op=rejoin`) and the race goes on | `M2_PROTO_RECONNECT=0`; the relay offers it in star mode (`resume=1`) | `main.go` (`leave`, `rejoin`), `relay_link.cpp` (`lost`) |
| 6 | **Barrier timing**: each game waits at the start barrier so that all leave together, from the latencies the relay measures | `M2_PROTO_BARRIER_TUNE=0` (m2run; always on in the game) | `src/runtime/online_settings.h` (`StartBarrier::holding`) |

A first version of 4 held the messages until the next poll and added up to a
frame of delay a hop (VPS: 76-82% smooth steps); 4b sends at once and is the
one measured below.

## Harness

`m2run --realtime --relay`, 4 cabinets, 7000 frames, the scripted race
(`relay-test/scen.sh`, not in the repository), three scenarios:

- **jitter**: local relay, 100 ms +- 15 ms added on the relay's way out (`tc netem`);
- **loss**: local relay, 100 ms and 1% packet loss;
- **VPS**: the test relay in Europe from Brazil (~245 ms round trip; 0 TCP
  retransmissions in a 2-minute probe), two cabinets direct and two through a
  local TCP proxy adding 40 ms each way (TCP only: those two have no UDP path).

Measures, in the race:

- **smooth steps**: the share of frames where the previous car's data moved by
  exactly one tick (the game's counter in the received block);
- **ticks**: the game's own tick rate (100% = full speed);
- **start spread**: first to last cabinet entering the race (same host clock);
- **data age**: how old each other car's data is when shown, by ring place
  (the cabinets log their own tick and the received slots' with a common
  clock, `M2_TEST_AGE_LOG`; `relay-test/age.py`);
- **sends/s**: `send()` calls a player.

## Results (min..max over the 4 cabinets)

| Variant | Jitter: smooth / age max | Loss 1%: smooth / ticks / age max | VPS: smooth / age mean, max / spread | Sends/s |
| --- | --- | --- | --- | --- |
| 0.12.0 | 96.7-97.9% / 452 ms | 82.3-84.8% / 91-96% / 497 ms | 85.5-86.6% / 658, 1008 ms / 140 ms | 116 |
| 4b one send | 98.1-99.5% / 466 | 87.3-91.7% / 93-98% / 488 | 81.5-85.8% / 691, 1061 / 37 | **58** |
| 3 adaptive | 96.5-98.0% / 504 | **96.0-97.8% / 99-100%** / 712 | 84.5-87.3% / 817, 1237 / 109 | 116 |
| 1 star | 97.5-98.4% / **173** | 81.9-86.3% / 91-95% / **190** | **91.5-93.1% / 348, 403** / 106 | 116 |
| 6 barrier | 96.3-99.2% / 467 | 82.5-84.5% / 92-96% / 502 | 83.7-86.9% / 658, 996 / **16** | 116 |
| 2 UDP | **99.7-99.9%** / 470 | **99.2-99.5% / 100%** / 518 | 83.6-89.2% / 665, 1038 / 28 | 116 |
| 1+3+4b+6 (TCP) | 96.0-98.6% / 208 | 96.5-97.8% / 99-100% / 269 | 92.5-93.9% / 393, 421 / 28 | 58 |
| **all six** | **99.4-99.6% / 155** | **99.4-99.7% / 100% / 168** | **91.1-93.1% / 418, 486** / 72 | **58** |

Rejoin (5), 2 cabinets, the relay-side socket of one closed mid-race
(`ss -K`): in star mode the player was back in the same second and the race
went on (99% smooth steps, both links up at the end); without star mode the
race ended for both (`link: lost`).

UDP in practice: through the local relay ~13,400 of ~13,500 messages a
cabinet arrived first by UDP; on the VPS, the two direct cabinets (behind a
home router) got 9,100-12,800 first by UDP; the two behind the TCP-only proxy
fell back to TCP by themselves.

## Reading

- **Star** is what makes rooms of 3 to 8 across continents usable: the
  oldest car went from ~1 s to ~0.4 s on the VPS, and all cars are equally
  old, whatever the ring place.
- **UDP** is what absorbs packet loss: with 1% loss, smooth steps went from
  82-85% to 99%+ and the game ran at full speed; on a clean route it changes
  little.
- **Adaptive reserve** also absorbs loss (96-98%), at the cost of age; with
  UDP the reserve rarely needs to grow (all six: loss age max 168 ms).
- **One send a frame** halves the packets (helps Wi-Fi and mobile links),
  neutral to slightly positive otherwise.
- **Barrier timing** narrows the start on real routes (140 -> 16 ms alone;
  28-72 ms in the combinations, where the start is already tight).
- **Rejoin** turns a dropped connection from "race over for everyone" into a
  hiccup, in star mode.

**The most effective set is all six together**: on every scenario it is the
best or within noise of the best (jitter and loss: 99%+ smooth, full speed,
every car under 170 ms old; VPS: 91-93% smooth, every car about 0.4 s old
instead of up to 1 s, half the packets).

## Not done / limits

- TLS (encrypting the relay's connection) is not part of this: the client
  would need a TLS library on every platform (OpenSSL, Schannel); the relay
  runs without a password by default and the game data is not sensitive.
- The VPS route showed no packet loss in the probes: UDP's gain was measured
  with simulated loss; the real gain depends on each player's line.
- Adopting the set changes what the game sends: relay protocol 5 and online
  version 9 (0.13.0), the relay with UDP published (7777/udp) and star mode
  on by default; players behind firewalls that block UDP keep working over
  TCP. A 0.12 game is refused by a 0.13 relay, and the other way round, with
  the version message.
