# Troubleshooting (Dark Souls Remastered)

The log is `%LOCALAPPDATA%\MarioSoulsProbe\remastered_host.log`. It is replaced on each launch, so copy it before starting the game again. For extra detail, add `diag=on` to `MarioSouls\settings.txt`; this costs some performance.

## Mario doesn't appear

- A message box about the ROM means none was found. Put the SM64 **USA** ROM next to `DarkSoulsRemastered.exe`. Any file name works, as `.z64`, `.n64` or `.v64`.
- Check that `dinput8.dll` is next to `DarkSoulsRemastered.exe` and that the `MarioSouls` folder, with `mario64.dll` and `sm64.dll`, is in the same folder.
- If there's no log file at all, the DLL wasn't loaded. Another `dinput8.dll`, a different game folder, or an antivirus quarantine are the usual causes.
- You might be in knight mode: hold L3 + R3, or press F6.

## Mario falls through the floor or walks on air

- On the very first launch, collision is still being built. Look for `install: built collision` lines in the log, and give it a few seconds after loading a save.
- If a map's cache looks wrong after a game update, delete `%LOCALAPPDATA%\MarioSoulsProbe\collision`. It is rebuilt on the next launch.
- Report the area and position (see [bug reports](BUG_REPORTING.md)). Some event-controlled floors and moving platforms may not be covered yet.

## Stuck as the knight, or stuck sitting

Control returns when a menu closes, or when the knight stands up or walks away from a bonfire. If it doesn't, toggle with L3 + R3 or F6 twice. Note in a report what you were doing: which menu, shop, NPC or bonfire.

## Attacks don't hit or don't cost stamina

Damage comes from Mario's Gear (the Caestus) and its upgrade level, and every move needs stamina. At 0 stamina nothing can be used until regen starts. This is intended.

## Visual problems

- A small flickering player shadow in sunlight: lower Shadow Quality.
- A one-frame purple stretch on first load is known and harmless.
- Missing NPC parts close to Mario (Solaire, Pinwheel) are known.

## Removing the mod

Delete `dinput8.dll` and the `MarioSouls` folder. See [installation](INSTALLING.md).
