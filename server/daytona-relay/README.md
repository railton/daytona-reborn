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
it has one). The image is about 2.5 MB and runs as an unprivileged user with
a read-only file system.

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

The race settings apply to every player of every room: each game takes them
when it joins, so the cabinets never disagree (the game cancels a link whose
cabinets are set differently). Each game sets its own LINK ID and CAR NUMBER:
the room's creator is the MASTER (car 1), the others SLAVEs (cars 2, 3...).

Example `.env`:

    DAYTONA_PASSWORD=change-this-password
    DAYTONA_DIFFICULTY=hard

## Playing

In the game's home menu, **Online**:

1. The room's creator types a name, the server (its IP or DNS name; `:port`
   only when it is not 7777), its password if it has one, and the number of
   players, then **Create the room**. An **invite code** shows (`DTN1-...`):
   **Copy the code** and send it to the friends. It holds the server, the room
   and the password, with a check, so a cut or mistyped code is refused.
2. The friends type a name, paste the code under **Join a room**, **Join**.
3. When the room is full the game starts on its own for everyone. The
   MASTER (the room's creator) presses **Start (Enter)**: every other player
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

and prints `link: up, cabinet N of 2` at the end. For more than a few
cabinets add `--realtime` to each, so they run at the arcade's pace as
players' games do (free-running ones flood each other); `link: lost` on
cabinets that end after another has closed is expected.

## Limits

- Not yet tried with players on different networks and cities. Every hop goes
  through the server: pick one close to every player.
- Only Revision A (`daytona`) has link play.

Every game tells the server its online version; a room only takes games of
the version that created it, so every player of a room behaves the same.

## Protocol

One TCP connection a player. A greeting line
(`DAYTONA-RELAY 1 op=create|join room=... players=N name=... pass=...`), then
frames `type (1) | epoch (4) | length (2) | data`: `D` the game's bytes, `S`
the room's state, `P` ping, `C` the games' own coordination (the MASTER's
course, to every other player), `E` a refusal (`code\ntext`; the game shows its
own text for the code, in the player's language). The epoch changes whenever
someone joins or leaves, and an old ring's data is dropped. Details in
`proto/proto.go`; the game's side is `src/app/online.*` and
`src/app/relay_link.*`.
