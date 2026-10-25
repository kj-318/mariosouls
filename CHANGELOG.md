# Changelog

Meaningful changes are recorded here by development version. The detailed, maintained inventory of current behavior and tuning is [Features and tweaks](docs/FEATURES_AND_TWEAKS.md). Git retains earlier implementation history; no public release date is implied by this development log.

## Unreleased

### Documentation

- Fork: rewrite the README and player guides (installation, playing, compatibility, troubleshooting, recovery, bug reports) for Dark Souls Remastered. Developer guides now describe the Remastered build and package; PTDE pages are marked as the original reference host.

- Add a source-pinned PTDE behavior checklist for a Remastered port, covering 24 areas with source files/symbols, resulting behavior and tuning values. Separate reusable runtime/core code, edition-specific integration, known candidates and removed experiments; leave implementation order and verification strategy to the port author. Link it from the feature inventory and port handoff. No gameplay or installation is changed.

## 0.9.0-dev.12 - Remastered: playable Mario, shippable package

### Live results (2026-10-21 to 2026-10-25, disposable character, offline)

- Ladders: Mario holds the climb pose on the rungs (offset along the ladder); holding B climbs faster; standing still no longer drops him off.
- Estus: the knight's flask and spill are hidden while Mario drinks, the heal glow is kept. The flask is a TAE-spawned effect (type-96 event 22750 at dummy 26 in anims 7586/7587, plus type-114 events 227/229); those events are patched in memory only while Mario is shown and restored otherwise.
- Breakables (pots, crates, barrels) break from Mario's punches, kicks, pounds and dodge, through the native object damage path.
- Dodge (B) gives a small stamina knockback to small enemies using the Caestus-kick attack row 4200410 (0% damage, high stamina damage), with a temporary 30 cm knockback.
- Stamina: punches, kicks and dodge need stamina; at 0 the bar stays at 0 until native regen restarts; no spending while the previous move is still playing (300 ms minimum gap and move-busy states).
- Death planes: elevator shafts and pits kill like PTDE (hit filters 14/15 swept while falling).
- Sloped walls with 0.01<|ny|<0.30 (Hellkite bridge underside, Capra arena) get vertical stand-ins, so Mario bonks and wall-kicks on them.
- Air wall teleport-back fixed (no air shortfall snap next to a static wall); stair step-assist no longer interferes with BLJs.
- Menus, item pickups, NPC talk and shops hand control back; bonfire sit releases when the knight stands up or walks away.
- Hollow Soldier Helm is the Wing Cap: Mario wears the winged cap with SM64 flight and the cap-on animation. New characters start with it.
- Text: Sen's Fortress is "Whomp's Fortress", Hollow Soldier Helm is "Wing Cap", Pyromancy Flame is "Flower Power".

### Packaging

- `tools/package_remastered.bat` builds `work/release/MarioSouls-DSR.zip`: `dinput8.dll` plus `MarioSouls/mario64.dll` and `sm64.dll`. Players add only their own SM64 (US) ROM (`.z64/.n64/.v64`) to the game folder.
- Collision is built on first launch from the player's game files by a C++ port of the Python exporter (`collision_build.h`: DCX, BND3, BHF3/BDF3, Havok 2015 tagfile, MSB1, EMEVD). Output is byte-identical to `prepare_collision.py` for the maps checked. No game data or ROM is distributed.

### Known issues / deferred

- Asylum to Firelink crow arrival, Solaire/Pinwheel part culling near Mario and lava are deferred.
- Wing cut-out shader fix, bonfire-toggle sit fix and the clean-install test await live confirmation.

## 0.9.0-dev.11 - Remastered: Mario replaces the player

### Live results (2026-10-19 to 2026-10-21, disposable character, offline)

- Mario tracks the character exactly while walking, sprinting, rolling and turning the camera, at full frame rate (build v65). The anchor is the player's own bone upload: the 2288-byte constant buffer holds the view-projection at 0 and the skinning palette from byte 0x1C0. The pelvis is the hip-gated slot-0 matrix repeated across the player's pieces, and the hip is the pelvis bind position (0, 0.970, -0.054) through that matrix. Bone mapping is from the game's FLVER2 files (`work/dsr/BONE_TABLE.md`, `tools/flver_tools.py`).
- The player's own model (body, armour, head and face) is hidden by skipping its draws, with no memory writes (v77-v84). Pieces are learned from their slot-0 bone (the pelvis, a bone of a confirmed piece, or the small-face rule), with guards against leftover palette data and a skinned-vertex-shader check. The learned pieces are also skipped in the 2048x2048 shadow map and in the smaller depth passes. Weapons are still drawn.
- `VirtualQuery` costs about 6.7 ms per call inside DSR, so render threads never call it.

### Known issues

- A flickering player-shaped shadow remains in sunlight. It doesn't come from the knight's meshes in the shadow map (`work/dsr/NEXT_STEPS.md`). Workaround: lower Shadow Quality.
- 1-frame purple stretch on first load (also with hiding off).

### Code

- `camera_capture.*`: DrawIndexed/DrawIndexedInstanced/Draw/DrawAuto hooks, the per-frame player-piece classifier and per-pass skipping.
- Working settings are built in (`kDefaultModeText`). `inject_mode.txt` only overrides them, and lines starting with `#` are ignored. `diag=on` enables the expensive probes and logging, which are off by default; this removed a periodic hitch.

## 0.9.0-dev.10 - Remastered world anchoring at the player

### Live results (2026-10-19, disposable character, offline)

- Player world position read through the DSR-Gadget `WorldChrMan` chain (found by AOB). Its player object matched our verified health base, which confirms it.
- Camera world position found by an automatic projection-consistency search: `[exe+1C6E188] + 0x60` (CamMan). The player stayed centred on 92 of 98 checks.
- A test triangle placed at `player - camera` stands at the character's feet, stays there while walking and turning, and is occluded by the character (`docs/REMASTERED_FINDINGS.md` section 23).

### Code

- `world_observation.h`: read-only AOB scanning, the player-position chain with the health cross-check, and the camera search with path adoption. `inject_mode.txt` gains `anchor=camera|player`.

## 0.9.0-dev.9 - Remastered injection target identified; world-anchored test geometry

### Live results (2026-10-19, disposable character, offline)

- The two-target binding (`fmt 26` + `fmt 87`, D24S8) is the visible main pass. Host writes to its target 0 appear every frame as flat, unshaded colour, so the pass is forward-shaded. Mario will need host-side shading. The earlier G-buffer assumption is withdrawn (`docs/REMASTERED_FINDINGS.md` section 22).
- Host triangles placed in camera-relative world space with the captured view-projection project correctly and are occluded by the game's depth using a standard `LESS_EQUAL` test.
- The earlier black triangles came from the old precompiled shaders (mismatched VS/PS signatures) plus a blend state that applied the target-0 write mask to every target. They did not come from the game.

### Tooling

- `tools/compile_test_shaders.py` compiles `test_shaders.hlsl` with the system `d3dcompiler_47.dll`. `tools/build_remastered.bat` runs it, then the host build, and writes `work/framework-build/last-build.txt`.
- `%LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt` selects test passes, targets, values and depth handling live (re-read every 500 ms). The log reports where the world markers project.

### Not yet done

- Camera/player world position, so geometry can be anchored to a world point rather than relative to the camera.
- Mario geometry and host-side shading.

## 0.9.0-dev.8 - Remastered test draw made safe to run

### Live result (2026-10-19, disposable character, offline)

- A host-issued draw now reaches the game's frame and stays stable. Resources were created ("injection armed") and draws were logged as "state saved and restored". The user saw a large magenta triangle in the centre of the screen, and the session ran normally until it was closed. The triangle flashes because the test draw is rate-limited to one draw per 250 ms, so only some frames carry it.
- Two bindings match the current signature (`fmt=26` colour target with `D24S8` depth at 1920x1080): binding #4 (`26` + `87` multi-target) and binding #5 (`26` alone). Which one carried the visible triangle is not yet known. Whether the triangle was lit by the game's lighting pass, which would confirm a G-buffer, is also not yet established.

### Fixes (built with `tools/build_remastered.bat`, run live as above)

- Device resources are created through the `ID3D11Device` interface instead of hand-typed vtable function pointers. The hand-typed `CreateVertexShader`/`CreatePixelShader` signatures passed a string as the `ID3D11ClassLinkage*`, and `CreateBuffer` had two extra parameters, so `D3D11_CPU_ACCESS_WRITE` (0x10000) arrived as `ppBuffer` and the runtime would write the new buffer pointer to address 0x10000.
- The test draw saves and restores every piece of pipeline state it changes (input layout, vertex buffer, topology, VS/PS with class instances, rasterizer, depth-stencil with stencil ref, blend with factor and mask), through the `ID3D11DeviceContext` interface. The earlier save crashed because of wrong slot numbers, not because saving was unsafe: slot 75 is `PSGetSamplers` (`IAGetInputLayout` is 78) and 93 is `SOGetTargets` (`OMGetRenderTargets` is 89). Removing the restore left the game drawing with the host's state.
- The draw is issued when the game rebinds *away from* a geometry-pass binding, not straight after binding it, so it follows the game's G-buffer clear and the pass viewport.
- Test depth state is ALWAYS with depth writes off, and culling is off, so the test result does not depend on depth convention or winding, and scene depth is left untouched.
- Contexts sharing a hooked vtable get their own bookkeeping entry with the original functions copied in. Previously an unregistered context found no entry: `Map` returned `E_FAIL` without mapping, `Unmap` was not forwarded, and `OMSetRenderTargets` was dropped.
- The binding copy no longer keeps the game's `rtv` array pointer, which may be stack memory.

### Documentation

- `work/dsr/MODDING_RESOURCES.md`: third-party DSR offsets (DSR-Gadget, Phokz tables), DX11/proxy-DLL mods, FLVER model tooling and the manual test-launch procedure. DSR-Gadget's PlayerIns HP offset matches our verified `+0x3E8`. Its stamina (`+0x3F8`) and position chain are unverified here.

## 0.9.0-dev.7 - Remastered render-target binding observation

### Observation

- `OMSetRenderTargets` hooked on all contexts; each distinct attachment signature logged once with a call count. Bindings are identified by target count, per-target format/dimensions/sample count and depth format, so passes are recognised from what is attached rather than from call order.
- Descriptors read through `ID3D11Texture2D` obtained by `QueryInterface`, with `GetDesc` at slot 10; resources reached from views via `ID3D11View::GetResource` at slot 7.
- Candidate geometry pass identified from attachment formats: `R16G16B16A16_FLOAT` at 1920x1080 with `D24_UNORM_S8_UINT` depth. Recorded as a candidate only — not confirmed as the geometry pass, which requires draw observation or a test draw.

### Corrections

- Four descriptor failures recorded in the findings doc: `SetEvictionPriority` mistaken for `GetDesc` (crashed the game), fabricated interface GUIDs (all `QueryInterface` calls returned `E_NOINTERFACE`), a wrong `D3D11_RESOURCE_DESC` layout that put `Format` at offset 52 instead of 20, and a validity check that rejected `Format=87` when `R8G8B8A8_UNORM` is 87.
- Vtable slots are no longer probed by calling them; doing so crashed the game twice. Interfaces are resolved by `QueryInterface` and methods reached only at documented indices.

### Not started

- No guest draw, pipeline-state management, or render-target binding for our own resources. Injection remains unimplemented.

## 0.9.0-dev.6 - Remastered context vtable slot identity verified

### Slot derivation

- Documented the `ID3D11DeviceContext` vtable layout: own method position plus **7** inherited slots (3 `IUnknown` + 4 `ID3D11DeviceChild`). `Map`=14, `Unmap`=15, `OMSetRenderTargets`=33. Slot constants are derived from the installed `d3d11.h` rather than assumed.
- Added a pre-patch guard that verifies a context's vtable is usable, that the patched slots are distinct from each other, and that they do not already hold a host hook. The guard refuses to patch and logs instead.
- Camera capture re-verified with all contexts patched from genuine `Map`/`Unmap` slots, tracking camera motion across thousands of updates.

### Corrections

- An intermediate change moved `Map`/`Unmap` from slots 14/15 to 10/11 by assuming only 3 inherited slots, ignoring the `ID3D11DeviceChild` layer. Slots 10/11 are `PSSetShader`/`VSSetShader`; patching them crashed the game on save load. The original 14/15 were correct. Recorded because the crash is what disproved the premise.
- Deferred contexts implement `ID3D11DeviceContext` (confirmed by `QueryInterface`, same object through `Context4`) but use separate implementations from the immediate context, so their vtables hold different function addresses at identical indices. A check that compared addresses across contexts rejected every deferred context; it was replaced with structural checks.
- `Map`, `Unmap` and `OMSetRenderTargets` are not `d3d11.dll` exports and cannot be identified via `GetProcAddress`; that approach reported a false mismatch on all slots.

### Not started

- Geometry-pass injection. `OMSetRenderTargets` is now known and verified, but no render-target hook, guest draw, or pipeline-state management exists.

## 0.9.0-dev.5 - Remastered health and stamina chains verified

### Player observation (live-verified)

- Health and stamina are reached through two static module-relative pointer chains instead of a heap scan. Health root `exe+01726B14`, stamina root `exe+01A31768`. Both resolved identically after a game restart; the executable has no ASLR, so the roots are fixed.
- Health verified live: HP moved `453 -> 401 -> 594` across damage and healing, with MaxHP matching the character's maximum.
- Stamina verified live: drained `95 -> 65 -> 35 -> 5` under sprinting and refilled to 95 across repeated cycles.
- The stamina field is a signed deficit accumulator that goes negative when overspent (observed to `-25`); the host clamps the reported value at 0 and tracks the observed peak as the pool size.
- The health chain's `+0xE0` link is null until a character is loaded, so resolution fails at the title screen and succeeds in play. Resolution is retried, and a base that stops validating is dropped so a level change re-resolves.

### Corrections

- Reading stamina at `base + 0x3F0` returned a constant 49 that never moved during play while the real pool was 95. It passed a loose sanity range, which is why it looked correct. Those offsets are marked wrong in `native_types.h`; the field is only trusted once observed moving.
- `buildCapabilities()` derived both Health and Stamina from `player.valid`, which only reflected HP, so it advertised a Stamina capability that was not working. Each capability now depends on its own observation.

### Stability

- Chain walking validates the source address *and* the resulting pointer value. Checking only the source let a garbage read be dereferenced, which crashed the game on save load during probing.

### Documentation

- Added `docs/REMASTERED_FINDINGS.md` A18 recording both chains, the stamina accumulator semantics, the corrected offset, the capability fix, and the chain-walking guard.

## 0.9.0-dev.4 - Remastered view-projection capture verified

### Camera capture (live-verified)

- Combined view-projection captured and confirmed to track camera motion across 500,000+ updates. Source is the `Map` payload of the 4112-byte dynamic constant buffer, at offset 0, read at `Unmap` (a `Map`-time read returns zeroes because the game writes the payload afterwards).
- Deferred contexts are now patched as they are created; the view-projection buffers exist only on deferred contexts, never on the immediate one.
- Reject the separate axis-aligned projection (1040-byte buffer, static `m11 = 2.5386`) and use only the 4112-byte buffer. The 2288-byte buffer is a different shader variant with its own FOV term, and mixing the two interleaved two different FOVs.
- Per-context `Map` bookkeeping replaces a single shared pointer. One shared pointer raced across the patched deferred contexts and read the wrong buffer as a matrix.

### Player observation

- Heap scanning for the player base is disabled. Four live runs produced only false positives (a recycled heap block, and 48 sliding-window matches across one unrelated structure). The verified `+0x3E8` offsets are correct, but the per-session heap base cannot be identified from value shape; an anchored pointer chain is required. `Health`/`Stamina` stay clear until then.

### Stability

- All vtable hooks install on a worker thread 15 s after startup. Eager patching during device creation causes a white screen at the main menu.
- Observation is driven by that worker and by buffer uploads, replacing a `Present` hook: the swap chain is created before the delayed hooks exist and `IDXGIFactory2::EnumSwapChains` does not report it, so `Present` was never reached.

### Documentation

- Added `docs/REMASTERED_FINDINGS.md` A17 recording the verified layout, invariants, Unmap requirement, per-context tracking, delayed-installation stability result, and the player-discovery failure modes.

## 0.9.0-dev.3 - Remastered host skeleton + player structure

### Remastered host (matrix-only integration)

- Verified player structure via live Cheat Engine session: HP at +0x3E8, Max HP at +0x3EC, Stamina at +0x3F0, Max Stamina at +0x3F4 (all int32). Base address changes per session; offsets are stable.
- Implemented `PlayerState` in `native_types.h` with SEH-protected `readFrom()` and verified offsets.
- Updated `capabilities.h::buildCapabilities()` to set `Health` and `Stamina` flags when player observation is valid.
- Added `scanForPlayerBase()`, `updatePlayerObservation()`, `getViewProjectionMatrix()`, `getObservation()`, `getCapabilities()` to `runtime.cpp`.
- Split `camera_capture.h` into header + `camera_capture.cpp` with internal `g_cameraState` and `getCameraStateInternal()` for hooks.
- Host skeleton (9 files: `runtime.cpp`, `camera_capture.h/.cpp`, `native_types.h`, `capabilities.h`, `loader.h`, `startup.h`, `memory.h`, `README.md`, `render_device.h`) compiles cleanly on MSVC and Zig.
- `python tools/framework.py build --check-portability` succeeds: x64 guest binaries, C/C++ SDK headers for x86/x64, no runtime execution.
- Composition point confirmed: inject during geometry pass (G-buffer + scene depth); Mario becomes part of G-buffer and is lit by lighting pass.
- View-projection matrix captured from GPU dynamic buffers (2288/4112 bytes) via Map/Unmap hooks on all 12 contexts (1 immediate + 11 deferred).

### Documentation

- Extended `docs/REMASTERED_FINDINGS.md` with §15 (player structure verified) and §16 (matrix-only integration implemented).
- All findings distinguish implemented behavior, live-session evidence, compilation checks, and unverified proposals.

## 0.9.0-dev.2 - experimental preview

### Fire Flower cast handoff

- Wait for confirmed native footing before sending either hand's cast input, using the existing 150-ms grounding policy instead of spending the button pulse during movement handoff. Cancel stale requests on focus/device/equipment changes, damage or a one-second preparation timeout. LB/L1 remains left cast; native spell charges and bouncing-fireball behavior are unchanged.
- Add bounded errors for rejected, cancelled or natively unaccepted cast requests. Compilation/source evidence only: the reported left-hand failure and this candidate still need live verification; no install is implied.

### Host-port preparation

- Separate validated PTDE camera/actor observation from neutral character vertices/camera matrices and backend graphics-state restoration. Split F8 native capture from reusable diagnostic shape/panel assembly; keep existing lens, lighting, layers, retention and interpolation behavior.
- Give module/mesh resources and gameplay/presentation/control/visual/native-restoration state explicit non-copyable owners. Preserve native release ordering, single-instance coordination and graphics-callback update scheduling; document remaining feature-local state/include-order debt.
- Compile the extracted CPU storage/presentation headers for x86/x64, inspect x64 guest/runtime PE machine types, and provide a concrete presentation-only Remastered handoff. These checks do not verify PTDE gameplay or implement Remastered. Game installation and live verification were not part of this preparation.

### Distribution

- Package a new source-based Windows installer from this committed revision, with matching source, manifest and SHA-256 checksums. Mark the GitHub release as experimental and retain pending game/installer checks explicitly; preserve the previous draft/assets and existing game installations.

## 0.9.0-dev.1 - earlier development snapshot

### Host-port preparation

- Define the intended dependency/authority/phase/renderer/ABI/build boundaries in a dedicated architecture specification. Make the design and explicit current-debt list required in AGENTS, contributor guidance and PR review, with links from host/adapter/SDK/porting guides. This documentation change adds no enforcement suite or gameplay behavior.
- Route equipment, cannon, impact flashes and F8 line/text/reticle presentation through the character's graphics-neutral renderer. Share CPU prop/cannon/quad generation; keep texture upload, D3D9 pipeline/resource/reset handling in the PTDE backend.
- Dispatch build/link and asset recipes by implemented host edition, select matching guest architecture, isolate non-PTDE outputs, and inspect DLL machine types before writing the build manifest. PTDE defaults/output paths and installation/version guards remain unchanged; Remastered is still unimplemented.
- Derive PTDE operation capabilities centrally from validated observations and hook readiness; gate movement, presentation, combat, interactions, platform observations and before-HUD composition while preserving native fallback and detailed guards.
- Document presentation under native locomotion as the first Remastered milestone, with later collision/interaction/combat porting by responsibility. No new behavior tests, gameplay tuning, installation or Remastered verification is implied; fresh PTDE game checks remain pending.

### Player installer candidate

- Add a locally built Windows setup executable with game/ROM selection and install/update/uninstall. It shares the existing PTDE setup backend, DSfix handling and recovery rules.
- Embed tracked source and official Python; download hash-pinned tools/packages, build the mod and export maps/equipment on the recipient's computer. Do not bundle ROMs, game exports, prebuilt guest runtime or compiled Mario geometry.
- Preserve source revision/hash provenance in builds from the packaged snapshot. Add a matching source archive, manifest, checksums and draft-release guidance; GitHub Actions is not required.
- Pin Windows Git line endings and use atomic dependency staging/short cache paths. In player asset preparation, record unsupported Havok types as native-only; retain strict source exports by default and flag Ash Lake traversal as unverified.
- The candidate is experimental and unsigned. Installer preparation and compilation do not establish GUI, install/removal or full-game compatibility; publication remains pending review.

### Control fixes

- Remove the legacy Y world-interaction shortcut in Mario mode. A remains contextual jump/interact, Q remains keyboard interaction, and Y still cancels an active cannon sequence.
- Map left-hand Fire Flower casting to LB/L1/right mouse instead of RT/Tab. Keep native spell charges, attunement, reinforcement and damage, right-hand casting, shield LB guard/RT parry and LT/L2 crouch/ground pound.
- Track primary-left and parry presses independently so a held input does not become a different action after an equipment change. Only a shield guard acts as the frontal-critical modifier. Native-character bindings remain unchanged.
- These input changes are compile-checked; live controller, spell-charge and interaction checks remain pending.

### Documentation

- Rewrite the player README around the Mario class, installation, basic controls, native-character toggle and collision overlay.
- Add a detailed feature/tweak inventory with current defaults, implementation references and verification limits. Require its maintenance in AGENTS.md and contribution/PR guidance.
- Document the host/core/SDK/adapter architecture, future Remastered work, capability fallbacks and the distinction between implemented Mario and possible future characters.
- Require fixes developed in forks to be pushed to the fork and proposed to the original project through a PR.

### Source and framework cleanup

- Retain Mario, F6, the initially hidden F8/F9/F10 overlay, DSfix, higher-rate visual interpolation and the technical foundations for a future host port.
- Group gameplay, terrain and drawing entry points, clarify native actor/movement ownership, and separate Mario profiles, pose mapping and item props.
- Remove unused telemetry storage/reads and improve native combat names. Preserve packet validation, native hook instruction sequences/calling conventions and public DLL export names.
- Remove the experimental Wind Waker Link adapter and its exclusive helpers, plus the custom equipment-load stamina penalty. Native armor/ring/stamina behavior remains.
- Remove test labs, executable behavioral suites, command bridges, cheats, recording/replay, F11 reporting, background automation and special Steam-free development-copy tools.
- Keep startup/error logging bounded; remove movement/session recording. Preserve existing local installations, saves, private assets, archives and checkpoint tags.

### Build and local installation

- Keep pinned dependencies/compiler checks, local asset recipes, licenses and build manifests with version, revision, source fingerprints and binary hashes.
- Add compile-only portability coverage for x86/x64 C/C++ SDK/shared declarations and x64 guest components, while retaining the x86 PTDE host.
- Limit CI to dependency preparation, compilation/linking and retained build logs. Build success is not gameplay evidence.
- Replace former installation/packaging tooling with one readable `MarioSouls.ps1 install` / `uninstall` entry point and a PTDE-owned implementation.
- Reuse asset paths/tuning, rebuild stale x86 binaries, preserve the supported DSfix chain/settings, validate ownership/hashes and keep reversible recovery snapshots.

### Gameplay retained from the development baseline

The following features predate the documentation rewrite; they are not newly implemented by it:

- Mario class/creation preview, persistent class intent, manual Mario/native switching, same-instance interactions and one-shot death presentation.
- Native collision/support integration for stairs, doors, dynamic floors, platforms, streamed maps and lethal terrain, with a smaller Mario body and reduced ordinary landing damage.
- Glove-gated Mario attacks, normal Caestus +0..15 progression, Strength/Dexterity scaling, distinct attack windows/stamina, kick push requests and confirmed-hit feedback.
- Supported one-handed weapon families, shields, parries and native critical attempts with authored Mario poses.
- Fire Flower appearance, native Fireball casting with a bouncing projectile profile, lava/fire reaction hooks and bounded hit-recovery presentation.
- Firelink cannon/Bob-omb Buddy, a wider camera, pre-HUD composition, multiple scene/UI resolutions, DSfix integration and visual interpolation.
- A guarded cutscene presentation path using the cinematic camera and player root; it remains a first pass rather than full skeletal retargeting.

See the [feature inventory](docs/FEATURES_AND_TWEAKS.md) for exact tuning and limitations rather than treating this summary as an exhaustive compatibility claim.

### Verification status

Focused historical gameplay checks exist for PTDE with/without DSfix. Recent source cleanup passed compilation and source/file inspection; installation file operations were checked in temporary directories. This documentation rewrite introduces no gameplay change or new live-game evidence. Full-game traversal, cutscenes, hazards, equipment coverage and different machines still need the [manual checklist](docs/TESTING.md).

Remastered, OoT Link and DK64 are not implemented.
