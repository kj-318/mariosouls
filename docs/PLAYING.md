# Playing MarioSouls (Dark Souls Remastered)

## Mario and the knight

Mario replaces your character on any save. The knight is still there underneath: he follows Mario's position and facing, and Dark Souls keeps handling HP, stamina, equipment, damage, menus and progression. Only the knight's model is hidden.

For a new game, choose **Mario**, which replaces the Pyromancer class. Mario starts with **Mario's Gear** (the reworked Caestus) for punches and kicks, and wears the **Wing Cap**. Older saves keep their equipment. To get a Wing Cap on an older save, set `givewingcap=on` once in `settings.txt` (see [installation](INSTALLING.md)).

To hand control back to the knight's own moveset, hold **L3 + R3** for one second, press **F6**, or hold **F8**. Do the same again to return to Mario.

## Controls

| Action | Controller | Keyboard / mouse |
| --- | --- | --- |
| Move | Left stick | WASD |
| Camera | Right stick | Mouse |
| Jump; interact at a prompt | A | E |
| Dodge (dive, roll) | B | Space |
| Punch | RB | Left mouse |
| Kick | RT | Right mouse |
| Crouch, long jump, backflip, ground pound | LT | Ctrl |
| Cast with Flower Power in that hand | LB / RB | — |
| Mario / knight toggle | Hold L3 + R3 | F6, or hold F8 |

All of SM64's moves work: triple jump, long jump, backflip, side flip, wall kick, dive and ground pound. Backwards long jumps (BLJs) work too.

## Combat

- **Punches and kicks** do native damage scaled from Mario's Gear and its upgrade level. Pound and dive attacks also hit.
- **Stamina:** every punch, kick and dodge needs stamina. When the bar empties, it stays at 0 until the game's normal regen starts. A new move can't start while the last one is still playing.
- **Dodge (B)** gives small enemies a light stamina knockback, like the knight's roll, with no HP damage.
- **Breakables:** pots, crates and barrels break from punches, kicks, pounds and the dodge.
- Weapons, shields, backstabs, ripostes and parries are not ported to Mario yet. Use the knight toggle for those.

## Equipment and items

- **Wing Cap** (renamed Tattered Cloth Hood, the Pyromancer's starting hood): Mario wears the winged cap and plays the cap-on animation. Triple jump to fly with SM64's wing-cap physics. With other helmets, Mario wears his normal cap.
- **Flower Power** (renamed Pyromancy Flame): casts natively from the hand it is in.
- **Estus:** Mario drinks with the native heal; the knight's flask is hidden, and the heal glow is kept.

## World

- **Ladders:** Mario holds onto the rungs. Hold B to climb faster. Standing still keeps him on the ladder.
- **Doors, levers, bonfires, NPCs, shops and item pickups** use the game's native actions. Control returns to Mario when the menu closes or the knight stands up from the bonfire.
- **Falls:** ordinary landings are softened like SM64. Pits, elevator shafts and kill planes still kill.
- **Walls:** slightly tilted walls (the Hellkite bridge underside, Capra's arena) count as walls for bonks and wall kicks.
- **Names:** Sen's Fortress appears as **"Whomp's Fortress"**.

## Known gaps

Cutscenes, lava and the Asylum → Firelink crow arrival are not handled for Mario yet. Some NPC parts can disappear when Mario stands very close (seen with Solaire and Pinwheel). A small player-shaped shadow can flicker in sunlight; lowering Shadow Quality hides it. The full list is in [PORT_STATUS](../framework/hosts/ds1remastered/PORT_STATUS.md).

Play offline, and test on a character you don't mind losing.
