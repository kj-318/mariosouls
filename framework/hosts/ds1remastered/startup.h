#pragma once

// Remastered host startup.
//
// All startup work runs on a dedicated thread created from DllMain.  The
// thread waits for d3d11.dll and dxgi.dll to load, then patches the IAT
// to intercept D3D11CreateDevice and CreateDXGIFactory.

#include <windows.h>

namespace remastered {

// Wait for a module to load, with timeout.
HMODULE waitForModule(const wchar_t *name, int maxAttempts = 1200, int sleepMs = 50);

// Startup sequence: patch IAT, wait for device creation, arm hooks.
void runStartup();

} // namespace remastered