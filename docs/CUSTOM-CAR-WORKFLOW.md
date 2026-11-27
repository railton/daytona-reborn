# Making a custom car: the path that worked

How a downloaded 3D model becomes a player's car in the game, to the
finished standard reached with the second car (a customised Beetle matched
to reference pictures), what was tried and dropped on the way, and every
problem met with its cause and fix, so that the next car is built to that
standard from the start. The file format and the settings are in
[CUSTOM-CARS.md](CUSTOM-CARS.md); this file is the workflow. The standard
is the section "The finished standard" below; the sections after it are
the reference (measurements, the first converter's stages) and the history.

The model, its converted files and the conversion scripts stay on the
working computer, outside the repository (rule 6 for the game's data; the
model's own licence for the car). The scripts are referred to here by name;
they live in the local working folder (`planos/carro-foto/ferramentas/`,
beside the repository): `carro/` (the generic converter: `car.py CONFIG
OUT_DIR`, the library `carlib/`, one config per car in `cars/`, its
README), `captura/` (shots.sh, game_car_preview.py, ref.py and the
inputs); the first three cars' own converters, kept as the reference the generic one
reproduces: `fusca/`, `camaro/`, `escort/`.

## The path, in short

1. Find an **artist-made 3D model** whose licence allows changing it, in
   glTF (`.glb`). Not a car generated from photos (see "Tried and dropped").
   Gather **reference pictures** of the look wanted (photos, or AI pictures
   of that model in that style); without them the model's own look is the
   reference. **Before starting, ask the person who wants the car whether
   there are more pictures** (other angles, the rear, the wheels, the
   interior, a detail) or wishes the pictures do not show (colour, plate,
   badges, flares, tyres): what the pictures leave out is decided by the
   person, not guessed (the Chevette's pictures carried an AI picture's
   mistakes: a Ford badge, "BRASILL").
2. **Try the look first** with `captura/ref.py GLB OUT_DIR`: the model in
   its own colours at the game's angles (the select screen's frames 1400
   to 1760, the start, chase and high views), before any conversion, and
   the reference to compare the captures with when there are no pictures
   (the DeLorean). (`fusca/preview.py` does four angles with the Beetle's
   recolour rule built in.) Not to be confused with the card's picture
   (`captura/game_car_preview.py`, step 8).
3. **Decide its size:** at its own size on its own axles (`M2WHEEL2`, the
   Beetle); or at its own proportions, its wheelbase scaled onto the game
   car's, with bigger wheels standing out of the body and the arches cut
   round them (`M2WHEEL2` too, the refined Camaro). Scaling the whole body
   onto the game car's wheels (the first Camaro) looked squat and is not
   done any more.
4. **Convert** it with the generic converter: a config in `carro/cars/`
   (copied from the nearest: `escort.py` for a model with plain materials,
   every face a flat colour; `fusca.py` for a model whose look is in
   shared texture atlases, baked; `delorean.py` for a dense model whose
   nodes are kinds of material, each face's colour read from its texture)
   and, for what is one-off, a short hook
   module; `carro/README.md` lists the fields. Build to the
   finished standard below: details cut into the skin, small black parts
   as their own faces, lamps and the plate on their own texture cells, the
   textures only paint and large areas.
5. **Its wheel:** the model's own or one made in the converter (a
   revolved profile, spokes and dish), the config's `WHEEL`.
6. **Check it** in every standard shot, the select screen's full turn and
   the cockpit, next to the reference; measure the smudges and the frame
   time (the performance protocol below).
7. **Refine** in rounds until the acceptance criterion holds; a refinement
   agent can run the loop (the brief below). End every round with a full
   review of every planned item against the plan and the previous
   versions: a round can silently remove a part (the Beetle lost its front
   bumper that way).
8. **Use it:** copy the game's files (`bodies.m2car`, `liveries.rgba`,
   `wheel.m2car`, `car.ini`, the credits, `preview.png`; not the review
   sheets) to the project's `cars/NAME/` and pick it in Settings > Cars (it
   changes at once; the tab shows once a folder there loads), or
   `M2_PLAYER_BODY=FOLDER`.
   Give it a `name=` in `car.ini`, a credits file whose first line (title
   and author) the card shows, links left out, and a `preview.png`: the
   select screen's chosen car (frame 1720, the left half) through
   `captura/game_car_preview.py FRAME OUT` (an exact key on the backdrop's
   (0, 0, 255), the edge pixels unmixed, about 800 px wide). Check the
   card: `M2_CAPTURE_SETTINGS_TAB=7 build-daytona/daytona --profile capture
   --settings --capture-home OUT.bmp` from the repository (every profile
   reads the same `cars/`); a folder left out is named under the cards and
   in the log (`daytona: cars/NAME left out: ...`).
9. **Clean up** once the car is accepted: keep the downloaded model, the
   converted folder the proof compares with (`modelo/`), the converter's
   code and config, the notes, the final sheet and the card's capture,
   the benchmark and proof logs; delete the rest (the shots of every
   round, the comparison sheets, reference renders, build outputs,
   `trabalho/generico/`, discarded variants, and any tool, model cache or
   Docker image of an approach that was dropped). A car's rounds leave
   hundreds of megabytes; the photo-to-3D trial left 15 GB. Run every
   `prove.sh` before the clean-up (all must print PROOF OK): the proofs
   rebuild into `trabalho/generico/`, which the clean-up then deletes.

## The finished standard

What the accepted Beetle does, which every new car follows. The reasons
are in "Problems met" and in the Beetle's history further down.

### What goes where

| Detail | How it is made | Why |
| --- | --- | --- |
| Paint, large areas | the planar textures (256 x 128 a surface), baked from the full model (the Beetle); or, when the model has a paint material of its own, every face a flat colour from a palette, nothing baked (the refined Camaro) | the only thing the textures render well; flat colours cannot smudge at all |
| Panel lines (wings, doors, lids), window rubbers, stripes, vents, slats, emblems | **cut into the skin** (`inlay()`): the band is cut out of the triangles it crosses, the pieces take a flat colour | drawn in a texture they blur and smear on curves; laid over the skin (even as decals) they lose to the game's per-face depth order |
| Panel lines: which ones | the ones the reference has, one 2 cm line per seam, from the model's geometry: its open panel edges on the exterior paint when the skin is made of panels (the Beetle); when the skin is one welded piece with the seams modelled as grooves (the Escort), its sharp creases (over 50 degrees, away from the glass, arches and sills), each groove's two creases merged into one centre line resampled every 5 cm, the band extruded along the axis it is seen along (side x, top y, ends z) | lines from the texture's noise are wobbly and unfaithful; a band built on the skin's own normal across a groove reads hairy, with black wedges |
| Small black parts (handles, mirrors, piping, running boards) | their own faces in a flat colour, kept out of the bake | baked, they smear the paint behind them |
| Headlamps, tail lamps, auxiliary lamps | their own texture cells (surface 8, 128 x 128 a kind); new geometry where the model's shape differs from the reference; a cluster behind a grid: its picture taken from the model (an orthographic bake of lens, reflector and grid) on one flat panel. Lenses are often thin shells with half their faces wound inward: keep the outward ones. A transparent lens material dropped leaves a silver reflector: apply the lens's tint to it | baked into the front texture a lamp gets about 20 texels; reduced, a grid and its lenses become loose triangles |
| Plate | its own quad (a decal when a bumper or lid face could cover it), its picture in surface 7's free half; a palette car has whole surfaces free, a plate each at 256 x 92; letters drawn with a font file kept beside the converter (DejaVu Sans Bold) | sharp letters |
| Wing flares | the skin cut open on a circle (the arch radius plus the flare's width); the flare a revolved strip from the cut edge (its \|x\| read per angle from the cut) out to the tyre's face, a dark liner drum inside, rivets inlaid into the flare's own faces | the model has none; repainting cannot make a shape |
| Short flares with the tyres in view | the flare's lip well inside the tyre's face (6 cm on the Chevette, the tyre 11 cm out of the wings), ending at the axle line with tapered ends, the cut skin below its ends closed; checked in the chase view from behind | the chase camera looks down from behind: a tyre shows only by what stands out of the body and the flare in plan; a flare 4 mm inside the tyre's face, ending at the axle, still hid the tyres completely |
| Tyres standing out, nothing over them | no flare (`flares(lip=False)`, `flare_w=0`): the skin cut at the arch's radius, the liner drawn on both sides, a dark ring from just behind the cut edge to the liner; `min_x` above the sill's tucked-under edge | flares down past the axle and proud of the tyre hid the tyres from behind; a cut wider than the liner showed the backdrop round the tyre; a ring in front of the edge showed as teeth, and so did a liner built below the axle where the sill tucks under (its edge's \|x\| falls, 0.75 to 0.63 on the Chevette) |
| Lowering | build everything unlowered and subtract the drop when writing the body; the wheel centre used for the arches is then ground + radius + drop | lowering part way through moves the arches off the wheels |
| Bumpers, splitter, diffuser, ducktail | rebuilt as new geometry to the reference when the model's differ | repainting keeps the wrong shape |
| Glass | flat tint (surface 6), a black rubber inlaid along its edge, `glass_opacity=0.75` when there is an interior | the join between glass and body is jagged |
| Interior | kept when seen through the glass, each face a flat colour from a palette; reduced (about 2,500 faces); what the model lacks is made (cushions, a rear bench, the parcel shelf, side panels, the windows' inner sides, a steering wheel: the Escort had only a black shell) | a see-through car with nothing inside looks hollow, the select screen's blue showing through |
| Driver | `piloto.py` (the converter's `DRIVER`): needs the car's steering wheel (centre, facing, radius), the cushion's top and the backrest; the seat may have to slide forward for the arms to reach (reach 0.89); scaled to the car's height (1.52 m in the 1.14 m high DeLorean) | the Beetle's copy finds them in hard-coded Beetle coordinates; at 1.72 m the DeLorean's helmet went through the roof |
| A dense model (each node a kind of material, hundreds of pieces over a colour atlas) | each face's colour read from the texture (swatches, flat colours: nothing baked); the never-seen test made on the full-detail model, with points by area; reduced piece by piece, the faces shared by seen area, cables kept round (one colour a cable); a fine wire on a panel a decal; pieces under 2 cm and wires under 8 mm left out; meshes with holes (perforated grilles) dropped | a node is not a part there; a test with a fixed number of points a face lets everything show through a wide panel; a mesh drawn without its holes greys what lies behind |
| Paint colour | a red of (212, 18, 20) rather than (234, 28, 22): a little deeper than the reference's sunlit paint, the game's light shading it | a bright value reads flat and orange in the select screen |
| Wheels | the model's own, or one built in the converter (revolved profile), on the car's axles; check each profile segment's normal and close every gap; the tread split into strips across its width; a dark band under the bead | a crude or wobbling wheel spoils the close shots; a full-width tread face is drawn over the body (mean depth); the gap between rim lip and bead shows the backdrop |
| Bigger wheels standing out | the size set in the body's converter (written to its JSON, read by `wheel.py`); the arch cut as a circle (tyre radius + 2 cm, 48 sides) through the paint faces only, after the inlays; a dark liner drum inside, its edge 2 cm in from the cut | cut before the inlays, stripes end ragged; cutting every material took the exhausts' faces |
| Seams, gaps, trim bits left in the textures | painted over: every thin dark texel next to the paint takes the paint's colour, except where a small black part lies | read as dirt |

### The drawing order inside the car

There is no depth buffer inside a car: the game draws its faces in the
order of each face's mean depth. Any two faces that overlap on screen and
lie within a few centimetres of each other will fight at some angle of the
select screen's turn. About half the Escort's refinement time went here,
a quarter of the DeLorean's (the rules were written by then). The rules
that worked:

1. **Skin under another part** (a bumper, a spoiler) takes that part's
   colour, cut along the part's outline: whichever is drawn last looks the
   same. Removing that skin instead leaves holes where the part does not
   cover its whole outline.
2. Or **replace the model's layered part with one made shell** (the
   Escort's bumpers).
3. **A decal only over faces of about its own size.** On a wide face (the
   third brake light on the spoiler) the wide face's mean depth can be
   nearer than the decal's nearest corner minus 2 cm: cut the detail into
   the face instead and remove the hidden skin behind it.
4. **Glass lying on a dark band:** make the glass the decal (surface 16).
5. **A wide skin face next to a small part** is drawn over the part's edge:
   subdivide the skin beside it.
6. **A cable or clip lying on a panel:** make it a decal and put the panel
   in small faces under it (cutting the skin round each cable cost the
   DeLorean 9,700 faces).
7. **Cabin parts and the driver right behind a flat panel** are drawn over
   it: the panel in faces of 16 cm or less, and leave out what only shows
   that way (the inner door panels below the windows, the driver's feet).
   A model's inner shell behind the panels (the DeLorean's black one) and
   a headliner right under the roof (the Chevette's) are dropped for the
   same reason. Faces of 16 cm or less cost: 3,300 faces on the Chevette,
   15 % of the budget.

Rule 1 also covers chrome bumpers standing off a painted valance (the
band bounded in z, or it shows past the bumper's ends), rule 6 window
frames and wipers lying on the skin, and made blocks on a bumper are
decals.
### Budget

One number: the body, driver and interior included, **under about 22,000
faces**, up to about 30,000 for a model whose detail is the point when the
median stays under 9 ms (the DeLorean, 26,483: below 24,645 its canisters
turned into spikes); each wheel under about 2,000; game + renderer CPU median under
about 9 ms in the benchmark (the game has 17.4 ms a frame). A face costs
about 0.3 microseconds a frame (the Escort measured a little less: 5.6 ms
over the game's car for 21,712 + 4 x 596 faces). Measured, 3440 x 1440,
everything on:

| Car | Body faces | Wheel faces | Median |
| --- | --- | --- | --- |
| The game's car | | | 1.20 ms |
| Beetle (driver, bumper) | 23,121 | 660 | 7.40 ms |
| Camaro, refined, big wheels | 17,574 | 1,512 | 7.29 ms |
| Escort (driver) | 21,712 | 596 | 6.84 ms |
| Escort, round 9 | 21,684 | 636 | 6.91 ms |
| DeLorean time machine (driver, cabin) | 26,483 | 495 | 7.74 ms |
| Chevette GT (driver, cabin), after the close review | 21,992 | 636 | 6.89 ms |

Pay for details by reducing the interior and the paint first.

### Performance protocol

Every change justified by speed: measure the baseline and estimate the
floor first; one change at a time; control and candidate run interleaved,
at least 6 runs each, medians (and p95, p99) compared; accepted only if it
beats both neighbouring control runs and nothing regresses by more than
about 3 %; each attempt, kept or dropped, appended to a local ledger.
`pgrep -x m2gpushot` guards against another capture only: look at the
control's p99 too (the game's car about 2.1 ms on a quiet machine) and run
again a round where it is high (the Chevette's sixth round: 3.15 ms).

### Acceptance criterion

A car is finished when all hold:

1. **Faithful:** every detail comes from the model's geometry or the
   reference's layout, never from baked noise.
2. **Continuous:** no breaks along any line.
3. **Even:** one width in metres along the whole car.
4. **No smudges:** the converter's ACCEPTANCE line, the thin dark blobs (6
   texels or more, up to 4 wide) left in the paint textures over the
   texels the faces use, outside the meant details: 0 on every surface.
   Meaningful only for a baked car: on a palette car it passes by
   construction, and criterion 5 is the check.
5. **In the game,** the deciding check: in every standard shot, every angle
   of the select screen's turn (both cars, the one not chosen dimmed) and
   the cockpit view, set next to the reference, every feature of the
   reference that the model can carry is there and nothing reads as dirt,
   blur or a misplaced part.
6. **Reviewed item by item, side by side, at close range:** a table with
   one row for every part of the reference and of the plan, and one row
   for each panel line (bonnet front, bonnet rear, bonnet sides, each
   door's front and rear, boot lid sides, boot lid rear), each frame,
   trim, lamp, badge, vent and row of rivets. Columns: left, right (or
   front, rear), correct, and the close crop each was checked in (S select
   screen, D dimmed, R rolling start, C chase, H high, K cockpit).
   "Present" is what the crop shows, never what the config says; a
   feature on both sides has both cells filled from two different crops;
   a cell without a crop is "not checked", not "yes". Made again after
   every round. The Escort's found a third brake light broken at three
   angles after the car otherwise looked finished; the Beetle's, a lost
   half plate; the Chevette's first review passed a bonnet line missing on
   one side, door lines never drawn and a plate's letters wider than the
   plate, all of them in its own frames, too small to see.
7. **Close range means** the detail cut out of the frame and enlarged until
   1 cm of the car is at least 4 px, with the same detail cut from the
   reference picture beside it. In a select-screen frame the car is about
   470 px long (a 1.4 cm line is 1.6 px, a plate 45 px): a whole frame, or
   a sheet of frames, decides nothing about a line, a letter or a lamp.
   Lettering and plates carry two measured ratios (the letters' height to
   the plate's, the row's width to the plate's) from the game's crop and
   from the picture's, within about 10 % of each other and of the real
   thing where there is one (a Mercosul plate: 400 x 130 mm, letters 65 mm).
8. **Left against right by numbers:** `captura/sym.py BODY_DIR 0.008 3`
   lists every colour's faces that have no partner mirrored in x. Outside
   the cabin the list holds only differences that are meant (a badge on
   one side, the exhaust, the wipers), each named in the notes.
9. **Every outline accounted for:** the converter's panel-line report
   (edges found, placed on the skin, dropped) shows no dropped run longer
   than 5 cm that the notes do not explain ("114 edges, 66 placed" was
   printed and not read), and every rule that picks pieces by face count
   matched at least one piece (the Chevette's doors' rule matched none,
   silently).

### Refinement rounds with an agent

The Beetle was finished by a refinement agent (a capable model, working
alone for about 45 minutes), briefed with: the reference pictures; the
converter and this document to read first; the commands to convert, to
capture the standard shots and the select screen's turn, and to
benchmark; the acceptance criterion; the budget; and the rules (change only
the local converter, keep a backup copy first, no git, nothing installed on
the host, the model never in the repository, install the result into the
project's `cars/NAME/`, a comparison sheet of the game's frames next to
the reference at the end, a report of what still differs). Changes the
game itself would need are described, not made, and reviewed afterwards.

A conversion runs in nine stages, and the agent reports each one as it
starts and as it ends ("Stage 3 of 9: parts and colours, started" ...
"done, 14 min, what was found"), in a progress file beside its notes, so
that whoever waits knows where it is:

1. Reading and inspecting the model (nodes, materials, axes, origin), and
   the pictures: what they show, what they miss, what to ask the person.
2. The first config: a plain body that builds and shows in the game.
3. Parts and colours: the palette, glass, cabin and driver.
4. The skin's details, lamps and plates (bands, cells, pictures).
5. Made parts and the wheel (flares, bumpers, spoilers, the stance).
6. Refinement rounds against the reference, every standard shot.
7. The close review: the close views, `sym.py`, the outline report, the
   table of criterion 6 with a crop for every cell; its fixes; the
   close-up sheet.
8. The A/B benchmark, the card's picture, the install and its card.
9. The proofs of every car, the notes, the clean-up and the final review.

The brief says it in words: the review is made on close crops, each side
on its own (criteria 6 to 9); a row is not "present" until a crop shows
it. (The Chevette's first review, all ten rounds and its table, took 4
minutes and missed a dozen defects; the close review that found them took
27.)

**Who does which stage.** The one who builds a car is the worst judge of
it: the Chevette's dozen defects were found by a second agent that had not
built it. So the stages are split between three agents, each briefed on
its own:

| Stages | Agent | Its job |
| --- | --- | --- |
| 1 to 6 | the builder: a capable model at high effort | reads the model, writes the config and the hook, makes the parts, refines against the reference |
| 7 | the reviewer: the most capable model, a separate agent that did not build the car | the close review (criteria 6 to 9): the table with a crop in every cell and the list of defects; it fixes nothing |
| 7, fixes | the builder again | fixes the reviewer's list, item by item |
| 7, check | the reviewer again | looks again at the fixed items only, from the same crops |
| 8 and 9 | a small, fast model | the benchmark, the card's picture, the install, the proofs, the clean-up: commands with known outputs |

The reviewer never fixes, so no one passes their own work. Each agent
writes its stage lines to the same progress file.

## Tried and dropped: a car generated from photos

The first attempt was a 1993 Chevette from photos: AI-generated pictures
(front, back, sides, three-quarters, top, on a plain background), turned
into a mesh by an image-to-3D model (Hunyuan3D-2, in Docker), then reduced.

- At the rolling start the car is seen from ahead, close, at high
  resolution: the generated mesh's front was visibly distorted (soft,
  asymmetric lamps and grille, wavy panels). Its texture, projected from
  the photos, smeared where the views met.
- Retopology by "artist-mesh" models (MeshAnything V2, DeepMesh) on that
  mesh did not fix the shape: the result was rejected too.

Conclusion: today's photo-to-3D output is not good enough for the closest
shots. Start from a model a person modelled.

## 1. Finding a model

- **Where:** Sketchfab worked. Only models with a "Download 3D model"
  button can be downloaded; many pages (the Chevette's) have none.
- **Format:** glTF binary (`.glb`): one file, its node transforms, PBR
  materials with textures. The converter reads it with `trimesh`.
- **Licence:** it must allow changes. The Camaro is CC BY-NC-SA 4.0: used
  locally, credited (`CREDITOS.txt` in its folder), never committed or
  shared. A model whose licence allows sharing could be distributed as a
  car folder; nothing derived from the game's ROM ever is.
- **Its origin:** a licence is only worth something if the uploader made
  the model. Signs of one extracted from a commercial game: a game's name
  in its tags (`csr2`, `forza`, `nfs`, `gta`), node and material names in a
  game's pattern (`LOD_A_..._mm_...`, `..._Geo_lodA_...`, `(Clone)`,
  `_rootJoint`), a car from a film. Such a model may be used on one's own
  computer at one's own risk and is never published, not even converted.
  `python3 -I` reading the `.glb`'s JSON chunk prints the node and
  material names, and its `asset.extras` the author, licence and source.
- **What makes a model easy:** separate materials (paint, glass, chrome,
  lamps, tyres, rims, interior), the paint as one material, real-world
  scale, no baked-in background. Detail does not need to be low: the
  converter reduces it (a 60,000-triangle model is fine).
- **Check before converting:** is the skin made of panels or one welded
  piece (the panel lines' recipe differs)? Are the lenses wound outward
  (the Escort's auxiliary lamps faced inward, invisible until flipped)?
  Has it an interior, or only a shell? Is a detail only in the texture (the
  Escort's pinstripe was in its paint texture: its path read from there and
  inlaid)? Which way are its axes: the tyres' centres give the axles, the
  headlamp glass the front, the steering wheel the car's left (the Escort:
  front +z, +x the car's left, so x mirrored and winding reversed; real
  scale; the Camaro needed x100 from its root node). Is it centred on x = 0
  (the Chevette stood at x 0.08..1.68: placed by its tyres,
  `AXES wheelbase='model'`)? Do its node names survive the export (two of
  the Chevette's hold broken characters and must be written as they read)?
  What is its badge really (the Chevette's is the "Chevette" script, not a
  bowtie)? `ref.py GLB OUT_DIR textures` also dumps each material's texture,
  where such details and texture-only vents show.
- **A quick look at the written files** before any game capture: a small
  rasteriser of `bodies.m2car` with back-face culling (`escort/view.py`)
  found the inward lenses, the seat and the driver's pose. A converter run
  is a few seconds and a full set of captures about 15 s, so the game
  stays the judge.

## 2. The game's car, measured

Measured once with a probe built against the game's libraries (outside the
repository): a solo Beginner race, throttle held, the player's car's keyed
polygons (`GeoPoly::mobile_key`, slot 0) with their object-space vertices
(`source_v`) and object matrices. Object space: +x the car's right, +y up,
+z forward, 1 unit about a metre.

| What | Value |
| --- | --- |
| Body | part `0x7d68`; 4.93 long, 1.73 wide, 1.27 high |
| Glass / cabin | part `0x7d94` |
| Checkered shadow | part `0x78b4`, at y = 0 |
| Wheels | parts `0x810c`, `0x8210` (left), `0x8198`, `0x8288` (right) |
| Wheelbase | 2.8125 (front axle z +1.4135, rear -1.399) |
| Wheel centres | x ±0.7245, y 0.32 |
| Tyre | radius 0.371, width 0.399, outer face x 0.924 |
| Drawn with a custom body | 10 % bigger (radius 0.408), standing on the same ground |
| Cockpit view (VR2) | its own bonnet + dashboard model, ROM object `0x94fff9`, keyed as the body |
| Mission select | two unkeyed cars: bodies `0x95ff9d` / `0x960dae`, glass `0x961bbf` / `0x961cb0` |

The ROM object numbers are Revision A's.

## 3. Converting the body (`camaro.py`)

The first converter's stages, kept as the reference for loading, placing,
reducing and the surfaces (the Camaro before its refinement: the current
`camaro.py` works as "The first car refined" below describes; the finished
standard above adds the inlaid details, the parts as faces and the
cleanup). `camaro.py GLB HORNET_JSON PARTS_TXT OUT_DIR`, in a Docker image with
Python, numpy, scipy, trimesh, pymeshlab and Pillow (about 3 minutes). It
writes the model's own paint and the "79" livery as two folders. Its
stages, and why each is there:

1. **Load** the glTF with its node transforms, per material. Convert the
   axes: this model's +x was the car's left, so x is mirrored and every
   face's winding reversed. Check the front by the tail lamps (at -z) and
   the steering wheel.
2. **Place** on the game car's wheels: scale so that the wheelbase equals
   2.8125 (x1.018 here), the axles on the game's, the body lifted only
   1 cm. The game's tyres are bigger than this car's; the arches are cut
   open for them (stage 4) instead of lifting the body, which floated.
3. **Keep the exterior**: paint, trims, chrome, glass, lamps, mirrors.
   Interior, engine, chassis and the model's wheels go.
   - **Overlay layers** (stripes, trims, emblems lying within 1.5 cm of the
     paint, facing the same way) go into the texture only, not the mesh:
     two layers that close fight over the drawing order (flicker).
   - **Lamps' reflector bowls** pressed into a disc at the lens, facing
     ahead (or back): under the game's light a bowl read as a ball; the
     painted lens and chrome ring do the rest.
   - The **grille's slats** pressed into one plane facing ahead: their
     slanted faces reflected the sky as light and blue streaks.
4. **Reduce**, per material (quadric edge collapse, boundaries and normals
   kept, no optimal placement: that made spikes on the roof). The targets
   that worked: paint 10,000 (from 17,500), chrome 2,600, AlumR 1,800,
   Niquel 600, plastic 400; glass, lamps, mirrors, other trims whole.
   - The paint within **30 cm of the glass** is kept whole: reduced, the skin
     lying under the rear window rose through the glass as yellow triangles.
   - **Trims are not reduced further** than those targets: they take their
     colour from the bake, so a trim face pushed out over the glass shows
     the paint's colours (a yellow and blue arrow on the rear window).
   - **Arches:** the faces the arch circle (the drawn tyre's radius + 2 cm)
     crosses are split, three levels, then those inside are removed and the
     edge corners still inside are pushed onto the circle. Removing whole
     faces left notches beyond the circle that showed the dark wheel well
     as black teeth at the front wing.
   - Faces never seen from outside (orthographic depth buffers from 42
     directions) are removed.
5. **Surfaces and the bake.** Each face goes to one of 10 surfaces by its
   normal and material; each surface is a planar projection (u, v linear in
   two of x, y, z) baked to 256 x 128 by rays from outside onto the full
   exterior (with the trims), reading texture x material colour.
   | Surface | Role |
   | --- | --- |
   | 0, 1 | right, left side |
   | 2 | roof (top) |
   | 3, 4 | front, back |
   | 5 | trunk lid (top) |
   | 6 | glass (reflects 15 %) |
   | 7 | wheel wells, underside (matte) |
   | 8 | grille mesh, chin spoiler (matte) |
   | 9 | bonnet (top, in more detail) |
   The cockpit view draws the car's own bonnet (its skin seen from above,
   ahead of the windscreen, set at the game's bonnet's rear edge and
   height); the painting of the game's bonnet from the tops (2, 5, 9,
   planar over x and z) stays only for a body with nothing to draw there.
6. **The livery** (the "79" variant): the paint recoloured, stripes and
   lower panels added, numbers drawn in a font. A number on a door is drawn
   mirrored in the texture so that it reads right on the car; on the roof
   it reads from behind (the chase camera); on the bonnet it is turned 180
   degrees and reads from ahead (as the game's own bonnet text does).
7. **Extra faces**: a dark wheel-well liner a little outside the arch (its
   edge never shows past the wing), a dark floor at the sills, a chin
   spoiler (the game's nose sits 11 cm off the road: without it the car
   floats), the grille as one flat panel (the reduction had left a hole).
8. **Write** `bodies.m2car` (3 bodies, the same faces) and `liveries.rgba`,
   plus previews (`superficies.png`, `turntable.png`).

## 4. Converting the wheel (`wheel.py`)

`wheel.py GLB BODY_DIR...`: the model's own wheel, in the game wheel's
space (hub at 0, axle along x, outer face +x), tyre radius 0.408, outer face
x 0.44, written as `wheel.m2car`; its textures go into the atlas's second
body. Run it after `camaro.py` (which rewrites `liveries.rgba`).

- Weld the vertices before reducing (unwelded, the rim would not reduce).
- Rim reduced to 600 faces; the tyre kept whole (reduced, its inner half
  collapsed and the tyre went narrow). Result: 1,679 faces.

## 5. Checking in the game

The local `captura/shots.sh NAME BODY_DIR INPUTS FROM TO EVERY [OUT]` dumps
`m2gpushot` frames as PNG (scale 4, MSAA 4x, smooth cars, reflections,
shadows). The shots that found every problem so far:

| Shot | Inputs | Frames |
| --- | --- | --- |
| Rolling start, the car from ahead, close | repository `scripts/inputs/attract_long.txt` | 2160..2260 |
| Mission select, the full turn of both cars (the one not chosen dimmed) | `captura/in_vr2.txt` | 1400..1760, every 10 (1770 on is an empty blue screen) |
| Race, normal chase | `captura/in_vr4.txt` | 2400..2700 |
| Race, high chase camera | `captura/in_vr4.txt` | 2800, 2900, 3000 |
| Cockpit view (VR2) | `captura/in_vr2.txt` | 2800, 2900, 3000 |

In a select-screen frame (1984 wide) the chosen car is the left half
(about x 60..960, y 740..1100), the dimmed one the right half (x
1040..1940, on a red backdrop once chosen). In a 1984 x 1536 race frame
the car is about x 500..1500, y 800..1500 in the chase view (the
bumper, diffuser and tyres reach y 1450) and x 700..1180, y 740..1120 in
the high one. The chase view from behind is the one that decides whether
the flares leave the tyres in view. `shots.sh` deletes its output
folder first: one folder per shot, never a shared one. Look at each next
to the same frame with the game's car (`none`), and zoom on the wheels and
arches, the glass, the numbers and the shadow. Yellow or green patches on
the paint at the start are its reflections of the scenery (a capture
without `--car-reflections` tells).

The close views, which the review (criteria 6 to 9) is made in:

| Close view | From | Shows |
| --- | --- | --- |
| The front from the car's left | rolling start, 2160..2260 every 10 (2250: the car 900 px wide) | the only frames of the set with the front seen from that side |
| The front from the car's right | select 1720, 1760, the car cut out, x4 | bonnet lines, lamps, grille, plate |
| The rear, and the rear from each side | select 1540; 1480 (right), 1600 (left), cut out, x4 | lid lines, lamps, plate, badges |
| Each side | select 1400 (right), 1640 (left), cut out, x4 | door lines, stripe, frames, vents, rivets |
| Any detail from any angle, both sides | `captura/closeview.py BODY_DIR OUT` | 20 views of the built files in the game's drawing order, 30 s; each left/right pair side by side, one flipped, the difference in red |

The select screen's turn shows the front only from the car's right, the
rolling start only from its left: a front detail is looked at in both.
`closeview.py` is for finding; the game's frames decide. The final sheet
has a close-up page: front, rear, each side and each plate, picture and
game cut to the same detail (`captura/closeups.py`; `captura/crops.py`
cuts the chosen car out of select frames). `captura/caps.sh` runs the
standard shots and `captura/bench.sh` the interleaved benchmark.

Then the frame time, with the heaviest settings (about 19 s a run; check
first that no other `m2gpushot` runs, `pgrep -x m2gpushot`, or the
timings mix):

    M2_PLAYER_BODY=FOLDER build-daytona/m2gpushot build-daytona/rom_cache/daytona 3600 \
      --dump OUT --every 100000 --nvram captura/nvram --inputs captura/in_vr4.txt \
      --aspect 21:9 --pixels 3440x1440 --msaa 4 --aniso 16 --smooth-cars \
      --car-reflections --car-shadows --bloom --crt --bench

The number to read is the line "game + renderer CPU per frame: median
...". The game has 17.4 ms a frame. The first Camaro (20,869 faces, four
1,679-face wheels): median 7.9 ms, p99 11.4 ms (the game's car: 1.2 ms);
the other cars are in "Budget" above.
In the game, the log's line "the race ran at N frames/s with this image
quality" appears only when a race fell short of full speed.

## Problems met, causes and fixes

| Seen | Cause | Fix |
| --- | --- | --- |
| Generated Chevette distorted at the start's front shot | photo-to-3D shape and texture | an artist-made model |
| Body refused, then a spike on the roof | the face limit (1,024, then 8,192); decimation's optimal placement | player's limit 32,768; no optimal placement |
| Rear glass looked like a mirror from the high camera | the game's own window reflection (an overlay of oba 4, texture `0x4409`/7064) drawn over the new glass | the overlay dropped with the glass; glass reflects 15 % |
| Light streaks inside the car's shadow | faces turned from the camera were not drawn, so the sun saw holes | they still cast the shadow (never drawn); now only those facing the sun |
| Crude tyres (the game's, too low in detail up close) | the game's wheel | the model's own wheel (`wheel.m2car`) |
| Wheels looked small beside the new body | the game's car's proportions | wheels drawn 10 % bigger, standing on the same ground |
| Mission select still showed the game's car | those two cars are not keyed | found by their ROM objects |
| The car not chosen at the mission select was bright | one light fit for both cars | each car lit by its own fit over the brightest's |
| Numbers read backwards | texture projection seen from the other side | drawn mirrored on the doors, turned 180 degrees on the bonnet |
| Cockpit view showed the game car's bonnet | a separate model, the camera inside the car | that bonnet painted with the body's top surfaces; the dashboard kept |
| Cockpit view kept the game bonnet's shape (seen on the Challenger) | the painting above | the body's own bonnet drawn there: its top skin only (what lies under it sorted over it, the view has no depth test), slid and raised to the game's bonnet's rear edge, drawn before the dashboard |
| Black teeth at the front wing | arch cut removing whole faces | faces crossing the arch split, then rounded onto the circle |
| A glitch and 46.9 frames/s in a race | about 42,000 polygons a frame for the car; the renderer's per-corner grouping | smaller mesh and wheel; corners sorted, the body's made once at load; turned-away faces only when facing the sun |
| Yellow triangles on the rear window after reducing | paint under the glass and trims reduced into it | paint near the glass and trims kept at their targets |
| Lamps read as balls; sky streaks on the grille | reflector bowls, slanted slats | pressed flat |
| Lamps blurred up close | about 20 texels each in the front and back textures | their own texture cells, their faces kept whole |
| Panel lines broken, wobbly or smudged | drawn in the planar textures from the bake's noise | the model's own panel edges cut into the skin (`inlay()`) |
| Lines laid over the skin showed in pieces, even as decals | the game draws faces in the order of each face's mean depth | cut into the skin instead: nothing lies over anything |
| Thin dark marks on the clean paint | the model's seams, gaps and trim baked into the textures | every thin dark texel next to the paint takes the paint's colour, the small black parts as their own faces |
| The car looked hollow at the select screen | see-through glass with too little behind it | glass 75 % covered, the interior kept |
| Black teeth beside the bigger tyre | the tread's full-width faces sorted by mean depth, drawn over the wing | the tread split into 4 strips |
| The backdrop showing between rim and tyre | a gap between the rim's lip and the bead | a dark band under the bead, the rear disc doubled |
| Stripes ending ragged at the arches; exhaust pipes lost faces | the arch cut made before the inlays, and through every material | cut after the inlays, through the paint only |

## The second car: a Beetle at its real size (history)

A 1960s Beetle (Sketchfab, CC BY 4.0), converted by `fusca.py` in the same
folder: how it reached the finished standard, round by round, the dead ends
included (the texture lines, the ribbons, the decals). Its final state is
the last items below and "The finished standard" above.

- **Try the colour first.** Before any conversion, `fusca/preview.py GLB
  OUT.png [HEX]` draws the whole model from four angles with its paint
  recoloured (point splats of the textured faces, a Lambert light, a rough
  sky reflection): a black paint keeping the model's own wear was chosen
  from it. Converted and seen in the game, the black did not look good; the
  car keeps the model's own paint for now, a little warmer (below).
- **Its size: real.** Its wheelbase is 2.40 against the game car's 2.81.
  Scaled to the game's wheels it would have been 1.93 m tall; at its own
  size its wheels sit in its own arches, drawn there by `M2WHEEL2` (see
  CUSTOM-CARS.md). No arch to cut, no body to lift. The game's collisions
  and checkered shadow stay the game car's (about 40 cm longer at each
  end).
- **No material is the paint.** Its four materials are texture atlases
  shared by paint, glass, trim and tyres. Faces are told apart by the
  texture under them: glass where the texture is transparent (alpha under
  250), paint by the texture's hue (the beige: 20 to 55 degrees, some
  saturation), and on the body panels' own atlas the pale, worn paint too.
  The paint is recoloured in the textures before the bake, each texel's
  light kept relative to the paint's median (the wear stays).
- **Interior kept, behind see-through glass** (`car.ini`, `glass_opacity=0.75`:
  at half, the select screen's backdrop showed through the side windows
  and the car looked hollow). The cabin's faces
  are those enclosed by the car (rays from them up, down, to both sides,
  ahead and behind all meet it) between the windscreen's and the rear
  window's bases, above the sills: limited to the cabin, the engine bay's
  and front boot's faces, reduced, poked through the lids as dark patches.
  Seen through the glass, they cannot take a projected texture (it would be
  the body's); each takes the mean colour of its own texture (darker, in
  the car's shade) from a palette in surface 8 (matte), in 2 x 2 texel
  cells. The visibility test counting the glass as opaque found almost
  none of them: the glass's faces mostly have opaque texture at their
  centre, and rays pass by them.
- **The paint, warmer.** The model's own sand (193, 158, 108) read brown
  under the game's light and sky reflection; its paint texels are turned
  7 degrees towards yellow, their saturation x1.25 and value x1.10
  (`fusca.py GLB OUT warm`).
- **A style from reference pictures** (`fusca.py GLB OUT tuned`): two
  pictures of a customised Beetle were matched with the textures and small
  changes only: one clean glossy yellow over every paint texel (wear and
  rust painted over), every chrome face (bright, grey) baked black
  (bumpers, mirrors, handles, emblems, bezels) through a per-face colour
  override, three black stripes between the arches, round headlamps
  (found as the bright or glass faces facing ahead on the wings) redrawn
  with a white halo, engine-lid louvres, red bars on the rear bumper, a
  black interior, the wheel 35 % wider with a black centre and its lip kept
  polished, the body 2.5 cm lower. The lamps are drawn in 3D, in the
  bake's colour lookup: every texel whose point lies inside a lamp's circle
  (seen along the car) and in front of its lens takes the lamp's design, so
  the same lamp shows on whichever surfaces see it (drawn on the front
  texture alone, a lamp seen at an angle showed pieces of everything).
  Headlamps are found by their lens's grey reflector facing ahead on the
  wings (the lens is not transparent in this model; a test with all bright
  faces ahead took in the bumper and missed the lamp); tail lamps by their
  saturated red and amber faces facing back. The paint mask leaves out
  saturated texels (the amber lenses had turned yellow). Dark tinted glass.
  Baked into the front and back textures a lamp got about 20 texels
  across and looked blurred: the lamps' faces are now kept whole and take
  their own surface (8; the interior palette moved into surface 7, beside
  the wheel wells' dark), a 128 x 128 cell per kind, each face mapped by
  its position on the lamp (mirrored on the left). Front and back lamps sit
  at nearly the same x and y: the kind is chosen by the end of the car
  first. A lamp found from every saturated face around it took in the
  bumper and paint (an ellipse of 0.3 m): only the lens's own depth counts.
- **The rear rebuilt to match the picture.** Painting over the model's own
  parts kept their shapes (round lamp housings with notches, a bumper with
  overriders and tubes), so they are replaced with new geometry: each tail
  lamp an oval dome (16 x 22 cm, on the model's lens and facing as it does,
  a black skirt back into the wing), textured with the lamp's own cell
  (red, an amber band, a bright core, a clear reversing lens); a straight
  gloss-black bar with red LED inserts in place of the bumper; a two-sided
  black ducktail lip along the engine lid (its root found by rays from
  behind). Vertical vents under the window, slats on the lid, and the
  plate painted over. New faces are wound outward by a helper that checks
  each one's normal against the direction it should face.
  Second pass: a straight bar looked flat against the round rear; it now
  follows a parabola fitted to the skin between the lamps (the apron below
  is ragged once the bumper is gone, and points near the lamps hit them),
  its curvature limited so the ends wrap gently. The lamps sit 2 cm into
  the wing with a lower dome, and the model's housing faces around them are
  painted the body colour (black, they read as a second lamp). A fictitious
  grey plate (RFN-0909) is its own quad in front of everything across its
  area, its picture in surface 7's free lower half. The trim within 3.5 cm
  of the glass takes the body colour: baked at the sides' resolution it
  smeared the windows' edges. 20,004 faces.
  The model's panel seams and gaps (the wings against the body, the lids)
  baked as thin broken dark lines and read as dirt on the clean paint:
  after the bake, dark texels in lines up to 4 texels wide (what a 5 x 5
  opening removes) next to the paint take the nearest paint's colour; wide
  dark areas and the gloss-black parts (handles, emblem, bezels: their
  exact colour) stay, and the stripes and vents are drawn afterwards.
  The wings' piping (a chrome strip, so baked gloss black) stayed as
  smudged lines: long thin chrome pieces (over 25 cm, under 3.5 cm wide on
  average) take the body colour before the bake, and after it gloss-black
  lines 1 to 2 texels wide and at least 8 long, and specks of 4 texels or
  fewer, take the paint's. The side stripes are drawn with each texel's
  coverage of the band (anti-aliased) instead of a hard edge.
  Removed, the panel lines were missed: the car looked featureless. So the
  lines found (the seams and the piping) are redrawn after the cleanup: their
  breaks closed (a closing with a 2-texel disc), small bits dropped, thinned
  to one texel (skeletonize), lines shorter than 10 texels dropped, and each
  drawn as a continuous anti-aliased gloss-black line from the distance to
  it. The piping's own faces are also sampled densely and drawn on the
  surface of the nearest face. The lines are about 3 texels wide (thin ones still read as
  broken) and breaks up to 6 texels are closed.
  Lines found in the texture stayed unfaithful (wobbly, smudged where the
  planar projection met a curve), so they are now **geometry**: the model's
  own open panel edges (the wings, doors, lids, window frames) on the
  exterior paint, chains under 15 cm and those by the old bumpers or below
  30 cm dropped, snapped onto the game's reduced skin, each edge a matte
  black ribbon 2 cm wide, 4 mm off the skin, written as decal faces
  (surface 17, see CUSTOM-CARS.md): 332 quads, 30 chains, 33.9 m. The
  texture keeps no line at all: every thin dark texel next to the paint
  takes the paint's colour, but where a small black part lies (handles,
  the emblem, mirrors: chrome pieces under 30 cm, projected onto each
  surface they face), and the glass's colour baked into the body textures
  goes too. The sills between the arches are a clean black skirt.

- **A sport wheel made in the converter.** The model's wheel is a steel
  hubcap wheel; recoloured, it could not become the picture's. The
  converter builds one: a revolved profile for a wide, low-profile tyre
  (34 cm, rim radius 0.25 of 0.362), a polished lip and a deep barrel
  (banded texture by angle, for the chrome's reflections), seven dished
  black spokes with sides, a dark brake disc behind them, a chrome cap;
  666 faces, set 3 cm further out (M2WHEEL2's half track) so it stands
  out of the wing. Two things to check on a revolved part: each profile
  segment's normal (its direction rotated a quarter turn) must point out of
  the part (a flipped one made the tread face inward and the tyre look
  hollow from behind), and the barrel must reach the disc (looked at
  obliquely, the gap between them showed the background through the
  spokes). (The ducktail and the diffuser came later, as new geometry;
  wider arches were still left out.)
- **A straight wheel.** The model's front wheels are steered about 9
  degrees: spun about the game's axle, that one wobbled. The rear wheel is
  used, its axle (the direction of least spread) turned exactly onto x.
- **Bumpers on their own surface.** Seen from behind, the rear bumper lay
  over the apron, whose texels then took the chrome (white patches below
  the engine lid, and on the sides' corners). The bumpers (bright trim, low,
  within 42 cm of the ends) get surface 5, the front bumper baked from ahead
  in one half and the rear from behind in the other; every other surface is
  baked with the bumpers taken out of the full-detail mesh, so its rays
  reach the body behind them. The engine lid joins the roof's surface (seen
  from above they do not overlap).
- **Its own wheel** from the model: centred, mirrored like the body, its
  origin moved to its inner face (as the game wheel's), the part of the
  atlas it uses resampled into the wheel texture.
- Intermediate result (before the refinement round): 19,924 faces (about
  4,000 of them the cabin), a 1,180-face wheel, median 6.7 ms.

- **Details inlaid in the skin.** Even as decals the ribbons still lost in
  places: the game's order is per face, and a long ribbon quad against
  big skin faces can still be sorted behind some of them; subdividing the
  skin under them reached 164,000 faces. What works: cutting each detail
  into the skin itself (`inlay()` in fusca.py). A detail's band is cut out
  of the triangles it crosses and the pieces inside take a flat colour, so
  nothing lies over anything and the order cannot break it. Done this way:
  the panel lines (one 2 cm line per seam; none on the roof gutters, the
  arch edges or around the lamps, which the pictures do not have; 115
  bands, 15 chains, 13.6 m), a black rubber along every window's edge
  (14.8 m, hiding the jagged join), the three side stripes, the lid's 29
  vertical vents, its slats and the VW emblem drawn in strokes. The small
  black parts (handles, mirrors, piping, running boards) are faces of
  their own in flat black, out of the bake. Front and rear rebuilt to the
  pictures: the front bumper, its brackets and the plate replaced by a
  black splitter, two round auxiliary lamps (their own texture), the
  headlamp redrawn (a projector, a thin halo, a black bezel), the bonnet
  emblem and horn grilles gone; at the rear a clean apron in the body
  colour with two dark inserts, a black diffuser, twin exhaust tips, a
  wedge ducktail; 32 rivets round the arches; a six-spoke wheel. Budget
  kept by reducing the interior (4,014 to 2,485 faces) and the paint and
  trim. 20,862 faces, a 660-face wheel, game + renderer CPU median 6.88 ms;
  smudges 0 on every surface (counted over the texels the faces use).
  Still unlike the pictures: no wing flares (they need new geometry), the
  rear plate kept (the pictures have none), the door's outline only the
  model's thin gap, the side stripes ending 12 cm short of the front wing,
  the interior simplified.
  Accepted by the user as finished.
- **A driver, and the front bumper back.** A seated driver from a
  separate model (a low-poly race driver, CC BY 4.0, no skin: 21 rigid
  pieces in an A pose): posed by rotating the pieces about hand-placed
  joints, the arms and legs by two-bone inverse kinematics onto the model's
  own steering wheel (left, hands at 10 and 2) and the pedals, small balls
  closing the shoulders, elbows and knees; 1.72 m so the helmet clears the
  roof by 4.6 cm; flat colours chosen by hand (sampled per face from his
  texture they read as grey noise behind the dark glass); 1,466 faces in
  palette cells of surface 7. The cockpit view does not draw the body, so
  he never covers it. The front bumper (the user wanted it back; a
  refinement round had replaced it with the pictures' splitter) is the
  model's own bar, overriders and brackets at full detail (reduced, the
  overriders became spikes) in flat gloss black. Both plates are decal
  quads: as plain faces, the bumper's or the lid's faces covered half of
  them at some angles. A final full review of every planned item, against
  the pictures and the earlier versions, found the rear plate's half lost
  at some select-screen angles and a stray triangle in front of the nose,
  both already in the accepted version; both fixed. 23,121 faces; median
  +0.6 to +0.9 ms against the version before (6 interleaved runs each).
  Lesson: after every refinement round, review the whole car item by item
  against the plan and the previous versions, not only what was asked.

## The first car refined: the Camaro (history)

The first Camaro (sections 3 and 4) was rebuilt to the Beetle's standard
by a refinement agent, then given bigger wheels; the user accepted it as
finished.

- **To the standard.** At its own proportions (the first one was narrowed
  by 7 % and scaled onto the game car's wheels), its wheelbase scaled onto
  the game car's axles, its own wheels on `M2WHEEL2`. Nothing baked by
  projection: every face a flat colour from a palette (paint, chrome,
  aluminium, black), since this model has a paint material of its own.
  The "79" livery's blue lower panels and stripes, the panel lines and the
  rocker mouldings are cut into the skin (`inlay()`); what needs a picture
  (the numbers, the grille with its lamps, the tail lamps, the badges, the
  louvres and vents) has its own texture cell at a few millimetres a
  texel, drawn or baked from the model's own geometry. The wheel: the
  model's Rally rim as a revolved profile carrying a picture of it. The
  select screen's turning car refined with it. 16,990 faces.
- **Bigger wheels standing out.** The game car's tyre radius (0.408, from
  0.334), 0.35 wide (from 0.25), the outer face at |x| 0.99: the tyres
  stand 7 cm out of the body at the front, 5 cm at the rear (the body
  keeps its real width, so less than the first Camaro's 9 cm; `WHEEL_OUT`
  and `WHEEL_W` in `camaro.py` push them further, the game's shadow
  reaching |x| 1.0). The converter sets the size and writes it to its JSON,
  `wheel.py` reads it; the wheel's centre rises, standing on the same
  ground. The arches are cut as circles (tyre + 2 cm) through the paint
  after the inlays, a dark liner drum inside. Two defects of the wheel
  fixed on the way (black teeth beside the tyre, the backdrop between rim
  and bead: see "Problems met"). Outside the arches every face and texture
  stayed byte-identical to the accepted version, checked face by face.
  17,574 faces, a 1,512-face wheel; median +0.35 ms (7 interleaved runs
  each). Left as they are: the rim large against a thin tyre wall (the
  whole wheel scaled), a steered front wheel may touch the arch's edge
  (2 cm clearance).

## The third car: a sporty Escort, the process validated (history)

A 1986 Ford Escort XR3 (Sketchfab, CC BY 4.0, 37,000 faces, 15 plain
materials, one welded skin), converted by an agent that followed this
document as the recipe and recorded its gaps (folded in above), matched to
two pictures of a widened version: wing flares with rivets, deep-dish
six-spoke wheels, black splitter, bumpers and ducktail, fog lamps, a
pinstripe, plates front and rear.

- **Method:** the refined Camaro's flat palette (nothing baked); seams
  from the skin's grooves; flares, bumpers, splitter, valance, exhaust, a
  made wheel and the cabin's missing parts as new geometry; the driver
  through an adapted `piloto.py`. Real size on its own axles (wheelbase
  2.383), body lowered 50 mm, wheels out to |x| 0.965 (`M2WHEEL2 1.1989
  -1.1844 0.6500`), a 9 cm dish.
- **Time:** about 63 minutes from the first reading to the installed car
  (9 rounds), 26 of them waiting for conversions, captures and the
  benchmarks. Half the refinement time went to the drawing order (the
  section above).
- **Result:** 21,684 faces (driver 1,473), a 636-face wheel, median
  6.91 ms (6 interleaved runs each; the Beetle 7.40). Left unlike the
  pictures: flat lamps, no diffuser shape, a dark plain interior, the body
  not as low.
- **Round 10:** the flares removed at the player's request (the tyres hidden from behind), the
  tyres standing out of the cut arches, then narrowed (0.315 to 0.255 wide, outer face
  |x| 0.905): 21,481 faces.

## The fourth car: a DeLorean, the first made with the generic converter (history)

A DeLorean DMC-12 time machine (461,595 triangles, 43 nodes, 30
materials), converted by an agent with the generic converter from the
start, kept on the working computer only: its tags and node names say it
was taken from a commercial game (section 1). No reference pictures: the
model itself, rendered at the game's angles (`captura/ref.py`), was the
reference.

- **Method:** the palette path, but each node was a kind of material
  ("Coloured", "Engine", "Interior") holding 700 to 1,100 pieces over a
  colour atlas, so a node could not be a part: the library gained a
  dense-model path (`carlib/dense.py`: colour from the texture face by
  face, the never-seen test on the full model, reduction in chunks keeping
  cables round, pictures baked from the model for the lamp clusters).
  Real size on its own axles (`M2WHEEL2 1.2242 -1.2097 0.6500`), one
  wheel size between the model's front and rear (radius 0.312), the
  model's turbine rim made into a 495-face wheel; the stainless panels one
  flat light grey with the game's reflections; the model's own thin panel
  gaps kept (widening them on this path is not in the library yet).
- **Time:** 56 minutes in all, 47 to the installed car (the Escort: 63),
  about 13 rounds, 26 minutes waiting.
- **Result:** 26,483 faces (driver 1,210, cabin 2,264), median 7.74 ms.
  Left out: badges and lettering, the perforated meshes, the glow layers,
  callipers, seat belts; marks smaller than a reduced face (the reactor's
  yellow ticks) take the face's average colour.
- **The converter:** a 170-line config and a 57-line hook; the library's
  additions are shared, and the three earlier cars' proofs stayed
  byte-identical.

## The fifth car: a Chevette GT, the workflow validated (history)

A Chevrolet Chevette SL 76 (Sketchfab, CC BY 4.0, 53,839 triangles, loose
body panels, the trim one node of 70 pieces over an atlas), restyled after
two pictures (olive paint, a GT stripe, short riveted flares, the Escort's
six-spoke wheel, chrome bumpers with black overriders, fog lamps, a boot
spoiler, a diffuser), converted by an agent that followed this document
stage by stage and reported each stage as it went.

- **Time:** 37 minutes in all, 29 to the installed car (the Escort 63, the
  DeLorean 47), about 13 of them waiting; four of the drawing-order rules
  applied as written.
- **Result:** 21,729 faces (driver 1,277, cabin 2,413), a 636-face wheel
  (`M2WHEEL2 1.1911 -1.1766 0.6400`), median 6.70 ms, below the Escort's in
  all six pairs. The tyres show from behind: the flares' lip 6 cm inside
  the tyre's face.
- **The close review:** the user then found, in a close front view, a
  bonnet line missing on one side and the plate's letters too large; a
  second agent's close review (criteria 6 to 9, written after it) found a
  dozen defects of the same kinds (door lines never drawn, the bonnet's
  rear line, the boot lid's outline, a lock drawn over the spoiler, an
  askew diffuser) and fixed them in 37 minutes: 21,992 faces, 6.89 ms.
- **The converter:** a copy of the Escort's config kept for its frame
  (stance, palettes, wheel, driver, files); the parts, bands and made faces
  are the model's own. 55 library lines (a model off centre, some faces of
  a node as a part, one mirror mirrored, panel lines for a by-node model of
  panels, tapered flares); the five earlier cars' proofs stayed
  byte-identical.

## For the next car

1. Follow "The path, in short" and build to "The finished standard" from
   the first conversion; do not start from the texture-only approach.
2. Copy the nearest config in `carro/cars/` (`escort.py`, `fusca.py` or
   `delorean.py`, as in "The path" step 4) and work down it (its
   README's "A new car, step by step"): the axes first, then a one-part
   build that already runs in the game, then the parts and their
   reduction targets, the skin's details, the cuts and made parts, the
   pictures, the wheel, the driver, the files.
3. Keep car-specific code in the hook module, as small as it can be; a
   step two cars need goes into the library, and `prove.sh` for every
   finished car must still print PROOF OK (the rebuilt files
   byte-identical to the installed ones).
4. Run the shots, the select screen's turn, the cockpit and the benchmark;
   refine (or brief a refinement agent) until the acceptance criterion
   holds, the item-by-item review included.
5. Write its credits file (author, link, licence, what was changed), make
   its `preview.png`, install it into `cars/NAME/`, check its card, and
   add it to the README's table with its `prove.sh` line.
6. Clean up ("The path" step 9) and run the proofs again.
