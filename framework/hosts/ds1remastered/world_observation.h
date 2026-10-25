#pragma once

// Remastered world-position observation (read-only).
//
// The captured view-projection is camera-relative (section 22 of
// docs/REMASTERED_FINDINGS.md), so placing anything at a world point needs the
// world positions of the player and of the camera.  Neither is in the GPU
// buffers, so both are read from game memory here.  Nothing is written.
//
// Sources (third-party, see work/dsr/MODDING_RESOURCES.md; unverified until
// this probe reports):
//   WorldChrMan  AOB 48 8B 05 ?? ?? ?? ?? 48 8B 48 68 48 85 C9 0F 84 ?? ?? ?? ??
//                    48 39 5E 10 0F 84 ?? ?? ?? ?? 48       (DSR-Gadget)
//     PlayerIns   = [WorldChrMan + 0x68]
//     ChrMapData  = [PlayerIns + 0x68]       (1.03 layout)
//     ChrPosData  = [ChrMapData + 0x28]; angle +0x4, X/Y/Z +0x10/+0x14/+0x18
//   ChrFollowCam AOB 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4E 68 48 8B 05
//                    ?? ?? ?? ?? 48 89 48 60                (DSR-Gadget)
//     struct      = [[[static]] + 0x60] + 0x60  (DSR-Gadget saves 512 bytes here)
//
// Cross-check: our verified health base (HP at +0x3E8) is expected to be the
// same object as PlayerIns, whose HP DSR-Gadget also reads at +0x3E8.  The
// probe logs whether the two agree; agreement is the evidence for the chain.
//
// Live result (2026-10-19): the PlayerIns cross-check matched our health base
// (same object), so the position chain is confirmed.  The follow-camera chain
// above did not resolve, so the camera's world position is found by the search
// below instead (searchCamera).

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "logging.h"

namespace remastered {

struct WorldObservation {
    bool playerValid;
    float player[3];
    float playerAngle;
    bool followCamValid;
    float followCam[128];  // first 512 bytes of the follow-camera struct
    bool cameraValid;      // camera world position from the search below
    float camera[3];
};

// Written by the observation worker, read by render threads.  A torn read can
// only misplace one test frame.
inline WorldObservation g_world = {};

// Addresses resolved by the worker (every 250 ms) and read directly at draw
// time.  Reading the positions only in the worker left them up to 250 ms older
// than the per-frame view-projection, so anything anchored to the player drifted
// sideways while the camera turned and snapped back on the next refresh
// (observed live 2026-10-19).
inline volatile uintptr_t g_livePlayerPos = 0;   // ChrPosData (+0x10 = XYZ)
inline volatile uintptr_t g_liveCameraPos = 0;   // camera position float3
// Native knight hiding (v160, PTDE character_visibility.h): while active the knight's model
// is not submitted at all (body, held items, shadow), so Mario is placed from memory
// instead of from the knight's bone uploads.  g_marioDraws counts Mario submissions for
// the watchdog that turns it off if Mario stops being drawn.
inline volatile LONG g_nativeHideActive = 0;
inline volatile LONG g_marioDraws = 0;
// v170: set while Mario is posed drinking (the knight's estus animation); the camera hook
// hides small rigid pieces at the knight's hands then (the knight's flask).
inline volatile LONG g_hideHeldItem = 0;
inline volatile LONG g_heldHideOn = 0;  // heldhide=on enables (v170h)
inline volatile uintptr_t g_livePlayerIns = 0;   // PlayerIns (ChrFlags1 at +0x2A4, 1.03)
inline volatile uintptr_t g_liveMapData = 0;     // ChrMapData (warp request at +0x108)
// When the worker last validated each address (GetTickCount).  Render threads
// read a validated address directly instead of paying for VirtualQuery.
inline volatile DWORD g_livePlayerChecked = 0;
inline volatile DWORD g_liveCameraChecked = 0;

// Latest plausible main-scene view-projection, unfiltered (camera_capture.cpp),
// used to score camera-position candidates without depending on any of them.
inline float g_rawVp[16] = {};
inline volatile LONG g_rawVpValid = 0;
// Camera-position candidate in use (camcand= in inject_mode.txt).
inline volatile LONG g_camCandidate = 0;
// vpfilter=on: accept only matrices that frame the player (camera_capture.cpp).
// Off by default: with a wrong camera position the filter rejects the real
// main view, so it can only be trusted once the camera source is settled.
inline volatile LONG g_vpFilter = 0;

namespace worldobs {

// VirtualQuery costs about 6.7 ms per call inside DSR (the game's protection
// intercepts it; measured 2026-10-19).  Called several times per Mario draw it
// stalled the render threads for ~25 ms a draw: the game lagged and Mario
// jittered.  A region found readable is trusted for 250 ms per thread before
// it is queried again.
inline bool readable(uintptr_t addr, size_t size) {
    if (!addr || addr >= 0x0000800000000000ull) return false;
    struct Region { uintptr_t base, end; DWORD tick; };
    static thread_local Region cache[8] = {};
    static thread_local unsigned nextSlot = 0;
    const DWORD now = GetTickCount();
    for (const Region &r : cache)
        if (r.end && addr >= r.base && addr + size <= r.end && now - r.tick < 250) return true;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
    if (addr + size > end) return false;
    cache[nextSlot++ % 8] = {base, end, now};
    return true;
}

inline bool readPtr(uintptr_t addr, uintptr_t *out) {
    if (!readable(addr, sizeof(uintptr_t))) return false;
    uintptr_t v = *reinterpret_cast<const uintptr_t *>(addr);
    if (!v || v >= 0x0000800000000000ull) return false;
    *out = v;
    return true;
}

// Parse "48 8B ?? .." into bytes and a mask.
inline size_t parsePattern(const char *text, uint8_t *bytes, bool *mask, size_t cap) {
    size_t n = 0;
    while (*text && n < cap) {
        while (*text == ' ') ++text;
        if (!*text) break;
        if (*text == '?') {
            mask[n] = false; bytes[n] = 0; ++n;
            while (*text == '?') ++text;
        } else {
            unsigned v = 0;
            if (sscanf(text, "%2x", &v) != 1) break;
            bytes[n] = static_cast<uint8_t>(v); mask[n] = true; ++n;
            text += 2;
        }
    }
    return n;
}

// Scan the executable sections of the main module, one committed readable
// region at a time, and resolve the RIP-relative operand at +3 of the match.
inline uintptr_t scanRelative(const char *pattern) {
    uint8_t bytes[96]; bool mask[96];
    size_t len = parsePattern(pattern, bytes, mask, 96);
    if (!len) return 0;
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t start = image + sec->VirtualAddress;
        uintptr_t end = start + sec->Misc.VirtualSize;
        uintptr_t p = start;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > end) regionEnd = end;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok && regionEnd - p >= len) {
                const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
                size_t span = regionEnd - p - len;
                for (size_t i = 0; i <= span; ++i) {
                    size_t k = 0;
                    while (k < len && (!mask[k] || b[i + k] == bytes[k])) ++k;
                    if (k == len) {
                        uintptr_t hit = p + i;
                        int32_t rel = *reinterpret_cast<const int32_t *>(hit + 3);
                        return hit + 7 + rel;
                    }
                }
            }
            p = regionEnd;
        }
    }
    return 0;
}


// Same scan, for a RIP-relative operand whose disp32 sits at dispAt in an
// instruction insLen bytes long (DSR-Gadget: "80 3D disp32 imm8" is dispAt 2,
// insLen 7).  Returns the address the instruction refers to.
inline uintptr_t scanRelativeAt(const char *pattern, size_t dispAt, size_t insLen) {
    uint8_t bytes[96]; bool mask[96];
    size_t len = parsePattern(pattern, bytes, mask, 96);
    if (!len) return 0;
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t start = image + sec->VirtualAddress;
        uintptr_t end = start + sec->Misc.VirtualSize;
        uintptr_t p = start;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<void *>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > end) regionEnd = end;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok && regionEnd - p >= len) {
                const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
                size_t span = regionEnd - p - len;
                for (size_t i = 0; i <= span; ++i) {
                    size_t k = 0;
                    while (k < len && (!mask[k] || b[i + k] == bytes[k])) ++k;
                    if (k == len) {
                        uintptr_t hit = p + i;
                        int32_t rel = *reinterpret_cast<const int32_t *>(hit + dispAt);
                        return hit + insLen + rel;
                    }
                }
            }
            p = regionEnd;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Camera world-position search.
//
// The follow camera keeps the player near the centre of the screen.  For a
// float triple C near the player to be the camera's world position, the
// camera-relative point (player + 1 m up) - C, transformed by the captured
// view-projection, must land near the screen centre in front of the camera.
// That is tested for every float triple in every structure reachable within
// three pointer hops from the camera-related statics, and the paths that pass
// are logged.  Read-only.
struct CamPath {
    uintptr_t root = 0;       // static address
    int hops = 0;             // number of dereferences after the root
    uint32_t off[3] = {};     // field offset dereferenced at each hop
    uint32_t field = 0;       // float offset of the position in the final struct
};

inline bool projectsToCentre(const float *vp, const float *player, const float *cam, float *ndcOut) {
    const float p[3] = {player[0] - cam[0], player[1] + 1.0f - cam[1], player[2] - cam[2]};
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = vp[r * 4] * p[0] + vp[r * 4 + 1] * p[1] + vp[r * 4 + 2] * p[2] + vp[r * 4 + 3];
    if (clip[3] < 0.5f || clip[3] > 25.0f) return false;
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    if (ndcOut) { ndcOut[0] = x; ndcOut[1] = y; ndcOut[2] = clip[3]; }
    return x > -0.35f && x < 0.35f && y > -0.6f && y < 0.6f;
}

inline bool samePath(const CamPath &a, const CamPath &b) {
    if (a.root != b.root || a.hops != b.hops || a.field != b.field) return false;
    for (int i = 0; i + 1 < a.hops; ++i) if (a.off[i] != b.off[i]) return false;
    return true;
}

inline uintptr_t resolveCamFieldAddress(const CamPath &cp) {
    uintptr_t a = cp.root;
    for (int h = 0; h < cp.hops; ++h) {
        uintptr_t next = 0;
        if (!readPtr(a + (h == 0 ? 0 : cp.off[h - 1]), &next)) return 0;
        a = next;
    }
    return a + cp.field;
}

inline bool resolveCamPath(const CamPath &cp, float *out) {
    uintptr_t a = cp.root;
    for (int h = 0; h < cp.hops; ++h) {
        uintptr_t next = 0;
        if (!readPtr(a + (h == 0 ? 0 : cp.off[h - 1]), &next)) return false;
        a = next;
    }
    uintptr_t fieldAddr = a + cp.field;
    if (!readable(fieldAddr, 12)) return false;
    memcpy(out, reinterpret_cast<const void *>(fieldAddr), 12);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Breadth-first search from the given statics.  Returns the number of passing
// paths written to `found`.
inline int searchCamera(const uintptr_t *roots, int rootCount, const float *vp, const float *player,
                        CamPath *found, int maxFound) {
    struct Node { uintptr_t addr; CamPath path; };
    static Node queue[6000];
    int head = 0, tail = 0, nFound = 0;
    static uintptr_t seen[6000];
    int seenCount = 0;
    auto wasSeen = [&](uintptr_t a) {
        for (int i = 0; i < seenCount; ++i) if (seen[i] == a) return true;
        if (seenCount < 6000) seen[seenCount++] = a;
        return false;
    };
    for (int r = 0; r < rootCount; ++r) {
        uintptr_t obj = 0;
        if (!roots[r] || !readPtr(roots[r], &obj) || wasSeen(obj) || tail >= 6000) continue;
        CamPath cp; cp.root = roots[r]; cp.hops = 1;
        queue[tail++] = {obj, cp};
    }
    while (head < tail && nFound < maxFound) {
        Node n = queue[head++];
        const size_t span = 0x400;
        if (!readable(n.addr, span)) continue;
        const uint8_t *base = reinterpret_cast<const uint8_t *>(n.addr);
        for (uint32_t o = 0; o + 12 <= span && nFound < maxFound; o += 4) {
            const float *v = reinterpret_cast<const float *>(base + o);
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
            float dx = v[0] - player[0], dy = v[1] - player[1], dz = v[2] - player[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 0.25f || d2 > 400.0f) continue;
            if (projectsToCentre(vp, player, v, nullptr)) {
                CamPath cp = n.path; cp.field = o;
                found[nFound++] = cp;
            }
        }
        if (n.path.hops >= 3) continue;
        for (uint32_t o = 0; o + 8 <= span; o += 8) {
            uintptr_t q = *reinterpret_cast<const uintptr_t *>(base + o);
            if (q < 0x10000 || q >= 0x0000800000000000ull || (q & 7)) continue;
            if (!readable(q, 0x400) || wasSeen(q) || tail >= 6000) continue;
            CamPath cp = n.path; cp.off[cp.hops - 1] = o; cp.hops += 1;
            queue[tail++] = {q, cp};
        }
    }
    return nFound;
}

inline void formatCamPath(const CamPath &cp, char *buf, size_t cap) {
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int o = _snprintf(buf, cap, "[exe+%llX]", static_cast<unsigned long long>(cp.root - image));
    for (int h = 1; h < cp.hops && o > 0 && static_cast<size_t>(o) < cap; ++h)
        o += _snprintf(buf + o, cap - o, " ->+0x%X", cp.off[h - 1]);
    if (o > 0 && static_cast<size_t>(o) < cap) _snprintf(buf + o, cap - o, " field+0x%X", cp.field);
}

struct ProbeState {
    bool scanned = false;
    uintptr_t worldChrStatic = 0;
    uintptr_t followCamStatic = 0;
    bool crossChecked = false;
    DWORD nextLog = 0;
    int dumps = 0;
    uintptr_t camManStatic = 0;
    DWORD nextSearch = 0;
    int searches = 0;
    bool havePath = false;
    CamPath path;
    int pathHits = 0, pathMisses = 0;
    // Camera-position candidates scored continuously (see worldObservationTick).
    static const int kCandidates = 10;
    CamPath candidates[kCandidates];
    int candHits[kCandidates] = {}, candTotal[kCandidates] = {};
    DWORD nextScoreLog = 0;
};

inline ProbeState g_probe;

} // namespace worldobs

// Called from the observation worker every 250 ms.  healthBase is our own
// verified health structure, used only for the cross-check log.
inline void worldObservationTick(void *healthBase, const float *vp) {
    using namespace worldobs;
    ProbeState &st = g_probe;
    if (!st.scanned) {
        st.scanned = true;
        st.worldChrStatic = scanRelative(
            "48 8B 05 ?? ?? ?? ?? 48 8B 48 68 48 85 C9 0F 84 ?? ?? ?? ?? 48 39 5E 10 0F 84 ?? ?? ?? ?? 48");
        st.followCamStatic = scanRelative(
            "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4E 68 48 8B 05 ?? ?? ?? ?? 48 89 48 60");
        st.camManStatic = scanRelative("48 8B 05 ?? ?? ?? ?? 48 63 D1 48 8B 44 D0 08 C3");
        // Known camera position (verified live 2026-10-19, findings section 23):
        // [CamMan static] + 0x60.  Used directly; the search below only runs if
        // this path stops keeping the player centred.
        // Every place the search found a camera position (findings section 23),
        // scored side by side: [CamMan]+0x60, and fields of the follow-camera
        // struct one and two hops from its static.
        {
            const uint32_t fc1[6] = {0x40, 0x120, 0x1D0, 0x1E0, 0x3E0, 0x180};
            const uint32_t fc2[3] = {0x40, 0xF0, 0x100};
            CamPath *c = st.candidates;
            c[0] = CamPath{}; c[0].root = st.camManStatic; c[0].hops = 1; c[0].field = 0x60;
            for (int i = 0; i < 6; ++i) {
                c[1 + i] = CamPath{}; c[1 + i].root = st.followCamStatic; c[1 + i].hops = 2;
                c[1 + i].off[0] = 0x60; c[1 + i].field = fc1[i];
            }
            for (int i = 0; i < 3; ++i) {
                c[7 + i] = CamPath{}; c[7 + i].root = st.followCamStatic; c[7 + i].hops = 3;
                c[7 + i].off[0] = 0x60; c[7 + i].off[1] = 0x60; c[7 + i].field = fc2[i];
            }
        }
        if (st.camManStatic) {
            st.path = CamPath{};
            st.path.root = st.camManStatic;
            st.path.hops = 1;
            st.path.field = 0x60;
            st.havePath = true;
            logWrite("world: camera path preset: [CamMan] + 0x60");
        }
        auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        logWrite("world: AOB CamMan static=%p (exe+%llX)", reinterpret_cast<void *>(st.camManStatic),
                 static_cast<unsigned long long>(st.camManStatic ? st.camManStatic - image : 0));
        logWrite("world: AOB WorldChrMan static=%p (exe+%llX) FollowCam static=%p (exe+%llX)",
                 reinterpret_cast<void *>(st.worldChrStatic),
                 static_cast<unsigned long long>(st.worldChrStatic ? st.worldChrStatic - image : 0),
                 reinterpret_cast<void *>(st.followCamStatic),
                 static_cast<unsigned long long>(st.followCamStatic ? st.followCamStatic - image : 0));
    }

    // Player position.
    WorldObservation w = g_world;
    w.playerValid = false;
    uintptr_t wcm = 0, playerIns = 0, mapData = 0, posData = 0;
    if (st.worldChrStatic && readPtr(st.worldChrStatic, &wcm) && readPtr(wcm + 0x68, &playerIns) &&
        readPtr(playerIns + 0x68, &mapData) && readPtr(mapData + 0x28, &posData) &&
        readable(posData, 0x1C)) {
        const float *f = reinterpret_cast<const float *>(posData);
        g_livePlayerPos = posData;
        g_liveMapData = readable(mapData + 0x100, 0x30) ? mapData : 0;
        g_livePlayerIns = readable(playerIns + 0x2A0, 8) ? playerIns : 0;
        g_livePlayerChecked = GetTickCount();
        w.playerAngle = f[1];
        w.player[0] = f[4]; w.player[1] = f[5]; w.player[2] = f[6];
        w.playerValid = std::isfinite(w.player[0]) && std::isfinite(w.player[1]) &&
                        std::isfinite(w.player[2]);
        if (!st.crossChecked && healthBase) {
            st.crossChecked = true;
            logWrite("world: cross-check PlayerIns=%p health base=%p -> %s",
                     reinterpret_cast<void *>(playerIns), healthBase,
                     reinterpret_cast<void *>(playerIns) == healthBase ? "SAME OBJECT" : "different");
        }
    }

    // Follow-camera struct.
    w.followCamValid = false;
    uintptr_t a = 0, b = 0;
    if (st.followCamStatic && readPtr(st.followCamStatic, &a) && readPtr(a, &b) &&
        readPtr(b + 0x60, &a) && readable(a + 0x60, sizeof(w.followCam))) {
        memcpy(w.followCam, reinterpret_cast<const void *>(a + 0x60), sizeof(w.followCam));
        w.followCamValid = true;
    }
    DWORD now = GetTickCount();

    // Camera position: search a few times, then follow the most frequent path.
    w.cameraValid = false;
    if (w.playerValid && vp) {
        if (!st.havePath && st.searches < 8 && now >= st.nextSearch) {
            st.nextSearch = now + 3000;
            ++st.searches;
            const uintptr_t roots[3] = {st.camManStatic, st.followCamStatic, st.worldChrStatic};
            static CamPath found[24];
            int n = searchCamera(roots, 3, vp, w.player, found, 24);
            logWrite("world: camera search %d found %d path(s)", st.searches, n);
            for (int i = 0; i < n && i < 12; ++i) {
                char desc[160];
                formatCamPath(found[i], desc, sizeof(desc));
                float c[3] = {}, ndc[3] = {};
                if (resolveCamPath(found[i], c)) projectsToCentre(vp, w.player, c, ndc);
                logWrite("world:   %s = (%.3f, %.3f, %.3f) player ndc=(%.2f, %.2f) dist=%.2f", desc,
                         c[0], c[1], c[2], ndc[0], ndc[1], ndc[2]);
            }
            // Adopt the first path once it has passed in two separate searches.
            static CamPath previous[24];
            static int previousCount = 0;
            for (int i = 0; i < n && !st.havePath; ++i)
                for (int j = 0; j < previousCount; ++j)
                    if (samePath(found[i], previous[j])) {
                        st.path = found[i];
                        st.havePath = true;
                        char desc[160];
                        formatCamPath(st.path, desc, sizeof(desc));
                        logWrite("world: camera path adopted: %s", desc);
                        break;
                    }
            for (int i = 0; i < n; ++i) previous[i] = found[i];
            previousCount = n;
        }
        LONG sel = g_camCandidate;
        if (sel >= 0 && sel < ProbeState::kCandidates && st.candidates[sel].root) {
            st.path = st.candidates[sel];
            st.havePath = true;
        }
        if (st.havePath && resolveCamPath(st.path, w.camera)) {
            w.cameraValid = true;
            g_liveCameraPos = resolveCamFieldAddress(st.path);
            if (g_liveCameraPos && readable(g_liveCameraPos, 12)) g_liveCameraChecked = GetTickCount();
            if (projectsToCentre(vp, w.player, w.camera, nullptr)) ++st.pathHits; else ++st.pathMisses;
        }
        // Score every candidate against the unfiltered latest matrix.
        if (g_rawVpValid) {
            float rawVp[16];
            memcpy(rawVp, g_rawVp, sizeof(rawVp));
            for (int i = 0; i < ProbeState::kCandidates; ++i) {
                float c[3];
                if (!st.candidates[i].root || !resolveCamPath(st.candidates[i], c)) continue;
                ++st.candTotal[i];
                if (projectsToCentre(rawVp, w.player, c, nullptr)) ++st.candHits[i];
            }
        }
        if (now >= st.nextScoreLog) {
            st.nextScoreLog = now + 5000;
            char line[600];
            int o = _snprintf(line, sizeof(line), "world: camcand scores (selected %ld):", sel);
            for (int i = 0; i < ProbeState::kCandidates && o > 0 && o < 560; ++i) {
                float c[3] = {};
                bool ok = st.candidates[i].root && resolveCamPath(st.candidates[i], c);
                o += _snprintf(line + o, sizeof(line) - o, " [%d] %d/%d%s", i, st.candHits[i],
                               st.candTotal[i], ok ? "" : "(unresolved)");
                st.candHits[i] = st.candTotal[i] = 0;
            }
            logWrite("%s", line);
        }
    }
    g_world = w;

    if (now < st.nextLog) return;
    st.nextLog = now + 2000;
    if (w.playerValid)
        logWrite("world: player pos=(%.3f, %.3f, %.3f) angle=%.3f camera=%s(%.3f, %.3f, %.3f) "
                 "centred %d/%d", w.player[0], w.player[1], w.player[2], w.playerAngle,
                 w.cameraValid ? "" : "none", w.camera[0], w.camera[1], w.camera[2], st.pathHits,
                 st.pathHits + st.pathMisses);
}

// Player and camera world positions read now, from the addresses the worker
// last resolved.  Falls back to the worker's copies when an address is missing
// or unreadable.  Returns false when neither is available.
// Native facing read at the moment of the call (ChrPosData +0x4).
inline bool readLiveAngle(float &angle) {
    uintptr_t p = g_livePlayerPos;
    if (!p || !(GetTickCount() - g_livePlayerChecked < 1000 || worldobs::readable(p + 0x4, 4))) return false;
    const float a = *reinterpret_cast<const float *>(p + 0x4);
    if (!std::isfinite(a)) return false;
    angle = a;
    return true;
}

inline bool readLivePositions(float player[3], float camera[3]) {
    WorldObservation w = g_world;
    if (!w.playerValid || !w.cameraValid) return false;
    for (int i = 0; i < 3; ++i) { player[i] = w.player[i]; camera[i] = w.camera[i]; }
    uintptr_t p = g_livePlayerPos, c = g_liveCameraPos;
    const DWORD nowTick = GetTickCount();
    if (p && (nowTick - g_livePlayerChecked < 1000 || worldobs::readable(p + 0x10, 12))) {
        const float *f = reinterpret_cast<const float *>(p + 0x10);
        if (std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]))
            for (int i = 0; i < 3; ++i) player[i] = f[i];
    }
    if (c && (nowTick - g_liveCameraChecked < 1000 || worldobs::readable(c, 12))) {
        const float *f = reinterpret_cast<const float *>(c);
        if (std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]))
            for (int i = 0; i < 3; ++i) camera[i] = f[i];
    }
    return true;
}

} // namespace remastered
