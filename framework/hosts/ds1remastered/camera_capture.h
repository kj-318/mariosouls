#pragma once
#include <cmath>

// Remastered host camera capture.
//
// The view-projection matrix is not stored in a persistent CPU-side camera
// structure.  Instead, it is computed by the game and uploaded to dynamic
// constant buffers (size 2288 or 4112) via Map/Unmap.  This module hooks
// Map/Unmap on all contexts to capture the matrix and cache it for rendering.
//
// The captured matrix is a combined view-projection:
//   m[0][0] = 1.425 (aspect-corrected FOV)
//   m[1][1] = 2.539 (1/tan(fov/2))
//   m[2][2] = 1.0 (infinite far plane)
//
// Usage:
//   1. Call armCameraCapture(device, context) after device creation.
//   2. Call getCameraState() to get the cached matrix.
//   3. Check CameraState::valid before using the matrix.

#include "native_types.h"

#include <d3d11.h>
// ID3D11DeviceContext1..4 are declared in dxgi1_3.h/d3d11_4.h rather than
// d3d11.h, and are needed to identify which interface a deferred context
// actually implements.
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>

#include "logging.h"

namespace remastered {

// Internal camera state (C++17 inline variable - single definition across TUs).
inline CameraState g_cameraState = {};

// Get the current camera state (thread-safe read).
// Returns a copy of the internally cached CameraState.
inline CameraState getCameraState() { return g_cameraState; }

// Check if a buffer is a view-matrix buffer (size 2288 or 4112).
inline bool isViewMatrixBuffer(UINT byteWidth) { return byteWidth == 2288 || byteWidth == 4112; }

// Internal access to camera state (for hooks).
// Returns reference to the internal static camera state.
inline CameraState &getCameraStateInternal() { return g_cameraState; }

// MRT counts above this are recorded as this many; the geometry pass uses far
// fewer, and the count is only part of a binding's fingerprint.
static const int kMaxRTViews = 8;

// Context hooks for camera capture (Map/Unmap).
//
// Map bookkeeping is per context.  A single shared "last mapped" pointer races
// once deferred contexts are patched: two contexts can interleave Map/Unmap and
// the wrong buffer's memory would be read as a matrix.
struct CamCtxHooks {
    void *ctx;
    void **vt;
    void *map;
    void *unmap;
    void *omSetRenderTargets;
    // Per-context pending map, consumed on the matching Unmap.
    void *mapData;
    void *mapResource;
    UINT mapByteWidth;
    // fxprobe=on: pending map of a dynamic vertex / shader-resource buffer.
    void *fxData;
    void *fxRes;
    UINT fxBytes, fxBind;
    // Render targets currently bound on this context.  Recorded so a pass can
    // be recognised from what is actually attached rather than from the order
    // of calls, and borrowed only -- never released, the game owns them.
    void *rtv[kMaxRTViews];
    void *depthView;
    UINT rtCount;
    // Whether the binding currently attached to this context matched the
    // geometry-pass signature.  The test draw runs when the game moves *off*
    // that binding, so it lands after the game's own clears and draws for the
    // pass, with the pass's viewport already set.
    bool geometryPassBound;
    double passStart;  // when the current geometry-pass binding began (QPC seconds)
    // Non-null colour targets in that binding: 2 for the multi-target
    // candidate (#4), 1 for the single-target one (#5).
    UINT geometryPassTargets;
    // The main-scene view-projection most recently uploaded on THIS context,
    // with the camera/player positions read when it changed.  Several deferred
    // contexts record in parallel, possibly for different frames, so the global
    // "latest" matrix can belong to a neighbouring frame; geometry drawn on this
    // context must use this context's matrix, the one the engine's own draws in
    // the same pass use (stutter observed live with the global one, 2026-10-19).
    bool vpValid;
    float vp[16];
    bool positionsValid;
    float cameraPos[3];
    float playerPos[3];
    // The main-camera eye most recently uploaded on THIS context, with the
    // facing of the view uploaded beside it.  When the camera only slides
    // behind the walking player its facing stays the same for many frames, so
    // facing alone cannot pick the drawn frame's eye; the context that records
    // the pass uploads that frame's eye (live 2026-10-19).
    bool eyeValid;
    float eye[3];
    float eyeForward[3];
    float eyeVp[16];  // view-projection uploaded beside that eye
    DWORD eyeTick;
    // The player's transform as the engine uploaded it on THIS context: from
    // a 2288-byte buffer whose offset 0 is the view-projection that draws it,
    // with an array of camera-relative 3x4 transforms from float 112 (stride
    // 12).  View-projection, transform and camera (the origin) all come from
    // one upload, so they belong to the same frame by construction (probe,
    // 2026-10-19).  upRot holds the three axis columns.
    bool upValid;
    DWORD upTick;
    UINT upIndex;
    float upVp[16];
    float upPos[3];
    float upRot[9];
    double upT;  // QPC seconds of that upload
    // Near-upright transforms near the player seen in this context's current
    // geometry pass, with how many uploads carried each exact one.  The
    // character's root is shared by every body-part draw, so it is the most
    // repeated; bones and held items appear in few (live 2026-10-20).
    struct PassCand { float pos[3], rot[9], vp[16]; int count; unsigned long long hash; float m[12]; };
    PassCand passCands[64];
    int passCandN;
    // Hiding the player's own model (hide=on): the original DrawIndexed, the
    // last 2288-byte bone upload on this context (if near the player) as
    // per-slot matrix hashes, and the draws recorded after such uploads in
    // the current pass.  Classified when the pass ends.
    void *drawIndexed;
    void *drawIndexedInstanced;
    void *draw;
    void *drawAuto;
    void *lastUpIb, *lastUpVb;  // buffers of the first draw after the upload
    bool lastUpNear;
    bool lastUpPlayer;  // first draw after this upload was a learned player mesh
    int lastUpDraws;
    unsigned long long lastUpHash[38];
    unsigned long long lastUpRigid;  // bit j: slot j is a rigid matrix
    unsigned long long prevUpHash[38];  // the 2288 upload before the last one
    bool lastUpLeftover;  // later slots repeat the previous upload's (leftovers)
    unsigned long long lastUpHeader, prevUpHeader;  // hash of floats 16..111
    bool lastUpCopied;
    bool shadowPass;  // current binding is the shadow map
    void *shadowDsvSeen; bool shadowDsvIs;  // per-context cache of that test
    UINT dsvW, dsvH;  // size of the bound depth target (cached with the above)
    bool offscreenPass;  // depth target smaller than the main pass's
    // Shadow-map bone upload: rotation-only hash per palette slot (bone
    // positions there are relative to the light's camera).
    unsigned long long shadowUpRot[38];
    bool shadowUpValid;
    // Shadow pass (v161): the last upload's skinned bones lie at the knight; the draws of
    // that piece (same buffers, at most 8) are skipped, so his shadow is never drawn.
    bool shadowNear;
    int shadowNearDraws;
    void *shadowNearIb, *shadowNearVb;
    // v168: fingerprint of this context's last main-pass palette upload, committed to the
    // knight's set when a learned knight piece is drawn with it.
    bool fpPending;
    int fpRef;
    uint64_t fpUsed;
    float fpRot[38][9];
    int shadowUpDraws;  // whole palette repeats the previous upload, header differs
    struct PassDraw { unsigned long long key, ikey, hash[38], rigidMask; UINT indexCount; bool hip0, leftover, copied; void *vs; };
    PassDraw passDraws[128];
    bool lastUpHip0;  // slot 0 puts the hip joint within 1 m of the player
    bool lastUpHeld;  // v170: a 1-2 bone piece at the knight's hands while he drinks
    int lastUpHeldDraws;
    int passDrawN;
};

// Latest player root transform from any context, with the view-projection of
// the same upload: the fallback for a context that has not uploaded one.
struct UploadAnchor { DWORD tick; UINT index; float vp[16], pos[3], rot[9], eye[3]; double t; };
inline SRWLOCK g_upLock = SRWLOCK_INIT;
inline UploadAnchor g_upLatest = {};
inline bool g_upLatestValid = false;
// Recent anchors, each with the camera position of its own upload, so a draw
// can take the one from its own frame (same eye) or move a neighbouring
// frame's into its own camera space.
inline UploadAnchor g_upRing[16] = {};
inline LONG g_upRingHead = 0;
// Diagnostics for the capture chain: plausible 2288-byte uploads, positions
// read, uploads with candidates, lock available, transform picked.
inline volatile LONG g_upDiag[5] = {};

// ---- Hiding the player's model --------------------------------------------
// Draw keys learned as the player's meshes, with how many frames voted for
// each.  A key is skipped (hide=on) once it has 3 or more votes.
struct HideEntry { unsigned long long key, ikey; LONG votes; DWORD lastTick; UINT indexCount; int rule, bones; };
inline HideEntry g_hideSet[1024] = {};
inline SRWLOCK g_hideLock = SRWLOCK_INIT;
inline volatile LONG g_hidePlayer = 0;
inline volatile unsigned long long g_lastPelvisHash = 0;
inline volatile DWORD g_lastPelvisTick = 0;
// Recent pelvis matrices (hashes) chosen by the anchor rule, from any
// context: a pass on one deferred context can hold the head or arms while
// another holds the pelvis of the same frame.
inline volatile unsigned long long g_pelvisRing[16] = {};
inline volatile LONG g_pelvisRingHead = 0;
// Bone matrices (hashes) of pieces recently classified as the player's, from
// any context, so arms and head recorded on another context still find two
// shared bones.
// Records from all contexts, classified together once per frame (about
// 40 ms of passes) so the order in which contexts finish does not matter.
struct PendingDraw { unsigned long long key, ikey, hash[38], rigidMask; UINT indexCount; bool hip0, leftover, copied; int rule; void *vs; };
inline PendingDraw g_pend[2048] = {};
inline LONG g_pendN = 0;
inline DWORD g_pendTick = 0;
inline SRWLOCK g_pendLock = SRWLOCK_INIT;
// near-player draws, recorded, classified as player, skipped, passes classified
inline volatile LONG g_hideDiag[5] = {};
// skipped right after a player upload, skipped by key alone (other passes),
// instanced draws skipped, instanced draws seen
inline volatile LONG g_hideDiag2[4] = {};
inline volatile LONG g_hideTrusted = 0;  // keys with 3 or more votes
// Index-only keys (index buffer, offset, range; no vertex buffer) of trusted
// meshes: the same mesh drawn from another vertex buffer (pre-skinned) still
// matches.
inline volatile unsigned long long g_hideIkeys[256] = {};
inline volatile LONG g_hideIkeyN = 0;
// Stream-output buffers bound while a player mesh was drawn: if the game
// skins into a buffer and draws that later, draws from it are the player's.
inline void *volatile g_hideSoBufs[64] = {};
inline volatile LONG g_hideSoN = 0;
// skipped by index key only (vertex buffer differs), player draws with a
// stream-output target, with a geometry shader, Draw calls, DrawAuto calls,
// skipped from a player stream-output buffer
inline volatile LONG g_hideDiag3[6] = {};
// Which hiding methods are active (hidemask=<hex>, live): 1 the learned key
// right after the player's bone upload, 2 the learned key in any other pass,
// 4 the index-only key, 8 draws from player stream-output buffers.
inline volatile LONG g_hideMask = 0x3D;
// Diagnostics (diag=on, live; default off): the probes below that read every
// upload or every draw, and the detailed hide logging.
inline volatile LONG g_diag = 0;
// Hide: an upload counts as the knight's when a bone is within this many centimetres of him
// horizontally (hideradius=, metres).  It was 2.5 m, which took NPCs standing next to him.
inline volatile LONG g_hideRadiusCm = 120;
inline float g_hipRadius2 = 0.25f;  // hip test radius squared (hiphide=<cm>, v170n)
inline volatile LONG g_shadowHideOn = 0;
inline volatile LONG g_shadowKeysOn = 0;
// Shadow by bone fingerprint (v166, option 1): the knight's main-pass palettes (rotation part of
// each bone, which is the same in every pass) are kept for 150 ms; a shadow-pass palette that
// matches one within tolerance is his, and that piece's shadow draws are skipped.
inline volatile LONG g_shadowFpOn = 0;  // shadowfp=on enables (off by default, v170: the user wants it solid)
// v167: rotations relative to the palette's first bone (invariant to the light's rotation of
// the shadow pass, which made absolute rotations never match).
struct PaletteFp { DWORD tick; uint64_t used; int ref; float rot[38][9]; };
// rot (row-major 3x3 per bone, columns normalised) -> rot of each bone relative to bone ref.
inline int relativeRotations(float rot[38][9], uint64_t used) {
    int ref = -1;
    for (int j = 0; j < 38; ++j) if (used & (1ull << j)) { ref = j; break; }
    if (ref < 0) return -1;
    for (int j = 0; j < 38; ++j) {
        if (!(used & (1ull << j))) continue;
        float *r = rot[j];
        for (int c = 0; c < 3; ++c) {  // normalise columns (scale removed)
            const float l = std::sqrt(r[c] * r[c] + r[3 + c] * r[3 + c] + r[6 + c] * r[6 + c]);
            if (l > 1e-6f) { r[c] /= l; r[3 + c] /= l; r[6 + c] /= l; }
        }
    }
    float R0[9];
    memcpy(R0, rot[ref], sizeof(R0));
    for (int j = 0; j < 38; ++j) {
        if (!(used & (1ull << j))) continue;
        float out[9];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                out[a * 3 + b] = R0[0 * 3 + a] * rot[j][0 * 3 + b] + R0[1 * 3 + a] * rot[j][1 * 3 + b] + R0[2 * 3 + a] * rot[j][2 * 3 + b];
        memcpy(rot[j], out, sizeof(out));
    }
    return ref;
}
inline volatile LONG g_sdFpBestErrMilli = 100000;
inline PaletteFp g_palFp[128] = {};
inline volatile LONG g_palFpNext = 0;
inline SRWLOCK g_palFpLock = SRWLOCK_INIT;
inline volatile LONG g_sdFpTested = 0, g_sdFpMatched = 0;  // shadowkeys=on: learned-key skipping in the shadow pass  // shadowhide=off disables the shadow-pass position test
inline volatile LONG g_shadowNearSkips = 0;
// Shadow summary (always logged every 5 s, v162): depth-only bindings by size, shadow-pass
// bone uploads, those at the knight, and shadow-pass draws seen / skipped.
inline volatile LONG g_sdBinds = 0, g_sdShadowBinds = 0, g_sdUploads = 0, g_sdNear = 0, g_sdDraws = 0;
inline volatile LONG g_sdSizeW[6] = {}, g_sdSizeH[6] = {}, g_sdSizeN[6] = {}, g_sdSizeShadow[6] = {};
// Index counts never skipped (hidenot=a,b,c; live): to find impostors.
inline volatile LONG g_hideNot[16] = {};
inline volatile LONG g_hideNotN = 0;
// Shadow pass (depth-only, square target of 1024 or more): learned keys
// skipped there with hidemask bit 0x10; seen / matched / skipped counts.
inline volatile LONG g_shadowDiag[3] = {};
// Main pass depth size (from the first skip right after a player upload), and
// skips in smaller-depth passes (hidemask bit 0x20: half-res AO / screen-space
// shadow passes).
inline volatile LONG g_mainDsvW = 0, g_mainDsvH = 0;
inline volatile LONG g_offscreenSkips = 0;
// Shadow-map draws queued for classification by bone rotation.
struct ShadowPending { unsigned long long key, ikey, rot[38]; UINT indexCount; };
inline ShadowPending g_shadowPend[2048] = {};
inline LONG g_shadowPendN = 0;
inline SRWLOCK g_shadowPendLock = SRWLOCK_INIT;
inline volatile LONG g_shadowLearnDiag[2] = {};  // shadow records queued, classified as player
// Untrusted shadow-pass draws by index count (diagnostic).
struct ShadowMiss { UINT indexCount; LONG n; };
inline ShadowMiss g_shadowMiss[48] = {};
inline volatile LONG g_shadowMissN = 0;
inline SRWLOCK g_shadowMissLock = SRWLOCK_INIT;
// Face-sized draws (2903/1280/308 indices): seen, after any 2288 upload, recorded
inline volatile LONG g_faceDiag[3] = {};
// Shadow-pass probe: 2288-byte uploads whose palette holds this frame's
// pelvis (any slot) but whose matrix at offset 0 is not a camera
// view-projection: counts per render-target binding, and a sample matrix.
struct ShadowProbe { void *rtv0, *dsv; UINT w, h, fmt, rtCount; LONG n, plausible; float vp[16]; UINT size, offset; float m[12]; };
inline ShadowProbe g_shadowProbe[32] = {};
inline volatile LONG g_shadowProbeN = 0;
// Latest pelvis matrices (3x4 floats) for the probe.
inline float g_pelvisMat[16][12] = {};
inline volatile LONG g_pelvisMatHead = 0;
// Records classified as the player that were flagged copied (diagnostic).
inline volatile LONG g_copiedDiag[2] = {};
// Vertex shaders of pieces found by the pelvis rule (skinned shaders); the
// chain and face rules require one of these, which level pieces carrying the
// arm's leftover bones do not use (walls hidden, live 2026-10-20, v80).
inline void *volatile g_playerVs[32] = {};
inline volatile LONG g_playerVsN = 0;
inline SRWLOCK g_shadowProbeLock = SRWLOCK_INIT;
// Where skipped draws land: per render-target binding (first target, or the
// depth view), with its size and format, counted per method (bits above).
struct HidePass { void *rtv0, *dsv; UINT w, h, fmt, rtCount; LONG n[4]; };
inline HidePass g_hidePass[32] = {};
inline volatile LONG g_hidePassN = 0;
inline SRWLOCK g_hidePassLock = SRWLOCK_INIT;
void STDMETHODCALLTYPE camHookedDrawIndexed(void *self, UINT indexCount, UINT startIndex, INT baseVertex);
void STDMETHODCALLTYPE camHookedDraw(void *self, UINT vertexCount, UINT startVertex);
void STDMETHODCALLTYPE camHookedDrawAuto(void *self);
void STDMETHODCALLTYPE camHookedDrawIndexedInstanced(void *self, UINT indexCountPerInstance, UINT instanceCount,
                                                     UINT startIndex, INT baseVertex, UINT startInstance);
// Rejections of transforms within 1.5 m: farther than 0.8 m, not rigid,
// tilted over 15 degrees, facing over 0.8 rad from the native angle.
inline volatile LONG g_upReject[4] = {};
// Live tuning (inject_mode.txt): tilt=<degrees> (default 2) and
// locktol=<centimetres> (default 10).  Stored as milli-degrees / millimetres.
inline volatile LONG g_upTiltMilliDeg = 2000;
inline volatile LONG g_upLockTolMm = 100;
// Tilt of picked transforms: sum (milli-degrees), count and maximum.
inline volatile LONG64 g_upTiltSum = 0;
inline volatile LONG g_upTiltCount = 0, g_upTiltMax = 0;

static const int kMaxCamCtx = 32;
inline CamCtxHooks g_camCtx[kMaxCamCtx] = {};
inline volatile LONG g_camCtxUsed = 0;

// Contexts of the same kind share one vtable, so patching it once hooks every
// context created from that implementation -- including deferred contexts this
// host never registered by address.  The original functions belong to the
// vtable; the bookkeeping (pending map, bound targets) belongs to the context.
// A context seen for the first time through a shared vtable therefore gets its
// own entry, carrying the originals copied from the entry that patched it.
//
// Without this, an unregistered context reaching a hook found no entry: Map
// returned E_FAIL without mapping, Unmap was never forwarded, and the game's
// OMSetRenderTargets call was silently dropped.
// ---------------------------------------------------------------------------
// Eye-position probe.
//
// A forward-shaded pass normally gives its shaders the camera's world position.
// That copy is uploaded with the frame it belongs to, unlike the copy in game
// memory, which runs a frame ahead.  For one second every ten, every constant
// buffer upload is scanned for a float3 within 0.6 m of the camera position from
// memory, and hits are counted per (buffer size, byte offset).  The worker logs
// the table at the end of each window.  Read-only.
struct EyeProbeEntry { UINT size; UINT offset; volatile LONG hits; };
// Eye positions found by the probe (live, 2026-10-19): float3 at byte 0x140 of
// the 2288-byte and of the 1040-byte constant buffers, uploaded thousands of
// times a second with the frame being drawn.  Latest value of each is kept.
inline float g_eyePos2288[3] = {};
inline volatile LONG g_eyeValid2288 = 0;
inline float g_eyePos1040[3] = {};
inline volatile LONG g_eyeValid1040 = 0;

// Eye positions paired with the facing of the view they were uploaded with.
// Uploads whose offset 0 holds a perspective view-projection record
// (forward row, eye at 0x140) here.  At draw time the entry whose forward
// matches the matrix being drawn with gives that frame's exact camera
// position: the memory camera runs frames ahead of the image during fast
// turns, and shadow / reflection views face elsewhere (live 2026-10-19).
struct PairedEye { volatile LONG seq; float forward[3]; float eye[3]; };
inline PairedEye g_pairedEyes[256] = {};
inline volatile LONG g_pairedEyeHead = 0;
// Finds the eye for a view-projection (rows).  Returns false when no recent
// upload faces the same way; *bestDot receives the closest match.
bool pairedEye(const float *vpRows, float *eyeOut, float *bestDot, const float *nearCamera = nullptr);

inline volatile LONG g_eyeProbeActive = 0;
inline float g_eyeProbeTarget[3] = {};
inline EyeProbeEntry g_eyeProbe[48] = {};
inline volatile LONG g_eyeProbeCount = 0;

inline void eyeProbeScan(const float *f, size_t bytes) {
    if (!g_eyeProbeActive || bytes < 12 || bytes > 16384) return;
    const float tx = g_eyeProbeTarget[0], ty = g_eyeProbeTarget[1], tz = g_eyeProbeTarget[2];
    const size_t n = bytes / 4;
    for (size_t i = 0; i + 2 < n; ++i) {
        float dx = f[i] - tx, dy = f[i + 1] - ty, dz = f[i + 2] - tz;
        if (dx * dx + dy * dy + dz * dz > 0.36f) continue;
        UINT off = static_cast<UINT>(i * 4), size = static_cast<UINT>(bytes);
        LONG count = InterlockedCompareExchange(&g_eyeProbeCount, 0, 0);
        bool found = false;
        for (LONG k = 0; k < count && k < 48; ++k)
            if (g_eyeProbe[k].size == size && g_eyeProbe[k].offset == off) {
                InterlockedIncrement(&g_eyeProbe[k].hits);
                found = true;
                break;
            }
        if (!found) {
            LONG slot = InterlockedIncrement(&g_eyeProbeCount) - 1;
            if (slot < 48) { g_eyeProbe[slot].size = size; g_eyeProbe[slot].offset = off; g_eyeProbe[slot].hits = 1; }
            else InterlockedDecrement(&g_eyeProbeCount);
        }
    }
}

inline CamCtxHooks *findCamCtx(void *c) {
    LONG n = InterlockedCompareExchange(&g_camCtxUsed, 0, 0);
    for (LONG i = 0; i < n && i < kMaxCamCtx; ++i) if (g_camCtx[i].ctx == c) return &g_camCtx[i];
    if (!c) return nullptr;

    void **vt = *reinterpret_cast<void ***>(c);
    const CamCtxHooks *owner = nullptr;
    for (LONG i = 0; i < n && i < kMaxCamCtx; ++i)
        if (g_camCtx[i].vt == vt && g_camCtx[i].map) { owner = &g_camCtx[i]; break; }
    if (!owner) return nullptr;

    LONG slot = InterlockedIncrement(&g_camCtxUsed) - 1;
    if (slot >= kMaxCamCtx) {
        InterlockedDecrement(&g_camCtxUsed);
        // Table full: still forward through the owner's originals so the
        // game's call is never dropped.  Bookkeeping is then shared with the
        // owner, which can only cost observation accuracy (Unmap still checks
        // the resource), never a lost or unforwarded call.
        return const_cast<CamCtxHooks *>(owner);
    }
    CamCtxHooks *h = &g_camCtx[slot];
    *h = CamCtxHooks{};
    h->map = owner->map;
    h->unmap = owner->unmap;
    h->omSetRenderTargets = owner->omSetRenderTargets;
    h->drawIndexed = owner->drawIndexed;
    h->drawIndexedInstanced = owner->drawIndexedInstanced;
    h->draw = owner->draw;
    h->drawAuto = owner->drawAuto;
    h->ctx = c;
    MemoryBarrier();
    h->vt = vt;
    return h;
}

// Distinct buffer widths seen so far, so a probe run can report whether the
// 2288/4112 view-projection buffers ever appear at all.
static const int kMaxCamSizes = 64;
inline UINT g_camSizes[kMaxCamSizes] = {};
inline volatile LONG g_camSizesUsed = 0;



// Defined in runtime.cpp: refreshes host observation and logs changes.
void hostOnCaptureTick();

HRESULT STDMETHODCALLTYPE camHookedMap(void *self, void *pResource, UINT subresource, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE *pMapped);
void STDMETHODCALLTYPE camHookedUnmap(void *self, void *pResource, UINT subresource);
void STDMETHODCALLTYPE camHookedOMSetRenderTargets(void *self, UINT rtCount, const void *const *rtv, void *depthView);

// Bound render targets for the context whose OMSetRenderTargets was last seen,
// used to recognise the geometry pass.

// Shape checks live in camera_capture.cpp.  Two different matrices are found at
// runtime and they must not be confused:
//   - the projection (1040-byte constant buffer), axis-aligned and static
//   - the combined view-projection (2288/4112-byte buffers), rotated and live
// Recognising the combined matrix by the projection's own m11 term is what makes
// the capture reliable; guessing from signs and magnitudes is not.

// ID3D11DeviceContext vtable slots.
//
// The absolute index is the method's own position in ID3D11DeviceContext plus
// every slot inherited from its base classes.  The base chain is
// ID3D11DeviceContext -> ID3D11DeviceChild -> IUnknown, which is 3 IUnknown
// slots plus 4 ID3D11DeviceChild slots (GetDevice, GetPrivateData, SetPrivateData,
// SetPrivateDataInterface) = 7, not 3.
//
// This matters: assuming a base of 3 put Map at 10, and patching slots 10/11
// crashed the game on save load, because those are PSSetSamplers and VSSetShader.
// Own indices come from work/dsr/vtable_slots.py over the installed d3d11.h.
namespace ctxslot {
inline constexpr int kInheritedSlots = 7;  // 3 IUnknown + 4 ID3D11DeviceChild
inline constexpr int kMapOwn = 7;
inline constexpr int kUnmapOwn = 8;
inline constexpr int kOMSetRenderTargetsOwn = 26;
inline constexpr int kOMSetDepthStencilStateOwn = 29;
inline constexpr int kOMSetBlendStateOwn = 28;
inline constexpr int kDrawOwn = 6;
inline constexpr int kDrawIndexedOwn = 5;
inline constexpr int kDrawIndexedInstancedOwn = 13;
inline constexpr int kDrawInstancedOwn = 14;
inline constexpr int kDrawAutoOwn = 31;
inline constexpr int kIASetInputLayoutOwn = 10;
inline constexpr int kIASetVertexBuffersOwn = 11;
inline constexpr int kIASetPrimitiveTopologyOwn = 17;
inline constexpr int kVSSetShaderOwn = 4;
inline constexpr int kPSSetShaderOwn = 2;
inline constexpr int kVSSetConstantBuffersOwn = 0;

inline constexpr int kMap = kMapOwn + kInheritedSlots;                    // 14
inline constexpr int kUnmap = kUnmapOwn + kInheritedSlots;                  // 15
inline constexpr int kOMSetRenderTargets = kOMSetRenderTargetsOwn + kInheritedSlots;              // 33
inline constexpr int kOMSetDepthStencilState = kOMSetDepthStencilStateOwn + kInheritedSlots;      // 36
inline constexpr int kOMSetBlendState = kOMSetBlendStateOwn + kInheritedSlots;                    // 35
inline constexpr int kDraw = kDrawOwn + kInheritedSlots;                  // 13
inline constexpr int kDrawAuto = kDrawAutoOwn + kInheritedSlots;          // 38
inline constexpr int kDrawIndexed = kDrawIndexedOwn + kInheritedSlots;    // 12
inline constexpr int kDrawIndexedInstanced = kDrawIndexedInstancedOwn + kInheritedSlots;  // 20
inline constexpr int kDrawInstanced = kDrawInstancedOwn + kInheritedSlots;                  // 21
inline constexpr int kIASetInputLayout = kIASetInputLayoutOwn + kInheritedSlots;            // 17
inline constexpr int kIASetVertexBuffers = kIASetVertexBuffersOwn + kInheritedSlots;        // 18
inline constexpr int kIASetPrimitiveTopology = kIASetPrimitiveTopologyOwn + kInheritedSlots;// 24
inline constexpr int kVSSetShader = kVSSetShaderOwn + kInheritedSlots;                      // 11
inline constexpr int kPSSetShader = kPSSetShaderOwn + kInheritedSlots;                      // 9
inline constexpr int kVSSetConstantBuffers = kVSSetConstantBuffersOwn + kInheritedSlots;    // 7
} // namespace ctxslot

// Reference vtable captured once, before any patching, used to prove every later
// context exposes the same function table.
static constexpr int kRefSlotMax = 40;
inline void *g_refSlots[kRefSlotMax + 1] = {};
inline bool g_refHave = false;

// Identify vtable slots by comparing them against each other.
//
// Map, Unmap and OMSetRenderTargets are not exported by d3d11.dll: they are
// methods reached only through the COM vtable, so GetProcAddress returns null
// and cannot serve as an identity reference.  Slot identity therefore rests on
// the header derivation in ctxslot (own index + 7 inherited slots), plus the
// structural checks in verifyContextSlots, plus the live behaviour that follows
// -- a correct hook produces real buffer payloads and render-target bindings.
//
// The reference vtable captured here is used only to report that a deferred
// context carries a different implementation of the same interface; address
// equality across contexts is neither expected nor required.
inline bool captureReferenceSlots(void *ctx, const char *label) {
    void **vt = ctx ? *reinterpret_cast<void ***>(ctx) : nullptr;
    if (!vt) return false;
    for (int i = 0; i <= kRefSlotMax; ++i) g_refSlots[i] = vt[i];
    g_refHave = true;
    logWrite("cam: reference vtable captured from %s ctx=%p", label, ctx);
    const int interesting[] = {ctxslot::kMap, ctxslot::kUnmap,
                                ctxslot::kIASetVertexBuffers,
                                ctxslot::kOMSetRenderTargets};
    for (int s : interesting)
        logWrite("cam:   reference slot [%d] = %p", s, vt[s]);
    return true;
}

inline void logIID(void *ctx, const char *label);

inline bool verifyContextSlots(void *ctx, const char *label) {
    if (!ctx) return false;
    void **vt = *reinterpret_cast<void ***>(ctx);
    if (!vt) return false;

    if (!g_refHave && !captureReferenceSlots(ctx, label)) return false;

    // Deferred contexts implement ID3D11DeviceContext -- confirmed by
    // QueryInterface, which returns the same object for ID3D11DeviceContext
    // through Context4.  They use a different implementation of that interface
    // than the immediate context, so the function pointers in the vtable differ
    // by address even though the slot layout is identical.  Comparing addresses
    // across contexts therefore fails for a correct slot index, which is what
    // this check previously did.
    //
    // What actually has to hold, and is what is checked here:
    //  - the vtable is present and non-null
    //  - the slots being patched are distinct from each other, so two hooks
    //    cannot overwrite one another
    //  - the slots being patched are inside the vtable and distinct from slots
    //    already patched by this host
    const int patchSlots[] = {ctxslot::kMap, ctxslot::kUnmap,
                              ctxslot::kOMSetRenderTargets};
    const int kPatchCount = 3;
    bool ok = true;
    // Every patched slot must hold a different function, otherwise two hooks
    // would overwrite one another.
    for (int a = 0; a < kPatchCount; ++a) {
        for (int b = a + 1; b < kPatchCount; ++b) {
            if (vt[patchSlots[a]] == vt[patchSlots[b]]) {
                logWrite("cam: slot verify %s FAILED: slots %d and %d alias at %p",
                         label, patchSlots[a], patchSlots[b], vt[patchSlots[a]]);
                ok = false;
            }
        }
    }
    // None of them may already hold a host hook.
    if (vt[ctxslot::kMap] == camHookedMap || vt[ctxslot::kUnmap] == camHookedUnmap ||
        vt[ctxslot::kOMSetRenderTargets] == camHookedOMSetRenderTargets) {
        logWrite("cam: slot verify %s FAILED: slot already holds a host hook", label);
        ok = false;
    }
    // Deferred contexts legitimately carry their own implementations, so a
    // differing address from the immediate context is expected, not a fault.
    if (vt[ctxslot::kMap] != g_refSlots[ctxslot::kMap])
        logWrite("cam: %s uses its own ID3D11DeviceContext implementation "
                 "(Map=%p vs immediate %p); layout is shared",
                 label, vt[ctxslot::kMap], g_refSlots[ctxslot::kMap]);
    void *mapSlot = vt[ctxslot::kMap];
    void *unmapSlot = vt[ctxslot::kUnmap];
    if (ok)
        logWrite("cam: slot verify %s OK ctx=%p Map=[%d]%p Unmap=[%d]%p OMSetRT=[%d]%p",
                 label, ctx, ctxslot::kMap, mapSlot,
                 ctxslot::kUnmap, unmapSlot,
                 ctxslot::kOMSetRenderTargets, vt[ctxslot::kOMSetRenderTargets]);
    else
        // Identify the interface authoritatively.  QueryInterface answers this
        // directly, whereas inferring it from which vtable slots happen to
        // match is guesswork.
        logIID(ctx, label);
    return ok;
}

// Report which D3D11 context interface a context actually implements.  Only
// used on a verification failure, where identifying the real interface is the
// fastest route to a correct slot index.
inline void logIID(void *ctx, const char *label) {
    if (!ctx) return;
    typedef HRESULT (STDMETHODCALLTYPE *PfnQI)(void *, REFIID, void **);
    PfnQI qi = reinterpret_cast<PfnQI>((*reinterpret_cast<void ***>(ctx))[0]);
    if (!qi) return;
    struct { const char *name; const GUID *iid; } probes[] = {
        {"ID3D11DeviceContext", &IID_ID3D11DeviceContext},
        {"ID3D11DeviceContext1", &IID_ID3D11DeviceContext1},
        {"ID3D11DeviceContext2", &IID_ID3D11DeviceContext2},
        {"ID3D11DeviceContext3", &IID_ID3D11DeviceContext3},
        {"ID3D11DeviceContext4", &IID_ID3D11DeviceContext4},
    };
    for (const auto &pr : probes) {
        void *obj = nullptr;
        HRESULT hr = qi(ctx, *pr.iid, &obj);
        if (SUCCEEDED(hr)) {
            logWrite("cam: %s implements %s (obj=%p)", label, pr.name, obj);
            if (obj) reinterpret_cast<IUnknown *>(obj)->Release();
        }
    }
}

inline void patchCamContext(void *ctx, const char *label) {
    if (!ctx) return;
    void **vt = *reinterpret_cast<void ***>(ctx);
    if (!vt) return;
    if (!verifyContextSlots(ctx, label)) {
        logWrite("cam: REFUSING to patch %s - slot identity unproven", label);
        return;
    }
    LONG n = InterlockedCompareExchange(&g_camCtxUsed, 0, 0);
    for (LONG i = 0; i < n && i < kMaxCamCtx; ++i) if (g_camCtx[i].vt == vt) return;
    LONG slot = InterlockedIncrement(&g_camCtxUsed) - 1;
    if (slot >= kMaxCamCtx) { InterlockedDecrement(&g_camCtxUsed); return; }
    CamCtxHooks *h = &g_camCtx[slot];
    h->ctx = ctx; h->vt = vt;
    h->map = vt[ctxslot::kMap];
    h->unmap = vt[ctxslot::kUnmap];
    h->omSetRenderTargets = vt[ctxslot::kOMSetRenderTargets];
    h->drawIndexed = vt[ctxslot::kDrawIndexed];
    h->drawIndexedInstanced = vt[ctxslot::kDrawIndexedInstanced];
    h->draw = vt[ctxslot::kDraw];
    h->drawAuto = vt[ctxslot::kDrawAuto];
    h->mapData = nullptr; h->mapResource = nullptr; h->mapByteWidth = 0;
    h->rtCount = 0; h->depthView = nullptr;
    for (int i = 0; i < kMaxRTViews; ++i) h->rtv[i] = nullptr;

    DWORD p = 0;
    // Map/Unmap are adjacent, so one protection change covers both.
    if (!VirtualProtect(&vt[ctxslot::kMap], sizeof(void *) * 2, PAGE_READWRITE, &p)) return;
    vt[ctxslot::kMap] = reinterpret_cast<void *>(&camHookedMap);
    vt[ctxslot::kUnmap] = reinterpret_cast<void *>(&camHookedUnmap);
    VirtualProtect(&vt[ctxslot::kMap], sizeof(void *) * 2, p, &p);

    // Render-target binding and draw submission are separate slots, patched
    // separately so a failure in one does not lose the other.
    bool rtPatched = false;
    if (VirtualProtect(&vt[ctxslot::kOMSetRenderTargets], sizeof(void *), PAGE_READWRITE, &p)) {
        vt[ctxslot::kOMSetRenderTargets] = reinterpret_cast<void *>(&camHookedOMSetRenderTargets);
        VirtualProtect(&vt[ctxslot::kOMSetRenderTargets], sizeof(void *), p, &p);
        rtPatched = true;
    }
    // DrawIndexed: only to skip the player's own meshes when hide=on (no
    // memory writes); every other draw is forwarded unchanged.
    bool diPatched = false;
    if (vt[ctxslot::kDrawIndexed] != reinterpret_cast<void *>(&camHookedDrawIndexed) &&
        VirtualProtect(&vt[ctxslot::kDrawIndexed], sizeof(void *), PAGE_READWRITE, &p)) {
        vt[ctxslot::kDrawIndexed] = reinterpret_cast<void *>(&camHookedDrawIndexed);
        VirtualProtect(&vt[ctxslot::kDrawIndexed], sizeof(void *), p, &p);
        diPatched = true;
    }
    bool diiPatched = false;
    if (vt[ctxslot::kDrawIndexedInstanced] != reinterpret_cast<void *>(&camHookedDrawIndexedInstanced) &&
        VirtualProtect(&vt[ctxslot::kDrawIndexedInstanced], sizeof(void *), PAGE_READWRITE, &p)) {
        vt[ctxslot::kDrawIndexedInstanced] = reinterpret_cast<void *>(&camHookedDrawIndexedInstanced);
        VirtualProtect(&vt[ctxslot::kDrawIndexedInstanced], sizeof(void *), p, &p);
        diiPatched = true;
    }
    bool dPatched = false, daPatched = false;
    if (vt[ctxslot::kDraw] != reinterpret_cast<void *>(&camHookedDraw) &&
        VirtualProtect(&vt[ctxslot::kDraw], sizeof(void *), PAGE_READWRITE, &p)) {
        vt[ctxslot::kDraw] = reinterpret_cast<void *>(&camHookedDraw);
        VirtualProtect(&vt[ctxslot::kDraw], sizeof(void *), p, &p);
        dPatched = true;
    }
    if (vt[ctxslot::kDrawAuto] != reinterpret_cast<void *>(&camHookedDrawAuto) &&
        VirtualProtect(&vt[ctxslot::kDrawAuto], sizeof(void *), PAGE_READWRITE, &p)) {
        vt[ctxslot::kDrawAuto] = reinterpret_cast<void *>(&camHookedDrawAuto);
        VirtualProtect(&vt[ctxslot::kDrawAuto], sizeof(void *), p, &p);
        daPatched = true;
    }
    logWrite("cam: Map/Unmap + OMSetRenderTargets%s + DrawIndexed%s + DrawIndexedInstanced%s + Draw%s + DrawAuto%s "
             "hooked on %s ctx=%p",
             rtPatched ? "" : "(FAILED)", diPatched ? "" : "(FAILED)", diiPatched ? "" : "(FAILED)",
             dPatched ? "" : "(FAILED)", daPatched ? "" : "(FAILED)", label, ctx);
}

inline void armCameraCapture(ID3D11Device *device, ID3D11DeviceContext *context) {
    if (!device || !context) return;
    patchCamContext(context, "immediate");
}

} // namespace remastered