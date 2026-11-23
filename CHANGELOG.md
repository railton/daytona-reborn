# Changelog

Changes in this fork, on top of Daytona Reborn beta 0.10 (`9d0eccc`).

## Unreleased

### Online play

- **Online mode** in the home menu (Revision A, `daytona`): race over the
  Internet with up to 8 players through a relay server. No port to open on
  any player's router; works behind CGNAT. The game connects out to the
  server (`app::RelayLink`, a link transport beside the LAN's `TcpLink`).
- **Rooms and invite codes.** Create a room (server, optional password,
  2 to 8 players) and pass on its invite code (`DTN1-...`), which holds the
  server, the room and the password with a CRC-16, so a cut or mistyped
  code is refused. Friends paste it and join.
- **Automatic cabinet settings.** Each game writes its own settings before
  the first frame: the room's creator is the MASTER (car 1), the others
  SLAVEs (cars 2, 3...), and every game takes the server's race settings, so
  linked cabinets always agree. The player's own settings are kept: the
  room's are not saved. Mapped from the test menu: LINK ID, CAR NUMBER,
  CABINET, COUNTRY, DIFFICULTY, GAME MODE, RIVAL ARROW, ADVERTISE SOUND,
  free play, and the settings CRC (CRC-16/XMODEM of 0x0a..0x7f).
- **The race starts on its own** when the room is full; if someone leaves,
  the others return to the room's screen and the race starts again when it
  is full again.
- **One Start for the room.** Only the MASTER starts a race (configurable);
  every other player joins it on their own (the cabinet's WAITING FOR YOUR
  ENTRY is answered for them).
- **The MASTER chooses the course** for everyone: the others' course cursor
  follows it and confirms when the MASTER does. The gearbox stays each
  player's own.
- **Players and standings over the race.** A list at the top left with each
  player's car number and colour (as the course map shows them), name, and
  the player's own row highlighted; during the race it is the live standings,
  in race order. Shown for the race's first 10 s and while Tab (or the
  gamepad's left stick button) is held.
- **Version check.** Each game sends its online version; a room only takes
  games of the version that created it.
- **Clear errors** in the player's language: server not found, wrong
  password, room full or missing, other version, too many connections...
- The ROM set is asked for before joining a room (a profile without one left
  the room at once).
- `--online` opens the Online screen at start.

### Relay server (`server/daytona-relay`)

- Go server, Docker image of about 2.5 MB (unprivileged, read-only file
  system): `docker compose up -d --build`.
- Race settings for every room from environment variables:
  `DAYTONA_CABINET`, `DAYTONA_COUNTRY` (default `usa`: the game in English),
  `DAYTONA_DIFFICULTY`, `DAYTONA_GAME_MODE`, `DAYTONA_RIVAL_ARROW`,
  `DAYTONA_ADVERTISE_SOUND`, `DAYTONA_FREE_PLAY` (default on: Start only),
  `DAYTONA_START` (`master` or `anyone`); and `DAYTONA_PORT`,
  `DAYTONA_PASSWORD`, `DAYTONA_MAX_ROOMS`, `DAYTONA_MAX_CONNECTIONS`,
  `DAYTONA_MAX_PER_IP`.
- Forms each room's ring in join order and passes each player's bytes to the
  next; control frames for the games' own coordination (course, standings);
  a new epoch whenever someone joins or leaves, so old data is dropped.
- Errors as codes with English text; the game translates them.
- Hardening: the greeting line is bounded (512 bytes), connections are
  limited in all and per address.
- `daytona-link`: a helper (Linux and Windows) bridging the unmodified game's
  LAN link play to the server.

### Interface and languages

- Brazilian Portuguese interface (`lang/pt.po`, every text).
- The Online screen: labels above full-width fields, larger fields and
  buttons, create and join side by side.
- `lang/` is copied next to the game on every build, so a changed
  translation reaches it without relinking.

### Building

- `docs/BUILDING.md`: from cloning to the Online mode, on Windows
  (`setup.ps1`) and Linux (Docker or `setup.sh`), with the real output paths,
  settings folders, updating, clean rebuilds and the usual problems.
- `scripts/docker-build.sh` and `docker/linux-build`: the Linux build in an
  Ubuntu 24.04 container, as the calling user; nothing but Docker on the
  host, and the game runs on 24.04 and anything newer.

### Tools and tests

- `m2run --relay` joins a room headless, as the game does (settings, Start,
  course, standings), and prints the standings it knew.
- `tests/test_online.cpp`: invite code, protocol text, cabinet settings,
  Start, course and standings logic. Go tests for the server's protocol and
  settings.
- `--capture-home` waits three frames, so auto-sized windows are captured;
  `M2_CAPTURE_ONLINE_NAMES=1` (or `=2`, as standings) draws the players list
  with sample names in a capture.

### Known limits

- Not yet tested between players on different networks.
- With 7 or 8 cabinets starting at very different speeds (headless runs) the
  link handshake sometimes does not complete, on direct LAN links as well as
  through the relay; an 8-player race through the relay has completed.
- Traffic: about 3.5 KB per cabinet per frame, passed round the ring, so
  roughly 3.3 Mbit/s each way per player with 2 players and 13 Mbit/s with
  8; the server carries the sum.
- The relay is not encrypted: the server password travels in clear, is in
  the invite code and is stored in `launcher.ini`.
- A brief network drop restarts the room's race.
