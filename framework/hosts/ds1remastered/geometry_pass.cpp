// Remastered geometry-pass test draw.
//
// Issues one host-owned triangle into the game's G-buffer to prove that host
// draw calls reach it.  Everything this touches is saved first and restored
// before returning, because a partially restored context corrupts the frame the
// game is still building.

#include "geometry_pass.h"
#include "d3d11_compat.h"
#include "camera_capture.h"
#include "fx_probe.h"
#include "world_observation.h"
#include "render_device.h"
#include "mario_presentation.h"
#include "logging.h"
#include "test_shaders_gen.h"  // generated from test_shaders.hlsl

#include <cstdio>
#include <cstring>

// STDMETHODCALL is __stdcall on x86, no-op on x64.  The bundled headers may
// not pull in winnt.h, so define it explicitly for this translation unit.
#ifndef STDMETHODCALL
#define STDMETHODCALL __stdcall
#endif

namespace remastered {

// Vertices in the world-marker buffer: four triangles, one per compass direction.
static constexpr int kWorldMarkerVertices = 12;

namespace {

struct InjectionResources {
    ID3D11Buffer *vertexBuffer = nullptr;
    ID3D11VertexShader *vertexShader = nullptr;       // clip-space passthrough
    ID3D11Buffer *psConstants = nullptr;              // values for targets 0 and 1
    ID3D11BlendState *blendRt0 = nullptr;             // writes target 0 only
    ID3D11BlendState *blendRt1 = nullptr;             // writes target 1 only
    ID3D11DepthStencilState *depthTestedWrite = nullptr; // LESS_EQUAL, writes depth
    ID3D11Buffer *worldVertexBuffer = nullptr;        // rewritten per draw
    ID3D11DepthStencilState *depthTested = nullptr;   // LESS_EQUAL, no writes
    ID3D11DepthStencilState *depthTestedReversed = nullptr; // GREATER_EQUAL
    ID3D11PixelShader *pixelShader = nullptr;
    ID3D11InputLayout *inputLayout = nullptr;
    ID3D11RasterizerState *rasterizer = nullptr;
    ID3D11DepthStencilState *depthState = nullptr;
    ID3D11BlendState *blend = nullptr;
    bool ready = false;
};

InjectionResources g_res;
volatile LONG g_initState = 0;
InjectionStats g_stats;

void releaseResources() {
    auto rel = [](IUnknown *p) { if (p) p->Release(); };
    rel(g_res.vertexBuffer);
    rel(g_res.vertexShader);
    rel(g_res.psConstants);
    rel(g_res.blendRt0);
    rel(g_res.blendRt1);
    rel(g_res.depthTestedWrite);
    rel(g_res.worldVertexBuffer);
    rel(g_res.depthTested);
    rel(g_res.depthTestedReversed);
    rel(g_res.pixelShader);
    rel(g_res.inputLayout);
    rel(g_res.rasterizer);
    rel(g_res.depthState);
    rel(g_res.blend);
    g_res = InjectionResources{};
}

} // namespace

bool injectionArmed() {
    return InterlockedCompareExchange(&g_initState, 0, 0) == 2 && g_res.ready;
}

InjectionStats getInjectionStats() { return g_stats; }

static bool describeView(const void *view, UINT *w, UINT *h, UINT *format) {
    return hostReadViewResourceDesc(const_cast<void *>(view), w, h, format,
                                    nullptr, nullptr, nullptr);
}

bool looksLikeGeometryPass(UINT rtCount, const void *const *rtv, void *depthView) {
    if (rtCount == 0 || !rtv || !rtv[0] || !depthView) return false;

    UINT w = 0, h = 0, fmt = 0;
    if (!describeView(rtv[0], &w, &h, &fmt)) return false;
    if (fmt != 26) return false;

    UINT dw = 0, dh = 0, dfmt = 0;
    if (!describeView(depthView, &dw, &dh, &dfmt)) return false;
    if (dfmt != 44) return false;

    if (w != dw || h != dh) return false;
    return true;
}

namespace {

// Any failure releases what was created and disables injection for the session.
bool failInit(const char *what, HRESULT hr) {
    logWrite("inject: %s failed hr=0x%08lx", what, static_cast<unsigned long>(hr));
    releaseResources();
    InterlockedExchange(&g_initState, 3);
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Live test controls.
//
// Which passes receive the test draw, and how the world marker is depth-tested,
// are read from %LOCALAPPDATA%\MarioSoulsProbe\inject_mode.txt and re-read every
// 500 ms, so one game session can compare variants without restarting (the
// game takes a 15 s anti-cheat delay plus a save load per launch).  Keys, any
// order, unknown text ignored:
//   targets=multi|single|both|none   (default multi)
//   screen=on|off                     centre triangle (default on)
//   world=on|off                      projected marker (default on)
//   depth=lequal|gequal|always        world marker depth test (default lequal)
//   depthwrite=on|off                 world marker writes scene depth (default off)
//   write=rt0|rt1|both                which targets the test writes (default rt1)
//   rt0=r,g,b,a  rt1=r,g,b,a          values written to targets 0 and 1
//                                     (default 1,0,1,1 = magenta for both)
//   anchor=camera|player              markers around the camera, or one at the
//                                     player's feet (default camera)
//   draw=test|renderer                hand-built test draw, or the player marker
//                                     through DSRRenderer (default test)
// A missing file keeps the defaults.  The file is only read, never written.
namespace {

enum : LONG { kTargetMulti = 1, kTargetSingle = 2 };
enum : LONG { kDepthLequal = 0, kDepthGequal = 1, kDepthAlways = 2 };

volatile LONG g_modeTargets = kTargetMulti;
volatile LONG g_modeScreen = 1;
volatile LONG g_modeWorld = 1;
volatile LONG g_modeDepth = kDepthLequal;
enum : LONG { kWriteBoth = 0, kWriteRt0 = 1, kWriteRt1 = 2 };
volatile LONG g_modeWrite = kWriteRt1;
volatile LONG g_modeWorldDepthWrite = 0;
// anchor=player places one marker at the player's feet, using the camera world
// position found by world_observation.h.  camoff= is retired (parsed, unused).
volatile LONG g_modeAnchorPlayer = 0;
// anchor=render: place the marker on the player's own draw transform found in
// the main-scene constant buffer (see CameraState::renderPlayerValid).
volatile LONG g_modeAnchorRender = 0;
// draw=mario: draw the posed Mario mesh (mario_presentation.h) with the renderer.
volatile LONG g_modeMario = 0;
// draw=renderer submits the player-anchored marker through DSRRenderer
// (render_device.h) instead of the hand-built test path.
volatile LONG g_modeRenderer = 0;
// Source of the matrix and positions for the renderer marker, switchable live
// to compare timing behaviour in one session:
//   vpsource=global   latest main-scene matrix from any context (default)
//   vpsource=context  the matrix last uploaded on the drawing context
//   possource=upload  positions read when that matrix was uploaded (default)
//   possource=draw    positions read at draw time
//   camsource=memory  camera position from game memory (default)
//   camsource=eye2288 / eye1040  camera position the engine uploads to its
//                     shaders (2288/1040-byte constant buffers, byte 0x140)
volatile LONG g_modeVpContext = 0;
volatile LONG g_modePosDraw = 0;
volatile LONG g_modeCamSource = 0;  // 0 memory, 1 eye2288, 2 eye1040
volatile LONG g_modeSyncOff = 0;    // sync=off: live player position, no frame alignment
volatile LONG g_modeUpload = 0;     // upload=on: place Mario from the player's own upload (anchor by upload)
volatile LONG g_modeUploadFill = 0; // upfill=on: also draw in pass chunks without the player's upload
//   vpslot=0|48       which of the two matrices in the 4112-byte buffer to use
volatile LONG g_modeVpSlot48 = 0;
volatile LONG g_modeCamOffset = -1;
// Values written to targets 0 and 1.  Copied per draw; a torn read during a
// mode change only affects one test frame.
float g_modeTargetValues[8] = {1, 0, 1, 1, 1, 0, 1, 1};

void parseFloat4(const char *t, const char *key, float *dst) {
    const char *p = strstr(t, key);
    if (!p) return;
    float v[4];
    if (sscanf(p + strlen(key), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4)
        for (int i = 0; i < 4; ++i) dst[i] = v[i];
}

// Built-in defaults: the working configuration (2026-10-21).  A key present in
// inject_mode.txt overrides its default; the file may be absent.
static const char kDefaultModeText[] =
    "targets=multi\nscreen=off\nworld=on\ndepth=lequal\nwrite=rt0\nrt0=1,0,1,1\nrt1=1,0,1,1\n"
    "anchor=player\ndraw=mario\nvpsource=global\npossource=draw\ncamsource=eye1040\nvpslot=0\n"
    "marioyaw=flip\ncamcand=0\nupload=on\nhide=on\nhidemask=3d\ndiag=off\n"
    // v171 (shippable): Mario simulated and driving the knight natively, as the tested setup.
    "mario=step\ndrive=native\ntravel=0.85\nknightfloat=on\n";

// True when text has "key" at the start of a line (so "write=" does not match
// inside "depthwrite=").
static bool hasModeKey(const char *t, const char *key) {
    const size_t n = strlen(key);
    for (const char *p = t; (p = strstr(p, key)) != nullptr; p += n)
        if (p == t || p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t') return true;
    return false;
}

// user text followed by each default line whose key the user did not set.
static void mergeModeDefaults(const char *user, char *out, size_t outSize) {
    _snprintf(out, outSize, "%s\n", user);
    out[outSize - 1] = 0;
    const char *line = kDefaultModeText;
    while (*line) {
        const char *eol = strchr(line, '\n');
        const size_t len = eol ? static_cast<size_t>(eol - line) : strlen(line);
        const char *eq = static_cast<const char *>(memchr(line, '=', len));
        if (eq) {
            char key[32] = {};
            const size_t kl = static_cast<size_t>(eq - line) + 1;
            if (kl < sizeof(key)) {
                memcpy(key, line, kl);
                if (!hasModeKey(user, key)) {
                    const size_t used = strlen(out);
                    if (used + len + 2 < outSize) {
                        memcpy(out + used, line, len);
                        out[used + len] = '\n';
                        out[used + len + 1] = 0;
                    }
                }
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
}

void applyModeText(const char *t) {
    LONG targets = kTargetMulti, screen = 1, world = 1, depth = kDepthLequal;
    if (strstr(t, "targets=single")) targets = kTargetSingle;
    else if (strstr(t, "targets=both")) targets = kTargetMulti | kTargetSingle;
    else if (strstr(t, "targets=none")) targets = 0;
    if (strstr(t, "screen=off")) screen = 0;
    if (strstr(t, "world=off")) world = 0;
    if (strstr(t, "depth=gequal")) depth = kDepthGequal;
    else if (strstr(t, "depth=always")) depth = kDepthAlways;
    LONG write = kWriteRt1;
    if (strstr(t, "write=both")) write = kWriteBoth;
    else if (strstr(t, "write=rt0")) write = kWriteRt0;
    LONG depthWrite = strstr(t, "depthwrite=on") ? 1 : 0;
    LONG anchorRender = strstr(t, "anchor=render") ? 1 : 0;
    LONG anchorPlayer = (strstr(t, "anchor=player") || anchorRender) ? 1 : 0;
    if (anchorRender != g_modeAnchorRender) logWrite("inject: anchor=%s", anchorRender ? "render" : "memory");
    InterlockedExchange(&g_modeAnchorRender, anchorRender);
    LONG useMario = strstr(t, "draw=mario") ? 1 : 0;
    LONG useRenderer = (strstr(t, "draw=renderer") || useMario) ? 1 : 0;
    if (useMario != g_modeMario) logWrite("inject: mario %s", useMario ? "on" : "off");
    InterlockedExchange(&g_modeMario, useMario);
    LONG camCand = 0;
    if (const char *cc = strstr(t, "camcand=")) camCand = atol(cc + 8);
    if (camCand != g_camCandidate) logWrite("inject: camcand=%ld", camCand);
    InterlockedExchange(&g_camCandidate, camCand);
    LONG vpFilter = strstr(t, "vpfilter=on") ? 1 : 0;
    if (vpFilter != g_vpFilter) logWrite("inject: vpfilter=%s", vpFilter ? "on" : "off");
    InterlockedExchange(&g_vpFilter, vpFilter);
    LONG yawFlip = strstr(t, "marioyaw=flip") ? 1 : 0;
    if (yawFlip != mario::g_yawFlip) logWrite("inject: marioyaw=%s", yawFlip ? "flip" : "native");
    InterlockedExchange(&mario::g_yawFlip, yawFlip);
    LONG vpContext = strstr(t, "vpsource=context") ? 1 : 0;
    LONG posDraw = strstr(t, "possource=draw") ? 1 : 0;
    if (vpContext != g_modeVpContext || posDraw != g_modePosDraw)
        logWrite("inject: vpsource=%s possource=%s", vpContext ? "context" : "global",
                 posDraw ? "draw" : "upload");
    InterlockedExchange(&g_modeVpContext, vpContext);
    LONG camSource = strstr(t, "camsource=eye2288") ? 1 : strstr(t, "camsource=eye1040") ? 2 : 0;
    if (camSource != g_modeCamSource)
        logWrite("inject: camsource=%s", camSource == 1 ? "eye2288" : camSource == 2 ? "eye1040" : "memory");
    InterlockedExchange(&g_modeCamSource, camSource);
    if (const char *tl = strstr(t, "tilt=")) {
        LONG v = static_cast<LONG>(atof(tl + 5) * 1000.0);
        if (v > 0 && v <= 30000 && v != g_upTiltMilliDeg) { logWrite("inject: tilt=%.2f degrees", v / 1000.0); g_upTiltMilliDeg = v; }
    }
    if (const char *lt = strstr(t, "locktol=")) {
        LONG v = static_cast<LONG>(atof(lt + 8) * 10.0);
        if (v > 0 && v <= 500 && v != g_upLockTolMm) { logWrite("inject: locktol=%.1f cm", v / 10.0); g_upLockTolMm = v; }
    }
    // hide=off (test): the overlay hide is off, so only the native player-hide
    // flag (hideplayer=on) acts.  hide=off wins over the built-in hide=on.
    // The overlay hide (learned player meshes) hides the knight's body; Hidden Body hides the
    // weapons.  (It also took nearby NPCs' heads: still to fix.)
    LONG hide = strstr(t, "hide=off") ? 0 : ((strstr(t, "hide=on") || strstr(t, "hide=overlay")) ? 1 : 0);
    if (hide != g_hidePlayer) logWrite("inject: hide=%s (player's own meshes %s)", hide ? "on" : "off",
                                       hide ? "skipped once learned" : "drawn");
    InterlockedExchange(&g_hidePlayer, hide);
    {
        // Experimental: the ChrFlags1 draw bit (mariosetup::hidePlayerTick) did not hide the knight
        // on 1.03, so it is off unless hideplayer=on.  The overlay hide (hide=on) does the hiding.
        const LONG native = strstr(t, "hideplayer=on") ? 1 : 0;
        if (native != mariosetup::g_hidePlayer) logWrite("inject: hideplayer=%s", native ? "on" : "off");
        InterlockedExchange(&mariosetup::g_hidePlayer, native);
        {
            LONG cm = 120;
            if (const char *hr = strstr(t, "hideradius=")) cm = static_cast<LONG>(atof(hr + 11) * 100.0);
            if (cm < 30 || cm > 500) cm = 120;
            if (cm != g_hideRadiusCm) logWrite("inject: hideradius=%.2f m", cm / 100.0);
            InterlockedExchange(&g_hideRadiusCm, cm);
            LONG hip = 50;
            if (const char *hp = strstr(t, "hiphide=")) hip = atol(hp + 8);
            if (hip < 20 || hip > 150) hip = 50;
            const float h2 = hip * hip * 0.0001f;
            if (h2 != g_hipRadius2) logWrite("inject: hiphide=%ld cm", hip);
            g_hipRadius2 = h2;
        }
        {
            const LONG fx = strstr(t, "fxprobe=on") ? 1 : 0;
            if (fx != fxprobe::g_on) logWrite("inject: fxprobe=%s", fx ? "on (particle buffer scan; costs frame time)" : "off");
            InterlockedExchange(&fxprobe::g_on, fx);
            // Off by default (v158): hiding or moving the particles only made the hand fire flicker.
            const LONG mv = strstr(t, "fxmove=flower") ? 2 : strstr(t, "fxmove=hide") ? 1 : 0;
            if (mv != fxprobe::g_moveOn)
                logWrite("inject: fxmove=%s", mv == 2 ? "flower (hand fire on the Fire Flower)" : mv ? "hide (no hand fire)" : "off");
            InterlockedExchange(&fxprobe::g_moveOn, mv);
            if (const char *hh = strstr(t, "fxhand=")) {
                float r = 0, u = 0, fw = 0;
                if (sscanf(hh + 7, "%f,%f,%f", &r, &u, &fw) == 3 &&
                    (r != fxprobe::g_hand[0] || u != fxprobe::g_hand[1] || fw != fxprobe::g_hand[2])) {
                    fxprobe::g_hand[0] = r; fxprobe::g_hand[1] = u; fxprobe::g_hand[2] = fw;
                    logWrite("inject: fxhand=%.2f,%.2f,%.2f (fire offset from the flower bloom, world m)", r, u, fw);
                }
            }
        }
        {
            // Shadow position test: off by default (v162): it matched over half of all shadow
            // casters (the shadow pass's palettes are not laid out like the main pass's).
            {
                const LONG fp = strstr(t, "shadowfp=on") ? 1 : 0;
                if (fp != g_shadowFpOn) logWrite("inject: shadowfp=%s", fp ? "on (knight's shadow by bone fingerprint)" : "off");
                InterlockedExchange(&g_shadowFpOn, fp);
            }
            const LONG sh = strstr(t, "shadowhide=on") ? 1 : 0;
            if (sh != g_shadowHideOn) logWrite("inject: shadowhide=%s", sh ? "on" : "off");
            InterlockedExchange(&g_shadowHideOn, sh);
            // Learned-key skipping in the shadow pass (hidemask 0x10) made the knight's shadow
            // flicker: off unless shadowkeys=on, so his shadow is steady under Mario.
            // shadowkeys=only (v164): just the learned-key skip in the shadow pass, without the
            // smaller-depth-pass and other paths (on = all of them, as before v162).
            const LONG sk = strstr(t, "shadowkeys=on") ? 1 : strstr(t, "shadowkeys=only") ? 2 : 0;
            static LONG lastSk = -1;
            if (sk != lastSk) {
                lastSk = sk;
                logWrite("inject: shadowkeys=%s", sk == 1 ? "on (all paths)" : sk == 2 ? "only (learned knight pieces in the shadow pass)"
                                                                            : "off (knight's shadow kept, steady)");
            }
            InterlockedExchange(&g_shadowKeysOn, sk);
        }
        {
            // Native knight hiding: off by default (v161): with the knight not submitted the
            // camera capture lost its matrices (camera froze, Mario jumped, enemies drew wrong).
            {
                LONG lift = 12;
                if (const char *sa = strstr(t, "stepassist=")) lift = atol(sa + 11);
                if (lift < 0 || lift > 40) lift = 12;
                if (lift != mario::g_stepLiftCm) logWrite("inject: stepassist=%ld cm", lift);
                InterlockedExchange(&mario::g_stepLiftCm, lift);
            }
            {
                const LONG hh = strstr(t, "heldhide=on") ? 1 : 0;  // off by default (v170h: the flask is an effect)
                if (hh != g_heldHideOn) logWrite("inject: heldhide=%s", hh ? "on" : "off");
                InterlockedExchange(&g_heldHideOn, hh);
            }
            {
                LONG lf = 170;
                if (const char *q = strstr(t, "ladderfast=")) lf = atol(q + 11);
                if (lf < 100 || lf > 400) lf = 170;
                if (lf != mario::g_ladderFastPct) logWrite("inject: ladderfast=%ld%%", lf);
                InterlockedExchange(&mario::g_ladderFastPct, lf);
            }
            {
                const LONG op = strstr(t, "objprobe=on") ? 1 : 0;
                if (op != objprobe::g_on) logWrite("inject: objprobe=%s", op ? "on (breakable object fields)" : "off");
                InterlockedExchange(&objprobe::g_on, op);
                LONG kr = 70;
                if (const char *k = strstr(t, "kickrange=")) kr = atol(k + 10);
                if (kr < 20 || kr > 100) kr = 70;
                if (kr != mariocombat::g_kickReachPct) logWrite("inject: kickrange=%ld%%", kr);
                InterlockedExchange(&mariocombat::g_kickReachPct, kr);
                LONG kc = 90;
                if (const char *k = strstr(t, "kickreach=")) kc = atol(k + 10);
                if (kc < 20 || kc > 400) kc = 90;
                if (kc != mariocombat::g_kickReachCm) logWrite("inject: kickreach=%ld cm", kc);
                InterlockedExchange(&mariocombat::g_kickReachCm, kc);
                LONG pr = 50;
                if (const char *k = strstr(t, "poundreach=")) pr = atol(k + 11);
                if (pr < 10 || pr > 400) pr = 50;
                if (pr != mariocombat::g_poundReachCm) logWrite("inject: poundreach=%ld cm", pr);
                InterlockedExchange(&mariocombat::g_poundReachCm, pr);
                const LONG oh = strstr(t, "objhit=off") ? 0 : 1;
                if (oh != mariocombat::g_objHitOn) logWrite("inject: objhit=%s", oh ? "on" : "off");
                InterlockedExchange(&mariocombat::g_objHitOn, oh);
                const LONG eh = strstr(t, "estushide=off") ? 0 : 1;
                if (eh != mariosetup::g_estusHideOn) logWrite("inject: estushide=%s", eh ? "on" : "off");
                InterlockedExchange(&mariosetup::g_estusHideOn, eh);
                {
                    LONG lr = 22, ll = 0;
                    if (const char *q = strstr(t, "ladderreach=")) lr = atol(q + 12);
                    if (const char *q = strstr(t, "ladderlift=")) ll = atol(q + 11);
                    if (lr < -100 || lr > 150) lr = 22;
                    if (ll < -100 || ll > 100) ll = 0;
                    if (lr != mario::g_ladderReachCm || ll != mario::g_ladderLiftCm) logWrite("inject: ladderreach=%ld cm, ladderlift=%ld cm", lr, ll);
                    InterlockedExchange(&mario::g_ladderReachCm, lr);
                    InterlockedExchange(&mario::g_ladderLiftCm, ll);
                }
                LONG lfw = 25, lup = 0;
                if (const char *q = strstr(t, "ladderfwd=")) lfw = atol(q + 10);
                if (const char *q = strstr(t, "ladderup=")) lup = atol(q + 9);
                if (lfw < -100 || lfw > 150) lfw = 25;
                if (lup < -150 || lup > 150) lup = 0;
                if (lfw != mario::g_ladderFwdCm || lup != mario::g_ladderUpCm) logWrite("inject: ladderfwd=%ld ladderup=%ld cm", lfw, lup);
                InterlockedExchange(&mario::g_ladderFwdCm, lfw);
                InterlockedExchange(&mario::g_ladderUpCm, lup);
                const LONG wc = strstr(t, "wingcap=off") ? 0 : 1;
                if (wc != mario::g_wingCapOn) logWrite("inject: wingcap=%s", wc ? "on" : "off");
                InterlockedExchange(&mario::g_wingCapOn, wc);
                const LONG awf = strstr(t, "airwall=off") ? 0 : 1;
                if (awf != mario::g_airWallFixOn) logWrite("inject: airwall=%s", awf ? "on" : "off");
                InterlockedExchange(&mario::g_airWallFixOn, awf);
                const LONG wsk = strstr(t, "wallfix=off") ? 0 : 1;
                if (wsk != mario::g_wallSkirtOn) logWrite("inject: wallfix=%s", wsk ? "on" : "off");
                InterlockedExchange(&mario::g_wallSkirtOn, wsk);
                const LONG dpl = strstr(t, "deathplane=off") ? 0 : 1;
                if (dpl != mario::g_deathPlaneOn) logWrite("inject: deathplane=%s", dpl ? "on" : "off");
                InterlockedExchange(&mario::g_deathPlaneOn, dpl);
                const LONG dh = strstr(t, "dodgehit=off") ? 0 : 1;
                if (dh != mariocombat::g_dodgeHitOn) logWrite("inject: dodgehit=%s", dh ? "on" : "off");
                InterlockedExchange(&mariocombat::g_dodgeHitOn, dh);
                LONG dp = 30;
                if (const char *k = strstr(t, "dodgepush=")) dp = atol(k + 10);
                if (dp < 0 || dp > 150) dp = 30;
                if (dp != mariocombat::g_dodgePushCm) logWrite("inject: dodgepush=%ld cm", dp);
                InterlockedExchange(&mariocombat::g_dodgePushCm, dp);
            }
            const LONG nh = strstr(t, "nativehide=on") ? 1 : 0;
            if (nh != mariosetup::g_nativeHideWanted) logWrite("inject: nativehide=%s", nh ? "on" : "off");
            InterlockedExchange(&mariosetup::g_nativeHideWanted, nh);
        }
        {
            // PTDE parity switches (v159), all on by default; "<name>=off" turns one off.
            struct Toggle { const char *off; volatile LONG *flag; const char *name; };
            const Toggle toggles[] = {
                {"footwait=off", &mario::g_footWaitOn, "footwait (native footing before casts, items, interactions)"},
                {"lava=off", &mario::g_envOn, "lava (burning bounce)"},
                {"pushrules=off", &mariocombat::g_pushRules, "pushrules (kick push needs a poise break)"},
                {"hitflash=off", &mariocombat::g_flashOn, "hitflash"},
                {"recovery=off", &mariosetup::g_recoveryOn, "recovery (faster hit reactions)"},
            };
            for (const Toggle &tg : toggles) {
                const LONG v = strstr(t, tg.off) ? 0 : 1;
                if (v != *tg.flag) logWrite("inject: %s %s", tg.name, v ? "on" : "off");
                InterlockedExchange(tg.flag, v);
            }
        }
        const LONG hb = strstr(t, "hiddenbody=on") ? 1 : 0;
        if (hb != mariosetup::g_hiddenBody) logWrite("inject: hiddenbody=%s", hb ? "on" : "off");
        InterlockedExchange(&mariosetup::g_hiddenBody, hb);
    }
    {
        LONG step = strstr(t, "mario=step") ? 1 : 0;
        if (step != mario::g_stepMode) logWrite("inject: mario=%s", step ? "step (SM64 simulated)" : "pose");
        InterlockedExchange(&mario::g_stepMode, step);
        InterlockedExchange(&mario::g_stepCamFlip, strstr(t, "stepcam=flip") ? 1 : 0);
        InterlockedExchange(&mario::g_stepXFlip, strstr(t, "stepx=flip") ? 1 : 0);
        InterlockedExchange(&mario::g_stepYFlip, strstr(t, "stepy=flip") ? 1 : 0);
        auto vkSetting = [&](const char *key, volatile LONG *dst, LONG fallback) {
            LONG v = fallback;
            if (const char *p = strstr(t, key)) v = static_cast<LONG>(strtol(p + strlen(key), nullptr, 0));
            if (v != *dst) logWrite("inject: %s0x%lX", key, v);
            InterlockedExchange(dst, v);
        };
        {
            LONG drive = strstr(t, "drive=warp") ? 1 : strstr(t, "drive=pos") ? 2 : strstr(t, "drive=input") ? 3
                       : strstr(t, "drive=native") ? 4 : 0;
            if (drive != mario::g_drive)
                logWrite("inject: drive=%s", drive == 1 ? "warp" : drive == 2 ? "pos" : drive == 3 ? "input"
                                             : drive == 4 ? "native" : "off");
            InterlockedExchange(&nativemotion::g_flipX, strstr(t, "nativex=flip") ? 1 : 0);
            InterlockedExchange(&mario::g_terrainOn, strstr(t, "terrain=off") ? 0 : 1);
            InterlockedExchange(&mario::g_terrainFlip, strstr(t, "terrainflip=off") ? 0 : 1);
            InterlockedExchange(&mario::g_knightXFlip, strstr(t, "knightx=flip") ? 1 : 0);
            InterlockedExchange(&mario::g_drive, drive);
        }
        {
            LONG speed = 1000;
            if (const char *p = strstr(t, "stepspeed=")) {
                const double v = atof(p + 10);
                if (v >= 0.1 && v <= 1.0) speed = static_cast<LONG>(v * 1000.0);
            }
            if (speed != mario::g_stepSpeedMilli) logWrite("inject: stepspeed=%.2f", speed / 1000.0);
            InterlockedExchange(&mario::g_stepSpeedMilli, speed);
            {
                const LONG fl = strstr(t, "knightfloat=on") ? 1 : 0;
                if (fl != nativemotion::g_floatOn) logWrite("inject: knightfloat=%s", fl ? "on" : "off");
                InterlockedExchange(&nativemotion::g_floatOn, fl);
            }
            {
                LONG ds = 47000;
                if (const char *p = strstr(t, "dodgespeed=")) {
                    const double v = atof(p + 11);
                    if (v >= 5.0 && v <= 60.0) ds = static_cast<LONG>(v * 1000.0);
                }
                if (ds != mario::g_dodgeSpeedMilli) logWrite("inject: dodgespeed=%.1f", ds / 1000.0);
                InterlockedExchange(&mario::g_dodgeSpeedMilli, ds);
            }
            {
                auto intSetting = [&](const char *key, volatile LONG *dst, LONG fallback) {
                    LONG v = fallback;
                    if (const char *p = strstr(t, key)) { const int x = atoi(p + strlen(key)); if (x >= 0 && x <= 200) v = x; }
                    if (v != *dst) logWrite("inject: %s%ld", key, v);
                    InterlockedExchange(dst, v);
                };
                intSetting("punchstamina=", &mario::g_punchStamina, 12);
                intSetting("kickstamina=", &mario::g_kickStamina, 20);
                intSetting("dodgestamina=", &mario::g_dodgeStamina, 16);
            }
            InterlockedExchange(&mariosetup::g_giveCaestus, strstr(t, "givecaestus=on") ? 1 : 0);
            InterlockedExchange(&mariosetup::g_giveWingCap, strstr(t, "givewingcap=on") ? 1 : 0);
            InterlockedExchange(&mariosetup::g_testHiddenBody, strstr(t, "givehiddenbody=on") ? 1 : 0);
            if (strstr(t, "givehiddenbody=on")) logWrite("inject: givehiddenbody=on");
            // hideplayer is set above (on unless hideplayer=off); not reset here.
            InterlockedExchange(&actionprobe::g_on, strstr(t, "actionprobe=on") ? 1 : 0);
            {
                const LONG c = strstr(t, "combat=off") ? 0 : 1;
                if (c != mariocombat::g_enabled) logWrite("inject: combat=%s", c ? "on" : "off");
                InterlockedExchange(&mariocombat::g_enabled, c);
            }
            LONG travel = 1000;
            if (const char *p = strstr(t, "travel=")) {
                const double v = atof(p + 7);
                if (v >= 0.5 && v <= 1.0) travel = static_cast<LONG>(v * 1000.0);
            }
            if (travel != mario::g_travelMilli) logWrite("inject: travel=%.2f", travel / 1000.0);
            InterlockedExchange(&mario::g_travelMilli, travel);
            LONG tether = 800;
            if (const char *p = strstr(t, "tether=")) {
                const double v = atof(p + 7);
                if (v >= 0.05 && v <= 2.0) tether = static_cast<LONG>(v * 1000.0);
            }
            if (tether != mario::g_tetherMm) logWrite("inject: tether=%.2f m", tether / 1000.0);
            InterlockedExchange(&mario::g_tetherMm, tether);
        }
        InterlockedExchange(&mario::g_diagMotion, strstr(t, "motionprobe=on") ? 1 : 0);
        vkSetting("keyjump=", &mario::g_keyJump, 'E');
        vkSetting("keyheavy=", &mario::g_keyHeavy, VK_RBUTTON);
        vkSetting("keydodge=", &mario::g_keyDodge, VK_SPACE);
        vkSetting("keyattack=", &mario::g_keyAttack, VK_LBUTTON);
        vkSetting("keycrouch=", &mario::g_keyCrouch, VK_LCONTROL);
        vkSetting("keywalk=", &mario::g_keyWalk, VK_MENU);
    }
    // "diag=on" as a whole key: "poisediag=on" contains it too (v134 turned the
    // camera diagnostics on with the poise one).
    LONG diag = 0;
    for (const char *k = strstr(t, "diag=on"); k && !diag; k = strstr(k + 1, "diag=on"))
        if (k == t || !isalpha(static_cast<unsigned char>(k[-1]))) diag = 1;
    if (diag != g_diag) logWrite("inject: diag=%s", diag ? "on" : "off");
    InterlockedExchange(&g_diag, diag);
    {
        const LONG poise = strstr(t, "poisediag=on") ? 1 : 0;
        if (poise != mariocombat::g_poiseDiag) logWrite("inject: poisediag=%s", poise ? "on" : "off");
        InterlockedExchange(&mariocombat::g_poiseDiag, poise);
    }
    {
        LONG n = 0;
        if (const char *hn = strstr(t, "hidenot=")) {
            const char *s = hn + 8;
            while (n < 16 && *s >= '0' && *s <= '9') {
                g_hideNot[n++] = strtol(s, const_cast<char **>(&s), 10);
                if (*s == ',') ++s;
            }
        }
        if (n != g_hideNotN) logWrite("inject: hidenot has %ld index counts", n);
        g_hideNotN = n;
    }
    if (const char *hm = strstr(t, "hidemask=")) {
        LONG v = static_cast<LONG>(strtol(hm + 9, nullptr, 16)) & 0x3F;
        if (v != g_hideMask) logWrite("inject: hidemask=%lx (1 after upload, 2 other passes, 4 index key, 8 stream-out)", v);
        InterlockedExchange(&g_hideMask, v);
    }
    LONG upFill = strstr(t, "upfill=on") ? 1 : 0;
    if (upFill != g_modeUploadFill) logWrite("inject: upfill=%s", upFill ? "on" : "off");
    InterlockedExchange(&g_modeUploadFill, upFill);
    LONG upload = strstr(t, "upload=on") ? 1 : 0;
    if (upload != g_modeUpload) logWrite("inject: upload=%s", upload ? "on" : "off");
    InterlockedExchange(&g_modeUpload, upload);
    LONG syncOff = strstr(t, "sync=off") ? 1 : 0;
    if (syncOff != g_modeSyncOff) logWrite("inject: sync=%s", syncOff ? "off" : "on");
    InterlockedExchange(&g_modeSyncOff, syncOff);
    LONG vpSlot48 = strstr(t, "vpslot=48") ? 1 : 0;
    if (vpSlot48 != g_modeVpSlot48) logWrite("inject: vpslot=%s", vpSlot48 ? "48" : "0");
    InterlockedExchange(&g_modeVpSlot48, vpSlot48);
    InterlockedExchange(&g_modePosDraw, posDraw);
    if (useRenderer != g_modeRenderer)
        logWrite("inject: draw=%s", useRenderer ? "renderer" : "test");
    InterlockedExchange(&g_modeRenderer, useRenderer);
    LONG camOffset = -1;
    if (const char *c = strstr(t, "camoff=")) {
        unsigned v = 0;
        if (sscanf(c + 7, "%x", &v) == 1 && v + 12 <= 512 && (v % 4) == 0) camOffset = static_cast<LONG>(v);
    }
    if (anchorPlayer != g_modeAnchorPlayer || camOffset != g_modeCamOffset)
        logWrite("inject: anchor=%s camoff=0x%lX", anchorPlayer ? "player" : "camera", camOffset);
    InterlockedExchange(&g_modeAnchorPlayer, anchorPlayer);
    InterlockedExchange(&g_modeCamOffset, camOffset);
    float values[8] = {1, 0, 1, 1, 1, 0, 1, 1};
    parseFloat4(t, "rt0=", values);
    parseFloat4(t, "rt1=", values + 4);
    bool valuesChanged = memcmp(values, g_modeTargetValues, sizeof(values)) != 0;
    memcpy(g_modeTargetValues, values, sizeof(values));
    if (write != g_modeWrite || depthWrite != g_modeWorldDepthWrite || valuesChanged) {
        InterlockedExchange(&g_modeWrite, write);
        InterlockedExchange(&g_modeWorldDepthWrite, depthWrite);
        logWrite("inject: write=%s rt0=(%.2f,%.2f,%.2f,%.2f) rt1=(%.2f,%.2f,%.2f,%.2f) world depthwrite=%s",
                 write == kWriteRt0 ? "rt0" : write == kWriteRt1 ? "rt1" : "both",
                 values[0], values[1], values[2], values[3], values[4], values[5], values[6],
                 values[7], depthWrite ? "on" : "off");
    }

    bool changed = targets != g_modeTargets || screen != g_modeScreen ||
                   world != g_modeWorld || depth != g_modeDepth;
    InterlockedExchange(&g_modeTargets, targets);
    InterlockedExchange(&g_modeScreen, screen);
    InterlockedExchange(&g_modeWorld, world);
    InterlockedExchange(&g_modeDepth, depth);
    if (changed)
        logWrite("inject: mode targets=%s screen=%s world=%s depth=%s",
                 targets == 0 ? "none" : targets == kTargetMulti ? "multi"
                                       : targets == kTargetSingle ? "single" : "both",
                 screen ? "on" : "off", world ? "on" : "off",
                 depth == kDepthGequal ? "gequal" : depth == kDepthAlways ? "always" : "lequal");
}

DWORD WINAPI modePollThread(void *) {
    static char last[8192] = {};
    for (;;) {
        wchar_t path[MAX_PATH] = {};
        wcscpy_s(path, MAX_PATH, remastered::install::settingsPath().c_str());  // v171: player settings file first
        char buf[4096] = {};
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
            CloseHandle(f);
            buf[got] = 0;
            for (DWORD i = 0; i < got; ++i)
                if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = static_cast<char>(buf[i] - 'A' + 'a');
            // Lines starting with '#' are comments: blank them so text in them
            // (such as "hide=off") is not read as a setting.
            bool lineStart = true, comment = false;
            for (DWORD i = 0; i < got; ++i) {
                if (lineStart) comment = buf[i] == '#';
                lineStart = buf[i] == '\n';
                if (comment && buf[i] != '\n') buf[i] = ' ';
            }
        }
        // A missing file means all defaults.
        static char merged[8192];
        mergeModeDefaults(buf, merged, sizeof(merged));
        if (strcmp(merged, last) != 0) {
            memcpy(last, merged, sizeof(last));
            applyModeText(merged);
        }
        Sleep(500);
    }
}

} // namespace

// Resources are created through the ID3D11Device interface declared by the
// toolchain's d3d11.h, not through hand-numbered vtable slots and hand-typed
// function pointers.  The earlier hand-typed signatures were wrong:
//   - CreateVertexShader/CreatePixelShader passed a string where the API takes
//     an ID3D11ClassLinkage*, so the runtime would treat text as a COM object;
//   - CreateBuffer carried two extra parameters, so the D3D11_CPU_ACCESS_WRITE
//     constant (0x10000) arrived in the ppBuffer position and the runtime wrote
//     the new buffer pointer to address 0x10000.
// The compiler-checked interface removes that whole class of mistake.
// ---------------------------------------------------------------------------
// DSRRenderer path.  One renderer for the device, created and prepared (shader
// compile, pipeline objects) on the worker thread; submissions come from
// whichever render thread records the main pass, so they are serialised.
namespace {
DSRRenderer *g_renderer = nullptr;
ID3D11Device *g_rendererDevice = nullptr;
CRITICAL_SECTION g_rendererLock;
volatile LONG g_rendererReady = 0;

void prepareRenderer(ID3D11Device *device) {
    InitializeCriticalSection(&g_rendererLock);
    g_renderer = new DSRRenderer(device, nullptr);
    g_rendererDevice = device;
    if (g_renderer->prepare()) {
        InterlockedExchange(&g_rendererReady, 1);
        logWrite("renderer: DSRRenderer prepared (shaders compiled, pipeline objects created)");
    } else {
        logWrite("renderer: DSRRenderer prepare FAILED; draw=renderer stays off");
    }
}

// Row-major, row-vector (v' = v * M) matrices as render_api.h expects.
void rendererCamera(const float *vpRows, const float *cameraPos, SCRenderAPI::Camera *out) {
    for (int i = 0; i < 16; ++i) {
        out->world[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        out->view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    out->world[12] = -cameraPos[0];
    out->world[13] = -cameraPos[1];
    out->world[14] = -cameraPos[2];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out->projection[c * 4 + r] = vpRows[r * 4 + c];
}

// ---------------------------------------------------------------------------
// Tracking-jitter measurement.
//
// For each new frame (a change in the main view-projection), the player point
// (feet + 1 m) is projected with several candidate (matrix, positions) pairings
// and the frame-to-frame second difference of its screen position is
// accumulated, bucketed by where the camera sits relative to the character's
// facing (four 90-degree sectors).  A pairing that matches the image
// moves the point smoothly; a mismatched one shows up as jitter.  Logged every
// 5 s.  Measurement only: the drawn marker still uses the configured pairing.
namespace {
enum { kVariants = 5, kSectors = 4 };
const char *kVariantNames[kVariants] = {"vp0+upload", "vp0+draw", "vp0+worker", "vp48+upload",
                                        "render"};
// Camera bearing minus facing angle; which of these is "behind" is read off
// a session where the camera is known to be behind the character.
const char *kSectorNames[kSectors] = {"r0", "r90", "r180", "r270"};
struct JitterState {
    float last[kVariants][2][2] = {};  // two previous NDC (x, y) per variant
    int seen[kVariants] = {};
    double sum[kVariants][kSectors] = {};
    int count[kVariants][kSectors] = {};
    float lastVp[16] = {};
    DWORD nextLog = 0;
};
JitterState g_jitter;

bool projectNdc(const float *vpRows, const float *camera, const float *player, float ndc[2]) {
    const float p[3] = {player[0] - camera[0], player[1] + 1.0f - camera[1], player[2] - camera[2]};
    float c[4];
    for (int r = 0; r < 4; ++r)
        c[r] = vpRows[r * 4] * p[0] + vpRows[r * 4 + 1] * p[1] + vpRows[r * 4 + 2] * p[2] + vpRows[r * 4 + 3];
    if (c[3] < 0.1f) return false;
    ndc[0] = c[0] / c[3];
    ndc[1] = c[1] / c[3];
    return true;
}

// Called with g_rendererLock held.
void measureJitter(const CameraState &cam) {
    JitterState &j = g_jitter;
    if (!cam.valid || !cam.positionsValid) return;
    if (!memcmp(j.lastVp, cam.viewProjection.m, sizeof(j.lastVp))) return;  // same frame
    memcpy(j.lastVp, cam.viewProjection.m, sizeof(j.lastVp));

    float livePlayer[3], liveCamera[3];
    bool live = readLivePositions(livePlayer, liveCamera);
    WorldObservation w = g_world;

    static const float origin[3] = {0, 0, 0};
    const float *vps[kVariants] = {cam.viewProjection.m, cam.viewProjection.m, cam.viewProjection.m,
                                   cam.viewProjectionAlt.m, cam.renderPlayerVp};
    const float *cams[kVariants] = {cam.cameraPos, live ? liveCamera : nullptr,
                                    w.cameraValid ? w.camera : nullptr, cam.cameraPos,
                                    cam.renderPlayerValid ? origin : nullptr};
    const float *players[kVariants] = {cam.playerPos, live ? livePlayer : nullptr,
                                       w.playerValid ? w.player : nullptr, cam.playerPos,
                                       cam.renderPlayerRel};

    // Sector: camera bearing around the character relative to its facing.
    float dx = cam.cameraPos[0] - cam.playerPos[0], dz = cam.cameraPos[2] - cam.playerPos[2];
    float rel = std::atan2(dx, dz) - w.playerAngle;
    while (rel < 0) rel += 6.2831853f;
    while (rel >= 6.2831853f) rel -= 6.2831853f;
    int sector = static_cast<int>((rel + 0.7853982f) / 1.5707963f) % kSectors;

    for (int v = 0; v < kVariants; ++v) {
        float ndc[2];
        if (!cams[v] || !players[v] || !projectNdc(vps[v], cams[v], players[v], ndc)) { j.seen[v] = 0; continue; }
        if (j.seen[v] >= 2) {
            float ax = ndc[0] - 2 * j.last[v][1][0] + j.last[v][0][0];
            float ay = ndc[1] - 2 * j.last[v][1][1] + j.last[v][0][1];
            j.sum[v][sector] += std::sqrt(ax * ax + ay * ay);
            ++j.count[v][sector];
        }
        j.last[v][0][0] = j.last[v][1][0]; j.last[v][0][1] = j.last[v][1][1];
        j.last[v][1][0] = ndc[0]; j.last[v][1][1] = ndc[1];
        if (j.seen[v] < 2) ++j.seen[v];
    }

    DWORD now = GetTickCount();
    if (now < j.nextLog) return;
    j.nextLog = now + 5000;
    for (int v = 0; v < kVariants; ++v) {
        char line[300];
        int o = _snprintf(line, sizeof(line), "jitter: %-11s", kVariantNames[v]);
        for (int s = 0; s < kSectors; ++s) {
            if (j.count[v][s])
                o += _snprintf(line + o, sizeof(line) - o, " %s=%.4f(n%d)", kSectorNames[s],
                               j.sum[v][s] / j.count[v][s], j.count[v][s]);
            j.sum[v][s] = 0; j.count[v][s] = 0;
        }
        logWrite("%s", line);
    }
}
} // namespace

// Game memory runs a few frames ahead of the image being drawn; the camera
// now comes from the frame itself (paired eye), so the player must be taken
// from the same moment or Mario leads the character while walking (live
// 2026-10-19).  Memory samples are kept with their time; the sample whose
// camera equals the frame's eye gives the lag, and the player is read at
// now - lag.  The lag is remembered for when the camera is still.
namespace framesync {
// Memory samples taken at each draw.  `frame` counts game updates: it advances
// whenever the camera or player value differs from the previous sample.
struct Sample { double t; float camera[3], player[3]; unsigned frame; };
SRWLOCK lock = SRWLOCK_INIT;
Sample history[128];
unsigned head = 0, count = 0;
// How many game updates memory runs ahead of the drawn frame, measured while
// the camera moves; the most common recent value is used when it is still.
// Whole updates replace the earlier millisecond estimate, which wandered
// between 14 and 234 ms and made Mario stray (live 2026-10-19).
unsigned leadRing[64];
unsigned leadCount = 0;
LONG exact = 0, counted = 0, live = 0;

double now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static float dist2(const float *a, const float *b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// memCamera/livePlayer: values read now.  eye: the drawn frame's camera.
// player receives the position at the frame's moment.
void align(const float memCamera[3], const float livePlayer[3], const float eye[3], float player[3]) {
    const double t = now();
    AcquireSRWLockExclusive(&lock);
    const unsigned prev = (head + 127) % 128;
    Sample &s = history[head];
    s.t = t;
    memcpy(s.camera, memCamera, sizeof(s.camera));
    memcpy(s.player, livePlayer, sizeof(s.player));
    s.frame = count == 0 ? 0
              : history[prev].frame + ((dist2(history[prev].camera, memCamera) > 0 ||
                                        dist2(history[prev].player, livePlayer) > 0) ? 1u : 0u);
    const unsigned newest = head;
    head = (head + 1) % 128;
    if (count < 128) ++count;

    memcpy(player, livePlayer, sizeof(float) * 3);

    // Exact match only while the camera is moving at this moment: the newest
    // sample must already differ from the drawn frame's eye.  A camera that
    // has just stopped matches every recent sample equally, and the newest
    // (several updates ahead) used to win.
    int bestIdx = -1;
    float bestD = 3e38f;
    if (dist2(history[newest].camera, eye) > 0.02f * 0.02f) {
        for (unsigned k = 0; k < count; ++k) {
            const Sample &h = history[k];
            if (t - h.t > 0.15) continue;
            const float d = dist2(h.camera, eye);
            if (d < bestD || (d == bestD && bestIdx >= 0 && h.t > history[bestIdx].t)) { bestD = d; bestIdx = int(k); }
        }
    }
    if (bestIdx >= 0 && bestD < 0.01f * 0.01f) {
        memcpy(player, history[bestIdx].player, sizeof(float) * 3);
        const unsigned lead = history[newest].frame - history[bestIdx].frame;
        if (lead < 16) leadRing[leadCount++ % 64] = lead;
        ++exact;
    } else if (leadCount) {
        // Most common lead over the recent measurements.
        unsigned votes[16] = {}, mode = 0;
        const unsigned n = leadCount < 64 ? leadCount : 64;
        for (unsigned i = 0; i < n; ++i) ++votes[leadRing[i]];
        for (unsigned v = 1; v < 16; ++v) if (votes[v] > votes[mode]) mode = v;
        // Newest sample that is `mode` updates behind the newest one.
        const unsigned want = history[newest].frame - mode;
        double bestT = -1;
        for (unsigned k = 0; k < count; ++k)
            if (history[k].frame == want && history[k].t > bestT) {
                bestT = history[k].t;
                memcpy(player, history[k].player, sizeof(float) * 3);
            }
        if (bestT >= 0) ++counted; else ++live;
    } else {
        ++live;
    }

    static double lastLog = 0;
    if (t - lastLog > 5.0) {
        lastLog = t;
        unsigned votes[16] = {}, mode = 0;
        const unsigned n = leadCount < 64 ? leadCount : 64;
        for (unsigned i = 0; i < n; ++i) ++votes[leadRing[i]];
        for (unsigned v = 1; v < 16; ++v) if (votes[v] > votes[mode]) mode = v;
        logWrite("sync: memory leads the drawn frame by %u game updates (%u of last %u measurements); player "
                 "from matched frame %ld, from update count %ld, live %ld",
                 mode, votes[mode], n, exact, counted, live);
    }
    ReleaseSRWLockExclusive(&lock);
}
} // namespace framesync

// Player marker through the renderer, in WORLD coordinates (the camera matrices
// carry the camera-relative shift).  Returns false when nothing was submitted.
// Cost of each Mario draw on the game's render threads, logged every 5 s, so a
// report of the game lagging can be checked against what the hook spends.
namespace drawperf {
// Per-stage time inside one draw: 0 positions+camera, 1 mesh copy, 2 waiting
// for the renderer lock, 3 renderer work, 4 the rest.
SRWLOCK lock = SRWLOCK_INIT;
double stage[5] = {};
thread_local double last = 0;
inline void begin() { last = framesync::now(); }
inline void lap(int i) {
    const double n = framesync::now(), d = n - last;
    last = n;
    AcquireSRWLockExclusive(&lock);
    stage[i] += d;
    ReleaseSRWLockExclusive(&lock);
}
} // namespace drawperf

struct DrawTimer {
    double start = framesync::now();
    DrawTimer() { drawperf::begin(); }
    ~DrawTimer() {
        static SRWLOCK lock = SRWLOCK_INIT;
        static double total = 0, worst = 0, lastLog = 0;
        static long count = 0;
        const double end = framesync::now(), spent = end - start;
        AcquireSRWLockExclusive(&lock);
        total += spent; ++count;
        if (spent > worst) worst = spent;
        if (end - lastLog > 5.0) {
            if (lastLog > 0) {
                logWrite("perf: %ld Mario draws in %.1f s, average %.3f ms, worst %.3f ms", count, end - lastLog,
                         total / count * 1000.0, worst * 1000.0);
                AcquireSRWLockExclusive(&drawperf::lock);
                logWrite("perf: per draw - positions/camera %.3f ms, mesh copy %.3f ms, lock wait %.3f ms, "
                         "renderer %.3f ms, other %.3f ms",
                         drawperf::stage[0] / count * 1000.0, drawperf::stage[1] / count * 1000.0,
                         drawperf::stage[2] / count * 1000.0, drawperf::stage[3] / count * 1000.0,
                         drawperf::stage[4] / count * 1000.0);
                for (double &s : drawperf::stage) s = 0;
                ReleaseSRWLockExclusive(&drawperf::lock);
            }
            lastLog = end; total = 0; worst = 0; count = 0;
        }
        ReleaseSRWLockExclusive(&lock);
    }
};

bool drawRendererMarker(ID3D11DeviceContext *context, void *colorView, void *depthView) {
    DrawTimer timer;
    if (!g_rendererReady || !colorView || !depthView) return false;
    // This context's own matrix and the positions read with it (see
    // CamCtxHooks::vp); the global latest matrix is only a fallback.
    CamCtxHooks *ctx = findCamCtx(context);
    CameraState cam = getCameraState();
    float vpLocal[16], player[3], camera[3];
    if (g_modeVpContext && ctx && ctx->vpValid && ctx->positionsValid) {
        memcpy(vpLocal, ctx->vp, sizeof(vpLocal));
        memcpy(player, ctx->playerPos, sizeof(player));
        memcpy(camera, ctx->cameraPos, sizeof(camera));
    } else if (cam.valid && cam.positionsValid) {
        memcpy(vpLocal, g_modeVpSlot48 ? cam.viewProjectionAlt.m : cam.viewProjection.m, sizeof(vpLocal));
        memcpy(player, cam.playerPos, sizeof(player));
        memcpy(camera, cam.cameraPos, sizeof(camera));
    } else {
        return false;
    }
    if (g_modePosDraw && !readLivePositions(player, camera)) return false;
    float memPlayer[3], memCamera0[3];
    memcpy(memPlayer, player, sizeof(memPlayer));
    memcpy(memCamera0, camera, sizeof(memCamera0));

    // anchor=render: the player's own draw transform and the matrix from the
    // same upload; positions are camera-relative, so the camera sits at origin.
    if (g_modeAnchorRender && cam.renderPlayerValid) {
        memcpy(vpLocal, cam.renderPlayerVp, sizeof(vpLocal));
        memcpy(player, cam.renderPlayerRel, sizeof(player));
        camera[0] = camera[1] = camera[2] = 0.0f;
    }
    const LONG camSource = g_modeCamSource;
    if (camSource == 1 && g_eyeValid2288) memcpy(camera, g_eyePos2288, sizeof(camera));
    else if (camSource == 2) {
        // The eye uploaded with this exact view (matched by facing); the
        // distance-filtered latest eye is only a fallback.
        float paired[3], dotBest = 0.0f, memCamera[3];
        memcpy(memCamera, camera, sizeof(memCamera));
        // First choice: the eye uploaded on this same context, if it faces the
        // way of the matrix being drawn with.
        bool hit = false;
        static volatile LONG fromContext = 0, fromRing = 0;
        if (ctx && ctx->eyeValid) {
            const float *fw = vpLocal + 12, *ef = ctx->eyeForward;
            const float d = fw[0] * ef[0] + fw[1] * ef[1] + fw[2] * ef[2];
            if (d >= 0.99999f) {
                memcpy(paired, ctx->eye, sizeof(paired));
                dotBest = d;
                hit = true;
                InterlockedIncrement(&fromContext);
            }
        }
        if (!hit) {
            hit = pairedEye(vpLocal, paired, &dotBest, memCamera);
            if (hit) InterlockedIncrement(&fromRing);
        }
        // On a miss, the last matched matrix and eye are drawn together; the
        // old fallback mixed a fresh matrix with an unrelated eye, which made
        // Mario jump on about one draw in ten (live 2026-10-19).
        static SRWLOCK pairLock = SRWLOCK_INIT;
        static float lastVp[16], lastEye[3];
        static bool havePair = false;
        bool usable = hit;
        if (hit) {
            AcquireSRWLockExclusive(&pairLock);
            memcpy(lastVp, vpLocal, sizeof(lastVp));
            memcpy(lastEye, paired, sizeof(lastEye));
            havePair = true;
            ReleaseSRWLockExclusive(&pairLock);
        } else {
            AcquireSRWLockShared(&pairLock);
            if (havePair) {
                memcpy(vpLocal, lastVp, sizeof(lastVp));
                memcpy(paired, lastEye, sizeof(lastEye));
                usable = true;
            }
            ReleaseSRWLockShared(&pairLock);
        }
        if (usable) {
            memcpy(camera, paired, sizeof(camera));
            const float livePlayer[3] = {player[0], player[1], player[2]};
            float aligned[3];
            framesync::align(memCamera, livePlayer, paired, aligned);
            if (!g_modeSyncOff) memcpy(player, aligned, sizeof(aligned));
        } else if (g_eyeValid1040) {
            memcpy(camera, g_eyePos1040, sizeof(camera));
        }
        static volatile LONG pairHit = 0, pairMiss = 0;
        InterlockedIncrement(hit ? &pairHit : &pairMiss);
        static volatile LONG lastPairLog = 0;
        LONG nowPair = static_cast<LONG>(GetTickCount()), prevPair = lastPairLog;
        if (nowPair - prevPair > 5000 && InterlockedCompareExchange(&lastPairLog, nowPair, prevPair) == prevPair)
            logWrite("cam: paired eye matched %ld (this context %ld, by facing %ld), fell back %ld (last best dot %.6f)",
                     pairHit, fromContext, fromRing, pairMiss, dotBest);
    }

    // upload=on: the player's root transform, view-projection and camera from
    // one upload the engine drew the player with (see CamCtxHooks::upValid).
    // This context's own upload first; otherwise the latest from any context,
    // still as a matched (matrix, transform) pair.  Memory only identifies the
    // player's transform and calibrates the feet offset while standing still.
    {
        static SRWLOCK calLock = SRWLOCK_INIT;
        static float calib[3];
        static int calibN = 0;
        static float lastMem[3];
        static double stillSince = 0;
        static volatile LONG samePass = 0, sameWindow = 0, previous = 0, missed = 0;
        const double tNow = framesync::now();
        const DWORD tick = GetTickCount();
        UploadAnchor use{};
        bool have = false;
        // Frame identity by time: the player transform uploaded on this
        // context since its geometry pass began is from this frame; failing
        // that, one uploaded by any context in the same interval; failing that,
        // the newest from before it (a frame behind rather than ahead - the
        // newest overall ran ahead, toward the camera).
        static volatile LONG byRepeat = 0;
        bool horizontalMode = false;  // bone gives x/z only; height from memory
        static volatile LONG64 repeatTop = 0, repeatSecond = 0;
        const bool uploadAnchor = g_modeUpload && !g_nativeHideActive;
        if (uploadAnchor && ctx && ctx->passStart > 0 && ctx->passCandN > 0) {
            // The transform repeated in the most uploads of this pass: the
            // character's root (see CamCtxHooks::passCands).
            int best = 0, second = 0;
            for (int q = 1; q < ctx->passCandN; ++q)
                if (ctx->passCands[q].count > ctx->passCands[best].count) best = q;
            for (int q = 0; q < ctx->passCandN; ++q)
                if (q != best && ctx->passCands[q].count > second) second = ctx->passCands[q].count;
            const CamCtxHooks::PassCand &pc = ctx->passCands[best];
            // Only the pelvis repeats across pieces (every leg and most chest
            // meshes carry it in slot 0).  A chunk whose best slot-0 bone
            // appears once holds some other bone; drawing from it put a second
            // Mario under the floor (live 2026-10-20).  Another chunk of the
            // same frame carries the pelvis and draws him.
            if (pc.count < 2) {
                static volatile LONG notPelvis = 0;
                InterlockedIncrement(&notPelvis);
                static volatile LONG lastNpLog = 0;
                LONG nowNp = static_cast<LONG>(GetTickCount()), prevNp = lastNpLog;
                if (nowNp - prevNp > 5000 && InterlockedCompareExchange(&lastNpLog, nowNp, prevNp) == prevNp)
                    logWrite("upload: skipped %ld chunks whose slot-0 bone appeared only once", notPelvis);
                return false;
            }
            use.tick = tick; use.index = 0; use.t = tNow;
            memcpy(use.vp, pc.vp, sizeof(use.vp));
            // The pelvis: the slot-0 bone matrix repeated most across this
            // pass's player uploads (see the slot-0 note in camera_capture).
            // The palette entry is a skinning matrix (current pose times
            // inverse bind), so its own translation is the model origin at the
            // feet carried round the hip: every hip twist swung it sideways
            // (Mario popped knee to knee, live 2026-10-20).  The hip joint
            // itself is the pelvis's bind position pushed through the matrix:
            // in chr/c0000.flver that is Master's translation (0, 0.970,
            // -0.054); Lower_Root and Pelvis add no offset.
            {
                static const float kPelvisBind[3] = {0.0f, 0.970f, -0.054f};
                const float *c0 = pc.rot, *c1 = pc.rot + 3, *c2 = pc.rot + 6;  // axis columns
                for (int k = 0; k < 3; ++k)
                    use.pos[k] = pc.pos[k] + c0[k] * kPelvisBind[0] + c1[k] * kPelvisBind[1] + c2[k] * kPelvisBind[2];
            }
            {
                static volatile LONG pelvisRepeated = 0, pelvisSingle = 0;
                InterlockedIncrement(pc.count >= 2 ? &pelvisRepeated : &pelvisSingle);
                static volatile LONG lastPelvisLog = 0;
                LONG nowP = static_cast<LONG>(GetTickCount()), prevP = lastPelvisLog;
                if (nowP - prevP > 5000 && InterlockedCompareExchange(&lastPelvisLog, nowP, prevP) == prevP)
                    logWrite("upload: pelvis (slot 0) repeated in 2+ uploads %ld, seen once %ld; last pass %d slot-0 "
                             "candidates, top count %d",
                             pelvisRepeated, pelvisSingle, ctx->passCandN, pc.count);
            }
            memcpy(use.rot, pc.rot, sizeof(use.rot));
            have = true;
            horizontalMode = true;
            InterlockedIncrement(&byRepeat);
            InterlockedAdd64(&repeatTop, pc.count);
            InterlockedAdd64(&repeatSecond, second);
        } else if (uploadAnchor) {
            // No pelvis candidate in this chunk: nothing to draw.  The older
            // root-transform path that used to run here shared the hip-to-feet
            // calibration and drew a second Mario about a metre under the floor
            // (live 2026-10-20).
            return false;
        } else if (false) {
            const double start = ctx->passStart;
            if (ctx->upValid && ctx->upT >= start) {
                use.tick = ctx->upTick; use.index = ctx->upIndex; use.t = ctx->upT;
                memcpy(use.vp, ctx->upVp, sizeof(use.vp));
                memcpy(use.pos, ctx->upPos, sizeof(use.pos));
                memcpy(use.rot, ctx->upRot, sizeof(use.rot));
                have = true;
                InterlockedIncrement(&samePass);
            } else if (!g_modeUploadFill) {
                // The forward pass is recorded in chunks; only the chunk that
                // drew the player has this frame's transform.  Drawing in the
                // others (with the previous frame's) showed Mario twice, a
                // frame apart (live 2026-10-20), so they are skipped.
                static volatile LONG skipped = 0;
                InterlockedIncrement(&skipped);
                static volatile LONG lastSkipLog = 0;
                LONG nowS = static_cast<LONG>(GetTickCount()), prevS = lastSkipLog;
                if (nowS - prevS > 5000 && InterlockedCompareExchange(&lastSkipLog, nowS, prevS) == prevS)
                    logWrite("upload: skipped %ld pass chunks without the player's upload", skipped);
                return false;
            } else {
                AcquireSRWLockShared(&g_upLock);
                int inWindow = -1, before = -1;
                for (int k = 0; k < 16; ++k) {
                    const UploadAnchor &a = g_upRing[k];
                    if (!a.tick || tNow - a.t > 0.25) continue;
                    if (a.t >= start) { if (inWindow < 0 || a.t > g_upRing[inWindow].t) inWindow = k; }
                    else if (before < 0 || a.t > g_upRing[before].t) before = k;
                }
                if (inWindow >= 0) { use = g_upRing[inWindow]; have = true; InterlockedIncrement(&sameWindow); }
                else if (before >= 0) { use = g_upRing[before]; have = true; InterlockedIncrement(&previous); }
                ReleaseSRWLockShared(&g_upLock);
            }
        }
        if (uploadAnchor && !have) InterlockedIncrement(&missed);
        static volatile LONG noPassStart = 0;
        if (uploadAnchor && !(ctx && ctx->passStart > 0)) InterlockedIncrement(&noPassStart);
        if (have && horizontalMode) {
            // Horizontal position from the frame's most-shared bone, offset
            // to the feet in the character's own facing frame (measured while
            // standing still); height from memory, which removes the bone's
            // walk bob.  A bone always moves with the animation, so only its
            // horizontal track is trusted.
            static float calibH[3];  // right, forward, up
            static int calibHN = 0;
            float a = g_world.playerAngle;
            readLiveAngle(a);
            const float fwd[3] = {std::sin(a), 0.0f, std::cos(a)}, right[3] = {std::cos(a), 0.0f, -std::sin(a)};
            const float memRel[3] = {memPlayer[0] - memCamera0[0], memPlayer[1] - memCamera0[1],
                                     memPlayer[2] - memCamera0[2]};
            AcquireSRWLockExclusive(&calLock);
            const float moved = std::fabs(memPlayer[0] - lastMem[0]) + std::fabs(memPlayer[1] - lastMem[1]) +
                                std::fabs(memPlayer[2] - lastMem[2]);
            if (moved > 0.001f) { stillSince = tNow; memcpy(lastMem, memPlayer, sizeof(lastMem)); }
            if (tNow - stillSince > 0.3) {
                const float d[3] = {memRel[0] - use.pos[0], memRel[1] - use.pos[1], memRel[2] - use.pos[2]};
                const float lr = d[0] * right[0] + d[2] * right[2], lf = d[0] * fwd[0] + d[2] * fwd[2];
                calibH[0] = calibHN ? calibH[0] * 0.95f + lr * 0.05f : lr;
                calibH[1] = calibHN ? calibH[1] * 0.95f + lf * 0.05f : lf;
                if (calibHN < 1000000) ++calibHN;
                calib[0] = calibH[0]; calib[1] = calibH[2]; calib[2] = calibH[1];
                if (calibN < 1000000) ++calibN;
            }
            // Height offset adapts continuously but slowly (about 1 s): it
            // follows the torso dropping while running (Mario rose when the
            // player stopped) without taking on per-frame camera jitter.
            {
                static bool haveU = false;
                const float du = memRel[1] - use.pos[1];
                calibH[2] = haveU ? calibH[2] * 0.98f + du * 0.02f : du;
                haveU = true;
                calib[1] = calibH[2];
            }
            const float oR = calibHN ? calibH[0] : 0.0f, oF = calibHN ? calibH[1] : 0.0f,
                        oU = calibH[2];
            ReleaseSRWLockExclusive(&calLock);
            // All three from the frame's bones: memory height mixed the
            // memory camera with the frame's matrix and jittered as the
            // camera moved.
            player[0] = use.pos[0] + right[0] * oR + fwd[0] * oF;
            player[2] = use.pos[2] + right[2] * oR + fwd[2] * oF;
            player[1] = use.pos[1] + oU;
            camera[0] = camera[1] = camera[2] = 0.0f;
            memcpy(vpLocal, use.vp, sizeof(use.vp));
        } else if (have) {
            float axis[3][3];
            for (int j = 0; j < 3; ++j) {
                const float *c = use.rot + j * 3;
                const float l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                for (int k = 0; k < 3; ++k) axis[j][k] = l > 1e-6f ? c[k] / l : 0.f;
            }
            const float memRel[3] = {memPlayer[0] - memCamera0[0], memPlayer[1] - memCamera0[1],
                                     memPlayer[2] - memCamera0[2]};
            AcquireSRWLockExclusive(&calLock);
            const float moved = std::fabs(memPlayer[0] - lastMem[0]) + std::fabs(memPlayer[1] - lastMem[1]) +
                                std::fabs(memPlayer[2] - lastMem[2]);
            if (moved > 0.001f) { stillSince = tNow; memcpy(lastMem, memPlayer, sizeof(lastMem)); }
            // Standing still for 0.3 s: memory and the frame agree, so the
            // feet offset in the root's own axes can be measured.
            if (tNow - stillSince > 0.3) {
                const float d[3] = {memRel[0] - use.pos[0], memRel[1] - use.pos[1], memRel[2] - use.pos[2]};
                for (int j = 0; j < 3; ++j) {
                    const float local = axis[j][0] * d[0] + axis[j][1] * d[1] + axis[j][2] * d[2];
                    calib[j] = calibN ? calib[j] * 0.95f + local * 0.05f : local;
                }
                if (calibN < 1000000) ++calibN;
            }
            float off[3] = {0, 0, 0};
            if (calibN) memcpy(off, calib, sizeof(off));
            ReleaseSRWLockExclusive(&calLock);
            // Camera-relative, drawn with the view-projection of the same
            // upload: a matched pair whatever frame it came from.
            for (int k = 0; k < 3; ++k)
                player[k] = use.pos[k] + axis[0][k] * off[0] + axis[1][k] * off[1] + axis[2][k] * off[2];
            camera[0] = camera[1] = camera[2] = 0.0f;
            memcpy(vpLocal, use.vp, sizeof(use.vp));
        }
        static volatile LONG lastUpLog = 0;
        LONG nowUp = static_cast<LONG>(tick), prevUp = lastUpLog;
        if (g_modeUpload && nowUp - prevUp > 5000 &&
            InterlockedCompareExchange(&lastUpLog, nowUp, prevUp) == prevUp) {
            AcquireSRWLockShared(&calLock);
            logWrite("upload: most-repeated transform %ld (average repeats %.1f, runner-up %.1f); this pass on this "
                     "context %ld, same interval other context %ld, previous frame %ld, none %ld; feet offset (%.3f,%.3f,%.3f)%s",
                     byRepeat, byRepeat ? double(repeatTop) / byRepeat : 0.0,
                     byRepeat ? double(repeatSecond) / byRepeat : 0.0,
                     samePass, sameWindow, previous, missed, calib[0], calib[1], calib[2],
                     calibN ? "" : " uncalibrated - stand still briefly");
            int fresh = 0;
            AcquireSRWLockShared(&g_upLock);
            for (const UploadAnchor &a : g_upRing) if (a.tick && tNow - a.t <= 0.25) ++fresh;
            ReleaseSRWLockShared(&g_upLock);
            logWrite("upload: capture chain - 2288 uploads %ld, positions read %ld, with candidates %ld, lock ready %ld, "
                     "picked %ld; draws without pass start %ld; recent anchors %d of 16; rejected near transforms: "
                     "too far %ld, not rigid %ld, tilted %ld, facing %ld",
                     g_upDiag[0], g_upDiag[1], g_upDiag[2], g_upDiag[3], g_upDiag[4], noPassStart, fresh,
                     g_upReject[0], g_upReject[1], g_upReject[2], g_upReject[3]);
            const LONG tc = g_upTiltCount;
            logWrite("upload: picked transforms tilt average %.2f degrees, maximum %.2f (limit tilt=%.2f, locktol=%.1f cm)",
                     tc ? g_upTiltSum / 1000.0 / tc : 0.0, g_upTiltMax / 1000.0, g_upTiltMilliDeg / 1000.0,
                     g_upLockTolMm / 10.0);
            g_upTiltSum = 0; g_upTiltCount = 0; g_upTiltMax = 0;
            ReleaseSRWLockShared(&calLock);
        }
    }

    // Every 3 s: how far the memory and engine-uploaded camera positions are
    // apart, so the choice can be checked from the log.
    static volatile LONG lastCmp = 0;
    LONG nowCmp = static_cast<LONG>(GetTickCount()), prevCmp = lastCmp;
    if (nowCmp - prevCmp > 3000 && InterlockedCompareExchange(&lastCmp, nowCmp, prevCmp) == prevCmp) {
        WorldObservation w = g_world;
        auto dist = [](const float *a, const float *b) {
            float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        logWrite("cam: memory=(%.3f,%.3f,%.3f) eye2288=(%.3f,%.3f,%.3f) d=%.3f eye1040=(%.3f,%.3f,%.3f) d=%.3f",
                 w.camera[0], w.camera[1], w.camera[2], g_eyePos2288[0], g_eyePos2288[1], g_eyePos2288[2],
                 dist(w.camera, g_eyePos2288), g_eyePos1040[0], g_eyePos1040[1], g_eyePos1040[2],
                 dist(w.camera, g_eyePos1040));
    }

    // The player must be on screen under the chosen view.  The 4112-byte buffer
    // also carries other perspective views (reflections and the like) that pass
    // the structural check; drawing through one of those made Mario vanish on
    // alternate frames while turning (live 2026-10-19).  Such a view is replaced
    // by the last one that framed the player.
    {
        static SRWLOCK goodLock = SRWLOCK_INIT;
        static float goodVp[16], goodCamera[3];
        static bool haveGood = false;
        static volatile LONG viewsUsed = 0, viewsReplaced = 0;
        const float rel[3] = {player[0] - camera[0], player[1] + 1.0f - camera[1], player[2] - camera[2]};
        float c[4];
        for (int r = 0; r < 4; ++r)
            c[r] = vpLocal[r * 4] * rel[0] + vpLocal[r * 4 + 1] * rel[1] + vpLocal[r * 4 + 2] * rel[2] + vpLocal[r * 4 + 3];
        const bool framed = c[3] > 0.3f && std::fabs(c[0] / c[3]) < 0.95f && std::fabs(c[1] / c[3]) < 0.98f;
        if (framed) {
            AcquireSRWLockExclusive(&goodLock);
            memcpy(goodVp, vpLocal, sizeof(goodVp));
            memcpy(goodCamera, camera, sizeof(goodCamera));
            haveGood = true;
            ReleaseSRWLockExclusive(&goodLock);
            InterlockedIncrement(&viewsUsed);
        } else {
            InterlockedIncrement(&viewsReplaced);
            static volatile LONG lastBadLog = 0;
            LONG nowBad = static_cast<LONG>(GetTickCount()), prevBad = lastBadLog;
            if (nowBad - prevBad > 2000 && InterlockedCompareExchange(&lastBadLog, nowBad, prevBad) == prevBad)
                logWrite("view: player off screen under chosen view (used %ld, replaced %ld): ndc=(%.2f,%.2f) "
                         "w=%.2f fwd=(%.3f,%.3f,%.3f) m11=%.3f eye=(%.2f,%.2f,%.2f)",
                         viewsUsed, viewsReplaced, c[3] != 0 ? c[0] / c[3] : 0.f, c[3] != 0 ? c[1] / c[3] : 0.f,
                         c[3], vpLocal[12], vpLocal[13], vpLocal[14], vpLocal[5], camera[0], camera[1], camera[2]);
            AcquireSRWLockShared(&goodLock);
            const bool have = haveGood;
            if (have) {
                memcpy(vpLocal, goodVp, sizeof(goodVp));
                memcpy(camera, goodCamera, sizeof(goodCamera));
            }
            ReleaseSRWLockShared(&goodLock);
            if (!have) return false;
        }
    }

    SCRenderAPI::Camera matrices{};
    rendererCamera(vpLocal, camera, &matrices);
    const uint32_t magenta = 0xFFFF00FFu;  // AARRGGBB
    const uint32_t fullShade = 0xFFFFFFFFu;
    // Two crossed upright triangles (X-Y and Z-Y planes) so the marker is
    // visible from every side; a single one vanished edge-on.
    SCRenderAPI::Vertex v[6] = {
        {player[0] - 0.3f, player[1], player[2], magenta, fullShade, 0, 0},
        {player[0] + 0.3f, player[1], player[2], magenta, fullShade, 0, 0},
        {player[0], player[1] + 2.0f, player[2], magenta, fullShade, 0, 0},
        {player[0], player[1], player[2] - 0.3f, magenta, fullShade, 0, 0},
        {player[0], player[1], player[2] + 0.3f, magenta, fullShade, 0, 0},
        {player[0], player[1] + 2.0f, player[2], magenta, fullShade, 0, 0},
    };
    SCRenderAPI::Mesh mesh{v, 2};
    SCRenderAPI::Material material{};  // vertex colour, depth test + write

    // draw=mario: the posed guest mesh at the player's feet instead of the
    // cross.  Falls back to the cross until a frame has been published.
    static thread_local std::vector<SCRenderAPI::Vertex> marioVerts;
    const bool wantMario = g_modeMario != 0;
    drawperf::lap(0);
    float marioYaw = g_world.playerAngle;
    readLiveAngle(marioYaw);  // facing at draw time; worker copy as fallback
    if (mario::g_yawFlip) marioYaw += 3.14159265f;
    uint32_t marioTriangles = wantMario ? mario::copyFrame(player, marioYaw, marioVerts) : 0;
    drawperf::lap(1);
    if (marioTriangles) {
        mesh = {marioVerts.data(), marioTriangles};
        material = mario::material();
    }
    SCRenderAPI::Context submit{SCRenderAPI::Phase::Scene, true};

    DSRGraphics::Composition targets{};
    targets.color = static_cast<ID3D11RenderTargetView *>(colorView);
    targets.depth = static_cast<ID3D11DepthStencilView *>(depthView);
    targets.depthVerified = true;  // the engine's main-pass scene depth (section 22)
    targets.targetsBound = true;

    drawperf::lap(4);
    EnterCriticalSection(&g_rendererLock);
    drawperf::lap(2);
    if (g_diag) measureJitter(cam);
    g_renderer->bind(context);
    bool ok = g_renderer->compose(g_rendererDevice, targets);
    if (ok) {
        g_renderer->camera(matrices);
        ok = g_renderer->mesh(mesh, material, submit);
    }
    g_renderer->bind(nullptr);
    LeaveCriticalSection(&g_rendererLock);
    drawperf::lap(3);

    static volatile LONG logged = 0;
    if (ok && marioTriangles) InterlockedIncrement(&g_marioDraws);
    LONG n = InterlockedIncrement(&logged);
    if (n <= 3 || (n % 2000) == 0)
        logWrite("renderer: %s submission #%ld on ctx=%p -> %s", marioTriangles ? "MARIO" : "marker", n,
                 context, ok ? "drawn" : "REFUSED");
    return ok;
}
} // namespace

bool initializeInjection(ID3D11Device *device) {
    if (!device) return false;
    LONG state = InterlockedCompareExchange(&g_initState, 0, 0);
    if (state == 2) return true;
    if (state == 3) return false;

    if (InterlockedCompareExchange(&g_initState, 1, 0) != 0) return false;

    HRESULT hr = device->CreateVertexShader(kTestVS, sizeof(kTestVS), nullptr, &g_res.vertexShader);
    if (FAILED(hr)) return failInit("CreateVertexShader", hr);

    hr = device->CreatePixelShader(kTestPS, sizeof(kTestPS), nullptr,
                                   &g_res.pixelShader);
    if (FAILED(hr)) return failInit("CreatePixelShader", hr);

    {
        D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = device->CreateInputLayout(elements, 1, kTestVS, sizeof(kTestVS), &g_res.inputLayout);
        if (FAILED(hr)) return failInit("CreateInputLayout", hr);
    }

    {
        struct Vtx { float x, y, z, w; };
        const Vtx verts[] = {
            {-0.8f, -0.8f, 0.0f, 1.0f},
            { 0.0f,  0.8f, 0.0f, 1.0f},
            { 0.8f, -0.8f, 0.0f, 1.0f},
        };
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(verts);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = 0;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = verts;
        hr = device->CreateBuffer(&desc, &init, &g_res.vertexBuffer);
        if (FAILED(hr)) return failInit("CreateBuffer", hr);

        // World markers: DEFAULT usage, filled with UpdateSubresource on the
        // drawing context (valid on deferred contexts with a null box).
        // Four triangles (12 vertices), one per compass direction.
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.ByteWidth = kWorldMarkerVertices * sizeof(Vtx);
        hr = device->CreateBuffer(&desc, nullptr, &g_res.worldVertexBuffer);
        if (FAILED(hr)) return failInit("CreateBuffer(world)", hr);
    }

    {
        // Test draw only: always pass and never write depth, so the triangle is
        // visible whatever depth convention the game uses (a reversed-Z clear
        // to 0 would make a LESS test reject everything) and scene depth is left
        // exactly as the game wrote it.  Occlusion belongs to the Mario draw,
        // once the projection is world-anchored.
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        desc.StencilEnable = FALSE;
        hr = device->CreateDepthStencilState(&desc, &g_res.depthState);
        if (FAILED(hr)) return failInit("CreateDepthStencilState", hr);

        // World marker is depth-tested against the scene so walls in front of
        // it hide it.  The captured projection maps near to 0 and infinity to
        // 1 (see docs/REMASTERED_FINDINGS.md section 22), so LESS_EQUAL.
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        hr = device->CreateDepthStencilState(&desc, &g_res.depthTested);
        if (FAILED(hr)) return failInit("CreateDepthStencilState(tested)", hr);
        desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        hr = device->CreateDepthStencilState(&desc, &g_res.depthTestedReversed);
        if (FAILED(hr)) return failInit("CreateDepthStencilState(reversed)", hr);
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        hr = device->CreateDepthStencilState(&desc, &g_res.depthTestedWrite);
        if (FAILED(hr)) return failInit("CreateDepthStencilState(tested+write)", hr);
    }

    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;  // winding cannot hide the test
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = FALSE;
        hr = device->CreateRasterizerState(&desc, &g_res.rasterizer);
        if (FAILED(hr)) return failInit("CreateRasterizerState", hr);
    }

    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        // Target 0 and 1 both written.
        desc.IndependentBlendEnable = TRUE;
        desc.RenderTarget[1].BlendEnable = FALSE;
        desc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &g_res.blend);
        if (FAILED(hr)) return failInit("CreateBlendState", hr);
        desc.RenderTarget[1].RenderTargetWriteMask = 0;
        hr = device->CreateBlendState(&desc, &g_res.blendRt0);
        if (FAILED(hr)) return failInit("CreateBlendState(rt0)", hr);
        desc.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &g_res.blendRt1);
        if (FAILED(hr)) return failInit("CreateBlendState(rt1)", hr);
    }

    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = 32;  // two float4
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&desc, nullptr, &g_res.psConstants);
        if (FAILED(hr)) return failInit("CreateBuffer(constants)", hr);
    }

    g_res.ready = true;
    InterlockedExchange(&g_initState, 2);
    if (HANDLE t = CreateThread(nullptr, 0, modePollThread, nullptr, 0, nullptr)) CloseHandle(t);
    prepareRenderer(device);
    mario::start();  // loads the x64 adapter/runtime/ROM; logs and stays off on failure
    logWrite("inject: initialised (vs=%uB ps=%uB, 3 vertices)",
             static_cast<unsigned>(sizeof(kTestVS)),
             static_cast<unsigned>(sizeof(kTestPS)));
    return true;
}

// World-direction marker.
//
// The captured view-projection has no translation (m03 = m13 = 0, rows 3 and 4
// equal apart from the -0.0501 term), so it takes camera-relative positions:
// clip_i = dot(row_i, (p, 1)) with the 16 floats read as four rows.  World Y
// is up (row 1 is dominated by its Y term).  Without the camera's world
// position the marker can only be placed relative to the camera, so it sits a
// fixed distance along world +Z: it keeps its compass bearing and perspective
// as the camera turns, and travels with the camera as it moves.  Correct
// placement on screen when turning is what this verifies; true world anchoring
// needs the camera position (section 22).
static bool projectWorldMarker(float out[kWorldMarkerVertices * 4]) {
    CameraState cam = getCameraState();
    if (!cam.valid) return false;
    const float *m = cam.viewProjection.m;

    // Player-anchored mode: a 0.6 m wide, 2 m tall upright triangle at the
    // player's position, made camera-relative with the candidate camera
    // position.  The remaining nine vertices are degenerate (not drawn).
    if (g_modeAnchorPlayer) {
        WorldObservation w = g_world;
        if (!w.playerValid || !w.cameraValid) return false;
        const float *cp = w.camera;
        const float base[3] = {w.player[0] - cp[0], w.player[1] - cp[1], w.player[2] - cp[2]};
        const float tri[3][3] = {
            {base[0] - 0.3f, base[1], base[2]},
            {base[0] + 0.3f, base[1], base[2]},
            {base[0], base[1] + 2.0f, base[2]},
        };
        for (int v = 0; v < kWorldMarkerVertices; ++v) {
            const float *p = tri[v < 3 ? v : 0];
            for (int r = 0; r < 4; ++r)
                out[v * 4 + r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] +
                                 m[r * 4 + 3];
        }
        return true;
    }
    // One upright triangle 8 m away along each of +Z, +X, -Z, -X, so one of
    // them is always near the view direction while the camera turns.
    const float tri[kWorldMarkerVertices][3] = {
        {-1.5f, 0.0f, 8.0f}, {1.5f, 0.0f, 8.0f}, {0.0f, 3.0f, 8.0f},
        {8.0f, 0.0f, 1.5f}, {8.0f, 0.0f, -1.5f}, {8.0f, 3.0f, 0.0f},
        {1.5f, 0.0f, -8.0f}, {-1.5f, 0.0f, -8.0f}, {0.0f, 3.0f, -8.0f},
        {-8.0f, 0.0f, -1.5f}, {-8.0f, 0.0f, 1.5f}, {-8.0f, 3.0f, 0.0f},
    };
    for (int v = 0; v < kWorldMarkerVertices; ++v) {
        for (int r = 0; r < 4; ++r)
            out[v * 4 + r] = m[r * 4 + 0] * tri[v][0] + m[r * 4 + 1] * tri[v][1] +
                             m[r * 4 + 2] * tri[v][2] + m[r * 4 + 3];
    }
    // Which markers have their apex on screen, every 3 s.
    static volatile LONG lastLog = 0;
    LONG now = static_cast<LONG>(GetTickCount());
    LONG prev = lastLog;
    if (now - prev > 3000 && InterlockedCompareExchange(&lastLog, now, prev) == prev) {
        static const char *names[4] = {"+Z", "+X", "-Z", "-X"};
        char line[256];
        int o = _snprintf(line, sizeof(line), "world: apex");
        for (int t = 0; t < 4; ++t) {
            const float *p = out + (t * 3 + 2) * 4;
            if (p[3] > 1e-4f)
                o += _snprintf(line + o, sizeof(line) - o, " %s ndc=(%.2f,%.2f) z/w=%.4f%s", names[t],
                               p[0] / p[3], p[1] / p[3], p[2] / p[3],
                               (p[0] / p[3] > -1 && p[0] / p[3] < 1 && p[1] / p[3] > -1 && p[1] / p[3] < 1)
                                   ? " ON-SCREEN" : "");
            else
                o += _snprintf(line + o, sizeof(line) - o, " %s behind", names[t]);
        }
        logWrite("%s", line);
    }
    return true;
}

// Everything the draw changes is read first and put back afterwards, through
// the ID3D11DeviceContext interface from d3d11.h.
//
// The earlier hand-numbered Get* calls were what crashed, not saving state as
// such: slot 75 is PSGetSamplers (IAGetInputLayout is 78) and slot 93 is
// SOGetTargets (OMGetRenderTargets is 89), so the "save" wrote through wrong
// argument lists and the "restore" bound a stream-output buffer as a render
// target.  Dropping the restore instead left the game's own input layout,
// vertex buffer, topology, shaders, rasterizer, depth and blend state replaced
// by ours for whatever it drew next.
//
// Render targets and viewports are not touched, so they are not saved; in
// particular OMSetRenderTargets is never called here, which keeps this out of
// our own OMSetRenderTargets hook.
void drawTestTriangle(ID3D11DeviceContext *context, UINT boundTargets, void *colorView,
                      void *depthView) {
    if (!context) return;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&g_stats.attempts));
    if (!injectionArmed()) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&g_stats.skippedNoArm));
        return;
    }
    const bool multiTarget = boundTargets >= 2;
    const LONG targets = g_modeTargets;
    if (!(targets & (multiTarget ? kTargetMulti : kTargetSingle))) return;
    const bool drawScreen = g_modeScreen != 0;
    const bool drawWorld = g_modeWorld != 0;
    if (!drawScreen && !drawWorld) return;

    // Renderer path: the backend saves and restores its own state.
    if (g_modeRenderer) {
        if (multiTarget) drawRendererMarker(context, colorView, depthView);
        return;
    }

    // ---- save ----
    ID3D11InputLayout *oldLayout = nullptr;
    ID3D11Buffer *oldVB = nullptr;
    UINT oldStride = 0, oldOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader *oldVS = nullptr;
    ID3D11PixelShader *oldPS = nullptr;
    ID3D11ClassInstance *oldVSInst[D3D11_SHADER_MAX_INTERFACES] = {};
    ID3D11ClassInstance *oldPSInst[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT oldVSInstCount = D3D11_SHADER_MAX_INTERFACES;
    UINT oldPSInstCount = D3D11_SHADER_MAX_INTERFACES;
    ID3D11RasterizerState *oldRS = nullptr;
    ID3D11DepthStencilState *oldDS = nullptr;
    UINT oldStencilRef = 0;
    ID3D11BlendState *oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0xFFFFFFFF;

    context->IAGetInputLayout(&oldLayout);
    context->IAGetVertexBuffers(0, 1, &oldVB, &oldStride, &oldOffset);
    context->IAGetPrimitiveTopology(&oldTopology);
    context->VSGetShader(&oldVS, oldVSInst, &oldVSInstCount);
    context->PSGetShader(&oldPS, oldPSInst, &oldPSInstCount);
    context->RSGetState(&oldRS);
    context->OMGetDepthStencilState(&oldDS, &oldStencilRef);
    context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    ID3D11Buffer *oldPSCB = nullptr;
    context->PSGetConstantBuffers(0, 1, &oldPSCB);

    // ---- draw ----
    UINT stride = 16, offset = 0;
    // Screen triangle: colour identifies the binding it went into.
    // Magenta = multi-target candidate (#4), green = single-target (#5).
    context->IASetInputLayout(g_res.inputLayout);
    context->IASetVertexBuffers(0, 1, &g_res.vertexBuffer, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    float values[8];
    memcpy(values, g_modeTargetValues, sizeof(values));
    context->UpdateSubresource(g_res.psConstants, 0, nullptr, values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &g_res.psConstants);
    context->VSSetShader(g_res.vertexShader, nullptr, 0);
    context->PSSetShader(g_res.pixelShader, nullptr, 0);
    context->RSSetState(g_res.rasterizer);
    context->OMSetDepthStencilState(g_res.depthState, 0);
    const LONG write = g_modeWrite;
    context->OMSetBlendState(write == kWriteRt0 ? g_res.blendRt0
                             : write == kWriteRt1 ? g_res.blendRt1 : g_res.blend,
                             nullptr, 0xFFFFFFFF);
    if (drawScreen) context->Draw(3, 0);

    // World-direction marker, in whichever enabled pass this is.
    float clip[kWorldMarkerVertices * 4];
    if (drawWorld && projectWorldMarker(clip)) {
        const LONG depthMode = g_modeDepth;
        context->UpdateSubresource(g_res.worldVertexBuffer, 0, nullptr, clip, 0, 0);
        context->IASetVertexBuffers(0, 1, &g_res.worldVertexBuffer, &stride, &offset);
        context->OMSetDepthStencilState(g_modeWorldDepthWrite ? g_res.depthTestedWrite
                                        : depthMode == kDepthGequal ? g_res.depthTestedReversed
                                        : depthMode == kDepthAlways ? g_res.depthState
                                                                    : g_res.depthTested, 0);
        context->Draw(kWorldMarkerVertices, 0);
    }

    // ---- restore ----
    context->IASetInputLayout(oldLayout);
    context->IASetVertexBuffers(0, 1, &oldVB, &oldStride, &oldOffset);
    context->IASetPrimitiveTopology(oldTopology);
    context->VSSetShader(oldVS, oldVSInst, oldVSInstCount);
    context->PSSetShader(oldPS, oldPSInst, oldPSInstCount);
    context->RSSetState(oldRS);
    context->OMSetDepthStencilState(oldDS, oldStencilRef);
    context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    context->PSSetConstantBuffers(0, 1, &oldPSCB);

    // Get* calls add a reference to every returned interface.
    auto rel = [](IUnknown *p) { if (p) p->Release(); };
    rel(oldLayout); rel(oldVB); rel(oldVS); rel(oldPS);
    rel(oldRS); rel(oldDS); rel(oldBlend); rel(oldPSCB);
    for (UINT i = 0; i < oldVSInstCount && i < D3D11_SHADER_MAX_INTERFACES; ++i) rel(oldVSInst[i]);
    for (UINT i = 0; i < oldPSInstCount && i < D3D11_SHADER_MAX_INTERFACES; ++i) rel(oldPSInst[i]);

    LONG64 n = InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&g_stats.injected));
    static volatile LONG64 perKind[2] = {};
    LONG64 k = InterlockedIncrement64(&perKind[multiTarget ? 1 : 0]);
    if (k <= 3 || (k % 2000) == 0)
        logWrite("inject: drew %s pass (binding targets=%u, kind #%lld, total #%lld) on ctx=%p",
                 multiTarget ? "multi-target" : "single-target", boundTargets,
                 static_cast<long long>(k), static_cast<long long>(n), context);
}

} // namespace remastered

// DirectInput object created by the game (runtime.cpp's DirectInput8Create):
// wrap its keyboard/mouse devices for the drive-mode input filter.
extern "C" void marioWrapDirectInput(void *di) { remastered::mario::wrapDirectInput(di); }

// Param/text patching from process start (v155): started from the first input poll it only
// ran once the delayed hooks were in (15 s), and a quick Continue loaded the save's Caestus
// model before its model was patched (Mario's Gear visible until re-equipped).
extern "C" void marioSetupStartEarly() { remastered::mariosetup::start(); }
