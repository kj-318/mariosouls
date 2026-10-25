#pragma once

// Remastered host native type definitions.
//
// These are placeholders until real native structures are discovered from a live
// Remastered session.  All offsets and layouts here are UNVERIFIED and must be
// replaced with measured values.  No gameplay code may depend on these until
// they are validated.

#include <cstdint>
#include <cstddef>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remastered {

// Placeholder native actor handle.  Remastered's actor structure is not yet
// discovered; this is a typed pointer alias for clarity.
using NativeActor = void *;

// 4x4 matrix (row-major, 16 floats).  Used for view-projection capture.
struct Matrix4x4 {
    float m[16];
};

// Camera state captured from GPU buffers.
// The view-projection matrix is captured from dynamic constant buffers
// (size 2288 or 4112) via Map/Unmap hook.  It is a combined view-projection
// matrix that changes with camera movement.
struct CameraState {
    Matrix4x4 viewProjection;  // Combined view-projection matrix
    bool valid = false;        // True when a matrix has been captured
    uint64_t lastUpdate = 0;   // Frame counter of last update
    // Camera and player world positions read at the moment this matrix was
    // uploaded, so the pair belongs to the same frame.  The camera position in
    // game memory runs one frame ahead of the image, so reading it at draw time
    // made player-anchored geometry snap when the camera turned (live,
    // 2026-10-19).
    // Second matrix in the same buffer (floats 48-63).  It differs from the
    // first only slightly from frame to frame, so one of the two is most likely
    // the previous frame's view-projection (kept for motion vectors).
    Matrix4x4 viewProjectionAlt;
    // The player character's own draw transform, found among the per-instance
    // 3x4 matrices (from float 112) of a main-scene buffer upload, together with
    // the view-projection from that SAME upload.  Its translation is the
    // character's rendered position relative to the camera, so the pair needs no
    // camera world position and cannot be out of step with the image.
    bool renderPlayerValid = false;
    float renderPlayerVp[16] = {};
    float renderPlayerRel[3] = {};   // camera-relative translation
    float renderPlayerRot[9] = {};   // rotation/scale rows (3x3)
    float renderPlayerError = 0.0f;  // distance from the memory-based estimate
    bool positionsValid = false;
    float cameraPos[3] = {};
    float playerPos[3] = {};
};

// Placeholder native camera handle.
using NativeCamera = void *;

// Placeholder native physics handle.
using NativePhysics = void *;

// Player health and stamina come from two different native structures, reached
// through two separate static pointer chains.  They are therefore reported
// independently: health can be valid while stamina is not.
struct PlayerState {
    // Health structure, reached via the chain in runtime.cpp.
    // Verified live: HP moved 453 -> 401 -> 594 while taking damage and healing,
    // and MaxHP matched the character's real maximum.
    static constexpr ptrdiff_t kHpOffset = 0x3E8;
    static constexpr ptrdiff_t kMaxHpOffset = 0x3EC;

    // These two offsets are NOT valid.  An earlier reading of them returned a
    // constant 49 that never moved during play while the real stamina pool was
    // 95; stamina actually lives in a separate structure behind its own chain.
    // Kept only so the mistake stays documented in the type.
    static constexpr ptrdiff_t kStaminaOffset = 0x3F0;      // WRONG
    static constexpr ptrdiff_t kMaxStaminaOffset = 0x3F4;   // WRONG

    // Stamina, from a separate structure and its own static chain.  Verified
    // live: it drained 95 -> 65 -> 36 -> 6 under sprinting and refilled 6 -> 95.
    //
    // The native field is a deficit accumulator, not a clamped pool: it reaches
    // negative values (observed to -25) when stamina is overspent, then refills
    // toward the character's maximum.  `stamina` here is clamped at 0, and
    // `maxStamina` tracks the observed peak, which is the true pool size.
    int32_t hp = 0;
    int32_t maxHp = 0;
    int32_t stamina = 0;
    int32_t maxStamina = 0;
    bool valid = false;         // health structure resolved
    bool staminaValid = false;  // stamina structure resolved

    // Read health from the health structure base.
    // Returns false if base is null or read fails.
    bool readFrom(void *base) {
        if (!base)
            return false;
        uint8_t *b = static_cast<uint8_t *>(base);
        // Validate pointers before dereferencing (cross-platform, no SEH)
        if (!isReadable(b + kHpOffset, 4) || !isReadable(b + kMaxHpOffset, 4)) {
            valid = false;
            return false;
        }
        hp = *reinterpret_cast<int32_t *>(b + kHpOffset);
        maxHp = *reinterpret_cast<int32_t *>(b + kMaxHpOffset);
        // A player is at full health on load and can be at 0 HP only while dead,
        // so maxHp must stay positive and hp may not exceed it.
        valid = (maxHp > 0 && hp >= 0 && hp <= maxHp);
        return valid;
    }

private:
    // Cross-platform memory readability check
    static bool isReadable(const void *ptr, size_t size) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        DWORD protect = mbi.Protect;
        return (mbi.State == MEM_COMMIT) &&
               !(protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
               (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
    }
};

// A native observation snapshot.  All fields are unverified placeholders
// except player state which uses verified offsets.
struct NativeObservation {
    NativeActor actor = nullptr;
    NativeCamera camera = nullptr;
    NativePhysics physics = nullptr;
    PlayerState player;
    CameraState cameraState;
    bool valid = false;
};

// Capability flags derived from verified observations.  These mirror the
// SCHost::Capabilities interface but are populated from Remastered-specific
// evidence only.
enum class HostCapability : uint32_t {
    ActorPose = 1 << 0,
    Visibility = 1 << 1,
    Motion = 1 << 2,
    Health = 1 << 3,
    Stamina = 1 << 4,
    NativeDamage = 1 << 5,
    Interactions = 1 << 6,
    InputRouting = 1 << 7,
    DynamicSupport = 1 << 8,
    SceneDepth = 1 << 9,
    BeforeHUD = 1 << 10,
};

} // namespace remastered