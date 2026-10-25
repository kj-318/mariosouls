// fx_probe.h -- diagnostic (fxprobe=on, live; default off).
//
// Finds where the game's particle effects (the Pyromancy Flame's hand fire) put their
// vertex data: every Map/Unmap of a dynamic vertex or shader-resource buffer is scanned
// for float triples near the knight's chest (world, and camera-relative), and draws that
// read a buffer with hits are tallied by shader pair and size.  Reads write-combined
// memory, so it costs frame time: on only for a short test.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstring>
#include "logging.h"

namespace remastered {
namespace fxprobe {

inline volatile LONG g_on = 0;

struct BufStat {
    void *res;
    UINT bytes, bind;
    LONG maps, mapsHit, hitsW, hitsR;
    LONG strideVotes[33];  // stride/4 between consecutive hits, up to 128 bytes
    float sample[4][3];
    int samples;
    UINT minOff, maxOff;
    DWORD lastHitTick;
};
inline BufStat g_bufs[24] = {};
inline LONG g_bufN = 0;
inline SRWLOCK g_lock = SRWLOCK_INIT;

struct DrawStat {
    void *vs, *ps, *res;
    UINT count, kind, stride, topo;
    LONG n;
    INT base;
    UINT start;
};
inline DrawStat g_draws[48] = {};
inline LONG g_drawN = 0;

inline BufStat *findBuf(void *res, bool add, UINT bytes, UINT bind) {
    for (LONG i = 0; i < g_bufN; ++i)
        if (g_bufs[i].res == res) return &g_bufs[i];
    if (!add || g_bufN >= 24) return nullptr;
    BufStat &b = g_bufs[g_bufN++];
    memset(&b, 0, sizeof(b));
    b.res = res; b.bytes = bytes; b.bind = bind; b.minOff = ~0u;
    return &b;
}

// Called at Unmap with the mapped bytes (still mapped).
inline void scan(void *res, const void *data, UINT bytes, UINT bind, const float player[3], const float camera[3]) {
    if (!data || bytes < 12) return;
    if (bytes > (1u << 20)) bytes = 1u << 20;
    const float cw[3] = {player[0], player[1] + 1.0f, player[2]};
    const float cr[3] = {cw[0] - camera[0], cw[1] - camera[1], cw[2] - camera[2]};
    static thread_local float local[16384];
    LONG hitsW = 0, hitsR = 0;
    long lastHit = -1;
    LONG votes[33] = {};
    float smp[4][3]; int ns = 0;
    UINT mn = ~0u, mx = 0;
    const UINT total = bytes / 4;
    for (UINT base = 0; base < total; base += 16384 - 2) {
        UINT n = total - base; if (n > 16384) n = 16384;
        memcpy(local, static_cast<const float *>(data) + base, n * 4);
        for (UINT i = 0; i + 2 < n; ++i) {
            const float x = local[i], y = local[i + 1], z = local[i + 2];
            bool w = std::fabs(x - cw[0]) < 1.5f && std::fabs(y - cw[1]) < 1.5f && std::fabs(z - cw[2]) < 1.5f;
            bool r = !w && std::fabs(x - cr[0]) < 1.5f && std::fabs(y - cr[1]) < 1.5f && std::fabs(z - cr[2]) < 1.5f;
            if (!w && !r) continue;
            if (w) ++hitsW; else ++hitsR;
            const long off = static_cast<long>((base + i) * 4);
            if (lastHit >= 0) { const long d = (off - lastHit) / 4; if (d > 0 && d <= 32) ++votes[d]; }
            lastHit = off;
            if (static_cast<UINT>(off) < mn) mn = off;
            if (static_cast<UINT>(off) > mx) mx = off;
            if (ns < 4) { smp[ns][0] = x - (w ? cw[0] : cr[0]); smp[ns][1] = y - (w ? cw[1] : cr[1]); smp[ns][2] = z - (w ? cw[2] : cr[2]); ++ns; }
            i += 2;
        }
        if (n < 16384) break;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (BufStat *b = findBuf(res, hitsW + hitsR > 0, bytes, bind)) {
        ++b->maps;
        if (hitsW + hitsR) {
            ++b->mapsHit; b->hitsW += hitsW; b->hitsR += hitsR;
            for (int k = 0; k < 33; ++k) b->strideVotes[k] += votes[k];
            for (int k = 0; k < ns; ++k) memcpy(b->sample[k], smp[k], 12);
            b->samples = ns;
            if (mn < b->minOff) b->minOff = mn;
            if (mx > b->maxOff) b->maxOff = mx;
            b->lastHitTick = GetTickCount();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Called before a draw.  kind: 0 Draw, 1 DrawIndexed, 2 DrawIndexedInstanced.
inline void noteDraw(void *self, UINT kind, UINT count, UINT start, INT base) {
    auto *dc = reinterpret_cast<ID3D11DeviceContext *>(self);
    void *hitRes = nullptr;
    UINT stride = 0;
    {
        ID3D11Buffer *vb = nullptr; UINT off = 0;
        dc->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
        if (vb) {
            vb->Release();
            AcquireSRWLockShared(&g_lock);
            BufStat *b = findBuf(vb, false, 0, 0);
            if (b && GetTickCount() - b->lastHitTick < 500) hitRes = vb;
            ReleaseSRWLockShared(&g_lock);
        }
    }
    if (!hitRes) {
        ID3D11ShaderResourceView *srv[4] = {};
        dc->VSGetShaderResources(0, 4, srv);
        for (int s = 0; s < 4; ++s) {
            if (!srv[s]) continue;
            ID3D11Resource *r = nullptr;
            srv[s]->GetResource(&r);
            if (r) {
                AcquireSRWLockShared(&g_lock);
                BufStat *b = findBuf(r, false, 0, 0);
                if (b && GetTickCount() - b->lastHitTick < 500) hitRes = r;
                ReleaseSRWLockShared(&g_lock);
                r->Release();
            }
            srv[s]->Release();
        }
    }
    if (!hitRes) return;
    ID3D11VertexShader *vs = nullptr; ID3D11PixelShader *ps = nullptr;
    dc->VSGetShader(&vs, nullptr, nullptr);
    dc->PSGetShader(&ps, nullptr, nullptr);
    if (vs) vs->Release();
    if (ps) ps->Release();
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    dc->IAGetPrimitiveTopology(&topo);
    AcquireSRWLockExclusive(&g_lock);
    LONG i = 0;
    for (; i < g_drawN; ++i) {
        DrawStat &d = g_draws[i];
        if (d.vs == vs && d.ps == ps && d.res == hitRes && d.kind == kind && d.count == count) break;
    }
    if (i == g_drawN && g_drawN < 48) {
        DrawStat &d = g_draws[g_drawN++];
        d = {vs, ps, hitRes, count, kind, stride, static_cast<UINT>(topo), 0, base, start};
    }
    if (i < 48) ++g_draws[i].n;
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- Hand fire on Mario (fxmove, default on; fxmove=off disables, live) -------------
// The Pyromancy Flame's hand fire is drawn from a small dynamic structured buffer (32 KB,
// shader-resource only) holding its particles in world space (fxprobe, v155).  At Unmap,
// before the GPU sees it, particles in a box around the knight's arms are shifted so the
// fire sits in Mario's hand.  The fire's centre is tracked (smoothed, relative to the
// knight) so the knight's arm movement does not drag it around Mario.
inline volatile LONG g_moveOn = 0;  // off by default (v158): it only made the fire flicker
inline float g_marioRoot[3] = {};     // Mario's feet, world metres (presentation tick)
inline float g_marioFace = 0;         // SM64 faceAngle (radians): forward = (sin, 0, cos)
inline volatile LONG g_flameHeld = 0; // a Pyromancy Flame in either hand
inline volatile LONG g_marioValid = 0;
// Offset of the fire's centre from the drawn flower bloom, world metres (fxhand=x,y,z).
inline float g_hand[3] = {0.f, 0.06f, 0.f};
// The Fire Flower's bloom centre as drawn this frame (copyFrame), world metres.
inline float g_flowerW[3] = {};
inline volatile DWORD g_flowerTick = 0;
inline float g_anchorRel[3] = {0, 0, 0};  // fire centre relative to the knight's feet
inline bool g_anchorValid = false;
inline SRWLOCK g_moveLock = SRWLOCK_INIT;
inline LONG g_moveMaps = 0, g_movePts = 0;

inline void moveHandFire(void *data, UINT bytes, UINT bind, const float player[3]) {
    if (!g_moveOn || !g_flameHeld || !g_marioValid || !data) return;
    // g_moveOn 1: hide the hand fire (v157 default: the user wants no floating fire at all);
    // 2: move it onto the drawn flower bloom (fxmove=flower).
    if (g_moveOn == 2 && GetTickCount() - g_flowerTick > 500) {  // no flower drawn
        static DWORD lastNo = 0;
        if (GetTickCount() - lastNo > 5000) { lastNo = GetTickCount(); logWrite("fxmove: flame held but no flower bloom drawn; fire left on the knight"); }
        return;
    }
    if (bind != D3D11_BIND_SHADER_RESOURCE || bytes > 65536 || bytes < 12) return;
    float *f = static_cast<float *>(data);
    const UINT total = bytes / 4;
    static thread_local float local[16384];
    static thread_local UINT hits[1024];
    memcpy(local, f, total * 4);
    UINT n = 0;
    double c[3] = {0, 0, 0};
    for (UINT i = 0; i + 2 < total && n < 1024; ++i) {
        const float x = local[i], y = local[i + 1], z = local[i + 2];
        if (std::fabs(x - player[0]) < 0.9f && std::fabs(z - player[2]) < 0.9f && y > player[1] + 0.2f && y < player[1] + 2.0f) {
            hits[n++] = i;
            c[0] += x; c[1] += y; c[2] += z;
            i += 2;
        }
    }
    if (!n) return;
    float delta[3];
    {
        AcquireSRWLockExclusive(&g_moveLock);
        const float rel[3] = {static_cast<float>(c[0] / n) - player[0], static_cast<float>(c[1] / n) - player[1],
                              static_cast<float>(c[2] / n) - player[2]};
        if (!g_anchorValid) { memcpy(g_anchorRel, rel, 12); g_anchorValid = true; }
        for (int k = 0; k < 3; ++k) g_anchorRel[k] += 0.05f * (rel[k] - g_anchorRel[k]);
        // Target: the flower's bloom as drawn (render-time, interpolated with Mario's mesh;
        // the 30 Hz simulated root made the fire jump against the drawn Mario, v155).
        const float hand[3] = {g_flowerW[0] + g_hand[0], g_flowerW[1] + g_hand[1], g_flowerW[2] + g_hand[2]};
        for (int k = 0; k < 3; ++k) delta[k] = hand[k] - (player[k] + g_anchorRel[k]);
        ++g_moveMaps; g_movePts += n;
        ReleaseSRWLockExclusive(&g_moveLock);
    }
    if (g_moveOn == 1) {
        // Far below the world: the particle is never seen.
        for (UINT j = 0; j < n; ++j) {
            const UINT i = hits[j];
            f[i + 1] = local[i + 1] - 10000.f;
        }
        static DWORD lastHideLog = 0;
        if (GetTickCount() - lastHideLog > 10000) {
            lastHideLog = GetTickCount();
            logWrite("fxmove: hand fire hidden (%u particles this map)", n);
        }
        return;
    }
    for (UINT j = 0; j < n; ++j) {
        const UINT i = hits[j];
        f[i] = local[i] + delta[0];
        f[i + 1] = local[i + 1] + delta[1];
        f[i + 2] = local[i + 2] + delta[2];
    }
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 5000) {
        lastLog = GetTickCount();
        logWrite("fxmove: hand fire on Mario: %ld maps, %ld particles in 5 s; fire centre rel knight (%.2f %.2f %.2f), shift (%.2f %.2f %.2f)",
                 g_moveMaps, g_movePts, g_anchorRel[0], g_anchorRel[1], g_anchorRel[2], delta[0], delta[1], delta[2]);
        g_moveMaps = g_movePts = 0;
    }
}

// Once every 2 s from the Unmap path.
inline void report() {
    static DWORD last = 0;
    if (GetTickCount() - last < 2000) return;
    last = GetTickCount();
    AcquireSRWLockExclusive(&g_lock);
    for (LONG i = 0; i < g_bufN; ++i) {
        BufStat &b = g_bufs[i];
        if (!b.mapsHit) { b.maps = 0; continue; }
        int best = 0;
        for (int k = 1; k < 33; ++k) if (b.strideVotes[k] > b.strideVotes[best]) best = k;
        logWrite("fxprobe: buf %p %u bytes bind=0x%X maps %ld with hits %ld, hits world %ld camrel %ld, stride ~%d B, "
                 "offsets %u..%u, sample rel chest (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
                 b.res, b.bytes, b.bind, b.maps, b.mapsHit, b.hitsW, b.hitsR, best * 4, b.minOff, b.maxOff,
                 b.sample[0][0], b.sample[0][1], b.sample[0][2], b.sample[1][0], b.sample[1][1], b.sample[1][2]);
        b.maps = b.mapsHit = b.hitsW = b.hitsR = 0;
        memset(b.strideVotes, 0, sizeof(b.strideVotes));
        b.minOff = ~0u; b.maxOff = 0;
    }
    for (LONG i = 0; i < g_drawN; ++i) {
        const DrawStat &d = g_draws[i];
        logWrite("fxprobe: draw kind %u count %u x%ld vs %p ps %p buf %p stride %u topo %u start %u base %d",
                 d.kind, d.count, d.n, d.vs, d.ps, d.res, d.stride, d.topo, d.start, d.base);
    }
    g_drawN = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace fxprobe
} // namespace remastered
