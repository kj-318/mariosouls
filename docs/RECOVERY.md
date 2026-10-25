# Recovery (Dark Souls Remastered)

The mod changes nothing on disk except what you copy in, plus its own cache folder. Game files and saves are not modified on disk. All of its param, text and animation changes are made in memory and disappear when the game closes.

## Remove the mod

Close the game, then delete `dinput8.dll` and the `MarioSouls` folder from the game folder. The game is back to vanilla. Optionally delete `%LOCALAPPDATA%\MarioSoulsProbe` (collision cache, ROM copy, log). If anything still seems wrong, use Steam's **Properties → Installed Files → Verify integrity of game files**.

## Saves

Characters made or played with the mod are ordinary saves. They keep items the mod's rows refer to, such as Mario's Gear (a Caestus) and the Wing Cap (a Tattered Cloth Hood). Without the mod these show their vanilla names. Back up saves before testing:

```text
%USERPROFILE%\Documents\NBGI\DARK SOULS REMASTERED\<id>\DRAKS0005.sl2
```

Copy that file somewhere safe. To restore it, close the game and copy it back.

## Earlier source

Commits are the source checkpoints. To build an earlier revision without touching current work:

```powershell
git worktree add --detach ..\mariosouls-recovered <commit>
```

Use `git revert <commit>` to undo a committed change while keeping history. Keep `work/` (dependencies, builds, private notes); Git cannot recover it.
