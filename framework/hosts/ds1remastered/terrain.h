#pragma once

// Static map collision for the SM64 guest (Phase 3), the Remastered
// counterpart of hosts/ds1ptde/terrain.h.
//
// Reads %LOCALAPPDATA%\MarioSoulsProbe\collision\*.bin written by
// prepare_collision.py (b"MSC2", uint32 count, count x {uint32 hit filter,
// float32[3][3] triangle} in world metres).  Script-switchable colliders are
// not in the cache (they stay native).  Triangles are indexed in an 8 m grid;
// the pose thread asks for those near Mario and hands them to SM64 as static
// surfaces.  Read-only; no game memory involved.

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "logging.h"
#include "install.h"

namespace remastered {
namespace terrain {

struct Triangle { float v[3][3]; uint32_t filter; };

inline std::vector<Triangle> g_tris;
inline std::vector<Triangle> g_hazards;  // 14/15 lethal surfaces (v170l, PTDE hazards.h)
inline uint32_t g_floorFilter = 0;  // collision filter of the floor floorBelowN last returned
inline std::unordered_map<int64_t, std::vector<uint32_t>> g_grid;
inline volatile LONG g_loaded = 0;  // 1 loaded, 0 loading, -1 none/failed
inline int g_mapKey = -2;          // world*100+area of the loaded file (-1 unknown map)
inline DWORD g_mapChangedTick = 0;  // GetTickCount() when the map last changed
// True for a few seconds after the game switches maps: Mario's position can
// still belong to the old area while the new area's collision is in place.
inline bool recentMapChange() { return GetTickCount() - g_mapChangedTick < 3000; }
inline size_t g_skipped = 0;        // trigger/death-plane/NPC-barrier triangles left out
constexpr float kCell = 8.0f;

inline int64_t cellKey(int cx, int cz) { return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz); }

// Loads one map's collision cache file (PTDE loadMap): m<world>_<area>_00_00.bin
// from %LOCALAPPDATA%\MarioSoulsProbe\collision.  Only the current map is kept, so
// geometry from other areas can never stand in for this one.  Worker thread only.
inline void ensureMap(int key) {
    if (key == g_mapKey) return;
    g_mapKey = key;
    g_mapChangedTick = GetTickCount();
    g_tris.clear();
    g_hazards.clear();
    g_grid.clear();
    g_skipped = 0;
    g_loaded = 0;
    if (key < 0) { g_loaded = -1; return; }
    {
        // v171: built from this install's own game files when missing (first launch).
        char id[16];
        snprintf(id, sizeof(id), "m%02d_%02d_00_00", key / 100, key % 100);
        install::ensureMapCache(id);
    }
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) { g_loaded = -1; return; }
    wchar_t file[MAX_PATH];
    swprintf_s(file, MAX_PATH, L"\\MarioSoulsProbe\\collision\\m%02d_%02d_00_00.bin", key / 100, key % 100);
    wcscat_s(path, MAX_PATH, file);
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        logWrite("terrain: no collision file for map %d (flat floor fallback)", key);
        g_loaded = -1;
        return;
    }
    char magic[4] = {};
    uint32_t count = 0;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "MSC2", 4) == 0 && fread(&count, 4, 1, f) == 1 &&
        count < 5000000) {
        struct Record { uint32_t filter; float t[3][3]; };
        std::vector<Record> records(count);
        if (fread(records.data(), sizeof(Record), count, f) == count) {
            for (const Record &r : records) {
                // Not floors (PTDE hazards.h guestTerrainFilter/hazardLethal):
                // 11/19 NPC-only barriers, 17 map-group switch, 13 trigger,
                // 14/15 death planes.  Mario stood on a death plane off the
                // map (v112); the knight falls to it natively instead.
                if (r.filter == 11 || r.filter == 13 || r.filter == 14 || r.filter == 15 || r.filter == 17 ||
                    r.filter == 19) {
                    if (r.filter == 14 || r.filter == 15) {
                        Triangle h;
                        memcpy(h.v, r.t, sizeof(h.v));
                        h.filter = r.filter;
                        g_hazards.push_back(h);
                    }
                    ++g_skipped;
                    continue;
                }
                Triangle t;
                memcpy(t.v, r.t, sizeof(t.v));
                t.filter = r.filter;
                bool ok = true;
                for (auto &p : t.v) for (float c : p) ok = ok && std::isfinite(c);
                if (ok) g_tris.push_back(t);
            }
        }
    }
    fclose(f);
    // Grid: each triangle in every cell its x/z bounds touch.
    for (uint32_t i = 0; i < g_tris.size(); ++i) {
        const Triangle &t = g_tris[i];
        float x0 = t.v[0][0], x1 = x0, z0 = t.v[0][2], z1 = z0;
        for (int k = 1; k < 3; ++k) {
            x0 = std::fmin(x0, t.v[k][0]); x1 = std::fmax(x1, t.v[k][0]);
            z0 = std::fmin(z0, t.v[k][2]); z1 = std::fmax(z1, t.v[k][2]);
        }
        for (int cx = static_cast<int>(std::floor(x0 / kCell)); cx <= static_cast<int>(std::floor(x1 / kCell)); ++cx)
            for (int cz = static_cast<int>(std::floor(z0 / kCell)); cz <= static_cast<int>(std::floor(z1 / kCell)); ++cz)
                g_grid[cellKey(cx, cz)].push_back(i);
    }
    logWrite("terrain: map %d: %zu triangles (%zu trigger/death/NPC-only left out, %zu lethal kept for contact), %zu grid cells", key,
             g_tris.size(), g_skipped, g_hazards.size(), g_grid.size());
    g_loaded = g_tris.empty() ? -1 : 1;
}

// Lethal surfaces (PTDE hazards.h / core/hazard.h touches): the segment from a to b (feet,
// world metres) crosses or lands on a 14 (lethal fall) or 15 (kill plane) triangle.  Elevator
// shafts and pits end in these; Mario's terrain leaves them out, so this is how he dies there.
inline bool hazardTouch(const float a[3], const float b[3]) {
    if (g_loaded != 1) return false;
    for (const Triangle &t : g_hazards) {
        const float *p0 = t.v[0], *p1 = t.v[1], *p2 = t.v[2];
        float lo = std::fmin(p0[1], std::fmin(p1[1], p2[1])), hi = std::fmax(p0[1], std::fmax(p1[1], p2[1]));
        if (std::fmax(a[1], b[1]) < lo - 0.05f || std::fmin(a[1], b[1]) > hi + 0.05f) continue;
        float x0 = std::fmin(p0[0], std::fmin(p1[0], p2[0])), x1 = std::fmax(p0[0], std::fmax(p1[0], p2[0]));
        float z0 = std::fmin(p0[2], std::fmin(p1[2], p2[2])), z1 = std::fmax(p0[2], std::fmax(p1[2], p2[2]));
        if (std::fmax(a[0], b[0]) < x0 || std::fmin(a[0], b[0]) > x1 || std::fmax(a[2], b[2]) < z0 || std::fmin(a[2], b[2]) > z1) continue;
        const float u[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, v[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (nn < 1e-8f) continue;
        auto dotn = [&](const float *q) { return (q[0] - p0[0]) * n[0] + (q[1] - p0[1]) * n[1] + (q[2] - p0[2]) * n[2]; };
        const float da = dotn(a), db = dotn(b), eps = 0.02f * std::sqrt(nn);
        if ((da > eps && db > eps) || (da < -eps && db < -eps)) continue;
        const float delta = da - db;
        const float s = std::fabs(delta) > 1e-7f ? std::fmax(0.f, std::fmin(1.f, da / delta)) : 1.f;
        float h[3] = {a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s, a[2] + (b[2] - a[2]) * s};
        const float k = dotn(h) / nn;
        for (int c = 0; c < 3; ++c) h[c] -= n[c] * k;
        const float w[3] = {h[0] - p0[0], h[1] - p0[1], h[2] - p0[2]};
        const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], uv = u[0] * v[0] + u[1] * v[1] + u[2] * v[2],
                    vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2], wu = w[0] * u[0] + w[1] * u[1] + w[2] * u[2],
                    wv = w[0] * v[0] + w[1] * v[1] + w[2] * v[2], den = uu * vv - uv * uv;
        if (den <= 1e-8f) continue;
        const float ss = (vv * wu - uv * wv) / den, rr = (uu * wv - uv * wu) / den;
        if (ss >= -1e-5f && rr >= -1e-5f && ss + rr <= 1.00001f) return true;
    }
    return false;
}

// Triangles within `radius` (x/z) and `height` (y) of a world point.
inline void query(const float center[3], float radius, float height, std::vector<uint32_t> &out) {
    out.clear();
    if (g_loaded != 1) return;
    static std::vector<uint8_t> seen;
    if (seen.size() != g_tris.size()) seen.assign(g_tris.size(), 0);
    const int cx0 = static_cast<int>(std::floor((center[0] - radius) / kCell)),
              cx1 = static_cast<int>(std::floor((center[0] + radius) / kCell));
    const int cz0 = static_cast<int>(std::floor((center[2] - radius) / kCell)),
              cz1 = static_cast<int>(std::floor((center[2] + radius) / kCell));
    for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz) {
            auto it = g_grid.find(cellKey(cx, cz));
            if (it == g_grid.end()) continue;
            for (uint32_t i : it->second) {
                if (seen[i]) continue;
                const Triangle &t = g_tris[i];
                float y0 = t.v[0][1], y1 = y0;
                for (int k = 1; k < 3; ++k) { y0 = std::fmin(y0, t.v[k][1]); y1 = std::fmax(y1, t.v[k][1]); }
                if (y1 < center[1] - height || y0 > center[1] + height) continue;
                seen[i] = 1;
                out.push_back(i);
            }
        }
    for (uint32_t i : out) seen[i] = 0;
}

// A steep static surface (|normal y| < 0.35: a wall) within `radius` metres of p horizontally,
// spanning p's height band (v170n: in the air the knight's bigger capsule stops at walls Mario
// is still wall-kicking along; that shortfall is not an enemy in the way).
inline bool wallNear(const float p[3], float radius) {
    if (g_loaded != 1) return false;
    static std::vector<uint32_t> idx;
    query(p, radius + 0.5f, 2.0f, idx);
    for (uint32_t i : idx) {
        const Triangle &t = g_tris[i];
        const float u[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
        const float v[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
        float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-6f) continue;
        for (float &q : n) q /= nl;
        if (std::fabs(n[1]) >= 0.35f) continue;
        float y0 = t.v[0][1], y1 = y0;
        for (int k = 1; k < 3; ++k) { y0 = std::fmin(y0, t.v[k][1]); y1 = std::fmax(y1, t.v[k][1]); }
        if (y1 < p[1] - 0.2f || y0 > p[1] + 1.8f) continue;
        const float d = (p[0] - t.v[0][0]) * n[0] + (p[1] - t.v[0][1]) * n[1] + (p[2] - t.v[0][2]) * n[2];
        if (std::fabs(d) > radius) continue;
        // The foot of the perpendicular inside the triangle's x/z bounds (loosely).
        const float fx = p[0] - n[0] * d, fz = p[2] - n[2] * d;
        float x0 = t.v[0][0], x1 = x0, z0 = t.v[0][2], z1 = z0;
        for (int k = 1; k < 3; ++k) { x0 = std::fmin(x0, t.v[k][0]); x1 = std::fmax(x1, t.v[k][0]); z0 = std::fmin(z0, t.v[k][2]); z1 = std::fmax(z1, t.v[k][2]); }
        if (fx < x0 - 0.1f || fx > x1 + 0.1f || fz < z0 - 0.1f || fz > z1 + 0.1f) continue;
        return true;
    }
    return false;
}

// As floorBelow, also giving the floor's normal y (1 = flat).
inline float floorBelowN(const float p[3], float up, float &normalY);

// Highest static floor (upward-facing) under the x/z of p, no higher than
// p.y + up (metres); -INFINITY when the cache has none there.
inline float floorBelow(const float p[3], float up) {
    float ny;
    return floorBelowN(p, up, ny);
}
inline float floorBelowN(const float p[3], float up, float &normalY) {
    normalY = 0;
    if (g_loaded != 1) return -INFINITY;
    auto it = g_grid.find(cellKey(static_cast<int>(std::floor(p[0] / kCell)), static_cast<int>(std::floor(p[2] / kCell))));
    if (it == g_grid.end()) return -INFINITY;
    float best = -INFINITY;
    for (uint32_t i : it->second) {
        const Triangle &t = g_tris[i];
        const float ax = t.v[0][0], az = t.v[0][2], bx = t.v[1][0], bz = t.v[1][2], cx = t.v[2][0], cz = t.v[2][2];
        const float d1 = (bx - ax) * (p[2] - az) - (bz - az) * (p[0] - ax);
        const float d2 = (cx - bx) * (p[2] - bz) - (cz - bz) * (p[0] - bx);
        const float d3 = (ax - cx) * (p[2] - cz) - (az - cz) * (p[0] - cx);
        if (!((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0))) continue;
        const float ux = bx - ax, uy = t.v[1][1] - t.v[0][1], uz = bz - az;
        const float wx = cx - ax, wy = t.v[2][1] - t.v[0][1], wz = cz - az;
        const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9f || std::fabs(ny) / len < 0.3f) continue;  // walls and steep faces are not floors
        const float y = t.v[0][1] - (nx * (p[0] - ax) + nz * (p[2] - az)) / ny;
        if (y <= p[1] + up && y > best) { best = y; normalY = std::fabs(ny) / len; g_floorFilter = t.filter; }
    }
    return best;
}

// Any triangle crossed by a-b with |normal.y| in [minNy, maxNy].
inline bool segmentHits(const float a[3], const float b[3], float minNy, float maxNy);
// A wall-like triangle (|normal.y| < 0.5) crossed by the segment a-b.
inline bool wallBetween(const float a[3], const float b[3]) { return segmentHits(a, b, 0.f, 0.5f); }
inline bool segmentHits(const float a[3], const float b[3], float minNy, float maxNy) {
    if (g_loaded != 1) return false;
    const int cx0 = static_cast<int>(std::floor(std::fmin(a[0], b[0]) / kCell)),
              cx1 = static_cast<int>(std::floor(std::fmax(a[0], b[0]) / kCell));
    const int cz0 = static_cast<int>(std::floor(std::fmin(a[2], b[2]) / kCell)),
              cz1 = static_cast<int>(std::floor(std::fmax(a[2], b[2]) / kCell));
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz) {
            auto it = g_grid.find(cellKey(cx, cz));
            if (it == g_grid.end()) continue;
            for (uint32_t i : it->second) {
                const Triangle &t = g_tris[i];
                const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
                const float e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl < 1e-9f || std::fabs(n[1]) / nl > maxNy || std::fabs(n[1]) / nl < minNy) continue;
                const float h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
                if (std::fabs(det) < 1e-9f) continue;
                const float inv = 1.f / det, s[3] = {a[0] - t.v[0][0], a[1] - t.v[0][1], a[2] - t.v[0][2]};
                const float u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
                if (u < 0 || u > 1) continue;
                const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                const float v = inv * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);
                if (v < 0 || u + v > 1) continue;
                const float tt = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
                if (tt >= 0 && tt <= 1) return true;
            }
        }
    return false;
}

} // namespace terrain
} // namespace remastered
