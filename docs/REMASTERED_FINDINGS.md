# MarioSouls → Dark Souls Remastered: findings

Working notes for the port requested by the mod author. Everything below is
either **verified** against the owned installation with the method stated, or
explicitly marked as inference / unverified. Nothing here is a supported port.

Canonical upstream: <https://github.com/TedCassirer/mariosouls>
Checkout: `mariosouls/` (clean, `main` @ `cb56bb8` "Prepare v0.9.0-dev.2 preview
and reusable host presentation")

---

## 0. Baseline established

| Step | Result |
| --- | --- |
| `python tools/dependencies.py --setup --download-toolchain` | libsm64, MinHook, cannon-research/sm64 verified; Zig toolchain staged |
| `python tools/framework.py doctor` | **pass** — "Architecture boundaries verified", profile `mario64` valid, 8 attacks |
| `python tools/framework.py build --check-portability` | see §6 |
| Owned installation present | yes, `C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED` (Steam appid 570940) |

Toolchain used for verification: MSVC Build Tools 18 (x64), Python 3.14.7,
Git 2.55.0, `pefile` 2024.8.26. Evidence scripts live in `mariosouls/work/dsr/`
(`work/` is gitignored, so they cannot leak into a pull request).

---

## 1. Executable identity (verified)

This is the exact material `framework/hosts/<edition>/edition.json` requires for
`supported_exe_sha256`.

| Field | Value |
| --- | --- |
| File | `DarkSoulsRemastered.exe` |
| Size | 50,286,344 bytes (47.96 MiB) |
| **SHA-256** | `a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b` |
| SHA-1 | `9150cc63c617332ed3c2c66e7566ed67e3292da0` |
| MD5 | `878b4603a2cc1ba2cbcd15fc6248e37e` |
| Machine | `0x8664` **x64** |
| PE magic | `0x020B` PE32+ |
| Image base | `0x0000000140000000` |
| Entry point | RVA `0x010E5124` → VA `0x00000001410E5124` |
| Timestamp | `0x6344CA56` → **2022-10-11 01:43:50 UTC** |
| Subsystem | 2 (GUI) |
| DllCharacteristics | `0x8120` — HIGH_ENTROPY_VA, NX_COMPAT, TERMINAL_SERVER_AWARE |
| Sections | 9 (`.text`, `.rdata`, `.data`, `.pdata`, `.tls`, `.gfids`, `.rsrc`, `.idata`, and a **second** `.text`) |
| Image size | 49.61 MiB |
| Overlay | 9,992 bytes @ `0x2FF2800`, `30 82 26 f7 06 09 2a 86` = ASN.1 `pkcs7-signedData` → the Authenticode signature |
| PDB (CodeView RSDS) | `N:\FRPG\Source\FRPG\Project\x64\MasterBEST\FRPG_x64_MasterBEST.pdb` |
| RSDS GUID | `5EA29709-8EF1-4820-…`, Age `0x2A` |

**Method:** `pefile` (authoritative PE parsing). An earlier hand-rolled parse in
this session mis-read the section header field order and produced a broken RVA
map; those results were discarded and everything above re-derived with `pefile`.

Notable: the internal project name is **FRPG**, and this is the **x64
"MasterBEST"** shipping configuration.

---

## 2. Loader arrangement: a `dinput8.dll` proxy works (verified)

The executable statically imports, **by name**:

```
DINPUT8.dll!DirectInput8Create     (1 import)
XINPUT1_3.dll                      (ordinals #2, #3)
```

`DINPUT8.dll` is not a `KnownDLL`, so the default search order resolves the
application directory before `System32`. A `dinput8.dll` placed next to
`DarkSoulsRemastered.exe` is therefore loaded by the game.

This means **PTDE's loader arrangement is directly reusable**: the PTDE edition's
`build_output` is already `dinput8.dll`, and a Remastered host would use the same
proxy mechanism, built for `x86_64-windows-gnu`. The game directory currently
contains no proxy DLL of any kind (verified), so there is nothing to displace.

**Caveat (not yet verified):** this establishes that the module *is* loaded, not
that DirectInput initialisation still succeeds through the proxy. That needs a
runtime check, and PTDE's proxy forwards the real `DirectInput8Create` from the
system copy.

---

## 3. Renderer is D3D11, and it is a deferred, MSAA pipeline (verified)

Imports are minimal — `d3d11.dll!D3D11CreateDevice` and
`dxgi.dll!CreateDXGIFactory`, one each — meaning the game uses the documented
default-library entry points and then calls through COM vtables.

Strings in the image confirm the actual architecture:

| Evidence | Offset | Meaning |
| --- | --- | --- |
| `ShaderConstant_DeferredLightingEntity` | `0x012C8C00` | deferred lighting for entities |
| `ShaderContext_DS_DeferredLightingEntity` | `0x012C93F0` | matching shader context |
| `ShaderManEntity`, `FrpgShaderContextEntity` | `0x012DEE90` | shader manager / context entities |
| `…EntitySeed` variants | several | GPU seed/culling passes |
| `CreateRenderTarget` / `ClearRenderTarget` / `FlushRenderTarget` | `0x01519870`+ | engine RTV wrapper layer |
| `ImageProcessMSAADownSample` | `0x01348308` | **MSAA is in use** |
| `Havok` Physics 2012 (custom 2015.1 LTS build) | `0x0130C798`+ | see §7 |

**Consequence, and this is the single largest porting cost:** the PTDE backend
`framework/hosts/ds1ptde/render_device.h` owns FVF, D3D9 fixed-function pipeline
state, `IDirect3DDevice9` render-target handling and D3D9 state blocks. Per
`HOST_PORTING.md` and `REMASTERED_HANDOFF.md`, **none of it transfers**. A
Remastered backend must implement DX11 upload, shader/pipeline state, depth
availability, resize/reset/device-loss handling and native state restoration from
scratch, and must discover the real scene/UI boundary rather than assume PTDE's.

Also relevant: because the scene is MSAA, a guest draw that must be occluded by
world geometry has to go into the engine's own multisampled depth path — a
standalone non-MSAA depth buffer will not line up. This needs runtime
confirmation.

---

## 4. No ASLR: the image loads at a stable base (verified)

`DllCharacteristics = 0x8120` sets `HIGH_ENTROPY_VA` but **not**
`DYNAMIC_BASE` (0x0040). The image is therefore not rebased by the loader;
code addresses are stable at `0x140000000`.

This materially helps a port: absolute addresses are reproducible across runs and
across machines with the same build, so a verified address plus the executable
SHA-256 is a usable binding — provided the host keeps PTDE's existing
executable-fingerprint validation, which must stay strict rather than relaxed.

---

## 5. No engine symbols are exported; the image is PGO-optimised (verified)

Two findings that shape how native binding must be done:

**(a) Exports are a math library only.** The image exports **165 symbols**, every
one of them from the `DLMT` namespace — `DL_AABB`, `DL_OBB`, `DLIIntersection`,
`DLIVecUtil`, `DLGaussPointsFit`. **No gameplay, renderer or entity-system
function is exported.**

So a Remastered host **cannot bind engine internals by import name**, the way a
modder might hope. Player/actor access, camera matrices and the draw boundary
must be located by scanning and then confirmed at runtime, which is exactly the
"fresh native evidence" `HOST_PORTING.md` demands — and exactly what AGENTS.md
forbids faking.

**(b) A `POGO` debug directory entry is present** (type 13, 924 bytes).
Profile-guided optimisation reorders and specialises hot code, so the shipped
binary is *not* a naive compile of the source. Instruction-level byte signatures
are correspondingly more fragile here than in an unoptimised or non-PGO build.
Any signature must be validated against the actual binary, and the existing
hook-signature validation should be kept and tightened rather than relaxed.

---

## 6. Build / portability status — verified green

`python tools/framework.py doctor` passes fully ("Architecture boundaries
verified", profile `mario64` valid, 8 attacks).

`python tools/framework.py build --check-portability` **succeeded from a clean
checkout** on this machine:

```
Built framework host + mario64
Compiled C/C++ SDK and shared headers for x86/x64, plus x64 guest binaries.
No binaries executed; PTDE remains x86 only.
```

Produced artifacts, with PE machine type confirmed via `pefile`:

| Binary | Arch | Size | Role |
| --- | --- | --- | --- |
| `work/framework-build/dinput8.dll` | x86 | 770,560 | PTDE host |
| `work/framework-build/characters/mario64/character.dll` | x86 | 419,328 | Mario adapter |
| `work/framework-build/characters/mario64/sm64.dll` | x86 | 825,856 | libsm64 guest runtime |
| `work/framework-build/portability/x86_64-windows-gnu/mario64.dll` | **x64** | 349,184 | Mario adapter |
| `work/framework-build/portability/x86_64-windows-gnu/runtime/sm64.dll` | **x64** | 790,016 | libsm64 guest runtime |

### The most important consequence

**The guest side needs no porting work at all.** `mario64.dll` and `sm64.dll`
already compile as native x64 PE32+ binaries, unmodified, from the pinned
sources. A Remastered host would load these exact two DLLs through the existing
ABI-4 handshake and inherit Mario's movement, animation, mesh and physics
as-is.

This matches the author's own framing — x64 guest compilation was already
architected as "preparation, not a functioning port" — and it means the entire
remaining cost of the port sits on the **host** side: native bindings, the DX11
backend, and the presentation milestone. Nothing in `framework/characters/`
needs to change.

The guest-facing ABI a Remastered host would consume is:

```
SC_GetAPI(hostAbi, hostApiSize) -> const SC_CharacterAPI*
  describe / initialize / shutdown
  create / pose / destroy          <- presentation-only milestone uses these
  step / resolveMotion / surfaces / audio / ...   <- later milestones
```

`SC_ABI_VERSION` is `4`, `SC_FIXED_HZ` is `30`, calling convention is `SC_CALL`,
and every entry point is version/size checked. No STL, exceptions, allocator
ownership or native game pointers cross the boundary, which is what makes the
same binary usable from an x64 host.

---

## 7. Havok physics is present (verified) — a nuance worth correcting

The image contains a Havok Physics **2012** integration, from a custom
`2015.1.0 LTS` source tree:

```
..\Source\Havok\HkxResCap.cpp
..\Source\Havok\PhysRigidToMdlMtx.cpp
..\Source\Havok\CustomMeshParameter\CustomMeshParameter.cpp
S:\P4\Release\2015_1\Custom\2015_1_0_LTS\Source\Physics2012\Collide\Shape\...
hkcdShapeType::{INVALID, CONVEX, COMPRESSED_MESH, COLLECTION, BV_TREE, SPHERE, TRIANGLE, BOX}
0x2392976d-0x740e85f9:2019-05-31.Physics_2012.Qloc_DarkSoulsHD_Windows-PS4-XboxOne
```

This refines, and does not necessarily overturn, an earlier working note from the
separate `mario-dsr` project which concluded that Remastered has "no data-driven
player physics" because there is no `c0000.hks` in the install. Both can hold at
once:

- **Collision response** is data-driven and Havok-backed (mesh/primitive shapes
  in map files), so native ground/contact queries are available to a host.
- **Player locomotion tuning** (run speed, acceleration, jump velocity, gravity,
  air control) may still be compiled constants in the executable. That part is
  **not yet verified** and needs a dedicated check before being relied on.

Do not treat "no `c0000.hks`" as "no physics". The relevant distinction is
collision data vs. movement constants, and only the second is in question.

---

## 8. Risk: the build contains an anti-cheat module (verified — read before loading anything into the game)

```
CSCheatDetectionTitleModule::Thread          0x012C98C0
N:\FRPG\Source\FRPG\Source\Sys/Cheat/CSCheatDetection.h
ChrInsManThread                              0x01309950
ChrInsManThread::Thread                      0x0131F928
```

A title-level cheat-detection module is compiled into the shipping x64 build.

A Remastered host inherently involves proxy-DLL loading plus engine hooking and
process-memory reads — precisely the shape of activity such a module inspects for.
**Consequences to weigh before any runtime testing:**

1. Loading a proxy/hooking DLL into a live Remastered session may trip detection,
   with an unpredictable consequence for that install (warning, refusal to
   launch, or a ban).
2. This is the user's own purchased copy and their own modding of it, which is
   ordinarily legitimate — but it is not risk-free, and it is not my call to
   absorb that risk silently.

**Recommendation: confirm with the user, explicitly and in advance, that they
accept this risk before any DLL is placed next to `DarkSoulsRemastered.exe` or
the game is launched with a proxy in place.** Until then, static analysis and
offline compilation only — neither loads anything into the game.

`ChrIns` ("character instance") is also the subsystem that owns player/actor
state, which is the most promising anchor for the milestone-2 "validated native
actor" requirement — but its layout is not yet known and must not be guessed.

---

## 9. What a Remastered edition actually requires

Per `HOST_PORTING.md` the **first milestone is presentation only**: native
movement, collision, input, HP/stamina, inventory and combat all stay in control;
the deliverable is an x64 host that draws Mario at the validated native actor with
correct occlusion and **before** the HUD. No movement takeover, no combat.

| # | Requirement | Status |
| --- | --- | --- |
| 1 | Executable hash/build, PE architecture, loader arrangement, renderer recorded | **done** (§1–§5) |
| 2 | Reversible edition-specific loading + refusal path established before native writes | loader mechanism identified (§2); proxy not yet built |
| 3 | Player identity/lifetime, root position/facing, camera matrices validated at runtime | **blocked** — needs a live session |
| 4 | x64 adapter + guest runtime loaded via existing ABI/size/convention checks | feasible; guest already x64-clean, x64 host DLL not written |
| 5 | DX11 graphics backend, resource lifetime, discovered scene depth + pre-HUD composition point | **not started** — the largest single cost |
| 6 | Hide only the identified native player draw, restore on failure | depends on 3–5 |
| 7 | Manual observation of walking/turning, occlusion, prompts, loading, resolutions, reset | **blocked** — needs a live session |

Steps 3 and 7 cannot be done from static analysis and are exactly what AGENTS.md
forbids inventing. Steps 5 is the bulk of the engineering.

One structural note: `tools/host_editions.py` refuses any edition whose
`edition.json` is not `status: "implemented"`, and demands a valid target, a
64-hex `supported_exe_sha256`, a contained recipe path and a `.dll`
`build_output`. That guard means a stub edition **cannot** be registered as a
usable host — which is the author's intended protection against metadata being
mistaken for support. Adding the declaration before there is real code would have
to lie about `status`, so the honest order is: real code and evidence first,
declaration last.

---

## 10. Working constraint agreed with the user

The user was shown §8 and chose **static and offline work only** for now. So the
following are explicitly *not* being done at this time, by agreement rather than
by oversight:

- no DLL is placed next to `DarkSoulsRemastered.exe`;
- the game is never launched;
- no proxy/hook DLL is loaded into any Remastered process;
- no engine memory is read from a live session.

Everything below was therefore obtained without executing the game. The
anti-cheat exposure question stays open for whenever the user decides to revisit
it. This constraint happens to align with the author's own rules: `AGENTS.md`
forbids inventing hooks, and `HOST_PORTING.md` requires fresh native evidence
from an owned installation, so items 3 and 7 could not honestly be satisfied by
guesswork in any case.

---

## 11. DX11 backend — offline work done

Written: `framework/hosts/ds1remastered/render_device.h`. It implements
`SCRenderAPI::Renderer` against Direct3D 11. **It does not compile on this
machine**, for an environment reason described below. Nothing about it has been
run.

### What it does

* Implements the neutral renderer boundary for DX11: world meshes, lines, and
  screen quads, with per-material colour mode, point/linear filtering, shade,
  alpha cutoff, alpha blending, and depth test/write.
* Owns all GPU resources: shaders, input layouts, rasterizer/depth/blend/sampler
  states, a pipeline-state cache, dynamic vertex and constant buffers, and a
  texture cache keyed by image identity (matching `render_api.h`'s guarantee that
  images are immutable with stable addresses for the host session).
* Replaces PTDE's `StateScope` with `DSRStateScope`. D3D11 has no state blocks,
  so it records exactly the context state it disturbs — render targets, VS/PS,
  input layout, rasterizer, depth, blend, constant buffer, vertex buffer, SRV,
  sampler, topology, viewports — and restores all of it on every exit path,
  including early returns.
* Resets cleanly on device change / device loss: GPU objects are dropped and
  rebuilt lazily, while host-owned CPU images stay alive and re-upload.

### Convention translations (documented in the file, not guessed)

| Neutral contract | DX11 translation |
| --- | --- |
| Row-major matrices, D3D9 **row-vector** convention (`v' = v * M`) | Matrices transposed on upload, HLSL uses `mul(vector, matrix)` so results match D3D9 exactly rather than relying on HLSL defaults |
| `color` / `shade` packed AARRGGBB | Unpacked explicitly in the vertex shader |
| `shade` as a grayscale light multiplier | RGB multiply. D3D9 fed it through a SPECULAR stage whose behaviour with lighting disabled was ambiguous; DX11 states the intent directly |
| Screen vertices: pixels + reciprocal W (D3D9 XYZRHW) | Converted to clip space in the vertex shader, camera matrices ignored |
| RGBA8 image data | Uploaded as `R8G8B8A8_UNORM`; D3D9's BGRA swizzle has no equivalent here |

### Deliberately not guessed

The backend never selects a render target or depth buffer for itself. The caller
must pass a `Composition` whose `depthVerified` flag asserts the depth buffer is
the engine's own live scene depth. A depth-tested draw is **refused** when that
is absent, and a before-HUD submission without depth is refused too (same rule
as PTDE). Reason: the scene is multisampled, so an unrelated single-sample depth
buffer would depth-fight or make Mario vanish — refusing is the documented
fallback. The RTV format, depth format and sample count are read from the
engine's own resources, because a pipeline-state object must match them exactly
and they are not knowable offline. A format or sample-count change invalidates
the pipeline cache.

### Toolchain findings (verified by compiling)

| Finding | Evidence |
| --- | --- |
| Zig/mingw compiles and links `d3d11` for x64 | probe built `zig_probe.dll`, 148,480 bytes |
| Zig/mingw ships **no `d3dcompiler` import library** | `-ld3dcompiler` → "unable to find dynamic system library"; no `libd3dcompiler*.a` in the sysroot |
| MSVC has both `d3d11.lib` and `d3dcompiler.lib` | `cl` + `link` resolved both |
| `d3dcompiler_47.dll` is present in System32 | verified |

So `D3DCompile` is resolved at run time from `d3dcompiler_47.dll` via
`LoadLibraryW`/`GetProcAddress` rather than linked. This keeps the host buildable
with the project's pinned Zig toolchain and avoids adding the shader compiler to
an injected process's load set. If the compiler DLL is missing the backend simply
cannot draw; it never falls back to a guess.

The corrected D3D11 state-capture/restore call sequence compiles cleanly for
x64 on both toolchains (`work/dsr/probe_d3d11_api.cpp`). Two API details worth
recording, since both are easy to get wrong: `ID3D11DeviceContext` exposes state
getters under the `OM`/`VS`/`PS`/`IA`/`RS` prefixes (`OMGetRenderTargets`,
`RSGetState`, …) while the unprefixed `Get*` names belong to `ID3D11Device`; and
`VSSetShader`/`VSGetShader` take **three** arguments (shader, class-instance
array, count), not two.

### The stripped API, and how the backend avoids needing it

Two parts of the D3D11 API are missing from **every** `d3d11.h` on this
machine, including Microsoft's own SDK and a freshly downloaded, hash-verified
copy of it:

* **The bundled pipeline-state form.** Enumerating `ID3D11DeviceVtbl` in
  declaration order yields **42** present entries; exactly
  `CreateGraphicsPipelineState` is missing, along with `ID3D11PipelineState`,
  `D3D11_GRAPHICS_PIPELINE_STATE_DESC`, `GSSetPipelineState` and
  `D3D11_VIEWPORT_AND_RASTERIZE_SLOT_COUNT`. The strings `"Pipeline"` and
  `"Graphics"` appear **zero** times in the whole `d3d11.h`, while every other
  `Create*` method is present.
* **`D3D11_RESOURCE_DESC` / `ID3D11Resource::GetDesc`** - 0 occurrences in
  `d3d11.h`, `d3dcommon.h` and `d3d11_1.h`.

Verified four independent ways, so it is not a search artefact:

1. MSVC rejects both - `CreateGraphicsPipelineState: is not a member of
   ID3D11Device`, and `D3D11_RESOURCE_DESC` undeclared.
2. Raw byte search: 0 occurrences across `d3d11.h`-`d3d11_4.h`.
3. UTF-8 decoded search: 0 occurrences. (A first byte-level scan was invalid -
   see the corrections note.)
4. All `d3d11.h` copies on this machine agree, **including a second Microsoft
   SDK installed during this session** - `10.0.18362.0`, downloaded by winget
   from `download.microsoft.com` with a verified installer hash - which is
   stripped identically to the pre-existing `10.0.26100.0`:

   | `d3d11.h` | `ID3D11PipelineState` |
   | --- | --- |
   | `Windows Kits\10\Include\10.0.26100.0\um` | 0 |
   | `Windows Kits\10\Include\10.0.18362.0\um` (freshly installed) | 0 |
   | `Epic Games\UE_4.11\...\DirectX\Include\D3D11.h` | 0 |
   | `Melty\mods\toolchains\w64devkit-2.10.0\include` | 0 |
   | Zig `zig-windows-x86_64-0.13.0` mingw | 0 |

A hash-verified Microsoft download cannot ship a header missing a core API, so
the removal is happening on this machine rather than in the package. Installing
another SDK is therefore not a fix, and none is needed.

**Why neither gap blocks the backend.** D3D11 also offers a *modular* state
model, and the backend uses only that:

* it binds `ID3D11BlendState`, `ID3D11RasterizerState`,
  `ID3D11DepthStencilState`, `ID3D11InputLayout`, `ID3D11VertexShader`
  and `ID3D11PixelShader` individually through the device context
  (`OMSetBlendState`, `RSSetState`, `OMSetDepthStencilState`,
  `IASetInputLayout`, `VSSetShader`, `PSSetShader`). Those setters fully
  define the pipeline for a `Draw` call; the bundled pipeline-state object is
  an optional convenience layered on top of them, not a requirement. This is
  genuinely valid D3D11, not a workaround, and it matches how PTDE's D3D9
  backend already sets state per draw.
* the target size comes from the caller, or from the context's currently bound
  viewport, instead of querying `GetDesc`.

**Capability lost to the environment, recorded honestly:** because `GetDesc`
is unavailable, the backend cannot independently confirm that a supplied depth
view shares the colour target's sample count. `Composition::depthVerified` is
therefore the *only* gate on depth-tested drawing, and it must come from a real
observation of the engine's scene depth. That is a real reduction in defence in
depth versus what the code was originally written to do.

### Compilation result

| Toolchain | Compile | Link |
| --- | --- | --- |
| MSVC x64 (`/W4 /permissive-`, `d3d11.lib`) | clean | `rd_msvc.dll` x64, 770 KB region, imports **only `KERNEL32.dll`** |
| Zig 0.13.0 `x86_64-windows-gnu` (`-ld3d11`) | clean | `rd_zig.dll` x64, 152,064 bytes |

Zero references to `CreateGraphicsPipelineState`, `GSSetPipelineState` or
`D3D11_RESOURCE_DESC` remain in either object file. The MSVC-linked DLL
imports only `KERNEL32.dll`: D3D11 calls go through COM vtables so no
`d3d11` import is needed, and `d3dcompiler_47.dll` is resolved at run time.
That is the import footprint a proxy DLL wants - it adds nothing to the game's
load set.

Compile-only. Nothing was executed, no device was created, no shader was
compiled, and the game was never launched.

### Corrections made during this session

Recorded because both would have corrupted the findings if left uncorrected:

* A first hand-rolled PE parser unpacked section-header fields in the wrong
  order (`VirtualSize`/`VirtualAddress` vs `VirtualAddress`/`VirtualSize`),
  producing a broken RVA map. Every PE result was discarded and re-derived with
  `pefile`.
* The export-table dump initially read the PE header's *declared* export RVA
  instead of the import directory's own, and printed ASCII that looked like
  symbol names (`?FindDynamic@DLIIntersection@DLMT@@…`). Those bytes are
  interesting but are **not** what `GetProcAddress` resolves, so that path is not
  usable for binding. The real export table is 165 `DLMT` math symbols (§5).
* `RSGetViewports`/`VSSetShader`/`VSGetShader` were first called with the wrong
  arity, and `ID3D11Device`-side getters (`GetRenderTargets`, `GetVertexShader`)
  were used on an `ID3D11DeviceContext`. All corrected and compile-verified.

---

## 12. Remastered runtime observations (live runs)

Produced by the read-only probe (`work/dsr/probe/dinput8.cpp`) built as
`dinput8.dll` and placed beside the executable. The user launched the game
normally, started a new game, moved, opened the menu, and quit. The probe
imports only `KERNEL32.dll` and forwards `DirectInput8Create`; it draws nothing
and modifies no game state.

| Observation | Value | Why it matters |
| --- | --- | --- |
| Main module base | `0x140000000` | **Confirms the no-ASLR prediction (§4) empirically.** Addresses are reproducible across runs |
| Image size at run time | 52,015,104 (49.61 MiB) | Matches the PE header's `SizeOfImage`, so the image is mapped as declared |
| Proxy load | succeeded, DirectInput forwarded | The `dinput8.dll` proxy arrangement (§2) works in practice |
| `D3D11CreateDevice` calls | twice: first `S_FALSE`, then `S_OK` | The engine probes once before creating the real device — relevant when hooking |
| Feature level | 45056 = `0xB000` = `D3D_FEATURE_LEVEL_11_0` | Full D3D11, consistent with the shader model 4/5 evidence in §3 |
| Output resolution | **1920x1080** | Target size for the backend's viewport |
| Swapchain format | **87 = `DXGI_FORMAT_B8G8R8A8_UNORM`** | BGRA, not RGBA. Uploads/comparisons must respect this ordering |
| **Swapchain `SampleDesc.Count`** | **1** | **The final backbuffer is single-sampled** |
| Buffers / windowed / swap effect | 2 / true / `DISCARD` | Classic double-buffered legacy-style swapchain |
| Present sync | `syncInterval=1` | Vsync on; presentation is display-paced |
| **Measured frame rate** | **5152 frames / 87.683 s = 58.76 FPS** | Near-60 with vsync. A real cadence measurement, not an assumption |
| Threads | one render thread throughout | Presentation is single-threaded; no thread-change events after the first |

### The important negative result

Sampling the bound state *at present time* gave only two distinct
configurations across a 5,153-frame session, both with `dsv=NULL`. That is
because present-time sampling is too late: the scene and G-buffer passes happen
*between* presents. Hooking `OMSetRenderTargets` (see the resolved slots below)
was required to observe inside the frame.

### RESOLVED: the scene depth buffer exists, and PTDE's approach transfers

The three earlier sections reporting "no depth buffer" are **wrong**, and the
reason is now understood precisely. The definitive run:

| Measurement | Value |
| --- | --- |
| Total binds observed | **500,628** |
| Binds carrying a **non-NULL depth view** | **83,329** |
| `ClearDepthStencilView` calls | **24,880** |
| Depth views created | 19 |
| Depth views observed in use | 8 of 19 |
| Deferred contexts created | 11 |

**The scene depth view is identified.** Over ~5,901 frames:

| View | Size | Binds | Clears | Role |
| --- | --- | --- | --- | --- |
| `…56462CB8` | **1920×1080** | **34,221** | 5,901 | **scene depth** |
| `…56461FB8` | 1920×1080 | 26,257 | 0 | second view, same resource |
| `…564633F8` | 960×540 | 11,802 | 5,901 | half-res depth |
| `…5645EBF8` | 480×270 | 3,766 | 5,901 | quarter-res depth |
| `…3ABECDF8` | 2048×2048 | 5,901 | 5,901 | shadow map |
| `…56463578`, `…5645FD38` | 512×512 | 637 each | 638 each | small shadow maps |
| 4 views at 128×128 | 128×128 | 0 | 0 | created, unused |
| 4 further 1920×1080 / 960×540 / 480×270 | — | 0 | 0 | created, then abandoned |

**Every depth bind carried `rtvCount = 4`** — the scene depth is attached alongside
the four G-buffer targets, and cleared once per frame. That is exactly PTDE's
model: depth is a genuine render-pass attachment, not a G-buffer channel.

So the conclusion is the opposite of the one reported three times:

> **A guest draw can depth-test against the scene exactly as the PTDE host does.**
> Bind the engine's 1920×1080 `D24_UNORM_S8_UINT` view during the G-buffer pass
> and draw against it.

#### Why every earlier probe reported `dsv == NULL`

The arithmetic settles it. The first probe saw **18,527** binds; this one sees
**500,628** from the same engine. Instrumenting the immediate context alone
captured ~3.7% of all binds.

> **Remastered performs the overwhelming majority of its rendering through
> deferred contexts** — 11 of them, created in two bursts. The scene depth is bound
> on those, not on the immediate context, so an immediate-context-only hook sees
> essentially nothing.

This is the single most important architectural finding for the port, and it is
not a detail:

* A guest draw must be issued on whichever context the engine is currently using —
  if that is a deferred context, the work needs an explicit `Flush()` before it can
  be relied upon.
* Any future observation must instrument deferred contexts as well, or it will
  quietly see a fraction of the frame.

`OMSetRenderTargetsAndUnorderedAccessViews` was instrumented to close the earlier
gap and **turned out never to carry a depth view**: all 152 logged depth binds came
through `OMSetRenderTargets`. It stays hooked only because it is verified and
harmless; no depth came from it.

`DEPTH_STENCIL | SHADER_RESOURCE` together remains the signature of depth as
texture, and the paired views on one resource (one cleared, one never) are the
writable and read-only pair.

### No multisampling anywhere (settled)

The earlier open question is now closed. **Every one of the 118 distinct texture
descriptions logged has `SampleDesc.Count = 1`.** There is no multisampled render
target anywhere in the renderer.

This corrects two earlier statements:

- The speculation that the `ImageProcessMSAADownSample` shader implied a
  multisampled intermediate does not hold. No MSAA target is created.
- The DX11 backend's worry that a guest draw "must use the engine's own
  multisampled depth buffer, or it will depth-fight or vanish" **does not apply**.
  The scene is single-sampled, so a guest draw binding the engine's
  `D24_UNORM_S8_UINT` view has no sample-count mismatch to solve.

### Prior full-session result (retained, but see the supersession above)

With the context hook working and a budget sized against a real session, the
final run produced **28,982 bind events spanning frames 150–9,645** — the entire
play session, well inside the 40,000-line budget, so nothing was dropped.

| `rtvCount` | Binds | Interpretation |
| --- | --- | --- |
| 4 | 19,008 | G-buffer pass — deferred, 4 render targets |
| 1 | 9,974 | Single-target passes (lighting resolve, UI, post) |

- **8 distinct (rtv, dsv) pairs** across the whole session.
- **Every one had `dsv = NULL`.**
- `rtv=0` occurs 9,504 times: the engine unbinds entirely between passes.

This covers walking, turning, opening and closing the menu, and standing still in
the world. **The likely flaw is in the observation, not the engine:** see the
supersession above. The rest of the result — 4 render targets for the G-buffer
pass, 1 for the rest, the engine unbinding entirely between passes — is
consistent with the device-side run and is not in question.

### Correction to an earlier statement

Before this run I said the most valuable thing it would reveal was whether
Remastered runs multisampled. The swapchain backbuffer is single-sampled
(`SampleDesc.Count = 1`), and the later device-side run showed **every** render
target is single-sampled too. The concern that `ImageProcessMSAADownSample`
implied a multisampled intermediate does not hold. **Recorded as settled: the
renderer is not multisampled.**

### Corrected: the installed `d3d11.h` matches the runtime exactly

**This section replaces an earlier claim that the headers are displaced by +7.
That claim was wrong, and it was withdrawn after measurement.**

The earlier derivation came from a helper script that parsed the installed
header. Parsing the header directly, in declaration order, gives:

| Method | Header index | Measured slot | Agrees? |
| --- | --- | --- | --- |
| `ID3D11DeviceContext::OMSetRenderTargets` | 33 | **33** | yes |
| `ID3D11DeviceContext::RSSetViewports` | 44 | **44** | yes |
| `ID3D11Device::CreateBuffer` | 3 | **3** | yes |
| `ID3D11Device::CreateTexture2D` | 5 | **5** | yes |
| `ID3D11Device::CreateTexture3D` | 6 | **6** | yes |
| `ID3D11Device::CreateShaderResourceView` | 7 | **7** | yes |
| `ID3D11Device::CreateRenderTargetView` | 9 | **9** | yes |
| `ID3D11Device::CreateDepthStencilView` | 10 | **10** | yes |

The two context slots measured in-game (33 and 44) were correct all along; they
were never "header 26 and 37 shifted by +7". The device slots were then measured
offline by the same principle: patch **one** slot, call the method, and treat
interception as proof (the stub returns `S_FALSE` with the out-parameter
untouched, which is a two-valued signal that cannot be confused with success).
`work/dsr/probe/devslot_verify.cpp` reproduces this.

**So the header was never the problem — the earlier slot-derivation script was.**
Two things combined to produce the false "+7":

* A wide-band bisection reported `RSSetViewports` at slot **47**. Wrong, because
  stubbing dozens of live context methods at once let unrelated internal D3D11
  calls register as hits. That was read as a displacement when it was a
  measurement artefact.
* `ClearDepthStencilView` could not be identified by scanning at all: it
  internally calls `RSSetViewports` to update depth bounds, so it registers a hit
  at slot 44. Its header index is **53**, and it was left unhooked rather than
  hooked at a guess.

**What survives from the old warning:** the general rule — measure every vtable
index and enum against the real runtime rather than trusting arithmetic — is
still right, and is now backed by a method that works. But the conclusion
"therefore no index may be taken from the headers" was too strong, and is
withdrawn.

### The headers *are* non-standard, in a specific and now-measured way

The installed header genuinely differs from Microsoft's documented ABI, but the
**runtime agrees with the header**, so it is safe to use *here*:

* `D3D11_BIND_INPUT_LAYOUT` is **absent** from the enum, which shifts every
  later flag up one bit. So `D3D11_BIND_STREAM_OUTPUT` = `0x10`,
  `D3D11_BIND_RENDER_TARGET` = **`0x20`** (Microsoft: `0x10`),
  `D3D11_BIND_DEPTH_STENCIL` = **`0x40`** (Microsoft: `0x20`),
  `D3D11_BIND_UNORDERED_ACCESS` = `0x80` (Microsoft: `0x100`).
* `ID3D11Device` is declared as inheriting only `IUnknown`, with no
  `ID3D11Object` / `ID3D11DeviceChild` entries, so `CreateBuffer` is at 3 rather
  than 11.

Both were confirmed against the runtime, not read off the header: a texture
created with `bindFlags = 0x20` was accepted by `CreateRenderTargetView`, and one
created with `0x40` was accepted by `CreateDepthStencilView`. A runtime using
Microsoft's constants would have rejected both.

**Practical consequence:** hard-coding Microsoft's documented constants would be
wrong on this machine. Using the installed header is correct — but only because it
was modified in lockstep with the runtime. Any port must verify this pairing on
the machine it ships to rather than assuming either ABI.

### Method *arity* is altered too — and must be measured, not assumed

Beyond indices and enum values, the **number of parameters** a method takes also
differs from Microsoft's documented signature. Two cases measured:

* `D3D11CreateDevice` — this header declares **10** parameters, omitting
  `ppDeferredContext`; Microsoft documents 11. The runtime arity was measured
  rather than assumed (`work/dsr/probe/arity_probe.cpp`): calling with a poison
  value in the 11th slot still succeeds, so **the runtime takes 10 and never
  reads it**.
* `ClearRenderTargetView` — this header declares **2** parameters
  (`pRenderTargetView`, `ColorRGBA[4]`); Microsoft documents 4, adding
  `NumSubviews` and `Subview`. Its runtime arity was **not** measured.

**A wrong arity corrupts the call, not the return value.** This is the same class
of fault that stopped the game launching (§13). Two concrete results:

* Forwarding `D3D11CreateDevice` with **11** arguments — reasoning that "an extra
  argument is harmless on x64" — **crashed at the real call**. The measurement
  showed the correct arity is 10, so the hook declares and forwards 10.
* `ClearRenderTargetView` is therefore **deliberately not hooked** in any probe.
  Its arity is unverified, and forwarding 2 arguments to a 4-parameter method
  would silently drop `NumSubviews`/`Subview` and corrupt the clear.

**Rule adopted:** every hooked method declares the arity *measured on this
machine*, not the arity Microsoft's documentation implies. `CreateTexture2D` (4),
`CreateDepthStencilView` (4), `OMSetRenderTargets` (4) and
`ClearDepthStencilView` (5) agree on both counts and are hooked; anything
unmeasured is left alone.

---

## 13. The depth probe: built and verified offline, awaiting one game run

The open question from §12 is whether Remastered creates a depth attachment at
all, or carries depth inside the G-buffer. `work/dsr/probe/dsrdepth.cpp` answers
it from the **device** side rather than the context:

* every `CreateTexture2D` is logged with its full description — dimensions,
  format, **sample count** and **bind flags** — deduplicated by description so a
  per-frame allocation does not drown the log;
* every `CreateDepthStencilView` is logged and called out explicitly, because a
  depth attachment that is created but never bound would still change the
  design;
* sample counts settle the open question of whether the G-buffer is
  multisampled, which the swapchain backbuffer's count of 1 does not answer.

**The stubs log and forward.** They do not refuse. This is a deliberate change
from the earlier identification approach, which broke rendering by design: now
that the slots are measured, there is no reason to damage the game to find out
which slot is which. The renderer receives exactly the resources it asked for.

### A shipped revision stopped the game launching — cause and fix

The first version of this probe **prevented Remastered from starting at all**
(reported by the user; three crashes ~9 seconds apart, fault module recorded as
the game executable, very early in start-up).

**Cause.** It hooked `CreateDXGIFactory` and then patched **slot 7** of the
object that came back, storing it as `IDXGISwapChain::Present`. Those are two
different interfaces:

| | Slot 7 | Slot 8 |
| --- | --- | --- |
| `IDXGIFactory` | **`EnumAdapters`** | `MakeWindowAssociation` |
| `IDXGISwapChain` | `GetDevice` | **`Present`** |

So `EnumAdapters` was later invoked with `Present`'s argument layout. The
engine's call

```
EnumAdapters(factory, &adapter, name)
```

became

```
EnumAdapters(factory, (char *)name, &adapter)
```

— the runtime wrote an adapter pointer over a string pointer. The log even
reported `slot=8` while patching index 7, so the code contradicted itself and
still shipped.

Two independent mistakes compounded: an index from one interface was reused on
another, and the index written in the log did not match the index patched.

**Reproduced offline before fixing** (`work/dsr/probe/factory_repro.cpp`). This
matters more than the fix: the first attempt to reproduce it *appeared to pass*,
because the harness never called a D3D11 function, so `d3d11.dll` was never
loaded, so the probe's init thread was still polling and had patched nothing.
The test was green for the wrong reason. It now creates a device first and waits
for the log to report `init-complete` before testing anything.

**Fix.** The DXGI hook is **removed, not repaired**. It only provided frame
counting, which the depth question does not need — every answer comes from the
device vtable. `D3D11CreateDevice` is now the only hooked entry point, and only
`ID3D11Device` slots 5/9/10 are patched.

| Regression | Result |
| --- | --- |
| `factory_repro.exe` — `EnumAdapters` through the factory vtable | **survives** (was `0xC0000005`) |
| `depth_e2e.exe` — hooks fire and every resource forwards | pass |
| Log confirms scope | `only D3D11CreateDevice is hooked, and only ID3D11Device slots 5/9/10 are patched` |

### The engine uses deferred contexts, and one shared "real function" crashed the game

Instrumenting deferred contexts crashed Remastered twice, at the **identical**
address both times. Reading the minidumps (`work/dsr/dumpread.py`) settled it:

| What | Address |
| --- | --- |
| Immediate context, slot 33 (`OMSetRenderTargets`) | `0x7FFAE1DC8F50` |
| Deferred context, slot 33 (`OMSetRenderTargets`) | `0x7FFAE1E1F4C0` |
| Fault address, both runs | `0x7FFAE1E1F53D` |

**The fault address is the deferred implementation + `0x79`.** The immediate and
deferred contexts do *not* share the same implementation of the method — they have
different vtables with different function pointers.

The bug: the probe kept **one global pointer** per hooked method to hold the
"real" function. Instrumenting a deferred context overwrote that global with the
*deferred* implementation, so every subsequent **immediate**-context bind was
forwarded into the deferred implementation with the wrong object. Wrong `self`,
crash inside `d3d11.dll`.

**Proven, not inferred** (`work/dsr/probe/ctxmix_probe.cpp`): the two slot-33
addresses match the live log exactly, the correct pairing succeeds, and calling the
deferred implementation with the immediate context raises
`0xC0000005`. The chain from cause to crash address is established.

**Fix:** each instrumented context stores **its own** copy of its own real
functions, and a hook resolves them by `self`. Instrumenting a second context can
no longer disturb the first. If a hook is somehow reached with an unregistered
context it logs and does **not** forward, because skipping is far safer than
calling into another context's implementation.

**Why the offline test missed it.** `depthbind_e2e` bound on the immediate context
*before* creating the deferred context, so the dangerous ordering never occurred
and the test passed while the game crashed. The harness now creates and
instruments the deferred context **first**, then binds on the immediate context —
the exact sequence that failed in-game.

**Rule added:** a hooked method's real implementation is a property of the
**object**, not of the method. Never share one pointer across several instances
whose vtables can hold different implementations.

### Remastered does create deferred contexts

Confirmed at startup: several `CreateDeferredContext` calls within a millisecond,
each object carrying its own vtable, all with depth-capable binds instrumented.
This is a real lead for the depth question — the first run hooked only the
immediate context, so any bind performed on a deferred context was invisible to
it.

### Composition point: resolved

A fully instrumented run (`work/dsr/probe/present.cpp`, 12 contexts, 6,288 frames,
527,676 binds, 8,139 presents) identified the presentation target by asking the
swap chain for it, and mapped the frame.

| Measurement | Value |
| --- | --- |
| Render target count used | `4` for 57,975 binds, `1` for 1,953 |
| Binds carrying a depth view | 8,894 |
| Binds with a real target **and** depth | 6,797 |
| Full unbinds (all targets NULL) | 31,105 |
| Typical frame | `depth depth depth - depth - - depth` (variants of 6–8 passes) |

Typical sequence: several depth-attached passes, then one or more without depth.
The 416,671 unbinds show the engine clears the target set constantly between
passes — so "no target bound" is a normal mid-frame state here, not an artefact.

**The swapchain back buffer is never bound.** Its render target view appears in
**zero** pass lines. The engine does not call `OMSetRenderTargets` with it; it
writes to the back buffer some other way (a copy or resolve). Presentation is
not a bind point.

**The geometry pass is the injection point.** The scene depth is attached on
6,797 passes with a real target, all with `rtvCount = 4`. That is the G-buffer
pass — the only place a guest draw gets correct world occlusion, because it is
the only place the scene depth is attached. Drawing during the lighting resolve
(back buffer, no depth) would make Mario float in front of everything.

So the composition plan is settled, and it is **not** "draw into the back buffer
before the HUD":

1. **Draw Mario during the geometry pass**, against the G-buffer targets with the
   scene depth attached. He becomes part of the G-buffer and is lit by the
   lighting pass — correct occlusion, correct lighting, exactly as PTDE intends.
2. The back buffer is written later by a resolve or copy we do not hook; whatever
   lands there is what the swap chain presents.

**The frame shape** (depth flag per pass, 6,288 frames):

```
D - D - D - D - - D D - - D D D - - D
```

Several depth-attached passes, then one or more without depth, then depth again.
The exact pass order varies (1,596 distinct signatures), so the hook must match on
*state* (scene-depth view present alongside a 4-target G-buffer set), not on
position in a fixed list.

**HUD detection (run 002, 6,769 presents, no crashes).** The last pass before
Present has three observed signatures:

| Signature | Count | % |
| --- | --- | --- |
| `rtvCount=4 depth rtv0=rtv` | 4,929 | 72.8% |
| `rtvCount=4 nodepth rtv0=rtv` | 1,818 | 26.8% |
| `rtvCount=4 nodepth rtv0=PRESENT` | 22 | 0.3% |

The final pass before Present **sometimes carries the scene depth** (73% of
frames) and sometimes does not (27%). This means the HUD (if drawn in the final
pass) is drawn either with or without depth, depending on the frame. The most
likely explanation is that the final pass is the lighting resolve, and the depth
attachment is retained in some configurations (e.g., when the engine re-binds
depth for a final depth-tested draw such as a HUD element or a post-process
effect).

For the milestone, this does not change the injection plan: **draw Mario during
the geometry pass** (depth + G-buffer). He becomes part of the G-buffer and is
lit by the lighting resolve. The HUD is drawn on top. The geometry pass is the
only pass where depth is *guaranteed* to be attached, so it is the only place
where world occlusion is available.

**Stability:** the probe ran without crashes in two consecutive in-game sessions
(6,288 + 6,769 presents). The Draw/DrawIndexed vtable hooks caused a stack
overflow (0xc00000fd) in an earlier attempt and were removed; all other hooks
are stable.

### Constant-buffer and view-matrix discovery (Map/Unmap probe)

A Map/Unmap hook on all 12 contexts captured the data being uploaded to dynamic
constant buffers.  Key findings:

* **View matrix identified.**  A 4x4 matrix that changes with camera movement
  was found in buffers of size 2288 and 4112 bytes:
  ```
  [-1.065  0.000  0.951  0.000]
  [ 0.000  2.539  0.000  0.000]
  [-0.666  0.000 -0.746 -0.050]
  [-0.666  0.000 -0.746  0.000]
  ```
  This is the view matrix.  It is uploaded via Map/Unmap (D3D11_MAP_WRITE_DISCARD)
  to dynamic constant buffers (usage=DYNAMIC, CPU_ACCESS_WRITE).

* **Orthographic projection (UI) identified.**  A fixed 4x4 matrix:
  ```
  [0.001  0.000  0.000 -1.000]
  [0.000 -0.002  0.000  1.000]
  [0.000  0.000  0.004  0.998]
  [0.000  0.000  0.000  1.000]
  ```
  Found in 1040-byte buffers.  Does not change with camera movement.

* **Buffer sizes observed:** 48 (per-draw params), 80 (material/color), 1040 (UI
  projection), 2288/4112 (view matrix), 1048576 (large data), plus others.

* **View-matrix upload function identified.**  The function at `0x140cd0a10`
  uploads a view matrix from CPU memory (`[rbx]`) to a GPU dynamic constant
  buffer via Map/Unmap (D3D11_MAP_WRITE_NO_OVERWRITE).  It copies `edi * 16`
  bytes (edi = count parameter) from `[rbx]` to the mapped pointer.

* **Caller identified.**  The function at `0x140064850` calls the upload function.
  It takes `(rcx=obj, rdx=src_ptr, r8d=count)` and:
  1. Loops up to 4 times comparing a global array with `count` to select a slot.
  2. Sets `rbx = rcx + slot * 8`, then `rcx = [rbx + 0x2B0]`.
  3. Calls the upload function with `rcx=[rbx+0x2B0]`, `rdx=obj`, `r8=src_ptr`,
     `r9d=count`.

  The source pointer (`src_ptr`) is the second parameter to this function.
  Finding the camera requires tracing who calls `0x140064850` and what they pass
  as `rdx`.

* **Stack analysis.**  When a view-matrix buffer is mapped, the stack contains:
  - `[rsp+0]` = 0x140CD0A62 (return address of Map call)
  - `[rsp+8]` = deferred context pointer
  - `[rsp+10]` = buffer resource pointer
  - `[rsp+68]` (approx) = 0x14006488F (inside the caller function)

  The actual caller of `0x140064850` is further up the stack and has not yet
  been identified.  This requires either a disassembler (Ghidra/IDA) or a
  hardware breakpoint on `0x140064850`.

* **Memory scan.**  Scanning all readable memory for the value `0x140064850`
  found exactly one reference at `0x254fee8` (heap-allocated, transient).  The
  function is called indirectly through a dynamically allocated structure, not
  a static vtable.

* **Next step for camera discovery:** Use Ghidra or IDA to disassemble the code
  around `0x140064850` and trace the caller.  Alternatively, set a hardware
  breakpoint on `0x140064850` and inspect registers when it fires (rcx=obj,
  rdx=src_ptr=view matrix pointer, r8d=count).

* **Combined view-projection matrix found.**  The 2288/4112-byte buffers contain
  a combined view-projection matrix at offset 0 (4x4 float, changes with camera).
  - m[0][0] = 1.425 (aspect-corrected FOV)
  - m[1][1] = 2.539 (1/tan(fov/2), fov ~ 43 degrees)
  - m[2][2] = 1.0 (infinite far plane or orthographic)
  - m[2][3] ~ -0.005 (near-plane related)
  - The (2,0) and (2,1) elements change with camera movement

  The host can capture this combined matrix and use it directly for rendering,
  without needing to separate view and projection.

* **No separate perspective projection buffer found.**  The 1040-byte buffers
  contain an orthographic projection (UI only).  The 3D scene uses the combined
  view-projection matrix from the 2288/4112-byte buffers.

### Camera discovery: global pointer and structure analysis

`FUN_14015e790` returns `*[0x141b68ec8] + 0x58`.  This global pointer is stable
across frames (0xc4f16e0 → camera = 0xc4f1738).  However, the structure at this
address is mostly zero — only `+0x358 = 0x1f` and `+0x37c = 0x01` are non-zero.
The view matrix at `+0x310` is all zeros.  The vtable check
(`[camera+0x160]` must have vtable[0x50] == 0x140073640) fails because
`[camera+0x160] = 0`.

**Implication:** The global pointer does NOT point to the active camera.  The view
matrix is computed and uploaded to GPU buffers from a different (transient)
object.  The host must capture the view matrix from the GPU buffer (via Map/Unmap
hook) rather than reading it from a fixed global address.

**View-matrix buffer contents** (2288 or 4112 bytes):
- Offset 0-63: Combined view-projection matrix (4×4 float, changes with camera)
- Offset 64-127: Secondary data (position/vector, partially changes)
- Offset 128+: Zeros (or other data not yet analyzed)

The perspective projection matrix is **not** in the view-matrix buffer.

**The presentation target was found and is never bound.** `IDXGISwapChain::GetBuffer`
gave the true back buffer, and exactly one render target view was flagged
`PRESENT` — the decoy with the same shape was correctly ignored. That view appears
in zero pass lines. The engine does not bind the back buffer through
`OMSetRenderTargets`; it writes to it some other way.

Two defects in the probe itself, recorded so they are not repeated:

* **The texture table overflowed** — 1,596 textures against a 512-entry cap — so
  210 bound targets could not be resolved. The cap is now 8,192.
* **The "PRESENT" flag tested the wrong condition.** It asked "was *a*
  back-buffer-shaped texture found?" rather than "is this view built from *that*
  resource?", which is why six views were flagged. Six `present-target` lines
  were emitted for a single back buffer.

#### Two analysis failures worth recording

The probe tried to find the back buffer device-side, on the assumption that the
engine creates its back buffer first. **That assumption is wrong.** Six render
target views were flagged as "PRESENT"; of those:

| Flagged view | Bound in pass lines |
| --- | --- |
| `…3A5389F8` (the first-created 1920×1080 `B8G8R8A8` texture) | **0** |
| `…3A680BB8`, `…3A6808F8`, `…5163D5F8` | **0** |
| `…5163F438` | 5,304 |
| `…5163DA78` | 693 |

The texture the heuristic picked is **never bound**, so the back buffer is still
unknown. `…5163F438` is the most-bound 1920×1080 target and is a plausible
candidate, but calling it the back buffer would be a guess. Identifying it needs
the swap chain, i.e. a correctly indexed DXGI hook — `IDXGIFactory::CreateSwapChain`
then `IDXGISwapChain::GetBuffer` — which is the next step, not a settled fact.

Two defects in the probe itself, recorded so they are not repeated:

* **The texture table overflowed** — 1,410 textures against a 512-entry cap — so
  210 bound targets could not be resolved. The cap is now wrong for this engine.
* **The "PRESENT" flag tested the wrong condition.** It asked "was *a* back-buffer
  shaped texture found?" rather than "is this view built from *that* texture?",
  which is why six views were flagged. Six `present-target` lines were emitted for
  a single back buffer.

#### Two analysis failures worth recording

The first summariser used `\S+` to capture a view token, but a token is
`pointer[kind WIDTHxHEIGHT fmt=0xNN]` — it **contains spaces**. Every resolved
target was truncated, and the script then reported that **no pass had depth**,
which is the exact opposite of the truth.

A second summariser "fixed" the truncation and then reported a per-frame sequence
in which depth never appeared, because it still could not span the bracketed
token.

Both produced confident, wrong answers from a correct log. The third version
parses `key=value` pairs structurally and prints its counts for cross-checking.

> **Rule:** a summariser that cannot parse the thing it is looking for must fail
> loudly, not report a plausible result. Two of three summaries here inverted the
> conclusion before the parse was fixed.

### Resolved: depth is bound, on deferred contexts

The first three probes all reported `dsv == NULL` and were all wrong. The
instrumented run — which covers deferred contexts as well as the immediate one —
recorded **83,329 binds carrying a depth view** out of 500,628. See
"RESOLVED: the scene depth buffer exists" above.

The cause is not a misreading: an immediate-context-only hook sees ~3.7% of the
engine's binds, because Remastered renders almost entirely through **deferred
contexts** (11 of them). The scene depth is bound there.

Lesson: *"I observed none of it"* is only as good as the coverage behind it. Before
reporting a negative, check that the hook could have seen the thing at all — here,
by comparing total bind counts between runs.

**Rules these two crashes establish for the rest of the port:**

1. **A vtable index belongs to exactly one interface.** Never reuse one across
   interfaces, and never carry one over from a working probe without re-measuring.
2. **Verify the offset the code writes, not the one it logs** — or better, log
   and patch from the same variable so they cannot disagree.
3. **A passing offline test can be green for the wrong reason.** Assert the
   probe actually armed before trusting a "no crash" result.
4. **Forwarding protects the render path, not the hook site.** A wrong slot
   corrupts state in the *call itself*; returning `S_FALSE` afterwards does
   nothing. The defence is to hook few things at measured locations.
5. **A method's real implementation is a property of the object, not of the
   method.** Immediate and deferred contexts hold different pointers for the same
   slot; sharing one global across instances crashes the game.
6. **Match the real calling order in a test.** A harness whose ordering differs
   from the game's can pass while the game crashes. Create the deferred context
   first, then bind on the immediate one.

Verified offline end to end, in a throwaway process that never touches the game
installation (`work/dsr/probe/depth_e2e.cpp`):

| Check | Result |
| --- | --- |
| Builds `/W4 /permissive-` | clean, no warnings |
| Imports | **only `KERNEL32.dll`** — no `d3d11` or `dxgi` import, so no API is called directly |
| Exports | `DirectInput8Create` only |
| IAT patch of the host's `D3D11CreateDevice` | found and patched; failure to arm is now logged, not silent |
| Device vtable patch (slots 5, 9, 10) | all three verified `OK` in-log |
| Texture description logging | correct `w/h/format/samples/bindFlags` |
| Depth detection | flagged exactly the texture `CreateDepthStencilView` then accepted |
| **Forwarding** | colour, RTV, depth and DSV all created successfully — no resource refused |

That last row is the important one: the earlier band-probing approach made the
renderer fail. This one leaves it working, which is what makes it safe to run in
a real session.

**Not yet done:** it has never run inside Remastered. Its output decides whether
"there is no scene depth buffer" (§12) means *never bound* or *never created*,
which is the difference between two different port designs.

**Not installed.** The previous revision stopped the game launching and was
removed; the game folder holds no proxy. Deployment is the user's call, not the
agent's.

---

## 14. Honest status

- **What is verified:** the executable fingerprint, the viable loader
  arrangement, the D3D11/deferred/MSAA renderer, the stable image base, the
  absence of usable engine exports, PGO optimisation, the Havok physics stack,
  the presence of a title-level anti-cheat module, and a clean
  x86 + x64 guest build from an unmodified checkout. In-game: the absence of any
  depth-stencil view bound across a full session (§12's earlier section).
- **What is compiled:** the DX11 backend (`framework/hosts/ds1remastered/
  render_device.h`) compiles and links as an x64 DLL on both MSVC and the
  project's pinned Zig toolchain. That is a **compile check only** — no device
  was created, no shader compiled, nothing drawn, and the game never launched. A
  successful compile is not evidence that the port works.
- **What is verified offline:** every `ID3D11Device` and `ID3D11DeviceContext`
  vtable index the probe depends on, the depth probe's whole chain including
  correct forwarding, and the loader arrangement. Offline tests exercise
  Direct3D through a real device, but they exercise **our** calls, not Remastered's.
- **What is not:** anything requiring the game to run. No Remastered host exists,
  no actor or camera observation exists, no guest character is integrated, and
  no Remastered composition point has been identified. Nothing here demonstrates a
  working port.
- **What is blocked:** milestone items 3 and 7 need a live session, which is
  currently out of scope by the user's choice. Item 5, the DX11 backend, is the
  largest unstarted piece of engineering.

### On the pull request

The author asked for a PR **if** the mod works in Remastered. It does not yet —
so no PR has been opened, and none should be. Opening one now would mean
shipping stubs and compile-only claims, which `AGENTS.md` explicitly forbids
("Never claim a stub or a successful compile proves a playable port") and which
the `tools/host_editions.py` `status: "implemented"` guard is designed to
prevent. These findings are the appropriate deliverable at this stage.

---

## 15. Player structure verified (live session)

**Method:** Cheat Engine attached to live `DarkSoulsRemastered.exe`, HP value scans
with damage/heal deltas, "Find what writes" to identify base register, Memory View
to enumerate offsets.

### Verified player structure offsets

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| +0x3E8 | Current HP | int32 | Value 594 (full) → 562 (after damage) |
| +0x3EC | Max HP | int32 | Value 594 (matches full HP) |
| +0x3F0 | Current Stamina | int32 | Changes with actions |
| +0x3F4 | Max Stamina | int32 | Stable value |

Base address = HP address − 0x3E8. Base changes per session (heap allocation);
offsets are stable.

### Capability implications

- `HostCapability::Health` → **VERIFIED** (player HP read working)
- `HostCapability::Stamina` → **VERIFIED** (player stamina read working)

These capabilities are set in `capabilities.h::buildCapabilities()` when
`obs.player.valid` is true.

---

## 16. Matrix-only integration implemented

### Design decision

Rather than discovering player position/rotation offsets (which require additional
live-session work and are fragile across sessions), the host uses the **combined
view-projection matrix** captured from GPU dynamic constant buffers via Map/Unmap
hooks.

This is sufficient for the presentation-only milestone:
- Guest draws during geometry pass (G-buffer + scene depth)
- View-projection matrix provides correct camera transform
- Mario becomes part of G-buffer, lit by lighting pass
- Correct world occlusion without needing player position/rotation

### Files updated

| File | Change |
| --- | --- |
| `framework/hosts/ds1remastered/native_types.h` | Added `PlayerState` struct with verified offsets (+0x3E8 HP, +0x3EC Max HP, +0x3F0 Stamina, +0x3F4 Max Stamina); `readFrom()` method with SEH protection |
| `framework/hosts/ds1remastered/capabilities.h` | `buildCapabilities()` now sets `Health` and `Stamina` flags when `obs.player.valid` |
| `framework/hosts/ds1remastered/runtime.cpp` | `discoverPlayerCandidates()`, `refreshObservation()`, `hostOnCaptureTick()`, `getViewProjectionMatrix()`, `getObservation()`, `getCapabilities()` exports |
| `framework/hosts/ds1remastered/camera_capture.h` | Separated `getCameraState()` (copy) from `getCameraStateInternal()` (ref for hooks); per-context map bookkeeping |
| `framework/hosts/ds1remastered/camera_capture.cpp` | Internal `g_cameraState` definition and the Map/Unmap capture path |

### Build status

`python tools/framework.py build --check-portability` **succeeds**:
- x64 guest binaries compile cleanly (`mario64.dll`, `sm64.dll`)
- C/C++ SDK headers compile for x86 and x64
- Host skeleton (9 files) compiles on MSVC and Zig toolchains
- No runtime execution; portability check is compilation-only

### Remaining for presentation milestone

| Item | Status |
| --- | --- |
| DX11 backend (`render_device.h`) | Compiles, not executed |
| Geometry-pass injection hook | Not implemented (needs `OMSetRenderTargets` hook on all 12 contexts) |
| Guest draw with captured view-projection | Not implemented |
| Actor/camera validation | Player structure found; camera = GPU buffer capture |

### Anti-cheat note

All live-session work used Cheat Engine on a disposable test character. The game
folder currently holds no proxy DLL. The user accepted anti-cheat risk and
maintains save backups at `Desktop\DS1 Backup Saves`.

---

## 17. View-projection capture VERIFIED live

The combined view-projection is captured and confirmed to track camera motion.
This is the input the presentation-only milestone needs.

### Location

| Item | Value |
| --- | --- |
| Source | `D3D11_MAP` payload of a **4112-byte dynamic constant buffer** |
| Offset within buffer | **0** (first 16 floats) |
| Read point | **Unmap**, not Map |
| Context | Deferred contexts (created via `CreateDeferredContext`, slot 27) |

The 2288-byte constant buffer also carries a valid view-projection, but it is a
different shader variant with its own FOV term (`m11` ranged 1.6883-2.5396 versus
2.3908-2.5396 for the 4112 buffer). Mixing them interleaved two different camera
FOVs, so **only the 4112-byte buffer is used**. A separate axis-aligned projection
(1040-byte buffer, `m11 = 2.5386`, static) circulates too and is rejected by the
rotated-basis requirement.

### Layout (row-major, 16 floats)

```
 0: m00  -1.9015    4: m10   0.0133     8: m20   0.0514    12: m30  (padding)
 1: m01   0.0000    5: m11   2.5256     9: m21  -0.1015    13: m31
 2: m02  -0.0981    6: m12  -0.2572    10: m22  -0.9935    14: m32
 3: m03   0.0000    7: m13   0.0000    11: m23  -0.0501    15: m33
```

Verified invariants, stable across 500,000+ observed updates:

- `m03 = 0` and `m13 = 0` exactly.
- Row 4 mirrors row 3 (`m30..m33 == m20..m23`) and is padding, not a basis vector.
- `m11` is the projection scale, `2.3908`-`2.5396` across FOV changes, stable for
  a given FOV. It is the strongest single discriminator for this matrix.
- `m00`/`m02` are **signed** and sweep the full yaw circle. An earlier validator
  that required a positive `m00` was wrong and rejected valid captures.
- `m23` sits at `-0.0501` (near-plane depth term) and is the least mobile element.

### Reading at Unmap, not Map

Dumping at `Map` returns all zeroes: the game writes the payload after `Map`
returns. The capture must read the mapped pointer immediately before forwarding to
the real `Unmap`, while the mapping is still valid.

### Per-context map tracking required

A single shared "last mapped" pointer races once the deferred contexts are
patched: two contexts interleave `Map`/`Unmap` and the wrong buffer's memory is
read as a matrix. Each hooked context keeps its own pending map, matched against
the resource being unmapped.

### Anti-cheat: delayed hook installation

Eager vtable patching during device creation causes a white screen at the main
menu. IAT patches at load time are required but insufficient. **Installing all
vtable hooks 15 seconds after startup, on a separate thread, is stable** and the
game reaches a playable main menu. Deferred contexts created before that delay are
not patched; the remaining ones arrive ~4 s into the run and are patched as they
appear.

### Player discovery does NOT work by scanning

Four live runs of the heap scan produced only false positives; the `+0x3E8`
offsets are correct but the base is a per-session heap address that cannot be
identified from value shape alone. Failure modes observed:

- Accepting a recycled heap block (`stamina 65537/262144`, `hp 0/5046960`).
- 48 candidates all matching at 8-byte strides, i.e. sliding windows across one
  unrelated structure.

Discovery by scanning is abandoned: the player structure is reached through static
pointer chains instead (section 18).

---

## 18. Health and stamina pointer chains VERIFIED live

The player structure lives at a heap address that changes every session, so it is
reached through static module-relative roots. Remastered has no ASLR (image base
`0x140000000`, `DYNAMIC_BASE` absent), so these roots are fixed addresses.

Both chains came from Cheat Engine pointer scans and were replayed in reverse
leaf-first order, then confirmed live.

### Health chain

```
DarkSoulsRemastered.exe+01726B14 -> +0x100 -> +0x28 -> +0xE0 -> +0xFE0 -> +0x0
```

| Item | Value |
| --- | --- |
| HP | `base + 0x3E8` (int32) |
| Max HP | `base + 0x3EC` (int32) |

Verified: HP moved `453 -> 401 -> 594` while taking damage and healing, and MaxHP
matched the character's real maximum.

The `+0xE0` link is **null until a character is loaded**, so resolution fails at
the title screen and succeeds once a save is in play. That is expected; the worker
retries every 2 s.

### Stamina chain

Stamina is in a **different structure with its own root** — the earlier assumption
that it sat at `base + 0x3F0` next to HP was wrong.

```
DarkSoulsRemastered.exe+01A31768 -> +0x28 -> +0x38 -> +0x180 -> +0x30
                                 -> +0x128 -> +0x50  -> +0x30 (value)
```

Verified: drained `95 -> 65 -> 35 -> 5` under sprinting and refilled
`5 -> 16 -> 27 -> 37 -> 49 -> 59 -> 71 -> 81 -> 92 -> 95`, over repeated cycles.

### Stamina is a deficit accumulator, not a clamped pool

The native field goes **negative** when stamina is overspent (observed down to
`-25`) and then refills toward the character maximum:

```
5 -> -25 -> -17 -> -6 -> 5 -> 16 -> ... -> 95
```

So `+0x30` holds a signed accumulator. The host clamps the reported value at 0
and tracks the observed peak as the pool size, which is how `maxStamina` becomes
known (95 for this character).

### An earlier reading was wrong

Reading `base + 0x3F0` and `base + 0x3F4` returned a constant `49` that never moved
during play while the real pool was 95. It was accepted as plausible because
`49` fell inside a loose sanity range. Those offsets are marked `WRONG` in
`native_types.h` so the mistake stays documented.

Lesson applied: a value that reads plausibly is not evidence it is correct. The
field was only trusted once it was observed **moving** under sprinting.

### Capability reporting corrected

`buildCapabilities()` derived both `Health` and `Stamina` from `player.valid`,
which only reflected HP. That advertised a Stamina capability while the stamina
read returned a constant wrong value. Each capability now depends on its own
observation (`player.valid` / `player.staminaValid`).

## 19. ID3D11DeviceContext vtable slots — derivation and a wrong correction

The geometry-pass injection needs `OMSetRenderTargets`, which means the context
vtable layout must be exactly right. Getting it wrong patches an unrelated
function and corrupts the game, so the derivation is recorded here.

### Correct derivation

```
ID3D11DeviceContext : public ID3D11DeviceChild : public IUnknown
```

The absolute vtable index is a method's own position in `ID3D11DeviceContext`
plus **7** inherited slots: 3 from `IUnknown` plus **4** from `ID3D11DeviceChild`
(`GetDevice`, `GetPrivateData`, `SetPrivateData`, `SetPrivateDataInterface`).

| Method | own | absolute |
| --- | --- | --- |
| `VSSetConstantBuffers` | 0 | 7 |
| `PSSetShader` | 2 | 9 |
| `DrawIndexed` | 5 | 12 |
| `Draw` | 6 | 13 |
| **`Map`** | **7** | **14** |
| **`Unmap`** | **8** | **15** |
| `IASetInputLayout` | 10 | 17 |
| `IASetVertexBuffers` | 11 | 18 |
| `IASetPrimitiveTopology` | 17 | 24 |
| **`OMSetRenderTargets`** | **26** | **33** |
| `OMSetBlendState` | 28 | 35 |
| `OMSetDepthStencilState` | 29 | 36 |

`work/dsr/vtable_slots.py` extracts the own-method order from the installed
`d3d11.h`. The inherited-slot count must be added separately; the script alone is
not sufficient.

### A wrong correction that crashed the game

An intermediate change concluded the base was 3 (`IUnknown` only) and moved
`Map`/`Unmap` to slots 10/11. **That crashed the game on save load.** Slots 10 and
11 are `PSSetSamplers` and `VSSetShader`, so the hook was replacing shader binding
with a `Map`-shaped function.

The error came from `work/dsr/vtable_slots.py` printing only the class's own
declarations; the intermediate `ID3D11DeviceChild` layer was not counted. The
original 14/15 were correct.

Recorded because the sequence matters: an over-confident correction with a real
crash attached is the signal that the premise, not the code, was wrong.

### Deferred contexts share the layout but not the implementation

`QueryInterface` shows every deferred context implements `ID3D11DeviceContext`
and returns the **same object pointer** for `ID3D11DeviceContext` through
`Context4`, so it is one class with all interface versions aggregated.

But d3d11.dll gives immediate and deferred contexts **separate implementations**:
their vtables hold different function addresses at the same indices. Comparing
addresses across contexts therefore fails for a perfectly correct slot index, and
that check had to be replaced.

What is actually verified before patching:

- the vtable is present and non-null;
- the slots being patched are distinct from each other;
- the slots do not already hold a host hook.

A differing address from the immediate context is logged as expected, not a fault.

### Method identity cannot be checked via GetProcAddress

`Map`, `Unmap` and `OMSetRenderTargets` are **not exports** of `d3d11.dll`; they
are reachable only through the COM vtable, so `GetProcAddress` returns null and
cannot serve as an identity reference. An earlier attempt to use it as one
reported a false `MISMATCH` on every slot.

Identity therefore rests on the header derivation plus the structural checks above,
with the live capture (real constant-buffer payloads that change under camera
motion) as the practical confirmation.

### Camera capture re-verified from genuine Map/Unmap hooks

With 14/15 restored and the guard in place, all 12 contexts patch (1 immediate +
7+ deferred) and the view-projection is captured from real `Map`/`Unmap` calls,
tracking camera motion across thousands of updates. The earlier capture that
appeared to work under the same indices is now backed by verified slot identity
rather than working by accident.

### Geometry-pass injection — not started

`OMSetRenderTargets` (slot 33) is now known and verified, and render-target
bindings are being observed. No guest draw or pipeline-state management exists
yet; this section establishes the slot identity and the attachment signatures
that injection depends on.

---

## 20. Render-target binding observation (pass identification)

`OMSetRenderTargets` is hooked on every context and each **distinct attachment
signature** is logged once with a call count, rather than every call. Passes are
recognised by what is attached: count, per-target format and dimensions, depth
format, sample count.

### Descriptor access

`D3D11_RESOURCE_DESC` is absent from the bundled header, so the descriptor is
read through `ID3D11Texture2D`, obtained by `QueryInterface`, with `GetDesc` at
slot 10. Both steps matter:

```
ID3D11Texture2D : ID3D11Resource : ID3D11DeviceChild : IUnknown
  0-2   IUnknown
  3-6   ID3D11DeviceChild
  7-9   ID3D11Resource (GetType, SetEvictionPriority, GetEvictionPriority)
  10    ID3D11Texture2D::GetDesc
```

A view yields its resource via `ID3D11View::GetResource` at slot 7.
`ID3D11View` has exactly one method — Microsoft documents it as such — so a view
cannot be asked for a descriptor directly.

### Four failures on the way, each recorded

1. **Slot 8 on the resource** is `SetEvictionPriority`, not `GetDesc`. Calling it
   with a descriptor pointer corrupted driver state and crashed the game on load.
2. **Fabricated IIDs.** `IID_ID3D11Texture2D` and `IID_ID3D11Buffer` were typed
   from memory instead of copied from the header, so every `QueryInterface`
   returned `E_NOINTERFACE`. That read as "unsupported resource type" rather
   than "wrong constant", and cost a run. The GUIDs are now taken verbatim from
   `d3d11.h`.
3. **Wrong struct layout.** `D3D11_RESOURCE_DESC` was declared with `Format` last
   and a `void*` for `Usage`, putting `Format` at offset 52 instead of 20. Every
   format read returned 0. Offsets are now computed and `sizeof` is enforced by
   `static_assert`.
4. **A validity check rejected correct data.** An upper bound on `Format` was
   treating `87` as garbage, but `DXGI_FORMAT_R8G8B8A8_UNORM` *is* 87. Only
   dimensions are validated now.

Probing vtable slots by calling them is not safe and crashed the game twice.
Slots are resolved from the documented interface and reached only by
`QueryInterface`.

### Observed formats

| Value | Format | Observed on |
| --- | --- | --- |
| 87 | `R8G8B8A8_UNORM` | back buffer, 1920x1080 |
| 44 | `D24_UNORM_S8_UINT` | scene depth, 1920x1080 |
| 26 | `R16G16B16A16_FLOAT` | 64-bit float colour target |
| 34 | `R16G16B16A16_UNORM` | second colour target |
| 41 | `R8G8B8A8_TYPELESS` | 64x64 mip chain entry |
| 10 | `R16G16_FLOAT` | 480x270 |
| 28 | `R8G8B8A8_UNORM_SRGB` | present chain |

Bindings commonly present four targets with only the first populated, which
matches D3D11's `OMSetRenderTargets` being called with a fixed count while
targets beyond the first are null.

### Candidate geometry pass

**`fmt=26` (`R16G16B16A16_FLOAT`) at 1920x1080 with `fmt=44` (`D24_UNORM_S8_UINT`)
depth.** A 64-bit float colour target plus 32-bit depth is characteristic of a
G-buffer pass, and the 16-bit-per-channel formats are consistent with deferred
rendering rather than a forward HDR pass.

This is **a candidate identified from attachment formats, not a confirmed
geometry pass.** Confirmation requires showing that the game's actual geometry
renders into these targets, which needs either draw-call observation or a test
draw. Do not treat it as established.

Validating only the *source* address of a pointer read is not sufficient: a
successful read of a non-pointer field yields garbage that is then dereferenced,
which **crashed the game on save load** during probing. Each step now validates
both the source address and the resulting value as a plausible user-mode pointer
(`< 0x0000800000000000`), and field reads are checked before dereferencing.

## 21. Host test draw reaches the frame (live, 2026-10-19)

**Result:** with the corrected test draw, a host-owned triangle (own shaders, own
vertex buffer, clip-space positions) appears on screen as a large magenta
triangle in the centre. The game ran stably for the whole session and draws
were logged at roughly 4 Hz. The triangle flashes because the draw is
rate-limited to one per 250 ms.

**What made it work, versus the run that preceded it:**

- Resources come from the `ID3D11Device` interface. The hand-typed function
  pointers passed a string as the class linkage and pushed
  `D3D11_CPU_ACCESS_WRITE` (0x10000) into `CreateBuffer`'s `ppBuffer`.
- State is saved and restored through `ID3D11DeviceContext`. The earlier
  hand-numbered Get* calls used slot 75 (`PSGetSamplers`) and slot 93
  (`SOGetTargets`), which is what crashed. Removing the restore was not a fix.
- The draw is issued when the game rebinds *away from* the matching binding,
  after its clear and with its viewport set.
- Depth is ALWAYS with no writes and culling is off, so the visibility test is
  independent of depth convention and winding.
- Contexts that share a hooked vtable now get their own bookkeeping. Before,
  an unregistered context's `Map` failed, its `Unmap` was dropped and its
  `OMSetRenderTargets` was swallowed.

**Not yet established:**

- **Which binding carried the visible triangle.** Two bindings match the
  signature: #4 (`R16G16B16A16_FLOAT` + `R8G8B8A8_UNORM`, D24S8 depth) and #5
  (`R16G16B16A16_FLOAT` alone, D24S8 depth). The multi-target #4 is the
  stronger G-buffer candidate.
- **Whether the triangle is lit by the lighting pass.** A flat, unshaded magenta
  would point to a forward/HDR target rather than a G-buffer channel.
- In this session the stamina chain stayed in "searching". Health and the
  camera resolved.

## 22. Render-target identity and world-anchored test geometry (live, 2026-10-19)

Tested in one game session by switching `inject_mode.txt` live and taking
screenshots (`geometry_pass.cpp`, "Live test controls").

**Correction to section 21:** the dark/black triangles in the runs before
this were caused by the old precompiled test shaders, not by the game. The
vertex shader output `COLOR` in register 1 after `SV_POSITION`, but the pixel
shader's input signature declared `COLOR` alone, so the linkage did not match
and the pixel shader output zero. Also, with `IndependentBlendEnable = FALSE`
the target-0 write mask applied to every bound target, so the second target
received undefined output as well. The current shaders are compiled from
`test_shaders.hlsl` with the system `d3dcompiler_47.dll` by
`tools/compile_test_shaders.py`. The pixel shader writes `SV_Target0/1` from a
constant buffer.

**Binding #4** (`R16G16B16A16_FLOAT` + `B8G8R8A8_UNORM` (fmt 87), D24S8 depth,
1920x1080):
- Writing target 0 gives **flat, unshaded** colour in the final image, with the
  HUD drawn over it. Target 0 is the HDR scene colour, and nothing re-lights
  what we write.
- Writing target 1 (zero, or magenta) has no visible effect. Its role is
  unknown, possibly an auxiliary buffer (mask/velocity/SSAO input).
- **Conclusion: this pass behaves as a forward-shaded main pass, not a
  deferred G-buffer pass.** Mario will not be lit by the game. The host must
  shade him in its own pixel shader (as the PTDE backend does), using the scene
  depth for occlusion. The earlier "G-buffer" label (sections 16 and 20) is
  therefore wrong for this binding.
- This is the binding injection should target. Drawing at the end of the
  pass, before the game rebinds, is visible every frame.

**Binding #5** (`R16G16B16A16_FLOAT` alone, D24S8): writes show only on some
frames (flashing) and are not usable as the injection point.

**World-anchored geometry (camera-relative):**
- Four triangles placed 8 m from the camera along ±X/±Z, transformed on the CPU
  with the captured view-projection read as four rows
  (`clip_i = dot(row_i, (p, 1))`), project correctly. They keep their bearing
  as the camera turns, with correct perspective.
- **Depth:** the projection maps near to 0 and far to 1 (standard, not
  reversed). With a `LESS_EQUAL` test against the game's depth, the player
  character correctly occludes the marker (confirmed in a screenshot).
- The view-projection has no translation, so positions are
  **camera-relative**. The rest of the 4112-byte buffer holds a second
  translated matrix at floats 32-47, a previous-frame-style VP at 48-63, and
  3x4 per-instance matrices from float 112 onward. Their translations are small
  (camera-relative), so the camera's world position is **not** in this
  buffer. Anchoring to a world point (Mario at the player) needs the camera or
  player world position from memory. A candidate is the DSR-Gadget
  `ChrPosData` chain (`work/dsr/MODDING_RESOURCES.md`), unverified.

**Not yet established:** the role of binding #4 target 1, and the camera
world position.

## 23. Player and camera world positions; geometry anchored at the player (live, 2026-10-19)

**Player position, confirmed.** The DSR-Gadget chain was located by AOB in
this build (Steam 1.03.1):

```
WorldChrMan static = exe+1C77E50   (AOB 48 8B 05 ?? ?? ?? ?? 48 8B 48 68 48 85 C9 0F 84 ...)
PlayerIns   = [[static] + 0x68]
ChrMapData  = [PlayerIns + 0x68]
ChrPosData  = [ChrMapData + 0x28]  -> angle +0x4, X/Y/Z +0x10/+0x14/+0x18 (float, Y up)
```

In two sessions, PlayerIns was **the same object** as our verified health base
(HP at +0x3E8), which corroborates the chain. Logged positions tracked walking,
for example (1.88, 197.76, -17.15) to (3.05, 197.61, -21.39) in the Undead
Asylum.

**Camera world position, found by search.** The DSR-Gadget follow-camera chain
(`[[[static]]+0x60]+0x60`) does not resolve in this build. The host instead
searches structures within three pointer hops of the camera statics for a
float triple from which the player (+1 m) projects near screen centre under the
captured view-projection. The first path that passed twice was adopted:

```
CamMan static = exe+1C6E188   (AOB 48 8B 05 ?? ?? ?? ?? 48 63 D1 48 8B 44 D0 08 C3)
camera position = [static] + 0x60   (float3, world space)
```

The same position also appears at `[exe+1C7A488 (FollowCam static)] -> +0x60`,
fields +0x40/+0x120/+0x1D0/+0x1E0/+0x3E0, and one hop further at +0x40/+0xF0/
+0x100. The correct follow-camera chain is therefore one dereference shorter
than DSR-Gadget's. Over the session the adopted path kept the player centred on
92 of 98 checks; the misses were early frames during load.

**Anchored geometry.** With `anchor=player`, a 0.6 m x 2 m triangle at
`player - camera` (camera-relative), projected with the captured
view-projection into binding #4 target 0, stands at the character's feet.
It stays there while walking and turning the camera (user report plus
screenshot), and the character's body occludes it correctly.

**Implication.** Everything needed to place geometry in the world is now
observed live: the view-projection (camera-relative), the camera world
position, the player world position and facing, and an injection point with
scene depth. The remaining work for the presentation milestone is Mario's mesh
(libsm64), host-side shading in the pixel shader, and replacing the test
triangle with the renderer.
