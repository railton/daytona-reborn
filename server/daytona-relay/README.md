# daytona-relay: the Online mode server

A server to race Daytona USA (Revision A, `daytona`) over the Internet, up to
8 players a room. The players only make **outgoing** connections to it: no
port to open on anyone's router, and it works behind CGNAT.

The arcade's link play joins the cabinets in a ring (each sends to the next).
The server forms that ring inside a room, in join order, and passes each
player's bytes to the next player. It does not read the game's protocol.

## Run it on a server (Docker)

Docker with the compose plugin. Copy this folder (`server/daytona-relay`) to
the server, then:

    docker compose up -d --build
    docker compose logs -f

Open TCP port **7777** in the server's firewall (and the provider's panel, if
it has one): it is the only port the server uses. The image is about
6 MB and runs as an unprivileged user with a read-only file system.

### Environment variables

Edit `docker-compose.yml`, or put them in a `.env` file beside it, then
`docker compose up -d` again.

| Variable | Default | Values |
| --- | --- | --- |
| `DAYTONA_PORT` | `7777` | TCP port |
| `DAYTONA_PASSWORD` | empty | the server's password; it travels in the invite code. Not needed for privacy: every room is private by its random code, and many groups can share one server |
| `DAYTONA_MAX_ROOMS` | `100` | rooms open at the same time |
| `DAYTONA_MAX_CONNECTIONS` | `1000` | connections at the same time |
| `DAYTONA_MAX_PER_IP` | `16` | connections from one address (players behind one router share it) |
| `DAYTONA_CABINET` | `twin` | `twin`, `uplight`, `deluxe` |
| `DAYTONA_COUNTRY` | `usa` | `usa` (the game in English), `jpn` (in Japanese), `export` |
| `DAYTONA_DIFFICULTY` | `normal` | `easy`, `normal`, `hard`, `hardest` |
| `DAYTONA_GAME_MODE` | `normal` | `normal`, `grandprix`, `endurance` |
| `DAYTONA_RIVAL_ARROW` | `on` | `on`, `off` |
| `DAYTONA_ADVERTISE_SOUND` | `on` | `on`, `off` |
| `DAYTONA_FREE_PLAY` | `on` | `on` (no coins, Start only), `off` |
| `DAYTONA_START` | `master` | `master` (only the room's creator starts a race), `anyone` |

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

## Playing

In the game's home menu, **Online**:

1. The room's creator types a name and the server (its IP or DNS name;
   `:port` only when it is not 7777), and presses **Connect** (a password
   field shows only when the server asks for one): the server answers its default race settings, and the room's
   settings (cabinet, country, difficulty, game mode, rival arrow, advertise
   sound, free play, who may start) become editable. Then **Create the
   room** (it takes up to 8 players). An **invite code** shows (`DTN1-...`):
   **Copy the code** and send it to the friends. It holds the server, the room
   and the password, with a check, so a cut or mistyped code is refused.
2. The friends type a name, paste the code under **Join a room**, **Join**.
   The room's screen lists the players, each with their latency to the
   server (round trip, measured every 2 s; green up to 60 ms, yellow up to
   120 ms, red above). Between two players the delay is about the sum of
   theirs.
3. The room's owner presses **Start the race** when everyone is in: the game
   starts for everyone then in the room, the owner being the MASTER. Who joins
   later waits on the room's screen for the next race, which the owner starts
   with **New race with everyone**. In the race the MASTER presses
   **Start (Enter)**: every other player
   joins that race on their own (the cabinet's WAITING FOR YOUR ENTRY is
   answered for them). With `DAYTONA_START=anyone`, any player's Start does.
4. The MASTER chooses the course: the others' course cursor follows it and
   confirms when the MASTER confirms. Each player chooses their own gearbox.
   Esc comes back to the room's screen; **Leave the room** ends it.

When someone leaves, the others come back to the room's screen; when it is
full again the race starts again on its own.

Outside the Online mode the player's own settings are unchanged: the room's
apply only to the online race and are not saved.

### Without the game's Online mode: `daytona-link`

For the unmodified game, each player runs the `daytona-link` helper (Linux or
Windows) beside it:

    daytona-link -server vps.example.com:7777 -room race1 -players 3 -name Ana

and in the launcher's Game tab: **Link to other cabinets**, port 15112, next
cabinet `127.0.0.1:15200`. LINK ID and CAR NUMBER then go in the test menu
(F2), as the helper says.

Build (Go 1.22+, or Docker):

    docker run --rm -v "$PWD:/src" -w /src golang:1.24 sh -c \
      'go test ./... && go build -o dist/daytona-link ./cmd/daytona-link && GOOS=windows go build -o dist/daytona-link.exe ./cmd/daytona-link'

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
  before the race each waits for the others (a start barrier) so all start
  together, and in the race the other cars are as old as the network makes
  them. Rooms of 4 or more with long round trips (from Brazil to Europe,
  ~230 ms each) do not get past the game's network check at boot yet; 2
  players do. See `../../docs/ONLINE_FRAME_SYNC.md`.
- Traffic: the game compresses its link data (`src/app/link_codec.h`): a
  game frame of 7170 bytes became 121 / 197 / 323 bytes on average over
  captured 2 / 4 / 8 player sessions, about 0.1 to 0.3 Mbit/s each way a
  player; the server carries the room's sum.
- Only Revision A (`daytona`) has link play.

Every game tells the server its online version; a room only takes games of
the version that created it, so every player of a room behaves the same.

The game's rooms and the `daytona-link` helper's do not mix (error
`room_kind`): the game compresses its link data, the helper passes the
unmodified game's bytes.

## Protocol

One TCP connection a player. A greeting line
(`DAYTONA-RELAY 4 op=create|join room=... name=... pass=... v=... [set=key:value,...]`;
`op=info`, answered by one `I` frame with the defaults; `op=auto players=N`
for the helper's fixed-size rooms), then
frames `type (1) | epoch (4) | length (2) | data`: `D` one comm-board wire
frame (compressed by the game, `src/app/link_codec.h`; raw from the helper),
`S` the room's state (with each player's latency, `lat=` and `wlat=` in ms),
`P` ping (the server's every 2 s carry an 8-byte time stamp), `O` the
game's answer to it (the same stamp: the round trip is the latency), `C` the games' own coordination (the MASTER's
course, to every other player), `G` the owner starting a race, `E` a refusal (`code\ntext`; the game shows its
own text for the code, in the player's language). The epoch changes whenever
someone joins or leaves, and an old ring's data is dropped. Details in
`proto/proto.go`; the game's side is `src/app/online.*` and
`src/app/relay_link.*`.
