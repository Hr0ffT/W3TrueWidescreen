// W3TrueWidescreen.mix - unstretched widescreen for Warcraft III 1.26a (Game.dll build 6401)
// Copyright (c) 2026 Hr0ffT - MIT License - https://github.com/Hr0ffT
//
// The engine lays out its UI in a virtual 0.8 x 0.6 screen and stretches it to the monitor.
// This mod widens that space to (0.6 * aspect) x 0.6, keeps the in-game console in the centred 4:3
// area, moves the menus to the screen edges, lets the world fill the whole screen, and restores the
// original 4:3 vertical field of view for the 3D menus and the game world.
//
// Build: i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -s -o W3TrueWidescreen.mix W3TrueWidescreen.c -lversion -lgdi32
#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stddef.h>
#define COBJMACROS
#include <d3d11.h>

#define INI_SECTION "W3TrueWidescreen"   // [section] in W3TrueWidescreen.ini
typedef LONG_PTR OAHWND_;

typedef uint32_t u32;

static u32 g_base;
static double g_uiW_d = 0.8;      // new UI width (double, used by UI<->screen conversions)
static float  g_uiW_f = 0.8f;     // new UI width (float, used by UI ortho projection)
static float  g_e = 0.0f;         // horizontal shift for HUD frames = (W - 0.8) / 2
static int    g_enabled = 0;
static FILE*  g_log = NULL;
static int    g_logCount = 0;
static int    g_debug = 0;        // Debug=1 in the ini: verbose log
static int    g_fpsLimit = 0;     // FpsLimit: 0 = leave the frame rate alone (see InstallFpsLimit)
static int    g_pathQuery = 1;    // AllowPathQuery: let other programs read the exe path (see ProtectProcess_hook)
#define dlogf_(...) do { if (g_debug) logf_(__VA_ARGS__); } while (0)
static int    g_checkedWindow = 0;

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// --- game offsets (1.26a) ---
#define RVA_SetFramePoint   0x606770
#define RVA_SetAllPoints    0x6067F0
#define RVA_ClearAllPoints  0x606160
#define RVA_OrthoFld        0x7AE566   // fld dword ptr [0.8f] -> ortho right edge for UI layers
#define RVA_GameUIVtbl      0x93631C
#define OFS_UI_Frame        0xB4
#define OFS_UI_World        0x3BC
#define OFS_UI_MouseBorders 0x420

static const u32 kDoubleSites[] = {   // imm32 operands referencing double 0.8 that mean "UI width"
    0x4C648A, 0x4C64B4, 0x4C64EA, 0x4C6514, 0x4C6546, 0x4C6586, // UI <-> normalized conversions
    0x33CE33, 0x55D9FD, 0x57136A                                 // tooltip right-edge clamps
};

typedef void (__fastcall *SetFramePoint_t)(u32 frame, u32 edx, u32 point, u32 parent, u32 rel, float x, float y, u32 flag);
typedef void (__fastcall *ClearAllPoints_t)(u32 frame, u32 edx);
static SetFramePoint_t  orig_SetFramePoint;
static ClearAllPoints_t ClearAllPoints;

static int WriteMem(u32 addr, const void* data, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)addr, data, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return 1;
}

static volatile u32 g_gameUI;   // CGameUI of the running game (for the interface state)
static int IsGameUIRoot(u32 parent)
{
    if (!parent) return 0;
    u32 obj = parent - OFS_UI_Frame;
    if (IsBadReadPtr((void*)obj, OFS_UI_MouseBorders + 4)) return 0;
    if (*(u32*)obj != g_base + RVA_GameUIVtbl) return 0;
    g_gameUI = obj;
    return 1;
}
// CGameUI+0x180: interface state (0/4 shown, 1 hiding, 3 hidden = cinematic mode, 2 showing again)
static int InCinematicMode(void)
{
    u32 ui = g_gameUI;
    if (!ui || IsBadReadPtr((void*)ui, 0x184) || *(u32*)ui != g_base + RVA_GameUIVtbl) return 0;
    u32 st = *(u32*)(ui + 0x180);
    return st == 1 || st == 3;
}

#define RVA_ScreenRootSlot1 0x60A140
#define RVA_SetAbsPoint     0x6061B0
static int g_glue = 1;

// any full-screen root frame (game UI root, glue/menu screens, loading screen):
// its layout vtable slot 1 recomputes the rect as the whole screen
static int IsScreenRoot(u32 parent)
{
    if (!parent || IsBadReadPtr((void*)parent, 4)) return 0;
    u32 vt = *(u32*)parent;
    if (!vt || IsBadReadPtr((void*)vt, 8)) return 0;
    return *(u32*)(vt + 4) == g_base + RVA_ScreenRootSlot1;
}

static u32 VtRva(u32 p)
{
    if (!p || IsBadReadPtr((void*)p, 4)) return 0;
    u32 vt = *(u32*)p;
    return vt >= g_base ? vt - g_base : vt;
}

// frame classes that host other frames' rendering (simple-frame renderer, cursor/tooltip layer):
// they must stay full screen, otherwise everything they draw is translated a second time
static u32 g_keepVt[16] = { 0x96E2D0, 0x970610 };
static int g_keepVtN = 2;

static int IsKeepClass(u32 frame)
{
    u32 vt = VtRva(frame);
    for (int i = 0; i < g_keepVtN; i++) if (vt == g_keepVt[i]) return 1;
    return 0;
}

// menus: 3D backgrounds (CSpriteFrame) fill the whole screen, the menu panels stay centered
static int g_menuBgFull = 1;
static int g_menuEdges = 1;   // MenuLayout=1: menu panels go to the screen edges (only loading screen stays centered)
static int IsKeepClassGlue(u32 frame)
{
    if (IsKeepClass(frame)) return 1;
    u32 vt = VtRva(frame);
    if (g_menuBgFull && vt == 0x96E558) return 1;               // CSpriteFrame: 3D backgrounds
    // screens drawn as one fixed 4:3 picture stay centred: CLoading (loading screen), CScoreScreen (mission score)
    if (g_menuEdges && vt != 0x96662C && vt != 0x967460) return 1;
    return 0;
}

// in-game: let the 3D world also fill the strips above/below the world view (behind the console)
static int   g_worldFull = 1;
static int   g_worldBottomDefault = 0, g_worldTopDefault = 0;
#define WORLD_B0 0.13f
#define WORLD_T0 (-0.02f)
// ---- map-click gate. Before picking the terrain under the cursor the world rejects any click whose UI y is
//      >= 0.577 (fld dword ptr [0.577f] at 0x39799C, just under the original 0.58 top of the world view). With
//      WorldFullHeight the world is drawn up to 0.6, so the strip beside the top bar showed the world but took no
//      clicks. Point that operand at our own value: no upper gate when the world is full height. ----
#define RVA_PickTopFld 0x39799C          // d9 05 <addr of 0.577f>
static float g_pickTop = 0.577f;

static int g_heroEdge = 1;     // HeroBarEdge=1: hero portraits (top-left) hug the left screen edge
static int g_cineFull = 1;     // CinematicFullWidth=1: cinematic letterbox panel spans the full width
static int IsFullWidthFrame(u32 frame, u32 ui)
{
    if (IsKeepClass(frame)) return 1;
    if (g_heroEdge && VtRva(frame) == 0x93F974) return 1;      // CHeroBar
    if (g_cineFull && VtRva(frame) == 0x93C794) return 1;      // CCinematicPanel: letterbox spans the whole width
    u32 world = *(u32*)(ui + OFS_UI_World);
    u32 mb    = *(u32*)(ui + OFS_UI_MouseBorders);
    if (world && (frame == world + OFS_UI_Frame || frame == world)) return 1;
    if (mb && (frame == mb || frame == mb + OFS_UI_Frame)) return 1;
    return 0;
}

static float ShiftX(u32 rel, float x)
{
    switch (rel % 3) {            // 0,3,6 = left column; 2,5,8 = right column; 1,4,7 = center
        case 0: return x + g_e;
        case 2: return x - g_e;
        default: return x;
    }
}

static void CheckWindowOnce(void)
{
    if (g_checkedWindow) return;
    g_checkedWindow = 1;
    HWND h = FindWindowA("Warcraft III", NULL);
    RECT r;
    if (h && GetClientRect(h, &r) && r.bottom > 0)
    {
        double wa = (double)r.right / r.bottom;
        if (fabs(0.6 * wa - g_uiW_d) > 0.01)
            logf_("WARNING: window is %ldx%ld (aspect %.4f) but the mod uses aspect %.4f - set Width/Height in W3TrueWidescreen.ini",
                  r.right, r.bottom, wa, g_uiW_d / 0.6);
        else dlogf_("window client %ldx%ld ok", r.right, r.bottom);
    }
}

static int g_consoleBacking = 1;
static int g_hpUnder = 1;         // HealthBarsUnderConsole=1: unit health bars drawn under the console panels   // ConsoleBacking=0: do not blacken the console area behind the world (diagnostics)

static void __fastcall SetFramePoint_hook(u32 frame, u32 edx, u32 point, u32 parent, u32 rel, float x, float y, u32 flag)
{
    (void)edx;
    if (g_enabled && g_cineFull && x == 0.0f && y == 0.0f && point == rel && (point == 0 || point == 8) && VtRva(parent) == 0x93C794) {
        // CinematicTopBorder (TOPLEFT) / CinematicBottomBorder (BOTTOMRIGHT) are 0.8 wide and anchored at one
        // corner of the full-width cinematic panel: pin the opposite side too, so they span the whole width
        u32 other = point == 0 ? 2 : 6;
        orig_SetFramePoint(frame, 0, point, parent, rel, x, y, flag);
        orig_SetFramePoint(frame, 0, other, parent, other, 0.0f, 0.0f, flag);
        dlogf_("cinematic border %08X pinned to both sides (points %u,%u)", frame, point, other);
        return;
    }
    if (g_enabled) {
        int isUI = IsGameUIRoot(parent);
        if (isUI || (g_glue && IsScreenRoot(parent))) {
            if (isUI) CheckWindowOnce();
            int keep = isUI ? IsFullWidthFrame(frame, parent - OFS_UI_Frame) : IsKeepClassGlue(frame);
            float nx = keep ? x : ShiftX(rel, x);
            if (isUI && g_worldFull) {
                u32 w = *(u32*)(parent - OFS_UI_Frame + OFS_UI_World);
                if (w && frame == w + OFS_UI_Frame) {
                    if (point == 6) { g_worldBottomDefault = fabsf(y - WORLD_B0) < 1e-4f; if (g_worldBottomDefault) y = 0.0f; }
                    if (point == 2) { g_worldTopDefault    = fabsf(y - WORLD_T0) < 1e-4f; if (g_worldTopDefault)    y = 0.0f; }
                }
            }
            if (g_logCount < 400) { g_logCount++; dlogf_("SetPoint %s child=%08X(vt %06X) parent vt %06X pt=%u rel=%u x=%.4f -> %.4f y=%.4f%s",
                isUI ? "UI  " : "GLUE", frame, VtRva(frame), VtRva(parent), point, rel, x, nx, y, keep ? " (full width)" : ""); }
            x = nx;
        }
    }
    orig_SetFramePoint(frame, 0, point, parent, rel, x, y, flag);
}

typedef void (__fastcall *SetAbsPoint_t)(u32 frame, u32 edx, u32 point, float x, float y, u32 flag);
static SetAbsPoint_t orig_SetAbsPoint;
static void DumpSimpleLevels(u32 bar);
static void __fastcall SetAbsPoint_hook(u32 frame, u32 edx, u32 point, float x, float y, u32 flag)
{
    (void)edx;
    u32 ret = (u32)__builtin_return_address(0) - g_base;
    // fixed positions laid out for the 0..0.8 space (info bar inventory, menu dialogs) -> move into the centered area.
    // positions computed at runtime (tooltips, HP bars, floating text) are already in the wide space and stay as is.
    int fixed = (ret == 0x37161A) || (!g_menuEdges && (ret == 0x5B8045 || ret == 0x5B8205 || ret == 0x5B83AB));
    float nx = (g_enabled && fixed) ? x + g_e : x;
    // Unit health bars (CStatBar) follow their units anywhere on screen. With the world drawn behind the console,
    // units down there get bars too, and those are drawn on top of the console. In the original these units are
    // outside the world view and have no bars: move such bars off screen (only where the console / top bar is).
    if (g_debug && g_enabled && VtRva(frame) == 0x93E604 && y < 0.2f) {
        static int dumped; if (!dumped) { dumped = 1; DumpSimpleLevels(frame); }
    }
    // With the bars drawn under the console (HealthBarsUnderConsole=1) its panels and decorated edges cover them,
    // but its see-through parts (portrait, info panel, minimap background) would still show them: hide bars that
    // lie below the console's top line (0.13; a bar is about 0.005 high). Without it, hide everything up to the
    // top of the decorated edge (minimap frame, command card, about 0.17).
    if (g_enabled && g_worldFull && VtRva(frame) == 0x93E604) {
        const float L = g_e, R = g_e + 0.8f;
        typedef float (__fastcall *GetWidth_t)(u32 self, u32 edx);
        float hw = ((GetWidth_t)(*(u32**)frame)[6])(frame, 0) * 0.5f;   // bar width (virtual GetWidth, as the game uses)
        if (!(hw > 0.001f && hw < 0.5f)) hw = 0.015f;          // building bars are up to about 0.25 wide
        int hid = 0;
        if (!g_hpUnder) {
            if (x > L && x < R && y < 0.17f) hid = 1;
        } else if (y < WORLD_B0 + 0.005f && x + hw > L && x - hw < R) {
            // below the console line; the bar reaches over the console
            if (x < L) x = L - hw;                              // mostly beside the console: keep it whole beside it
            else if (x > R) x = R + hw;
            else hid = 1;                                       // mostly over it: its unit is under the console
        }
        if (x > L && x < R && y > 0.6f + WORLD_T0) hid = 1;     // under the top bar
        if (hid) {
            static int n; if (n++ < 5) dlogf_("health bar under the console hidden (x %.3f y %.3f ret %06X)", x, y, ret);
            y = -1.0f;
        }
        nx = x;                                                 // (unit bars are never "fixed" positions)
    }
    if (fixed && g_logCount < 400) { g_logCount++; dlogf_("AbsPoint child=%08X(vt %06X) pt=%u x=%.4f -> %.4f y=%.4f ret=%06X", frame, VtRva(frame), point, x, nx, y, ret); }
    orig_SetAbsPoint(frame, 0, point, nx, y, flag);
}

static void __fastcall SetAllPoints_hook(u32 frame, u32 edx, u32 parent, u32 flag)
{
    (void)edx;
    float dx = 0.0f;
    if (g_enabled) {
        int isUI = IsGameUIRoot(parent);
        if (isUI || (g_glue && IsScreenRoot(parent))) {
            int keep = isUI ? IsFullWidthFrame(frame, parent - OFS_UI_Frame) : IsKeepClassGlue(frame);
            if (!keep) dx = g_e;
            if (g_logCount < 400) { g_logCount++; dlogf_("SetAll   %s child=%08X(vt %06X) parent vt %06X dx=%.4f", isUI ? "UI  " : "GLUE", frame, VtRva(frame), VtRva(parent), dx); }
        }
    }
    float dxL = dx, dxR = -dx;
    // GlueSpriteLayerTopRight (right-hand menu decorations: chains, panel frames, post):
    // its model is authored for a 0.8-wide screen, so slide the whole layer to the right edge.
    if (g_enabled && g_glue && g_menuEdges && ((u32)__builtin_return_address(0) - g_base) == 0x57BB33) {
        dxL = 2.0f * g_e; dxR = 2.0f * g_e;
        dlogf_("GlueSpriteLayerTopRight shifted by %.4f", dxL);
    }
    ClearAllPoints(frame, 0);
    orig_SetFramePoint(frame, 0, 0, parent, 0, dxL, 0.0f, 0);
    orig_SetFramePoint(frame, 0, 8, parent, 8, dxR, 0.0f, flag);
}


// ---- world projection: keep the original zoom and screen position when the world view is extended to full height ----
#define RVA_Perspective 0x7B66F0
typedef void (__fastcall *Persp_t)(u32 out, u32 edx, float fovY, float aspect, float zn, float zf);
static Persp_t orig_Persp;
static float g_zoom = 1.0f;
static int g_fovFix = 1;
static int g_fadeFix = 1;     // FadeFix=1: widen the campaign menu screen fade
static char g_moviePlayerOpt[MAX_PATH] = "0";
static char g_moviePlayer[MAX_PATH];
static int g_movieNative = 1;   // MovieNativeMode=1: no 800x600 mode switch and no gamma ramp for cinematics
static int g_movieFullWin = 1;  // MovieFullWindow=1: renderer window covers the whole screen, letterbox drawn by the renderer
static int g_movieHdr = 1, g_movieSuperRes = 1, g_movieSwap = 1, g_movieDetach = 1, g_movieReinit = 1, g_movieTexFmt = 10, g_movieVrDebug = 0;
static int g_movieRenderer = 1; // MovieRenderer=1: play cinematics through W3TrueWidescreen\\MpcVideoRenderer.ax if present
     // FovFix=1: keep the original vertical field of view on wide screens
// CameraZoomOut: >1 shows more of the map (world view only, cinematics untouched)
static volatile DWORD g_lastWorldTick = 0;   // set by RenderWorld_hook: are we in a game (world being drawn)?
static int g_persLog = 0;
// WC3 derives its FOV from the view diagonal, so a wider view gets a smaller vertical FOV (looks zoomed in).
// For views that were widened by us (full-screen menu backgrounds, the game world) rebuild the projection
// so the vertical FOV equals the original 4:3 one and the extra width simply shows more (Hor+).
static void __fastcall Persp_hook(u32 out, u32 edx, float fovY, float aspect, float zn, float zf)
{
    // aspect = viewport width / height; the engine spreads fovY over the view DIAGONAL
    (void)edx;
    const float W = g_uiW_f;
    const float H1 = 0.6f, H0 = 0.6f - WORLD_B0 + WORLD_T0;      // 0.45: original world view height
    int isFull = fabsf(aspect - W / H1) < 4e-3f;                  // full screen (menus, world at full height)
    int isNorm = fabsf(aspect - W / H0) < 4e-3f;                  // world view between the original strips
    int inGame = (GetTickCount() - g_lastWorldTick) < 500;
    if (g_persLog < 30 && (inGame || g_persLog < 3)) { g_persLog++; dlogf_("persp fov=%.4f aspect=%.4f full=%d norm=%d inGame=%d ret=%06X", fovY, aspect, isFull, isNorm, inGame, (u32)__builtin_return_address(0) - g_base); }
    if (!g_enabled || W < 0.8f + 1e-3f || (!isFull && !isNorm)) { orig_Persp(out, 0, fovY, aspect, zn, zf); return; }
    int worldDef = g_worldBottomDefault && g_worldTopDefault;
    float* m = (float*)out;
    if (inGame && isFull && g_worldFull && worldDef) {
        // build the projection of the original world strip, then extend it to the full height at the same scale
        orig_Persp(out, 0, fovY, g_fovFix ? 0.8f / H0 : W / H0, zn, zf);
        m[5] *= H0 / H1;
        if (g_fovFix) m[0] = m[5] / aspect;
        m[0] /= g_zoom; m[5] /= g_zoom;
        float yc0 = (WORLD_B0 + 0.6f + WORLD_T0) * 0.5f, yc1 = 0.3f;
        m[9] += (yc0 - yc1) * 2.0f / H1;                         // view centre stays where it was on screen
        return;
    }
    if (inGame && isNorm) {
        orig_Persp(out, 0, fovY, g_fovFix ? 0.8f / H0 : aspect, zn, zf);
        if (g_fovFix) m[0] = m[5] / aspect;
        if (worldDef) { m[0] /= g_zoom; m[5] /= g_zoom; }
        return;
    }
    if (!inGame && isFull && g_fovFix) {                         // 3D menu backgrounds: vertical FOV of 4:3
        orig_Persp(out, 0, fovY, 0.8f / H1, zn, zf);
        m[0] = m[5] / aspect;
        return;
    }
    orig_Persp(out, 0, fovY, aspect, zn, zf);
}

// ---- with the world at full height, keep the console area itself black (its textures have see-through holes) ----
#define RVA_RenderWorld 0x395900
typedef int (__fastcall *RenderWorld_t)(u32 ecx, u32 edx);
static RenderWorld_t orig_RenderWorld;
typedef struct { DWORD X, Y, Width, Height; float MinZ, MaxZ; } VP8;
typedef struct { LONG x1, y1, x2, y2; } RECT8;
static int IsD3D8Device(u32* vt)
{
    static int state = -1;                                       // -1 unknown, 0 no, 1 yes
    if (state >= 0) return state;
    state = 0;
    HMODULE d3d = GetModuleHandleA("d3d8.dll");
    MEMORY_BASIC_INFORMATION mi;
    if (d3d && !IsBadReadPtr(vt, 42 * 4) && VirtualQuery((void*)vt[36], &mi, sizeof mi) && mi.AllocationBase == (void*)d3d)
        state = 1;
    if (!state) logf_("renderer is not Direct3D 8, console backing off");
    return state;
}
static int __fastcall RenderWorld_hook(u32 ecx, u32 edx)
{
    g_lastWorldTick = GetTickCount();
    int r = orig_RenderWorld(ecx, edx);
    g_lastWorldTick = GetTickCount();
    if (g_debug && g_gameUI && !IsBadReadPtr((void*)g_gameUI, 0x184)) {
        static u32 lastSt = 0xFFFFFFFF; u32 st = *(u32*)(g_gameUI + 0x180);
        if (st != lastSt) { lastSt = st; dlogf_("interface state %u (ui %08X)", st, g_gameUI); }
    }
    if (!(g_enabled && g_consoleBacking && g_worldFull && g_worldBottomDefault && g_worldTopDefault)) return r;
    u32 gx = *(u32*)(g_base + 0xACBD40);
    if (!gx || IsBadReadPtr((void*)gx, 0x590)) return r;
    u32 dev = *(u32*)(gx + 0x584);
    if (!dev || IsBadReadPtr((void*)dev, 4)) return r;
    u32* vt = *(u32**)dev;
    if (!IsD3D8Device(vt)) return r;                             // OpenGL mode or unknown renderer: leave it alone
    typedef HRESULT (__stdcall *SetVP_t)(u32, const VP8*);
    typedef HRESULT (__stdcall *GetVP_t)(u32, VP8*);
    typedef HRESULT (__stdcall *Clear_t)(u32, DWORD, const RECT8*, DWORD, DWORD, float, DWORD);
    VP8 old; if (((GetVP_t)vt[41])(dev, &old) != 0) return r;
    int W = *(int*)(gx + 0x9C), H = *(int*)(gx + 0xA0);
    if (W <= 0 || H <= 0) return r;
    VP8 full = { 0, 0, (DWORD)W, (DWORD)H, old.MinZ, old.MaxZ };
    ((SetVP_t)vt[40])(dev, &full);
    LONG x1 = (LONG)(g_e / g_uiW_f * W + 0.5f), x2 = (LONG)((g_e + 0.8f) / g_uiW_f * W + 0.5f);
    // in cinematic mode the letterbox (semi-transparent stone texture) spans the whole width; as in the
    // original, it must lie on black, not on the world
    if (g_cineFull && InCinematicMode()) { x1 = 0; x2 = W; }
    RECT8 rc[2] = {
        { x1, (LONG)(H * (1.0f - WORLD_B0 / 0.6f)), x2, H },            // console band
        { x1, 0, x2, (LONG)(H * (-WORLD_T0 / 0.6f) + 0.5f) }            // thin strip under the top bar
    };
    ((Clear_t)vt[36])(dev, 2, rc, 1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);
    // The 3D portraits (unit portrait in the console, speaker portrait of cinematics and transmissions) are
    // drawn later with depth testing. In the original nothing of the world is drawn in these strips, so the
    // depth buffer there is empty; here the world wrote its depth first and would hide parts of the portraits.
    // Reset the depth in both strips across the whole width (nothing else 3D is drawn there).
    RECT8 rz[2] = { { 0, rc[0].y1, W, H }, { 0, 0, W, rc[1].y2 } };
    ((Clear_t)vt[36])(dev, 2, rz, 2 /*D3DCLEAR_ZBUFFER*/, 0, 1.0f, 0);
    ((SetVP_t)vt[40])(dev, &old);
    return r;
}

static u32 MakeTrampoline(u32 target, int len)
{
    uint8_t* t = (uint8_t*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, (void*)target, len);
    t[len] = 0xE9;
    *(int32_t*)(t + len + 1) = (int32_t)((target + len) - ((u32)t + len + 5));
    return (u32)t;
}

static int WriteJmp(u32 from, u32 to)
{
    uint8_t j[5]; j[0] = 0xE9; *(int32_t*)(j + 1) = (int32_t)(to - (from + 5));
    return WriteMem(from, j, 5);
}

static u32 GetGameBuild(void)
{
    DWORD h; DWORD sz = GetFileVersionInfoSizeA("Game.dll", &h);
    if (!sz) return 0;
    char* buf = (char*)malloc(sz); u32 build = 0;
    if (GetFileVersionInfoA("Game.dll", h, sz, buf)) {
        VS_FIXEDFILEINFO* vi; UINT l;
        if (VerQueryValueA(buf, "\\", (LPVOID*)&vi, &l)) build = vi->dwFileVersionLS & 0xFFFF;
    }
    free(buf);
    return build;
}

static double ReadAspect(char* src, size_t srcLen)
{
    // 1) explicit override from W3TrueWidescreen.ini
    char ini[MAX_PATH]; GetModuleFileNameA(NULL, ini, MAX_PATH);
    char* s = strrchr(ini, '\\'); if (s) *(s + 1) = 0;
    strcat(ini, "W3TrueWidescreen.ini");
    char v[64];
    GetPrivateProfileStringA(INI_SECTION, "Aspect", "", v, sizeof v, ini);
    double a = atof(v);
    g_glue = GetPrivateProfileIntA(INI_SECTION, "Menus", 1, ini);
    g_menuBgFull = GetPrivateProfileIntA(INI_SECTION, "MenuBackgroundFull", 1, ini);
    g_menuEdges = GetPrivateProfileIntA(INI_SECTION, "MenuLayout", 1, ini);
    g_worldFull = GetPrivateProfileIntA(INI_SECTION, "WorldFullHeight", 1, ini);
    g_heroEdge = GetPrivateProfileIntA(INI_SECTION, "HeroBarEdge", 1, ini);
    g_cineFull = GetPrivateProfileIntA(INI_SECTION, "CinematicFullWidth", 1, ini);
    g_consoleBacking = GetPrivateProfileIntA(INI_SECTION, "ConsoleBacking", 1, ini);
    g_hpUnder = GetPrivateProfileIntA(INI_SECTION, "HealthBarsUnderConsole", 1, ini);
    g_fovFix = GetPrivateProfileIntA(INI_SECTION, "FovFix", 1, ini);
    g_fadeFix = GetPrivateProfileIntA(INI_SECTION, "FadeFix", 1, ini);
    g_debug = GetPrivateProfileIntA(INI_SECTION, "Debug", 0, ini);
    g_pathQuery = GetPrivateProfileIntA(INI_SECTION, "AllowPathQuery", 1, ini);
    g_fpsLimit = GetPrivateProfileIntA(INI_SECTION, "FpsLimit", 0, ini);
    if (g_fpsLimit < 0 || g_fpsLimit > 1000) g_fpsLimit = 0;
    g_movieRenderer = GetPrivateProfileIntA(INI_SECTION, "MovieRenderer", 1, ini);
    g_movieNative = GetPrivateProfileIntA(INI_SECTION, "MovieNativeMode", 1, ini);
    g_movieFullWin = GetPrivateProfileIntA(INI_SECTION, "MovieFullWindow", 1, ini);
    GetPrivateProfileStringA(INI_SECTION, "MoviePlayer", "0", g_moviePlayerOpt, sizeof g_moviePlayerOpt, ini);
    g_movieHdr = GetPrivateProfileIntA(INI_SECTION, "MovieRtxHdr", 1, ini);
    g_movieSuperRes = GetPrivateProfileIntA(INI_SECTION, "MovieSuperRes", 1, ini);
    g_movieSwap = GetPrivateProfileIntA(INI_SECTION, "MovieSwapEffect", 1, ini);
    g_movieDetach = GetPrivateProfileIntA(INI_SECTION, "MovieOwnWindow", 1, ini);
    g_movieReinit = GetPrivateProfileIntA(INI_SECTION, "MovieReinit", 1, ini);
    g_movieTexFmt = GetPrivateProfileIntA(INI_SECTION, "MovieTexFormat", 10, ini);
    g_movieVrDebug = GetPrivateProfileIntA(INI_SECTION, "MovieRendererDebug", 0, ini);
    {   char zv[32]; GetPrivateProfileStringA(INI_SECTION, "CameraZoomOut", "1.0", zv, sizeof zv, ini);
        g_zoom = (float)atof(zv); if (g_zoom < 0.5f || g_zoom > 2.0f) g_zoom = 1.0f; }
    {   char kv[256]; GetPrivateProfileStringA(INI_SECTION, "KeepFull", "", kv, sizeof kv, ini);
        char* t = strtok(kv, ", ");
        while (t && g_keepVtN < 16) { g_keepVt[g_keepVtN++] = strtoul(t, NULL, 16); t = strtok(NULL, ", "); } }
    if (a > 0.5) { snprintf(src, srcLen, "ini Aspect=%s", v); return a; }
    int w = GetPrivateProfileIntA(INI_SECTION, "Width", 0, ini), hgt = GetPrivateProfileIntA(INI_SECTION, "Height", 0, ini);
    if (w > 0 && hgt > 0) { snprintf(src, srcLen, "ini %dx%d", w, hgt); return (double)w / hgt; }
    // 2) game video settings in registry
    HKEY k; DWORD rw = 0, rh = 0, sz = 4;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Blizzard Entertainment\\Warcraft III\\Video", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, "reswidth", NULL, NULL, (LPBYTE)&rw, &sz); sz = 4;
        RegQueryValueExA(k, "resheight", NULL, NULL, (LPBYTE)&rh, &sz);
        RegCloseKey(k);
    }
    if (rw > 0 && rh > 0) { snprintf(src, srcLen, "registry %lux%lu", rw, rh); return (double)rw / rh; }
    // 3) primary monitor
    w = GetSystemMetrics(SM_CXSCREEN); hgt = GetSystemMetrics(SM_CYSCREEN);
    snprintf(src, srcLen, "desktop %dx%d", w, hgt);
    return hgt ? (double)w / hgt : 4.0 / 3.0;
}

// ---- screen fade of the campaign menu: its model is a black quad 0.8 units wide, so on a wide screen it
//      would cover only the left part. On first use the original model is read from the game archives,
//      its x coordinates are stretched to the screen width, the result is written to the W3TrueWidescreen_cache folder
//      and the game is given that copy. No game files are shipped or changed. ----
typedef BOOL  (__stdcall *SOpenEx_t)(HANDLE, const char*, DWORD, HANDLE*);
typedef BOOL  (__stdcall *SRead_t)(HANDLE, void*, DWORD, DWORD*, void*);
typedef DWORD (__stdcall *SSize_t)(HANDLE, DWORD*);
typedef BOOL  (__stdcall *SClose_t)(HANDLE);
static SOpenEx_t orig_SOpenEx;
static SRead_t   S_Read;
static SSize_t   S_Size;
static SClose_t  S_Close;
static int  g_fadeState = 0;          // 0 not built yet, 1 cache ready, -1 failed (use original)
static char g_fadePath[MAX_PATH];
static const char kFadeName[] = "UI\\Glues\\SinglePlayer\\Campaign-Fade\\Campaign-Fade.mdx";

static int SameName(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '/' ? '\\' : *a, y = *b == '/' ? '\\' : *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

// stretch every vertex x (and the extents / pivots that repeat those values) by f
static int WidenMdx(uint8_t* d, DWORD n, float f)
{
    if (n < 16 || memcmp(d, "MDLX", 4) != 0) return 0;
    float xs[64]; int nx = 0;
    DWORD p = 4;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) return 0;
        if (memcmp(d + p, "GEOS", 4) == 0) {
            DWORD g = p + 8, end = p + 8 + sz;
            while (g + 4 <= end) {
                DWORD gsz = *(DWORD*)(d + g);
                if (gsz < 12 || g + gsz > end) return 0;
                if (memcmp(d + g + 4, "VRTX", 4) == 0) {
                    DWORD cnt = *(DWORD*)(d + g + 8);
                    if (12 + cnt * 12 > gsz) return 0;
                    for (DWORD v = 0; v < cnt; v++) {
                        float x; memcpy(&x, d + g + 12 + v * 12, 4);
                        int dup = 0; for (int k = 0; k < nx; k++) if (xs[k] == x) dup = 1;
                        if (!dup && nx < 64) xs[nx++] = x;
                    }
                }
                g += gsz;
            }
        }
        p += 8 + sz;
    }
    if (!nx) return 0;
    int changed = 0;
    for (DWORD q = 4; q + 4 <= n; q += 4) {
        float v; memcpy(&v, d + q, 4);
        for (int k = 0; k < nx; k++)
            if (v == xs[k] && v != 0.0f) { v *= f; memcpy(d + q, &v, 4); changed++; break; }
    }
    return changed;
}

static int BuildFadeCache(HANDLE mpq, const char* name, DWORD scope)
{
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* buf = (n > 0 && n < (1u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    if (ok) ok = WidenMdx(buf, n, g_uiW_f / 0.8f) > 0;
    if (ok) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
        strcat(dir, "W3TrueWidescreen_cache");
        CreateDirectoryA(dir, NULL);
        snprintf(g_fadePath, sizeof g_fadePath, "%s\\Campaign-Fade.mdx", dir);
        FILE* f = fopen(g_fadePath, "wb");
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f) fclose(f);
    }
    free(buf);
    return ok;
}

static BOOL __stdcall SOpenEx_hook(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (g_fadeState >= 0 && name && ph && !IsBadStringPtrA(name, MAX_PATH) && SameName(name, kFadeName)) {
        if (g_fadeState == 0) {
            g_fadeState = BuildFadeCache(mpq, name, scope) ? 1 : -1;
            logf_(g_fadeState > 0 ? "campaign fade widened" : "campaign fade: could not build widened copy, using original");
        }
        if (g_fadeState > 0) {
            // search scope bits: 1|2 = archives and disk (what the game passes with "Allow Local Files" on),
            // 4 = raw OS handle (must not be set). Asking for disk explicitly makes this work without that setting.
            BOOL ok = orig_SOpenEx(mpq, g_fadePath, (scope & ~4u) | 3u, ph);
            if (ok && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; ok = FALSE; }   // raw OS handle: not usable
            if (ok) return TRUE;
            logf_("campaign fade: widened copy could not be opened (scope %X, error %u), using original", scope, GetLastError());
            g_fadeState = -1;
        }
    }
    return orig_SOpenEx(mpq, name, scope, ph);
}

static void InstallFadeFix(void)
{
    if (!g_fadeFix) return;
    HMODULE st = GetModuleHandleA("Storm.dll");
    if (!st) return;
    u32 f = (u32)GetProcAddress(st, (LPCSTR)268);
    S_Read  = (SRead_t)GetProcAddress(st, (LPCSTR)269);
    S_Size  = (SSize_t)GetProcAddress(st, (LPCSTR)265);
    S_Close = (SClose_t)GetProcAddress(st, (LPCSTR)253);
    if (!f || !S_Read || !S_Size || !S_Close || memcmp((void*)f, "\x81\xEC\x1C\x01\x00\x00", 6) != 0) {
        logf_("Storm.dll not recognised, campaign fade fix off"); return;
    }
    orig_SOpenEx = (SOpenEx_t)MakeTrampoline(f, 6);
    if (orig_SOpenEx) WriteJmp(f, (u32)SOpenEx_hook);
}

// ---- cinematics: DirectShow plays them in a child window, outside the game's Direct3D (so RTX HDR etc. never
//      see them). If W3TrueWidescreen\MpcVideoRenderer.ax is present, it is put into the game's filter graph before the
//      game renders the movie, so the video goes through MPC Video Renderer (RTX Video HDR / super resolution).
//      The .ax is loaded directly from the game folder; it does not have to be registered. ----
#define RVA_IAT_CoCreateInstance 0x86DAA8
typedef HRESULT (WINAPI *CoCreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);
static CoCreate_t orig_CoCreate;
static DllGetClassObject_t g_mpcvrGetClass;
static char g_mpcvrPath[MAX_PATH];
static int LoadMovieComponents(void);
static DllGetClassObject_t g_lavGetClass;
static DllGetClassObject_t g_lavSplitGetClass;     // optional: W3TrueWidescreen\\LAV\\LAVSplitter.ax (unpacks DivX packed B-frames)
static const GUID kCLSID_FilterGraph = {0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID kCLSID_LAVSplitter = {0x171252a0,0x8820,0x4afe,{0x9d,0xf8,0x5c,0x92,0xb2,0xd6,0x6b,0x04}};
static const GUID kCLSID_LAVVideo    = {0xee30215d,0x164f,0x4a92,{0xa4,0xeb,0x9d,0x4c,0x13,0x39,0x0f,0x9f}};
static const GUID kCLSID_MPCVR       = {0x71f080aa,0x8661,0x4093,{0xb1,0x5e,0x4f,0x69,0x03,0xe7,0x7d,0x0a}};
static const GUID kIID_IClassFactory = {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID kIID_IBaseFilter   = {0x56a86895,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID kIID_IFilterGraph  = {0x56a8689f,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

typedef HRESULT (__stdcall *QI_t)(void*, REFIID, void**);
typedef ULONG   (__stdcall *Release_t)(void*);
typedef HRESULT (__stdcall *CFCreate_t)(void*, void*, REFIID, void**);
typedef HRESULT (__stdcall *AddFilter_t)(void*, void*, LPCWSTR);
typedef HRESULT (__stdcall *Rect4_t)(void*, long, long, long, long);
#define VTBL(p) (*(void***)(p))

// settings the renderer must have inside the game window, whatever the user chose for his media player;
// they are applied only while our instance is created (the renderer reads them in its constructor)
static struct { const char* name; DWORD val; } kMpcvrForced[] = {
    { "ExclusiveFullscreen", 0 },     // never grab the screen away from the game
    { "VPRTXVideoHDR", 1 },           // MovieRtxHdr in the ini
    { "VPSuperResolution", 0 },       // MovieSuperRes in the ini
    { "SwapEffect", 1 },              // flip presentation (MovieSwapEffect in the ini: 0 = discard, 1 = flip)
    { "TextureFormat", 0 },           // MovieTexFormat in the ini: 0 auto, 10 = 10-bit (keeps the game's RTX HDR off the video)
    { "ShowStatistics", 0 },          // MovieRendererDebug=1 turns this on (see below)
};
static void AddSimpleFilter(void* fg, DllGetClassObject_t gco, const GUID* clsid, const WCHAR* name)
{
    void* cf = NULL; void* f = NULL;
    if (!gco || FAILED(gco(clsid, &kIID_IClassFactory, &cf)) || !cf) return;
    HRESULT hr = ((CFCreate_t)VTBL(cf)[3])(cf, NULL, &kIID_IBaseFilter, &f);
    ((Release_t)VTBL(cf)[2])(cf);
    if (SUCCEEDED(hr) && f) {
        hr = ((AddFilter_t)VTBL(fg)[3])(fg, f, name);
        dlogf_("movies: %ls added to the graph (%08lX)", name, hr);
        ((Release_t)VTBL(f)[2])(f);
    } else logf_("movies: could not create %ls (%08lX)", name, hr);
}

static BOOL CALLBACK FindGameWndProc(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r);
    HWND* best = (HWND*)lp;
    RECT br = { 0 }; if (*best) GetClientRect(*best, &br);
    if (!*best || (long)r.right * r.bottom > (long)br.right * br.bottom) *best = h;
    return TRUE;
}
static HWND FindGameWindow(void)      // the game's main (largest visible, unowned) window in this process
{
    HWND best = NULL;
    EnumWindows(FindGameWndProc, (LPARAM)&best);
    return best;
}
static void AddMpcvrToGraph(void* graph)
{
    void* cf = NULL; void* flt = NULL; void* fg = NULL;
    if (FAILED(g_mpcvrGetClass(&kCLSID_MPCVR, &kIID_IClassFactory, &cf)) || !cf) { logf_("movies: MPC Video Renderer class not available"); return; }
    HKEY k = NULL, probe = NULL; DWORD saved[8]; BOOL had[8] = { 0 };
    // the renderer reads its settings from the registry when it is created: set ours for that moment only
    BOOL hadParent = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\MPC-BE Filters", 0, KEY_READ, &probe) == ERROR_SUCCESS;
    if (probe) RegCloseKey(probe);
    DWORD disp = 0;
    kMpcvrForced[1].val = g_movieHdr ? 1 : 0;
    kMpcvrForced[2].val = g_movieSuperRes ? 1 : 0;       // 1 = RTX super resolution for SD video
    kMpcvrForced[3].val = g_movieSwap;
    kMpcvrForced[4].val = g_movieTexFmt;
    const int nf = sizeof kMpcvrForced / sizeof kMpcvrForced[0];
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MPC-BE Filters\\MPC Video Renderer", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, &disp) == ERROR_SUCCESS) {
        for (int i = 0; i < nf; i++) {
            DWORD sz = 4; had[i] = RegQueryValueExA(k, kMpcvrForced[i].name, NULL, NULL, (LPBYTE)&saved[i], &sz) == ERROR_SUCCESS;
            DWORD v = kMpcvrForced[i].val;
            if (g_movieVrDebug && !strcmp(kMpcvrForced[i].name, "ShowStatistics")) v = 1;
            RegSetValueExA(k, kMpcvrForced[i].name, 0, REG_DWORD, (const BYTE*)&v, 4);
        }
    } else k = NULL;
    HRESULT hr = ((CFCreate_t)VTBL(cf)[3])(cf, NULL, &kIID_IBaseFilter, &flt);
    ((Release_t)VTBL(cf)[2])(cf);
    if (k) {
        for (int i = 0; i < nf; i++) {
            if (had[i]) RegSetValueExA(k, kMpcvrForced[i].name, 0, REG_DWORD, (const BYTE*)&saved[i], 4);
            else RegDeleteValueA(k, kMpcvrForced[i].name);
        }
        RegCloseKey(k);
        if (disp == REG_CREATED_NEW_KEY) {                 // leave no trace for people without MPC-BE / the renderer
            RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\MPC-BE Filters\\MPC Video Renderer");
            if (!hadParent) RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\MPC-BE Filters");
        }
    }
    dlogf_("movies: renderer created (%08lX)", hr);
    if (FAILED(hr) || !flt) { logf_("movies: could not create MPC Video Renderer (%08lX)", hr); return; }
    // Give the renderer its window (owner + full size) BEFORE the game renders the file. The game only does this
    // after RenderFile, so the renderer would build its swap chain for a 0x0 window (debug trace: "Invalid window
    // size 0x0, use 8x8") and later just resize it - which shows up as stale blocks on screen.
    {
        static const GUID kIID_IVideoWindow = {0x56a868b4,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
        HWND gw = FindGameWindow();
        void* vw = NULL;
        if (gw && SUCCEEDED(((QI_t)VTBL(flt)[0])(flt, &kIID_IVideoWindow, &vw)) && vw) {
            typedef HRESULT (__stdcall *PutOwner_t)(void*, LONG_PTR);
            RECT rc; GetClientRect(gw, &rc);
            HRESULT h1 = ((PutOwner_t)VTBL(vw)[29])(vw, (LONG_PTR)gw);                        // put_Owner
            HRESULT h2 = ((Rect4_t)VTBL(vw)[0x9c / 4])(vw, 0, 0, rc.right, rc.bottom);          // SetWindowPosition
            logf_("movies: renderer window prepared %ldx%ld (%08lX %08lX)", rc.right, rc.bottom, h1, h2);
            ((Release_t)VTBL(vw)[2])(vw);
        } else logf_("movies: could not prepare renderer window (game window %p)", gw);
    }
    if (SUCCEEDED(((QI_t)VTBL(graph)[0])(graph, &kIID_IFilterGraph, &fg)) && fg) {
        hr = ((AddFilter_t)VTBL(fg)[3])(fg, flt, L"MPC Video Renderer");
        dlogf_("movies: MPC Video Renderer added to the graph (%08lX)", hr);
        // The Windows MPEG-4 decoder corrupts these DivX streams when it feeds MPC Video Renderer;
        // LAV Video (ffmpeg) decodes them correctly. Filters already in the graph are tried first.
        // The Microsoft AVI Splitter passes DivX "packed bitstream" B-frames through as is, which leaves the
        // decoder with smeared remains of earlier frames; LAV Splitter unpacks them (as media players do).
        AddSimpleFilter(fg, g_lavSplitGetClass, &kCLSID_LAVSplitter, L"LAV Splitter");
        void* cf2 = NULL; void* lav = NULL;
        if (g_lavGetClass && SUCCEEDED(g_lavGetClass(&kCLSID_LAVVideo, &kIID_IClassFactory, &cf2)) && cf2) {
            // LAV decodes MPEG-4 ASP in software unless hardware decoding was enabled for it in LAV's own settings
            HRESULT h2 = ((CFCreate_t)VTBL(cf2)[3])(cf2, NULL, &kIID_IBaseFilter, &lav);
            ((Release_t)VTBL(cf2)[2])(cf2);
            if (SUCCEEDED(h2) && lav) {
                h2 = ((AddFilter_t)VTBL(fg)[3])(fg, lav, L"LAV Video Decoder");
                dlogf_("movies: LAV Video added to the graph (%08lX)", h2);
                ((Release_t)VTBL(lav)[2])(lav);
            } else logf_("movies: could not create LAV Video (%08lX)", h2);
        }
        ((Release_t)VTBL(fg)[2])(fg);
    }
    ((Release_t)VTBL(flt)[2])(flt);
}

static void HookVideoExtensions(void);
static HRESULT WINAPI CoCreate_hook(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out)
{
    static int extLog;
    if (g_debug && !extLog && IsEqualGUID(clsid, &kCLSID_FilterGraph)) { extLog = 1; HookVideoExtensions(); }   // not from DllMain (loader lock)
    if (g_mpcvrPath[0] && IsEqualGUID(clsid, &kCLSID_FilterGraph)) LoadMovieComponents();
    HRESULT hr = orig_CoCreate(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && out && *out && g_mpcvrGetClass && IsEqualGUID(clsid, &kCLSID_FilterGraph))
        AddMpcvrToGraph(*out);
    return hr;
}

static const GUID kIID_IBasicVideo = {0x56a868b5,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
typedef HRESULT (__stdcall *EnumF_t)(void*, void**);
typedef HRESULT (__stdcall *Next_t)(void*, ULONG, void**, ULONG*);
typedef struct { WCHAR name[128]; void* graph; } FINFO;
static void LogGraph(void* anyGraphItf)
{
    void* fg = NULL; void* en = NULL; void* f = NULL; ULONG got = 0;
    if (FAILED(((QI_t)VTBL(anyGraphItf)[0])(anyGraphItf, &kIID_IFilterGraph, &fg)) || !fg) return;
    if (SUCCEEDED(((EnumF_t)VTBL(fg)[5])(fg, &en)) && en) {
        while (((Next_t)VTBL(en)[3])(en, 1, &f, &got) == S_OK && got) {
            FINFO fi; memset(&fi, 0, sizeof fi);
            typedef HRESULT (__stdcall *QFI_t)(void*, FINFO*);
            if (SUCCEEDED(((QFI_t)VTBL(f)[12])(f, &fi))) {
                logf_("movies: graph has '%ls'", fi.name);
                if (fi.graph) ((Release_t)VTBL(fi.graph)[2])(fi.graph);
            }
            ((Release_t)VTBL(f)[2])(f);
        }
        ((Release_t)VTBL(en)[2])(en);
    }
    ((Release_t)VTBL(fg)[2])(fg);
}
// The renderer's window is a child of the game window; drawn that way, the display shows stale blocks from
// earlier frames (the renderer's own output is clean). Moving it into a separate borderless window that sits
// above the game (owned by it, never activated) makes Windows compose it like a normal video player.
static LRESULT CALLBACK MovieHostProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == 2) {
            // the renderer's first (SDR -> HDR) initialisation leaves stale blocks on screen until it is
            // re-initialised; a display-change notification makes it rebuild its swap chain once
            KillTimer(h, 2);
            HWND o = GetWindow(h, GW_OWNER);
            if (o && g_movieReinit) {
                HDC dc = GetDC(NULL);
                WPARAM bpp = dc ? (WPARAM)GetDeviceCaps(dc, BITSPIXEL) : 32;
                if (dc) ReleaseDC(NULL, dc);
                SendMessageW(o, WM_DISPLAYCHANGE, bpp, MAKELPARAM(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
                dlogf_("movies: renderer re-initialised");
            }
            return 0;
        }
        if (!GetWindow(h, GW_CHILD)) { KillTimer(h, 1); DestroyWindow(h); }   // movie finished
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_KEYDOWN: {
        HWND o = GetWindow(h, GW_OWNER);
        if (o) PostMessageW(o, m, wp, lp);                                   // let the game skip the movie
        return 0; }
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void DetachMovieWindow(void* vw)
{
    typedef HRESULT (__stdcall *GetOwner_t)(void*, OAHWND_*);
    OAHWND_ ow = 0;
    HRESULT ho = ((GetOwner_t)VTBL(vw)[30])(vw, &ow);                      // IVideoWindow::get_Owner
    if (FAILED(ho) || !ow) { logf_("movies: detach: no owner (%08lX)", ho); return; }
    HWND owner = (HWND)ow;
    HWND vr = FindWindowExW(owner, NULL, L"VRWindow", NULL);
    if (!vr) { logf_("movies: detach: renderer window not found under %p", owner); return; }
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = MovieHostProc; wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"W3TrueWidescreenMovieHost"; wc.hCursor = NULL;
        cls = RegisterClassExW(&wc);
    }
    RECT rc; GetClientRect(owner, &rc);
    POINT p0 = { 0, 0 }; ClientToScreen(owner, &p0);
    HWND host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"W3TrueWidescreenMovieHost", L"", WS_POPUP,
                                p0.x, p0.y, rc.right, rc.bottom, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!host) { logf_("movies: detach: host window failed (%lu)", GetLastError()); return; }
    SetParent(vr, host);
    ShowWindow(host, SW_SHOWNOACTIVATE);
    SetTimer(host, 1, 250, NULL);
    SetTimer(host, 2, 300, NULL);
    logf_("movies: renderer window moved to its own top-level window %p (%ldx%ld)", host, rc.right, rc.bottom);
}

static HRESULT __stdcall SetWinPos_wrap(void* vw, long l, long t, long w, long h)
{
    static int logged = 0;
    if (logged++ < 2) LogGraph(vw);
    // The game sizes the video window to the picture (letterbox strips are its parent window). With
    // MovieFullWindow=1 the renderer window covers the whole movie window and draws the letterbox itself:
    // a flip-model swap chain that covers the screen is shown by Windows directly (independent flip),
    // as in a fullscreen video player, instead of being composited as a smaller window.
    long ww = w, wh = h, dl = 0, dt = 0;
    int mw = *(int*)(g_base + 0xACC698), mh = *(int*)(g_base + 0xACC69C);  // movie window size (game globals)
    if (g_movieFullWin && mw >= w && mh >= t + h && mw > 0 && mh > 0) { ww = mw; wh = mh; dl = l; dt = t; l = 0; t = 0; }
    HRESULT hr = ((Rect4_t)VTBL(vw)[0x9c / 4])(vw, l, t, ww, wh);      // IVideoWindow::SetWindowPosition
    void* bv = NULL;
    if (SUCCEEDED(((QI_t)VTBL(vw)[0])(vw, &kIID_IBasicVideo, &bv)) && bv) {
        HRESULT hr2 = ((Rect4_t)VTBL(bv)[31])(bv, dl, dt, w, h);      // IBasicVideo::SetDestinationPosition
        logf_("movies: window %ld,%ld %ldx%ld, picture %ld,%ld %ldx%ld (%08lX)", l, t, ww, wh, dl, dt, w, h, hr2);
        ((Release_t)VTBL(bv)[2])(bv);
    }
    if (g_movieDetach) DetachMovieWindow(vw);
    return hr;
}


// ---- diagnostics: with MovieRendererDebug=1 the debug build of MPC Video Renderer is loaded and its
//      OutputDebugString trace is written to W3TrueWidescreen_mpcvr.log ----
static FILE* g_vrLog;
static void WINAPI ODS_hook(LPCWSTR s)
{
    if (!g_vrLog || !s) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_vrLog, "%02d:%02d:%02d.%03d [%5lu] %ls", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), s);
    size_t n = wcslen(s); if (!n || s[n - 1] != L'\n') fputc('\n', g_vrLog);
    fflush(g_vrLog);
}
static void HookModuleImport(HMODULE m, const char* dll, const char* fn, void* hook)
{
    uint8_t* b = (uint8_t*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((char*)(b + d->Name), dll)) continue;
        IMAGE_THUNK_DATA* nameT = (IMAGE_THUNK_DATA*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* addrT = (IMAGE_THUNK_DATA*)(b + d->FirstThunk);
        for (; nameT->u1.AddressOfData; nameT++, addrT++) {
            if (nameT->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(b + nameT->u1.AddressOfData);
            if (!strcmp((char*)ibn->Name, fn)) { u32 h = (u32)hook; WriteMem((u32)&addrT->u1.Function, &h, 4); return; }
        }
    }
}

// war3.exe has no Windows 10 compatibility manifest, so Windows reports itself as "Windows 8" to it
// (VerifyVersionInfo). MPC Video Renderer then turns HDR passthrough off, and with it NVIDIA RTX Video HDR.
// Inside the renderer, version checks get the real version (RtlGetVersion).
typedef BOOL (WINAPI *VerifyVersion_t)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);
typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOEXW*);
static VerifyVersion_t orig_VerifyVersion;
static BOOL VerTest(int op, DWORD a, DWORD b)          // a = actual, b = requested
{
    switch (op) {
    case VER_EQUAL: return a == b;
    case VER_GREATER: return a > b;
    case VER_GREATER_EQUAL: return a >= b;
    case VER_LESS: return a < b;
    case VER_LESS_EQUAL: return a <= b;
    }
    return TRUE;
}
static BOOL WINAPI VerifyVersion_real(LPOSVERSIONINFOEXW vi, DWORD mask, DWORDLONG cond)
{
    static RtlGetVersion_t rgv;
    if (!rgv) rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    const DWORD known = VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR;
    OSVERSIONINFOEXW r; memset(&r, 0, sizeof r); r.dwOSVersionInfoSize = sizeof r;
    if (!vi || !rgv || (mask & ~known) || !(mask & known) || rgv(&r) != 0) return orig_VerifyVersion(vi, mask, cond);
    #define VOP(bit) ((int)((cond >> (3 * __builtin_ctz(bit))) & 7))
    // major.minor.spmajor.spminor compare as one version (as Windows does), the build number on its own
    DWORD act[4] = { r.dwMajorVersion, r.dwMinorVersion, r.wServicePackMajor, r.wServicePackMinor };
    DWORD req[4] = { vi->dwMajorVersion, vi->dwMinorVersion, vi->wServicePackMajor, vi->wServicePackMinor };
    static const DWORD bits[4] = { VER_MAJORVERSION, VER_MINORVERSION, VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR };
    BOOL ok = TRUE; int op = -1;
    for (int i = 0; i < 4; i++) {
        if (!(mask & bits[i])) continue;
        if (op < 0) op = VOP(bits[i]);
        if (act[i] != req[i]) { ok = VerTest(op, act[i], req[i]); op = -2; break; }
    }
    if (op >= 0) ok = VerTest(op, 0, 0);                      // all fields equal
    if (ok && (mask & VER_BUILDNUMBER)) ok = VerTest(VOP(VER_BUILDNUMBER), r.dwBuildNumber, vi->dwBuildNumber);
    #undef VOP
    if (!ok) SetLastError(ERROR_OLD_WIN_VERSION);
    return ok;
}

// Debug=1: log what the driver answers when the renderer asks for NVIDIA RTX Video super resolution / HDR
// (ID3D11VideoContext::VideoProcessorSetStreamExtension; the runtime's video context vtable is shared by all
// devices in the process, so patching it once through a throwaway device covers the renderer's device too)
typedef HRESULT (STDMETHODCALLTYPE *SSE_t)(ID3D11VideoContext*, ID3D11VideoProcessor*, UINT, const GUID*, UINT, void*);
static SSE_t orig_SSE;
static HRESULT STDMETHODCALLTYPE SSE_hook(ID3D11VideoContext* c, ID3D11VideoProcessor* vp, UINT st, const GUID* g, UINT n, void* d)
{
    HRESULT hr = orig_SSE(c, vp, st, g, n, d);
    static int cnt;
    if (cnt++ < 40) {
        const UINT* u = (const UINT*)d;
        const char* what = !g ? "?" : g->Data1 == 0xd43ce1b3 ? "super resolution" : g->Data1 == 0xfdd62bb4 ? "RTX Video HDR" : "other";
        logf_("movies: driver extension %s (%08lX) v%u method %u enable %u -> %08lX", what, g ? g->Data1 : 0,
              n >= 4 ? u[0] : 0, n >= 8 ? u[1] : 0, n >= 12 ? (u[2] & 1) : 0, hr);
    }
    return hr;
}
static void HookVideoExtensions(void)
{
    typedef HRESULT (WINAPI *CreateDev_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d = LoadLibraryA("d3d11.dll");
    CreateDev_t cd = d ? (CreateDev_t)GetProcAddress(d, "D3D11CreateDevice") : NULL;
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; ID3D11VideoContext* vc = NULL;
    HRESULT hr0 = cd ? cd(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx) : E_FAIL;
    if (!cd || FAILED(hr0)) {
        logf_("movies: no D3D11 device for the extension log (%08lX)", cd ? hr0 : 0); return;
    }
    static const GUID kIID_VC = {0x61F21C45,0x3C0E,0x4a74,{0x9C,0xEA,0x67,0x10,0x0D,0x9A,0xD5,0xE4}};
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &kIID_VC, (void**)&vc)) && vc) {
        void** vt = *(void***)vc;
        size_t idx = offsetof(ID3D11VideoContextVtbl, VideoProcessorSetStreamExtension) / sizeof(void*);
        if (vt[idx] != (void*)SSE_hook) {
            orig_SSE = (SSE_t)vt[idx];
            void* h = (void*)SSE_hook; WriteMem((u32)&vt[idx], &h, 4);
            logf_("movies: driver extension log on");
        }
        ID3D11VideoContext_Release(vc);
    }
    ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
}

// MPC Video Renderer (and LAV) are loaded at the first movie, not at startup: initialising the renderer
// changes the process enough to expose memory bugs in some maps (DracoL1ch DotA: heap corruption at hero pick)
static int LoadMovieComponents(void)
{
    static int tried; if (tried) return g_mpcvrGetClass != NULL; tried = 1;
    char p[MAX_PATH]; strcpy(p, g_mpcvrPath);
    HMODULE m = LoadLibraryA(p);
    g_mpcvrGetClass = m ? (DllGetClassObject_t)GetProcAddress(m, "DllGetClassObject") : NULL;
    if (m && g_movieVrDebug) {
        char lp[MAX_PATH]; GetModuleFileNameA(NULL, lp, MAX_PATH);
        char* sl2 = strrchr(lp, '\\'); if (sl2) *(sl2 + 1) = 0;
        strcat(lp, "W3TrueWidescreen_mpcvr.log");
        g_vrLog = fopen(lp, "w");
        HookModuleImport(m, "KERNEL32.dll", "OutputDebugStringW", (void*)ODS_hook);
        logf_("movies: renderer debug trace -> %s (%s)", lp, p);
    }
    if (!g_mpcvrGetClass) { logf_("movies: could not load %s", p); return 0; }
    orig_VerifyVersion = (VerifyVersion_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "VerifyVersionInfoW");
    if (orig_VerifyVersion) HookModuleImport(m, "KERNEL32.dll", "VerifyVersionInfoW", (void*)VerifyVersion_real);
    char* w = strrchr(p, '\\'); if (w) *(w + 1) = 0;
    strcat(p, "LAV\\LAVVideo.ax");
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
        HMODULE lm = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   // its ffmpeg DLLs sit next to it
        g_lavGetClass = lm ? (DllGetClassObject_t)GetProcAddress(lm, "DllGetClassObject") : NULL;
        if (!g_lavGetClass) logf_("movies: could not load %s (error %lu)", p, GetLastError());
        char* w2 = strrchr(p, '\\'); if (w2) *(w2 + 1) = 0;
        strcat(p, "LAVSplitter.ax");
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            HMODULE sm = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            g_lavSplitGetClass = sm ? (DllGetClassObject_t)GetProcAddress(sm, "DllGetClassObject") : NULL;
            if (!g_lavSplitGetClass) logf_("movies: could not load %s (error %lu)", p, GetLastError());
        }
    }
    logf_("movies: MPC Video Renderer loaded%s%s", g_lavGetClass ? ", LAV Video decoder" : "", g_lavSplitGetClass ? ", LAV Splitter" : "");
    return 1;
}

static void InstallMovieRenderer(void)
{
    if (!g_movieRenderer) return;
    char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH);
    char* sl = strrchr(p, '\\'); if (sl) *(sl + 1) = 0;
    strcat(p, "W3TrueWidescreen\\MpcVideoRenderer.ax");
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return;       // optional component not installed
    if (g_movieVrDebug) {
        char dp[MAX_PATH]; strcpy(dp, p);
        char* e = strrchr(dp, '.'); if (e) strcpy(e, "_dbg.ax");
        if (GetFileAttributesA(dp) != INVALID_FILE_ATTRIBUTES) strcpy(p, dp);
    }
    strcpy(g_mpcvrPath, p);
    // The game sizes the movie window from IBasicVideo::get_VideoWidth/Height, which MPC Video Renderer does not
    // implement (the values stay uninitialised, the window lands off screen). Ask GetVideoSize instead.
    static const uint8_t kOrigSize[] = { 0x8b,0x08,0x8d,0x54,0x24,0x0c,0x52,0x50,0x8b,0x41,0x28,0xff,0xd0,0x8b,0x04,0x24,
        0x8b,0x08,0x8d,0x54,0x24,0x08,0x52,0x50,0x8b,0x41,0x2c,0xff,0xd0,0x8b,0x04,0x24,0x8b,0x08,0x8b,0x51,0x08,0x50,0xff,0xd2,0xeb,0x10 };
    static const uint8_t kNewSize[] = {
        0x8b,0x08,                         // mov  ecx, [eax]
        0x8d,0x54,0x24,0x08, 0x52,         // lea  edx, [esp+8]  (height) ; push edx
        0x8d,0x54,0x24,0x10, 0x52,         // lea  edx, [esp+10h] (width) ; push edx
        0x50,                              // push eax
        0xff,0x91,0x88,0x00,0x00,0x00,     // call [ecx+88h]  IBasicVideo::GetVideoSize
        0x8b,0x04,0x24,0x8b,0x08,0x8b,0x51,0x08,0x50,0xff,0xd2,   // pBV->Release()
        0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
        0xeb,0x10 };                       // jmp to the window placement code (unchanged)
    if (sizeof kNewSize == sizeof kOrigSize && memcmp((void*)(g_base + 0x52B55E), kOrigSize, sizeof kOrigSize) == 0)
        WriteMem(g_base + 0x52B55E, kNewSize, sizeof kNewSize);
    else { logf_("movies: movie window code not recognised, MPC Video Renderer off"); g_mpcvrGetClass = NULL; g_mpcvrPath[0] = 0; return; }
    // MPC Video Renderer draws into the rectangle given by IBasicVideo::SetDestinationPosition, which the game
    // never calls (the system renderer defaults to the whole window). Route the game's SetWindowPosition call
    // through a wrapper that also sets the destination to the whole window.
    static const uint8_t kOrigPos[] = { 0x8b,0x87,0x9c,0x00,0x00,0x00, 0x52, 0xff,0xd0 };
    if (memcmp((void*)(g_base + 0x52B5D3), kOrigPos, sizeof kOrigPos) == 0) {
        uint8_t p[6] = { 0xb8, 0, 0, 0, 0, 0x90 };               // mov eax, SetWinPos_wrap ; nop
        u32 f = (u32)SetWinPos_wrap; memcpy(p + 1, &f, 4);
        WriteMem(g_base + 0x52B5D3, p, 6);
    } else { logf_("movies: movie window code not recognised, MPC Video Renderer off"); g_mpcvrGetClass = NULL; g_mpcvrPath[0] = 0; return; }
    u32 iat = g_base + RVA_IAT_CoCreateInstance;
    if (*(u32*)iat != (u32)GetProcAddress(GetModuleHandleA("ole32.dll"), "CoCreateInstance") &&
        *(u32*)iat != (u32)GetProcAddress(GetModuleHandleA("combase.dll"), "CoCreateInstance"))
        dlogf_("movies: CoCreateInstance import is already hooked by something else, chaining");
    orig_CoCreate = (CoCreate_t)*(u32*)iat;
    u32 hook = (u32)CoCreate_hook;
    WriteMem(iat, &hook, 4);
    logf_("movies: MPC Video Renderer on (loaded at the first movie)");
}

// ---- cinematics. For every movie the game switches the display to 800x600 (ChangeDisplaySettings) and
//      brightens the whole screen with a gamma ramp of x^(1/2.2): on a modern display the movie ends up
//      low-resolution, washed out, and the near-black noise of these DivX streams is lifted into visible
//      blotches. MovieNativeMode=1 keeps the desktop mode and the gamma untouched; the movie window then
//      covers the native screen (the game already scales the picture to the window width). ----
static void PatchCallGamma(u32 at)     // call [SetDeviceGammaRamp]  ->  add esp,8 (drop its 2 arguments)
{
    uint8_t want[6] = { 0xff, 0x15 }; u32 slot = g_base + 0x86D05C; memcpy(want + 2, &slot, 4);
    static const uint8_t repl[6] = { 0x83, 0xc4, 0x08, 0x90, 0x90, 0x90 };
    if (!memcmp((void*)at, want, 6)) WriteMem(at, repl, 6);
    else logf_("movies: gamma code at %06X not recognised", at - g_base);
}
// ---- unit health bars under the console. The engine draws the simple UI textures layer by layer: the console
//      panels are BACKGROUND, a health bar's fill is ARTWORK, so bars end up on top of the console. Create the
//      fill of the unit bars (CStatBar) in the BACKGROUND layer too; only unit bars, console bars stay as they are.
typedef int (__fastcall *SBSetTex_t)(u32 self, u32 edx, u32 a1, u32 a2);
static int __fastcall StatBarSetTexture_wrap(u32 self, u32 edx, u32 a1, u32 a2)
{
    static const uint8_t bg = 0x00, art = 0x02;
    WriteMem(g_base + 0x60E692, &bg, 1);                  // CSimpleTexture(parent, layer 2 -> 0, ...)
    int r = ((SBSetTex_t)(g_base + 0x60E610))(self, edx, a1, a2);
    WriteMem(g_base + 0x60E692, &art, 1);
    return r;
}
// The simple UI is drawn level by level (depth in the frame tree), each level layer by layer, frames of one
// level in the order they were shown. Unit bars and the console are both top-level (level 0) frames and the
// console was shown first, so it is drawn under the bars. Whenever a unit bar is shown, move the other
// top-level frames behind it in that order (the engine's own register call appends at the end).
typedef int  (__fastcall *FrameShow_t)(u32 self, u32 edx);
typedef void (__fastcall *TopRegister_t)(u32 top, u32 edx, u32 frame, u32 level);
static void BarsFirst(void)
{
    u32 top = *(u32*)(g_base + 0xACE758);                      // CSimpleTop
    if (!top || IsBadReadPtr((void*)top, 0x200)) return;
    u32 others[96]; int n = 0;
    int node = *(int*)(top + 0x194);                            // level 0 list: first node
    while (node > 0 && n < 96 && !IsBadReadPtr((void*)node, 12)) {
        u32 f = *(u32*)(node + 8);
        if (f && VtRva(f) != 0x93E604 && f != *(u32*)(top + 0x16C)) others[n++] = f;   // not bars, not the mouse focus
        node = *(int*)(node + 4);
    }
    TopRegister_t reg = (TopRegister_t)(g_base + 0x60C760);
    for (int i = 0; i < n; i++) reg(top, 0, others[i], 0);
    static int logged; if (logged++ < 3) dlogf_("health bars first: %d other top-level frames moved behind", n);
}
static int __fastcall StatBarShow_hook(u32 self, u32 edx)
{
    int wasShown = *(u32*)(self + 0x94);
    int r = ((FrameShow_t)(g_base + 0x609B50))(self, edx);
    if (!wasShown && *(u32*)(self + 0x94) && *(u32*)(self + 0x84) == 0) BarsFirst();
    return r;
}
typedef u32 (__fastcall *StatBarCtor_t)(u32 self, u32 edx, u32 a1, u32 a2, u32 a3);
static u32 __fastcall StatBarCtor_wrap(u32 self, u32 edx, u32 a1, u32 a2, u32 a3)
{
    u32 r = ((StatBarCtor_t)(g_base + 0x359CC0))(self, edx, a1, a2, a3);
    if (*(u32*)(self + 0x94) && *(u32*)(self + 0x84) == 0) BarsFirst();
    static int n; if (n++ < 3) dlogf_("health bar created %08X (level %u, shown %u)", self, *(u32*)(self + 0x84), *(u32*)(self + 0x94));
    return r;
}
static void DumpSimpleLevels(u32 bar)
{
    u32 top = *(u32*)(g_base + 0xACE758);
    if (!top) return;
    logf_("bar %08X: level %u shown %u parent %08X (vt %06X)", bar, *(u32*)(bar + 0x84), *(u32*)(bar + 0x94), *(u32*)(bar + 0x6C), VtRva(*(u32*)(bar + 0x6C)));
    for (int L = 0; L < 10; L++) {
        char buf[900]; int len = 0, n = 0;
        int node = *(int*)(top + 0x194 + L * 12);
        while (node > 0 && n < 400 && !IsBadReadPtr((void*)node, 12)) {
            u32 f = *(u32*)(node + 8);
            if (len < 860) len += snprintf(buf + len, sizeof buf - len, " %06X%s", VtRva(f), f == bar ? "*" : "");
            n++; node = *(int*)(node + 4);
        }
        buf[len] = 0;
        if (n) logf_("level %d: %d frames:%s", L, n, buf);
    }
}
static void InstallHealthBarLayer(void)
{
    if (!g_hpUnder || !g_worldFull) return;
    u32 at = g_base + 0x359E4A;
    if (*(uint8_t*)at != 0xE8 || at + 5 + *(int32_t*)(at + 1) != g_base + 0x60E610 ||
        memcmp((void*)(g_base + 0x60E68F), "\x6a\x01\x6a\x02\x6a\x00", 6)) { logf_("health bar code not recognised"); g_hpUnder = 0; return; }
    uint8_t c[5] = { 0xE8 }; int32_t rel = (int32_t)((u32)StatBarSetTexture_wrap - (at + 5)); memcpy(c + 1, &rel, 4);
    WriteMem(at, c, 5);
    // right after creating a unit bar the game raises its frame level by one (0x379B58: add eax,1 before
    // SetFrameLevel) so that it is drawn after the top-level console: keep it at the console's level instead
    static const uint8_t kRaise[] = { 0x8B, 0x81, 0x84, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x01 };
    if (!memcmp((void*)(g_base + 0x379B52), kRaise, sizeof kRaise)) { static const uint8_t z = 0x00; WriteMem(g_base + 0x379B5A, &z, 1); }
    else logf_("health bar level code not recognised");
    u32 slot = g_base + 0x93E604 + 0x68;                        // CStatBar vtable: Show
    if (*(u32*)slot == g_base + 0x609B50 && !memcmp((void*)(g_base + 0x60C760), "\x56\x8b\xf1\x8b\x4c\x24\x08", 7)) {
        u32 h = (u32)StatBarShow_hook; WriteMem(slot, &h, 4);
        static const u32 kCtorCalls[] = { 0x35DFF5, 0x35E07F, 0x35F784, 0x35F811, 0x36904B, 0x3690D2, 0x369493, 0x379AFF };
        for (size_t i = 0; i < sizeof kCtorCalls / sizeof kCtorCalls[0]; i++) {
            u32 a = g_base + kCtorCalls[i];
            if (*(uint8_t*)a != 0xE8 || a + 5 + *(int32_t*)(a + 1) != g_base + 0x359CC0) { logf_("health bar call %06X not recognised", kCtorCalls[i]); continue; }
            uint8_t cc[5] = { 0xE8 }; int32_t rr = (int32_t)((u32)StatBarCtor_wrap - (a + 5)); memcpy(cc + 1, &rr, 4);
            WriteMem(a, cc, 5);
        }
    } else logf_("health bar order code not recognised");
    logf_("unit health bars drawn under the console");
}

static void InstallMovieNative(void)
{
    if (!g_movieNative) return;
    static const uint8_t kModeLoop[] = { 0xc7, 0x44, 0x24, 0x44, 0x00, 0x00, 0x5c, 0x00 };
    static const uint8_t kRestore[] = { 0x33, 0xd2, 0x32, 0xc9, 0xe8, 0xd1, 0xfd, 0xff, 0xff };
    if (memcmp((void*)(g_base + 0x52B9FF), kModeLoop, sizeof kModeLoop) || memcmp((void*)(g_base + 0x52BB56), kRestore, sizeof kRestore)) {
        logf_("movies: display mode code not recognised, native mode off"); return;
    }
    // the mode search runs after the current desktop mode has been stored as the movie size: skip the search
    uint8_t j[8] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((g_base + 0x52BAD7) - (g_base + 0x52B9FF + 5)); memcpy(j + 1, &rel, 4);
    WriteMem(g_base + 0x52B9FF, j, sizeof j);
    static const uint8_t nop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    WriteMem(g_base + 0x52BB5A, nop5, 5);            // ...and nothing to restore afterwards
    PatchCallGamma(g_base + 0x52C910);                 // movie start: x^(1/2.2) ramp
    PatchCallGamma(g_base + 0x52B850);                 // movie end: restore of the ramp saved at the start
    logf_("movies: native display mode, no gamma change");
}

// ---- cinematics in an external player (MoviePlayer in the ini), e.g. 64-bit MPC-HC: NVIDIA RTX Video
//      (HDR / super resolution) works there, and it shows these DivX movies cleanest. The game's
//      play-movie function is replaced: the movie runs in the player (fullscreen, closes at the end;
//      Esc / Enter / Space / click skip it, as in the game), then the game carries on as after its own
//      playback. If the player cannot be started, the game plays the movie itself. ----
static BOOL CALLBACK CloseProcWindows(HWND h, LPARAM pid)
{
    DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p == (DWORD)pid && IsWindowVisible(h)) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}
static BOOL PlayExternal(const char* file)
{
    WCHAR wfile[MAX_PATH], full[MAX_PATH], dir[MAX_PATH], link[MAX_PATH], exe[MAX_PATH], cmd[2048];
    if (!MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH) || !GetFullPathNameW(wfile, MAX_PATH, full, NULL)) return FALSE;
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) { logf_("movies: %s not found, playing in the game", file); return FALSE; }
    // give the player a normal extension: hard link (or copy) Movies\X.mpq -> W3TrueWidescreen_cache\X.avi
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR* sl = wcsrchr(dir, L'\\'); if (sl) *(sl + 1) = 0;
    wcscat(dir, L"W3TrueWidescreen_cache");
    CreateDirectoryW(dir, NULL);
    const WCHAR* base = wcsrchr(full, L'\\'); base = base ? base + 1 : full;
    swprintf(link, MAX_PATH, L"%ls\\%ls", dir, base);
    WCHAR* dot = wcsrchr(link, L'.'); if (dot && dot > wcsrchr(link, L'\\')) wcscpy(dot, L".avi"); else wcscat(link, L".avi");
    WIN32_FILE_ATTRIBUTE_DATA a1, a2;
    BOOL have = GetFileAttributesExW(link, GetFileExInfoStandard, &a2) && GetFileAttributesExW(full, GetFileExInfoStandard, &a1)
                && a1.nFileSizeLow == a2.nFileSizeLow && a1.nFileSizeHigh == a2.nFileSizeHigh;
    if (!have) { DeleteFileW(link); if (!CreateHardLinkW(link, full, NULL) && !CopyFileW(full, link, FALSE)) wcscpy(link, full); }
    MultiByteToWideChar(CP_ACP, 0, g_moviePlayer, -1, exe, MAX_PATH);
    swprintf(cmd, 2048, L"\"%ls\" \"%ls\" /play /fullscreen /close /new /start 0", exe, link);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    HWND game = FindGameWindow();
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        logf_("movies: could not start %s (%lu), playing in the game", g_moviePlayer, GetLastError()); return FALSE;
    }
    logf_("movies: %ls played in %s", base, g_moviePlayer);
    static const int kSkip[] = { VK_ESCAPE, VK_RETURN, VK_SPACE, VK_LBUTTON, VK_RBUTTON };
    BOOL was[5]; for (int i = 0; i < 5; i++) was[i] = (GetAsyncKeyState(kSkip[i]) & 0x8000) != 0;   // keys still held from the menu don't count
    DWORD closeAt = 0; BOOL quit = FALSE; WPARAM quitCode = 0;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 30, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        // keep the game's windows responsive, but don't hand them keyboard/mouse input meant for the movie
        MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = TRUE; quitCode = m.wParam; continue; }
            if ((m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST) || (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST)) continue;
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        DWORD fgPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        BOOL mine = fgPid == pi.dwProcessId || fgPid == GetCurrentProcessId();
        for (int i = 0; i < 5; i++) {
            BOOL down = (GetAsyncKeyState(kSkip[i]) & 0x8000) != 0;
            if (down && !was[i] && mine && !closeAt) {
                EnumWindows(CloseProcWindows, (LPARAM)pi.dwProcessId);
                closeAt = GetTickCount() | 1;
                dlogf_("movies: skipped");
            }
            was[i] = down;
        }
        if (quit || (closeAt && GetTickCount() - closeAt > 3000)) { TerminateProcess(pi.hProcess, 0); closeAt = 0; }
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    { MSG m; while (PeekMessageW(&m, NULL, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
             while (PeekMessageW(&m, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {} }
    if (game) { if (IsIconic(game)) ShowWindow(game, SW_RESTORE); SetForegroundWindow(game); }
    if (quit) PostQuitMessage((int)quitCode);
    return TRUE;
}
// 0x52C940: play a movie (edx = file, 3 stack arguments, callee cleans) = __fastcall with an unused ecx
typedef int (__attribute__((fastcall)) *PlayMovie_t)(void*, const char*, const char*, int, int);
static PlayMovie_t orig_PlayMovie;
static int __attribute__((fastcall)) PlayMovie_hook(void* c, const char* file, const char* subs, int a, int b)
{
    if (g_moviePlayer[0] && file && file[0] && PlayExternal(file)) return 0;
    return orig_PlayMovie(c, file, subs, a, b);
}

static void InstallExternalPlayer(const char* opt)
{
    if (!opt[0] || !strcmp(opt, "0")) return;
    if (!strcmp(opt, "1") || !_stricmp(opt, "auto")) {
        HKEY k; DWORD sz = sizeof g_moviePlayer, t;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\MPC-HC\\MPC-HC", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "ExePath", NULL, &t, (LPBYTE)g_moviePlayer, &sz) != ERROR_SUCCESS) g_moviePlayer[0] = 0;
            RegCloseKey(k);
        }
    } else snprintf(g_moviePlayer, sizeof g_moviePlayer, "%s", opt);
    if (!g_moviePlayer[0] || GetFileAttributesA(g_moviePlayer) == INVALID_FILE_ATTRIBUTES) {
        logf_("movies: external player not found (%s)", g_moviePlayer[0] ? g_moviePlayer : "MPC-HC not installed");
        g_moviePlayer[0] = 0; return;
    }
    // 0x526F42: call 0x52C940 (play movie)  ->  call PlayMovie_hook
    u32 at = g_base + 0x526F42;
    if (*(uint8_t*)at != 0xe8 || at + 5 + *(int32_t*)(at + 1) != g_base + 0x52C940) {
        logf_("movies: movie code not recognised, external player off"); g_moviePlayer[0] = 0; return;
    }
    orig_PlayMovie = (PlayMovie_t)(g_base + 0x52C940);
    uint8_t c[5] = { 0xe8 }; int32_t rel = (int32_t)((u32)PlayMovie_hook - (at + 5)); memcpy(c + 1, &rel, 4);
    WriteMem(at, c, 5);
    logf_("movies: external player %s", g_moviePlayer);
}

// GameMain starts with 0x00BAB0 (called at 0x00986C), Blizzard's anti-hack: it replaces the process DACL with a
// single ACE that denies Everyone 0xF01FFFFE. The generic rights in that mask map to all process rights, so no
// other program can even read the exe path (PROCESS_QUERY_LIMITED_INFORMATION), and NVIDIA App loses per-game
// settings such as RTX HDR (it looks them up by the exe path and restores zeros at launch). Keep the protection,
// allow only that query: memory access, injection, handle duplication etc. stay denied. The deny mask must list
// specific rights only, a generic bit would cover the query again.
static int __cdecl ProtectProcess_hook(void)
{
    SID_IDENTIFIER_AUTHORITY world = { SECURITY_WORLD_SID_AUTHORITY };
    PSID everyone = NULL;
    union { ACL acl; BYTE b[0x200]; } buf;
    DWORD err = ERROR_INVALID_FUNCTION;
    if (!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone)) return 0;
    if (InitializeAcl(&buf.acl, sizeof buf, ACL_REVISION)
        && AddAccessDeniedAce(&buf.acl, ACL_REVISION, 0x001FFFFF & ~PROCESS_QUERY_LIMITED_INFORMATION, everyone)
        && AddAccessAllowedAce(&buf.acl, ACL_REVISION, PROCESS_QUERY_LIMITED_INFORMATION, everyone))
        err = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, &buf.acl, NULL);
    FreeSid(everyone);
    if (err != ERROR_SUCCESS) logf_("process protection: SetSecurityInfo failed (%lu)", err);
    else dlogf_("process protection: exe path readable by other programs");
    return err == ERROR_SUCCESS;
}

static void InstallPathQuery(void)
{
    if (!g_pathQuery) return;
    u32 at = g_base + 0x00986C;   // call 0x00BAB0 in GameMain; usually done already (Miles loads the .mix later)
    if (*(uint8_t*)at == 0xe8 && at + 5 + *(int32_t*)(at + 1) == g_base + 0x00BAB0) {
        uint8_t c[5] = { 0xe8 }; int32_t rel = (int32_t)((u32)ProtectProcess_hook - (at + 5)); memcpy(c + 1, &rel, 4);
        WriteMem(at, c, 5);
    }
    ProtectProcess_hook();
}

// ---- frame rate limit. The engine draws a frame whenever GetTickCount() changes (0x62D710 fires the game tick
//      and flags a render, 0x62D7D0 sends the render event 0x11). Windows moves that counter every 15.625 ms, which
//      is where the classic 64 fps cap comes from: nothing in the game asks for 64, and its own 'maxfps' setting
//      changes nothing. FpsLimit=N gives Game.dll a 1 ms GetTickCount (performance counter), keeps the tick gate
//      on the real counter so the game tick and everything stepped per tick stay exactly as before, and raises
//      the render flag on its own schedule, 1000/N ms apart. ----
#define RVA_RenderEvent 0x62D7D0           // 53 57 8d 7e 10: push ebx; push edi; lea edi,[esi+10]
u32 g_renderTr;
static LARGE_INTEGER g_lastFrame; static double g_qpcToMs, g_minFrameMs;
// GetTickCount itself only moves every 15.625 ms on current Windows, timeBeginPeriod or not, so give Game.dll a
// 1 ms version: the system counter plus the performance counter's progress since we started. Never behind the
// real counter, never going backwards; other modules keep calling the real one through their own import tables.
#define IAT_GetTickCount 0x86D230
typedef DWORD (WINAPI *GetTickCount_t)(void);
static GetTickCount_t g_realGTC;
static DWORD g_tickBase, g_lastHires; static LARGE_INTEGER g_qpcBase;
#define RVA_TickGateRet 0x62D73A           // return address of the GetTickCount call in the tick gate 0x62D710
static DWORD WINAPI GetTickCount_hires(void)
{
    // the tick gate (0x62D710) fires the game tick whenever this value changes: keep it on the real 15.625 ms
    // counter so the tick rate (and everything stepped per tick) stays exactly as in the original game
    if ((u32)__builtin_return_address(0) - g_base == RVA_TickGateRet) return g_realGTC();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    DWORD v = g_tickBase + (DWORD)((double)(c.QuadPart - g_qpcBase.QuadPart) * g_qpcToMs);
    DWORD real = g_realGTC();
    if ((int)(v - real) < 0) v = real;
    if ((int)(v - g_lastHires) < 0) v = g_lastHires;
    g_lastHires = v;
    return v;
}
static void InstallHiresTicks(void)
{
    u32 slot = g_base + IAT_GetTickCount;
    g_realGTC = *(GetTickCount_t*)slot;
    if (!g_realGTC) { logf_("fps limit: GetTickCount import not found"); return; }
    g_tickBase = g_realGTC(); QueryPerformanceCounter(&g_qpcBase); g_lastHires = g_tickBase;
    void* h = (void*)GetTickCount_hires; WriteMem(slot, &h, 4);
    logf_("fps limit: 1 ms GetTickCount for Game.dll (base %lu)", g_tickBase);
}
static void RemoveHiresTicks(void)
{
    if (!g_realGTC) return;
    void* r = (void*)g_realGTC; WriteMem(g_base + IAT_GetTickCount, &r, 4); g_realGTC = NULL;
}
__attribute__((used, cdecl)) void RenderGate(u32 self)
{
    // self = the frame object; bit 4 of +0x44 = "render pending", which the original dispatcher consumes.
    // The tick sets it 64 times a second; we take it over: raise it on our schedule, drop it when too soon.
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    u32* flags = (u32*)(self + 0x44);
    double el = g_lastFrame.QuadPart ? (double)(c.QuadPart - g_lastFrame.QuadPart) * g_qpcToMs : 1e9;
    if (el >= g_minFrameMs) {
        *flags |= 4;
        if (el > 2.0 * g_minFrameMs) g_lastFrame = c;                       // way behind: resync
        else g_lastFrame.QuadPart += (LONGLONG)(g_minFrameMs / g_qpcToMs); // on time: step without drift
    } else *flags &= ~4u;
}
static void __attribute__((naked)) RenderEvent_det(void)
{
    asm volatile(
        "pushal\n" "push %%esi\n" "call _RenderGate\n" "add $4, %%esp\n" "popal\n"
        "jmp *_g_renderTr\n" ::: "memory");
}
static void InstallFpsLimit(void)
{
    if (!g_fpsLimit) return;
    u32 a = g_base + RVA_RenderEvent;
    if (memcmp((void*)a, "\x53\x57\x8d\x7e\x10", 5) != 0) { logf_("fps limit: render event code not recognised, off"); return; }
    LARGE_INTEGER f; if (!QueryPerformanceFrequency(&f) || !f.QuadPart) { logf_("fps limit: no performance counter, off"); return; }
    g_qpcToMs = 1000.0 / (double)f.QuadPart;
    g_minFrameMs = 1000.0 / (double)g_fpsLimit;
    g_renderTr = MakeTrampoline(a, 5);
    if (!g_renderTr) { logf_("fps limit: trampoline failed, off"); return; }
    WriteJmp(a, (u32)RenderEvent_det);
    InstallHiresTicks();
    logf_("fps limit: %d fps (min frame %.3f ms)", g_fpsLimit, g_minFrameMs);
}

static void Install(void)
{
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *(s + 1) = 0;
    strcat(path, "W3TrueWidescreen.log");
    g_log = fopen(path, "w");

    g_base = (u32)GetModuleHandleA("Game.dll");
    u32 build = GetGameBuild();
    logf_("W3TrueWidescreen 1.5.1  Game.dll build %u", build);
    if (!g_base || build != 6401) { logf_("unsupported game version, doing nothing (need 1.26a / 6401)"); return; }

    char src[128];
    double aspect = ReadAspect(src, sizeof src);
    logf_("aspect %.4f (%s)", aspect, src);
    InstallPathQuery();
    if (aspect < 1.34) { logf_("aspect is 4:3 or narrower, nothing to do"); return; }

    g_uiW_d = 0.6 * aspect;
    g_uiW_f = (float)g_uiW_d;
    g_e = (float)((g_uiW_d - 0.8) * 0.5);
    dlogf_("UI width %.4f, HUD shift %.4f", g_uiW_d, g_e);

    // 1) UI <-> screen conversions use our width
    u32 pd = (u32)&g_uiW_d;
    for (size_t i = 0; i < sizeof kDoubleSites / sizeof kDoubleSites[0]; i++) {
        u32 a = g_base + kDoubleSites[i];
        u32 cur = *(u32*)a;
        if (cur != g_base + 0x8768B0) { logf_("site %06X unexpected operand %08X, abort", kDoubleSites[i], cur); return; }
    }
    if (*(uint16_t*)(g_base + RVA_OrthoFld) != 0x05D9 || *(u32*)(g_base + RVA_OrthoFld + 2) != g_base + 0x93A8E8) { logf_("ortho site mismatch, abort"); return; }
    if (memcmp((void*)(g_base + RVA_SetFramePoint), "\x53\x8B\x5C\x24\x08", 5) != 0) { logf_("SetFramePoint prologue mismatch, abort"); return; }
    if (memcmp((void*)(g_base + RVA_SetAllPoints), "\x56\x57\x8B\xF1\xE8", 5) != 0) { logf_("SetAllPoints prologue mismatch, abort"); return; }

    for (size_t i = 0; i < sizeof kDoubleSites / sizeof kDoubleSites[0]; i++)
        WriteMem(g_base + kDoubleSites[i], &pd, 4);

    // 2) UI ortho projection: right edge = our width
    u32 pf = (u32)&g_uiW_f;
    WriteMem(g_base + RVA_OrthoFld + 2, &pf, 4);
    // 2b) map-click gate: let clicks reach the world in the strip beside the top bar (WorldFullHeight)
    if (*(uint16_t*)(g_base + RVA_PickTopFld) == 0x05D9 && *(u32*)(g_base + RVA_PickTopFld + 2) == g_base + 0x941548) {
        g_pickTop = g_worldFull ? 1.0f : 0.577f;
        u32 pp = (u32)&g_pickTop;
        WriteMem(g_base + RVA_PickTopFld + 2, &pp, 4);
        logf_("map-click gate: top %.3f", g_pickTop);
    } else logf_("map-click gate code not recognised, left as is");

    // 3) keep HUD frames in the centered 4:3 area
    ClearAllPoints = (ClearAllPoints_t)(g_base + RVA_ClearAllPoints);
    orig_SetFramePoint = (SetFramePoint_t)MakeTrampoline(g_base + RVA_SetFramePoint, 5);
    if (!orig_SetFramePoint) { logf_("trampoline alloc failed"); return; }
    WriteJmp(g_base + RVA_SetFramePoint, (u32)SetFramePoint_hook);
    WriteJmp(g_base + RVA_SetAllPoints, (u32)SetAllPoints_hook);
    if (memcmp((void*)(g_base + RVA_SetAbsPoint), "\x53\x8B\x5C\x24\x08", 5) == 0) {
        orig_SetAbsPoint = (SetAbsPoint_t)MakeTrampoline(g_base + RVA_SetAbsPoint, 5);
        if (orig_SetAbsPoint) WriteJmp(g_base + RVA_SetAbsPoint, (u32)SetAbsPoint_hook);
    }

    if (memcmp((void*)(g_base + RVA_Perspective), "\x51\xD9\x44\x24\x0C", 5) == 0) {
        orig_Persp = (Persp_t)MakeTrampoline(g_base + RVA_Perspective, 5);
        if (orig_Persp) WriteJmp(g_base + RVA_Perspective, (u32)Persp_hook);
        if (!orig_Persp) logf_("perspective hook failed");
    } else logf_("perspective code not recognised, field-of-view fix off");
    if (memcmp((void*)(g_base + RVA_RenderWorld), "\x53\x56\x8B\xF1\x8B\x8E\x38\x03\x00\x00", 10) == 0) {
        orig_RenderWorld = (RenderWorld_t)MakeTrampoline(g_base + RVA_RenderWorld, 10);
        if (orig_RenderWorld) WriteJmp(g_base + RVA_RenderWorld, (u32)RenderWorld_hook);
    } else logf_("world render code not recognised, console backing off");
    InstallFadeFix();
    InstallHealthBarLayer();
    InstallMovieNative();
    InstallMovieRenderer();
    InstallExternalPlayer(g_moviePlayerOpt);
    InstallFpsLimit();
    g_enabled = 1;
    logf_("active");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_DETACH) RemoveHiresTicks();
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Install();
    }
    return TRUE;
}
