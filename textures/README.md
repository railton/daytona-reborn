# The high-resolution texture pack goes here

The race's textures upscaled four times by AI models (Settings > **AI
textures**, [docs/HIRES-TEXTURES.md](../docs/HIRES-TEXTURES.md)). At each
start the game looks in this folder:

1. `hires.m2tex`: the pack, used when it is valid;
2. else a `.zip` (or `.7z`) holding a `.m2tex` file: checked, unpacked here
   as `hires.m2tex` and used (the setting "Use the high-resolution textures"
   is turned on); a file that is not a valid pack is left out, and the
   game's log says why (`daytona: high-resolution textures: ...`);
3. else a pack made before in the game's data folder is moved here;
4. none: Settings > Advanced > AI textures makes one from your own ROM set (Linux, for
   now), saved here.

```
textures/
├── README.md        this file
├── hires.m2tex      the pack in use
└── hires.zip        optional: a pack to bring in (hires.m2tex inside)
```

The pack is made of the game's own textures: git ignores everything in this
folder but this file, and a pack is never shared. It is used by the
Hardware renderer only; a texture the pack does not have is drawn as before.
