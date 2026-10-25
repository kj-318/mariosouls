#pragma once

// Remastered host loader.
//
// The host loads as a dinput8.dll proxy beside DarkSoulsRemastered.exe, the
// same arrangement as the observation probes.  IAT patching redirects
// D3D11CreateDevice and CreateDXGIFactory so the host can observe and hook
// the D3D11 device and DXGI swap chain.
//
// This is the composition root's entry point.  All heavy work runs on a
// startup thread; DllMain stays small.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "memory.h"

namespace remastered {

// Forward declarations for the composition root.
void onDeviceCreated(ID3D11Device *device, ID3D11DeviceContext *context);
void onSwapChainCreated(IDXGISwapChain *swapChain);
void onFramePresent();

// IAT patching utility (same approach as the probes).
namespace iat {
bool patch(HMODULE module, const char *wantedDll, const char *wantedFunction,
           void *replacement);
}

// Startup thread entry point.
DWORD WINAPI startupThread(void *param);

// DllMain handler (called from the actual DllMain).
BOOL handleDllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);

} // namespace remastered