// Remastered host camera capture implementation.
//
// The view-projection matrix is not stored in a persistent CPU-side camera
// structure.  Instead, it is computed by the game and uploaded to dynamic
// constant buffers via Map/Unmap.  This module hooks Map/Unmap on the immediate
// context and on every deferred context as it is created, and caches the matrix
// for rendering.
//
// The captured matrix is a combined view-projection:
//   m[0][0] = 1.425 (aspect-corrected FOV)
//   m[1][1] = 2.539 (1/tan(fov/2))
//   m[2][2] = 1.0 (infinite far plane)
//
// Matrix bytes are only valid while the resource is still mapped, so they are
// read in Unmap *before* forwarding to the real Unmap.

#include "camera_capture.h"
#include "geometry_pass.h"
#include "world_observation.h"
#include "logging.h"
#include "fx_probe.h"


namespace remastered {

// Distinct render-target binding shapes retained for identification.  A pass is
// recognised by its attachment signature, so only new shapes are interesting.
static const int kMaxSeenBindings = 64;
static volatile LONG g_bindingHits[kMaxSeenBindings] = {};
static void noteRenderTargetBinding(void *ctx, UINT count, void *const *rtv, void *depthView);

// D3D11_RESOURCE_DESC and ID3D11Resource::GetDesc are absent from both the
// bundled and SDK headers here, so neither is usable.
//
// ID3D11Resource's own vtable ends at GetEvictionPriority: GetDesc is declared
// on the derived interfaces (ID3D11Texture2D and friends), so there is no fixed
// resource slot for it.  Probing past slot 9 lands on whatever the concrete type
// happens to declare there, which is why a fixed index read plausible widths
// for one resource type and would read garbage for another -- and why blindly
// probing several indices crashed the game.
//
// The view is queried directly instead.  ID3D11View has no virtuals of its own,
// so its GetDesc index is fixed and safe, and the descriptor describes exactly
// the attachment being inspected.
// Layout must match D3D11_RESOURCE_DESC exactly on x64.  Verified by computing
// offsets: Width 0, Height 4, Depth 8, MipLevels 12, ArraySize 16, Format 20,
// SampleDesc 24, Usage 32, BindFlags 36, CPUAccessFlags 40, MiscFlags 44,
// sizeof 48.  An earlier version declared Format last and used a void* for
// Usage, which put Format at offset 52 -- every format read then returned 0 and
// every descriptor was discarded as implausible.
struct ResourceDescLocal {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    UINT SampleDesc_Count;
    UINT SampleDesc_Quality;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};
static_assert(sizeof(ResourceDescLocal) == 48, "D3D11_RESOURCE_DESC layout");

// D3D11_RESOURCE_DESC is absent from the bundled header, so it is declared here
// with the documented layout, enforced by the static_assert above.
//
// GetDesc does not live on ID3D11View: Microsoft documents ID3D11View as having
// exactly one method, GetResource.  GetDesc is declared on the concrete resource
// type, and for a texture the chain is:
//
//   0-2   IUnknown
//   3-6   ID3D11DeviceChild
//   7-9   ID3D11Resource (GetType, SetEvictionPriority, GetEvictionPriority)
//   10    ID3D11Texture2D::GetDesc
//
// So a view yields its resource via slot 7, and the descriptor is read from the
// resource's own vtable at slot 10.  Two earlier attempts were wrong: slot 8 on
// the resource is SetEvictionPriority, which corrupted driver state and crashed
// the game; and calling GetDesc on the view read unrelated memory.
static constexpr int kViewGetResourceSlot = 7;

// Read a resource descriptor through ID3D11Texture2D, obtained by
// QueryInterface rather than by assuming the object's concrete type.
//
// Calling slot 10 on whatever ID3D11Resource was returned produced a coherent
// Width/Height but nonsense in every other field (ArraySize = 87, which is a
// fragment of a pointer, not a count), because the object was not necessarily a
// texture and slot 10 on it was not GetDesc.  QueryInterface returns the
// documented interface or E_NOINTERFACE, so the descriptor is always read
// through the right vtable.  D3D11_TEXTURE2D_DESC and ID3D11Texture2D are absent
// from the bundled header, so both are declared locally.
struct Texture2DDescLocal {
    UINT Width;
    UINT Height;
    UINT ArraySize;
    UINT MipLevels;
    UINT Format;
    UINT SampleDesc_Count;
    UINT SampleDesc_Quality;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};

// GUIDs are taken verbatim from the bundled d3d11.h's DEFINE_GUID lines.  An
// earlier version typed plausible-looking GUIDs from memory; every QueryInterface
// then returned E_NOINTERFACE because the IIDs did not exist, which looked like
// "unsupported resource type" rather than a wrong constant.
static const GUID kIIDTexture2D = {
    0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

// ID3D11Texture2D : ID3D11Resource : ID3D11DeviceChild : IUnknown
//   0-2   IUnknown
//   3-6   ID3D11DeviceChild
//   7-9   ID3D11Resource (GetType, SetEvictionPriority, GetEvictionPriority)
//   10    ID3D11Texture2D::GetDesc
static constexpr int kTex2DGetDescSlot = 10;

// Call GetDesc on an object already known to implement ID3D11Texture2D.
static bool readTexture2DDesc(void *tex2d, Texture2DDescLocal *td) {
    if (!tex2d || !td) return false;
    void **tvt = *reinterpret_cast<void ***>(tex2d);
    if (!tvt || !tvt[kTex2DGetDescSlot]) return false;
    *td = Texture2DDescLocal{};
    typedef void(STDMETHODCALLTYPE *PfnGetDesc)(void *, Texture2DDescLocal *);
    reinterpret_cast<PfnGetDesc>(tvt[kTex2DGetDescSlot])(tex2d, td);
    return true;
}

// Resolve a resource's descriptor through QueryInterface, then the documented
// GetDesc index.  Both steps are required: the resource is only known to be an
// ID3D11Resource, and GetDesc is declared on the concrete texture type, so slot
// 10 on the bare resource is not guaranteed to be it.
static bool readResourceDesc(ID3D11Resource *res, ResourceDescLocal *out) {
    if (!res || !out) return false;
    void **vt = *reinterpret_cast<void ***>(res);
    if (!vt) return false;
    typedef HRESULT(STDMETHODCALLTYPE *PfnQI)(void *, REFIID, void **);
    PfnQI qi = reinterpret_cast<PfnQI>(vt[0]);
    if (!qi) return false;
    void *tex2d = nullptr;
    if (FAILED(qi(res, kIIDTexture2D, &tex2d)) || !tex2d) return false;

    Texture2DDescLocal td = {};
    bool ok = readTexture2DDesc(tex2d, &td);
    reinterpret_cast<IUnknown *>(tex2d)->Release();
    if (!ok) return false;

    // Only dimensions are validated.  Format values look large but are
    // legitimate: DXGI_FORMAT_R8G8B8A8_UNORM is 87 and D32_FLOAT is 40, and this
    // title's back buffer reads exactly that.  An earlier upper bound on Format
    // rejected correct descriptors and made every binding look null.
    if (td.Width == 0 || td.Width > 32768) return false;
    if (td.Height == 0 || td.Height > 32768) return false;
    if (td.Format == 0) return false;

    out->Width = td.Width;
    out->Height = td.Height;
    out->ArraySize = td.ArraySize;
    out->MipLevels = td.MipLevels;
    out->Format = static_cast<DXGI_FORMAT>(td.Format);
    out->SampleDesc_Count = td.SampleDesc_Count;
    out->SampleDesc_Quality = td.SampleDesc_Quality;
    out->Usage = td.Usage;
    out->BindFlags = td.BindFlags;
    out->CPUAccessFlags = td.CPUAccessFlags;
    out->MiscFlags = td.MiscFlags;
    return true;
}

// Read a view's attached resource descriptor.  Exposed so the geometry-pass
// unit can recognise the G-buffer by the same proven path rather than
// duplicating the QueryInterface and GetDesc sequence.
//
// Any output pointer may be null when the caller does not need that field.
bool hostReadViewResourceDesc(void *view, UINT *width, UINT *height, UINT *format,
                              UINT *sampleCount, UINT *usage, UINT *bindFlags) {
    if (!view) return false;
    // ID3D11View declares only GetResource, at slot 7.
    constexpr int kViewGetResourceSlot = 7;
    typedef void(STDMETHODCALLTYPE *PfnGetResource)(void *, ID3D11Resource **);
    ID3D11Resource *res = nullptr;
    reinterpret_cast<PfnGetResource>(
        (*reinterpret_cast<void ***>(view))[kViewGetResourceSlot])(view, &res);
    if (!res) return false;
    ResourceDescLocal d = {};
    bool ok = readResourceDesc(res, &d);
    res->Release();
    if (!ok) return false;
    if (width) *width = d.Width;
    if (height) *height = d.Height;
    if (format) *format = static_cast<UINT>(d.Format);
    if (sampleCount) *sampleCount = d.SampleDesc_Count;
    if (usage) *usage = d.Usage;
    if (bindFlags) *bindFlags = d.BindFlags;
    return true;
}

// One-off confirmation that the first attached resource is a texture and that
// its descriptor reads coherently.  Unknown slots are never called: doing so
// crashed the game twice.
static volatile LONG g_resDumpDone = 0;
static void dumpResourceDesc(ID3D11Resource *res) {
    if (InterlockedCompareExchange(&g_resDumpDone, 1, 0)) return;
    if (!res) return;
    ResourceDescLocal d = {};
    bool ok = readResourceDesc(res, &d);
    logWrite("res: first attached resource %p -> ok=%d %ux%u array=%u mips=%u "
             "fmt=%u sample=%u usage=%u bind=0x%X",
             res, ok, d.Width, d.Height, d.ArraySize, d.MipLevels,
             static_cast<int>(d.Format), d.SampleDesc_Count, d.Usage, d.BindFlags);
}

// The 4112-byte constant buffer holds the main scene's view-projection.  The
// 2288-byte one holds a different shader variant with its own FOV term, so only
// one of the two is used.
static const size_t kMainViewProjectionBytes = 4112;

static bool nearlyEqual(float a, float b, float tol) {
    float d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

// The combined view-projection sits at offset 0 of the 2288/4112-byte constant
// buffers.  Measured from live sessions:
//
//   m00 = -1.8898  m01 = 0  m02 = -0.2318  m03 = 0
//   m10 = -0.0007  m11 = 2.5386  m12 = 0.0058  m13 = 0
//   m20 =  0.1219  m21 = 0.0022  m22 = -0.9926  m23 = -0.0501
//
// A previous run observed m00 = -0.5104 for the same slot, so m00 is signed and
// varies with camera yaw; only its magnitude is bounded.  m11 is the projection
// scale (2.5386, stable across runs and resolutions) and is the strongest single
// discriminator.  m23 carries the camera translation and must be able to move.
//
// The 4th row repeats the 3rd (m30..m33 == m20..m23), which is why the check
// ignores it -- it is padding, not a fourth basis vector.
// ---------------------------------------------------------------------------
// Player-matrix probe (diagnostic, 2026-10-19).
//
// Looks for the engine's own upload of the player's model transform, so Mario
// can later be placed from the same upload as the frame's view-projection
// instead of from game memory (which runs a frame or two ahead and cannot be
// matched to the drawn frame while the camera only slides behind the player).
//
// For 1 s in every 4 s every constant-buffer upload is scanned at 16-byte steps
// for a rigid, upright transform whose translation is within 0.5 m of the
// player, in two layouts (3x4 rows with translation in column 3; 4x4 with
// translation in row 3) and two spaces (camera-relative: player - camera, the
// space the 4112-byte view-projection uses; absolute: world position).  Hits
// are counted per (buffer size, float offset, layout, space) with the mean
// distance and how well the transform's facing matches the native angle.
// Read-only; logs the most frequent slots at the end of each window.
namespace playerprobe {
struct Slot {
    UINT size, index, kind;  // kind: bit0 layout B (4x4), bit1 absolute space
    LONG hits;
    double dist, yaw0, yawPi;  // sums: distance, |yaw delta|, |yaw delta - pi|
};
const int kSlots = 96;
Slot slots[kSlots];
int used = 0;
LONG uploads = 0, scanned = 0;
SRWLOCK lock = SRWLOCK_INIT;
volatile LONG windowStart = 0, windowOpen = 0;

inline double seconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

inline float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a <= -3.14159265f) a += 6.2831853f;
    return a;
}

// Rigid upright rotation with axes r0, r1, r2 (rows of the 3x3 block); the
// up axis is r1 for 4x4 row-vector matrices and the second column for 3x4.
inline bool rigid(const float *a, const float *b, const float *c) {
    auto len2 = [](const float *v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; };
    auto dot = [](const float *u, const float *v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
    const float la = len2(a), lb = len2(b), lc = len2(c);
    if (!(la > 0.25f && la < 4.0f && lb > 0.25f && lb < 4.0f && lc > 0.25f && lc < 4.0f)) return false;
    return std::fabs(dot(a, b)) < 0.05f * std::sqrt(la * lb) && std::fabs(dot(a, c)) < 0.05f * std::sqrt(la * lc) &&
           std::fabs(dot(b, c)) < 0.05f * std::sqrt(lb * lc);
}

inline void record(UINT size, UINT index, UINT kind, float dist, float yaw, float angle) {
    const float d0 = std::fabs(wrapPi(yaw - angle)), dPi = std::fabs(wrapPi(yaw - angle - 3.14159265f));
    AcquireSRWLockExclusive(&lock);
    int at = -1;
    for (int s = 0; s < used; ++s)
        if (slots[s].size == size && slots[s].index == index && slots[s].kind == kind) { at = s; break; }
    if (at < 0 && used < kSlots) { at = used++; slots[at] = {size, index, kind, 0, 0, 0, 0}; }
    if (at >= 0) {
        Slot &s = slots[at];
        ++s.hits; s.dist += dist; s.yaw0 += d0; s.yawPi += dPi;
    }
    ReleaseSRWLockExclusive(&lock);
}

void report() {
    AcquireSRWLockExclusive(&lock);
    logWrite("playerprobe: window done, %ld constant-buffer uploads, %ld scanned, %d distinct slots",
             uploads, scanned, used);
    for (int shown = 0; shown < 12; ++shown) {
        int best = -1;
        for (int s = 0; s < used; ++s)
            if (slots[s].hits > 0 && (best < 0 || slots[s].hits > slots[best].hits)) best = s;
        if (best < 0) break;
        Slot &s = slots[best];
        logWrite("playerprobe:   %4u B float %4u (+0x%03X) %s %s hits=%ld dist=%.3f m yaw-diff=%.2f "
                 "(vs angle+pi %.2f)",
                 s.size, s.index, s.index * 4, (s.kind & 1) ? "4x4" : "3x4", (s.kind & 2) ? "absolute" : "camera-rel",
                 s.hits, s.dist / s.hits, s.yaw0 / s.hits, s.yawPi / s.hits);
        s.hits = -s.hits;  // shown; excluded from the next pick
    }
    used = 0;
    uploads = scanned = 0;
    ReleaseSRWLockExclusive(&lock);
}

void scan(const float *f, size_t bytes) {
    const LONG now = static_cast<LONG>(GetTickCount());
    if (!windowOpen) {
        if (now - windowStart < 3000) return;
        if (InterlockedCompareExchange(&windowOpen, 1, 0) == 0) windowStart = now;
        return;
    }
    if (now - windowStart > 1000) {
        if (InterlockedCompareExchange(&windowOpen, 0, 1) == 1) {
            windowStart = now;
            report();
        }
        return;
    }
    InterlockedIncrement(&uploads);
    if (bytes < 48 || bytes > 16384) return;
    float player[3], camera[3], angle = 0;
    if (!readLivePositions(player, camera) || !readLiveAngle(angle)) return;
    InterlockedIncrement(&scanned);
    const float rel[3] = {player[0] - camera[0], player[1] - camera[1], player[2] - camera[2]};
    const size_t n = bytes / 4;
    for (size_t i = 0; i + 12 <= n; i += 4) {
        const float *m = f + i;
        // Layout A: 3x4 rows, translation in column 3; columns are the axes.
        {
            const float tx = m[3], ty = m[7], tz = m[11];
            for (int space = 0; space < 2; ++space) {
                const float *ref = space ? player : rel;
                const float dx = tx - ref[0], dy = ty - ref[1], dz = tz - ref[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (!(d2 < 0.25f)) continue;
                const float c0[3] = {m[0], m[4], m[8]}, c1[3] = {m[1], m[5], m[9]}, c2[3] = {m[2], m[6], m[10]};
                const float l1 = std::sqrt(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
                if (!rigid(c0, c1, c2) || std::fabs(c1[1]) < 0.9f * l1) continue;
                record(static_cast<UINT>(bytes), static_cast<UINT>(i), space ? 2u : 0u, std::sqrt(d2),
                       std::atan2(c2[0], c2[2]), angle);
            }
        }
        // Layout B: 4x4 with translation in row 3 (m[12..14]), m[15] == 1.
        if (i + 16 <= n && std::fabs(m[15] - 1.0f) < 1e-3f && m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f) {
            const float tx = m[12], ty = m[13], tz = m[14];
            for (int space = 0; space < 2; ++space) {
                const float *ref = space ? player : rel;
                const float dx = tx - ref[0], dy = ty - ref[1], dz = tz - ref[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (!(d2 < 0.25f)) continue;
                const float *r0 = m, *r1 = m + 4, *r2 = m + 8;
                const float l1 = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
                if (!rigid(r0, r1, r2) || std::fabs(r1[1]) < 0.9f * l1) continue;
                record(static_cast<UINT>(bytes), static_cast<UINT>(i), space ? 3u : 1u, std::sqrt(d2),
                       std::atan2(r2[0], r2[2]), angle);
            }
        }
    }
}
} // namespace playerprobe

// Several upright transforms sit near the player (the body root and items
// that only turn).  Each keeps its own offset from the player's feet in its
// own axes, so that offset identifies the object.  Offsets are clustered over
// time and the most frequent cluster - the body root, used by every body-part
// draw - is the one followed.  Mixing objects made Mario jitter and jump
// (feet offset seen switching between three values, live 2026-10-19).
namespace rootlock {
struct Cluster { float c[3]; double weight; };
Cluster clusters[8];
int count = 0;
SRWLOCK lock = SRWLOCK_INIT;
DWORD lastDecay = 0;

// Adds an observation; returns nothing.  Caller holds no lock.
void observe(const float L[3]) {
    AcquireSRWLockExclusive(&lock);
    const DWORD now = GetTickCount();
    if (now - lastDecay > 1000) {
        for (int i = 0; i < count; ++i) clusters[i].weight *= 0.5;
        lastDecay = now;
    }
    int at = -1;
    for (int i = 0; i < count; ++i) {
        const float dx = clusters[i].c[0] - L[0], dy = clusters[i].c[1] - L[1], dz = clusters[i].c[2] - L[2];
        if (dx * dx + dy * dy + dz * dz < 0.08f * 0.08f) { at = i; break; }
    }
    if (at < 0) {
        if (count < 8) at = count++;
        else {
            at = 0;
            for (int i = 1; i < count; ++i) if (clusters[i].weight < clusters[at].weight) at = i;
        }
        memcpy(clusters[at].c, L, sizeof(clusters[at].c));
        clusters[at].weight = 0;
    }
    Cluster &k = clusters[at];
    for (int a = 0; a < 3; ++a) k.c[a] = k.c[a] * 0.98f + L[a] * 0.02f;
    k.weight += 1.0;
    ReleaseSRWLockExclusive(&lock);
}

// Whether L belongs to the dominant cluster: it must be nearer that cluster
// than any other and within 0.3 m.  *dist2 receives the squared distance.
// An absolute 10 cm limit failed while moving (memory runs ahead of the frame
// and skews L along the motion) and Mario vanished; the two upright objects
// near the feet are 0.37 m apart, mostly sideways, so nearest-cluster
// assignment still separates them.
bool belongsToLocked(const float L[3], float *dist2) {
    AcquireSRWLockShared(&lock);
    int best = -1, nearest = -1;
    float nearestD = 3e38f;
    for (int i = 0; i < count; ++i) {
        if (clusters[i].weight > 0 && (best < 0 || clusters[i].weight > clusters[best].weight)) best = i;
        const float dx = clusters[i].c[0] - L[0], dy = clusters[i].c[1] - L[1], dz = clusters[i].c[2] - L[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (clusters[i].weight > 0.5 && d < nearestD) { nearestD = d; nearest = i; }
    }
    ReleaseSRWLockShared(&lock);
    if (dist2) *dist2 = nearestD;
    return best >= 0 && nearest == best && nearestD < 0.3f * 0.3f;
}

// Centre of the dominant cluster.
bool locked(float out[3]) {
    AcquireSRWLockShared(&lock);
    int best = -1;
    for (int i = 0; i < count; ++i)
        if (clusters[i].weight > 0 && (best < 0 || clusters[i].weight > clusters[best].weight)) best = i;
    if (best >= 0) memcpy(out, clusters[best].c, sizeof(float) * 3);
    ReleaseSRWLockShared(&lock);
    return best >= 0;
}
} // namespace rootlock

// ---- Hiding the player's model --------------------------------------------
namespace hideplayer {
inline unsigned long long hashMatrix(const float *m) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(m);
    for (int i = 0; i < 48; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

// Hash of the rotation part (3x3) of a 3x4 row-major matrix.
inline unsigned long long hashRotation(const float *m) {
    static const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    unsigned long long h = 1469598103934665603ull;
    for (int k : idx) {
        const unsigned char *b = reinterpret_cast<const unsigned char *>(m + k);
        for (int i = 0; i < 4; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    return h ? h : 1;
}

inline int slotOf(unsigned long long key) { return static_cast<int>((key ^ (key >> 29)) & 1023); }

// Votes recorded for a draw key (0 when unknown).
// F9: forget every learned piece (walls hidden by mistake come back at once);
// the knight's own pieces are learned again within a few seconds.
inline void reset() {
    AcquireSRWLockExclusive(&g_hideLock);
    memset(const_cast<HideEntry *>(g_hideSet), 0, sizeof(g_hideSet));
    InterlockedExchange(&g_hideTrusted, 0);
    InterlockedExchange(&g_hideIkeyN, 0);
    InterlockedExchange(&g_playerVsN, 0);
    for (auto &r : g_pelvisRing) r = 0;
    ReleaseSRWLockExclusive(&g_hideLock);
    logWrite("hide: learned pieces cleared (F9); relearning");
}

inline LONG votes(unsigned long long key) {
    AcquireSRWLockShared(&g_hideLock);
    LONG v = 0;
    for (int i = 0, s = slotOf(key); i < 1024; ++i, s = (s + 1) & 1023) {
        if (!g_hideSet[s].key) break;
        if (g_hideSet[s].key == key) { v = g_hideSet[s].votes; break; }
    }
    ReleaseSRWLockShared(&g_hideLock);
    return v;
}

inline bool ikeyTrusted(unsigned long long ikey) {
    const LONG n = g_hideIkeyN;
    for (LONG i = 0; i < n && i < 256; ++i) if (g_hideIkeys[i] == ikey) return true;
    return false;
}

inline bool soBufPlayer(void *b) {
    if (!b) return false;
    const LONG n = g_hideSoN;
    for (LONG i = 0; i < n && i < 64; ++i) if (g_hideSoBufs[i] == b) return true;
    return false;
}

inline void vote(unsigned long long key, unsigned long long ikey, UINT indexCount, int rule, int bones) {
    AcquireSRWLockExclusive(&g_hideLock);
    for (int i = 0, s = slotOf(key); i < 1024; ++i, s = (s + 1) & 1023) {
        HideEntry &e = g_hideSet[s];
        if (!e.key) { e.key = key; e.ikey = ikey; e.rule = rule; e.bones = bones; e.votes = 1; e.lastTick = GetTickCount(); e.indexCount = indexCount; break; }
        if (e.key == key) {
            if (e.rule & 0x100) { e.lastTick = GetTickCount(); break; }  // refused in a burst
            if (e.votes < 1000000) ++e.votes;
            if (e.votes == 3) {
                // Burst guard: the player's own pieces are learned a few at a
                // time (body, armour, weapons).  83 new ones in 30 s were level
                // pieces after a long jump into a corner (walls vanished, v110);
                // 38 more after the v114 ledge-grab teleports.  The first 30 s
                // of a load learn freely (the whole body); after that at most 10
                // new in 10 s (an armour change), the rest refused for good.
                static DWORD windowStart = 0, firstTrust = 0;
                static LONG windowCount = 0;
                const DWORD now = GetTickCount();
                if (!g_hideTrusted || !firstTrust) firstTrust = now;
                if (now - windowStart > 10000) { windowStart = now; windowCount = 0; }
                // The same mesh again under a new key (the game re-keys the
                // knight's pieces over time; refused, his clothes reappeared
                // after standing a while, v122): same index count as a piece
                // already trusted is always accepted and not counted.
                bool known = false;
                for (const HideEntry &t : g_hideSet)
                    if (t.key && t.key != e.key && t.votes >= 3 && !(t.rule & 0x100) && t.indexCount == e.indexCount) {
                        known = true;
                        break;
                    }
                if (!known && now - firstTrust > 30000 && ++windowCount > 20) {
                    e.votes = 2;
                    e.rule |= 0x100;
                    e.lastTick = now;
                    logWrite("hide: refused a new player mesh in a burst (rule %d, %u indices)", e.rule & 0xFF, e.indexCount);
                    break;
                }
                InterlockedIncrement(&g_hideTrusted);
                if (!ikeyTrusted(e.ikey) && g_hideIkeyN < 256) {
                    g_hideIkeys[g_hideIkeyN] = e.ikey;
                    MemoryBarrier();
                    InterlockedIncrement(&g_hideIkeyN);
                }
            }
            e.lastTick = GetTickCount(); break; }
    }
    ReleaseSRWLockExclusive(&g_hideLock);
}

// Classifies the queued records (caller holds g_pendLock).  A piece is the
// player's when its slot-0 matrix is the pelvis (anchor rule) or a bone of a
// piece already found to be the player's: arms start at R_Forearm and the head
// at Spine1, both in the body's palette.  Slot 0 is always written for the
// draw, whereas later slots can hold matrices left from earlier uploads; the
// old "two shared bones" rule matched level pieces through those leftovers
// (196 meshes learned, walls and floors hidden, live 2026-10-20, v72).
// Shadow-map records: a piece is the player's when its slot-0 rotation is the
// pelvis's (from the main pass, same frame) or a bone rotation of a shadow
// piece already found to be the player's.  Positions there are relative to
// the light's camera, so only rotations can match.
static void classifyShadowPending() {
    static unsigned long long set[4096];
    static bool player[2048];
    memset(set, 0, sizeof(set));
    auto inSet = [&](unsigned long long x) {
        for (int i = 0, s = static_cast<int>((x ^ (x >> 31)) & 4095); i < 4096; ++i, s = (s + 1) & 4095) {
            if (!set[s]) return false;
            if (set[s] == x) return true;
        }
        return false;
    };
    auto put = [&](unsigned long long x) {
        if (!x) return;
        for (int i = 0, s = static_cast<int>((x ^ (x >> 31)) & 4095); i < 4096; ++i, s = (s + 1) & 4095) {
            if (set[s] == x) return;
            if (!set[s]) { set[s] = x; return; }
        }
    };
    unsigned long long pelvisRot[16];
    for (int r = 0; r < 16; ++r) pelvisRot[r] = g_pelvisMat[r][0] != 0.0f ? hideplayer::hashRotation(g_pelvisMat[r]) : 0;
    AcquireSRWLockExclusive(&g_shadowPendLock);
    const int n = g_shadowPendN;
    for (int i = 0; i < n; ++i) {
        player[i] = false;
        const unsigned long long x = g_shadowPend[i].rot[0];
        for (int r = 0; r < 16 && !player[i]; ++r) player[i] = x && pelvisRot[r] == x;
        if (player[i]) for (unsigned long long y : g_shadowPend[i].rot) put(y);
    }
    for (int round = 0; round < 4; ++round) {
        bool grew = false;
        for (int i = 0; i < n; ++i) {
            if (player[i] || !g_shadowPend[i].rot[0] || !inSet(g_shadowPend[i].rot[0])) continue;
            player[i] = true;
            for (unsigned long long y : g_shadowPend[i].rot) put(y);
            grew = true;
        }
        if (!grew) break;
    }
    for (int i = 0; i < n; ++i) {
        if (!player[i]) continue;
        int bones = 0;
        for (unsigned long long y : g_shadowPend[i].rot) if (y) ++bones;
        hideplayer::vote(g_shadowPend[i].key, g_shadowPend[i].ikey, g_shadowPend[i].indexCount, 4, bones);
        InterlockedIncrement(&g_shadowLearnDiag[1]);
    }
    g_shadowPendN = 0;
    ReleaseSRWLockExclusive(&g_shadowPendLock);
}

static bool playerVs(void *vs) {
    if (!vs) return false;
    const LONG n = g_playerVsN;
    for (LONG i = 0; i < n && i < 32; ++i) if (g_playerVs[i] == vs) return true;
    return false;
}

static void classifyPending() {
    {
        static bool f9Was = false;
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9Was) reset();
        f9Was = f9;
    }
    static unsigned long long set[8192];
    static bool player[2048];
    memset(set, 0, sizeof(set));
    const int n = g_pendN;
    auto inSet = [&](unsigned long long x) {
        for (int i = 0, s = static_cast<int>((x ^ (x >> 31)) & 8191); i < 8192; ++i, s = (s + 1) & 8191) {
            if (!set[s]) return false;
            if (set[s] == x) return true;
        }
        return false;
    };
    auto add = [&](const PendingDraw &p) {
        for (unsigned long long x : p.hash) {
            if (!x) continue;
            for (int i = 0, s = static_cast<int>((x ^ (x >> 31)) & 8191); i < 8192; ++i, s = (s + 1) & 8191) {
                if (set[s] == x) break;
                if (!set[s]) { set[s] = x; break; }
            }
        }
    };
    for (int i = 0; i < n; ++i) {
        player[i] = false;
        const unsigned long long x = g_pend[i].hash[0];
        if (!x) continue;
        if (g_pend[i].copied) { InterlockedIncrement(&g_copiedDiag[0]); continue; }
        for (int r = 0; r < 16; ++r) if (g_pelvisRing[r] == x) { player[i] = true; break; }
        if (player[i]) {
            g_pend[i].rule = 1;
            add(g_pend[i]);
            if (g_pend[i].vs && !playerVs(g_pend[i].vs) && g_playerVsN < 32) {
                g_playerVs[g_playerVsN] = g_pend[i].vs;
                MemoryBarrier();
                InterlockedIncrement(&g_playerVsN);
            }
        }
    }
    for (int round = 0; round < 4; ++round) {
        bool grew = false;
        for (int i = 0; i < n; ++i) {
            if (player[i] || !g_pend[i].hash[0] || g_pend[i].copied || !playerVs(g_pend[i].vs) ||
                !inSet(g_pend[i].hash[0])) continue;
            player[i] = true;
            g_pend[i].rule = 2;
            add(g_pend[i]);
            grew = true;
        }
        if (!grew) break;
    }
    // Small pieces whose own root bone is not in the body's palette: the face
    // (3 bones: two head bones of its own, and the neck shared with the body;
    // live 2026-10-20, v76).  Taken when slot 0 sits at the player (hip test),
    // at most 8 bones are used, and the later slots are a mix of player bones
    // and others.  Level pieces with leftover matrices match the player on
    // every later slot (24 of 24), which this excludes.
    for (int i = 0; i < n; ++i) {
        PendingDraw &p = g_pend[i];
        // (No shader requirement here: the face may use its own skin shader.)
        // A helmet reuses the face's head and neck bones, so it looks like a
        // leftover; the skinned player shader tells it apart from the level
        // pieces the leftover test was made for (v88).
        if (player[i] || !p.hash[0] || !p.hip0 || (p.leftover && !playerVs(p.vs)) || p.copied) continue;
        int used = 0, inPlayer = 0;
        for (int a = 1; a < 38; ++a) {
            if (!p.hash[a]) continue;
            ++used;
            if (inSet(p.hash[a])) ++inPlayer;
        }
        // The mix (some later bones not the player's) rules out leftover
        // copies; with the player's skinned shader that guard is not needed:
        // the helmet's two later bones are both the player's (v88 log).
        const bool skinned = playerVs(p.vs);
        // Slot 0 itself one of the knight's bones this frame: every bone is exactly his (an NPC's
        // never equal his), so the face plate (1280 and 308 indices, its own shader, all 3 bones
        // the knight's) is his; the "mix" guard rejected it.
        const bool rootIsPlayer = inSet(p.hash[0]);
        if (used + 1 <= 8 && inPlayer >= 1 && (inPlayer < used || skinned || rootIsPlayer)) {
            player[i] = true;
            g_pend[i].rule = 3;
            add(p);
        }
    }
    // Pieces hanging off the head (helmets, hoods) start at a head bone that
    // only the face carries: chain again now that the face's bones are known.
    // A new helmet stayed visible before this (live 2026-10-21, v86).
    for (int round = 0; round < 4; ++round) {
        bool grew = false;
        for (int i = 0; i < n; ++i) {
            if (player[i] || !g_pend[i].hash[0] || g_pend[i].copied || !playerVs(g_pend[i].vs) ||
                !inSet(g_pend[i].hash[0])) continue;
            player[i] = true;
            g_pend[i].rule = 2;
            add(g_pend[i]);
            grew = true;
        }
        if (!grew) break;
    }
    // Diagnostics: unlearned pieces at the player (hip test) whose slot 0 is not a
    // player bone, with their bone counts.  Also on without diag=on while the face
    // plate is being chased.
    if (g_diag || g_hidePlayer) {
        static DWORD lastMissLog = 0;
        if (GetTickCount() - lastMissLog > 5000) {
            lastMissLog = GetTickCount();
            int logged = 0;
            for (int i = 0; i < n && logged < 8; ++i) {
                const PendingDraw &p = g_pend[i];
                if (player[i] || !p.hash[0] || !p.hip0) continue;
                int used = 0, inPlayer = 0;
                for (int a2 = 0; a2 < 38; ++a2) if (p.hash[a2]) { ++used; if (inSet(p.hash[a2])) ++inPlayer; }
                ++logged;
                logWrite("hide-unlearned: indices %u, bones %d, player bones %d, leftover %d, copied %d, skinned shader %d",
                         p.indexCount, used, inPlayer, p.leftover ? 1 : 0, p.copied ? 1 : 0, playerVs(p.vs) ? 1 : 0);
            }
        }
    }
    // Diagnostics: pieces left out that share two or more bones with the
    // player's (the face?): slot layout, logged every 5 s.
    {
        static DWORD lastLog = 0;
        if (g_diag && GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            int logged = 0;
            for (int i = 0; i < n && logged < 10; ++i) {
                if (player[i]) continue;
                const PendingDraw &p = g_pend[i];
                char shared[256];
                int o = 0, cnt = 0, nz = 0;
                for (int a = 0; a < 38; ++a) {
                    if (!p.hash[a]) continue;
                    ++nz;
                    if (inSet(p.hash[a])) { ++cnt; if (o < 180) o += _snprintf(shared + o, sizeof(shared) - o, " %d", a); }
                }
                shared[o] = 0;
                const bool face = p.indexCount == 2903 || p.indexCount == 1280 || p.indexCount == 308;
                if (!face) continue;
                int s0rep = 0;  // other recorded pieces with the same slot 0
                for (int j = 0; j < n; ++j) if (j != i && g_pend[j].hash[0] == p.hash[0] && p.hash[0]) ++s0rep;
                (void)cnt;
                ++logged;
                o += _snprintf(shared + o, sizeof(shared) - o, " | slot0 shared with %d records, in set %d", s0rep,
                               p.hash[0] && inSet(p.hash[0]) ? 1 : 0);
                logWrite("hide-miss: indices %u, slots used %d, rigid mask %llx, slot0 %s, shared slots:%s", p.indexCount,
                         nz, p.rigidMask, p.hash[0] ? ((p.rigidMask & 1) ? "rigid" : "scaled") : "empty", shared);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        if (!player[i]) continue;
        int bones = 0;
        for (unsigned long long x : g_pend[i].hash) if (x) ++bones;
        hideplayer::vote(g_pend[i].key, g_pend[i].ikey, g_pend[i].indexCount, g_pend[i].rule, bones);
        InterlockedIncrement(&g_hideDiag[2]);
    }
    g_pendN = 0;
    classifyShadowPending();
}

// End of a pass on one context: which recorded draws are the player's.
// Body and leg meshes carry the pelvis in bone slot 0 (repeated, or equal to
// the pelvis seen most recently); arms and head share at least two exact bone
// matrices with those (forearms and twists; Spine1, Neck and clavicle).
inline void classify(CamCtxHooks *h) {
    const int n = h->passDrawN;
    h->passDrawN = 0;
    if (n <= 0) return;
    InterlockedIncrement(&g_hideDiag[4]);
    // The pelvis is the anchor's own choice: the hip-gated slot-0 matrix
    // repeated across the most uploads (not sections) of this pass.  Picking
    // any repeated slot 0 among recorded draws took the face's (one piece,
    // several sections) and learned only the face (live 2026-10-20, v71).
    int best = -1;
    for (int q = 0; q < h->passCandN; ++q)
        if (h->passCands[q].count >= 2 && (best < 0 || h->passCands[q].count > h->passCands[best].count)) best = q;
    if (best >= 0) {
        const LONG at = InterlockedIncrement(&g_pelvisRingHead) & 15;
        g_pelvisRing[at] = h->passCands[best].hash;
        const LONG pm = InterlockedIncrement(&g_pelvisMatHead) & 15;
        memcpy(g_pelvisMat[pm], h->passCands[best].m, sizeof(g_pelvisMat[pm]));
    }
    // Queue this pass's records; classify the batch once per frame.
    AcquireSRWLockExclusive(&g_pendLock);
    const DWORD now = GetTickCount();
    if (g_pendN == 0) g_pendTick = now;
    for (int i = 0; i < n && g_pendN < 2048; ++i) {
        PendingDraw &p = g_pend[g_pendN++];
        p.key = h->passDraws[i].key;
        p.ikey = h->passDraws[i].ikey;
        p.indexCount = h->passDraws[i].indexCount;
        p.rigidMask = h->passDraws[i].rigidMask;
        p.rule = 0;
        p.hip0 = h->passDraws[i].hip0;
        p.leftover = h->passDraws[i].leftover;
        p.copied = h->passDraws[i].copied;
        p.vs = h->passDraws[i].vs;
        memcpy(p.hash, h->passDraws[i].hash, sizeof(p.hash));
    }
    if (now - g_pendTick >= 40 || g_pendN >= 2048) classifyPending();
    ReleaseSRWLockExclusive(&g_pendLock);
}

void report() {
    static volatile LONG last = 0;
    LONG now = static_cast<LONG>(GetTickCount()), prev = last;
    if (now - prev < 5000 || InterlockedCompareExchange(&last, now, prev) != prev) return;
    int learned = 0, trusted = 0;
    char sizes[400];
    int o = 0;
    AcquireSRWLockShared(&g_hideLock);
    for (const HideEntry &e : g_hideSet) {
        if (!e.key) continue;
        ++learned;
        if (e.votes >= 3) {
            ++trusted;
            if (o < 380) o += _snprintf(sizes + o, sizeof(sizes) - o, " %u(%s,%d)", e.indexCount,
                                        e.rule == 1 ? "pelvis" : e.rule == 2 ? "chain" : e.rule == 3 ? "face" : e.rule == 4 ? "shadow" : "?",
                                        e.bones);
        }
    }
    ReleaseSRWLockShared(&g_hideLock);
    sizes[o] = 0;
    logWrite("hide: %s; near-player draws %ld, recorded %ld, player %ld, skipped %ld (after upload %ld, by key "
             "elsewhere %ld, instanced %ld of %ld seen), passes %ld; meshes learned %d, trusted %d (index counts:%s)",
             g_hidePlayer ? "ON" : "off", g_hideDiag[0], g_hideDiag[1], g_hideDiag[2], g_hideDiag[3], g_hideDiag2[0],
             g_hideDiag2[1], g_hideDiag2[2], g_hideDiag2[3], g_hideDiag[4], learned, trusted, sizes);
    if (!g_diag) return;
    logWrite("hide2: skipped by index key only %ld; player draws with stream-out %ld, with geometry shader %ld; "
             "Draw %ld, DrawAuto %ld; skipped from player stream-out buffers %ld (%ld buffers)",
             g_hideDiag3[0], g_hideDiag3[1], g_hideDiag3[2], g_hideDiag3[3], g_hideDiag3[4], g_hideDiag3[5], g_hideSoN);
    AcquireSRWLockShared(&g_shadowProbeLock);
    for (LONG i = 0; i < g_shadowProbeN; ++i) {
        const ShadowProbe &p = g_shadowProbe[i];
        logWrite("shadow-probe %ld: upload %u bytes, pelvis at byte %u; rtv0=%p dsv=%p targets=%u %ux%u fmt=%u; uploads %ld; "
                 "m0=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                 i, p.size, p.offset, p.rtv0, p.dsv, p.rtCount, p.w, p.h, p.fmt, p.n, p.vp[0], p.vp[1], p.vp[2], p.vp[3],
                 p.vp[4], p.vp[5], p.vp[6], p.vp[7], p.vp[8], p.vp[9], p.vp[10], p.vp[11], p.vp[12], p.vp[13],
                 p.vp[14], p.vp[15]);
        logWrite("shadow-probe %ld: pelvis matrix there [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                 i, p.m[0], p.m[1], p.m[2], p.m[3], p.m[4], p.m[5], p.m[6], p.m[7], p.m[8], p.m[9], p.m[10], p.m[11]);
    }
    ReleaseSRWLockShared(&g_shadowProbeLock);
    logWrite("hide-copied: records with a copied palette rejected %ld; player vertex shaders %ld; shadow records %ld, "
             "classified as player %ld", g_copiedDiag[0], g_playerVsN, g_shadowLearnDiag[0], g_shadowLearnDiag[1]);
    logWrite("hide-shadow: shadow-pass draws %ld, learned player meshes among them %ld, skipped %ld; main depth %ldx%ld, "
             "skipped in smaller-depth passes %ld", g_shadowDiag[0], g_shadowDiag[1], g_shadowDiag[2], g_mainDsvW,
             g_mainDsvH, g_offscreenSkips);
    {
        char miss[600];
        int mo = 0;
        AcquireSRWLockShared(&g_shadowMissLock);
        for (LONG q = 0; q < g_shadowMissN && mo < 560; ++q)
            mo += _snprintf(miss + mo, sizeof(miss) - mo, " %u:%ld", g_shadowMiss[q].indexCount, g_shadowMiss[q].n);
        ReleaseSRWLockShared(&g_shadowMissLock);
        miss[mo] = 0;
        logWrite("hide-shadow: unlearned shadow-pass draws (indices:count):%s", miss);
    }
    logWrite("hide-face: face-sized draws %ld, after a near-player upload %ld, recordable %ld", g_faceDiag[0],
             g_faceDiag[1], g_faceDiag[2]);
    AcquireSRWLockShared(&g_hidePassLock);
    for (LONG i = 0; i < g_hidePassN; ++i) {
        const HidePass &p = g_hidePass[i];
        logWrite("hide-pass %ld: rtv0=%p dsv=%p targets=%u %ux%u fmt=%u; skipped after-upload %ld, other-pass %ld, "
                 "index-key %ld, stream-out %ld", i, p.rtv0, p.dsv, p.rtCount, p.w, p.h, p.fmt, p.n[0], p.n[1], p.n[2],
                 p.n[3]);
    }
    ReleaseSRWLockShared(&g_hidePassLock);
}
} // namespace hideplayer

static bool isPlausibleCombinedViewProjection(const float *f);
static void describeView(ID3D11View *view, UINT *w, UINT *hgt, UINT *fmt);

// Shadow-pass probe (see g_shadowProbe): which binding the player's bones are
// uploaded under besides the depth and main passes.
static void shadowProbe(CamCtxHooks *h, const float *f, size_t size) {
    // Shadow pass only: searches its uploads for this frame's pelvis by the
    // rotation part alone.  Bone translations are relative to the rendering
    // camera, which in the shadow pass is the light, so whole matrices never
    // matched (v79); rotations do not depend on the camera.
    if (!h->shadowPass || size < 48 || size > 16384 || g_pelvisMatHead == 0) return;
    const size_t nfl = size / 4;
    UINT found = 0xFFFFFFFF;
    static const int rot[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (size_t o = 0; o + 12 <= nfl && found == 0xFFFFFFFF; o += 4)
        for (int r = 0; r < 16; ++r) {
            if (f[o] != g_pelvisMat[r][0] || f[o] == 0.0f) continue;
            bool same = true;
            for (int k : rot) if (f[o + k] != g_pelvisMat[r][k]) { same = false; break; }
            if (same) { found = static_cast<UINT>(o * 4); break; }
        }
    if (found == 0xFFFFFFFF) return;
    void *rtv0 = h->rtCount > 0 ? h->rtv[0] : nullptr, *dsv = h->depthView;
    AcquireSRWLockExclusive(&g_shadowProbeLock);
    LONG i = 0;
    for (; i < g_shadowProbeN; ++i)
        if (g_shadowProbe[i].rtv0 == rtv0 && g_shadowProbe[i].dsv == dsv && g_shadowProbe[i].size == size &&
            g_shadowProbe[i].offset == found) break;
    if (i == g_shadowProbeN && i < 32) {
        ShadowProbe &p = g_shadowProbe[i];
        p = ShadowProbe{};
        p.rtv0 = rtv0; p.dsv = dsv; p.rtCount = h->rtCount;
        p.size = static_cast<UINT>(size); p.offset = found;
        describeView(reinterpret_cast<ID3D11View *>(rtv0 ? rtv0 : dsv), &p.w, &p.h, &p.fmt);
        ++g_shadowProbeN;
    }
    if (i < 32) {
        ++g_shadowProbe[i].n;
        memcpy(g_shadowProbe[i].vp, f, sizeof(g_shadowProbe[i].vp));
        memcpy(g_shadowProbe[i].m, f + found / 4, sizeof(g_shadowProbe[i].m));
    }
    ReleaseSRWLockExclusive(&g_shadowProbeLock);
}

bool pairedEye(const float *vpRows, float *eyeOut, float *bestDot, const float *nearCamera) {
    // Another upload stream carries an eye far from the scene (seen as
    // (4.0, 4.1, 3.9)) with the main camera's exact facing.  Facing alone
    // cannot tell them apart, so among entries facing the same way the eye
    // nearest the memory camera wins (live 2026-10-19).
    const float *fw = vpRows + 12;
    float best = -2.0f, bestDist = 3.4e38f, eye[3] = {};
    for (int k = 0; k < 256; ++k) {
        PairedEye &s = g_pairedEyes[k];
        LONG before = s.seq;
        if (before == 0 || (before & 1)) continue;
        float f0 = s.forward[0], f1 = s.forward[1], f2 = s.forward[2];
        float e0 = s.eye[0], e1 = s.eye[1], e2 = s.eye[2];
        MemoryBarrier();
        if (s.seq != before) continue;
        float d = f0 * fw[0] + f1 * fw[1] + f2 * fw[2];
        float dist = 0;
        if (nearCamera) {
            float dx = e0 - nearCamera[0], dy = e1 - nearCamera[1], dz = e2 - nearCamera[2];
            dist = dx * dx + dy * dy + dz * dz;
        }
        // The far stream copies the 4112-byte matrix exactly, so it always
        // won on facing; the real eye's own matrix differs in the last digits.
        // Gate by distance first (memory runs ~0.1 s ahead; 20 m is far more
        // than the camera can travel in that time), then take the best facing.
        if (nearCamera && dist > 20.0f * 20.0f) continue;
        if (d > best || (d == best && dist < bestDist)) {
            best = d;
            bestDist = dist;
            eye[0] = e0; eye[1] = e1; eye[2] = e2;
        }
    }
    if (bestDot) *bestDot = best;
    if (best < 0.99999f) return false;  // about 0.25 degrees
    memcpy(eyeOut, eye, sizeof(eye));
    return true;
}

static bool isPlausibleCombinedViewProjection(const float *f) {
    for (int i = 0; i < 16; ++i) {
        float v = f[i];
        if (!(v == v) || v > 1e6f || v < -1e6f) return false;  // NaN / runaway
    }
    // Orientation-independent structure of a camera-relative perspective
    // view-projection, read as rows (clip_i = dot(row_i, (p, 1))):
    //   row 3 = (forward, 0): a unit vector with no translation;
    //   row 2 = (forward, -near): same direction, small negative constant;
    //   rows 0 and 1 = right * xscale and up * yscale, no translation;
    //   rows 0, 1 and 3 mutually perpendicular.
    // The previous version required m00 < 0 and m22 < 0.  Both are components
    // of the camera's facing and change sign as the camera turns, so the
    // matrix stopped updating whenever the camera faced one half of the world
    // (Mario off screen) and flickered near the boundary (one-frame stutter).
    // Live 2026-10-19.
    const float *r0 = f, *r1 = f + 4, *r2 = f + 8, *r3 = f + 12;
    auto dot3 = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float l0 = std::sqrt(dot3(r0, r0)), l1 = std::sqrt(dot3(r1, r1)), l3 = std::sqrt(dot3(r3, r3));
    if (!nearlyEqual(l3, 1.0f, 0.02f)) return false;          // unit forward
    if (!(l0 > 0.3f && l0 < 8.0f) || !(l1 > 0.3f && l1 < 8.0f)) return false;
    if (!nearlyEqual(r0[3], 0.0f, 1e-4f) || !nearlyEqual(r1[3], 0.0f, 1e-4f) ||
        !nearlyEqual(r3[3], 0.0f, 1e-4f))
        return false;                                         // camera-relative
    if (!(r2[3] < 0.0f && r2[3] > -1.0f)) return false;       // -near
    for (int i = 0; i < 3; ++i)
        if (!nearlyEqual(r2[i], r3[i], 1e-3f)) return false;  // depth row = forward
    if (std::fabs(dot3(r0, r1)) > 0.02f * l0 * l1 || std::fabs(dot3(r0, r3)) > 0.02f * l0 ||
        std::fabs(dot3(r1, r3)) > 0.02f * l1)
        return false;                                         // perpendicular axes
    return true;
}

// Record a buffer width the first time it is seen and log it once.
static void noteBufferSize(UINT byteWidth, void *ctx, UINT bindFlags, UINT mapType) {
    LONG n = InterlockedCompareExchange(&g_camSizesUsed, 0, 0);
    for (LONG i = 0; i < n && i < kMaxCamSizes; ++i)
        if (g_camSizes[i] == byteWidth) return;
    LONG slot = InterlockedIncrement(&g_camSizesUsed) - 1;
    if (slot >= kMaxCamSizes) { InterlockedDecrement(&g_camSizesUsed); return; }
    g_camSizes[slot] = byteWidth;
    logWrite("cam: buffer width %u (bindFlags=%u mapType=%u) first seen on ctx=%p%s",
             byteWidth, bindFlags, mapType, ctx,
             isViewMatrixBuffer(byteWidth) ? "  <-- view-projection candidate" : "");
}

// Hooked Map.
HRESULT STDMETHODCALLTYPE camHookedMap(void *self, void *pResource, UINT subresource,
    D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE *pMapped) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->map) return E_FAIL;
    auto fn = reinterpret_cast<HRESULT(STDMETHODCALLTYPE *)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *)>(h->map);
    HRESULT hr = fn(self, pResource, subresource, mapType, mapFlags, pMapped);
    if (FAILED(hr) || !pMapped || !pMapped->pData || !pResource)
        return hr;

    D3D11_BUFFER_DESC desc = {};
    void **vt = *reinterpret_cast<void ***>(pResource);
    if (!vt || !vt[10]) return hr;
    typedef void (STDMETHODCALLTYPE *PfnGetDesc)(void *, D3D11_BUFFER_DESC *);
    reinterpret_cast<PfnGetDesc>(vt[10])(pResource, &desc);
    noteBufferSize(desc.ByteWidth, self, desc.BindFlags, static_cast<UINT>(mapType));
    if ((fxprobe::g_on && !(desc.BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_INDEX_BUFFER)) &&
         (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE))) ||
        (fxprobe::g_moveOn && fxprobe::g_flameHeld && desc.BindFlags == D3D11_BIND_SHADER_RESOURCE && desc.ByteWidth <= 65536)) {
        h->fxData = pMapped->pData; h->fxRes = pResource; h->fxBytes = desc.ByteWidth; h->fxBind = desc.BindFlags;
        return hr;
    }

    // Only constant buffers can carry a view-projection.  Vertex buffers pass a
    // naive shape check on raw float data and produced a bogus matrix, so the
    // bind flags are part of the signature, not a hint.
    // 16 bytes is the smallest constant buffer; small ones are tracked for the
    // eye-position probe, and the view-projection check requires 4112 anyway.
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.ByteWidth < 16) return hr;

    // Track the map on this context; contents are inspected at Unmap, because
    // the game writes the payload *after* Map returns.
    h->mapData = pMapped->pData;
    h->mapResource = pResource;
    h->mapByteWidth = desc.ByteWidth;
    return hr;
}

// Hooked Unmap.  The mapped bytes are only valid while the resource is mapped,
// so they are read here, before forwarding to the real Unmap.  Reading at Map
// time yields zeroes: the game writes the payload after Map returns.
void STDMETHODCALLTYPE camHookedUnmap(void *self, void *pResource, UINT subresource) {
    CamCtxHooks *h = findCamCtx(self);
    if (h && h->fxRes && h->fxRes == pResource) {
        float player[3], camera[3];
        if (readLivePositions(player, camera)) {
            if (fxprobe::g_on) fxprobe::scan(pResource, h->fxData, h->fxBytes, h->fxBind, player, camera);
            fxprobe::moveHandFire(h->fxData, h->fxBytes, h->fxBind, player);
        }
        h->fxRes = nullptr; h->fxData = nullptr;
        if (fxprobe::g_on) fxprobe::report();
    }
    if (h && h->mapData && h->mapResource == pResource) {
        const float *f = static_cast<const float *>(h->mapData);
        size_t size = h->mapByteWidth;
        if (g_diag) eyeProbeScan(f, size);
        // playerprobe::scan(f, size);  // diagnostic; found the 2288-byte transforms (v43)
        // Player transform for anchor=upload (see CamCtxHooks::upValid).
        if (size == 2288) { h->lastUpNear = false; h->lastUpPlayer = false; h->lastUpHeld = false; }
        if (g_diag) shadowProbe(h, f, size);
        if (size == 2288) h->shadowUpValid = false;
        if (size == 2288) h->shadowNear = false;
        if (size == 2288) h->fpPending = false;
        if (size == 2288 && !h->shadowPass && g_shadowFpOn) {
            uint64_t used = 0;
            for (int j = 0; j < 38; ++j) {
                const float *m = f + 112 + 12 * j;
                bool zero = true;
                for (int q = 0; q < 12 && zero; ++q) zero = m[q] == 0.0f;
                const bool identity = m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[1] == 0.0f && m[2] == 0.0f &&
                                      m[4] == 0.0f && m[6] == 0.0f && m[8] == 0.0f && m[9] == 0.0f;
                if (zero || identity) continue;
                used |= 1ull << j;
                const float r[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                memcpy(h->fpRot[j], r, sizeof(r));
            }
            if (__builtin_popcountll(used) >= 3) {
                h->fpUsed = used;
                h->fpRef = relativeRotations(h->fpRot, used);
                h->fpPending = h->fpRef >= 0;
            }
        }
        if (size == 2288 && h->shadowPass && g_shadowFpOn) {
            // Compare this palette's bone rotations with the knight's recent main-pass ones.
            float rot[38][9];
            uint64_t used = 0;
            for (int j = 0; j < 38; ++j) {
                const float *m = f + 112 + 12 * j;
                bool zero = true;
                for (int q = 0; q < 12 && zero; ++q) zero = m[q] == 0.0f;
                const bool identity = m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[1] == 0.0f && m[2] == 0.0f &&
                                      m[4] == 0.0f && m[6] == 0.0f && m[8] == 0.0f && m[9] == 0.0f;
                if (zero || identity) continue;
                used |= 1ull << j;
                const float r[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                memcpy(rot[j], r, sizeof(r));
            }
            InterlockedIncrement(&g_sdFpTested);
            bool match = false;
            const int ref = relativeRotations(rot, used);
            float bestErr = 1e9f;
            if (__builtin_popcountll(used) >= 3) {
                const DWORD now = GetTickCount();
                AcquireSRWLockShared(&g_palFpLock);
                for (int e = 0; e < 128 && !match; ++e) {
                    const PaletteFp &fp = g_palFp[e];
                    if (!fp.tick || now - fp.tick > 150 || fp.ref != ref) continue;
                    const uint64_t both = fp.used & used;
                    const int n = __builtin_popcountll(both);
                    if (n < 3 || n * 4 < __builtin_popcountll(used) * 3) continue;
                    int good = 0, tested = 0;
                    float errSum = 0;
                    for (int j = 0; j < 38; ++j) {
                        if (!(both & (1ull << j)) || j == ref) continue;
                        ++tested;
                        float err = 0;
                        for (int q = 0; q < 9; ++q) err = std::fmax(err, std::fabs(rot[j][q] - fp.rot[j][q]));
                        errSum += err;
                        if (err < 0.08f) ++good;
                    }
                    if (tested) bestErr = std::fmin(bestErr, errSum / tested);
                    match = good >= 3 && good * 4 >= tested * 3;
                }
                ReleaseSRWLockShared(&g_palFpLock);
            }
            if (bestErr < 1e8f) {
                const LONG e = static_cast<LONG>(bestErr * 1000.f);
                LONG cur = g_sdFpBestErrMilli;
                while (e < cur && InterlockedCompareExchange(&g_sdFpBestErrMilli, e, cur) != cur) cur = g_sdFpBestErrMilli;
            }
            if (match) {
                h->shadowNear = true;
                h->shadowNearDraws = 0;
                InterlockedIncrement(&g_sdFpMatched);
            }
        }
        if (size == 2288 && h->shadowPass && g_shadowHideOn) {
            // The knight's shadow by position (v161): learned keys alone flickered (the shadow
            // pass runs before the main pass, with this frame's pose).  A skinned upload whose
            // bones carry the body centre (0, 1, 0) to within 0.7 m of the knight horizontally,
            // three bones or more, read camera-relative or absolute.
            float pl[3], cm[3];
            if (readLivePositions(pl, cm)) {
                const float rel[3] = {pl[0] - cm[0], pl[1] - cm[1], pl[2] - cm[2]};
                int nearRel = 0, nearAbs = 0, bones = 0;
                for (int j = 0; j < 38; ++j) {
                    const float *m = f + 112 + 12 * j;
                    bool zero = true;
                    for (int q = 0; q < 12 && zero; ++q) zero = m[q] == 0.0f;
                    if (zero) continue;
                    const bool identity = m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[3] == 0.0f &&
                                          m[7] == 0.0f && m[11] == 0.0f;
                    if (identity) continue;
                    ++bones;
                    const float px = m[1] + m[3], py = m[5] + m[7], pz = m[9] + m[11];
                    const float rx = px - rel[0], rz = pz - rel[2], ax = px - pl[0], az = pz - pl[2];
                    if (rx * rx + rz * rz < 0.49f && std::fabs(py - rel[1] - 1.f) < 1.2f) ++nearRel;
                    if (ax * ax + az * az < 0.49f && std::fabs(py - pl[1] - 1.f) < 1.2f) ++nearAbs;
                }
                h->shadowNear = bones >= 3 && (nearRel >= 3 || nearAbs >= 3);
                h->shadowNearDraws = 0;
                h->shadowNearIb = h->shadowNearVb = nullptr;
                InterlockedIncrement(&g_sdUploads);
                if (h->shadowNear) InterlockedIncrement(&g_sdNear);
            }
        }
        if (size == 2288 && h->shadowPass) {
            for (int j = 0; j < 38; ++j) {
                const float *m = f + 112 + 12 * j;
                bool zero = true;
                for (int q = 0; q < 12 && zero; ++q) zero = m[q] == 0.0f;
                const bool identity = m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[1] == 0.0f && m[2] == 0.0f &&
                                      m[4] == 0.0f && m[6] == 0.0f && m[8] == 0.0f && m[9] == 0.0f;
                h->shadowUpRot[j] = (zero || identity) ? 0 : hideplayer::hashRotation(m);
            }
            h->shadowUpValid = h->shadowUpRot[0] != 0;
            h->shadowUpDraws = 0;
        }
        if (size == 2288 && isPlausibleCombinedViewProjection(f)) {
            InterlockedIncrement(&g_upDiag[0]);
            float player[3], camera[3], angle = 0;
            const float *upEye = f + 0x140 / 4;
            bool mainEye = false;
            if (readLivePositions(player, camera) && readLiveAngle(angle)) {
                // The player is also drawn in other passes (reflections); only
                // an upload whose eye is the main camera (within 3 m of the
                // memory camera) gives the main view's transform.
                // The eye field of the player's uploads is not the main
                // camera (it holds the (4.0, 4.1, 3.9)-like value), so it
                // cannot gate or identify the frame; the transform and the
                // view-projection beside it are still a matched pair.
                (void)upEye;
                mainEye = true;
            }
            if (mainEye) {
                InterlockedIncrement(&g_upDiag[1]);
                const float rel[3] = {player[0] - camera[0], player[1] - camera[1], player[2] - camera[2]};
                // Bone hashes for hiding: an upload is near the player when a
                // rigid palette entry maps the body centre (0, 1, 0) to within
                // g_hideRadiusCm (1.2 m; it was 2.5 m and took nearby NPCs' heads)
                // of the player horizontally.
                {
                    const float rad = static_cast<float>(g_hideRadiusCm) * 0.01f, rad2 = rad * rad;
                    bool closeToPlayer = false;
                    int bones = 0;
                    h->lastUpRigid = 0;
                    memcpy(h->prevUpHash, h->lastUpHash, sizeof(h->prevUpHash));
                    h->prevUpHeader = h->lastUpHeader;
                    {
                        unsigned long long hh = 1469598103934665603ull;
                        const unsigned char *b = reinterpret_cast<const unsigned char *>(f + 16);
                        for (int q = 0; q < 96 * 4; ++q) { hh ^= b[q]; hh *= 1099511628211ull; }
                        h->lastUpHeader = hh;
                    }
                    for (int j = 0; j < 38; ++j) {
                        const float *m = f + 112 + 12 * j;
                        const float c0[3] = {m[0], m[4], m[8]}, c1[3] = {m[1], m[5], m[9]}, c2[3] = {m[2], m[6], m[10]};
                        // Scaled bones (the face's shape bones) are kept for
                        // matching; only rigid ones count for nearness.  The
                        // face stayed visible while they were dropped (live
                        // 2026-10-20, v73).
                        bool zero = true;
                        for (int q = 0; q < 12 && zero; ++q) zero = m[q] == 0.0f;
                        const bool identity = m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[3] == 0.0f &&
                                              m[7] == 0.0f && m[11] == 0.0f;
                        h->lastUpHash[j] = (zero || identity) ? 0 : hideplayer::hashMatrix(m);
                        if (!playerprobe::rigid(c0, c1, c2)) {
                            if (!h->lastUpHash[j]) continue;
                            ++bones;
                            const float tx = m[3] - rel[0], tz = m[11] - rel[2];
                            if (tx * tx + tz * tz < rad2) closeToPlayer = true;
                            continue;
                        }
                        h->lastUpRigid |= 1ull << j;
                        const float px = m[1] + m[3] - rel[0], pz = m[9] + m[11] - rel[2];
                        if (!identity && px * px + pz * pz < rad2) closeToPlayer = true;
                        if (!identity) ++bones;
                    }
                    // Level pieces carry one world matrix; the player's skinned
                    // pieces several bones.  Counting single-matrix uploads as
                    // near the player filled each pass's record with level
                    // geometry (about 9000 a second), so the body was never
                    // recorded and level pieces could be learned (sky flashing
                    // with v69, live 2026-10-20).
                    {
                        const float *m = f + 112;
                        const float hx = m[3] + m[1] * 0.970f - m[2] * 0.054f - rel[0];
                        const float hz = m[11] + m[9] * 0.970f - m[10] * 0.054f - rel[2];
                        // 0.5 m (v170n; was 1 m): an NPC right next to the knight (Solaire, Pinwheel)
                        // passed, and with leftover knight bones in its palette its pieces were
                        // learned as his and hidden (Solaire's arms vanished up close).
                        h->lastUpHip0 = h->lastUpHash[0] != 0 && hx * hx + hz * hz < g_hipRadius2;
                    }
                    h->lastUpNear = closeToPlayer && bones >= 3;
                    // The knight's held item while he drinks (v170): a piece of one or two rigid
                    // bones placed within 0.8 m of him, between knee and head height.
                    h->lastUpHeld = false;
                    if (g_hideHeldItem) {
                        // Diagnostic (v170b): uploads near the knight while he drinks, to find the flask.
                        static int heldDiag = 0;
                        float best = 1e9f, bestDy = 0;
                        int nz = 0, rig = 0;
                        for (int j = 0; j < 38; ++j) {
                            if (!h->lastUpHash[j]) continue;
                            ++nz;
                            if (h->lastUpRigid & (1ull << j)) ++rig;
                            const float *m = f + 112 + 12 * j;
                            const float dx = m[3] - rel[0], dz = m[11] - rel[2];
                            const float d = std::sqrt(dx * dx + dz * dz);
                            if (d < best) { best = d; bestDy = m[7] - rel[1]; }
                        }
                        if (heldDiag < 0 && best < 2.f && nz <= 6) {  // off (v170e)
                            ++heldDiag;
                            logWrite("held-diag: upload bones %d (rigid %d, counted %d), nearest %.2f m, dy %.2f, near %d, learned %d",
                                     nz, rig, bones, best, bestDy, (closeToPlayer && bones >= 3) ? 1 : 0, h->lastUpPlayer ? 1 : 0);
                        }
                    }
                    if (g_hideHeldItem && g_heldHideOn && bones >= 1 && bones <= 2) {
                        bool all = true, any = false;
                        for (int j = 0; j < 38 && all; ++j) {
                            if (!h->lastUpHash[j]) continue;
                            const float *m = f + 112 + 12 * j;
                            const float dx = m[3] - rel[0], dy = m[7] - rel[1], dz = m[11] - rel[2];
                            any = true;
                            all = dx * dx + dz * dz < 0.8f * 0.8f && dy > 0.3f && dy < 2.0f;
                        }
                        if (any && all) {
                            h->lastUpHeld = true;
                            h->lastUpHeldDraws = 0;
                            static DWORD lastHeldLog = 0;
                            if (GetTickCount() - lastHeldLog > 1000) {
                                lastHeldLog = GetTickCount();
                                logWrite("held: hiding a %d-bone piece at the knight's hands (drinking)", bones);
                            }
                        }
                    }
                    // Fingerprint of the knight's palette for the shadow pass (v166).
                    if (h->lastUpNear && g_shadowFpOn) {
                        AcquireSRWLockExclusive(&g_palFpLock);
                        PaletteFp &fp = g_palFp[g_palFpNext];
                        g_palFpNext = (g_palFpNext + 1) % 128;
                        fp.tick = GetTickCount();
                        fp.used = 0;
                        for (int j = 0; j < 38; ++j) {
                            const float *m = f + 112 + 12 * j;
                            if (!h->lastUpHash[j]) continue;
                            fp.used |= 1ull << j;
                            const float r[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                            memcpy(fp.rot[j], r, sizeof(r));
                        }
                        fp.ref = relativeRotations(fp.rot, fp.used);
                        ReleaseSRWLockExclusive(&g_palFpLock);
                    }
                    // A piece whose own slot 0 differs from the previous upload
                    // while every later slot repeats it carries leftovers: a
                    // bonfire piece drawn after the face held the face's head
                    // and neck bones and was learned (live 2026-10-20, v77).
                    {
                        bool later = false, allSame = true;
                        for (int j = 1; j < 38; ++j) {
                            if (!h->lastUpHash[j]) continue;
                            later = true;
                            if (h->lastUpHash[j] != h->prevUpHash[j]) { allSame = false; break; }
                        }
                        h->lastUpLeftover = later && allSame && h->lastUpHash[0] != h->prevUpHash[0];
                        // Whole palette repeated, slot 0 included, while the
                        // rest of the buffer changed: a draw that does not use
                        // the palette (a level piece) over the player's
                        // leftovers.  Walls were learned this way (v78).
                        h->lastUpCopied = allSame && h->lastUpHash[0] == h->prevUpHash[0] && h->lastUpHash[0] &&
                                          h->lastUpHeader != h->prevUpHeader;
                    }
                    h->lastUpPlayer = false;
                    h->lastUpDraws = 0;
                    h->lastUpIb = h->lastUpVb = nullptr;
                }
                // Candidates: upright transforms within 0.6 m facing the
                // player's way, each with the feet offset in its own axes.
                int candK[16];
                float candL[16][3];
                bool candStrict[16];  // essentially untilted (< 0.06 degrees)
                int nCand = 0;
                for (int k = 112; k + 12 <= 572 && nCand < 16; k += 12) {
                    const float *m = f + k;
                    const float dx = m[3] - rel[0], dy = m[7] - rel[1], dz = m[11] - rel[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (!(d2 < (k == 112 ? 2.5f * 2.5f : 1.5f * 1.5f))) continue;  // slot 0: rolls swing it
                    const float c0[3] = {m[0], m[4], m[8]}, c1[3] = {m[1], m[5], m[9]}, c2[3] = {m[2], m[6], m[10]};
                    const float l1 = std::sqrt(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
                    const bool isRigid = playerprobe::rigid(c0, c1, c2) && l1 >= 1e-6f;
                    // Bone slot 0 only.  The game's part files (FLVER2, parsed
                    // 2026-10-20) list each mesh's bones in skeleton order, so
                    // slot 0 of every body and leg mesh is the Pelvis; arms
                    // start at R_Forearm and the head at Spine1.  The pelvis
                    // is therefore the slot-0 matrix repeated across the
                    // player's uploads in a pass.  Upright within 25 degrees
                    // so a forearm cannot stand in for it.
                    // Gated by the hip joint (the pelvis bind position (0, 0.970,
                    // -0.054) through the matrix), not by tilt or by the
                    // matrix's own translation: a dodge roll turns the pelvis
                    // over and swung both outside the old limits.  The repeat
                    // count at draw time separates the pelvis from the rest.
                    const float hipX = m[3] + m[1] * 0.970f - m[2] * 0.054f;
                    const float hipY = m[7] + m[5] * 0.970f - m[6] * 0.054f;
                    const float hipZ = m[11] + m[9] * 0.970f - m[10] * 0.054f;
                    const float hx = hipX - rel[0], hy = hipY - rel[1], hz = hipZ - rel[2];
                    if (k == 112 && isRigid && hx * hx + hz * hz < 0.8f * 0.8f && hy > -0.5f && hy < 2.0f) {
                        int at = -1;
                        for (int q = 0; q < h->passCandN; ++q) {
                            const float *pp = h->passCands[q].pos;
                            if (std::fabs(pp[0] - m[3]) < 1e-3f && std::fabs(pp[1] - m[7]) < 1e-3f &&
                                std::fabs(pp[2] - m[11]) < 1e-3f) { at = q; break; }
                        }
                        if (at < 0 && h->passCandN < 64) {
                            at = h->passCandN++;
                            CamCtxHooks::PassCand &pc = h->passCands[at];
                            pc.pos[0] = m[3]; pc.pos[1] = m[7]; pc.pos[2] = m[11];
                            const float r[9] = {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]};
                            memcpy(pc.rot, r, sizeof(r));
                            memcpy(pc.vp, f, sizeof(pc.vp));
                            pc.count = 0;
                            pc.hash = hideplayer::hashMatrix(m);
                            memcpy(pc.m, m, sizeof(pc.m));
                        }
                        if (at >= 0) ++h->passCands[at].count;
                    }
                    if (!(d2 < 0.8f * 0.8f)) { InterlockedIncrement(&g_upReject[0]); continue; }
                    // Near-upright only.  A strict 0.06-degree limit found the
                    // root while standing but nothing at all while walking
                    // (capture counters, live 2026-10-20); the lock below
                    // still tells the root from other objects.
                    if (!playerprobe::rigid(c0, c1, c2) || l1 < 1e-6f) { InterlockedIncrement(&g_upReject[1]); continue; }
                    static float tiltCos = 0.99939f;
                    static LONG tiltFor = -1;
                    if (tiltFor != g_upTiltMilliDeg) {
                        tiltFor = g_upTiltMilliDeg;
                        tiltCos = std::cos(tiltFor / 1000.0f * 3.14159265f / 180.0f);
                    }
                    if (c1[1] < tiltCos * l1) { InterlockedIncrement(&g_upReject[2]); continue; }
                    if (std::fabs(playerprobe::wrapPi(std::atan2(c2[0], c2[2]) - angle)) > 0.8f) {
                        InterlockedIncrement(&g_upReject[3]);
                        continue;
                    }
                    const float l0 = std::sqrt(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]),
                                l2 = std::sqrt(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]);
                    const float d[3] = {-dx, -dy, -dz};  // feet minus transform
                    candL[nCand][0] = (c0[0] * d[0] + c0[1] * d[1] + c0[2] * d[2]) / l0;
                    candL[nCand][1] = (c1[0] * d[0] + c1[1] * d[1] + c1[2] * d[2]) / l1;
                    candL[nCand][2] = (c2[0] * d[0] + c2[1] * d[1] + c2[2] * d[2]) / l2;
                    candStrict[nCand] = std::fabs(c1[0]) <= 1e-3f * l1 && std::fabs(c1[2]) <= 1e-3f * l1 &&
                                        std::fabs(c0[1]) <= 1e-3f * l0 && std::fabs(c2[1]) <= 1e-3f * l2;
                    candK[nCand++] = k;
                }
                // Learn only while slow: memory runs a frame or two ahead, so
                // at a run the measured offset is skewed along the motion.
                static float lastP[3];
                static double lastT = 0;
                const double tNowP = playerprobe::seconds();
                const double dtP = tNowP - lastT;
                bool slow = false;
                if (dtP > 0.05) {
                    const float mx = player[0] - lastP[0], mz = player[2] - lastP[2];
                    slow = std::sqrt(mx * mx + mz * mz) / dtP < 0.5;
                    memcpy(lastP, player, sizeof(lastP));
                    lastT = tNowP;
                }
                // The lock learns only from untilted transforms: standing
                // still, that is the root alone.  Learning from every near-
                // upright candidate let foot bones (more numerous) take over,
                // and Mario sat beside the character (live 2026-10-20).
                if (slow)
                    for (int c = 0; c < nCand; ++c)
                        if (candStrict[c]) rootlock::observe(candL[c]);
                int bestK = -1;
                float lockC[3];
                if (nCand) InterlockedIncrement(&g_upDiag[2]);
                if (nCand && rootlock::locked(lockC)) {
                    InterlockedIncrement(&g_upDiag[3]);
                    float bestD = 3e38f;
                    for (int c = 0; c < nCand; ++c) {
                        float e2 = 0;
                        if (rootlock::belongsToLocked(candL[c], &e2) && e2 < bestD) { bestD = e2; bestK = candK[c]; }
                    }
                }
                if (bestK >= 0) {
                    InterlockedIncrement(&g_upDiag[4]);
                    const float *m = f + bestK;
                    {
                        const float l = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
                        const float c = l > 1e-6f ? m[5] / l : 1.0f;
                        const LONG md = static_cast<LONG>(std::acos(c > 1.0f ? 1.0f : c) * 180.0f / 3.14159265f * 1000.0f);
                        InterlockedAdd64(&g_upTiltSum, md);
                        InterlockedIncrement(&g_upTiltCount);
                        if (md > g_upTiltMax) g_upTiltMax = md;
                    }
                    memcpy(h->upVp, f, sizeof(h->upVp));
                    h->upPos[0] = m[3]; h->upPos[1] = m[7]; h->upPos[2] = m[11];
                    const float rot[9] = {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]};
                    memcpy(h->upRot, rot, sizeof(rot));
                    h->upIndex = static_cast<UINT>(bestK);
                    h->upTick = GetTickCount();
                    h->upT = playerprobe::seconds();
                    h->upValid = true;
                    AcquireSRWLockExclusive(&g_upLock);
                    g_upLatest.tick = h->upTick;
                    g_upLatest.index = h->upIndex;
                    memcpy(g_upLatest.vp, h->upVp, sizeof(g_upLatest.vp));
                    memcpy(g_upLatest.pos, h->upPos, sizeof(g_upLatest.pos));
                    memcpy(g_upLatest.rot, h->upRot, sizeof(g_upLatest.rot));
                    for (int i = 0; i < 3; ++i) g_upLatest.eye[i] = f[0x140 / 4 + i];
                    g_upLatest.t = h->upT;
                    g_upLatestValid = true;
                    g_upRing[g_upRingHead++ & 15] = g_upLatest;
                    ReleaseSRWLockExclusive(&g_upLock);
                }
            }
        }
        if (size >= 0x140 + 12 && isPlausibleCombinedViewProjection(f)) {
            const float *e = f + 0x140 / 4;
            if (std::isfinite(e[0]) && std::isfinite(e[1]) && std::isfinite(e[2]) &&
                (e[0] != 0.0f || e[1] != 0.0f || e[2] != 0.0f)) {
              // Per-context copy, main camera only (within 20 m of the memory
              // camera; the far stream sits about 200 m away).
              if (g_world.cameraValid) {
                  const float dx = e[0] - g_world.camera[0], dy = e[1] - g_world.camera[1],
                              dz = e[2] - g_world.camera[2];
                  // Main view only: the context's own main-scene matrix must
                  // face the same way (other views recorded on this context
                  // put Mario 16 m away or behind the camera, live 2026-10-19).
                  const bool sameView = h->vpValid &&
                      f[12] * h->vp[12] + f[13] * h->vp[13] + f[14] * h->vp[14] >= 0.99999f;
                  if (sameView && dx * dx + dy * dy + dz * dz < 3.0f * 3.0f) {
                      for (int i = 0; i < 3; ++i) { h->eye[i] = e[i]; h->eyeForward[i] = f[12 + i]; }
                      memcpy(h->eyeVp, f, sizeof(h->eyeVp));
                      h->eyeTick = GetTickCount();
                      h->eyeValid = true;
                  }
              }
              // Record each distinct (facing, eye) once.  One stream repeats a
              // far-away eye about 200 times a frame; stored every time it
              // pushed the real eye out of the ring and 70% of draws fell
              // back to a stale camera (live 2026-10-19).
              // Compared with the 32 most recent entries: repeats arrive back
              // to back, and a 256-entry ring keeps several frames of distinct
              // eyes so the real one is not pushed out by other views.
              bool known = false;
              const LONG headNow = g_pairedEyeHead;
              for (int k = 1; k <= 32 && !known; ++k) {
                  const PairedEye &q = g_pairedEyes[(headNow - k) & 255];
                  known = q.seq != 0 && !(q.seq & 1) && q.eye[0] == e[0] && q.eye[1] == e[1] &&
                          q.eye[2] == e[2] && q.forward[0] == f[12] && q.forward[1] == f[13] &&
                          q.forward[2] == f[14];
              }
              if (!known) {
                PairedEye &slot = g_pairedEyes[(InterlockedIncrement(&g_pairedEyeHead) - 1) & 255];
                InterlockedIncrement(&slot.seq);  // odd: being written
                for (int i = 0; i < 3; ++i) { slot.forward[i] = f[12 + i]; slot.eye[i] = e[i]; }
                InterlockedIncrement(&slot.seq);  // even: complete
                static volatile LONG pairSizes[4] = {};
                const int si = size == 2288 ? 0 : size == 1040 ? 1 : size == 4112 ? 2 : 3;
                InterlockedIncrement(&pairSizes[si]);
                static DWORD lastPairLog = 0;
                DWORD nowPair = GetTickCount();
                if (nowPair - lastPairLog > 5000) {
                    lastPairLog = nowPair;
                    logWrite("eye: paired uploads by size 2288=%ld 1040=%ld 4112=%ld other=%ld (last size %u)",
                             pairSizes[0], pairSizes[1], pairSizes[2], pairSizes[3], (unsigned)size);
                }
              }
            }
        }
        if ((size == 2288 || size == 1040) && std::isfinite(f[0x140 / 4]) &&
            std::isfinite(f[0x140 / 4 + 1]) && std::isfinite(f[0x140 / 4 + 2])) {
            // The same slot also carries the eye of other views (shadow or
            // reflection passes); taking those made Mario flash every frame
            // (live 2026-10-19).  Only an eye within 2 m of the follow camera
            // read from memory is the main camera; memory and engine copies are
            // at most a frame or two apart.
            const float *e = f + 0x140 / 4;
            WorldObservation we = g_world;
            bool mainEye = true;
            if (we.cameraValid) {
                float dx = e[0] - we.camera[0], dy = e[1] - we.camera[1], dz = e[2] - we.camera[2];
                mainEye = dx * dx + dy * dy + dz * dz < 4.0f;
            }
            static volatile LONG eyeTaken = 0, eyeSkipped = 0;
            InterlockedIncrement(mainEye ? &eyeTaken : &eyeSkipped);
            static DWORD lastEyeLog = 0;
            DWORD nowEye = GetTickCount();
            if (nowEye - lastEyeLog > 5000) {
                lastEyeLog = nowEye;
                logWrite("eye: %u-byte main-camera eyes taken %ld, other views skipped %ld", (unsigned)size,
                         eyeTaken, eyeSkipped);
            }
            if (mainEye) {
                float *dst = size == 2288 ? g_eyePos2288 : g_eyePos1040;
                for (int i = 0; i < 3; ++i) dst[i] = e[i];
                InterlockedExchange(size == 2288 ? &g_eyeValid2288 : &g_eyeValid1040, 1);
            }
        }

        // The combined view-projection lives at offset 0 of the 2288/4112-byte
        // constant buffers.  A separate axis-aligned projection also circulates
        // (1040-byte buffer, m11 = 2.5386, static); it is rejected by the
        // rotated-basis requirement rather than by buffer size alone.
        //
        // Both the 2288 and 4112 buffers carry a valid view-projection, but they
        // are distinct shader variants with different FOV terms, so they must
        // not be mixed: an m11 of 1.6883 from one variant was interleaved with
        // 2.5386 from the other.  The 4112-byte buffer is the main-scene one.
        // The 4112-byte buffer also carries view-projections of OTHER views
        // (which ones depends on what is on screen).  Only a matrix under which
        // the player sits roughly where a follow camera frames them is taken as
        // the main camera: with the worker's player/camera positions, the point
        // 1 m above the feet must project in front of the camera and inside a
        // generous central window.  Without positions yet, any plausible matrix
        // is accepted as before.  (Direction-dependent misplacement, live
        // 2026-10-19: the camera position was right, the matrix was not.)
        bool mainView = false;
        if (size == kMainViewProjectionBytes && isPlausibleCombinedViewProjection(f)) {
            memcpy(g_rawVp, f, sizeof(g_rawVp));
            InterlockedExchange(&g_rawVpValid, 1);
            WorldObservation wv = g_world;
            mainView = true;
            if (g_vpFilter && wv.playerValid && wv.cameraValid) {
                const float p[3] = {wv.player[0] - wv.camera[0], wv.player[1] + 1.0f - wv.camera[1],
                                    wv.player[2] - wv.camera[2]};
                float c[4];
                for (int r = 0; r < 4; ++r)
                    c[r] = f[r * 4] * p[0] + f[r * 4 + 1] * p[1] + f[r * 4 + 2] * p[2] + f[r * 4 + 3];
                mainView = c[3] > 0.5f && std::fabs(c[0] / c[3]) < 0.7f && std::fabs(c[1] / c[3]) < 0.9f;
                static volatile LONG accepted = 0, rejected = 0;
                if (mainView) InterlockedIncrement(&accepted); else InterlockedIncrement(&rejected);
                static DWORD lastViewLog = 0;
                DWORD nowView = GetTickCount();
                if (!mainView && nowView - lastViewLog > 2000) {
                    lastViewLog = nowView;
                    logWrite("cam: rejected non-main view (accepted %ld, rejected %ld): m00=%.3f m11=%.3f "
                             "fwd=(%.3f,%.3f,%.3f) player ndc=(%.2f,%.2f) w=%.2f",
                             accepted, rejected, f[0], f[5], f[12], f[13], f[14],
                             c[3] != 0 ? c[0] / c[3] : 0.f, c[3] != 0 ? c[1] / c[3] : 0.f, c[3]);
                }
            }
        }
        if (mainView) {
            CameraState &cam = getCameraStateInternal();
            bool first = !cam.valid;
            bool changed = first;
            for (int i = 0; i < 16 && !changed; ++i)
                if (cam.viewProjection.m[i] != f[i]) changed = true;
            for (int i = 0; i < 16; ++i) cam.viewProjection.m[i] = f[i];
            for (int i = 0; i < 16; ++i) cam.viewProjectionAlt.m[i] = f[48 + i];
            // Player draw transform in this same upload (see CameraState).
            if (cam.positionsValid) {
                const float expect[3] = {cam.playerPos[0] - cam.cameraPos[0],
                                         cam.playerPos[1] - cam.cameraPos[1],
                                         cam.playerPos[2] - cam.cameraPos[2]};
                const size_t floats = size / sizeof(float);
                float best = 0.5f;  // metres
                int bestAt = -1;
                for (size_t k = 112; k + 12 <= floats; k += 12) {
                    const float *m = f + k;
                    float tx = m[3], ty = m[7], tz = m[11];
                    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) continue;
                    // Upright transform: the Y axis row must be close to unit up.
                    if (std::fabs(m[5]) < 0.9f) continue;
                    float dx = tx - expect[0], dy = ty - expect[1], dz = tz - expect[2];
                    float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d < best) { best = d; bestAt = static_cast<int>(k); }
                }
                if (bestAt >= 0) {
                    const float *m = f + bestAt;
                    memcpy(cam.renderPlayerVp, f, sizeof(cam.renderPlayerVp));
                    cam.renderPlayerRel[0] = m[3]; cam.renderPlayerRel[1] = m[7]; cam.renderPlayerRel[2] = m[11];
                    const float rot[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                    memcpy(cam.renderPlayerRot, rot, sizeof(rot));
                    cam.renderPlayerError = best;
                    cam.renderPlayerValid = true;
                    static DWORD lastFoundLog = 0;
                    DWORD nowFound = GetTickCount();
                    if (nowFound - lastFoundLog > 3000) {
                        lastFoundLog = nowFound;
                        logWrite("render: player transform at float %d, rel=(%.3f,%.3f,%.3f), %.3f m from memory estimate",
                                 bestAt, m[3], m[7], m[11], best);
                    }
                }
            }
            // Per-context copy (see CamCtxHooks::vp).
            bool ctxChanged = !h->vpValid || memcmp(h->vp, f, sizeof(h->vp)) != 0;
            if (ctxChanged) {
                memcpy(h->vp, f, sizeof(h->vp));
                float player[3], camera[3];
                h->positionsValid = readLivePositions(player, camera);
                if (h->positionsValid)
                    for (int i = 0; i < 3; ++i) { h->cameraPos[i] = camera[i]; h->playerPos[i] = player[i]; }
                h->vpValid = true;
            }
            // The buffer is re-uploaded many times per frame with the same
            // matrix; positions are re-read only when the matrix changes.
            if (changed || !cam.positionsValid) {
                float player[3], camera[3];
                cam.positionsValid = readLivePositions(player, camera);
                if (cam.positionsValid)
                    for (int i = 0; i < 3; ++i) { cam.cameraPos[i] = camera[i]; cam.playerPos[i] = player[i]; }
            }
            cam.valid = true;
            cam.lastUpdate++;
            // Log every change, but rate-limited: uploads repeat with identical
            // content, so an unguarded log would either be silent or a flood.
            // Continuous variation under camera motion is the evidence that this
            // is a live view-projection and not a static constant.
            static DWORD lastLog = 0;
            DWORD nowLog = GetTickCount();
            // One-off survey of the rest of this buffer, to find the camera's
            // world position (the matrix itself is camera-relative).  A few
            // dumps spaced 4 s apart, so movement between them shows which
            // floats track the camera.
            static DWORD lastDump = 0;
            static int dumps = 0;
            if (dumps < 8 && nowLog - lastDump > 4000) {
                lastDump = nowLog;
                ++dumps;
                const size_t floats = size / sizeof(float);
                for (size_t base = 16; base < floats && base < 16 + 128; base += 16) {
                    char row[300];
                    int o = _snprintf(row, sizeof(row), "cbuf: dump %d f[%zu..%zu]", dumps, base, base + 15);
                    for (size_t i = base; i < base + 16 && i < floats && o < 280; ++i)
                        o += _snprintf(row + o, sizeof(row) - o, " %.3f", f[i]);
                    logWrite("%s", row);
                }
            }
            if (first || (changed && nowLog - lastLog > 1500)) {
                lastLog = nowLog;
                char line[256];
                int off = 0;
                for (int i = 0; i < 16; ++i)
                    off += _snprintf(line + off, sizeof(line) - off, "%s%.4f", i ? "," : "", f[i]);
                logWrite("cam: view-projection #%llu (%zu bytes) %s",
                         static_cast<unsigned long long>(cam.lastUpdate), size, line);
            }
        }
        h->mapData = nullptr;
        h->mapResource = nullptr;
        h->mapByteWidth = 0;
    }
    if (h && h->unmap) {
        auto fn = reinterpret_cast<void(STDMETHODCALLTYPE *)(void*, void*, UINT)>(h->unmap);
        fn(self, pResource, subresource);
    }
    // Buffer uploads are a reliable per-frame signal, so use them to refresh
    // host observation rather than depending on the swap chain's Present.
    hostOnCaptureTick();
}

// Hooked OMSetRenderTargets.
//
// Records each binding for pass identification and, when the binding being
// replaced was the geometry pass, issues the host test draw while those
// targets are still attached.  Drawing at the *end* of the pass rather than
// straight after it is bound matters: a draw issued immediately after binding
// would precede the game's own clear of the G-buffer (erasing it) and, on a
// deferred context, could precede the pass's RSSetViewports (rasterising
// nothing).  The game's call is then forwarded unchanged.
void STDMETHODCALLTYPE camHookedOMSetRenderTargets(void *self, UINT rtCount,
                                                   const void *const *rtv, void *depthView) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->omSetRenderTargets) return;
    hideplayer::classify(h);
    hideplayer::report();

    // Every matching pass is drawn into (no rate limit), so the marker is on
    // every frame rather than flashing; drawTestTriangle rate-limits its log.
    if (h->geometryPassBound) {
        h->geometryPassBound = false;
        drawTestTriangle(reinterpret_cast<ID3D11DeviceContext *>(self), h->geometryPassTargets,
                         h->rtv[0], h->depthView);
    }

    // Copy the binding: the game's array may be stack memory and is not valid
    // after this call returns.  Views are borrowed, never released.
    h->rtCount = (rtCount > kMaxRTViews) ? kMaxRTViews : rtCount;
    for (UINT i = 0; i < h->rtCount; ++i)
        h->rtv[i] = const_cast<void *>(rtv ? rtv[i] : nullptr);
    h->depthView = depthView;
    {
        bool anyRt = false;
        for (UINT i = 0; i < h->rtCount; ++i) if (h->rtv[i]) anyRt = true;
        bool shadow = false;
        if (!anyRt && depthView) {
            // Cached per context: a cache shared by all render threads could
            // pair one thread's view with another's answer (the shadow
            // flickered, live 2026-10-20, v81).
            if (depthView == h->shadowDsvSeen) shadow = h->shadowDsvIs;
            else {
                UINT w = 0, hh = 0, fmt = 0;
                describeView(reinterpret_cast<ID3D11View *>(depthView), &w, &hh, &fmt);
                shadow = w == hh && w >= 1024;
                h->shadowDsvSeen = depthView;
                h->shadowDsvIs = shadow;
                h->dsvW = w; h->dsvH = hh;
            }
        }
        h->shadowPass = shadow;
        if (!anyRt && depthView) {
            InterlockedIncrement(&g_sdBinds);
            if (shadow) InterlockedIncrement(&g_sdShadowBinds);
            const LONG w = static_cast<LONG>(h->dsvW), hh2 = static_cast<LONG>(h->dsvH);
            for (int q = 0; q < 6; ++q) {
                if (g_sdSizeN[q] && (g_sdSizeW[q] != w || g_sdSizeH[q] != hh2)) continue;
                if (!g_sdSizeN[q]) { g_sdSizeW[q] = w; g_sdSizeH[q] = hh2; }
                InterlockedIncrement(&g_sdSizeN[q]);
                if (shadow) InterlockedIncrement(&g_sdSizeShadow[q]);
                break;
            }
        }
        {
            static volatile LONG lastSd = 0;
            const LONG nowSd = static_cast<LONG>(GetTickCount()), prevSd = lastSd;
            if (nowSd - prevSd > 5000 && InterlockedCompareExchange(&lastSd, nowSd, prevSd) == prevSd) {
                char sizes[256];
                int so = 0;
                for (int q = 0; q < 6 && g_sdSizeN[q]; ++q)
                    so += snprintf(sizes + so, sizeof(sizes) - so, " %ldx%ld:%ld(shadow %ld)", g_sdSizeW[q], g_sdSizeH[q],
                                   g_sdSizeN[q], g_sdSizeShadow[q]);
                sizes[so] = 0;
                static LONG prevSeen = 0, prevTrusted = 0, prevSkipped = 0;
                const LONG seen = g_shadowDiag[0], trustedN = g_shadowDiag[1], skippedN = g_shadowDiag[2];
                logWrite("shadowdiag: depth-only binds %ld (shadow %ld), sizes%s; shadow draws %ld, learned knight pieces "
                         "among them %ld, skipped by key %ld; fingerprint: palettes tested %ld, matched %ld (best mean "
                         "error %.3f); skipped %ld",
                         g_sdBinds, g_sdShadowBinds, sizes, seen - prevSeen, trustedN - prevTrusted, skippedN - prevSkipped,
                         g_sdFpTested, g_sdFpMatched, g_sdFpBestErrMilli / 1000.0, g_shadowNearSkips);
                g_sdFpTested = g_sdFpMatched = 0;
                g_sdFpBestErrMilli = 100000;
                prevSeen = seen; prevTrusted = trustedN; prevSkipped = skippedN;
                g_sdBinds = g_sdShadowBinds = g_sdUploads = g_sdNear = g_sdDraws = 0;
                g_shadowNearSkips = 0;
                for (int q = 0; q < 6; ++q) g_sdSizeN[q] = g_sdSizeShadow[q] = 0, g_sdSizeW[q] = g_sdSizeH[q] = 0;
            }
        }
        // Any binding with a depth target: note its size for the offscreen test.
        if (depthView && depthView != h->shadowDsvSeen) {
            UINT w = 0, hh = 0, fmt = 0;
            describeView(reinterpret_cast<ID3D11View *>(depthView), &w, &hh, &fmt);
            h->shadowDsvSeen = depthView;
            h->shadowDsvIs = !anyRt && w == hh && w >= 1024;
            h->dsvW = w; h->dsvH = hh;
        }
        h->offscreenPass = depthView && g_mainDsvW > 0 &&
                           (static_cast<LONG>(h->dsvW) != g_mainDsvW || static_cast<LONG>(h->dsvH) != g_mainDsvH);
    }
    noteRenderTargetBinding(self, h->rtCount, h->rtv, depthView);
    h->geometryPassBound =
        looksLikeGeometryPass(h->rtCount, const_cast<const void *const *>(h->rtv), depthView);
    if (h->geometryPassBound) {
        h->passStart = playerprobe::seconds();
        h->passCandN = 0;
        UINT targets = 0;
        for (UINT i = 0; i < h->rtCount; ++i) if (h->rtv[i]) ++targets;
        h->geometryPassTargets = targets;
    }

    auto fn = reinterpret_cast<void(STDMETHODCALLTYPE *)(void *, UINT, const void *const *, void *)>(
        h->omSetRenderTargets);
    fn(self, rtCount, rtv, depthView);
}

// Describe a binding's shape: how many render targets, each one's format and
// dimensions, and the depth format.  This is the fingerprint a pass is
// recognised by -- the G-buffer is a wide multi-target with scene depth, while
// the back buffer is a single single-sampled colour target.
struct RtFingerprint {
    UINT count;
    UINT widths[kMaxRTViews];
    UINT heights[kMaxRTViews];
    DXGI_FORMAT formats[kMaxRTViews];
    UINT sampleCounts[kMaxRTViews];
    UINT usages[kMaxRTViews];
    UINT bindFlagses[kMaxRTViews];
    DXGI_FORMAT depthFormat;
    UINT depthWidth, depthHeight;
    UINT depthSample, depthUsage, depthBind;
};

static bool describeBinding(UINT count, void *const *rtv, void *depthView, RtFingerprint *out) {
    if (!out) return false;
    *out = RtFingerprint{};
    out->count = count;
    for (UINT i = 0; i < count && i < kMaxRTViews; ++i) {
        ID3D11RenderTargetView *view = reinterpret_cast<ID3D11RenderTargetView *>(rtv[i]);
        if (!view) continue;
        ResourceDescLocal desc = {};
        ID3D11Resource *res = nullptr;
        typedef void(STDMETHODCALLTYPE *PfnGetResource)(void *, ID3D11Resource **);
        reinterpret_cast<PfnGetResource>((*reinterpret_cast<void ***>(view))[kViewGetResourceSlot])(
            view, &res);
        if (res) {
            if (i == 0) dumpResourceDesc(res);
            if (readResourceDesc(res, &desc) && i < kMaxRTViews) {
                out->widths[i] = desc.Width;
                out->heights[i] = desc.Height;
                out->formats[i] = desc.Format;
                out->sampleCounts[i] = desc.SampleDesc_Count;
                out->usages[i] = desc.Usage;
                out->bindFlagses[i] = desc.BindFlags;
            }
            res->Release();
        }
    }
    ID3D11DepthStencilView *dsv = reinterpret_cast<ID3D11DepthStencilView *>(depthView);
    if (dsv) {
        ResourceDescLocal desc = {};
        ID3D11Resource *res = nullptr;
        typedef void(STDMETHODCALLTYPE *PfnGetResource)(void *, ID3D11Resource **);
        reinterpret_cast<PfnGetResource>((*reinterpret_cast<void ***>(dsv))[kViewGetResourceSlot])(
            dsv, &res);
        bool got = res && readResourceDesc(res, &desc);
        if (res) res->Release();
        if (got) {
            out->depthFormat = desc.Format;
            out->depthWidth = desc.Width;
            out->depthHeight = desc.Height;
            out->depthSample = desc.SampleDesc_Count;
            out->depthUsage = desc.Usage;
            out->depthBind = desc.BindFlags;
        }
    }
    return true;
}

static bool sameFingerprint(const RtFingerprint &a, const RtFingerprint &b) {
    if (a.count != b.count || a.depthFormat != b.depthFormat) return false;
    if (a.depthWidth != b.depthWidth || a.depthHeight != b.depthHeight) return false;
    for (UINT i = 0; i < a.count && i < kMaxRTViews; ++i) {
        if (a.formats[i] != b.formats[i]) return false;
        if (a.widths[i] != b.widths[i] || a.heights[i] != b.heights[i]) return false;
        if (a.sampleCounts[i] != b.sampleCounts[i]) return false;
    }
    return true;
}

// Log each distinct binding shape once, with a call count, then stay quiet.
// Remastered binds render targets thousands of times a second; logging every
// call would drown the log and slow the game.  The distinct shapes and their
// relative frequencies are what identify the passes.
static void noteRenderTargetBinding(void *ctx, UINT count, void *const *rtv, void *depthView) {
    static RtFingerprint seen[kMaxSeenBindings] = {};
    static volatile LONG seenCount = 0;

    RtFingerprint fp = {};
    if (!describeBinding(count, rtv, depthView, &fp)) return;

    LONG n = InterlockedCompareExchange(&seenCount, 0, 0);
    for (LONG i = 0; i < n && i < kMaxSeenBindings; ++i) {
        if (sameFingerprint(seen[i], fp)) {
            InterlockedIncrement(&g_bindingHits[i]);
            // Every 2000 hits, refresh the count so frequency is visible without
            // logging on every single call.
            if ((g_bindingHits[i] % 2000) == 0)
                logWrite("rt: binding #%ld still active (%ld hits) count=%u fmt0=%d %ux%u depth=%d %ux%u",
                         i, g_bindingHits[i], fp.count, static_cast<int>(fp.formats[0]),
                         fp.widths[0], fp.heights[0], static_cast<int>(fp.depthFormat),
                         fp.depthWidth, fp.depthHeight);
            return;
        }
    }
    LONG slot = InterlockedIncrement(&seenCount) - 1;
    if (slot >= kMaxSeenBindings) { InterlockedDecrement(&seenCount); return; }
    seen[slot] = fp;
    g_bindingHits[slot] = 1;

    char fmts[192] = {};
    int off = 0;
    for (UINT i = 0; i < fp.count && i < kMaxRTViews && off < 150; ++i)
        off += _snprintf(fmts + off, sizeof(fmts) - off, "%s%d:%ux%ux%u", i ? "," : "",
                         static_cast<int>(fp.formats[i]), fp.widths[i], fp.heights[i],
                         fp.sampleCounts[i]);
    logWrite("rt: NEW binding #%ld ctx=%p count=%u rtv=[%s] depth=%d %ux%u",
             slot, ctx, fp.count, fmts, static_cast<int>(fp.depthFormat),
             fp.depthWidth, fp.depthHeight);
    // Log usage/bind flags for the first few bindings.  These identify what a
    // target actually is -- render-target versus shader-resource versus depth --
    // and are the basis for recognising the geometry pass.
    if (slot < 4)
        for (UINT i = 0; i < fp.count && i < kMaxRTViews; ++i)
            logWrite("rt:   rtv[%u] %ux%u fmt=%d sample=%u usage=%u bind=%u",
                     i, fp.widths[i], fp.heights[i], static_cast<int>(fp.formats[i]),
                     fp.sampleCounts[i], fp.usages[i], fp.bindFlagses[i]);
    if (slot < 4 && fp.depthFormat)
        logWrite("rt:   depth %ux%u fmt=%d sample=%u usage=%u bind=%u",
                 fp.depthWidth, fp.depthHeight, static_cast<int>(fp.depthFormat),
                 fp.depthSample, fp.depthUsage, fp.depthBind);
}


// Size and format of a view's texture (0 when not a 2D texture).
static void describeView(ID3D11View *view, UINT *w, UINT *hgt, UINT *fmt) {
    *w = *hgt = *fmt = 0;
    if (!view) return;
    ID3D11Resource *res = nullptr;
    view->GetResource(&res);
    if (!res) return;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d = {};
        reinterpret_cast<ID3D11Texture2D *>(res)->GetDesc(&d);
        *w = d.Width; *hgt = d.Height; *fmt = d.Format;
    }
    res->Release();
}

// Counts a skipped draw against the context's current render-target binding.
static void notePassSkip(CamCtxHooks *h, int method) {
    void *rtv0 = h->rtCount > 0 ? h->rtv[0] : nullptr, *dsv = h->depthView;
    AcquireSRWLockShared(&g_hidePassLock);
    const LONG n = g_hidePassN;
    for (LONG i = 0; i < n; ++i)
        if (g_hidePass[i].rtv0 == rtv0 && g_hidePass[i].dsv == dsv) {
            InterlockedIncrement(&g_hidePass[i].n[method]);
            ReleaseSRWLockShared(&g_hidePassLock);
            return;
        }
    ReleaseSRWLockShared(&g_hidePassLock);
    AcquireSRWLockExclusive(&g_hidePassLock);
    if (g_hidePassN < 32) {
        HidePass &p = g_hidePass[g_hidePassN];
        p = HidePass{};
        p.rtv0 = rtv0; p.dsv = dsv; p.rtCount = h->rtCount;
        describeView(reinterpret_cast<ID3D11View *>(rtv0 ? rtv0 : dsv), &p.w, &p.h, &p.fmt);
        p.n[method] = 1;
        ++g_hidePassN;
    }
    ReleaseSRWLockExclusive(&g_hidePassLock);
}

// Draw key of the current input-assembler state: index and vertex buffers,
// their offsets, and the draw's range.  The buffers are returned (not
// referenced) so the caller can compare them.
static unsigned long long playerDrawKey(void *self, UINT indexCount, UINT startIndex, INT baseVertex, void **ibOut,
                                        void **vbOut, unsigned long long *ikeyOut = nullptr) {
    auto *dc = reinterpret_cast<ID3D11DeviceContext *>(self);
    ID3D11Buffer *ib = nullptr, *vb = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0, stride = 0, vbOff = 0;
    dc->IAGetIndexBuffer(&ib, &fmt, &ibOff);
    dc->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
    unsigned long long key = 1469598103934665603ull;
    const unsigned long long parts[] = {reinterpret_cast<unsigned long long>(ib), reinterpret_cast<unsigned long long>(vb),
                                        ibOff, vbOff, startIndex, static_cast<unsigned long long>(baseVertex), indexCount};
    for (unsigned long long v : parts) { key ^= v; key *= 1099511628211ull; }
    if (!key) key = 1;
    if (ikeyOut) {
        unsigned long long k = 14695981039346656037ull;
        const unsigned long long ip[] = {reinterpret_cast<unsigned long long>(ib), ibOff, startIndex, indexCount};
        for (unsigned long long v : ip) { k ^= v; k *= 1099511628211ull; }
        *ikeyOut = k ? k : 1;
    }
    if (ibOut) *ibOut = ib;
    if (vbOut) *vbOut = vb;
    if (ib) ib->Release();
    if (vb) vb->Release();
    return key;
}

// Hooked DrawIndexed.  After a bone upload near the player, the draws of that
// piece (all sections sharing the first draw's buffers) are recorded for
// classification.  With hide=on, any draw whose key has been learned as the
// player's (3 or more votes) is not forwarded, in every pass: the player's
// pieces are drawn again in later passes without a new bone upload, so
// checking only the draws right after an upload hid the face alone (live
// 2026-10-20, v67/v68).  The bone uploads still happen (Mario's anchor); no
// game memory is written.
// v170g: the knight's Estus flask exists only while he drinks (spawned at his hand's dummy
// point, its liquid in the transparent pass), so it is the draw that is new then.  Every
// main-pass draw's index count is remembered with when it was last drawn; while he drinks, a
// draw whose count was not drawn in the 4 s before is hidden.  (By index count only, cheap
// enough for every draw; a full table never hides.)
static volatile LONG g_seenCount[8192];
static volatile LONG g_seenTick[8192];
static bool heldNewDraw(UINT indexCount) {
    if (indexCount < 30) return false;
    const DWORD now = GetTickCount();
    int slot = static_cast<int>((indexCount * 2654435761u) >> 19) & 8191, found = -1, empty = -1;
    for (int q = 0; q < 32; ++q, slot = (slot + 1) & 8191) {
        const LONG c = g_seenCount[slot];
        if (c == static_cast<LONG>(indexCount)) { found = slot; break; }
        if (c == 0) { empty = slot; break; }
    }
    if (!g_hideHeldItem || !g_heldHideOn) {
        if (found >= 0) g_seenTick[found] = static_cast<LONG>(now);
        else if (empty >= 0) {
            g_seenTick[empty] = static_cast<LONG>(now);
            InterlockedExchange(&g_seenCount[empty], static_cast<LONG>(indexCount));
        }
        return false;
    }
    const bool fresh = found < 0 ? empty >= 0 : now - static_cast<DWORD>(g_seenTick[found]) > 4000;
    if (!fresh) return false;
    static volatile LONG logged[32];
    static volatile LONG loggedN = 0;
    bool known = false;
    for (LONG q = 0; q < loggedN && q < 32; ++q) known = known || logged[q] == static_cast<LONG>(indexCount);
    if (!known && loggedN < 32) {
        logged[loggedN] = static_cast<LONG>(indexCount);
        InterlockedIncrement(&loggedN);
        logWrite("held: drink hides a new draw of %u indices", indexCount);
    }
    return true;
}

void STDMETHODCALLTYPE camHookedDrawIndexed(void *self, UINT indexCount, UINT startIndex, INT baseVertex) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->drawIndexed) return;
    auto fn = reinterpret_cast<void(STDMETHODCALLTYPE *)(void *, UINT, UINT, INT)>(h->drawIndexed);
    if (fxprobe::g_on) fxprobe::noteDraw(self, 1, indexCount, startIndex, baseVertex);
    // Tiny draws (particles and decals, 4 indices) are never considered.
    if (indexCount < 30) { fn(self, indexCount, startIndex, baseVertex); return; }
    if (!h->shadowPass && heldNewDraw(indexCount)) return;  // v170g: the knight's flask
    if (g_hideHeldItem && g_heldHideOn && !h->shadowPass) {
        // While the knight drinks (v170b): every main-pass draw after an upload at the knight
        // (his body is hidden anyway; Mario is not drawn through here), which takes his flask.
        const bool held = h->lastUpHeld && h->lastUpHeldDraws < 4;
        if (held) {  // (v170e: not every draw after a knight upload - that hid the whole level)
            if (held) ++h->lastUpHeldDraws;
            static UINT seen[24] = {};
            static int seenN = 0;
            bool known = false;
            for (int q = 0; q < seenN; ++q) known = known || seen[q] == indexCount;
            if (!known && seenN < 24) {
                seen[seenN++] = indexCount;
                logWrite("held: drink hides a draw of %u indices (%s)", indexCount, held ? "small piece" : "after a knight upload");
            }
            return;
        }
    }
    if (g_diag && (indexCount == 2903 || indexCount == 1280 || indexCount == 308)) {
        InterlockedIncrement(&g_faceDiag[0]);
        if (h->lastUpNear) InterlockedIncrement(&g_faceDiag[1]);
        if (h->lastUpNear && h->lastUpDraws < 8) InterlockedIncrement(&g_faceDiag[2]);
    }
    const bool afterNear = h->lastUpNear && h->lastUpDraws < 8;
    if (!afterNear && !h->shadowPass && !(g_hideTrusted > 0 && g_hidePlayer)) { fn(self, indexCount, startIndex, baseVertex); return; }
    void *ib = nullptr, *vb = nullptr;
    unsigned long long ikey = 0;
    const unsigned long long key = playerDrawKey(self, indexCount, startIndex, baseVertex, &ib, &vb, &ikey);
    bool record = false;
    if (afterNear) {
        if (h->lastUpDraws == 0) { h->lastUpIb = ib; h->lastUpVb = vb; record = true; }
        else record = ib == h->lastUpIb && vb == h->lastUpVb;  // another section of the same piece
        if (record) {
            ++h->lastUpDraws;
            InterlockedIncrement(&g_hideDiag[0]);
            if (h->passDrawN < 128) {
                CamCtxHooks::PassDraw &d = h->passDraws[h->passDrawN++];
                d.key = key;
                d.hip0 = h->lastUpHip0;
                d.rigidMask = h->lastUpRigid;
                d.leftover = h->lastUpLeftover;
                d.copied = h->lastUpCopied;
                {
                    ID3D11VertexShader *vs = nullptr;
                    reinterpret_cast<ID3D11DeviceContext *>(self)->VSGetShader(&vs, nullptr, nullptr);
                    d.vs = vs;
                    if (vs) vs->Release();
                }
                d.ikey = ikey;
                d.indexCount = indexCount;
                memcpy(d.hash, h->lastUpHash, sizeof(d.hash));
                InterlockedIncrement(&g_hideDiag[1]);
            }
        }
    }
    const bool trusted = g_hideTrusted > 0 && hideplayer::votes(key) >= 3;
    if (trusted && !h->shadowPass && h->fpPending && g_shadowFpOn) {
        // A learned knight piece drawn with this palette: it is his (v168).
        h->fpPending = false;
        AcquireSRWLockExclusive(&g_palFpLock);
        PaletteFp &fp = g_palFp[g_palFpNext];
        g_palFpNext = (g_palFpNext + 1) % 128;
        fp.tick = GetTickCount();
        fp.used = h->fpUsed;
        fp.ref = h->fpRef;
        memcpy(fp.rot, h->fpRot, sizeof(fp.rot));
        ReleaseSRWLockExclusive(&g_palFpLock);
    }
    if (trusted) {
        // Does the game skin the player into a stream-output buffer (drawn
        // later from that buffer), or use a geometry shader?
        auto *dc = reinterpret_cast<ID3D11DeviceContext *>(self);
        ID3D11Buffer *so[4] = {};
        dc->SOGetTargets(4, so);
        bool anySo = false;
        for (ID3D11Buffer *b : so) {
            if (!b) continue;
            anySo = true;
            if (!hideplayer::soBufPlayer(b) && g_hideSoN < 64) {
                g_hideSoBufs[g_hideSoN] = b;
                MemoryBarrier();
                InterlockedIncrement(&g_hideSoN);
            }
            b->Release();
        }
        if (anySo) InterlockedIncrement(&g_hideDiag3[1]);
        ID3D11GeometryShader *gs = nullptr;
        dc->GSGetShader(&gs, nullptr, nullptr);
        if (gs) { InterlockedIncrement(&g_hideDiag3[2]); gs->Release(); }
    }
    if (trusted) {
        const LONG nn = g_hideNotN;
        for (LONG q = 0; q < nn && q < 16; ++q)
            if (static_cast<UINT>(g_hideNot[q]) == indexCount) { fn(self, indexCount, startIndex, baseVertex); return; }
    }
    if (h->shadowPass && h->shadowUpValid && h->shadowUpDraws < 8) {
        ++h->shadowUpDraws;
        AcquireSRWLockExclusive(&g_shadowPendLock);
        if (g_shadowPendN < 2048) {
            ShadowPending &sp = g_shadowPend[g_shadowPendN++];
            sp.key = key;
            sp.ikey = ikey;
            sp.indexCount = indexCount;
            memcpy(sp.rot, h->shadowUpRot, sizeof(sp.rot));
            InterlockedIncrement(&g_shadowLearnDiag[0]);
        }
        ReleaseSRWLockExclusive(&g_shadowPendLock);
    }
    // Every shadow-pass draw until the next bone upload (v162): one upload serves the piece
    // in each shadow cascade; skipping only the first few draws left the knight in the other
    // cascades, and the shadow flickered as the camera moved between them.
    if (h->shadowPass) InterlockedIncrement(&g_sdDraws);
    if (h->shadowPass && h->shadowNear && g_hidePlayer && (g_shadowHideOn || g_shadowFpOn)) {
        ++h->shadowNearDraws;
        InterlockedIncrement(&g_shadowNearSkips);
        return;
    }
    if (h->shadowPass) {
        InterlockedIncrement(&g_shadowDiag[0]);
        if (trusted) InterlockedIncrement(&g_shadowDiag[1]);
        else if (g_diag) {
            AcquireSRWLockExclusive(&g_shadowMissLock);
            LONG q = 0;
            for (; q < g_shadowMissN; ++q) if (g_shadowMiss[q].indexCount == indexCount) break;
            if (q == g_shadowMissN && q < 48) { g_shadowMiss[q].indexCount = indexCount; g_shadowMiss[q].n = 0; ++g_shadowMissN; }
            if (q < 48) ++g_shadowMiss[q].n;
            ReleaseSRWLockExclusive(&g_shadowMissLock);
        }
        if (trusted && g_hidePlayer && (g_hideMask & 0x10) && g_shadowKeysOn) {
            InterlockedIncrement(&g_shadowDiag[2]);
            return;
        }
    }
    if (g_hidePlayer && g_hideTrusted > 0) {
        const LONG mask = g_hideMask;
        if (trusted && record && g_mainDsvW == 0 && h->depthView && h->dsvW) {
            g_mainDsvW = static_cast<LONG>(h->dsvW);
            g_mainDsvH = static_cast<LONG>(h->dsvH);
        }
        // The shadow map is a smaller-depth pass too: leave it alone unless shadowkeys=on
        // (v163: the knight's shadow flickered through this rule with shadow skipping off).
        if (h->shadowPass && g_shadowKeysOn != 1) { fn(self, indexCount, startIndex, baseVertex); return; }
        if (trusted && !record && h->offscreenPass && (mask & 0x20)) {
            InterlockedIncrement(&g_offscreenSkips);
            notePassSkip(h, 1);
            return;
        }
        if (trusted && (mask & (record ? 1 : 2))) {
            InterlockedIncrement(&g_hideDiag[3]);
            InterlockedIncrement(&g_hideDiag2[record ? 0 : 1]);
            notePassSkip(h, record ? 0 : 1);
            return;
        }
        if (!trusted && (mask & 4) && hideplayer::ikeyTrusted(ikey)) {
            InterlockedIncrement(&g_hideDiag3[0]);
            notePassSkip(h, 2);
            return;
        }
        if ((mask & 8) && hideplayer::soBufPlayer(vb)) {
            InterlockedIncrement(&g_hideDiag3[5]);
            notePassSkip(h, 3);
            return;
        }
    }
    fn(self, indexCount, startIndex, baseVertex);
}

// First vertex buffer bound on a context (not referenced).
static void *boundVb0(void *self) {
    ID3D11Buffer *vb = nullptr;
    UINT stride = 0, off = 0;
    reinterpret_cast<ID3D11DeviceContext *>(self)->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
    if (vb) vb->Release();
    return vb;
}

// Hooked Draw and DrawAuto: counted, and skipped (hide=on) when they draw from
// a stream-output buffer the player's meshes were skinned into.
void STDMETHODCALLTYPE camHookedDraw(void *self, UINT vertexCount, UINT startVertex) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->draw) return;
    InterlockedIncrement(&g_hideDiag3[3]);
    if (fxprobe::g_on) fxprobe::noteDraw(self, 0, vertexCount, startVertex, 0);
    if (g_hidePlayer && (g_hideMask & 8) && g_hideSoN > 0 && !(h->shadowPass && g_shadowKeysOn != 1) &&
        hideplayer::soBufPlayer(boundVb0(self))) {
        InterlockedIncrement(&g_hideDiag3[5]);
        notePassSkip(h, 3);
        return;
    }
    reinterpret_cast<void(STDMETHODCALLTYPE *)(void *, UINT, UINT)>(h->draw)(self, vertexCount, startVertex);
}

void STDMETHODCALLTYPE camHookedDrawAuto(void *self) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->drawAuto) return;
    InterlockedIncrement(&g_hideDiag3[4]);
    if (g_hidePlayer && (g_hideMask & 8) && g_hideSoN > 0 && !(h->shadowPass && g_shadowKeysOn != 1) &&
        hideplayer::soBufPlayer(boundVb0(self))) {
        InterlockedIncrement(&g_hideDiag3[5]);
        notePassSkip(h, 3);
        return;
    }
    reinterpret_cast<void(STDMETHODCALLTYPE *)(void *)>(h->drawAuto)(self);
}

// Hooked DrawIndexedInstanced: the same learned keys (per-instance range) are
// skipped, in case a pass draws the player's pieces instanced.
void STDMETHODCALLTYPE camHookedDrawIndexedInstanced(void *self, UINT indexCountPerInstance, UINT instanceCount,
                                                     UINT startIndex, INT baseVertex, UINT startInstance) {
    CamCtxHooks *h = findCamCtx(self);
    if (!h || !h->drawIndexedInstanced) return;
    auto fn = reinterpret_cast<void(STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, INT, UINT)>(h->drawIndexedInstanced);
    if (fxprobe::g_on) fxprobe::noteDraw(self, 2, indexCountPerInstance * 1000 + (instanceCount > 999 ? 999 : instanceCount), startIndex, baseVertex);
    if (!h->shadowPass && heldNewDraw(indexCountPerInstance)) return;  // v170g
    if (g_hidePlayer && (g_hideMask & 6) && g_hideTrusted > 0 && indexCountPerInstance >= 30 &&
        !(h->shadowPass && g_shadowKeysOn != 1)) {
        InterlockedIncrement(&g_hideDiag2[3]);
        unsigned long long ikey = 0;
        if (hideplayer::votes(playerDrawKey(self, indexCountPerInstance, startIndex, baseVertex, nullptr, nullptr, &ikey)) >= 3 ||
            hideplayer::ikeyTrusted(ikey)) {
            InterlockedIncrement(&g_hideDiag2[2]);
            notePassSkip(h, 2);
            return;
        }
    }
    fn(self, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

} // namespace remastered