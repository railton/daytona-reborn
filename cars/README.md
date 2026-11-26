# Your custom cars go here

Each car is a folder in this one, `cars/NAME/`. When at least one of them is
in the format below, Settings shows a **Cars** tab with a card for each one
(and one for the game's own car); with none, the tab is not shown. A click on
a card changes the car you drive at once, also during a race. It needs the
Hardware renderer. Only the look changes: the handling stays the game's, and
other players online see the game's car.

## How the folder looks

One folder per car, its files directly inside it (not in a subfolder). The
folder's name is free (letters, digits, `-`, `_`; no spaces is safest): it
is what Settings remembers.

```
cars/
├── README.md              this file
├── game-car.png           optional: the picture on the game's own car's card
├── my-car/                a car
│   ├── bodies.m2car       needed
│   ├── liveries.rgba      needed
│   ├── wheel.m2car        optional
│   ├── car.ini            optional
│   ├── preview.png        optional
│   └── CREDITS.txt        optional
└── another-car/
    ├── bodies.m2car
    └── liveries.rgba
```

The smallest car is a folder with `bodies.m2car` and `liveries.rgba`. A
`car.ini` for it:

```
name=My Car
glass_opacity=0.75
```

## A car's folder

| File | Needed | What it is |
| --- | --- | --- |
| `bodies.m2car` | yes | the body: `M2CAR1`, then three bodies of 1 to 32,768 faces (the player's car uses the first) |
| `liveries.rgba` | yes | its textures: exactly 5,222,400 bytes (3 bodies x 10 surfaces of 256 x 128 with their mip levels, RGBA) |
| `wheel.m2car` | no | its own wheel (`M2WHEEL1`, or `M2WHEEL2 FRONT_Z REAR_Z HALF_TRACK` for a car of another size); without it, the game's wheels |
| `car.ini` | no | `key=value` lines: `name=` the name on the card (else the folder's name), `glass_opacity=` 0.25 to 1 for see-through glass |
| `preview.png` | no | the card's picture, any size; a transparent background reads best |
| `CREDITS.txt` | no | the model's credits; the card shows its first line (title and author), links left out |

A folder that is not in this format is left out: the tab lists it as left
out, and the game's log says why (`daytona: cars/NAME left out: ...`).

`game-car.png` in this folder, if you make one, is the picture on the game's
own car's card: the select screen's frame of your own game, the blue backdrop
made transparent. Being the game's art, it is never shared.

The formats in full: [docs/CUSTOM-CARS.md](../docs/CUSTOM-CARS.md). How a
downloaded 3D model becomes a car: [docs/CUSTOM-CAR-WORKFLOW.md](../docs/CUSTOM-CAR-WORKFLOW.md).

## Sharing cars

Git ignores everything in this folder but this file: a car made from someone
else's model follows that model's licence (credit its author in
`CREDITS.txt`), and no car comes with the project. Never put anything taken
from the game's ROM in a car.
