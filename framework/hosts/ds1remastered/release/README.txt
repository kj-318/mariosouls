MarioSouls for DARK SOULS: REMASTERED
=====================================

Play Dark Souls Remastered as Mario, with Super Mario 64's own movement, moves and sounds.

WHAT YOU NEED
  - DARK SOULS: REMASTERED (Steam, current version).
  - Your own Super Mario 64 (USA) ROM. It is not included and the mod does not download one.
    A .z64 file is best; .n64 and .v64 dumps are converted automatically.

INSTALL
  1. Open the game folder: in Steam, right-click DARK SOULS: REMASTERED > Manage > Browse local files.
     (Usually C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED)
  2. Drag everything from this zip into that folder:
       dinput8.dll           (next to DarkSoulsRemastered.exe)
       MarioSouls\           (the folder, with mario64.dll and sm64.dll inside)
  3. Put your Super Mario 64 (USA) ROM in the same folder, next to DarkSoulsRemastered.exe.
     Any file name works.
  4. Start the game. The first time you load a save, the mod reads your own game's map files
     and builds Mario's collision for every area (a few seconds, once).

  New characters: choose the Pyromancer class (shown as "Mario"). Mario starts with Mario's Gear
  (punches and kicks) and the Wing Cap.

CONTROLS (controller)
  Left stick     move                 A        jump (A at a prompt: interact)
  B              dodge (dive / roll)   RB       punch
  RT             kick                  LT       crouch (long jump, backflip, ground pound)
  LB / RB        cast with the Flower Power (Pyromancy Flame) in that hand
  L3 + R3 (hold 1 s) or F8 / F6        switch between Mario and the knight's own controls
  Keyboard: WASD move, E jump, Space dodge, left mouse punch, right mouse kick, Ctrl crouch.

  Wear the Wing Cap (Hollow Soldier Helm) and triple jump to fly.

SETTINGS (optional)
  Copy MarioSouls\settings-example.txt to MarioSouls\settings.txt and edit it while the game
  runs; changes apply within a second. Every line is optional.

UNINSTALL
  Delete dinput8.dll and the MarioSouls folder from the game folder. Collision caches and the
  log live in %LOCALAPPDATA%\MarioSoulsProbe and can be deleted too.

TROUBLESHOOTING
  - Mario does not appear: check that the ROM is the USA release and sits next to
    DarkSoulsRemastered.exe, and that the MarioSouls folder is in the game folder.
  - The log is %LOCALAPPDATA%\MarioSoulsProbe\remastered_host.log (paste it when reporting).
  - Play offline: the mod changes how the game draws and controls your character.

Super Mario 64 is (c) Nintendo. Dark Souls is (c) FromSoftware / Bandai Namco. This is an unofficial
fan mod; it contains no game or ROM data.
