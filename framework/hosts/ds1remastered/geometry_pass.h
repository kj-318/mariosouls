#pragma once

// Remastered geometry-pass injection.
//
// A minimal, self-contained proof that the host can issue its own draw into the
// game's G-buffer: one large magenta triangle drawn with the host's own shaders
// and state, injected immediately after a geometry-pass draw call, with every
// piece of touched state saved beforehand and restored afterwards.
//
// Scope and intent
// ----------------
// This is a *test draw*. It establishes only that host-issued draw calls reach
// the G-buffer and are picked up by the game's later lighting pass. It uses clip
// space directly, so it is not world-anchored, is not occluded correctly, and
// does not involve Mario, the guest adapter, or the neutral renderer interface.
//
// Safety rules observed here, each learned from a crash in this project:
//   - Interfaces are obtained through QueryInterface, never assumed.
//   - Methods are called only at documented vtable indices; unknown slots are
//     never invoked speculatively.
//   - All state the host touches is read, then restored, before returning.
//   - Nothing is called from DllMain, and no hook runs before the anti-cheat
//     delay has elapsed.
//   - Any single failure disables injection for the rest of the session rather
//     than retrying into an unknown state.

#include "native_types.h"

#include <d3d11.h>

namespace remastered {

// Provided by camera_capture: reads a view's attached resource descriptor using
// the proven QueryInterface + documented GetDesc sequence.
bool hostReadViewResourceDesc(void *view, unsigned int *width, unsigned int *height,
                              unsigned int *format, unsigned int *sampleCount,
                              unsigned int *usage, unsigned int *bindFlags);

// Identifies the attachments a geometry pass uses.  Format values are the
// observed DXGI_FORMAT numbers for this title (see docs/REMASTERED_FINDINGS.md
// section 20): 26 = R16G16B16A16_FLOAT, 44 = D24_UNORM_S8_UINT.
struct GeometryPassSignature {
    UINT width = 0;
    UINT height = 0;
    UINT colorFormat = 26;  // R16G16B16A16_FLOAT
    UINT depthFormat = 44;  // D24_UNORM_S8_UINT
};

// Whether a binding looks like the geometry pass.  Deliberately conservative:
// both a colour target and a depth buffer must be present at full resolution,
// because injecting into the wrong pass would corrupt the frame.
bool looksLikeGeometryPass(UINT rtCount, const void *const *rtv, void *depthView);

// One-time initialisation of host-owned D3D11 resources.  Safe to call from any
// thread; does nothing after the first successful attempt.
bool initializeInjection(ID3D11Device *device);

// Whether injection is currently armed.  False if initialisation failed, so a
// failed session degrades to observation instead of retrying into a bad state.
bool injectionArmed();

// Draw the test triangle on a context whose geometry-pass targets are still
// bound (called just before the game rebinds away from them).  Saves and
// restores all pipeline state it touches.  A no-op unless injection is armed.
void drawTestTriangle(ID3D11DeviceContext *context, UINT boundTargets, void *colorView,
                      void *depthView);

// Counters for the log, so the host can report what actually happened.
struct InjectionStats {
    int64_t attempts;
    int64_t injected;
    int64_t skippedNotGeometryPass;
    int64_t skippedNoArm;
};

InjectionStats getInjectionStats();

} // namespace remastered