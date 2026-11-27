# daytona-relay: the Online mode server

A server to race Daytona USA (Revision A, `daytona`) over the Internet, up to
8 players a room. The players only make **outgoing** connections to it: no
port to open on anyone's router, and it works behind CGNAT.

The arcade's link play joins the cabinets in a ring (each sends to the next).
The server forms that ring inside a room, in join order. In the game's rooms
it also reads the compressed link blocks (star mode, the default): each
player gets, in one hop, a block with every other car's latest data, instead
of the data passed from player to player round the ring. With 4 players from
Brazil through a relay in Europe the farthest car went from about 1 s old to
0.4 s. Each car's data reaches each player in order, and when a player's
data is late the server keeps the others' going at the frame's pace itself
(`DAYTONA_STAR_PACE_MS`). A player whose connection drops in a race has 10 s
to come back to their place, and the game reconnects on its own.

## Run it on a server (Docker)

Docker with the compose plugin. Copy this folder (`server/daytona-relay`) to
the server, then:

    docker compose up -d --build
    docker compose logs -f

Open port **7777**, TCP and UDP, in the server's firewall (and the provider's
panel, if it has one). TCP is what every game needs; UDP carries a copy of
the race data, which gets through packet loss without waiting for TCP's
retransmission (1% loss: the other cars' data moved smoothly in 99% of the
frames instead of 82-85%). Without UDP, or with a player whose network blocks
it, the games use TCP alone. The image is about
6 MB and runs as an unprivileged user with a read-only file system.

### Environment variables

Edit `docker-compose.yml`, or put them in a `.env` file beside it, then
`docker compose up -d` again.

| Variable | Default | Values |
| --- | --- | --- |
| `DAYTONA_PORT` | `7777` | the port, TCP and UDP |
| `DAYTONA_PASSWORD` | empty | the server's password; it travels in the invite code. Not needed for privacy: every room is private by its random code, and many groups can share one server |
| `DAYTONA_MAX_ROOMS` | `100` | rooms open at the same time |
| `DAYTONA_MAX_CONNECTIONS` | `1000` | connections at the same time |
| `DAYTONA_MAX_PER_IP` | `16` | connections from one address (players behind one router share it) |
| `DAYTONA_MAX_ROOMS_PER_IP` | `8` | rooms open from one address |
| `DAYTONA_ROOM_ALONE_MIN` | `30` | minutes a room's creator may wait alone before the room is closed |
| `DAYTONA_ROOM_IDLE_MIN` | `120` | minutes a room may go without a race (no game data) before it is closed |
| `DAYTONA_JOIN_TRIES_PER_MIN` | `20` | failed attempts a minute from one address (a room that does not exist, a wrong password) |
| `DAYTONA_CABINET` | `twin` | `twin`, `uplight`, `deluxe` |
| `DAYTONA_COUNTRY` | `export` | `export` (the game in English, speeds in km/h: "International" in the game), `usa` (English, mph), `jpn` (Japanese, km/h) |
| `DAYTONA_DIFFICULTY` | `normal` | `easy`, `normal`, `hard`, `hardest` |
| `DAYTONA_GAME_MODE` | `normal` | `normal`, `grandprix`, `endurance` |
| `DAYTONA_RIVAL_ARROW` | `on` | `on`, `off` |
| `DAYTONA_ADVERTISE_SOUND` | `on` | `on`, `off` |
| `DAYTONA_FREE_PLAY` | `on` | `on` (no coins, Start only), `off` |
| `DAYTONA_START` | `master` | `master` (only the room's creator starts a race), `anyone` |
| `DAYTONA_UDP_REPEAT` | `2` | earlier messages repeated in each datagram: what covers a burst of lost datagrams (fewer: less traffic) |
| `DAYTONA_STAR` | `1` | `1` star mode (above); `0` the ring of 0.12, for comparisons (no rejoin) |
| `DAYTONA_STAR_HISTORY` | `2` | star mode: each car's data goes to each player in order, one slot per block, at most this many behind its newest; `0` the latest at each send (0.22's: a car stopped or jumped when its updates and the sender's did not line up) |
| `DAYTONA_STAR_PACE_MS` | `35` | star mode: when a player's predecessor in the ring is this many ms late (a burst of loss on its line), the server sends that player its blocks itself, once a frame, with every other car's newest data and the late car standing; the late car's blocks are then taken in but not sent, so the player never gets a burst; at most 0.5 s at a time, only in a steady race; `0` off (each player's blocks composed only when its predecessor sends) |

The race variables are the **defaults**: the game shows them when its
player connects to create a room, and the room's creator may change each
one for their room (`op=create set=...`). The room's settings then apply to
every player of it: each game takes them when it joins, so the cabinets
never disagree (the game cancels a link whose cabinets are set
differently). Each game sets its own LINK ID and CAR NUMBER: the room's
owner is the MASTER (car 1), the others SLAVEs (cars 2, 3...).

Example `.env`:

    DAYTONA_PASSWORD=change-this-password
    DAYTONA_DIFFICULTY=hard

## Abuse limits

A room's players are the only ones who can reach it, but one of them, or a
few addresses, could still take the server from the others. The relay
(`cmd/daytona-relay/guard.go`) limits:

- **each connection's traffic**: 400 frames, 1 MB and 20 control messages a
  second (a game sends about 120 frames and a few control messages). One
  that floods is disconnected itself (error `flood`), and the other players
  keep racing; each player's datagrams likewise (the excess is dropped);
- **rooms per address**: 8, and 16 connections;
- **idle rooms**: closed after 30 minutes with their creator alone, or 2
  hours without a race (error `idle`); a room whose games are linked is
  never idle;
- **guessing**: 20 failed attempts a minute from one address (a room that
  does not exist, a wrong password; error `slow_down`), so room codes
  (31^8) and a server password cannot be tried quickly;
- **datagram addresses**: taken only from the address of the player's own
  connection, so nobody can send a race's datagrams elsewhere;
- **the course and the gearbox**: the control messages that choose them for
  the room (`cursor=`, `course=`, `gearcursor=`, `gearbox=`) are passed on
  only from the race's MASTER (the ring's first), so that a modified game
  cannot choose for the others; the other control messages (standings,
  arrival) from anyone.

The container runs as nobody, read-only, without capabilities, with at
most 256 MB of memory and 128 processes.

## Logs

    docker compose logs -f            # as it goes
    docker compose logs --since 24h   # a day of it

Times are in **UTC**, with milliseconds. Besides rooms created, players
joining, dropping, coming back and leaving, each race (a ring of players)
gets a line per player every minute and when it ends: the game messages the
server received from that player (and how many a second: a game short of
data sends fewer), how many came first by UDP, the messages sent to it, its
latency to the server (min/avg/max over the race), its send queue at its
longest, and its drops and returns. Set them against the players' own logs
(`daytona.log`, "Generate logs" in the game's Settings, on by default), whose
online lines carry the time in UTC too. Docker keeps about 100 MB of it
(`docker-compose.yml`, `logging`).

## Playing

In the game's home menu, the **Online** card:

1. The room's creator presses **Create a new room**. The room's settings
   screen asks for a name (needed before connecting) and the server (its IP
   or DNS name; `:port` only when it is not 7777), and **Connect** (a
   password field shows only when the server asks for one): the server
   answers its default race settings, and the room's settings (cabinet,
   country, difficulty, game mode, rival arrow, advertise sound, free play,
   who may start) become choices. Then **Create the room** (it takes up to
   8 players). **Copy the invite code** (`DTN1-...`) and send it to the
   friends. It holds the server, the room and the password, with a check, so
   a cut or mistyped code is refused.
2. The friends copy the code: the Online card pastes it on its own, and
   **Join the room** (the game asks for a name the first time). The home
   screen shows the room; **Room details...** lists the players, each with
   their latency to the server (round trip, measured every 2 s; green up to
   60 ms, yellow up to 120 ms, red above). Between two players the delay is
   about the sum of theirs. The home screen also asks the saved server every
   minute (`op=info`) to show whether it answers.
3. The room's owner presses **Start the race** when everyone is in: the game
   starts for everyone then in the room, the owner being the MASTER. Who joins
   later waits on the room's screen for the next race, which the owner starts
   with **New race with everyone**. In the race the MASTER presses
   **Start (Enter)**: every other player
   joins that race on their own (the cabinet's WAITING FOR YOUR ENTRY is
   answered for them). With `DAYTONA_START=anyone`, any player's Start does.
4. The MASTER chooses the course and the gearbox: the others' cursors
   follow it and confirm when the MASTER confirms, so the room races with
   the MASTER's gearbox. At the start each game waits for the others (15 s
   at most). Esc shows the room's details while the race runs on behind
   them (no pause online); **Leave the room** ends it. The cabinets' test
   menu (F2) is locked online: their settings are the room's.

When someone leaves, the others come back to the room's screen; when it is
full again the race starts again on its own.

Outside the Online mode the player's own settings are unchanged: the room's
apply only to the online race and are not saved.

## On a local network

Run the server on one of the computers (the `daytona-relay` program, built
with Go, or Docker): when it starts, it prints this computer's addresses on
its network, the server the others give in the Online screen:

    players on this computer's network: server 192.168.0.10:7777 (give it to the others)

(not inside a container, where the computer's own addresses are not seen;
virtual bridges such as Docker's are left out). Allow it through the
firewall on the private network.

## Testing headless

The project's `m2run` joins a room as the game does:

    build-daytona/m2run build-daytona/rom_cache/daytona 6000 --relay 127.0.0.1 --relay-create test:2 --relay-name A
    build-daytona/m2run build-daytona/rom_cache/daytona 6000 --relay 127.0.0.1 --relay-join test --relay-name B

and prints `link: up, cabinet N of 2` at the end; `--relay-settings
difficulty:hard,start:anyone` sets the created room's settings. For more than a few cabinets
add `--realtime` to each, so they run at the arcade's pace as players' games
do (free-running ones flood each other); `link: lost` on cabinets that end
after another has closed is expected.

## Limits

- Not yet tried with players on different networks and cities. Every hop goes
  through the server: pick one close to every player.
- The arcade's link expects the ring's round trip within a frame (cables
  between cabinets). Online games run at full speed without frame sync;
  before the race each waits for the others (a start barrier, released from
  the latencies the server measures) so all start together, and in the race
  the other cars are as old as the network makes them: about half the round
  trip of their player plus half of yours, in star mode. The link comes up
  at once from the room (no numbering round the ring) and the games' link
  is paced against jitter, with a reserve of blocks that grows and shrinks
  with the line, so rooms of 4 across continents run at full speed. See
  `../../docs/ONLINE-PROTOCOL.md` (how it works, the measurements and the
  switches for diagnostics).
- The connection is not encrypted (no TLS): the race data is not sensitive,
  but a server password travels in clear.
- Traffic: the game compresses its link data (`src/app/link_codec.h`): a
  game frame of 7170 bytes became 121 / 197 / 323 bytes on average over
  captured 2 / 4 / 8 player sessions, about 0.1 to 0.3 Mbit/s each way a
  player; the server carries the room's sum.
- Only Revision A (`daytona`) has link play.

Every game tells the server its online version; a room only takes games of
the version that created it, so every player of a room behaves the same.

## Protocol

One TCP connection a player. A greeting line
(`DAYTONA-RELAY 5 op=create|join room=... name=... pass=... v=... [set=key:value,...] [udp=1]`;
`op=info`, answered by one `I` frame with the defaults and, once a race has
started on this server, `game_online=N`, the newest game online version (`v=`)
raced with since it started; `op=rejoin room=... key=...` back to a
place after a drop, with the key the `S` frame gave, `key=` and `resume=1`), then
frames `type (1) | epoch (4) | length (2) | data`: `D` one comm-board wire
frame (compressed by the game, `src/app/link_codec.h`),
`S` the room's state (with each player's latency, `lat=` and `wlat=` in ms),
`P` ping (the server's every 2 s carry an 8-byte time stamp), `O` the
game's answer to it (the same stamp: the round trip is the latency), `C` the games' own coordination (the MASTER's
course, to every other player), `G` the owner starting a race, `E` a refusal (`code\ntext`, then any
`key=value` lines: `server_protocol=N` with `version`, `room_online=V` with `game_version`; the game shows its
own text for the code, in the player's language, and says from them which side has to update). The epoch changes whenever
someone joins or leaves, and an old ring's data is dropped.

A game that says `udp=1` numbers its `D` messages (a 4-byte sequence number
first) and sends each one in a UDP datagram to the same port as well; the
server does the same towards it. Datagrams: `h` + key (the game's address,
every 2 s, which also keeps its router's mapping open) and `d` epoch (4),
count (1), then count x [seq (4), length (2), message], newest first: the
messages of one go (a frame's block and token) share a datagram, with the
two messages before them repeated (`DAYTONA_UDP_REPEAT`; the game:
`M2_PROTO_UDP_REPEAT`). Each side takes every number once, in order, from
whichever path brings it first. Details in `proto/proto.go` and
`cmd/daytona-relay/udp.go`; the game's side is `src/app/online.*` and
`src/app/relay_link.*`.
