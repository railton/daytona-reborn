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
it has one), and **8080** for the web interface (below). The image is about
6 MB and runs as an unprivileged user with a read-only file system.

### Environment variables

Edit `docker-compose.yml`, or put them in a `.env` file beside it, then
`docker compose up -d` again.

| Variable | Default | Values |
| --- | --- | --- |
| `DAYTONA_PORT` | `7777` | TCP port |
| `DAYTONA_PASSWORD` | empty | the server's password; it travels in the invite code |
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
| `DAYTONA_WEB_PORT` | `8080` | TCP port of the web interface; `0` turns it off |
| `DAYTONA_PUBLIC_ADDRESS` | empty | `host:port` the players reach the relay at (`:port` may be left out for 7777); the web page's invite codes need it |
| `DAYTONA_WEB_PASSWORD` | empty | the password to create rooms on the web page; empty: `DAYTONA_PASSWORD` |

The race settings apply to every player of a room: each game takes them
when it joins, so the cabinets never disagree (the game cancels a link whose
cabinets are set differently). Rooms created by a game use the variables
above; rooms created on the web page keep their own (the variables are then
the form's defaults). Each game sets its own LINK ID and CAR NUMBER: the
first player in the room is the MASTER (car 1), the others SLAVEs (cars
2, 3...).

Example `.env`:

    DAYTONA_PASSWORD=change-this-password
    DAYTONA_DIFFICULTY=hard
    DAYTONA_PUBLIC_ADDRESS=vps.example.com:7777

## Web interface

The relay serves a page on `DAYTONA_WEB_PORT` (`http://vps.example.com:8080/`),
readable on a phone:

- **Open rooms**, refreshed every 3 seconds: players joined (names, the
  MASTER first), waiting or racing (full and linked), the room's race
  settings, its age. A **public** room shows its code (and, on a server
  without a password, a Copy invite button); a **private** room, and every
  room a game created, never shows its code.
- **Create a room** (up to 8 players; the first to join owns it): each race setting, who may
  start, public or private. The new room's code and its **invite code**
  (`DTN1-...`, the one the game's Online mode takes) show, with a Copy
  button. The invite code needs `DAYTONA_PUBLIC_ADDRESS`; without it the
  page shows the room code only.

A room created on the page exists before anyone joins (the games join it,
`op=join`) and keeps its own settings for its whole life. It is removed
when it has stayed empty for **10 minutes** (`webRoomIdle` in
`cmd/daytona-relay/main.go`), counted from its creation or from when its last
player left. Rooms created by a game close with their last player, as
before. `DAYTONA_MAX_ROOMS` counts both; one address may have at most 10
empty rooms from the page waiting at once.

**Password.** When the server has a password, the invite code carries it,
so creating a room asks for it (`DAYTONA_WEB_PASSWORD`, or else
`DAYTONA_PASSWORD`; HTTP basic authentication, any user name). The rooms
list stays open to everyone and never shows an invite code then. The page is
plain HTTP: to send the password over the Internet, put it behind a reverse
proxy with HTTPS (the game's own connection is not encrypted either).

The JSON API behind the page (errors are `{"code": ..., "error": ...}`):

| Request | Answer |
| --- | --- |
| `GET /api/config` | the options and their values, the server's defaults, `password_required`, `server` (the public address) |
| `GET /api/rooms` | `rooms`: `code` (public only), `public`, `origin` (`web`/`game`), `size`, `joined`, `players`, `state`, `settings`, `age_seconds`, `expires_in` (empty web rooms) |
| `POST /api/rooms` | body `{"players": 4, "public": true, "difficulty": "hard", ...}` (JSON keys: `cabinet`, `country`, `difficulty`, `game_mode`, `rival_arrow`, `advertise_sound`, `free_play`, `start`; missing ones take the server's); `201` with `room`, `invite`, `server`, `settings`, `expires_in`; `400` invalid, `401` password, `413` over 4 KB, `415` not JSON, `429` too many empty rooms, `503` server full |

    curl -u :change-this-password -H 'Content-Type: application/json' \
      -d '{"players":3,"public":true,"game_mode":"endurance"}' http://localhost:8080/api/rooms

## Playing

In the game's home menu, **Online**:

1. The room's creator types a name, the server (its IP or DNS name; `:port`
   only when it is not 7777) and its password if it has one, then **Create
   the room** (it takes up to 8 players). An **invite code** shows (`DTN1-...`):
   **Copy the code** and send it to the friends. It holds the server, the room
   and the password, with a check, so a cut or mistyped code is refused.
2. The friends type a name, paste the code under **Join a room**, **Join**.
   A room can also be created on the web page (above): every player then
   joins with its invite code, and the first to join owns the room.
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

and prints `link: up, cabinet N of 2` at the end. For a room made on the web
page, every `m2run` uses `--relay-join ROOM`. For more than a few cabinets
add `--realtime` to each, so they run at the arcade's pace as players' games
do (free-running ones flood each other); `link: lost` on cabinets that end
after another has closed is expected.

## Limits

- Not yet tried with players on different networks and cities. Every hop goes
  through the server: pick one close to every player.
- Only Revision A (`daytona`) has link play.

Every game tells the server its online version; a room only takes games of
the version that created it, so every player of a room behaves the same.

Online version 5 enables the communication board's frame synchronization in
both the desktop game and `m2run --relay`. A faster cabinet waits for the
ring instead of advancing its countdown independently. The existing
3-second token timeout still applies, and a slow cabinet can slow the room.
All players must rebuild to join the same room; the relay needs no update
for this version change. Local timing measurements and diagnostic commands
are in `../../docs/ONLINE_FRAME_SYNC.md`.

## Protocol

One TCP connection a player. A greeting line
(`DAYTONA-RELAY 2 op=create|join room=... name=... pass=... v=...`; `op=auto
players=N` for the helper's fixed-size rooms), then
frames `type (1) | epoch (4) | length (2) | data`: `D` the game's bytes, `S`
the room's state, `P` ping, `C` the games' own coordination (the MASTER's
course, to every other player), `G` the owner starting a race, `E` a refusal (`code\ntext`; the game shows its
own text for the code, in the player's language). The epoch changes whenever
someone joins or leaves, and an old ring's data is dropped. Details in
`proto/proto.go`; the game's side is `src/app/online.*` and
`src/app/relay_link.*`.
