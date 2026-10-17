# Build Yield Preview Fix

A small native mod for Civilization VI: Gathering Storm (DX12, Steam build 15038592). It makes the
yields on a builder's improvement tooltip ("+2 Food") match what the tile actually gains. It is a
GameCore Mod Loader (GCML) plugin, so it runs next to Civ6PerfFix or any other
plugin that uses the loader.

## What was wrong

For every yield the game's tooltip printed the improvement's own yield plus the tile's whole
plot-yield modifier total with the improvement in place. The tile's real yield replaces its
current modifier total instead of adding to it, so the tooltip:

- counted modifiers the tile already gets as a gain (Lady of the Reeds, Earth Goddess, the
  Watermill's wheat and rice, Lighthouse coast food, Petra, Russia's tundra bonus, ...);
- did not take off the improvement it replaces, nor the modifiers tied to that one;
- never showed a yield that goes down (lines at 0 or below were skipped, and the text has a fixed
  "+").

Each line now shows the change: the game's number minus the tile's current modifier total and the
current improvement's own yield. Losses show as "-1 Food" (the game's own `LOC_TYPE_TRAIT_YIELD`
text, which prints the sign).

Not covered: modifiers the new improvement brings with it (such as the Lumber Mill's river bonus)
start only once it is built, so neither preview has them, and changes to neighbouring tiles are not
part of this tooltip.

The decoding notes are at the top of `BuildYieldPreviewFix.cpp`. On any other game build the patch
checks the code it would change, finds it different, and leaves the game alone (the loader's log,
`Documents\My Games\Sid Meier's Civilization VI\Logs\GameCoreModLoader.log`, says so).

## Build and package

```
msbuild BuildYieldPreviewFix.vcxproj -p:Configuration=Release -p:Platform=x64
powershell -File package.ps1 [-Install]
```

The plugin is `bin\BuildYieldPreviewFix.dll` and the package `bin\workshop\BuildYieldPreviewFix\`
(`-Install` also copies it into your Mods folder as a local mod). Turning the mod off takes effect
after restarting the game.

The loader is not built here. `gcml/` holds a release of the GameCore Mod Loader repository
(`release.ps1` there writes `bin\release\gcml\`): the loader DLL, `GameCorePlugin.h`, the protocol
SQL and the loader build. To move to a newer loader, replace the folder and package again.

Licensed AGPL-3.0-only.
