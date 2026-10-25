# Installation (Dark Souls Remastered)

This fork's MarioSouls runs on **DARK SOULS: REMASTERED (Steam, Windows x64)**. Installing means copying files into the game folder: there is no installer, nothing is patched on disk and nothing needs administrator rights beyond writing to the game folder. (The original project's PTDE installer, `MarioSouls.ps1` and `Setup.exe`, is for Prepare to Die Edition and does not install this port.)

## What you need

- Dark Souls Remastered, current Steam version.
- Your own **Super Mario 64 (USA)** ROM, 8 MiB. `.z64` (big-endian), `.n64` and `.v64` byte orders are all accepted. The mod checks the internal name and region and writes a normalised copy to `%LOCALAPPDATA%\MarioSoulsProbe\sm64_us.z64`. It never downloads a ROM.

## Install

1. Get `MarioSouls-DSR.zip`, or build it from source (see [development](DEVELOPMENT.md)).
2. Find the game folder: in Steam, right-click **DARK SOULS: REMASTERED → Manage → Browse local files**. By default:

   ```text
   C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED
   ```

3. Copy the zip's contents there, so the folder looks like this:

   ```text
   DarkSoulsRemastered.exe
   dinput8.dll                 <- MarioSouls host
   MarioSouls\mario64.dll      <- SM64 adapter
   MarioSouls\sm64.dll         <- libsm64 runtime
   MarioSouls\settings-example.txt
   <your SM64 ROM>.z64
   ```

4. Start the game normally through Steam.

On the first launch, a background thread builds SM64 collision for every map from your own game's `map\` and `event\` files. It writes the result to `%LOCALAPPDATA%\MarioSoulsProbe\collision`. This happens once. If you enter an area before its map is done, that map is built on demand. No game data is shipped with the mod.

If another mod already uses `dinput8.dll` in the game folder, the two cannot both be loaded this way. Remove or rename the other one first.

## Settings

The defaults are built in. To change them, copy `MarioSouls\settings-example.txt` to `MarioSouls\settings.txt` and remove the `#` in front of a line. The file is re-read while the game runs. The available options are listed in the example file. They include the Wing Cap, the dodge pot-breaking and bump, the ladder pose, the attack reach, the death planes and estus flask hiding.

## Update

Close the game and copy the new `dinput8.dll` and `MarioSouls` folder over the old ones. Your `settings.txt` is kept. If an update changes the collision format, the cache is rebuilt automatically.

## Uninstall

Close the game and delete `dinput8.dll` and the `MarioSouls` folder from the game folder. Optionally delete `%LOCALAPPDATA%\MarioSoulsProbe` (collision cache, ROM copy and log). Saves are not touched by install or uninstall. Back them up separately if you care about them; see [recovery](RECOVERY.md).
