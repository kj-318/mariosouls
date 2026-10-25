# MarioSouls — Dark Souls Remastered port: status update

Hi — progress report on the Remastered work, current state, what's proven, what's
next, and where I think the risk is. Written before starting the geometry-pass
injection so there's a clear checkpoint to compare against.

---

## TL;DR

The first milestone's hard part — **getting a correct camera matrix and live
player state out of Remastered** — is done and verified against live gameplay.
Both are read passively; nothing is written to the game.

**Mario does not render yet.** No geometry-pass injection, no guest draw. What
exists today is an observation-only host that runs alongside the game and logs
what it sees.

Branch: `port/ds1remastered-observation` (unpushed, see "Repo hygiene" below).

---

## What's verified

### 1. View-projection matrix — captured and tracking

The camera matrix is not stored anywhere on the CPU side. It's computed by the
game and uploaded to GPU constant buffers. I hook `Map`/`Unmap` and read it.

| Item | Finding |
| --- | --- |
| Source | `Map` payload of the **4112-byte** dynamic constant buffer |
| Offset in buffer | **0** (first 16 floats) |
| Read point | **`Unmap`**, not `Map` |
| Context | **Deferred** contexts only — never the immediate one |

Verified across 500,000+ updates: rotation sweeps the full yaw circle, translation
moves, FOV responds to zoom. Structural invariants held throughout (`m03`/`m13`
exactly zero, row 4 mirroring row 3).

Three things this corrected, all of which I'd previously got wrong:

- **The buffers only exist on deferred contexts.** I was reading the immediate
  context and saw nothing useful.
- **Must read at `Unmap`.** The game writes the payload *after* `Map` returns, so
  a `Map`-time read gives all zeroes. My first dumps were empty for this reason.
- **`m00` is signed.** My validator required a positive `m00` and rejected valid
  captures.

A separate axis-aligned projection also circulates (1040-byte buffer, static
`m11 = 2.5386`); it's rejected. The 2288-byte buffer is a *different shader
variant* with its own FOV — mixing the two interleaved two different FOVs, so only
the 4112-byte buffer is used.

### 2. Health — static pointer chain, live

```
DarkSoulsRemastered.exe+01726B14 -> +0x100 -> +0x28 -> +0xE0 -> +0xFE0 -> +0x0
HP = base + 0x3E8,  MaxHP = base + 0x3EC
```

Verified: HP moved `453 → 401 → 594` across damage and healing; MaxHP matched the
character's real maximum. The root is module-relative and Remastered has no ASLR
(image base `0x140000000`), so it's a fixed address that survives restarts.

The `+0xE0` link is **null until a character is loaded** — resolution fails at the
title screen and succeeds in play. Expected; the host retries.

### 3. Stamina — separate structure, live

Stamina is **not** next to HP. It's in a different structure with its own root:

```
DarkSoulsRemastered.exe+01A31768 -> +0x28 -> +0x38 -> +0x180 -> +0x30
                                 -> +0x128 -> +0x50 -> value
```

Verified: drained `95 → 65 → 35 → 5` under sprinting, refilled
`5 → 16 → 27 → 37 → 49 → 59 → 71 → 81 → 92 → 95`, over repeated cycles.

**It's a deficit accumulator, not a clamped pool** — it goes negative when
overspent (observed to `-25`) then refills. The host clamps the reported value at
0 and derives max from the observed peak.

---

## Two corrections worth flagging

**I was wrong about stamina being at `base + 0x3F0`.** Reading there returned a
constant `49` that never moved during play while the real pool was 95. It *passed*
my sanity check, because 49 sat inside a loose plausible range. That's the real
lesson: a value that reads plausibly is not evidence it's correct. It only became
trustworthy once I watched it move under sprinting. Those offsets are now marked
`WRONG` in `native_types.h` so the mistake stays documented.

**Capability reporting was lying.** `buildCapabilities()` derived both Health and
Stamina from a single `player.valid` flag, which only reflected HP — so it
advertised a Stamina capability that had never worked. Each capability now depends
on its own observation.

---

## Stability: anti-cheat

Remastered has anti-cheat and eager hooking is detected.

| Approach | Result |
| --- | --- |
| Patch vtables during device creation | **White screen / freeze** at main menu |
| IAT patches at load only | Crashed before device creation |
| **Install all vtable hooks 15 s after startup, on a separate thread** | **Stable** |

The working approach is the third: IAT patches at load, vtable hooks installed on
a worker thread after a 15-second delay. Game reaches a playable main menu. This
is a workaround, not a principled solution — it's tuned to "past the initial scan"
and may be version-fragile.

Live testing used a disposable character only. Your saves are backed up.

---

## Crash I caused

One diagnostic run crashed the game on save load. My chain-walk guard validated the
*source* address of a pointer read but not the *value* it returned, so a
non-pointer field was followed into garbage. Both ends are now validated, plus a
plausible-user-pointer check. Mentioning it because it cost a debugging cycle and
is the kind of thing worth knowing before anyone else runs this.

---

## Next: geometry-pass injection

This is the remaining milestone. Plan: hook `OMSetRenderTargets` on all 12 contexts
(1 immediate + 11 deferred), inject the guest draw during the geometry pass while
G-buffer and scene depth are attached, so Mario is lit by the game's lighting pass
and correctly occluded by world geometry.

**Known risk:** this is the highest anti-cheat exposure so far. It patches far
more surface than anything I've done, on all contexts, during active rendering.
The 15-second delay may not be sufficient here. If it fails, the fallbacks are
XINPUT proxy / external injector, or signing the module — none of which I've tried.

Composition point is confirmed: injection during geometry pass, not back buffer
(the back buffer is never bound; presentation happens via copy/resolve). No MSAA
anywhere — all textures single-sampled.

---

## Repo hygiene — needs your call

- Branch `port/ds1remastered-observation` has **no upstream and is unpushed.**
- `origin` points at `TedCassirer/mariosouls` — I need to confirm whether this
  working copy is a fork before I push anything. If it *is* a fork, this branch
  and a PR should go there, not to the original repo.
- The host files are currently **untracked** (`git status` shows
  `runtime.cpp`, `camera_capture.*`, `native_types.h`, `capabilities.h`,
  `build.py`, etc. as new).
- Docs updated: `docs/REMASTERED_FINDINGS.md` §17 (matrix) and §18 (health/
  stamina), `CHANGELOG.md` `0.9.0-dev.4` and `0.9.0-dev.5`.
- `doctor` and `build --check-portability` both pass.

**No PR will be opened yet** — the mod does not run in Remastered. Opening one now
would misrepresent the state.

---

## Honest summary of what's real

| Claim | Status |
| --- | --- |
| View-projection capture | **Verified live**, 500k+ updates tracking camera |
| HP / MaxHP | **Verified live**, moves with damage and healing |
| Stamina | **Verified live**, drains and refills correctly |
| Camera + health + stamina resolving together | **Verified** |
| Game stability under hooks | **Verified** with delayed install |
| DX11 backend (`render_device.h`) | Compiles — **has never executed** |
| Geometry-pass injection | **Not implemented** |
| Mario rendered with world occlusion | **Not implemented** |
| Player position/rotation | **Not implemented** (deliberately — matrix-only milestone) |

The remaining gap is between "we can see the game" and "the game draws Mario".
That gap is the injection work, and it's where I'd expect the difficulty.

---

## Backup

Saves backed up to `Desktop\DS1 Backup Saves\Remastered-20261018-230524`
(hash-verified against source; original untouched). Game folder currently clean —
no proxy DLL present.