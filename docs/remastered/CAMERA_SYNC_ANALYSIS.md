# Remastered: Mario position sync, state and analysis (2026-10-19 14:05)

Backup of this state: `mariosouls-dsr\backup-20261019-1404\` (host sources,
probe settings, log, built `dinput8_v42.dll`). The save is in
`Desktop\DS1 Backup Saves\Remastered-20261019-1404\`.

## Where it stands (build v42)

- **Performance is fixed.** `VirtualQuery` costs about 6.7 ms per call inside
  DSR, presumably because the game's protection intercepts it. Mario's draw
  called it 3 times, about 25 ms per draw, which lagged the game and made Mario
  jitter. Two fixes:
  - A readable region is now cached for 250 ms (`worldobs::readable`).
  - The draw path trusts addresses the worker validated in the last second
    (`g_livePlayerChecked` and `g_liveCameraChecked`).

  Draws now average 0.05 ms, worst case 0.3 ms.
- **Turning the camera is solved.** The matrix comes from the 4112-byte upload,
  checked by orientation-independent structure. The camera position is the
  engine's eye (offset 0x140 of uploads whose offset 0 holds a perspective
  view-projection), paired with the matrix by its facing. That needed:
  - a 256-entry ring;
  - the far eye stream at (4.0, 4.1, 3.9) gated out by a 20 m limit;
  - consecutive duplicates skipped;
  - the last matched (matrix, eye) pair reused on a miss.
- **Walking with a moving camera is mostly good.** The player position is
  taken from the memory sample whose camera equals the frame's eye
  (`framesync::align`, exact path).
- **Still not right:** Mario strays while walking when the user isn't moving
  the camera.

## Why it still strays (from the v42 log)

The camera in DSR follows the player even when the user doesn't move it. On a
straight walk it slides behind the player with the same facing for many frames.

1. **Facing can't identify the frame.** About 80% of eye matches were still
   "by facing" (2705 of 3382). With several frames sharing the same facing,
   the tie picks the eye nearest the memory camera, which is the newest one,
   ahead of the drawn frame. The per-context eye added in v42 matched only
   about 20% of the time, so the 2288-byte eye is mostly uploaded on a
   different context from the one drawing the pass.
2. **The memory lead is noisy.** It measures as 1 or 2 game updates, but the
   most common value wins only 25 to 33 of 64 measurements. Memory samples are
   taken at draw time (about 120 a second) and are not tied to the game's
   frame boundary.

Both problems come from the same place: nothing ties the drawn frame to a
memory sample except camera-position equality, and that is ambiguous whenever
the camera only translates.

## Next step: anchor to the engine's own player transform

Use what the engine uploaded **for the drawn frame**: the player model's world
transform. Per-object matrices in the 4112-byte buffer are camera-relative.
The earlier search (`render: player transform at float 112/568`) found the
player's matrix about 0.3 m from the memory estimate. That 0.3 m is consistent
with the memory lead, or with a constant root-bone offset.

If the player's transform is identified reliably in each frame, then:
- matrix = the same upload's view-projection;
- camera = origin;
- player = that transform's translation.

All three come from one upload, so they are synchronised by construction, with
no memory timing at all. `anchor=render` already wires this path; what's
missing is reliable identification. Ideas:

- Track continuity: the player's transform in the next frame is within a few
  cm of the last one, after the camera's own motion (eye difference) is
  removed.
- Use the transform's rotation: its yaw should match the native facing angle,
  which is a much tighter key than position.
- Log the candidate slot and offset statistics for a minute of play, to see
  whether the player always sits at the same float offset in the buffer.

## Corrections to REMSTERED_ISSUES.md (written by the other session)

- Player and camera positions are not refreshed only every 500 ms:
  `readLivePositions` reads game memory directly on every call.
- The 10 m "garbage read" check it added compared live reads against the
  worker's snapshot. When sprinting, that rejected good reads, which was a
  likely cause of the "teleport about every second". Removed in v35.
- The camera smoothing it added (EMA in `readLivePositions` and a 0.5 blend at
  draw time) caused lag. Removed in v35. Its versions are kept in
  `work\dsr\other-session-1330\`.
- The underlying "game lag" was the `VirtualQuery` cost above, not timing in
  the game.

---

## SOLVED (build v65, 2026-10-20 01:24) - backup-20261020-0125

Mario tracks the character exactly while walking, sprinting, rolling and moving
the camera. How it works (`upload=on` in inject_mode.txt):

1. **Source of truth is the player's own draw data.** Each 2288-byte constant
   buffer upload for a player mesh has the view-projection at offset 0 and the
   mesh's bone palette from float 112 (byte 0x1C0), 12 floats (3x4) per slot.
   Matrix, palette and camera (origin; everything is camera-relative) come from
   one upload, so they are frame-synchronised by construction.
2. **Slot 0 is the Pelvis** in every body and leg mesh (parsed from the game's
   FLVER2 part files, 95 of 95 meshes in a 32-part sample; see
   `work/dsr/BONE_TABLE.md` and `tools/flver_tools.py`). Per geometry-pass
   chunk, the slot-0 matrix repeated in 2 or more uploads near the player is the
   pelvis. Chunks with no repeated slot-0 bone draw nothing; another chunk of
   the same frame does.
3. **Palette entries are skinning matrices** (pose times inverse bind). The hip
   joint is the pelvis bind position (0, 0.970, -0.054) (Master's translation in
   chr/c0000.flver) pushed through the matrix. Using the matrix's own
   translation swung with every hip twist.
4. **Feet offset:** horizontal, in the character's facing frame, measured while
   standing still; vertical, adapted slowly (about 1 s).
5. **Performance:** VirtualQuery costs about 6.7 ms inside DSR; render threads
   never call it (region cache plus worker-validated addresses).

Memory (WorldChrMan player position and the follow camera) is used only to
gate candidates to the player's vicinity, to calibrate, and for Mario's yaw.

Dead ends, kept so they are not retried:
- the memory camera with smoothing;
- eye pairing by facing (ambiguous when the camera only translates);
- the 2288 upload's own eye field (not the main camera);
- time and frame-count lag estimates;
- untilted "root" locks (several near-bind bones);
- most-repeated or centroid bone picks (legs).
