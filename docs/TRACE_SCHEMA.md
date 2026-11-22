# Trace schema for the native-scene feasibility test

Environment: `M2_OBJECT_TRACE=<path.csv>` (unset = no trace; no behavior change).
The trace is written by `src/runtime/geo.cpp`, one row per object before cull;
`frame` comes from `M2Board::frame_`. The header is written at the first frame.

```
frame,pool,oba,obc,mode,matrix0..matrix11,vertices,source,command_word,opcode,tpa,tha
```

- `frame`: M2Board frame counter.
- `pool`: reserved (0 in this port).
- `oba`, `obc`, `mode`: object address/count/parse mode from `geo_object_data`.
- `matrix0..11`: current `geo->matrix` in native array order (model-view): three
  basis columns followed by translation, **not** three consecutive rows of four.
  The equivalent row-major matrix is `[[m0,m3,m6,m9], [m1,m4,m7,m10],
  [m2,m5,m8,m11]]`. For example, identity plus translation `(tx,ty,tz)` is
  `[1,0,0,0,1,0,0,0,1,tx,ty,tz]` in the CSV. The analyzer normalizes this layout
  before computing relative matrices. No format autodetection is performed.
- `vertices`: vertices submitted before cull for this object (sum of 3/4 per link).
- `source`: `ROM`, `RAM0`, `RAM1` from `oba` bits; `direct` for command 02
  (`geo_direct_data`, no object/matrix).

## Run

```powershell
$env:M2_OBJECT_TRACE="build-daytona\trace.csv"
build-native-daytona\Release\m2run.exe build-native-daytona\images 3000 --inputs scripts\inputs\race_basic.txt
python tools\analyze-object-trace.py build-native-daytona\trace.csv --json
```

## Geometry keys and limits

The analyzer groups ROM rows for matching by `(pool, oba, obc, mode)`; the matrix is not part
of the key. `obc` is a link-prefix/LOD discriminator, not an instance id, so
ambiguity detection uses `(pool, oba, mode)` only. If any frame has two or more
ROM rows with the same `(pool, oba, mode)`, even when their `obc` or matrices
differ, the trace is ambiguous: the schema has no persistent instance id and the
analyzer will not invent one, pair rows by ordinal, or deduplicate equal matrices.

This case fails closed with verdict `alternative_indeterminee`. It reports
`ambiguous_frames` (number of frames containing a repeat),
`duplicate_rom_key_groups` (number of repeated `(frame, pool, oba, mode)`
groups), and `duplicate_rom_rows` (number of ROM rows in those groups), plus a
`reason`. `rows`, `frames`, `source_vertices`, and `rom_share` are still computed
from all rows, so repeated instances remain in the accounting; clique fields are
not computed. The same `(pool, oba, mode)` appearing once in each distinct frame
is valid.

## Adjacent-frame draw order

Run `python tools/check_scene_draw_order.py OBJECT_TRACE.csv` for aggregate
counts on a completed capture. The diagnostic requires a unique geometry
key and equal finite model-view components on each side of an adjacent
frame pair. It reports command-offset changes, changes in the ordinal
position among all draws, and changes in the ordinal position within
the geometry key. Insertion and removal can shift either ordinal.
The unmatched counters describe unmatched geometry-and-pose keys.
A changed camera can alter these keys even when every object persists.
The results leave occurrence identity and interpolation unresolved.

For two runs with the same tick axis, use
`python tools/compare_scene_traces.py A.csv B.csv START_TICK END_TICK`
with an inclusive window. It reports differences in the full
multisets, relative inversions among unique keys, changes in
command offsets and ticks present only on one side. The key
also compares opcode, TPA and THA. The counts for excluded rows
distinguish a missing counterpart from an ambiguous repeated key.
The same tool rejects windows with no common ticks.

For a controlled cross-run comparison, run the same input sequence
twice and add `--draw-distance 1 --draw-order-only --draw-budget 5000`
to the second run. The hook reorders all original entries. It keeps
duplicates and appends entries outside the selected rings in their
original order. A tick without a hook can still expose a list that the
game updated after a previous permutation. The scenery log records the
post-tick RAM list, while the object trace records a Geo parse. Matching
frame numbers do not join a cell entry to an individual command.

On Beginner Revision A the complete baseline replay matched all
three logs. With the order control, 230 compared Geo frames had equal
multisets of geometry, pose and command arguments and different draw
sequences. There were 1,766 command offset changes among 13,403
unique same-frame comparisons. One post-tick scenery list differed
in multiplicity at a tick with no hook. A separate player-cell-only
control changed all 230 Geo multisets and removed 2,098 rows from
the comparison, with zero budget rejections in either run during
the comparison window. This isolates a list selection effect at the
aggregate level. Individual cell provenance still requires tracing
the producer or a more selective intervention.

In a headless run at the original aspect,
`--omit-scenery-cell 1729:137` requests the removal of cell 137 at
each draw-list update during zero-based simulation tick 1729.
Keep the budget and other options identical in the baseline and
intervention. The tool reports
`scenery_omission,TICK,CELL,HOOK_CALLS,REMOVED_ENTRIES` on stderr
and fails when the selected tick has no draw-list update. A hook
can run and remove zero entries. The game can reuse the modified
RAM list after the tick, so compare the traces over a window to
determine the extent of the effect.

Run once with the original selection, then again with
`--draw-distance 1 --draw-order-only --draw-budget 5000` to test the
same omission after a change of draw order. The hook removes the
cell before it sorts the remaining entries. Changing draw distance
without `--draw-order-only` is incompatible with this control.

On Beginner Revision A, removing cell 137 or 155 separately at
simulation tick 1729 removes one distinct ROM command per run at
Geo tick 1730. Repeating at simulation tick 1731 again removes
the respective mesh at Geo tick 1732 with a changed model-view
matrix. The absent-cell control is byte-identical and the
no-hook control fails explicitly. On Advanced the individual
removal of cell 101 or 115 at simulation tick 1271 removes one
command at Geo tick 1272 per run. There are no budget rejections
in the comparison windows. The intervention establishes a
dependency in these captures. A cell can contain several entries,
and the results do not identify an authored occurrence across
frames, courses or repeated geometry.
Repeating all four omissions after permutation removes the same
geometry, pose and argument signature in each case. Its draw rank
changes in three cases. The baseline permutations change sequence
in 73 Beginner and 50 Advanced frames with equal multisets.

In one Beginner frame, all nine cells were removed independently.
Each removes a single command with a ROM mesh key used once in
that frame. Across 230 Beginner and 50 Advanced frames, the
player-cell-only control keeps all commands whose ROM mesh key
is repeated in the baseline. It removes 2,098 and 400 other rows
respectively with no budget rejections in those windows. These
captures leave the source context of the repeated meshes open.

The writer trace attributes all five header and argument words of
3,958 repeated-mesh ROM commands on Beginner and 946 on Advanced to
the geometry port. A same-board join on Advanced matches all 4,730
distinct word versions to completed port writes and preserves the
object and writer logs byte for byte. In each of the 300 repeated-mesh
groups, every opcode write has the same sampled instruction pointer
as its peers. An object source requires context before that sampled
write, even when the port event and consumed word join exactly.

## Test and verdict

The analyzer has26 synthetic regressions plus9 helper tests. Positive evidence
requires at least two frames and two mutually rigid objects, ROM share>=50%,
and a single clique covering>=50% of common ROM vertices in **every** frame.
Selection maximizes the minimum exact Fraction coverage, then total raw weight,
cardinality and lexicographic key order. `clique_cover` is the worst frame;
`clique_cover_by_frame` follows ascending frame order.

Unsupported transforms are unknown edges. An optimistic graph includes those
edges to determine whether missing evidence could change the outcome. Supported
mismatches remain decisive, as does ROM share below50%. Duplicate geometry keys
still exit as indeterminate before matching or matrix interpretation. Exact
search is bounded to22nodes/100000visits: reaching a bound returns indeterminate,
not an approximate positive/negative result. `search_limit` records that cause.

## Matrix-layout correction (10 November)

The previous documentation, parser and synthetic fixture writer all treated the
raw array as row-major 3x4. Runtime trace emission was correct and is unchanged.
Earlier nonambiguous rigidity/clique verdicts from that parser require reanalysis;
this correction does not retroactively validate them. Duplicate-key detection
and source-vertex accounting occur before matrix parsing and are unchanged.
An unsupported sample cannot prove a rigid edge. Any two supported samples can
still disprove a constant relative pose; missing evidence never erases that mismatch.


## Optional command provenance (diagnostics only)

Four appended CSV columns preserve command provenance without changing the
existing columns. `command_word` is the opcode's word offset in command-buffer
RAM, captured in Geo::parse before opcode consumption and after any jump.
It is not a byte offset, command rank, CPU PC, writer provenance or instance ID.
The same slot can be reused by a different object in a later frame. `opcode`
is the raw command word; `tpa`/`tha` are initial texture point/header arguments,
not resolved material identity. Direct commands also carry those arguments.
UINT32_MAX denotes a row emitted without known parser context; frame_begin
resets context. Metadata is copied when an object begins, so a later command
cannot relabel the previous object's delayed flush. Existing trace omissions
such as speed-mode skipped objects remain; no complete writer trace is claimed.

The new CTest geo_provenance uses synthetic buffers, a jump, reused command slot,
and direct rows to check offsets, delayed flush and reset. No ROM is required.
Actual trace-on/off comparison is separate; passing this test alone does not
prove full simulation/raster equivalence or persistent object identity.


## Optional buffer writer provenance (experimental)

Set `M2_WRITE_TRACE` to a private CSV path before constructing the game. The
sidecar allocates per-word ownership only when enabled. It does not modify guest
memory values, timing requests, command addresses or persistent instance IDs.
Use a different output path from `M2_OBJECT_TRACE`. Keep both traces local: they
contain runtime information derived from protected game data.

Columns: `frame,command_word,argument_index,buffer_word,unknown_mask,i960_mask,
tgp_mask,port_mask,last_mask,writes` (one header line in the actual CSV).
`command_word` and `buffer_word` are word offsets, not byte addresses. Each
object command (kinds1/17) snapshots five consecutive words; each direct command
(kinds2/18) snapshots three. Argument0 is the opcode. Arguments1/2 are tpa/tha;
object arguments3/4 are oba/obc. Addresses wrap at0x8000words, as the geometrizer
cursor does. Snapshots are taken before command dispatch, not after raster culling.

The four ownership masks form a disjoint partition of32bits. Initially every
bit is unknown. An i960 masked write updates only the selected bits; TGP memory
writes and geometry-port pushes replace ownership for all32bits. A same-value
write still updates ownership. A zero-mask write changes neither ownership nor
count. `last_mask` is the last nonzero write mask; `writes` counts nonzero writes
since board construction. Program-upload writes are not geometry-buffer writes.

This records writer *classes*, not writer instruction PCs, object addresses or
causal identities. Reused command slots and ambiguous repeated meshes remain
ambiguous. Do not feed these fields directly to interpolation as stable IDs.
The diagnostic can add substantial CPU/I/O work; headless throughput with this
trace enabled is not a presentation-rate benchmark.

### Trace frame versus m2run capture number

The trace `frame` is the board counter before `vblank_end` increments it.
`m2run` writes `run_NNNNN.rgb` after the tick using the incremented board counter.
For this normal no-frame-skip path, trace frame F corresponds to capture tick F+1.
Do not report the lack of trace frame1400 as missing evidence for capture1400:
that capture corresponds to trace1399. Some ticks have no object command rows.
The `.rgb` filename does not imply RGB24: m2run writes uint32 pixels (4bytes each),
with dimensions `Video::width()` by `Video::H`. Default496x384 is761856bytes.

## Experimental completed geometry-port observations

Set M2_PORT_OBSERVATION_TRACE to a NEW private file path. Exclusive creation
rejects an existing path, including a second board using the same name, without
truncation. Each board owns its file; the writer borrows it and dies first.
Writes/each-vblank flushes are checked; destruction checks fclose and aborts an
opt-in diagnostic run on failure. This does not promise disk durability/fsync.
No overhead from capture/serialization occurs when the variable is absent.

The45 CSV columns are sequence,frame,address,lane_mask,data_masked,buffer_cursor,
cpu_state_present,observed_ip,observed_count,r0..r31,buffer_provenance_available,
buffer_write_observed,buffer_word_written,buffer_write_count.
CPU values and cursor are sampled BEFORE the geometry handler; the row is
emitted only AFTER successful return. An exception produces no completed row.
The scope is geometry register/data-port handlers00800000..00807fff, not all
RAM/TGP writes. Ignored program uploads and cursor changes are nonwrite records.
Maskless handlers receive data&lane_mask exactly as before; even zero effective
input can push a full geometry word. This differs from a zero-mask buffer_w.

Enable M2_WRITE_TRACE alongside this trace for per-word provenance. Without it,
provenance_available is0 and no completed buffer write is attributed. With it,
the pre-handler cursor identifies a word and a one-step counter increment with
full GeometryPort ownership proves the completed event. No-write coordinates
are normalized to0. Event counters count all tracked origins and never wrap.

The join requires both streams to come from the same board lifetime. The
schema carries no board identifier, so the caller must establish that.
(buffer_word_written,buffer_write_count) identifies a completed write within
that lifetime. A consumed writer row (buffer_word,writes) observes the version
of the word present before dispatch. Several commands can read that version
across frames, even before a later write overwrites it. Duplicate completed
write keys are ambiguous. A match establishes the observed word version.
Object identity, freshness and which command caused the write require
separate evidence. A sampledIP can be a block entry. CPU attached means its
state was available.

Tests/port_observation includes serialization and real geometry handlers with
synthetic memories and a trap for TGP execution. Tests/checked_trace_file covers
exclusive ownership and close errors. Actual-game traceoff/on evidence remains
separate. TraceF precedes board vblank_end increment; captured tick isF+1.
