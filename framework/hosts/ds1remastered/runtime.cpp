// Remastered host composition root.
//
// This is a minimal, presentation-only host.  It loads as a dinput8.dll proxy,
// hooks D3D11CreateDevice and CreateDXGIFactory, and provides the DX11 backend
// for rendering.  No gameplay integration, no native hooks, no actor/camera
// observation yet.  Those are added incrementally as native evidence is gathered.
//
// Build: x64 DLL, linked against d3d11.lib and dxgi.lib.
// Install: copy as dinput8.dll beside DarkSoulsRemastered.exe.
// Revert: delete dinput8.dll.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>

#include "render_device.h"
#include "loader.h"
#include "startup.h"
#include "memory.h"
#include "native_types.h"
#include "capabilities.h"
#include "camera_capture.h"
#include "geometry_pass.h"
#include "world_observation.h"
#include "logging.h"

namespace remastered {

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// Implemented in logging.cpp

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_context = nullptr;
static IDXGISwapChain *g_swapChain = nullptr;
static HMODULE g_realDirectInput = nullptr;
static void *g_realDI8Create = nullptr;

using PfnCreateDevice = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, UINT, UINT,
                                          const void *, UINT, UINT, ID3D11Device **,
                                          D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
using PfnCreateFactory = HRESULT(WINAPI *)(void *, void **);

static PfnCreateDevice g_realCreateDevice = nullptr;
static PfnCreateFactory g_realCreateFactory = nullptr;

// Present hook
static HRESULT (STDMETHODCALLTYPE *g_realPresent)(IDXGISwapChain *, UINT, UINT) = nullptr;

// Player observation state.
static void *g_playerBase = nullptr;
static NativeObservation g_currentObservation = {};

// Factory hooks for CreateSwapChain.
//
// Keyed by vtable rather than by object.  Several IDXGIFactory objects share a
// single vtable, and only vtables we actually patched are ever recorded here --
// so a lookup miss means "we never hooked this factory", never "call slot 10
// anyway".  Blanking down to slot 10 on a miss would re-enter the hook itself
// and recurse until the stack ran out.
struct FactoryHooks { void *factory; void **vt; void *createSwapChain; };
static const int kMaxFactories = 8;
static FactoryHooks g_factories[kMaxFactories] = {};
static volatile LONG g_factoryCount = 0;

static FactoryHooks *findFactory(void *f) {
    void **vt = f ? *reinterpret_cast<void ***>(f) : nullptr;
    if (!vt) return nullptr;
    LONG n = InterlockedCompareExchange(&g_factoryCount, 0, 0);
    for (LONG i = 0; i < n && i < kMaxFactories; ++i) if (g_factories[i].vt == vt) return &g_factories[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
const NativeObservation &getObservation();
HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(void *self, void *device, void *desc, void **out);
HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags);
DWORD WINAPI delayedHookInstallThread(void *param);

// ---------------------------------------------------------------------------
// IAT patching
// ---------------------------------------------------------------------------
namespace iat {
bool patch(HMODULE module, const char *wantedDll, const char *wantedFunction, void *replacement) {
    if (!module) return false;
    BYTE *base = reinterpret_cast<BYTE *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto *d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; d->Name; ++d) {
        const char *dll = reinterpret_cast<const char *>(base + d->Name);
        if (_stricmp(dll, wantedDll) != 0) continue;
        DWORD lr = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        auto *lookup = reinterpret_cast<uint64_t *>(base + lr);
        auto *addrs = reinterpret_cast<uint64_t *>(base + d->FirstThunk);
        for (; *lookup; ++lookup, ++addrs) {
            if (*lookup & (1ull << 63)) continue;
            const char *name = reinterpret_cast<const char *>(base + (*lookup & 0x7fffffff)) + 2;
            if (strcmp(name, wantedFunction) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(addrs, sizeof(uint64_t), PAGE_READWRITE, &old)) return false;
            *addrs = reinterpret_cast<uint64_t>(replacement);
            VirtualProtect(addrs, sizeof(uint64_t), old, &old);
            FlushInstructionCache(GetCurrentProcess(), addrs, sizeof(uint64_t));
            logWrite("iat-patch %s!%s -> %p", dll, name, replacement);
            return true;
        }
    }
    return false;
}
} // namespace iat

// ---------------------------------------------------------------------------
// Hooked functions
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE hookedCreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
                                             UINT sw, UINT flags, const void *create,
                                             UINT levels, UINT sdk, ID3D11Device **device,
                                             D3D_FEATURE_LEVEL *obtained,
                                             ID3D11DeviceContext **context) {
    HRESULT hr = g_realCreateDevice(adapter, type, sw, flags, create, levels, sdk, device, obtained, context);
    logWrite("create-device hr=0x%08lx device=%p context=%p", hr, device ? (void *)*device : nullptr, context ? (void *)*context : nullptr);
    if (hr == S_OK) { g_device = device ? *device : nullptr; g_context = context ? *context : nullptr; if (g_device && g_context) onDeviceCreated(g_device, g_context); }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateFactory(void *riid, void **out) {
    HRESULT hr = g_realCreateFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        void *factory = *out; logWrite("factory created: %p", factory);
        void **factVt = *reinterpret_cast<void ***>(factory);
        if (factVt) {
            LONG n = InterlockedCompareExchange(&g_factoryCount, 0, 0);
            for (LONG i = 0; i < n && i < kMaxFactories; ++i) if (g_factories[i].vt == factVt) return hr;
            LONG slot = InterlockedIncrement(&g_factoryCount) - 1;
            if (slot < kMaxFactories) { FactoryHooks *h = &g_factories[slot]; h->factory = factory; h->vt = factVt; h->createSwapChain = factVt[10]; logWrite("factory %p recorded for delayed patching", factory); }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(void *self, void *device, void *desc, void **out) {
    logWrite("hookedCreateSwapChain called: self=%p device=%p", self, device);
    void **vt = *reinterpret_cast<void ***>(self);
    if (vt) logWrite("  self vtable[0]=%p vtable[10]=%p vtable[15]=%p", vt[0], vt[10], vt[15]);
    FactoryHooks *h = findFactory(self);
    if (!h || !h->createSwapChain) {
        logWrite("hookedCreateSwapChain: no original recorded for vtable %p; not recursing", vt);
        return E_FAIL;
    }
    auto fn = reinterpret_cast<HRESULT(STDMETHODCALLTYPE *)(void*, void*, void*, void**)>(h->createSwapChain);
    logWrite("calling real CreateSwapChain: %p", h->createSwapChain);
    HRESULT hr = fn(self, device, desc, out);
    logWrite("real CreateSwapChain returned hr=0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out) { logWrite("CreateSwapChain created swap chain: %p", *out); onSwapChainCreated(reinterpret_cast<IDXGISwapChain *>(*out)); }
    return hr;
}

static HRESULT (STDMETHODCALLTYPE *g_realCreateDeferredContext)(ID3D11Device *, UINT, ID3D11DeviceContext **) = nullptr;
HRESULT STDMETHODCALLTYPE hookedCreateDeferredContext(ID3D11Device *device, UINT flags, ID3D11DeviceContext **out) {
    HRESULT hr = g_realCreateDeferredContext(device, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        logWrite("deferred context created: %p", (void *)*out);
        // Patch Map/Unmap on the new deferred context too.  The view-projection
        // uploads were previously assumed to live on the immediate context;
        // they do not, so this is the only place the matrix can be seen.
        patchCamContext(*out, "deferred");
    }
    return hr;
}

void onDeviceCreated(ID3D11Device *device, ID3D11DeviceContext *context) { logWrite("onDeviceCreated: device=%p context=%p", (void *)device, (void *)context); }
void onSwapChainCreated(IDXGISwapChain *swapChain) { logWrite("onSwapChainCreated: swapChain=%p", (void *)swapChain); g_swapChain = swapChain; }

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags) {
    static uint64_t frameCount = 0;
    if ((frameCount++ % 60) == 0) {
        logWrite("Present #%llu sync=%u flags=%u", frameCount, syncInterval, flags);
        getObservation();
        if (g_currentObservation.player.valid) logWrite("player obs: hp=%d/%d stam=%d/%d base=%p", g_currentObservation.player.hp, g_currentObservation.player.maxHp, g_currentObservation.player.stamina, g_currentObservation.player.maxStamina, g_playerBase);
        CameraState cam = getCameraState();
        if (cam.valid) logWrite("camera obs: valid=1 m[0]=%.3f m[5]=%.3f m[10]=%.3f m[15]=%.3f", cam.viewProjection.m[0], cam.viewProjection.m[5], cam.viewProjection.m[10], cam.viewProjection.m[15]);
    }
    if (g_realPresent) return g_realPresent(swapChain, syncInterval, flags);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Player observation
// ---------------------------------------------------------------------------
// The player structure lives at a heap address that changes every session, so it
// is reached through the static pointer chain below rather than by scanning.
//
// From a Cheat Engine pointer scan that resolved identically after a game
// restart:
//
//   DarkSoulsRemastered.exe+01726B14 -> +0x100 -> +0x28 -> +0xE0
//                                    -> +0xFE0 -> +0x0  -> +0x3E8 (HP)
//
// The root is module-relative and the executable has no ASLR (image base
// 0x140000000, DYNAMIC_BASE absent), so the root is a fixed address.
//
// The Cheat Engine offset list is stored leaf-first, so the chain is replayed in
// the reverse of that order and the final +0x3E8 is applied to the last pointer.
// That ordering was confirmed live: it resolved to hp 453/594 and stamina 49/49,
// matching the values from the original Cheat Engine verification.
//
// The +0xE0 link is null until a character is loaded, so resolution fails while
// at the title screen and starts succeeding once a save is in play.  That is
// expected, not a fault: the worker retries until it succeeds.
static const uintptr_t kPlayerChainRootRva = 0x1726B14;

// Applied in order; each entry dereferences, except the final HP add.
static const uintptr_t kPlayerChainOffsets[] = {0x100, 0x28, 0xE0, 0xFE0, 0x0};
static const size_t kPlayerChainSteps =
    sizeof(kPlayerChainOffsets) / sizeof(kPlayerChainOffsets[0]);

// Stamina lives in a different structure behind its own static root.  The
// leaf-first Cheat Engine list was 30,50,128,30,180,38,28, replayed as the
// deref sequence below with +0x30 as the final add.  Note 0x30 appears both as a
// deref step and as the terminator, which is why the order is logged rather
// than assumed.
static const uintptr_t kStaminaChainRootRva = 0x1A31768;
static const uintptr_t kStaminaChainOffsets[] = {0x28, 0x38, 0x180, 0x30, 0x128, 0x50};
static const size_t kStaminaChainSteps =
    sizeof(kStaminaChainOffsets) / sizeof(kStaminaChainOffsets[0]);
static const uintptr_t kStaminaFinalOffset = 0x30;

// A pointer read is only trustworthy if both the source address is readable and
// the value it produces is itself a plausible pointer.  Validating only the
// source lets a wild dereference follow garbage into unmapped memory and fault
// inside the game process -- which crashed the game during probing.
static bool plausiblePointer(uintptr_t v) {
    if (!v) return false;
    // Remastered runs as a 64-bit process; user-mode allocations sit well below
    // the canonical-user ceiling.  Anything at or above it cannot be a heap or
    // module pointer.
    return v < 0x0000800000000000ull;
}

static bool readablePointer(uintptr_t addr, uintptr_t *out) {
    if (!plausiblePointer(addr)) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    uintptr_t v = *reinterpret_cast<const uintptr_t *>(addr);
    if (!plausiblePointer(v)) return false;
    *out = v;
    return true;
}

static void *walkChain(uintptr_t rootRva, const uintptr_t *offsets, size_t steps, bool verbose);

// Walk the health chain and return its structure base (HP at base+0x3E8).
static void *resolvePlayerBase() {
    return walkChain(kPlayerChainRootRva, kPlayerChainOffsets, kPlayerChainSteps, false);
}

// Walk an arbitrary static chain and return the final structure base.
// Shared by the health and stamina chains so both get identical guarding.
static void *walkChain(uintptr_t rootRva, const uintptr_t *offsets, size_t steps, bool verbose) {
    uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!image) return nullptr;
    uintptr_t p = image + rootRva;
    if (!readablePointer(p, &p)) return nullptr;
    for (size_t i = 0; i < steps; ++i) {
        uintptr_t next = 0;
        if (!readablePointer(p + offsets[i], &next)) {
            if (verbose)
                logWrite("  chain root+%X step %zu (+%X) failed at %p", rootRva, i, offsets[i],
                         reinterpret_cast<void *>(p + offsets[i]));
            return nullptr;
        }
        p = next;
        if (verbose)
            logWrite("  chain root+%X step %zu (+%X) -> %p", rootRva, i, offsets[i],
                     reinterpret_cast<void *>(p));
    }
    return reinterpret_cast<void *>(p);
}

// The stamina structure base, retained so the value can be re-read every tick
// rather than only at resolve time.  Reading once proves nothing: a field that
// happens to equal current stamina while full is indistinguishable from a
// static max-stamina value.
static void *g_staminaBase = nullptr;

// Resolve stamina and record it.  Kept separate from health because the two
// live in different structures; a failure here must not disturb health.
static void resolveStamina() {
    void *base = walkChain(kStaminaChainRootRva, kStaminaChainOffsets, kStaminaChainSteps, true);
    if (!base) {
        // Expected while no character is loaded; the worker retries.
        return;
    }
    uintptr_t field = reinterpret_cast<uintptr_t>(base) + kStaminaFinalOffset;
    uintptr_t dummy = 0;
    if (!plausiblePointer(field) || !readablePointer(field, &dummy)) {
        logWrite("stamina field at %p unreadable", reinterpret_cast<void *>(field));
        return;
    }
    int32_t raw = *reinterpret_cast<const int32_t *>(reinterpret_cast<void *>(field));
    g_staminaBase = base;
    PlayerState &p = g_currentObservation.player;
    p.stamina = raw > 0 ? raw : 0;
    if (raw > p.maxStamina) p.maxStamina = raw;
    p.staminaValid = (p.maxStamina > 0);
    logWrite("stamina base=%p raw=%d", base, raw);
}

// Re-read stamina from the cached base and log movement.  Continuous variation
// under sprinting/rolling is the evidence that this is live stamina rather than
// a static pool size.
static void refreshStamina() {
    if (!g_staminaBase) return;
    uintptr_t field = reinterpret_cast<uintptr_t>(g_staminaBase) + kStaminaFinalOffset;
    uintptr_t dummy = 0;
    if (!plausiblePointer(field) || !readablePointer(field, &dummy)) {
        g_staminaBase = nullptr;
        g_currentObservation.player.staminaValid = false;
        return;
    }
    int32_t raw = *reinterpret_cast<const int32_t *>(reinterpret_cast<void *>(field));
    // The field is a deficit accumulator, not a clamped pool: it goes negative
    // when stamina is overspent (observed down to -25) and refills toward the
    // character's maximum.  Report the clamped value and track the peak as the
    // pool size, since the peak is the true maximum.
    PlayerState &p = g_currentObservation.player;
    if (raw != p.stamina) {
        logWrite("stamina: %d -> %d", p.stamina, raw);
        p.stamina = raw > 0 ? raw : 0;
    }
    if (raw > p.maxStamina) p.maxStamina = raw;
    p.staminaValid = (p.maxStamina > 0);
}

// Refresh the cached snapshot.  Reads only; resolution runs on the worker
// thread so no caller can stall a render thread on it.
static void refreshObservation() {
    if (g_playerBase) {
        g_currentObservation.player.readFrom(g_playerBase);
        if (g_currentObservation.player.valid) g_currentObservation.valid = true;
    }
    g_currentObservation.cameraState = getCameraState();
}

// Driven from the buffer-upload hook, so it must stay cheap: it runs on the
// game's own render threads.  A handful of reads plus a changed-value log.
void hostOnCaptureTick() {
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (now - lastTick < 250) return;
    lastTick = now;
    refreshStamina();
    if (!g_playerBase) return;

    PlayerState &p = g_currentObservation.player;
    int32_t hp = p.hp, maxHp = p.maxHp;
    p.readFrom(g_playerBase);
    if (p.valid) g_currentObservation.valid = true;
    if (p.hp != hp || p.maxHp != maxHp)
        logWrite("health: hp=%d/%d base=%p camera=%s",
                 p.hp, p.maxHp, g_playerBase,
                 getCameraState().valid ? "captured" : "none");
}

bool getViewProjectionMatrix(Matrix4x4 *out) { CameraState cam = getCameraState(); if (cam.valid && out) { *out = cam.viewProjection; return true; } return false; }
const NativeObservation &getObservation() { refreshObservation(); return g_currentObservation; }
CapabilitySet getCapabilities() { return buildCapabilities(getObservation()); }

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
HMODULE waitForModule(const wchar_t *name, int maxAttempts, int sleepMs) { for (int i = 0; i < maxAttempts; ++i) { HMODULE m = GetModuleHandleW(name); if (m) return m; Sleep(sleepMs); } return nullptr; }

void runStartup() {
    logOpen(); logWrite("remastered host starting");
    HMODULE main = GetModuleHandleW(nullptr);
    HMODULE d3d = waitForModule(L"d3d11.dll", 100, 100);
    if (!d3d) { logWrite("FAILED: d3d11.dll never loaded"); return; }
    FARPROC p = GetProcAddress(d3d, "D3D11CreateDevice");
    if (!p) { logWrite("FAILED: D3D11CreateDevice not found"); return; }
    g_realCreateDevice = reinterpret_cast<PfnCreateDevice>(p);
    if (!iat::patch(main, "d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void *>(&hookedCreateDevice))) { logWrite("FAILED: IAT patch for D3D11CreateDevice did not apply"); return; }
    HMODULE dxgi = waitForModule(L"dxgi.dll", 100, 100);
    if (!dxgi) { logWrite("NOTE: dxgi.dll never loaded; swap chain will not be identified"); logWrite("startup complete (DXGI unavailable)"); return; }
    FARPROC fp = GetProcAddress(dxgi, "CreateDXGIFactory");
    if (!fp) { logWrite("NOTE: CreateDXGIFactory not found"); logWrite("startup complete (DXGI factory missing)"); return; }
    g_realCreateFactory = reinterpret_cast<PfnCreateFactory>(fp);
    if (!iat::patch(main, "dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void *>(&hookedCreateFactory))) { logWrite("NOTE: IAT patch for CreateDXGIFactory failed"); } else { logWrite("startup complete (DXGI factory hooked)"); }
    HANDLE t = CreateThread(nullptr, 0, delayedHookInstallThread, nullptr, 0, nullptr);
    if (t) { logWrite("delayed hook installer thread started"); CloseHandle(t); }
}

extern "C" void marioSetupStartEarly();
DWORD WINAPI startupThread(void *param) { (void)param; marioSetupStartEarly(); runStartup(); return 0; }

DWORD WINAPI delayedHookInstallThread(void *param) {
    (void)param; logWrite("delayed hook installer: waiting 15s for anti-cheat scan..."); Sleep(15000);
    logWrite("delayed hook installer: installing vtable hooks now");
    if (!g_device || !g_context) { logWrite("delayed hook installer: device/context not ready, aborting"); return 0; }
    void **devVt = *reinterpret_cast<void ***>(g_device);
    if (devVt) { g_realCreateDeferredContext = reinterpret_cast<decltype(g_realCreateDeferredContext)>(devVt[27]); DWORD p = 0; if (VirtualProtect(&devVt[27], sizeof(void *), PAGE_READWRITE, &p)) { devVt[27] = reinterpret_cast<void *>(&hookedCreateDeferredContext); VirtualProtect(&devVt[27], sizeof(void *), p, &p); logWrite("delayed: device vtable slot 27 patched for CreateDeferredContext"); } }
    armCameraCapture(g_device, g_context); logWrite("delayed: camera capture armed on immediate context");

// Initialise the geometry-pass test draw on the game's own device.  Done here,
// after the anti-cheat delay, on this worker thread -- never from DllMain and
// never from a render thread.  A failure leaves injection disarmed and the host
// continues as observation-only.
    if (initializeInjection(g_device))
        logWrite("delayed: geometry-pass injection armed");
    else
        logWrite("delayed: geometry-pass injection NOT armed (resources failed)");
    logWrite("delayed: patching factory CreateSwapChain hooks");
    for (int i = 0; i < 8; ++i) {
        if (g_factories[i].factory && g_factories[i].vt) {
            DWORD p = 0;
            if (VirtualProtect(&g_factories[i].vt[10], sizeof(void *), PAGE_READWRITE, &p)) { g_factories[i].createSwapChain = g_factories[i].vt[10]; g_factories[i].vt[10] = reinterpret_cast<void *>(&hookedCreateSwapChain); VirtualProtect(&g_factories[i].vt[10], sizeof(void *), p, &p); logWrite("delayed: factory %p vtable slot 10 patched for CreateSwapChain", g_factories[i].factory); }
            if (VirtualProtect(&g_factories[i].vt[15], sizeof(void *), PAGE_READWRITE, &p)) { g_factories[i].vt[15] = reinterpret_cast<void *>(&hookedCreateSwapChain); VirtualProtect(&g_factories[i].vt[15], sizeof(void *), p, &p); }
            if (VirtualProtect(&g_factories[i].vt[16], sizeof(void *), PAGE_READWRITE, &p)) { g_factories[i].vt[16] = reinterpret_cast<void *>(&hookedCreateSwapChain); VirtualProtect(&g_factories[i].vt[16], sizeof(void *), p, &p); }
            logWrite("delayed: factory %p vtable slots 10/15/16 patched", g_factories[i].factory);
        }
    }
    if (g_swapChain) { void **scVt = *reinterpret_cast<void ***>(g_swapChain); if (scVt) { g_realPresent = reinterpret_cast<decltype(g_realPresent)>(scVt[8]); DWORD p = 0; if (VirtualProtect(&scVt[8], sizeof(void *), PAGE_READWRITE, &p)) { scVt[8] = reinterpret_cast<void *>(&hookedPresent); VirtualProtect(&scVt[8], sizeof(void *), p, &p); logWrite("delayed: swap chain vtable slot 8 patched for Present"); } } } else { logWrite("delayed: no swap chain captured; observation is driven by this worker and by buffer uploads"); }
    logWrite("delayed hook installation complete");

    // Observation worker.  The swap chain is created before these hooks exist
    // and IDXGIFactory2::EnumSwapChains does not report it, so Present is not a
    // dependable per-frame driver; polling from here is.  Pointer resolution
    // runs on this thread rather than on a game render thread.
    //
    // The player chain is re-resolved rather than cached forever: it resolves to
    // null while no character is loaded, and the chain can change when a
    // different area or save is entered.  A base that stops yielding a valid
    // player is dropped so the next attempt can find the new one.
    DWORD nextResolve = 0, nextReport = GetTickCount() + 10000;
    for (;;) {
        Sleep(250);
        DWORD now = GetTickCount();

        if (!g_playerBase && now >= nextResolve) {
            nextResolve = now + 2000;
            if (!g_staminaBase) resolveStamina();
            void *base = resolvePlayerBase();
            if (base) {
                PlayerState probe;
                if (probe.readFrom(base) && probe.maxHp > 0) {
                    g_playerBase = base;
                    logWrite("player base resolved: %p hp=%d/%d stamina=%d/%d",
                             base, probe.hp, probe.maxHp, probe.stamina, probe.maxStamina);
                } else {
                    logWrite("player chain resolved to %p but values are not a player "
                             "(hp=%d/%d stamina=%d/%d); discarding",
                             base, probe.hp, probe.maxHp, probe.stamina, probe.maxStamina);
                }
            }
        }

        // Drop a base that stopped looking like a player, so a level change
        // re-resolves instead of logging nonsense forever.
        if (g_playerBase) {
            g_currentObservation.player.readFrom(g_playerBase);
            if (!g_currentObservation.player.valid) {
                logWrite("player base %p no longer valid; will re-resolve", g_playerBase);
                g_playerBase = nullptr;
            }
        }

        {
            CameraState cam = getCameraState();
            worldObservationTick(g_playerBase, cam.valid ? cam.viewProjection.m : nullptr);
        }
        // Eye-position probe window: 1 s every 10 s once the camera is known.
        {
            static DWORD probeStart = 0, probeNext = 0;
            WorldObservation w = g_world;
            if (!g_eyeProbeActive && w.cameraValid && now >= probeNext) {
                for (int i = 0; i < 3; ++i) g_eyeProbeTarget[i] = w.camera[i];
                InterlockedExchange(&g_eyeProbeCount, 0);
                probeStart = now;
                InterlockedExchange(&g_eyeProbeActive, 1);
            } else if (g_eyeProbeActive && now - probeStart >= 1000) {
                InterlockedExchange(&g_eyeProbeActive, 0);
                probeNext = now + 9000;
                LONG n = InterlockedCompareExchange(&g_eyeProbeCount, 0, 0);
                char line[900];
                int o = _snprintf(line, sizeof(line), "eye: probe target=(%.2f,%.2f,%.2f) hits:",
                                  g_eyeProbeTarget[0], g_eyeProbeTarget[1], g_eyeProbeTarget[2]);
                for (LONG k = 0; k < n && k < 48 && o > 0 && o < 840; ++k)
                    o += _snprintf(line + o, sizeof(line) - o, " [%uB +0x%X x%ld]", g_eyeProbe[k].size,
                                   g_eyeProbe[k].offset, g_eyeProbe[k].hits);
                logWrite("%s", line);
            }
        }
        hostOnCaptureTick();
        if (now >= nextReport) {
            nextReport = now + 10000;
            logWrite("status: health=%s stamina=%s camera=%s bufferWidthsSeen=%ld",
                     g_currentObservation.player.valid ? "resolved" : "searching",
                     g_currentObservation.player.staminaValid ? "resolved" : "searching",
                     getCameraState().valid ? "captured" : "none",
                     InterlockedCompareExchange(&g_camSizesUsed, 0, 0));
        }
    }
    return 0;
}

BOOL handleDllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved; switch (reason) {
    case DLL_PROCESS_ATTACH: { DisableThreadLibraryCalls(instance); logOpen(); wchar_t path[MAX_PATH] = {}; if (GetSystemDirectoryW(path, MAX_PATH)) { wcscat_s(path, MAX_PATH, L"\\dinput8.dll"); g_realDirectInput = LoadLibraryW(path); } if (!g_realDirectInput) g_realDirectInput = LoadLibraryW(L"dinput8.dll"); if (g_realDirectInput) g_realDI8Create = reinterpret_cast<void *>(GetProcAddress(g_realDirectInput, "DirectInput8Create")); logWrite("attach: real dinput8=%p", (void *)g_realDirectInput); HANDLE t = CreateThread(nullptr, 0, startupThread, nullptr, 0, nullptr); if (t) CloseHandle(t); break; }
    case DLL_PROCESS_DETACH: if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; } break; } return TRUE;
}

} // namespace remastered

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return remastered::handleDllMain(instance, reason, reserved); }
extern "C" void marioWrapDirectInput(void *di);
extern "C" __declspec(dllexport) HRESULT STDMETHODCALLTYPE DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID out, LPUNKNOWN outer) {
    if (!remastered::g_realDI8Create) return E_FAIL;
    auto fn = reinterpret_cast<HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID, LPUNKNOWN)>(remastered::g_realDI8Create);
    const HRESULT hr = fn(instance, version, riid, out, outer);
    // Keyboard/mouse filter for Mario's drive mode (mario_presentation.h).
    if (SUCCEEDED(hr) && out && *static_cast<void **>(out)) marioWrapDirectInput(*static_cast<void **>(out));
    return hr;
}