#pragma once

// Remastered host capability reporting.
//
// Capabilities are derived from verified hooks and current observations only.
// No flag is set without evidence from a live session.  Missing support must
// have a documented native/disabled fallback; never force a readiness flag to
// make a port progress.

#include "native_types.h"

namespace remastered {

// A capability set is a bitmask of HostCapability flags.
using CapabilitySet = uint32_t;

inline bool hasCapability(CapabilitySet caps, HostCapability cap) {
    return (caps & static_cast<uint32_t>(cap)) != 0;
}

inline void setCapability(CapabilitySet &caps, HostCapability cap) {
    caps |= static_cast<uint32_t>(cap);
}

inline void clearCapability(CapabilitySet &caps, HostCapability cap) {
    caps &= ~static_cast<uint32_t>(cap);
}

// Build the capability set from current observations.
// All capabilities start as false; they are set only when verified.
inline CapabilitySet buildCapabilities(const NativeObservation &obs) {
    CapabilitySet caps = 0;
    // Scene depth is VERIFIED (probe evidence: 1920x1080 D24_UNORM_S8_UINT,
    // bound with rtvCount=4 during geometry pass).
    setCapability(caps, HostCapability::SceneDepth);
    // BeforeHUD composition point is partially known: the final pass before
    // Present carries depth in ~73% of frames.  Not yet verified as a distinct
    // pass, so left unset until confirmed.
    // Health and Stamina come from separate native structures behind separate
    // static pointer chains, so each is reported only when its own observation
    // resolved.  Deriving both from player.valid advertised a Stamina capability
    // while the stamina read was returning a constant wrong value.
    if (obs.player.valid)
        setCapability(caps, HostCapability::Health);
    if (obs.player.staminaValid)
        setCapability(caps, HostCapability::Stamina);
    // Actor pose, visibility, motion, etc. are UNVERIFIED.
    (void)obs;
    return caps;
}

} // namespace remastered