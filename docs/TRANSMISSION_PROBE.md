# Bounded transmission-selection experiment

`m2selectionprobe` is an experimental diagnostic for Revision A, not the PC
application's start service. It supports the design document's Reference &
validation work without modifying guest instructions or live gameplay RAM.
It does not invent a guest gearbox flag.

With locally generated Daytona sources, configure normally and explicitly
build `m2selectionprobe` (excluded from the default build):

```powershell
cmake --build BUILD_DIR --config Release --target m2selectionprobe
BUILD_DIR/Release/m2selectionprobe.exe IMAGES_DIR NEW_OUTPUT_DIR 0x80
BUILD_DIR/Release/m2selectionprobe.exe IMAGES_DIR OTHER_NEW_OUTPUT_DIR 0xe0
```

The output parent must exist; an existing output directory is refused. The
probe runs exactly 1,400 simulation ticks on Beginner. It reuses PcSolo's
startup input timing and settings. During mode 18 / step 7 only, steering is
replaced by the specified byte. After entering mode 22 it applies
acceleration. Captures are `run_<completed-frame>.rgb` every 50 completed
frames plus the race-entry frame. Mode 22 by itself is not success, not proof
of gameplay, and not a win.

Exit 0 requires both an observed transition into mode 22 and `selection_samples > 0`.
`selection_samples` freezes at first race entry and counts ticks up to and including the entry tick (input is applied before observing the resulting mode) whose mode and step before `run_frame` are
18 and 7 and whose steer byte passed into that frame equals the requested
byte. The JSON line reports `frames`, `entered_race`, `i960`, `steer`, and
`selection_samples`. Exit 1 means the fixed run finished without that
evidence. Exit 2 is usage, I/O, a port the script cannot represent, or a
broken host frame clock.

`selection.csv` names `tick_before`, actual `frame_before` and `frame_after`. `frame_after` is
`GameLoop::frames()` after the tick. The probe requires
`frame_after == frame_before + 1` and fresh-run `frame_before == tick_before`. That check uses one host counter. It does
not compare the probe tick with the guest mode byte or with a second clock.
RGB names use that same completed-frame number, unpadded. `m2run` writes
`run_%05d.rgb` for the completed frame. Compare equal numbers: 28 regular
samples (50 through 1400). An empty comparison is not a pass. The extra
race-entry capture has no regular-grid counterpart. Inspect selection and
racing HUD images as well as hashes: a difference alone cannot name the
selected transmission.

`rt::Inputs` defaults are steer `0x80`, accel `0x20`, brake `0x20`, IN0
`0xff`, IN1 `0x8f`, IN2 `0xff`. They are not a held Start and not zero steer.
`tools::Script::at` rebuilds those defaults on every tick. `start=0` is a
no-op because the parser presses Start only when the value is non-zero.
`start=1` clears IN0 bit `0x10`. The probe writes `steer`, `accel`, `brake`,
and `start` (`0` or `1`) on every tick. IN1, IN2, and every IN0 bit other
than Start must stay at those defaults; otherwise the probe fails instead of
claiming a full `Inputs` replay. Gear buttons are not emitted.

Replay the recorded stimulus through the unmodified diagnostic runner:

```powershell
m2run.exe IMAGES_DIR 1400 --nvram PROBE_OUTPUT --inputs PROBE_OUTPUT/inputs.txt --dump NEW_CAPTURE_DIR --every 50
```

Create the capture directory first. Native `--pc-solo` replaces scripted
inputs during startup, so it must not be used for this replay experiment.
This omission belongs to the diagnostic only; normal PC play still uses the
native service. A production gearbox option must be added to that service and
independently reviewed, not implemented as a replay of these fixed tick inputs.

`tests/probe_evidence` is a standalone C++17 test of the sample counter: race
entry with no mode-18/step-7 window fails, real matching samples pass, and a
wrong mode, wrong step, or different steer does not count.

All generated settings, inputs, logs and images stay in ignored build folders.
See the dated transmission experiment report for actual results and limits.

The default requested byte0x80 is a valid control run; it is not required to
produce a non-default signal. The companion0xe0 run is the discriminating
stimulus. Neither sample count proves the guest latched that transmission.
A sample after first entry cannot retroactively satisfy the selection gate.
An already-racing observation is not counted as a new transition.
