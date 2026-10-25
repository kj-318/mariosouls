# Remastered: from test triangle to Mario (plan, 2026-10-19)

Written at the end of the 2026-10-19 session. The live facts this plan relies on
are in `docs/REMASTERED_FINDINGS.md` sections 21-23.

## Already working (live)
- The injection point: binding #4 (`fmt 26` + `fmt 87`, D24S8). The draw is
  issued just before the game rebinds away from it, with state saved and
  restored (`geometry_pass.cpp`).
- Camera-relative view-projection (`camera_capture.cpp`). Treat the 16 floats
  as rows: `clip_i = dot(row_i, (p,1))`. Depth is standard (near 0, far 1), so
  use LESS_EQUAL.
- Camera world position at `[exe+1C6E188] + 0x60`, and the player position and
  facing through `WorldChrMan` (`world_observation.h`).
- The pass is forward-shaded, so **Mario must be lit by our own pixel shader.**

## Steps
1. **Wire `DSRRenderer` (render_device.h) into the injection point.**
   It has never run before. Fix these first:
   - `worldVS` outputs `SV_POSITION, COLOR, TEXCOORD`, but `pixelPS` takes
     `COLOR, TEXCOORD, SHADE`. That is the same VS/PS signature mismatch that
     made the test triangles black. Add `SHADE` to the VS output, or remove it
     from the PS.
   - The header comment still says the renderer is deferred and targets the
     G-buffer. Both are wrong per section 22, so update it.
   - Camera mapping: the shaders compute `mul(v, world*view*proj)` (row-vector
     form), so pass `world = translate(-cameraPos)`, `view = identity`, and
     `projection = transpose(capturedVP)`. Check this against
     `transpose16` and HLSL column-major packing with the test triangle first,
     by drawing the same player-anchored triangle through `DSRRenderer::mesh`.
   - `compose()` takes the colour target and depth from the binding recorded in
     `CamCtxHooks` (rtv[0] + depthView) with `depthVerified = true`.
2. **Mario geometry:** libsm64 (`work/libsm64`) builds the guest x64 runtime.
   Feed it the player position each frame, and draw Mario's triangles with
   `mesh()` at the player position (presentation under native locomotion,
   the first milestone in AGENTS.md).
3. **Shading:** start with simple lambert plus ambient in `pixelPS`, using the
   vertex normal/shade term. Matching the game's lighting comes later.
4. **Hide the native character model** (DSR-Gadget `ChrFlags1` ToggleDraw at
   PlayerIns+0x2A4, bit 0x00800000). This is the first *write* to game memory,
   so it needs its own decision and test.

## Testing approach that worked tonight
- Keep the variants switchable in `inject_mode.txt`, so one launch can test
  several.
- Screenshot the game window to check results, with no input to the game.
  Only build while the game is closed.
- Stage every file written to the PC under a new name, and read it back
  afterwards (one stale write happened tonight).
