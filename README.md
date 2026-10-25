# MarioSouls — Dark Souls Remastered

MarioSouls is a mod for **DARK SOULS: REMASTERED on Windows (Steam, x64)** that replaces your character with **Mario**. Play the game with Super Mario 64 movement, jumps and attacks, while keeping Dark Souls progression, equipment and world interactions.

> **About this fork.** This is the Dark Souls Remastered port of [MarioSouls by Ted Cassirer](https://github.com/TedCassirer/mariosouls). The original mod targets Dark Souls: Prepare to Die Edition (PTDE). This fork targets **Remastered only**: its install steps, controls and docs are for Remastered. Ted's PTDE host code is still in the tree (`framework/hosts/ds1ptde/`) as the reference implementation the port follows. It is not what this fork builds or ships. The current port state is in [PORT_STATUS](framework/hosts/ds1remastered/PORT_STATUS.md).

The mod is experimental and has been tested offline on disposable characters. Play offline.

## Installation

You need your own copy of Dark Souls Remastered and **your own Super Mario 64 (USA) ROM**. `.z64` is best; `.n64` and `.v64` dumps are converted automatically. ROMs and game data are not included, and the mod never downloads them.

1. Get `MarioSouls-DSR.zip`, or build it yourself (see [development](docs/DEVELOPMENT.md)).
2. Open the game folder: in Steam, right-click **DARK SOULS: REMASTERED → Manage → Browse local files**. This is usually `C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED`.
3. Copy `dinput8.dll` and the `MarioSouls` folder from the zip into that folder, next to `DarkSoulsRemastered.exe`.
4. Put your SM64 ROM in the same folder. Any file name works.
5. Start the game. The first time you load a save, the mod reads your own game's map files and builds Mario's collision for every area. This runs once and takes a few seconds.

To uninstall, delete `dinput8.dll` and the `MarioSouls` folder. Caches and the log are in `%LOCALAPPDATA%\MarioSoulsProbe` and can be deleted too. More detail: [installation](docs/INSTALLING.md).

## Playing

Mario appears in place of your character on any save. For a new game, choose **Mario** (it replaces the Pyromancer class). Mario starts with **Mario's Gear** for punches and kicks, and with the **Wing Cap**.

| Action | Controller | Keyboard / mouse |
| --- | --- | --- |
| Move / camera | Left / right stick | WASD / mouse |
| Jump; interact at a prompt | A | E |
| Dodge (dive / roll) | B | Space |
| Punch | RB | Left mouse |
| Kick | RT | Right mouse |
| Crouch (long jump, backflip, ground pound) | LT | Ctrl |
| Cast with Flower Power in that hand | LB / RB | — |
| Switch between Mario and the knight's own controls | Hold L3 + R3 (1 s) | F6, or hold F8 |

Wear the **Wing Cap** (the renamed Hollow Soldier Helm) and triple jump to fly. Punches, kicks and the dodge cost stamina. Pots, crates and barrels break when Mario hits or dodges into them. More in [the player guide](docs/PLAYING.md).

## How it works

MarioSouls uses [libsm64](https://github.com/libsm64/libsm64) to run Mario's movement and animation. The Remastered host is a `dinput8.dll` proxy:
- It loads the Mario adapter and libsm64.
- It builds SM64 collision from the game's own map files.
- It draws Mario in the game's DX11 geometry pass and hides the knight's model.
- It drives the native character to follow Mario.

Dark Souls stays in charge of health, stamina, inventory, enemy damage, quests and rewards. Mario simulates at 30 Hz.

## More information

- [Documentation index](docs/README.md)
- [Port status and handoff](framework/hosts/ds1remastered/PORT_STATUS.md), and [version history](CHANGELOG.md)
- [Compatibility](docs/COMPATIBILITY.md), [troubleshooting](docs/TROUBLESHOOTING.md) and [bug reports](docs/BUG_REPORTING.md)
- [Framework architecture](framework/README.md), [development](docs/DEVELOPMENT.md) and [contributing](CONTRIBUTING.md)

## Credits

The original MarioSouls mod, framework, SM64 adapter and PTDE reference implementation are by [Ted Cassirer](https://github.com/TedCassirer/mariosouls). The Remastered port is by [KJ-318](https://github.com/kj-318), developed with Claude (Anthropic).

Built with libsm64, the [SM64 decompilation](https://github.com/n64decomp/sm64), [MinHook](https://github.com/TsudaKageyu/minhook) and [Soulstruct](https://github.com/Grimrukh/soulstruct). Super Mario 64 is © Nintendo; Dark Souls is © FromSoftware / Bandai Namco. This is an unofficial fan mod.

Original project code is [GPL-3.0-only](LICENSE). See [third-party notices](THIRD_PARTY.md).
