# Changelog

Changes in this fork, on top of Daytona Reborn beta 0.10 (`9d0eccc`). The
fork's version is in `VERSION`, raised with every code change (rules.md
rule 14); each version has its section here.

## Unreleased (version to be decided)

### A custom body for the player's car (experimental)

Settings > Enhancements > Player's car draws a body from the data folder's
`cars/NAME/` (or `M2_PLAYER_BODY=FOLDER`) in place of the player's car body,
with the hardware renderer (docs/CUSTOM-CARS.md). Only the look changes; the
game's wheels, light, shadow and physics stay, and online the others see
the game's car.

- The body's faces are lit by the game's own light: each frame the game
  car's faces fit luma = a + w . n (their object-space normals and the luma
  the game gave them), which lights the new faces by their normals. They
  keep the game car's identity, so smooth shading, the sky reflection and
  the car shadow (its own outline) apply too.
- Surface roles: 6 glass (15 % reflection, the sun's glint too), 7 and 8
  matte (wheel wells, grille mesh, spoilers). The game's own window
  reflection (an overlay of oba 4, texture 0x4409/7064 on the glass's
  projection) is dropped with the glass: it made the new glass a mirror.
- The game's wheels are drawn 10 % bigger with a custom body, standing on
  the same ground, so they stand proud of the new body as on the game's car.
- Up to 32768 faces for the player's body (opponents: 1024); its faces turned away from the camera still cast the shadow (the sun sees them).
- Its own wheels (optional `wheel.m2car`, `M2WHEEL1`): one wheel in the
  game wheel's space (hub at 0, axle along x, outer face +x), drawn on each
  of the game's four wheels through their matrices, so they spin and
  steer; mirrored for the left, raised to stand on the same ground, its
  textures the atlas's second body.
- A car of another size: `wheel.m2car` may start `M2WHEEL2 FRONT_Z REAR_Z
  HALF_TRACK`, which draws the wheels on that car's own axles (body space)
  instead of the game car's; only the drawing moves. With the game's axles
  (1.4135, -1.399, 0.525) the frames match M2WHEEL1's (a few hundred
  pixels differ: the game's left and right hubs are 1.6 mm apart). Checked
  with a real-size 1960s Beetle (wheelbase 2.40 against the game car's
  2.81): 15,840 faces and a 1,180-face wheel, game + renderer CPU median
  5.9 ms at 3440 x 1440 with every image option.
- The mission select screen's two turning cars (not keyed: known by their
  ROM objects) take the body and wheels too, each lit by its own game light
  over the brightest's, so the car not chosen stays dimmed as the game's.
- The cockpit view (VR2): the game draws its own model of the bonnet and
  dashboard there (ROM object 0x94fff9, keyed as the body, the camera
  inside the car). Its bonnet and wings are painted with the new body seen
  from above (per point, the surface of the body's highest face, its
  texture fitted as a plane over x and z); the dashboard stays.
- Faster: smooth car shading grouped the corners in a hash of vectors, one
  per corner, each frame; it now sorts them, the same light to the bit (the
  frames byte-identical with and without a custom body). A face turned
  from the camera is kept only when it faces the sun (it only casts the
  shadow), and none without car shadows. The custom body's and wheel's
  smooth corner normals are made once, when they load (they do not change
  shape), and turned by the car's matrix each frame; their light is the
  game's light fit at those normals. 3440 x 1440, MSAA 4x, shadows,
  reflections, bloom and CRT, 3,600 frames of a race: game + renderer CPU
  median 22.4 -> 7.9 ms (p99 36.0 -> 11.4 ms) with the test body, now
  20,869 faces and four 1,679-face wheels (were 27,731 and 3,289); 1.2 ms
  with the game's car.
- Checked with a 20,869-face body made from a third-party model (kept local,
  its licence's): no flicker between consecutive frames, the shadow with its
  outline, the numbers upright from every camera.

## 0.21.0 (2026-11-25)

### Fixes from an external graphics review

- **High-resolution textures fall back safely:** a failed upload of the
  pack's index (a transfer buffer that could not be mapped) counted as done
  and the texture RAM as checked, so the shader could read an empty or stale
  index. Now the replacement is asked for only once this texture RAM's
  index is on the GPU; until then the game's textures are drawn and the
  upload is tried again next frame. Checked with `M2_TEST_HIRES_INDEX_FAIL=3`
  (a test hook): the first three frames equal the pack-off frames, the rest
  the pack-on frames, byte for byte.
- **`m2gpushot` reads its options in any order:** `--smooth-2d` was missing
  from its switches, so placed before `--dump DIR --every N` it failed, and
  elsewhere it could swallow an option. Each option now takes its own
  argument count; the switch first and last give identical frames (21).
- **High-resolution detail fades in:** the shader switched from the game's
  texel to the upscaled one where a texture's footprint reached one texel:
  a line across the road, fixed on screen, where the grain began (6 to 9 %
  of the pixels a frame). Across the last quarter before it the two now
  blend. 3840 x 2160, MSAA 4x, pack on: renderer CPU 0.16 -> 0.19 ms.
- **Smooth menus and HUD at the screen edges:** with "HUD at the screen
  edges" the front layers are composed on the CPU and drawn as a texture,
  without the smoothing; they now take the same colour vote, their edges
  blended. With smoothing off that path is unchanged.
- **Opponents' shadows:** a car's faces went to the near or far cascade
  each by its own depth, so a car crossing 40 units had its shadow split
  between two sharpnesses; now a whole car goes to one cascade.
- **The pack's index refresh:** each texture RAM change hashed every entry
  again (124 refreshes in 9,000 frames, in bursts of 31 to 61 frames at the
  loads, 4.8 ms each). A copy of the RAM as last checked now tells which
  32 x 32 cells changed and only the entries over them are hashed: 0.46 ms
  on average (max 1.4 ms), 7 of 360 entries; a full check only at the
  pack's first frame. Test: after 50 rounds of random writes the index
  equals a full rebuild. `M2_HIRES_TRACE=FILE` logs each refresh.
- **`m2gpushot --bench` reports the slow frames:** the median, p95, p99 and
  maximum of the game's frame plus the renderer's CPU work, and how many
  frames took over half a frame. Attract race, 9,000 frames, 1920 x 1440:
  pack off median 0.82, p95 1.23, p99 1.41 ms; pack on 0.88, 1.25, 1.40 ms.
- Tried and left out: snapping the shadow cascades to whole texels and
  quarter-octave sizes did not steady the shadows' edges (11.8 % vs 11.9 %
  of the shadow's pixels changing a frame, 61 frames of the race): the
  change comes from the cars and the camera moving.
- **The race's speed check** (whether the graphics card kept up) also runs
  with only sharp car textures, smooth menus and HUD, the texture pack,
  bloom or the arcade monitor on.

### Windows build fixes

- Two tests used `std::llabs` and `std::puts` without their headers
  (`<cstdlib>`, `<cstdio>`): llvm-mingw's libc++ stopped there on Windows 11
  (GCC pulled them in). Every target cross-built with llvm-mingw 20261117.
- `Build Daytona.bat` writes the build's own output (CMake, the compiler's
  errors, Python) to `windows-build.log`, which PowerShell 5.1's transcript
  left out; names the ROM set file in full; retries its downloads (three
  tries, then Windows' curl.exe) after "the decryption operation failed".

### The home screen says when a newer version is out

Once a day the game asks GitHub for the project's latest release, through
the system's curl; nothing about the player is sent. When the release is
newer than the build, a card on the home screen shows its version, a button
to its release page, and what to do: update the copy of the source (git
pull, or download it again) and build the game again; games are never
offered for download. Dismiss hides it until the next version. Settings >
Game > Updates turns the check off. Offline or without curl it stays
silent (a log line). A fork points it at its own repository with the CMake
option DAYTONA_UPDATE_REPO (owner/name; empty turns it off). Checked
against the real API (the latest release, v0.18.0); new test update_check
(semver, the release's fields, the daily limit). M2_UPDATE_CHECK_VERSION
pretends another version, for a capture.

### High-resolution textures: made from the game (experimental, Linux)

Settings > **AI textures** makes the high-resolution texture pack on this
computer and uses it, with no command line (docs/HIRES-TEXTURES.md, which
now lists the tools it needs). Marked as a feature in testing.

- The tab shows what the computer has (an NVIDIA card through `nvidia-smi`,
  Docker usable by the user, the NVIDIA Container Toolkit), then makes the
  pack in the background with its progress: the capture (`m2texcap`, which
  now reads the ROM set's zip and prints its progress), the upscaling
  container built by Docker from `hires/Dockerfile`, the upscaling, the
  pack checked and saved as `hires.m2tex` in the data folder. After a
  success the tools are removed (the captured textures, the models, the
  container image, and its base image unless it was there before).
- The processor can do it without a card ("On the processor": about 30
  minutes on 20 threads, estimated from a 3 % sample).
- Every command, its output and exit code go to `hires-textures.log` in the
  data folder; a failure says which step failed and names the log; the
  tools are kept for the next try.
- "Use the high-resolution textures" loads or drops the pack at once;
  `M2_HIRES_TEXTURES` still wins. `daytona --make-hires-pack` runs the job
  without the window.
- The build copies the kit next to the game (`hires/`: the script, the
  textures' kinds, the Dockerfile, the capture's session).
- Measured on Linux with an RTX 5090, the base image already there: the
  whole job 1 min 57 s, a pack of 360 textures (84 MB) with the default
  cabinet settings (the 410 earlier came from another EEPROM's settings).
- The pack reader also refuses sizes that are not powers of two; new test
  `hires` (the reader, the index's slots, cut-outs, refused files, the
  maker's progress lines).
- Windows and macOS: not yet (the plan: portable tools, as the build's).

## 0.20.0 (2026-11-25)

### Smooth menus and HUD

Settings > Enhancements > **Smooth menus and HUD** (hardware renderer, off
by default): the game's 2D layers (menus, the select screens, the
speedometer and lap times, results) magnified with smooth curves and
diagonals instead of square pixels.

- Each output pixel takes the four logical pixels around it (a cell); each
  distinct colour weighs as much as its corners' bilinear weights. Two
  contrasting colours (a channel 96 apart, or a hole): the heavier wins,
  blended over about one output pixel; three with the pair diagonal (a thin
  diagonal line): the same; three with the pair side by side: a straight
  edge, so T junctions stay square; a checkerboard (the game's dither),
  shades of a hand-drawn gradient and four colours (photographic art):
  nearest. No colour is invented. The front layers' edges over the 3D are
  blended (their pipeline now blends; a pixel of alpha 1 replaces the
  target as before).
- A first version voted in every cell: it pinched thin frame lines at
  their joints (the course select boxes' rivets), beaded one-pixel
  gradient rows and rounded hand-drawn shading into blobs.
- Chosen over Scale4x, a two-colour threshold and an AI upscaler, compared
  on six screens: the AI took 2.5 s a frame on a GeForce RTX 5090 and lost
  the dither; Scale4x left the curves stepped.
- Measured (m2gpushot, attract race, 3600 frames, MSAA 4x): GPU wait per
  frame 0.01 -> 0.04 ms at 3840 x 2160 (the first version: 0.33 -> 1.11 ms
  at 7680 x 4320).
  Off, the frames are unchanged (8 captures compared). Checked in the game
  at 1920 x 1080 in 16:9.
- Not applied to the CPU front layers ("HUD at the screen edges"), nor to
  the stretched backdrop's margins.
- `m2gpushot --smooth-2d`.

## 0.19.0 (2026-11-25)

### High-resolution textures (experimental)

The race's textures upscaled 4x by AI models, from the player's own ROM
set, on their computer (docs/HIRES-TEXTURES.md); the game draws them with
`M2_HIRES_TEXTURES=PACK` (no launcher option yet).

- `m2texcap` captures the textures a session uses (the attract loop, a race
  on each course, scripted sessions through the select screens and game
  over): 410 textures, 263 of them cut-outs.
- `scripts/hires_textures.py` upscales each with the model best at its kind
  (`scripts/hires_classes.csv`, by hash): 4xTextureDAT2_otf for lettering
  and flat areas, 4x-PBRify_UpscalerV4 for rock, foliage and crowds,
  RealESRGAN_x4plus for the road; cut-outs have their holes filled from
  their neighbours and their opacity upscaled too. `docker/hires-textures`
  runs it (PyTorch, CUDA 12.8). Under 2 minutes on a GeForce RTX 5090.
- The shader reads the upscaled tones where a texture is magnified and the
  texture RAM holds its texels (hash checked when texture RAM changes, 2 to
  5 ms), through the game's own luma and colour; cut-outs follow the
  upscaled opacity (alpha to coverage with MSAA). An index cell holds up to
  4 textures: 36 of the 361 places the race uses start more than one.
- Measured (3840 x 2160, MSAA 4x, 3600 frames): 294 of 406 textures in use,
  15 to 24 % of the pixels changed, renderer CPU time 0.16 -> 0.30 ms per
  frame. Without a pack the frames are unchanged.
- Known limits: the 2D layers (menus, HUD, results) are not covered; the
  pack is made per computer and never shared.

## 0.18.0 (2026-11-25)

### Online server: the course and the gearbox are the MASTER's

The relay passed every player's control messages to the others, and the
games could not tell who sent them, so a modified game could choose the
course or the gearbox for the room. The relay now passes those messages
(`cursor=`, `course=`, `gearcursor=`, `gearbox=`) only from the race's
MASTER (the ring's first); standings and arrival from anyone. The games are
unchanged (`kOnlineVersion` stays 10); the relay needs updating. Checked: a
relay test (from another player they are dropped, from the MASTER they
arrive; it fails without the change) and two headless cabinets on the new
relay, both on the MASTER's course.

### Image enhancements

- **Car shadows in two cascades**: the near cars (up to 40 camera units) in
  one half of a 4096 x 2048 depth map, as before, the others up to 140 in
  the other half, each fitted to its own cars: the opponents ahead cast
  shadows now, and the player's car keeps its detail (a single map out to
  140 made it blocky).
- **In place of the game's own shadow** (with car shadows, on by default):
  the game's checkered shadow under the player's car (part 0x78b4, the only
  checkered car part in 17 race frames surveyed) is left out while the
  cars' shadows are drawn.
- **Bloom and monitor strength**: sliders (10-100 % and 20-100 %).
- **The race's check**: in the session's first race with an image
  enhancement on, the frames drawn are counted from 3 to 13 s after the
  start; under 92 % of the game's rate the player is told a lower quality
  may help (measured: 14.5 frames/s at Maximum on Vulkan in software).
- The log says when the graphics card has fewer MSAA samples than asked.
- `m2gpushot --car-shadows-and-game-shadow`.

With every enhancement off the frame is unchanged (13 captures compared).

## 0.17.0 (2026-11-25)

### Image enhancements (hardware renderer, all off by default)

Settings > Enhancements > **Image quality**. With every one off the frame is
the one before, pixel for pixel (13 race and attract captures compared).

- **Antialiasing (MSAA 2x, 4x, 8x)**: the polygons' edges, and the cut-out
  textures' (trees, tyre marks) through alpha to coverage. Applies the next
  time the game starts; not with the ray-traced sun shadows.
- **Texture filtering**: anisotropic 2x to 16x, in the polygon shader: the
  road and the walls stay sharp into the distance without shimmering.
- **Smooth car shading**: each corner of a car's face takes the light the
  game gave the faces around it (creases over 50 degrees stay sharp), blended
  across the face.
- **Paint reflections**: the cars' bodies reflect the course's own sky (the
  back tilemap layers, drawn each frame into a small texture whose mip
  levels blur it) and the ground, more at grazing angles, and the game's sun
  as a highlight.
- **Sharp car textures up close**: numbers and logos keep crisp, rounded
  edges instead of the bilinear blur.
- **Car shadows**: the cars near the camera drawn from the game's sun into a
  2048 x 2048 depth map; the road and the walls look it up (3 x 3 samples).
  Any graphics card. The game's own shadow under the car is still drawn.
- **Native resolution on Vulkan** (Linux): it fell back to the arcade's
  resolution outside Direct3D 12.
- **Maximum quality (graphics card)**: one button for all of the above.

### Screen effects

Settings > Enhancements > **Screen effects**, both renderers, applied as the
frame is shown (any resolution, widescreen or 4:3):

- **Bloom**: the bright parts glow, from a quarter-size blurred copy.
- **Arcade monitor (CRT)**: a beam per game line (384), wider where
  brighter, an aperture grille and darker corners; faded out where the
  window has fewer than about 3 pixels a line.

### Automatic quality

At the first start from the PC home, and from Settings' **Detect
automatically**, a window measures the graphics card and recommends a level
(Original, Medium, High, Maximum); the player picks one or keeps the
settings. Each level's stand-in (a tilted textured plane through the game's
own polygon shader, with the level's MSAA and filtering, nothing from the
ROM) is drawn at its resolution; a level fits under 8 ms a frame. Calibrated
against the game: at 7680 x 4320 with every option the stand-in takes
3.5 ms, a race frame at most 2.1 ms (RTX 5090). Measured: RTX 5090 Maximum at
1080p, 1440p and 4K (0.9, 1.6, 3.6 ms); Vulkan in software (llvmpipe) Medium.
Bloom and the CRT are never changed.

### Fixed

- A solo started from the home menu was silent on the cabinet's course and
  gearbox screens: the sound stayed off until the race started. It is off
  now only while the game boots behind the home menu (measured: silent from
  0 to 35 s before, heard from the course screen on, 5 s after Play).

### Tools

- `m2gpushot`: `--msaa`, `--aniso`, `--smooth-cars`, `--car-reflections`,
  `--sharp-car-textures`, `--car-shadows`, `--bloom`, `--crt`, `--window`,
  `--pixels WxH` (the 3D at that size, as the native mode) and
  `--quality-probe WxH` (the automatic quality's measurement).

## 0.16.0 (2026-11-24)

### Online server: abuse limits

A security review of the relay found that a player in a room could push the
others out, and that a few addresses could hold every room. The relay now
limits (`server/daytona-relay/cmd/daytona-relay/guard.go`, "Abuse limits"
in its README):

- **Each connection's traffic**: 400 frames, 1 MB and 20 control messages a
  second; a game sends about 120 frames and a few control messages. Before,
  a player flooding control messages filled the other players' send queues
  and the server disconnected *them* (a player on a 1 Mbit/s line was out
  in 0.9 s); now the flooder is disconnected (error `flood`) and the race
  goes on. Each player's datagrams likewise (the excess is dropped).
- **Rooms**: 8 open from one address (one address held 16, so 7 addresses
  could take all 100); a room whose creator waits alone 30 minutes, or that
  goes 2 hours without a race, is closed (error `idle`). The games' link
  data counts as activity, so a race is never cut.
- **Guessing**: 20 failed attempts a minute from one address (a room that
  does not exist, a wrong password; error `slow_down`); one address tried
  4,187 room codes a second before.
- **Datagram addresses** are taken only from the address of the player's
  own connection (before, anyone with a player's key could point that
  player's datagrams at another machine), and forgotten when the player
  leaves.
- Settings: `DAYTONA_MAX_ROOMS_PER_IP`, `DAYTONA_ROOM_ALONE_MIN`,
  `DAYTONA_ROOM_IDLE_MIN`, `DAYTONA_JOIN_TRIES_PER_MIN`.
- The container: at most 256 MB of memory and 128 processes; Go 1.27.4,
  pinned by digest (1.24 no longer gets security fixes).
- Checked: the relay's decoder and wire parsers fuzzed (53 million inputs,
  no crash), `govulncheck` (nothing that applies), the attacks above
  repeated against the new relay.

### Online: a lighter relay

- **The relay's codec is 6 times faster** (8 bytes compared at a time, a
  reference given up once it has lost, its buffers reused): 28 to 4.3 µs
  to encode a block, decoding about 6 times faster; the bytes are the same
  as the game's (its test against the game's codec, fuzzed 45 s more).
- **A frame's messages share one datagram**, in the game and in the relay
  (before, the block and the token went in two, each repeating the ones
  before), and the relay sends its datagrams once its lock is released.
- Measured, 8 headless players on a local relay, a session each: the
  relay's CPU 9.4% to about 5.5% of a core, what it sends 0.63 to 0.46
  Mbit/s a player (a full room about 1.7 GB an hour instead of 2.3), the
  other cars as smooth (99.1-99.9%). Synthetic load on 2 CPUs: about 35%
  less CPU at 5 rooms; it still saturates near 20 rooms (the codec is no
  longer the limit there).
- The repeats each datagram carries stay at two: with loss in bursts the
  worst player was 98.1% smooth with them and 96.2% without (with random
  loss they hardly matter); `DAYTONA_UDP_REPEAT` (and the game's
  `M2_PROTO_UDP_REPEAT`, diagnostics) lower them for less traffic.

The game shows its own text for the four new errors, in the player's
language. The online version stays 10: 0.15 games play with 0.16 games and
with the new relay (they show the server's English text for the new
errors).

## 0.15.0 (2026-11-24)

### Online play

- **The start no longer looks frozen.** Each game waits for the others at
  the start so that everyone leaves together; the panel saying so was never
  drawn (the interface was drawn over a race only with the players list or
  the name tags showing), so the wait showed as a frozen picture. Now a
  panel says **Waiting for the other players**, who is still missing, and
  the seconds left. The longest wait is **15 s** (it was 30): then the race
  starts, and a late player joins it as their game arrives.
- **Esc no longer stops an online race.** Opening the menu paused the
  player's game: it stopped answering the link, the others' games froze
  waiting for it, and after about 5 s its connection gave up, which ended
  the race for everyone. Online there is no pause now: behind the menu the
  car runs on without its driver, as on linked cabinets.
- **The MASTER chooses the gearbox for the room**, as the course: the other
  games' gearbox screens follow its cursor and confirm when it confirms, so
  nobody waits for anyone there. Linked cabinets show the gearbox at step 5
  of the selection (a cabinet alone at step 7): keys and gamepads could
  only ever pick its first option there; they pick both now.
- **The cabinets' test menu is locked online** (F2, and the service
  switch): a player opening it left the link (in a test, the other game
  went 62% of the race without new data) and could change what the room
  set. The coin switch stays, for rooms set to coins.
- **A chime when a player joins the room**, on the room's screens (not over
  a race), at the settings' effects volume: two notes synthesised by the
  game.
- `m2run --relay-invite DTN1-...` joins a room with its invite code;
  `M2_TEST_GEARBOX=manual` (the MASTER's), `M2_TEST_SELECTION_TRACE=1`.

Online version 10: 0.14 games and 0.15 games do not share a room. The
relay's protocol is unchanged (5): servers need no update for games to play.

### Logs

- **Generate logs**, in Settings > Game, on by default, with the logs
  folder and **Open the logs folder**: the game keeps **daytona.log** on
  Windows, Linux and macOS (before, only Windows wrote one; Linux and macOS
  still print to the terminal too), and the five sessions before it as
  daytona.1.log .. daytona.5.log, so a player can send the log of the
  session where something went wrong.
- What it holds besides: the system (cores, memory, GPU driver, renderer);
  online, with the time in UTC, who joined and left the room, drops and
  returns, why a connection ended, the wait at the start and who came last
  (or never), and a line for each online race: its game speed, how smoothly
  the other car's data moved (smooth, no new data, skips), the link's
  reserve, sends and messages a second, what came first by UDP, the time in
  the menu, and every player's latency.
- **The relay's log** (no protocol change): times in UTC with
  milliseconds; each race gets a line per player every minute and when it
  ends: the game messages received (and how many a second: a game short of
  data sends fewer), how many came first by UDP, those sent, its latency
  (min/avg/max), its send queue at its longest, its drops and returns.
  Docker keeps about 100 MB of it (it kept 30). Set against the players'
  logs by room, name and time.

### Home screen

- **Solo as at the arcade**: the Solo card sets the cabinet's **Country**
  (USA English, the default; Japan Japanese; Export) and **Difficulty**
  (Easy to Hardest, Normal by default), kept for the next time, and
  **Play**. The game starts the cabinet and, from its selection screen on,
  shows it and hands it to the player: the course, the gearbox, and Time
  Attack (Start held while confirming the gearbox) are chosen on the
  game's own screens. Solo used to fall into Japanese: Revision A's factory
  settings are Japan's. `--solo` (tests, captures) still answers the
  screens itself.
- The Online card: the invite code and **Create a new room**.

### Building

- **`Build Daytona.command`: the macOS build in a double-click**, as
  `Build Daytona.bat` on Windows (`scripts/macos-build.sh`; Apple Silicon
  or Intel). It checks Apple's command line tools (the compiler, the SDK,
  git, python3; macOS offers to install them when missing), downloads
  portable CMake 4.5.1 and Ninja 1.13.2 into `.tools/`, each checked
  against its pinned SHA-256, asks for the ROM set in a file window, fetches
  the libraries, recompiles and builds, and puts a **Daytona (recomp)** app
  on the desktop. No Homebrew, no administrator rights; `--update` pulls a
  git clone first; everything goes to `macos-build.log`. Tried on a Mac,
  with the Windows build alongside.

### Known limits

- With 6 players through a relay in Europe (headless), the game ran at
  96-97% of its speed and the other cars' data moved smoothly in about 90%
  of the frames (3 players: 100% and 98%): games other than the MASTER's
  run short of link blocks. Under investigation; the new logs measure it.

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
