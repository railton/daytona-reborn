# Changelog

Changes in this fork, on top of Daytona Reborn beta 0.10 (`9d0eccc`).

## Unreleased

### Controls

- **Link play tab marked advanced.** Settings' "Link play (advanced)" says
  to use Online in the home menu for the Internet, and that its frame sync
  is for a local network only.
- **Course and gearbox selection with keys or a gamepad.** The cabinet's
  course and gearbox screens choose by where the wheel is held, so keys and
  sticks, which spring back to the centre, always fell back to the first
  option. Now each push left or right moves the choice one option and it
  stays when released (course steering <= 0x85 Beginner, 0x86-0xc5
  Advanced, >= 0xc6 Expert; gearbox <= 0x97 / >= 0x98, measured with m2run
  sweeps). An analogue wheel keeps its own position. Affects the Online
  race and the arcade launcher; the PC solo race picks the course in the
  home menu. `m2run --key-steering` applies the same logic headless (two
  taps right confirmed Expert; without it the course fell back to Beginner).

### Online play

- **No frame sync over the Internet; a start barrier instead.** The comm
  board's frame sync holds every game to the ring's round trip each frame:
  through the relay the whole session slowed down with latency (2 players:
  57.5 frames/s at 0 ms, 22.1 at 20 ms, 11.7 at 40 ms) and a paused player
  froze the others for 3 s, then lost the link for both (Esc, then
  Resume). Online now runs without it, at full speed. Each game, entering
  the last mode before the race (21), tells the room and holds there (its
  link still answering) until every game has arrived, so all start
  together (`rt::online::StartBarrier`; a "waiting for the other players"
  box shows meanwhile). Through a relay in Europe from Brazil (2 players,
  ~230 ms round trip, one +80 ms more): without the barrier the MASTER
  started 0.02 to 10.5 s after the other depending on the run; with it,
  0.28 s (the first waited 2.7 s). `docs/ONLINE_FRAME_SYNC.md` has the
  measurements.
- **Link data fetched while the game waits for it.** The comm board fetched
  what the network had brought only once a frame (or in frame sync's
  wait): a game waiting for the other cabinets' data inside a frame waited
  out its own timeout instead, 6 to 9 s at once entering the selection
  (measured), which also made the MASTER reach the race seconds late. It
  now also fetches while the game polls the board (at most once a
  millisecond). Online and link play without frame sync both gain.
- **Compressed link data.** Each comm-board wire frame goes as one message
  against the sender's last two blocks (`src/app/link_codec.h`, no
  library): a game frame of 7170 bytes became 121 / 197 / 323 bytes on
  average over captured 2 / 4 / 8 player sessions (no mismatch on the round
  trip), under 10 µs to encode. About 0.1 to 0.3 Mbit/s each way a player
  instead of 3.3; a room of 8 on the server about 2 Mbit/s instead of 26.5.
- **Each player's latency in the room's list**, round trip to the server,
  green up to 60 ms, yellow up to 120 ms, red above: the server stamps its
  ping every 2 s, the game sends the stamp back, the server smooths it and
  tells the room when it moves by 3 ms or more.
- The game's rooms and the `daytona-link` helper's no longer mix (error
  `room_kind`): the helper passes raw link bytes. Relay protocol version 4,
  online version 7.
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
- **The room's settings are chosen in the game.** The Online screen puts
  **Join a room** first, under the name. **Create a room** asks for the
  server, then **Connect** (a password field shows only once the server
  asks for one: a room is already private by its random code, so servers
  are meant to run without one): the server answers its default
  race settings (`op=info`), and the room's settings (cabinet, country,
  difficulty, game mode, rival arrow, advertise sound, free play, who may
  start) become editable; **Create the room** sends them (`op=create
  set=...`) and the game remembers them for the next room. The server's
  `DAYTONA_*` race variables are now the defaults. The server's web interface
  is removed (with `DAYTONA_WEB_PORT`, `DAYTONA_PUBLIC_ADDRESS`,
  `DAYTONA_WEB_PASSWORD`): it only publishes TCP port 7777. Relay protocol
  version 3, online version 6; `m2run --relay-settings KEY:VALUE,...`.
  The server and every player's game are updated together: a version-2
  game is refused by this server.
- **Rooms of up to 8, started by their owner.** No player count to choose:
  friends join, the room's owner (its creator) presses **Start the race**, and everyone then in the room
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
  it now also describes the name tags over the cars,
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
- `tests/test_link_codec.cpp`: the link codec's round trip on synthetic
  frames shaped as the comm board's, and malformed messages refused.
- `m2run`: `M2_TEST_RELAY_SYNC=on` holds frame sync on for a whole relay
  session, `M2_TEST_RELAY_BARRIER=0` turns the start barrier off
  (comparisons); `M2_TEST_STALL_AT=FRAME M2_TEST_STALL_MS=MS` stalls once,
  as the game's Esc pause does; a failed relay connection is printed with
  its frame.
- `tests/test_car_tags.cpp`: the tags' place from synthetic polygons (the
  car records' number byte, projection, widescreen origin, what hides a
  car) and the game's picture in the window.

### Known limits

- Not yet tested between players on different networks.
- Headless test cabinets must run at the arcade's pace (`m2run --realtime`)
  for large rooms: free-running ones at different speeds flood each other
  and lose the link.
- Rooms of 4 players or more with long round trips (4 players through a
  relay in Europe from Brazil, ~230 to 310 ms each) do not get past the
  game's own network check at boot: the cabinets restart the link every
  ~14 s and never reach attract. 2 players on the same path play.
  At the arcade's pace 8 cabinets through the relay link and stay linked
  (two runs of 104 s and 157 s, no link lost before the end).
- Traffic: compressed, about 0.1 to 0.3 Mbit/s each way per player (2 to
  8 players, measured on captured sessions); the server carries the sum.
  Before compression it was 3.3 Mbit/s whatever the number of players.
- The relay is not encrypted: the server password travels in clear, is in
  the invite code and is stored in `launcher.ini`.
- A brief network drop restarts the room's race.
- Name tags: whether a car is hidden is judged from the centres of its 8
  largest faces against the opaque polygons drawn before them, so trees and
  fences (cut-out textures) never hide one, and a car seen only by an edge
  can lose its tag. Checked on headless frames (m2run, 3 and 4 cabinets,
  widescreen too), not yet in a race between players.
