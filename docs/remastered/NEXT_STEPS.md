# MarioSouls DSR: status and next steps (updated 2026-10-21 01:50)

## v100 (2026-10-21 01:50): Phase 2 complete

Backup: `mariosouls-dsr\backup-20261021-0150-phase2-done\`.
- **False walls:** a native shortfall must persist 2 ticks before Mario's
  momentum is cut (stairs and slopes gave one-tick shortfalls that stopped him
  mid-run). Past 0.6 m, Mario is realigned without touching momentum.
- **Keyboard and mouse filter:** runtime.cpp's DirectInput8Create wraps the
  game's IDirectInput8::CreateDevice (slot 3). The keyboard/mouse devices'
  GetDeviceState (9) and GetDeviceData (10) are vtable-patched. While driving,
  WASD, Mario's jump/crouch/walk keys and his mouse attack button read as
  released for the game. F8 held 1 s toggles driving (like L3+R3).
- **Checked live:** everything feels great.

## v99 (2026-10-21 01:43): Phase 2 working: Mario drives the player (native motion)

Backup: `mariosouls-dsr\backup-20261021-0143-phase2-native\`. Settings:
`mario=step`, `drive=native`, `travel=0.85`.

- **Native motion hook** (`native_motion.h`, the counterpart of PTDE world.h):
  DarkSoulsRemastered.exe RVA 0x2BB020,
  `void SetMotion(ChrPosData *rcx, float xmm1, const vec4 *r8, bool r9b)`. It
  copies three vec4s to ChrPosData +0x70/+0x80/+0x90 before the physics step.
  +0x70 is the frame displacement in the local frame (forward -Z).
  - **Finding it:** the motion probe (`motionprobe=on`) found +0x70 live, then
    the function was found statically. It starts `40 53 48 83 EC 20 FF 81 00 01 00 00`
    (inc [rcx+100h], like PTDE's inc [esi+0E4h]). MinHook (already linked);
    installed only when `drive=native` is first enabled, after a byte check.
  - **Mailbox:** the 30 Hz Mario tick publishes a target (Mario's position) and
    facing (SM64 faceAngle + pi). The hook applies the remaining distance spread
    over the next 2 game frames (one frame made top speed stutter), then zero.
    Local delta = [cs*dx - sn*dz, dy, sn*dx + cs*dz] with a = facing.
  - **Reconcile:** before stepping, only when the knight finished his step
    (g_pending==0) and ended clearly short of the target (more than 5 cm or 30%
    of the step). Then Mario is corrected to him, with momentum removed into
    the shortfall (a wall). Checking mid-step or every tick read as walls and
    crawled (v98). A safety net applies if the knight stalls: 1.5 m.
- **Input:** the game's XInputGetState is wrapped (import table). While driving,
  the knight gets a neutral left stick and no A/B/LT. Mario reads the pad
  through his own XInput handle. L3+R3 held 1 s toggles driving (for menus).
  The keyboard is NOT filtered yet (the knight still rolls on Space).
- **Speed:** `travel=` maps SM64 movement into the world at a scale (SM64 itself
  unchanged, so dive/run thresholds hold). Scaling the stick (`stepspeed=`)
  loses the run below 0.85 and the dive below about 0.91 (SM64 dive needs
  forward speed 29 of 32), so keep stepspeed=1.
- **Dead ends:** the warp request (ChrMapData +0x108) ignores collision;
  writing ChrPosData +0x10 is overwritten by physics; steering the knight's
  stick (drive=input) lags and overshoots (ice feel).

Next: keyboard filtering (DirectInput, we are the dinput8 proxy); defaults
(`mario=step`, `drive=native`) once settled; combat (Mario's attacks to native
damage, PTDE native_combat.h); jump/ledge heights (the knight doesn't jump;
SM64 floor is a flat patch at his feet: Phase 3 collision).

## v89 (2026-10-21 00:30): real SM64 Mario, Phase 1 working

Backup: `mariosouls-dsr\backup-20261021-0030-sm64-phase1\`. Plan:
`work\dsr\SM64_MARIO_PLAN.md`.
- `mario=step` (in inject_mode.txt; the built-in default is still `pose`):
  SM64 simulates Mario from the controller (XInput, read-only).
  - **Controls:** stick to walk/run; A jump; RB punch/kick; a quick B tap is an
    SM64 B press (dive while running); LT is Z (long jump, backflip, side flip,
    ground pound).
  - **Floor:** a 12 m floor square at the knight's feet, refreshed every tick.
  - **Tether:** the knight is authoritative horizontally (`resolveMotion` every
    tick); SM64 keeps jump height, momentum, action and animation.
  - **Checked live:** directions and every move tested are correct.
- **Hiding:** helmets, hoods and other pieces hanging off the head are now
  hidden. The face rule shares its bones with the chain rule. For skinned
  (player-shader) pieces, the leftover test and the "mixed bones" guard are
  waived, because a helmet reuses the face's head and neck bones.
- **On load:** the knight shows for a few seconds after loading in, while
  pieces collect their 3 votes.
- `diag=on` adds `hide-unlearned` lines (unlearned pieces at the player, with
  their bone counts and flags).

Next: make `mario=step` the default once it's been played for a while. Phase 2
(Mario drives the knight's movement) needs DSR's movement request found.

## v85 (2026-10-21 00:05): defaults built in, diagnostics off

Backup: `mariosouls-dsr\backup-20261021-0005-v85\`.
- The working settings are built into the DLL (`kDefaultModeText` in
  geometry_pass.cpp). inject_mode.txt may be empty or missing, and any
  `key=value` line in it overrides that default, live. Caution: the reader
  doesn't skip comments yet, so a comment containing `hide=off` would act as
  a setting. Lines starting with `#` will be ignored from the next build.
- `diag=on` turns on the expensive probes and logging: the eye scan (it read
  every upload for 1 s in every 10), shadow probes, face counters, the
  hide-miss/pass/shadow logs and jitter measurement. Default off; no hitching.
- Dead experiment code is now switched off but not yet deleted. Delete it in a
  later pass once this is confirmed stable.

## Where things stand

- **Mario renders in place of the player, tracking exactly**, while walking,
  sprinting, rolling and moving the camera, at full frame rate. This is build
  v65.
- **Backup of that state:** `mariosouls-dsr\backup-20261020-0125\`. The save is
  in `Desktop\DS1 Backup Saves\Remastered-20261020-0125\`.
- **Settings file:** `%LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt` must keep
  `upload=on`. The old `tilt=` and `locktol=` keys are now unused.
- **How it works:** see `work\dsr\CAMERA_SYNC_ANALYSIS.md` (the "SOLVED"
  section). The bone mapping is in `work\dsr\BONE_TABLE.md`, and
  `tools\flver_tools.py` reads the game's FLVER2 and BND3 files.

## UPDATE (v84, 2026-10-21 00:00): hiding hardened; shadow parked

Backup: `mariosouls-dsr\backup-20261021-0000-hide-v84\`. Settings: `hide=on`,
`hidemask=3d`.

Since v77:
- **Bonfire strobing (v78):** level pieces drawn after the face kept the
  face's bones as leftovers. The face rule now ignores an upload whose later
  slots all repeat the previous upload while its slot 0 differs.
- **Walls disappearing (v80-v81):** level pieces carried the arm's 24
  leftover bones and were chained through them. The chain rule now also
  requires one of the vertex shaders used by pelvis-rule pieces (skinned).
  The face rule doesn't require it. Also: an upload whose whole palette is an
  exact copy of the previous one while the header differs is rejected.
- **Learned pieces are logged** with their rule: `hide:` lines show
  `indices(rule,bones)`. A healthy session learns 9 or 10 (pelvis, chain, face).
- **Live exclude list:** `hidenot=a,b,c` (index counts) stops hiding those
  pieces. It was used to confirm the wall impostors.
- **Shadow map:** a 2048x2048 depth-only target, found per context. Learned
  keys are skipped there (hidemask 0x10). Its 2288 uploads have the same
  layout (light view-projection at 0, palette at 0x1C0), but **bone
  translations are relative to the light's camera; only rotations match the
  main pass**. v84 learns shadow pieces by rotation (rule "shadow"): it
  matched about 26k records, all of them pieces already learned.
- **Smaller-depth passes** (960x540, 480x270): learned keys are skipped there
  (hidemask 0x20; the 480x270 pass did have the player).

### Shadow: still a flickering knight shadow (PARKED)
Every knight piece is skipped in the shadow map, yet a flickering shadow
remains under the player (on stairs, in sun). So it doesn't come from the
knight's meshes in the shadow map. Candidates:
- a low-detail shadow proxy mesh: frequent unlearned shadow-pass draws are
  3627, 966, 3495, 932/142/3640 indices (see the `hide-shadow: unlearned` line);
- a shadow map cached across frames;
- a separate character-shadow or decal technique.

For now: lower or turn off Shadow Quality in the game. Next time: test with
`hidenot`-style live switches that skip those candidate index counts in the
shadow pass only.

**Mario's own shadow (later):** draw Mario's mesh depth-only into the
2048x2048 shadow map, using the light view-projection from the shadow pass's
2288 upload. Place him with the same pelvis/hip method as on screen, using
that upload's own pelvis (rotation match, translation relative to the light
camera).

## DONE (v77, 2026-10-20 10:44): the player's own model is hidden

Backup: `mariosouls-dsr\backup-20261020-1045-hide\` (sources, `dinput8_v77.dll`,
settings). Settings need `hide=on` and `hidemask=d`.

How it works (camera_capture.cpp, `hideplayer` / `classifyPending`):
- DrawIndexed is hooked. After a 2288-byte bone upload near the player with 3
  or more bones, the draws of that piece (all sections with the same buffers)
  are recorded with the upload's per-slot matrix hashes.
- Records from all contexts are classified together about once per frame.
  A piece is the player's when its **slot 0** is the anchor's pelvis, or a bone
  of a piece already found to be the player's (arms: R_Forearm; head: Spine1).
  Only slot 0 counts, because later slots can hold **leftover matrices** from
  earlier uploads: level pieces carried the arm's 24 bones, and the old "two
  shared bones" rule learned 196 meshes and hid walls and floors (v72).
- The face (3 bones: two of its own plus the neck) is taken by a narrow rule:
  slot 0 passes the hip test, 8 or fewer bones, and the later slots are a mix
  of player and non-player bones.
- A draw key (buffers, offsets, range) with 3 or more votes is skipped when
  it's drawn right after its own upload (hidemask bit 1). The player is drawn in
  2 passes (depth, then the 4-target main pass), each with its own upload.
- Do **not** enable hidemask bit 2 (skip the key in other passes): the sky
  flashed (v69). Bits 4 and 8 never matched anything and can be removed.
- Weapons are still drawn (Mario gets his own weapon later).

Still open: the 1-frame purple stretch on first load. It happens with hide=off
too, so it isn't the hiding.

## (old plan, kept for reference) hide the player's own model (graphics side)

Decision taken: skip the player's draw calls and keep their constant-buffer
uploads, because Mario's tracking depends on those uploads. No game-memory
writes. The memory flag `ToggleDraw` (`PlayerIns+0x2A4`, bit `0x800000`) is
the fallback, but it would probably stop the uploads.

Plan:

1. **Hook `DrawIndexed`** (ID3D11DeviceContext vtable slot 12) next to the
   existing Map (14), Unmap (15) and OMSetRenderTargets (33) hooks.
   - They are installed in `camera_capture.h` around lines 398-481
     (`ctxslot::`, `CamCtxHooks`, and the clone in `findCamCtx`).
   - Install it with the same 15 s delay.
   - First log which draw call the player's meshes use; add
     `DrawIndexedInstanced` (slot 20) if needed.
2. **Record each draw in the current pass** on each context:
   - the draw key: vertex buffer (`IAGetVertexBuffers` slot 0), index buffer,
     start index, base vertex and index count;
   - the palette of the most recent 2288-byte upload on that context: per-slot
     hashes of the 3x4 matrices from float 112, skipping zero and identity
     matrices.
3. **Classify when the pass ends** (in the OMSetRenderTargets hook, before the
   pass resets):
   - Player uploads are those whose slot 0 is the pelvis: the repeated slot-0
     value, or the latest pelvis value recorded this frame.
   - Spread the label to uploads that share **two or more** identical matrices
     with a player upload. Arms share the forearms and twist bones; the head
     shares Spine1, Neck and the clavicle. Requiring two guards against stale
     data left in unused palette slots.
   - Optionally also require every valid slot to map (0, 1, 0) to within about
     2 m of the player.
   - Add their draw keys to a global "player meshes" set, and relearn when
     equipment changes.
4. **Skip player draws:** draws whose key is in the set are not forwarded, when
   the live setting `hide=on` is set (default off at first). This also removes
   the player's shadow and reflection, since the same meshes are drawn there.
5. **Test:**
   - Mario is visible and the player isn't, in every pass.
   - Enemies are untouched, including grabs and close combat.
   - Changing armour still hides the new pieces.
   - The per-draw hook cost is logged.

## After that

- **Mario's shadow:** draw Mario into the shadow-map pass as well, since the
  player's shadow goes away with step 4.
- **Code defaults:** make `marioyaw=flip` and `upload=on` the defaults in code;
  remove dead paths (eye pairing, framesync, rootlock, memory-camera smoothing)
  and the diagnostics probes (`playerprobe`, the eye probe, `measureJitter`).
- **Docs:** add `REMASTERED_FINDINGS` and `CHANGELOG` entries for the 2026-10-19
  and 2026-10-20 work: VirtualQuery cost, the upload anchor, and the bone table.
- **Run animation:** Mario uses only the walk clip, with cycle speed taken from
  ground speed. Add SM64 run/sprint clips and states (roll, attack) later, from
  the PTDE presentation work.

## Hard-won facts, so they aren't re-learned

- `VirtualQuery` costs about 6.7 ms per call inside DSR. Never call it on a
  render thread.
- Game memory (player and camera) runs about 2 game updates ahead of the drawn
  frame, and nothing reliable ties a memory sample to a frame.
- The 2288-byte constant buffer layout:
  - view-projection at offset 0;
  - an eye-like field at 0x140, which is **not** the main camera for player
    draws;
  - the bone palette from float 112, 3x4 matrices, slot j = `mesh.bones[j]`.
- Palette entries are **skinning matrices**. The hip joint is the pelvis bind
  position (0, 0.970, -0.054) pushed through the matrix.
- The forward pass is recorded in chunks across deferred contexts. Only chunks
  containing the player's draws have this frame's player data.
