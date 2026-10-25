# Dark Souls Remastered port — status and handoff

This file is the up-to-date summary of the Remastered (DSR) host for the
original author, Ted Cassirer, and for anyone picking the port up. It
supersedes the "nothing here has been run" text in this folder's `README.md`.
That README is kept as the record of the early observation work.

**Status:** playable, experimental. Mario replaces the knight in DSR
(Steam, x64, DX11). He moves with SM64 physics on collision built from the
player's own game files, and he fights, breaks objects, climbs ladders, uses
estus, wears the Wing Cap and flies. Testing was offline on a disposable
character. The PTDE host is unchanged.

There is still no `edition.json`. DSR is not declared in `tools/host_editions.py`,
and the PTDE installer and `MarioSouls.ps1` do not know about it. DSR has its own
build script and package (below). Declaring the edition is left to the
maintainer.

## Branch layout

- Branch `ds1remastered` sits on upstream `main`, after "Document PTDE
  behavior checklist for Remastered".
- New or changed files:
  - `framework/hosts/ds1remastered/` — the host. About 15k lines of C++ headers,
    plus Python helpers and `release/` player docs.
  - `tools/build_remastered.bat`, `tools/package_remastered.bat` — build and
    package on Windows.
  - `tools/compile_test_shaders.py` — the HLSL in `test_shaders.hlsl` →
    `test_shaders_gen.h`. The generated file is not committed.
  - `tools/flver_tools.py` — FLVER2 bone and mesh inspection, used for the
    knight bone table.
  - `docs/REMASTERED_FINDINGS.md` — numbered findings from live runs (offsets,
    buffers, passes).
  - `docs/remastered/` — working notes from the port: bone table, camera sync
    analysis, render and SM64 plans, modding resources, issues list. These are
    historical; this file is the current state.
  - `CHANGELOG.md` — entries 0.9.0-dev.3 to dev.12 cover the DSR work.
- `.gitignore` additions: `docs/remastered/*.md`, `framework/**/*.hlsl`,
  `ds1remastered/release/*.txt`, `tools/*_remastered.bat`. Generated `*_gen.h`,
  `work/`, game data and ROMs stay excluded.

## Building and packaging

Requirements: Windows, Python 3, and the repo's pinned toolchain (the same
dependencies as PTDE: libsm64 with `patches/libsm64`, and Zig/MSVC as in
`docs/DEVELOPMENT.md`).

1. Run `tools\build_remastered.bat`. It compiles the test shaders, then runs
   `framework\hosts\ds1remastered\build.py`. The output is
   `work\framework-build\dinput8_x64.dll`, and the log goes to
   `work\framework-build\last-build.txt`. Warnings are errors.
2. Run `tools\package_remastered.bat`. It writes `work\release\MarioSouls-DSR\`
   and `MarioSouls-DSR.zip`.

The package contains:

```text
dinput8.dll                     host (imports only UCRT/KERNEL32/USER32/WINMM)
MarioSouls\mario64.dll          SM64 adapter
MarioSouls\sm64.dll             libsm64 runtime
MarioSouls\settings-example.txt
README - MarioSouls.txt
```

The player copies the package into the DSR game folder and adds their own SM64
(US) ROM there. Any of `.z64`, `.n64` or `.v64` works; it is converted to
`%LOCALAPPDATA%\MarioSoulsProbe\sm64_us.z64` and validated (internal name
"SUPER MARIO 64", region E). No ROM and no game data ship.

On first launch, `install.h` starts a background thread. It builds collision
for every `m1*.msb` map from the player's own `map\` and `event\` files into
`%LOCALAPPDATA%\MarioSoulsProbe\collision`. `collision_build.h` is a self-contained C++
port of `prepare_collision.py` and `prepare_breakables.py`. It parses DCX (zlib),
BND3, BHF3/BDF3, Havok 2015 tagfiles, MSB1 and DS1R EMEVD. Its output was checked
byte for byte against the Python tools for m10_00 and m10_01. A map that is needed
before the thread reaches it is built on demand (`install::ensureMapCache`).

Settings use the built-in defaults (`kDefaultModeText` in `geometry_pass.cpp`).
Two files can override them: `<game>\MarioSouls\settings.txt`, or the
developer file `%LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt`. Both are re-read
live.

## How it works (short)

| Area | Approach | Main files |
| --- | --- | --- |
| Loading | `dinput8.dll` proxy; IAT hooks for `D3D11CreateDevice` / `CreateDXGIFactory` | `runtime.cpp`, `loader.h`, `startup.h` |
| Camera | View-projection read at `Unmap` of the game's constant buffers (deferred contexts) | `camera_capture.*` |
| Player / world state | `WorldChrMan` chain (AOB), PlayerIns HP +0x3E8, stamina +0x3F8; CamMan `[exe+1C6E188]+0x60` | `world_observation.h`, `native_types.h` |
| Drawing Mario | Our draw is injected into the geometry pass on the game's deferred context and depth-tested against scene depth (D24S8, single-sample) | `geometry_pass.*`, `render_device.h`, `test_shaders.hlsl` |
| Hiding the knight | Its draws are skipped (body, armour, face pieces learned from the bone palette slot 0), including shadow and depth passes; no memory writes | `camera_capture.cpp` |
| Mario sim | libsm64 via `mario64.dll`; terrain from cached collision, with streaming per map | `mario_presentation.h`, `terrain.h` |
| Driving the knight | The knight follows Mario's position and facing (`drive=native`); native animations are chosen from Mario's action | `native_motion.h`, `native_actions.h` |
| Combat | Per-hit windows → native attack rows applied to targets; breakables through the native object damage function | `mario_combat.h`, `mario_setup.h` |
| Text / params | Runtime FMG and param patching (names, starting helm, attack rows) | `mario_setup.h` |

## Status against `framework/docs/REMASTERED_PARITY.md`

Key: **done**, means working in live testing; **partial**; **not started**; **deferred**,
meaning parked at the user's request.

| # | PTDE behavior | DSR state |
| --- | --- | --- |
| 01 | Ordinary-floor traction | done |
| 02 | Body size, doorways, stair stepping | done (step assist is guarded so BLJs still work) |
| 03 | Native motion acknowledgement | done |
| 04 | Missing floors, event geometry, barriers | done: EMEVD-controlled collisions; death planes for filters 14/15 while falling |
| 05 | Lifts / moving support | partial: elevators and pit death planes work; Asylum→Firelink crow arrival deferred |
| 06 | Reduced landing damage, no pit immunity | done |
| 07 | Fixed sim cadence, short taps | done (30 Hz sim) |
| 08 | Mesh / held-prop interpolation | partial: Mario is interpolated; held-item hiding is reduced to the estus effect patch |
| 09 | Camera, depth, UI order | done at 1080p; other resolutions not tested in depth |
| 10 | Mesh-only hiding, audio | done for meshes; a small player shadow artifact remains in sunlight |
| 11 | Cutscene presentation | not started |
| 12 | Mario class / creation / intent | partial: Mario always replaces the player; starting Wing Cap via CharaInitParam 3007 |
| 13 | Seamless areas vs actor replacement | done (actor-replacement resets) |
| 14 | Contextual A, menus, estus, footing | done: menus, shops, pickups, talking, bonfires, ladders, estus |
| 15 | Native actions, doors, death | done (deaths and respawns; recovery after many quick hits) |
| 16 | Gloves and Mario power | partial |
| 17 | Hit windows, stamina, large targets | done: per-hit windows; stamina gating with hold at 0 |
| 18 | Weapons, shields, parries, criticals | not started |
| 19 | Kicks and hit feedback | done; dodge uses the 4200410 stamina knockback |
| 20 | Poise / recovery | partial |
| 21 | One health pool, statuses | done (native HP pool) |
| 22 | Fire Flower | partial: renamed "Flower Power"; Mario visuals and bouncing fireballs are not ported yet |
| 23 | Lava, enemy fire | deferred |
| 24 | Cannon, diagnostics, small behaviors | partial: diagnostics behind `diag=on` |

DSR-only additions:
- Wing Cap: the Hollow Soldier Helm (480000–480010) gives Mario the SM64 wing cap,
  with flight and the cap-on animation.
- Renames: Sen's Fortress → "Whomp's Fortress".
- Breakables can be hit by punches, kicks, pounds and the dodge.

## Key DSR facts found during the port

- Estus flask: it is not a mesh. It is FFX effect 227, spawned from the TAE in
  `c0000.anibnd` (anims 7586/7587) by:
  - type-96 events (22750, dummy 26);
  - type-114 events 227 (flask) and 229 (spill). 228 is the heal glow and is
    kept.

  The host scans memory for those events and patches them to unused IDs only while
  Mario is shown. Changing the goods row `sfxVariationId` (+0x04) does not hide the
  flask.
- Breakables: ObjIns vtable RVA 0x13186F8. The matrix translation is at
  +0x8C/+0x9C/+0xAC. Native object damage is at RVA 0x3C9B50. The model pointer at
  +0x60 is not an alive flag.
- Dodge knockback: behavior 104200410 → AtkParam 4200410. Knockback at +0x10 is
  raised to 30 cm for the hit and then restored.
- SM64 counts only |ny| ≤ 0.01 as a wall. DSR has many slightly tilted walls, so
  `terrain.h` adds vertical stand-ins for 0.01<|ny|<0.30.
- Head armour ID: `[[BaseB]+0x10]+0x344`. CharaInitParam 3007 `equip_Helm` is at
  +0x20.
- Menus: detected by snapshot diffs of MenuMan fields with a 250 ms revert rule.
  Bonfire sit is also released when the rest animation ends or the knight moves
  more than 0.6 m.
- `VirtualQuery` costs about 6.7 ms per call inside DSR, so it is never called on
  render threads.

More detail is in `docs/REMASTERED_FINDINGS.md` and the CHANGELOG entries.

## Known issues and next steps

- Waiting for live confirmation:
  - the wing texture cut-out (the white square around the wings);
  - the bonfire sit fix when toggling control during a sit;
  - the starting Wing Cap on a new character;
  - a clean install from the zip.
- Deferred: Asylum→Firelink crow arrival, Solaire and Pinwheel parts culled near
  Mario, and lava.
- Not started:
  - weapons and shields on Mario;
  - backstabs, ripostes and criticals;
  - Fire Flower visuals;
  - an Ice Mario / iceball idea;
  - animations for ladder kick-down, levers and the egg lie-down;
  - other SM64 characters;
  - cutscenes.
- Distribution: the package ships the libsm64 binaries
  (`sm64.dll`, built from the pinned source with `patches/libsm64`). The ROM
  stays with the player. Check `THIRD_PARTY.md` and the licenses before any
  public release.

## Credits

MarioSouls and the PTDE host are by **Ted Cassirer**
(<https://github.com/TedCassirer/mariosouls>). This Remastered port builds on his
framework, SDK, SM64 adapter and PTDE reference behavior, and follows his
porting guides (`framework/docs/HOST_PORTING.md`, `REMASTERED_HANDOFF.md`,
`REMASTERED_PARITY.md`). The port was developed by Josh Fraser with Claude
(Anthropic). It also uses DSR-Gadget offsets and community modding references
listed in `docs/remastered/MODDING_RESOURCES.md`.
