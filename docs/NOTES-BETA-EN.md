# Daytona Arcade Native: beta 0.10

Second public release, for Windows. Setup guide:
[INSTALLATION-EN.md](INSTALLATION-EN.md) (French:
[INSTALLATION-FR.md](INSTALLATION-FR.md)).

## Where it comes from

Daytona Arcade Native builds on alphanu1's static recompilation
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp).
The cabinet's program is translated to native C++, with no emulator. This
edition keeps the original simulation, so the cabinet's driving and race,
and works on the picture and on comfort on PC.

## New in 0.10

These answer requests from the video comments and GitHub.

- **Interface language**: open Settings in the launcher and use "Langue / Language".
  French and English first. The choice is kept, and the first run follows
  Windows' language. Portuguese, Spanish, Chinese, Japanese and Arabic are
  being prepared.
- **Picture format**: now first in the display settings. **Arcade 4:3**
  (the cabinet's picture, centred with bars), Automatic (the window's
  shape), 16:10, 16:9 or 21:9. When the window is wider than the selected
  format, the game centres the picture with side bars. A narrower window
  limits the width of the widescreen view.
- **120 frames/s smoothing repaired**: in a race, 20 of the 39 opponent
  cars shook or doubled. All of them are tracked now, and the motion is
  steadier. The option is still off until checked.

## Already in 0.9

- A PC menu: normal race or Time Attack, automatic or manual gearbox,
  settings.
- Widescreen: scenery and cars are visible at the sides.
- An adjustable draw distance.
- 3D scenery, optional: fir trees, grandstands, Dinosaur Canyon's bridge
  pillars.
- A tube-cabinet screen curve and a saturation setting.
- Opponent cars always detailed, optional: one model at every distance.

## Known limits

- **120 frames/s smoothing** needs the Hardware renderer and the "Double
  buffer" draw mode. The frame rate depends on the display and the
  graphics card.
- **Opponent cars** are the cabinet's original models. A more detailed
  reconstruction is planned.
- **Ray tracing** (sun shadows, light in the air) is still being tuned and
  is not in this release.
- Only the *Daytona USA* Revision A ROM set (`daytona`) gives the PC menu and
  the enhancements.

## No game data

The repository contains no ROM and no code derived from the ROM. The game is
rebuilt on your computer from your own copy.
