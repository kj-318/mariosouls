// obj_probe.h -- diagnostic (objprobe=on, live; default off), v165.
//
// Breakable objects for Mario's attacks (PTDE object_combat.h): PTDE passes the map object to
// the same native damage builder as characters, reading its HP (int16) and a breakable flag
// from the object.  Remastered's layout is not known yet, so this finds ObjIns instances (by
// their vtable, from the exe's RTTI: ObjIns vtable RVA 0x13186F8) and logs the ones near the
// knight: a dump when first seen, then every field that changes (break one with the knight to
// see its HP and state fields move).  Read-only.
#pragma once

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "logging.h"
#include "world_observation.h"

namespace remastered {
namespace objprobe {

inline volatile LONG g_on = 0;
inline volatile LONG g_scanWanted = 0;  // object hits need the instance list (v168)
constexpr uintptr_t kObjInsVtRva = 0x13186F8;
constexpr size_t kDump = 0x300;

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::vector<uintptr_t> g_objs;
inline volatile LONG g_scanning = 0;

inline bool rd(uintptr_t at, void *out, size_t n) {
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, n, &got) && got == n;
}

// Background: every private read-write region scanned for the ObjIns vtable pointer.
inline DWORD WINAPI scanThread(void *) {
    const uintptr_t vt = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kObjInsVtRva;
    std::vector<uint8_t> buf;
    for (;;) {
        if (g_on || g_scanWanted) {
            std::vector<uintptr_t> found;
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t at = 0x10000;
            while (at < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
                const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const size_t size = mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READWRITE &&
                    !(mbi.Protect & PAGE_GUARD) && size <= (512u << 20)) {
                    for (size_t off = 0; off < size; off += (16u << 20)) {
                        const size_t len = size - off < (16u << 20) ? size - off : (16u << 20);
                        buf.resize(len);
                        if (!rd(start + off, buf.data(), len)) continue;
                        for (size_t i = 0; i + 8 <= len; i += 8) {
                            uintptr_t v;
                            memcpy(&v, buf.data() + i, 8);
                            if (v == vt && found.size() < 16384) found.push_back(start + off + i);
                        }
                    }
                }
                at = start + size;
                if (!size) break;
            }
            AcquireSRWLockExclusive(&g_lock);
            g_objs.swap(found);
            ReleaseSRWLockExclusive(&g_lock);
            logWrite("objprobe: %zu ObjIns instances", g_objs.size());
        }
        Sleep(15000);
    }
}

inline void start() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (HANDLE h = CreateThread(nullptr, 0, scanThread, nullptr, 0, nullptr)) CloseHandle(h);
}

// Game thread, every 500 ms.
inline void tick(const float player[3]) {
    if (!g_on) return;
    start();
    static DWORD last = 0;
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    struct Seen { uintptr_t obj; uint8_t bytes[kDump]; };
    static std::vector<Seen> seen;
    static int lines = 0;
    if (lines > 3000) return;
    std::vector<uintptr_t> objs;
    AcquireSRWLockShared(&g_lock);
    objs = g_objs;
    ReleaseSRWLockShared(&g_lock);
    uint8_t b[kDump];
    for (uintptr_t o : objs) {
        if (!rd(o, b, kDump)) continue;
        // Near: a float triple within 3 m of the knight in the object, or (v167: the position is
        // not stored in ObjIns itself) in an object one of its first 0x100 bytes points at.
        auto nearIn = [&](const uint8_t *bb, size_t n) -> int {
            for (size_t i = 0; i + 12 <= n; i += 4) {
                float f[3];
                memcpy(f, bb + i, 12);
                if (std::fabs(f[0] - player[0]) < 3.f && std::fabs(f[1] - player[1]) < 3.f && std::fabs(f[2] - player[2]) < 3.f)
                    return static_cast<int>(i);
            }
            return -1;
        };
        int posAt = nearIn(b, kDump), posPtr = -1;
        if (posAt < 0) {
            static uint8_t sub[0x200];
            for (size_t k = 8; k < 0x100 && posAt < 0; k += 8) {
                uintptr_t q;
                memcpy(&q, b + k, 8);
                if (q < 0x10000 || q >= 0x0000800000000000ull || !rd(q, sub, sizeof(sub))) continue;
                const int at = nearIn(sub, sizeof(sub));
                if (at >= 0) { posAt = at; posPtr = static_cast<int>(k); }
            }
        }
        Seen *s = nullptr;
        for (Seen &q : seen) if (q.obj == o) { s = &q; break; }
        if (posAt < 0 && !s) continue;
        if (!s) {
            if (seen.size() >= 64) continue;
            seen.push_back({});
            s = &seen.back();
            s->obj = o;
            memcpy(s->bytes, b, kDump);
            if (posPtr >= 0)
                logWrite("objprobe: object %p near the knight (position at [+0x%X]+0x%X); dump:", reinterpret_cast<void *>(o), posPtr, posAt);
            else
                logWrite("objprobe: object %p near the knight (position at +0x%X); dump:", reinterpret_cast<void *>(o), posAt);
            for (size_t r = 0; r < kDump; r += 32) {
                char line[160];
                int n = snprintf(line, sizeof(line), "objprobe:   +%03zX:", r);
                for (size_t k = 0; k < 32; k += 4) {
                    uint32_t v;
                    memcpy(&v, b + r + k, 4);
                    n += snprintf(line + n, sizeof(line) - n, " %08X", v);
                }
                logWrite("%s", line);
                ++lines;
            }
            continue;
        }
        char dl[900];
        int dn = 0;
        for (size_t k = 0; k < kDump; k += 2) {
            uint16_t ov, nv;
            memcpy(&ov, s->bytes + k, 2);
            memcpy(&nv, b + k, 2);
            if (ov == nv) continue;
            if (dn > 800) break;
            dn += snprintf(dl + dn, sizeof(dl) - dn, " +%03zX:%04X>%04X", k, ov, nv);
        }
        if (dn) {
            logWrite("objprobe: object %p changed:%s", reinterpret_cast<void *>(o), dl);
            ++lines;
            memcpy(s->bytes, b, kDump);
        }
    }
}

} // namespace objprobe
} // namespace remastered
