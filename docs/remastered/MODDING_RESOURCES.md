# Dark Souls Remastered: modding resources and test launch

Collected 2026-10-19 for the MarioSouls Remastered port. Third-party offsets
below come from their sources and **have not been verified in this project**
unless the notes say so. Before using one, confirm it live the same way HP and
stamina were confirmed (`docs/REMASTERED_FINDINGS.md` §18).

---

## 1. Launching the game for a test run

Each test session follows these steps. Do them yourself. Builds never install
anything, and nothing here is automated.

**Before the first run of the day**

1. Back up your saves:
   `Documents\NBGI\DARK SOULS REMASTERED\<steam-id>\DRAKS0005.sl2`. The last
   backup is at `Desktop\DS1 Backup Saves\Remastered-20261018-230524`.
2. Use only the disposable test character, never the level-65 one.
3. Play offline. DSR's PC servers are live, and a modified client can be
   soft-banned. Use one or more of these:
   - Steam → *Steam* menu → *Go Offline…*.
   - In game: *System* → *Network Settings* → *Launch setting: Start offline*.
   - Optionally, add a firewall rule that blocks `DarkSoulsRemastered.exe`.

**Build and install the host**

From `mariosouls\` in a terminal:

```
python framework\hosts\ds1remastered\build.py
```

The build writes `work\framework-build\dinput8_x64.dll`. Check
`work\framework-build\*.build.log` for errors. The `-static-libstdc++` warning
is harmless.

Copy that file into the game folder **renamed to `dinput8.dll`**, next to
`DarkSoulsRemastered.exe`. The default location is
`C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED\`. If a
`dinput8.dll` is already there from another mod, back it up first. The two
mods can't load together without a chain loader.

**Run**

1. Launch DSR from Steam as usual.
2. **Wait at the title screen for at least 15 seconds.** The host installs its
   vtable hooks only after that delay because of the anti-cheat scan. Input
   before then is fine, but nothing is hooked yet.
3. Load the test character.
4. Watch the log while playing:
   `%LOCALAPPDATA%\MarioSoulsProbe\remastered_host.log`. To open it, paste
   that path into Explorer's address bar. To watch it live in PowerShell, run
   `Get-Content $env:LOCALAPPDATA\MarioSoulsProbe\remastered_host.log -Wait`.

**What to look for in the current test-draw build**

| Log line | Meaning |
| --- | --- |
| `delayed: geometry-pass injection armed` | Resources were created. The earlier build would have crashed or failed here. |
| `delayed: geometry-pass injection NOT armed` | Look for the `inject: <Create…> failed hr=…` line just above it. |
| `inject: drew test triangle #1 … (state saved and restored)` | The draw was issued into a geometry-pass binding. |
| `rt: NEW binding …` | Pass identification, the same as before. |

On screen, a large magenta triangle should appear somewhere in the lit scene,
probably tinted by lighting. Its exact look depends on which G-buffer channel
render target 0 holds. A visible triangle confirms that the pass is the
geometry pass. If the frame looks corrupted with no triangle, or a triangle
appears on the HUD, note what you saw and send the log.

**Afterwards**

Delete `dinput8.dll` from the game folder before any online play.

---

## 2. Memory offsets (third-party, version 1.03.x)

### DSR-Gadget (JKAnderson): AOB-anchored, the most useful for this port
Source: `DSR-Gadget/DSROffsets.cs` and `DSRHook.cs`. Every AOB resolves
RIP-relative: `target = addr + *(int32*)(addr+3) + 7`.

| Item | AOB / chain |
| --- | --- |
| WorldChrBase | `48 8B 05 ? ? ? ? 48 8B 48 68 48 85 C9 0F 84 ? ? ? ? 48 39 5E 10 0F 84 ? ? ? ? 48` |
| PlayerIns (ChrData1) | `[[WorldChrBase] + 0x68]` |
| HP / MaxHP | PlayerIns `+0x3E8` / `+0x3EC` (base 0x3D8 + 0x10 version boost) |
| Stamina / MaxStamina | PlayerIns `+0x3F8` / `+0x3FC` |
| ChrFlags1 / ChrFlags2 | `+0x2A4` (0x284 + 0x20) / `+0x524` (0x514 + 0x10) |
| ChrMapData | `[PlayerIns + 0x68]` (0x48 + 0x20) |
| ChrAnimData → AnimSpeed | `[ChrMapData + 0x18]` `+0xA8` |
| **ChrPosData → position** | `[ChrMapData + 0x28]`: angle `+0x4`, X/Y/Z `+0x10/+0x14/+0x18` (float) |
| Warp request | ChrMapData `+0x108` (flag), `+0x110/114/118` XYZ, `+0x124` angle |
| ChrClassWarp (last bonfire, stable pos) | AOB `48 8B 05 ? ? ? ? 66 0F 7F 80 ? ? ? ? 0F 28 02 66 0F 7F 80 ? ? ? ? C6 80`; `+0xB34`, `+0xBA0..` (+0x10 boost) |
| ChrClassBase (stats) | AOB `48 8B 05 ? ? ? ? 48 85 C0 ? ? F3 0F 58 80 AC 00 00 00` → `[+0x10]`: VIT 0x40, END 0x50, STR 0x58, DEX 0x60, SL 0x90, souls 0x94 |
| CamMan | AOB `48 8B 05 ? ? ? ? 48 63 D1 48 8B 44 D0 08 C3`, offset `0x10` |
| ChrFollowCam | AOB `48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B 4E 68 48 8B 05 ? ? ? ? 48 89 48 60` → `+0x60 → +0x60`. Gadget treats it as "character loaded" when non-null. |
| ChrDbg flags | AOB `80 3D ? ? ? ? 00 48 8B 8F ? ? ? ? 0F B6 DB` (byte flags: NoDead 0x0, NoStaminaConsume 0x2, AllNoUpdateAI 0xD …) |
| GroupMask (draw toggles) | AOB `80 3D ? ? ? ? 00 BE 00 00 00 80`: map/objects/chars/SFX/cutscenes at +0..+4 |
| EventFlags | AOB `48 8B 0D ? ? ? ? 99 33 C2 45 33 C0 2B C2 8D 50 F6` |
| MenuMan | AOB `48 8B 05 ? ? ? ? 89 88 28 08 00 00 85 C9 ? ? C7 80 34 08 00 00 FF FF FF FF C3` |
| GraphicsData (colour filter) | AOB `48 8B 05 ? ? ? ? 48 8B 48 08 48 8B 01 48 8B 40 58` → `+0x738` |

**How this cross-checks against our findings**

- Our live-verified **HP at `+0x3E8`** matches Gadget's boosted Health offset.
  That points to our health chain ending on the same PlayerIns object.
- Gadget puts **stamina at `+0x3F8`**. We wrongly used `+0x3F0`, which turned
  out to be a constant 49, and switched to a separate chain. `+0x3F8` is
  worth a live check, because it would remove the second chain.
- **The player position chain fills the "Player position/rotation: not
  implemented" gap.** It is AOB-based, so it is also less version-fragile than
  our fixed `exe+…` roots.

Gadget's version map recognises module sizes 1.01 to 1.03 and applies the
1.03 boosts to any unknown size, which includes the current 1.03.1.

### Phokz cheat tables (via the dark-souls-remastered-stream-overlay POINTER_MAP)
- BaseB: AOB `48 8B 05 ?? ?? ?? ?? 45 33 ED 48 8B F1 48 85 C0`.
  ChrStat = `[BaseB + 0x10]`: HP 0x14, MaxHP 0x1C, MP 0x20, Stamina 0x30,
  MaxStamina 0x34, stats 0x40 to 0x88, SL 0x90, souls 0x94.
  BaseB itself holds deaths 0x98, play time 0xA4 and NG+ count 0x78.
- Other bases: BaseA `48 89 05 ?? ?? ?? ?? 8D 42`; BaseC (= Gadget
  ChrClassWarp); GetX `48 8B 05 ?? ?? ?? ?? 48 39 48 68 0F 94 C0 C3`, and
  others.
- These are the "character params" view of HP and stamina, which is different
  from the PlayerIns copy.

### universal-modder "DSR portal gun" note (PR #147)
- Steam build 10943698, exe size 50,286,344 bytes.
- BulletMan lists `+0x28` (stride 0x308) and `+0x08` (stride 0x348). Their
  author left these unresolved.
- Uses a dinput8 proxy, offline only.

---

## 3. DX11 and DLL mods to learn from

- **DS1 FPS Unlock (Remastered, DX11)**, zofrea1. This is a `dinput8.dll`
  proxy with byte-checked patches: on an unsupported build it refuses to patch
  rather than corrupting the game. It targets build SHA-1
  `9150CC63C617332ED3C2C66E7566ED67E3292DA0` (1.03.1, 2022-10-11). Hash your
  exe against that to confirm which build you have. The README notes that
  "Remastered's code is protected" and warns about soft bans online.
  [GitHub](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered),
  [Nexus 1484](https://www.nexusmods.com/darksoulsremastered/mods/1484)
- **dark-souls-remastered-stream-overlay**, JoShMiQueL. A native
  `dinput8.dll` proxy that finds the player structure by pattern scan and
  re-scans after read failures. It has no graphics hook.
  [GitHub](https://github.com/JoShMiQueL/dark-souls-remastered-stream-overlay)
- **ModEngine2 (DSR)** loads loose or modified files without repacking.
  [GitHub](https://github.com/soulsmods/ModEngine2/releases),
  [Nexus 790](https://www.nexusmods.com/darksoulsremastered/mods/790)
- **DS1R Debug Menu** restores the debug menu. It needs the exe downgraded
  through SteamDB. [Nexus 266](https://www.nexusmods.com/darksoulsremastered/mods/266)
- **DS1R Param Reloader** hot-reloads params.
  [GitHub](https://github.com/Grimrukh/DS1RParamReloader)

None of these does what we're doing (an injected draw into the deferred
G-buffer), so there's no prior art to copy for that part. All of them use the
same dinput8 proxy approach and stay offline-only.

---

## 4. Custom character models (FLVER route)

This is the alternative to our runtime draw: replace or add a model in the
game files.

- **Unpacking:** [Nuxe](https://github.com/JKAnderson/Nuxe) (current) or
  [UXM Selective Unpack](https://github.com/Nordgaren/UXM-Selective-Unpack).
  Both extract the archives and patch the exe to load loose files. Repack and
  edit with [WitchyBND](https://github.com/ividyon/WitchyBND/releases).
- **Model import/export:** [Soulstruct for Blender](https://github.com/Grimrukh/soulstruct-blender)
  is explicitly DS1R. It covers FLVER models, HKX collision, navmesh and HKX
  animation. Also see [FBX2FLVER](https://github.com/Meowmaritus/FBX2FLVER) and
  [FBX2FLVER-MTDFIX](https://github.com/infernoplus/FBX2FLVER-MTDFIX) for
  correct DS1 material layouts, [FLVER Editor 2](https://github.com/Pear0533/FLVER_Editor/releases),
  and the [Aqua Toolset](https://github.com/Shadowth117/Aqua-Toolset) for FLVER→FBX.
- **Animation events:** [DSAnimStudio](https://github.com/Meowmaritus/DSAnimStudio/releases).
- **Params, maps and text:** [Smithbox](https://github.com/vawser/Smithbox),
  [DSMapStudio](https://github.com/soulsmods/DSMapStudio), and Paramdex
  `DS1R/Defs` for param layouts.
- **Libraries:** [SoulsFormats](https://github.com/JKAnderson/SoulsFormats) and
  [SoulsFormatsNEXT](https://github.com/soulsmods/SoulsFormatsNEXT).
- **Worked examples:** [Paz the Hero (DS1R) character replacement](https://www.nexusmods.com/darksoulsremastered/mods/227)
  and the [export/edit/import tutorial](https://soulsmodding.wikidot.com/tutorial:export-edit-and-import-models).
- **Porting PTDE mods:** [PTDE-to-DSR Porter](https://github.com/kingborehaha/PTDE-to-DSR-Porter).

Index of everything above: [Souls Modding Wiki tools](https://soulsmodding.com/doku.php?id=tool%3Amain).
