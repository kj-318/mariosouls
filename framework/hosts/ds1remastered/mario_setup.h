#pragma once

// Mario's character setup, the PTDE host's starting_class.h /
// unarmed_equipment.h way, for Remastered (v114).
//
// * New characters: the Pyromancer class (slot 7) is Mario.  Its starting
//   right weapon (CharaInitParam 3007 +0x10, Hand Axe 700000) and its tutorial
//   weapon pickup (ItemLotParam 1810260 +0, Hand Axe) become the Caestus
//   (901000), Mario's Gear.  Only those exact vanilla values are changed.
// * Text: class name "Pyromancer" (Menu_others 132027 / 132043) reads "Mario",
//   its description (132327) Mario's; the Caestus and Caestus+1..+15 (Weapon_name
//   901000-901015) read "Mario's Gear(+N)".  The game keeps its own entries;
//   only the string offsets of those entries are pointed at our strings.
// * The loaded character: givecaestus=on gives one Caestus once per session
//   with the game's own item pickup (DarkSoulsRemastered.exe 0x1407479E0, the
//   function DSR-Gadget's item spawner calls; inventory = [[0x141C8A530] +
//   0x10] + 0x280).
//
// In memory the params are the files as loaded (x64 PARAM: type name at
// +0x0C, row count u16 at +0x0A, 12-byte rows {id, data offset, name offset}
// from +0x30), and the FMGs are version 1 (u32 string offsets from the FMG
// start; groups {first index, first id, last id} from +0x1C), found by
// scanning committed memory once after start.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cwchar>
#include <vector>

#include "logging.h"
#include "world_observation.h"
#include "install.h"

namespace remastered {
namespace mariosetup {

constexpr int kCaestus = 901000, kHandAxe = 700000;
// Hidden Body (SpEffect 530, visuals SpEffectVfx row 8) on the weapons: off unless
// hiddenbody=on.  Read when the params are patched at load.
inline volatile LONG g_hiddenBody = 0;  // off: the weapon models hide the weapons (hiddenbody=on)
constexpr int kHiddenBodySp = 530, kHiddenBodyVfx = 8;
inline volatile LONG g_giveCaestus = 0;  // givecaestus=on
inline volatile LONG g_paramsPatched = 0, g_textEntries = 0, g_given = 0;
inline int g_pass = 0;
// Live addresses of the Caestus attack rows' knockback distance (0 heavy kick,
// 1 slide kick), set by each parameter pass.  mario_combat writes the per-hit
// distance through them (PTDE's attack packet distance).
inline volatile float *g_kickRow[3] = {nullptr, nullptr, nullptr};  // [2]: 4200410 (the dodge bump, v170c)
// Poise damage (AtkParam atkSuperArmor, +0x5E) of the heavy kick and slide kick rows.
inline int g_kickPoiseFlat[2] = {0, 0}, g_kickPoiseCorr[2] = {100, 100};
inline int g_caestusPoise = 20;  // EquipParamWeapon saWeaponDamage (+0xCE) of Mario's Gear
inline int kickPoiseDamage(int i) { return g_caestusPoise * g_kickPoiseCorr[i] / 100 + g_kickPoiseFlat[i]; }
// NpcParam rows in the game's memory (v159): an enemy's ChrIns points at its row, which
// gives its size (hitHeight +0x10, hitRadius +0x14) and maximum poise
// (superArmorDurability +0x118) for PTDE's enhanced-push rule.
inline volatile uintptr_t g_npcFirst = 0, g_npcLast = 0;  // first copy found (any copy: see rowRanges)
// Up to four copies of a param in memory (row data ranges); an object's pointer may refer to
// any of them.
struct RowRanges {
    uintptr_t lo[4] = {}, hi[4] = {}, stride = 0;
    int n = 0;
    bool add(uintptr_t l, uintptr_t h, uintptr_t st) {
        for (int k = 0; k < n; ++k) if (lo[k] == l) return false;
        if (n >= 4) return false;
        lo[n] = l; hi[n] = h; stride = st; ++n;
        return true;
    }
    bool contains(uintptr_t q) const {
        for (int k = 0; k < n; ++k)
            if (q >= lo[k] && q <= hi[k] && stride && (q - lo[k]) % stride == 0) return true;
        return false;
    }
};
inline RowRanges g_npcRows, g_spRows;
// SpEffectParam rows in the game's memory (v159): the player's active effect list points at
// them; stateInfo (+0x156) 142 is lava (4030/4031), 186 the Orange Charred Ring's protection.
inline volatile uintptr_t g_spFirst = 0, g_spLast = 0;
inline volatile uintptr_t g_spStride = 0;
inline volatile LONG g_poiseBonus = 1;  // poisebonus=off disables (read before the first scan)
inline bool g_poiseDone = false;
inline float g_camScale = 1.5f;  // camwide=off: 1.0 (read before the first scan)
inline bool g_camDone = false;
// Param copies already patched (by the real address of their type name): the overlapping
// scan chunks and later passes see the same blob again; each copy is changed once.
inline uintptr_t g_camBlobs[8] = {}, g_poiseBlobs[8] = {};
inline bool onceFor(uintptr_t (&list)[8], uintptr_t at) {
    for (uintptr_t v : list) if (v == at) return false;
    for (uintptr_t &v : list) if (!v) { v = at; return true; }
    return false;
}
constexpr uintptr_t kNpcRowSize = 0x154;

inline uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

inline bool copyMem(uintptr_t at, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, n, &got) && got == n;
}

// Each candidate region is first copied (ReadProcessMemory fails cleanly on
// memory freed meanwhile; direct reads crashed the game at start, v115); f
// gets the copy and the region's real address.
inline std::vector<uint8_t> g_regionCopy;
template <typename F> inline void forEachRegionCopy(F &&f) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t at = 0x10000;
    while (at < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD p = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && size <= (1024u << 20) &&
            (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE) &&
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            // 32 MB chunks overlapping by 8 MB (every FMG/param blob is
            // smaller): v117 skipped regions over 64 MB, where they live.
            constexpr size_t kChunk = 32u << 20, kStep = 24u << 20;
            for (size_t off = 0; off < size; off += kStep) {
                const size_t len = size - off < kChunk ? size - off : kChunk;
                g_regionCopy.resize(len);
                if (copyMem(start + off, g_regionCopy.data(), len)) f(start + off, len, g_regionCopy.data());
                if (off + len >= size) break;
            }
        }
        at = start + size;
        if (!size) break;
    }
}

// ---- Params ----
inline volatile int32_t *g_estusSfx[16] = {};
inline int32_t g_estusSfxOrig[16] = {};
inline volatile LONG g_estusHideOn = 1;  // estushide=off keeps the knight's flask effect
// The flask mesh itself (v170j): c0000's drink animations 7586/7587 spawn effect 22750 at
// dummy 26 directly (TAE event type 96, no variation), so the goods change leaves it.  Its
// event parameters (96, offset, 22750, 26, 0) are found in the loaded TAE once and set to
// 22799 (no such effect) while Mario is shown.
inline volatile int32_t *g_estusTae[16] = {};
inline int32_t g_estusTaeOrig[16] = {};
inline volatile LONG g_estusTaeN = 0, g_estusTaeScanning = 0;
inline DWORD WINAPI estusTaeScan(void *) {
    std::vector<uint8_t> buf;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t at = 0x10000;
    LONG n = 0;
    while (at < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && size <= (1024u << 20) &&
            (pr == PAGE_READWRITE || pr == PAGE_WRITECOPY) && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            constexpr size_t kChunk = 16u << 20;
            for (size_t off = 0; off < size; off += kChunk - 64) {
                const size_t len = size - off < kChunk ? size - off : kChunk;
                buf.resize(len);
                if (copyMem(start + off, buf.data(), len)) {
                    for (size_t i = 8; i + 16 <= len; i += 4) {
                        int32_t v[3], ty;
                        memcpy(v, buf.data() + i, 12);
                        // (v170k: not the word after - the 7587 event is followed by the next
                        // event's type, so only one of the two was found.)
                        if ((v[0] != 22750 && v[0] != 22799) || v[1] != 26 || v[2] != 0) continue;
                        memcpy(&ty, buf.data() + i - 8, 4);
                        if (ty != 96) continue;
                        const uintptr_t addr = start + off + i;
                        bool dup = false;
                        for (LONG k = 0; k < n; ++k) dup = dup || reinterpret_cast<uintptr_t>(g_estusTae[k]) == addr;
                        if (!dup && n < 16) { g_estusTaeOrig[n] = 22750; g_estusTae[n++] = reinterpret_cast<volatile int32_t *>(addr); }
                        // v170l: the same drink animations' variation events (type 114) for 227 (flask)
                        // and 229 (spill) lie just before/after these; 228 (the healing glow) stays.
                        const size_t lo = i > 0x240 ? i - 0x240 : 8, hi = i + 0x80 < len - 12 ? i + 0x80 : len - 12;
                        for (size_t j = lo; j <= hi; j += 4) {
                            int32_t ev[3];
                            memcpy(ev, buf.data() + j - 8, 12);
                            if (ev[0] != 114 || (ev[2] != 227 && ev[2] != 229 && ev[2] != 9999)) continue;
                            const uintptr_t a2 = start + off + j;
                            bool d2 = false;
                            for (LONG k = 0; k < n; ++k) d2 = d2 || reinterpret_cast<uintptr_t>(g_estusTae[k]) == a2;
                            if (!d2 && n < 16 && ev[2] != 9999) { g_estusTaeOrig[n] = ev[2]; g_estusTae[n++] = reinterpret_cast<volatile int32_t *>(a2); }
                        }
                    }
                }
                if (off + len >= size) break;
            }
        }
        at = start + size;
        if (!size) break;
    }
    InterlockedExchange(&g_estusTaeN, n);
    logWrite("mario setup: %ld Estus flask events (22750 at dummy 26, flask 227 and spill 229) in the loaded animation data", n);
    InterlockedExchange(&g_estusTaeScanning, 0);
    return 0;
}
// Game thread: the flask effect is replaced while Mario is shown (driving), restored otherwise.
inline void estusFlaskTick(bool marioShown) {
    const bool hide = marioShown && g_estusHideOn;
    {
        static DWORD lastScan = 0;
        if (hide && !g_estusTaeN && !g_estusTaeScanning && (!lastScan || GetTickCount() - lastScan > 30000)) {
            lastScan = GetTickCount();
            InterlockedExchange(&g_estusTaeScanning, 1);
            if (HANDLE h = CreateThread(nullptr, 0, estusTaeScan, nullptr, 0, nullptr)) CloseHandle(h);
            else InterlockedExchange(&g_estusTaeScanning, 0);
        }
        static int taeState = -1;
        static LONG taeDone = 0;
        if (g_estusTaeN && (taeState != (hide ? 1 : 0) || taeDone != g_estusTaeN)) {
            for (LONG k = 0; k < g_estusTaeN; ++k) {
                volatile int32_t *e = g_estusTae[k];
                if (!e || !worldobs::readable(reinterpret_cast<uintptr_t>(e), 4)) continue;
                const int32_t orig = g_estusTaeOrig[k], off = orig == 22750 ? 22799 : 9999;
                if (*e == orig || *e == off) *e = hide ? off : orig;
            }
            taeState = hide ? 1 : 0;
            taeDone = g_estusTaeN;
        }
    }
    // (v170l: the goods variation is no longer changed - the TAE events above take the flask and
    // spill, and the variation keeps the healing glow 22850.  Rows are restored once if a
    // previous version changed them in this session.)
    static int state = -1;
    if (state == 0) return;
    int done = 0;
    for (int k = 0; k < 16; ++k) {
        volatile int32_t *f = g_estusSfx[k];
        if (!f || !worldobs::readable(reinterpret_cast<uintptr_t>(f), 4)) continue;
        *f = g_estusSfxOrig[k];
        ++done;
    }
    if (!done) return;
    state = 0;
    (void)hide;
    logWrite("mario setup: Estus effect variation kept (%d rows; healing glow stays)", done);
}
inline uint8_t *paramRow(uintptr_t blob, size_t room, int id) {
    const uint16_t rows = *reinterpret_cast<const uint16_t *>(blob + 0x0A);
    if (!rows || 0x30 + size_t(rows) * 12 > room) return nullptr;
    for (uint16_t i = 0; i < rows; ++i) {
        const uint8_t *r = reinterpret_cast<const uint8_t *>(blob + 0x30 + size_t(i) * 12);
        int rid;
        uint32_t off;
        memcpy(&rid, r, 4);
        memcpy(&off, r + 4, 4);
        if (rid == id) return off < room ? reinterpret_cast<uint8_t *>(blob + off) : nullptr;
    }
    return nullptr;
}

// Params, Mario's Gear model and text in ONE scan of copied memory per pass
// (read from the copy, written to the game's own address).
inline void patchScanParams(uintptr_t start, size_t size, const uint8_t *p, int &patched) {
    static const char kInit[] = "CHARACTER_INIT_PARAM", kLot[] = "ITEMLOT_PARAM_ST", kWep[] = "EQUIP_PARAM_WEAPON_ST",
                      kAtk[] = "ATK_PARAM_ST", kSp[] = "SP_EFFECT_PARAM_ST", kVfx[] = "SP_EFFECT_VFX_PARAM_ST",
                      kNpc[] = "NPC_PARAM_ST", kProt[] = "EQUIP_PARAM_PROTECTOR_ST", kCam[] = "LOCK_CAM_PARAM_ST";
    const uintptr_t cbase = reinterpret_cast<uintptr_t>(p);
    auto real = [&](const uint8_t *q) { return start + (reinterpret_cast<uintptr_t>(q) - cbase); };
    for (size_t i = 0x0C; i + 32 < size; i += 4) {
        const bool init = p[i] == 'C' && memcmp(p + i, kInit, sizeof(kInit)) == 0;
        const bool lot = !init && p[i] == 'I' && memcmp(p + i, kLot, sizeof(kLot)) == 0;
        const bool wep = !init && !lot && p[i] == 'E' && memcmp(p + i, kWep, sizeof(kWep)) == 0;
        const bool atk = !init && !lot && !wep && p[i] == 'A' && memcmp(p + i, kAtk, sizeof(kAtk)) == 0;
        // PTDE 20 (v159): +20 poise.  Poise is the sum of the four armour slots'
        // saDurability (+0xAE), and an empty slot holds its bare row (900000-903000), so +5 on
        // every protector row is +20 whatever is worn.  Once per process.
        // PTDE 09 (v159): a wider camera, 1.5x the distance of every LockCamParam row
        // (camDistTarget, +0x00; normal and lock-on cameras).  Once per process.
        if (g_camScale > 1.f && p[i] == 'L' && memcmp(p + i, kCam, sizeof(kCam)) == 0 &&
            onceFor(g_camBlobs, real(p + i))) {
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            const uint16_t rows = *reinterpret_cast<const uint16_t *>(blob + 0x0A);
            if (rows && 0x30 + size_t(rows) * 12 <= room) {
                int done = 0;
                for (uint16_t r = 0; r < rows; ++r) {
                    uint32_t off;
                    memcpy(&off, reinterpret_cast<const void *>(blob + 0x30 + size_t(r) * 12 + 4), 4);
                    if (off + 4 > room) continue;
                    volatile float *dist = reinterpret_cast<volatile float *>(real(reinterpret_cast<const uint8_t *>(blob + off)));
                    const float v = *dist;
                    if (std::isfinite(v) && v > 0.5f && v < 30.f) { *dist = v * g_camScale; ++done; }
                }
                g_camDone = true;
                logWrite("mario setup: camera distance x%.2f on %d LockCamParam rows", double(g_camScale), done);
            }
            continue;
        }
        if (p[i] == 'E' && memcmp(p + i, "EQUIP_PARAM_GOODS_ST", 21) == 0) {
            // The knight's Estus flask (v170h, PTDE effects.h): the drink spawns effect 227 (the
            // flask mesh at the hand dummy) and 229 (the spill) with the goods row's
            // sfxVariationId (+0x04): 50 full -> 22750/22950, 49 empty -> 22749.  228 is the
            // healing glow.  estusFlaskTick swaps the variation while Mario is shown: 43 for a
            // full flask (22843 heal exists, no 22743/22943), 99 for an empty one (none exist).
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            int found = 0;
            for (int id = 200; id <= 215; ++id) {
                const uint8_t *row = paramRow(blob, room, id);
                if (!row) continue;
                int32_t v;
                memcpy(&v, row + 4, 4);
                if (v != 49 && v != 50) continue;
                g_estusSfx[id - 200] = reinterpret_cast<volatile int32_t *>(real(row + 4));
                g_estusSfxOrig[id - 200] = v;
                ++found;
            }
            static bool logged = false;
            if (found && !logged) { logged = true; logWrite("mario setup: %d Estus rows (flask effect variation)", found); }
            continue;
        }
        if (g_poiseBonus && p[i] == 'E' && memcmp(p + i, kProt, sizeof(kProt)) == 0 &&
            onceFor(g_poiseBlobs, real(p + i))) {
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            const uint16_t rows = *reinterpret_cast<const uint16_t *>(blob + 0x0A);
            if (rows && 0x30 + size_t(rows) * 12 <= room) {
                int done = 0;
                for (uint16_t r = 0; r < rows; ++r) {
                    uint32_t off;
                    memcpy(&off, reinterpret_cast<const void *>(blob + 0x30 + size_t(r) * 12 + 4), 4);
                    if (off + 0xB0 > room) continue;
                    volatile int16_t *sa = reinterpret_cast<volatile int16_t *>(real(reinterpret_cast<const uint8_t *>(blob + off + 0xAE)));
                    *sa = static_cast<int16_t>(*sa + 5);
                    ++done;
                }
                g_poiseDone = true;
                logWrite("mario setup: +5 poise on %d armour rows (+20 for the four slots)", done);
            }
            continue;
        }
        if (p[i] == 'N' && memcmp(p + i, kNpc, sizeof(kNpc)) == 0) {
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            const uint16_t rows = *reinterpret_cast<const uint16_t *>(blob + 0x0A);
            if (rows > 1 && 0x30 + size_t(rows) * 12 <= room) {
                // Row size from the offsets (0x150 in memory; the paramdef says 0x154, v160).
                uint32_t lo = ~0u, hi = 0, lo2 = ~0u;
                for (uint16_t r = 0; r < rows; ++r) {
                    uint32_t off;
                    memcpy(&off, reinterpret_cast<const void *>(blob + 0x30 + size_t(r) * 12 + 4), 4);
                    if (off < lo) { lo2 = lo; lo = off; } else if (off < lo2) lo2 = off;
                    if (off > hi) hi = off;
                }
                const uint32_t stride = lo2 - lo;
                if (stride >= 0x120 && stride <= 0x200 && hi + stride <= room && (hi - lo) % stride == 0) {
                    const uintptr_t l = real(reinterpret_cast<const uint8_t *>(blob + lo)),
                                    h = real(reinterpret_cast<const uint8_t *>(blob + hi));
                    if (g_npcRows.add(l, h, stride)) {
                        if (!g_npcFirst) { g_npcFirst = l; g_npcLast = h; }
                        logWrite("mario setup: NpcParam %u rows at %p (enemy size and poise)", unsigned(rows),
                                 reinterpret_cast<void *>(l));
                    }
                }
            }
            continue;
        }
        if (p[i] == 'S' && memcmp(p + i, kSp, sizeof(kSp)) == 0) {
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            const uint16_t rows = *reinterpret_cast<const uint16_t *>(blob + 0x0A);
            if (rows > 1 && 0x30 + size_t(rows) * 12 <= room) {
                uint32_t lo = ~0u, hi = 0, lo2 = ~0u;
                for (uint16_t r = 0; r < rows; ++r) {
                    uint32_t off;
                    memcpy(&off, reinterpret_cast<const void *>(blob + 0x30 + size_t(r) * 12 + 4), 4);
                    if (off < lo) { lo2 = lo; lo = off; } else if (off < lo2) lo2 = off;
                    if (off > hi) hi = off;
                }
                const uint32_t stride = lo2 - lo;
                if (stride >= 0x100 && stride <= 0x400 && hi + stride <= room && (hi - lo) % stride == 0) {
                    const uintptr_t l = real(reinterpret_cast<const uint8_t *>(blob + lo)),
                                    h = real(reinterpret_cast<const uint8_t *>(blob + hi));
                    if (g_spRows.add(l, h, stride)) {
                        if (!g_spFirst) { g_spStride = stride; g_spFirst = l; g_spLast = h; }
                        logWrite("mario setup: SpEffectParam %u rows at %p, stride 0x%X (lava and ring effects)", unsigned(rows),
                                 reinterpret_cast<void *>(l), stride);
                    }
                }
            }
        }
        const bool sp = g_hiddenBody && p[i] == 'S' && memcmp(p + i, kSp, sizeof(kSp)) == 0;
        const bool vfx = g_hiddenBody && !sp && p[i] == 'S' && memcmp(p + i, kVfx, sizeof(kVfx)) == 0;
        if (sp || vfx) {
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            const int rid = sp ? kHiddenBodySp : kHiddenBodyVfx;
            const uint8_t *row = paramRow(blob, room, rid);
            if (!row || room < size_t(row - reinterpret_cast<const uint8_t *>(blob)) + 0x40) continue;
            if (sp) {
                // Enemies still see and hear the player (Hidden Body cut their sight by 90%).
                volatile int32_t *sight = reinterpret_cast<volatile int32_t *>(real(row + 0xFC));
                volatile int32_t *hearing = reinterpret_cast<volatile int32_t *>(real(row + 0x100));
                // Permanent: as a weapon's resident effect it still ran out after 30 s.
                volatile float *endurance = reinterpret_cast<volatile float *>(real(row + 0x08));
                if (*endurance != -1.f) {
                    logWrite("mario setup: Hidden Body (SpEffect %d) duration %.0f s -> permanent", rid, double(*endurance));
                    *endurance = -1.f;
                }
                if (*sight != 0 || *hearing != 0) {
                    logWrite("mario setup: Hidden Body (SpEffect %d) enemy sight cut %d -> 0, hearing cut %d -> 0",
                             rid, int(*sight), int(*hearing));
                    *sight = 0;
                    *hearing = 0;
                }
            } else {
                // The fade is measured from the local player, so the player's own character sits at
                // distance 0: below the old 12-17 m band it was never camouflaged.  Start at 0 m and
                // end below zero, so distance 0 is already fully camouflaged.
                // (A negative end turned camouflage off entirely; 0.1/0 hid the weapon.)
                constexpr float kFadeBegin = 0.1f, kFadeEnd = 0.f;
                // Full camouflage at any distance (no half-visible shimmer, no fade band), weapons and
                // effects (the flame) hidden too, also for the player's own view ("friend").
                volatile int32_t *midst = reinterpret_cast<volatile int32_t *>(real(row + 0x00));
                volatile float *begin = reinterpret_cast<volatile float *>(real(row + 0x18));
                volatile float *end = reinterpret_cast<volatile float *>(real(row + 0x1C));
                volatile uint8_t *f2e = reinterpret_cast<volatile uint8_t *>(real(row + 0x2E));
                volatile uint8_t *f2f = reinterpret_cast<volatile uint8_t *>(real(row + 0x2F));
                // Half camouflage off: with it on, the player's own body goes translucent, it leaves the
                // opaque pass, and Mario (anchored on the knight's bones there) stops drawing.  Off,
                // the body stays solid and only the weapons are hidden (the body hide does the rest).
                const uint8_t want2e = uint8_t((*f2e | 0x04 | 0x08 | 0x10) & ~0x40);
                const uint8_t want2f = uint8_t(*f2f | 0x01);
                if (*midst != -1 || *begin != kFadeBegin || *end != kFadeEnd || *f2e != want2e || *f2f != want2f) {
                    logWrite("mario setup: Hidden Body visuals (SpEffectVfx %d): shimmer %d -> none, fade %.1f/%.1f m -> %.1f/%.1f m, "
                             "flags %02X %02X -> %02X %02X (full camouflage, hidden weapons and effects)",
                             rid, int(*midst), double(*begin), double(*end), double(kFadeBegin), double(kFadeEnd),
                             unsigned(*f2e), unsigned(*f2f),
                             unsigned(want2e), unsigned(want2f));
                    *midst = -1;
                    *begin = kFadeBegin;
                    *end = kFadeEnd;
                    *f2e = want2e;
                    *f2f = want2f;
                }
            }
            continue;
        }
        if (atk) {
            // Caestus attacks' knockback (PTDE, its attack packet's distance at
            // +0x10): the heavy kick (4200100) 1.40 m, the slide kick (4200110)
            // 1.60 m.  Only the Caestus behaviors reference these rows.
            const uintptr_t blob = cbase + i - 0x0C;
            const size_t room = cbase + size - blob;
            struct KnockRow { int id; float metres; };
            static const KnockRow kRows[] = {{4200100, 1.40f}, {4200110, 1.60f}};
            if (const uint8_t *drow = paramRow(blob, room, 4200410)) {
                if (!g_kickRow[2]) logWrite("mario setup: dodge bump attack 4200410 found (knockback %.2f m)",
                                            *reinterpret_cast<const float *>(drow + 0x10));
                g_kickRow[2] = reinterpret_cast<volatile float *>(real(drow + 0x10));
            }
            for (const KnockRow &k : kRows) {
                const uint8_t *row = paramRow(blob, room, k.id);
                if (!row || room < size_t(row - reinterpret_cast<const uint8_t *>(blob)) + 0x14) continue;
                volatile float *at = reinterpret_cast<volatile float *>(real(row + 0x10));
                g_kickRow[k.id == 4200100 ? 0 : 1] = at;
                {
                    // Poise damage = weapon saWeaponDamage x atkSuperArmorCorrection (+0x4E, %)
                    // + atkSuperArmor (+0x5E); the weapon part is applied in pushScale (v161:
                    // atkSuperArmor alone is 0 for the Caestus rows).
                    uint16_t sa = 0, corr = 0;
                    memcpy(&sa, row + 0x5E, 2);
                    memcpy(&corr, row + 0x4E, 2);
                    g_kickPoiseFlat[k.id == 4200100 ? 0 : 1] = sa;
                    g_kickPoiseCorr[k.id == 4200100 ? 0 : 1] = corr;
                    logWrite("mario setup: Caestus attack %d poise: correction %u%%, flat %u", k.id, unsigned(corr), unsigned(sa));
                }
                if (*at != k.metres) {
                    *at = k.metres;
                    logWrite("mario setup: Caestus attack %d knockback -> %.2f m", k.id, k.metres);
                }
            }
            continue;
        }
        if (!init && !lot && !wep) continue;
        const uintptr_t blob = cbase + i - 0x0C;
        const size_t room = cbase + size - blob;
        if (wep) {
            // Mario's Gear only (the Fists' model category 7); every weapon and
            // shield at 1005 crashed the game at the title (v115).
            for (int lv = 0; lv <= 15; ++lv) {
                const uint8_t *row = paramRow(blob, room, kCaestus + lv);
                if (!row) continue;
                uint16_t model;
                memcpy(&model, row + 0xB8, 2);
                if (!lv) {
                    int16_t saw = 0;
                    memcpy(&saw, row + 0xCE, 2);
                    if (saw > 0 && saw < 1000) g_caestusPoise = saw;
                }
                if (model == 1000) {
                    *reinterpret_cast<volatile uint16_t *>(real(row + 0xB8)) = 1005;
                    if (!lv) logWrite("mario setup: Mario's Gear (Caestus) model -> none (Fists)");
                }
            }
            // (The Pyromancy Flame's part 911 has no mesh; the hand fire comes from the equipped
            // weapon, not the part: swapping it to the Talisman part only showed the Talisman.)
            // Hidden Body always on: Mario's Gear (Caestus) and the Pyromancy Flame carry it as their
            // resident SpEffect, so it needs no ring slot.
            if (g_hiddenBody) {
                struct WepRows { int base, step; };
                static const WepRows kRowsHb[] = {{kCaestus, 1}, {1330000, 100}, {1332000, 100}};
                for (const WepRows &w : kRowsHb) {
                    for (int lv = 0; lv <= 15; ++lv) {
                        const uint8_t *hrow = paramRow(blob, room, w.base + lv * w.step);
                        if (!hrow) continue;
                        volatile int32_t *res = reinterpret_cast<volatile int32_t *>(real(hrow + 0x4C));
                        if (*res == -1) {
                            *res = kHiddenBodySp;
                            if (!lv) logWrite("mario setup: weapon %d resident SpEffect -> Hidden Body (%d)", w.base, kHiddenBodySp);
                        }
                    }
                }
            }
            continue;
        }
        const uint8_t *row = paramRow(blob, room, init ? 3007 : 1810260);
        if (!row) continue;
        if (init) {
            // v170o: Mario starts wearing the Wing Cap (the Hollow Soldier Helm, 480000) instead
            // of the Pyromancer's Tattered Cloth Hood (230000): equip_Helm +0x20.
            int helm;
            memcpy(&helm, row + 0x20, 4);
            if (helm == 230000) {
                *reinterpret_cast<volatile int *>(real(row + 0x20)) = 480000;
                logWrite("mario setup: Mario's starting head -> Wing Cap (Hollow Soldier Helm 480000)");
            }
        }
        int weapon;
        memcpy(&weapon, row + (init ? 0x10 : 0x00), 4);
        if (weapon == kHandAxe) {
            *reinterpret_cast<volatile int *>(real(row + (init ? 0x10 : 0x00))) = kCaestus;
            ++patched;
            logWrite("mario setup: %s -> Caestus (Mario's Gear)",
                     init ? "Pyromancer starting weapon (CharaInitParam 3007)" : "tutorial weapon pickup (ItemLot 1810260)");
        } else if (weapon == kCaestus) {
            ++patched;
        }
    }
}

// ---- Text ----
struct Override { int id; const wchar_t *expectStart; const wchar_t *text; bool exact = false; };

inline wchar_t g_gearNames[16][32];
inline wchar_t g_capFrom[11][40], g_capTo[11][40], g_flameFrom[16][40], g_flameTo[16][40];
inline const wchar_t kMarioClass[] = L"Mario";
inline const wchar_t kMarioDescription[] =
    L"Hero from another world.\nStarts with Mario's Gear for punches and kicks,\nand the Wing Cap for flight.";

// Our strings must sit within 4 GB above the FMG (u32 offsets).
inline wchar_t *nearCopy(uintptr_t fmg, const wchar_t *s) {
    struct Block { uintptr_t at; size_t used; };
    static Block blocks[16];
    static int count = 0;
    const size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    for (int i = 0; i < count; ++i)
        if (blocks[i].at > fmg && blocks[i].at - fmg < 0xF0000000ull && blocks[i].used + bytes <= 0x10000) {
            wchar_t *out = reinterpret_cast<wchar_t *>(blocks[i].at + blocks[i].used);
            memcpy(out, s, bytes);
            blocks[i].used += (bytes + 7) & ~size_t(7);
            return out;
        }
    if (count >= 16) return nullptr;
    for (uintptr_t hint = (fmg + 0x10000) & ~uintptr_t(0xFFFF); hint - fmg < 0x80000000ull; hint += 0x10000) {
        void *m = VirtualAlloc(reinterpret_cast<void *>(hint), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!m) continue;
        blocks[count++] = {reinterpret_cast<uintptr_t>(m), 0};
        return nearCopy(fmg, s);
    }
    return nullptr;
}

inline const wchar_t *fmgText(uintptr_t fmg, uint32_t size, uint32_t offsetsAt, int index) {
    const uint32_t off = *reinterpret_cast<const uint32_t *>(fmg + offsetsAt + size_t(index) * 4);
    return off && off < size ? reinterpret_cast<const wchar_t *>(fmg + off) : nullptr;
}

constexpr int kTextTarget = 2 * (19 + 1 + 11 + 32);  // base + patch FMG copies of every entry
inline void textList(Override *list, int &n) {
    for (int lv = 0; lv <= 15; ++lv)
        if (lv) swprintf(g_gearNames[lv], 32, L"Mario's Gear+%d", lv);
        else wcscpy(g_gearNames[0], L"Mario's Gear");
    n = 0;
    for (int lv = 0; lv <= 15; ++lv) list[n++] = {kCaestus + lv, L"Caestus", g_gearNames[lv]};
    list[n++] = {132027, L"Pyromancer", kMarioClass};
    list[n++] = {132043, L"Pyromancer", kMarioClass};
    list[n++] = {132327, nullptr, kMarioDescription};  // only in an FMG whose 132027 is "Pyromancer"
    // Place name 1500 (v170n): the area banner and the bonfire warp list.
    list[n++] = {1500, L"Sen's Fortress", L"Whomp's Fortress", true};
    // v170o: the Hollow Soldier Helm is the Wing Cap, the Pyromancy Flame is Flower Power
    // (names only, whole-string matches; every upgrade level has its own entry).
    for (int lv = 0; lv <= 10; ++lv) {
        if (lv) { swprintf(g_capFrom[lv], 40, L"Hollow Soldier Helm +%d", lv); swprintf(g_capTo[lv], 40, L"Wing Cap +%d", lv); }
        else { wcscpy(g_capFrom[0], L"Hollow Soldier Helm"); wcscpy(g_capTo[0], L"Wing Cap"); }
        list[n++] = {480000 + lv, g_capFrom[lv], g_capTo[lv], true};
    }
    for (int lv = 0; lv <= 15; ++lv) {
        if (lv) { swprintf(g_flameFrom[lv], 40, L"Pyromancy Flame+%d", lv); swprintf(g_flameTo[lv], 40, L"Flower Power+%d", lv); }
        else { wcscpy(g_flameFrom[0], L"Pyromancy Flame"); wcscpy(g_flameTo[0], L"Flower Power"); }
        list[n++] = {1330000 + lv * 100, g_flameFrom[lv], g_flameTo[lv], true};
        list[n++] = {1332000 + lv * 100, g_flameFrom[lv], g_flameTo[lv], true};
    }
}

inline void patchScanText(uintptr_t start, size_t size, const uint8_t *p, int &done) {
    static Override list[80];
    static int n = 0;
    if (!n) textList(list, n);
    for (size_t i = 0; i + 0x1C < size; i += 4) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1 || p[i + 3] != 0) continue;
        uint32_t fsize, one, groups, strings, offsetsAt;
        memcpy(&fsize, p + i + 4, 4);
        memcpy(&one, p + i + 8, 4);
        memcpy(&groups, p + i + 0x0C, 4);
        memcpy(&strings, p + i + 0x10, 4);
        memcpy(&offsetsAt, p + i + 0x14, 4);
        if (one != 1 || fsize < 0x40 || fsize > (8u << 20) || i + fsize > size || !groups || groups > 20000 ||
            !strings || strings > 100000 || offsetsAt < 0x1C + groups * 12 || offsetsAt + strings * 4 > fsize)
            continue;
        const uint8_t *f = p + i;          // the copy
        const uintptr_t fmg = start + i;   // the game's FMG
        // Once per pass (chunks overlap).
        static uintptr_t seen[256];
        static int seenN = 0;
        static int seenPass = -1;
        if (seenPass != g_pass) { seenPass = g_pass; seenN = 0; }
        bool dup = false;
        for (int k = 0; k < seenN; ++k) if (seen[k] == fmg) dup = true;
        if (dup) { i += ((fsize + 3) & ~3u) - 4; continue; }
        if (seenN < 256) seen[seenN++] = fmg;
        auto indexOf = [&](int id) -> int {
            for (uint32_t g = 0; g < groups; ++g) {
                int gi[3];
                memcpy(gi, f + 0x1C + g * 12, 12);
                if (id >= gi[1] && id <= gi[2]) {
                    const int idx = gi[0] + (id - gi[1]);
                    return idx >= 0 && uint32_t(idx) < strings ? idx : -1;
                }
            }
            return -1;
        };
        // Text of an entry, from the copy; ours (outside the FMG) reads as "ours".
        auto text = [&](int idx, bool &ours) -> const wchar_t * {
            uint32_t off;
            memcpy(&off, f + offsetsAt + size_t(idx) * 4, 4);
            ours = off >= fsize;
            return off && off < fsize ? reinterpret_cast<const wchar_t *>(f + off) : nullptr;
        };
        bool classFmg = false, ours = false;
        {
            const int ci = indexOf(132027);
            const wchar_t *t = ci >= 0 ? text(ci, ours) : nullptr;
            classFmg = (t && wcsncmp(t, L"Pyromancer", 10) == 0) || (ci >= 0 && ours);
        }
        for (int k = 0; k < n; ++k) {
            const int idx = indexOf(list[k].id);
            if (idx < 0) continue;
            const wchar_t *t = text(idx, ours);
            if (ours) { ++done; continue; }  // already pointed at our string
            if (!t) continue;
            if (list[k].expectStart
                    ? (list[k].exact ? wcscmp(t, list[k].expectStart) != 0
                                     : wcsncmp(t, list[k].expectStart, wcslen(list[k].expectStart)) != 0)
                    : !classFmg)
                continue;
            wchar_t *s = nearCopy(fmg, list[k].text);
            if (!s) continue;
            *reinterpret_cast<volatile uint32_t *>(fmg + offsetsAt + size_t(idx) * 4) = uint32_t(uintptr_t(s) - fmg);
            ++done;
        }
        i += ((fsize + 3) & ~3u) - 4;  // past this FMG
    }
}

// Worker: params and text once they are loaded (retried every 2 s for two
// minutes: they load during the boot logos).  Started from the first input poll at the
// title, so the params are patched before a save loads its equipment (a late patch only
// applied after re-equipping).
// Settings that must be known before the first param scan (the live settings file is
// first read 15 s after launch): camwide=off, poisebonus=off.
inline void readEarlySettings() {
    wchar_t path[MAX_PATH] = {};
    wcscpy_s(path, MAX_PATH, install::settingsPath().c_str());  // v171
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char t[8192] = {};
    DWORD got = 0;
    ReadFile(f, t, sizeof(t) - 1, &got, nullptr);
    CloseHandle(f);
    if (strstr(t, "camwide=off")) g_camScale = 1.f;
    if (strstr(t, "poisebonus=off")) g_poiseBonus = 0;
    logWrite("mario setup: early settings: camera x%.2f, poise bonus %s", double(g_camScale), g_poiseBonus ? "on" : "off");
}
inline DWORD WINAPI setupThread(void *) {
    readEarlySettings();
    // Started at process start: polled every second (params load during the boot logos) for
    // up to three minutes.
    for (int attempt = 0; attempt < 180; ++attempt) {
        Sleep(1000);
        int patched = 0, done = 0;
        ++g_pass;
        forEachRegionCopy([&](uintptr_t start, size_t size, const uint8_t *copy) {
            if (g_paramsPatched < 2) patchScanParams(start, size, copy, patched);
            if (g_textEntries < kTextTarget) patchScanText(start, size, copy, done);
        });
        if (g_paramsPatched < 2) g_paramsPatched = patched;
        if (g_textEntries < kTextTarget) g_textEntries = done;
        if (patched || done || attempt % 10 == 0)
            logWrite("mario setup: pass %d: starting-weapon rows %ld/2, text entries %ld", attempt + 1, g_paramsPatched,
                     g_textEntries);
        if (g_paramsPatched >= 2 && g_textEntries >= kTextTarget) break;  // base + patch FMG copies
    }
    return 0;
}

inline void start() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (HANDLE h = CreateThread(nullptr, 0, setupThread, nullptr, 0, nullptr)) CloseHandle(h);
}

// Game thread: give the loaded character one Caestus (givecaestus=on).
using ItemGetFn = void (*)(void *inventory, int category, int id, int quantity, int64_t a5, int64_t a6, int64_t a7,
                           int64_t a8);
inline void giveTick() {
    if (!g_giveCaestus || g_given || !g_livePlayerIns || GetTickCount() - g_livePlayerChecked > 1000) return;
    static const uint8_t sig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
    const uintptr_t fn = base() + 0x7479E0;
    g_given = 1;
    if (!worldobs::readable(fn, sizeof(sig)) || memcmp(reinterpret_cast<void *>(fn), sig, sizeof(sig)) != 0) {
        logWrite("mario setup: item pickup signature mismatch; no Caestus given");
        return;
    }
    const uintptr_t at = base() + 0x1C8A530;
    if (!worldobs::readable(at, 8)) return;
    const uintptr_t classBase = *reinterpret_cast<const uintptr_t *>(at);
    if (!classBase || !worldobs::readable(classBase + 0x10, 8)) return;
    const uintptr_t gameData = *reinterpret_cast<const uintptr_t *>(classBase + 0x10);
    if (!gameData || !worldobs::readable(gameData + 0x280, 8)) return;
    reinterpret_cast<ItemGetFn>(fn)(reinterpret_cast<void *>(gameData + 0x280), 0, kCaestus, 1, 1, 1, 0, 1);
    logWrite("mario setup: gave the loaded character a Caestus (Mario's Gear)");
}

// givewingcap=on (v170o): put one Wing Cap (Hollow Soldier Helm 480000, armour category
// 0x10000000) in the loaded character's inventory, once per game session - for saves made
// before Mario started with it.
inline volatile LONG g_giveWingCap = 0, g_wingGiven = 0;
inline void giveWingCapTick() {
    if (!g_giveWingCap || g_wingGiven || !g_livePlayerIns || GetTickCount() - g_livePlayerChecked > 1000) return;
    static const uint8_t sig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
    const uintptr_t fn = base() + 0x7479E0;
    g_wingGiven = 1;
    if (!worldobs::readable(fn, sizeof(sig)) || memcmp(reinterpret_cast<void *>(fn), sig, sizeof(sig)) != 0) {
        logWrite("mario setup: item pickup signature mismatch; no Wing Cap given");
        return;
    }
    const uintptr_t at = base() + 0x1C8A530;
    if (!worldobs::readable(at, 8)) return;
    const uintptr_t classBase = *reinterpret_cast<const uintptr_t *>(at);
    if (!classBase || !worldobs::readable(classBase + 0x10, 8)) return;
    const uintptr_t gameData = *reinterpret_cast<const uintptr_t *>(classBase + 0x10);
    if (!gameData || !worldobs::readable(gameData + 0x280, 8)) return;
    reinterpret_cast<ItemGetFn>(fn)(reinterpret_cast<void *>(gameData + 0x280), 0x10000000, 480000, 1, 1, 1, 0, 1);
    logWrite("mario setup: gave the loaded character a Wing Cap (Hollow Soldier Helm)");
}

// ---- Test only (givehiddenbody=on): for each newly loaded character, learn
// Hidden Body (spell 3410), attune it in slot 1 and raise Intelligence and
// Attunement to 20 so the spell can be cast.  Not a starting-character change.
// Stats and attunement use the same chains as DSR-Gadget and the cheat table:
// gameData = [[ChrClassBase] + 0x10]; stats at +0x48 (Attunement), +0x68
// (Intelligence); attunement slot 1 = [[[BaseB] + 0x18] + 0x418] + 0x10.
inline volatile LONG g_testHiddenBody = 0;  // givehiddenbody=on
inline void writeMem(uintptr_t at, const void *v, size_t n) {
    SIZE_T wrote = 0;
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), v, n, &wrote);
}
inline void testHiddenBodyTick() {
    if (!g_testHiddenBody || !g_livePlayerIns || GetTickCount() - g_livePlayerChecked > 1000) return;
    static uintptr_t doneFor = 0;
    if (g_livePlayerIns == doneFor) return;
    const uintptr_t classAt = base() + 0x1C8A530;
    static uintptr_t baseB = 0;
    static int scans = 0;
    if (!baseB && scans < 60) {
        ++scans;
        baseB = worldobs::scanRelative("48 8B 05 ? ? ? ? 45 33 ED 48 8B F1 48 85 C0");
        if (baseB) logWrite("test: BaseB global at %p", reinterpret_cast<void *>(baseB));
    }
    const bool classOk = worldobs::readable(classAt, 8);
    const uintptr_t classBase = classOk ? *reinterpret_cast<const uintptr_t *>(classAt) : 0;
    const uintptr_t gameData = classBase && worldobs::readable(classBase + 0x10, 8)
                                   ? *reinterpret_cast<const uintptr_t *>(classBase + 0x10)
                                   : 0;
    if (!baseB || !gameData || !worldobs::readable(gameData + 0x280, 8)) {
        static int reports = 0;
        if (reports++ < 3)
            logWrite("test: not yet (BaseB %p, class %p, gameData %p)", reinterpret_cast<void *>(baseB),
                     reinterpret_cast<void *>(classBase), reinterpret_cast<void *>(gameData));
        return;
    }
    doneFor = g_livePlayerIns;
    logWrite("test: loaded character %p: gameData %p, BaseB %p", reinterpret_cast<void *>(g_livePlayerIns),
             reinterpret_cast<void *>(gameData), reinterpret_cast<void *>(baseB));

    // Stats: at least 20 Attunement and 20 Intelligence.
    for (uintptr_t off : {uintptr_t(0x48), uintptr_t(0x68)}) {
        int v = 0;
        if (!copyMem(gameData + off, &v, 4)) continue;
        const int want = v < 20 ? 20 : v;
        writeMem(gameData + off, &want, 4);
        logWrite("test: stat +0x%02X %d -> %d", unsigned(off), v, want);
    }
    // Give the Hidden Body test ring (accessory 116, category Accessories
    // 0x20000000).  The ring's resident SpEffect is Hidden Body (530) in the
    // patched param file; equip it in the menu once, before driving Mario.
    static const uint8_t sig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
    const uintptr_t fn = base() + 0x7479E0;
    if (worldobs::readable(fn, sizeof(sig)) && memcmp(reinterpret_cast<void *>(fn), sig, sizeof(sig)) == 0) {
        reinterpret_cast<ItemGetFn>(fn)(reinterpret_cast<void *>(gameData + 0x280), 0x20000000, 116, 1, 1, 1, 0, 1);
        logWrite("test: gave the loaded character the Hidden Body ring (accessory 116)");
    } else {
        logWrite("test: item pickup signature mismatch; no Hidden Body ring");
    }
}

// ---- Hide the knight (hideplayer=on): the game's own player-hide debug flag
// (DSR-Gadget "Player Hide": a bool in the global found by the pattern below,
// read as "80 3D disp32 00", disp at +2, 7-byte instruction).  Written each poll
// while the setting is on, cleared when it is off.
inline volatile LONG g_hidePlayer = 0;  // hideplayer=on
// Current hand IDs (primary left, primary right, secondary right, secondary left), set by equipDiagTick.
inline uint32_t g_handIds[4] = {};
inline uint32_t g_ringIds[2] = {0xFFFFFFFEu, 0xFFFFFFFEu};  // first read always logged
// Pyromancy Flames: 1330000 (+100 per level) and the ascended 1332000 (+100 per level).
inline bool isFlame(uint32_t id) { return id >= 1330000u && id < 1333000u; }
inline bool flameLeft() { return isFlame(g_handIds[0]); }
inline bool flameRight() { return isFlame(g_handIds[1]); }
// Diagnostic (equipment): the four hand weapon IDs from the cheat table chain
// Current map as world*100+area (PTDE currentMap).  The cheat table reads the
// world byte at [BaseE] + 0xA23 and the area byte at [BaseE] + 0xA22.  -1 when
// the global cannot be read or the values are not a Lordran map.
inline int currentMapKey() {
    static uintptr_t baseE = 0;
    static int scans = 0;
    if (!baseE && scans < 60) {
        ++scans;
        baseE = worldobs::scanRelative("48 8B 05 ? ? ? ? 48 8B 88 98 0B 00 00 8B 41 3C C3");
    }
    if (!baseE || !worldobs::readable(baseE, 8)) return -1;
    const uintptr_t x = *reinterpret_cast<const uintptr_t *>(baseE);
    if (!x || !worldobs::readable(x + 0xA23, 2)) return -1;
    const int world = *reinterpret_cast<const uint8_t *>(x + 0xA23);
    const int area = *reinterpret_cast<const uint8_t *>(x + 0xA22);
    static int lastWorld = -1, lastArea = -1;
    if (world != lastWorld || area != lastArea) {
        lastWorld = world;
        lastArea = area;
        logWrite("map: game reports world %d area %d (BaseE %p)", world, area, reinterpret_cast<void *>(x));
    }
    return world >= 10 && world <= 18 && area <= 3 ? world * 100 + area : -1;
}

// Bullet spawn probe (universal-modder kb, PR #249): the spawn function is at image
// address 0x14042A920.  Log its first bytes and the image base once so the prologue can
// be checked before anything calls it.  Read-only.
inline void dumpBytes(const char *tag, uintptr_t addr, size_t count) {
    if (!worldobs::readable(addr, count)) {
        logWrite("bullet: %s at %p not readable", tag, reinterpret_cast<void *>(addr));
        return;
    }
    const uint8_t *b = reinterpret_cast<const uint8_t *>(addr);
    logWrite("bullet: %s at %p, %zu bytes follow", tag, reinterpret_cast<void *>(addr), count);
    for (size_t row = 0; row < count / 16; ++row) {
        char line[128];
        int n = 0;
        for (int i = 0; i < 16; ++i) n += snprintf(line + n, sizeof(line) - n, "%02X ", b[row * 16 + i]);
        logWrite("bullet %s+%03zX: %s", tag, row * 16, line);
    }
}

// Hooks the bullet spawn function at RVA 0x42A920 with MinHook and logs which call site
// reaches it (the return address), so the fire path can be matched to a cast.  The hook
// forwards all four pointer arguments and the return value unchanged.  Read-only.
extern "C" {
int WINAPI MH_Initialize(void);
int WINAPI MH_CreateHook(LPVOID target, LPVOID detour, LPVOID *original);
int WINAPI MH_EnableHook(LPVOID target);
}
typedef void *(*SpawnFn4)(void *, void *, void *, void *);
inline SpawnFn4 g_spawnOrig = nullptr;
inline LONG g_spawnHits = 0;
// 1 while the flower is in the left hand: the native knight fireball is skipped (the
// callers null-check the result) and the host flower projectile is the only fireball.
inline volatile LONG g_suppressNativeFire = 0;
inline LONG g_suppressedCount = 0;
inline uintptr_t g_bulletMgr = 0;   // BulletMan passed to the spawn function
inline DWORD g_bulletArmed = 0;    // tick of the last spawn; the watch runs for a while after
inline DWORD g_flySpawn = 0;        // tick of the last site-A spawn (the flying fireball)
inline volatile LONG g_flyPending = 0;  // 1: the next bounce tick takes the list head as the fireball
inline uintptr_t g_flyNode = 0;    // the site-A fireball being bounced
inline float g_rootWorld[3] = {};   // Mario root in world metres (set by the pose tick)

inline volatile LONG g_castStart = 0;
inline int g_knightAnim = -1;  // current animation ID (animLogTick)
__attribute__((noinline)) inline void *hookedBulletSpawn(void *a, void *b, void *c, void *d) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_bulletMgr = reinterpret_cast<uintptr_t>(a);
    g_bulletArmed = GetTickCount();
    // Site A (call at RVA 0x429BF9) spawns the flying fireball; site B (0x429FF5) spawns a
    // second, motionless object about 100 ms later that becomes the list head.
    // Only the player's own cast (v159): enemy firebombs come through the same call site and
    // bounced too.  The knight must be in a cast animation (or within 1.5 s of a cast press)
    // with a flame in hand; the bounce tick also checks the bullet starts next to him.
    if (ret - base == 0x429BFE) {
        const DWORD now = GetTickCount(), cs = static_cast<DWORD>(g_castStart);
        const bool ours = (flameLeft() || flameRight()) &&
                          ((g_knightAnim >= 6200 && g_knightAnim < 6600) || (cs && now - cs < 1500));
        if (ours) {
            g_flySpawn = now;
            InterlockedExchange(&g_flyPending, 1);
        }
    }
    const LONG n = InterlockedIncrement(&g_spawnHits);
    if (n <= 60 || n % 50 == 0)
        logWrite("bullet spawn #%ld from RVA %p, mgr %p", static_cast<long>(n),
                 reinterpret_cast<void *>(ret - base), a);
    if (InterlockedCompareExchange(&g_suppressNativeFire, 1, 1) == 1) {
        const LONG s = InterlockedIncrement(&g_suppressedCount);
        if (s <= 20 || s % 50 == 0)
            logWrite("bullet spawn #%ld suppressed (flower in hand), total %ld", static_cast<long>(n),
                     static_cast<long>(s));
        return nullptr;
    }
    return g_spawnOrig(a, b, c, d);
}

// Keeps the suppression flag in step with the hand: flower in the left hand means the
// native fireball is replaced by the host flower projectile.
inline void suppressTick() {
    // Native fireball is back on: the flower also throws it (suppression is off).
    InterlockedExchange(&g_suppressNativeFire, 0);
}

// Bounce (Mario fireball style): the DSR fireball's position (0x10, 0x14, 0x18) shares the
// frame of the collision cache.  Its live velocity is 0x30..0x38 (0x40..0x48 is a copy the
// game refreshes); 0x1F4 is its remaining life in seconds (about 0.5 s, so it normally
// explodes after ~7 m).  On impact with a floor the game freezes the bullet and explodes it,
// so the bounce happens just before: while the site-A fireball is falling within 0.6 m of
// the floor below it, its vertical velocity is set to 2.6 m/s up (a third of the earlier
// 4.5 m/s height).  Its life is topped up while it moves, so it keeps bouncing until it hits
// a wall or an enemy (the position stops changing); then the game's explosion plays out.
// Safety cap: 10 s.
typedef float (*FloorFn)(const float *p);
inline void bulletBounceTick(FloorFn floorAt) {
    static uintptr_t node = 0;
    static int bounces = 0;
    static DWORD lastBounce = 0;
    static float lastPos[3] = {};
    static DWORD lastMoved = 0;
    static bool hit = false;
    if (!g_bulletMgr || !g_flySpawn || !floorAt) return;
    const DWORD now = GetTickCount();
    if (now - g_flySpawn > 10000) return;
    if (InterlockedExchange(&g_flyPending, 0)) {
        // The site-A fireball is the list head right after its spawn; lock onto it.
        if (!worldobs::readable(g_bulletMgr + 0x08, 8)) return;
        const uintptr_t head = *reinterpret_cast<const uintptr_t *>(g_bulletMgr + 0x08);
        if (!head || !worldobs::readable(head + 0x10, 12)) return;
        // Started within 3 m of the knight (an enemy's bomb spawned at the same moment is not).
        float me[3], cam[3];
        if (readLivePositions(me, cam)) {
            const float *bp = reinterpret_cast<const float *>(head + 0x10);
            const float dx = bp[0] - me[0], dy = bp[1] - me[1], dz = bp[2] - me[2];
            if (dx * dx + dy * dy + dz * dz > 3.f * 3.f) {
                logWrite("bullet bounce: spawn %.1f m from the knight is not his; left alone",
                         std::sqrt(dx * dx + dy * dy + dz * dz));
                return;
            }
        }
        g_flyNode = head;
    }
    const uintptr_t fly = g_flyNode;
    if (!fly || !worldobs::readable(fly, 0x200)) return;
    float *p = reinterpret_cast<float *>(fly + 0x10);
    float *v = reinterpret_cast<float *>(fly + 0x30);    // live velocity
    float *v40 = reinterpret_cast<float *>(fly + 0x40);  // copy
    float *life = reinterpret_cast<float *>(fly + 0x1F4);
    if (fly != node) {
        node = fly;
        bounces = 0;
        lastBounce = 0;
        hit = false;
        lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2];
        lastMoved = now;
        // Shorter hops: a quarter less horizontal speed (the bounce height stays).
        v[0] *= 0.75f; v[2] *= 0.75f;
        v40[0] = v[0]; v40[2] = v[2];
        logWrite("bullet bounce: tracking node %p, pos %.2f %.2f %.2f, velocity %.2f %.2f %.2f, life %.3f",
                 reinterpret_cast<void *>(fly), p[0], p[1], p[2], v[0], v[1], v[2], *life);
    }
    if (hit) return;
    // Impact (wall, enemy, or a missed floor): the game stops moving the bullet.
    if (p[0] != lastPos[0] || p[1] != lastPos[1] || p[2] != lastPos[2]) {
        lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2];
        lastMoved = now;
    } else if (now - lastMoved > 100) {
        hit = true;
        logWrite("bullet bounce: impact after %d bounce(s), %.1f s, at %.2f %.2f %.2f", bounces,
                 (now - g_flySpawn) / 1000.f, p[0], p[1], p[2]);
        return;
    }
    // Keep it alive while it moves.
    if (*life < 0.3f) *life = 0.5f;
    if (v[1] >= 0.f || now - lastBounce < 120) return;
    const float floor = floorAt(p);
    if (!std::isfinite(floor)) return;
    const float gap = p[1] - floor;
    if (gap > 0.6f || gap < -1.0f) return;
    v[1] = 2.6f;
    v40[1] = 2.6f;
    if (gap < 0.35f) p[1] = floor + 0.35f;  // lift out of the floor so the game sees no contact
    ++bounces;
    lastBounce = now;
    if (bounces <= 10 || bounces % 10 == 0)
        logWrite("bullet bounce #%d: floor %.2f, gap %.2f, velocity now %.2f %.2f %.2f", bounces, floor, gap,
                 v[0], v[1], v[2]);
}

// Faster cast: the knight's animation playback rate (DSR-Gadget: PlayerIns +0x68 ChrMapData,
// +0x18 ChrAnimData, +0xA8 float speed).  From the LB press with the flower in hand until the
// fireball spawns (1.5 s at most), the knight's animation runs at kCastSpeed; then it is 1 again.
constexpr float kCastSpeed = 4.0f;
// PTDE 20 (v159): faster recovery from hits.  The knight's hit reaction (an animation in
// 2000-2999 starting within 300 ms of an HP loss) plays at 1.25x; one still running after
// 500 ms (a knockdown) plays at 4x, so control returns within about a second.  Not for
// grabs, scripts or death (other animation ranges; HP 0).
inline volatile LONG g_headId = -1;  // the knight's head armour id (equipDiagTick)
inline volatile DWORD g_hitAt = 0;      // set by the presentation tick on native HP loss
inline volatile LONG g_recoveryOn = 1;  // recovery=off disables
inline float recoveryRate() {
    static int anim = -1;
    static DWORD since = 0;
    const DWORD now = GetTickCount();
    const DWORD hit = g_hitAt;
    if (!g_recoveryOn) { anim = -1; return 1.f; }
    if (hit && now - hit < 300 && g_knightAnim >= 2000 && g_knightAnim < 3000 && g_knightAnim != anim) {
        anim = g_knightAnim;
        since = now;
        logWrite("mario: hit reaction animation %d: faster recovery", anim);
    }
    if (anim < 0) return 1.f;
    if (g_knightAnim != anim || now - since > 3000) {
        anim = -1;
        return 1.f;
    }
    return now - since < 500 ? 1.25f : 4.f;
}
inline volatile LONG g_ladderSpeedPct = 0;  // set by the input filter while climbing with B held
inline void castSpeedTick() {
    static bool fast = false;
    const DWORD now = GetTickCount();
    const DWORD start = static_cast<DWORD>(g_castStart);
    // The knight's cast animation (6404 for the flame; 64xx left-hand casts) drives it, so
    // keyboard and controller casts both speed up; the LB press still covers its first frame.
    // Either hand (v159, PTDE 22): 62xx-65xx are the cast animations (64xx/65xx left/right release).
    const bool casting = g_knightAnim >= 6200 && g_knightAnim < 6600 && (flameLeft() || flameRight());
    const bool castWant = casting || (start && now - start < 300);
    const float rec = castWant ? 1.f : recoveryRate();
    // Ladder with B held (v170): the knight's climb clips run faster, as the knight's own dash-climb.
    const LONG lp = g_ladderSpeedPct;
    const float lad = (!castWant && lp > 100 && lp <= 400) ? lp / 100.f : 1.f;
    const bool want = castWant || rec != 1.f || lad != 1.f;
    const float target = castWant ? kCastSpeed : lad != 1.f ? lad : rec;
    if (!want && !fast) return;
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || !worldobs::readable(ins + 0x68, 8)) return;
    const uintptr_t map = *reinterpret_cast<const uintptr_t *>(ins + 0x68);
    if (!map || !worldobs::readable(map + 0x18, 8)) return;
    const uintptr_t anim = *reinterpret_cast<const uintptr_t *>(map + 0x18);
    if (!anim || !worldobs::readable(anim + 0xA8, 4)) return;
    volatile float *speed = reinterpret_cast<volatile float *>(anim + 0xA8);
    if (want) {
        if (*speed != target) *speed = target;
        if (!fast && castWant) logWrite("mario fire: cast at %.1fx animation speed", kCastSpeed);
        fast = true;
    } else {
        *speed = 1.f;
        fast = false;
        logWrite("mario: knight animation speed back to 1x");
    }
}

// Alpha probe: every 500 ms, compare PlayerIns (0x1000 bytes) and ChrMapData (0x400 bytes) with
// the previous snapshot and log floats in 0..1.5 that moved by more than 0.05.  Hidden Body's
// fade value for the player's own body should show when the effect turns on and off (equip
// or unequip the Caestus and the flame).  At most 300 lines.  Read-only.
inline void alphaProbeTick() {
    static uint8_t prevIns[0x1000], prevMap[0x400];
    static uintptr_t lastIns = 0, lastMap = 0;
    static DWORD lastAt = 0;
    static int lines = 0;
    if (lines >= 300) return;
    const DWORD now = GetTickCount();
    if (now - lastAt < 500) return;
    lastAt = now;
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || !worldobs::readable(ins, 0x1000) || !worldobs::readable(ins + 0x68, 8)) return;
    const uintptr_t map = *reinterpret_cast<const uintptr_t *>(ins + 0x68);
    const bool mapOk = map && worldobs::readable(map, 0x400);
    auto scan = [&](const char *tag, uintptr_t at, uint8_t *prev, size_t n, uintptr_t &last) {
        const uint8_t *cur = reinterpret_cast<const uint8_t *>(at);
        if (last == at) {
            for (size_t off = 0; off + 4 <= n && lines < 300; off += 4) {
                float a, b;
                memcpy(&a, prev + off, 4);
                memcpy(&b, cur + off, 4);
                if (!std::isfinite(a) || !std::isfinite(b)) continue;
                if (a < -0.001f || a > 1.5f || b < -0.001f || b > 1.5f) continue;
                if (std::fabs(a - b) <= 0.05f) continue;
                logWrite("alpha probe: %s+0x%03zX %.3f -> %.3f", tag, off, a, b);
                ++lines;
            }
        }
        memcpy(prev, cur, n);
        last = at;
    };
    scan("ins", ins, prevIns, sizeof(prevIns), lastIns);
    if (mapOk) scan("map", map, prevMap, sizeof(prevMap), lastMap);
}

// Animation log: the knight's current animation ID (DSR-Gadget: [[PlayerIns+0x68]+0x48]+0x80),
// logged when it changes (first 300 changes), to find the cast animations.  Read-only.
inline void animLogTick() {
    static int changes = 0;
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || !worldobs::readable(ins + 0x68, 8)) return;
    const uintptr_t map = *reinterpret_cast<const uintptr_t *>(ins + 0x68);
    if (!map || !worldobs::readable(map + 0x48, 8)) return;
    const uintptr_t anim = *reinterpret_cast<const uintptr_t *>(map + 0x48);
    if (!anim || !worldobs::readable(anim + 0x80, 4)) return;
    const int id = *reinterpret_cast<const int *>(anim + 0x80);
    if (id == g_knightAnim) return;
    if (changes < 300) {
        ++changes;
        logWrite("anim: knight %d -> %d (left hand %08X, right %08X)", g_knightAnim, id, g_handIds[0], g_handIds[1]);
    }
    g_knightAnim = id;
}

// Probe: after each native bullet spawn, log the first bullet in the BulletMan active list
// (head at [mgr+0x08]) over its flight: raw bytes plus the plausible float fields, so the
// position, velocity and impact state can be matched across snapshots.  Read-only.
inline void bulletWatchTick() {
    static int snaps = 0;
    static DWORD lastSnap = 0;
    if (!g_bulletMgr || snaps >= 60) return;
    const DWORD now = GetTickCount();
    if (now - g_bulletArmed > 2000 || now - lastSnap < 60) return;
    if (!worldobs::readable(g_bulletMgr + 0x08, 8)) return;
    const uintptr_t head = *reinterpret_cast<const uintptr_t *>(g_bulletMgr + 0x08);
    if (!head || !worldobs::readable(head, 0x360)) return;
    lastSnap = now;
    ++snaps;
    logWrite("bullet watch #%d: head %p, tick +%lu ms", snaps, reinterpret_cast<void *>(head),
             static_cast<unsigned long>(now - g_bulletArmed));
    char line[1600];
    int n = 0;
    line[0] = 0;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(head);
    for (size_t off = 0; off + 4 <= 0x360; off += 4) {
        float f = 0.f;
        memcpy(&f, b + off, 4);
        if (std::isfinite(f) && std::fabs(f) > 1e-3f && std::fabs(f) < 1e5f)
            n += snprintf(line + n, sizeof(line) - n, "%02zX=%.3f ", off, f);
        if (n > 1400) break;
    }
    logWrite("bullet floats: %s", line);
    // Frame offset: Mario root (world) minus the bullet's position fields (0x10, 0x14, 0x18).
    float bx = 0, by = 0, bz = 0;
    memcpy(&bx, b + 0x10, 4); memcpy(&by, b + 0x14, 4); memcpy(&bz, b + 0x18, 4);
    logWrite("bullet frame: root %.2f %.2f %.2f, bullet %.2f %.2f %.2f, offset %.2f %.2f %.2f",
             g_rootWorld[0], g_rootWorld[1], g_rootWorld[2], bx, by, bz,
             g_rootWorld[0] - bx, g_rootWorld[1] - by, g_rootWorld[2] - bz);
    // Integer-like fields that changed since the last snapshot of this bullet (the position and
    // velocity floats change every frame, so they are skipped).
    static uint8_t prev[0x360];
    static uintptr_t prevHead = 0;
    if (prevHead == head) {
        char dl[1000];
        int dn = 0;
        dl[0] = 0;
        for (size_t off = 0x40; off + 4 <= 0x360 && dn >= 0; off += 4) {
            if (off >= 0x58 && off < 0x60) continue;
            uint32_t oldv = 0, newv = 0;
            memcpy(&oldv, prev + off, 4);
            memcpy(&newv, b + off, 4);
            if (oldv == newv) continue;
            if (dn > 40) break;
            dn += snprintf(dl + dn, sizeof(dl) - dn, "%03zX:%08X>%08X ", off, oldv, newv);
            if (dn > 900) break;
        }
        if (dn > 0) logWrite("bullet changed: %s", dl);
    }
    prevHead = head;
    memcpy(prev, b, 0x360);
}

inline void installBulletSpawnHook() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t *target = reinterpret_cast<uint8_t *>(base + 0x42A920);
    static const uint8_t expected[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30};
    if (!worldobs::readable(reinterpret_cast<uintptr_t>(target), 6) || memcmp(target, expected, 6) != 0) {
        logWrite("bullet: spawn prologue mismatch at %p: %02X %02X %02X %02X %02X %02X %02X %02X; hook not installed",
                 target, target[0], target[1], target[2], target[3], target[4], target[5], target[6], target[7]);
        return;
    }
    const int init = MH_Initialize();
    if (init != 0 && init != 1) {
        logWrite("bullet: MinHook init failed (%d)", init);
        return;
    }
    int r = MH_CreateHook(target, reinterpret_cast<void *>(&hookedBulletSpawn),
                          reinterpret_cast<void **>(&g_spawnOrig));
    if ((r != 0 && r != 1) || !g_spawnOrig) {
        logWrite("bullet: spawn hook creation failed (%d)", r);
        return;
    }
    if (MH_EnableHook(target) != 0) {
        logWrite("bullet: spawn hook enable failed");
        return;
    }
    logWrite("bullet: spawn hook installed at %p", target);
}

// ---- Native knight hiding (v160, PTDE character_visibility.h) ----
// PlayerIns vtable entry 163 (RVA 0x3549E0, PTDE's E4B2C0 at entry 161) is the per-frame
// visibility updater: it sets or clears bit 0 of the body model's flags ([PlayerIns+0x60]
// +0x92, change mask +0x84 |= 0x200) and of the alternate model ([+0x850] +0xAA, +0x9C).
// After it runs, both bits are cleared while Mario is shown: the knight, his held items
// (the Estus flask) and his shadow are not drawn; animation, equipment and physics carry on.
// The updater restores the bits itself on its next call once this stops.
using VisibilityFn = void (*)(uintptr_t);
inline VisibilityFn g_visOrig = nullptr;
inline volatile LONG g_nativeHideWanted = 0;  // nativehide=on enables (off by default, v161)
inline volatile LONG g_nativeHideArmed = 0;   // Mario is being shown (presentation tick)
inline volatile LONG g_nativeHideFailed = 0;  // the watchdog turned it off for this session
inline volatile LONG g_visHookReady = 0;
inline void clearModelBit(uintptr_t model, size_t flags, size_t dirty) {
    if (!model || !worldobs::readable(model + dirty, 4) || !worldobs::readable(model + flags, 1)) return;
    volatile uint8_t *f = reinterpret_cast<volatile uint8_t *>(model + flags);
    if (!(*f & 1)) return;
    *reinterpret_cast<volatile uint32_t *>(model + dirty) |= 0x200u;
    *f = static_cast<uint8_t>(*f & ~1u);
}
inline void hookedVisibility(uintptr_t ins) {
    g_visOrig(ins);
    if (!g_nativeHideActive || ins != g_livePlayerIns || !worldobs::readable(ins + 0x858, 8)) return;
    clearModelBit(*reinterpret_cast<const uintptr_t *>(ins + 0x60), 0x92, 0x84);
    clearModelBit(*reinterpret_cast<const uintptr_t *>(ins + 0x850), 0xAA, 0x9C);
}
inline void installVisibilityHook() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t *target = reinterpret_cast<uint8_t *>(base + 0x3549E0);
    static const uint8_t expected[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                       0x80, 0xB9, 0xA6, 0x02, 0x00, 0x00, 0x80};
    if (!worldobs::readable(reinterpret_cast<uintptr_t>(target), sizeof(expected)) ||
        memcmp(target, expected, sizeof(expected)) != 0) {
        logWrite("hide: native visibility updater signature mismatch at %p; overlay hiding only", target);
        return;
    }
    const int init = MH_Initialize();
    if (init != 0 && init != 1) return;
    if (MH_CreateHook(target, reinterpret_cast<void *>(&hookedVisibility), reinterpret_cast<void **>(&g_visOrig)) != 0 ||
        !g_visOrig || MH_EnableHook(target) != 0) {
        logWrite("hide: native visibility hook failed; overlay hiding only");
        return;
    }
    g_visHookReady = 1;
    logWrite("hide: native visibility hook installed at %p (knight, held items and shadow)", target);
}
// Game thread: decide whether the knight is hidden natively, with a watchdog: if Mario has
// not been drawn for 2 s while it is active, it is turned off for the session (Mario's
// placement without the knight's uploads failed) and the overlay hiding takes over again.
inline void nativeHideTick() {
    static LONG lastDraws = 0;
    static DWORD lastChange = 0, activeSince = 0;
    const DWORD now = GetTickCount();
    const bool want = g_visHookReady && g_nativeHideWanted && g_nativeHideArmed && !g_nativeHideFailed;
    if (want != (g_nativeHideActive != 0)) {
        InterlockedExchange(&g_nativeHideActive, want ? 1 : 0);
        logWrite("hide: native knight hiding %s", want ? "on (Mario placed from memory)" : "off");
        activeSince = now;
        lastChange = now;
        lastDraws = g_marioDraws;
    }
    if (!g_nativeHideActive) return;
    const LONG d = g_marioDraws;
    if (d != lastDraws) { lastDraws = d; lastChange = now; return; }
    if (now - lastChange > 2000 && now - activeSince > 2000) {
        InterlockedExchange(&g_nativeHideFailed, 1);
        InterlockedExchange(&g_nativeHideActive, 0);
        logWrite("hide: Mario not drawn for 2 s with the knight hidden natively; native hiding off for this session");
    }
}

inline void bulletProbe() {
    static bool done = false;
    if (done) return;
    done = true;
    installBulletSpawnHook();
    installVisibilityHook();
}

// [[BaseB] + 0x10] + 0x324 (primary left), 0x328 (primary right), 0x32C (secondary
// right), 0x330 (secondary left).  Logged when any of them changes.
inline void equipDiagTick() {
    bulletProbe();
    static uintptr_t baseB = 0;
    static int scans = 0;
    static uint32_t last[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    static DWORD lastPoll = 0;
    if (GetTickCount() - lastPoll < 250) return;
    lastPoll = GetTickCount();
    if (!baseB && scans < 60) {
        ++scans;
        baseB = worldobs::scanRelative("48 8B 05 ? ? ? ? 45 33 ED 48 8B F1 48 85 C0");
    }
    if (!baseB || !worldobs::readable(baseB, 8)) return;
    const uintptr_t x = *reinterpret_cast<const uintptr_t *>(baseB);
    if (!x || !worldobs::readable(x + 0x10, 8)) return;
    const uintptr_t y = *reinterpret_cast<const uintptr_t *>(x + 0x10);
    if (!y || !worldobs::readable(y + 0x330, 4)) return;
    const uint32_t ids[4] = {*reinterpret_cast<const uint32_t *>(y + 0x324),
                             *reinterpret_cast<const uint32_t *>(y + 0x328),
                             *reinterpret_cast<const uint32_t *>(y + 0x32C),
                             *reinterpret_cast<const uint32_t *>(y + 0x330)};
    for (int i = 0; i < 4; ++i) g_handIds[i] = ids[i];
    if (worldobs::readable(y + 0x344, 4)) {
        // Head armour (slot 8: hands +0x324.., ammo +0x334.., head +0x344, v170n).
        const LONG h = static_cast<LONG>(*reinterpret_cast<const uint32_t *>(y + 0x344));
        if (h != g_headId) logWrite("equip: head %ld", h);
        InterlockedExchange(&g_headId, h);
    }
    // Rings: equipment slots 13 and 14 after the hands (PTDE stats +0x280/+0x284 with its
    // hands at +0x24C; here the hands start at +0x324).
    if (worldobs::readable(y + 0x35C, 4)) {
        const uint32_t r1 = *reinterpret_cast<const uint32_t *>(y + 0x358), r2 = *reinterpret_cast<const uint32_t *>(y + 0x35C);
        if (r1 != g_ringIds[0] || r2 != g_ringIds[1])
            logWrite("equip: rings %u and %u%s", r1, r2, (r1 == 139 || r2 == 139) ? " (Orange Charred Ring)" : "");
        g_ringIds[0] = r1;
        g_ringIds[1] = r2;
    }
    bool changed = false;
    for (int i = 0; i < 4; ++i) changed = changed || ids[i] != last[i];
    if (!changed) return;
    for (int i = 0; i < 4; ++i) last[i] = ids[i];
    logWrite("equip: primary left %08X, primary right %08X, secondary right %08X, secondary left %08X",
             ids[0], ids[1], ids[2], ids[3]);
}
// ---- Lava (PTDE 23, v159) ----
// The player's active SpEffect list, located once per character by its shape: a pointer in
// PlayerIns to a manager whose head node points at SpEffectParam rows, nodes linked by a
// next pointer.  (PTDE: ChrIns+0x1E0 manager, +4 head, node+0x2C param, node+0x30 next.)
inline volatile LONG g_envLava = 0, g_envProtected = 0, g_envReady = 0;
inline bool spRow(uintptr_t q) {
    return g_spRows.contains(q);
}
inline uintptr_t qword(uintptr_t at) {
    uintptr_t v = 0;
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), &v, 8, &got) && got == 8 ? v : 0;
}
inline void environmentTick() {
    static DWORD last = 0;
    static uintptr_t ins0 = 0;
    static int A = -1, B = -1, C = -1, D = -1;
    static DWORD lastSearch = 0;
    const DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || !g_spFirst) { g_envReady = 0; return; }
    static int searches = 0;
    if (ins != ins0) { ins0 = ins; A = -1; searches = 0; }
    if (A < 0) {
        if (now - lastSearch < 5000 || searches >= 6) { g_envReady = 0; return; }
        ++searches;
        lastSearch = now;
        int best = 0;
        // The x64 form of PTDE's layout (node+0x2C param, +0x30 next): param +0x38, next +0x40,
        // head at manager +0x08, found live at PlayerIns+0x278 (v159).  Preferred when it holds;
        // the shape search below is the fallback (it once took a one-node list with next +0x18).
        {
            const uintptr_t m = qword(ins + 0x278), n = m ? qword(m + 0x08) : 0;
            if (n && spRow(qword(n + 0x38))) {
                int len = 1;
                for (uintptr_t node = qword(n + 0x40); node && len < 64; node = qword(node + 0x40)) {
                    if (!spRow(qword(node + 0x38))) { len = 0; break; }
                    ++len;
                }
                if (len > 0) { A = 0x278; B = 0x08; C = 0x38; D = 0x40; best = len; }
            }
        }
        const bool known = A >= 0;
        for (int a = 0; a < 0x1000 && !known; a += 8) {
            const uintptr_t m = qword(ins + a);
            if (!m || m >= 0x0000800000000000ull) continue;
            for (int b = 0; b <= 0x40; b += 8) {
                const uintptr_t n = qword(m + b);
                if (!n || n >= 0x0000800000000000ull) continue;
                for (int c = 0; c <= 0x80; c += 8) {
                    if (!spRow(qword(n + c))) continue;
                    for (int dd = 0; dd <= 0x80; dd += 8) {
                        if (dd == c) continue;
                        int len = 1;
                        uintptr_t node = qword(n + dd);
                        bool ok = true;
                        while (node && len < 64) {
                            if (!spRow(qword(node + c))) { ok = false; break; }
                            ++len;
                            node = qword(node + dd);
                        }
                        if (ok && len > best) { best = len; A = a; B = b; C = c; D = dd; }
                    }
                }
            }
        }
        if (A < 0) {
            static int misses = 0;
            if (++misses <= 3) logWrite("mario: lava: active SpEffect list not found yet");
            g_envReady = 0;
            return;
        }
        logWrite("mario: lava: active SpEffect list at PlayerIns+0x%X, head +0x%X, param +0x%X, next +0x%X (%d effects)", A, B, C, D, best);
    }
    bool lava = false, prot = false;
    uintptr_t node = qword(qword(ins + A) + B);
    for (int k = 0; node && k < 128; ++k) {
        const uintptr_t row = qword(node + C);
        if (!spRow(row)) break;
        uint8_t state = 0;
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(row + 0x156), &state, 1, &got) && got == 1) {
            lava |= state == 142;
            prot |= state == 186;
        }
        node = qword(node + D);
    }
    prot |= g_ringIds[0] == 139 || g_ringIds[1] == 139;
    if (lava != (g_envLava != 0)) logWrite("mario: lava contact %s%s", lava ? "on" : "off", lava && prot ? " (protected)" : "");
    g_envLava = lava;
    g_envProtected = prot;
    g_envReady = 1;
}

inline void hidePlayerTick() {
    static uintptr_t flag = 0;
    static int scans = 0;
    static bool stealthCleared = false;
    static bool wasOn = false;
    static uintptr_t hiddenIns = 0;
    if (!flag && scans < 60) {
        ++scans;
        flag = worldobs::scanRelativeAt("80 3D ? ? ? ? 00 48 8B 8F ? ? ? ? 0F B6 DB", 2, 7);
        if (flag) logWrite("hide: debug PlayerHide flag at %p", reinterpret_cast<void *>(flag));
    }
    // That debug flag is DSR-Gadget's "Player Hide": enemies ignore the player; it does not
    // hide the model.  Keep it off.
    if (flag && !stealthCleared) {
        const uint8_t zero = 0;
        writeMem(flag, &zero, 1);
        stealthCleared = true;
        logWrite("hide: debug PlayerHide (enemy stealth) kept off");
    }
    // The model: PlayerIns ChrFlags1 (+0x2A4 on 1.03) bit 0x00800000, DSR-Gadget "ToggleDraw".
    const bool on = g_hidePlayer != 0;
    const uintptr_t ins = g_livePlayerIns;
    if (!ins || !worldobs::readable(ins + 0x2A4, 4)) return;
    volatile LONG *flags = reinterpret_cast<volatile LONG *>(ins + 0x2A4);
    const bool set = (*flags & 0x00800000) != 0;
    if (on && !set) InterlockedOr(flags, 0x00800000);
    if (!on && set && wasOn) InterlockedAnd(flags, ~LONG(0x00800000));
    if (on != wasOn || (on && ins != hiddenIns)) {
        logWrite("hide: knight draw bit %s (PlayerIns %p, ChrFlags1 now %08lX)", on ? "set" : "cleared",
                 reinterpret_cast<void *>(ins), static_cast<unsigned long>(*flags));
        hiddenIns = ins;
    }
    wasOn = on;
}

} // namespace mariosetup
} // namespace remastered
