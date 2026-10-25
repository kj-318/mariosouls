# Remastered: real SM64 Mario (plan, 2026-10-21)

Goal: Mario moves with SM64's own engine (run, skid, jump, dive, punch, kick)
instead of replaying a walk clip. The work is phased so each phase is playable
and can be backed out.

## What exists already

- **Adapter (x64 build):** the x64 `mario64.dll` and `sm64.dll`, built by
  `build --check-portability`, already export the full ABI4: `step(id, SC_Input,
  state, mesh)` advances SM64 one 30 Hz tick, and `resolveMotion` corrects
  Mario to a host-resolved position while keeping momentum along the surface,
  the current action and the animation phase. `surfaces()` takes collision
  triangles in local cm.
- **Host (ours):** a 30 Hz pose thread (`mario_presentation.h`), exact
  placement (upload anchor), the player model hidden, and the live player
  position and facing from memory.
- **PTDE reference:** `hosts/ds1ptde/` shows the full design: XInput input
  routing, native motion tickets, static collision from map archives, combat
  packets. None of its addresses or hooks carry over; only the design does.

## Phase 1: SM64 drives Mario's animation; the knight still drives the world

- Read the controller with XInput (read-only; the knight still gets the same
  input, so world collision, interactions, HP and combat stay native).
- Each 30 Hz tick, call `step` instead of `pose`:
  - **Stick:** sets `moveX`/`moveY`, relative to the camera (`camLookX`/`camLookZ`
    from the view matrix). Holding sprint gives full magnitude (SM64 run);
    otherwise it's walk magnitude.
  - **DSR buttons map to SM64 meaning:** roll (B tap) to dive/roll, attack (RB/R1)
    to punch/kick, jump (sprint + B) to jump.
  - **Floor:** a flat surface patch at the knight's floor height under Mario,
    re-centred as he moves.
  - **Position:** the knight stays authoritative. Each tick
    `resolveMotion(knight position)` keeps Mario on the knight horizontally.
    Height comes from SM64 while airborne, so his jumps show.
- Placement stays the upload anchor (render). Only the mesh source changes from
  `pose` to `step`.
- No memory writes and no input blocking. Risk: Mario's and the knight's speeds
  differ, so `resolveMotion` will tug him. Tune the walk/run magnitudes.

## Phase 2: Mario drives movement (knight follows Mario)

- Find DSR's native movement request (the equivalent of PTDE's motion tickets
  in `world.h`), or a safe position write with native collision resolution.
- Filter the stick away from the knight so only Mario's motion moves him.
- Needs reverse engineering on the x64 executable.

## Phase 3: world collision for SM64

- Extract static collision from DSR map files (PTDE: `prepare_collision.py`;
  DSR collision is likely a newer 64-bit Havok format) and feed `surfaces()`.
  Use native walls (`SC_CAP_NATIVE_WALLS`) where possible.

## Phase 4: combat and interactions

- Mario's attacks become native damage packets (PTDE `native_combat.h`).
  Interactions (bonfire, doors, ladders, NPCs) use native ownership with guest
  poses (PTDE `interaction.h`, `actions.h`).
