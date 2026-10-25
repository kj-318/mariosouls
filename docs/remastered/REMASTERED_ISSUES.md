# Remastered Host: Camera Tracking and Mario Rendering Issues

## Current State

The Dark Souls Remastered host renders Mario (752 triangles, 30 Hz pose updates) into the game's main forward pass. Mario appears and follows the player, but exhibits three visual artifacts:
1. **Stuttering/jitter when turning camera** - Mario shakes as the camera rotates
2. **Straying from character** - Mario drifts away from the player when camera moves fast, then snaps back
3. **Teleporting every ~1 second** - Mario jumps to a new position periodically

## Architecture

### Key Files
- `framework/hosts/ds1remastered/geometry_pass.cpp` - Main rendering logic, camera tracking, framesync
- `framework/hosts/ds1remastered/world_observation.h` - Game memory reading (player/camera positions)
- `framework/hosts/ds1remastered/camera_capture.cpp` - DX11 hooking, view-projection capture
- `framework/hosts/ds1remastered/mario_presentation.h` - Mario pose/animation thread
- `framework/hosts/ds1remastered/render_device.h` - DX11 renderer backend

### Camera Tracking Pipeline
1. **View-projection matrix**: Captured from GPU constant buffer Map/Unmap (4112 bytes, deferred context). Camera-relative, read as 4 rows.
2. **Camera position**: Read from game memory via pointer chain (WorldChrMan -> PlayerIns -> ChrMapData -> ChrPosData). Also available from eye buffer (1040/2288 byte constant buffers).
3. **Player position**: Read from game memory via same pointer chain.
4. **Frame synchronization**: `framesync::align()` matches camera from memory to eye position from GPU buffer to determine lag, then reads player position from `t - lag`.

### Rendering Pipeline
- Hook: `OMSetRenderTargets` on all contexts (immediate + deferred)
- Draw at end of engine's main pass (targetsBound=true)
- View-projection from global (latest main-scene matrix from any context)
- Camera position from game memory or eye buffer (configurable via inject_mode.txt)
- Player position from game memory (with sync=off, uses live position directly)

## Configuration (inject_mode.txt)
```
targets=multi
screen=off
world=on
depth=lequal
write=rt0
rt0=1,0,1,1
rt1=1,0,1,1
anchor=player
draw=mario
vpsource=global
possource=draw
camsource=memory
vpslot=0
marioyaw=flip
camcand=0
sync=off
```

Key settings:
- `vpsource=global` - use latest main-scene VP from any context
- `possource=draw` - read positions at draw time
- `camsource=memory` - camera from game memory (vs eye1040/eye2288 from GPU buffer)
- `sync=off` - disable frame synchronization (use live positions directly)

## Root Causes of Issues

### 1. Camera Jitter
The camera position from game memory (`g_liveCameraPos`) is slightly different each frame. The world observation updates every 500ms, but the camera position from the pointer chain might be jittery.

**Attempted fixes:**
- Exponential moving average (alpha=0.1 to 0.5) - reduces jitter but introduces lag
- Outlier rejection (> 1m, > 5m, > 2m jumps) - helps with large jumps but not small jitter
- Blending with previous (alpha=0.5, threshold=2m) - still jittery
- Using camera from world observation instead of game memory - too slow (500ms updates)

### 2. Mario Displacement When Camera Moves
The view-projection matrix changes immediately when the camera rotates, but the camera position from game memory lags by 1-2 frames. This causes Mario to be displaced.

**Attempted fixes:**
- Skip drawing for 2 frames after view change - causes Mario to vanish
- Use previous camera position for small changes - causes detachment
- Blend camera position (alpha=0.5) - reduces but doesn't eliminate

### 3. Animation Reset / Teleport
The player position from game memory is updated every 500ms (world observation interval). Between updates, the position is stale, causing Mario to teleport when it updates.

**Attempted fixes:**
- Read player position from game memory directly (g_livePlayerPos) - more current but potentially stale address
- Distance check (only override if within 10m) - helps with garbage reads

## Key Code Sections

### framesync::align (geometry_pass.cpp ~line 448)
Matches camera from memory to eye position from GPU buffer to determine lag. When camera is still, can't measure lag, falls back to live player position (which is ahead of frame).

### drawRendererMarker (geometry_pass.cpp ~line 526)
Main rendering function. Reads positions, applies camera transform, draws Mario mesh through DSRRenderer.

### readLivePositions (world_observation.h ~line 467)
Reads player and camera positions from game memory and world observation.

### World observation (world_observation.h ~line 290)
Worker thread that scans for camera/player positions. Updates every 500ms.

## What Worked Before (from logs)
- Earlier session achieved smooth rendering with "paired eye matched 4035, fell back 1" - meaning camera tracking was very accurate
- The key was using `pairedEye()` to find the eye position from the uploaded view-projection buffer
- Camera position came from the paired eye, not from game memory
- The `isPlausibleCombinedViewProjection` check filtered out reflection views

## Suggested Next Steps
1. **Use pairedEye camera source** (camsource=eye1040) instead of memory - the eye buffer position is more synchronized with the view-projection
2. **Re-enable framesync** with the lag-measurement fix (lower spread threshold to 0.01)
3. **Don't smooth camera position** - use raw eye buffer position to match view-projection timing
4. **Smooth player position only** - the player position from game memory can be smoothed since it's not tied to the view-projection
5. The "goodVp" filter (replace bad views with last good one) helps prevent flashing but may cause displacement if the good view is stale

## Files Modified This Session
- `framework/hosts/ds1remastered/geometry_pass.cpp` - framesync, camera tracking, view filtering
- `framework/hosts/ds1remastered/world_observation.h` - readLivePositions, update interval
- `framework/hosts/ds1remastered/camera_capture.cpp` - OMSetRenderTargets hook
- `framework/hosts/ds1remastered/camera_capture.h` - hook declarations
- `%LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt` - runtime configuration