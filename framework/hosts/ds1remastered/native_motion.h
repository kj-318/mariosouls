#pragma once

// Native motion for Mario-driven movement (drive=native), the Remastered
// counterpart of PTDE's hosts/ds1ptde/world.h motion hook.
//
// DarkSoulsRemastered.exe RVA 0x2BB020 (live-identified 2026-10-21):
//   void ChrPhysics_SetMotion(ChrPosData *rcx, float xmm1, const vec4 *r8, bool r9b)
//   40 53 48 83 EC 20 FF 81 00 01 00 00   push rbx; sub rsp,20h; inc [rcx+100h]
// It copies three vec4s from r8 to ChrPosData +0x70/+0x80/+0x90 before the
// physics step: +0x70 is the frame's displacement in the character's local
// frame (forward is -Z, found with the motion probe), +0x80 = (0,0,0,1),
// +0x90 = (1,1,1,1).  PTDE's equivalent copies to ChrPhysics +0x60 and starts
// with inc [esi+0E4h].
//
// While Mario drives the player, the hook replaces the displacement with the
// remaining distance to Mario's latest target, once per published target
// (never chasing an old destination twice), and zero otherwise.  Native
// physics then moves the capsule with collision; the pose thread reads the
// resolved position back and corrects Mario.  Every other character, and the
// player when not driven, is passed through unchanged.

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "logging.h"
#include "world_observation.h"

extern "C" {
int WINAPI MH_Initialize(void);
int WINAPI MH_CreateHook(LPVOID target, LPVOID detour, LPVOID *original);
int WINAPI MH_EnableHook(LPVOID target);
}

namespace remastered {
namespace nativemotion {

using SetMotionFn = void (*)(void *physics, float x, const float *motion, bool flag);
inline SetMotionFn g_original = nullptr;
inline volatile LONG g_ready = 0;

// Mailbox: the latest target for the player's capsule (world metres) and
// facing (native angle), published by the 30 Hz pose thread.
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline float g_target[3] = {};
inline float g_yaw = 0;
inline volatile int g_pending = 0;  // game frames left to reach the target (spread over 2)
inline volatile LONG g_enabled = 0;     // the pose thread drives the player now
inline volatile DWORD g_heartbeat = 0;  // last publish/keepalive (GetTickCount)
inline volatile LONG g_calls = 0, g_applied = 0, g_rejected = 0;
inline volatile LONG g_flipX = 0;        // nativex=flip (live) if motion is mirrored
// The player's own calls: whether the game keeps stepping him in the air.
inline volatile LONG g_playerCalls = 0;
inline volatile DWORD g_lastPlayerCall = 0;

inline void hookedSetMotion(void *physics, float x, const float *motion, bool flag) {
    InterlockedIncrement(&g_calls);
    const uintptr_t player = g_livePlayerPos;
    if (physics && reinterpret_cast<uintptr_t>(physics) == player) {
        InterlockedIncrement(&g_playerCalls);
        g_lastPlayerCall = GetTickCount();
    }
    if (!g_enabled || !physics || reinterpret_cast<uintptr_t>(physics) != player ||
        GetTickCount() - g_heartbeat > 250) {
        g_original(physics, x, motion, flag);
        return;
    }
    // Driven: our displacement only (zero unless a new target is pending).
    alignas(16) float v[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    if (motion) {
        // Keep the game's second and third vectors (observed (0,0,0,1) and
        // (1,1,1,1)); only the displacement is ours.
        memcpy(v + 4, motion + 4, sizeof(float) * 8);
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_pending > 0) {
        // Spread each 30 Hz step over the game's two 60 fps frames: sending it
        // in one frame moved the knight 30 cm every other frame at top speed,
        // and Mario, drawn at the knight, stuttered (live 2026-10-21, v96).
        const int parts = g_pending--;
        const float *pos = reinterpret_cast<const float *>(player + 0x10);
        float dx = (g_target[0] - pos[0]) / parts, dy = (g_target[1] - pos[1]) / parts,
              dz = (g_target[2] - pos[2]) / parts;
        // Long catch-ups (after a jump, the knight's steps pause in the air)
        // are taken in steps of at most 1 m instead of rejected (v102 log).
        const float h = std::sqrt(dx * dx + dz * dz);
        if (std::isfinite(h) && h > 1.0f && h < 8.0f) { dx /= h; dz /= h; }
        if (std::isfinite(dy) && std::fabs(dy) > 1.0f) dy = dy > 0 ? 1.0f : -1.0f;
        if (std::isfinite(dx) && std::isfinite(dz) && dx * dx + dz * dz <= 1.01f) {
            // Facing first, then the world delta in the character's frame.
            *reinterpret_cast<float *>(player + 0x4) = g_yaw;
            const float a = g_yaw, cs = std::cos(a), sn = std::sin(a);
            const float wx = g_flipX ? -dx : dx;
            v[0] = cs * wx - sn * dz;
            v[1] = dy;
            v[2] = sn * wx + cs * dz;
            InterlockedIncrement(&g_applied);
        } else {
            InterlockedIncrement(&g_rejected);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_original(physics, x, v, flag);
}

inline void publish(const float target[3], float yaw) {
    AcquireSRWLockExclusive(&g_lock);
    g_target[0] = target[0]; g_target[1] = target[1]; g_target[2] = target[2];
    g_yaw = yaw;
    g_pending = 2;
    g_heartbeat = GetTickCount();
    ReleaseSRWLockExclusive(&g_lock);
}

inline void keepAlive() { g_heartbeat = GetTickCount(); }

// Game warp request (DSR-Gadget: ChrMapData +0x108 flag, +0x110 XYZ): used
// only to put the knight onto a ledge Mario is standing on (real static
// floor), which the knight's capsule cannot climb.
inline volatile LONG g_warps = 0;
inline bool warpTo(const float target[3]) {
    const uintptr_t map = g_liveMapData;
    if (!map || GetTickCount() - g_livePlayerChecked > 1000) return false;
    float *t = reinterpret_cast<float *>(map + 0x110);
    t[0] = target[0]; t[1] = target[1]; t[2] = target[2];
    *reinterpret_cast<volatile uint8_t *>(map + 0x108) = 1;
    InterlockedIncrement(&g_warps);
    return true;
}

// Floating knight (knightfloat=on): DSR-Gadget's switches.  No gravity
// (PlayerIns ChrFlags1 +0x2A4 bit 0x4000, 1.03 layout; 0x284 + 0x20) keeps
// the game from ever putting him in its airborne state, so he takes every
// step, up and down, along Mario's path.  Map hit off (ChrMapData +0x104 bit
// 0x10) while Mario is in the air, so his capsule does not catch on ledge lips
// Mario jumps over.  Both are put back when driving stops.
inline volatile LONG g_floatOn = 0;  // setting
inline bool g_noGravitySet = false, g_noHitSet = false;
inline uintptr_t g_flagIns = 0, g_flagMap = 0;
inline void setFlag(uintptr_t addr, uint32_t bit, bool on) {
    volatile LONG *p = reinterpret_cast<volatile LONG *>(addr);
    if (on) InterlockedOr(p, static_cast<LONG>(bit));
    else InterlockedAnd(p, ~static_cast<LONG>(bit));
}
inline void applyFloat(bool noGravity, bool noHit) {
    const uintptr_t ins = g_livePlayerIns, map = g_liveMapData;
    const bool live = ins && map && GetTickCount() - g_livePlayerChecked < 1000;
    // A new character (load, death): the old flags went with the old one.
    if (live && (ins != g_flagIns || map != g_flagMap)) {
        g_noGravitySet = g_noHitSet = false;
        g_flagIns = ins; g_flagMap = map;
    }
    if (!live) return;
    if (noGravity != g_noGravitySet) { setFlag(ins + 0x2A4, 0x4000, noGravity); g_noGravitySet = noGravity; }
    if (noHit != g_noHitSet) { setFlag(map + 0x104, 0x10, noHit); g_noHitSet = noHit; }
}

// Dodge invincibility: the knight (the hurtbox enemies hit) gets DSR-Gadget's
// EnableInvincible (PlayerIns ChrFlags1 +0x2A4 bit 0x08000000: no damage, super
// armor) for the i-frames.  NoHit (v109) also let him pass through enemies.
inline bool g_iframesSet = false;
inline uintptr_t g_iframesIns = 0;
inline void applyIframes(bool on) {
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || GetTickCount() - g_livePlayerChecked > 1000) return;
    if (ins != g_iframesIns) { g_iframesSet = false; g_iframesIns = ins; }
    if (on != g_iframesSet) { setFlag(ins + 0x2A4, 0x08000000, on); g_iframesSet = on; }
}

inline bool install() {
    if (g_ready) return true;
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    uint8_t *target = base + 0x2BB020;
    static const uint8_t expected[12] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xFF, 0x81, 0x00, 0x01, 0x00, 0x00};
    if (!worldobs::readable(reinterpret_cast<uintptr_t>(target), sizeof(expected)) ||
        memcmp(target, expected, sizeof(expected)) != 0) {
        logWrite("native motion: signature mismatch at %p; drive=native unavailable", target);
        return false;
    }
    const int init = MH_Initialize();
    if (init != 0 && init != 1) {  // MH_OK, MH_ERROR_ALREADY_INITIALIZED
        logWrite("native motion: MinHook init failed (%d)", init);
        return false;
    }
    void *original = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void *>(&hookedSetMotion), &original) != 0 || !original) {
        logWrite("native motion: hook creation failed");
        return false;
    }
    g_original = reinterpret_cast<SetMotionFn>(original);
    if (MH_EnableHook(target) != 0) {
        logWrite("native motion: hook enable failed");
        return false;
    }
    InterlockedExchange(&g_ready, 1);
    logWrite("native motion: hooked ChrPhysics motion at %p", target);
    return true;
}

} // namespace nativemotion
} // namespace remastered
