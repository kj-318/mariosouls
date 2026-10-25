# Compatibility (Dark Souls Remastered)

## Supported game

This fork targets **DARK SOULS: REMASTERED, Steam, Windows x64, DirectX 11**. The host is found by function signatures (AOB scans) and the verified pointer chains recorded in [REMASTERED_FINDINGS](REMASTERED_FINDINGS.md). Prepare to Die Edition is not supported by this fork's build. The original project supports it: see [TedCassirer/mariosouls](https://github.com/TedCassirer/mariosouls).

Other editions are not supported by the Remastered host: console versions and modified executables.

## What has been checked

All testing so far has been offline, on disposable characters, at 1920×1080. A full playthrough has not been done.

| Area | State |
| --- | --- |
| Rendering | Mario drawn in the geometry pass, depth-tested, with the knight's meshes (including shadows) hidden. Checked at 1080p, with other resolutions only briefly. Not multisampled; the game's renderer has no MSAA target. |
| Movement and collision | Collision built from your own map files for every `m1*` map. Areas played include the Undead Asylum, Firelink Shrine, Undead Burg, Sen's Fortress and the Catacombs. |
| Interactions | Ladders, doors, bonfires, NPC dialogue, shops, pickups and estus. |
| Combat | Mario's Gear punches and kicks, stamina gating, the dodge bump and breakables. |
| Not yet handled | Cutscenes, lava, Asylum → Firelink arrival, Mario wielding weapons and shields, criticals. |

See [PORT_STATUS](../framework/hosts/ds1remastered/PORT_STATUS.md) for the item-by-item state.

## Other mods and tools

- Another `dinput8.dll` mod in the game folder conflicts, because only one can load.
- ReShade (as `dxgi.dll` or `d3d11.dll`) and overlays are untested. Report what you find.
- DSR-Gadget and Cheat Engine reading the same memory are fine for testing, but writes can confuse Mario's state.
- Graphics options: lower **Shadow Quality** if you see a small flickering player shadow. Other options have no known effect.
- **Online play is not supported.** The mod changes param and text data in memory and drives your character in unusual ways. Play offline.

The ROM must be the USA release; other regions are refused.
