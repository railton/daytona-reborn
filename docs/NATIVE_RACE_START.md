# Native race-start boundary

The PC home submits `StartRaceRequest{RaceCourse}` to `GameLoop::request_race`.
It no longer knows `PcSolo`, EEPROM offsets, the Free Play assignment, guest
selection steps, or the model's Start/pedal pulse schedule. `race_status()`
reports an application state without exposing guest addresses.

This follows the design document's **Audio, inputs, force feedback, link play**
boundary: board input protocols remain native runtime code. The new request
service changes ownership, not the original race rules or simulation clock.

## Lifecycle

- Load the validated Revision A images and saved settings into a fresh GameLoop.
- Submit the request before the first simulation tick. The guest is still
  booting; waiting for attract before preparing compatibility settings would
  change the established initialization sequence.
- The runtime validates the course and rejects an already configured network.
  The compatibility adapter prepares SINGLE / Free Play once, before boot.
- The runtime advances startup inputs once per simulation tick, preserving
  the 30-tick delay, 24-tick repeat and five-tick confirmation window.
- Identical pending requests return `AlreadyPending`; a different course
  returns `ConflictingRequest`. Neither resets counters nor rewrites settings.
- The confirmed course transfers input control back to the player. Wrong
  course and timeout are latched terminal errors.
- Cancellation abandons startup at a frame boundary. It releases future
  startup inputs; it does not roll back or reset the already-running guest.
  Retrying requires a new GameLoop. Cancellation cannot stop an active race.
- Returning to attract after a confirmed race reports `Finished`.

The compatibility launcher remains available. No instruction is patched and
no live gameplay RAM is written to jump past selection or the boot sequence.

## What this establishes and what remains

This is a functional PC intent interface over a minimal compatibility adapter.
The EEPROM settings and guest selection state machine still exist internally.
They have not been proven removable while preserving physics, AI, sound,
timers and input timing. A direct guest entry-point experiment must compare
those effects against the current path before it can replace the adapter.

The synthetic `race_session` test exercises three courses, repeated and
conflicting requests, invalid enum values, active races, exact confirmation
edges, the 1,500-tick timeout, cancellation and wrong-course detection. It
compares 2,100 generated input ticks with the pre-existing `PcSolo` adapter.
It validates the request wrapper, not original-hardware equivalence. Real-ROM
replays and graphical checks are recorded separately in the handoff.

## Validation on Windows, 2026-11-10

Built from `80d2c3a` with Visual Studio 2022 / ClangCL, using the existing
generated Revision A sources read-only. `race_session` passes under ClangCL
and a separate MSVC 19.44 build. Its checks remain active in Release.

Three 1,400-tick headless runs were compared with the preserved pre-change
executable. Each uses the default player inputs after startup. The start tick,
i960/TGP/68000 instruction counts, interrupt count, sound command count and
drive command histogram match. Six RGB frames per course (1,300 through
1,400, every 20 ticks) have identical SHA-256 hashes: 18 matches out of 18.

| Native course | Start tick, before and after | Final screen hash |
| --- | ---: | --- |
| Beginner | 837 | `8ac3c65c1383c1ee` |
| Advanced | 1156 | `251a01fb90318c5b` |
| Expert | 1157 | `6a41cb6273447e9f` |

The reference executable SHA-256 is
`ba813b83c13a4e39d68ac7b5e10fcc0a93f832b422d2992e82819b6d25e4f161`;
the new `m2run.exe` is
`0d1efafce06706b770d1f447b2e7cce11b98feaa9fb15e92039dc9b0b9feba84`.
Local logs, binary provenance and frame comparisons are retained under
`build-race/validation`; game-derived frames are not committed.

Reproduce with local, importer-validated images, creating the output directory
first, and repeat for courses 2 and 3 with distinct output directories:

```powershell
ctest --test-dir build-race -C Release -R '^race_session$' --output-on-failure
New-Item -ItemType Directory -Force build-race/check-1
build-race/Release/m2run.exe <images-dir> 1400 --pc-solo 1 --dump build-race/check-1 --dump-from 1300 --every 20
```

The desktop D3D12 application also completed `--start-solo 1 --frames 1000`
with exit 0, using a separate `verification-native-intent` profile. That exit
requires the requested course to have reached Racing. A 3440x1440 home capture
exited 0 and was visually inspected: the menu, course selector and diorama
remain visible and correctly laid out. The capture is a single rendered frame,
not evidence of animation cadence or physical mouse/controller navigation.

```powershell
build-race/Release/daytona.exe --rom <local-archive> --gpu direct3d12 --window 1440x810 --profile verification-native-intent --start-solo 1 --frames 1000
build-race/Release/daytona.exe --rom <local-archive> --gpu direct3d12 --window 3440x1440 --profile verification-native-intent --capture-home build-race/home.bmp
```

These checks establish unchanged startup behavior and the sampled early-race
output. They do not establish full-race physics parity, real force feedback,
perceived audio quality, input latency or a presentation frame-rate target.
