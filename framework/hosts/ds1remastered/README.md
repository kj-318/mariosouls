> **Current state:** see [PORT_STATUS.md](PORT_STATUS.md). The host now runs and is playable. The text below is the original observation-phase README, kept for history.

# Dark Souls Remastered edition (Remastered) — work in progress

**This is not an implemented host edition.** PTDE remains the only implemented
host, and this directory contains no `edition.json`, so
`tools/host_editions.py` will refuse `ds1remastered` before any compilation.
That refusal is intentional and correct: declaring the edition would require
`status: "implemented"`, which would be a false claim.

Nothing here has been run. No Remastered process has been loaded, no hook or
engine address is known, and there is no gameplay evidence of any kind. See
`docs/REMASTERED_FINDINGS.md` in the project root for the verified findings this
work is based on, and `framework/docs/HOST_PORTING.md` for the milestone this
is meant to satisfy.

## What is here

| File | State |
| --- | --- |
| `render_device.h` | A DX11 backend implementing `SCRenderAPI::Renderer`. Compiles and links as an x64 DLL on both MSVC and the pinned Zig toolchain. Not run. |
| `runtime.cpp` | Composition root: dinput8.dll proxy, IAT hooking for D3D11CreateDevice and CreateDXGIFactory, startup thread. Compiles and links as x64 DLL. Not run. |
| `loader.h` | DLL entry point and IAT patching declarations. |
| `startup.h` | Startup thread and module-wait utilities. |
| `memory.h` | 64-bit memory read/write utilities with bounds checking. |
| `native_types.h` | Placeholder native type definitions (unverified). Includes Matrix4x4 and CameraState. |
| `capabilities.h` | Host capability reporting (SceneDepth verified; others unset). |
| `camera_capture.h` | View-projection matrix capture from GPU buffers via Map/Unmap hook. |

## What is not here (and why)

| Missing | Reason |
| --- | --- |
| `edition.json` | Requires real code *and* milestone evidence; metadata alone never establishes support |
| Build recipe / loader DLL | The loader arrangement is identified (§2 of the findings) but a proxy is not built, and none may be placed next to the executable while runtime work is out of scope |
| Native bindings, actor/camera observation | Requires fresh native evidence from a live session. Inventing addresses is explicitly forbidden |
| Composition / before-HUD discovery | Requires observing the real scene depth and UI boundary |
| Installation path | PTDE's installer stays PTDE-only; a new edition needs independent, reversible guards |

## The DX11 backend

`render_device.h` translates the neutral internal renderer boundary
(`framework/sdk/render_api.h`) into Direct3D 11. It is **not** a port of PTDE's
D3D9 backend: fixed-function FVF, texture-stage states and D3D9 state blocks do
not exist in D3D11, so pipeline state is expressed as shaders, input layouts and
pipeline-state objects.

It deliberately does not decide anything that requires observing the game:

* it never selects a render target or depth buffer — the caller supplies a
  `Composition` and asserts `depthVerified`;
* a depth-tested draw is **refused** without verified scene depth, and a
  before-HUD submission without depth is refused, matching PTDE's rule;
* RTV format, depth format and sample count are read from the engine's own
  resources, and a change invalidates the pipeline cache.

## What the live runs established

**A scene depth buffer exists, and PTDE's approach transfers.** A fully
instrumented run recorded **83,329 binds carrying a depth view** out of 500,628,
plus 24,880 `ClearDepthStencilView` calls. The scene depth view is **1920×1080**,
`D24_UNORM_S8_UINT` over `R24G8_TYPELESS`, cleared once per frame and bound with
**four render targets** — attached alongside the G-buffer during the geometry pass.
Depth is a genuine render-pass attachment, so a guest draw can bind it and
depth-test against the world.

Three earlier probes reported `dsv == NULL` and were wrong. The cause was not a
misreading but a coverage gap: **Remastered renders almost entirely through
deferred contexts** (11 observed). An immediate-context-only hook sees roughly 3.7%
of the engine's binds and reports almost nothing. A guest draw must be issued on
whichever context the engine is currently using, and deferred work requires an
explicit `Flush()`.

Also established: depth resources carry `D3D11_BIND_SHADER_RESOURCE` alongside
`D3D11_BIND_DEPTH_STENCIL`, so depth is sampled as a texture; 960×540 and 480×270
views are reduced-resolution depth cleared once per frame; 2048×2048 and 512×512
are shadow maps. Nine of nineteen depth views were created and then never used.

**The renderer is not multisampled.** All 118 distinct texture descriptions the
engine creates have `SampleDesc.Count == 1`. The earlier warning here — that a
guest draw must use the engine's multisampled depth buffer or depth-fight — is
withdrawn; there is no MSAA target to match against.

The depth path in `render_device.h` still refuses depth-tested drawing unless the
caller supplies a verified depth view. That gate is deliberate: the view exists and
has been identified, but binding it at the wrong moment produces wrong occlusion
rather than an obvious failure.

Other measurement notes worth keeping:

* The installed `d3d11.h` on a machine like this one **differs from Microsoft's
  documented D3D11 ABI, and the runtime agrees with it**. `D3D11_BIND_INPUT_LAYOUT`
  is absent from the enum, so `RENDER_TARGET` is `0x20` and `DEPTH_STENCIL` is
  `0x40`; `ID3D11Device` also inherits only `IUnknown`. Both were confirmed
  against the runtime, not read off the header. **Hard-coding Microsoft's
  documented constants would be wrong here**, so a port must verify the
  header/runtime pairing on the machine it ships to.
* `ClearDepthStencilView` (header index 53) cannot be identified by stub-scanning,
  because it internally calls `RSSetViewports`. It is deliberately left unhooked
  rather than hooked at a guessed slot.

An earlier draft of this file claimed the vtable indices were displaced by +7.
**That was wrong, and it is withdrawn.** The header indices match the measured
runtime exactly: `OMSetRenderTargets` 33, `RSSetViewports` 44,
`CreateTexture2D` 5, `CreateDepthStencilView` 10. The false conclusion came from
a mis-parsing helper script plus a wide-band bisection that registered unrelated
internal D3D11 calls as hits. See `docs/REMASTERED_FINDINGS.md`.

**A vtable index belongs to exactly one interface.** An earlier probe reused
`IDXGISwapChain`'s `Present` index on `IDXGIFactory`, where it is `EnumAdapters`,
and the game stopped launching. Indices are now taken only from the interface
they were measured on.

Conventions are translated explicitly rather than inherited: the contract's
row-major, row-vector matrices are transposed on upload and applied with
`mul(vector, matrix)`; packed AARRGGBB colour is unpacked in the shader; `shade`
is applied as an explicit grayscale multiply; screen vertices convert from the
D3D9 XYZRHW form to clip space.

State restoration uses `DSRStateScope`, which records exactly the context state
the backend disturbs and restores it on every exit path — D3D11 has no
equivalent of `IDirect3DDevice9` state blocks.

## Known environment constraints

Two parts of the Direct3D 11 API are missing from **every** `d3d11.h` on the
development machine, including Microsoft's own SDK and a freshly downloaded,
hash-verified copy of it:

* the bundled pipeline-state form — `ID3D11PipelineState`,
  `D3D11_GRAPHICS_PIPELINE_STATE_DESC`,
  `ID3D11Device::CreateGraphicsPipelineState`, `GSSetPipelineState`, and
  `D3D11_VIEWPORT_AND_RASTERIZE_SLOT_COUNT`;
* `D3D11_RESOURCE_DESC` / `ID3D11Resource::GetDesc`.

The backend is written so neither is required:

* it binds the **discrete** D3D11 state objects — `ID3D11BlendState`,
  `ID3D11RasterizerState`, `ID3D11DepthStencilState`, `ID3D11InputLayout`,
  `ID3D11VertexShader`, `ID3D11PixelShader` — individually through the device
  context. That is a complete pipeline definition for a draw call in D3D11; the
  bundled pipeline-state form is optional. It also keeps the state footprint
  explicit, matches how PTDE's D3D9 backend already sets state per draw, and
  avoids declaring a COM interface that is not there.
* the target size is taken from the caller, or read from the context's bound
  viewport, instead of querying `GetDesc`.

One consequence to be aware of: because `GetDesc` is unavailable, the backend
**cannot independently confirm** that a supplied depth view has the same sample
count as the colour target. The `Composition::depthVerified` assertion is
therefore the only gate on depth-tested drawing, and it must come from a real
observation. The sample-count check is listed in the findings as capability
lost to the environment, not as a decision.

`D3DCompile` is likewise resolved at run time from `d3dcompiler_47.dll` rather
than linked, because the pinned Zig/mingw toolchain ships no `d3dcompiler`
import library. The backend refuses to draw if that DLL is unavailable; it never
falls back to a guess.

## Rules this file must keep

* no native reads, hooks, game addresses, or use of `Present` as a clock;
* drawing never steps gameplay, consumes input, or advances a simulation clock;
* no graphics type crosses the adapter DLL ABI;
* PTDE headers are never included, and PTDE addresses, IDs or layouts are never
  translated mechanically.