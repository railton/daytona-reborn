# A custom body for the player's car (experimental)

Settings > Enhancements > **Player's car** draws a body you made in place of
the player's car body, with the hardware renderer. Only the look changes: the
game's own wheels (drawn 10 % bigger), its light, the car shadow, the physics
and the online data stay the game's. Other players online still see the
game's car.

How a car was made from a downloaded 3D model, what was tried and dropped,
and the problems met: [CUSTOM-CAR-WORKFLOW.md](CUSTOM-CAR-WORKFLOW.md).

A body is a folder in the game's data folder, `cars/NAME/` (on Linux
`~/.local/share/daytona-recomp/daytona/cars/NAME/`), holding two files. The
setting lists every such folder; it takes effect at the next start.
`M2_PLAYER_BODY=FOLDER` uses another folder for one run.

Third-party models stay on your computer: a body made from someone else's
model follows that model's licence, and none is part of this repository.

## The files

`bodies.m2car`, text (`src/app/gpu/opponent_bodies.cpp`):

    M2CAR1
    then 3 bodies (the player's car uses the first; repeat it), each:
      FACES (1..32768 for the player's car; 1..1024 for opponents)
      per face: COUNT (3 or 4) SURFACE (0..9) SHADE (0..2)
                then COUNT corners: x y z u v

- Model space is the game car's: +x its right, +y up, +z forward, about one
  unit a metre, |x|, |y|, |z| at most 3. Measured on the game's car: wheel
  centres x ±0.7245, y 0.32, axles z +1.4135 and -1.399, radius 0.371 (drawn
  0.408 with a custom body, standing on the same ground; outer face x 0.944);
  its body is 4.93 long, 1.73 wide.
- Faces are wound outward. Their light comes from the game: each frame the
  game car's faces fit the light the game gave them, which lights yours by
  their normals; SHADE is used only when that fit is not available.
- Surfaces are textures, 256 x 128 each. Their roles for the player's car:
  6 glass (reflects the sky at 15 %), 7 and 8 matte (wheel wells, a grille's
  mesh, spoilers: no reflection), the others painted bodywork (full
  reflection with "Paint reflections").
- The cockpit view (VR2) keeps the game's own bonnet and dashboard, the
  bonnet painted with your body seen from above: at each point the surface
  of your highest face there, so the surfaces seen from above must have a
  texture that is a plane over x and z (u and v each a linear function of
  x and z), as a top-down projection gives.
- Cost: every face is work for the processor each frame, about 0.3
  microseconds (a 20,000-face body with four 1,700-face wheels: about 5 ms
  a frame, against 0.3 ms with the game's car). Keep the body under about
  20,000 faces and the wheel under about 2,000.

`liveries.rgba`: exactly 3 x 10 x (256 x 128 + 128 x 64 + 64 x 32 + 32 x 16)
RGBA pixels (5,222,400 bytes): per body, per surface, the 256 x 128 texture
and its mip levels, rows top down, as plain colour (unlit).

## Its own wheels (optional)

`wheel.m2car`: `M2WHEEL1`, then one body in the same face format: a right
wheel in the game wheel's own space (the hub at 0, the axle along x, the
outer face towards +x; the game's spans x 0..0.399, radius 0.371). It is
drawn on each of the game's wheels, which it follows as they spin and
steer, mirrored for the left ones, and raised so that its tyre (radius: its
farthest corner from the axle) stands on the same ground. Its surfaces are
the atlas's second body's (`liveries.rgba`, body 1); SHADE is its light.

## Limits

- The game's car is replaced in the race, its start, replays and the
  mission select screen (its two turning cars); in the cockpit view (VR2)
  only the bonnet's paint changes.
- The game's own window reflection is dropped with its glass.
- Opponents keep their bodies (`M2_OPPONENT_BODIES`, the 1024-face format,
  is separate and unlit).
