#pragma once

// Native actions while Mario drives (v118): the PTDE host's actions.h /
// interaction.h / runtime.cpp nativeInteractionState, for Remastered.
//
// Interactions (bonfire, item, message, door, lever, ladder) and item use
// (estus) are the game's own: the knight performs them, Mario stops being
// simulated, is drawn at the knight in the matching SM64 pose (drink, rest,
// climb, door, walk), and gets control back once the knight is idle again.
//
// Offsets (live probe 2026-10-22, actionprobe=on):
//   MenuMan [0x141C88D98] +0x6C: 1 while an action prompt is shown (PTDE +0x58).
//   MenuMan fields that own the controls while a menu/box is open (bonfire
//   menu, item get, message, ...): +0x54 +0x74 +0xAC +0x104 +0x12C +0x150
//   +0x1AC +0x27C (PTDE checks a longer list the same way).
//   Animation: PlayerIns +0x16C (full-body animation: 7710/7711/7712 at a
//   bonfire); the tracks [ChrAnimData(+0x68 -> +0x18) +0x30] + 0x38 + n*0xA8
//   (drink 7585-7587 in track 8, rest in track 11; PTDE: 29 tracks of 0x9C).
// Event animations (id % 100000): 7000-7099 ladder, 7100-7399 doors/levers,
// 7585-7587 drink, 7700-7749 rest, anything in 7000-8999 is native-owned.

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "logging.h"
#include "world_observation.h"

namespace remastered {
namespace nativeact {

inline bool copy(uintptr_t at, void *out, size_t n) {
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, n, &got) && got == n;
}
inline uintptr_t ptr(uintptr_t at) {
    uintptr_t v = 0;
    return copy(at, &v, 8) ? v : 0;
}

inline bool g_takeSnapshot = false;
inline bool g_snapFresh = false;
inline volatile LONG g_padActivity = 0;  // GetTickCount() of the last pad / key change (input filter)
inline uint32_t g_snap[0xA0];
inline bool g_volatile[0xA0];

struct Observation {
    bool valid = false, prompt = false, menu = false;
    int anim = -1;        // event animation (id % 100000) currently shown, or -1
    int trackAnim = -1;   // the tracks' event animation that most recently changed
    int src = -1;         // v172: where anim came from: 0 full-body (+0x16C), 1+n track n
    int raw = -1;         // v172: the raw id read there
};
inline int g_trackSrc = -1, g_trackRaw = -1;  // v172: set by observe() for the track pick

inline Observation observe() {
    Observation o;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t menu = ptr(base + 0x1C88D98), ins = g_livePlayerIns;
    if (!menu || !ins) return o;
    uint32_t m[0xA0];  // MenuMan +0x000 .. +0x280
    if (!copy(menu, m, sizeof(m))) return o;
    o.valid = true;
    o.prompt = m[0x6C / 4] != 0;
    // Menus (v119): MenuMan compared with a snapshot taken when the player
    // acted (A at a prompt, Start/Select, an event animation).  Fields that
    // were already changing in the 2 s before are left out: several change in
    // ordinary play (+0x74, +0x150, +0x1AC, +0x1C8 ...), and treating them as
    // menus froze Mario for good (v118).
    {
        static uint32_t prevM[0xA0];
        static DWORD changedAt[0xA0];
        static bool init = false;
        const DWORD now = GetTickCount();
        // The snapshot is the PREVIOUS observation (before the press opened
        // anything); volatility is judged up to then too.
        if (g_takeSnapshot && init) {
            g_takeSnapshot = false;
            memcpy(g_snap, prevM, sizeof(g_snap));
            g_snapFresh = true;
            for (int i = 0; i < 0xA0; ++i) g_volatile[i] = now - changedAt[i] < 2000 || i == 0x6C / 4;
        }
        for (int i = 0; i < 0xA0; ++i) {
            if (init && m[i] != prevM[i]) changedAt[i] = now;
            prevM[i] = m[i];
        }
        init = true;
        // v170n: a menu ends when one of the fields that opened it goes back (250 ms).  Fields
        // that changed and stay changed (counters: souls, items picked up, a shop's stock) held
        // the knight for good after item pickups, talks, shops and bonfires; on the close they are
        // taken into the snapshot.
        static bool active = false, opened[0xA0];
        static DWORD revertSince = 0;
        if (g_snapFresh) { g_snapFresh = false; active = false; }
        bool diff[0xA0], any = false;
        for (int i = 0; i < 0xA0; ++i) {
            diff[i] = !g_volatile[i] && m[i] != g_snap[i] && m[i] < 0x10000 && g_snap[i] < 0x10000;
            any = any || diff[i];
        }
        if (!any) active = false;
        else if (!active) {
            active = true;
            revertSince = 0;
            for (int i = 0; i < 0xA0; ++i) opened[i] = diff[i];
        }
        if (active) {
            bool reverted = false;
            for (int i = 0; i < 0xA0 && !reverted; ++i) reverted = opened[i] && !diff[i];
            if (reverted) {
                if (!revertSince) revertSince = now;
                if (now - revertSince >= 250) {
                    for (int i = 0; i < 0xA0; ++i) if (diff[i]) g_snap[i] = m[i];
                    active = false;
                    any = false;
                    logWrite("native: menu closed (an opening field went back); lasting changes taken as the new normal");
                }
            } else {
                revertSince = 0;
            }
        }
        o.menu = any;
    }
    // The full-body animation stays at its last value afterwards (7712 after
    // a bonfire), so it counts for 2 s after it changed; the bonfire's long
    // rest loop is covered by its menu.
    int full = 0;
    if (copy(ins + 0x16C, &full, 4)) {
        static int prevFull = -1;
        static DWORD fullChanged = 0;
        const DWORD now = GetTickCount();
        if (full != prevFull) { prevFull = full; fullChanged = now; }
        const int a = full % 100000;
        if (full >= 0 && a >= 7000 && a < 9000 && now - fullChanged < 2000) { o.anim = a; o.src = 0; o.raw = full; }
    }
    // Tracks: an event animation counts while it keeps changing or for 2 s
    // after it last changed (stale values stay behind in idle tracks).
    static int prev[29];
    static DWORD changed[29];
    static bool init = false;
    const uintptr_t mapData = ptr(ins + 0x68), animData = mapData ? ptr(mapData + 0x18) : 0;
    const uintptr_t tracks = animData ? ptr(animData + 0x30) : 0;
    if (tracks) {
        const DWORD now = GetTickCount();
        DWORD newest = 0;
        for (int i = 0; i < 29; ++i) {
            int id = -1;
            if (!copy(tracks + 0x38 + size_t(i) * 0xA8, &id, 4)) break;
            if (init && id != prev[i]) changed[i] = now;
            prev[i] = id;
            const int a = id % 100000;
            if (id >= 0 && a >= 7000 && a < 9000 && init && now - changed[i] < 2000 && changed[i] >= newest) {
                newest = changed[i];
                o.trackAnim = a;
                g_trackSrc = 1 + i;
                g_trackRaw = id;
            }
        }
        init = true;
    }
    if (o.anim < 0 && o.trackAnim >= 0) { o.anim = o.trackAnim; o.src = g_trackSrc; o.raw = g_trackRaw; }
    // v172: an event animation must read the same for 3 polls (~100 ms) in a row.  After a
    // death in the lower Catacombs (fall into the pit, respawn in m13_00) the reads gave a
    // different 7000-8999 id every poll; each one took the knight from Mario for a moment
    // (about 50 times a minute) and Mario lost control.  Real event animations (doors,
    // ladders, levers, drinks, rests) last 400 ms or more, so the delay costs nothing.
    {
        static int candAnim = -1, candCount = 0, flickers = 0, lastSrc = -1, lastRaw = -1;
        static DWORD flickerLog = 0;
        if (o.anim == candAnim) {
            if (candCount < 1000) ++candCount;
        } else {
            if (candAnim >= 0 && candCount < 3) { ++flickers; lastSrc = o.src >= 0 ? o.src : lastSrc; }
            candAnim = o.anim;
            candCount = 1;
            if (o.anim >= 0) { lastSrc = o.src; lastRaw = o.raw; }
        }
        const DWORD now = GetTickCount();
        if (flickers && now - flickerLog > 5000) {
            flickerLog = now;
            logWrite("native: %d unsteady animation reads ignored (last from %s %d, raw %d)", flickers,
                     lastSrc == 0 ? "full-body" : "track", lastSrc > 0 ? lastSrc - 1 : 0, lastRaw);
            flickers = 0;
        }
        if (o.anim >= 0 && candCount < 3) o.anim = -1;
    }
    return o;
}

// Synthetic pad A for the knight (pressed for a few game polls), through the
// game's XInput wrapper.
inline volatile LONG g_synthA = 0, g_synthX = 0;

// Owned by the game right now: Mario is posed, not simulated.
inline volatile LONG g_owned = 0;

} // namespace nativeact
} // namespace remastered
