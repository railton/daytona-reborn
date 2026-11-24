# Changelog

Changes in this fork, on top of Daytona Reborn beta 0.10 (`9d0eccc`).

## Unreleased

### Online play

- **Keep online cabinets on the same frame clock.** Online now enables the
  communication board's existing frame synchronization. With an artificial
  22 ms delay on the master, the previous asynchronous mode let the slave
  enter the race 6.926 s earlier; synchronization reduced that gap to 27 ms
  on the local relay. Online compatibility version is now 5. See
  `docs/ONLINE_FRAME_SYNC.md` for measurements and limits.
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
- **Rooms of up to 8, started by their owner.** No player count to choose:
  friends join, the room's owner (its creator, or the first to join a room
  made on the web) presses **Start the race**, and everyone then in the room
  races. Who joins during a race waits on the room's screen for the next one;
  **New race with everyone** forms a new race with them. If a racer leaves,
  the others return to the room's screen until the owner starts again; if
  the owner leaves, the next player owns the room. Relay protocol version 2,
  online version 4 (the daytona-link helper keeps fixed-size rooms that
  start on their own).
- **One Start for the room.** Only the MASTER starts a race (configurable);
  every other player joins it on their own (the cabinet's WAITING FOR YOUR
  ENTRY is answered for them).
- **The MASTER chooses the course** for everyone: the others' course cursor
  follows it and confirms when the MASTER does. The gearbox stays each
  player's own.
- **Players and standings over the race.** A list at the top left with each
  player's car number and colour (as the course map shows them), name, and
  the player's own row highlighted; during the race it is the live standings,
  in race order. Drawn in an arcade style of its own: leaning bars, outlined
  italic figures, a chequered strip, places in lime as the HUD's POSITION;
  each name in its car's paint (measured on the eight linked cars). Shown for the race's first 10 s and while Tab (or the
  gamepad's left stick button) is held.
- **Name tags over the cars.** In an online race each other player's car
  carries a small tag with their car number and name in their car's colour,
  in the list's arcade style, all race long. It follows the car as drawn
  (widescreen, the "4:3" mode, super sampling and the four camera views
  alike), shrinks and fades with distance (gone beyond about 160 units,
  where a car is a few pixels wide), fades out while the scenery or another
  car hides it, and is not drawn for a car off the screen or behind the
  camera. Each linked car's number comes from the game's own car records:
  in a linked race the byte at +0x94 of each opponent record (0x505480 +
  n x 0x300) is the CAR NUMBER - 1 of the cabinet driving it, 8 for a
  computer car (found on 3 and 4 linked cabinets, `runtime/car_tags.h`).
- **Version check.** Each game sends its online version; a room only takes
  games of the version that created it.
- **Clear errors** in the player's language: server not found, wrong
  password, room full or missing, other version, too many connections...
- The ROM set is asked for before joining a room (a profile without one left
  the room at once).
- `--online` opens the Online screen at start, `--settings` the settings;
  `M2_CAPTURE_SETTINGS_TAB=N` opens a settings tab in a `--capture-home`
  image.

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
- **Web interface** on its own port (`DAYTONA_WEB_PORT`, default 8080, 0
  turns it off): one page, in the home screen's sunset-speedway style, that
  lists the open rooms (code for public rooms only, players, waiting or
  racing, settings, age; refreshed every 3 s) and creates rooms with their
  own race settings (2 to 8 players, every option above, who may start,
  public or private). It shows the new room's invite code with a Copy
  button; the code needs `DAYTONA_PUBLIC_ADDRESS` (the relay's host:port as
  players reach it). Creating asks a password when the server has one
  (`DAYTONA_WEB_PASSWORD`, or else `DAYTONA_PASSWORD`, which travels in the
  invite). JSON API behind it: `GET /api/config`, `GET /api/rooms`,
  `POST /api/rooms`. Standard library only, the page embedded (20 KB); the
  image grows from about 2.5 MB to 6 MB.
- **Per-room race settings.** A room created on the web keeps its own
  settings and start rule, sent to its players instead of the environment's;
  it exists before anyone joins and is removed after 10 minutes empty. Rooms
  created by a game keep the environment's settings and close with their
  last player. `DAYTONA_MAX_ROOMS` counts both. The wire protocol is
  unchanged.

### Interface and languages

- **English is the project's language.** Interface texts are written in
  English in the code (the translation key); French moved to `lang/fr.po`,
  Portuguese is keyed by English, `lang/en.po` is gone. English is also the
  default when no language is chosen and the system's is not available.
  The texts the original left untranslated (link status, home menu
  messages) are translated too.
- Brazilian Portuguese interface (`lang/pt.po`, every text).
- **Settings redesigned** in the Online screen's style: one centred column,
  a larger font, a label above each full-width field, help text that wraps
  to the width, sections with titles and two columns where they fit; the
  long page split into tabs: Game, Display, Enhancements, Link play,
  Controls, Sound; the language choice in the header.
- The Online screen: labels above full-width fields, larger fields and
  buttons, create and join side by side; long labels wrap.
- `lang/` is copied next to the game on every build, so a changed
  translation reaches it without relinking.
- **New home screen.** The 3D backdrop is now a low-sun speedway seen from
  the first turn: sky with sun and clouds, far hills, a grandstand with its
  crowd, start gantry and lights, pit road, scoring pylon, infield lake and
  palms; ten stock cars run as a pack, two wide, at an even speed through
  the turns, and the camera drifts round the turn with the focus following
  them. The menu leans like racing type: italic title, slanted buttons, a
  yellow Play with running chevrons, course tiles with their difficulty, an
  arcade cursor on the keyboard/gamepad choice, start lights while a race
  loads. On Linux it uses the desktop's bold sans (Noto, Liberation or
  DejaVu) when there is one. Same shaders; the scene is 84k vertices
  (3.4 MB, uploaded once), drawn in two calls plus one per car.

### Documentation

- README rewritten for this fork (highlights, quick start, playing, Online,
  link play, platforms, developers), with a screenshot of the home screen;
  it now also describes the relay's web page, the name tags over the cars,
  8-cabinet rooms and what happens when a race ends.
- rules.md: this fork's rules (CHANGELOG on every visible change, English
  with interface text through lang/*.po, online version and error codes,
  tests on every commit, the Docker build, never sharing game builds).

### Building

- `docs/BUILDING.md`: from cloning to the Online mode, on Windows
  (`setup.ps1`) and Linux (Docker or `setup.sh`), with the real output paths,
  settings folders, updating, clean rebuilds and the usual problems.
- `scripts/docker-build.sh` and `docker/linux-build`: the Linux build in an
  Ubuntu 24.04 container, as the calling user; nothing but Docker on the
  host, and the game runs on 24.04 and anything newer.

### Tools and tests

- Removed the original fork author's private agent tooling (`tools/team_*.py`,
  `tools/grok-acp-bridge.py`, `tools/schedule-*.ps1`), its tests and its
  workflow: none of it builds or runs the game.

- `m2run --relay` joins a room headless, as the game does (settings, Start,
  course, standings), and prints the standings it knew; `m2run --realtime`
  holds each frame to the arcade's 57.52 Hz, as players' games run.
- `tests/test_online.cpp`: invite code, protocol text, cabinet settings,
  Start, course and standings logic. Go tests for the server's protocol and
  settings.
- `--capture-home` waits three frames, so auto-sized windows are captured;
  `M2_CAPTURE_ONLINE_NAMES=1` (or `=2`, as standings) draws the players list
  with sample names in a capture, `=3` the name tags over three cars.
- `tests/test_car_tags.cpp`: the tags' place from synthetic polygons (the
  car records' number byte, projection, widescreen origin, what hides a
  car) and the game's picture in the window.

### Known limits

- Not yet tested between players on different networks.
- Headless test cabinets must run at the arcade's pace (`m2run --realtime`)
  for large rooms: free-running ones at different speeds flood each other
  and lose the link. Frame sync drops the link after 3 s without its token;
  this timeout still applies to the synchronized Online mode.
  At the arcade's pace 8 cabinets through the relay link and stay linked
  (two runs of 104 s and 157 s, no link lost before the end).
- Traffic: about 3.5 KB per cabinet per frame, passed round the ring, so
  roughly 3.3 Mbit/s each way per player with 2 players and 13 Mbit/s with
  8; the server carries the sum.
- The relay is not encrypted: the server password travels in clear, is in
  the invite code and is stored in `launcher.ini`.
- A brief network drop restarts the room's race.
- Name tags: whether a car is hidden is judged from the centres of its 8
  largest faces against the opaque polygons drawn before them, so trees and
  fences (cut-out textures) never hide one, and a car seen only by an edge
  can lose its tag. Checked on headless frames (m2run, 3 and 4 cabinets,
  widescreen too), not yet in a race between players.
