# Verification (Dark Souls Remastered)

## What the build checks

`tools\build_remastered.bat` compiles and links the Remastered host with warnings as errors. `python tools/framework.py build --check-portability` compiles the shared SDK/core and the x64 adapter and runtime. Neither runs the game. There are no automated gameplay tests; everything below is manual.

## Manual checks

Use a disposable character, play offline and back up saves first. Scope each check to what changed. This is a checklist, not a statement that every row passes.

| Area | Check |
| --- | --- |
| Clean install | Zip contents and a ROM in a fresh game folder; first-launch `install: built collision` lines in the log; no ROM message box |
| New character | Class shows as Mario; starts with Mario's Gear and the Wing Cap |
| Toggle | L3 + R3 (1 s), F6 and F8 switch to the knight and back cleanly, including during a bonfire sit |
| Movement | Run, jumps, wall kicks, BLJs, slopes, stairs, narrow doors, sloped walls (Hellkite bridge, Capra arena) |
| Falls | Soft ordinary landings; pits and elevator shafts kill; Firelink arrival doesn't kill |
| Ladders | Mario on the rungs; B climbs faster; standing still holds |
| Interactions | Doors, levers, bonfires, NPCs, shops, pickups; control returns after each menu |
| Estus | No knight flask or spill; the heal glow shows; empty flask too |
| Combat | Punch, kick, pound, dive; stamina empties and stays at 0 until regen; no spending mid-move |
| Dodge | Bump on small enemies with no HP damage; breaks pots |
| Breakables | Pots, crates and barrels break from each attack |
| Wing Cap | Wings drawn without a square around them; cap-on animation; triple-jump flight |
| Text | Whomp's Fortress, Wing Cap, Flower Power, Mario's Gear names |
| Death | Death and respawn, including after many quick hits; full control afterwards |
| Rendering | Mario depth-correct; knight hidden including shadows; resolution change |

## Recording evidence

Record:
- the commit or build;
- the settings file;
- the area and steps;
- what you saw;
- the matching log lines;
- the cases left untested.

A screenshot of an animation doesn't prove damage happened; check the HP or stamina bars. Keep captures, saves and game data out of Git.
