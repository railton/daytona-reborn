# Native session evidence

This complements the design document's **Reference & validation** section.
It checks the application's native session contract. It does not replace a
full-race MAME comparison, validate physics, or establish a race victory.

Generate a private JSONL sidecar with `m2run --pc-solo 1 --race-status-log FILE`.
The journal must begin at tick zero after the native request is accepted.
Transitions must follow the session state machine at increasing simulation
ticks; the sole same-tick exception is cancellation immediately after begin at
frame0, before the first simulation tick. An end record preserves the last state.

```powershell
python tools/validate_race_lifecycle.py JOURNAL --require started --expect-frames 1400
python tools/validate_race_lifecycle.py JOURNAL --require returned --expect-frames 20000
```

`started` requires an observed transition into racing. `returned` additionally
requires a transition from racing to finished: in this contract, finished
means return to attract, not a win. Merely reaching the requested run length
is insufficient. Stopping while racing satisfies only `started`.

The producer is `RaceSession::observe` in `src/runtime/race_session.h`: it sets
Finished only when Racing and `PcSolo::in_attract(board)` is true. Startup
WrongCourse/TimedOut mappings occur only while Starting; `cancel()` likewise
rejects cancellation after Racing. Consequently racing-to-startup-failure logs
are malformed for this producer, rather than valid incomplete native evidence.

Exit 0 means the selected requirement passed; exit 1 means structurally valid
evidence did not meet it; exit 2 means missing or malformed evidence (including
an interrupted journal without its end record). JSON on stdout describes the
result. Duplicate fields, noninteger ticks, illegal transitions and reordered
events are rejected. Logs from a diagnostic run with no native start request
are outside this tool's scope.
Excessive JSON nesting is also reported as malformed evidence (exit2), including
decoder RecursionError; it must not escape as a traceback/exit1.

Combine this with `tools/compare_startup_runs.py` for repeated runs: lifecycle
acceptance alone cannot demonstrate identical counters or rendered frames.
Keep inputs, RGB captures and ROM-derived artifacts in ignored build folders.
Synthetic unit tests need no ROM and run through CTest as
`validate_race_lifecycle` or Python unittest discovery.

## Beginner timeout and return

The input script holds the accelerator after the native solo start and
releases it at tick 6000. Use a fresh private output directory.

```powershell
m2run.exe IMAGES_DIR 10000 --pc-solo 1 --transmission automatic --inputs scripts/inputs/native_beginner_timeout.txt --dump CAPTURE_DIR --dump-from 4000 --every 100 --race-status-log JOURNAL
python tools/validate_race_lifecycle.py JOURNAL --require returned --expect-frames 10000
```

Two Windows Release runs using the same executable produced identical
journals and 61 byte-identical captures each. The guest began racing
at tick 837 and returned to attract at tick 7741. The capture at tick
6500 shows two seconds left in lap 3 of 8. The capture at tick 7000
shows the results and GAME OVER. The journal establishes the lifecycle.
The captures distinguish this timeout from finishing all eight laps.

Put each control assignment on a separate line, even for the same tick
range. The input reader rejects extra assignments rather than ignoring
them. For Beginner, the `racing` event can precede the end of the
rolling start. Validate steering after the visible countdown before
using the script as a driving control.

The first-bend control begins steering left at tick 1500, then changes
direction at tick 1750. At tick 1860 the car is upright in the sampled
image, while the control with the later change at tick 1850 shows a
rollover. Two runs using the same executable match all 41 captures
from ticks 1720 to 2520 and the full lifecycle journal.

```powershell
m2run.exe IMAGES_DIR 2520 --pc-solo 1 --transmission automatic --inputs scripts/inputs/native_beginner_first_bend.txt --dump CAPTURE_DIR --dump-from 1720 --every 20 --race-status-log JOURNAL
```

With the same steering prefix and the accelerator held through tick
9999, the results screen shows a best lap of 23.57 seconds compared
with 26.20 for neutral steering. Both trials reach lap 3 of 8 at tick
6500 and return to attract at tick 7741 after the time expires.

Add `--scenery-log FILE` to record the course grid cell from the last
draw-list update in each captured tick. The 16-by-16 grid uses
`cell = x + 16*y`. The log writes `null` when the hook does not run
or its last observation is invalid. Cell transitions provide a coarse
route comparison. Steering and lap completion still need independent
evidence.

On Windows Revision A, the first-bend scenario matched all 41 RGB
captures and the lifecycle journal byte for byte with the log enabled
and disabled. Its 801 scenery records had one tick without a draw-list
update (1755). It reached cell 121 at tick 1726, compared with tick 1760
for neutral steering. In a second-sector sweep, the same ten observed
cells are reached sooner with a short steering correction from tick
2470. Cell 104 is observed at tick 2999, 48 ticks earlier. Over 10,000
ticks this correction improves the displayed best lap from 23.57 to
23.01 seconds. Both runs still reach lap 3/8, show GAME OVER and return
to attract at tick 7741.

Three trials replayed the steering pulse on the second lap,
shifted by 1480, 1500 or 1520 ticks. All show lap 3/8 at tick 6500,
the same best lap of 23.01 seconds on the results screen and
return to attract at 7741. A fixed shift from the first lap did
not improve this run.

## Capturing a driving replay

Use a fresh desktop profile and choose a new trace path. A capture is
bounded by the requested frame count or the session's return to attract,
whichever occurs first. Keep the course, transmission, ROM revision and
initial settings with the private trace. These values are required for
meaningful playback.

```powershell
daytona.exe --profile race-capture --rom DAYTONA.zip --start-solo 1 --frames 18000 --trace-player-inputs TRACE.csv
python tools/convert_player_input_trace.py TRACE.csv INPUTS.txt
m2run.exe IMAGES_DIR FRAME_COUNT --pc-solo 1 --transmission automatic --inputs INPUTS.txt --race-status-log JOURNAL
```

Use the frame count from the first line of INPUTS.txt. The trace records
the six input bytes supplied to each simulation tick. The native start
adapter applies its own startup inputs in both runs. Compare the replay
lifecycle and sampled frames with the original before using it as
evidence of a finish. Keep traces and captures in private build folders.

For a reference comparison, replace `--trace-player-inputs` with
`--trace-effective-inputs`. This writes the inputs assigned to the native
I/O board at vblank, including the compatibility adapter's startup pulses.
Use one capture option per run. The CSV format and completion marker are
the same. The effective trace does not establish when the arcade program
copied those values into its dual-port RAM.

In two local 1290-tick Advanced controls, 149 samples differ between the
requested and effective traces, all before racing. A read-only native
probe found one tick when the latest dual-port values differ from the board
inputs. Compare reference samples at a matched boundary before claiming
cross-implementation parity.

To produce an M2IN stream, supply a layout recorded by the Daytona Revision A
MAME plugin and a new private destination.

```powershell
python tools/convert_effective_input_m2in.py EFFECTIVE.csv REV_A_HEADER.m2in REPLAY.m2in
```

The converter preserves the header layout, emits a neutral gear selector
for native gear code zero and rejects unsupported inputs. It passes the
Lua codec check using a header built from the published MAME port definitions.
Live playback still needs the Model 1 I/O firmware, matching cabinet settings
and verification of the sampling boundary.

The desktop command above defaults to Automatic. Add
`--transmission manual` alongside `--start-solo` to capture Manual.
A private Advanced control replays the same input script with both headless
transmissions. Both journals start racing at tick 1156 and end at
1290. The manual frame shows the selected gear and SHIFT indicator.
The journal must be accompanied by frame evidence when comparing the
actual guest transmission.

### Checking an explicit upshift

Use the same input script in Manual and Automatic. For a third control, copy
the script to the private run directory and replace its two gear ranges
with `1157-1699 gear1=1`. Give each run a new capture directory.

```powershell
m2run.exe IMAGES_DIR 1700 --pc-solo 2 --transmission manual --inputs scripts/inputs/native_advanced_shift.txt --dump CAPTURES --dump-from 1390 --every 10
```

The two Manual controls match at tick 1390 in first gear. At tick 1500, the
upshift run shows second gear and 155 km/h. The hold run remains in first at
112 km/h. The Automatic run reaches 155 km/h and has no manual SHIFT indicator.
All three lifecycle journals match, so the HUD and speed samples supply the
distinguishing evidence. This control exercises a scripted shift on Advanced.

### Comparing private session metadata

Use the opt-in builder to record the exact files for each replay. Repeat
`--rom NAME=FILE` for every image consumed by the build, using the same logical
names in both runs. Record the EEPROM and backup RAM used at startup, before
the game updates them.

```powershell
python tools/build_player_replay_metadata.py --course 1 --transmission automatic --frames FRAME_COUNT --script INPUTS.txt --exe m2run.exe --rom program=PROGRAM_IMAGE --rom main_data=DATA_IMAGE --eeprom INITIAL_EEPROM --backup-ram INITIAL_BACKUP_RAM --out PRIVATE_MANIFEST.json
```

The ROM arguments above illustrate the syntax. Supply the full set for the
actual run. The builder checks the frame declaration in the converted script
and refuses to overwrite an existing destination. Keep the manifest in a
private build directory.

Use `python tools/validate_player_replay_metadata.py --expected CAPTURE.json
--actual REPLAY.json` to compare course, transmission, frame count, and
the supplied SHA-256 fingerprints of the input script, ROM set, initial
NVRAM and expected headless build. Build the replay description from
the files and options actually used. The comparator reports field
names and leaves the files and fingerprints private. A match covers the
declared configuration. Verify the lifecycle and race outcome separately.

## Native transmission requests

`StartRaceRequest` now carries `RaceTransmission::Automatic` or `Manual`.
Omitting the field preserves Automatic. An invalid enum is rejected before
board access. A changed course or transmission during startup is a conflicting
request; an identical request is idempotent and does not rewrite settings.

For the diagnostic runner, add `--transmission automatic|manual` together with
`--pc-solo 1|2|3`. The transmission option can appear before or after the course
option. Missing/invalid values and transmission without native solo return
exit 2. The existing default command remains unchanged:

```powershell
m2run.exe IMAGES_DIR 1400 --pc-solo 1 --transmission manual --race-status-log JOURNAL
```

The PC test launcher has Automatic and Manual controls and passes the
selection in its race request. The desktop `--start-solo` diagnostic
uses the same typed request and defaults to Automatic. The compatibility
adapter applies steering only during the original transmission-selection state;
it does not alter physics, clocks or guest instructions. Native `Racing` checks the course, mode and
committed gearbox value before handing control to the player. Revision A
commits 0 for Automatic and 1 for Manual. A different value emits
`wrong_transmission` in the diagnostic journal. Gear changes during the race
are separate from this startup check.
