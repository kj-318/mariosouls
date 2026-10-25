#pragma once

// Remastered Mario presentation (first milestone: pose-only, native locomotion).
//
// Loads the existing x64 character adapter (SDK ABI4) and its libsm64 runtime,
// poses one guest instance from the observed player position/facing at 30 Hz on
// a dedicated thread, and publishes CPU vertices for the render thread.  Nothing
// here steps guest physics, reads input or writes game memory; the native
// character stays visible (hiding it is a separate, later decision).
//
// Reused unchanged: sdk/character_api.h, core/validation.h (SC::api,
// SC::descriptor, SC::frame), core/character_storage.h (MeshBuffers) and
// core/character_presentation.h (mesh assembly + shading term), following
// docs/REMASTERED_HANDOFF.md.  The adapter/runtime are the x64 builds produced
// by `build --check-portability` (work/framework-build/portability/...).
//
// Configuration: %LOCALAPPDATA%\MarioSoulsProbe\remastered.ini
//   [MarioSouls]
//   Adapter=<path to x64 mario64.dll>
//   Runtime=<path to x64 sm64.dll>
//   ROM=<path to the owned US Super Mario 64 ROM, big-endian .z64, 8 MB>
// Missing or invalid configuration leaves presentation off and is logged.

#include "fx_probe.h"
#include "obj_probe.h"
#include "../../sdk/character_api.h"
#include "../../sdk/environment_api.h"
#include "../../core/validation.h"
#include "../../core/character_storage.h"
#include "../../core/character_presentation.h"
#include "logging.h"
#include "world_observation.h"
#include "native_motion.h"
#include "terrain.h"
#include "mario_combat.h"
#include "mario_setup.h"
#include "action_probe.h"
#include "native_actions.h"
#include "install.h"

#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <cmath>
#include <vector>

namespace remastered {
namespace mario {

struct Adapter {
    HMODULE module = nullptr;
    const SC_CharacterAPI *api = nullptr;
    const SC_Descriptor *profile = nullptr;
    SCCharacterStorage::MeshBuffers mesh;
};

// Published frame: vertices relative to the player's feet (metres), so the
// render thread can place them at the position paired with its own matrix.
// Poses are produced at SM64's 30 Hz; the render thread blends each vertex
// between the previous and current pose by the time since the current one was
// published, so motion is smooth at the game's 60 fps (the same policy as
// PTDE's visual interpolation, core/mesh_interpolation.h).  Blending applies
// only when both poses have the same triangle count, UVs and colours.
struct Frame {
    std::vector<SCRenderAPI::Vertex> vertices, previous;
    uint32_t triangles = 0;
    bool paired = false;
    double stamp = 0;  // seconds, QueryPerformanceCounter
    uint64_t sequence = 0;
};

inline double nowSeconds() {
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(frequency.QuadPart);
}

// Native facing angle -> SDK yaw.  Remastered needs no offset (Mario faced the
// camera with PTDE's angle - pi, live 2026-10-19); yawflip adds pi back.
inline volatile LONG g_yawFlip = 0;
// mario=step (live): SM64 simulates Mario from the controller (Phase 1 of
// work/dsr/SM64_MARIO_PLAN.md).  The knight stays authoritative for position:
// each tick Mario is resolved onto the knight horizontally, keeping SM64's
// momentum, action, animation and jump height.  mario=pose keeps the walk-clip
// presentation.  stepcam=flip / stepx=flip / stepy=flip correct axis
// conventions live.
inline volatile LONG g_stepMode = 0;
inline volatile LONG g_stepCamFlip = 0, g_stepXFlip = 0, g_stepYFlip = 0;
// drive=warp|pos (live; default off): Phase 2 experiment.  Mario leads: each
// tick the knight is moved to Mario, by the game's warp request (ChrMapData
// +0x108 flag, +0x110 XYZ; DSR-Gadget) or by writing ChrPosData +0x10 XYZ
// directly.  The knight is read back the next tick; if the game did not put
// him where asked (a wall), Mario is corrected to him.  These are game-memory
// writes, made only on the validated live player.
inline volatile LONG g_drive = 0;  // 0 off, 1 warp, 2 pos, 3 input, 4 native
// drive=input: the knight is steered by his own (filtered) stick toward Mario,
// so native locomotion and collision move him; Mario is held within a tether
// of the knight.  (Warp ignores collision; a ChrPosData write is overwritten by
// the game's physics: both live 2026-10-21.)
inline volatile LONG g_knightStickX = 0, g_knightStickY = 0;  // -32767..32767
inline volatile LONG g_knightXFlip = 0;
// tether=<metres> (live, default 0.8): free gap between Mario and the knight
// before Mario is held; when the knight is blocked (a wall) the hold is 0.1 m.
// A fixed 0.6 m let him sink into walls; 0.25 m everywhere yanked him back and
// forth (live 2026-10-21).  Millimetres.
inline volatile LONG g_tetherMm = 800;
// Knight sprints (B held for him) while Mario runs fast.  Held at least 0.6 s
// so a release can never read as a roll.
inline volatile LONG g_knightSprint = 0;
// stepspeed=<0.1..1> (live, default 0.5): the stick is scaled before SM64 sees
// it, so his top speed (about 9.6 m/s at full stick) drops while his
// animations still match his speed.  Stored in thousandths.
inline volatile LONG g_stepSpeedMilli = 1000;
// travel=<0.5..1> (live, default 1; drive=native only): SM64 runs unchanged
// (speeds, dive and run thresholds, animations) and his movement is mapped
// into the world at this scale, so he covers less ground per move.  Lowering
// the stick instead lost the run (below 0.85) and the dive (below about 0.91,
// SM64 needs forward speed 29 of 32): live 2026-10-21.  Thousandths.
inline volatile LONG g_travelMilli = 1000;
// terrain=off (live) keeps the flat floor patch; terrainflip=off keeps the
// cache's triangle winding (PTDE flips by default, FlipWinding=1).
inline volatile LONG g_terrainOn = 1, g_terrainFlip = 1;
// In-game switch for driving (L3+R3 held 1 s, as in PTDE): menus need the
// stick, so driving and the knight's input filter can be turned off and on.
inline volatile LONG g_driveActive = 1;
inline volatile LONG g_diagMotion = 0;  // motionprobe=on
struct MotionProbe;
// Logs every place the drive moves Mario back to the knight (1 per second
// per reason is plenty), with the knight's own motion-call age.
inline void snapLog(const char *why, float gap, float knightY, float marioY, float k);
// Ledge grab hold (ticks left) and the stick's angle from Mario's facing.
inline int g_grabHold = 0;
inline float g_grabStickDiff = -1.f;
// Floating knight's support patch (event floors): on, and its height (world m).
inline bool g_patchOn = false;
inline float g_patchY = 0;
inline float g_patchX = 0, g_patchZ = 0;  // centre (world m): where the knight was last held on it
// Last tick a support patch was in use: event floors (native-only collision) are around there.
inline DWORD g_patchSeenTick = 0;
// Native floor probe: while Mario is on a support patch the knight is sent this far below him.
// A real floor, a moving lift or a stair holds him above it; only with no floor does he reach
// it.  (5 cm, PTDE's, matched a lift going down and 5 cm stair steps: patches dropped, v138.)
// Adaptive: 0.35 m normally (a deeper pull dragged the knight down stairs, Mario could not climb
// them), up to 1 m while the knight is already going down fast (lifts move up to ~0.47 m a tick;
// a 0.35 m probe dropped them).
inline float g_floorProbe = 0.35f;
inline float g_patchDescent = 0.f;  // the patch's last downward step (m per tick), decaying
// Near an event floor (a support patch within 3.5 m in the last 4 s): the cached geometry around
// it ends in seams a few cm above the native floor, which are not ledges (v138: grabbed them).
inline bool nearEventFloor(const float *p) {
    if (!g_patchSeenTick || GetTickCount() - g_patchSeenTick > 4000) return false;
    const float dx = p[0] - g_patchX, dz = p[2] - g_patchZ;
    return dx * dx + dz * dz < 3.5f * 3.5f;
}
// Off the map (no cached floor within 15 m under a falling Mario): the knight
// gets his gravity back and falls natively, to the death plane like the game's
// own falls; Mario falls on his own (v111: he fell forever with the floating
// knight).
inline bool g_fallHandoff = false;
inline float g_lastTargetY = 0;  // the knight's last published height (world m)
inline void motionProbeTick(const float player[3]);

// The game's XInputGetState (import table) is wrapped: while Mario drives,
// the knight gets a neutral left stick and no A, B or LT, so he does not move,
// jump-roll or parry on his own.  Mario reads the pad through his own
// XInput handle (readPad), which the wrapper does not touch.
// Mario simulated for a live player in the last half second (not the title
// screen or a load, where the knight must get the controls: the main menu
// could not be used with the controller, v103).
inline volatile DWORD g_stepHeartbeat = 0;
inline volatile LONG g_padLatch = 0;  // buttons seen pressed by the game's poll since the last tick
// Native footing (PTDE GroundSettle, v159): since when Mario is grounded and the knight's
// height steady (0 = not settled).  Casts, items and interactions wait for 150 ms of it
// (1000 ms at most), so native falling on stairs or slopes does not cancel them.
inline volatile DWORD g_footSince = 0;
inline float g_wallSkirtMaxNy = 0.45f;  // steeper than about 63 degrees from flat is a wall (v170m)
inline volatile LONG g_ladderFwdCm = 25, g_ladderUpCm = 0;  // ladderfwd= / ladderup= (cm, toward the ladder / up)
inline volatile LONG g_wingCapOn = 1;  // wingcap=off
inline volatile LONG g_airWallFixOn = 1;  // airwall=off
inline volatile LONG g_wallSkirtOn = 1;  // wallfix=off: no vertical stand-ins for leaning walls
inline volatile LONG g_killRequest = 0;   // lethal surface touched (pose thread -> game thread)
inline volatile LONG g_deathPlaneOn = 1;  // deathplane=off disables
inline volatile LONG g_ladderReachCm = 22, g_ladderLiftCm = 0;  // ladderreach=, ladderlift= (cm)
inline volatile LONG g_ladderSince = 0;      // nonzero while the knight is on a ladder (owned)
inline volatile LONG g_ladderFastPct = 170;  // ladderfast=<percent>: climb speed with B held (100 off)
inline volatile LONG g_footWaitOn = 1;  // footwait=off disables
inline volatile LONG g_envOn = 1;       // lava=off disables the lava reaction
inline volatile LONG g_stepLiftCm = 12;  // stepassist=<cm>: lift for a knight held at a step edge (0 off)
inline bool footingFor(DWORD ms) {
    const DWORD f = g_footSince;
    return !g_footWaitOn || (f && GetTickCount() - f >= ms);
}
inline bool drivingFiltersInput() {
    // Not while the game owns the knight (an interaction, item use or menu,
    // v118): the pad and keys are the game's then (menus could not be used).
    return g_stepMode && g_drive && g_driveActive && GetTickCount() - g_stepHeartbeat < 500 && !nativeact::g_owned;
}

using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
inline XInputGetStateFn g_gameXInputGetState = nullptr;
inline volatile LONG g_inputFiltered = 0;
// Fire Flower: a fresh LB press sets g_fireRequest; the step loop throws the host fireball.
inline volatile LONG g_fireRequest = 0;
// Host-owned bouncing fireball (PTDE-style), world metres, step-mode frame.
// Profile values from characters/mario64/profiles.h flowerProjectile (cm, s).
struct HostFire {
    bool active = false;
    float pos[3] = {}, vel[3] = {};
    int bounces = 0;
    DWORD born = 0;
};
inline HostFire g_fire;
// Off: the DSR fireball spell is used instead (bounced in mariosetup::bulletBounceTick).
inline bool g_hostFireOn = false;
// Mario root in world metres (same frame as g_fire.pos), set each pose tick.  copyFrame
// draws the fireball at g_fire.pos - g_marioRootW in the mesh frame, which is offset by origin.
inline float g_marioRootW[3] = {};
inline void staminaHoldTick();  // below (v170e)
inline DWORD WINAPI filteredXInputGetState(DWORD user, XINPUT_STATE *state) {
    const DWORD r = g_gameXInputGetState ? g_gameXInputGetState(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    // Only while Mario drives: presses made in a menu must not reach him afterwards.
    if (user == 0 && r == ERROR_SUCCESS && state) {
        if (drivingFiltersInput()) InterlockedOr(&g_padLatch, state->Gamepad.wButtons);
        else InterlockedExchange(&g_padLatch, 0);
    }
    if (user == 0) {
        const bool bHeld = (r == ERROR_SUCCESS && state && (state->Gamepad.wButtons & XINPUT_GAMEPAD_B)) ||
                           ([] { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }() &&
                            (GetAsyncKeyState(VK_SPACE) & 0x8000));
        InterlockedExchange(&mariosetup::g_ladderSpeedPct, (g_ladderSince && nativeact::g_owned && bHeld) ? g_ladderFastPct : 0);
    }
    // The game's thread: apply Mario's queued hits (native damage).
    if (user == 0) {
        mariosetup::start();  // params patched from the title on (idempotent)
        mariocombat::flush();
        mariosetup::giveTick();
        mariosetup::giveWingCapTick();
        mariosetup::testHiddenBodyTick();
        mariosetup::hidePlayerTick();
        mariosetup::equipDiagTick();
        mariosetup::suppressTick();
        mariosetup::bulletWatchTick();
        mariosetup::bulletBounceTick([](const float *p) -> float {
            return terrain::g_loaded == 1 ? terrain::floorBelow(p, 0.5f) : NAN;
        });
        mariosetup::castSpeedTick();
        if (g_stepMode && g_drive && g_driveActive) staminaHoldTick();
        if (InterlockedExchange(&g_killRequest, 0) && g_livePlayerIns && worldobs::readable(g_livePlayerIns + 0x3E8, 4)) {
            volatile int *hp = reinterpret_cast<volatile int *>(g_livePlayerIns + 0x3E8);
            if (*hp > 0) { *hp = 0; logWrite("mario: knight HP set to 0 (lethal surface)"); }
        }
        mariosetup::animLogTick();
        mariosetup::environmentTick();
        if (objprobe::g_on) {
            float op[3], oc[3];
            if (readLivePositions(op, oc)) objprobe::tick(op);
        }
        {
            // Mario shown (driving, or posed at the knight during a native action).
            const bool shown = g_stepMode && g_drive && g_driveActive &&
                               (GetTickCount() - g_stepHeartbeat < 500 || nativeact::g_owned);
            InterlockedExchange(&mariosetup::g_nativeHideArmed, shown ? 1 : 0);
            mariosetup::estusFlaskTick(shown);
            mariosetup::nativeHideTick();
        }
    }
    if (r == ERROR_SUCCESS && state && drivingFiltersInput()) {
        XINPUT_GAMEPAD &pad = state->Gamepad;
        const WORD rawButtons = pad.wButtons;
        pad.sThumbLX = pad.sThumbLY = 0;
        if (g_drive == 3) {
            pad.sThumbLX = static_cast<SHORT>(g_knightStickX);
            pad.sThumbLY = static_cast<SHORT>(g_knightStickY);
        }
        pad.bLeftTrigger = 0;
        pad.bRightTrigger = 0;  // Mario's heavy attack (kick)
        pad.wButtons &= ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_X);
        // Fire Flower in either hand (v159, PTDE 22): LB casts with the flame in the left hand,
        // RB with it in the right (RB is then the knight's, not Mario's punch).  A cast press
        // waits for native footing (150 ms, 1000 ms at most) and is then sent as a short pulse.
        {
            const bool fl = mariosetup::flameLeft(), fr = mariosetup::flameRight();
            const WORD castMask = (fl ? XINPUT_GAMEPAD_LEFT_SHOULDER : 0) | (fr ? XINPUT_GAMEPAD_RIGHT_SHOULDER : 0);
            static WORD prevRaw = 0, pending = 0, pulseBtn = 0;
            static DWORD pendSince = 0;
            static int pulse = 0;
            const DWORD now = GetTickCount();
            const WORD edge = rawButtons & castMask & ~prevRaw;
            prevRaw = rawButtons;
            if (fr && (rawButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (pending && !(pending & castMask)) pending = 0;  // the flame left that hand
            if (pulse > 0) {
                pad.wButtons |= pulseBtn;
                --pulse;
            } else if (pending) {
                pad.wButtons &= ~pending;
                const bool settled = footingFor(150);
                if (settled || now - pendSince > 1000) {
                    pulseBtn = pending;
                    pulse = 6;
                    pad.wButtons |= pulseBtn;
                    InterlockedExchange(&mariosetup::g_castStart, static_cast<LONG>(now));
                    logWrite("mario fire: cast (%s hand) after %lu ms waiting for footing%s", pending == XINPUT_GAMEPAD_LEFT_SHOULDER ? "left" : "right",
                             static_cast<unsigned long>(now - pendSince), settled ? "" : " (timeout)");
                    pending = 0;
                }
            } else if (edge) {
                const WORD b = (edge & XINPUT_GAMEPAD_LEFT_SHOULDER) ? XINPUT_GAMEPAD_LEFT_SHOULDER : XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (footingFor(150)) {
                    InterlockedExchange(&mariosetup::g_castStart, static_cast<LONG>(now));
                } else {
                    pending = b;
                    pendSince = now;
                    pad.wButtons &= ~b;
                }
            }
        }
        if (g_drive == 3 && g_knightSprint) pad.wButtons |= XINPUT_GAMEPAD_B;
        InterlockedIncrement(&g_inputFiltered);
    }
    // Synthetic A (Mario pressed A / Q at a prompt): the knight interacts.  With
    // no pad connected a neutral pad is reported for those few polls.
    if (state && user == 0 && (nativeact::g_synthA > 0 || nativeact::g_synthX > 0)) {
        if (r != ERROR_SUCCESS) { memset(state, 0, sizeof(*state)); }
        static DWORD packet = 0x4000000;
        state->dwPacketNumber = ++packet;
        if (nativeact::g_synthA > 0) { state->Gamepad.wButtons |= XINPUT_GAMEPAD_A; InterlockedDecrement(&nativeact::g_synthA); }
        if (nativeact::g_synthX > 0) { state->Gamepad.wButtons |= XINPUT_GAMEPAD_X; InterlockedDecrement(&nativeact::g_synthX); }
        return ERROR_SUCCESS;
    }
    return r;
}

// Keyboard and mouse (virtual-key codes, live: keyjump=, keyattack=, keycrouch=,
// keywalk=, as hex like 0x20).  Defaults follow the PTDE host: WASD, Space
// jump, left mouse attack, left Ctrl crouch (Z), Alt walk.
// v109: Space dodges (the game's own dodge key), E jumps, left mouse punches
// (light attack), right mouse kicks (heavy attack).
inline volatile LONG g_keyJump = 'E', g_keyAttack = VK_LBUTTON, g_keyCrouch = VK_LCONTROL, g_keyWalk = VK_MENU;
inline volatile LONG g_keyHeavy = VK_RBUTTON, g_keyDodge = VK_SPACE;
inline volatile LONG g_dodgeSpeedMilli = 47000;  // dodgespeed= (SM64 units per frame x1000)
// Stamina per punch / kick (punchstamina=, kickstamina=); calibrated against
// the knight's own Caestus attacks (logged as "knight stamina -N").
inline volatile LONG g_punchStamina = 12, g_kickStamina = 20, g_dodgeStamina = 16;
// The knight's stamina (PlayerIns +0x3F8, int; DSR-Gadget 0x3E8 + 0x10).
inline int *knightStamina() {
    const uintptr_t ins = remastered::g_livePlayerIns;
    // (v170: no freshness test - the player check lapses often, and every punch, kick and
    // dodge was then free; Mario could attack forever with an empty bar.)
    if (!ins || !worldobs::readable(ins + 0x3E8, 0x20)) return nullptr;
    int *st = reinterpret_cast<int *>(ins + 0x3F8);
    if (*st < -1000 || *st > 2000) return nullptr;  // not a stamina value
    return st;
}
// Takes the cost if he has any stamina left (the game's rule: an attack
// starts with stamina above zero and may go negative to zero).
// Regeneration hold (v170e): while g_stHoldUntil is ahead, the knight's stamina may only fall
// (g_stCap follows it down); the game thread enforces it every frame.
inline volatile LONG g_stHoldUntil = 0, g_stCap = -1;
inline void staminaHoldFor(DWORD ms) {
    const LONG until = static_cast<LONG>(GetTickCount() + ms);
    if (static_cast<LONG>(until - g_stHoldUntil) > 0) InterlockedExchange(&g_stHoldUntil, until);
}
inline bool staminaHeld() { return static_cast<LONG>(static_cast<DWORD>(g_stHoldUntil) - GetTickCount()) > 0; }
inline void staminaHoldTick() {
    int *s = knightStamina();
    if (!s) return;
    if (!staminaHeld()) { InterlockedExchange(&g_stCap, -1); return; }
    const LONG cap = g_stCap;
    if (cap < 0 || *s < cap) InterlockedExchange(&g_stCap, *s);
    else if (*s > cap) *s = static_cast<int>(cap);
}
inline bool spendStamina(LONG cost) {
    int *s = knightStamina();
    if (!s) {
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 5000) { lastLog = GetTickCount(); logWrite("mario: knight stamina unreadable; move allowed"); }
        return true;  // unknown: do not block Mario
    }
    if (remastered::g_livePlayerIns && worldobs::readable(remastered::g_livePlayerIns + 0x3E8, 4) &&
        *reinterpret_cast<const int *>(remastered::g_livePlayerIns + 0x3E8) <= 0)
        return false;  // dead: no moves (v170n)
    // One move at a time (v170f): nothing more is charged within 300 ms of the last move.
    static DWORD lastSpend = 0;
    if (GetTickCount() - lastSpend < 300) return false;
    int have = *s;
    if (staminaHeld() && g_stCap >= 0 && g_stCap < have) have = static_cast<int>(g_stCap);
    if (have <= 0) return false;  // empty: no attack, kick or dodge until it regenerates
    const int left = have > cost ? have - static_cast<int>(cost) : 0;
    *s = left;
    InterlockedExchange(&g_stCap, left);  // stays there (0 when emptied) until regeneration starts
    lastSpend = GetTickCount();
    staminaHoldFor(500);
    return true;
}
inline bool keyDown(LONG vk) { return vk > 0 && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; }

// ---- Keyboard and mouse filter (DirectInput; we are the dinput8 proxy) ----
// While Mario drives, the knight does not get Mario's keys: WASD, jump,
// crouch, walk, and Mario's mouse attack button.  Camera mouse movement and
// every other key pass through.  F8 held 1 s toggles driving (like L3+R3).
inline void *g_kbDevice = nullptr, *g_mouseDevice = nullptr;
inline void *g_origCreateDevice = nullptr, *g_origGetDeviceState = nullptr, *g_origGetDeviceData = nullptr;
inline volatile LONG g_kbFiltered = 0, g_kbStateCalls = 0, g_kbDataCalls = 0;


// DirectInput key codes (scan codes) Mario uses; mouse buttons by index.
inline bool isMouseVk(LONG vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON; }
inline int marioScanCodes(BYTE out[12]) {
    int n = 0;
    out[n++] = 0x11; out[n++] = 0x1E; out[n++] = 0x1F; out[n++] = 0x20;  // W A S D
    for (LONG vk : {g_keyJump, g_keyCrouch, g_keyWalk, g_keyAttack, g_keyHeavy, g_keyDodge, LONG('R')}) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk <= 0) continue;
        if (vk == g_keyAttack && mariosetup::flameRight()) continue;  // the knight casts with it (v159)
        UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (vk == VK_LCONTROL) sc = 0x1D;
        if (vk == VK_MENU || vk == VK_LMENU) sc = 0x38;
        if (vk == VK_SPACE) sc = 0x39;
        if (sc && n < 12) out[n++] = static_cast<BYTE>(sc);
    }
    return n;
}
// Mouse buttons Mario uses (bit 0 left, 1 right, 2 middle).
inline int marioMouseMask() {
    int mask = 0;
    for (LONG vk : {g_keyAttack, g_keyHeavy, g_keyDodge, g_keyJump}) {
        if (vk == g_keyAttack && mariosetup::flameRight()) continue;  // the knight casts with it (v159)
        mask |= vk == VK_LBUTTON ? 1 : vk == VK_RBUTTON ? 2 : vk == VK_MBUTTON ? 4 : 0;
    }
    return mask;
}

inline HRESULT STDMETHODCALLTYPE filteredGetDeviceState(void *self, DWORD size, void *data) {
    auto fn = reinterpret_cast<HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, void *)>(g_origGetDeviceState);
    const HRESULT hr = fn(self, size, data);
    if (FAILED(hr) || !data || !drivingFiltersInput()) return hr;
    if (self == g_kbDevice && size >= 256) {
        InterlockedIncrement(&g_kbStateCalls);
        BYTE codes[12];
        const int n = marioScanCodes(codes);
        auto *keys = static_cast<BYTE *>(data);
        for (int i = 0; i < n; ++i) keys[codes[i]] = 0;
        InterlockedIncrement(&g_kbFiltered);
    } else if (self == g_mouseDevice && size >= sizeof(DIMOUSESTATE)) {
        const int mask = marioMouseMask();
        for (int b = 0; b < 3; ++b)
            if (mask & (1 << b)) static_cast<DIMOUSESTATE *>(data)->rgbButtons[b] = 0;
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE filteredGetDeviceData(void *self, DWORD objSize, DIDEVICEOBJECTDATA *rgdod,
                                                       DWORD *inOut, DWORD flags) {
    auto fn = reinterpret_cast<HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, DIDEVICEOBJECTDATA *, DWORD *, DWORD)>(
        g_origGetDeviceData);
    const HRESULT hr = fn(self, objSize, rgdod, inOut, flags);
    if (FAILED(hr) || !rgdod || !inOut || !drivingFiltersInput() || objSize < sizeof(DIDEVICEOBJECTDATA)) return hr;
    if (self != g_kbDevice && self != g_mouseDevice) return hr;
    if (self == g_kbDevice) InterlockedIncrement(&g_kbDataCalls);
    BYTE codes[12];
    const int n = marioScanCodes(codes);
    const int mask = marioMouseMask();
    auto *base = reinterpret_cast<BYTE *>(rgdod);
    for (DWORD i = 0; i < *inOut; ++i) {
        auto *e = reinterpret_cast<DIDEVICEOBJECTDATA *>(base + i * objSize);
        bool drop = false;
        if (self == g_kbDevice) {
            for (int k = 0; k < n; ++k) if (e->dwOfs == codes[k]) drop = true;
        } else {
            for (int b = 0; b < 3; ++b)
                if ((mask & (1 << b)) && e->dwOfs == static_cast<DWORD>(DIMOFS_BUTTON0 + b)) drop = true;
        }
        if (drop) e->dwData = 0;  // key/button reported released
    }
    return hr;
}

inline void patchSlot(void **vt, int slot, void *fn, void **orig) {
    if (vt[slot] == fn) return;
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return;
    if (!*orig) *orig = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}

inline HRESULT STDMETHODCALLTYPE wrappedCreateDevice(void *self, REFGUID guid, void **device, void *outer) {
    auto fn = reinterpret_cast<HRESULT(STDMETHODCALLTYPE *)(void *, REFGUID, void **, void *)>(g_origCreateDevice);
    const HRESULT hr = fn(self, guid, device, outer);
    if (FAILED(hr) || !device || !*device) return hr;
    // GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000}, GUID_SysMouse {...2B60...}
    static const GUID kb = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const GUID mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    const bool isKb = memcmp(&guid, &kb, sizeof(GUID)) == 0, isMouse = memcmp(&guid, &mouse, sizeof(GUID)) == 0;
    if (!isKb && !isMouse) return hr;
    if (isKb) g_kbDevice = *device; else g_mouseDevice = *device;
    void **vt = *reinterpret_cast<void ***>(*device);
    patchSlot(vt, 9, reinterpret_cast<void *>(&filteredGetDeviceState), &g_origGetDeviceState);
    patchSlot(vt, 10, reinterpret_cast<void *>(&filteredGetDeviceData), &g_origGetDeviceData);
    logWrite("mario: input filter: game %s device %p wrapped", isKb ? "keyboard" : "mouse", *device);
    return hr;
}

// Called with the IDirectInput8 object the game just created.
inline void wrapDirectInput(void *di) {
    if (!di) return;
    void **vt = *reinterpret_cast<void ***>(di);
    patchSlot(vt, 3, reinterpret_cast<void *>(&wrappedCreateDevice), &g_origCreateDevice);
    logWrite("mario: input filter: DirectInput %p wrapped", di);
}

// Replaces XInputGetState in the game executable's import table.
inline void hookGameXInput() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto *base = reinterpret_cast<uint8_t *>(exe);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { logWrite("mario: input filter: no import table"); return; }
    int patched = 0;
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char *dll = reinterpret_cast<const char *>(base + imp->Name);
        if (_strnicmp(dll, "xinput", 6) != 0) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto *funcs = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            bool match = false;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) match = IMAGE_ORDINAL(names->u1.Ordinal) == 2;  // XInputGetState
            else match = strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name,
                                "XInputGetState") == 0;
            if (!match) continue;
            DWORD old = 0;
            if (!VirtualProtect(&funcs->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            g_gameXInputGetState = reinterpret_cast<XInputGetStateFn>(funcs->u1.Function);
            funcs->u1.Function = reinterpret_cast<ULONG_PTR>(&filteredXInputGetState);
            VirtualProtect(&funcs->u1.Function, sizeof(void *), old, &old);
            ++patched;
            logWrite("mario: input filter: %s XInputGetState wrapped", dll);
        }
    }
    if (!patched) logWrite("mario: input filter: game does not import XInputGetState (not wrapped)");
}

// Controller, read-only (the game reads it too).  Same XInput generations and
// deadzone mapping as the PTDE host (hosts/ds1ptde/controller.h).
struct PadSample { bool connected; float x, y; WORD buttons; BYTE lt, rt; };
inline PadSample readPad() {
    using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
    static GetState getState = nullptr;
    static bool initialized = false;
    static int slot = -1;
    static DWORD scanTime = 0;
    if (!initialized) {
        initialized = true;
        for (const char *name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryA(name);
            if (!module) continue;
            getState = reinterpret_cast<GetState>(reinterpret_cast<void *>(GetProcAddress(module, "XInputGetState")));
            if (getState) { logWrite("mario: controller via %s", name); break; }
        }
    }
    PadSample out = {};
    if (!getState) return out;
    XINPUT_STATE state = {};
    bool ok = false;
    if (slot >= 0 && getState(static_cast<DWORD>(slot), &state) == ERROR_SUCCESS) ok = true;
    else {
        slot = -1;
        const DWORD now = GetTickCount();
        if (now - scanTime >= 1000) {
            scanTime = now;
            for (DWORD i = 0; i < 4 && !ok; ++i) {
                state = {};
                if (getState(i, &state) == ERROR_SUCCESS) { slot = static_cast<int>(i); ok = true; }
            }
        }
    }
    if (!ok) return out;
    const XINPUT_GAMEPAD &pad = state.Gamepad;
    out.connected = true;
    const float x = pad.sThumbLX, y = pad.sThumbLY, length = std::sqrt(x * x + y * y);
    const float deadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    if (length > deadzone) {
        float speed = (length - deadzone) / (32767.f - deadzone);
        if (speed > 1) speed = 1;
        out.x = x / length * speed;
        out.y = y / length * speed;
    }
    // PTDE InputLatch (v159): a tap shorter than a 30 Hz tick, seen by the game's own
    // per-frame poll, is not lost between ticks.
    out.buttons = pad.wButtons | static_cast<WORD>(InterlockedExchange(&g_padLatch, 0));
    out.lt = pad.bLeftTrigger;
    out.rt = pad.bRightTrigger;
    return out;
}

// Game window has focus (no input while alt-tabbed).
inline bool gameFocused() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

inline Adapter g_adapter;
inline CRITICAL_SECTION g_frameLock;
inline Frame g_frame;
inline volatile LONG g_ready = 0;     // adapter initialised; atlas valid
inline volatile LONG g_started = 0;

inline void SC_CALL adapterLog(const char *message) {
    logWrite("mario: adapter: %s", message ? message : "");
}

inline bool readIni(const wchar_t *key, char *out, DWORD size) {
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return false;
    wcscat_s(path, MAX_PATH, L"\\MarioSoulsProbe\\remastered.ini");
    wchar_t value[MAX_PATH] = {};
    GetPrivateProfileStringW(L"MarioSouls", key, L"", value, MAX_PATH, path);
    if (!value[0]) return false;
    return WideCharToMultiByte(CP_ACP, 0, value, -1, out, int(size), nullptr, nullptr) > 0;
}

// ---- Direct SM64 engine calls for Mario's moves (v109) ----
// The runtime (sm64.dll, loaded by the adapter) exports these; the adapter's
// SDK has no command for "set action with argument", facing or speed.  Used
// for the dodge roll, light/heavy attacks and the ledge-grab assist.
namespace sm64direct {
using SetAction = void (*)(int32_t, uint32_t);
using SetActionArg = void (*)(int32_t, uint32_t, uint32_t);
using SetFloat = void (*)(int32_t, float);
using SetVec = void (*)(int32_t, float, float, float);
inline SetVec setVelocity = nullptr;
// PTDE's applyNativeHurt: SM64's hurt reaction (0 none, 1 light, 2 heavy).
using Hurt = void (*)(int32_t, int32_t);
inline Hurt hurt = nullptr;
inline SetAction setAction = nullptr;
inline SetActionArg setActionArg = nullptr;
inline SetFloat setFacing = nullptr, setForwardVelocity = nullptr;
// Water and poison gas levels (SM64 units, s16).  libsm64's find_water_level is a stub that
// returns -10000, set once at spawn: Mario swam whenever his local height fell 10000 units
// (about 150 m) below his origin, as in New Londo.  Dark Souls water is not SM64 water.
using SetLevel = void (*)(int32_t, int);
inline SetLevel setWaterLevel = nullptr, setGasLevel = nullptr;
constexpr int kNoWaterLevel = -32000;
// Caps (v170n, the Wing Cap): libsm64's sm64_mario_interact_cap(id, capFlag, capTime, playMusic)
// and sm64_set_mario_state(id, flags) (MarioState.flags).
using InteractCap = void (*)(int32_t, uint32_t, uint16_t, uint8_t);
using SetState = void (*)(int32_t, uint32_t);
inline InteractCap interactCap = nullptr;
inline SetState setState = nullptr;
constexpr uint32_t MARIO_WING_CAP = 0x08, MARIO_CAP_ON_HEAD = 0x10, MARIO_CAP_IN_HAND = 0x20;
constexpr uint32_t ACT_PUTTING_ON_CAP = 0x0000133D;
inline char g_runtimePath[MAX_PATH] = {};
inline bool ready() {
    static int state = 0;  // 0 untried, 1 bound, -1 unavailable
    if (state) return state > 0;
    HMODULE mod = GetModuleHandleA(g_runtimePath);
    if (!mod) {
        const char *slash = strrchr(g_runtimePath, '\\');
        mod = GetModuleHandleA(slash ? slash + 1 : "sm64.dll");
    }
    if (mod) {
        setAction = reinterpret_cast<SetAction>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_action")));
        setActionArg = reinterpret_cast<SetActionArg>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_action_arg")));
        setFacing = reinterpret_cast<SetFloat>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_faceangle")));
        setForwardVelocity =
            reinterpret_cast<SetFloat>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_forward_velocity")));
        setVelocity = reinterpret_cast<SetVec>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_velocity")));
        hurt = reinterpret_cast<Hurt>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_mario_host_health")));
        setWaterLevel = reinterpret_cast<SetLevel>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_water_level")));
        setGasLevel = reinterpret_cast<SetLevel>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_gas_level")));
        interactCap = reinterpret_cast<InteractCap>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_mario_interact_cap")));
        setState = reinterpret_cast<SetState>(reinterpret_cast<void *>(GetProcAddress(mod, "sm64_set_mario_state")));
        logWrite("mario: water level control %s", setWaterLevel ? "ready" : "missing");
        logWrite("mario: hurt reaction %s", hurt ? "ready" : "missing");
    }
    state = mod && setAction && setActionArg && setFacing && setForwardVelocity && setVelocity ? 1 : -1;
    logWrite("mario: engine moves %s (runtime %p)", state > 0 ? "ready" : "unavailable", mod);
    return state > 0;
}
// SM64 action IDs (decomp include/sm64.h).
constexpr uint32_t ACT_FREEFALL = 0x0100088C, ACT_FORWARD_ROLLOUT = 0x010008A6, ACT_BACKWARD_ROLLOUT = 0x010008AD,
                   ACT_AIR_HIT_WALL = 0x000008A7, ACT_BACKWARD_AIR_KB = 0x010208B0, ACT_SOFT_BONK = 0x010208B8,
                   ACT_LEDGE_GRAB = 0x0800034B, ACT_PUNCHING = 0x00800380, ACT_MOVE_PUNCHING = 0x00800457,
                   ACT_SLIDE_KICK = 0x018008AA, ACT_DIVE = 0x0188088A, ACT_DIVE_SLIDE = 0x00880456,
                   ACT_JUMP_KICK = 0x018008AC;
constexpr uint32_t ACT_GROUP_MASK = 0x1C0, GROUP_STATIONARY = 0x000, GROUP_MOVING = 0x040, GROUP_AIRBORNE = 0x080;
} // namespace sm64direct

// ---- Mario's sounds (v112): the PTDE host's waveOut stream ----
// The adapter mixes SM64's sound effects and voice (sm64_audio_tick) into
// 16-bit stereo at its own rate; the pose thread pumps it every tick.
inline HWAVEOUT g_audioOut = nullptr;
inline WAVEHDR g_audioHeaders[4] = {};
inline int16_t g_audioBuffers[4][2176] = {};
inline void openAudio(uint32_t rate) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&g_audioOut, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_audioOut = nullptr;
        logWrite("mario: audio output unavailable");
        return;
    }
    waveOutSetVolume(g_audioOut, 0x60006000);
    for (int i = 0; i < 4; ++i) {
        g_audioHeaders[i].lpData = reinterpret_cast<LPSTR>(g_audioBuffers[i]);
        g_audioHeaders[i].dwBufferLength = sizeof(g_audioBuffers[i]);
        waveOutPrepareHeader(g_audioOut, &g_audioHeaders[i], sizeof(WAVEHDR));
    }
    logWrite("mario: audio %u Hz", rate);
}
// Silent while the knight is dead (from "YOU DIED" until he is alive again):
// what was queued is cut and no new sound is mixed in (v115).
inline void pumpAudio(const SC_CharacterAPI *api) {
    if (!g_audioOut || !api) return;
    {
        static bool muted = false;
        const uintptr_t ins = g_livePlayerIns;
        const bool dead = ins && GetTickCount() - g_livePlayerChecked < 1000 &&
                          *reinterpret_cast<const int *>(ins + 0x3E8) <= 0;
        if (dead != muted) {
            muted = dead;
            if (dead) waveOutReset(g_audioOut);
            logWrite("mario: audio %s", dead ? "silenced (dead)" : "back");
        }
        if (dead) {
            static int16_t scratch[2176];
            api->audio(0, 1100, scratch, 1088);  // keep the mixer drained
            return;
        }
    }
    unsigned queued = 0;
    int freeIndex = -1;
    for (int i = 0; i < 4; ++i) {
        if (g_audioHeaders[i].dwFlags & WHDR_INQUEUE) queued += g_audioHeaders[i].dwBufferLength / 4;
        else if (freeIndex < 0) freeIndex = i;
    }
    if (freeIndex < 0) return;
    const unsigned n = api->audio(queued, 1100, g_audioBuffers[freeIndex], 1088);
    if (n && n <= 1088) {
        mariocombat::mixImpact(g_audioBuffers[freeIndex], n);  // PTDE impact thump
        g_audioHeaders[freeIndex].dwBufferLength = n * 4;
        waveOutWrite(g_audioOut, &g_audioHeaders[freeIndex], sizeof(WAVEHDR));
    }
}

// Load and initialise the adapter.  Called once, on the worker thread.
inline bool loadAdapter() {
    char adapterPath[MAX_PATH] = {}, runtimePath[MAX_PATH] = {}, romPath[MAX_PATH] = {};
    // v171 (shippable): without remastered.ini, the files next to this DLL (MarioSouls\mario64.dll,
    // MarioSouls\sm64.dll) and the player's own Super Mario 64 (USA) ROM in the game folder.
    // (ANSI paths: LoadLibraryExA and the adapter's ROM path take the system code page.)
    install::startBuild();  // collision caches from this install's own files, first launch
    auto ansi = [](const std::wstring &w, char *out) {
        return WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, out, MAX_PATH, nullptr, nullptr) > 0;
    };
    if (!readIni(L"Adapter", adapterPath, MAX_PATH)) ansi(install::hostDir() + L"\\MarioSouls\\mario64.dll", adapterPath);
    if (!readIni(L"Runtime", runtimePath, MAX_PATH)) ansi(install::hostDir() + L"\\MarioSouls\\sm64.dll", runtimePath);
    if (!readIni(L"ROM", romPath, MAX_PATH)) {
        std::string rom;
        if (install::findRom(rom)) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, rom.c_str(), -1, w, MAX_PATH);
            ansi(w, romPath);
        }
    }
    if (!romPath[0]) {
        logWrite("mario: no Super Mario 64 (USA) ROM found: put your .z64 (or .n64/.v64) in the game folder; presentation off");
        MessageBoxW(nullptr,
                    L"MarioSouls needs your own Super Mario 64 (USA) ROM.\n\nPut the .z64 file (an .n64 or .v64 works too) "
                    L"in the Dark Souls Remastered folder, next to DarkSoulsRemastered.exe, and start the game again.",
                    L"MarioSouls", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return false;
    }
    if (GetFileAttributesA(adapterPath) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(runtimePath) == INVALID_FILE_ATTRIBUTES) {
        logWrite("mario: %s or %s missing: the MarioSouls folder belongs next to dinput8.dll; presentation off", adapterPath,
                 runtimePath);
        return false;
    }
    logWrite("mario: adapter %s, runtime %s, ROM %s", adapterPath, runtimePath, romPath);
    Adapter &a = g_adapter;
    a.module = LoadLibraryExA(adapterPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!a.module) {
        logWrite("mario: adapter load failed: %s error=%lu", adapterPath, GetLastError());
        return false;
    }
    auto entry = reinterpret_cast<SC_GetAPI>(
        reinterpret_cast<void *>(GetProcAddress(a.module, "SoulsCharacterGetAPI")));
    a.api = entry ? entry(SC_ABI_VERSION, sizeof(SC_CharacterAPI)) : nullptr;
    if (!SC::api(a.api) || !SC::descriptor(a.api->describe())) {
        logWrite("mario: adapter ABI/descriptor rejected");
        FreeLibrary(a.module);
        a.module = nullptr;
        a.api = nullptr;
        return false;
    }
    a.profile = a.api->describe();
    a.mesh.allocate(*a.profile);
    strcpy_s(sm64direct::g_runtimePath, runtimePath);
    SC_Init init = {sizeof(SC_Init), romPath, runtimePath, adapterLog};
    SC_Texture atlas = {a.mesh.atlasPixels.data(), uint32_t(a.mesh.atlasPixels.size())};
    if (!a.api->initialize(&init, &atlas)) {
        logWrite("mario: adapter initialisation failed (runtime/ROM); presentation off");
        a.api->shutdown();
        FreeLibrary(a.module);
        a.module = nullptr;
        a.api = nullptr;
        a.profile = nullptr;
        return false;
    }
    if ((a.profile->capabilities & SC_CAP_AUDIO) && a.profile->audioRate) openAudio(a.profile->audioRate);
    mariosetup::start();  // Mario class, Mario's Gear (PTDE starting_class.h)
    logWrite("mario: adapter ready id=%s triangles<=%u atlas=%ux%u caps=0x%X scale=%.2f",
             a.profile->id, a.profile->maxTriangles, a.profile->textureWidth,
             a.profile->textureHeight, a.profile->capabilities, a.profile->body.modelScale);
    return true;
}

// 30 Hz pose loop.  Mirrors PTDE's presentationTick at its essentials: one
// instance created at the local origin, yaw = native angle - pi, WALK while the
// native position moves, NEUTRAL otherwise; the mesh is published only after
// SC::frame validation.  The instance is destroyed when the player is lost.
// HP the landing damage just wrote (-1 none).  The hit reaction re-syncs to it
// so a fall never plays SM64's hurt reaction.
inline int g_landedHp = -1;

inline DWORD WINAPI poseThread(void *) {
    Adapter &a = g_adapter;
    int32_t id = -1;
    float previous[3] = {}, travelCm = 0;
    bool havePrevious = false;
    std::vector<SCRenderAPI::Vertex> scratch;
    uint64_t sequence = 0;
    DWORD lastFail = 0;
    // Exact 30 Hz clock.  Sleep(33) alone runs at about 21 Hz under the
    // default 15.6 ms timer, which slowed every animation (live 2026-10-19).
    const double tick = 1.0 / SC_FIXED_HZ;
    double nextTick = nowSeconds() + tick;
    double walkFrames = 0;           // walk-cycle frames owed, fractional
    // Step mode: instance origin (metres) and mode the instance was made for.
    float stepOrigin[3] = {};
    LONG instanceMode = -1;
    // Moves (v109): last tick's engine state, button edges, timers.
    uint32_t lastAction = 0, lastArg = 0;
    float lastVy = 0;                                      // SM64 units per second, last tick
    float lastFace = 0, lastFwd = 0;                      // radians; SM64 units per frame
    bool prevDodge = false, prevLight = false, prevHeavy = false;
    int lightBuffer = 0, iframeTicks = 0, grabAssist = 0;
    float grabYaw = 0;
    float driveTarget[3] = {};  // where the knight was sent last tick (metres)
    bool nativePublished = false;
    float nativeWanted[3] = {};  // Mario's movement last tick (metres), for wall tests
    unsigned nativeWalls = 0, nativeTicks = 0;
    float lastMarioX = 0, lastMarioY = 0, lastMarioZ = 0;  // local cm, after last tick
    float nativeTargetX = 0, nativeTargetZ = 0;           // world metres, last published
    int shortTicks = 0;                                   // consecutive ticks the knight fell short
    bool lastGrounded = true;                             // Mario on the ground after last tick
    int gapTicks = 0;                                     // grounded ticks with a gap over 0.6 m
    int ledgeTicks = 0;                                   // ticks Mario stood above an unreachable knight
    float terrainCenter[3] = {};                          // world point the static surfaces were built around
    bool terrainBuilt = false;
    float terrainK = 1.0f;
    LONG terrainFlipBuilt = -1;
    LONG skirtBuilt = -1;
    DWORD terrainMapBuilt = 0;  // terrain::g_mapChangedTick when the surfaces were built
    std::vector<uint32_t> terrainIdx;
    std::vector<SC_Surface> terrainSurf;
    bool driveSent = false;
    unsigned driveBlocked = 0, driveTicks = 0;
    DWORD rateWindow = GetTickCount();
    unsigned rateTicks = 0;
    for (;;) {
        double wait = nextTick - nowSeconds();
        if (wait > 0.002) { Sleep(static_cast<DWORD>((wait - 0.001) * 1000.0)); continue; }
        while (nowSeconds() < nextTick) YieldProcessor();
        nextTick += tick;
        // PTDE 07 (v159): at most two ticks of catch-up; a longer stall resyncs.
        if (nowSeconds() - nextTick > 2 * tick) nextTick = nowSeconds() + tick;
        ++rateTicks;
        if (GetTickCount() - rateWindow >= 30000) {
            logWrite("mario: pose rate %.1f Hz", rateTicks * 1000.0 / (GetTickCount() - rateWindow));
            rateWindow = GetTickCount();
            rateTicks = 0;
        }
        float player[3], camera[3];
        WorldObservation w = g_world;
        bool valid = readLivePositions(player, camera) && std::isfinite(w.playerAngle);
        if (valid && g_diagMotion) motionProbeTick(player);
        if (!valid) {
            if (id >= 0) {
                a.api->destroy(id);
                id = -1;
                EnterCriticalSection(&g_frameLock);
                g_frame.triangles = 0;
                LeaveCriticalSection(&g_frameLock);
                logWrite("mario: player lost; instance destroyed");
            }
            havePrevious = false;
            continue;
        }
        // Actual actor replacement (PTDE 13, v159): a new PlayerIns (load, death and respawn)
        // invalidates the support patch, the fall handoff and queued hits; the same actor
        // across a seamless area change keeps them.
        {
            static uintptr_t lastIns = 0;
            const uintptr_t ins = g_livePlayerIns;
            if (ins && ins != lastIns) {
                if (lastIns) {
                    g_patchOn = false;
                    g_fallHandoff = false;
                    g_footSince = 0;
                    mariocombat::clearQueue();
                    // v170n: everything a previous life left running (rapid hits and death broke
                    // Mario's control on respawn): native ownership, holds, timers.
                    nativeact::g_owned = 0;
                    InterlockedExchange(&nativeact::g_synthA, 0);
                    InterlockedExchange(&nativeact::g_synthX, 0);
                    InterlockedExchange(&g_hideHeldItem, 0);
                    InterlockedExchange(&g_ladderSince, 0);
                    InterlockedExchange(&g_stHoldUntil, 0);
                    InterlockedExchange(&g_stCap, -1);
                    InterlockedExchange(&g_killRequest, 0);
                    InterlockedExchange(&mariocombat::g_dodgeUntil, 0);
                    mariosetup::g_hitAt = 0;
                    logWrite("mario: player character replaced (%p -> %p): native state reset",
                             reinterpret_cast<void *>(lastIns), reinterpret_cast<void *>(ins));
                }
                lastIns = ins;
            }
        }
        // Hit stop: hold the last published pose while the hit freezes Mario.
        if (int32_t(GetTickCount() - mariocombat::g_hitStopUntil) < 0) continue;
        // Posed facing the guest's +Z; the live facing is applied per drawn
        // frame (copyFrame), so Mario turns with the player instead of up to
        // two poses later.  Only the lighting uses the pose-time facing.
        const LONG mode = g_stepMode;
        const float yaw = mode ? 0.0f : w.playerAngle + (g_yawFlip ? 3.14159265f : 0.0f);
        // A pose instance and a simulated one differ in state; recreate on a
        // mode change, and re-centre the step origin when far from it.
        if (id >= 0 && (mode != instanceMode ||
                        (mode && std::fabs(player[0] - stepOrigin[0]) + std::fabs(player[2] - stepOrigin[2]) > 300.f))) {
            a.api->destroy(id);
            id = -1;
        }
        if (id < 0) {
            a.api->surfaces(nullptr, 0, 0);
            a.api->surfaces(nullptr, 0, 1);
            if (mode) {
                stepOrigin[0] = player[0]; stepOrigin[1] = player[1]; stepOrigin[2] = player[2];
                // Facing: SDK yaw 0 faces +Z.  The native angle convention is
                // the one marioyaw=flip corrects; use it for the start only.
                id = a.api->create(0, 0, 0, w.playerAngle + (g_yawFlip ? 3.14159265f : 0.0f));
            } else {
                id = a.api->create(0, 0, 0, 0);
            }
            instanceMode = mode;
            if (id < 0) {
                DWORD now = GetTickCount();
                if (now - lastFail > 5000) { lastFail = now; logWrite("mario: create failed"); }
                continue;
            }
            logWrite("mario: instance %d created", id);
            // Breakable objects ready before the first attack (v170: loading on the first hit stalled Mario).
            objprobe::g_scanWanted = 1;
            objprobe::start();
            mariocombat::loadBreakables();
            terrainBuilt = false;
            havePrevious = false;
            travelCm = 0;
        }
        float moved = 0;
        if (havePrevious) {
            float dx = player[0] - previous[0], dz = player[2] - previous[2];
            moved = std::sqrt(dx * dx + dz * dz);
        }
        previous[0] = player[0]; previous[1] = player[1]; previous[2] = player[2];
        havePrevious = true;
        travelCm += moved * 100.0f;
        // Ground speed over the last 6 ticks (0.2 s).  The native position does
        // not change on every 30 Hz tick, so per-tick movement flickered
        // between walk and neutral; each switch restarted SM64's walk cycle
        // ("resetting animation", live 2026-10-19).
        static float recent[6] = {};
        static unsigned recentAt = 0;
        recent[recentAt++ % 6] = moved;
        float window = 0;
        for (float r : recent) window += r;
        const float perTick = window / 6.0f;  // metres per tick, smoothed

        if (mode) {
            // ---- Phase 1: SM64 simulates Mario from the controller ----
            // Knight's feet in local centimetres.
            // Travel scale (drive=native): Mario's space is the world divided
            // by k around the step origin.  1 otherwise.
            const float k = (g_driveActive && g_drive == 4 && nativemotion::g_ready && g_travelMilli > 0)
                                ? g_travelMilli / 1000.0f : 1.0f;
            const float knight[3] = {(player[0] - stepOrigin[0]) * 100.f / k, (player[1] - stepOrigin[1]) * 100.f / k,
                                     (player[2] - stepOrigin[2]) * 100.f / k};
            // Toggling Mario / the knight (L3+R3, F8, F6) changes k (travel scale while driving,
            // 1 otherwise): Mario's local position meant a different world point and he jumped
            // back or down through the floor (v172).  On a change of k he is put on the knight.
            {
                static float prevK = -1.f;
                if (prevK > 0.f && std::fabs(prevK - k) > 1e-4f && id >= 0) {
                    SC_State prev = {};
                    SC_MotionResolution r = {};
                    r.size = sizeof(r);
                    r.position[0] = knight[0]; r.position[1] = knight[1]; r.position[2] = knight[2];
                    a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                    if (sm64direct::setVelocity) sm64direct::setVelocity(id, 0.f, 0.f, 0.f);
                    if (sm64direct::setForwardVelocity) sm64direct::setForwardVelocity(id, 0.f);
                    lastMarioX = knight[0]; lastMarioY = knight[1]; lastMarioZ = knight[2];
                    g_patchOn = false;
                    g_fallHandoff = false;
                    logWrite("mario: control switched (scale %.2f -> %.2f): Mario put on the knight", prevK, k);
                }
                prevK = k;
            }
            // Floor.  With the collision cache (Phase 3): the real static map
            // collision within 40 m of Mario, in his space (world / k around
            // the step origin), rebuilt when he has moved 15 m or k changed.
            // Without it: a 12 m square at the knight's feet every tick.
            terrain::ensureMap(mariosetup::currentMapKey());
            if (terrain::recentMapChange() && nativemotion::g_ready) nativemotion::applyFloat(true, false);
            const bool haveTerrain = g_terrainOn && terrain::g_loaded == 1;
            if (haveTerrain) {
                const float mw[3] = {stepOrigin[0] + lastMarioX * .01f * k, stepOrigin[1] + lastMarioY * .01f * k,
                                     stepOrigin[2] + lastMarioZ * .01f * k};
                const float *c = terrainBuilt ? mw : player;
                const float ddx = c[0] - terrainCenter[0], ddz = c[2] - terrainCenter[2], ddy = c[1] - terrainCenter[1];
                if (!terrainBuilt || ddx * ddx + ddz * ddz > 15.f * 15.f || std::fabs(ddy) > 15.f || terrainK != k ||
                    terrainFlipBuilt != g_terrainFlip || terrainMapBuilt != terrain::g_mapChangedTick || skirtBuilt != g_wallSkirtOn) {
                    terrainCenter[0] = c[0]; terrainCenter[1] = c[1]; terrainCenter[2] = c[2];
                    terrain::query(terrainCenter, 40.f, 40.f, terrainIdx);
                    terrainSurf.clear();
                    terrainSurf.reserve(terrainIdx.size());
                    const bool flip = g_terrainFlip != 0;
                    size_t skirts = 0;
                    for (uint32_t i : terrainIdx) {
                        const terrain::Triangle &t = terrain::g_tris[i];
                        SC_Surface s = {};
                        bool ok = true;
                        for (int v = 0; v < 3; ++v) {
                            const float *p = t.v[flip ? 2 - v : v];
                            for (int ax = 0; ax < 3; ++ax) {
                                const float local = (p[ax] - stepOrigin[ax]) * 100.f / k;
                                if (!(std::fabs(local) < 30000.f)) ok = false;
                                s.vertices[v][ax] = static_cast<int32_t>(std::lround(local));
                            }
                        }
                        if (ok) terrainSurf.push_back(s);
                        // Steep-but-not-vertical triangles (v170m): SM64 counts only |normal y| <= 0.01
                        // as a wall; a cliff face leaning a few degrees is a floor or a ceiling to it,
                        // and Mario walked through it (under the Hellkite bridge, Capra's arena).  A
                        // vertical stand-in over the same span, facing the same way, blocks him.
                        if (ok && g_wallSkirtOn) {
                            float w3[3][3];
                            for (int v = 0; v < 3; ++v) for (int ax = 0; ax < 3; ++ax) w3[v][ax] = t.v[flip ? 2 - v : v][ax];
                            const float u[3] = {w3[1][0] - w3[0][0], w3[1][1] - w3[0][1], w3[1][2] - w3[0][2]};
                            const float vv[3] = {w3[2][0] - w3[1][0], w3[2][1] - w3[1][1], w3[2][2] - w3[1][2]};
                            float n[3] = {u[1] * vv[2] - u[2] * vv[1], u[2] * vv[0] - u[0] * vv[2], u[0] * vv[1] - u[1] * vv[0]};
                            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (nl > 1e-6f) {
                                for (float &q : n) q /= nl;
                                const float hl = std::sqrt(n[0] * n[0] + n[2] * n[2]);
                                if (std::fabs(n[1]) > 0.01f && std::fabs(n[1]) < g_wallSkirtMaxNy && hl > 0.5f) {
                                    const float hx = n[0] / hl, hz = n[2] / hl, tx = -hz, tz = hx;
                                    const float cx = (w3[0][0] + w3[1][0] + w3[2][0]) / 3.f, cz = (w3[0][2] + w3[1][2] + w3[2][2]) / 3.f;
                                    float t0 = 1e9f, t1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                                    for (int v = 0; v < 3; ++v) {
                                        const float tt = (w3[v][0] - cx) * tx + (w3[v][2] - cz) * tz;
                                        t0 = std::fmin(t0, tt); t1 = std::fmax(t1, tt);
                                        y0 = std::fmin(y0, w3[v][1]); y1 = std::fmax(y1, w3[v][1]);
                                    }
                                    if (t1 - t0 > 0.05f && y1 - y0 > 0.05f) {
                                        const float q[4][3] = {{cx + tx * t0, y0, cz + tz * t0}, {cx + tx * t1, y0, cz + tz * t1},
                                                               {cx + tx * t1, y1, cz + tz * t1}, {cx + tx * t0, y1, cz + tz * t0}};
                                        int order[2][3] = {{0, 1, 2}, {0, 2, 3}};
                                        // SM64's normal: cross(v2 - v1, v3 - v2); it must face (hx, 0, hz).
                                        const float a1[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]};
                                        const float a2[3] = {q[2][0] - q[1][0], q[2][1] - q[1][1], q[2][2] - q[1][2]};
                                        const float cxn = a1[1] * a2[2] - a1[2] * a2[1], czn = a1[0] * a2[1] - a1[1] * a2[0];
                                        if (cxn * hx + czn * hz < 0) { std::swap(order[0][1], order[0][2]); std::swap(order[1][1], order[1][2]); }
                                        for (int tt = 0; tt < 2; ++tt) {
                                            SC_Surface ws = {};
                                            bool ok2 = true;
                                            for (int v = 0; v < 3; ++v)
                                                for (int ax = 0; ax < 3; ++ax) {
                                                    const float local = (q[order[tt][v]][ax] - stepOrigin[ax]) * 100.f / k;
                                                    if (!(std::fabs(local) < 30000.f)) ok2 = false;
                                                    ws.vertices[v][ax] = static_cast<int32_t>(std::lround(local));
                                                }
                                            if (ok2) { terrainSurf.push_back(ws); ++skirts; }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    a.api->surfaces(terrainSurf.data(), static_cast<uint32_t>(terrainSurf.size()), 0);
                    a.api->surfaces(nullptr, 0, 1);
                    terrainBuilt = true;
                    terrainK = k;
                    terrainFlipBuilt = g_terrainFlip;
                    terrainMapBuilt = terrain::g_mapChangedTick;
                    skirtBuilt = g_wallSkirtOn;
                    logWrite("mario: terrain: %zu static triangles around (%.1f, %.1f, %.1f), %zu of them vertical stand-ins",
                             terrainSurf.size(), terrainCenter[0], terrainCenter[1], terrainCenter[2], skirts);
                }
                // Native support: where the cache has no floor at the knight's
                // feet (event-controlled floors such as the Asylum's breakable
                // one, lifts, skipped shapes), a 1.2 m patch at his feet while
                // he stands still vertically (PTDE world.h's support patch).
                // Mario fell through the unbroken Asylum hole (live 2026-10-21).
                // Floating knight (knightfloat=on): he has no gravity, so
                // "steady" says nothing (he paused at the top of every jump and
                // Mario landed on a patch in mid-air, v107).  There the patch is
                // made only when the knight, with map hit on, refused to follow
                // Mario down (a real native floor held him), and it then stays at
                // that height, under Mario, through jumps, until the cache has a
                // floor under Mario again or the knight goes below it.
                // Rescue (v138): the knight stands still on a floor Mario cannot see (native-only, or
                // after the long-fall handoff gave the knight his gravity back and a native floor
                // caught him) while Mario keeps falling far below: Mario goes back onto that floor.
                // Not on ordinary drops: then the knight follows Mario down instead of standing still,
                // and a cached floor below Mario (within 8 m of the knight) is left to catch him.
                {
                    static float rescuePrevY = 0;
                    static int rescueSteady = 0;
                    rescueSteady = std::fabs(player[1] - rescuePrevY) < 0.02f ? rescueSteady + 1 : 0;
                    rescuePrevY = player[1];
                    const float rmw[3] = {stepOrigin[0] + lastMarioX * .01f * k, stepOrigin[1] + lastMarioY * .01f * k,
                                          stepOrigin[2] + lastMarioZ * .01f * k};
                    const bool noFloorBelow = !std::isfinite(terrain::floorBelow(rmw, 0.f));
                    if (rescueSteady >= 6 && !lastGrounded && rmw[1] < player[1] - 2.5f &&
                        (noFloorBelow || rmw[1] < player[1] - 8.f)) {
                        SC_State prev = {};
                        SC_MotionResolution r = {};
                        r.size = sizeof(r);
                        r.position[0] = knight[0]; r.position[1] = knight[1] + 5.f / k; r.position[2] = knight[2];
                        a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                        lastMarioX = knight[0]; lastMarioY = knight[1]; lastMarioZ = knight[2];
                        g_patchOn = true;
                        g_patchY = player[1];
                        g_patchX = player[0]; g_patchZ = player[2];
                        g_fallHandoff = false;
                        rescueSteady = 0;
                        logWrite("mario: rescued from a fall %.1f m below the knight: back on his floor at %.2f (%s)",
                                 player[1] - rmw[1], player[1], noFloorBelow ? "no cached floor below" : "far below");
                    }
                }
                bool &patchOn = g_patchOn;
                float &patchY = g_patchY;  // world metres
                if (nativemotion::g_floatOn) {
                    const float mwY = stepOrigin[1] + lastMarioY * .01f * k;
                    const float mw[3] = {stepOrigin[0] + lastMarioX * .01f * k, mwY, stepOrigin[2] + lastMarioZ * .01f * k};
                    static int heldTicks = 0;
                    const float knightFloor = terrain::floorBelow(player, 0.3f);
                    // Held: map hit on, the knight steady while Mario is clearly
                    // below him, and no cached floor under Mario within 1.5 m (so a
                    // bonfire or other object the knight stands on next to a
                    // cached floor is not taken for an event floor, v109).
                    static float prevKnightY = 0;
                    const bool knightSteady = std::fabs(player[1] - prevKnightY) < 0.02f;
                    prevKnightY = player[1];
                    const float marioFloor = terrain::floorBelow(mw, 0.3f);
                    // Not against cached geometry: a ledge or lip within 0.45 m of
                    // his capsule at his height can hold him while Mario slides
                    // down a wall (a patch in mid-air, v109).
                    bool nearCached = false;
                    for (int s = 0; s < 8 && !nearCached; ++s) {
                        const float ang = s * 0.785398f;
                        const float q[3] = {player[0] + 0.45f * std::cos(ang), player[1] + 0.1f, player[2] + 0.45f * std::sin(ang)};
                        nearCached = terrain::floorBelow(q, 0.f) > player[1] - 0.35f;
                    }
                    const float hx = player[0] - mw[0], hz = player[2] - mw[2];
                    const bool inLine = hx * hx + hz * hz < 0.15f * 0.15f;
                    // Just walked or ran off a cached floor (grounded within 8 ticks):
                    // the lip/line checks are for wall slides in the air, and next
                    // to the Asylum hole's cached edges they refused its event
                    // floor (no patch at all, Mario fell through, v112).
                    static int sinceGround = 0;
                    sinceGround = lastGrounded ? 0 : sinceGround + 1;
                    const bool fresh = sinceGround <= 8;
                    // PTDE: native physics is authoritative for floors the cache does
                    // not have.  The knight was sent down (Mario's height) and the
                    // game held him, on something that is not cached level geometry:
                    // Mario lands there, whatever he is doing (dives and rolls fell
                    // through the Asylum's event floors, v124: the old lip, line and
                    // just-grounded conditions failed mid-move).
                    (void)fresh; (void)inLine; (void)nearCached;
                    // Perched on a cached edge (v127): walking off the Asylum's
                    // parapet above the bonfire, the knight's capsule rested on its
                    // corner, a patch was made in mid-air and Mario stood on
                    // nothing (the "invisible wall" there).  A capsule on a corner
                    // sits a little BELOW the top next to it and slides down; an
                    // event floor flush with the cached floor (the Asylum hole)
                    // holds him level with it and perfectly still.
                    float nearTop = -INFINITY;
                    for (int s = 0; s < 16; ++s) {
                        const float ang = (s & 7) * 0.785398f, r = s < 8 ? 0.25f : 0.5f;
                        const float q[3] = {player[0] + r * std::cos(ang), player[1] + 0.1f, player[2] + r * std::sin(ang)};
                        const float f = terrain::floorBelow(q, 0.f);
                        if (f > player[1] - 0.35f && f > nearTop) nearTop = f;
                    }
                    const bool nearEdge = std::isfinite(nearTop);
                    static int stillTicks = 0;
                    static float stillY = 0;
                    stillTicks = std::fabs(player[1] - stillY) < 0.005f ? stillTicks + 1 : 0;
                    stillY = player[1];
                    // A capsule on a corner slides; an event floor a few cm below the cached floor
                    // next to it (Frampt's pool in Firelink) holds him perfectly still.  Still for
                    // 4 ticks is a floor, not a perch (refused there, Mario fell through, v138).
                    const bool perched = nearEdge && nearTop - player[1] > 0.004f && stillTicks < 4;
                    // After the long-fall handoff the knight falls on his own gravity
                    // (no target below him), so "sent down" is the handoff itself; he
                    // must be still 3 ticks so the start of his fall is not a floor.
                    // Off the side of the New Londo / Frampt's stairs the knight landed
                    // on the native floor below while Mario fell through it (v154).
                    const bool requestedDown = g_lastTargetY < player[1] - 0.05f || (g_fallHandoff && stillTicks >= 3);
                    const bool held = !nativemotion::g_noHitSet && knightSteady && requestedDown &&
                                      knightFloor < player[1] - 0.25f && player[1] > mwY + 0.10f &&
                                      hx * hx + hz * hz < 2.0f * 2.0f && !(marioFloor > mwY - 1.5f && player[1] - marioFloor < 0.45f) && !perched &&
                                      (!nearEdge || stillTicks >= 2);
                    {
                        static DWORD lastPerchLog = 0;
                        if (perched && requestedDown && player[1] > mwY + 0.10f && GetTickCount() - lastPerchLog > 1000) {
                            lastPerchLog = GetTickCount();
                            logWrite("mario: knight perched on a ledge edge (%.3f m below its top); no support patch",
                                     nearTop - player[1]);
                        }
                    }
                    heldTicks = held ? heldTicks + 1 : 0;
                    if (!patchOn && heldTicks >= 1) {
                        patchOn = true;
                        patchY = player[1];
                        g_patchX = player[0]; g_patchZ = player[2];
                        // Mario dropped a little below it while it was confirmed.
                        SC_State prev = {};
                        SC_MotionResolution r = {};
                        r.size = sizeof(r);
                        r.position[0] = lastMarioX; r.position[1] = knight[1]; r.position[2] = lastMarioZ;
                        a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                        lastMarioY = knight[1];
                        if (g_fallHandoff) logWrite("mario: the falling knight landed on a native floor; Mario joins him");
                        g_fallHandoff = false;
                        logWrite("mario: native floor held the knight; support patch at %.2f", patchY);
                    }
                    // Dropped when Mario is back over a cached floor, the knight
                    // went below it, or Mario jumped off it (PTDE: discarded as
                    // Mario jumps; the knight finds it again on the way down).
                    // Dropped when Mario is back over a cached floor, or the knight
                    // went below it: while Mario stands on it the knight is sent 5 cm
                    // lower (PTDE's downward probe), so with no real floor there he
                    // sinks and it goes.
                    // Native floors lead (stairs, slopes, lifts): while Mario stands on the patch it
                    // follows the knight's native height every tick.  Gone only when the knight
                    // reached the requested probe height (5 cm under Mario) two ticks running: no
                    // floor held him.  A lift or a stair puts him wherever it is instead (v138: a
                    // flat patch let Mario walk through the stairs at Frampt's pool and fall through
                    // the New Londo lift).
                    if (patchOn && (lastGrounded || mwY < patchY + 0.5f)) {
                        // The knight reached the full probe depth: nothing held him.
                        const bool reachedProbe = std::fabs(player[1] - g_lastTargetY) < 0.01f && g_lastTargetY < patchY - 0.17f;
                        const float d = player[1] - patchY;
                        // Probe depth from how fast the floor is already going down.
                        g_patchDescent = d < 0.f ? std::max(-d, g_patchDescent * 0.8f) : g_patchDescent * 0.8f;
                        g_floorProbe = std::min(1.0f, std::max(0.35f, 2.5f * g_patchDescent));
                        if (reachedProbe) {
                            logWrite("mario: support patch gone: the knight sank to the probe at %.2f (patch %.2f)", player[1], patchY);
                            patchY = player[1] + 1.f;  // dropped below (player under patchY - 0.03)
                        } else if (std::fabs(d) < 1.2f) {
                            if (std::fabs(d) > 0.005f) {
                                static DWORD lastTrackLog = 0;
                                if (GetTickCount() - lastTrackLog > 1000) {
                                    lastTrackLog = GetTickCount();
                                    logWrite("mario: support patch follows the knight %s %.2f m (%.2f -> %.2f)", d > 0 ? "up" : "down",
                                             std::fabs(d), patchY, player[1]);
                                }
                            }
                            patchY = player[1];
                            g_patchX = player[0]; g_patchZ = player[2];
                        } else {
                            static DWORD lastLeftLog = 0;
                            if (GetTickCount() - lastLeftLog > 2000) {
                                lastLeftLog = GetTickCount();
                                logWrite("mario: support patch left: the knight is %.2f m from it", d);
                            }
                        }
                    }
                    if (patchOn && (terrain::floorBelow(mw, 0.3f) > patchY - 0.25f || player[1] < patchY - 0.03f)) {
                        patchOn = false;
                        heldTicks = 0;
                    }
                    if (!patchOn) { g_patchDescent = 0.f; g_floorProbe = 0.35f; }
                    if (patchOn) {
                        g_patchSeenTick = GetTickCount();
                        // A 3 m square around where the knight was last held on the
                        // native floor (moved with him while Mario stands on it and
                        // the 5 cm probe holds), so walking, diving and corners on
                        // it never run off a small patch (v111: falls, stuck).
                        if (lastGrounded && player[1] >= patchY - 0.03f) { g_patchX = player[0]; g_patchZ = player[2]; }
                        SC_Surface tri[2] = {};
                        const float h = 150.f / k, py = (patchY - stepOrigin[1]) * 100.f / k;
                        const float cx = (g_patchX - stepOrigin[0]) * 100.f / k, cz = (g_patchZ - stepOrigin[2]) * 100.f / k;
                        const float c[4][3] = {{cx - h, py, cz - h},
                                               {cx + h, py, cz - h},
                                               {cx + h, py, cz + h},
                                               {cx - h, py, cz + h}};
                        static const int idx[6] = {0, 2, 1, 0, 3, 2};
                        for (int tt = 0; tt < 2; ++tt)
                            for (int kk = 0; kk < 3; ++kk)
                                for (int ax = 0; ax < 3; ++ax)
                                    tri[tt].vertices[kk][ax] = static_cast<int32_t>(std::lround(c[idx[tt * 3 + kk]][ax]));
                        a.api->surfaces(tri, 2, 1);
                    } else {
                        a.api->surfaces(nullptr, 0, 1);
                    }
                } else {
                    patchOn = false;
                    static float lastKnightY = 0;
                    const bool knightSteady = std::fabs(player[1] - lastKnightY) < 0.02f;
                    lastKnightY = player[1];
                    const float cacheFloor = terrain::floorBelow(player, 0.3f);
                    if (knightSteady && cacheFloor < player[1] - 0.25f) {
                        SC_Surface tri[2] = {};
                        const float h = 60.f / k;
                        const float c[4][3] = {{knight[0] - h, knight[1], knight[2] - h},
                                               {knight[0] + h, knight[1], knight[2] - h},
                                               {knight[0] + h, knight[1], knight[2] + h},
                                               {knight[0] - h, knight[1], knight[2] + h}};
                        static const int idx[6] = {0, 2, 1, 0, 3, 2};
                        for (int tt = 0; tt < 2; ++tt)
                            for (int kk = 0; kk < 3; ++kk)
                                for (int ax = 0; ax < 3; ++ax)
                                    tri[tt].vertices[kk][ax] = static_cast<int32_t>(std::lround(c[idx[tt * 3 + kk]][ax]));
                        a.api->surfaces(tri, 2, 1);
                    } else {
                        a.api->surfaces(nullptr, 0, 1);
                    }
                }
            } else {
                if (terrainBuilt) { a.api->surfaces(nullptr, 0, 0); terrainBuilt = false; }
                SC_Surface tri[2] = {};
                const float h = 600.f;
                const float c[4][3] = {{knight[0] - h, knight[1], knight[2] - h},
                                       {knight[0] + h, knight[1], knight[2] - h},
                                       {knight[0] + h, knight[1], knight[2] + h},
                                       {knight[0] - h, knight[1], knight[2] + h}};
                static const int idx[6] = {0, 2, 1, 0, 3, 2};  // upward-facing (PTDE world.h)
                for (int t = 0; t < 2; ++t)
                    for (int kk = 0; kk < 3; ++kk)
                        for (int ax = 0; ax < 3; ++ax)
                            tri[t].vertices[kk][ax] = static_cast<int32_t>(std::lround(c[idx[t * 3 + kk]][ax]));
                a.api->surfaces(tri, 2, 1);
            }
            SC_Input input = {};
            float fx = player[0] - camera[0], fz = player[2] - camera[2];
            const float fl = std::sqrt(fx * fx + fz * fz);
            if (fl > 1e-3f) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
            if (g_stepCamFlip) { fx = -fx; fz = -fz; }
            input.camLookX = fx;
            input.camLookZ = fz;
            const bool focused = gameFocused();
            const PadSample pad = focused ? readPad() : PadSample{};
            float mx = pad.x, my = pad.y;
            // Keyboard movement when the stick is idle (PTDE gameplay.h).
            bool keyJump = false, keyAttack = false, keyCrouch = false;
            if (focused) {
                float kx = (keyDown('D') ? 1.f : 0.f) - (keyDown('A') ? 1.f : 0.f);
                float ky = (keyDown('W') ? 1.f : 0.f) - (keyDown('S') ? 1.f : 0.f);
                if (mx == 0 && my == 0 && (kx != 0 || ky != 0)) {
                    const float n = std::sqrt(kx * kx + ky * ky);
                    const float speed = keyDown(g_keyWalk) ? 0.45f : 1.0f;  // walk modifier
                    mx = kx / n * speed;
                    my = ky / n * speed;
                }
                keyJump = keyDown(g_keyJump);
                keyAttack = keyDown(g_keyAttack);
                keyCrouch = keyDown(g_keyCrouch);
            }
            const float speedScale = g_stepSpeedMilli / 1000.0f;
            mx *= speedScale;
            my *= speedScale;
            input.moveX = g_stepXFlip ? -mx : mx;
            input.moveY = g_stepYFlip ? -my : my;
            // DSR buttons with their SM64 meaning: A (interact) jumps, RB
            // (attack) punches, a short B tap (roll) is an SM64 B press: a dive
            // while running, a punch or kick standing.
            {
                // L3+R3 held 1 s toggles driving (and the knight's filter).
                static DWORD comboSince = 0;
                static bool comboFired = false;
                const WORD combo = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
                if ((pad.buttons & combo) == combo || (focused && keyDown(VK_F8))) {
                    if (!comboSince) comboSince = GetTickCount();
                    if (!comboFired && GetTickCount() - comboSince >= 1000) {
                        comboFired = true;
                        g_driveActive = !g_driveActive;
                        logWrite("mario: driving %s (L3+R3 / F8)", g_driveActive ? "on" : "off");
                    }
                } else {
                    comboSince = 0;
                    comboFired = false;
                }
            }
            {
                // F6 toggles Mario / the knight at once (PTDE's key, v159).
                static bool prevF6 = false;
                const bool f6 = focused && keyDown(VK_F6);
                if (f6 && !prevF6) {
                    g_driveActive = !g_driveActive;
                    logWrite("mario: driving %s (F6)", g_driveActive ? "on" : "off");
                }
                prevF6 = f6;
            }
            input.jump = ((pad.buttons & XINPUT_GAMEPAD_A) || keyJump) ? 1 : 0;
            // LT is SM64's Z (crouch): long jump, backflip, side flip, ground
            // pound.  Same choice as the PTDE host.
            input.crouch = (pad.lt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD || keyCrouch) ? 1 : 0;
            // ---- Moves (v109) ----
            // B / Space: dodge roll on the ground (Mario's forward rollout, the
            // roll-and-hop-up, in the stick direction; backward rollout with
            // the stick idle, like the knight's backstep), with i-frames; in
            // the air it is SM64's B (dive, jump kick).
            // RB / left mouse: light attack = punches (never the third-hit kick).
            // RT / right mouse: heavy attack = kick (slide kick when running).
            {
                namespace d = sm64direct;
                const bool dodgeNow = (pad.buttons & XINPUT_GAMEPAD_B) || (focused && keyDown(g_keyDodge));
                // The flame in the right hand: RB / left mouse cast natively instead (v159).
                const bool lightNow = !mariosetup::flameRight() && ((pad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) || keyAttack);
                const bool heavyNow = pad.rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD || (focused && keyDown(g_keyHeavy));
                const bool dodgePress = dodgeNow && !prevDodge, lightPress = lightNow && !prevLight,
                           heavyPress = heavyNow && !prevHeavy;
                prevDodge = dodgeNow; prevLight = lightNow; prevHeavy = heavyNow;
                const uint32_t group = lastAction & d::ACT_GROUP_MASK;
                const bool punching = lastAction == d::ACT_PUNCHING || lastAction == d::ACT_MOVE_PUNCHING;
                const bool groundFree = lastGrounded && !punching && (group == d::GROUP_STATIONARY || group == d::GROUP_MOVING);
                const bool air = group == d::GROUP_AIRBORNE;
                const bool direct = d::ready();
                // A dodge, dive, kick or rollout still playing (v170f): another press is not a new
                // move and costs nothing (spamming B and RT charged stamina mid-animation).
                const bool moveBusy = lastAction == d::ACT_DIVE || lastAction == d::ACT_DIVE_SLIDE ||
                                      lastAction == d::ACT_JUMP_KICK || lastAction == d::ACT_SLIDE_KICK ||
                                      lastAction == d::ACT_FORWARD_ROLLOUT || lastAction == d::ACT_BACKWARD_ROLLOUT;
                bool feedB = false;
                bool staminaHold = false;  // a dodge or air attack: regeneration pauses as for an attack
                if (dodgePress && !nativeact::g_owned) {  // owned (a ladder): B is the knight's
                    if (lastAction == d::ACT_DIVE_SLIDE) {
                        feedB = true;  // SM64: B on the belly slide = the roll back up
                    } else if (groundFree && direct && !moveBusy && spendStamina(g_dodgeStamina)) {
                        staminaHold = true;
                        // Dodge = SM64's running dive (B at top speed), in the
                        // stick direction (facing with the stick idle), then the
                        // belly slide; B again rolls him up (above).
                        const float mag = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
                        const float speed = g_dodgeSpeedMilli / 1000.0f;
                        const float yaw = mag > 0.2f ? std::atan2(input.moveX, input.moveY) +
                                                           std::atan2(input.camLookX, input.camLookZ)
                                                     : lastFace;
                        // Always the full-speed dive (B at top running speed): the
                        // dive adds 15 to the forward speed it starts from (max 48).
                        const float base = speed - 15.f;
                        d::setFacing(id, yaw);
                        d::setForwardVelocity(id, base > 0 ? base : 0);
                        d::setAction(id, d::ACT_DIVE);
                        iframeTicks = 12;  // 0.4 s, like the knight's medium roll
                        InterlockedExchange(&mariocombat::g_dodgeUntil, static_cast<LONG>(GetTickCount() + 700));
                        logWrite("mario: dodge dive (yaw %.2f, was facing %.2f, speed %.1f)", yaw, lastFace, base + 15.f);
                    } else if (air && !moveBusy && spendStamina(g_kickStamina)) {
                        feedB = true;  // SM64's air B (dive / jump kick)
                        staminaHold = true;
                    }
                }
                // Light: punch 1 and punch 2 alternate for as long as it is
                // pressed again, with no pause: a buffered press moves the punch
                // sequence on at each punch's late window (SM64 action arg 2 ->
                // second punch, 5 -> first punch again; never 6, the kick).
                const bool groundOk = direct && lastGrounded;
                // At most one punch every 0.4 s (half the v110 rate).
                static DWORD lastPunch = 0;
                const bool punchReady = GetTickCount() - lastPunch >= 400;
                const bool kicking = (lastAction == d::ACT_PUNCHING && lastArg >= 6) || lastAction == d::ACT_JUMP_KICK;
                if (lightPress) {
                    if (punching) lightBuffer = 12;
                    else if (groundFree && direct && punchReady) {
                        if (spendStamina(g_punchStamina)) {
                            lastPunch = GetTickCount();
                            if (group == d::GROUP_MOVING) d::setAction(id, d::ACT_MOVE_PUNCHING);
                            else d::setActionArg(id, d::ACT_PUNCHING, 0);
                        }
                    } else if (air && !moveBusy && spendStamina(g_kickStamina)) {
                        feedB = true;
                        staminaHold = true;
                    }
                }
                if (lightBuffer > 0) {
                    --lightBuffer;
                    if (!punching) lightBuffer = 0;
                    else if (kicking) lightBuffer = 0;
                    else if ((lastArg == 2 || lastArg == 5) && groundOk && punchReady) {
                        if (spendStamina(g_punchStamina)) {
                            d::setActionArg(id, lastAction, lastArg == 2 ? 3 : 0);
                            lastPunch = GetTickCount();
                        }
                        lightBuffer = 0;
                    }
                }
                // Heavy: SM64's ground kick standing, his jump kick moving.
                // Not while a kick is already going (spamming it charged stamina
                // for kicks that never started, v110), and at most one per 0.4 s.
                static DWORD lastKick = 0;
                if (heavyPress) {
                    if ((groundFree || punching) && !kicking && groundOk && GetTickCount() - lastKick >= 400) {
                        lastKick = GetTickCount();
                        if (spendStamina(g_kickStamina)) {
                            // Always SM64's jump kick (a small hop and a kick).
                            d::setAction(id, d::ACT_JUMP_KICK);
                            d::setVelocity(id, std::sin(lastFace) * lastFwd, 20.f, std::cos(lastFace) * lastFwd);
                        }
                    } else if (air && !moveBusy && spendStamina(g_kickStamina)) {
                        feedB = true;
                        staminaHold = true;
                    }
                }
                // Stamina behaves as with a weapon: no regeneration during an
                // attack and for 0.5 s after it (it refilled mid-combo, v111).
                // v170e: the hold is applied on the game thread every frame (staminaHoldTick); from
                // here, at 30 Hz, the game's per-frame regeneration slipped through between ticks
                // and Mario always had a point or two to attack with.
                if (punching || kicking || staminaHold || lastAction == d::ACT_DIVE || lastAction == d::ACT_DIVE_SLIDE)
                    staminaHoldFor(500);
                input.attack = feedB ? 1 : 0;
                // Ledge grab assist: Mario bounced off a wall with a ledge top
                // within his grab reach (set after the step, below).  Put him
                // back into a fall facing the wall, pressing into it, so SM64's
                // own ledge check catches the lip as he comes down.
                if (grabAssist > 0 && direct) {
                    if (grabAssist == 15) {
                        d::setAction(id, d::ACT_FREEFALL);
                        d::setFacing(id, grabYaw);
                    }
                    if (lastAction == d::ACT_FREEFALL || grabAssist == 15) d::setForwardVelocity(id, 8.f);
                    else grabAssist = 1;  // grabbed, landed or something else: done
                    --grabAssist;
                }
                // Ledge grab (v115): falling, moving or pushing toward a real lip
                // just ahead: a wall between Mario and it, its top 70-165 units
                // above his feet, floor-free space in front of him below it.  One
                // attempt, then 15 ticks' rest (v114 re-grabbed every tick onto
                // ceiling slabs above him and dragged him into them).
                static int grabRest = 0;
                if (grabRest > 0) --grabRest;
                // Near the top of a rise counts too (a wall kick reaches the lip
                // still rising a little; SM64 grabbed those, v120 did not).
                if (direct && air && grabRest == 0 && lastVy <= 300.f && g_terrainOn && terrain::g_loaded == 1 &&
                    !nearEventFloor(player) && lastAction != d::ACT_DIVE && lastAction != d::ACT_JUMP_KICK && (lastAction & 0xFFF) != 0x8A9 /*pound*/) {
                    const float kq = (g_driveActive && g_drive == 4 && nativemotion::g_ready && g_travelMilli > 0)
                                         ? g_travelMilli / 1000.0f : 1.0f;
                    const float u = .01f * kq;
                    const float mag = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
                    const float stickYaw = std::atan2(input.moveX, input.moveY) + std::atan2(input.camLookX, input.camLookZ);
                    const float diff = std::fabs(std::remainder(stickYaw - lastFace, 6.2831853f));
                    // Not moving away from it (wall kicks turn him round, so the
                    // stick need not point at the wall).
                    const bool pushing = (mag > 0.3f && diff < 1.6f) || lastFwd > 0.5f || mag < 0.3f;
                    if (pushing) {
                        const float feet[3] = {stepOrigin[0] + lastMarioX * u, stepOrigin[1] + lastMarioY * u,
                                               stepOrigin[2] + lastMarioZ * u};
                        // Along his movement: drifting backwards onto a ledge counts
                        // (v124); he turns to face it when he grabs.
                        const float sgn = lastFwd < -0.5f ? -1.f : 1.f;
                        const float dx = sgn * std::sin(lastFace), dz = sgn * std::cos(lastFace);
                        const float grabFace = sgn < 0 ? lastFace + 3.14159265f : lastFace;
                        for (int s = 0; s <= 120; s += 10) {
                            const float p[3] = {feet[0] + dx * s * u, feet[1] + 170.f * u, feet[2] + dz * s * u};
                            float topNy = 0;
                            const float top = terrain::floorBelowN(p, 0.f, topNy);
                            const float rel = std::isfinite(top) ? (top - feet[1]) / u : -1e9f;
                            if (rel < (lastVy < 0.f ? -40.f : 30.f)) continue;  // open in front of him at this column
                            // From slightly above too, drifting down past it (v121).
                            // Rising: only lips near the top of a jump (v128: floor lips
                            // 30-100 units up were grabbed on every jump).
                            if (s < 30 || rel > 170.f || rel < (lastVy < 0.f ? -40.f : 100.f)) break;
                            // The face below the lip: halfway up it, or just under the top
                            // when Mario is level with or above the lip.
                            const float mid = feet[1] + (rel > 40.f ? rel * 0.5f : rel - 20.f) * u;
                            const float from[3] = {feet[0], mid, feet[2]};
                            const float to[3] = {feet[0] + dx * (s + 5) * u, mid, feet[2] + dz * (s + 5) * u};
                            // A real ledge is a wall from the floor up to the lip; a floor
                            // lip is a short riser (v128): the face must also stand at
                            // 35 units above his feet.
                            const float low[3] = {feet[0], feet[1] + 35.f * u, feet[2]};
                            const float lowTo[3] = {feet[0] + dx * (s + 5) * u, feet[1] + 35.f * u, feet[2] + dz * (s + 5) * u};
                            if (!terrain::wallBetween(low, lowTo)) {
                                static DWORD lastLowLog = 0;
                                if (GetTickCount() - lastLowLog > 300) {
                                    lastLowLog = GetTickCount();
                                    logWrite("mario: ledge %.0f units above, %d ahead: not grabbed (no wall from the floor)", rel, s);
                                }
                                break;
                            }
                            if (!terrain::wallBetween(from, to)) {  // a slab edge, not a wall face
                                static DWORD lastSlabLog = 0;
                                if (GetTickCount() - lastSlabLog > 300) {
                                    lastSlabLog = GetTickCount();
                                    logWrite("mario: ledge %.0f units above, %d ahead: not grabbed (no wall between)", rel, s);
                                }
                                break;
                            }
                            // A ledge that looks like one (v118: seams between two
                            // pieces and arch tops were grabbed): a flat top (SM64's
                            // own 0.906 limit), running at least 60 units back at
                            // the same height, a wall face right under the lip,
                            // and nothing above the hands for 1.5 m.
                            static DWORD lastLedgeLog = 0;
                            auto ledgeMiss = [&](const char *why) {
                                if (GetTickCount() - lastLedgeLog > 300) {
                                    lastLedgeLog = GetTickCount();
                                    logWrite("mario: ledge %.0f units above, %d ahead: not grabbed (%s)", rel, s, why);
                                }
                            };
                            if (topNy < 0.906f) { ledgeMiss("top not flat"); break; }
                            // At least ~30 units of top behind the lip (v127: 45-55
                            // refused the Asylum's 42 cm parapet, a real ledge).
                            bool deep = true;
                            for (int back : {15, 30}) {
                                float bny = 0;
                                const float q[3] = {feet[0] + dx * (s + back) * u, top + 10.f * u, feet[2] + dz * (s + back) * u};
                                const float b = terrain::floorBelowN(q, 0.f, bny);
                                if (!(std::fabs(b - top) < 12.f * u) || bny < 0.906f) deep = false;
                            }
                            if (!deep) { ledgeMiss("top too shallow"); break; }
                            const float hiFrom[3] = {feet[0], top - 20.f * u, feet[2]};
                            const float hiTo[3] = {feet[0] + dx * (s + 5) * u, top - 20.f * u, feet[2] + dz * (s + 5) * u};
                            if (!terrain::wallBetween(hiFrom, hiTo)) { ledgeMiss("no wall face under the lip"); break; }
                            const float up0[3] = {feet[0] + dx * (s + 15) * u, top + 8.f * u, feet[2] + dz * (s + 15) * u};
                            const float up1[3] = {up0[0], top + 100.f * u, up0[2]};
                            if (terrain::segmentHits(up0, up1, 0.f, 1.f)) { ledgeMiss("something overhead"); break; }
                            const float lx = lastMarioX + dx * (s + 10), lz = lastMarioZ + dz * (s + 10);
                            a.api->setPosition(id, lx, (top - stepOrigin[1]) / u, lz);
                            d::setAction(id, d::ACT_LEDGE_GRAB);
                            d::setFacing(id, grabFace);
                            grabAssist = 0;
                            grabRest = 15;
                            g_grabHold = 20;
                            logWrite("mario: ledge grab (lip %.0f units above, %d ahead)", rel, s);
                            break;
                        }
                    }
                }
                // Just grabbed (v127): SM64 lets go of a ledge when the stick
                // points away from it on the 10th frame of hanging; drifting back
                // onto a ledge after a wall kick, the stick still pointed away
                // and he dropped off (soft bonk) 0.3 s after every grab.  For the
                // first 20 ticks a stick pointing away is ignored; A or toward the
                // ledge climbs, and holding away afterwards still drops.
                if (g_grabHold > 0) {
                    --g_grabHold;
                    const float mag = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
                    if (mag > 0.3f) {
                        const float stickYaw = std::atan2(input.moveX, input.moveY) + std::atan2(input.camLookX, input.camLookZ);
                        g_grabStickDiff = std::fabs(std::remainder(stickYaw - lastFace, 6.2831853f));
                        if (lastAction == d::ACT_LEDGE_GRAB && g_grabStickDiff > 1.5f) input.moveX = input.moveY = 0;
                    } else {
                        g_grabStickDiff = -1.f;
                    }
                }
                if (iframeTicks > 0) --iframeTicks;
                if (g_drive == 4) nativemotion::applyIframes(iframeTicks > 0);
            }
            const LONG driveNow = g_driveActive ? g_drive : 0;
            nativemotion::g_enabled = driveNow == 4 && nativemotion::g_ready && !nativeact::g_owned;
            if (driveNow == 4 && !nativemotion::g_ready) {
                static bool tried = false;
                if (!tried) { tried = true; nativemotion::install(); }
            }
            if (driveNow == 4 && nativemotion::g_ready && nativePublished && nativemotion::g_pending == 0 &&
                !nativeact::g_owned) {
                // PTDE order: reconcile with where native physics put the
                // knight, then step.  Only a real shortfall (the knight ended
                // clearly short of the target he was sent to) corrects Mario,
                // with momentum removed into it (a wall).  Checking while the
                // knight was still mid-step, or correcting every tick, read as
                // walls and slowed Mario to a crawl (live 2026-10-21, v98).
                const float sx = (nativeTargetX - player[0]), sz = (nativeTargetZ - player[2]);
                const float shortfall = std::sqrt(sx * sx + sz * sz);
                const float step = std::sqrt(nativeWanted[0] * nativeWanted[0] + nativeWanted[2] * nativeWanted[2]);
                // At least 12 cm and 40% of the step, for 3 ticks: flat-ground
                // shortfalls of a few centimetres stopped Mario mid-run (v122).
                const float limit = 0.12f > 0.4f * step ? 0.12f : 0.4f * step;
                // A wall only when the shortfall persists for 2 ticks (stairs,
                // slopes and bumps leave one-frame shortfalls that stopped
                // Mario mid-run, v99); a single one just carries into the next
                // step.  Beyond 0.6 m, realign without touching momentum.
                // In the air Mario is free (up to 3 m): the knight bumping a
                // ledge or a pot must not pull him back mid-jump.
                const bool airborne = !lastGrounded;
                // Hanging from / climbing a ledge (SM64's automatic group): the
                // knight cannot match the lip; Mario is never moved then (he let
                // go of every ledge, v112).
                const bool hanging = (lastAction & sm64direct::ACT_GROUP_MASK) == 0x140;
                // Knight on another level or blocked under a jump: Mario jumped
                // up a ledge, or over a lip/railing the knight's capsule cannot
                // pass, down to a lower area.  After 3 ticks the knight is warped
                // onto the static floor under Mario instead of pulling Mario back
                // (v105: he teleported back up and the camera stayed behind).
                const float marioWorldY = stepOrigin[1] + lastMarioY * .01f * k;
                const float levelGap = marioWorldY - player[1];
                const bool otherLevel = std::fabs(levelGap) > 0.5f;
                {
                    // Floating knight: the air is his too, so no mid-jump warps.
                    const bool want = !g_fallHandoff && !hanging &&
                                      ((!airborne && otherLevel) || (airborne && shortfall > 0.8f && !nativemotion::g_floatOn));
                    ledgeTicks = ledgeTicks < 0 ? ledgeTicks + 1 : (want ? ledgeTicks + 1 : 0);
                    if (ledgeTicks >= 3) {
                        const float mw[3] = {stepOrigin[0] + lastMarioX * .01f * k, marioWorldY,
                                             stepOrigin[2] + lastMarioZ * .01f * k};
                        const float floorY = terrain::floorBelow(mw, 0.05f);
                        if (std::isfinite(floorY) && floorY > marioWorldY - 6.0f) {
                            const float spot[3] = {mw[0], floorY + 0.05f, mw[2]};
                            if (nativemotion::warpTo(spot))
                                logWrite("mario: knight warped to the floor under Mario (%s, %+.2f m, %.2f m away)",
                                         airborne ? "mid-jump" : "landed", floorY - player[1], shortfall);
                        }
                        ledgeTicks = -10;  // give the warp time before testing again
                        shortTicks = gapTicks = 0;
                    }
                }
                // Floating knight: he follows Mario through the air with map hit
                // off, so a shortfall there is a character (an enemy) in the
                // way: Mario stops at him (he dove through enemies, v109).
                bool checkWalls = !airborne || (nativemotion::g_floatOn && !g_fallHandoff);
                if (checkWalls && airborne && g_airWallFixOn) {
                    // v170n: beside a static wall in the air the shortfall is the knight's capsule
                    // at that wall, not an enemy: snapping Mario back to him broke wall kicks.
                    const float mw0[3] = {stepOrigin[0] + lastMarioX * .01f * k, stepOrigin[1] + lastMarioY * .01f * k,
                                          stepOrigin[2] + lastMarioZ * .01f * k};
                    if (terrain::wallNear(mw0, 1.0f)) checkWalls = false;
                }
                shortTicks = (checkWalls && !hanging && !otherLevel && ledgeTicks == 0 && shortfall > limit) ? shortTicks + 1 : 0;
                const bool wall = shortTicks >= 3;
                // PTDE: disagreement alone never moves Mario in the air (only
                // contact planes clip him); on the ground, a gap over 0.6 m
                // realigns him.
                // Realign only after the gap has lasted 0.3 s on the ground: the
                // knight catches up within a few frames after most landings.
                gapTicks = (!airborne && !hanging && !otherLevel && ledgeTicks == 0 && shortfall > 0.6f) ? gapTicks + 1 : 0;
                if (wall || gapTicks >= 9) {
                    if (!wall) { snapLog("realign", shortfall, knight[1], lastMarioY, k); gapTicks = 0; }
                    SC_State prev = {};
                    SC_MotionResolution r = {};
                    r.size = sizeof(r);
                    r.position[0] = knight[0];
                    r.position[2] = knight[2];
                    r.position[1] = lastMarioY < knight[1] ? knight[1] : lastMarioY;
                    const float into = -(sx * nativeWanted[0] + sz * nativeWanted[2]);
                    // In the air (a dive into an enemy) only the position is held,
                    // with no momentum change (that bounced him off, v111).
                    if (wall && into < 0 && !airborne) {
                        r.normalCount = 1;
                        r.normals[0][0] = -sx / shortfall; r.normals[0][1] = 0; r.normals[0][2] = -sz / shortfall;
                        ++nativeWalls;
                        static DWORD lastWallLog = 0;
                        if (GetTickCount() - lastWallLog > 500) {
                            lastWallLog = GetTickCount();
                            logWrite("mario: wall: knight %.2f m short of a %.2f m step (knight y %.2f, Mario y %.2f)", shortfall, step,
                                     player[1], stepOrigin[1] + lastMarioY * .01f * k);
                        }
                    }
                    a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                }
                ++nativeTicks;
            }
            const float marioW[3] = {stepOrigin[0] + lastMarioX * .01f * k, stepOrigin[1] + lastMarioY * .01f * k,
                                     stepOrigin[2] + lastMarioZ * .01f * k};
            g_marioRootW[0] = marioW[0]; g_marioRootW[1] = marioW[1]; g_marioRootW[2] = marioW[2];
            mariosetup::g_rootWorld[0] = marioW[0]; mariosetup::g_rootWorld[1] = marioW[1]; mariosetup::g_rootWorld[2] = marioW[2];
            for (int q = 0; q < 3; ++q) fxprobe::g_marioRoot[q] = marioW[q];
            fxprobe::g_marioFace = lastFace;
            {
                const bool flame = mariosetup::flameLeft() || mariosetup::flameRight();
                InterlockedExchange(&fxprobe::g_flameHeld, flame ? 1 : 0);
                InterlockedExchange(&fxprobe::g_marioValid, 1);
            }
            if (driveNow == 4 && nativemotion::g_ready && nativePublished && haveTerrain && !nativeact::g_owned &&
                !g_fallHandoff && lastMarioY < knight[1] - 300.f / k && !std::isfinite(terrain::floorBelow(marioW, 0.5f))) {
                // Only where the cache has no floor under Mario at all: dropping
                // off a ledge onto a lower floor is not a fall-through (v103:
                // this pulled him back up ledges).
                // Fell through somewhere the static caches have no floor (an
                // event-controlled floor, a skipped shape): back onto the knight.
                SC_State prev = {};
                SC_MotionResolution r = {};
                r.size = sizeof(r);
                r.position[0] = knight[0]; r.position[1] = knight[1]; r.position[2] = knight[2];
                a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                snapLog("fell (no floor under Mario)", 0, knight[1], lastMarioY, k);
            } else if (driveNow == 4 && nativemotion::g_ready && nativePublished && nativemotion::g_pending != 0 &&
                       !nativeact::g_owned) {
                // Knight not taking his steps (menu, cutscene, a stall): keep
                // Mario from running off more than 1.5 m without him.
                // The knight's steps pause while he is in the air; normal jumps
                // must not trip this (they snapped Mario back, v102).
                const float gx = (nativeTargetX - player[0]), gz = (nativeTargetZ - player[2]);
                // PTDE expires the ticket and hands back; here Mario is only put
                // back when grounded and 4 m away (never mid-jump).
                const float levelGap = stepOrigin[1] + lastMarioY * .01f * k - player[1];
                if (lastGrounded && std::fabs(levelGap) < 0.5f && gx * gx + gz * gz > 16.f) {
                    snapLog("stall", std::sqrt(gx * gx + gz * gz), knight[1], lastMarioY, k);
                    SC_State prev = {};
                    SC_MotionResolution r = {};
                    r.size = sizeof(r);
                    r.position[0] = knight[0]; r.position[1] = knight[1]; r.position[2] = knight[2];
                    a.api->resolveMotion(id, &r, &prev, &a.mesh.view);
                }
            }
            // ---- Taking hits (PTDE combat/gameplay: native HP loss plays SM64's
            // hurt reaction).  HP is the game's own (+0x3E8); a drop of 15% or
            // more of his HP is a heavy hit.  Owned by the game: no reaction.
            if (driveNow == 4 && nativemotion::g_ready && !nativeact::g_owned && sm64direct::hurt) {
                static int prevHp = -1;
                static DWORD lastHit = 0;
                if (g_landedHp >= 0) {
                    prevHp = g_landedHp;
                    g_landedHp = -1;
                }
                int hp = 0;
                if (g_livePlayerIns && nativeact::copy(g_livePlayerIns + 0x3E8, &hp, 4)) {
                    if (hp <= 0) {
                        prevHp = -1;  // dead: the next life starts fresh
                    } else {
                        if (prevHp > 0 && hp < prevHp) {
                            const int drop = prevHp - hp;
                            const int severity = drop * 100 >= prevHp * 15 ? 2 : 1;
                            sm64direct::hurt(id, severity);
                            mariosetup::g_hitAt = GetTickCount() | 1;
                            logWrite("mario: hit for %d HP (%s), HP now %d", drop, severity == 2 ? "heavy" : "light", hp);
                            lastHit = GetTickCount();
                        }
                        prevHp = hp;
                    }
                }
                (void)lastHit;
            }
            // ---- Native actions (v118, PTDE actions.h) ----
            // A / Q at an action prompt: the knight interacts (synthetic A).
            // Any event animation (rest, drink, pickup, door, ladder) or menu:
            // the game owns the knight; Mario is posed at him, not simulated,
            // until he is idle again (PTDE ReturnGate timings).
            {
                static DWORD ownedSince = 0, quietSince = 0, phaseStart = 0, itemAt = 0;
                static int itemKind = 0;  // 1 interaction (A), 2 item (X)
                static uint32_t lastKind = SC_POSE_NEUTRAL;
                static const char *startedBy = "";
                static float takeMario[3], takeKnight[3];
                static int lastEventAnim = -1;
                static float ladderTravel = 0, prevKnightY = 0;
                static bool sawEvent = false, prevA = false, prevQ = false;
                static int lastSeenAnim = -1, ownedAnim = -2;
                const nativeact::Observation no = nativeact::observe();
                const bool aNow = (pad.buttons & XINPUT_GAMEPAD_A) != 0, qNow = focused && keyDown('Q');
                const bool aEdge = (aNow && !prevA) || (qNow && !prevQ);
                prevA = aNow;
                prevQ = qNow;
                // Menu buttons: Start / Select, Esc / Tab / I.
                static bool prevMenuBtn = false;
                const bool menuNow = (pad.buttons & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) != 0 ||
                                     (focused && (keyDown(VK_ESCAPE) || keyDown(VK_TAB) || keyDown('I')));
                const bool menuEdge = menuNow && !prevMenuBtn;
                prevMenuBtn = menuNow;
                const DWORD now = GetTickCount();
                if (!nativeact::g_owned && driveNow == 4 && no.valid) {
                    const char *why = nullptr;
                    static bool prevX = false, prevR = false;
                    const bool xNow = (pad.buttons & XINPUT_GAMEPAD_X) != 0, rNow = focused && keyDown('R');
                    const bool itemEdge = (xNow && !prevX) || (rNow && !prevR);
                    prevX = xNow;
                    prevR = rNow;
                    if (aEdge && no.prompt && lastGrounded) {
                        input.jump = 0;
                        // Sent once the knight's footing is steady (PTDE GroundSettle, v159).
                        itemAt = now;
                        itemKind = 1;
                        why = "A at a prompt";
                    } else if (itemEdge && lastGrounded) {
                        // PTDE actions.h: release the knight first (gravity and his
                        // own motion back: a drink started while floating or moving
                        // was cancelled at once, v121), then press X for him.
                        itemAt = now;
                        itemKind = 2;
                        why = "item";
                    } else if (no.anim >= 0 && no.anim != lastSeenAnim) {
                        why = "event animation";
                    }
                    (void)menuEdge;  // menus with Mario active: given up (v120)
                    if (why) {
                        // Asleep: woken first (posed, his own wake-up never ran and
                        // the snoring went on after he got up, v121).
                        if (lastAction == 0x0C400202 || lastAction == 0x0C000203) {
                            sm64direct::setAction(id, 0x0C000204);  // ACT_WAKING_UP
                            SC_State ws = {};
                            SC_Input none = {};
                            a.api->step(id, &none, &ws, &a.mesh.view);
                        }
                        nativeact::g_owned = 1;
                        startedBy = why;
                        takeMario[0] = lastMarioX; takeMario[1] = lastMarioY; takeMario[2] = lastMarioZ;
                        takeKnight[0] = player[0]; takeKnight[1] = player[1]; takeKnight[2] = player[2];
                        nativeact::g_takeSnapshot = true;  // MenuMan as it was before
                        lastKind = SC_POSE_NEUTRAL;
                        ladderTravel = 0;
                        prevKnightY = player[1];
                        ownedSince = quietSince = phaseStart = now;
                        sawEvent = false;
                        ownedAnim = -2;
                        nativemotion::applyFloat(false, false);
                        nativemotion::applyIframes(false);
                        logWrite("mario: the game takes the knight (%s, anim %d, menu %d)", why, no.anim, no.menu ? 1 : 0);
                    }
                }
                lastSeenAnim = no.anim;
                // PTDE native_action_preparation (v159): the knight is released first, then the
                // press waits for his footing to be steady for 150 ms (1000 ms at most), so
                // native falling on stairs and slopes does not cancel the item or interaction.
                if (nativeact::g_owned && itemAt) {
                    static float itemPrevY = 0;
                    static DWORD itemSteadySince = 0;
                    const bool steadyNow = std::fabs(player[1] - itemPrevY) < 0.01f;
                    itemPrevY = player[1];
                    if (!steadyNow) itemSteadySince = 0;
                    else if (!itemSteadySince) itemSteadySince = now | 1;
                    const bool settled = itemSteadySince && now - itemSteadySince >= 150;
                    if (!g_footWaitOn || (settled && now - itemAt >= 50) || now - itemAt >= 1000) {
                        if (itemKind == 1) nativeact::g_synthA = 4;
                        else nativeact::g_synthX = 4;
                        if (g_footWaitOn)
                            logWrite("mario: %s sent after %lu ms waiting for footing%s", itemKind == 1 ? "interaction" : "item",
                                     static_cast<unsigned long>(now - itemAt), settled ? "" : " (timeout)");
                        itemAt = 0;
                        itemSteadySince = 0;
                        ownedSince = now;  // the action starts now
                    }
                }
                if (nativeact::g_owned) {
                    if (no.anim >= 0) lastEventAnim = no.anim;
                    // A menu keeps the game in control only for a bonfire rest, a
                    // message or an item box (A at a prompt, no other animation):
                    // a fog wall's boss bar changed MenuMan for good and held
                    // Mario forever, even after death (v122).
                    bool menuHolds = no.menu && (lastEventAnim < 0 || (lastEventAnim >= 7700 && lastEventAnim < 7750) ||
                                                 (lastEventAnim >= 7500 && lastEventAnim < 7580));
                    // v170o: the menu reading can stay "open" for good when control was switched
                    // mid-rest (the snapshot was then taken with the bonfire menu already open).
                    // Not a menu any more once the knight has stood up from the rest (its x2 clip,
                    // 7702/7712/7722) or walks away from where the game took him.
                    if (menuHolds && no.anim < 0) {
                        const bool stoodUp = lastEventAnim >= 7700 && lastEventAnim < 7750 && lastEventAnim % 10 == 2;
                        const float wdx = player[0] - takeKnight[0], wdz = player[2] - takeKnight[2];
                        const bool walked = wdx * wdx + wdz * wdz > 0.6f * 0.6f;
                        if (stoodUp || walked) {
                            menuHolds = false;
                            static DWORD lastLog = 0;
                            if (GetTickCount() - lastLog > 2000) {
                                lastLog = GetTickCount();
                                logWrite("mario: menu reading ignored (%s)", stoodUp ? "the knight stood up from the rest" : "the knight walked away");
                            }
                        }
                    }
                    int ownedHp = 1;
                    if (g_livePlayerIns) nativeact::copy(g_livePlayerIns + 0x3E8, &ownedHp, 4);
                    if (ownedHp <= 0) quietSince = 0, ownedSince = 0, sawEvent = true;  // dead: hand back
                    if (no.anim >= 0 || menuHolds) sawEvent = true;
                    if (itemAt) quietSince = now;  // still preparing
                    // A guard: ten seconds with neither (nothing really started).
                    if (!sawEvent && now - ownedSince > 10000) quietSince = 0;
                    if (no.anim != ownedAnim) { ownedAnim = no.anim; phaseStart = now; }
                    if (menuHolds || no.anim >= 0) quietSince = now;
                    // On a ladder: resting on it (no animation between climb steps) is not the
                    // end - handing back there dropped Mario off the ladder while the knight
                    // stayed on it (v170).  Only the exit clips (70x3+) end the ladder.
                    const bool onLadder = lastEventAnim >= 7000 && lastEventAnim < 7100 && (lastEventAnim % 10) < 3;
                    if (onLadder && no.anim < 0 && ownedHp > 0) quietSince = now;
                    if (onLadder || (no.anim >= 7000 && no.anim < 7100)) InterlockedExchange(&g_ladderSince, static_cast<LONG>(now | 1));
                    else InterlockedExchange(&g_ladderSince, 0);
                    const bool done = !no.valid || driveNow != 4 ||
                                      (now - quietSince >= 350 && now - ownedSince >= (sawEvent ? 400u : 1200u));
                    if (done) {
                        nativeact::g_owned = 0;
                        InterlockedExchange(&g_ladderSince, 0);
                        InterlockedExchange(&g_hideHeldItem, 0);
                        lastEventAnim = -1;
                        // (No ChrFlags1 0x00800000 here: clearing it stops the knight
                        // being drawn, and Mario's camera comes from his draws - Mario
                        // vanished after a bonfire, v123.)
                        // PTDE: resume (the adapter's host-pose exit; a plain idle
                        // command left him sitting, v124).  Where the knight barely
                        // moved, Mario keeps his own spot (snapping him onto the knight
                        // nudged him, sometimes into a wall); after a ladder or door,
                        // the knight's.
                        float rx = knight[0], ry = knight[1], rz = knight[2];
                        const float mdx = player[0] - takeKnight[0], mdz = player[2] - takeKnight[2];
                        if (mdx * mdx + mdz * mdz < 0.5f * 0.5f && std::fabs(player[1] - takeKnight[1]) < 0.3f) {
                            rx = takeMario[0]; ry = takeMario[1]; rz = takeMario[2];
                        }
                        float angle = 0;
                        nativeact::copy(g_livePlayerPos + 4, &angle, 4);
                        a.api->resume(id, rx, ry, rz, angle - 3.14159265f);
                        lastMarioX = rx; lastMarioY = ry; lastMarioZ = rz;
                        logWrite("mario: back to Mario after %lu ms (%s)", now - ownedSince, startedBy);
                    } else {
                        // Mario at the knight, in the matching pose (PTDE phases).
                        const float t = float(now - phaseStart) / 1000.f;
                        SC_Pose pose = {};
                        pose.kind = SC_POSE_NEUTRAL;
                        pose.phase = -1;
                        const int an = no.anim;
                        if (an < 0) {
                            // Between animations (the long bonfire rest loop, a ladder
                            // pause): keep the last pose held (v121 stood him up).
                            pose.kind = lastKind;
                            pose.phase = lastKind == SC_POSE_REST ? 1.f : lastKind == SC_POSE_DOOR ? 1.f : -1.f;
                            if (lastKind == SC_POSE_DRINK) { pose.kind = SC_POSE_NEUTRAL; pose.phase = -1; }
                        }
                        if (an >= 7000 && an < 7100) {
                            pose.kind = SC_POSE_CLIMB;
                        } else if (an >= 7585 && an <= 7587) {
                            pose.kind = SC_POSE_DRINK;
                            pose.phase = an == 7585 ? std::fmin(1.f, t / .65f) : an == 7586 ? 1.f : std::fmax(0.f, 1.f - t / .75f);
                        } else if (an >= 7700 && an < 7750) {
                            const int part = an % 10;
                            pose.kind = SC_POSE_REST;
                            pose.phase = part == 0 ? std::fmin(1.f, t / 1.1f) : part == 2 ? std::fmax(0.f, 1.f - t / 1.1f) : 1.f;
                        } else if (an == 7410) {
                            pose.kind = SC_POSE_WALK;
                        } else if (an >= 7100 && an < 7400) {
                            pose.kind = SC_POSE_DOOR;
                            pose.phase = std::fmin(1.f, t / 2.2f);
                        }
                        if (an >= 0) lastKind = pose.kind;
                        InterlockedExchange(&g_hideHeldItem, pose.kind == SC_POSE_DRINK ? 1 : 0);
                        // Ladder: the climb clip follows the knight's height (PTDE travel).
                        if (pose.kind == SC_POSE_CLIMB) {
                            ladderTravel += (player[1] - prevKnightY) * 100.f;
                            pose.travelCm = ladderTravel;
                        }
                        prevKnightY = player[1];
                        pose.position[0] = knight[0]; pose.position[1] = knight[1]; pose.position[2] = knight[2];
                        float angle = 0;
                        nativeact::copy(g_livePlayerPos + 4, &angle, 4);
                        pose.yaw = angle - 3.14159265f;
                        if (pose.kind == SC_POSE_CLIMB) {
                            // On a ladder (v170m): Mario is smaller than the knight and his climb
                            // clip holds his hands closer in, so at the knight's spot he hung in the
                            // air; he is moved toward the ladder (his facing) and up or down.
                            const float fwd = static_cast<float>(g_ladderFwdCm) / k, up = static_cast<float>(g_ladderUpCm) / k;
                            pose.position[0] += std::sin(pose.yaw) * fwd;
                            pose.position[2] += std::cos(pose.yaw) * fwd;
                            pose.position[1] += up;
                        }
                        if (pose.kind == SC_POSE_CLIMB) {
                            // On a ladder (v170m): the knight hangs at his own arm's length; Mario,
                            // smaller, is moved toward the rungs (ladderreach=<cm>, SM64 forward is
                            // (sin yaw, cos yaw)) so his hands are on the ladder, not in the air.
                            const float r = static_cast<float>(g_ladderReachCm);
                            pose.position[0] += std::sin(pose.yaw) * r;
                            pose.position[2] += std::cos(pose.yaw) * r;
                            pose.position[1] += static_cast<float>(g_ladderLiftCm);
                        }
                        SC_State ps = {};
                        if (a.api->pose(id, &pose, &ps, &a.mesh.view) && a.mesh.view.numTrianglesUsed) {
                            pumpAudio(a.api);
                            const float feet[3] = {knight[0], knight[1], knight[2]};
                            SCCharacterPresentation::mesh({a.mesh.view, feet,
                                                           {(k * (feet[0] - knight[0]) - feet[0]) * .01f,
                                                            (k * (feet[1] - knight[1]) - feet[1]) * .01f,
                                                            (k * (feet[2] - knight[2]) - feet[2]) * .01f},
                                                           a.profile->body.modelScale > 0 ? a.profile->body.modelScale : 1.0f,
                                                           nullptr, 1.0f, nullptr},
                                                          scratch);
                            // Lighting and hand-off to the renderer, as at the end
                            // of every tick (skipped before: the pose was made but
                            // never shown, Mario looked frozen, v119).
                            {
                                float L[3] = {-.3f, .8f, -.5f};
                                const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                                for (float &v : L) v /= ln;
                                const float *nrm = a.mesh.view.normal;
                                for (size_t i = 0; i < scratch.size(); ++i) {
                                    const float nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
                                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                                    const float dd = nl > 1e-4f ? (nx * L[0] + ny * L[1] + nz * L[2]) / nl : 0.f;
                                    const int light = int(255 * (.4f + .6f * (dd > 0 ? dd : 0)));
                                    scratch[i].shade = 0xff000000u | (light << 16) | (light << 8) | light;
                                }
                                const double tnow = nowSeconds();
                                EnterCriticalSection(&g_frameLock);
                                g_frame.previous.swap(g_frame.vertices);
                                g_frame.vertices.swap(scratch);
                                g_frame.triangles = a.mesh.view.numTrianglesUsed;
                                g_frame.paired = false;
                                g_frame.stamp = tnow;
                                g_frame.sequence = ++sequence;
                                LeaveCriticalSection(&g_frameLock);
                            }
                            static int loggedAnim = -99;
                            if (an != loggedAnim) {
                                loggedAnim = an;
                                logWrite("mario: native pose %u for animation %d", pose.kind, an);
                            }
                        }
                        g_stepHeartbeat = GetTickCount();
                        continue;
                    }
                }
            }
            // Equipment pose for the simulated (step) path, the one the game uses:
            // the Pyromancy Flame in the left hand is the Fire Flower; right stays empty.
            if (a.api->equipment) {
                SC_EquipmentPose ep = {sizeof(SC_EquipmentPose)};
                ep.rightKind = mariosetup::flameRight() ? SC_EQUIPMENT_PYROMANCY : SC_EQUIPMENT_NONE;
                ep.leftKind = mariosetup::flameLeft() ? SC_EQUIPMENT_PYROMANCY : SC_EQUIPMENT_NONE;
                a.api->equipment(id, &ep);
            }
            // No SM64 water: hold the water and gas levels at the bottom of their range every tick.
            if (sm64direct::ready() && sm64direct::setWaterLevel) {
                sm64direct::setWaterLevel(id, sm64direct::kNoWaterLevel);
                if (sm64direct::setGasLevel) sm64direct::setGasLevel(id, sm64direct::kNoWaterLevel);
            }
            // Lava (PTDE 23, v159): the adapter's environment extension starts SM64's
            // burning-butt bounce on an unprotected lava contact (Orange Charred Ring or its
            // effect protects); native damage is unchanged.
            if (g_envOn && mariosetup::g_envReady) {
                static const SC_EnvironmentAPI *env = nullptr;
                static bool envTried = false;
                if (!envTried) {
                    envTried = true;
                    auto get = reinterpret_cast<SC_GetEnvironmentAPI>(
                        reinterpret_cast<void *>(GetProcAddress(a.module, "SoulsCharacterGetEnvironmentAPI")));
                    env = get ? get(SC_ENVIRONMENT_VERSION, sizeof(SC_EnvironmentAPI)) : nullptr;
                    if (env && (env->size != sizeof(SC_EnvironmentAPI) || !env->observe)) env = nullptr;
                    logWrite("mario: environment reactions %s", env ? "ready (lava)" : "unavailable");
                }
                if (env) {
                    SC_Environment e = {sizeof(SC_Environment), SC_ENVIRONMENT_BURNING_FLOOR, 0, GetTickCount()};
                    if (mariosetup::g_envLava) e.flags |= SC_ENV_CONTACT;
                    if (mariosetup::g_envProtected) e.flags |= SC_ENV_PROTECTED;
                    if (lastGrounded) e.flags |= SC_ENV_GROUNDED;
                    if (!nativeact::g_owned) e.flags |= SC_ENV_CONTROL_ALLOWED;
                    const uint32_t er = env->observe(id, &e);
                    if (er & SC_ENV_REACTION_STARTED) logWrite("mario: lava: burning bounce");
                }
            }
            SC_State state = {};
            {
                // Dead knight (v170n): Mario takes no input until the respawn (he kept diving and
                // moving after death, and the respawned instance inherited it).
                int dhp = 1;
                if (g_livePlayerIns) nativeact::copy(g_livePlayerIns + 0x3E8, &dhp, 4);
                if (dhp <= 0) {
                    const float cx = input.camLookX, cz = input.camLookZ;
                    input = {};
                    input.camLookX = cx;
                    input.camLookZ = cz;
                }
            }
            const bool stepped = a.api->step(id, &input, &state, &a.mesh.view);
            actionprobe::tick();
            pumpAudio(a.api);
            if (!stepped || !a.mesh.view.numTrianglesUsed) {
                DWORD now = GetTickCount();
                if (now - lastFail > 5000) { lastFail = now; logWrite("mario: step rejected"); }
                continue;
            }
            const LONG drive = g_driveActive ? g_drive : 0;
            if (drive == 4 && nativemotion::g_ready) {
                // Publish Mario's new position as the knight's target (height
                // left to native gravity and floors) and his facing.
                // Height: with real terrain, a grounded Mario takes the knight to
                // his height (onto ledges, down steps); airborne, the knight
                // keeps his own and native gravity.
                const bool grounded = (state.flags & SC_GROUNDED) != 0;
                // The knight follows Mario's height in the air too (the game's
                // own running jump moves him vertically through this motion),
                // so he rises over pots and onto ledges with Mario; kept on the
                // ground he hit the ledge and pulled Mario back (v101).
                // Height: the static floor under Mario, not Mario himself.  The
                // game does keep stepping the knight in the air (v104 log: 60
                // calls/s), but an airborne knight takes only part of the
                // horizontal step and falls slower than Mario, so he landed up
                // to 1.8 m behind and Mario was pulled back (v104).  Kept on the
                // floor under Mario he stays grounded through jumps, is lifted
                // onto a ledge once Mario is over it, and follows him down drops.
                const float mY = stepOrigin[1] + state.position[1] * .01f * k;
                float targetY = player[1];
                if (nativemotion::g_floatOn) {
                    // Floating knight: Mario's own height, always (no gravity of
                    // his own; map hit off while Mario is in the air).
                    targetY = g_patchOn && (grounded || mY < g_patchY + 0.5f) ? mY - g_floorProbe : mY;
                    // Step assist (v162): floating, the knight does not step up on his own; held at a
                    // small step's edge he pulled Mario back (lower Undead Burg).  While he is short
                    // of his target on the ground, he is sent a little higher so his capsule clears
                    // the lip; past it he settles back onto the step.
                    // v163: big steps (lower Undead Burg's first two) needed more: the lift grows by
                    // stepassist each blocked tick up to 45 cm, and where the cache has the step's
                    // top just ahead of the knight he is sent onto it.
                    // (v170n: not while Mario moves backwards or very fast - a backwards long jump
                    // touches the ground between jumps, the knight lags behind it, and the lift
                    // broke the BLJ chain.)
                    if (grounded && shortTicks >= 1 && !g_patchOn && g_stepLiftCm > 0 && state.forwardSpeed >= 0.f &&
                        state.forwardSpeed < 1500.f) {
                        // v170: without a cached step ahead the lift stays at stepassist (12 cm):
                        // growing it carried Mario over breakable pots and crates.
                        float lift = g_stepLiftCm * 0.01f;
                        const float dx = nativeTargetX - player[0], dz = nativeTargetZ - player[2];
                        const float dl = std::sqrt(dx * dx + dz * dz);
                        if (dl > 0.01f) {
                            const float ahead[3] = {player[0] + dx / dl * 0.35f, player[1], player[2] + dz / dl * 0.35f};
                            const float top = terrain::floorBelow(ahead, 0.6f);
                            if (std::isfinite(top) && top > player[1] + 0.02f && top + 0.05f - mY > lift)
                                lift = top + 0.05f - mY;
                        }
                        targetY += lift;
                    }
                    // Map hit off while Mario rises (lips, railings), or comes
                    // down over a floor the cache has (Mario lands on it).  Over
                    // no cached floor it stays on, so a native-only floor (the
                    // Asylum's breakable one, lifts) can hold the knight.
                    const float mwp[3] = {stepOrigin[0] + state.position[0] * .01f * k, mY,
                                          stepOrigin[2] + state.position[2] * .01f * k};
                    const float floorUnder = haveTerrain ? terrain::floorBelow(mwp, 0.05f) : -INFINITY;
                    const bool overCache = floorUnder > mY - 8.0f;
                    // Off the map: no cached floor within 15 m under a falling Mario
                    // (4 m caught ordinary ledge drops and changed his jumps, v111).
                    // ...and Mario already 3 m below where he last stood: walking onto
                    // the Asylum's event floor (pit 16 m below) released the knight
                    // before the native floor could hold him (v114).
                    static float lastGroundY = 0;
                    if (grounded) lastGroundY = mY;
                    if (!grounded && state.velocity[1] < 0 && !g_patchOn && !(floorUnder > mY - 15.0f) &&
                        mY < lastGroundY - 3.0f && !terrain::recentMapChange()) {
                        if (!g_fallHandoff) logWrite("mario: long fall: knight falls on his own (gravity back)");
                        g_fallHandoff = true;
                    }
                    if (grounded) g_fallHandoff = false;
                    if (g_fallHandoff) {
                        nativemotion::applyFloat(false, false);
                        targetY = player[1];
                    } else {
                        // Over a support patch the map hit stays on (a dive on the
                        // Asylum's event floor took the knight through it, v110).
                        // Map hit off only to pass a lip of the cached level geometry
                        // between the knight and Mario (and while hanging): doors,
                        // fog walls and event floors are not cached, and with map hit
                        // off dives and jump kicks went through them (v121).
                        const bool hang = (state.rawAction & sm64direct::ACT_GROUP_MASK) == 0x140;
                        bool lip = false;
                        // (Only rising or above him: coming down next to walls it let
                        // the knight sink through event floors, v124.)
                        // Over a support patch the knight is held below Mario by the
                        // probe on purpose, so "above him" must allow for it: coming
                        // down onto event stairs the probe gap read as a lip, map hit
                        // went off and the knight sank through the stairs (v153).
                        const bool onPatch = g_patchOn && mY < g_patchY + 0.5f;
                        const float aboveBy = onPatch ? g_floorProbe + 0.1f : 0.1f;
                        if (!grounded && haveTerrain && !(onPatch && state.velocity[1] <= 0) &&
                            (state.velocity[1] > 0 || mY > player[1] + aboveBy)) {
                            for (float h : {0.3f, 0.9f}) {
                                const float from[3] = {player[0], player[1] + h, player[2]};
                                const float to[3] = {mwp[0], mY + h, mwp[2]};
                                if (terrain::segmentHits(from, to, 0.f, 1.f)) lip = true;
                            }
                        }
                        // Dropping off a cached floor the knight stands on (the upper
                        // Asylum walk down to the bonfire, v122): his capsule caught on
                        // the edge Mario jumped past.
                        if (!grounded && haveTerrain && mY < player[1] - 0.3f) {
                            const float kf = terrain::floorBelow(player, 0.3f);
                            if (std::fabs(kf - player[1]) < 0.4f) lip = true;
                        }
                        // Walking or jumping off a cached edge at his own height (v128):
                        // the knight rests on the edge and blocked Mario in mid-air
                        // (the invisible wall above the bonfire).  Only where Mario has
                        // no cached floor under him and the knight does.
                        if (!grounded && haveTerrain && !lip && !hang) {
                            const float kf = terrain::floorBelow(player, 0.3f);
                            const float mf = terrain::floorBelow(mwp, 0.3f);
                            if (std::fabs(kf - player[1]) < 0.4f && !std::isfinite(mf)) lip = true;
                        }
                        (void)overCache;
                        // Just after a map change the arrival point can sit partway into the
                        // new floor: lift onto the floor above it (5 m search, grace only).
                        if (terrain::recentMapChange()) {
                            const float up = terrain::floorBelow(mwp, 5.f);
                            if (std::isfinite(up) && up > mY) {
                                targetY = up;
                                static DWORD lastSnapLog = 0;
                                if (GetTickCount() - lastSnapLog > 1000) {
                                    lastSnapLog = GetTickCount();
                                    logWrite("mario: map change: knight lifted onto the floor at %.2f m (Mario %.2f m)", up, mY);
                                }
                            }
                        }
                        nativemotion::applyFloat(true, hang || lip);
                    }
                } else if (haveTerrain) {
                    // Hold the knight's gravity off through a map change (grace window).
                    nativemotion::applyFloat(terrain::recentMapChange(), false);
                    const float mw[3] = {stepOrigin[0] + state.position[0] * .01f * k, mY,
                                         stepOrigin[2] + state.position[2] * .01f * k};
                    const float floorY = terrain::floorBelow(mw, terrain::recentMapChange() ? 5.f : 0.05f);
                    targetY = std::isfinite(floorY) && floorY > mY - 6.0f ? floorY : (grounded ? mY : player[1]);
                }
                g_lastTargetY = targetY;
                const float target[3] = {stepOrigin[0] + state.position[0] * .01f * k, targetY,
                                         stepOrigin[2] + state.position[2] * .01f * k};
                nativeWanted[0] = (state.position[0] - lastMarioX) * .01f * k;
                nativeWanted[2] = (state.position[2] - lastMarioZ) * .01f * k;
                nativemotion::publish(target, state.faceAngle + 3.14159265f);
                nativeTargetX = target[0];
                nativeTargetZ = target[2];
                nativePublished = true;
                static DWORD lastNativeLog = 0;
                if (GetTickCount() - lastNativeLog > 5000) {
                    lastNativeLog = GetTickCount();
                    logWrite("mario: drive=native ticks %u, wall corrections %u, game motion calls %ld, applied %ld, "
                             "rejected %ld, ledge warps %ld, player calls %ld, hits queued %ld applied %ld (last %ld), gap now %.2f m",
                             nativeTicks, nativeWalls, nativemotion::g_calls, nativemotion::g_applied,
                             nativemotion::g_rejected, nativemotion::g_warps, nativemotion::g_playerCalls,
                             mariocombat::g_queued, mariocombat::g_applied, mariocombat::g_lastResult,
                             std::sqrt((target[0] - player[0]) * (target[0] - player[0]) +
                                       (target[2] - player[2]) * (target[2] - player[2])));
                    nativeTicks = nativeWalls = 0;
                }
            } else if (!drive || drive == 4) {
                nativePublished = false;
                nativemotion::applyFloat(false, false);  // the knight is the game's again
                nativemotion::applyIframes(false);
                g_fallHandoff = false;
            // The knight is authoritative horizontally; SM64 keeps its height
            // (jumps), and its momentum/action/animation phase.
            SC_MotionResolution r = {};
            r.size = sizeof(r);
            r.position[0] = knight[0];
            r.position[1] = state.position[1] < knight[1] ? knight[1] : state.position[1];
            r.position[2] = knight[2];
            a.api->resolveMotion(id, &r, &state, &a.mesh.view);
            driveSent = false;
            } else if (drive == 3) {
                // Steer the knight: feed-forward with the player's own stick
                // (same camera frame as Mario's input), plus a correction toward
                // Mario beyond 0.15 m.  Chasing Mario's position alone made the
                // knight start late and overshoot when Mario stopped (v93).
                const float tx = stepOrigin[0] + state.position[0] * .01f - player[0];
                const float tz = stepOrigin[2] + state.position[2] * .01f - player[2];
                const float d = std::sqrt(tx * tx + tz * tz);
                float sx = mx, sy = my;  // stick before speed scaling
                if (speedScale > 0) { sx /= speedScale; sy /= speedScale; }
                if (d > 0.15f) {
                    const float along = (tx * fx + tz * fz) / d, side = (tx * fz - tz * fx) / d;
                    float k = (d - 0.15f) / 0.5f;
                    if (k > 1) k = 1;
                    sx += side * k * (g_knightXFlip ? -1.f : 1.f);
                    sy += along * k;
                    // Above the game's stick deadzone when correcting alone.
                    const float n = std::sqrt(sx * sx + sy * sy);
                    if (n > 0 && n < 0.3f) { sx *= 0.3f / n; sy *= 0.3f / n; }
                }
                {
                    const float n = std::sqrt(sx * sx + sy * sy);
                    if (n > 1) { sx /= n; sy /= n; }
                }
                InterlockedExchange(&g_knightStickX, static_cast<LONG>(sx * 32767.f));
                InterlockedExchange(&g_knightStickY, static_cast<LONG>(sy * 32767.f));
                // Knight progress this tick: pushed but not moving = blocked
                // (a wall), for 3 ticks in a row.
                static float lastKnight[3] = {};
                static int stuckTicks = 0;
                const float kdx = player[0] - lastKnight[0], kdz = player[2] - lastKnight[2];
                const float kmoved = std::sqrt(kdx * kdx + kdz * kdz);
                lastKnight[0] = player[0]; lastKnight[1] = player[1]; lastKnight[2] = player[2];
                const bool pushed = sx * sx + sy * sy > 0.5f * 0.5f && d > 0.15f;
                stuckTicks = (pushed && kmoved < 0.02f) ? stuckTicks + 1 : 0;
                const bool blocked = stuckTicks >= 3;
                // Sprint for the knight while Mario runs fast.
                {
                    // From the stick: full stick for 0.4 s (Mario's speed could
                    // not rise above the knight's jog to trigger it, v93).
                    static DWORD sprintSince = 0, slowSince = 0, fullSince = 0;
                    const DWORD now = GetTickCount();
                    const float ux = speedScale > 0 ? mx / speedScale : 0, uy = speedScale > 0 ? my / speedScale : 0;
                    const float stick = std::sqrt(ux * ux + uy * uy);  // pad or keyboard, before scaling
                    if (stick > 0.9f) { if (!fullSince) fullSince = now; } else fullSince = 0;
                    if (fullSince && now - fullSince >= 400) {
                        slowSince = 0;
                        if (!g_knightSprint) { g_knightSprint = 1; sprintSince = now; }
                    } else if (g_knightSprint) {
                        if (!slowSince) slowSince = now;
                        if (now - sprintSince >= 600 && now - slowSince >= 300) g_knightSprint = 0;
                    }
                }
                // Tether: Mario stays within reach of the knight.  Only the
                // position is corrected (SM64 keeps his speed and animation);
                // momentum is removed only when the knight is blocked, so a
                // wall stops Mario.  Removing it on every pull kept him from
                // ever reaching running speed (live 2026-10-21, v92).
                const float tether = blocked ? 0.1f : g_tetherMm / 1000.0f;
                if (d > tether) {
                    ++driveBlocked;
                    SC_MotionResolution r = {};
                    r.size = sizeof(r);
                    r.position[0] = knight[0] + tx / d * tether * 100.f;
                    r.position[1] = state.position[1] < knight[1] ? knight[1] : state.position[1];
                    r.position[2] = knight[2] + tz / d * tether * 100.f;
                    if (blocked) {
                        r.normalCount = 1;
                        r.normals[0][0] = -tx / d; r.normals[0][1] = 0; r.normals[0][2] = -tz / d;
                    }
                    a.api->resolveMotion(id, &r, &state, &a.mesh.view);
                }
                ++driveTicks;
                static DWORD lastInputLog = 0;
                if (GetTickCount() - lastInputLog > 5000) {
                    lastInputLog = GetTickCount();
                    logWrite("mario: drive=input ticks %u, tethered %u, gap now %.2f m, knight stick (%.2f, %.2f), "
                             "sprint %ld, blocked %d, Mario speed %.0f cm/s, knight input filtered %ld times",
                             driveTicks, driveBlocked, d, sx, sy, g_knightSprint, blocked ? 1 : 0, state.forwardSpeed,
                             g_inputFiltered);
                    driveTicks = driveBlocked = 0;
                }
            } else {
                // Phase 2: Mario leads.  Did the knight get where he was sent
                // last tick?  If not (blocked), Mario is corrected to him, with
                // momentum removed into the blocking direction.
                ++driveTicks;
                if (driveSent) {
                    const float dx = player[0] - driveTarget[0], dz = player[2] - driveTarget[2];
                    const float miss = std::sqrt(dx * dx + dz * dz);
                    if (miss > 0.12f && miss < 5.0f) {
                        ++driveBlocked;
                        SC_MotionResolution r = {};
                        r.size = sizeof(r);
                        r.position[0] = knight[0];
                        r.position[1] = state.position[1] < knight[1] ? knight[1] : state.position[1];
                        r.position[2] = knight[2];
                        r.normalCount = 1;
                        r.normals[0][0] = dx / miss; r.normals[0][1] = 0; r.normals[0][2] = dz / miss;
                        a.api->resolveMotion(id, &r, &state, &a.mesh.view);
                    } else if (miss >= 5.0f) {
                        // Far off (a game warp, a cutscene): follow the knight.
                        SC_MotionResolution r = {};
                        r.size = sizeof(r);
                        r.position[0] = knight[0]; r.position[1] = knight[1]; r.position[2] = knight[2];
                        a.api->resolveMotion(id, &r, &state, &a.mesh.view);
                    }
                }
                // Send the knight to Mario (horizontal; native gravity and floor
                // keep his height).
                driveTarget[0] = stepOrigin[0] + state.position[0] * .01f;
                driveTarget[1] = player[1];
                driveTarget[2] = stepOrigin[2] + state.position[2] * .01f;
                driveSent = false;
                const uintptr_t pos = g_livePlayerPos, map = g_liveMapData;
                if (GetTickCount() - g_livePlayerChecked < 1000) {
                    if (drive == 1 && map) {
                        float *t = reinterpret_cast<float *>(map + 0x110);
                        t[0] = driveTarget[0]; t[1] = driveTarget[1]; t[2] = driveTarget[2];
                        *reinterpret_cast<volatile uint8_t *>(map + 0x108) = 1;
                        driveSent = true;
                    } else if (drive == 2 && pos) {
                        float *p = reinterpret_cast<float *>(pos + 0x10);
                        p[0] = driveTarget[0]; p[2] = driveTarget[2];
                        driveSent = true;
                    }
                }
                static DWORD lastDriveLog = 0;
                if (GetTickCount() - lastDriveLog > 5000) {
                    lastDriveLog = GetTickCount();
                    logWrite("mario: drive=%s ticks %u, knight short of target %u, map=%p pos=%p, knight input "
                             "filtered %ld times",
                             drive == 1 ? "warp" : "pos", driveTicks, driveBlocked, reinterpret_cast<void *>(map),
                             reinterpret_cast<void *>(pos), g_inputFiltered);
                    driveTicks = driveBlocked = 0;
                }
            }
            if (!SC::frame(state, a.mesh.view, *a.profile)) {
                DWORD now = GetTickCount();
                if (now - lastFail > 5000) { lastFail = now; logWrite("mario: step frame rejected"); }
                continue;
            }
            {
                namespace d = sm64direct;
                const float kk = (g_driveActive && g_drive == 4 && nativemotion::g_ready && g_travelMilli > 0)
                                     ? g_travelMilli / 1000.0f : 1.0f;
                // Mario's hits (v113): native damage on what his own attack reaches.
                if (g_driveActive && g_drive == 4) {
                    const float mw[3] = {stepOrigin[0] + state.position[0] * .01f * kk,
                                         stepOrigin[1] + state.position[1] * .01f * kk,
                                         stepOrigin[2] + state.position[2] * .01f * kk};
                    {
                        // Lethal surfaces (v170l, PTDE gameplay.h hazardObserve): Mario's or the
                        // knight's feet crossing a 14/15 surface (elevator shafts, pits, kill
                        // planes), or world height below -1000 m, kill the knight natively.
                        static float prevM[3], prevK[3];
                        static bool have = false;
                        static DWORD lastKill = 0;
                        int khp = 1;
                        if (g_livePlayerIns) nativeact::copy(g_livePlayerIns + 0x3E8, &khp, 4);
                        // Mario himself falling (v170n): a carried or scripted knight (the crow out of
                        // the Asylum) is not a fall into a pit.
                        const bool marioFalling = !(state.flags & SC_GROUNDED) && state.velocity[1] < 0.f;
                        const bool usable = !nativeact::g_owned && !terrain::recentMapChange() && g_deathPlaneOn && khp > 0 &&
                                            marioFalling;
                        if (usable && have && GetTickCount() - lastKill > 5000) {
                            // Only going down (v170n): the crow carried the knight out of the Asylum
                            // up through its kill plane and he died on arrival in Firelink.  A jump
                            // of over 3 m in one tick is a warp or a carry, not a fall.
                            auto falling = [](const float *from, const float *to) {
                                const float dx = to[0] - from[0], dz = to[2] - from[2], dy = to[1] - from[1];
                                return dy < -0.005f && dx * dx + dz * dz + dy * dy < 9.f;
                            };
                            const bool hitM = falling(prevM, mw) && terrain::hazardTouch(prevM, mw);
                            const bool hitK = falling(prevK, player) && terrain::hazardTouch(prevK, player);
                            if (hitM || hitK || mw[1] < -1000.f) {
                                lastKill = GetTickCount();
                                InterlockedExchange(&g_killRequest, 1);
                                logWrite("mario: lethal surface (%s) at (%.2f %.2f %.2f): the knight dies natively",
                                         hitM ? "Mario" : hitK ? "knight" : "below -1000 m", mw[0], mw[1], mw[2]);
                            }
                        }
                        memcpy(prevM, mw, sizeof(prevM));
                        memcpy(prevK, player, sizeof(prevK));
                        have = usable;
                    }
                    mariocombat::tick(id, a.api, state, mw, [&](const float w[3], float out[3]) {
                        for (int ax = 0; ax < 3; ++ax) out[ax] = (w[ax] - stepOrigin[ax]) * 100.f / kk;
                    });
                }
                // Fire Flower: a fresh LB press with the flame in the left hand throws the host
                // fireball from the flower's hand position, facing Mario's direction (12 m/s).
                if (InterlockedExchange(&g_fireRequest, 0) && g_hostFireOn && mariosetup::flameLeft() &&
                    a.api->attachments) {
                    SC_Attachments att = {sizeof(SC_Attachments)};
                    if (a.api->attachments(id, &att) && (att.valid & 2)) {
                        g_fire.pos[0] = player[0] + att.left[12] * .01f;
                        g_fire.pos[1] = player[1] + att.left[13] * .01f;
                        g_fire.pos[2] = player[2] + att.left[14] * .01f;
                        g_fire.vel[0] = 12.f * std::sin(state.faceAngle);
                        g_fire.vel[1] = 0.f;
                        g_fire.vel[2] = 12.f * std::cos(state.faceAngle);
                        g_fire.bounces = 0;
                        g_fire.born = GetTickCount();
                        g_fire.active = true;
                        logWrite("mario fire: flower fireball at %.2f %.2f %.2f, facing %.2f",
                                 g_fire.pos[0], g_fire.pos[1], g_fire.pos[2], state.faceAngle);
                    }
                }
                if (g_fire.active) {
                    // 30 Hz step: gravity 24 m/s^2, bounce 6 m/s off floors, 3 s lifetime, 6 bounces.
                    const float dt = 1.f / 30.f;
                    g_fire.vel[1] -= 24.f * dt;
                    for (int ax = 0; ax < 3; ++ax) g_fire.pos[ax] += g_fire.vel[ax] * dt;
                    float fny = 0;
                    const float fl = terrain::floorBelowN(g_fire.pos, 1.f, fny);
                    if (std::isfinite(fl) && g_fire.pos[1] <= fl && g_fire.vel[1] < 0.f) {
                        g_fire.pos[1] = fl;
                        g_fire.vel[1] = 6.f;
                        if (++g_fire.bounces > 6) g_fire.active = false;
                    }
                    if (GetTickCount() - g_fire.born > 3000) g_fire.active = false;
                }
                const uint32_t act = state.rawAction;
                const bool bonk = act == d::ACT_AIR_HIT_WALL || act == d::ACT_BACKWARD_AIR_KB || act == d::ACT_SOFT_BONK;
                // SM64 wall bonk: a thud when Mario enters a wall-bonk action.
                const bool wallBonk = act == d::ACT_AIR_HIT_WALL || act == d::ACT_SOFT_BONK;
                const bool wasWallBonk = lastAction == d::ACT_AIR_HIT_WALL || lastAction == d::ACT_SOFT_BONK;
                if (wallBonk && !wasWallBonk) {
                    mariocombat::playImpact(.6f);
                    logWrite("mario bonk: action 0x%08X -> 0x%08X arg %u flags 0x%08X vel %.1f,%.1f,%.1f",
                             lastAction, act, state.rawActionArg, state.rawFlags,
                             state.velocity[0], state.velocity[1], state.velocity[2]);
                }
                // Diagnostic (wall punch/kick knockback): every action change that
                // leaves an attack, with SM64's action, arg, flags and timer.
                {
                    const bool wasAttack = lastAction == d::ACT_PUNCHING || lastAction == d::ACT_MOVE_PUNCHING ||
                                           lastAction == d::ACT_SLIDE_KICK || lastAction == d::ACT_JUMP_KICK ||
                                           lastAction == d::ACT_DIVE;
                    if (wasAttack && act != lastAction)
                        logWrite("mario attack: action 0x%08X -> 0x%08X arg %u flags 0x%08X timer %u vel %.1f,%.1f,%.1f",
                                 lastAction, act, state.rawActionArg, state.rawFlags, state.rawActionTimer,
                                 state.velocity[0], state.velocity[1], state.velocity[2]);
                }
                const bool wasJumping = (lastAction & d::ACT_GROUP_MASK) == d::GROUP_AIRBORNE &&
                                        lastAction != d::ACT_AIR_HIT_WALL && lastAction != d::ACT_BACKWARD_AIR_KB &&
                                        lastAction != d::ACT_SOFT_BONK;
                // (The v109 bounce grab assist is off: the direct grab replaced it,
                // and it turned Mario back into walls he jumped off, v124.)
                if (false && bonk && wasJumping && grabAssist == 0 && g_terrainOn && terrain::g_loaded == 1) {
                    // Is there a ledge top just past the wall, within reach?  SM64
                    // grabs ledges 100-160 units above his feet; a little lower
                    // still counts (he is rising and will drop into range).
                    const float u = .01f * kk;  // metres per SM64 unit
                    const float feet[3] = {stepOrigin[0] + state.position[0] * u, stepOrigin[1] + state.position[1] * u,
                                           stepOrigin[2] + state.position[2] * u};
                    const float probe[3] = {feet[0] + std::sin(lastFace) * 70.f * u, feet[1] + 160.f * u,
                                            feet[2] + std::cos(lastFace) * 70.f * u};
                    const float top = terrain::floorBelow(probe, 0.f);
                    const float rel = std::isfinite(top) ? (top - feet[1]) / u : -1.f;
                    if (rel > 30.f && rel <= 160.f) {
                        grabAssist = 15;
                        grabYaw = lastFace;
                        logWrite("mario: ledge %.0f units above, in reach: grab assist", rel);
                    }
                }
                if (act == d::ACT_LEDGE_GRAB && lastAction != d::ACT_LEDGE_GRAB && nearEventFloor(player) && d::ready()) {
                    d::setAction(id, d::ACT_FREEFALL);
                    logWrite("mario: ledge grab next to an event floor cancelled (seam, not a ledge)");
                } else if (act == d::ACT_LEDGE_GRAB && lastAction != d::ACT_LEDGE_GRAB) logWrite("mario: hanging from a ledge");
                if (lastAction == d::ACT_LEDGE_GRAB && act != d::ACT_LEDGE_GRAB)
                    logWrite("mario: ledge released -> action 0x%08X (stick %.2f rad from facing, -1 none; hold %d)", act,
                             g_grabStickDiff, g_grabHold);
                // Calibration: the knight's own stamina drops (attack costs with
                // driving off, L3+R3 / F8).
                if (int *st = knightStamina()) {
                    static int prevSt = -1;
                    if (prevSt >= 0 && *st < prevSt - 3) logWrite("mario: knight stamina -%d (driving %s)", prevSt - *st, g_driveActive ? "on" : "off");
                    prevSt = *st;
                }
                {
                    // Idle diagnostic (v170h): Mario's idle (head left, right, centre) restarted.
                    // Logs idle action / animation changes and frame jumps back, 80 lines.
                    static int idleLines = 0;
                    static int prevAnim = -1, prevFrame = -1;
                    const bool idleNow = act == 0x0C400201u, idleWas = lastAction == 0x0C400201u;
                    if (idleLines < 80 && (idleNow || idleWas)) {
                        if (act != lastAction || state.rawAnimation != prevAnim ||
                            (state.rawAnimation == prevAnim && state.rawAnimationFrame + 2 < prevFrame)) {
                            ++idleLines;
                            logWrite("idle-diag: action 0x%08X -> 0x%08X, anim %d -> %d, frame %d -> %d, state arg %u timer %u",
                                     lastAction, act, prevAnim, state.rawAnimation, prevFrame, state.rawAnimationFrame,
                                     state.rawActionArg, state.rawActionTimer);
                        }
                    }
                    prevAnim = state.rawAnimation;
                    prevFrame = state.rawAnimationFrame;
                }
                {
                    // Wing Cap (v170n): the Hollow Soldier Helm (480000-480010) on the knight's head
                    // is Mario's Wing Cap - SM64's own wings, flight (triple jump) and gravity.
                    // Put on with SM64's cap animation (standing or walking), worn for as long as
                    // the helm is: the cap timer's end is undone by setting the flag back.
                    namespace d = sm64direct;
                    const uint32_t head = static_cast<uint32_t>(mariosetup::g_headId);
                    const bool want = g_wingCapOn && head >= 480000 && head <= 480010;
                    static bool putOn = false;
                    const uint32_t fl = state.rawFlags;
                    if (want && d::interactCap && d::setState) {
                        if (!putOn) {
                            d::interactCap(id, d::MARIO_WING_CAP, 1, 0);
                            putOn = true;
                            logWrite("mario: Wing Cap on (Hollow Soldier Helm %u)", head);
                        } else if (!(fl & d::MARIO_WING_CAP) && act != d::ACT_PUTTING_ON_CAP) {
                            d::setState(id, (fl | d::MARIO_WING_CAP | d::MARIO_CAP_ON_HEAD) & ~d::MARIO_CAP_IN_HAND);
                        }
                    } else if (putOn) {
                        putOn = false;
                        if (d::setState && (fl & d::MARIO_WING_CAP)) d::setState(id, fl & ~d::MARIO_WING_CAP);
                        logWrite("mario: Wing Cap off");
                    }
                }
                lastAction = act;
                lastArg = state.rawActionArg;
                lastFace = state.faceAngle;
                lastFwd = state.forwardSpeed / 30.f;
                lastVy = state.velocity[1];
            }
            // ---- Landing damage (PTDE combat: a fall past 8 m costs HP along
            // the same curve, scale 0.05, at most 5% of max HP, never lethal).
            // Written straight to HP; the hit block syncs so no reaction plays.
            {
                static bool falling = false;
                static float peakM = 0.f;
                const float yM = state.position[1] * .01f * k;
                if (!(state.flags & SC_GROUNDED)) {
                    if (!falling) {
                        falling = true;
                        peakM = yM;
                    } else if (yM > peakM) {
                        peakM = yM;
                    }
                } else if (falling) {
                    falling = false;
                    const float drop = peakM - yM;
                    int hp = 0, maxHp = 0;
                    if (drop > 8.f && g_livePlayerIns && !nativeact::g_owned &&
                        nativeact::copy(g_livePlayerIns + 0x3E8, &hp, 4) &&
                        nativeact::copy(g_livePlayerIns + 0x3EC, &maxHp, 4) && hp > 1 && maxHp > 0 &&
                        maxHp < 100000) {
                        const float frac = std::min(1.f, (drop - 8.f) / 12.f);
                        const int base = frac >= 1.f ? maxHp : int(std::ceil(maxHp * .5f * frac));
                        const int scaled = int(std::ceil(base * .05f));
                        const int cap = int(std::floor(maxHp * .05f));
                        const int damage = std::max(0, std::min({scaled, cap, hp - 1}));
                        if (damage > 0) {
                            const int after = hp - damage;
                            SIZE_T wrote = 0;
                            if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(g_livePlayerIns + 0x3E8),
                                                   &after, 4, &wrote) && wrote == 4)
                                g_landedHp = after;
                            logWrite("mario: landed from %.1f m, landing damage %d (HP %d -> %d)", drop, damage, hp, after);
                        } else {
                            logWrite("mario: landed from %.1f m, no landing damage", drop);
                        }
                    }
                }
            }
            // Diagnostic: log the collision filter under Mario's feet when it changes
            // while grounded, to identify lava and fire floors (filter values).
            {
                static uint32_t lastFloorFilter = ~0u;
                const float fk = (g_driveActive && g_drive == 4 && nativemotion::g_ready && g_travelMilli > 0)
                                     ? g_travelMilli / 1000.0f : 1.0f;
                const float fp[3] = {stepOrigin[0] + state.position[0] * .01f * fk,
                                     stepOrigin[1] + state.position[1] * .01f * fk + .2f,
                                     stepOrigin[2] + state.position[2] * .01f * fk};
                float fny = 0;
                const float fy = terrain::floorBelowN(fp, .6f, fny);
                if (std::isfinite(fy) && (state.flags & SC_GROUNDED) && terrain::g_floorFilter != lastFloorFilter) {
                    lastFloorFilter = terrain::g_floorFilter;
                    logWrite("mario: floor filter %u at y %.2f m", lastFloorFilter, fy);
                }
            }
            lastGrounded = (state.flags & SC_GROUNDED) != 0;
            {
                static float footPrevY = 0;
                const bool steady = std::fabs(player[1] - footPrevY) < 0.02f;
                footPrevY = player[1];
                if (lastGrounded && steady) { if (!g_footSince) g_footSince = GetTickCount() | 1; }
                else g_footSince = 0;
            }
            g_stepHeartbeat = GetTickCount();
            lastMarioX = state.position[0];
            lastMarioY = state.position[1];
            lastMarioZ = state.position[2];
            // Rebase (PTDE gameplay.h): past 40 m from the origin, SM64's local range
            // (about 82 m) drops the floor, so move the origin onto Mario and zero his
            // local position.  World position is unchanged.
            if (std::fabs(state.position[0]) > 4000.f || std::fabs(state.position[2]) > 4000.f ||
                std::fabs(state.position[1]) > 5000.f) {
                const float dx = state.position[0] * .01f * k, dy = state.position[1] * .01f * k,
                            dz = state.position[2] * .01f * k;
                stepOrigin[0] += dx; stepOrigin[1] += dy; stepOrigin[2] += dz;
                a.api->setPosition(id, 0, 0, 0);
                state.position[0] = state.position[1] = state.position[2] = 0;
                lastMarioX = lastMarioY = lastMarioZ = 0;
                terrainBuilt = false;
                logWrite("mario: rebased origin by %.2f %.2f %.2f m (Mario now local)", dx, dy, dz);
            }
            // Vertices relative to the knight's feet (metres): horizontal body
            // offsets plus the jump height.
            const float feet[3] = {state.position[0], state.position[1], state.position[2]};
            // World offset from the knight is k * (Mario - knight) in Mario
            // space; the body itself keeps its size (only travel is scaled).
            SCCharacterPresentation::mesh({a.mesh.view, feet,
                                           {(k * (feet[0] - knight[0]) - feet[0]) * .01f,
                                            (k * (feet[1] - knight[1]) - feet[1]) * .01f,
                                            (k * (feet[2] - knight[2]) - feet[2]) * .01f},
                                           a.profile->body.modelScale > 0 ? a.profile->body.modelScale : 1.0f,
                                           nullptr, 1.0f, nullptr},
                                          scratch);
            static DWORD lastStepLog = 0;
            if (GetTickCount() - lastStepLog > 5000) {
                lastStepLog = GetTickCount();
                logWrite("mario: step action=0x%08X anim=%d speed=%.0f cm/s height %.0f cm, pad %s (%.2f, %.2f) buttons=%04X",
                         state.rawAction, state.rawAnimation, state.forwardSpeed, state.position[1] - knight[1],
                         pad.connected ? "on" : "none", pad.x, pad.y, pad.buttons);
            }
        }
        SC_Pose pose = {};
        pose.kind = perTick > 0.003f ? SC_POSE_WALK : SC_POSE_NEUTRAL;
        pose.yaw = 0;
        pose.phase = -1;
        pose.travelCm = travelCm;
        // The walk clip steps one frame per pose call.  SM64 plays it at
        // forwardVel / 4 frames per tick (units ~ cm at Mario's own scale), so
        // the cycle is advanced by ground covered: legs keep pace with the
        // native movement instead of shuffling at a fixed slow rate.
        int calls = 1;
        if (pose.kind == SC_POSE_WALK) {
            const float scale = a.profile->body.modelScale > 0 ? a.profile->body.modelScale : 1.0f;
            walkFrames += (perTick * 100.0f / scale) / 4.0f;
            calls = static_cast<int>(walkFrames);
            if (calls < 1) calls = 1;
            if (calls > 8) calls = 8;
            walkFrames -= calls;
            if (walkFrames < -1) walkFrames = -1;
        } else {
            walkFrames = 0;
        }
        if (!mode) {
        // Equipment pose from the native hand slots (cheat-table primary hands):
        // the Pyromancy Flame in the left hand is the Fire Flower; the right hand
        // stays empty so the glove moves remain.
        if (id >= 0 && a.api->equipment) {
            SC_EquipmentPose ep = {sizeof(SC_EquipmentPose)};
            ep.rightKind = mariosetup::flameRight() ? SC_EQUIPMENT_PYROMANCY : SC_EQUIPMENT_NONE;
            ep.leftKind = mariosetup::flameLeft() ? SC_EQUIPMENT_PYROMANCY : SC_EQUIPMENT_NONE;
            a.api->equipment(id, &ep);
        }
        SC_State state = {};
        bool posed = true;
        for (int c = 0; c < calls && posed; ++c) posed = a.api->pose(id, &pose, &state, &a.mesh.view);
        if (!posed || !a.mesh.view.numTrianglesUsed ||
            !SC::frame(state, a.mesh.view, *a.profile)) {
            DWORD now = GetTickCount();
            if (now - lastFail > 5000) { lastFail = now; logWrite("mario: pose rejected"); }
            continue;
        }
        SCCharacterPresentation::mesh({a.mesh.view, pose.position, {0, 0, 0},
                                       a.profile->body.modelScale > 0 ? a.profile->body.modelScale : 1.0f,
                                       nullptr, 1.0f, nullptr},
                                      scratch);
        }
        if (!mode) {
            // Keep the body over the native position.  The walk clip carries
            // root travel along Mario's facing that SM64 would cancel with its
            // own physics; here it made him stride away and snap back at each
            // loop, or walk off screen when strafing (live 2026-10-19).  The
            // horizontal mean of the posed mesh is moved onto the feet.
            double mx = 0, mz = 0;
            for (const auto &v : scratch) { mx += v.x; mz += v.z; }
            if (!scratch.empty()) { mx /= scratch.size(); mz /= scratch.size(); }
            static float maxDrift = 0;
            const float drift = static_cast<float>(std::sqrt(mx * mx + mz * mz));
            if (drift > maxDrift) maxDrift = drift;
            static DWORD lastDriftLog = 0;
            if (GetTickCount() - lastDriftLog > 5000) {
                lastDriftLog = GetTickCount();
                logWrite("mario: body offset from feet now (%.3f, %.3f) m, largest %.3f m (removed)", mx, mz,
                         maxDrift);
                maxDrift = 0;
            }
            for (auto &v : scratch) { v.x -= static_cast<float>(mx); v.z -= static_cast<float>(mz); }
        }
        {
            // Lighting for the pose-time facing: the fixed light expressed in
            // the guest's +Z-facing frame (mesh() lit it as if yaw were 0).
            const float c = std::cos(yaw), s = std::sin(yaw);
            float L[3] = {-.3f, .8f, -.5f};
            const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            for (float &v : L) v /= ln;
            const float lx = L[0] * c - L[2] * s, lz = L[0] * s + L[2] * c, ly = L[1];
            const float *nrm = a.mesh.view.normal;
            for (size_t i = 0; i < scratch.size(); ++i) {
                float nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
                float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                float d = nl > 1e-4f ? (nx * lx + ny * ly + nz * lz) / nl : 0.f;
                int light = int(255 * (.4f + .6f * (d > 0 ? d : 0)));
                scratch[i].shade = 0xff000000u | (light << 16) | (light << 8) | light;
            }
        }
        const double now = nowSeconds();
        EnterCriticalSection(&g_frameLock);
        bool paired = g_frame.triangles == a.mesh.view.numTrianglesUsed &&
                      g_frame.vertices.size() == scratch.size() && now - g_frame.stamp < 0.1;
        for (size_t i = 0; paired && i < scratch.size(); ++i) {
            const auto &o = g_frame.vertices[i], &n = scratch[i];
            if (o.u != n.u || o.v != n.v || o.color != n.color) paired = false;
            // PTDE 08 (v159): a jump of the mesh (teleport, pose-mode switch, warp) snaps
            // instead of blending through the gap.
            if (std::fabs(o.x - n.x) > 0.8f || std::fabs(o.y - n.y) > 0.8f || std::fabs(o.z - n.z) > 0.8f) paired = false;
        }
        g_frame.previous.swap(g_frame.vertices);
        g_frame.vertices.swap(scratch);
        g_frame.triangles = a.mesh.view.numTrianglesUsed;
        g_frame.paired = paired;
        g_frame.stamp = now;
        g_frame.sequence = ++sequence;
        LeaveCriticalSection(&g_frameLock);
        if (sequence == 1 || sequence % 900 == 0)
            logWrite("mario: frame %llu triangles=%u pose=%s", static_cast<unsigned long long>(sequence),
                     a.mesh.view.numTrianglesUsed, pose.kind == SC_POSE_WALK ? "walk" : "neutral");
    }
}

// Motion-field probe (diag=on): finds the per-frame movement vector inside the
// knight's physics object (ChrPosData), the field PTDE's motion hook writes
// (its ChrPhysics +0x60, three vec4s, before the Havok step).  Each 30 Hz
// tick the first 512 bytes are compared with how far the knight moved: a slot
// whose length is near zero standing and tracks the movement when walking is
// a candidate.  Read-only.
struct MotionProbe {
    float last[3] = {};
    bool have = false;
    double sumRatio[32] = {}, sumRatio2[32] = {}, idleMag[32] = {};
    int moving = 0, idle = 0;
};
inline void snapLog(const char *why, float gap, float knightY, float marioY, float k) {
    static DWORD last = 0;
    static const char *lastWhy = nullptr;
    const DWORD now = GetTickCount();
    if (why == lastWhy && now - last < 1000) return;
    last = now;
    lastWhy = why;
    logWrite("mario: snapped to knight (%s): gap %.2f m, knight above Mario %.2f m, knight's last motion call %lu ms ago, "
             "player calls %ld",
             why, gap, (knightY - marioY) * .01f * k, now - nativemotion::g_lastPlayerCall, nativemotion::g_playerCalls);
}

inline void motionProbeTick(const float player[3]) {
    static MotionProbe p;
    const uintptr_t base = g_livePlayerPos;
    if (!base || GetTickCount() - g_livePlayerChecked > 1000 || !worldobs::readable(base, 0x200)) {
        p.have = false;
        return;
    }
    const float *f = reinterpret_cast<const float *>(base);
    float dx = player[0] - p.last[0], dy = player[1] - p.last[1], dz = player[2] - p.last[2];
    const float moved = std::sqrt(dx * dx + dz * dz);
    if (p.have) {
        for (int s = 0; s < 32; ++s) {
            const float *v = f + s * 4;
            const float mag = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!std::isfinite(mag)) continue;
            if (moved > 0.02f && moved < 1.0f) { const double r = mag / moved; p.sumRatio[s] += r; p.sumRatio2[s] += r * r; }
            else if (moved < 0.001f) p.idleMag[s] += mag;
        }
        if (moved > 0.02f && moved < 1.0f) ++p.moving;
        else if (moved < 0.001f) ++p.idle;
    }
    p.last[0] = player[0]; p.last[1] = player[1]; p.last[2] = player[2];
    p.have = true;
    (void)dy;
    if (p.moving >= 90) {
        // Report slots with a steady ratio (low spread) and near-zero idle.
        char line[1024];
        int o = 0;
        for (int s = 0; s < 32 && o < 960; ++s) {
            const double mean = p.sumRatio[s] / p.moving;
            const double var = p.sumRatio2[s] / p.moving - mean * mean;
            const double cv = mean > 1e-6 ? std::sqrt(var > 0 ? var : 0) / mean : 99;
            const double idle = p.idle ? p.idleMag[s] / p.idle : -1;
            if (mean > 1e-4 && cv < 0.6)
                o += _snprintf(line + o, sizeof(line) - o, " +0x%X(ratio %.3f spread %.2f idle %.4f)", s * 16, mean, cv,
                               idle);
        }
        line[o] = 0;
        logWrite("mario: motion probe (%d moving, %d idle ticks): candidates%s", p.moving, p.idle, o ? line : " none");
        logWrite("mario: motion probe raw now: +0x40 [%.4f %.4f %.4f %.4f] +0x50 [%.4f %.4f %.4f %.4f] +0x60 [%.4f %.4f %.4f %.4f] "
                 "+0x70 [%.4f %.4f %.4f %.4f] +0x80 [%.4f %.4f %.4f %.4f] +0x90 [%.4f %.4f %.4f %.4f]",
                 f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23], f[24], f[25], f[26], f[27], f[28], f[29], f[30],
                 f[31], f[32], f[33], f[34], f[35], f[36], f[37], f[38], f[39]);
        p = MotionProbe{};
        p.last[0] = player[0]; p.last[1] = player[1]; p.last[2] = player[2];
        p.have = true;
    }
}

// Worker-thread entry: load once, then start the pose thread.
inline void start() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    InitializeCriticalSection(&g_frameLock);
    if (!loadAdapter()) return;
    InterlockedExchange(&g_ready, 1);
    hookGameXInput();
    if (HANDLE t = CreateThread(nullptr, 0, poseThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// Render-thread copy of the latest frame, translated to `origin` (the player's
// feet in the caller's coordinate space).  Returns triangle count (0 = none).
inline uint32_t copyFrame(const float origin[3], float yaw, std::vector<SCRenderAPI::Vertex> &out) {
    if (!g_ready) return 0;
    EnterCriticalSection(&g_frameLock);
    uint32_t triangles = g_frame.triangles;
    const size_t n = size_t(triangles) * 3;
    if (triangles && g_frame.vertices.size() >= n) {
        out.assign(g_frame.vertices.begin(), g_frame.vertices.begin() + n);
        if (g_frame.paired && g_frame.previous.size() >= n) {
            double t = (nowSeconds() - g_frame.stamp) * SC_FIXED_HZ;
            float alpha = float(t < 0 ? 0 : t > 1 ? 1 : t);
            for (size_t i = 0; i < n; ++i) {
                const auto &p = g_frame.previous[i];
                out[i].x = p.x + (out[i].x - p.x) * alpha;
                out[i].y = p.y + (out[i].y - p.y) * alpha;
                out[i].z = p.z + (out[i].z - p.z) * alpha;
            }
        }
    } else {
        triangles = 0;
    }
    LeaveCriticalSection(&g_frameLock);
    // Diagnostic (throttled): the inputs to the host fireball draw below.
    {
        static DWORD lastFireDiag = 0;
        if (g_fire.active && GetTickCount() - lastFireDiag > 500) {
            lastFireDiag = GetTickCount();
            logWrite("fire draw: triangles %u out %zu stepMode %ld pos %.2f %.2f %.2f origin %.2f %.2f %.2f",
                     static_cast<unsigned>(triangles), out.size(), static_cast<long>(g_stepMode),
                     g_fire.pos[0], g_fire.pos[1], g_fire.pos[2], g_marioRootW[0], g_marioRootW[1], g_marioRootW[2]);
        }
    }
    // Host fireball: two crossed quads in the mesh frame (step mode has no yaw, so
    // local = world - origin), orange, 0.3 m across.
    if (triangles && g_fire.active && g_stepMode && !out.empty()) {
        const float s = 0.15f;
        const float c[3] = {g_fire.pos[0] - g_marioRootW[0], g_fire.pos[1] - g_marioRootW[1], g_fire.pos[2] - g_marioRootW[2]};
        const float p[4][3] = {{c[0] - s, c[1] - s, c[2]}, {c[0] + s, c[1] - s, c[2]},
                               {c[0] + s, c[1] + s, c[2]}, {c[0] - s, c[1] + s, c[2]}};
        const float r[4][3] = {{c[0], c[1] - s, c[2] - s}, {c[0], c[1] - s, c[2] + s},
                               {c[0], c[1] + s, c[2] + s}, {c[0], c[1] + s, c[2] - s}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int qd = 0; qd < 2; ++qd) {
            const float (*k)[3] = qd ? r : p;
            for (int j : idx) {
                SCRenderAPI::Vertex o{};
                o.x = k[j][0]; o.y = k[j][1]; o.z = k[j][2];
                o.color = 0xffff7a1au;  // orange fireball
                o.shade = 0xffffffffu;
                o.u = 1.f; o.v = 1.f;
                out.push_back(o);
            }
        }
        triangles += 4;
    }
    // Hit flash (PTDE feedback, v159): a star at the confirmed hit, 180-230 ms, shrinking.
    // Three crossed star polygons (the renderer has no camera billboard here), opaque.
    if (triangles && g_stepMode && !out.empty() && mariocombat::g_flash.born) {
        const mariocombat::Flash f = mariocombat::g_flash;
        const DWORD life = 180u + DWORD(50.f * f.strength);
        const float t = float(GetTickCount() - f.born) / float(life);
        if (t >= 0.f && t < 1.f) {
            const float radius = (.30f + .14f * f.strength) * (1.f + .6f * t) * (1.f - t * t);
            const float c[3] = {f.at[0] - g_marioRootW[0], f.at[1] - g_marioRootW[1], f.at[2] - g_marioRootW[2]};
            for (int plane = 0; plane < 3; ++plane) {
                for (int i = 0; i < 8; ++i) {
                    const float a0 = i * .78539816f, a1 = a0 + .39269908f, a2 = a0 + .78539816f;
                    auto at = [&](float ang, float r, float o[3]) {
                        const float u = std::cos(ang) * r, v = std::sin(ang) * r;
                        o[0] = c[0] + (plane == 0 ? u : plane == 1 ? 0.f : u);
                        o[1] = c[1] + (plane == 2 ? 0.f : v);
                        o[2] = c[2] + (plane == 0 ? 0.f : plane == 1 ? u : v);
                    };
                    float q[3][3];
                    for (int k = 0; k < 3; ++k) q[0][k] = c[k];
                    at(a0, radius * .35f, q[1]);
                    at(a1, radius, q[2]);
                    float q3[3];
                    at(a2, radius * .35f, q3);
                    const float tri[2][3][3] = {{{q[0][0], q[0][1], q[0][2]}, {q[1][0], q[1][1], q[1][2]}, {q[2][0], q[2][1], q[2][2]}},
                                                {{q[0][0], q[0][1], q[0][2]}, {q[2][0], q[2][1], q[2][2]}, {q3[0], q3[1], q3[2]}}};
                    for (int tt = 0; tt < 2; ++tt)
                        for (int k = 0; k < 3; ++k) {
                            SCRenderAPI::Vertex o{};
                            o.x = tri[tt][k][0]; o.y = tri[tt][k][1]; o.z = tri[tt][k][2];
                            o.color = k == 0 ? 0xfffff5cbu : 0xffffb34fu;
                            o.shade = 0xffffffffu;
                            o.u = 1.f; o.v = 1.f;
                            out.push_back(o);
                        }
                    triangles += 2;
                }
            }
        }
    }
    // SM64 facing rotation: the guest's +Z turns to (sin yaw, 0, cos yaw).
    // Simulated (step) meshes already face the way SM64 turned them.
    if (g_stepMode) yaw = 0;
    const float c = std::cos(yaw), s = std::sin(yaw);
    // The Fire Flower's bloom as drawn (v156): the pale-yellow inner band of the procedural
    // flower (fire_flower.h, colour 1/.96/.55).  The hand fire is moved onto it (fxprobe).
    double fc[3] = {0, 0, 0};
    int fn = 0;
    for (auto &v : out) {
        const float x = v.x * c + v.z * s, z = -v.x * s + v.z * c;
        v.x = x + origin[0]; v.y += origin[1]; v.z = z + origin[2];
        if (v.color == 0xFFFFF48Cu) { fc[0] += v.x; fc[1] += v.y; fc[2] += v.z; ++fn; }
    }
    if (fn >= 6) {
        for (int q = 0; q < 3; ++q) fxprobe::g_flowerW[q] = static_cast<float>(fc[q] / fn);
        fxprobe::g_flowerTick = GetTickCount();
    }
    return triangles;
}

inline SCRenderAPI::Material material() {
    const Adapter &a = g_adapter;
    SCRenderAPI::Material m{};
    m.image = {a.mesh.atlasPixels.data(), a.profile->textureWidth, a.profile->textureHeight};
    m.color = (a.profile->capabilities & SC_CAP_TEXTURE_MODULATE) ? SCRenderAPI::ColorMode::Modulate
                                                                   : SCRenderAPI::ColorMode::TextureAlpha;
    m.shade = true;
    m.depthTest = true;
    m.depthWrite = true;
    m.alphaCutoff = (a.profile->capabilities & SC_CAP_ALPHA_TEST) ? 127 : 0;
    return m;
}

} // namespace mario
} // namespace remastered
