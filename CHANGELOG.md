# Changelog

Changes in this fork, on top of Daytona Reborn beta 0.10 (`9d0eccc`). The
fork's version is in `VERSION`, raised with every code change (rules.md
rule 14); each version has its section here.

## 0.14.5 (2026-11-24)

### Fixed

- **Windows: paths with accents open.** A ROM set in a folder such as
  "Área de Trabalho" was refused ("cannot open"): the game keeps its paths
  in UTF-8, but Windows read them in the system's regional code page. The
  game's executable now carries a manifest that makes its code page UTF-8
  (`src/app/windows/daytona.manifest`; Windows 10 1903 or later), for every
  file it opens: the ROM set, its settings, translations and log (a Windows
  user name with accents too). MinGW builds (`Build Daytona.bat`, the
  Docker cross-build) embed it through `daytona.rc`, Visual Studio builds
  take the manifest itself.

## 0.14.4 (2026-11-24)

### Home screen

- **Room details in the neon look**, as the room's settings: a panel over
  the dimmed speedway with the room's count, its invite code and **Copy**;
  a row a player with their car's colour and number, the owner's crown,
  **(you)**, signal bars and the latency in green, yellow or red; who waits
  for the next race apart. The main button is **Start the race** (the
  owner), **Resume the race** (during one) or waits; **Leave the room** is
  lit red, **Back** dark; **New race with everyone** for the owner during a
  race. Esc goes back into the race when one is on. The same screen shows
  while connecting to a room.
- The dark buttons' text no longer shrinks for a play sign they do not
  have (**Back** on the room's screens).
- `M2_CAPTURE_ONLINE_ROOM=1|race` shows the room's details with example
  players, for a `--capture-home` image. The online guide shows it.

## 0.14.3 (2026-11-24)

### Changed

- **Example names are generic**: the capture diagnostics, the tests and the
  relay's protocol comments use "Alex" where a real name was.
- `M2_CAPTURE_HOME_ONLINE=solo` shows the Solo card with a name and a server
  that answered; `M2_CAPTURE_ONLINE_CONNECTED=1` fills the room's settings
  with an example name and server (shown, not saved). For the README's
  pictures.
- **README: new pictures** of the home screen (Solo, Online, in a room),
  the room's settings and Settings, in `docs/images/`.

## 0.14.2 (2026-11-24)

### Fixed

- **0.14.1 drew the old Settings over the home screen** at every start:
  the line giving Settings the home screen's fonts split the chain that
  picks the screen, so the launcher was drawn whatever else was shown. It
  now comes before that chain.

## 0.14.1 (2026-11-24)

### Home screen

- **Settings in the same neon look** as the room's settings: a panel over
  the dimmed speedway (a paused race stays behind it), the title with a
  gear, the language at the right, one column that scrolls, and **Back**
  at the panel's foot. The six tabs and their settings are unchanged; the
  cabinet launcher outside the PC home keeps its look.

## 0.14.0 (2026-11-24)

### Home screen

- **A new home screen, in a neon look.** Two cards of the same size, tabs:
  **Solo** (game mode, gearbox and course as choices that light up, then
  **Play**) and **Online**. Each card has its own emblem (a speedometer, a
  turning globe with players linked across it), drawn by the game.
- **Online from the home screen.** The Online card has the invite code
  (pasted on its own when the clipboard holds a valid `DTN1-` code; long
  codes shown shortened) and **Create a new room**; **Join the room** lights
  up once a code is there. In a room the card shows its eight places in the
  cars' colours, the owner crowned, and what the room waits for, with **Copy
  the invite code**, **Room details...** and **Leave the room**; the main
  button becomes **Start the race** for the owner, **Resume the race** during
  one. Play on the Solo card while in a room asks first (**Leave the room?**).
- **The room's settings screen**, redrawn in the same look: the player's
  name (needed before **Connect**), the server, then the eight race settings
  as rows of choices instead of lists; it connects on its own to a saved
  server when a name is set, and keeps each choice at once. The name, the
  invite and the introduction no longer repeat what the home screen shows.
- **At the top right**: the Online state (Online, Offline, Other version,
  Testing, In a room), the player's name (▾ changes it; asked once, the
  computer's user name suggested) and the latency, beside **Settings** and
  **Quit**. The home screen asks the saved server every minute (`op=info`),
  and the bottom bar says **ONLINE READY** or what is missing (a name, a
  server, the server not answering, another version).
- `M2_CAPTURE_HOME_ONLINE=online|room` shows the Online card (a code pasted,
  or in a room of three) for a `--capture-home` image.

### Known limits

- The keyboard and gamepad navigation test (`test_home_menu_navigation`,
  updated for the new order: the choices, then Play) builds on Windows only
  and was not run for this version.

## 0.13.0 (2026-11-24)

### Online play

Six changes to the link, measured one by one and together against 0.12.0
(4 players; `docs/ONLINE_PROTOCOL_STUDY.md`), all on by default:

- **Star mode on the server.** The relay reads the games' compressed
  blocks and sends each player, in one hop, every other car's latest data,
  instead of the data passed from player to player round the ring. All
  cars are equally old, whatever their place. Through the relay in Europe
  from Brazil: the farthest car 1.0 s old before, 0.4 s now; the other
  cars' data moving smoothly 85-87% of the frames before, 91-93% now.
- **The race data also goes by UDP.** Every message is numbered and sent by
  TCP and by UDP at once, each datagram repeating the two before it; each
  side takes it from whichever path brings it first. A lost packet is
  covered a frame later instead of after TCP's retransmission. 1% packet
  loss: smooth 82-85% before, 99% now, and the game at full speed instead
  of 91-96%. Where UDP is blocked the game uses TCP alone, on its own.
- **A dropped player comes back to the race.** The server keeps their
  place 10 s and the game reconnects on its own with its key; the race goes
  on for everyone (before: the race ended for the room).
- **The link's reserve adapts to the line**: it grows when frames went
  without a block, shrinks when there was always one to spare.
- **One send a frame**: the block and the link's token together, sent at
  once: 58 packets a second a player instead of 116 (Wi-Fi, mobile).
- **The start barrier lets everyone go together**, from the latencies the
  server measures: the start spread through the relay in Europe 140 ms
  before, 16-72 ms now.

Relay protocol 5 and online version 9: a 0.12 game and a 0.13 server (or
the other way round) refuse each other with the version message. The
server publishes port 7777 by UDP too (`docker-compose.yml`); open it in the
firewall beside TCP. `DAYTONA_STAR=0` on the server and `M2_PROTO_UDP=0`,
`M2_PROTO_COALESCE=0`, `M2_PROTO_RECONNECT=0` in the game (with
`M2_PROTO_ADAPTIVE=0`, `M2_PROTO_BARRIER_TUNE=0` in `m2run`) turn each one
off for comparisons. `m2run --relay` prints the sends a second, the link's
reserve and the messages UDP brought first; `M2_TEST_AGE_LOG` logs the
other cars' data age.

### Known limits

- The connection is not encrypted (no TLS).
- UDP's gain was measured with simulated loss; the route to the relay in
  Europe lost no packets in the probes.

## 0.12.0 (2026-11-24)

### Online play

- **The other players' cars no longer flicker.** With the link paced
  (0.11.0), a received block went into the game's memory on arrival while
  its flag waited for the game's read, so the data the game saw ran ahead
  or behind its flag; and a SLAVE answered each block on arrival, so a burst
  of two sent the MASTER the same state twice, then a gap. The game drew
  the other cars stalling and jumping (seen as flicker). Now each block
  enters the game's memory with its flag, one a frame, and a SLAVE answers
  when its game takes the block. The other car's data moving by exactly one
  step a frame in the race, 2 players at 100 +- 15 ms: MASTER 21% (0.11.0)
  to 95.7%, SLAVE 41% to 99.5%; 4 players 95.9-98.6%; the game's speed
  100% in the race. Seen gone in a race through the relay in Europe.
- **The players' colours are their cars' colours.** The list and the name
  tags used a palette measured from captures read with red and blue
  swapped (violet for the red car 1, orange for the blue car 2). Now: red,
  blue, yellow, green, charcoal, pink, cyan, orange, as the cars are drawn.
- `m2run --relay` prints how the other car's data moved in the race (steps
  of one, frames without new data, skips) and the online trace has the
  game's tick counter; `M2_TEST_RELAY_FLAG_ONLY=1` restores 0.11.0's pacing,
  `M2_TEST_AUTODRIVE=1` taps the pedal through the screens and holds it in
  the race. The app logs each online race's LINK ID, CAR NUMBER and ring
  place.

### Building

- **`Build Daytona.bat`: the Windows build without installing anything.**
  Double-click it (Windows 10/11): `scripts/windows-build.ps1` downloads
  portable tools into `.tools/` (llvm-mingw 20261117, CMake 4.5.1, Ninja
  1.13.2, Python 3.14.10 embeddable, MinGit 2.56.1, about 330 MB), each
  checked against its pinned SHA-256, no administrator rights; asks for the
  ROM set in a file window and copies it to `roms/`; fetches the libraries,
  recompiles and builds; puts a "Daytona (recomp)" shortcut on the desktop;
  logs to `windows-build.log`. `-Update` pulls a git clone first.
  `scripts/setup.py` takes `M2_COMPILER=mingw` (llvm-mingw with Ninja, no
  Visual Studio).
- **A Windows build made on Linux**: `scripts/docker-build-windows.sh`
  cross-compiles `daytona.exe` in Docker with the same compiler
  (`docker/windows-cross`, `cmake/toolchains/llvm-mingw-cross.cmake`),
  reusing the code recompiled by the Linux build. MinGW builds link the C++
  runtime in (`-static`): the `.exe` uses only Windows' own DLLs.
- **The `roms/` folder comes with the project** (a README naming the file;
  git still ignores any ROM set in it), and **the game finds
  `roms/daytona.zip` by itself** the first time, beside its build folder or
  in the working folder: no Browse needed after building.
- **The ROM set is chosen on the first screen.** Without a verified ROM
  set the PC home opens on one screen ("Your ROM set": Choose the ROM
  set..., or its path typed, the language, the refused files listed) and
  nothing else works until it is accepted. Settings' Game tab keeps only the
  set's path and state, with **Change...** to open that screen again. The
  cabinet launcher and the mobile builds keep the full controls.
  `M2_CAPTURE_ROM_GATE=1` shows it in a `--capture-home` image.
- Checked: the five tools downloaded, verified and unpacked by the script's
  own code (PowerShell 7 on Linux; the script parses with no errors), the
  cross-built `daytona.exe` imports no compiler DLL, the ROM found from
  another working folder. Not yet run on Windows itself.

## 0.11.0 (2026-11-24)

The fork's first numbered version: everything since Daytona Reborn beta
0.10.

### Version

- **The version shows on the home screen** (the bottom bar's middle: the
  version, the commit it was built from, "+changes" for a build with
  uncommitted changes, and the online version), in Settings beside the
  title, and in the log at start. It comes from `VERSION` and git at each
  build (`cmake/app_version.cmake`).

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
- **Online rooms of 4 to 8 across continents.** The link comes up at once
  from the relay's room (`CommBoard::preset_ring`: each game's link id
  from its place, `rt::online::link_id`) instead of the FF/FE numbering,
  which waits 232 frames and then a trip round the ring: with 4 players at
  ~230 to 310 ms each the game's own network check at boot failed every
  time; now they reach attract at the usual frame.
- **No slow motion when the network jitters.** The game reads the link's
  flag once a frame against a reference taken by its vblank interrupt; a
  block landing in the vblank tick, or two in one frame, cost it that
  frame's logic tick (4 players at 100 +- 15 ms: the game ran at ~25%, it
  never left attract). Online now holds each block's flag flip for the
  game's own wait, at most one a frame, with 2 blocks in reserve
  (`CommBoard::set_paced`, `rt::online::kLinkReserve`; about 34 ms more on
  the other cars' data): 96-100% of the ticks at 100 +- 15 ms, and 100% in
  the race with no latency (88-100% before). Through the relay in Europe
  from Brazil, 4 players: 97% in the selection, 98-100% in the race,
  started 29 ms apart (without it 53-81% in the race). Link play keeps the
  board as MAME has it. Online version 8.
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
  session, `M2_TEST_RELAY_BARRIER=0` turns the start barrier off,
  `M2_TEST_RELAY_PRESET=0` uses the FF/FE numbering, `M2_TEST_RELAY_PACE=0`
  the board's unpaced flag (`=N`: N-1 blocks of reserve) (comparisons);
  each race's course and the standings at its end are printed; the online
  trace has the game's own tick counter (`tick`); `--link-place N` sets
  LINK ID and CAR NUMBER for link play; `M2_TEST_STALL_AT=FRAME M2_TEST_STALL_MS=MS` stalls once,
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
- Over long distances the other cars are as old as the network makes them
  (one relay hop each way, ~100 to 300 ms, plus ~34 ms of reserve); the
  ring passes each car's data through the players before it, so in a room
  of 8 the farthest car's is older still. A star topology at the relay
  would cut that to one hop.
- Tested headless: rooms of 8 at 100 +- 15 ms, 4 through the relay in
  Europe from Brazil, two races in a row with a course change, a player
  pausing 5 s and one leaving mid-race; not yet with people on different
  networks.
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
