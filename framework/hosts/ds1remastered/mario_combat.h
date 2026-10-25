#pragma once

// Mario's own hits, through the game's native damage (v113).
//
// Detection is Mario's: while SM64 reports an active attack (punch, kick,
// jump kick, ground pound), every live character within reach is offered to
// the engine's own attack test (SC probeContact) at a point on its body, the
// PTDE host's combat.h method.  A hit is applied once per enemy per attack.
//
// Damage is the game's: DarkSoulsRemastered.exe 0x1403C9B50, the builder the
// event script function CreateDamage_NoCollision calls (found 2026-10-22 from
// its Lua binding, DLConcreteMethodInvoker<FrpgEventProxy, void(int,int,int,
// float,float,float)>; PTDE's 0xDEA8E0):
//   int Damage(DamageMan *[0x141C7A050], int behaviorId, ChrIns *owner,
//              ChrIns *target, const float point[4], int -1, void *0)
// with the knight as owner and the Caestus's own BehaviorParam_PC rows
// (weapon behavior variation 4200), so the damage, poise, defences, stagger,
// death and souls are the game's, scaled by the knight's equipped Caestus and
// its upgrade.  The call runs on the game's thread (the XInput poll), never on
// the pose thread.
//
// Characters: WorldChrMan [0x141C77E50] (the same table CreateDamage resolves
// through): groups [+0x28] x 0xE8 bytes from [+0x30] + 0x48 {int count; entry
// *entries (0x38 bytes, ChrIns* first)}, plus the list at +0x38 {count,
// entries at +0x40}.  HP at ChrIns +0x3E8; position ChrIns +0x68 -> +0x28 ->
// +0x10 (as for the player).

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <xinput.h>

#include "../../core/feedback.h"
#include "logging.h"
#include "mario_setup.h"
#include "install.h"
#include "world_observation.h"
#include "obj_probe.h"

namespace remastered {
namespace mariocombat {

constexpr uintptr_t kWorldChrManRva = 0x1C77E50, kDamageManRva = 0x1C7A050, kDamageFnRva = 0x3C9B50;
using DamageFn = int (*)(void *manager, int behavior, void *owner, void *target, const float *point, int unk,
                         void *unk2);

// Caestus (BehaviorVariationID 4200) BehaviorParam_PC rows.
constexpr int kPunch1 = 104200000, kPunch2 = 104200010, kHeavy = 104200100, kPlunge = 104200050;
// Dive, slide kick and automatic stomp use the other Caestus rows (judge IDs 110,
// 100 and 20).  Their mapping is by judge ID only and needs live verification.
constexpr int kSlideKick = 104200110, kStomp = 104200020;
// The B dodge (v170c): the Caestus kick behavior (judge 410, AtkParam 4200410): no HP damage
// (physical correction 0%), 700% stamina damage and a knockback, set small for each hit.
constexpr int kDodge = 104200410;

inline volatile LONG g_enabled = 1;  // combat=off disables
inline volatile LONG g_queued = 0, g_applied = 0, g_lastResult = 0;

inline uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

inline bool ready() {
    static int state = 0;
    if (state) return state > 0;
    static const uint8_t expected[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                       0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40};
    const uintptr_t fn = base() + kDamageFnRva;
    state = worldobs::readable(fn, sizeof(expected)) && memcmp(reinterpret_cast<void *>(fn), expected, sizeof(expected)) == 0
                ? 1
                : -1;
    logWrite("mario combat: native damage %s at %p", state > 0 ? "ready" : "signature mismatch (off)",
             reinterpret_cast<void *>(fn));
    return state > 0;
}

inline bool readPtr(uintptr_t at, uintptr_t &out) {
    if (!at || !worldobs::readable(at, 8)) return false;
    out = *reinterpret_cast<const uintptr_t *>(at);
    return out != 0;
}

struct Target { uintptr_t chr; float pos[3]; int hp; };

// Every character in WorldChrMan's tables except the player.
template <typename F> inline void forEachCharacter(F &&f) {
    uintptr_t wcm = 0;
    if (!readPtr(base() + kWorldChrManRva, wcm) || !worldobs::readable(wcm, 0x48)) return;
    const uintptr_t player = g_livePlayerIns;
    auto visit = [&](uintptr_t countAt, uintptr_t entriesAt) {
        if (!worldobs::readable(countAt, 4)) return;
        const int n = *reinterpret_cast<const int *>(countAt);
        uintptr_t entries = 0;
        if (n <= 0 || n > 4096 || !readPtr(entriesAt, entries) || !worldobs::readable(entries, size_t(n) * 0x38)) return;
        for (int i = 0; i < n; ++i) {
            const uintptr_t chr = *reinterpret_cast<const uintptr_t *>(entries + size_t(i) * 0x38);
            if (!chr || chr == player || !worldobs::readable(chr + 0x3E8, 8)) continue;
            uintptr_t map = 0, pos = 0;
            if (!readPtr(chr + 0x68, map) || !readPtr(map + 0x28, pos) || !worldobs::readable(pos + 0x10, 12)) continue;
            Target t;
            t.chr = chr;
            memcpy(t.pos, reinterpret_cast<const void *>(pos + 0x10), 12);
            t.hp = *reinterpret_cast<const int *>(chr + 0x3E8);
            if (!std::isfinite(t.pos[0]) || !std::isfinite(t.pos[1]) || !std::isfinite(t.pos[2])) continue;
            f(t);
        }
    };
    const int groups = *reinterpret_cast<const int *>(wcm + 0x28);
    uintptr_t groupArray = 0;
    if (groups > 0 && groups <= 64 && readPtr(wcm + 0x30, groupArray))
        for (int g = 0; g < groups; ++g) {
            const uintptr_t block = groupArray + size_t(g) * 0xE8 + 0x48;
            visit(block, block + 8);
        }
    visit(wcm + 0x38, wcm + 0x40);
}

// ---- Queue: pose thread -> game thread ----
struct Hit { uintptr_t target; int behavior; float point[4]; float knockback; bool object = false; };

// ---- Breakable map objects (v168, PTDE object_combat.h) ----
// ObjIns (vtable RVA 0x13186F8) records are 0x120 bytes: world position at +0x8C/+0x9C/+0xAC
// (the translation column of its matrix at +0x80), model pointer at +0x60 (cleared when the
// object breaks).  PTDE hands the object to the same native damage builder as a character;
// Mario's punches, kicks and ground pound do so (not the automatic stomp).  objhit=off.
inline volatile LONG g_objHitOn = 1;
inline volatile LONG g_dodgeUntil = 0;    // set when Mario's B dodge starts (pose thread)
inline volatile LONG g_dodgeHitOn = 1;    // dodgehit=off: the dodge breaks nothing and pushes no one
inline volatile LONG g_dodgePushCm = 30;  // dodgepush=<cm>: the dodge bump's knockback distance
inline volatile LONG g_kickReachCm = 90;
inline volatile LONG g_poundReachCm = 50;  // poundreach=<cm>  // kickreach=<cm>: heavy kicks hit within this of the body
// World position (v171): the matrix inside ObjIns, translation at +0x8C/+0x9C/+0xAC, is the
// object's MSB placement exactly (o1150_01 in Undead Burg: -59.07, -22.85, -25.41).
inline bool objectAlive(uintptr_t obj, float pos[3]) {
    uint8_t b[0x120];
    if (!objprobe::rd(obj, b, sizeof(b))) return false;
    uintptr_t vt = 0, model = 0;
    memcpy(&vt, b, 8);
    memcpy(&model, b + 0x60, 8);
    // (v170: no model test - +0x60 was set on only 56 of 783 objects, none of them breakable;
    // the breakable table match decides, and the native damage call decides whether it breaks.)
    (void)model;
    if (vt != base() + objprobe::kObjInsVtRva) return false;
    memcpy(&pos[0], b + 0x8C, 4);
    memcpy(&pos[1], b + 0x9C, 4);
    memcpy(&pos[2], b + 0xAC, 4);
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

// Breakable objects from the map files (prepare_breakables.py: MSB object parts whose
// ObjectParam hp > 0), every map, loaded once from the collision cache folder.
struct Breakable { float pos[3]; int model; int16_t hp; };
inline std::vector<Breakable> g_breakables;
inline void loadBreakables() {
    static bool tried = false;
    if (tried) return;
    // v171: the first-launch builder writes the tables; wait for it.
    if (install::g_buildState == 1) return;
    tried = true;
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    wcscat_s(dir, MAX_PATH, L"\\MarioSoulsProbe\\collision\\");
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls*.brk", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logWrite("mario combat: no breakable object tables (*.brk) in the collision folder");
        return;
    }
    int files = 0;
    do {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls%ls", dir, fd.cFileName);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char head[8];
        DWORD got = 0;
        if (ReadFile(f, head, 8, &got, nullptr) && got == 8 && memcmp(head, "MSB1", 4) == 0) {
            uint32_t n = 0;
            memcpy(&n, head + 4, 4);
            for (uint32_t i = 0; i < n && i < 100000; ++i) {
                uint8_t r[20];
                if (!ReadFile(f, r, 20, &got, nullptr) || got != 20) break;
                Breakable b{};
                memcpy(b.pos, r, 12);
                memcpy(&b.model, r + 12, 4);
                memcpy(&b.hp, r + 16, 2);
                g_breakables.push_back(b);
            }
            ++files;
        }
        CloseHandle(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    logWrite("mario combat: %zu breakable objects from %d map tables", g_breakables.size(), files);
}
inline const Breakable *breakableAt(const float p[3]) {
    for (const Breakable &b : g_breakables) {
        const float dx = b.pos[0] - p[0], dy = b.pos[1] - p[1], dz = b.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) return &b;
    }
    return nullptr;
}

// PTDE's push distances (metres): finishing kick 1.40, jump kick 1.15, slide
// kick 1.60; the dive and everything else pushes nothing.
inline float knockbackFor(int attackId) {
    switch (attackId) {
    case 2: return 1.40f;
    case 3: return 1.15f;
    case 5: return 1.60f;
    default: return 0.f;
    }
}

// Pushes write the row's distance first (game thread, right before the native
// damage call), so each hit carries its own distance.
inline void writeKnockback(int behavior, float metres) {
    volatile float *p = behavior == kHeavy ? mariosetup::g_kickRow[0]
                        : behavior == kSlideKick ? mariosetup::g_kickRow[1] : nullptr;
    if (!p || !worldobs::readable(reinterpret_cast<uintptr_t>(p), 4)) return;
    *p = metres;
}

// PTDE SCImpact::distance (v159): the enhanced push only at a possible poise break, scaled to
// zero at 80 maximum poise, never on large targets (radius over 0.80 m or height over 2.80 m).
// The target's NpcParam row (found by its pointer in ChrIns) gives size and maximum poise; its
// current poise is not read, so a break means the kick's poise damage exceeds the maximum.
inline uintptr_t npcRow(uintptr_t chr) {
    static uintptr_t who[32], row[32];
    static int next = 0;
    for (int i = 0; i < 32; ++i) if (who[i] == chr) return row[i];
    uintptr_t found = 0;
    if (mariosetup::g_npcFirst && worldobs::readable(chr, 0x1000))
        for (size_t o = 0; o < 0x1000 && !found; o += 8) {
            const uintptr_t v = *reinterpret_cast<const uintptr_t *>(chr + o);
            if (mariosetup::g_npcRows.contains(v)) found = v;
        }
    who[next] = chr; row[next] = found; next = (next + 1) % 32;
    return found;
}
inline volatile LONG g_pushRules = 1;
inline volatile LONG g_kickReachPct = 70;  // kickrange=<percent> of the heavy kicks' reach (v165)  // pushrules=off: every kick pushes (v158 behaviour)
inline float pushScale(uintptr_t chr, int behavior) {
    if (!g_pushRules) return 1.f;
    const uintptr_t row = npcRow(chr);
    if (!row || !worldobs::readable(row, mariosetup::kNpcRowSize)) {
        static DWORD lastNoRow = 0;
        if (GetTickCount() - lastNoRow > 10000) {
            lastNoRow = GetTickCount();
            logWrite("mario combat: no NpcParam row for %p; kick pushes as before", reinterpret_cast<void *>(chr));
        }
        return 1.f;
    }
    float height = 0, radius = 0;
    int16_t maxPoise = 0;
    memcpy(&height, reinterpret_cast<const void *>(row + 0x10), 4);
    memcpy(&radius, reinterpret_cast<const void *>(row + 0x14), 4);
    memcpy(&maxPoise, reinterpret_cast<const void *>(row + 0x118), 2);
    const int dmg = mariosetup::kickPoiseDamage(behavior == kSlideKick ? 1 : 0);
    float scale = 0.f;
    const char *why = "";
    if (!std::isfinite(radius) || !std::isfinite(height) || radius > .8f || height > 2.8f) why = "too large";
    else if (maxPoise >= 80) why = "poise 80 or more";
    else if (dmg > 0 && dmg <= maxPoise) why = "no poise break";
    else scale = 1.f - maxPoise / 80.f;
    logWrite("mario combat: push on %p: radius %.2f height %.2f max poise %d kick poise %d -> x%.2f %s",
             reinterpret_cast<void *>(chr), radius, height, int(maxPoise), dmg, scale, why);
    return scale;
}

// PTDE: one enhanced push per enemy every 1.2 seconds.
inline bool pushReady(uintptr_t chr, DWORD now) {
    static uintptr_t who[32];
    static DWORD when[32];
    static int next = 0;
    for (int i = 0; i < 32; ++i)
        if (who[i] == chr) {
            if (DWORD(now - when[i]) < 1200) return false;
            when[i] = now;
            return true;
        }
    who[next] = chr;
    when[next] = now;
    next = (next + 1) % 32;
    return true;
}
// Characters the game does not let us damage (script-inactive like the
// Asylum pit's Stray Demon, friendly or invincible): after 3 hits with no HP
// change they are left alone (a crash followed repeated hits on one, v114).
inline uintptr_t g_immune[32] = {};
inline int g_immuneCount = 0;
inline bool immune(uintptr_t chr) {
    for (int i = 0; i < g_immuneCount; ++i) if (g_immune[i] == chr) return true;
    return false;
}
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline Hit g_queue[16];
inline int g_count = 0;

inline void queue(const Hit &h) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_count < 16) g_queue[g_count++] = h;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_queued);
}

inline void clearQueue() {
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_immuneCount = 0;
}
inline void poiseStart(uintptr_t chr, int hit, int hp);  // poisediag, defined below
inline void impactStart(uintptr_t chr, int hp, const float *at);  // hit feel, defined below

// Game thread (the game's XInput poll).  Each target is re-found in the table
// first: a character unloaded since the pose thread saw it is skipped.
inline void flush() {
    if (!g_count) return;
    Hit local[16];
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    n = g_count;
    memcpy(local, g_queue, sizeof(Hit) * size_t(n));
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
    const uintptr_t player = g_livePlayerIns;
    uintptr_t manager = 0;
    if (!player || !ready() || !readPtr(base() + kDamageManRva, manager)) return;
    auto damage = reinterpret_cast<DamageFn>(base() + kDamageFnRva);
    for (int i = 0; i < n; ++i) {
        if (local[i].behavior == kDodge) {
            // Dodge bump: the kick attack with its knockback set for this hit, then restored.
            bool alive = false;
            forEachCharacter([&](const Target &t) { if (t.chr == local[i].target && t.hp > 0) alive = true; });
            if (!alive) continue;
            volatile float *kb = mariosetup::g_kickRow[2];
            const bool kbOk = kb && worldobs::readable(reinterpret_cast<uintptr_t>(kb), 4);
            const float saved = kbOk ? *kb : 0.f;
            if (kbOk) *kb = local[i].knockback;
            alignas(16) float point[4] = {local[i].point[0], local[i].point[1], local[i].point[2], 0.f};
            const int r = damage(reinterpret_cast<void *>(manager), kDodge, reinterpret_cast<void *>(player),
                                 reinterpret_cast<void *>(local[i].target), point, -1, nullptr);
            if (kbOk) *kb = saved;
            logWrite("mario combat: dodge bump on %p -> %d (knockback %.2f m%s)", reinterpret_cast<void *>(local[i].target), r,
                     local[i].knockback, kbOk ? "" : ", row not found: native distance");
            continue;
        }
        if (local[i].object) {
            float op[3];
            if (!g_objHitOn || !objectAlive(local[i].target, op)) continue;
            {
                // Layout search (v170): the record before each hit (the next hit's dump shows what
                // the previous one changed: HP and the broken state).  First 8 hits.
                static int dumps = 0;
                if (dumps < 8) {
                    ++dumps;
                    uint8_t ob[0x180];
                    if (objprobe::rd(local[i].target, ob, sizeof(ob))) {
                        logWrite("mario combat: object %p record before hit %d:", reinterpret_cast<void *>(local[i].target), dumps);
                        for (size_t r = 0; r < sizeof(ob); r += 32) {
                            char line[160];
                            int ln = snprintf(line, sizeof(line), "mario combat:   +%03zX:", r);
                            for (size_t k = 0; k < 32; k += 4) {
                                uint32_t v;
                                memcpy(&v, ob + r + k, 4);
                                ln += snprintf(line + ln, sizeof(line) - ln, " %08X", v);
                            }
                            logWrite("%s", line);
                        }
                    }
                }
            }
            alignas(16) float opoint[4] = {local[i].point[0], local[i].point[1], local[i].point[2], 1.f};
            const int r = damage(reinterpret_cast<void *>(manager), local[i].behavior, reinterpret_cast<void *>(player),
                                 reinterpret_cast<void *>(local[i].target), opoint, -1, nullptr);
            logWrite("mario combat: object %p hit (behavior %d) -> %d", reinterpret_cast<void *>(local[i].target),
                     local[i].behavior, r);
            continue;
        }
        bool alive = false;
        forEachCharacter([&](const Target &t) { if (t.chr == local[i].target && t.hp > 0) alive = true; });
        if (!alive) continue;
        // HP before this hit; compared on the next hit on the same target.
        static uintptr_t lastTarget = 0;
        static int lastHp = 0, unchanged = 0;
        int hpNow = 0;
        forEachCharacter([&](const Target &t) { if (t.chr == local[i].target) hpNow = t.hp; });
        if (lastTarget == local[i].target) {
            unchanged = hpNow >= lastHp ? unchanged + 1 : 0;
            if (unchanged >= 3 && g_immuneCount < 32) {
                g_immune[g_immuneCount++] = local[i].target;
                logWrite("mario combat: %p takes no damage; leaving it alone", reinterpret_cast<void *>(local[i].target));
                continue;
            }
        } else {
            unchanged = 0;
        }
        lastTarget = local[i].target;
        lastHp = hpNow;
        alignas(16) float point[4] = {local[i].point[0], local[i].point[1], local[i].point[2], 0.f};
        float push = local[i].knockback;
        if (push > 0) push *= pushScale(local[i].target, local[i].behavior);
        if (push > 0 && !pushReady(local[i].target, GetTickCount())) push = 0;
        writeKnockback(local[i].behavior, push);
        poiseStart(local[i].target, 0, hpNow);
        impactStart(local[i].target, hpNow, local[i].point);
        const int r = damage(reinterpret_cast<void *>(manager), local[i].behavior, reinterpret_cast<void *>(player),
                             reinterpret_cast<void *>(local[i].target), point, -1, nullptr);
        g_lastResult = r;
        InterlockedIncrement(&g_applied);
    }
}

// ---- Diagnostic (poisediag=on): which enemy fields change on a Mario hit ----
// Snapshots the target's first 2 KB just before the hit and 350 ms after it,
// then logs each float that moved.  The enemy's poise is one of them.
inline volatile LONG g_poiseDiag = 0;
struct PoiseSnap {
    bool open = false, late = false;
    uintptr_t chr = 0;
    DWORD when = 0;
    int hit = 0, hpBefore = 0;
    uint32_t before[512], after[512], later[512];
};
inline PoiseSnap g_poiseSnaps[8];
inline int g_poiseHits = 0;

// Logs the floats that differ between two captures (the fields that moved).
inline void poiseDiff(const char *label, const uint32_t *from, const uint32_t *to) {
    int lines = 0;
    for (int w = 0; w < 512 && lines < 40; ++w) {
        float a, b;
        memcpy(&a, &from[w], 4);
        memcpy(&b, &to[w], 4);
        if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(a) > 5000.f || std::fabs(b) > 5000.f) continue;
        if (std::fabs(a - b) < 0.01f) continue;
        logWrite("poisediag:   %s +0x%03X %.3f -> %.3f", label, w * 4, a, b);
        ++lines;
    }
}

// Two captures per hit: 350 ms after (the immediate reaction) and 3 s after
// (recovery: a poise field climbs back to where it was before the hit).
// ---- Hit feel (PTDE feedback, rumble part): when an enemy's HP falls after
// Mario's hit, Mario's pad rumbles; a heavy drop (15% or more) rumbles harder.
// Confirmed by the enemy's HP 150 ms after the hit, as PTDE confirms hits.
using XInputSetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_VIBRATION *);
// ---- Impact sound (PTDE feedback): the generated thump is mixed into Mario's own
// audio stream (mixImpact, called by pumpAudio), so no second output device.
inline const unsigned kImpactRate = 32000, kImpactFrames = 12800;  // 0.4 s
inline int16_t g_impactPcm[kImpactFrames * 2] = {};
inline volatile LONG g_impactPos = -1;  // next frame to mix, -1 when silent
inline void playImpact(float strength) {
    InterlockedExchange(&g_impactPos, -1);  // stop before rewriting the buffer
    SCFeedback::sound(g_impactPcm, kImpactFrames, kImpactRate, strength, GetTickCount(), 0.6f);
    InterlockedExchange(&g_impactPos, 0);
}
inline void mixImpact(int16_t *out, unsigned frames) {
    LONG pos = g_impactPos;
    if (pos < 0) return;
    unsigned i = 0;
    for (; i < frames && LONG(pos + i) < LONG(kImpactFrames); ++i)
        for (int c = 0; c < 2; ++c) {
            const int v = out[i * 2 + c] + g_impactPcm[(pos + i) * 2 + c];
            out[i * 2 + c] = int16_t(std::clamp(v, -32767, 32767));
        }
    pos += LONG(i);
    if (pos >= LONG(kImpactFrames)) pos = -1;
    InterlockedExchange(&g_impactPos, pos);
}

inline XInputSetStateFn g_setVibration = nullptr;
inline bool vibrationReady() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) g_setVibration = reinterpret_cast<XInputSetStateFn>(GetProcAddress(m, "XInputSetState"));
    }
    return g_setVibration != nullptr;
}
inline DWORD g_rumbleUntil = 0;
inline void rumble(WORD left, WORD right, DWORD ms) {
    if (!vibrationReady()) return;
    XINPUT_VIBRATION v = {left, right};
    g_setVibration(0, &v);
    g_rumbleUntil = GetTickCount() + ms;
}
struct Impact {
    uintptr_t chr;
    int hpBefore;
    DWORD when;
    bool open;
    float at[3];
};
// PTDE feedback flash (v159): a short star at the confirmed hit, drawn by copyFrame.
struct Flash { float at[3]; DWORD born; float strength; };
inline Flash g_flash = {};
inline volatile LONG g_flashOn = 1;  // hitflash=off disables
inline Impact g_impacts[8];
// PTDE hit stop: Mario's simulation holds its last pose until this tick count.
inline DWORD g_hitStopUntil = 0;
inline void impactStart(uintptr_t chr, int hp, const float *at) {
    for (Impact &i : g_impacts)
        if (!i.open) {
            i.chr = chr;
            i.hpBefore = hp;
            i.when = GetTickCount();
            i.open = true;
            memcpy(i.at, at, 12);
            return;
        }
}
inline void impactCheck() {
    if (g_rumbleUntil && int32_t(GetTickCount() - g_rumbleUntil) >= 0) {
        g_rumbleUntil = 0;
        if (vibrationReady()) {
            XINPUT_VIBRATION off = {0, 0};
            g_setVibration(0, &off);
        }
    }
    for (Impact &i : g_impacts) {
        if (!i.open || GetTickCount() - i.when < 70) continue;
        i.open = false;
        if (!worldobs::readable(i.chr, 0x800)) continue;
        int hpAfter = 0;
        memcpy(&hpAfter, reinterpret_cast<const void *>(i.chr + 0x3E8), 4);
        if (hpAfter < i.hpBefore) {
            const bool heavy = (i.hpBefore - hpAfter) * 100 >= i.hpBefore * 15;
            if (heavy) rumble(30000, 20000, 180);
            else rumble(14000, 9000, 100);
            playImpact(heavy ? 1.f : .5f);
            g_hitStopUntil = GetTickCount() + (heavy ? 67 : 34);  // PTDE stopMs 34..67
            if (g_flashOn) {
                Flash f;
                memcpy(f.at, i.at, 12);
                f.born = GetTickCount() | 1;
                f.strength = heavy ? 1.f : 0.f;
                g_flash = f;
            }
        }
    }
}

inline void poiseCheck() {
    if (!g_poiseDiag) return;
    for (PoiseSnap &s : g_poiseSnaps) {
        if (!s.open) continue;
        const DWORD age = GetTickCount() - s.when;
        if (!s.late && age >= 350) {
            if (!worldobs::readable(s.chr, 0x800)) { logWrite("poisediag: hit %d: target gone", s.hit); s.open = false; continue; }
            memcpy(s.after, reinterpret_cast<const void *>(s.chr), sizeof(s.after));
            int hpAfter = 0;
            memcpy(&hpAfter, reinterpret_cast<const void *>(s.chr + 0x3E8), 4);
            logWrite("poisediag: hit %d on %p, HP %d -> %d (350 ms)", s.hit, reinterpret_cast<void *>(s.chr),
                     s.hpBefore, hpAfter);
            poiseDiff("hit", s.before, s.after);
            s.late = true;
        } else if (s.late && age >= 3000) {
            s.open = false;
            if (!worldobs::readable(s.chr, 0x800)) { logWrite("poisediag: hit %d: target gone at 3 s", s.hit); continue; }
            memcpy(s.later, reinterpret_cast<const void *>(s.chr), sizeof(s.later));
            int hpLater = 0;
            memcpy(&hpLater, reinterpret_cast<const void *>(s.chr + 0x3E8), 4);
            logWrite("poisediag: hit %d at 3 s, HP %d", s.hit, hpLater);
            poiseDiff("recover", s.after, s.later);
            poiseDiff("vs-before", s.before, s.later);
        }
    }
}

inline void poiseStart(uintptr_t chr, int hit, int hp) {
    if (!g_poiseDiag || g_poiseHits >= 24) return;
    for (PoiseSnap &s : g_poiseSnaps) {
        if (s.open) continue;
        if (!worldobs::readable(chr, 0x800)) return;
        s.open = true;
        s.chr = chr;
        s.when = GetTickCount();
        s.hit = ++g_poiseHits;
        s.hpBefore = hp;
        memcpy(s.before, reinterpret_cast<const void *>(chr), sizeof(s.before));
        return;
    }
}

// ---- Pose thread: Mario's attack against everyone in reach ----
// attackId: the adapter's moves (Punch1 0, Punch2 1, Kick 2, JumpKick 3,
// Dive 4, SlideKick 5, Stomp 6, Pound 7).
inline int behaviorFor(int attackId) {
    switch (attackId) {
    case 0: return kPunch1;
    case 1: return kPunch2;
    case 2: case 3: return kHeavy;
    // Dive (4) is not a damaging move: the dodge roll uses it (v134).
    case 5: return kSlideKick;  // slide kick
    case 6: return kStomp;      // automatic stomp
    case 7: return kPlunge;
    default: return 0;
    }
}

struct Window {
    int attackId = -1;
    uint32_t serial = 0;
    uintptr_t hit[16] = {};
    int hits = 0;
};

// mario: world position of Mario's feet; toLocal maps a world point into the
// adapter's local centimetres.
template <typename ToLocal>
inline void tick(int32_t id, const SC_CharacterAPI *api, const SC_State &stateIn, const float mario[3], ToLocal toLocal) {
    poiseCheck();
    impactCheck();
    static Window w;
    // The B dodge (v170): breaks breakables and nudges small enemies aside, with no damage.
    const bool dodge = g_dodgeHitOn && static_cast<LONG>(GetTickCount() - static_cast<DWORD>(g_dodgeUntil)) < 0;
    SC_State state = stateIn;
    if (dodge) { state.attackId = 4; state.flags |= SC_ATTACK_ACTIVE; }
    const bool active = (state.flags & SC_ATTACK_ACTIVE) != 0;
    const int behavior = dodge ? kPunch1 : active ? behaviorFor(state.attackId) : 0;
    if (!behavior || state.attackId != w.attackId) {
        if (!behavior) { w.attackId = -1; w.hits = 0; return; }
        w.attackId = state.attackId;
        w.hits = 0;
        ++w.serial;
    }
    if (!g_enabled || !ready() || !api || !api->probeContact) return;
    // PTDE 17 (v159): a stomp once per airborne cycle and only with stamina left; a ground
    // pound hits three targets at most.
    static bool stompedThisAir = false;
    if (state.flags & SC_GROUNDED) stompedThisAir = false;
    if (state.attackId == 6) {
        if (stompedThisAir) return;
        int st = 1;
        if (g_livePlayerIns && worldobs::readable(g_livePlayerIns + 0x3F8, 4))
            memcpy(&st, reinterpret_cast<const void *>(g_livePlayerIns + 0x3F8), 4);
        if (st <= 0) return;
    }
    if (dodge) {
        // Small enemies only (as the knight's roll shoves a hollow): radius <= 0.6 m, height
        // <= 2.2 m, touched by Mario's body.  One native hit per dodge with the Caestus kick's
        // attack: stamina damage and a short knockback (dodgepush=<cm>), no HP damage.
        forEachCharacter([&](const Target &t) {
            if (t.hp <= 0 || t.hp > 1000000) return;
            float radius = 0.4f, tall = 1.7f;
            if (const uintptr_t row = npcRow(t.chr)) {
                float rr = 0, hh = 0;
                if (worldobs::readable(row + 0x10, 8)) {
                    memcpy(&hh, reinterpret_cast<const void *>(row + 0x10), 4);
                    memcpy(&rr, reinterpret_cast<const void *>(row + 0x14), 4);
                }
                if (std::isfinite(rr) && rr > 0.05f && rr < 6.f) radius = rr;
                if (std::isfinite(hh) && hh > 0.5f && hh < 12.f) tall = hh;
            }
            if (radius > 0.6f || tall > 2.2f) return;
            const float dx = t.pos[0] - mario[0], dz = t.pos[2] - mario[2], dy = t.pos[1] - mario[1];
            const float hd = std::sqrt(dx * dx + dz * dz);
            if (hd - radius > 0.35f || dy < -0.6f || dy > 1.2f) return;
            for (int i = 0; i < w.hits; ++i) if (w.hit[i] == t.chr) return;
            if (w.hits < 16) w.hit[w.hits++] = t.chr;
            Hit h = {t.chr, kDodge, {t.pos[0], t.pos[1] + 0.7f * tall / 1.7f, t.pos[2], 0.f}, g_dodgePushCm * 0.01f};
            queue(h);
            logWrite("mario combat: dodge bumps %p (radius %.2f, height %.2f)", reinterpret_cast<void *>(t.chr), radius, tall);
        });
    }
    if (state.attackId == 7 && w.hits >= 3) return;
    if (!dodge) forEachCharacter([&](const Target &t) {
        if (state.attackId == 7 && w.hits >= 3) return;
        if (t.hp <= 0 || t.hp > 1000000 || immune(t.chr)) return;
        const float dx = t.pos[0] - mario[0], dz = t.pos[2] - mario[2], dy = t.pos[1] - mario[1];
        if (dx * dx + dz * dz > 3.5f * 3.5f || dy < -2.5f || dy > 2.5f) return;
        // Heavy kicks (RT, jump and slide kick): only within kickreach of the target's body
        // (v168: hits landed 2-3.4 m from Mario).
        if (state.attackId == 2 || state.attackId == 3 || state.attackId == 5 || state.attackId == 7) {
            float radius = 0.4f;
            if (const uintptr_t row = npcRow(t.chr)) {
                float rr = 0;
                if (worldobs::readable(row + 0x14, 4)) memcpy(&rr, reinterpret_cast<const void *>(row + 0x14), 4);
                if (std::isfinite(rr) && rr > 0.05f && rr < 6.f) radius = rr;
            }
            // Ground pound: half its old reach (v172), poundreach=<cm>.
            const float cap = (state.attackId == 7 ? g_poundReachCm : g_kickReachCm) * 0.01f;
            if (std::sqrt(dx * dx + dz * dz) - radius > cap) return;
        }
        for (int i = 0; i < w.hits; ++i) if (w.hit[i] == t.chr) return;
        // Points on the body in height bands (PTDE 17 samples the target's shape, not its
        // feet): knee, chest and head height, or the top for a ground pound.  Taller targets
        // (NpcParam hitHeight) get their bands spread over their height.
        float tall = 1.7f;
        if (const uintptr_t row = npcRow(t.chr)) {
            float hh = 0;
            if (worldobs::readable(row + 0x10, 4)) memcpy(&hh, reinterpret_cast<const void *>(row + 0x10), 4);
            if (std::isfinite(hh) && hh > 0.5f && hh < 12.f) tall = hh;
        }
        if (dy < -2.5f - (tall - 1.7f) || dy > 2.5f) return;
        const float bands[3] = {0.3f * tall / 1.7f, 0.7f * tall / 1.7f, 1.3f * tall / 1.7f};
        float pt[3] = {t.pos[0], 0, t.pos[2]}, local[3];
        bool contactHit = false;
        for (int b = 0; b < (state.attackId == 7 ? 1 : 3) && !contactHit; ++b) {
            pt[1] = t.pos[1] + (state.attackId == 7 ? std::min(1.0f, tall * 0.6f) : bands[b]);
            // Heavy (RT) kicks reach 30% less (v165): the point is tested 1/0.7 times as far
            // from Mario, so only targets within 70% of the kick's reach are hit.
            float probe[3] = {pt[0], pt[1], pt[2]};
            if ((state.attackId == 2 || state.attackId == 3 || state.attackId == 5) && g_kickReachPct > 0 &&
                g_kickReachPct < 100) {
                const float s = 100.f / float(g_kickReachPct);
                probe[0] = mario[0] + (pt[0] - mario[0]) * s;
                probe[2] = mario[2] + (pt[2] - mario[2]) * s;
            }
            toLocal(probe, local);
            contactHit = (api->probeContact(id, local[0], local[1], local[2]) & SC_CONTACT_ATTACK) != 0;
        }
        if (!contactHit) return;
        if (state.attackId == 6) stompedThisAir = true;
        if (w.hits < 16) w.hit[w.hits++] = t.chr;
        Hit h = {t.chr, behavior, {pt[0], pt[1], pt[2], 0.f}, knockbackFor(state.attackId)};
        queue(h);
        if (w.hits == 1 && api->commitContact) api->commitContact(id, local[0], local[1], local[2], 160.f);
        logWrite("mario combat: %s hit %p (hp %d, %.1f m from centre)",
                 state.attackId == 0 ? "punch 1" : state.attackId == 1 ? "punch 2" : state.attackId == 7 ? "ground pound" : "kick",
                 reinterpret_cast<void *>(t.chr), t.hp, std::sqrt(dx * dx + dz * dz));
    });
    // Breakable objects in reach (not the automatic stomp, as PTDE).
    if (g_objHitOn && state.attackId != 6) {
        objprobe::g_scanWanted = 1;
        objprobe::start();
        std::vector<uintptr_t> objs;
        AcquireSRWLockShared(&objprobe::g_lock);
        objs = objprobe::g_objs;
        ReleaseSRWLockShared(&objprobe::g_lock);
        const float fx = std::sin(state.faceAngle), fz = std::cos(state.faceAngle);
        loadBreakables();
        // Measured to the object's origin (its base), so the reach allows for its size (v171:
        // 0.9 m from the centre missed Mario standing against a barrel).
        const float reach = state.attackId == 4 ? 1.1f : state.attackId == 7 ? 0.9f : (state.attackId == 0 || state.attackId == 1) ? 1.4f : 1.5f;
        float nearest = 1e9f, nearestDy = 0;
        uintptr_t nearestObj = 0;
        int alive = 0, breakableNear = 0;
        float anyNear = 1e9f, anyPos[3] = {};
        uintptr_t anyObj = 0;
        for (uintptr_t o : objs) {
            float op[3];
            if (!objectAlive(o, op)) continue;
            ++alive;
            const float dx = op[0] - mario[0], dz = op[2] - mario[2], dy = op[1] - mario[1];
            const float hd = std::sqrt(dx * dx + dz * dz);
            if (hd < anyNear) { anyNear = hd; anyObj = o; memcpy(anyPos, op, 12); }
            if (hd > 3.f) continue;
            if (!breakableAt(op)) continue;  // only the map's breakable objects
            ++breakableNear;
            if (hd < nearest) { nearest = hd; nearestDy = dy; nearestObj = o; }
            if (dx * dx + dz * dz > reach * reach || dy < -0.8f || dy > 1.8f) continue;
            if (state.attackId != 7 && dx * fx + dz * fz < -0.1f) continue;  // behind Mario
            bool done = false;
            for (int i = 0; i < w.hits; ++i) if (w.hit[i] == o) done = true;
            if (done) continue;
            if (w.hits < 16) w.hit[w.hits++] = o;
            Hit h = {o, behavior, {op[0], op[1] + 0.4f, op[2], 1.f}, 0.f, true};
            queue(h);
            logWrite("mario combat: object %p in reach (%.2f m)", reinterpret_cast<void *>(o), std::sqrt(dx * dx + dz * dz));
        }
        static uint32_t loggedSerial = 0;
        if (loggedSerial != w.serial) {
            loggedSerial = w.serial;
            // Nearest breakable from the map tables, whether or not a live object matched it.
            float tabNear = 1e9f, tabPos[3] = {};
            int tabModel = 0;
            for (const Breakable &b : g_breakables) {
                const float dx = b.pos[0] - mario[0], dz = b.pos[2] - mario[2];
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d < tabNear && std::fabs(b.pos[1] - mario[1]) < 3.f) { tabNear = d; memcpy(tabPos, b.pos, 12); tabModel = b.model; }
            }
            logWrite("mario combat: attack %d at (%.2f %.2f %.2f): %zu objects, %d intact, %d breakable matched within 3 m "
                     "(nearest %p %.2f m, dy %.2f); nearest object %p at %.2f m (%.2f %.2f %.2f); nearest table breakable o%04d at %.2f m "
                     "(%.2f %.2f %.2f)", state.attackId, mario[0], mario[1], mario[2], objs.size(), alive, breakableNear,
                     reinterpret_cast<void *>(nearestObj), breakableNear ? nearest : -1.f, nearestDy, reinterpret_cast<void *>(anyObj), anyNear, anyPos[0], anyPos[1], anyPos[2],
                     tabModel, tabNear, tabPos[0], tabPos[1], tabPos[2]);
        }
    }
}

} // namespace mariocombat
} // namespace remastered
