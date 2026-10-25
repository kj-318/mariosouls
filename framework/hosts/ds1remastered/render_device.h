#pragma once

// DX11 backend for the Dark Souls Remastered edition.
//
// This translates the neutral internal renderer boundary
// (framework/sdk/render_api.h) into Direct3D 11. It is NOT a port of PTDE's
// D3D9 backend: fixed-function FVF, texture-stage states and D3D9 state blocks
// do not exist in D3D11, so pipeline state is expressed as shaders, input
// layouts and pipeline-state objects here.
//
// Verified context, from the owned installation (docs/REMASTERED_FINDINGS.md):
//   * DarkSoulsRemastered.exe is x64 PE32+, loads at 0x140000000 (no ASLR), and
//     imports only d3d11.dll!D3D11CreateDevice and dxgi.dll!CreateDXGIFactory.
//   * Feature level D3D_FEATURE_LEVEL_11_0; 1920x1080; swapchain format
//     DXGI_FORMAT_B8G8R8A8_UNORM; backbuffer SampleDesc.Count == 1.
//   * The main pass binds 2 render targets (colour + an auxiliary BGRA8 target)
//     with scene depth; see the composition note below.
//   * NOT MULTISAMPLED. Every one of the 118 distinct texture descriptions the
//     engine creates has SampleDesc.Count == 1. An earlier note here warned that
//     a guest draw "must use the engine's own multisampled depth buffer or it
//     will depth-fight"; that warning is withdrawn - there is no MSAA target to
//     match against, so a depth-tested guest draw has no sample-count conflict.
//
// DEPTH: a scene depth view EXISTS, and PTDE's approach is available.
// A device- and context-instrumented run recorded 83,329 binds carrying a
// non-NULL depth view out of 500,628, plus 24,880 ClearDepthStencilView calls.
//
// The scene depth view is 1920x1080, created from an
// DXGI_FORMAT_R24G8_TYPELESS resource and exposed as
// DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D. It is cleared
// once per frame and bound in four-slot render-target calls (two slots
// populated) during the forward main pass. That is exactly PTDE's model:
// depth is a genuine render-pass attachment, so a guest draw can bind it and
// depth-test against the world. An earlier version of this file said the
// opposite; that was wrong and is withdrawn.
//
// Smaller views at 960x540 and 480x270 are reduced-resolution depth (cleared once
// per frame, presumably for volumetrics or soft shadows), and 2048x2048 /
// 512x512 are shadow maps. Depth resources also carry
// D3D11_BIND_SHADER_RESOURCE alongside D3D11_BIND_DEPTH_STENCIL, so depth can be
// sampled as a texture; the writable and read-only views of one resource appear as
// a pair.
//
// CRITICAL, and it was the cause of three wrong "no depth buffer" reports:
// Remastered renders almost entirely through DEFERRED contexts (11 of them
// observed). An immediate-context-only hook sees roughly 3.7% of the engine's
// binds and will report almost nothing. A guest draw must therefore be issued on
// whichever context the engine is using, and deferred work requires an explicit
// Flush() before it can be relied on.
//
// Because of that, the depth path below stays gated: depth is refused unless the
// caller supplies a verified depth view. This is a real, recorded gap rather than
// a silent fallback - do not "fix" it by binding an unrelated surface, which
// would depth-fight or make the guest vanish.
//
// Composition point (verified live 2026-10-19, docs/REMASTERED_FINDINGS.md
// sections 21-23): the engine's main pass binds R16G16B16A16_FLOAT (HDR scene
// colour) + B8G8R8A8_UNORM with D24S8 scene depth.  The host submits at the
// END of that pass, just before the engine rebinds, with the engine's targets
// still attached (Composition::targetsBound).  That pass is FORWARD-shaded:
// whatever the guest writes to target 0 appears unlit, so the guest must shade
// itself in pixelPS.  The earlier description of a G-buffer pass whose output
// the engine re-lights was wrong and is withdrawn.  The HUD is drawn later and
// stays on top.
//
// Projection convention (verified live): the captured view-projection is
// camera-relative, read as four rows (clip_i = dot(row_i, (p, 1))), standard
// depth (near 0, far 1).  Hosts pass it as Camera::projection = transpose(VP),
// view = identity, world = translation(-camera position), so world-space
// vertices go through this backend's row-vector mul() unchanged.
//
// Scope rules honoured here:
//   * no native reads, no hooks, no game addresses, no Present-hook clock;
//   * drawing never steps gameplay, consumes input or advances a simulation
//     clock (it only consumes borrowed CPU snapshots);
//   * no graphics type crosses the adapter DLL ABI - this header is host-only;
//   * PTDE headers are not included.
//
// Build requirement: only the discrete D3D11 state objects are used
// (ID3D11BlendState, ID3D11RasterizerState, ID3D11DepthStencilState,
// ID3D11InputLayout, ID3D11VertexShader, ID3D11PixelShader) bound individually
// through the device context. This file deliberately does NOT use
// ID3D11PipelineState / ID3D11Device::CreateGraphicsPipelineState /
// GSSetPipelineState, which bundle that same state. The bundled form is
// optional in D3D11 and nothing here needs it: the discrete setters fully
// define the pipeline for a Draw call. Avoiding it also keeps the backend
// buildable against header sets that expose only the modular D3D11 state model,
// and it matches how PTDE's D3D9 backend already sets state per draw. See
// docs/REMASTERED_FINDINGS.md for the environment note.

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../../sdk/render_api.h"
#include "render_shaders_gen.h"  // generated by tools/compile_test_shaders.py

// ---------------------------------------------------------------------------
// Backend namespace
// ---------------------------------------------------------------------------
namespace DSRGraphics {

// The scene targets for one composition. Borrowed from the edition's
// composition layer and only valid for the synchronous call.
struct Composition {
    ID3D11RenderTargetView *color = nullptr;
    ID3D11DepthStencilView *depth = nullptr;
    // True only when `depth` is confirmed to be the engine's own scene depth
    // at this point in the frame. Guards against drawing a depth-tested guest
    // against an unrelated surface, which is worse than not drawing at all.
    //
    // The backend cannot verify this independently: it would need
    // ID3D11Resource::GetDesc to compare the colour and depth sample counts,
    // and that query is unavailable in the header set this builds against (see
    // docs/REMASTERED_FINDINGS.md). The caller's assertion is therefore the
    // only gate, and it must be made from a real observation.
    bool depthVerified = false;
    // Target size in pixels. When zero, the backend reads the context's
    // currently bound viewport rather than inventing a size.
    UINT width = 0, height = 0;
    // True when `color` and `depth` are already bound on the context by the
    // engine (Remastered injects at the end of the engine's own main pass).
    // The backend then leaves the render targets alone: rebinding them would
    // drop the pass's second target and re-enter the host's OMSetRenderTargets
    // hook.
    bool targetsBound = false;
};

// Resource lifetime. One instance per device; a device change or an explicit
// reset() drops every GPU object so they are rebuilt lazily. Mirrors PTDE's
// "CPU images stay immutable and alive for the host session; Reset clears
// backend textures for lazy re-upload".
class Resources {
public:
    ID3D11Device *device = nullptr;

    ID3D11VertexShader *vs[3] = {nullptr, nullptr, nullptr};
    ID3D11InputLayout *layout[3] = {nullptr, nullptr, nullptr};
    ID3D11PixelShader *ps = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    ID3D11DepthStencilState *depthState[4] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11BlendState *blendState[2] = {nullptr, nullptr};
    ID3D11SamplerState *sampler[2] = {nullptr, nullptr};
    ID3D11Buffer *constants = nullptr;
    ID3D11Buffer *vertices = nullptr;
    size_t vertexCapacity = 0;

    struct Image {
        const uint8_t *rgba;
        uint32_t width, height;
        ID3D11Texture2D *texture;
        ID3D11ShaderResourceView *view;
    };
    static const size_t kMaxImages = 64;
    Image images[kMaxImages];
    size_t imageCount = 0;

    ~Resources() { release(); }

    template <typename T> static void release(T *&p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    void release() {
        for (auto &p : depthState)
            release(p);
        for (auto &p : blendState)
            release(p);
        for (auto &p : sampler)
            release(p);
        for (auto &p : layout)
            release(p);
        for (auto &p : vs)
            release(p);
        release(ps);
        release(raster);
        release(constants);
        release(vertices);
        vertexCapacity = 0;
        for (size_t i = 0; i < imageCount; ++i) {
            release(images[i].view);
            release(images[i].texture);
        }
        imageCount = 0;
    }

    // Drops GPU state only. The host keeps its CPU images alive, so they are
    // simply re-uploaded on next use - exactly the PTDE reset behaviour.
    void reset() { release(); }
};

// D3DCompile is resolved from d3dcompiler_47.dll at run time rather than
// linked. Reasons, both observed on the build host:
//   * the project's pinned Zig/mingw toolchain compiles d3d11 but ships no
//     d3dcompiler import library;
//   * a proxy DLL that statically imports the shader compiler adds a DLL to the
//     game's load set, which is best avoided in an injected process.
// A missing compiler degrades to "backend cannot draw", never to a guess.
struct Compiler {
    using Fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                 ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                 ID3DBlob **);
    Fn compile = nullptr;

    Compiler() {
        static const wchar_t *candidates[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll",
                                              L"d3dcompiler_45.dll", L"d3dcompiler_43.dll"};
        for (const wchar_t *name : candidates) {
            HMODULE m = LoadLibraryW(name);
            if (!m)
                continue;
            compile = reinterpret_cast<Fn>(
                reinterpret_cast<void *>(GetProcAddress(m, "D3DCompile")));
            if (compile)
                return;
            FreeLibrary(m);
        }
    }
};

// ---------------------------------------------------------------------------
// Shaders
//
// Conventions, stated explicitly because this is where a D3D9 -> D3D11
// translation is easiest to get silently wrong:
//
//   * render_api.h supplies ROW-MAJOR matrices under the D3D9 ROW-VECTOR
//     convention (v' = v * M). D3D11/HLSL has no such fixed convention, so the
//     backend transposes each matrix on upload and uses mul(vector, matrix).
//     That reproduces the D3D9 result bit-for-bit rather than relying on HLSL
//     defaults.
//   * colour is packed AARRGGBB (see core/character_presentation.h), so it is
//     unpacked explicitly instead of depending on a fixed-function channel.
//   * `shade` is a grayscale AARRGGBB light multiplier (character_presentation.h
//     writes 0xff000000 | g<<16 | g<<8 | g). D3D9 fed it through a SPECULAR
//     stage whose behaviour with lighting disabled was ambiguous; DX11 states
//     the intent directly as an RGB multiply.
//   * screen vertices carry x,y in target pixels plus a reciprocal W, matching
//     D3D9 XYZRHW, and ignore the camera matrices.
// ---------------------------------------------------------------------------
namespace shaders {
inline const char *worldVS = R"HLSL(
cbuffer C : register(b0) { float4x4 world; float4x4 view; float4x4 proj; float4 vpSize; };
struct In  { float3 pos : POSITION; uint color : COLOR; uint shade : SHADE; float2 uv : TEXCOORD; };
struct Out { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; nointerpolation uint shade : SHADE; };
Out main(In i) {
    float4 p = mul(mul(mul(float4(i.pos, 1.0f), world), view), proj);
    Out o;
    o.pos = p;
    o.color = float4(((i.color >> 16) & 0xffu), ((i.color >> 8) & 0xffu), (i.color & 0xffu),
                     ((i.color >> 24) & 0xffu)) / 255.0f;
    o.uv = i.uv;
    o.shade = i.shade;
    return o;
}
)HLSL";

inline const char *lineVS = R"HLSL(
cbuffer C : register(b0) { float4x4 world; float4x4 view; float4x4 proj; float4 vpSize; };
struct In  { float3 pos : POSITION; uint color : COLOR; };
struct Out { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; nointerpolation uint shade : SHADE; };
Out main(In i) {
    float4 p = mul(mul(mul(float4(i.pos, 1.0f), world), view), proj);
    Out o;
    o.pos = p;
    o.color = float4(((i.color >> 16) & 0xffu), ((i.color >> 8) & 0xffu), (i.color & 0xffu),
                     ((i.color >> 24) & 0xffu)) / 255.0f;
    o.uv = float2(0.0f, 0.0f);
    o.shade = 0xffffffffu;
    return o;
}
)HLSL";

inline const char *screenVS = R"HLSL(
cbuffer C : register(b0) { float4x4 world; float4x4 view; float4x4 proj; float4 vpSize; };
struct In  { float4 pos : POSITION; uint color : COLOR; float2 uv : TEXCOORD; };
struct Out { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; nointerpolation uint shade : SHADE; };
Out main(In i) {
    Out o;
    // x,y are target pixels; i.pos.w is the reciprocal homogeneous W.
    float ndcX = (i.pos.x - vpSize.x) * 2.0f / vpSize.z - 1.0f;
    float ndcY = 1.0f - (i.pos.y - vpSize.y) * 2.0f / vpSize.w;
    float w = i.pos.w;
    o.pos = float4(ndcX * w, ndcY * w, i.pos.z * w, w);
    o.color = float4(((i.color >> 16) & 0xffu), ((i.color >> 8) & 0xffu), (i.color & 0xffu),
                     ((i.color >> 24) & 0xffu)) / 255.0f;
    o.uv = i.uv;
    o.shade = 0xffffffffu;
    return o;
}
)HLSL";

// One pixel shader for every material variant. Colour mode, shading and the
// alpha cutoff arrive as constants so the pipeline-state count stays small
// instead of multiplying per feature.
inline const char *pixelPS = R"HLSL(
cbuffer C : register(b0) {
    float4x4 world; float4x4 view; float4x4 proj; float4 vpSize;
    int   colorMode;   // 0 vertex, 1 modulate, 2 texture-alpha
    int   shadeOn;
    float alphaCutoff; // normalised; 0 disables the test
};
Texture2D<float4> image : register(t0);
SamplerState      imageSampler : register(s0);
// Must match the vertex shaders' output signature exactly, SV_POSITION
// included: a pixel shader whose inputs do not line up register-for-register
// with the vertex outputs reads zeros.  That produced black output in the
// Remastered test draw (docs/REMASTERED_FINDINGS.md section 22).
struct In  { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; nointerpolation uint shade : SHADE; };
struct Out { float4 color : SV_TARGET; };
Out main(In i) {
    float4 base = i.color;
    float4 result;
    if (colorMode == 0) {
        result = base;
    } else if (colorMode == 1) {
        result = base * image.Sample(imageSampler, i.uv);
    } else {
        // D3D9 D3DTOP_BLENDTEXTUREALPHA: blend from the vertex colour to the
        // texture by the TEXTURE's alpha (rgb = tex*tex.a + diffuse*(1-tex.a)).
        // libsm64 relies on this: untextured triangles sample a transparent
        // texel and keep their vertex colour.  The earlier "texture RGB scaled by
        // vertex alpha" turned every untextured part of Mario black (live,
        // 2026-10-19).
        float4 t = image.Sample(imageSampler, i.uv);
        result = float4(lerp(base.rgb, t.rgb, t.a), base.a);
        // Wing Cap (v170o): the last two atlas tiles (u 9/11..1) are the wing halves, a cut-out:
        // their transparent texels are holes, not the vertex colour (that drew a white square).
        // Untextured triangles sit exactly at (1, 1), the atlas' last (transparent) texel.
        const bool untextured = i.uv.x > 0.9995f && i.uv.y > 0.9995f;
        if (!untextured && i.uv.x >= 9.0f / 11.0f && i.uv.x <= 1.0f && i.uv.y >= 0.0f && i.uv.y <= 1.0f) {
            if (t.a < 0.5f) discard;
            result.a = t.a;
        }
    }
    if (shadeOn) {
        float3 s = float3(((i.shade >> 16) & 0xffu), ((i.shade >> 8) & 0xffu),
                          (i.shade & 0xffu)) / 255.0f;
        result.rgb *= s;
    }
    if (alphaCutoff > 0.0f && result.a < alphaCutoff)
        discard;
    Out o;
    o.color = result;
    return o;
}
)HLSL";
} // namespace shaders

// ---------------------------------------------------------------------------
// Pipeline construction
// ---------------------------------------------------------------------------
enum Kind : uint32_t { kWorld = 0, kLine = 1, kScreen = 2 };

inline bool compileShader(Compiler::Fn fn, const char *source, const char *entry,
                          const char *target, ID3DBlob **blob) {
    ID3DBlob *errors = nullptr;
    HRESULT hr = fn(source, std::strlen(source), "mariosouls", nullptr, nullptr, entry, target,
                    0, 0, blob, &errors);
    if (errors)
        errors->Release();
    return SUCCEEDED(hr) && blob && *blob;
}

inline bool prepareShaders(Resources &r) {
    if (r.ps)
        return true;
    if (!r.device)
        return false;

    // Bytecode is compiled at build time by tools/compile_test_shaders.py from
    // the HLSL strings above (render_shaders_gen.h), so compile errors surface in
    // the build log and the game process never loads d3dcompiler.  A runtime
    // compile here first failed silently in game (2026-10-19).
    struct Blob { const void *p; SIZE_T n; };
    const Blob world{kRenderWorldVS, sizeof(kRenderWorldVS)};
    const Blob line{kRenderLineVS, sizeof(kRenderLineVS)};
    const Blob screen{kRenderScreenVS, sizeof(kRenderScreenVS)};
    const Blob pixel{kRenderPixelPS, sizeof(kRenderPixelPS)};
    bool ok = SUCCEEDED(r.device->CreateVertexShader(world.p, world.n, nullptr, &r.vs[kWorld])) &&
              SUCCEEDED(r.device->CreateVertexShader(line.p, line.n, nullptr, &r.vs[kLine])) &&
              SUCCEEDED(r.device->CreateVertexShader(screen.p, screen.n, nullptr, &r.vs[kScreen])) &&
              SUCCEEDED(r.device->CreatePixelShader(pixel.p, pixel.n, nullptr, &r.ps));
    if (!ok)
        return false;

    // Input layouts mirror the neutral vertex structs byte for byte.
    const D3D11_INPUT_ELEMENT_DESC worldElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SHADE", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const D3D11_INPUT_ELEMENT_DESC lineElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const D3D11_INPUT_ELEMENT_DESC screenElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};

    ok = SUCCEEDED(r.device->CreateInputLayout(worldElements, 4, world.p, world.n, &r.layout[kWorld])) &&
         SUCCEEDED(r.device->CreateInputLayout(lineElements, 2, line.p, line.n, &r.layout[kLine])) &&
         SUCCEEDED(r.device->CreateInputLayout(screenElements, 3, screen.p, screen.n, &r.layout[kScreen]));
    if (!ok)
        return false;

    // Cull none, no scissor, front counter-clockwise - matches PTDE's
    // D3DCULL_NONE and D3DRS_SCISSORTESTENABLE FALSE.
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = FALSE;
    ok = SUCCEEDED(r.device->CreateRasterizerState(&raster, &r.raster));

    for (int i = 0; i < 2 && ok; ++i) {
        D3D11_SAMPLER_DESC s{};
        s.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.ComparisonFunc = D3D11_COMPARISON_NEVER;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(r.device->CreateSamplerState(&s, &r.sampler[i]));
    }

    // Depth states: 4 combinations of (test, write); PTDE uses LESSEQUAL.
    for (int i = 0; i < 4 && ok; ++i) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = (i & 1) ? TRUE : FALSE;
        d.DepthWriteMask = (i & 2) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ok = SUCCEEDED(r.device->CreateDepthStencilState(&d, &r.depthState[i]));
    }

    D3D11_BLEND_DESC opaque{};
    opaque.RenderTarget[0].BlendEnable = FALSE;
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC blended = opaque;
    blended.RenderTarget[0].BlendEnable = TRUE;
    blended.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blended.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blended.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blended.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blended.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blended.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (ok)
        ok = SUCCEEDED(r.device->CreateBlendState(&opaque, &r.blendState[0])) &&
             SUCCEEDED(r.device->CreateBlendState(&blended, &r.blendState[1]));

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = 256;
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (ok)
        ok = SUCCEEDED(r.device->CreateBuffer(&constantDesc, nullptr, &r.constants));

    return ok;
}

inline ID3D11ShaderResourceView *upload(Resources &r, SCRenderAPI::Texture t) {
    if (!r.device)
        return nullptr;
    if (!t.rgba || !t.width || !t.height || t.width > 4096 || t.height > 4096)
        return nullptr;
    for (size_t i = 0; i < r.imageCount; ++i) {
        auto &image = r.images[i];
        if (image.rgba == t.rgba && image.width == t.width && image.height == t.height)
            return image.view;
    }
    if (r.imageCount >= Resources::kMaxImages)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = t.width;
    desc.Height = t.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // render_api.h supplies RGBA8 in memory order, so upload it as-is; the D3D9
    // backend needed a BGRA swizzle that has no equivalent here.
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = t.rgba;
    initial.SysMemPitch = t.width * 4;

    ID3D11Texture2D *texture = nullptr;
    if (FAILED(r.device->CreateTexture2D(&desc, &initial, &texture)))
        return nullptr;
    ID3D11ShaderResourceView *view = nullptr;
    if (FAILED(r.device->CreateShaderResourceView(texture, nullptr, &view))) {
        texture->Release();
        return nullptr;
    }
    // Cache by image identity: render_api.h guarantees stable, immutable
    // addresses for the host session.
    auto &slot = r.images[r.imageCount++];
    slot.rgba = t.rgba;
    slot.width = t.width;
    slot.height = t.height;
    slot.texture = texture;
    slot.view = view;
    return view;
}

} // namespace DSRGraphics

// ---------------------------------------------------------------------------
// State restoration
//
// D3D11 has no equivalent of IDirect3DDevice9 state blocks, so this scope
// records exactly the context state the backend disturbs and restores all of it
// on every exit path, including early returns. Restoring what we changed - not
// "everything" - keeps the game's own state intact and documents our footprint.
// ---------------------------------------------------------------------------
class DSRStateScope {
    static const UINT kViewportSlots = 16;
    static const UINT kMaxInstances = 256;  // D3D11_SHADER_MAX_INTERFACES

    ID3D11DeviceContext *context = nullptr;
    bool restoreTargets = false;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11ClassInstance *vsInst[kMaxInstances] = {};
    ID3D11ClassInstance *psInst[kMaxInstances] = {};
    UINT vsInstCount = kMaxInstances, psInstCount = kMaxInstances;
    ID3D11InputLayout *layout = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    ID3D11DepthStencilState *depth = nullptr;
    UINT stencilRef = 0;
    ID3D11BlendState *blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11Buffer *vsConstants = nullptr;
    ID3D11Buffer *psConstants = nullptr;
    ID3D11Buffer *vertices = nullptr;
    UINT stride = 0, offset = 0;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT viewport[kViewportSlots]{};
    UINT viewportCount = 0;
    bool captured = false;

public:
    // Records every piece of context state drawPrimitives changes, exactly as
    // the engine left it (stencil reference, blend factor/mask, vertex stride and
    // offset and shader class instances included), and puts it back on
    // destruction.  Render targets are only recorded when the backend binds its
    // own (restoreTargets); in the engine-bound case they are never touched.
    DSRStateScope(ID3D11DeviceContext *c, bool withTargets) : context(c), restoreTargets(withTargets) {
        if (!context)
            return;
        if (restoreTargets)
            context->OMGetRenderTargets(1, &rtv, &dsv);
        context->VSGetShader(&vs, vsInst, &vsInstCount);
        context->PSGetShader(&ps, psInst, &psInstCount);
        context->IAGetInputLayout(&layout);
        context->RSGetState(&raster);
        context->OMGetDepthStencilState(&depth, &stencilRef);
        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        context->VSGetConstantBuffers(0, 1, &vsConstants);
        context->PSGetConstantBuffers(0, 1, &psConstants);
        context->IAGetVertexBuffers(0, 1, &vertices, &stride, &offset);
        context->PSGetShaderResources(0, 1, &srv);
        context->PSGetSamplers(0, 1, &sampler);
        context->IAGetPrimitiveTopology(&topology);
        viewportCount = kViewportSlots;
        context->RSGetViewports(&viewportCount, viewport);
        captured = true;
    }

    DSRStateScope(const DSRStateScope &) = delete;
    DSRStateScope &operator=(const DSRStateScope &) = delete;

    ~DSRStateScope() {
        if (!captured)
            return;
        if (restoreTargets) {
            if (rtv)
                context->OMSetRenderTargets(1, &rtv, dsv);
            else
                context->OMSetRenderTargets(0, nullptr, dsv);
        }
        context->VSSetShader(vs, vsInst, vsInstCount);
        context->PSSetShader(ps, psInst, psInstCount);
        context->IASetInputLayout(layout);
        context->RSSetState(raster);
        context->OMSetDepthStencilState(depth, stencilRef);
        context->OMSetBlendState(blend, blendFactor, sampleMask);
        context->VSSetConstantBuffers(0, 1, &vsConstants);
        context->PSSetConstantBuffers(0, 1, &psConstants);
        context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->IASetPrimitiveTopology(topology);
        if (viewportCount)
            context->RSSetViewports(viewportCount, viewport);
        IUnknown *held[] = {rtv,   dsv,         vs,          ps,       layout, raster, depth,
                            blend, vsConstants, psConstants, vertices, srv,    sampler};
        for (auto *p : held)
            if (p)
                p->Release();
        for (UINT i = 0; i < vsInstCount && i < kMaxInstances; ++i)
            if (vsInst[i]) vsInst[i]->Release();
        for (UINT i = 0; i < psInstCount && i < kMaxInstances; ++i)
            if (psInst[i]) psInst[i]->Release();
    }

    bool valid() const { return captured; }
};

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
class DSRRenderer final : public SCRenderAPI::Renderer {
    ID3D11DeviceContext *context = nullptr;
    DSRGraphics::Resources resources;
    DSRGraphics::Composition composition;
    SCRenderAPI::Viewport targetViewport{};

    static bool allowed(SCRenderAPI::Context c) {
        // Mirrors PTDE: a before-HUD submission without verified depth must not
        // draw over native UI.
        return c.phase != SCRenderAPI::Phase::BeforeHUD || c.depthAvailable;
    }

    // Depth-tested drawing requires the engine's own scene depth. Refusing is
    // the documented fallback; never bind a substitute surface.
    bool depthUsable(bool wanted) const {
        if (!wanted)
            return true;
        return composition.depth && composition.depthVerified;
    }

    void transpose16(const float *src, float *dst) const {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                dst[c * 4 + r] = src[r * 4 + c];
    }

    bool uploadVertices(const void *data, size_t bytes) {
        if (!context || !resources.device)
            return false;
        if (!data || !bytes)
            return false;
        if (bytes > resources.vertexCapacity) {
            DSRGraphics::Resources::release(resources.vertices);
            resources.vertexCapacity = 0;
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = UINT(bytes);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(resources.device->CreateBuffer(&desc, nullptr, &resources.vertices)))
                return false;
            resources.vertexCapacity = bytes;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(resources.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, data, bytes);
        context->Unmap(resources.vertices, 0);
        return true;
    }

    bool drawPrimitives(uint32_t kind, uint32_t count, const SCRenderAPI::Material &m,
                        SCRenderAPI::Context c, bool depthTest, bool depthWrite) {
        if (!context || !composition.color)
            return false;
        if (!DSRGraphics::prepareShaders(resources))
            return false;
        if (!allowed(c) || !depthUsable(depthTest))
            return false;

        ID3D11ShaderResourceView *srv = nullptr;
        if (m.color != SCRenderAPI::ColorMode::Vertex) {
            srv = DSRGraphics::upload(resources, m.image);
            if (!srv)
                return false;
        }

        uint32_t colorMode = m.color == SCRenderAPI::ColorMode::Vertex      ? 0
                             : m.color == SCRenderAPI::ColorMode::Modulate ? 1
                                                                          : 2;
        // Everything below changes context state; the scope restores it on
        // every exit path.
        DSRStateScope restore(context, !composition.targetsBound);
        bool alphaTest = m.alphaCutoff != 0;
        bool alphaBlend = m.alphaBlend;
        bool shade = m.shade;

        // Constants: transposed matrices, viewport and per-draw material state.
        float matrix[3 * 16];
        transpose16(cameraWorld, matrix);
        transpose16(cameraView, matrix + 16);
        transpose16(cameraProjection, matrix + 32);
        float vp[4] = {targetViewport.x, targetViewport.y, targetViewport.width,
                       targetViewport.height};
        // Layout must match the HLSL cbuffer exactly: three float4x4, one
        // float4, then four scalars.
        struct Constants {
            float matrix[3 * 16];
            float viewport[4];
            int colorMode;
            int shadeOn;
            float alphaCutoff;
            float pad;
        } constantsData;
        std::memcpy(constantsData.matrix, matrix, sizeof(matrix));
        std::memcpy(constantsData.viewport, vp, sizeof(vp));
        constantsData.colorMode = int(colorMode);
        constantsData.shadeOn = shade ? 1 : 0;
        constantsData.alphaCutoff = alphaTest ? float(m.alphaCutoff) / 255.0f : 0.0f;
        constantsData.pad = 0.0f;
        static_assert(sizeof(Constants) <= 256, "constant buffer stride");

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(resources.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, &constantsData, sizeof(constantsData));
        context->Unmap(resources.constants, 0);

        D3D11_VIEWPORT d3dViewport{};
        d3dViewport.TopLeftX = targetViewport.x;
        d3dViewport.TopLeftY = targetViewport.y;
        d3dViewport.Width = targetViewport.width;
        d3dViewport.Height = targetViewport.height;
        d3dViewport.MinDepth = 0.0f;
        d3dViewport.MaxDepth = 1.0f;

        if (!composition.targetsBound) {
            ID3D11RenderTargetView *rtv = composition.color;
            context->OMSetRenderTargets(1, &rtv, depthTest ? composition.depth : nullptr);
        }
        context->RSSetState(resources.raster);
        context->RSSetViewports(1, &d3dViewport);
        context->IASetPrimitiveTopology(kind == DSRGraphics::kLine
                                             ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                             : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(resources.vs[kind], nullptr, 0);
        context->PSSetShader(resources.ps, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &resources.constants);
        // The pixel shader reads the same constants (colour mode, shading,
        // alpha cutoff); without this it read whatever the engine had bound.
        context->PSSetConstantBuffers(0, 1, &resources.constants);
        context->IASetInputLayout(resources.layout[kind]);
        UINT stride = UINT(kind == DSRGraphics::kLine ? sizeof(SCRenderAPI::LineVertex)
                                                       : sizeof(SCRenderAPI::Vertex));
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &resources.vertices, &stride, &offset);
        context->PSSetShaderResources(0, 1, &srv);
        int filter = m.filter == SCRenderAPI::Filter::Point ? 0 : 1;
        ID3D11SamplerState *samplerState = resources.sampler[filter];
        context->PSSetSamplers(0, 1, &samplerState);
        context->OMSetDepthStencilState(
            resources.depthState[(depthTest ? 1 : 0) | (depthWrite ? 2 : 0)], 0);
        context->OMSetBlendState(resources.blendState[alphaBlend ? 1 : 0], nullptr, 0xFFFFFFFFu);

        // Everything the rasteriser needs is now bound discretely. D3D11 also
        // offers ID3D11PipelineState / GSSetPipelineState to bundle these, but
        // it is optional and deliberately unused: binding the state objects
        // individually keeps the footprint obvious, matches how PTDE's D3D9
        // backend already sets state per draw, and avoids declaring a COM
        // interface the installed headers do not provide.
        context->Draw(count, 0);
        return true;
    }

    float cameraWorld[16]{};
    float cameraView[16]{};
    float cameraProjection[16]{};

public:
    DSRRenderer(ID3D11Device *device, ID3D11DeviceContext *c) : context(c) {
        resources.device = device;
    }

    // Remastered submits on whichever deferred context is recording the main
    // pass, so the context changes between submissions.  Callers serialise
    // submissions: the dynamic vertex/constant buffers are shared.
    void bind(ID3D11DeviceContext *c) { context = c; }

    // Compile shaders and create pipeline objects ahead of the first draw, off
    // the render threads.
    bool prepare() { return DSRGraphics::prepareShaders(resources); }

    // Call once per composition, before any submission. A device change drops
    // all GPU state; CPU images stay owned by the host and re-upload lazily.
    bool compose(ID3D11Device *device, DSRGraphics::Composition targets) {
        if (!device || device != resources.device) {
            resources.reset();
            resources.device = device;
            targetViewport = {};
            composition = {};
        }
        composition = targets;
        if (!context || !composition.color)
            return false;

        // Target size: the caller states it when it knows, otherwise the
        // engine's currently bound viewport is used. Never assumed.
        UINT width = composition.width, height = composition.height;
        if (!width || !height) {
            UINT count = 1;
            D3D11_VIEWPORT bound[1]{};
            context->RSGetViewports(&count, bound);
            if (count && bound[0].Width > 0.0f && bound[0].Height > 0.0f) {
                width = UINT(bound[0].Width);
                height = UINT(bound[0].Height);
            }
        }
        if (!width || !height)
            return false;
        targetViewport = {0.0f, 0.0f, float(width), float(height)};
        return true;
    }

    void shutdown() {
        resources.reset();
        composition = {};
        targetViewport = {};
    }

    bool texture(SCRenderAPI::Texture t) override {
        if (!resources.device)
            return false;
        return DSRGraphics::upload(resources, t) != nullptr;
    }

    SCRenderAPI::Viewport viewport() const override { return targetViewport; }

    void camera(const SCRenderAPI::Camera &c) override {
        std::memcpy(cameraWorld, c.world, sizeof(cameraWorld));
        std::memcpy(cameraView, c.view, sizeof(cameraView));
        std::memcpy(cameraProjection, c.projection, sizeof(cameraProjection));
    }

    bool mesh(SCRenderAPI::Mesh m, const SCRenderAPI::Material &style,
              SCRenderAPI::Context c) override {
        if (!m.triangles)
            return true;
        if (!m.vertices)
            return false;
        if (!uploadVertices(m.vertices, size_t(m.triangles) * 3 * sizeof(SCRenderAPI::Vertex)))
            return false;
        return drawPrimitives(DSRGraphics::kWorld, m.triangles * 3, style, c, style.depthTest,
                              style.depthWrite);
    }

    bool lines(SCRenderAPI::Lines m, bool depthTest, SCRenderAPI::Context c) override {
        SCRenderAPI::Material style{};
        style.depthTest = depthTest;
        style.depthWrite = false;
        if (!m.segments)
            return true;
        if (!m.vertices)
            return false;
        if (!uploadVertices(m.vertices, size_t(m.segments) * 2 * sizeof(SCRenderAPI::LineVertex)))
            return false;
        return drawPrimitives(DSRGraphics::kLine, m.segments * 2, style, c, depthTest, false);
    }

    bool screen(SCRenderAPI::ScreenMesh m, const SCRenderAPI::Material &style,
                SCRenderAPI::Context c) override {
        if (!m.triangles)
            return true;
        if (!m.vertices)
            return false;
        if (!uploadVertices(m.vertices,
                            size_t(m.triangles) * 3 * sizeof(SCRenderAPI::ScreenVertex)))
            return false;
        // Screen quads ignore depth, matching PTDE's XYZRHW path.
        return drawPrimitives(DSRGraphics::kScreen, m.triangles * 3, style, c, false, false);
    }
};