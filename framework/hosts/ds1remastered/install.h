#pragma once

// install.h -- the shippable layout (v171).
//
//   <game>\dinput8.dll               this host
//   <game>\MarioSouls\mario64.dll    the Mario adapter
//   <game>\MarioSouls\sm64.dll       libsm64
//   <game>\*.z64 (or .n64 / .v64)    the player's own Super Mario 64 (USA) ROM
//
// remastered.ini (%LOCALAPPDATA%\MarioSoulsProbe) still overrides each path when present
// (the development setup).  Collision caches are built on first launch from the player's own
// game files (collision_build.h) into %LOCALAPPDATA%\MarioSoulsProbe\collision.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collision_build.h"
#include "logging.h"

namespace remastered {
namespace install {

inline std::string utf8(const std::wstring &w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring dirOf(const wchar_t *path) {
    std::wstring p = path;
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
// The game's folder (DarkSoulsRemastered.exe).
inline std::wstring gameDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return dirOf(p);
}
// The folder of this DLL (the game folder when installed as dinput8.dll).
inline std::wstring hostDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&gameDir), &self);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(self, p, MAX_PATH);
    return dirOf(p);
}
inline std::wstring dataDir() {
    wchar_t p[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", p, MAX_PATH)) return hostDir() + L"\\MarioSouls";
    std::wstring d = std::wstring(p) + L"\\MarioSoulsProbe";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
inline std::wstring collisionDir() {
    std::wstring d = dataDir() + L"\\collision";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
inline bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// Live settings: <game>\MarioSouls\settings.txt when it exists (players), otherwise
// %LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt (development).
inline std::wstring settingsPath() {
    const std::wstring player = hostDir() + L"\\MarioSouls\\settings.txt";
    if (exists(player)) return player;
    return dataDir() + L"\\inject_mode.txt";
}

// ---- The ROM ----
// Super Mario 64 (USA), 8 MB, in any of the three common byte orders; libsm64 reads the
// big-endian (.z64) form, so another order is converted once into the data folder.
inline bool findRom(std::string &out) {
    std::vector<std::wstring> dirs = {gameDir(), hostDir(), hostDir() + L"\\MarioSouls"};
    for (const std::wstring &d : dirs) {
        for (const wchar_t *pat : {L"\\*.z64", L"\\*.n64", L"\\*.v64", L"\\*.rom", L"\\*.bin"}) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.nFileSizeLow != 8u << 20 || fd.nFileSizeHigh) continue;
                const std::wstring path = d + L"\\" + fd.cFileName;
                collisionbuild::Bytes rom;
                if (!collisionbuild::readFile(utf8(path), rom) || rom.size() != (8u << 20)) continue;
                const uint8_t z64[4] = {0x80, 0x37, 0x12, 0x40}, v64[4] = {0x37, 0x80, 0x40, 0x12}, n64[4] = {0x40, 0x12, 0x37, 0x80};
                int order = !memcmp(rom.data(), z64, 4) ? 0 : !memcmp(rom.data(), v64, 4) ? 1 : !memcmp(rom.data(), n64, 4) ? 2 : -1;
                if (order < 0) continue;
                if (order == 1) for (size_t i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
                if (order == 2)
                    for (size_t i = 0; i + 3 < rom.size(); i += 4) { std::swap(rom[i], rom[i + 3]); std::swap(rom[i + 1], rom[i + 2]); }
                // Internal name and region (E = USA): libsm64's tables are for the US release.
                if (memcmp(rom.data() + 0x20, "SUPER MARIO 64", 14) != 0) continue;
                if (rom[0x3E] != 'E') {
                    logWrite("install: %s is Super Mario 64 but not the USA release (region '%c'); skipped",
                             utf8(path).c_str(), rom[0x3E]);
                    continue;
                }
                FindClose(h);
                if (order == 0) { out = utf8(path); return true; }
                const std::wstring conv = dataDir() + L"\\sm64_us.z64";
                if (!collisionbuild::writeFile(utf8(conv), rom)) return false;
                logWrite("install: %s converted to big-endian (%s)", utf8(path).c_str(), utf8(conv).c_str());
                out = utf8(conv);
                return true;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return false;
}

// ---- Collision caches (first launch) ----
inline volatile LONG g_buildState = 0;  // 0 not started, 1 building, 2 done
inline SRWLOCK g_buildLock = SRWLOCK_INIT;
inline std::unordered_map<int, int> g_objectHp;
inline collisionbuild::Bytes g_commonEmevd;
inline bool g_buildInputs = false;

inline bool buildInputs() {
    if (g_buildInputs) return true;
    const std::string game = utf8(gameDir());
    if (!collisionbuild::objectParamHp(game, g_objectHp)) logWrite("install: ObjectParam not read (no breakable tables)");
    collisionbuild::Bytes ce;
    if (collisionbuild::readFile(game + "/event/common.emevd.dcx", ce)) collisionbuild::dcx(ce, g_commonEmevd);
    g_buildInputs = true;
    return true;
}
// One map (e.g. "m10_01_00_00") when its cache is missing.  Any thread; serialised.
inline bool ensureMapCache(const std::string &mapId) {
    const std::wstring dir = collisionDir();
    std::wstring wmap(mapId.begin(), mapId.end());
    if (exists(dir + L"\\" + wmap + L".bin") && exists(dir + L"\\" + wmap + L".brk")) return true;
    AcquireSRWLockExclusive(&g_buildLock);
    bool ok = exists(dir + L"\\" + wmap + L".bin") && exists(dir + L"\\" + wmap + L".brk");
    if (!ok) {
        buildInputs();
        const DWORD t0 = GetTickCount();
        const collisionbuild::MapResult r =
            collisionbuild::buildMap(utf8(gameDir()), mapId, utf8(dir), g_objectHp, g_commonEmevd);
        logWrite("install: built collision for %s: %zu triangles, %zu native-owned, %zu unsupported, %zu breakables%s%s (%lu ms)",
                 mapId.c_str(), r.triangles, r.nativeOwned, r.failed, r.breakables, r.error.empty() ? "" : "; ",
                 r.error.c_str(), GetTickCount() - t0);
        ok = r.error.empty();
    }
    ReleaseSRWLockExclusive(&g_buildLock);
    return ok;
}
inline DWORD WINAPI buildAllThread(void *) {
    InterlockedExchange(&g_buildState, 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gameDir() + L"\\map\\MapStudio\\m1*.msb").c_str(), &fd);
    int maps = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n.size() != 16) continue;  // m10_01_00_00.msb
            ensureMapCache(utf8(n.substr(0, 12)));
            ++maps;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    logWrite("install: collision caches ready (%d maps)", maps);
    InterlockedExchange(&g_buildState, 2);
    return 0;
}
inline void startBuild() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (HANDLE h = CreateThread(nullptr, 0, buildAllThread, nullptr, 0, nullptr)) CloseHandle(h);
}

}  // namespace install
}  // namespace remastered
