#pragma once

// One-off probe (actionprobe=on) for the 64-bit offsets PTDE's interaction
// and estus system needs (hosts/ds1ptde/runtime.cpp nativeInteractionState):
//   * MenuMan [0x141C88D98]: small integer fields that change (the action
//     prompt is PTDE's +0x58; menus own controls through others).
//   * the knight's animation tracks: where an animation ID (id % 100000 in the
//     drink 7585-7587, rest 7700-7749, door 7100-7399, ladder 7000-7099
//     ranges) appears under PlayerIns, ChrMapData (+0x68), its ChrAnimData
//     (+0x18) and one level of pointers inside those.
// Read-only; logs changes at most 5 times a second.

#include <windows.h>
#include <cstdint>

#include "logging.h"
#include "world_observation.h"

namespace remastered {
namespace actionprobe {

inline volatile LONG g_on = 0;

// Every read is a copy that fails cleanly (ReadProcessMemory): game memory
// can be freed between a check and a read (v115 crashed at start).
inline bool copy(uintptr_t at, void *out, size_t n) {
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, n, &got) && got == n;
}
inline bool rd(uintptr_t at, uintptr_t &out) { return copy(at, &out, 8) && out != 0; }

inline bool interesting(int v) {
    if (v < 7000 || v >= 1000000) return false;
    const int a = v % 100000;
    return (a >= 7000 && a < 7400) || (a >= 7580 && a <= 7590) || (a >= 7700 && a < 7750);
}

inline void scan(const char *name, uintptr_t at, size_t size, int depth) {
    static uint8_t bufs[2][0x1000];
    if (size > 0x1000 || depth > 1) return;
    uint8_t *buf = bufs[depth];
    if (!copy(at, buf, size)) return;
    static uintptr_t seenAt[64];
    static int seenVal[64], seenN = 0;
    for (size_t o = 0; o + 4 <= size; o += 4) {
        int v;
        memcpy(&v, buf + o, 4);
        if (interesting(v)) {
            bool known = false;
            for (int i = 0; i < seenN; ++i)
                if (seenAt[i] == at + o) { known = seenVal[i] == v; seenVal[i] = v; break; }
            if (!known) {
                if (seenN < 64) { seenAt[seenN] = at + o; seenVal[seenN++] = v; }
                logWrite("animprobe: %s+0x%zX = %d (anim %d)", name, o, v, v % 100000);
            }
        }
        if (depth > 0 && (o % 8) == 0 && o + 8 <= size) {
            uintptr_t q;
            memcpy(&q, buf + o, 8);
            if (q > 0x10000 && q < 0x7FFFFFFF0000ull && (q & 7) == 0) {
                char sub[64];
                _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s+0x%zX->", name, o);
                scan(sub, q, 0x800, 0);
            }
        }
    }
}

inline void tick() {
    if (!g_on) return;
    static DWORD last = 0;
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    // MenuMan
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t mm = 0;
    static uint32_t cur[512];
    if (rd(base + 0x1C88D98, mm) && copy(mm, cur, sizeof(cur))) {
        static uint32_t prev[512];
        static bool init = false;
        int lines = 0;
        for (int i = 0; i < 512; ++i) {
            const uint32_t v = cur[i];
            if (init && v != prev[i] && v < 0x100000 && prev[i] < 0x100000 && lines < 16) {
                logWrite("menuprobe: MenuMan+0x%X %u -> %u", i * 4, prev[i], v);
                ++lines;
            }
            prev[i] = v;
        }
        init = true;
    }
    // Animation tracks
    const uintptr_t ins = g_livePlayerIns;
    if (!ins) return;
    scan("PlayerIns", ins, 0x800, 0);
    uintptr_t map = 0, anim = 0;
    if (rd(ins + 0x68, map)) {
        scan("ChrMapData", map, 0x400, 0);
        if (rd(map + 0x18, anim)) scan("ChrAnimData", anim, 0x400, 1);
    }
}

} // namespace actionprobe
} // namespace remastered
