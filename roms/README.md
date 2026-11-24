# Your ROM set goes here

Copy your own Daytona USA ROM set into this folder, named exactly after the
set:

- `daytona.zip` (or `daytona.7z`): Revision A, 1994 (MAME's `daytona`), the
  version with the PC home menu and the Online mode.
- `daytona93.zip` (or `.7z`), optional: Deluxe '93 (MAME's `daytona93`).

Then build (`docs/BUILDING.md`); on Windows, `Build Daytona.bat` asks for the
file and copies it here itself. The game finds `roms/daytona.zip` on its own.

Git ignores everything in this folder but this file: a ROM set is never
uploaded. No ROM set comes with the project.
