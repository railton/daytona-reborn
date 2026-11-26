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
beside the repository): `camaro/` (camaro.py, wheel.py), `fusca/`
(fusca.py, preview.py), `captura/` (shots.sh and its inputs).

## The path, in short

1. Find an **artist-made 3D model** whose licence allows changing it, in
   glTF (`.glb`). Not a car generated from photos (see "Tried and dropped").
   Gather **reference pictures** of the look wanted (photos, or AI pictures
   of that model in that style); without them the model's own look
   (`preview.py`) is the reference.
2. **Try the look first** with `preview.py` (its colour, before any
   conversion).
3. **Decide its size:** on the game car's wheels (scaled, arches cut) or at
   its own size on its own axles (`M2WHEEL2`).
4. **Convert** it with the converter nearer the model (`camaro.py`: a paint
   material of its own; `fusca.py`: shared texture atlases), to the
   finished standard below: details cut into the skin, small black parts
   as their own faces, lamps and the plate on their own texture cells, the
   textures only paint and large areas.
5. **Its wheel:** the model's own (`wheel.py`) or one made in the converter
   (the Beetle's sport wheel).
6. **Check it** in every standard shot, the select screen's full turn and
   the cockpit, next to the reference; measure the smudges and the frame
   time (the performance protocol below).
7. **Refine** in rounds until the acceptance criterion holds; a refinement
   agent can run the loop (the brief below). End every round with a full
   review of every planned item against the plan and the previous
   versions: a round can silently remove a part (the Beetle lost its front
   bumper that way).
8. **Use it:** copy the folder to the data folder's `cars/NAME/` and pick it
   in Settings > Cars (it changes at once), or `M2_PLAYER_BODY=FOLDER`.
   Give it a `preview.png` for its card (the select screen's frame of it,
   the blue backdrop made transparent) and a `name=` in `car.ini`.

## The finished standard

What the accepted Beetle does, which every new car follows. The reasons
are in "Problems met" and in the Beetle's history further down.

### What goes where

| Detail | How it is made | Why |
| --- | --- | --- |
| Paint, large areas | the planar textures (256 x 128 a surface), baked from the full model | the only thing they render well |
| Panel lines (wings, doors, lids), window rubbers, stripes, vents, slats, emblems | **cut into the skin** (`inlay()`): the band is cut out of the triangles it crosses, the pieces take a flat colour | drawn in a texture they blur and smear on curves; laid over the skin (even as decals) they lose to the game's per-face depth order |
| Panel lines: which ones | only the model's own open panel edges on the exterior paint, one 2 cm line per seam, the ones the reference has | lines from the texture's noise are wobbly and unfaithful |
| Small black parts (handles, mirrors, piping, running boards) | their own faces in a flat colour, kept out of the bake | baked, they smear the paint behind them |
| Headlamps, tail lamps, auxiliary lamps | their own texture cells (surface 8, 128 x 128 a kind); new geometry where the model's shape differs from the reference | baked into the front texture a lamp gets about 20 texels |
| Plate | its own quad, its picture in surface 7's free half | sharp letters |
| Bumpers, splitter, diffuser, ducktail | rebuilt as new geometry to the reference when the model's differ | repainting keeps the wrong shape |
| Glass | flat tint (surface 6), a black rubber inlaid along its edge, `glass_opacity=0.75` when there is an interior | the join between glass and body is jagged |
| Interior | kept when seen through the glass, each face a flat colour from a palette; reduced (about 2,500 faces) | a see-through car with nothing inside looks hollow |
| Wheels | the model's own, or one built in the converter (revolved profile), on the car's axles; check each profile segment's normal and close every gap | a crude or wobbling wheel spoils the close shots |
| Seams, gaps, trim bits left in the textures | painted over: every thin dark texel next to the paint takes the paint's colour, except where a small black part lies | read as dirt |

### Budget

The body under about 22,000 faces, each wheel under about 2,000; game +
renderer CPU median under about 9 ms in the benchmark (the game has 17.4 ms
a frame). The Beetle: 20,862 faces, a 660-face wheel, 6.88 ms. Pay for
details by reducing the interior and the paint first.

### Performance protocol

Every change justified by speed: measure the baseline and estimate the
floor first; one change at a time; control and candidate run interleaved,
at least 6 runs each, medians (and p95, p99) compared; accepted only if it
beats both neighbouring control runs and nothing regresses by more than
about 3 %; each attempt, kept or dropped, appended to a local ledger.

### Acceptance criterion

A car is finished when all hold:

1. **Faithful:** every detail comes from the model's geometry or the
   reference's layout, never from baked noise.
2. **Continuous:** no breaks along any line.
3. **Even:** one width in metres along the whole car.
4. **No smudges:** the converter's ACCEPTANCE line, the thin dark blobs (6
   texels or more, up to 4 wide) left in the paint textures over the
   texels the faces use, outside the meant details: 0 on every surface.
5. **In the game,** the deciding check: in every standard shot, every angle
   of the select screen's turn (both cars, the one not chosen dimmed) and
   the cockpit view, set next to the reference, every feature of the
   reference that the model can carry is there and nothing reads as dirt,
   blur or a misplaced part.

### Refinement rounds with an agent

The Beetle was finished by a refinement agent (a capable model, working
alone for about 45 minutes), briefed with: the reference pictures; the
converter and this document to read first; the commands to convert, to
capture the standard shots and the select screen's turn, and to
benchmark; the acceptance criterion; the budget; and the rules (change only
the local converter, keep a backup copy first, no git, nothing installed on
the host, the model never in the repository, install the result into the
data folders' `cars/NAME/`, a comparison sheet of the game's frames next to
the reference at the end, a report of what still differs). Changes the
game itself would need are described, not made, and reviewed afterwards.

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
- **What makes a model easy:** separate materials (paint, glass, chrome,
  lamps, tyres, rims, interior), the paint as one material, real-world
  scale, no baked-in background. Detail does not need to be low: the
  converter reduces it (a 60,000-triangle model is fine).

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

The first converter's stages (the Camaro before its refinement; the
finished standard above adds the inlaid details, the parts as faces and
the cleanup). `camaro.py GLB HORNET_JSON PARTS_TXT OUT_DIR`, in a Docker image with
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
   The tops (2, 5, 9) must stay planar over x and z: the cockpit view
   paints the game's bonnet from them.
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
| Mission select, the full turn of both cars (the one not chosen dimmed) | `captura/in_vr2.txt` | 1400..1780, every 10 |
| Race, normal chase | `captura/in_vr4.txt` | 2400..2700 |
| Race, high chase camera | `captura/in_vr4.txt` | after 2760 |
| Cockpit view (VR2) | `captura/in_vr2.txt` | after 2760 |

Look at each next to the same frame with the game's car (`none`), and zoom
on the wheels and arches, the glass, the numbers and the shadow.

Then the frame time, with the heaviest settings:

    M2_PLAYER_BODY=FOLDER build-daytona/m2gpushot build-daytona/rom_cache/daytona 3600 \
      --dump OUT --every 100000 --nvram NVRAM --inputs captura/in_vr4.txt \
      --aspect 21:9 --pixels 3440x1440 --msaa 4 --aniso 16 --smooth-cars \
      --car-reflections --car-shadows --bloom --crt --bench

The game has 17.4 ms a frame. The Camaro (20,869 faces, four 1,679-face
wheels): game + renderer CPU median 7.9 ms, p99 11.4 ms (the game's car:
1.2 ms). Every face costs the processor about 0.3 microseconds a frame:
keep the body under about 20,000 faces and the wheel under about 2,000.
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
| Black teeth at the front wing | arch cut removing whole faces | faces crossing the arch split, then rounded onto the circle |
| A glitch and 46.9 frames/s in a race | about 42,000 polygons a frame for the car; the renderer's per-corner grouping | smaller mesh and wheel; corners sorted, the body's made once at load; turned-away faces only when facing the sun |
| Yellow triangles on the rear window after reducing | paint under the glass and trims reduced into it | paint near the glass and trims kept at their targets |
| Lamps read as balls; sky streaks on the grille | reflector bowls, slanted slats | pressed flat |
| Lamps blurred up close | about 20 texels each in the front and back textures | their own texture cells, their faces kept whole |
| Panel lines broken, wobbly or smudged | drawn in the planar textures from the bake's noise | the model's own panel edges cut into the skin (`inlay()`) |
| Lines laid over the skin showed in pieces, even as decals | the game draws faces in the order of each face's mean depth | cut into the skin instead: nothing lies over anything |
| Thin dark marks on the clean paint | the model's seams, gaps and trim baked into the textures | every thin dark texel next to the paint takes the paint's colour, the small black parts as their own faces |
| The car looked hollow at the select screen | see-through glass with too little behind it | glass 75 % covered, the interior kept |

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

## For the next car

1. Follow "The path, in short" and build to "The finished standard" from
   the first conversion; do not start from the texture-only approach.
2. Copy the converter nearer the model and adapt it: the axes (which way
   is the front, which side the right) first; the material names
   (`camaro.py`) or the colour rules (`fusca.py`); the size (game wheels or
   `M2WHEEL2`); the style (paint, livery, the reference's details).
3. Reuse `fusca.py`'s parts: `inlay()` for every line and stripe, the
   lamp cells, the plate, the small black parts as faces, the interior
   palette, the cleanup, the sport wheel, the ACCEPTANCE line.
4. Run the shots, the select screen's turn, the cockpit and the benchmark;
   refine (or brief a refinement agent) until the acceptance criterion
   holds.
5. Write its `CREDITOS.txt` (author, link, licence, what was changed) and
   install it into `cars/NAME/`.
