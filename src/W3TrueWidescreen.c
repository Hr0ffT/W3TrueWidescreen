// W3TrueWidescreen.mix - unstretched widescreen for Warcraft III 1.26a (Game.dll build 6401), 1.27b (build 7085) and
// 1.28.5 (build 7680)
// Copyright (c) 2026 Hr0ffT - MIT License - https://github.com/Hr0ffT
//
// The engine lays out its UI in a virtual 0.8 x 0.6 screen and stretches it to the monitor.
// This mod widens that space to (0.6 * aspect) x 0.6, keeps the in-game console in the centred 4:3
// area, moves the menus to the screen edges, lets the world fill the whole screen, and restores the
// original 4:3 vertical field of view for the 3D menus and the game world.
//
// Build: i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -s -o W3TrueWidescreen.mix W3TrueWidescreen.c -lversion -lgdi32 -ladvapi32
#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
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
static int    g_dotaOrig = 0;    // DotAOriginal=1: DracoL1ch's DotA Allstars maps run as without the mod (see SetDotAMode)
static int    g_dotaMode = 0;    // 1 while such a map is loaded / played
static int    g_dotaLoading = 0; // its loading screen is up: shown stretched over the whole screen
static int    g_dotaRun = 0;     // such a map is being played with the mod on (DotAOriginal=0): 3D view left to the map
static FILE*  g_log = NULL;
static int    g_logCount = 0;
static int    g_debug = 0;        // Debug=1 in the ini: verbose log
static int    g_fpsLimit = 0;
static float  g_uiScale = 1.0f;   // UIScale (percent in the ini): size of the whole interface on screen
static double g_uiH_d = 0.6;      // virtual UI screen height (0.6 / g_uiScale)
static float  g_uiH_f = 0.6f;
static float  g_f = 0.0f;
static double g_aspect = 4.0 / 3.0;
static int    g_scaleOn = 0, g_scaleGameSeen = 0;
static u32    g_screenRoot = 0;
static int    g_narrowOn = 0;   // MenuLayout=0 and the menus are up: interface 0.8 wide (see SetGlueNarrow)
static int    g_loadFull = 0;   // LoadingFullScreen=1: loading screen pictures stretched over the whole screen
static int    g_scoreMode = 3;  // ScoreScreen: 1 = 4:3 with black bars, 2 = sheet extended to both edges, 3 = sheet stretched below its header         // vertical shift for top-anchored absolute positions (g_uiH - 0.6)     // FpsLimit: 0 = leave the frame rate alone (see InstallFpsLimit)
static int    g_pathQuery = 1;    // AllowPathQuery: let other programs read the exe path (see ProtectProcess_hook)
#define dlogf_(...) do { if (g_debug) logf_(__VA_ARGS__); } while (0)
static int    g_checkedWindow = 0;

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// --- game offsets: one table per supported Game.dll build ---
#define OFS_UI_Frame        0xB4
#define OFS_UI_World        0x3BC
#define OFS_UI_MouseBorders 0x420

typedef struct { u32 rva; int len; const char* bytes; } Code;   // code expected at rva (len bytes)
typedef struct {
    u32 build; const char* name;
    int sse;                         // 1.27: UI constants are floats (SSE code), 1.26: doubles (x87)
    // frames
    Code setFramePoint, setAllPoints, setAbsPoint;
    u32 clearAllPoints, setWidth, setHeight, screenRootSlot1;
    // the UI width / height operands
    u32 widthSites[16], widthConst;  // operands (addresses) pointing at the 0.8 constant: "UI width"
    u32 heightSites[12], heightConst; // the same for 0.6: "UI height" (UIScale)
    int orthoImm;                    // 1: ortho edges are immediates (mov [esp+n], 0.8f), 0: fld [const] operands
    u32 orthoW, orthoWConst, orthoH, orthoHConst;
    u32 pickTop, pickTopConst;       // map-click gate operand (0.577f)
    // classes (vtables)
    u32 vtGameUI, vtGameUIFrame, vtSimpleTop, vtCursor, vtSprite, vtLoading, vtScore, vtHeroBar, vtCinePanel,
        vtTimeOfDay, vtStatBar, vtSimpleFrame, vtFramePointRel;
    // return addresses
    u32 retInventory, retMenu[3], retGlueTopRight;
    // world view, projection, rendering
    Code persp, renderWorld;
    u32 simpleTop, gx, gxDevice, gxWidth;   // gxDevice / gxWidth 0: found at runtime
    // health bars
    u32 statBarCtor, sbSetTexCall, sbSetTex, sbLayerPush, ctorCalls[8], frameShow;
    Code topRegister, barRaise; u32 barRaiseAt; uint8_t barRaiseOn, barRaiseOff;
    // cinematics
    Code movieSize, moviePos, modeLoop, modeRestore;
    const char* movieSizeNew; u32 modeLoopTo, gammaStart, gammaEnd, playCall, playFn; int playArgs;
    u32 movieW, movieH, subY0, subCount, subIdx, subTab;
    // frame rate limit
    Code renderEvent; int renderSelfEcx; u32 tickGateRet;
    // anti-hack process protection (0: none in this build)
    u32 protectCall, protectFn;
    int dota;                        // DracoL1ch's DotA helpers exist for this build
    Code worldPick;                  // the world's pick under a UI point (x, y): where clicks reach the world
    Code enableSelect;               // CGameUI::EnableSelect(select, selectUI): what the native EnableSelect calls
} GameAddrs;

static const GameAddrs kGames[] = {
  { 6401, "1.26a", 0,
    { 0x606770, 5, "\x53\x8B\x5C\x24\x08" }, { 0x6067F0, 5, "\x56\x57\x8B\xF1\xE8" }, { 0x6061B0, 5, "\x53\x8B\x5C\x24\x08" },
    0x606160, 0x605D90, 0x605DB0, 0x60A140,
    { 0x4C648A, 0x4C64B4, 0x4C64EA, 0x4C6514, 0x4C6546, 0x4C6586,     // UI <-> normalized conversions
      0x33CE33, 0x55D9FD, 0x57136A }, 0x8768B0,                        // tooltip right-edge clamps
    { 0x4C649A, 0x4C64C9, 0x4C64FA, 0x4C6529, 0x4C6566, 0x4C65A6 }, 0x93C218,
    0, 0x7AE568, 0x93A8E8, 0x7AE55A, 0x93D84C,
    0x39799E, 0x941548,
    0x93631C, 0x9362F4, 0x96E2D0, 0x970610, 0x96E558, 0x96662C, 0x967460, 0x93F974, 0x93C794,
    0x93FE38, 0x93E604, 0x96DFB4, 0x97076C,
    0x37161A, { 0x5B8045, 0x5B8205, 0x5B83AB }, 0x57BB33,
    { 0x7B66F0, 5, "\x51\xD9\x44\x24\x0C" }, { 0x395900, 10, "\x53\x56\x8B\xF1\x8B\x8E\x38\x03\x00\x00" },
    0xACE758, 0xACBD40, 0x584, 0x9C,
    0x359CC0, 0x359E4A, 0x60E610, 0x60E68F,
    { 0x35DFF5, 0x35E07F, 0x35F784, 0x35F811, 0x36904B, 0x3690D2, 0x369493, 0x379AFF }, 0x609B50,
    { 0x60C760, 7, "\x56\x8b\xf1\x8b\x4c\x24\x08" }, { 0x379B52, 9, "\x8B\x81\x84\x00\x00\x00\x83\xC0\x01" }, 0x379B5A, 0x01, 0x00,
    { 0x52B55E, 42, "\x8b\x08\x8d\x54\x24\x0c\x52\x50\x8b\x41\x28\xff\xd0\x8b\x04\x24\x8b\x08\x8d\x54\x24\x08\x52\x50\x8b\x41\x2c\xff\xd0\x8b\x04\x24\x8b\x08\x8b\x51\x08\x50\xff\xd2\xeb\x10" },
    { 0x52B5D3, 9, "\x8b\x87\x9c\x00\x00\x00\x52\xff\xd0" },
    { 0x52B9FF, 8, "\xc7\x44\x24\x44\x00\x00\x5c\x00" }, { 0x52BB56, 9, "\x33\xd2\x32\xc9\xe8\xd1\xfd\xff\xff" },
    "\x8b\x08"                          // mov  ecx, [eax]
    "\x8d\x54\x24\x08\x52"              // lea  edx, [esp+8]  (height) ; push edx
    "\x8d\x54\x24\x10\x52"              // lea  edx, [esp+10h] (width) ; push edx
    "\x50"                              // push eax
    "\xff\x91\x88\x00\x00\x00"          // call [ecx+88h]  IBasicVideo::GetVideoSize
    "\x8b\x04\x24\x8b\x08\x8b\x51\x08\x50\xff\xd2"   // pBV->Release()
    "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
    "\xeb\x10",                         // jmp to the window placement code (unchanged)
    0x52BAD7, 0x52C910, 0x52B850, 0x526F42, 0x52C940, 3,
    0xACC698, 0xACC69C, 0xACC088, 0xACC6BC, 0xACC6C0, 0xACC6D0,
    { 0x62D7D0, 5, "\x53\x57\x8d\x7e\x10" }, 0, 0x62D73A,
    0x00986C, 0x00BAB0,
    1,
    { 0x397900, 6, "\x83\xEC\x1C\xD9\xEE\x56" },
    { 0x2F9290, 7, "\x8B\x44\x24\x04\x85\xC0\x56" } },
  { 7085, "1.27b", 1,
    { 0x111600, 7, "\x55\x8B\xEC\x53\x8B\x5D\x08" }, { 0x1114B0, 5, "\x55\x8B\xEC\x56\x57" }, { 0x111590, 7, "\x55\x8B\xEC\x53\x8B\x5D\x08" },
    0x110A00, 0x1116C0, 0x111520, 0x11DFF0,
    { 0x8B37A8, 0x8B37B9, 0x8B3800, 0x8B3848, 0x8B3858, 0x8B3869, 0x8B38B0, 0x8B38F8,
      0x3C1A4D, 0x2B5877, 0x302383 }, 0xAA9328,
    { 0x8B37CB, 0x8B37DD, 0x8B3815, 0x8B3838, 0x8B387B, 0x8B388D, 0x8B38C5, 0x8B38E8 }, 0xAA9324,
    1, 0x193556, 0x3F4CCCCD, 0x193546, 0x3F19999A,
    0x38418E, 0xABB988,
    0xAB9D90, 0xAB9E78, 0xA8D1BC, 0xA8DB04, 0xA8B304, 0xAB13C8, 0xAB2730, 0xABE7FC, 0xABD8C4,
    0xAC1F48, 0xABCE78, 0xA8D204, 0xA8DDD0,
    0x39E94F, { 0x2FB129, 0x2F0BE8, 0x2E81EF }, 0x2E0CC8,
    { 0x126E30, 6, "\x55\x8B\xEC\x83\xE4\xF8" }, { 0x385950, 5, "\x56\x8B\xF1\x33\xD2" },
    0xD47B68, 0xD52EB8, 0, 0,
    0x3A16E0, 0x3A18D7, 0x10FE90, 0x10FF07,
    { 0x3926BF, 0x39C69E, 0x39C724, 0x39E361, 0x39E3E2, 0x39E522, 0x39F051, 0x39F0D7 }, 0x10DE60,
    { 0x10C1D0, 10, "\x55\x8b\xec\x83\xec\x08\x53\x8b\x5d\x08" }, { 0x39270F, 7, "\x8B\x81\x84\x00\x00\x00\x40" }, 0x392715, 0x40, 0x90,
    { 0x13D361, 34, "\x8b\x01\x8d\x55\xcc\x52\x51\xff\x50\x28\x8b\x45\xd8\x8d\x55\xd0\x52\x50\x8b\x08\xff\x51\x2c\x8b\x45\xd8\x50\x8b\x08\xff\x51\x08\xeb\x0e" },
    { 0x13D3D3, 8, "\x8b\x11\xff\x92\x9c\x00\x00\x00" },
    { 0x13C47D, 7, "\xc7\x45\x88\x00\x00\x5c\x00" }, { 0x13D766, 9, "\x33\xd2\x32\xc9\xe8\x31\xec\xff\xff" },
    "\x8b\x01"                          // mov  eax, [ecx]
    "\x8d\x55\xd0\x52"                  // lea  edx, [ebp-30h] (height) ; push edx
    "\x8d\x55\xcc\x52"                  // lea  edx, [ebp-34h] (width) ; push edx
    "\x51"                              // push ecx
    "\xff\x90\x88\x00\x00\x00"          // call [eax+88h]  IBasicVideo::GetVideoSize
    "\x8b\x45\xd8\x50\x8b\x08\xff\x51\x08"           // pBV->Release()
    "\x90\x90\x90\x90\x90\x90"
    "\xeb\x0e",                         // jmp to the window placement code (unchanged)
    0x13C526, 0x13D72C, 0x13CE93, 0x136A40, 0x13CB50, 5,
    0, 0, 0xD53760, 0xD52F1C, 0xD52F20, 0xD52F30,
    { 0x07B780, 5, "\x53\x56\x57\x8b\xf9" }, 1, 0x07B6ED,
    0, 0,
    0,
    { 0x3840E0, 6, "\x55\x8B\xEC\x83\xEC\x18" },
    { 0x36C2F0, 9, "\x55\x8B\xEC\x8B\x55\x08\x8B\x45\x0C" } },
  { 7680, "1.28.5", 1,
    { 0x13FC20, 7, "\x55\x8B\xEC\x53\x8B\x5D\x08" }, { 0x13FAD0, 5, "\x55\x8B\xEC\x56\x57" }, { 0x13FBB0, 7, "\x55\x8B\xEC\x53\x8B\x5D\x08" },
    0x13F020, 0x13FCE0, 0x13FB40, 0x14C6A0,
    { 0x867F18, 0x867F29, 0x867F70, 0x867FB8, 0x867FD8, 0x867FE9, 0x868030, 0x868078,
      0x3F5B7D, 0x2E80C7, 0x335923,
      0x13EBA1, 0x13EBA9, 0x13EF50 }, 0xA7CF34,     // (1.28.5 only: UI -> normalized copies near the frame code; the
                                                    //  minimap's mouse position among them)
    { 0x867F3B, 0x867F4D, 0x867F85, 0x867FA8, 0x867FFB, 0x86800D, 0x868045, 0x868068,
      0x13EB89, 0x13EBBB, 0x13EF65 }, 0xA7CF30,
    1, 0x1C5DF6, 0x3F4CCCCD, 0x1C5DE6, 0x3F19999A,
    0x3B829E, 0xAAC330,
    0xAAA730, 0xAAA818, 0xA7CBCC, 0xA7D51C, 0xA7AD14, 0xAA14D0, 0xAA298C, 0xAAF1C4, 0xAAE290,
    0xAB2980, 0xAAD83C, 0xA7CC14, 0xA7D7E0,
    0x3D2A5F, { 0x32E6F9, 0x323C38, 0x31B39F }, 0x313CB8,
    { 0x1554F0, 6, "\x55\x8B\xEC\x83\xE4\xF8" }, { 0x3B9A60, 5, "\x56\x8B\xF1\x33\xD2" },
    0xD0F948, 0xD1ACA8, 0, 0,
    0x3D57F0, 0x3D59E7, 0x13E3C0, 0x13E437,
    { 0x3C67CF, 0x3D07AE, 0x3D0834, 0x3D2471, 0x3D24F2, 0x3D2632, 0x3D3161, 0x3D31E7 }, 0x13C390,
    { 0x13A700, 10, "\x55\x8b\xec\x83\xec\x08\x53\x8b\x5d\x08" }, { 0x3C681F, 7, "\x8B\x81\x84\x00\x00\x00\x40" }, 0x3C6825, 0x40, 0x90,
    // 1.28 reads the size with get_VideoWidth / get_VideoHeight, keeps a default for 0 and crops 8 lines itself
    // (put_SourceHeight): only the two reads are replaced, by GetVideoSize
    { 0x16DF35, 23, "\x8b\x01\x8d\x55\xd8\x52\x51\xff\x50\x28\x8b\x45\xd4\x8d\x55\xd0\x52\x50\x8b\x08\xff\x51\x2c" },
    { 0x16DFF2, 6, "\xff\x96\x9c\x00\x00\x00" },
    { 0x16D02D, 7, "\xc7\x45\x88\x00\x00\x5c\x00" }, { 0x16E3D6, 9, "\x33\xd2\x32\xc9\xe8\x71\xeb\xff\xff" },
    "\x8b\x01"                          // mov  eax, [ecx]
    "\x8d\x55\xd0\x52"                  // lea  edx, [ebp-30h] (height) ; push edx
    "\x8d\x55\xd8\x52"                  // lea  edx, [ebp-28h] (width) ; push edx
    "\x51"                              // push ecx
    "\xff\x90\x88\x00\x00\x00"          // call [eax+88h]  IBasicVideo::GetVideoSize
    "\x90\x90\x90\x90\x90\x90",
    0x16D0D6, 0x16E39B, 0x16DA43, 0x166759, 0x16D770, 5,
    0, 0, 0xD1B554, 0xD1AD10, 0xD1AD14, 0xD1AD24,
    { 0x0A5090, 5, "\x53\x56\x57\x8b\xf9" }, 1, 0x0A4FFD,
    0, 0,
    0,
    { 0x3B81F0, 6, "\x55\x8B\xEC\x83\xEC\x18" },
    { 0x3A0340, 9, "\x55\x8B\xEC\x8B\x55\x08\x8B\x45\x0C" } },
};
static const GameAddrs* G;           // the running build
static int BuildIdx(void) { return G->build == 6401 ? 0 : G->build == 7085 ? 1 : 2; }   // row in the per-build tables
#define VT(name) (G->vt##name)
static int CodeIs(const Code* c) { return c->rva && !memcmp((void*)(g_base + c->rva), c->bytes, c->len); }

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
    if (*(u32*)obj != g_base + VT(GameUI)) return 0;
    g_gameUI = obj;
    return 1;
}
// CGameUI+0x180: interface state (0/4 shown, 1 hiding, 3 hidden = cinematic mode, 2 showing again)
static int InCinematicMode(void)
{
    u32 ui = g_gameUI;
    if (!ui || IsBadReadPtr((void*)ui, 0x184) || *(u32*)ui != g_base + VT(GameUI)) return 0;
    u32 st = *(u32*)(ui + 0x180);
    return st == 1 || st == 3;
}

static int g_glue = 1;

// any full-screen root frame (game UI root, glue/menu screens, loading screen):
// its layout vtable slot 1 recomputes the rect as the whole screen
static int IsScreenRoot(u32 parent)
{
    if (!parent || IsBadReadPtr((void*)parent, 4)) return 0;
    u32 vt = *(u32*)parent;
    if (!vt || IsBadReadPtr((void*)vt, 8)) return 0;
    if (*(u32*)(vt + 4) != g_base + G->screenRootSlot1) return 0;
    g_screenRoot = parent;
    return 1;
}

static u32 VtRva(u32 p)
{
    if (!p || IsBadReadPtr((void*)p, 4)) return 0;
    u32 vt = *(u32*)p;
    return vt >= g_base ? vt - g_base : vt;
}

// frame classes that host other frames' rendering (simple-frame renderer, cursor/tooltip layer):
// they must stay full screen, otherwise everything they draw is translated a second time
static u32 g_keepVt[16];
static int g_keepVtN = 2;          // [0], [1]: CSimpleTop, CCursorFrame (set at install)

static int IsKeepClass(u32 frame)
{
    u32 vt = VtRva(frame);
    for (int i = 0; i < g_keepVtN; i++) if (vt == g_keepVt[i]) return 1;
    return 0;
}

// menus: 3D backgrounds (CSpriteFrame) fill the whole screen, the menu panels stay centered
static double g_menuAspectOpt = 0.0; static float g_menuCut = 0.0f;
static void MenuClipPoll(void);
static void MenuClipAddFrame(u32 layout);
static int IsNarrowed(u32 f);
static void SfWrapVtable(u32 vt, const char* what);
static int g_loadingActive, g_scoreActive, g_customLoadPic; static u32 g_loadMinimap, g_loadMinimapParent, g_loadBar[2], g_loadBarParent[2]; static float g_loadBarX[2], g_loadBarY[2];  // MenuAspect: how far the menus' screen edges move in from the screen edges on each side
static int g_menuBgFull = 1;   // 0: 3D menu backgrounds stretched as in the original game
static int g_menuEdges = 3;   // MenuLayout: 1 = menu panels at the screen edges, 2 = also widened towards the centre,
                              // 3 = as 2, but the custom game screen's contents stay centred in its panels
static int g_menuWide = 0;    // MenuLayout=2 in effect (wide screen) // MenuLayout=2: how far the left / right panels' inner edges and
                                                // their contents move towards the centre
// MenuLayout=2: the screens whose panels are widened (CSkirmish, CLoadSavedGameScreen, CLocalMultiplayerJoin /
// Create / Load, CCustomCampaignMenu, CViewReplayScreen); frames placed on them move with the panels' inner edges
// (last: CGameChatroom, the game lobby, whose contents otherwise stay at the panels' outer edges)
static const u32 kWideScreens[][8] = {
    { 0x965534, 0x964DFC, 0x962A44, 0x962924, 0x962BC8, 0x967A60, 0x965644, 0x9624D4 },   // 1.26a
    { 0xAB2058, 0xAB1EF8, 0xAB21B8, 0xAB2318, 0xAB247C, 0xAB1D98, 0xAB1C2C, 0xAB0CA8 },   // 1.27b
    { 0xAA2294, 0xAA2134, 0xAA23F8, 0xAA2560, 0xAA26C8, 0xAA1FD0, 0xAA1E5C, 0xAA0D94 },   // 1.28.5
};
static float MenuPanelMove(float in, int right);
static int WideScreen(u32 parent)     // 0: no; 1: skirmish; 2: the others; 3: the game lobby
{
    u32 vt = VtRva(parent);
    const u32* t = kWideScreens[BuildIdx()];
    for (int i = 0; i < 8; i++) if (vt == t[i]) return i == 0 ? 1 : i == 7 ? 3 : 2;
    return 0;
}
static int IsKeepClassGlue(u32 frame)
{
    if (IsKeepClass(frame)) return 1;
    u32 vt = VtRva(frame);
    if (vt == VT(Sprite)) return 1;                               // CSpriteFrame: 3D backgrounds
    // screens drawn as one fixed 4:3 picture stay centred: CLoading (loading screen), CScoreScreen (mission score)
    // (CBattleNetMain, the Battle.net login, spans the screen: its doors are widened, see ServeBnetDoors)
    if (g_menuEdges && vt != VT(Loading) && vt != VT(Score)) return 1;
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
static float g_pickTop = 0.577f;

static int g_heroEdge = 1;     // HeroBarEdge=1: hero portraits (top-left) hug the left screen edge
static int g_cineFull = 1;     // CinematicFullWidth=1: cinematic letterbox panel spans the full width
static double g_cineAspectOpt = 0.0;   // CinematicAspect: the speaker's portrait, name and text stay in a centred area of this shape
static int IsFullWidthFrame(u32 frame, u32 ui)
{
    if (IsKeepClass(frame)) return 1;
    if (g_heroEdge && VtRva(frame) == VT(HeroBar)) return 1;      // CHeroBar
    if (g_cineFull && VtRva(frame) == VT(CinePanel)) return 1;      // CCinematicPanel: letterbox spans the whole width
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
        if (fabs(g_uiH_d * wa - g_uiW_d) > 0.01)        // (UIScale: the virtual screen is taller than 0.6)
            logf_("WARNING: window is %ldx%ld (aspect %.4f) but the mod uses aspect %.4f - set Width/Height in W3TrueWidescreen.ini",
                  r.right, r.bottom, wa, g_uiW_d / g_uiH_d);
        else dlogf_("window client %ldx%ld ok", r.right, r.bottom);
    }
}

static int g_consoleBacking = 1;
// occupancy marks (COccupUI) above burrows and gold mines: under the console like the health bars
static const u32 kOccupVt[3] = { 0x93D06C, 0xAC085C, 0xAB1270 };
static int g_occupUnder;
static int IsOccupTree(u32 f);
static void OccupUnder(u32 ui);
typedef int (__fastcall *FrameShow_t)(u32 self, u32 edx);
static int g_hpUnder = 1;         // HealthBarsUnderConsole=1: unit health bars drawn under the console panels   // ConsoleBacking=0: do not blacken the console area behind the world (diagnostics)

static void DotAFrameEvent(u32 frame, u32 parent);
static int FromGame(u32 ret);
static void __fastcall SetFramePoint_hook(u32 frame, u32 edx, u32 point, u32 parent, u32 rel, float x, float y, u32 flag)
{
    (void)edx;
    DotAFrameEvent(frame, parent);
    if (g_enabled && g_cineFull && x == 0.0f && y == 0.0f && point == rel && (point == 0 || point == 8) && VtRva(parent) == VT(CinePanel)) {
        // CinematicTopBorder (TOPLEFT) / CinematicBottomBorder (BOTTOMRIGHT) are 0.8 wide and anchored at one
        // corner of the full-width cinematic panel: pin the opposite side too, so they span the whole width
        u32 other = point == 0 ? 2 : 6;
        orig_SetFramePoint(frame, 0, point, parent, rel, x, y, flag);
        orig_SetFramePoint(frame, 0, other, parent, other, 0.0f, 0.0f, flag);
        dlogf_("cinematic border %08X pinned to both sides (points %u,%u)", frame, point, other);
        return;
    }
    // CinematicAspect: the frames placed on the full-width cinematic panel (the speaker's portrait, name and text)
    // move in from its edges to a centred area of that shape; the letterbox borders above keep the whole width
    if (g_enabled && g_cineFull && g_cineAspectOpt > 1.33 && rel <= 8 && rel % 3 != 1 && VtRva(parent) == VT(CinePanel)) {
        float cut = (float)((g_uiW_d - g_uiH_d * g_cineAspectOpt) * 0.5);
        if (cut > 0.0f) {
            float nx = rel % 3 == 0 ? x + cut : x - cut;
            dlogf_("cinematic frame %08X (vt %06X) pt=%u rel=%u x=%.4f -> %.4f y=%.4f", frame, VtRva(frame), point, rel, x, nx, y);
            x = nx;
        }
    }
    // the loading screen's minimap: remembered, so it can be fitted into a custom picture's frame (FitLoadingMinimap),
    // which is only known when the picture is read, after the minimap has been placed
    if (g_enabled && VtRva(parent) == VT(Loading) && point == 3 && rel == 3 && fabsf(x - 0.056875f) < 1e-4f && fabsf(y - 0.0425f) < 1e-4f) {
        g_loadMinimap = frame; g_loadMinimapParent = parent;
    }
    if (g_enabled && g_scaleOn && VtRva(parent) == VT(Loading)) {        // frames placed on the loading screen
        float k = g_uiH_f / 0.6f;
        if (point == 7 && rel == 7) y += 0.0025f * (k - 1.0f);           // LoadingBarText: on the bar, as the bar moves
        else { x *= k; y *= k; }
        if (point == 6 && rel == 6 && VtRva(frame) == VT(Sprite)) x += 0.4f * (k - 1.0f);   // LoadingBar (model)
    }
    if (g_enabled && VtRva(parent) == VT(Loading) && point == rel && (point == 6 || point == 7)) {   // the bar and its text
        int t = point == 7; g_loadBar[t] = frame; g_loadBarParent[t] = parent; g_loadBarX[t] = x; g_loadBarY[t] = y;
        if (!t && VtRva(frame) != VT(Sprite)) g_loadBar[0] = 0;
    }
    int ws;
    // Battle.net screens (CBattleNetChatPanel spans the screen): the top area (banner, logo, button bar) goes to the
    // centre; below it the contents move with the widened panels' inner edges (MenuLayout=3: half as far, so they
    // stay centred in them); the bottom right button panel stays
    // (chat panel first: the only one with the top area; then standard game, profile, team game, custom game
    // create / join / load)
    static const u32 kBnetScreens[3][7] = {
        { 0x964224, 0x965C00, 0x966A38, 0x965D24, 0x960EB8, 0x9612F0, 0x96153C },
        { 0xAAC1CC, 0xAAF87C, 0xAAF1BC, 0xAAFE70, 0xAADA34, 0xAAE050, 0xAAE3E4 },
        { 0xA9C228, 0xA9F930, 0xA9F270, 0xA9FF3C, 0xA9DAB0, 0xA9E0D8, 0xA9E470 } };
    int bs = -1;
    if (g_enabled && g_menuWide && rel < 8) {
        u32 pv = VtRva(parent);
        for (int i = 0; i < 7; i++) if (pv == kBnetScreens[BuildIdx()][i]) bs = i;
    }
    if (bs >= 0) {
        float m = MenuPanelMove(0.5f, 0), h = bs == 0 && y > -0.14f && rel <= 2 ? 1.0f : g_menuEdges == 3 ? 0.5f : 1.0f;
        // profile: the portrait and the name sit in their own boxes at the top left, which keep their places (only
        // the name box's right end moves); the home page and the rest below (from y -0.1825) move as elsewhere
        if (bs == 2 && rel == 0 && y > -0.178f) h = 0.0f;
        float nx = rel % 3 == 0 ? x + h * m : rel % 3 == 2 ? x - h * m : x;
        if (g_logCount < 400) { g_logCount++; dlogf_("SetPoint BNET %d child=%08X(vt %06X) pt=%u rel=%u x=%.4f -> %.4f y=%.4f", bs, frame, VtRva(frame), point, rel, x, nx, y); }
        x = nx;
    }
    if (g_enabled && g_menuWide && rel <= 8 && (ws = WideScreen(parent)) != 0) {
        // left column of the screen: towards the centre; right column: towards the centre, except the bottom right
        // (the button panel there keeps its place)
        // the same moves as the panels' inner ends (skirmish: 0.5 / 0.499, the others: 0.43 / 0.424 in the models)
        // (MenuLayout=3: on every such screen the contents move half as far, so they stay centred in the widened
        // panels; the lobby's contents sit at the panels' outer edges, so with MenuLayout=2 they stay there)
        float h = g_menuEdges == 3 ? 0.5f : ws == 3 ? 0.0f : 1.0f;
        float nx = rel % 3 == 0 ? x + h * MenuPanelMove(ws == 1 ? 0.5f : 0.43f, 0)
                 : (rel % 3 == 2 && rel != 8) ? x - h * MenuPanelMove(ws == 1 ? 0.499f : 0.424f, 1) : x;
        if (g_logCount < 400) { g_logCount++; dlogf_("SetPoint SCRN child=%08X(vt %06X) parent vt %06X pt=%u rel=%u x=%.4f -> %.4f y=%.4f",
            frame, VtRva(frame), VtRva(parent), point, rel, x, nx, y); }
        x = nx;
    }
    if (g_enabled && g_f != 0.0f && point == 6 && rel == 6 && VtRva(frame) == VT(TimeOfDay)) {
        y += g_f;
        // the frame sits at the bottom of the screen and its model draws the top bar ornament far above it: with
        // UIScale over 100 the frame goes below the screen, the game finds it off-screen and does not draw the
        // model at all. 0x40 in the frame flags: not clipped by the screen (layout part at +0xB4 of the object)
        if (g_f < 0.0f) *(u32*)(frame - 0xB4 + 0xC) |= 0x40;
        dlogf_("time of day indicator raised by %.4f", g_f);
    }
    if (g_enabled) {
        int isUI = IsGameUIRoot(parent);
        if (isUI || (g_glue && IsScreenRoot(parent))) {
            if (isUI) CheckWindowOnce();
            int keep = isUI ? IsFullWidthFrame(frame, parent - OFS_UI_Frame) : IsKeepClassGlue(frame);
            if (isUI && rel == 0 && y > -0.1f && x < 0.12f && !FromGame((u32)__builtin_return_address(0))) keep = 1;   // map helper: top-left corner group
            float nx = keep ? x : ShiftX(rel, x);
            if (!isUI && keep && g_menuCut > 0.0f && VtRva(frame) != VT(Sprite) && !IsKeepClass(frame))   // MenuAspect: the menu area's edges
                nx = rel % 3 == 0 ? x + g_menuCut : rel % 3 == 2 ? x - g_menuCut : x;
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
    // frames placed on the world view's top or bottom edge (the map's multiboard and leaderboard, below the top bar):
    // the edges moved to the screen edges (WorldFullHeight), keep such frames where they were
    if (g_enabled && g_worldFull && g_gameUI && !IsBadReadPtr((void*)(g_gameUI + OFS_UI_World), 4)) {
        u32 w = *(u32*)(g_gameUI + OFS_UI_World);
        if (w && parent == w + OFS_UI_Frame && frame != parent) {
            float dy = rel <= 2 && g_worldTopDefault ? WORLD_T0 : rel >= 6 && rel <= 8 && g_worldBottomDefault ? WORLD_B0 : 0.0f;
            if (dy != 0.0f) { dlogf_("frame %08X (vt %06X) on the world view edge %u: y %.4f -> %.4f", frame, VtRva(frame), rel, y, y + dy); y += dy; }
        }
    }
    orig_SetFramePoint(frame, 0, point, parent, rel, x, y, flag);
}

typedef void (__fastcall *SetAbsPoint_t)(u32 frame, u32 edx, u32 point, float x, float y, u32 flag);
static SetAbsPoint_t orig_SetAbsPoint;
static void DumpSimpleLevels(u32 bar);

// ---- interfaces built by map helper DLLs (DracoL1ch's DotA Allstars, with DotAOriginal=0) ----
static u32 g_gameSize;
static int FromGame(u32 ret)
{
    if (!g_gameSize && g_base) g_gameSize = ((IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew))->OptionalHeader.SizeOfImage;
    return ret - g_base < g_gameSize;
}
// helper functions that place labels over game buttons from the buttons' real (already moved) rectangles
static const struct { const char* module; u32 size; u32 from, to; } kOverlayPlacers[] = {
    { "WarCraftHelperv9k.dll", 0x482000, 0x14480, 0x148BA },     // shop / item hotkey and price labels
    { "WarCraftHelperv9k.dll", 0x482000, 0x151B0, 0x160F8 },     // inventory and command card labels
};
static int __attribute__((noinline)) OverlayPlacement(void)
{
    static u32 base[2], end[2]; static int init;
    if (!init) {
        init = 1;
        for (size_t i = 0; i < sizeof kOverlayPlacers / sizeof kOverlayPlacers[0]; i++) {
            HMODULE m = GetModuleHandleA(kOverlayPlacers[i].module);
            if (!m) { init = 0; return 0; }                                 // helper not loaded yet: look again later
            u32 sz = ((IMAGE_NT_HEADERS*)((u32)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew))->OptionalHeader.SizeOfImage;
            if (sz != kOverlayPlacers[i].size) { logf_("map helper %s: unknown build, button labels may be off", kOverlayPlacers[i].module); continue; }
            base[i] = (u32)m + kOverlayPlacers[i].from; end[i] = (u32)m + kOverlayPlacers[i].to;
        }
    }
    // look at the first few return addresses on the stack only (deeper values may be stale data from earlier calls)
    u32* sp = (u32*)__builtin_frame_address(0);
    int rets = 0;
    for (int i = 0; i < 0x300 && rets < 24; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        u32 v = sp[i];
        if (v < 0x10000 || IsBadReadPtr((void*)(v - 7), 7)) continue;
        uint8_t* c = (uint8_t*)(v - 7);
        int isRet = c[2] == 0xE8 || (c[1] == 0xFF && ((c[2] >> 3) & 7) == 2) || (c[5] == 0xFF && ((c[6] >> 3) & 7) == 2 && c[6] >= 0xD0)
                 || (c[4] == 0xFF && ((c[5] >> 3) & 7) == 2) || (c[0] == 0xFF && ((c[1] >> 3) & 7) == 2);
        if (!isRet) continue;
        rets++;
        for (int k = 0; k < 2; k++) if (base[k] && v > base[k] && v < end[k]) return 1;
    }
    return 0;
}

static void __fastcall SetAbsPoint_hook(u32 frame, u32 edx, u32 point, float x, float y, u32 flag)
{
    if (g_debug && g_loadingActive) { static int n; if (n < 40) { n++;
        dlogf_("loading SetAbsPoint frame %08X (vt %06X) pt=%u x=%.5f y=%.5f", frame, VtRva(frame), point, x, y); } }
    (void)edx;
    u32 ret = (u32)__builtin_return_address(0) - g_base;
    // fixed positions laid out for the 0..0.8 space (info bar inventory, menu dialogs) -> move into the centered area.
    // positions computed at runtime (tooltips, HP bars, floating text) are already in the wide space and stay as is.
    int fixed = (ret == G->retInventory) || (!g_menuEdges && (ret == G->retMenu[0] || ret == G->retMenu[1] || ret == G->retMenu[2]));
    float nx = (g_enabled && fixed) ? x + g_e : x;
    if (g_enabled && !fixed && !FromGame(ret + g_base) && VtRva(frame) != VT(StatBar) && !OverlayPlacement()) {
        if (y > 0.5f && x < 0.06f) { }                    // top-left corner group: left screen edge
        else if (y > 0.5f && x > 0.7f) nx = x + 2 * g_e;   // top-right corner group: right screen edge
        else nx = x + g_e;                                // laid out around the 0.8-wide console
        if (y > 0.55f) y += g_f;                          // top strip: follows the top edge; the rest stays with the console
    }
    // Unit health bars (CStatBar) follow their units anywhere on screen. With the world drawn behind the console,
    // units down there get bars too, and those are drawn on top of the console. In the original these units are
    // outside the world view and have no bars: move such bars off screen (only where the console / top bar is).
    if (g_debug && g_enabled && VtRva(frame) == VT(StatBar) && y < 0.2f) {
        static int dumped; if (!dumped) { dumped = 1; DumpSimpleLevels(frame); }
    }
    // With the bars drawn under the console (HealthBarsUnderConsole=1) its panels and decorated edges cover them,
    // but its see-through parts (portrait, info panel, minimap background) would still show them: hide bars that
    // lie below the console's top line (0.13; a bar is about 0.005 high). Without it, hide everything up to the
    // top of the decorated edge (minimap frame, command card, about 0.17).
    if (g_enabled && g_worldFull && VtRva(frame) == VT(StatBar)) {
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
        if (x > L && x < R && y > g_uiH_f + WORLD_T0) hid = 1;     // under the top bar
        if (hid) {
            static int n; if (n++ < 5) dlogf_("health bar under the console hidden (x %.3f y %.3f ret %06X)", x, y, ret);
            y = -1.0f;
        }
        nx = x;                                                 // (unit bars are never "fixed" positions)
    }
    // The occupancy marks above a burrow or a gold mine (COccupUI, workers inside) are placed by their top point and
    // kept inside the world view's rectangle. With WorldFullHeight that rectangle reaches the screen edges, so the
    // marks of a building behind the console were drawn over it. They are now drawn under the console like the
    // health bars (OccupUnder) and follow the same rules: beside the console they move out to its side, over it
    // they are hidden, as under the top bar
    if (g_enabled && g_worldFull && point == 1 && VtRva(frame) == kOccupVt[BuildIdx()]) {
        const float L = g_e, R = g_e + 0.8f;
        typedef float (__fastcall *GetSize_t)(u32 self, u32 edx);
        float hw = ((GetSize_t)(*(u32**)frame)[6])(frame, 0) * 0.5f, h = ((GetSize_t)(*(u32**)frame)[7])(frame, 0);
        if (!(hw > 0.0f && hw < 0.5f)) hw = 0.02f;
        if (!(h > 0.0f && h < 0.2f)) h = 0.01f;
        if (g_occupUnder) OccupUnder(frame);
        float bot = y - h;
        int hid = 0;
        if (!g_occupUnder) {
            if (x + hw > L && x - hw < R && bot < 0.17f) hid = 1;
        } else if (bot < WORLD_B0 + 0.005f && x + hw > L && x - hw < R) {
            if (x < L) x = L - hw;                              // mostly beside the console: whole beside it
            else if (x > R) x = R + hw;
            else hid = 1;
        }
        if (x > L && x < R && y - 0.5f * h > g_uiH_f + WORLD_T0) hid = 1;     // under the top bar
        if (hid) {
            static int n; if (n++ < 5) dlogf_("occupancy marks over the console hidden (x %.3f y %.3f)", x, y);
            y = -1.0f;
        }
        nx = x;
    }
    if (fixed && g_logCount < 400) { g_logCount++; dlogf_("AbsPoint child=%08X(vt %06X) pt=%u x=%.4f -> %.4f y=%.4f ret=%06X", frame, VtRva(frame), point, x, nx, y, ret); }
    orig_SetAbsPoint(frame, 0, point, nx, y, flag);
}

// MenuAspect: the menu screens narrowed to the menu area. Sprite frames placed over a whole screen (the campaign
// screen's 3D scene, the campaign fade, the Battle.net login doors, widened to the screen edges as without
// MenuAspect) are widened back to the whole screen; sprites placed by a point (logos) stay in the menu area.
static u32 g_narrowed[32]; static int g_narrowedN;
static int IsNarrowed(u32 f) { for (int i = 0; i < g_narrowedN && i < 32; i++) if (g_narrowed[i] == f) return 1; return 0; }
static void AddNarrowed(u32 f) { if (!IsNarrowed(f)) g_narrowed[g_narrowedN++ % 32] = f; }
static void __fastcall SetAllPoints_hook(u32 frame, u32 edx, u32 parent, u32 flag)
{
    (void)edx;
    DotAFrameEvent(frame, parent);
    float dx = 0.0f;
    if (g_enabled) {
        int isUI = IsGameUIRoot(parent);
        if (isUI || (g_glue && IsScreenRoot(parent))) {
            int keep = isUI ? IsFullWidthFrame(frame, parent - OFS_UI_Frame) : IsKeepClassGlue(frame);
            if (!keep) dx = g_e;
            else if (!isUI && VtRva(frame) != VT(Sprite) && !IsKeepClass(frame)) { dx = g_menuCut; if (dx > 0.0f) AddNarrowed(frame); }   // MenuAspect: screens span the menu area only
            // (not the cursor: its frame is the whole screen, mouse positions are measured in it)
            if (!isUI && VtRva(frame) != VT(Score) && VtRva(frame) != VT(Loading)) g_scoreActive = 0;   // a menu screen again
            if (g_logCount < 400) { g_logCount++; dlogf_("SetAll   %s child=%08X(vt %06X) parent vt %06X dx=%.4f", isUI ? "UI  " : "GLUE", frame, VtRva(frame), VtRva(parent), dx); }
        }
    }
    if (g_enabled && g_menuCut > 0.0f && VtRva(frame) == VT(Sprite) && IsNarrowed(parent)) {
        dx = -g_menuCut;
        dlogf_("sprite %08X over a narrowed screen (vt %06X): whole screen again", frame, VtRva(parent));
    }
    float dxL = dx, dxR = -dx;
    // GlueSpriteLayerTopRight (right-hand menu decorations: chains, panel frames, post):
    // its model is authored for a 0.8-wide screen, so slide the whole layer to the right edge.
    if (g_enabled && g_glue && g_menuEdges && ((u32)__builtin_return_address(0) - g_base) == G->retGlueTopRight) {
        dxL = 2.0f * g_e - g_menuCut; dxR = dxL;
        if (g_menuCut > 0.0f) MenuClipAddFrame(frame);
        dlogf_("GlueSpriteLayerTopRight shifted by %.4f", dxL);
    }
    // GlueSpriteLayerTopLeft (left-hand menu panels): with MenuAspect to the menu area's left edge
    static const u32 kRetGlueTopLeft[3] = { 0x57BA63, 0x2E0BEE, 0x313BDE };
    if (g_enabled && g_glue && g_menuEdges && g_menuCut > 0.0f && ((u32)__builtin_return_address(0) - g_base) == kRetGlueTopLeft[BuildIdx()]) {
        dxL = g_menuCut; dxR = dxL;
        MenuClipAddFrame(frame);
        dlogf_("GlueSpriteLayerTopLeft shifted by %.4f", dxL);
    }
    float dyT = 0.0f, dyB = 0.0f;
    if (g_enabled && g_scaleOn && VtRva(frame) == VT(Loading)) {                 // CLoading: 0.8k x 0.6k, centred
        float k = g_uiH_f / 0.6f;
        dxL = (g_uiW_f - 0.8f * k) * 0.5f; dxR = -dxL;
    }
    ClearAllPoints(frame, 0);
    orig_SetFramePoint(frame, 0, 0, parent, 0, dxL, dyT, 0);
    orig_SetFramePoint(frame, 0, 8, parent, 8, dxR, dyB, flag);
}


// ---- world projection: keep the original zoom and screen position when the world view is extended to full height ----
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
static int g_subBright = 100;    // MovieSubtitleBrightness: subtitle white level in percent
static int g_subSize = 100;      // MovieSubtitleSize: movie subtitle size in percent (with the movie renderer)
static int g_movieRenderer = 1; // MovieRenderer=1: play cinematics through W3TrueWidescreen\\MpcVideoRenderer.ax if present
     // FovFix=1: keep the original vertical field of view on wide screens
// CameraZoomOut: >1 shows more of the map (world view only, cinematics untouched)
static volatile DWORD g_lastWorldTick = 0;   // set by RenderWorld_hook: are we in a game (world being drawn)?
static int g_persLog = 0;
// WC3 derives its FOV from the view diagonal, so a wider view gets a smaller vertical FOV (looks zoomed in).
// For views that were widened by us (full-screen menu backgrounds, the game world) rebuild the projection
// so the vertical FOV equals the original 4:3 one and the extra width simply shows more (Hor+).
static void MapWidescreenOptionPoll(void);
static void __fastcall Persp_hook(u32 out, u32 edx, float fovY, float aspect, float zn, float zf)
{
    // aspect = viewport width / height; the engine spreads fovY over the view DIAGONAL
    (void)edx;
    MapWidescreenOptionPoll();
    if (g_fpsLimit) {                                   // once: the display mode the game actually runs in
        static DWORD t0; static int done;
        if (!t0) t0 = GetTickCount();
        else if (!done && GetTickCount() - t0 > 2000) {
            done = 1; DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
                logf_("display: %lux%lu at %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        }
    }
    const float W = g_uiW_f;
    const float H1 = 0.6f, H0 = 0.6f - WORLD_B0 + WORLD_T0;      // 0.45: original world view height
    const float Hs = g_uiH_f;                                     // UI screen height (0.6 unless UIScale)
    int isFull = fabsf(aspect - W / Hs) < 4e-3f;                  // full screen (menus, world at full height)
    int isNorm = fabsf(aspect - W / (Hs - WORLD_B0 + WORLD_T0)) < 4e-3f;   // world view between the strips
    int inGame = (GetTickCount() - g_lastWorldTick) < 500;
    if (!inGame) MenuClipPoll();
    if (g_persLog < 30 && (inGame || g_persLog < 3)) { g_persLog++; dlogf_("persp fov=%.4f aspect=%.4f full=%d norm=%d inGame=%d ret=%06X", fovY, aspect, isFull, isNorm, inGame, (u32)__builtin_return_address(0) - g_base); }
    if (g_enabled && !g_menuBgFull && !inGame && isFull) {   // MenuBackgroundFull=0: 3D menu backgrounds
        orig_Persp(out, 0, fovY, 0.8f / H1, zn, zf);                         // stretched as in the original game
        return;
    }
    if (g_enabled && g_narrowOn && !inGame && isFull) {             // MenuLayout=0: stretched menus, unstretched 3D background
        orig_Persp(out, 0, fovY, 0.8f / H1, zn, zf);
        float* mm = (float*)out; mm[0] = mm[5] / (float)g_aspect;
        return;
    }
    if (!g_enabled || W < 0.8f * Hs / H1 + 1e-3f || (!isFull && !isNorm)) { orig_Persp(out, 0, fovY, aspect, zn, zf); return; }
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
    if (inGame && isFull && g_dotaRun && g_fovFix) {               // DotA: world made full screen by the map itself
        orig_Persp(out, 0, fovY, 0.8f / H1, zn, zf);
        m[0] = m[5] / aspect;
        if (worldDef) { m[0] /= g_zoom; m[5] /= g_zoom; }
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
typedef int (__fastcall *RenderWorld_t)(u32 ecx, u32 edx);
static RenderWorld_t orig_RenderWorld;
typedef struct { DWORD X, Y, Width, Height; float MinZ, MaxZ; } VP8;
typedef struct { LONG x1, y1, x2, y2; } RECT8;
// the device must be a Direct3D 8 device (not the OpenGL renderer's object) before its methods are called. Not only
// Microsoft's d3d8.dll: a wrapper loaded in its place (d3d8to9, dgVoodoo, ReShade, ... as d3d8.dll in the game folder
// or under another name) implements the same interface. Accepted: the device's methods in the module loaded as
// d3d8.dll, or in any other module that answers QueryInterface for IDirect3DDevice8 (or exports Direct3DCreate8)
static const GUID kIID_D3DDevice8 = {0x7385e5df,0x8fe8,0x41d5,{0x86,0xb6,0xd7,0xb4,0x85,0x47,0xb6,0xcf}};
static int IsD3D8Device(u32 dev)
{
    static int state = -1;                                       // -1 unknown, 0 no, 1 yes
    if (state >= 0) return state;
    state = 0;
    u32* vt = *(u32**)dev;
    HMODULE m = NULL; char mn[MAX_PATH] = "?";
    if (!IsBadReadPtr(vt, 42 * 4) &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[36], &m) && m)
        GetModuleFileNameA(m, mn, sizeof mn);
    const char* how = NULL;
    HMODULE d8 = GetModuleHandleA("d3d8.dll");
    if (m && m == d8) how = "d3d8.dll";
    else if (m && m != (HMODULE)g_base && GetProcAddress(m, "Direct3DCreate8")) how = "exports Direct3DCreate8";
    else if (!IsBadReadPtr(vt, 42 * 4)) {
        // methods redirected by an overlay or a hook: the rest of the table still in d3d8.dll, or the object
        // answers QueryInterface for IDirect3DDevice8 (QueryInterface is entry 0 of every COM interface)
        int ind8 = 0;
        for (int i = 0; d8 && i < 42; i++) {
            HMODULE mi = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[i], &mi) && mi == d8) ind8++;
        }
        if (ind8 >= 21) how = "mostly d3d8.dll";
        else {
            typedef HRESULT (__stdcall *QI_t)(u32, const GUID*, void**);
            typedef ULONG (__stdcall *Rel_t)(u32);
            HMODULE m0 = NULL; u32 q = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &m0) && m0 &&
                m0 != (HMODULE)g_base && ((QI_t)vt[0])(dev, &kIID_D3DDevice8, (void**)&q) == S_OK && q) {
                ((Rel_t)(*(u32**)q)[2])(q);
                how = "IDirect3DDevice8";
            }
        }
    }
    if (how) { state = 1; logf_("Direct3D 8 device %08X (%s: %s)", dev, mn, how); }
    else {
        char m0n[MAX_PATH] = "?"; HMODULE m0 = NULL;
        if (!IsBadReadPtr(vt, 4) && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &m0) && m0)
            GetModuleFileNameA(m0, m0n, sizeof m0n);
        logf_("renderer is not Direct3D 8 (device %08X, methods in %s / %s), console backing off", dev, m0n, mn);
    }
    return state;
}

// the game's Direct3D device and its back buffer size. 1.26: a Direct3D 8 device at a known place in the graphics
// object; 1.27: Direct3D 9, found once by asking the pointers in that object for the device interface
typedef struct { u32 dev; int clear, setVp, getVp, W, H; } RenderDev;
static const GUID kIID_D3DDevice9 = {0xd0223b96,0xbf7a,0x43fd,{0x92,0xbd,0xa4,0x3b,0x0d,0x82,0xb9,0xeb}};
static int GetRenderDevice(RenderDev* d)
{
    u32 gx = *(u32*)(g_base + G->gx);
    if (!gx || IsBadReadPtr((void*)gx, 4)) return 0;
    if (G->gxDevice) {
        if (IsBadReadPtr((void*)gx, G->gxDevice + 4)) return 0;
        u32 dev = *(u32*)(gx + G->gxDevice);
        if (!dev || IsBadReadPtr((void*)dev, 4) || !IsD3D8Device(dev)) return 0;
        d->dev = dev; d->clear = 36; d->setVp = 40; d->getVp = 41;
        d->W = *(int*)(gx + G->gxWidth); d->H = *(int*)(gx + G->gxWidth + 4);
        return d->W > 0 && d->H > 0;
    }
    typedef HRESULT (__stdcall *QI_t)(u32, const GUID*, void**);
    typedef ULONG (__stdcall *Rel_t)(u32);
    static int ofs = -1;
    if (ofs == -1) {                                  // 1.27: the device sits at the same place as in 1.26 (1.28: at 0x5A0,
        ofs = -2;                                     // after the IDirect3D9 at 0x59C); check it once
        u32 at = G->build == 7680 ? 0x5A0 : 0x584;
        u32 p = IsBadReadPtr((void*)gx, at + 4) ? 0 : *(u32*)(gx + at);
        u32* vt = (p && !IsBadReadPtr((void*)p, 4)) ? *(u32**)p : NULL;
        u32 q = 0; HRESULT hr = E_POINTER;
        if (vt && !IsBadReadPtr(vt, 119 * 4)) hr = ((QI_t)vt[0])(p, &kIID_D3DDevice9, (void**)&q);
        char mn[MAX_PATH] = "?"; HMODULE vm = NULL;
        if (vt && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &vm) && vm)
            GetModuleFileNameA(vm, mn, sizeof mn);
        if (hr == S_OK && q) { ((Rel_t)(*(u32**)q)[2])(q); ofs = (int)at; logf_("Direct3D 9 device %08X (%s)", p, mn); }
        else logf_("Direct3D 9 device not found (%08X, %08lX, %s), console backing off", p, hr, mn);
    }
    if (ofs < 0) return 0;
    u32 dev = *(u32*)(gx + ofs);
    if (!dev || IsBadReadPtr((void*)dev, 4)) return 0;
    u32* vt = *(u32**)dev;
    d->dev = dev; d->clear = 43; d->setVp = 47; d->getVp = 48;
    // back buffer size = size of render target 0 (IDirect3DDevice9::GetRenderTarget, IDirect3DSurface9::GetDesc)
    typedef HRESULT (__stdcall *GetRT_t)(u32, DWORD, u32*);
    typedef HRESULT (__stdcall *GetDesc_t)(u32, DWORD*);
    u32 rt = 0; DWORD desc[8];
    if (((GetRT_t)vt[38])(dev, 0, &rt) != S_OK || !rt) return 0;
    HRESULT hr = ((GetDesc_t)(*(u32**)rt)[12])(rt, desc);
    ((Rel_t)(*(u32**)rt)[2])(rt);
    if (hr != S_OK) return 0;
    d->W = (int)desc[6]; d->H = (int)desc[7];
    return d->W > 0 && d->H > 0;
}

// ---- DotA top hero bar: its slot frames are placed for the 0.8-wide interface by a path the hooks do not see ----
static void DotATopBarFix(void)
{
    u32 top = *(u32*)(g_base + G->simpleTop);
    if (!top || IsBadReadPtr((void*)top, 0x200) || !orig_SetAbsPoint) return;
    int budget = 6000;
    for (int L = 0; L < 6; L++) {
        int node = *(int*)(top + 0x194 + L * 12);
        while (node > 0 && budget-- > 0 && !IsBadReadPtr((void*)node, 12)) {
            u32 f = *(u32*)(node + 8);
            node = *(int*)(node + 4);
            if (!f || IsBadReadPtr((void*)f, 0x30) || VtRva(f) != VT(SimpleFrame)) continue;
            u32 po = *(u32*)(f + 8 + 6 * 4);
            if (!po || IsBadReadPtr((void*)po, 0x14) || *(u32*)po - g_base != VT(FramePointRel)) continue;
            if (VtRva(*(u32*)(po + 4)) != VT(GameUIFrame) || *(u32*)(po + 8) != 6) continue;
            float x = *(float*)(po + 12), y = *(float*)(po + 16);
            if (y < 0.55f || y > 0.6f) continue;
            if (x < 0.1f || x > 0.65f) continue;
            orig_SetAbsPoint(f, 0, 6, x + g_e, y + g_f, 1);   // an absolute point: not matched again until the map re-anchors the slot
            static int n; if (g_debug && n++ < 40) dlogf_("top bar slot %08X moved %.4f -> %.4f", f, x, x + g_e);
        }
    }
}

// ---- clicks through the console. In the original the world view ends at the console (y 0.13) and under the top
//      bar (the 0.577 gate below); with WorldFullHeight the world is also behind both, and a click on the console's
//      or top bar's artwork (not on a button) went to the world under it. Clicks on that artwork are dropped (its
//      outline, including the raised frames of the minimap and the command card), in the centred 4:3 area only:
//      beside the console and the top bar the world is visible and takes clicks. ----
// the console's top edge (UI y, thousandths) over its width in 128 steps of 0.00625: the highest opaque texel of the
// four races' console textures (ConsoleUI.fdf: bottom tiles 0.176 / 0.15 high); the top bar reaches 0.025 down
static const uint8_t kConsoleTop[128] = {
    171, 171, 174, 173, 172, 167, 167, 165, 166, 165, 165, 165, 166, 166, 165, 166,
    168, 165, 166, 167, 166, 165, 165, 166, 166, 169, 169, 166, 166, 169, 168, 171,
    165, 163, 160, 159, 147, 141, 140, 140, 140, 140, 140, 141, 142, 150, 143, 147,
    148, 142, 141, 142, 146, 142, 140, 141, 141, 142, 142, 142, 141, 140, 140, 140,
    140, 141, 141, 141, 140, 140, 139, 139, 139, 145, 145, 146, 140, 139, 141, 141,
    142, 142, 147, 142, 142, 145, 148, 148, 145, 146, 149, 151, 170, 167, 172, 167,
    166, 167, 164, 160, 163, 168, 168, 160, 166, 165, 165, 172, 172, 163, 161, 162,
    161, 159, 159, 159, 159, 164, 163, 168, 166, 161, 167, 164, 165, 173, 171, 170,
};
static int OverConsole(float x, float y)
{
    float cx = x - g_e;
    if (cx < 0.0f || cx > 0.8f) return 0;
    int i = (int)(cx / 0.00625f); if (i > 127) i = 127;
    return y < kConsoleTop[i] * 0.001f || y >= g_uiH_f - 0.025f;
}
typedef int (__fastcall *WorldPick_t)(u32 ecx, u32 edx, float x, float y, u32 a, u32 b, u32 c);
static WorldPick_t orig_WorldPick;
static int __fastcall WorldPick_hook(u32 ecx, u32 edx, float x, float y, u32 a, u32 b, u32 c)
{
    if (g_enabled && g_worldFull && g_worldBottomDefault && g_worldTopDefault && !g_dotaRun
        && OverConsole(x, y))
        return 0;
    return orig_WorldPick(ecx, edx, x, y, a, b, c);
}

// ---- HideUIKey: a key that hides the whole interface in a game, for screenshots. Everything the game draws after
//      the world in a frame is the interface (console, top bar, portraits, minimap, messages, cursor); while hidden,
//      those draw calls are dropped at the Direct3D device, and the console strips show the world. ----
static int g_hideKey = 0, g_hideMods = 0;      // virtual key (0: off); 1 Ctrl, 2 Shift, 4 Alt
static volatile int g_uiHidden = 0, g_afterWorld = 0;
static int UiHiding(void) { return g_uiHidden && g_afterWorld && GetTickCount() - g_lastWorldTick < 300; }
typedef HRESULT (__stdcall *Draw4_t)(u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw5_t)(u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw6_t)(u32, u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw7_t)(u32, u32, u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw9_t)(u32, u32, u32, u32, u32, u32, u32, u32, u32);
// originals per device vtable: after a movie the renderer DLL can be reloaded elsewhere and make a new device
typedef struct { u32* vt; void* f[4]; } DrawOrig;
static DrawOrig g_drawOrig[4]; static int g_drawOrigN;
static void** DrawOrigOf(u32 dev)
{
    u32* vt = *(u32**)dev;
    for (int i = 0; i < g_drawOrigN; i++) if (g_drawOrig[i].vt == vt) return g_drawOrig[i].f;
    return g_drawOrigN ? g_drawOrig[g_drawOrigN - 1].f : NULL;
}
#define o_DP(d, ...)    ((Draw4_t)DrawOrigOf(d)[0])(d, __VA_ARGS__)
#define o_DIP8(d, ...)  ((Draw6_t)DrawOrigOf(d)[1])(d, __VA_ARGS__)
#define o_DIP9(d, ...)  ((Draw7_t)DrawOrigOf(d)[1])(d, __VA_ARGS__)
#define o_DPUP(d, ...)  ((Draw5_t)DrawOrigOf(d)[2])(d, __VA_ARGS__)
#define o_DIPUP(d, ...) ((Draw9_t)DrawOrigOf(d)[3])(d, __VA_ARGS__)
// MenuAspect: the menus are drawn only inside the menu area. Parts of the menu models that were off the screen
// (gears, chains, the panels' outer ends, the Battle.net doors when open) would otherwise show beside it. Every
// interface draw call in the menus (orthographic projection; the 3D backgrounds use a perspective one and are left
// alone) gets two user clip planes at the area's edges.
typedef ULONG (__stdcall *Rel_t2)(u32);
static int g_menuClipD9; static DWORD g_sfThread;
static int MenuClipWanted(void)
{
    return g_menuCut > 0.0f && g_enabled && !g_loadingActive && !g_scoreActive && GetTickCount() - g_lastWorldTick > 500;
}
// which draws to cut: those made while one of the menu panel layers is being drawn. Every virtual method of the
// sprite frame class (both of its vtables) is wrapped: on entry the frame (`this`) goes on a small shadow stack and the
// return address is swapped for a stub that pops it again, so any argument count and calling convention passes
// through unchanged. A draw call is cut when one of the panel layers is on that stack. Backgrounds (also sprite
// frames, but other objects), fades, the cursor and the rest are left whole.
static u32 g_clipFrames[8]; static int g_clipFrameN;
static struct { u32 self, ret; } g_sfStack[64]; static int g_sfDepth;
static void MenuClipAddFrame(u32 layout)
{
    for (int i = 0; i < g_clipFrameN; i += 2) if (g_clipFrames[i] == layout) return;
    if (g_clipFrameN >= 8) { memmove(g_clipFrames, g_clipFrames + 2, 6 * sizeof(u32)); g_clipFrameN = 6; }   // the latest four layers
    g_clipFrames[g_clipFrameN++] = layout; g_clipFrames[g_clipFrameN++] = layout - OFS_UI_Frame;
    if (!g_sfThread) g_sfThread = GetCurrentThreadId();
    // the frame object starts 0xB4 before its layout part: its own vtable (the frame methods, drawing among them)
    // and the layout part's
    u32 obj = layout - OFS_UI_Frame;
    if (IsBadReadPtr((void*)obj, 0x1B0)) return;
    SfWrapVtable(*(u32*)obj, "the sprite frame");
    SfWrapVtable(*(u32*)layout, "the sprite frame layout");
}
static int InClipFrame(void)
{
    // the innermost sprite frame being drawn decides: a layer can draw other sprite frames (3D scenes, models placed
    // on it) from inside its own methods, and those stay whole
    if (g_sfDepth <= 0) return 0;
    u32 v = g_sfStack[g_sfDepth - 1].self;
    for (int i = 1; i < g_clipFrameN; i += 2) if (v - g_clipFrames[i] < 0x1B0) return 1;
    return 0;
}
extern void SfExitStub(void);
void __stdcall SfEnter(u32* retSlot, u32 self)
{
    if (g_sfDepth >= 64 || GetCurrentThreadId() != g_sfThread) return;          // (too deep: leave this call unwrapped)
    g_sfStack[g_sfDepth].self = self; g_sfStack[g_sfDepth].ret = *retSlot; g_sfDepth++;
    *retSlot = (u32)SfExitStub;
}
u32 __stdcall SfLeave(void) { return g_sfDepth > 0 ? g_sfStack[--g_sfDepth].ret : 0; }
__asm__(".globl _SfExitStub\n_SfExitStub:\n"
        "  sub $4, %esp\n  push %eax\n  push %edx\n  push %ecx\n"
        "  call _SfLeave@0\n  mov %eax, 12(%esp)\n"
        "  pop %ecx\n  pop %edx\n  pop %eax\n  ret\n");
static void SfWrapVtable(u32 vt, const char* what)
{
    static u32 done[8]; static int nd;
    if (!vt || vt < g_base || IsBadReadPtr((void*)vt, 4)) return;
    for (int i = 0; i < nd; i++) if (done[i] == vt) return;
    if (nd >= 8) return;
    done[nd++] = vt;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    u32 lo = g_base + nt->OptionalHeader.BaseOfCode, hi = lo + nt->OptionalHeader.SizeOfCode;
    int n = 0; while (n < 96 && !IsBadReadPtr((void*)(vt + n * 4), 4) && *(u32*)(vt + n * 4) >= lo && *(u32*)(vt + n * 4) < hi) n++;
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, n * 32 + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code || !n) return;
    for (int i = 0; i < n; i++) {
        uint8_t* t = code + i * 32; u32 orig = *(u32*)(vt + i * 4);
        // lea eax,[esp] / push ecx / push ecx / push eax / call SfEnter / pop ecx / jmp orig
        int k = 0;
        t[k++] = 0x8D; t[k++] = 0x04; t[k++] = 0x24;
        t[k++] = 0x51; t[k++] = 0x51; t[k++] = 0x50;
        t[k++] = 0xE8; *(u32*)(t + k) = (u32)SfEnter - (u32)(t + k + 4); k += 4;
        t[k++] = 0x59;
        t[k++] = 0xE9; *(u32*)(t + k) = orig - (u32)(t + k + 4); k += 4;
        u32 np = (u32)t; WriteMem(vt + i * 4, &np, 4);
    }
    logf_("menus: %d methods of %s wrapped (vtable %06X)", n, what, vt - g_base);
}
static DWORD g_clipStatT; static int g_clipStat[3];
static int MenuClipBegin(u32 dev)
{
    if (!MenuClipWanted()) return 0;
    u32* vt = *(u32**)dev; int d9 = g_menuClipD9;
    typedef HRESULT (__stdcall *GetT_t)(u32, u32, float*);
    typedef HRESULT (__stdcall *SetCP_t)(u32, u32, const float*);
    typedef HRESULT (__stdcall *SetRS_t)(u32, u32, u32);
    typedef HRESULT (__stdcall *GetVS_t)(u32, u32*);
    float V[16], P[16];
    if (g_debug) { g_clipStat[0]++; DWORD t = GetTickCount(); if (t - g_clipStatT > 5000) { g_clipStatT = t;
        dlogf_("menu clip: %d draws, %d in panel layers, %d cut (%d layer frames)", g_clipStat[0], g_clipStat[1], g_clipStat[2], g_clipFrameN / 2);
        g_clipStat[0] = g_clipStat[1] = g_clipStat[2] = 0; } }
    if (!InClipFrame()) return 0;
    if (g_debug) g_clipStat[1]++;
    if (((GetT_t)vt[d9 ? 45 : 38])(dev, 3, P) != S_OK) return 0;
    float xl = -1.0f + 2.0f * g_menuCut / g_uiW_f, xr = -xl;
    float pl[4], pr[4];
    u32 vs = 0;
    if (d9) ((GetVS_t)vt[93])(dev, &vs);
    if (vs) {                                                                        // shaders: planes in clip space
        pl[0] = 1; pl[1] = 0; pl[2] = 0; pl[3] = -xl; pr[0] = -1; pr[1] = 0; pr[2] = 0; pr[3] = xr;
        ((Rel_t2)(*(u32**)vs)[2])(vs);
    } else {                                                                         // fixed function: world space
        if (((GetT_t)vt[d9 ? 45 : 38])(dev, 2, V) != S_OK) return 0;
        float M[16];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            float a = 0; for (int k = 0; k < 4; k++) a += V[r * 4 + k] * P[k * 4 + c]; M[r * 4 + c] = a; }
        for (int k = 0; k < 4; k++) { pl[k] = M[k * 4 + 0] - xl * M[k * 4 + 3]; pr[k] = xr * M[k * 4 + 3] - M[k * 4 + 0]; }
    }
    ((SetCP_t)vt[d9 ? 55 : 48])(dev, 0, pl);
    ((SetCP_t)vt[d9 ? 55 : 48])(dev, 1, pr);
    ((SetRS_t)vt[d9 ? 57 : 50])(dev, 152, 3);                                        // D3DRS_CLIPPLANEENABLE
    if (g_debug) g_clipStat[2]++;
    return 1;
}
static void MenuClipEnd(u32 dev, int on)
{
    typedef HRESULT (__stdcall *SetRS_t)(u32, u32, u32);
    if (on) ((SetRS_t)(*(u32**)dev)[g_menuClipD9 ? 57 : 50])(dev, 152, 0);
}
static HRESULT __stdcall h_DP(u32 d, u32 a, u32 b, u32 c)
    { if (UiHiding()) return 0; int k = MenuClipBegin(d); HRESULT r = o_DP(d, a, b, c); MenuClipEnd(d, k); return r; }
static HRESULT __stdcall h_DPUP(u32 d, u32 a, u32 b, u32 c, u32 e)
    { if (UiHiding()) return 0; int k = MenuClipBegin(d); HRESULT r = o_DPUP(d, a, b, c, e); MenuClipEnd(d, k); return r; }
static HRESULT __stdcall h_DIP8(u32 d, u32 a, u32 b, u32 c, u32 e, u32 f)
    { if (UiHiding()) return 0; int k = MenuClipBegin(d); HRESULT r = o_DIP8(d, a, b, c, e, f); MenuClipEnd(d, k); return r; }
static HRESULT __stdcall h_DIP9(u32 d, u32 a, u32 b, u32 c, u32 e, u32 f, u32 g)
    { if (UiHiding()) return 0; int k = MenuClipBegin(d); HRESULT r = o_DIP9(d, a, b, c, e, f, g); MenuClipEnd(d, k); return r; }
static HRESULT __stdcall h_DIPUP(u32 d, u32 a, u32 b, u32 c, u32 e, u32 f, u32 g, u32 h, u32 i)
    { if (UiHiding()) return 0; int k = MenuClipBegin(d); HRESULT r = o_DIPUP(d, a, b, c, e, f, g, h, i); MenuClipEnd(d, k); return r; }
static void HideUiInstall(const RenderDev* rd)
{
    u32* vt = *(u32**)rd->dev;
    int d9 = rd->clear == 43;                               // Direct3D 9 (1.27) or 8 (1.26)
    g_menuClipD9 = d9;
    int dp = d9 ? 81 : 70;                                  // DrawPrimitive, DrawIndexedPrimitive, ...UP, ...IndexedUP
    if (IsBadReadPtr(vt, (dp + 4) * 4)) return;
    for (int i = 0; i < g_drawOrigN; i++) if (g_drawOrig[i].vt == vt) return;     // hooked already
    if (g_drawOrigN >= 4) return;
    void* fn[4] = { (void*)h_DP, d9 ? (void*)h_DIP9 : (void*)h_DIP8, (void*)h_DPUP, (void*)h_DIPUP };
    for (int i = 0; i < 4; i++) if (vt[dp + i] == (u32)fn[i]) return;          // (another vtable copy with ours in it)
    DrawOrig* o = &g_drawOrig[g_drawOrigN];
    o->vt = vt;
    for (int i = 0; i < 4; i++) o->f[i] = (void*)vt[dp + i];
    g_drawOrigN++;
    for (int i = 0; i < 4; i++) WriteMem((u32)&vt[dp + i], &fn[i], 4);
}
static void MenuClipPoll(void)
{
    if (g_menuCut <= 0.0f) return;
    RenderDev rd;
    if (!GetRenderDevice(&rd)) return;
    HideUiInstall(&rd);
    static int logged; if (!logged) { logged = 1; logf_("menus: drawn inside the menu area only (Direct3D %d)", g_menuClipD9 ? 9 : 8); }
    static u32 lastDev, lastVt, lastSlot;
    u32* vt = *(u32**)rd.dev; int dp = g_menuClipD9 ? 81 : 70;
    if (rd.dev != lastDev || (u32)vt != lastVt || vt[dp + 1] != lastSlot) {
        lastDev = rd.dev; lastVt = (u32)vt; lastSlot = vt[dp + 1];
        logf_("menus: device %08X vtable %08X draw %08X (ours %08X / %08X)", rd.dev, (u32)vt, vt[dp + 1], (u32)h_DIP8, (u32)h_DIP9);
    }
}
// the selection circles are drawn with the world: hide them the way the native EnableSelect(true, false) does
// (selection itself keeps working); afterwards the selection UI flag gets back the value it had
static void HideSelectionCircles(int hide)
{
    static int applied = 0; static u32 savedUi, appliedTo;
    typedef void (__fastcall *EnableSelect_t)(u32 ui, u32 edx, u32 sel, u32 selUi);
    u32 ui = g_gameUI;
    if (applied && ui != appliedTo) applied = 0;                // a new game
    if (hide == applied || !ui || IsBadReadPtr((void*)ui, 0x2A8) || *(u32*)ui != g_base + VT(GameUI) || !CodeIs(&G->enableSelect)) return;
    EnableSelect_t es = (EnableSelect_t)(g_base + G->enableSelect.rva);
    if (hide) { savedUi = *(u32*)(ui + 0x2A4); es(ui, 0, *(u32*)(ui + 0x2A0), 0); appliedTo = ui; }
    else es(ui, 0, *(u32*)(ui + 0x2A0), savedUi);
    applied = hide;
}
// the key is taken from the game window's messages, so the game never sees it (Ctrl+H does not also order Hold)
static WNDPROC g_hideOrigProc; static HWND g_hideWnd; static int g_hideSwallowUp;
static int HideModsMatch(void)
{
    return ((g_hideMods & 1) != 0) == ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        && ((g_hideMods & 2) != 0) == ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        && ((g_hideMods & 4) != 0) == ((GetKeyState(VK_MENU) & 0x8000) != 0);
}
static LRESULT CALLBACK HideUiWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((int)w == g_hideKey) {
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && HideModsMatch()) {
            if (!(l & (1 << 30)) && GetTickCount() - g_lastWorldTick < 300) {   // in a game; not an auto-repeat
                g_uiHidden = !g_uiHidden; logf_("interface %s", g_uiHidden ? "hidden" : "shown");
            }
            g_hideSwallowUp = 1;
            return 0;
        }
        if ((m == WM_KEYUP || m == WM_SYSKEYUP) && g_hideSwallowUp) { g_hideSwallowUp = 0; return 0; }
        if ((m == WM_CHAR || m == WM_SYSCHAR) && g_hideSwallowUp) return 0;
    }
    return CallWindowProcA(g_hideOrigProc, h, m, w, l);
}
static void HideUiPoll(void)
{
    if (g_hideWnd && IsWindow(g_hideWnd)) return;
    HWND h = GetActiveWindow();
    if (!h) return;
    g_hideOrigProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)HideUiWndProc);
    if (g_hideOrigProc) { g_hideWnd = h; logf_("HideUIKey: on"); }
}
// "F5", "Ctrl+Shift+H", "ScrollLock", ...: virtual key and modifiers; 0 if not understood
static int ParseKey(const char* s, int* mods)
{
    static const struct { const char* n; int vk; } kNames[] = {
        { "ScrollLock", VK_SCROLL }, { "Pause", VK_PAUSE }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE },
        { "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
        { "Backspace", VK_BACK }, { "Tab", VK_TAB }, { "Space", VK_SPACE }, { "Tilde", VK_OEM_3 }, { "Backquote", VK_OEM_3 },
    };
    *mods = 0;
    char b[64]; snprintf(b, sizeof b, "%s", s);
    char* t = b;
    for (;;) {
        while (*t == ' ') t++;
        if (!_strnicmp(t, "Ctrl+", 5)) { *mods |= 1; t += 5; }
        else if (!_strnicmp(t, "Shift+", 6)) { *mods |= 2; t += 6; }
        else if (!_strnicmp(t, "Alt+", 4)) { *mods |= 4; t += 4; }
        else break;
    }
    char* e = t + strlen(t); while (e > t && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (!*t) return 0;
    if ((t[0] == 'F' || t[0] == 'f') && t[1] >= '1' && t[1] <= '9') { int n = atoi(t + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (!t[1] && ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z'))) return toupper((unsigned char)*t);
    if (!t[1] && *t >= '0' && *t <= '9') return *t;
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) if (!_stricmp(t, kNames[i].n)) return kNames[i].vk;
    return 0;
}

static int __fastcall RenderWorld_hook(u32 ecx, u32 edx)
{
    g_afterWorld = 0;
    if (g_uiHidden && GetTickCount() - g_lastWorldTick > 2000) g_uiHidden = 0;   // a new game starts with it shown
    g_lastWorldTick = GetTickCount();
    int r = orig_RenderWorld(ecx, edx);
    g_lastWorldTick = GetTickCount();
    if (g_hideKey) {
        HideUiPoll();
        RenderDev hd;
        if (GetRenderDevice(&hd)) HideUiInstall(&hd);
        HideSelectionCircles(g_uiHidden);
        g_afterWorld = 1;                                   // from here to the next frame's world: the interface
        if (g_uiHidden) return r;                           // (no console backing: the world fills the screen)
    }
    if (g_enabled && g_dotaRun) DotATopBarFix();
    if (g_debug && g_gameUI && !IsBadReadPtr((void*)g_gameUI, 0x184)) {
        static u32 lastSt = 0xFFFFFFFF; u32 st = *(u32*)(g_gameUI + 0x180);
        if (st != lastSt) { lastSt = st; dlogf_("interface state %u (ui %08X)", st, g_gameUI); }
    }
    if (!(g_enabled && g_consoleBacking && ((g_worldFull && g_worldBottomDefault && g_worldTopDefault) || g_dotaRun))) return r;
    RenderDev rd;
    if (!GetRenderDevice(&rd)) return r;                         // OpenGL mode or unknown renderer: leave it alone
    u32 dev = rd.dev; u32* vt = *(u32**)dev; int W = rd.W, H = rd.H;
    typedef HRESULT (__stdcall *SetVP_t)(u32, const VP8*);
    typedef HRESULT (__stdcall *GetVP_t)(u32, VP8*);
    typedef HRESULT (__stdcall *Clear_t)(u32, DWORD, const RECT8*, DWORD, DWORD, float, DWORD);
    VP8 old; if (((GetVP_t)vt[rd.getVp])(dev, &old) != 0) return r;
    VP8 full = { 0, 0, (DWORD)W, (DWORD)H, old.MinZ, old.MaxZ };
    ((SetVP_t)vt[rd.setVp])(dev, &full);
    LONG x1 = (LONG)(g_e / g_uiW_f * W + 0.5f), x2 = (LONG)((g_e + 0.8f) / g_uiW_f * W + 0.5f);
    // in cinematic mode the letterbox (semi-transparent stone texture) spans the whole width; as in the
    // original, it must lie on black, not on the world
    if (g_cineFull && InCinematicMode()) { x1 = 0; x2 = W; }
    RECT8 rc[2] = {
        { x1, (LONG)(H * (1.0f - WORLD_B0 / g_uiH_f)), x2, H },            // console band
        { x1, 0, x2, (LONG)(H * (-WORLD_T0 / g_uiH_f) + 0.5f) }            // thin strip under the top bar
    };
    ((Clear_t)vt[rd.clear])(dev, g_dotaRun ? 1 : 2, rc, 1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);   // DotA: own top bar
    // The 3D portraits (unit portrait in the console, speaker portrait of cinematics and transmissions) are
    // drawn later with depth testing. In the original nothing of the world is drawn in these strips, so the
    // depth buffer there is empty; here the world wrote its depth first and would hide parts of the portraits.
    // Reset the depth in both strips across the whole width (nothing else 3D is drawn there).
    RECT8 rz[2] = { { 0, rc[0].y1, W, H }, { 0, 0, W, rc[1].y2 } };
    ((Clear_t)vt[rd.clear])(dev, g_dotaRun ? 1 : 2, rz, 2 /*D3DCLEAR_ZBUFFER*/, 0, 1.0f, 0);
    ((SetVP_t)vt[rd.setVp])(dev, &old);
    return r;
}

// address of Game.dll's import slot for dll!name (0 if it has none)
static u32 FindImport(const char* dll, const char* name)
{
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(g_base + d->Name), dll)) continue;
        u32* names = (u32*)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (int i = 0; names[i]; i++)
            if (!(names[i] & 0x80000000) && !strcmp((const char*)(g_base + names[i] + 2), name))
                return g_base + d->FirstThunk + i * 4;
    }
    return 0;
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
    g_menuEdges = GetPrivateProfileIntA(INI_SECTION, "MenuLayout", 3, ini);
    { char k[32]; GetPrivateProfileStringA(INI_SECTION, "MenuAspect", "", k, sizeof k, ini);   // e.g. 16:9 or 1.78
      int aw = 0, ah = 0; double v = 0;
      if (sscanf(k, "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0) v = (double)aw / ah; else v = atof(k);
      g_menuAspectOpt = v; }
    { char k[32]; GetPrivateProfileStringA(INI_SECTION, "CinematicAspect", "", k, sizeof k, ini);   // e.g. 16:9 or 1.78
      int aw = 0, ah = 0; double v = 0;
      if (sscanf(k, "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0) v = (double)aw / ah; else v = atof(k);
      g_cineAspectOpt = v; }
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
    { char k[64]; GetPrivateProfileStringA(INI_SECTION, "HideUIKey", "Ctrl+H", k, sizeof k, ini);
      g_hideKey = k[0] ? ParseKey(k, &g_hideMods) : 0;
      if (k[0] && !g_hideKey) logf_("HideUIKey=%s not understood, off", k); }
    g_loadFull = GetPrivateProfileIntA(INI_SECTION, "LoadingFullScreen", 0, ini);
    g_scoreMode = GetPrivateProfileIntA(INI_SECTION, "ScoreScreen", 3, ini);
    if (g_scoreMode < 1 || g_scoreMode > 3) g_scoreMode = 3;
    { int pct = GetPrivateProfileIntA(INI_SECTION, "UIScale", 100, ini); if (pct < 50) pct = 50; if (pct > 150) pct = 150; g_uiScale = pct / 100.0f; }
    g_dotaOrig = GetPrivateProfileIntA(INI_SECTION, "DotAOriginal", 0, ini);
    if (g_fpsLimit < 0 || g_fpsLimit > 1000) g_fpsLimit = 0;
    g_movieRenderer = GetPrivateProfileIntA(INI_SECTION, "MovieRenderer", 1, ini);
    g_subSize = GetPrivateProfileIntA(INI_SECTION, "MovieSubtitleSize", 100, ini);
    if (g_subSize < 25) g_subSize = 25;
    if (g_subSize > 400) g_subSize = 400;
    g_subBright = GetPrivateProfileIntA(INI_SECTION, "MovieSubtitleBrightness", 100, ini);
    if (g_subBright < 10) g_subBright = 10;
    if (g_subBright > 100) g_subBright = 100;
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


// ---- DracoL1ch's DotA Allstars maps build their own interface with helper DLLs (DotAAllstarsHelper /
//      WarCraftHelper): they place it for the original 0.8-wide screen, partly correct it for the screen width
//      themselves and project the world with their own WideScreen option. With DotAOriginal=1 the mod steps aside
//      while such a map runs: original interface width, no frame moves, world projection, health bar or frame
//      rate changes. The map is recognised by its script naming the helper; the script is read during loading,
//      seconds before the game interface is built. ----
static double g_wideD; static float g_wideE, g_pickTopWide; static int g_barLevelPatched;
// 1.27 has the ortho edges of the UI projection as immediates in the code: rewrite them whenever the size changes
static int g_orthoPatched;
static void SyncImmediates(void)
{
    if (!g_orthoPatched || !G->orthoImm) return;
    if (*(float*)(g_base + G->orthoW) != g_uiW_f) WriteMem(g_base + G->orthoW, &g_uiW_f, 4);
    if (g_uiScale != 1.0f && *(float*)(g_base + G->orthoH) != g_uiH_f) WriteMem(g_base + G->orthoH, &g_uiH_f, 4);
}
static void SetDotAMode(int on)
{
    if (on == g_dotaMode || !g_wideD) return;
    if (on) {
        g_enabled = 0; g_dotaMode = 1;
        g_uiW_d = 0.8; g_uiW_f = 0.8f; g_e = 0.0f; g_pickTop = 0.577f; g_uiH_d = 0.6; g_uiH_f = 0.6f; g_f = 0.0f;
        SyncImmediates();
        if (g_barLevelPatched) WriteMem(g_base + G->barRaiseAt, &G->barRaiseOn, 1);
        logf_("DotA Allstars map (DracoL1ch): mod off while it runs (DotAOriginal=1)");
    } else {
        g_uiW_d = g_wideD; g_uiW_f = (float)g_wideD; g_e = g_wideE; g_pickTop = g_pickTopWide;
        g_uiH_d = 0.6; g_uiH_f = 0.6f; g_f = 0.0f;
        SyncImmediates();
        if (g_barLevelPatched) WriteMem(g_base + G->barRaiseAt, &G->barRaiseOff, 1);
        g_dotaMode = 0; g_enabled = 1;
        logf_("DotA Allstars map left: mod on again");
    }
}
static int g_dotaGameSeen;
#include <tlhelp32.h>
static void MapWidescreenOption(int suppress)
{
    static uint8_t* fn; static u32* flag; static uint8_t saved[5]; static u32 savedFlag; static int active;
    if (suppress && !active) {
        static DWORD lastTry; static int bad; DWORD now = GetTickCount();
        if (bad) return;
        if (lastTry && now - lastTry < 1000) return;
        lastTry = now;
        fn = NULL;
        HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        MODULEENTRY32 me; me.dwSize = sizeof me;
        if (sn != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Module32First(sn, &me); ok && !fn; ok = Module32Next(sn, &me))
                fn = (uint8_t*)GetProcAddress(me.hModule, "SetWidescreenFixState");
            CloseHandle(sn);
        }
        if (!fn) return;                                                    // helper not loaded yet: next time
        if (memcmp(fn, "\x55\x8B\xEC\x8B\x45\x08\xA3", 7)) { logf_("map WideScreen option not recognised"); bad = 1; return; }
        flag = *(u32**)(fn + 7);
        if (IsBadWritePtr(flag, 4)) { logf_("map WideScreen option not recognised"); return; }
        memcpy(saved, fn, 5); savedFlag = *flag;
        static const uint8_t noop[5] = { 0x31, 0xC0, 0xC2, 0x04, 0x00 };     // xor eax,eax; ret 4
        WriteMem((u32)fn, noop, 5); *flag = 0; active = 1;
        logf_("map WideScreen option off while the map runs (was %u): the mod's projection gives the same view", savedFlag);
    } else if (!suppress && active) {
        WriteMem((u32)fn, saved, 5); *flag = savedFlag; active = 0;
    }
}
static void MapWidescreenOptionPoll(void)
{
    if (g_dotaRun) MapWidescreenOption(1);
}
static void SetDotARun(int on)
{
    if (on == g_dotaRun) return;
    static const uint8_t lvlRaise = 0x01, lvlKeep = 0x00;
    (void)lvlRaise; (void)lvlKeep;
    g_dotaRun = on;
    MapWidescreenOption(on);
    logf_(on ? "DotA Allstars map with the mod on (DotAOriginal=0)" : "DotA Allstars map left");
}
static void SetUIScaleActive(int on)
{
    if (g_uiScale == 1.0f || on == g_scaleOn || (on && (g_dotaMode || !g_enabled))) return;
    g_scaleOn = on;
    g_uiH_d = on ? 0.6 / g_uiScale : 0.6; g_uiH_f = (float)g_uiH_d; g_f = (float)(g_uiH_d - 0.6);
    g_uiW_d = g_uiH_d * g_aspect; g_uiW_f = (float)g_uiW_d; g_e = (float)((g_uiW_d - 0.8) * 0.5);
    SyncImmediates();
    if (!on) g_scaleGameSeen = 0;
    logf_("UI scale %s: screen %.4f x %.4f", on ? "on" : "off", g_uiW_d, g_uiH_d);
    u32 r = g_screenRoot;
    if (r && !IsBadReadPtr((void*)r, 0x60)) {
        typedef void (__fastcall *SetSize_t)(u32 self, u32 edx, float v);
        ((SetSize_t)(g_base + G->setWidth))(r, 0, g_uiW_f);
        ((SetSize_t)(g_base + G->setHeight))(r, 0, g_uiH_f);
    }

}
// MenuLayout=0: the menus as in the original game, stretched over the wide screen. While the menus are up the
// interface is 0.8 wide again (screen frame, projection, mouse), nothing is moved or replaced; the loading screen
// switches to the wide interface, the menus after a game switch back.
static int g_narrowWant, g_narrowGameSeen; static u32 g_narrowRoot;
static void NarrowRootSize(void)
{
    u32 r = g_screenRoot;
    if (!r || IsBadReadPtr((void*)r, 0x60)) return;
    g_narrowRoot = r;
    typedef void (__fastcall *SetSize_t)(u32 self, u32 edx, float v);
    ((SetSize_t)(g_base + G->setWidth))(r, 0, g_uiW_f);
}
static void SetGlueNarrow(int on)
{
    if (!g_narrowWant || on == g_narrowOn || (on && g_dotaMode)) return;
    g_narrowOn = on;
    if (on) { g_uiW_d = 0.8; g_uiW_f = 0.8f; g_e = 0.0f; }
    else { g_uiW_d = g_uiH_d * g_aspect; g_uiW_f = (float)g_uiW_d; g_e = (float)((g_uiW_d - 0.8) * 0.5); }
    SyncImmediates();
    NarrowRootSize();
    logf_("menus %s: screen %.4f x %.4f", on ? "as in the original (stretched)" : "left, wide interface", g_uiW_d, g_uiH_d);
}
static void DotAModeCheckExit(u32 parent)
{
    if (IsGameUIRoot(parent)) { g_dotaGameSeen = 1; return; }
    u32 ui = g_gameUI;
    int uiAlive = ui && !IsBadReadPtr((void*)ui, 4) && *(u32*)ui == g_base + VT(GameUI);
    if (g_dotaGameSeen && !uiAlive && IsScreenRoot(parent)) { g_dotaGameSeen = 0; SetDotAMode(0); SetDotARun(0); }
}
// The script is also read when a map is only selected in the menus (lobby settings): remember the result and switch
// when that map's loading screen (CLoading) is laid out; back to normal when its score screen (CScoreScreen)
// appears, or at the menus if a game ends without one.
static int g_dotaPending;
static void SetDotAPending(int found)
{
    g_dotaPending = found;
    if (found && g_loadingActive && !g_dotaMode && g_dotaOrig) SetDotAMode(1);     // loading screen already up (map started directly)
}
static void DotAFrameEvent(u32 frame, u32 parent)
{
    u32 vt = VtRva(frame);
    if (vt == VT(Loading) || VtRva(parent) == VT(Loading)) {               // CLoading (or a frame placed on it)
        if (!g_loadingActive) logf_("loading screen (DotA map selected: %d)", g_dotaPending);
        SetGlueNarrow(0);
        if (!g_scaleOn && !(g_dotaPending && g_dotaOrig)) SetUIScaleActive(1);
        g_loadingActive = 1; g_dotaLoading = g_dotaPending;
    } else if (vt == VT(Score)) {                                     // CScoreScreen
        g_loadingActive = 0; g_dotaLoading = 0; g_scoreActive = 1;
        if (g_dotaMode) SetDotAMode(0);
        if (g_dotaRun) SetDotARun(0);
    }
    // the score screen keeps its original size: its parts can be placed on it before the screen frame itself
    if (vt == VT(Score) || VtRva(parent) == VT(Score)) SetUIScaleActive(0);
    if (g_scaleOn) {
        if (IsGameUIRoot(parent)) g_scaleGameSeen = 1;
        else if (g_scaleGameSeen && !g_loadingActive && IsScreenRoot(parent)) {
            u32 ui = g_gameUI;
            if (!(ui && !IsBadReadPtr((void*)ui, 4) && *(u32*)ui == g_base + VT(GameUI))) SetUIScaleActive(0);
        }
    }
    if (IsGameUIRoot(parent)) {
        if (g_dotaPending && !g_dotaMode && !g_dotaRun && g_loadingActive) {
            if (g_dotaOrig) { logf_("DotA map: switched at the game interface"); SetDotAMode(1); }
            else SetDotARun(1);
        }
        g_loadingActive = 0; g_dotaLoading = 0;
    }
    if (g_dotaMode || g_dotaRun) DotAModeCheckExit(parent);
    if (g_narrowWant) {
        if (g_narrowOn) { if (IsScreenRoot(parent) && parent != g_narrowRoot) NarrowRootSize(); }   // (a new screen frame)
        else if (IsGameUIRoot(parent)) g_narrowGameSeen = 1;
        else if (g_narrowGameSeen && !g_loadingActive && vt != VT(Score) && VtRva(parent) != VT(Score) && IsScreenRoot(parent)) {
            u32 ui = g_gameUI;
            if (!(ui && !IsBadReadPtr((void*)ui, 4) && *(u32*)ui == g_base + VT(GameUI))) { g_narrowGameSeen = 0; SetGlueNarrow(1); }
        }
    }
}
// a melee loading picture of a Battle.net server (Eurobattle.net) scaled by UIScale / LoadingFullScreen: its frame
// around the minimap has chains and a plate attached and can't be moved to the minimap, so the minimap is fitted into
// the scaled frame instead (square, centred in its inside: texels 72..278 x 227..432 of the top left quad)
static void FitLoadingMinimap(float fx, float fy)
{
    u32 f = g_loadMinimap, p = g_loadMinimapParent;
    if (!f || !p || IsBadReadPtr((void*)f, 4) || IsBadReadPtr((void*)p, 4) || VtRva(p) != VT(Loading)) return;
    #define LC_X(t) ((((t) * 0.4f / 512.0f) - 0.4f) * fx + 0.4f * fy)
    #define LC_Y(t) ((0.6f - (t) * 0.4f / 512.0f) * fy)
    float x0 = LC_X(72.0f), x1 = LC_X(278.0f), y0 = LC_Y(432.0f), y1 = LC_Y(227.0f);
    #undef LC_X
    #undef LC_Y
    float sd = fminf(x1 - x0, y1 - y0), cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    // (the minimap frame has its own size, a second point does not resize it)
    typedef void (__fastcall *SetSize_t)(u32 self, u32 edx, float v);
    ((SetSize_t)(g_base + G->setWidth))(f, 0, sd);
    ((SetSize_t)(g_base + G->setHeight))(f, 0, sd);
    ClearAllPoints(f, 0);
    orig_SetFramePoint(f, 0, 4, p, 6, cx, cy, 1);
    logf_("melee loading screen: minimap fitted into the custom picture's frame (%.4f square)", sd);
    // the loading bar is drawn by its model about 0.06 above its frame (in the bar's slot of the picture), at its own
    // size: as the picture grows by fy, raise it with the slot (it and its text)
    float dy = 0.06f * (fy - 1.0f);
    for (int t = 0; t < 2 && dy > 0.0f; t++) {
        u32 b = g_loadBar[t], bp = g_loadBarParent[t];
        if (!b || !bp || IsBadReadPtr((void*)b, 4) || bp != p) continue;
        u32 pt = t ? 7 : 6;
        orig_SetFramePoint(b, 0, pt, bp, pt, g_loadBarX[t], g_loadBarY[t] + dy, 1);
    }
}
static char g_loadModel[MAX_PATH];        // custom loading screen model of the selected DotA map (from war3map.w3i)
static void ReadLoadingModel(HANDLE mpq, const char* scriptName, DWORD scope)
{
    char w3i[MAX_PATH]; lstrcpynA(w3i, scriptName, MAX_PATH);
    char* b = strrchr(w3i, '\\'); b = b ? b + 1 : w3i;
    strcpy(b, "war3map.w3i");
    g_loadModel[0] = 0;
    HANDLE h = 0;
    // war3map.w3i is at the map's root; the script may be there or in Scripts\ (DotA 6.8x)
    if ((!orig_SOpenEx(mpq, w3i, scope, &h) || !h) && (h = 0, !orig_SOpenEx(mpq, "war3map.w3i", scope, &h) || !h)) return;
    DWORD n = S_Size(h, NULL), got = 0;
    char* buf = (n > 0 && n < (1u << 20)) ? (char*)malloc(n + 1) : NULL;
    if (buf && S_Read(h, buf, n, &got, NULL) && got == n) {
        buf[n] = 0;
        for (DWORD i = 0; i < n; ) {                        // first zero-terminated string ending in .mdx / .mdl
            DWORD len = (DWORD)strnlen(buf + i, n - i);
            if (len > 4 && len < MAX_PATH && (!_stricmp(buf + i + len - 4, ".mdx") || !_stricmp(buf + i + len - 4, ".mdl"))) {
                DWORD st = i; for (DWORD k = i; k < i + len; k++) if ((unsigned char)buf[k] < 0x20 || (unsigned char)buf[k] > 0x7E) st = k + 1;   // the path follows binary fields
                lstrcpynA(g_loadModel, buf + st, MAX_PATH); break;
            }
            i += len + 1;
        }
    }
    free(buf); S_Close(h);
    if (g_loadModel[0]) dlogf_("map loading screen model %s", g_loadModel);
}
static void CheckMapScript(HANDLE mpq, const char* name, DWORD scope)
{
    if (!g_wideD || !S_Read || !S_Size || !S_Close || !G->dota) return;   // (the DotA helpers exist for 1.26 only)
    const char* b = strrchr(name, '\\'); b = b ? b + 1 : name;
    if (!SameName(b, "war3map.j")) return;
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return;
    DWORD n = S_Size(h, NULL), got = 0;
    static DWORD lastSize; static int lastResult;
    if (n && n == lastSize) { S_Close(h); SetDotAPending(lastResult); return; }   // the game opens it several times
    char* buf = (n > 0 && n < (64u << 20)) ? (char*)malloc(n) : NULL;
    int found = 0;
    if (buf && S_Read(h, buf, n, &got, NULL) && got == n) {
        static const char key[] = "DotAAllstarsHelper";
        for (DWORD i = 0; i + sizeof key - 1 <= n && !found; i++)
            if (buf[i] == 'D' && !memcmp(buf + i, key, sizeof key - 1)) found = 1;
    }
    free(buf); S_Close(h);
    lastSize = n; lastResult = found;
    ReadLoadingModel(mpq, name, scope);
    SetDotAPending(found);
}

// stretch the vertex x of every geoset to the screen width; the model is drawn relative to the loading screen frame,
// which stays in the centred 4:3 area, so also move it left by that area's offset
static int StretchMdxVertices(uint8_t* d, DWORD n, float fx, float fy, float k)
{
    if (n < 16 || memcmp(d, "MDLX", 4) != 0) return 0;
    int changed = 0; DWORD p = 4;
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
                        float xy[2]; memcpy(xy, d + g + 12 + v * 12, 8);
                        xy[0] = (xy[0] - 0.4f) * fx + 0.4f * k; xy[1] = (xy[1] - 0.3f) * fy + 0.3f * k;
                        memcpy(d + g + 12 + v * 12, xy, 8); changed++;
                    }
                }
                g += gsz;
            }
        }
        p += 8 + sz;
    }
    return changed;
}

// copy the geosets that touch the left (x = 0) or right (x = 0.8) edge of the 0.8-wide model and lay the copies over
// [-e, 0] / [0.8, 0.8 + e], textured with the outer `strip` (0..1) of their texture
static uint8_t* AddSideGeosets(const uint8_t* d, DWORD n, float e, float strip, DWORD* outN)
{
    if (n < 16 || memcmp(d, "MDLX", 4)) return NULL;
    DWORD p = 4, gp = 0, gsz = 0;
    while (p + 8 <= n) { DWORD sz = *(DWORD*)(d + p + 4); if (!memcmp(d + p, "GEOS", 4)) { gp = p; gsz = sz; break; } p += 8 + sz; }
    if (!gp || gp + 8 + gsz > n) return NULL;
    uint8_t* extra = (uint8_t*)malloc(gsz * 2 + 16); DWORD en = 0;
    for (DWORD g = gp + 8; g + 4 <= gp + 8 + gsz; ) {
        DWORD sz = *(DWORD*)(d + g);
        if (sz < 12 || g + sz > gp + 8 + gsz) { free(extra); return NULL; }
        // find VRTX and UVBS
        DWORD vrtx = 0, uvbs = 0, cnt = 0;
        for (DWORD q = g + 4; q + 8 <= g + sz; ) {
            const uint8_t* t = d + q; DWORD k = *(DWORD*)(t + 4);
            if (!memcmp(t, "VRTX", 4)) { vrtx = q; cnt = k; q += 8 + k * 12; }
            else if (!memcmp(t, "NRMS", 4)) q += 8 + k * 12;
            else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + k * 4;
            else if (!memcmp(t, "PVTX", 4)) q += 8 + k * 2;
            else if (!memcmp(t, "GNDX", 4)) q += 8 + k;
            else if (!memcmp(t, "MATS", 4)) { q += 8 + k * 4 + 12 + 28; DWORD na = *(DWORD*)(d + q); q += 4 + na * 28; }
            else if (!memcmp(t, "UVAS", 4)) q += 8;
            else if (!memcmp(t, "UVBS", 4)) { uvbs = q; q += 8 + k * 8; }
            else break;
        }
        if (vrtx && uvbs && cnt && *(DWORD*)(d + uvbs + 4) == cnt) {
            float mn = 1e9f, mx = -1e9f;
            for (DWORD v = 0; v < cnt; v++) { float x = *(float*)(d + vrtx + 8 + v * 12); if (x < mn) mn = x; if (x > mx) mx = x; }
            float ymx = -1e9f, zmx = 0.0f;
            for (DWORD v = 0; v < cnt; v++) { float y = *(float*)(d + vrtx + 8 + v * 12 + 4), z = *(float*)(d + vrtx + 8 + v * 12 + 8); if (y > ymx) ymx = y; if (fabsf(z) > zmx) zmx = fabsf(z); }
            int left = fabsf(mn) < 1e-4f && mx <= 0.4001f, right = fabsf(mx - 0.8f) < 1e-4f && mn >= 0.3999f;
            if ((left || right) && zmx < 1e-4f && fabsf(ymx - 0.6f) < 1e-4f && cnt == 4) {   // the two upper tiles
                memcpy(extra + en, d + g, sz);
                uint8_t* c = extra + en;
                for (DWORD v = 0; v < cnt; v++) {
                    float* x = (float*)(c + (vrtx - g) + 8 + v * 12);
                    float* y = x + 1;
                    float* u = (float*)(c + (uvbs - g) + 8 + v * 8);
                    int outer = left ? *x < 0.2f : *x > 0.6f, top = *y > 0.4f;
                    if (left) *x = outer ? -e : 0.0f; else *x = outer ? 0.8f + e : 0.8f;
                    *y = top ? 0.6f : 0.0f;
                    u[0] = outer ? strip : strip + 0.02f;               // a speck of the wood bar on top
                    u[1] = top ? 0.06f : 0.08f;
                }
                en += sz;
            }
        }
        g += sz;
    }
    if (!en) { free(extra); return NULL; }
    uint8_t* o = (uint8_t*)malloc(n + en);
    DWORD end = gp + 8 + gsz;
    memcpy(o, d, end); memcpy(o + end, extra, en); memcpy(o + end + en, d + end, n - end);
    *(DWORD*)(o + gp + 4) = gsz + en;
    free(extra);
    *outN = n + en;
    return o;
}
// ---- score screen: extended background ------------------------------------------------------------------------
// The score screen background is a 0.8 x 0.6 sheet made of four 512-texel tiles (L1 R1 on top, L2 R2 below).
// With LoadingFullScreen the sheet is extended to the screen edges: its right border group (gold line, torn edge,
// frame) moves to the right screen edge, a mirrored copy goes to the left edge, and the gaps are filled with
// textures built here from the game's own tiles: plain sheet with the header line, the wood bar on top and the
// bottom border, tone-matched to their neighbours so no seams show. The left edge of the sheet is darkened in the
// original picture; those columns are brightened in a copy that sits at the end of the left filler.
#define SS_ROWS 768                 // L1/R1 (512) over L2/R2 (256)
#define SS_S0 70                    // narrow source: L columns between the first two header cells
#define SS_S1 160
#define SS_SPLIT 88                 // R rows above: the right group starts at SS_BR (the metal bracket), below: at SS_CR
#define SS_CR 431
#define SS_BR 343
#define SS_PW 48                    // R columns CR-PW..CR-1 are copied (brightened) into the right filler
#define SS_LPW 160                  // L columns 2..LPW-1 are copied (brightened) into the left filler
#define SS_LDIM 0.12                // ... keeping a gentle darkening towards the edge (this much at column 2)
#define SS_B0 141                   // plain parchment rows
#define SS_B1 706
#define SS_X0 136                   // crossfade from the narrow source to the wide one
#define SS_X1 146
#define SS_NW ((SS_S1 - SS_S0) * 2)
#define SS_RB0 392                  // right filler, top rows: R columns of the metal bracket bar (no hanging bits)
#define SS_RB1 452
#define SS_RBN ((SS_RB1 - SS_RB0) * 2)
#define SS_RBH 96                   // ... used above this row, faded out from SS_RBF
#define SS_RBF 82
#define SS_WW ((512 - SS_S0 + 420) * 2)

typedef struct { float* L; float* R; uint8_t pal[2][1024]; uint8_t hdr[2][28]; } SsSrc;

// paletted BLP1 (what the score screen tiles are) -> RGB floats into dst rows [row0, row0 + h)
static int SsReadBlp(const uint8_t* d, DWORD n, float* dst, int row0, int wantH, uint8_t* pal, uint8_t* hdr)
{
    if (n < 156 + 1024 || memcmp(d, "BLP1", 4)) return 0;
    DWORD comp = *(DWORD*)(d + 4), w = *(DWORD*)(d + 12), h = *(DWORD*)(d + 16), off = *(DWORD*)(d + 28);
    if (comp != 1 || w != 512 || (int)h != wantH || off + w * h > n) return 0;
    const uint8_t* p = d + 156;
    for (DWORD y = 0; y < h; y++)
        for (DWORD x = 0; x < w; x++) {
            const uint8_t* c = p + d[off + y * w + x] * 4;           // BGRA
            float* o = dst + ((row0 + y) * 512 + x) * 3;
            o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
        }
    if (pal) memcpy(pal, p, 1024);
    if (hdr) memcpy(hdr, d, 28);
    return 1;
}

// centred box blur with clamped edges, 3 passes (about a Gaussian), over n elements `stride` floats apart
static void SsBlurLine(float* a, int n, int stride, int r, float* tmp)
{
    for (int it = 0; it < 3; it++) {
        double s = 0;
        for (int k = -r; k <= r; k++) s += a[(k < 0 ? 0 : k >= n ? n - 1 : k) * stride];
        for (int i = 0; i < n; i++) {
            tmp[i] = (float)(s / (2 * r + 1));
            int out = i - r, in = i + r + 1;
            s += a[(in >= n ? n - 1 : in) * stride] - a[(out < 0 ? 0 : out) * stride];
        }
        for (int i = 0; i < n; i++) a[i * stride] = tmp[i];
    }
}

static int SsKind(int r)            // 0: gain 1 (wood, bottom edge), 1: measured (parchment), 2: interpolated
{
    if (r < 66 || r >= 746) return 0;
    if ((r >= 78 && r < 98) || (r >= SS_B0 && r < 746)) return 1;
    return 2;
}

// per-row gain that brings `own` (the filler's tone) to `target`, smooth over rows
static void SsGains(const float* target, const float* own, float* g)
{
    static float num[SS_ROWS], den[SS_ROWS], tmp[SS_ROWS];
    for (int ch = 0; ch < 3; ch++) {
        for (int r = 0; r < SS_ROWS; r++) {
            float w = SsKind(r) == 1 ? 1.0f : 0.0f;
            float o = own[r * 3 + ch]; if (o < 1) o = 1;
            num[r] = target[r * 3 + ch] / o * w; den[r] = w;
        }
        SsBlurLine(num, SS_ROWS, 1, 24, tmp); SsBlurLine(den, SS_ROWS, 1, 24, tmp);
        float col[SS_ROWS]; int ok[SS_ROWS];
        for (int r = 0; r < SS_ROWS; r++) {
            int k = SsKind(r);
            ok[r] = k != 2;
            col[r] = k == 1 ? num[r] / (den[r] > 1e-6f ? den[r] : 1e-6f) : 1.0f;
        }
        for (int r = 0; r < SS_ROWS; r++) {
            float v = col[r];
            if (!ok[r]) {
                int a = r, b = r;
                while (a >= 0 && !ok[a]) a--;
                while (b < SS_ROWS && !ok[b]) b++;
                if (a < 0) v = col[b]; else if (b >= SS_ROWS) v = col[a];
                else v = col[a] + (col[b] - col[a]) * (float)(r - a) / (float)(b - a);
            }
            g[r * 3 + ch] = v < 0.5f ? 0.5f : v > 1.6f ? 1.6f : v;
        }
    }
}

typedef struct {
    float* tileN;                   // SS_ROWS x SS_NW
    float* tileW;                   // SS_ROWS x SS_WW
    float patch[SS_ROWS][SS_LPW - 2][3];  // L columns 2..LPW-1, brightened
    float rpatch[SS_ROWS][SS_PW][3];      // R columns CR-PW..CR-1, brightened
    float gN[SS_ROWS][3], gP[SS_ROWS][3], gR[SS_ROWS][3], gNR[SS_ROWS][3], mix[SS_ROWS];
    float tileB[SS_RBH][SS_RBN][3];       // the right filler's top rows: the bracket bar continued to the border
    const float* R;                       // the source tiles (for the seams and variant 2)
    const float* L;
} SsGen;

static void SsTileAt(const SsGen* G, int r, int col, float* o)
{
    const float* a = G->tileN + ((size_t)r * SS_NW + col % SS_NW) * 3;
    const float* b = G->tileW + ((size_t)r * SS_WW + col % SS_WW) * 3;
    float m = G->mix[r];
    for (int c = 0; c < 3; c++) o[c] = a[c] * (1 - m) + b[c] * m;
}

// the right filler: like SsTileAt, with the bracket bar of the R tile in the top rows
static float SsMixB(int r) { return r < SS_RBF ? 1.0f : r >= SS_RBH ? 0.0f : 1.0f - (float)(r - SS_RBF) / (SS_RBH - SS_RBF); }
static void SsTileAtR(const SsGen* G, int r, int col, float* o)
{
    SsTileAt(G, r, col, o);
    float m = SsMixB(r);
    if (m > 0) for (int c = 0; c < 3; c++) o[c] = o[c] * (1 - m) + G->tileB[r][col % SS_RBN][c] * m;
}

static int SsPrepare(SsGen* G, const SsSrc* S)
{
    const float* L = S->L; const float* R = S->R;
    G->R = R; G->L = L;
    G->tileN = (float*)malloc((size_t)SS_ROWS * SS_NW * 3 * sizeof(float));
    G->tileW = (float*)malloc((size_t)SS_ROWS * SS_WW * 3 * sizeof(float));
    const int ww = SS_WW / 2;
    float* low = (float*)malloc((size_t)SS_ROWS * ww * 3 * sizeof(float));
    float* tmp = (float*)malloc(sizeof(float) * (SS_ROWS > ww ? SS_ROWS : ww));
    if (!G->tileN || !G->tileW || !low || !tmp) { free(low); free(tmp); return 0; }
    // narrow source, its columns brought to one tone, mirrored
    const int nw = SS_S1 - SS_S0;
    double cm[SS_S1 - SS_S0][3] = {{0}}, all[3] = {0};
    for (int r = SS_B0; r < SS_B1; r++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 3; c++) cm[x][c] += L[(r * 512 + SS_S0 + x) * 3 + c];
    for (int x = 0; x < nw; x++) for (int c = 0; c < 3; c++) all[c] += cm[x][c] / nw;
    for (int r = 0; r < SS_ROWS; r++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 3; c++) {
                float v = L[(r * 512 + SS_S0 + x) * 3 + c] * (float)(all[c] / (cm[x][c] > 1 ? cm[x][c] : 1));
                G->tileN[((size_t)r * SS_NW + x) * 3 + c] = v;
                G->tileN[((size_t)r * SS_NW + SS_NW - 1 - x) * 3 + c] = v;
            }
    // wide source: the sheet from L column S0 to R column 420, low frequencies removed, mirrored
    #define SSW(r, x, c) ((x) < 512 - SS_S0 ? L[((r) * 512 + SS_S0 + (x)) * 3 + (c)] : R[((r) * 512 + (x) - (512 - SS_S0)) * 3 + (c)])
    for (int r = 0; r < SS_ROWS; r++)
        for (int x = 0; x < ww; x++)
            for (int c = 0; c < 3; c++) low[((size_t)r * ww + x) * 3 + c] = SSW(r < SS_B1 ? r : SS_B1 - 1, x, c);
    for (int x = 0; x < ww; x++) for (int c = 0; c < 3; c++) SsBlurLine(low + x * 3 + c, SS_ROWS, ww * 3, 16, tmp);
    for (int r = 0; r < SS_ROWS; r++) for (int c = 0; c < 3; c++) SsBlurLine(low + (size_t)r * ww * 3 + c, ww, 3, 16, tmp);
    double lm[3] = {0};
    for (int r = SS_B0; r < SS_B1; r++) for (int x = 0; x < ww; x++) for (int c = 0; c < 3; c++) lm[c] += low[((size_t)r * ww + x) * 3 + c];
    for (int c = 0; c < 3; c++) lm[c] /= (double)(SS_B1 - SS_B0) * ww;
    for (int r = 0; r < SS_ROWS; r++) {
        int lr = r < SS_B0 ? SS_B0 : r;
        for (int x = 0; x < ww; x++)
            for (int c = 0; c < 3; c++) {
                float lo = low[((size_t)lr * ww + x) * 3 + c];
                float v = SSW(r, x, c) * (float)(lm[c] / (lo > 1 ? lo : 1));
                G->tileW[((size_t)r * SS_WW + x) * 3 + c] = v;
                G->tileW[((size_t)r * SS_WW + SS_WW - 1 - x) * 3 + c] = v;
            }
    }
    #undef SSW
    free(low); free(tmp);
    // tone of the filler per row
    static float own[SS_ROWS][3], ownR[SS_ROWS][3], nearT[SS_ROWS][3], nearR[SS_ROWS][3], pref[SS_ROWS][3], rref[SS_ROWS][3];
    for (int r = 0; r < SS_RBH; r++)
        for (int x = 0; x < SS_RB1 - SS_RB0; x++) {
            memcpy(G->tileB[r][x], &R[(r * 512 + SS_RB0 + x) * 3], 12);
            memcpy(G->tileB[r][SS_RBN - 1 - x], &R[(r * 512 + SS_RB0 + x) * 3], 12);
        }
    for (int r = 0; r < SS_ROWS; r++) {
        float m = (float)(r - SS_X0) / (SS_X1 - SS_X0);
        G->mix[r] = m < 0 ? 0 : m > 1 ? 1 : m;
        for (int c = 0; c < 3; c++) {
            double a = 0, b = 0;
            for (int x = 0; x < SS_NW; x++) a += G->tileN[((size_t)r * SS_NW + x) * 3 + c];
            for (int x = 0; x < SS_WW; x++) b += G->tileW[((size_t)r * SS_WW + x) * 3 + c];
            own[r][c] = (float)(a / SS_NW * (1 - G->mix[r]) + b / SS_WW * G->mix[r]);
            double bb = 0; float mb = SsMixB(r);
            if (mb > 0) for (int x = 0; x < SS_RBN; x++) bb += G->tileB[r][x][c];
            ownR[r][c] = own[r][c] * (1 - mb) + (float)(bb / SS_RBN) * mb;
        }
    }
    // brightened copies of the sheet's left edge and of the right edge of the part that stays
    for (int side = 0; side < 2; side++) {
        const float* T = side ? R : L;
        int c0 = side ? SS_CR - SS_PW : 2, c1 = side ? SS_CR : SS_LPW, rc = side ? SS_CR - SS_PW - 4 : SS_LPW;   // rc: reference columns
        double m[512][3];
        for (int x = 0; x < 512; x++) for (int c = 0; c < 3; c++) m[x][c] = 0;
        for (int r = SS_B0; r < SS_B1; r++) for (int x = (c0 < rc ? c0 : rc); x < (c1 > rc + 4 ? c1 : rc + 4); x++) for (int c = 0; c < 3; c++) m[x][c] += T[(r * 512 + x) * 3 + c];
        for (int c = 0; c < 3; c++) {
            double ref = (m[rc][c] + m[rc + 1][c] + m[rc + 2][c] + m[rc + 3][c]) / 4;
            for (int x = c0; x < c1; x++) {
                double ms = 0; int mn = 0;                    // column tone, smoothed over a few columns
                int sr = x - c0 < 12 || c1 - x < 12 ? 0 : 4;   // not where the edge darkening is steep
                for (int k = x - sr; k <= x + sr; k++) if (k >= c0 && k < c1) { ms += m[k][c]; mn++; }
                ms /= mn;
                double tgt = side ? ref : ref * (1 - SS_LDIM * (1 - (double)(x - c0) / (c1 - c0)));
                double g = tgt / (ms > 1 ? ms : 1);
                g = g < 1 ? 1 : g > 2.5 ? 2.5 : g;
                for (int r = 0; r < SS_ROWS; r++) {
                    // only on the sheet: not on the wood bar, the header cells or below the bottom border; feathered
                    float k = r < 66 ? 0.0f : r < 72 ? (r - 66) / 6.0f : r < 94 ? 1.0f : r < 98 ? (98 - r) / 4.0f : r <= 136 ? 0.0f :
                              r <= 140 ? (r - 136) / 4.0f : r < 740 ? 1.0f : r < 750 ? (750 - r) / 10.0f : 0.0f;
                    if (!side && x >= 68 && r >= 94 && r <= 140) k = 1.0f;   // left: the header cell ends at column 66
                    float v = (float)(T[(r * 512 + x) * 3 + c] * (1 + (g - 1) * k));
                    if (v > 255) v = 255;
                    if (side) G->rpatch[r][x - c0][c] = v; else G->patch[r][x - c0][c] = v;
                }
            }
        }
    }
    for (int r = 0; r < SS_ROWS; r++) {
        int cc = r < SS_SPLIT ? SS_BR : SS_CR;
        for (int c = 0; c < 3; c++) {
            nearT[r][c] = (R[(r * 512 + cc - 4) * 3 + c] + R[(r * 512 + cc - 3) * 3 + c] + R[(r * 512 + cc - 2) * 3 + c] + R[(r * 512 + cc - 1) * 3 + c]) / 4;
            pref[r][c] = (G->patch[r][0][c] + G->patch[r][1][c] + G->patch[r][2][c] + G->patch[r][3][c]) / 4;
            nearR[r][c] = (R[(r * 512 + SS_CR - 4) * 3 + c] + R[(r * 512 + SS_CR - 3) * 3 + c] + R[(r * 512 + SS_CR - 2) * 3 + c] + R[(r * 512 + SS_CR - 1) * 3 + c]) / 4;
            rref[r][c] = (G->rpatch[r][SS_PW - 4][c] + G->rpatch[r][SS_PW - 3][c] + G->rpatch[r][SS_PW - 2][c] + G->rpatch[r][SS_PW - 1][c]) / 4;
        }
    }
    SsGains(&nearT[0][0], &own[0][0], &G->gN[0][0]);
    SsGains(&pref[0][0], &own[0][0], &G->gP[0][0]);
    SsGains(&rref[0][0], &ownR[0][0], &G->gR[0][0]);
    SsGains(&nearR[0][0], &ownR[0][0], &G->gNR[0][0]);
    return 1;
}


// match a filler to its neighbour at one seam: per row, the ratio of the neighbour's tone (ref) to the filler's first
// columns, smoothed over a few rows (keeps thin lines like the bottom border), faded out over `n` columns.
// x0: the filler column at the seam, dir: +1 when the filler continues to the right, -1 to the left; xs: per-row x0
// offset (rows above SS_SPLIT may start elsewhere), NULL for none
static void SsSeam(float* img, int w, int x0, int dir, int n, const float (*ref)[3], const int* xs)
{
    static float ratio[3][SS_ROWS], tmp[SS_ROWS];
    for (int r = 0; r < SS_ROWS; r++) {
        int a = x0 + (xs ? xs[r] : 0);
        for (int c = 0; c < 3; c++) {
            float cur = 0;
            for (int k = 0; k < 3; k++) cur += img[((size_t)r * w + a + k * dir) * 3 + c] / 3;
            float q = ref[r][c] / (cur > 1 ? cur : 1);
            if (r >= 97 && r <= 140) q = 1;                     // header row: cells on one side, a line on the other
            ratio[c][r] = q < 0.7f ? 0.7f : q > 1.4f ? 1.4f : q;
        }
    }
    for (int c = 0; c < 3; c++) { SsBlurLine(ratio[c], SS_ROWS, 1, 1, tmp); }
    for (int r = 0; r < SS_ROWS; r++) {
        int a = x0 + (xs ? xs[r] : 0);
        for (int j = 0; j < n; j++) {
            float f = 1.0f - (float)j / n; f = f * f * (3 - 2 * f);
            int x = a + j * dir;
            if (x < 0 || x >= w) break;
            for (int c = 0; c < 3; c++) {
                float* p = &img[((size_t)r * w + x) * 3 + c];
                float v = *p * (1 + (ratio[c][r] - 1) * f);
                *p = v > 255 ? 255 : v;
            }
        }
    }
}
#define SS_BW (SS_CR - SS_BR)        // the top rows of R columns BR..CR-1 (bracket end, torn edge) go into the fillers
#define SS_RFW(wt) (SS_PW + (wt))
#define SS_LFW(wl) ((wl) + SS_LPW - 2)
// right filler: [R edge copy (PW) | filler (wt) | top rows: R columns BR..CR-1 (BW)]
// left filler:  [top rows: R columns CR-1..BR mirrored (up to BW), then filler (wl in all) | L edge copy (PW-2)]
// RGB, SS_ROWS rows each. The R block is toned to the filler below it, so the group's top no longer meets the
// lighter filler in a horizontal seam.
static void SsBuild(const SsGen* G, int wt, int wl, int leftGroup, float* rf, float* lf)
{
    const int lw = SS_LFW(wl), rw = SS_RFW(wt), bw = !leftGroup ? 0 : wl < SS_BW ? wl : SS_BW;
    const float* R = G->R;
    float t3[3];
    #define PX(img, w, r, x) (&(img)[((size_t)(r) * (w) + (x)) * 3])
    for (int r = 0; r < SS_ROWS; r++) {
        for (int x = 0; x < SS_PW; x++) memcpy(PX(rf, rw, r, x), G->rpatch[r][x], 12);
        for (int x = 0; x < wt; x++) {
            float t = (x + 0.5f) / wt;
            SsTileAtR(G, r, x, t3);
            for (int c = 0; c < 3; c++) {
                float v = t3[c] * (G->gR[r][c] * (1 - t) + G->gNR[r][c] * t);
                PX(rf, rw, r, SS_PW + x)[c] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
        }
        for (int x = 0; x < wl; x++) {
            float t = (x + 0.5f) / wl;
            SsTileAt(G, r, x + SS_WW / 2, t3);
            for (int c = 0; c < 3; c++) {
                float v = t3[c] * (G->gN[r][c] * (1 - t) + G->gP[r][c] * t);
                PX(lf, lw, r, x)[c] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
        }
        if (r < SS_SPLIT) for (int x = 0; x < bw; x++) memcpy(PX(lf, lw, r, x), &R[(r * 512 + SS_CR - 1 - x) * 3], 12);
        for (int x = 0; x < SS_LPW - 2; x++) memcpy(PX(lf, lw, r, wl + x), G->patch[r][x], 12);
    }
    // the top rows (above SS_SPLIT) sit over different columns than the rows below them (the R block, and on the
    // right the filler starts at BR on top but at CR-PW below): tone the sheet rows above the split to what lies
    // right below, per column (smoothed), fading in from the torn edge
    for (int side = 1; side < 2; side++) {                  // (left only: on the right the columns no longer shift)
        float* img = side ? lf : rf; int w = side ? lw : rw, xa = side ? 0 : SS_PW, xb = side ? wl : rw;
        int n = xb - xa;
        if (n <= 0) continue;
        float* gg = (float*)malloc(sizeof(float) * 3 * n * 2);
        if (!gg) continue;
        float* tmp = gg + 3 * n;
        for (int x = 0; x < n; x++) {
            int s = SS_BR + x;                                // right: texel column of the sheet on screen
            for (int c = 0; c < 3; c++) {
                double up = 0, dn = 0;
                for (int r = SS_SPLIT - 8; r < SS_SPLIT; r++) up += PX(img, w, r, xa + x)[c];
                for (int r = SS_SPLIT; r < SS_SPLIT + 8; r++)
                    dn += side ? PX(img, w, r, x)[c] : s < SS_CR - SS_PW ? R[(r * 512 + s) * 3 + c] : PX(rf, rw, r, s - (SS_CR - SS_PW))[c];
                float g = (float)(dn / (up > 1 ? up : 1));
                gg[x * 3 + c] = g < 0.7f ? 0.7f : g > 1.4f ? 1.4f : g;
            }
        }
        for (int c = 0; c < 3; c++) SsBlurLine(gg + c, n, 3, 8, tmp);
        for (int r = 60; r < SS_SPLIT; r++) {
            float k = (r - 60) / (float)(SS_SPLIT - 1 - 60);
            for (int x = 0; x < n; x++) for (int c = 0; c < 3; c++) {
                float* p = PX(img, w, r, xa + x); float v = p[c] * (1 + (gg[x * 3 + c] - 1) * k); p[c] = v > 255 ? 255 : v;
            }
        }
        free(gg);
    }
    // the bottom gold line: drawn slightly differently along the sheet, so in the fillers it runs from the
    // neighbour's line on one end to the other's
    for (int r = 706; r <= 713; r++) {
        float a3[3], b3[3];
        for (int c = 0; c < 3; c++) {
            a3[c] = (G->rpatch[r][SS_PW - 1][c] + G->rpatch[r][SS_PW - 2][c]) / 2; b3[c] = (R[(r * 512 + SS_CR) * 3 + c] + R[(r * 512 + SS_CR + 1) * 3 + c]) / 2;
        }
        for (int x = 0; x < wt; x++) { float t = (x + 0.5f) / wt; for (int c = 0; c < 3; c++) PX(rf, rw, r, SS_PW + x)[c] = a3[c] * (1 - t) + b3[c] * t; }
        for (int c = 0; c < 3; c++) a3[c] = (G->patch[r][0][c] + G->patch[r][1][c]) / 2;
        for (int x = 0; x < wl; x++) { float t = (x + 0.5f) / wl; for (int c = 0; c < 3; c++) PX(lf, lw, r, x)[c] = b3[c] * (1 - t) + a3[c] * t; }
    }
    // seams: each filler end against what lies next to it
    static float ref[SS_ROWS][3]; static int xs[SS_ROWS];
    int nr = wt / 3 < 24 ? wt / 3 : 24, nl = (wl - bw) / 3 < 24 ? (wl - bw) / 3 : 24, nl2 = wl / 3 < 24 ? wl / 3 : 24;
    #define RAVG(r, c0, ch) ((R[((r) * 512 + (c0)) * 3 + (ch)] + R[((r) * 512 + (c0) + 1) * 3 + (ch)]) / 2)
    for (int r = 0; r < SS_ROWS; r++) for (int c = 0; c < 3; c++)       // right filler, left end: the R main / its edge copy
        ref[r][c] = (G->rpatch[r][SS_PW - 1][c] + G->rpatch[r][SS_PW - 2][c]) / 2;
    SsSeam(rf, rw, SS_PW, +1, nr, ref, NULL);
    for (int r = 0; r < SS_ROWS; r++) for (int c = 0; c < 3; c++)       // right filler, right end: the block / the group
        ref[r][c] = RAVG(r, SS_CR, c);
    SsSeam(rf, rw, SS_PW + wt - 1, -1, nr, ref, NULL);
    for (int r = 0; r < SS_ROWS; r++) for (int c = 0; c < 3; c++)       // left filler, right end: the left edge copy
        ref[r][c] = (G->patch[r][0][c] + G->patch[r][1][c]) / 2;
    SsSeam(lf, lw, wl - 1, -1, nl2, ref, NULL);
    if (leftGroup && wl - bw >= 4) {                                    // left filler, left end: the block / the group
        for (int r = 0; r < SS_ROWS; r++) {
            xs[r] = r < SS_SPLIT ? bw : 0;
            for (int c = 0; c < 3; c++) ref[r][c] = r < SS_SPLIT ? (PX(lf, lw, r, bw - 1)[c] + PX(lf, lw, r, bw - 2 < 0 ? 0 : bw - 2)[c]) / 2 : RAVG(r, SS_CR, c);
        }
        SsSeam(lf, lw, 0, +1, nl < nl2 ? nl2 : nl2, ref, xs);
    }
    #undef RAVG
    #undef PX
}


// a 256-colour palette for an image (median cut over a 5-bit histogram, colours averaged at full precision), BGRA
static void SsMedianCut(const float* rgb, size_t npx, uint8_t* pal)
{
    static unsigned cnt[32768]; static double sum[32768][3];
    memset(cnt, 0, sizeof cnt); memset(sum, 0, sizeof sum);
    for (size_t i = 0; i < npx; i++) {
        int r = (int)rgb[i * 3], g = (int)rgb[i * 3 + 1], b = (int)rgb[i * 3 + 2];
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        int k = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        cnt[k]++; sum[k][0] += rgb[i * 3]; sum[k][1] += rgb[i * 3 + 1]; sum[k][2] += rgb[i * 3 + 2];
    }
    typedef struct { int lo[3], hi[3]; unsigned n; } Box;
    static Box box[256]; int nb = 1;
    box[0] = (Box){ {0, 0, 0}, {31, 31, 31}, (unsigned)npx };
    #define FOR_BOX(B) for (int a0 = (B).lo[0]; a0 <= (B).hi[0]; a0++) for (int a1 = (B).lo[1]; a1 <= (B).hi[1]; a1++) for (int a2 = (B).lo[2]; a2 <= (B).hi[2]; a2++)
    // shrink every box to its occupied range, then split the most populated splittable box at the median
    for (;;) {
        for (int i = 0; i < nb; i++) {
            Box* B = &box[i]; int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0}; unsigned n = 0;
            FOR_BOX(*B) { unsigned c = cnt[(a0 << 10) | (a1 << 5) | a2]; if (!c) continue; n += c;
                int v[3] = {a0, a1, a2}; for (int d = 0; d < 3; d++) { if (v[d] < lo[d]) lo[d] = v[d]; if (v[d] > hi[d]) hi[d] = v[d]; } }
            if (n) { memcpy(B->lo, lo, sizeof lo); memcpy(B->hi, hi, sizeof hi); }
            B->n = n;
        }
        if (nb >= 256) break;
        int best = -1; double bs = 0;
        for (int i = 0; i < nb; i++) {
            int span = 0; for (int d = 0; d < 3; d++) if (box[i].hi[d] - box[i].lo[d] > span) span = box[i].hi[d] - box[i].lo[d];
            double sc = (double)box[i].n * span;
            if (span > 0 && sc > bs) { bs = sc; best = i; }
        }
        if (best < 0) break;
        Box* B = &box[best]; int d = 0;
        for (int k = 1; k < 3; k++) if (B->hi[k] - B->lo[k] > B->hi[d] - B->lo[d]) d = k;
        unsigned hist[32] = {0};
        FOR_BOX(*B) { int v[3] = {a0, a1, a2}; hist[v[d]] += cnt[(a0 << 10) | (a1 << 5) | a2]; }
        unsigned acc = 0; int cut = B->lo[d];
        for (int v = B->lo[d]; v < B->hi[d]; v++) { acc += hist[v]; cut = v; if (acc * 2 >= B->n) break; }
        Box nbx = *B; B->hi[d] = cut; nbx.lo[d] = cut + 1;
        box[nb++] = nbx;
    }
    memset(pal, 0, 1024);
    for (int i = 0; i < nb; i++) {
        double s3[3] = {0, 0, 0}; double n = 0;
        FOR_BOX(box[i]) { int k = (a0 << 10) | (a1 << 5) | a2; n += cnt[k]; for (int c = 0; c < 3; c++) s3[c] += sum[k][c]; }
        if (n < 1) continue;
        for (int c = 0; c < 3; c++) { int v = (int)(s3[c] / n + 0.5); pal[i * 4 + 2 - c] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
    }
    #undef FOR_BOX
}
// RGB rows [row0, row0 + h), columns [x0, x0 + cw) of a stride-wide image; its own palette is made for it (`pal`
// only gives the alpha bytes) -> paletted BLP1 with mipmaps, width rounded up to a power of two
static uint8_t* SsMakeBlp(const float* rgb, int stride, int x0, int cw, int row0, int h, const uint8_t* pal, const uint8_t* hdr, DWORD* outN, int* outW)
{
    int W = 1; while (W < cw) W <<= 1;
    static uint8_t lut[1 << 18]; static uint8_t have[1 << 18];
    uint8_t mypal[1024];
    int levels = 0; DWORD total = 156 + 1024;
    for (int w = W, hh = h; ; w = w > 1 ? w / 2 : 1, hh = hh > 1 ? hh / 2 : 1) { total += (DWORD)w * hh; levels++; if ((w == 1 && hh == 1) || levels == 16) break; }
    uint8_t* o = (uint8_t*)calloc(1, total);
    float* cur = (float*)malloc(sizeof(float) * 3 * W * h);
    if (!o || !cur) { free(o); free(cur); return NULL; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < W; x++)
            memcpy(cur + ((size_t)y * W + x) * 3, rgb + ((size_t)(row0 + y) * stride + x0 + (x < cw ? x : cw - 1)) * 3, 12);
    SsMedianCut(cur, (size_t)W * h, mypal);
    for (int k = 0; k < 256; k++) mypal[k * 4 + 3] = pal[3];
    pal = mypal;
    memset(have, 0, sizeof have);
    memcpy(o, hdr, 28);
    *(DWORD*)(o + 12) = W; *(DWORD*)(o + 16) = h;
    memcpy(o + 156, pal, 1024);
    DWORD pos = 156 + 1024; int w = W, hh = h;
    for (int lv = 0; lv < levels; lv++) {
        *(DWORD*)(o + 28 + lv * 4) = pos; *(DWORD*)(o + 92 + lv * 4) = (DWORD)w * hh;
        for (int i = 0; i < w * hh; i++) {
            int R8 = (int)(cur[i * 3] + 0.5f), G8 = (int)(cur[i * 3 + 1] + 0.5f), B8 = (int)(cur[i * 3 + 2] + 0.5f);
            int key = ((R8 >> 2) << 12) | ((G8 >> 2) << 6) | (B8 >> 2);
            if (!have[key]) {
                int best = 0, bd = 1 << 30, rr = (R8 & ~3) + 2, gg = (G8 & ~3) + 2, bb = (B8 & ~3) + 2;
                for (int k = 0; k < 256; k++) {
                    int dr = pal[k * 4 + 2] - rr, dg = pal[k * 4 + 1] - gg, db = pal[k * 4] - bb, dd = dr * dr + dg * dg + db * db;
                    if (dd < bd) { bd = dd; best = k; }
                }
                lut[key] = (uint8_t)best; have[key] = 1;
            }
            o[pos + i] = lut[key];
        }
        pos += (DWORD)w * hh;
        if (lv + 1 < levels) {                      // 2 x 2 average for the next level
            int nw = w > 1 ? w / 2 : 1, nh = hh > 1 ? hh / 2 : 1;
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    for (int c = 0; c < 3; c++) {
                        int x0 = x * 2, y0 = y * 2, x1 = w > 1 ? x0 + 1 : x0, y1 = hh > 1 ? y0 + 1 : y0;
                        cur[((size_t)y * nw + x) * 3 + c] = (cur[((size_t)y0 * w + x0) * 3 + c] + cur[((size_t)y0 * w + x1) * 3 + c] +
                                                             cur[((size_t)y1 * w + x0) * 3 + c] + cur[((size_t)y1 * w + x1) * 3 + c]) / 4;
                    }
            w = nw; hh = nh;
        }
    }
    *(DWORD*)(o + 24) = 1;                          // has mipmaps
    free(cur);
    *outN = total; *outW = W;
    return o;
}

// geoset helpers: locate VRTX / UVBS / material id of a geoset
static int SsGeosetParts(uint8_t* g, DWORD sz, float** v, float** uv, DWORD** mat, DWORD* cnt)
{
    *v = NULL; *uv = NULL; *mat = NULL; *cnt = 0;
    for (DWORD q = 4; q + 8 <= sz; ) {
        uint8_t* t = g + q; DWORD k = *(DWORD*)(t + 4);
        if (!memcmp(t, "VRTX", 4)) { *v = (float*)(t + 8); *cnt = k; q += 8 + k * 12; }
        else if (!memcmp(t, "NRMS", 4)) q += 8 + k * 12;
        else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + k * 4;
        else if (!memcmp(t, "PVTX", 4)) q += 8 + k * 2;
        else if (!memcmp(t, "GNDX", 4)) q += 8 + k;
        else if (!memcmp(t, "MATS", 4)) { *mat = (DWORD*)(t + 8 + k * 4); q += 8 + k * 4 + 12 + 28; if (q + 4 > sz) return 0; DWORD na = *(DWORD*)(g + q); q += 4 + na * 28; }
        else if (!memcmp(t, "UVAS", 4)) q += 8;
        else if (!memcmp(t, "UVBS", 4)) { *uv = (float*)(t + 8); q += 8 + k * 8; }
        else return 0;
    }
    return *v && *uv && *mat && *cnt == 4;
}
static void SsSetQuad(uint8_t* g, DWORD sz, float x0, float x1, float y0, float y1, float u0, float u1, float v0, float v1, DWORD mat)
{
    float *v, *uv; DWORD *m, cnt;
    if (!SsGeosetParts(g, sz, &v, &uv, &m, &cnt)) return;
    float xm = 0, ym = 0;
    for (DWORD i = 0; i < 4; i++) { xm += v[i * 3] / 4; ym += v[i * 3 + 1] / 4; }
    for (DWORD i = 0; i < 4; i++) {
        int right = v[i * 3] > xm, top = v[i * 3 + 1] > ym;
        v[i * 3] = right ? x1 : x0; v[i * 3 + 1] = top ? y1 : y0;
        uv[i * 2] = right ? u1 : u0; uv[i * 2 + 1] = top ? v0 : v1;
    }
    *m = mat;
}

#define SS_TEXDIR "UI\\Glues\\ScoreScreen\\ScoreScreen-Background\\W3TW-ScoreSide-"

// the model with the extended sheet; texW: widths of the RF / LF textures, wt / wl as in SsBuild
// one quad of the background: material, x range, top tile (y 0.2..0.6, 512 rows) or bottom (0..0.2, 256 rows),
// u range in texels of a texture tw wide, v range in texel rows
typedef struct { int mat; float x0, x1; int top; float u0, u1, v0, v1, tw; } SsQuad;
static uint8_t* SsBuildModelQ(const uint8_t* d, DWORD n, const SsQuad* q, int nq, const char (*tex)[16], int ntex, DWORD* outN);

// the model with quads q (the first four replace the original tiles' geosets) and textures 5.. named by tex
static uint8_t* SsBuildModelQ(const uint8_t* d, DWORD n, const SsQuad* q, int nq, const char (*tex)[16], int ntex, DWORD* outN)
{
    if (n < 16 || memcmp(d, "MDLX", 4) || nq < 4) return NULL;
    const float K = 0.4f / 512;
    #undef SQ
    const int added = nq - 4;
    // find chunks
    DWORD p = 4, gp = 0, gsz = 0, mp = 0, msz = 0, tp = 0, tsz = 0;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) return NULL;
        if (!memcmp(d + p, "GEOS", 4)) { gp = p; gsz = sz; }
        if (!memcmp(d + p, "MTLS", 4)) { mp = p; msz = sz; }
        if (!memcmp(d + p, "TEXS", 4)) { tp = p; tsz = sz; }
        p += 8 + sz;
    }
    if (!gp || !mp || !tp || tsz != 5 * 268 || msz < 48 || *(DWORD*)(d + mp + 8) != 48) return NULL;
    // first geoset: the template for the added quads
    DWORD g0sz = *(DWORD*)(d + gp + 8);
    if (g0sz < 12 || g0sz > gsz) return NULL;
    { float *v, *uv; DWORD *m, cnt; uint8_t* t = (uint8_t*)malloc(g0sz); memcpy(t, d + gp + 8, g0sz);
      int ok = SsGeosetParts(t, g0sz, &v, &uv, &m, &cnt); free(t); if (!ok) return NULL; }
    DWORD on = n + ntex * 48 + ntex * 268 + added * g0sz;
    uint8_t* o = (uint8_t*)malloc(on);
    if (!o) return NULL;
    DWORD w = 0; p = 4; memcpy(o, d, 4); w = 4;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        memcpy(o + w, d + p, 8 + sz);
        if (p == mp) {
            for (int i = 0; i < ntex; i++) {
                uint8_t* m = o + w + 8 + sz + i * 48;
                memcpy(m, d + mp + 8, 48);
                *(DWORD*)(m + 32) = 5 + i;          // layer texture id
            }
            *(DWORD*)(o + w + 4) = sz + ntex * 48; w += 8 + sz + ntex * 48;
        } else if (p == tp) {
            for (int i = 0; i < ntex; i++) {
                uint8_t* t = o + w + 8 + sz + i * 268;
                memset(t, 0, 268);
                snprintf((char*)t + 4, 260, SS_TEXDIR "%s.blp", tex[i]);
            }
            *(DWORD*)(o + w + 4) = sz + ntex * 268; w += 8 + sz + ntex * 268;
        } else if (p == gp) {
            uint8_t* base = o + w + 8;
            // originals 0..3 edited in place
            DWORD g = 0;
            for (int i = 0; i < 4 && g + 4 <= sz; i++) {
                DWORD s = *(DWORD*)(base + g);
                const SsQuad* Q = &q[i];
                float y0 = Q->top ? 0.6f - Q->v1 * K : 0.2f - Q->v1 * K, y1 = Q->top ? 0.6f - Q->v0 * K : 0.2f - Q->v0 * K;
                float th = Q->top ? 512.0f : 256.0f;
                SsSetQuad(base + g, s, Q->x0, Q->x1, y0, y1, Q->u0 / Q->tw, Q->u1 / Q->tw, Q->v0 / th, Q->v1 / th, Q->mat);
                g += s;
            }
            // geoset 4: the highlighted header strip of the sorted column, a copy of the whole header row; its ends
            // would show the original sheet's edges over the extension
            if (g + 4 <= sz) {
                DWORD s = *(DWORD*)(base + g);
                float *v = NULL, *uv = NULL; DWORD *m, cnt = 0;
                for (DWORD qq = 4; qq + 8 <= s; ) {
                    uint8_t* t = base + g + qq; DWORD k = *(DWORD*)(t + 4);
                    if (!memcmp(t, "VRTX", 4)) { v = (float*)(t + 8); cnt = k; qq += 8 + k * 12; }
                    else if (!memcmp(t, "NRMS", 4)) qq += 8 + k * 12;
                    else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) qq += 8 + k * 4;
                    else if (!memcmp(t, "PVTX", 4)) qq += 8 + k * 2;
                    else if (!memcmp(t, "GNDX", 4)) qq += 8 + k;
                    else if (!memcmp(t, "MATS", 4)) { m = (DWORD*)(t + 8 + k * 4); (void)m; qq += 8 + k * 4 + 12 + 28; if (g + qq + 4 > sz) break; qq += 4 + *(DWORD*)(base + g + qq) * 28; }
                    else if (!memcmp(t, "UVAS", 4)) qq += 8;
                    else if (!memcmp(t, "UVBS", 4)) { uv = (float*)(t + 8); if (v && k == cnt) for (DWORD i = 0; i < cnt; i++) {
                            float* x = &v[i * 3]; float* u = &uv[i * 2];
                            if (*x > 0.799f) { *u = *u * SS_CR / 512.0f; *x = 0.4f + SS_CR * K; }
                            else if (*x < 0.001f) { *u = 2.0f / 512; *x = 2 * K; } }
                        qq += 8 + k * 8; }
                    else break;
                }
            }
            for (int i = 4; i < nq; i++) {
                uint8_t* t = base + sz + (i - 4) * g0sz;
                memcpy(t, d + gp + 8, g0sz);
                const SsQuad* Q = &q[i];
                float y0 = Q->top ? 0.6f - Q->v1 * K : 0.2f - Q->v1 * K, y1 = Q->top ? 0.6f - Q->v0 * K : 0.2f - Q->v0 * K;
                float th = Q->top ? 512.0f : 256.0f;
                SsSetQuad(t, g0sz, Q->x0, Q->x1, y0, y1, Q->u0 / Q->tw, Q->u1 / Q->tw, Q->v0 / th, Q->v1 / th, Q->mat);
            }
            *(DWORD*)(o + w + 4) = sz + added * g0sz; w += 8 + sz + added * g0sz;
        } else w += 8 + sz;
        p += 8 + sz;
    }
    *outN = w;
    return o;
}

// ---- variant 2 (LoadingFullScreen=2): the sheet stretched to the screen width --------------------------------------
// Below the header the sheet is stretched over the width, except its right border group, which keeps its width at the
// right screen edge. The top (wood bar, tabs, header cells) is the extended layout of variant 1 without the mirrored
// left group, so tabs and cells stay where the score screen's texts are; it is toned to the stretched sheet below it
// and crossfades into it over a few rows of plain parchment.
#define SS_V2B0 160
#define SS_V2B1 196
#define SS_V2W(wt) (1024 + 2 * (wt))
#define SS_V2RK 192
// lf: SsBuild's left filler made with wl = wt + 2 and no left group; returns SS_ROWS x SS_V2W(wt) RGB
static float* SsCompose2(const SsGen* G, int wt, const float* rf, const float* lf)
{
    const float* L = G->L; const float* R = G->R;
    const int W = SS_V2W(wt), rw = SS_RFW(wt), lw = wt + SS_LPW, gx = W - (512 - SS_CR), sw = 512 + SS_CR - 2;
    float* E = (float*)malloc(sizeof(float) * 3 * (size_t)SS_ROWS * W);
    float* gg = (float*)malloc(sizeof(float) * 3 * (size_t)W * 2);
    if (!E || !gg) { free(E); free(gg); return NULL; }
    #define P(img, w, r, x) (&(img)[((size_t)(r) * (w) + (x)) * 3])
    // the stretched sheet (rows from just above the crossfade)
    for (int r = SS_V2B0 - 8; r < SS_ROWS; r++)
        for (int X = 0; X < W; X++) {
            float* o = P(E, W, r, X);
            if (X >= gx) { memcpy(o, P(R, 512, r, X - gx + SS_CR), 12); continue; }
            // the last SS_V2RK columns before the right border keep their width (the darkened corner stays as it is)
            float cs = X >= gx - SS_V2RK ? (float)(512 + SS_CR - (gx - X)) : 2 + (X + 0.5f) * (sw - SS_V2RK) / (gx - SS_V2RK) - 0.5f;
            int c0 = (int)floorf(cs); float f = cs - c0;
            int c1 = c0 + 1 < 512 + SS_CR ? c0 + 1 : c0;
            const float* a = c0 < 512 ? P(L, 512, r, c0) : P(R, 512, r, c0 - 512);
            const float* b = c1 < 512 ? P(L, 512, r, c1) : P(R, 512, r, c1 - 512);
            for (int c = 0; c < 3; c++) o[c] = a[c] + (b[c] - a[c]) * f;
        }
    // the extended top, into a band buffer
    float* B = (float*)malloc(sizeof(float) * 3 * (size_t)SS_V2B1 * W);
    if (!B) { free(E); free(gg); return NULL; }
    for (int r = 0; r < SS_V2B1; r++)
        for (int X = 0; X < W; X++) {
            const float* s;
            if (X < lw) s = P(lf, lw, r, X);
            else if (X < wt + 512) s = P(L, 512, r, X - wt);
            else {
                int xr = X - wt - 512;
                if (xr >= SS_CR + wt) s = P(R, 512, r, xr - wt);
                else s = xr < SS_CR - SS_PW ? P(R, 512, r, xr) : P(rf, rw, r, xr - (SS_CR - SS_PW));
            }
            memcpy(P(B, W, r, X), s, 12);
        }
    // tone the band to the stretched sheet right below it, per column (smoothed)
    float* tmp = gg + 3 * W;
    for (int X = 0; X < W; X++)
        for (int c = 0; c < 3; c++) {
            double up = 0, dn = 0;
            for (int r = SS_V2B0 - 8; r < SS_V2B0; r++) up += P(B, W, r, X)[c];
            for (int r = SS_V2B0; r < SS_V2B0 + 8; r++) dn += P(E, W, r, X)[c];
            float g = (float)(dn / (up > 1 ? up : 1));
            gg[X * 3 + c] = g < 0.7f ? 0.7f : g > 1.4f ? 1.4f : g;
        }
    for (int c = 0; c < 3; c++) SsBlurLine(gg + c, W, 3, 16, tmp);
    // (only below the header row: above it the selected tab's highlight, a piece of the original picture, lies over
    // the band, so the band keeps the original's tone there)
    for (int r = 136; r < SS_V2B1; r++) {
        float k = r < SS_V2B0 ? (float)(r - 136) / (SS_V2B0 - 136) : 1.0f;
        for (int X = 0; X < W; X++) for (int c = 0; c < 3; c++) {
            float* p = P(B, W, r, X); float v = p[c] * (1 + (gg[X * 3 + c] - 1) * k); p[c] = v > 255 ? 255 : v;
        }
    }
    // band on top, crossfade, stretched sheet below
    for (int r = 0; r < SS_V2B1; r++) {
        float t = r < SS_V2B0 ? 0.0f : (float)(r - SS_V2B0) / (SS_V2B1 - SS_V2B0);
        t = t * t * (3 - 2 * t);
        for (int X = 0; X < W; X++) for (int c = 0; c < 3; c++) {
            float* o = P(E, W, r, X);
            o[c] = r < SS_V2B0 - 8 ? P(B, W, r, X)[c] : P(B, W, r, X)[c] * (1 - t) + o[c] * t;
        }
    }
    #undef P
    free(B); free(gg);
    return E;
}

// variant 1 as one composed picture in the layout of variant 2 (SS_V2W(wt) wide, 1 texel = one texel of the sheet):
// mirrored right border group, left filler, the original sheet, right filler, right border group
static float* SsCompose1(const SsGen* G, int wt, int wl, const float* rf, const float* lf)
{
    const float* L = G->L; const float* R = G->R;
    const int W = SS_V2W(wt), g = 512 - SS_CR, lw = SS_LFW(wl), rw = SS_RFW(wt);
    float* E = (float*)malloc(sizeof(float) * 3 * (size_t)SS_ROWS * W);
    if (!E) return NULL;
    for (int r = 0; r < SS_ROWS; r++)
        for (int X = 0; X < W; X++) {
            const float* src; int x = X;
            if (x < g) src = &R[((size_t)r * 512 + 511 - x) * 3];
            else if ((x -= g) < lw) src = &lf[((size_t)r * lw + x) * 3];
            else if ((x -= lw) < 512 - SS_LPW) src = &L[((size_t)r * 512 + SS_LPW + x) * 3];
            else if ((x -= 512 - SS_LPW) < SS_CR - SS_PW) src = &R[((size_t)r * 512 + x) * 3];
            else if ((x -= SS_CR - SS_PW) < rw) src = &rf[((size_t)r * rw + x) * 3];
            else src = &R[((size_t)r * 512 + SS_CR + (x - rw)) * 3];
            memcpy(&E[((size_t)r * W + X) * 3], src, 12);
        }
    return E;
}
// the light of the parchment: the original sheet is lit brighter in its middle and darker towards its edges. On a
// wide screen the extended sheet would keep that bright middle as a 4:3 patch; the low-frequency light of the
// original is stretched over the whole width instead and laid on the composed picture (plain parchment rows only)
static void SsRelight(float* E, int W, int xa, int xb, const SsGen* G)
{
    const float* L = G->L; const float* R = G->R;
    const int oa = 2, ob = 512 + SS_CR, ow = ob - oa, r0 = SS_B0, r1 = SS_B1, nh = r1 - r0, RX = 48, RY = 40, F = 24;
    float* lo = (float*)malloc(sizeof(float) * (size_t)nh * ow);
    float* le = (float*)malloc(sizeof(float) * (size_t)nh * W);
    float* tmp = (float*)malloc(sizeof(float) * (size_t)(W > nh ? W : nh) * 2 + 64);
    if (!lo || !le || !tmp) { free(lo); free(le); free(tmp); return; }
    for (int r = 0; r < nh; r++) {
        for (int c = 0; c < ow; c++) { int x = oa + c;
            const float* p = x < 512 ? &L[((size_t)(r0 + r) * 512 + x) * 3] : &R[((size_t)(r0 + r) * 512 + x - 512) * 3];
            lo[(size_t)r * ow + c] = (p[0] + p[1] + p[2]) / 3; }
        for (int X = 0; X < W; X++) { const float* p = &E[((size_t)(r0 + r) * W + X) * 3]; le[(size_t)r * W + X] = (p[0] + p[1] + p[2]) / 3; }
        SsBlurLine(lo + (size_t)r * ow, ow, 1, RX, tmp);
        SsBlurLine(le + (size_t)r * W + xa, xb - xa, 1, RX, tmp);
    }
    for (int c = 0; c < ow; c++) SsBlurLine(lo + c, nh, ow, RY, tmp);
    for (int X = xa; X < xb; X++) SsBlurLine(le + X, nh, W, RY, tmp);
    for (int r = 0; r < nh; r++) {
        float fy = r < F ? (float)r / F : r >= nh - F ? (float)(nh - 1 - r) / F : 1.0f;
        for (int X = xa; X < xb; X++) {
            float fx = X - xa < F ? (float)(X - xa) / F : xb - 1 - X < F ? (float)(xb - 1 - X) / F : 1.0f;
            float cs = (X - xa + 0.5f) * ow / (xb - xa) - 0.5f; int c = (int)cs; if (c < 0) c = 0; if (c > ow - 1) c = ow - 1;
            float t = lo[(size_t)r * ow + c], cur = le[(size_t)r * W + X];
            float gn = cur > 1 ? t / cur : 1.0f; gn = gn < 0.6f ? 0.6f : gn > 1.6f ? 1.6f : gn;
            gn = 1 + (gn - 1) * fx * fy;
            float* p = &E[((size_t)(r0 + r) * W + X) * 3];
            for (int k = 0; k < 3; k++) { float v = p[k] * gn; p[k] = v > 255 ? 255 : v; }
        }
    }
    free(lo); free(le); free(tmp);
}

// the model for variant 2: the composed sheet in tiles of 512 texels (top 512 rows / bottom 256 rows each);
// texW[i]: the texture widths of the tiles (top and bottom of a column share it)
static uint8_t* SsBuildModel2(const uint8_t* d, DWORD n, float e, int wt, const int* texW, DWORD* outN)
{
    const int W = SS_V2W(wt), ncol = (W + 511) / 512;
    SsQuad* q = (SsQuad*)malloc(sizeof(SsQuad) * 2 * ncol);
    char (*tex)[16] = (char (*)[16])malloc(16 * 2 * ncol);
    if (!q || !tex) { free(q); free(tex); return NULL; }
    const float s = (0.8f + 2 * e) / W;
    for (int i = 0; i < ncol; i++) {
        int x0 = i * 512, cw = W - x0 < 512 ? W - x0 : 512;
        for (int t = 0; t < 2; t++) {
            int k = i * 2 + t;
            q[k] = (SsQuad){ 5 + k, -e + x0 * s, -e + (x0 + cw) * s, !t, 0, (float)cw, 0, t ? 256.0f : 512.0f, (float)texW[i] };
            snprintf(tex[k], 16, "T%d%c", i, t ? 'B' : 'A');
        }
    }
    uint8_t* o = SsBuildModelQ(d, n, q, 2 * ncol, (const char (*)[16])tex, 2 * ncol, outN);
    free(q); free(tex);
    return o;
}

static uint8_t* SsReadFile(HANDLE mpq, const char* name, DWORD scope, DWORD* outN)
{
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return NULL;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* b = (n > 0 && n < (4u << 20)) ? (uint8_t*)malloc(n) : NULL;
    if (b && !(S_Read(h, b, n, &got, NULL) && got == n)) { free(b); b = NULL; }
    S_Close(h);
    *outN = n;
    return b;
}
static void SsCacheDir(char* dir)
{
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
    strcat(dir, "W3TrueWidescreen_cache"); CreateDirectoryA(dir, NULL);
}
static int SsWrite(const char* path, const uint8_t* b, DWORD n)
{
    FILE* f = fopen(path, "wb"); int ok = f && fwrite(b, 1, n, f) == n; if (f) fclose(f);
    return ok;
}
// the extended sheet: model plus its four filler textures written to the cache folder; 0 if it can't be made
static uint8_t* ExtendScoreBackground(HANDLE mpq, DWORD scope, const uint8_t* model, DWORD n, float e, int variant, DWORD* outN)
{
    // variant 1: the sheet extended with fillers, border at both edges; variant 2: the sheet stretched below the header
    int wt = (int)lroundf(e * 1280.0f), wl = variant == 2 ? wt + 2 : wt - (512 - SS_CR) + 2, lw = SS_LFW(wl), rw = SS_RFW(wt);
    if (wt - (512 - SS_CR) + 2 < 8) return NULL;              // too narrow (about 3:2 and below)
    static const char* const tiles[4] = { "L1", "R1", "L2", "R2" };
    SsSrc S = { 0 }; SsGen* G = NULL; float *rf = NULL, *lf = NULL, *E = NULL; uint8_t* o = NULL;
    S.L = (float*)malloc(sizeof(float) * SS_ROWS * 512 * 3); S.R = (float*)malloc(sizeof(float) * SS_ROWS * 512 * 3);
    G = (SsGen*)calloc(1, sizeof(SsGen));
    int ok = S.L && S.R && G;
    for (int i = 0; i < 4 && ok; i++) {
        char nm[MAX_PATH]; DWORD bn = 0;
        snprintf(nm, sizeof nm, "UI\\Glues\\ScoreScreen\\ScoreScreen-Background\\scorescreen-background-%s.blp", tiles[i]);
        uint8_t* b = SsReadFile(mpq, nm, scope, &bn);
        int top = i < 2, right = i & 1;
        ok = b && SsReadBlp(b, bn, right ? S.R : S.L, top ? 0 : 512, top ? 512 : 256, right ? NULL : S.pal[top ? 0 : 1], right ? NULL : S.hdr[top ? 0 : 1]);
        free(b);
    }
    if (ok) ok = SsPrepare(G, &S);
    if (ok) {
        rf = (float*)malloc(sizeof(float) * SS_ROWS * rw * 3); lf = (float*)malloc(sizeof(float) * SS_ROWS * lw * 3);
        ok = rf && lf;
    }
    char dir[MAX_PATH], path[MAX_PATH]; SsCacheDir(dir);
    if (ok) SsBuild(G, wt, wl, variant != 2, rf, lf);
    if (ok) {
        E = variant != 2 ? SsCompose1(G, wt, wl, rf, lf) : SsCompose2(G, wt, rf, lf);
        if (E) SsRelight(E, SS_V2W(wt), variant != 2 ? 512 - SS_CR : 0, SS_V2W(wt) - (512 - SS_CR), G);
        const int W = SS_V2W(wt), ncol = (W + 511) / 512;
        int texW[64];
        ok = E && ncol <= 64;
        for (int i = 0; i < ncol && ok; i++)
            for (int t = 0; t < 2 && ok; t++) {
                DWORD bn = 0; int x0 = i * 512, cw = W - x0 < 512 ? W - x0 : 512;
                uint8_t* b = SsMakeBlp(E, W, x0, cw, t ? 512 : 0, t ? 256 : 512, S.pal[t], S.hdr[t], &bn, &texW[i]);
                snprintf(path, sizeof path, "%s\\ScoreSide-T%d%c.blp", dir, i, t ? 'B' : 'A');
                ok = b && SsWrite(path, b, bn);
                free(b);
            }
        if (ok) o = SsBuildModel2(model, n, e, wt, texW, outN);
    }
    free(S.L); free(S.R); if (G) { free(G->tileN); free(G->tileW); } free(G); free(rf); free(lf); free(E);
    return o;
}
static int ServeScoreSide(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    const char* bn = strrchr(name, '\\'); bn = bn ? bn + 1 : name;
    if (_strnicmp(bn, "W3TW-ScoreSide-", 15)) return 0;
    char dir[MAX_PATH], path[MAX_PATH]; SsCacheDir(dir);
    snprintf(path, sizeof path, "%s\\ScoreSide-%s", dir, bn + 15);
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    return r;
}
static int ServeScoreBackground(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0, on = 0;
    uint8_t* buf = (n > 0 && n < (1u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    float e = (float)((0.6 * g_aspect - 0.8) * 0.5);
    int ext = 0;
    uint8_t* o = NULL;
    // ScoreScreen=3 stretches the sheet: past about 21:9 (3x on 32:9) it gets visibly blurred, extend it instead
    int mode = g_scoreMode == 3 && g_aspect > 2.4 ? 2 : g_scoreMode;
    if (mode != g_scoreMode) logf_("score screen: the screen is too wide to stretch the sheet, extending it (ScoreScreen=2)");
    if (ok) { o = ExtendScoreBackground(mpq, scope, buf, n, e, mode == 3 ? 2 : 1, &on); ext = o != NULL; }
    if (ok && !o) o = AddSideGeosets(buf, n, e, 0.10f, &on);
    free(buf);
    if (!o) { logf_("score screen background not adapted"); return 0; }
    static char path[MAX_PATH];
    char dir[MAX_PATH]; SsCacheDir(dir);
    snprintf(path, sizeof path, "%s\\ScoreScreen-Background.mdx", dir);
    ok = SsWrite(path, o, on);
    free(o);
    if (!ok) return 0;
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    logf_("score screen background: %s (%d)", !ext ? "sides covered" : mode == 3 ? "sheet stretched" : "sheet extended", r);
    return r;
}
// ---- melee loading screen: the minimap frame ----------------------------------------------------------------------
// The melee loading pictures (UI\Glues\Loading\Multiplayer\) have the frame around the minimap drawn into their top
// left texture. When the picture is scaled differently from the minimap (LoadingFullScreen=1 stretches it, UIScale
// scales it but not the minimap), that frame no longer fits the map. The top left quad is cut into pieces: the frame
// is drawn unscaled around the minimap, and the frame lines of the scaled picture are covered with the parchment on
// both sides of them, mirrored and crossfaded across the line (geosets of a blended copy of the material, each with
// its own alpha), so no seams show.
#define LB_U(c) ((c) * 0.4f / 512.0f)                   // texel column -> model x (the texture spans 0..0.4)
#define LB_V(r) (0.6f - (r) * 0.4f / 512.0f)            // texel row -> model y (0.6..0.2)
#define LB_BANDS 8
typedef struct { float x0, x1, y0, y1; } LbRect;
typedef struct { float x0, x1, y0, y1, u[4], v[4]; } LbQuad;   // corners: (x0,y1) (x0,y0) (x1,y1) (x1,y0)
// texel edges: the frame (lines plus one texel), and its four lines with 3 texels on each side
static const short kLbBox[4] = { 62, 288, 217, 443 };                 // c0 c1 r0 r1
static const short kLbLine[4][4] = { { 60, 290, 215, 229 }, { 60, 290, 430, 444 }, { 60, 74, 215, 444 }, { 276, 290, 215, 444 } };
static int LbIn(const LbRect* r, float x, float y) { return x > r->x0 && x < r->x1 && y > r->y0 && y < r->y1; }
static int LbCutAdd(float* a, int n, float v, float lo, float hi)
{
    v = v < lo ? lo : v > hi ? hi : v;
    for (int i = 0; i < n; i++) if (fabsf(a[i] - v) < 1e-6f) return n;
    a[n] = v; return n + 1;
}
static int LbCmp(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
static int TexNameIs(const char* s, const char* end)
{
    size_t a = strnlen(s, 260), b = strlen(end);
    return a >= b && !_stricmp(s + a - b, end);
}
// one geoset of quads, from the template geoset `t` (sub-chunk offsets `at`), with material `mat`
static uint8_t* LbGeoset(uint8_t* w, const uint8_t* t, const DWORD* at, const LbQuad* q, int nq, int mat)
{
    const int nv = nq * 4;
    const DWORD matsLen = at[8] - at[7];
    const DWORD gsz = 4 + (8 + nv * 12) * 2 + 12 + 12 + (8 + nq * 12) + (8 + nv) + 12 + matsLen + 8 + (8 + nv * 8);
    #define LB_PUT(v) do { DWORD _v = (DWORD)(v); memcpy(w, &_v, 4); w += 4; } while (0)
    #define LB_PUTF(v) do { float _f = (v); memcpy(w, &_f, 4); w += 4; } while (0)
    LB_PUT(gsz);
    memcpy(w, "VRTX", 4); w += 4; LB_PUT(nv);
    for (int i = 0; i < nq; i++) {
        const float px[4] = { q[i].x0, q[i].x0, q[i].x1, q[i].x1 }, py[4] = { q[i].y1, q[i].y0, q[i].y1, q[i].y0 };
        for (int c = 0; c < 4; c++) { LB_PUTF(px[c]); LB_PUTF(py[c]); LB_PUTF(0.0f); }
    }
    memcpy(w, "NRMS", 4); w += 4; LB_PUT(nv);
    for (int i = 0; i < nv; i++) { memcpy(w, t + at[1] + 8, 12); w += 12; }
    memcpy(w, t + at[2], 12); w += 12;                                  // PTYP: triangles
    memcpy(w, "PCNT", 4); w += 4; LB_PUT(1); LB_PUT(nq * 6);
    memcpy(w, "PVTX", 4); w += 4; LB_PUT(nq * 6);
    for (int i = 0; i < nq; i++) {
        unsigned short ix[6] = { i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3, i * 4 + 2, i * 4 + 1 };
        memcpy(w, ix, 12); w += 12;
    }
    memcpy(w, "GNDX", 4); w += 4; LB_PUT(nv); memset(w, 0, nv); w += nv;
    memcpy(w, t + at[6], 12); w += 12;                                  // MTGC
    memcpy(w, t + at[7], matsLen);                                      // MATS, material, extents
    *(int*)(w + 8 + *(DWORD*)(t + at[7] + 4) * 4) = mat; w += matsLen;
    memcpy(w, t + at[8], 8); w += 8;                                    // UVAS
    memcpy(w, "UVBS", 4); w += 4; LB_PUT(nv);
    for (int i = 0; i < nq; i++) for (int c = 0; c < 4; c++) {
        float U = q[i].u[c] / 0.4f, V = (0.6f - q[i].v[c]) / 0.4f;     // (kept inside the texture: no wrap-around)
        LB_PUTF(U < 0 ? 0 : U > 1 ? 1 : U); LB_PUTF(V < 0 ? 0 : V > 1 ? 1 : V);
    }
    #undef LB_PUT
    #undef LB_PUTF
    return w;
}
// d: the model already scaled by (fx, fy) (StretchMdxVertices with k = fy); returns a new model or NULL
static uint8_t* FixMeleeLoadingBox(const uint8_t* d, DWORD n, float fx, float fy, DWORD* outN)
{
    if (n < 16 || memcmp(d, "MDLX", 4)) return NULL;
    DWORD p = 4, mtls = 0, mtlsN = 0, texs = 0, texsN = 0, geos = 0, geosN = 0;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) return NULL;
        if (!memcmp(d + p, "MTLS", 4)) { mtls = p; mtlsN = sz; }
        else if (!memcmp(d + p, "TEXS", 4)) { texs = p + 8; texsN = sz; }
        else if (!memcmp(d + p, "GEOS", 4)) { geos = p; geosN = sz; }
        else if (!memcmp(d + p, "GEOA", 4)) return NULL;                // (the stock models have none)
        p += 8 + sz;
    }
    if (!mtls || !texs || !geos || geos < mtls) return NULL;
    // the materials (one layer each) and their textures
    int matTex[16], nm = 0; DWORD matOff[16];
    for (DWORD m = mtls + 8; m + 4 <= mtls + 8 + mtlsN; ) {
        DWORD msz = *(DWORD*)(d + m);
        if (nm >= 16 || msz < 48 || m + msz > mtls + 8 + mtlsN || memcmp(d + m + 12, "LAYS", 4) || *(DWORD*)(d + m + 16) != 1) return NULL;
        matOff[nm] = m; matTex[nm++] = *(int*)(d + m + 32);
        m += msz;
    }
    // the geoset textured with Multiplayer\Loading-TopLeft.blp
    DWORD tg = 0, tgN = 0; int ng = 0, tmat = -1;
    for (DWORD g = geos + 8; g + 4 <= geos + 8 + geosN; ng++) {
        DWORD gsz = *(DWORD*)(d + g);
        if (gsz < 12 || g + gsz > geos + 8 + geosN) return NULL;
        for (DWORD q = g + 4; !tg && q + 8 <= g + gsz; ) {              // find MATS
            const uint8_t* t = d + q; DWORD c = *(DWORD*)(t + 4);
            if (!memcmp(t, "VRTX", 4) || !memcmp(t, "NRMS", 4)) q += 8 + c * 12;
            else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + c * 4;
            else if (!memcmp(t, "PVTX", 4)) q += 8 + c * 2;
            else if (!memcmp(t, "GNDX", 4)) q += 8 + c;
            else if (!memcmp(t, "MATS", 4)) {
                int mat = *(int*)(t + 8 + c * 4), tex = mat >= 0 && mat < nm ? matTex[mat] : -1;
                if (tex >= 0 && (DWORD)(tex + 1) * 268 <= texsN
                    && TexNameIs((const char*)d + texs + tex * 268 + 4, "Multiplayer\\Loading-TopLeft.blp")) { tg = g; tgN = gsz; tmat = mat; }
                break;
            } else break;
        }
        g += gsz;
    }
    if (!tg) return NULL;
    // its sub-chunks: VRTX NRMS PTYP PCNT PVTX GNDX MTGC MATS ... UVAS UVBS
    DWORD at[10] = { 0 }; static const char tags[10][5] = { "VRTX", "NRMS", "PTYP", "PCNT", "PVTX", "GNDX", "MTGC", "MATS", "UVAS", "UVBS" };
    for (DWORD q = tg + 4, i = 0; i < 10; i++) {
        if (i == 8) { while (q + 8 <= tg + tgN && memcmp(d + q, "UVAS", 4)) q += 4; }   // past MATS' extents
        if (q + 8 > tg + tgN || memcmp(d + q, tags[i], 4)) return NULL;
        at[i] = q;
        DWORD c = *(DWORD*)(d + q + 4);
        q += i <= 1 ? 8 + c * 12 : i == 4 ? 8 + c * 2 : i == 5 ? 8 + c : i == 8 ? 8 : i == 9 ? 8 + c * 8 : 8 + c * 4;
        if (i == 9 && q != tg + tgN) return NULL;
    }
    if (*(DWORD*)(d + at[2] + 4) != 1 || *(DWORD*)(d + at[2] + 8) != 4 || *(DWORD*)(d + at[3] + 4) != 1
        || *(DWORD*)(d + at[6] + 4) != 1) return NULL;

    // the pieces, in model coordinates (the frame's: 0.8 fy x 0.6 fy): x = (u - 0.4) fx + 0.4 fy, y = v fy
    const float ox = 0.4f * fy;
    #define LB_SX(u) (((u) - 0.4f) * fx + ox)
    #define LB_SY(v) ((v) * fy)
    // the quad's own (scaled) corners, so the pieces meet the neighbouring quads exactly
    LbRect T = { 1e9f, -1e9f, 1e9f, -1e9f };
    for (DWORD i = 0, c = *(DWORD*)(d + at[0] + 4); i < c; i++) {
        const float* v = (const float*)(d + at[0] + 8 + i * 12);
        T.x0 = fminf(T.x0, v[0]); T.x1 = fmaxf(T.x1, v[0]); T.y0 = fminf(T.y0, v[1]); T.y1 = fmaxf(T.y1, v[1]);
    }
    if (fabsf(T.x0 - LB_SX(0.0f)) > 1e-3f || fabsf(T.x1 - LB_SX(0.4f)) > 1e-3f || fabsf(T.y0 - LB_SY(0.2f)) > 1e-3f
        || fabsf(T.y1 - LB_SY(0.6f)) > 1e-3f) return NULL;
    // the minimap: LEFT at the frame's LEFT + (0.056875, 0.0425), both scaled with the frame; 0.16 x 0.16
    const float mx = 0.056875f * fy + 0.08f, my = 0.3f * fy + 0.0425f * fy;
    const float uc = LB_U(175.0f), vc = LB_V(329.5f);                   // the frame's centre (texels 63..286, 218..440)
    LbRect O = { mx + LB_U(kLbBox[0]) - uc, mx + LB_U(kLbBox[1]) - uc, my + LB_V(kLbBox[3]) - vc, my + LB_V(kLbBox[2]) - vc };
    if (O.x0 < T.x0 || O.x1 > T.x1 || O.y0 < T.y0 || O.y1 > T.y1) return NULL;
    LbRect L[4];
    for (int i = 0; i < 4; i++)
        L[i] = (LbRect){ LB_SX(LB_U(kLbLine[i][0])), LB_SX(LB_U(kLbLine[i][1])), LB_SY(LB_V(kLbLine[i][3])), LB_SY(LB_V(kLbLine[i][2])) };
    float xs[16], ys[16]; int nx = 0, ny = 0;
    const LbRect* all[6] = { &T, &O, &L[0], &L[1], &L[2], &L[3] };
    for (int i = 0; i < 6; i++) {
        nx = LbCutAdd(xs, nx, all[i]->x0, T.x0, T.x1); nx = LbCutAdd(xs, nx, all[i]->x1, T.x0, T.x1);
        ny = LbCutAdd(ys, ny, all[i]->y0, T.y0, T.y1); ny = LbCutAdd(ys, ny, all[i]->y1, T.y0, T.y1);
    }
    qsort(xs, nx, sizeof(float), LbCmp); qsort(ys, ny, sizeof(float), LbCmp);
    const int nc = (nx - 1) * (ny - 1);
    LbQuad* base = (LbQuad*)malloc(sizeof(LbQuad) * nc);
    LbQuad* band = (LbQuad*)malloc(sizeof(LbQuad) * nc * LB_BANDS);
    int* bandOf = (int*)malloc(sizeof(int) * nc * LB_BANDS);
    if (!base || !band || !bandOf) { free(base); free(band); free(bandOf); return NULL; }
    int nb = 0, nO = 0, nL = 0;
    // texture position of a model point: plain (scaled picture), or mirrored across a line's edge e (texels), or the frame
    #define LB_TU(x) (((x) - ox) / fx + 0.4f)
    #define LB_TV(y) ((y) / fy)
    for (int j = 0, k = 0; j + 1 < ny; j++)
        for (int i = 0; i + 1 < nx; i++, k++) {
            LbQuad Q = { xs[i], xs[i + 1], ys[j], ys[j + 1] };
            const float px[4] = { Q.x0, Q.x0, Q.x1, Q.x1 }, py[4] = { Q.y1, Q.y0, Q.y1, Q.y0 };
            float cx = (Q.x0 + Q.x1) * 0.5f, cy = (Q.y0 + Q.y1) * 0.5f;
            int kind = LbIn(&O, cx, cy) ? 4 : -1, corner = 0;           // 4: the frame, 0..3: on a line
            for (int l = 0; l < 4 && kind < 0; l++) if (LbIn(&L[l], cx, cy)) kind = l;
            if (kind >= 0 && kind < 2) corner = LbIn(&L[2], cx, cy) || LbIn(&L[3], cx, cy);
            nO += kind == 4; nL += kind >= 0 && kind < 4;
            for (int c = 0; c < 4; c++) {
                float u = LB_TU(px[c]), v = LB_TV(py[c]);
                if (kind == 4) { u = px[c] - mx + uc; v = py[c] - my + vc; }
                else if (kind >= 0) {                                   // mirrored from the outer side of the line
                    float e = kind == 0 ? LB_V(kLbLine[0][2]) : kind == 1 ? LB_V(kLbLine[1][3]) : kind == 2 ? LB_U(kLbLine[2][0]) : LB_U(kLbLine[3][1]);
                    if (kind < 2) v = 2 * e - v; else u = 2 * e - u;
                }
                Q.u[c] = u; Q.v[c] = v;
            }
            base[k] = Q;
            if (kind < 0 || kind > 3 || corner) continue;
            // mirrored from the inner side, in bands of rising alpha from the outer edge to the inner one
            const short* ln = kLbLine[kind];
            float ei = kind == 0 ? LB_V(ln[3]) : kind == 1 ? LB_V(ln[2]) : kind == 2 ? LB_U(ln[1]) : LB_U(ln[0]);
            float a0 = kind < 2 ? L[kind].y0 : L[kind].x0, a1 = kind < 2 ? L[kind].y1 : L[kind].x1;  // across the line
            int outerHigh = kind == 0 || kind == 3;                     // the outer edge is at a1 (top line, right line)
            for (int b = 0; b < LB_BANDS; b++) {
                float t0 = (float)b / LB_BANDS, t1 = (float)(b + 1) / LB_BANDS;      // from the outer edge
                float s0 = outerHigh ? a1 - t1 * (a1 - a0) : a0 + t0 * (a1 - a0), s1 = outerHigh ? a1 - t0 * (a1 - a0) : a0 + t1 * (a1 - a0);
                LbQuad B = Q;
                if (kind < 2) { B.y0 = Q.y0 > s0 ? Q.y0 : s0; B.y1 = Q.y1 < s1 ? Q.y1 : s1; if (B.y1 <= B.y0) continue; }
                else { B.x0 = Q.x0 > s0 ? Q.x0 : s0; B.x1 = Q.x1 < s1 ? Q.x1 : s1; if (B.x1 <= B.x0) continue; }
                const float bx[4] = { B.x0, B.x0, B.x1, B.x1 }, by[4] = { B.y1, B.y0, B.y1, B.y0 };
                for (int c = 0; c < 4; c++) {
                    float u = LB_TU(bx[c]), v = LB_TV(by[c]);
                    if (kind < 2) v = 2 * ei - v; else u = 2 * ei - u;
                    B.u[c] = u; B.v[c] = v;
                }
                bandOf[nb] = b; band[nb++] = B;
            }
        }
    #undef LB_SX
    #undef LB_SY
    #undef LB_TU
    #undef LB_TV
    // the model: TopLeft replaced by the pieces, a blended copy of its material, the bands (one geoset per alpha)
    // and their geoset animations (static alpha) after GEOS
    const DWORD matSz = *(DWORD*)(d + matOff[tmat]);
    const DWORD cap = n + matSz + (DWORD)(nc + nb) * 200 + LB_BANDS * 600 + 64;
    uint8_t* o = (uint8_t*)malloc(cap);
    LbQuad* sel = (LbQuad*)malloc(sizeof(LbQuad) * (nb ? nb : 1));
    if (!o || !sel) { free(o); free(sel); free(base); free(band); free(bandOf); return NULL; }
    uint8_t* w = o;
    DWORD mEnd = mtls + 8 + mtlsN;
    memcpy(w, d, mEnd); w += mEnd;                                      // ... MTLS
    memcpy(w, d + matOff[tmat], matSz); *(DWORD*)(w + 24) = 2; w += matSz;   // + the blended material (filter mode 2)
    *(DWORD*)(o + mtls + 4) = mtlsN + matSz;
    memcpy(w, d + mEnd, tg - mEnd); w += tg - mEnd;                     // ... GEOS up to TopLeft
    uint8_t* gs = o + (geos + matSz);
    w = LbGeoset(w, d, at, base, nc, tmat);
    memcpy(w, d + tg + tgN, geos + 8 + geosN - tg - tgN); w += geos + 8 + geosN - tg - tgN;
    int nBandGeo = 0;
    for (int b = 0; b < LB_BANDS; b++) {
        int ns = 0;
        for (int i = 0; i < nb; i++) if (bandOf[i] == b) sel[ns++] = band[i];
        if (!ns) continue;
        w = LbGeoset(w, d, at, sel, ns, nm);
        bandOf[nBandGeo++] = b;                                         // (reused: the band of each new geoset)
    }
    *(DWORD*)(gs + 4) = (DWORD)(w - gs - 8);
    if (nBandGeo) {
        memcpy(w, "GEOA", 4); *(DWORD*)(w + 4) = nBandGeo * 28; w += 8;
        for (int i = 0; i < nBandGeo; i++) {
            float alpha = (bandOf[i] + 0.5f) / LB_BANDS, one = 1.0f;
            DWORD sz = 28, flags = 0, gid = ng + i;
            memcpy(w, &sz, 4); memcpy(w + 4, &alpha, 4); memcpy(w + 8, &flags, 4);
            memcpy(w + 12, &one, 4); memcpy(w + 16, &one, 4); memcpy(w + 20, &one, 4); memcpy(w + 24, &gid, 4);
            w += 28;
        }
    }
    memcpy(w, d + geos + 8 + geosN, n - (geos + 8 + geosN)); w += n - (geos + 8 + geosN);
    free(sel); free(base); free(band); free(bandOf);
    if ((DWORD)(w - o) > cap) { free(o); return NULL; }                 // (can't happen: cap is generous)
    *outN = (DWORD)(w - o);
    logf_("melee loading screen: minimap frame unscaled (%d pieces: %d frame, %d on lines; %d bands)", nc, nO, nL, nb);
    return o;
}

static int ServeWidenedLoading(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (g_scoreMode >= 2 && g_aspect > 1.34 && SameName(name, "UI\\Glues\\ScoreScreen\\ScoreScreen-Background\\ScoreScreen-Background.mdx"))
        return ServeScoreBackground(mpq, name, scope, ph);
    if (g_scoreMode >= 2 && g_aspect > 1.34 && ServeScoreSide(mpq, name, scope, ph)) return 1;
    int dota = g_dotaPending && g_loadModel[0] && SameName(name, g_loadModel);
    int custom = !dota && g_loadModel[0] && SameName(name, g_loadModel);
    static const char kBg[] = "UI\\Glues\\Loading\\Backgrounds\\", kGen[] = "UI\\Glues\\Loading\\Load-Generic\\",
                      kMp[] = "UI\\Glues\\Loading\\Multiplayer\\";       // melee maps: Load-Multiplayer-<race>
    int stock = !_strnicmp(name, kBg, sizeof kBg - 1) || !_strnicmp(name, kGen, sizeof kGen - 1)
             || !_strnicmp(name, kMp, sizeof kMp - 1);
    size_t nl = strlen(name);
    if (nl < 4 || (_stricmp(name + nl - 4, ".mdx") && _stricmp(name + nl - 4, ".mdl"))) return 0;
    if (!dota && !custom && !stock) return 0;
    double H = (g_dotaPending && g_dotaOrig) ? 0.6 : 0.6 / g_uiScale, W = H * g_aspect;   // the screen while loading
    float fy = (float)(H / 0.6), fx = fy;                    // fill the height, keep the picture's shape
    if (dota || g_loadFull) fx = (float)(W / 0.8);           // fill the whole screen
    if (fx == 1.0f && fy == 1.0f) return 0;
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* buf = (n > 0 && n < (4u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    if (ok) ok = StretchMdxVertices(buf, n, fx, fy, fy) > 0;
    // melee: the minimap frame around the minimap. Only on the game's own picture: Battle.net servers and loaders
    // (Eurobattle.net, ...) put their own artwork under the same names, with the frame elsewhere
    int stockPic = 0; g_customLoadPic = 0;
    if (ok && !_strnicmp(name, kMp, sizeof kMp - 1)) {
        HANDLE t = 0;
        if (orig_SOpenEx(mpq, "UI\\Glues\\Loading\\Multiplayer\\Loading-TopLeft.blp", scope, &t) && t) {
            DWORD tn = S_Size(t, NULL); S_Close(t);
            stockPic = tn != 592622;                                  // (Eurobattle.net's picture; the game's own differs
                                                                       //  in size between languages)
            if (!stockPic) { g_customLoadPic = 1; logf_("melee loading screen: custom picture (%lu bytes)", tn); FitLoadingMinimap(fx, fy); }
        }
    }
    if (ok && stockPic) {
        DWORD on = 0; uint8_t* o = FixMeleeLoadingBox(buf, n, fx, fy, &on);
        if (o) { free(buf); buf = o; n = on; }
    }
    static char path[MAX_PATH];
    if (ok) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
        strcat(dir, "W3TrueWidescreen_cache");
        CreateDirectoryA(dir, NULL);
        const char* bn = strrchr(name, '\\'); bn = bn ? bn + 1 : name;
        snprintf(path, sizeof path, "%s\\Loading-%s", dir, bn);
        FILE* f = fopen(path, "wb");
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f) fclose(f);
    }
    free(buf);
    if (!ok) { logf_("loading screen model %s not adapted", name); return 0; }
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    logf_("loading screen %s scaled %.3f x %.3f (%d)", name, fx, fy, r);
    return r;
}

// ---- MenuLayout=2: menu panels widened towards the centre ------------------------------------------------------
// All menu panels are in two models, TopLeftPanel and TopRightPanel (one panel per screen, each moved in by its own
// root bone). The panels of the two-panel screens are widened so that they meet near the screen centre: everything
// within MP_CUT of a panel's inner end (rivet strip, corner pieces, chain hooks, plates) moves towards the centre,
// the rest (up to the screen edge) stays. The frame bars are made of 0.2-wide segments with a strip of rivets
// textured over each; the segment that crosses the cut gets longer, and its texture is repeated rather than
// stretched (the metal texture wraps), by a whole number of rivet periods, so the rivets keep their size and spacing.
// Pivots of the bones go with the vertices, so the chains still swing about their hooks.
#define MP_MAXNODE 512
#define MP_CUT 0.21f                // from the inner end: just past the first bar segment, between the chain hooks
// rivets along the four bar strips of the metal texture (u, from the texture; the same in RoC and TFT)
static const float kRivets[4][6] = {
    { 0.175f, 0.350f, 0.517f, 0.720f, 0, 0 },
    { 0.155f, 0.330f, 0.495f, 0.675f, 0.840f, 0 },
    { 0.150f, 0.320f, 0.490f, 0.665f, 0.835f, 0.995f },
    { 0.130f, 0.300f, 0.470f, 0.640f, 0.810f, 0.980f },
};
// where to split a bar strip (row 0..3) and how far back the copy starts, to add `ex` (u) of strip without
// breaking the rivet pattern: whole tiles plus the distance between two rivets; the split lies in the gap after the
// later rivet. `rev`: the strip runs the other way (u decreasing towards the inner end).
static void MpSplitPlan(int row, int rev, float ex, float* um, float* k)
{
    float r[6]; int n = 0;
    for (int i = 0; i < 6; i++) if (kRivets[row][i] > 0.0f) r[n++] = kRivets[row][i];
    if (rev) { for (int i = 0; i < n; i++) r[i] = 1.0f - kRivets[row][n - 1 - i]; }
    float best = 1e9f; *um = 0.5f; *k = ex;
    float T0 = floorf(ex);
    for (int t = 0; t < 2; t++)
        for (int i = 0; i + 1 < n; i++)
            for (int j = 0; j <= i; j++) {
                float kk = T0 + t + (r[i] - r[j]), e = fabsf(kk - ex);
                if (kk > 0.0f && e < best) { best = e; *k = kk; *um = 0.5f * (r[i] + r[i + 1]); }
            }
    if (rev) *um = 1.0f - *um;
}
// how far a panel's inner end moves towards the screen centre: so that the two panels meet as on a 4:3 screen (as
// in the original centred layout), while their outer ends stay at the screen edges
static float MenuPanelMove(float in, int right)
{
    (void)in; (void)right;
    float m = (float)((0.6 * g_aspect - 0.8) * 0.5) - g_menuCut;      // (MenuAspect: up to the narrower menu area)
    return m > 0.0f ? m : 0.0f;
}
// geoset sub-chunks: VRTX, UVBS, first bone, material
typedef struct { DWORD vrtx, cnt, uvbs, mat; int b0; } MpGeo;
static void MpParseGeo(const uint8_t* d, DWORD g, DWORD gsz, MpGeo* o)
{
    o->vrtx = o->cnt = o->uvbs = 0; o->mat = 0xFFFFFFFF; o->b0 = -1;
    for (DWORD q = g + 4; q + 8 <= g + gsz; ) {
        const uint8_t* t = d + q; DWORD k = *(DWORD*)(t + 4);
        if (!memcmp(t, "VRTX", 4)) { o->vrtx = q; o->cnt = k; q += 8 + k * 12; }
        else if (!memcmp(t, "NRMS", 4)) q += 8 + k * 12;
        else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + k * 4;
        else if (!memcmp(t, "PVTX", 4)) q += 8 + k * 2;
        else if (!memcmp(t, "GNDX", 4)) q += 8 + k;
        else if (!memcmp(t, "MATS", 4)) {
            if (k) o->b0 = (int)*(DWORD*)(t + 8);
            q += 8 + k * 4; o->mat = *(DWORD*)(d + q); q += 12 + 28;
            DWORD ne = *(DWORD*)(d + q); q += 4 + ne * 28;
        }
        else if (!memcmp(t, "UVAS", 4)) q += 8;
        else if (!memcmp(t, "UVBS", 4)) { o->uvbs = q; q += 8 + k * 8; }
        else break;
    }
    if (o->uvbs && *(DWORD*)(d + o->uvbs + 4) != o->cnt) o->uvbs = 0;
}
// The bar segment crossing the cut is split at its middle: the outer half stays as it is (the geoset itself), the inner
// half is laid by a copy of the geoset (its other quads collapsed) from the middle to the moved inner end, with the
// texture repeated: it starts that many rivet periods back, so both the joint in the middle and the inner end, where
// the next segment begins, keep the original look.
static uint8_t* AdaptMenuPanels(const uint8_t* src, DWORD n, int right, int* panelsOut, DWORD* outN)
{
    *panelsOut = 0;
    if (n < 16 || memcmp(src, "MDLX", 4)) return NULL;
    uint8_t* d = (uint8_t*)malloc(n); memcpy(d, src, n);
    DWORD geos = 0, geosz = 0, geosHdr = 0, bone = 0, bonesz = 0, pivt = 0, pivtsz = 0, mtls = 0, mtlsz = 0, texs = 0, texsz = 0,
          geoa = 0, geoasz = 0, geoaHdr = 0;
    for (DWORD p = 4; p + 8 <= n; ) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) { free(d); return NULL; }
        if (!memcmp(d + p, "GEOS", 4)) { geosHdr = p; geos = p + 8; geosz = sz; }
        else if (!memcmp(d + p, "GEOA", 4)) { geoaHdr = p; geoa = p + 8; geoasz = sz; }
        else if (!memcmp(d + p, "BONE", 4)) { bone = p + 8; bonesz = sz; }
        else if (!memcmp(d + p, "PIVT", 4)) { pivt = p + 8; pivtsz = sz; }
        else if (!memcmp(d + p, "MTLS", 4)) { mtls = p + 8; mtlsz = sz; }
        else if (!memcmp(d + p, "TEXS", 4)) { texs = p + 8; texsz = sz; }
        p += 8 + sz;
    }
    if (!geos || !bone || (geoa && geoaHdr < geosHdr)) { free(d); return NULL; }
    // materials whose (first layer's) texture is the wrapping metal bar texture
    static uint8_t wrapMat[64];
    memset(wrapMat, 0, sizeof wrapMat);
    for (DWORD p = mtls, i = 0; mtls && p + 36 <= mtls + mtlsz && i < 64; i++) {
        DWORD msz = *(DWORD*)(d + p);
        if (msz < 36 || p + msz > mtls + mtlsz || memcmp(d + p + 12, "LAYS", 4)) break;
        DWORD tid = *(DWORD*)(d + p + 20 + 12);                   // LAYS, count, layer: size, filter, shading, texture
        if (texs && (tid + 1) * 268 <= texsz) {
            char nm[261]; memcpy(nm, d + texs + tid * 268 + 4, 260); nm[260] = 0;
            for (char* c = nm; *c; c++) *c = (char)tolower((unsigned char)*c);
            wrapMat[i] = strstr(nm, "metalpieces") != NULL ? 1 : strstr(nm, "bnetchannel") != NULL ? 2 : 0;
        }
        p += msz;
    }
    static int parent[MP_MAXNODE], root[MP_MAXNODE], named[MP_MAXNODE];
    static float lo[MP_MAXNODE], hi[MP_MAXNODE], mv[MP_MAXNODE];
    for (int i = 0; i < MP_MAXNODE; i++) { parent[i] = -1; named[i] = 0; lo[i] = 1e9f; hi[i] = -1e9f; mv[i] = 0.0f; }
    // the panels of the two-panel screens, by their root bones: skirmish Plane12 / Plane13, game lobby Plane76,
    // saved games, LAN and custom campaigns Plane117 (+ Plane134 below it on the LAN screen) / Plane103 or Plane107
    // (the same panel twice). The others (main menu, options, the button panel at the bottom right) keep their shape.
    // Battle.net: welcome and chat room Plane35 / Plane58 (welcome) or Plane33 (chat room), channels Plane132
    // standard game Plane122, profile Plane142 / Plane148, team game Plane164 / Plane166 (left; Plane107 right)
    static const char* const kWideL[] = { "Plane12", "Plane76", "Plane117", "Plane134", "Plane35", "Plane132", "Plane122",
                                          "Plane142", "Plane148", "Plane164", "Plane166" },
                     * const kWideR[] = { "Plane13", "Plane103", "Plane107", "Plane58", "Plane33" };
    static int planeOf33 = -1; planeOf33 = -1;
    for (DWORD p = bone; p + 96 <= bone + bonesz; ) {             // BONE: node (inclusive size, name, id, parent, ...) + 8
        DWORD isz = *(DWORD*)(d + p), id = *(DWORD*)(d + p + 84), par = *(DWORD*)(d + p + 88);
        if (isz < 96 || p + isz + 8 > bone + bonesz) { free(d); return NULL; }
        if (id < MP_MAXNODE) {
            parent[id] = par < MP_MAXNODE ? (int)par : -1;
            char nm[81]; memcpy(nm, d + p + 4, 80); nm[80] = 0;
            for (int k = 0; k < (right ? 5 : 11); k++) if (!strcmp(nm, right ? kWideR[k] : kWideL[k])) named[id] = 1;
            if (right && !strcmp(nm, "Plane33")) planeOf33 = (int)id;
        }
        p += isz + 8;
    }
    for (int i = 0; i < MP_MAXNODE; i++) { int r = i, k = 0; while (parent[r] >= 0 && k++ < 64) r = parent[r]; root[i] = r; }
    for (DWORD g = geos; g + 4 <= geos + geosz; ) {                // extent of every panel
        DWORD gsz = *(DWORD*)(d + g);
        if (gsz < 12 || g + gsz > geos + geosz) { free(d); return NULL; }
        MpGeo G; MpParseGeo(d, g, gsz, &G);
        if (G.vrtx && G.b0 >= 0 && G.b0 < MP_MAXNODE) {
            int r = root[G.b0];
            for (DWORD v = 0; v < G.cnt; v++) {
                float x = *(float*)(d + G.vrtx + 8 + v * 12);
                if (x < lo[r]) lo[r] = x;
                if (x > hi[r]) hi[r] = x;
            }
        }
        g += gsz;
    }
    int panels = 0;
    for (int i = 0; i < MP_MAXNODE; i++)
        if (root[i] == i && named[i] && hi[i] - lo[i] > 0.3f) {
            mv[i] = MenuPanelMove(right ? lo[i] : hi[i], right);
            if (mv[i] > 0.0f) { panels++; dlogf_("menu panel %d: %.3f..%.3f moved %.4f", i, lo[i], hi[i], mv[i]); }
        }
    // move the vertices; copies of the geosets with bar segments crossing the cut go to `extra`
    uint8_t* extra = (uint8_t*)malloc(geosz + 16); DWORD en = 0;
    static int copyOf[256]; int ncopy = 0, gi = 0;
    for (DWORD g = geos; g + 4 <= geos + geosz; gi++) {
        DWORD gsz = *(DWORD*)(d + g);
        MpGeo G; MpParseGeo(d, g, gsz, &G);
        int r = (G.vrtx && G.b0 >= 0 && G.b0 < MP_MAXNODE) ? root[G.b0] : -1;
        if (r < 0 || !(mv[r] > 0.0f) || G.cnt > 256) { g += gsz; continue; }
        float in = right ? lo[r] : hi[r], m = right ? -mv[r] : mv[r];
        int wrap = G.uvbs && G.mat < 64 && wrapMat[G.mat] == 1 && G.cnt % 4 == 0;
        int tiles = G.uvbs && G.mat < 64 && wrapMat[G.mat] == 2 && G.cnt % 4 == 0;   // Battle.net channel box rails
        static float ox[256]; static uint8_t moved[256], split[256], rigid[256];
        for (DWORD v = 0; v < G.cnt; v++) {
            ox[v] = *(float*)(d + G.vrtx + 8 + v * 12);
            moved[v] = (right ? ox[v] - in : in - ox[v]) < MP_CUT;
            split[v] = 0; rigid[v] = 0;
        }
        // small pieces (chain links, hooks, plates: quads narrower than a bar segment) move or stay as a whole,
        // by their centre; wide ones (backgrounds, bars) are stretched or split
        for (DWORD q = 0; G.cnt % 4 == 0 && q < G.cnt; q += 4) {
            float a = ox[q], b = ox[q];
            for (int k = 1; k < 4; k++) { if (ox[q + k] < a) a = ox[q + k]; if (ox[q + k] > b) b = ox[q + k]; }
            if (b - a > 0.15f) continue;
            float cx = 0.5f * (a + b); int mvq = (right ? cx - in : in - cx) < MP_CUT;
            for (int k = 0; k < 4; k++) { moved[q + k] = (uint8_t)mvq; rigid[q + k] = 1; }
        }
        // the chat room's right panel (Plane33) holds the exit button's panel at its foot (quads centred below 0.676;
        // the upper panel's lowest bar is centred at 0.684): that part keeps its shape and place
        for (DWORD q = 0; r == planeOf33 && G.cnt % 4 == 0 && q < G.cnt; q += 4) {
            float cy = 0.0f;
            for (int k = 0; k < 4; k++) cy += 0.25f * *(float*)(d + G.vrtx + 8 + (q + k) * 12 + 4);
            if (cy < 0.676f) for (int k = 0; k < 4; k++) moved[q + k] = 0;
        }
        // bar quads crossing the cut (4 vertices each, two x values, a full strip of the texture)
        int splits = 0;
        for (DWORD q = 0; wrap && q < G.cnt; q += 4) {
            int nm = 0; float umin = 1e9f, umax = -1e9f, vmax = -1e9f;
            for (int k = 0; k < 4; k++) {
                const float* uv = (const float*)(d + G.uvbs + 8 + (q + k) * 8);
                nm += moved[q + k];
                if (uv[0] < umin) umin = uv[0];
                if (uv[0] > umax) umax = uv[0];
                if (uv[1] > vmax) vmax = uv[1];
            }
            if (nm == 2 && !rigid[q] && umax - umin > 0.9f && vmax <= 0.51f) { for (int k = 0; k < 4; k++) split[q + k] = 1; splits++; }
        }
        uint8_t* c = NULL;
        if (splits && ncopy < 256 && en + gsz <= geosz + 16) { c = extra + en; memcpy(c, d + g, gsz); }
        for (DWORD q = 0; q < G.cnt; q += 4) {
            for (int k = 0; k < 4 && q + k < G.cnt; k++) {
                DWORD v = q + k;
                float* x = (float*)(d + G.vrtx + 8 + v * 12);
                if (!split[v]) {
                    if (moved[v]) *x = ox[v] + m;
                    if (c) *(float*)(c + (G.vrtx - g) + 8 + v * 12 + 4) = -100.0f;     // collapsed in the copy
                    continue;
                }
                // the vertex at the other end of the segment, same edge
                DWORD w = v;
                for (int j = 0; j < 4; j++) if (moved[q + j] != moved[v] && fabsf(*(float*)(d + G.vrtx + 8 + (q + j) * 12 + 4) - *(float*)(d + G.vrtx + 8 + v * 12 + 4)) < 1e-4f) w = q + j;
                if (w == v) continue;
                DWORD vi = moved[v] ? v : w, vo = moved[v] ? w : v;           // inner (moving) / outer end
                const float* uvI = (const float*)(src + G.uvbs + 8 + vi * 8), * uvO = (const float*)(src + G.uvbs + 8 + vo * 8);
                float rho = (uvI[0] - uvO[0]) / (ox[vi] - ox[vo]);
                float ex = fabsf(rho * m);                                     // strip to add (u)
                int rev = uvI[0] < uvO[0];
                float vv = uvI[1]; for (int j = 0; j < 4; j++) { float t = *(const float*)(src + G.uvbs + 8 + (q + j) * 8 + 4); if (t < vv) vv = t; }
                int row = (int)(vv * 8.0f + 0.01f); if (row < 0) row = 0; if (row > 3) row = 3;   // strip of the texture
                float um, k; MpSplitPlan(row, rev, ex, &um, &k);
                float xm = ox[vo] + (um - uvO[0]) / (uvI[0] - uvO[0]) * (ox[vi] - ox[vo]);
                float* uv = (float*)(d + G.uvbs + 8 + v * 8);
                float* cx = (float*)(c + (G.vrtx - g) + 8 + v * 12);
                float* cuv = (float*)(c + (G.uvbs - g) + 8 + v * 8);
                if (moved[v]) {                     // inner end: the original part ends at the split; the copy goes on
                    *cx = ox[v] + m;                // to the moved inner end with the original texture there
                    *x = xm; uv[0] = um;
                } else {                            // outer end: stays in the original; the copy starts at the split,
                    *cx = xm; cuv[0] = rev ? um + k : um - k;      // that much strip further back
                }
            }
        }
        // a stretched quad of a repeating texture (u over more than one tile: the profile's name box) keeps its texel
        // size: the moved end's u goes on further
        for (DWORD q = 0; !wrap && !tiles && G.uvbs && G.cnt % 4 == 0 && q < G.cnt; q += 4) {
            int nm = 0; float ua = 1e9f, ub = -1e9f, xa = 1e9f, xb = -1e9f;
            for (int k = 0; k < 4; k++) {
                const float* uv = (const float*)(src + G.uvbs + 8 + (q + k) * 8);
                nm += moved[q + k]; ua = fminf(ua, uv[0]); ub = fmaxf(ub, uv[0]); xa = fminf(xa, ox[q + k]); xb = fmaxf(xb, ox[q + k]);
            }
            if (nm != 2 || rigid[q] || ub - ua < 1.5f || xb - xa < 1e-4f) continue;
            float rho = (ub - ua) / (xb - xa);
            for (int k = 0; k < 4; k++) {
                if (!moved[q + k]) continue;
                float* uv = (float*)(d + G.uvbs + 8 + (q + k) * 8);
                // u grows towards the end with the larger u: away from the kept end
                float uk = 0; for (int j = 0; j < 4; j++) if (!moved[q + j]) uk = ((const float*)(src + G.uvbs + 8 + (q + j) * 8))[0];
                uv[0] = uk + (uv[0] - uk) * (1.0f + fabsf(m) * rho / (ub - ua));
            }
        }
        // Battle.net channel box: its rails are rows of equal tile quads, each moved or kept as a whole; the gap
        // between the kept and the moved tiles of a row is filled with copies of the last kept tile (in a copy of
        // the geoset, the last one cut to fit)
        if (tiles && !c && ncopy < 256 && en + gsz <= geosz + 16) {
            int nq = (int)G.cnt / 4, slot = 0;
            // clean quads: four consecutive vertices, a rectangle, whose triangles use only them
            static uint8_t clean[64];
            const uint16_t* tri = NULL; DWORD ntri = 0;
            for (DWORD q = g + 4; q + 8 <= g + gsz; ) {
                const uint8_t* t = d + q; DWORD k = *(DWORD*)(t + 4);
                if (!memcmp(t, "PVTX", 4)) { tri = (const uint16_t*)(t + 8); ntri = k; break; }
                q += 8 + (!memcmp(t, "VRTX", 4) || !memcmp(t, "NRMS", 4) ? k * 12 : !memcmp(t, "GNDX", 4) ? k : k * 4);
            }
            for (int a = 0; a < nq && a < 64; a++) {
                float xs[4], ys[4]; int nxv = 0, nyv = 0;
                for (int k = 0; k < 4; k++) {
                    float vx = ox[a * 4 + k], vy = *(float*)(d + G.vrtx + 8 + (a * 4 + k) * 12 + 4); int seen;
                    seen = 0; for (int j = 0; j < nxv; j++) if (fabsf(xs[j] - vx) < 1e-5f) seen = 1; if (!seen) xs[nxv++] = vx;
                    seen = 0; for (int j = 0; j < nyv; j++) if (fabsf(ys[j] - vy) < 1e-5f) seen = 1; if (!seen) ys[nyv++] = vy;
                }
                clean[a] = nxv == 2 && nyv == 2 && tri;
            }
            for (DWORD t = 0; tri && t + 2 < ntri; t += 3) {
                int q0 = tri[t] / 4, q1 = tri[t + 1] / 4, q2 = tri[t + 2] / 4;
                if (q0 != q1 || q0 != q2) { if (q0 < 64) clean[q0] = 0; if (q1 < 64) clean[q1] = 0; if (q2 < 64) clean[q2] = 0; }
            }
            uint8_t* cc = extra + en; memcpy(cc, d + g, gsz);
            for (DWORD v = 0; v < G.cnt; v++) *(float*)(cc + (G.vrtx - g) + 8 + v * 12 + 4) = -100.0f;
            for (int a = 0; a < nq; a++) {                          // a: a kept tile next to the gap
                if (moved[a * 4] || a >= 64 || !clean[a]) continue;
                float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
                for (int k = 0; k < 4; k++) { float vx = ox[a * 4 + k], vy = *(float*)(d + G.vrtx + 8 + (a * 4 + k) * 12 + 4);
                    ax0 = fminf(ax0, vx); ax1 = fmaxf(ax1, vx); ay0 = fminf(ay0, vy); ay1 = fmaxf(ay1, vy); }
                // the nearest moved tile of the same row, on the inner side
                float gEnd = right ? -1e9f : 1e9f; int found = 0;
                for (int b = 0; b < nq; b++) {
                    if (!moved[b * 4]) continue;
                    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f;
                    for (int k = 0; k < 4; k++) { float vx = *(float*)(d + G.vrtx + 8 + (b * 4 + k) * 12); bx0 = fminf(bx0, vx); bx1 = fmaxf(bx1, vx);
                        by0 = fminf(by0, *(float*)(d + G.vrtx + 8 + (b * 4 + k) * 12 + 4)); }
                    if (fabsf(by0 - ay0) > 1e-4f) continue;
                    if (!right && bx0 > ax1 - 1e-4f && bx0 < gEnd) { gEnd = bx0; found = 1; }
                    if (right && bx1 < ax0 + 1e-4f && bx1 > gEnd) { gEnd = bx1; found = 1; }
                }
                if (!found) continue;
                // the kept tile must be the last kept one before the gap
                int last = 1;
                for (int b = 0; b < nq && last; b++) {
                    if (moved[b * 4] || b == a) continue;
                    float bx0 = 1e9f, by0 = 1e9f;
                    for (int k = 0; k < 4; k++) { bx0 = fminf(bx0, ox[b * 4 + k]); by0 = fminf(by0, *(float*)(d + G.vrtx + 8 + (b * 4 + k) * 12 + 4)); }
                    if (fabsf(by0 - ay0) < 1e-4f && (right ? bx0 < ax0 - 1e-4f && bx0 > gEnd - 1e-4f : bx0 > ax0 + 1e-4f && bx0 < gEnd)) last = 0;
                }
                if (!last) continue;
                float w = ax1 - ax0;
                for (float s0 = right ? ax0 - w : ax1; right ? s0 + w > gEnd + 1e-5f : s0 < gEnd - 1e-5f; s0 += right ? -w : w) {
                    while (slot < nq && (slot >= 64 || !clean[slot])) slot++;
                    if (slot >= nq) break;
                    float x0 = s0, x1 = s0 + w, f0 = 0.0f, f1 = 1.0f;      // f: part of the tile shown
                    if (!right && x1 > gEnd) { f1 = (gEnd - x0) / w; x1 = gEnd; }
                    if (right && x0 < gEnd) { f0 = (gEnd - x0) / w; x0 = gEnd; }
                    float sx0 = 1e9f, sy0 = 1e9f;                      // the slot quad's own corners give each vertex its role
                    for (int k = 0; k < 4; k++) { sx0 = fminf(sx0, ox[slot * 4 + k]); sy0 = fminf(sy0, *(float*)(d + G.vrtx + 8 + (slot * 4 + k) * 12 + 4)); }
                    for (int k = 0; k < 4; k++) {
                        DWORD dv = slot * 4 + k, sv = a * 4;
                        int dl = fabsf(ox[dv] - sx0) < 1e-5f, db = fabsf(*(float*)(d + G.vrtx + 8 + dv * 12 + 4) - sy0) < 1e-5f;
                        for (int j = 0; j < 4; j++) {
                            int sl = fabsf(ox[a * 4 + j] - ax0) < 1e-5f, sb = fabsf(*(float*)(d + G.vrtx + 8 + (a * 4 + j) * 12 + 4) - ay0) < 1e-5f;
                            if (sl == dl && sb == db) sv = a * 4 + j;
                        }
                        const float* suv = (const float*)(src + G.uvbs + 8 + sv * 8);
                        float t = (ox[sv] - ax0) / w, u0 = 0, u1 = 0;     // u at the tile's two ends
                        for (int j = 0; j < 4; j++) { const float* q2 = (const float*)(src + G.uvbs + 8 + (a * 4 + j) * 8);
                            if (fabsf(ox[a * 4 + j] - ax0) < 1e-5f) u0 = q2[0]; else u1 = q2[0]; }
                        float ft = t < 0.5f ? f0 : f1;
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12) = t < 0.5f ? x0 : x1;
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12 + 4) = *(float*)(d + G.vrtx + 8 + sv * 12 + 4);
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12 + 8) = *(float*)(d + G.vrtx + 8 + sv * 12 + 8);
                        *(float*)(cc + (G.uvbs - g) + 8 + dv * 8) = u0 + (u1 - u0) * ft;
                        *(float*)(cc + (G.uvbs - g) + 8 + dv * 8 + 4) = suv[1];
                    }
                    slot++;
                }
            }
            // a row whose last kept tile before the gap is not a plain tile (a joint, as in the profile's name box):
            // the gap is filled with copies of the first moved tile instead, outwards from it
            for (int b = 0; b < nq && b < 64; b++) {
                if (!moved[b * 4] || !clean[b]) continue;
                float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
                for (int k = 0; k < 4; k++) { float vx = ox[b * 4 + k], vy = *(float*)(d + G.vrtx + 8 + (b * 4 + k) * 12 + 4);
                    bx0 = fminf(bx0, vx); bx1 = fmaxf(bx1, vx); by0 = fminf(by0, vy); by1 = fmaxf(by1, vy); }
                int first = 1, ka = -1; float kx = right ? 1e9f : -1e9f;
                for (int a = 0; a < nq; a++) {
                    if (a == b) continue;
                    float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
                    for (int k = 0; k < 4; k++) { float vx = ox[a * 4 + k], vy = *(float*)(d + G.vrtx + 8 + (a * 4 + k) * 12 + 4);
                        ax0 = fminf(ax0, vx); ax1 = fmaxf(ax1, vx); ay0 = fminf(ay0, vy); ay1 = fmaxf(ay1, vy); }
                    if (ay1 < by0 + 1e-4f || ay0 > by1 - 1e-4f) continue;          // not in this row
                    if (moved[a * 4]) { if (right ? ax0 > bx0 + 1e-4f : ax1 < bx1 - 1e-4f) first = 0; continue; }
                    // kept: its inner edge, on the outer side of b
                    for (int k = 0; k < 4; k++) {
                        float vx = ox[a * 4 + k], vy = *(float*)(d + G.vrtx + 8 + (a * 4 + k) * 12 + 4);
                        if (vy < by0 - 1e-4f || vy > by1 + 1e-4f) continue;
                        if (!right && vx <= bx0 + 1e-4f && vx > kx) { kx = vx; ka = a; }
                        if (right && vx >= bx1 - 1e-4f && vx < kx) { kx = vx; ka = a; }
                    }
                }
                if (!first || ka < 0 || ka >= 64 || clean[ka]) continue;
                float w = bx1 - bx0, nb0 = bx0 + m, nb1 = bx1 + m;                 // b where it moved to
                for (float s0 = right ? nb1 : nb0 - w; right ? s0 < kx - 1e-5f : s0 + w > kx + 1e-5f; s0 += right ? w : -w) {
                    while (slot < nq && (slot >= 64 || !clean[slot])) slot++;
                    if (slot >= nq) break;
                    float x0 = s0, x1 = s0 + w, f0 = 0.0f, f1 = 1.0f;
                    if (!right && x0 < kx) { f0 = (kx - x0) / w; x0 = kx; }
                    if (right && x1 > kx) { f1 = (kx - x0) / w; x1 = kx; }
                    float sx0 = 1e9f, sy0 = 1e9f;
                    for (int k = 0; k < 4; k++) { sx0 = fminf(sx0, ox[slot * 4 + k]); sy0 = fminf(sy0, *(float*)(d + G.vrtx + 8 + (slot * 4 + k) * 12 + 4)); }
                    float u0 = 0, u1 = 0;
                    for (int j = 0; j < 4; j++) { const float* q2 = (const float*)(src + G.uvbs + 8 + (b * 4 + j) * 8);
                        if (fabsf(ox[b * 4 + j] - bx0) < 1e-5f) u0 = q2[0]; else u1 = q2[0]; }
                    for (int k = 0; k < 4; k++) {
                        DWORD dv = slot * 4 + k, sv = b * 4;
                        int dl = fabsf(ox[dv] - sx0) < 1e-5f, db = fabsf(*(float*)(d + G.vrtx + 8 + dv * 12 + 4) - sy0) < 1e-5f;
                        for (int j = 0; j < 4; j++) {
                            int sl = fabsf(ox[b * 4 + j] - bx0) < 1e-5f, sb = fabsf(*(float*)(d + G.vrtx + 8 + (b * 4 + j) * 12 + 4) - by0) < 1e-5f;
                            if (sl == dl && sb == db) sv = b * 4 + j;
                        }
                        float ft = dl ? f0 : f1;
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12) = dl ? x0 : x1;
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12 + 4) = *(float*)(d + G.vrtx + 8 + sv * 12 + 4);
                        *(float*)(cc + (G.vrtx - g) + 8 + dv * 12 + 8) = *(float*)(d + G.vrtx + 8 + sv * 12 + 8);
                        *(float*)(cc + (G.uvbs - g) + 8 + dv * 8) = u0 + (u1 - u0) * ft;
                        *(float*)(cc + (G.uvbs - g) + 8 + dv * 8 + 4) = ((const float*)(src + G.uvbs + 8 + sv * 8))[1];
                    }
                    slot++;
                }
            }
            if (slot) { copyOf[ncopy++] = gi; en += gsz; c = cc; }
        }
        if (c && !tiles) { copyOf[ncopy++] = gi; en += gsz; }
        g += gsz;
    }
    for (DWORD i = 0; pivt && i < pivtsz / 12 && i < MP_MAXNODE; i++) {   // pivots: by object id
        int r = root[i];
        if (!(mv[r] > 0.0f)) continue;
        float* x = (float*)(d + pivt + i * 12);
        float in = right ? lo[r] : hi[r], u = right ? *x - in : in - *x;
        if (u < MP_CUT) *x += right ? -mv[r] : mv[r];
    }
    // each copy goes right after its original (drawn in the same place among the other geosets: under the pieces
    // drawn over its original, such as the profile's name box over the top bar); geoset ids after it move up by one
    // in the geoset animations, and the copies get their originals' animations
    #define MP_NEWID(i) ({ int _k = 0; for (int _j = 0; _j < ncopy; _j++) if (copyOf[_j] < (i)) _k++; (i) + _k; })
    uint8_t* ga = (uint8_t*)malloc(geoasz + 16); DWORD gan = 0;
    for (int j = 0; geoa && j < ncopy; j++)
        for (DWORD p = geoa; p + 28 <= geoa + geoasz; ) {
            DWORD isz = *(DWORD*)(d + p);
            if (isz < 28 || p + isz > geoa + geoasz) break;
            if (*(DWORD*)(d + p + 24) == (DWORD)copyOf[j] && gan + isz <= geoasz + 16) {
                memcpy(ga + gan, d + p, isz); *(DWORD*)(ga + gan + 24) = (DWORD)(MP_NEWID(copyOf[j]) + 1); gan += isz;
                break;
            }
            p += isz;
        }
    for (DWORD p = geoa; geoa && p + 28 <= geoa + geoasz; ) {          // (in d: copied to the output below)
        DWORD isz = *(DWORD*)(d + p);
        if (isz < 28 || p + isz > geoa + geoasz) break;
        int id = (int)*(DWORD*)(d + p + 24);
        if (id >= 0 && id < 4096) *(DWORD*)(d + p + 24) = (DWORD)MP_NEWID(id);
        p += isz;
    }
    #undef MP_NEWID
    DWORD total = n + en + gan;
    uint8_t* o = (uint8_t*)malloc(total);
    DWORD geosEnd = geos + geosz, geoaEnd = geoa ? geoa + geoasz : 0;
    if (!geoa || geoaHdr < geosHdr) gan = 0, total = n + en;
    memcpy(o, d, geos);
    {   // the geosets, each followed by its copy
        DWORD w = geos, eo = 0; int j = 0, k = 0;
        for (DWORD g = geos; g + 4 <= geosEnd; k++) {
            DWORD gsz = *(DWORD*)(d + g);
            if (gsz < 4 || g + gsz > geosEnd) { memcpy(o + w, d + g, geosEnd - g); w += geosEnd - g; break; }
            memcpy(o + w, d + g, gsz); w += gsz;
            if (j < ncopy && copyOf[j] == k) { memcpy(o + w, extra + eo, gsz); w += gsz; eo += gsz; j++; }
            g += gsz;
        }
    }
    if (gan) {
        memcpy(o + geosEnd + en, d + geosEnd, geoaEnd - geosEnd);
        memcpy(o + geoaEnd + en, ga, gan);
        memcpy(o + geoaEnd + en + gan, d + geoaEnd, n - geoaEnd);
        *(DWORD*)(o + geoaHdr + en + 4) = geoasz + gan;
    } else memcpy(o + geosEnd + en, d + geosEnd, n - geosEnd);
    *(DWORD*)(o + geosHdr + 4) = geosz + en;
    free(d); free(extra); free(ga);
    *panelsOut = panels; *outN = total;
    dlogf_("menu panels: %d bar geosets split", ncopy);
    return o;
}
static int ServeMenuPanels(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (!g_menuWide) return 0;
    const char* bn = strrchr(name, '\\'); bn = bn ? bn + 1 : name;
    int right;
    if (!_stricmp(bn, "TopLeftPanel.mdx") || !_stricmp(bn, "TopLeftPanel-Expansion.mdx")) right = 0;
    else if (!_stricmp(bn, "TopRightPanel.mdx") || !_stricmp(bn, "TopRightPanel-Expansion.mdx")) right = 1;
    else return 0;
    static const char kSl[] = "UI\\Glues\\SpriteLayers\\";
    if (_strnicmp(name, kSl, sizeof kSl - 1)) return 0;
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* buf = (n > 0 && n < (4u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    int panels = 0; DWORD on = 0;
    uint8_t* o = ok ? AdaptMenuPanels(buf, n, right, &panels, &on) : NULL;
    free(buf); buf = o; n = on;
    static char path[MAX_PATH];
    ok = o && panels > 0;
    if (ok) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
        strcat(dir, "W3TrueWidescreen_cache");
        CreateDirectoryA(dir, NULL);
        snprintf(path, sizeof path, "%s\\Menu-%s", dir, bn);
        FILE* f = fopen(path, "wb");
        ok = f && fwrite(buf, 1, n, f) == n;
        if (f) fclose(f);
    }
    free(buf);
    if (!ok) { logf_("menu panels %s not adapted", name); return 0; }
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    logf_("menu panels %s: %d panels widened (%d)", bn, panels, r);
    return r;
}


// ---- Battle.net login screen: the doors ---------------------------------------------------------------------------
// BattleNetMainFrame spans the whole screen (its fields and buttons go to the screen edges), but its picture,
// BattlenetLoginGlue.mdx, is two doors 0.4 wide each, drawn from the left edge of the frame. Each door is 2 x 3 tiles
// of 0.2 (posed open: left door at x -0.6..-0.2, right door at 1.0..1.4; their bones slide them in by 0.6). The model
// is moved to the centre of the screen and every door gets copies of its tiles past its outer edge: mirrored across
// the edge, then the plain tiles again past those, so the doors reach the screen edges with the pattern continuing
// (the outer corner ornaments meet their mirror images). The copies are skinned to the same bones, so the doors
// still slide in and out. Texture u = 0 is moved half a texel in, so the mirror line shows no wrapped-around column.
static uint8_t* AdaptBnetDoors(const uint8_t* d, DWORD n, float e, int* nTiles, DWORD* outN)
{
    if (n < 16 || memcmp(d, "MDLX", 4)) return NULL;
    DWORD p = 4, mtls = 0, mtlsN = 0, texs = 0, texsN = 0, geos = 0, geosN = 0;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) return NULL;
        if (!memcmp(d + p, "MTLS", 4)) { mtls = p + 8; mtlsN = sz; }
        else if (!memcmp(d + p, "TEXS", 4)) { texs = p + 8; texsN = sz; }
        else if (!memcmp(d + p, "GEOS", 4)) { geos = p; geosN = sz; }
        p += 8 + sz;
    }
    if (!mtls || !texs || !geos) return NULL;
    int matTex[64], nm = 0;
    for (DWORD m = mtls; m + 4 <= mtls + mtlsN; ) {
        DWORD msz = *(DWORD*)(d + m);
        if (nm >= 64 || msz < 36 || m + msz > mtls + mtlsN || memcmp(d + m + 12, "LAYS", 4)) return NULL;
        matTex[nm++] = *(int*)(d + m + 32);
        m += msz;
    }
    // the door tiles: geosets textured with Battlenet-Login-LeftDoor01..06 / RightDoor01..06
    DWORD tileAt[32], tileSz[32]; int tileSide[32], nt = 0;
    float outerL = 1e9f, outerR = -1e9f;
    for (DWORD g = geos + 8; g + 4 <= geos + 8 + geosN; ) {
        DWORD gsz = *(DWORD*)(d + g);
        if (gsz < 12 || g + gsz > geos + 8 + geosN) return NULL;
        DWORD vrtx = 0; int side = -1;
        for (DWORD q = g + 4; q + 8 <= g + gsz; ) {
            const uint8_t* t = d + q; DWORD c = *(DWORD*)(t + 4);
            if (!memcmp(t, "VRTX", 4)) { vrtx = q; q += 8 + c * 12; }
            else if (!memcmp(t, "NRMS", 4)) q += 8 + c * 12;
            else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + c * 4;
            else if (!memcmp(t, "PVTX", 4)) q += 8 + c * 2;
            else if (!memcmp(t, "GNDX", 4)) q += 8 + c;
            else if (!memcmp(t, "MATS", 4)) {
                int mat = *(int*)(t + 8 + c * 4), tex = mat >= 0 && mat < nm ? matTex[mat] : -1;
                if (tex >= 0 && (DWORD)(tex + 1) * 268 <= texsN) {
                    const char* tn = (const char*)d + texs + tex * 268 + 4;
                    for (int k = 1; k <= 6 && side < 0; k++) {
                        char l[32], r[32];
                        snprintf(l, sizeof l, "LeftDoor%02d.blp", k); snprintf(r, sizeof r, "RightDoor%02d.blp", k);
                        if (TexNameIs(tn, l)) side = 0; else if (TexNameIs(tn, r)) side = 1;
                    }
                }
                break;
            } else break;
        }
        if (side >= 0 && vrtx) {
            if (nt >= 32) return NULL;
            DWORD c = *(DWORD*)(d + vrtx + 4);
            for (DWORD i = 0; i < c; i++) {
                float x = *(const float*)(d + vrtx + 8 + i * 12);
                if (side == 0) outerL = fminf(outerL, x); else outerR = fmaxf(outerR, x);
            }
            tileAt[nt] = g; tileSz[nt] = gsz; tileSide[nt++] = side;
        }
        g += gsz;
    }
    if (nt != 12 || outerL > 0.0f || outerR < 0.8f) return NULL;
    // copies: per tile, mirrored (k = 1) and plain one door width further out (k = 2)
    DWORD extra = 0;
    for (int i = 0; i < nt; i++) extra += tileSz[i] * 2;
    uint8_t* o = (uint8_t*)malloc(n + extra);
    if (!o) return NULL;
    memcpy(o, d, geos + 8 + geosN);
    uint8_t* w = o + geos + 8 + geosN;
    for (int k = 1; k <= 2; k++)
        for (int i = 0; i < nt; i++) {
            memcpy(w, d + tileAt[i], tileSz[i]);
            float a = tileSide[i] ? outerR : outerL, dw = tileSide[i] ? 0.8f : -0.8f;   // dw: two door widths outwards
            for (DWORD q = 4; q + 8 <= tileSz[i]; ) {
                uint8_t* t = w + q; DWORD c = *(DWORD*)(t + 4);
                if (!memcmp(t, "VRTX", 4)) {
                    for (DWORD v = 0; v < c; v++) { float* x = (float*)(t + 8 + v * 12); *x = k == 1 ? 2 * a - *x : *x + dw; }
                    q += 8 + c * 12;
                } else if (!memcmp(t, "NRMS", 4)) q += 8 + c * 12;
                else if (!memcmp(t, "PVTX", 4)) {
                    if (k == 1) for (DWORD v = 0; v + 2 < c; v += 3) {         // mirrored: keep the triangles' winding
                        unsigned short* ix = (unsigned short*)(t + 8 + v * 2), tmp = ix[1]; ix[1] = ix[2]; ix[2] = tmp;
                    }
                    q += 8 + c * 2;
                } else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + c * 4;
                else if (!memcmp(t, "GNDX", 4)) q += 8 + c;
                else if (!memcmp(t, "MATS", 4)) { while (q + 8 <= tileSz[i] && memcmp(w + q, "UVAS", 4)) q += 4; }
                else if (!memcmp(t, "UVAS", 4)) q += 8;
                else if (!memcmp(t, "UVBS", 4)) q += 8 + c * 8;
                else break;
            }
            w += tileSz[i];
        }
    memcpy(w, d + geos + 8 + geosN, n - (geos + 8 + geosN)); w += n - (geos + 8 + geosN);
    *(DWORD*)(o + geos + 4) = geosN + extra;
    DWORD on = (DWORD)(w - o);
    // all geosets: x moved to the screen centre; door tiles: u = 0 half a texel in (256-texel textures)
    p = 4;
    while (p + 8 <= on) {
        DWORD sz = *(DWORD*)(o + p + 4);
        if (!memcmp(o + p, "GEOS", 4)) {
            for (DWORD g = p + 8; g + 4 <= p + 8 + sz; ) {
                DWORD gsz = *(DWORD*)(o + g); int door = 0;
                for (DWORD q = g + 4; q + 8 <= g + gsz; ) {
                    uint8_t* t = o + q; DWORD c = *(DWORD*)(t + 4);
                    if (!memcmp(t, "VRTX", 4)) { for (DWORD v = 0; v < c; v++) *(float*)(t + 8 + v * 12) += e; q += 8 + c * 12; }
                    else if (!memcmp(t, "NRMS", 4)) q += 8 + c * 12;
                    else if (!memcmp(t, "PVTX", 4)) q += 8 + c * 2;
                    else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + c * 4;
                    else if (!memcmp(t, "GNDX", 4)) q += 8 + c;
                    else if (!memcmp(t, "MATS", 4)) {
                        int mat = *(int*)(t + 8 + c * 4), tex = mat >= 0 && mat < nm ? matTex[mat] : -1;
                        door = tex >= 0 && (DWORD)(tex + 1) * 268 <= texsN && strstr((const char*)d + texs + tex * 268 + 4, "Door0")
                            && !TexNameIs((const char*)d + texs + tex * 268 + 4, "Door07.blp") && !TexNameIs((const char*)d + texs + tex * 268 + 4, "Door08.blp")
                            && !TexNameIs((const char*)d + texs + tex * 268 + 4, "Door09.blp");
                        while (q + 8 <= g + gsz && memcmp(o + q, "UVAS", 4)) q += 4;
                    }
                    else if (!memcmp(t, "UVAS", 4)) q += 8;
                    else if (!memcmp(t, "UVBS", 4)) {
                        if (door) for (DWORD v = 0; v < c; v++) { float* u = (float*)(t + 8 + v * 8); if (*u < 0.5f / 256) *u = 0.5f / 256; }
                        q += 8 + c * 8;
                    } else break;
                }
                g += gsz;
            }
        } else if (!memcmp(o + p, "PIVT", 4)) {
            for (DWORD v = 0; v + 12 <= sz; v += 12) *(float*)(o + p + 8 + v) += e;
        }
        p += 8 + sz;
    }
    *nTiles = nt; *outN = on;
    return o;
}
static int ServeBnetDoors(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (!g_enabled || !g_glue || !g_menuEdges || g_aspect < 1.34) return 0;
    const char* bn = strrchr(name, '\\'); bn = bn ? bn + 1 : name;
    if (_strnicmp(bn, "BattlenetLoginGlue.md", 21)) return 0;
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* buf = (n > 0 && n < (4u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    int tiles = 0; DWORD on = 0;
    uint8_t* o = ok ? AdaptBnetDoors(buf, n, g_e, &tiles, &on) : NULL;
    free(buf);
    static char path[MAX_PATH];
    ok = o != NULL;
    if (ok) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
        strcat(dir, "W3TrueWidescreen_cache");
        CreateDirectoryA(dir, NULL);
        snprintf(path, sizeof path, "%s\\Bnet-BattlenetLoginGlue.mdx", dir);
        FILE* f = fopen(path, "wb");
        ok = f && fwrite(o, 1, on, f) == on;
        if (f) fclose(f);
    }
    free(o);
    if (!ok) { logf_("Battle.net login doors %s not adapted", name); return 0; }
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    logf_("Battle.net login doors widened by %.4f (%d tiles copied) (%d)", g_e, tiles, r);
    return r;
}


// ---- Battle.net screens: the button bar --------------------------------------------------------------------------
// CenterPanel(-Expansion).mdx is one strip 0.8 wide (four tiles: 512, 512, 512 and 64 texels of 128 high, 2000
// texels per unit), drawn from the left edge of the screen-wide chat panel, with a lion medallion at each end, cut
// by the edge. It is moved to the centre (with the banner and the buttons) and continued to the screen edges:
// mirrored across each lion's centre line (the medallion becomes whole, then the ornament panel and the stud column
// next to it), then the plain metal between two button slots, mirrored back and forth. Each piece is a copy of a
// tile geoset (same bone) with its own x span and texture u span.
#define BB_CL 47.0f            // left lion centre (texels)
#define BB_CR 1551.0f          // right lion centre
#define BB_SL 321.0f           // left: last texel before the first button slot
#define BB_SR 1281.0f          // right: first texel after the last button slot (and its bevel)
// the middle rows past the mirrored part, back and forth: plain metal from a pillar between two slots, without their
// bevels, as light as the metal where the mirrored part stops (left: the pillar after the first slot; right: the
// pillar before the last one, lit like the right end -- the first one is darker than that, it showed as a seam)
#define BB_ML0 641.0f
#define BB_ML1 659.0f
#define BB_MR0 1180.0f          // (read from here, next to the mirrored part: the pillar's column closest to the metal
#define BB_MR1 1166.0f          // where that part stops, row by row)
#define BB_B0 330.0f           // the bar's top and bottom bands over a slot, without snow (past the studs)
#define BB_B1 530.0f
#define BB_R0 (11.0f / 128.0f) // rows of plain metal: below the bar's top edge, above its bottom edge (as by the studs)
#define BB_R1 (115.0f / 128.0f)
typedef struct { float x0, x1, s0, s1, v0, v1; int tile; } BbPiece;   // model x span (texels), texel span (s0 at x0), rows
static int BbTileOf(float a, float b) { float m = (a + b) * 0.5f; return m < 512 ? 0 : m < 1024 ? 1 : m < 1536 ? 2 : 3; }
// pieces from a texel range [a, b] mirrored back and forth, laid from x = start outwards (dir -1: to the left)
static int BbAlt(BbPiece* o, int n, int max, float start, float len, int dir, float a, float b, float v0, float v1)
{
    float w = fabsf(b - a), sg = b < a ? -1.0f : 1.0f, done = 0;   // (b < a: the range read backwards)
    for (int k = 0; done < len - 1e-4f && n < max; k++) {
        float part = len - done < w ? len - done : w;
        float xi = start + dir * done, xo = start + dir * (done + part);     // inner / outer end
        float si = k % 2 == 0 ? a : b, so = k % 2 == 0 ? a + sg * part : b - sg * part;
        o[n++] = dir < 0 ? (BbPiece){ xo, xi, so, si, v0, v1, BbTileOf(a, b) } : (BbPiece){ xi, xo, si, so, v0, v1, BbTileOf(a, b) };
        done += part;
    }
    return n;
}
static int BbPieces(float ex, float xr, BbPiece* o, int max)   // ex: texels to add on each side; xr: right end of the strip
{
    int n = 0;
    // left (x < 0): mirrored across the lion's centre line up to the first slot (the medallion, the ornament panel,
    // the studs); past that the bands go on as over a slot and the middle rows are plain metal
    float a = 2 * BB_CL - BB_SL, x0 = a > -ex ? a : -ex;
    o[n++] = (BbPiece){ x0, 0.0f, 2 * BB_CL - x0, 2 * BB_CL, 0.0f, 1.0f, 0 };
    if (x0 > -ex) {
        float len = x0 + ex;
        n = BbAlt(o, n, max, x0, len, -1, BB_B0, BB_B1, 0.0f, BB_R0);
        n = BbAlt(o, n, max, x0, len, -1, BB_B0, BB_B1, BB_R1, 1.0f);
        n = BbAlt(o, n, max, x0, len, -1, BB_ML0, BB_ML1, BB_R0, BB_R1);
    }
    // right (x > 1600)
    float b = 2 * BB_CR - BB_SR, x1 = b < xr + ex ? b : xr + ex;
    if (n < max) { o[n] = (BbPiece){ xr, x1, 2 * BB_CR - xr, 2 * BB_CR - x1, 0.0f, 1.0f, 0 }; o[n].tile = BbTileOf(o[n].s0, o[n].s1); n++; }
    if (x1 < xr + ex) {
        float len = xr + ex - x1;
        n = BbAlt(o, n, max, x1, len, 1, BB_B0, BB_B1, 0.0f, BB_R0);
        n = BbAlt(o, n, max, x1, len, 1, BB_B0, BB_B1, BB_R1, 1.0f);
        n = BbAlt(o, n, max, x1, len, 1, BB_MR0, BB_MR1, BB_R0, BB_R1);
    }
    return n;
}
static uint8_t* AdaptBnetBar(const uint8_t* d, DWORD n, float e, int* nPieces, DWORD* outN)
{
    if (n < 16 || memcmp(d, "MDLX", 4)) return NULL;
    DWORD p = 4, mtls = 0, mtlsN = 0, texs = 0, texsN = 0, geos = 0, geosN = 0;
    while (p + 8 <= n) {
        DWORD sz = *(DWORD*)(d + p + 4);
        if (p + 8 + sz > n) return NULL;
        if (!memcmp(d + p, "MTLS", 4)) { mtls = p + 8; mtlsN = sz; }
        else if (!memcmp(d + p, "TEXS", 4)) { texs = p + 8; texsN = sz; }
        else if (!memcmp(d + p, "GEOS", 4)) { geos = p; geosN = sz; }
        p += 8 + sz;
    }
    if (!mtls || !texs || !geos) return NULL;
    int matTex[16], nm = 0;
    for (DWORD m = mtls; m + 4 <= mtls + mtlsN; ) {
        DWORD msz = *(DWORD*)(d + m);
        if (nm >= 16 || msz < 36 || m + msz > mtls + mtlsN || memcmp(d + m + 12, "LAYS", 4)) return NULL;
        matTex[nm++] = *(int*)(d + m + 32); m += msz;
    }
    // the four tile geosets (one quad each), by texture CenterPanel01..04
    DWORD tg[4] = { 0 }, tsz[4] = { 0 }, tv[4] = { 0 }, tu[4] = { 0 };
    for (DWORD g = geos + 8; g + 4 <= geos + 8 + geosN; ) {
        DWORD gsz = *(DWORD*)(d + g), vrtx = 0, uvbs = 0; int tile = -1;
        if (gsz < 12 || g + gsz > geos + 8 + geosN) return NULL;
        for (DWORD q = g + 4; q + 8 <= g + gsz; ) {
            const uint8_t* t = d + q; DWORD c = *(DWORD*)(t + 4);
            if (!memcmp(t, "VRTX", 4)) { vrtx = q; q += 8 + c * 12; }
            else if (!memcmp(t, "NRMS", 4)) q += 8 + c * 12;
            else if (!memcmp(t, "PTYP", 4) || !memcmp(t, "PCNT", 4) || !memcmp(t, "MTGC", 4)) q += 8 + c * 4;
            else if (!memcmp(t, "PVTX", 4)) q += 8 + c * 2;
            else if (!memcmp(t, "GNDX", 4)) q += 8 + c;
            else if (!memcmp(t, "MATS", 4)) {
                int mat = *(int*)(t + 8 + c * 4), tex = mat >= 0 && mat < nm ? matTex[mat] : -1;
                if (tex >= 0 && (DWORD)(tex + 1) * 268 <= texsN) {
                    const char* tn = (const char*)d + texs + tex * 268 + 4;
                    for (int k = 0; k < 4; k++) {
                        char a[32], b[32]; snprintf(a, sizeof a, "CenterPanel%02d", k + 1); snprintf(b, sizeof b, "CenterPanel%02d-Expansion", k + 1);
                        if (TexNameIs(tn, a) || TexNameIs(tn, b) || (snprintf(a, sizeof a, "CenterPanel%02d.blp", k + 1), TexNameIs(tn, a))
                            || (snprintf(b, sizeof b, "CenterPanel%02d-Expansion.blp", k + 1), TexNameIs(tn, b))) tile = k;
                    }
                }
                while (q + 8 <= g + gsz && memcmp(d + q, "UVAS", 4)) q += 4;
            }
            else if (!memcmp(t, "UVAS", 4)) q += 8;
            else if (!memcmp(t, "UVBS", 4)) { uvbs = q; q += 8 + c * 8; }
            else break;
        }
        if (tile >= 0 && vrtx && uvbs && *(DWORD*)(d + vrtx + 4) == 4 && *(DWORD*)(d + uvbs + 4) == 4) { tg[tile] = g; tsz[tile] = gsz; tv[tile] = vrtx - g; tu[tile] = uvbs - g; }
        g += gsz;
    }
    for (int k = 0; k < 4; k++) if (!tg[k]) return NULL;
    float xr = 0.0f;
    for (int c = 0; c < 4; c++) xr = fmaxf(xr, *(const float*)(d + tg[3] + tv[3] + 8 + c * 12) * 2000.0f);
    static BbPiece pc[256]; int np = BbPieces(e * 2000.0f + (1600.0f - xr), xr, pc, 256);
    DWORD extra = 0;
    for (int i = 0; i < np; i++) extra += tsz[pc[i].tile];
    uint8_t* o = (uint8_t*)malloc(n + extra);
    if (!o) return NULL;
    memcpy(o, d, geos + 8 + geosN);
    uint8_t* w = o + geos + 8 + geosN;
    static const float tileS0[4] = { 0, 512, 1024, 1536 }, tileW[4] = { 512, 512, 512, 64 };
    for (int i = 0; i < np; i++) {
        int t = pc[i].tile;
        memcpy(w, d + tg[t], tsz[t]);
        float* v = (float*)(w + tv[t] + 8); float* uv = (float*)(w + tu[t] + 8);
        float xmin = 1e9f, xmax = -1e9f, ylo = 1e9f, yhi = -1e9f;
        for (int c = 0; c < 4; c++) { xmin = fminf(xmin, v[c * 3]); xmax = fmaxf(xmax, v[c * 3]); ylo = fminf(ylo, v[c * 3 + 1]); yhi = fmaxf(yhi, v[c * 3 + 1]); }
        for (int c = 0; c < 4; c++) {
            int atMin = fabsf(v[c * 3] - xmin) < fabsf(v[c * 3] - xmax), top = fabsf(v[c * 3 + 1] - yhi) < fabsf(v[c * 3 + 1] - ylo);
            float x = atMin ? pc[i].x0 : pc[i].x1, sx = atMin ? pc[i].s0 : pc[i].s1, vv = top ? pc[i].v0 : pc[i].v1;
            v[c * 3] = x / 2000.0f;
            v[c * 3 + 1] = yhi - vv * (yhi - ylo);                  // texture v 0 at the top edge
            uv[c * 2 + 1] = vv;
            float u = (sx - tileS0[t]) / tileW[t];
            uv[c * 2] = u < 0.5f / tileW[t] ? 0.5f / tileW[t] : u > 1 - 0.5f / tileW[t] ? 1 - 0.5f / tileW[t] : u;
        }
        w += tsz[t];
    }
    memcpy(w, d + geos + 8 + geosN, n - (geos + 8 + geosN)); w += n - (geos + 8 + geosN);
    *(DWORD*)(o + geos + 4) = geosN + extra;
    DWORD on = (DWORD)(w - o);
    // everything to the centre of the screen
    for (p = 4; p + 8 <= on; ) {
        DWORD sz = *(DWORD*)(o + p + 4);
        if (!memcmp(o + p, "GEOS", 4)) {
            for (DWORD g = p + 8; g + 4 <= p + 8 + sz; ) {
                DWORD gsz = *(DWORD*)(o + g);
                if (!memcmp(o + g + 4, "VRTX", 4)) for (DWORD v = 0, c = *(DWORD*)(o + g + 8); v < c; v++) *(float*)(o + g + 12 + v * 12) += e;
                g += gsz;
            }
        } else if (!memcmp(o + p, "PIVT", 4)) for (DWORD v = 0; v + 12 <= sz; v += 12) *(float*)(o + p + 8 + v) += e;
        p += 8 + sz;
    }
    *nPieces = np; *outN = on;
    return o;
}
static int ServeBnetBar(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (!g_enabled || !g_glue || !g_menuWide || g_aspect < 1.34) return 0;
    const char* bn = strrchr(name, '\\'); bn = bn ? bn + 1 : name;
    if (_stricmp(bn, "CenterPanel.mdx") && _stricmp(bn, "CenterPanel-Expansion.mdx") && _stricmp(bn, "CenterPanel.mdl") && _stricmp(bn, "CenterPanel-Expansion.mdl")) return 0;
    if (!strstr(name, "BattlenetChatGlue") && !strstr(name, "BattleNetChatGlue")) return 0;
    HANDLE h = 0;
    if (!orig_SOpenEx(mpq, name, scope, &h) || !h) return 0;
    DWORD n = S_Size(h, NULL), got = 0;
    uint8_t* buf = (n > 0 && n < (1u << 20)) ? (uint8_t*)malloc(n) : NULL;
    int ok = buf && S_Read(h, buf, n, &got, NULL) && got == n;
    S_Close(h);
    int pieces = 0; DWORD on = 0;
    uint8_t* o = ok ? AdaptBnetBar(buf, n, g_e - g_menuCut, &pieces, &on) : NULL;
    free(buf);
    static char path[MAX_PATH];
    ok = o != NULL;
    if (ok) {
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, '\\'); if (sl) *(sl + 1) = 0;
        strcat(dir, "W3TrueWidescreen_cache");
        CreateDirectoryA(dir, NULL);
        snprintf(path, sizeof path, "%s\\Bnet-%s", dir, bn);
        FILE* f = fopen(path, "wb");
        ok = f && fwrite(o, 1, on, f) == on;
        if (f) fclose(f);
    }
    free(o);
    if (!ok) { logf_("Battle.net button bar %s not adapted", name); return 0; }
    BOOL r = orig_SOpenEx(mpq, path, (scope & ~4u) | 3u, ph);
    if (r && (u32)*ph < 0x10000) { CloseHandle(*ph); *ph = 0; r = FALSE; }
    logf_("Battle.net button bar centred and continued to the edges (%d pieces) (%d)", pieces, r);
    return r;
}

static BOOL __stdcall SOpenEx_hook(HANDLE mpq, const char* name, DWORD scope, HANDLE* ph)
{
    if (name && ph && !IsBadStringPtrA(name, MAX_PATH) && ServeBnetBar(mpq, name, scope, ph)) return TRUE;
    if (name && ph && !IsBadStringPtrA(name, MAX_PATH) && ServeBnetDoors(mpq, name, scope, ph)) return TRUE;
    if (name && ph && !IsBadStringPtrA(name, MAX_PATH) && ServeMenuPanels(mpq, name, scope, ph)) return TRUE;
    if (name && ph && !IsBadStringPtrA(name, MAX_PATH) && ServeWidenedLoading(mpq, name, scope, ph)) return TRUE;
    if (name && ph && !IsBadStringPtrA(name, MAX_PATH)) CheckMapScript(mpq, name, scope);
    if (g_fadeFix && g_fadeState >= 0 && name && ph && !IsBadStringPtrA(name, MAX_PATH) && SameName(name, kFadeName)) {
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

// ---- text shadows. The text renderer takes a string's shadow offset in screen fractions (x * screen width in
//      pixels), while the interface passes it in its own units, which only matched on 4:3 (0.8 wide = the screen); on
//      a wider screen the shadow slides right with the screen width (by 4x on 32:9). The x offset is scaled to 0.8 /
//      the interface width, the vertical one is already right. ----
typedef void (__fastcall *TextShadow_t)(u32 str, const u32* color, float* off);
static TextShadow_t orig_TextShadow;
static void __fastcall TextShadow_hook(u32 str, const u32* color, float* off)
{
    if (!g_enabled || !off || IsBadReadPtr(off, 8)) { orig_TextShadow(str, color, off); return; }
    float o[2] = { off[0] * (float)(4.0 / 3.0 / g_aspect), off[1] };
    static int n; if (g_debug && n < 8) { n++; dlogf_("text shadow %.4f %.4f -> %.4f", off[0], off[1], o[0]); }
    orig_TextShadow(str, color, o);
}
static void InstallTextShadow(void)
{
    static const Code kTs[3] = { { 0x7B8FE0, 5, "\x56\x8B\xF1\x85\xF6" }, { 0x122720, 6, "\x55\x8B\xEC\x56\x8B\xF1" },
                                 { 0x150DD0, 6, "\x55\x8B\xEC\x56\x8B\xF1" } };
    const Code* c = &kTs[BuildIdx()];
    if (!CodeIs(c)) { logf_("text shadow code not recognised, shadows left as they are"); return; }
    orig_TextShadow = (TextShadow_t)MakeTrampoline(g_base + c->rva, c->len);
    if (orig_TextShadow) WriteJmp(g_base + c->rva, (u32)TextShadow_hook);
}
static void InstallFadeFix(void)
{
    // (always installed: also recognises DracoL1ch's DotA maps, see CheckMapScript)
    HMODULE st = GetModuleHandleA("Storm.dll");
    if (!st) return;
    u32 f = (u32)GetProcAddress(st, (LPCSTR)268);
    S_Read  = (SRead_t)GetProcAddress(st, (LPCSTR)269);
    S_Size  = (SSize_t)GetProcAddress(st, (LPCSTR)265);
    S_Close = (SClose_t)GetProcAddress(st, (LPCSTR)253);
    int len = 0;
    if (f && !memcmp((void*)f, "\x81\xEC\x1C\x01\x00\x00", 6)) len = 6;                  // 1.26
    else if (f && !memcmp((void*)f, "\x55\x8B\xEC\x81\xEC\x1C\x01\x00\x00", 9)) len = 9;  // 1.27
    if (!len || !S_Read || !S_Size || !S_Close) {
        logf_("Storm.dll not recognised, campaign fade fix off"); return;
    }
    orig_SOpenEx = (SOpenEx_t)MakeTrampoline(f, len);
    if (orig_SOpenEx) WriteJmp(f, (u32)SOpenEx_hook);
}

// ---- cinematics: DirectShow plays them in a child window, outside the game's Direct3D (so RTX HDR etc. never
//      see them). If W3TrueWidescreen\MpcVideoRenderer.ax is present, it is put into the game's filter graph before the
//      game renders the movie, so the video goes through MPC Video Renderer (RTX Video HDR / super resolution).
//      The .ax is loaded directly from the game folder; it does not have to be registered. ----
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
// 1.28 keeps the movie's filter graph (or a reference to it) after the movie, and MPC Video Renderer refuses to
// create a second instance while one exists: the next movie fell back to the system renderer. So the graph is
// held from the moment the renderer joins it, and when the game's play function returns (the movie has ended)
// the renderer is taken out of it and the graph let go.
typedef ULONG (__stdcall *AddRef_t)(void*);
typedef HRESULT (__stdcall *RemoveFilter_t)(void*, void*);
static void* g_movieGraph; static void* g_movieVr;
static void MovieEnded(void)
{
    if (!g_movieGraph) return;
    HRESULT hr = ((RemoveFilter_t)VTBL(g_movieGraph)[4])(g_movieGraph, g_movieVr);
    ULONG left = ((Release_t)VTBL(g_movieGraph)[2])(g_movieGraph);
    dlogf_("movies: renderer taken out of the graph (%08lX), graph references left %lu", hr, left);
    g_movieGraph = g_movieVr = NULL;
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
        if (SUCCEEDED(hr) && !g_movieGraph) {              // kept until the movie has ended (see MovieEnded)
            ((AddRef_t)VTBL(fg)[1])(fg); g_movieGraph = fg; g_movieVr = flt;
        }
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
static int InstallPlayHook(void);
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

// ---- subtitles inside the video: XySubFilter interfaces (SubRenderIntf.h) ----
static const GUID kIID_SubRenderConsumer = {0x9DF90966,0xFE9F,0x4F0E,{0x88,0x1E,0xDA,0xF8,0xA5,0x72,0xD9,0x00}};
typedef struct { void** vt; } ComObj;
static void* g_subConsumer;                 // MPC VR's ISubRenderConsumer
static int g_subW, g_subH;                   // output rect of our frames (the renderer window)
static uint32_t* g_subPix; static int g_subBw, g_subBh, g_subBx, g_subBy; static ULONGLONG g_subId = 1;
static HRESULT __stdcall Sub_QI(ComObj* t, REFIID r, void** o)
{
    *o = t; return S_OK;                     // one interface per object; callers only ask for what it is
}
static ULONG __stdcall Sub_AddRef(ComObj* t) { (void)t; return 2; }
static ULONG __stdcall Sub_Release(ComObj* t) { (void)t; return 1; }
static HRESULT __stdcall Sub_GetNI(ComObj* t, LPCSTR f, void* v) { (void)t; (void)v; static int n; if (n++ < 40) dlogf_("subs: provider Get %s", f); return E_NOTIMPL; }
static HRESULT __stdcall Sub_GetNI3(ComObj* t, LPCSTR f, void* v, int* c) { (void)t; (void)v; (void)c; static int n; if (n++ < 40) dlogf_("subs: provider Get %s", f); return E_NOTIMPL; }
static HRESULT __stdcall Sub_GetString(ComObj* t, LPCSTR f, LPWSTR* v, int* c)
{
    (void)t;
    const WCHAR* s = !strcmp(f, "name") ? L"W3TrueWidescreen" : !strcmp(f, "version") ? L"1.8" : !strcmp(f, "yuvMatrix") ? L"None" : NULL;
    if (!s) { static int n; if (n++ < 40) dlogf_("subs: provider GetString %s", f); return E_NOTIMPL; }
    int len = lstrlenW(s);
    *v = (LPWSTR)LocalAlloc(0, (len + 1) * sizeof(WCHAR)); if (!*v) return E_OUTOFMEMORY;
    lstrcpyW(*v, s); if (c) *c = len;
    return S_OK;
}
static HRESULT __stdcall Sub_GetBool(ComObj* t, LPCSTR f, uint8_t* v)
{
    (void)t;
    if (!strcmp(f, "combineBitmaps")) { *v = 0; return S_OK; }
    if (!strcmp(f, "isMovable")) { *v = 1; return S_OK; }
    static int n; if (n++ < 40) dlogf_("subs: provider GetBool %s", f); return E_NOTIMPL;
}
static HRESULT __stdcall Sub_Set4(ComObj* t, LPCSTR f, DWORD a) { (void)t; (void)a; dlogf_("subs: provider Set %s", f); return S_OK; }
static HRESULT __stdcall Sub_Set8(ComObj* t, LPCSTR f, DWORD a, DWORD b) { (void)t; (void)a; (void)b; dlogf_("subs: provider Set %s", f); return S_OK; }
static HRESULT __stdcall Sub_Set16(ComObj* t, LPCSTR f, DWORD a, DWORD b, DWORD c, DWORD d) { (void)t; (void)a; (void)b; (void)c; (void)d; dlogf_("subs: provider Set %s", f); return S_OK; }
// frame
static HRESULT __stdcall Frm_Rect(ComObj* t, RECT* r) { (void)t; if (!r) return E_POINTER; r->left = 0; r->top = 0; r->right = g_subW; r->bottom = g_subH; return S_OK; }
static HRESULT __stdcall Frm_Count(ComObj* t, int* c) { (void)t; if (!c) return E_POINTER; *c = g_subPix ? 1 : 0; return S_OK; }
static HRESULT __stdcall Frm_Bitmap(ComObj* t, int i, ULONGLONG* id, POINT* pos, SIZE* sz, LPCVOID* px, int* pitch)
{
    (void)t;
    if (i != 0 || !g_subPix) return E_INVALIDARG;
    if (id) *id = g_subId;
    if (pos) { pos->x = g_subBx; pos->y = g_subBy; }
    if (sz) { sz->cx = g_subBw; sz->cy = g_subBh; }
    if (px) *px = g_subPix;
    if (pitch) *pitch = g_subBw * 4;
    static int n; if (n++ < 5) dlogf_("subs: bitmap asked (%p %p %p %p %p)", id, pos, sz, px, pitch);
    return S_OK;
}
static void* kFrameVt[] = { Sub_QI, Sub_AddRef, Sub_Release, Frm_Rect, Frm_Rect, Frm_Count, Frm_Bitmap };
static ComObj g_subFrame = { kFrameVt };
static void SubUpdate(void);
typedef HRESULT (__stdcall *Deliver_t)(void*, LONGLONG, LONGLONG, LPVOID, void*);
static HRESULT __stdcall Prov_Request(ComObj* t, LONGLONG a, LONGLONG b, LPVOID ctx)
{
    (void)t;
    static int n; if (n++ < 3) dlogf_("subs: frame requested %lld..%lld", a, b);
    SubUpdate();
    void* c = g_subConsumer;
    if (!c) return E_FAIL;
    return ((Deliver_t)VTBL(c)[22])(c, a, b, ctx, &g_subFrame);
}
static HRESULT __stdcall Prov_Disconnect(ComObj* t) { (void)t; dlogf_("subs: provider disconnected"); g_subConsumer = NULL; return S_OK; }
static void* kProvVt[] = { Sub_QI, Sub_AddRef, Sub_Release,
    Sub_GetBool, Sub_GetNI, Sub_GetNI, Sub_GetNI, Sub_GetNI, Sub_GetNI, Sub_GetString, Sub_GetNI3,
    Sub_Set4, Sub_Set4, Sub_Set8, Sub_Set16, Sub_Set8, Sub_Set8, Sub_Set8, Sub_Set8,
    Prov_Request, Prov_Disconnect };
static ComObj g_subProvider = { kProvVt };
// test bitmap: one line of white text with a black outline, premultiplied ARGB
#define SUB_MAXL 6
static void SubRenderLines(const WCHAR** lines, int nl)
{
    int W = g_subW, H = g_subH; if (W <= 0 || H <= 0) return;
    if (nl <= 0) { g_subPix = NULL; return; }
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontW(-(int)(H / 44.0 * g_subSize / 100.0), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
    HGDIOBJ of = SelectObject(dc, font);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    int lh = tm.tmHeight + tm.tmExternalLeading, maxw = 0;
    for (int i = 0; i < nl; i++) { SIZE ts; GetTextExtentPoint32W(dc, lines[i], lstrlenW(lines[i]), &ts); if (ts.cx > maxw) maxw = ts.cx; }
    int bw = maxw + 16, bh = lh * nl + 16;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB } };
    void* bits = NULL; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ ob = SelectObject(dc, bm);
    uint8_t* outline = (uint8_t*)calloc(bw * bh, 1); uint8_t* fill = (uint8_t*)calloc(bw * bh, 1);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    for (int pass = 0; pass < 2; pass++) {
        memset(bits, 0, bw * bh * 4);
        SetTextAlign(dc, TA_CENTER | TA_TOP);
        for (int l = 0; l < nl; l++) {
            int y = 8 + l * lh, x = bw / 2, len = lstrlenW(lines[l]);
            if (pass == 0) { for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) TextOutW(dc, x + dx, y + dy, lines[l], len); }
            else TextOutW(dc, x, y, lines[l], len);
        }
        GdiFlush();
        uint8_t* dst = pass ? fill : outline;
        for (int i = 0; i < bw * bh; i++) dst[i] = ((uint8_t*)bits)[i * 4 + 1];
    }
    uint32_t* px = (uint32_t*)malloc(bw * bh * 4);
    for (int i = 0; i < bw * bh; i++) {
        uint32_t a = outline[i] > fill[i] ? outline[i] : fill[i], c = fill[i];
        c = c * (uint32_t)g_subBright / 100;
        px[i] = (a << 24) | (c << 16) | (c << 8) | c;                       // premultiplied: white * fill
    }
    free(outline); free(fill);
    SelectObject(dc, ob); DeleteObject(bm); SelectObject(dc, of); DeleteObject(font); DeleteDC(dc);
    static uint32_t* prev[2]; free(prev[1]); prev[1] = prev[0]; prev[0] = px;  // the renderer may still read the last one
    int y0 = *(int*)(g_base + G->subY0);                                       // under the picture, as the game draws them
    g_subBw = bw; g_subBh = bh; g_subBx = (W - bw) / 2;
    g_subBy = (y0 > 0 && y0 + bh < H) ? y0 + 8 : H - bh - H / 20;
    g_subId++;
    g_subPix = px;
}
static void SubUpdate(void)
{
    static WCHAR last[2048]; WCHAR cur[2048]; cur[0] = 0;
    const WCHAR* lines[SUB_MAXL]; int nl = 0;
    int count = *(int*)(g_base + G->subCount);
    u32* idx = *(u32**)(g_base + G->subIdx); uint8_t* tab = *(uint8_t**)(g_base + G->subTab);
    if (count > 0 && idx && tab && !IsBadReadPtr(idx, count * 4)) {
        for (int i = 0; i < count && nl < SUB_MAXL; i++) {
            const WCHAR* t = *(const WCHAR**)(tab + idx[i] * 20 + 8);
            if (!t || IsBadStringPtrW(t, 512)) continue;
            lines[nl++] = t;
            if (lstrlenW(cur) + lstrlenW(t) + 2 < 2048) { lstrcatW(cur, t); lstrcatW(cur, L"\n"); }
        }
    }
    if (!lstrcmpW(cur, last)) return;
    lstrcpyW(last, cur);
    SubRenderLines(lines, nl);
}
// size of the window the movie is shown in: 1.26 keeps it in globals (the display mode set for the movie),
// 1.27 places the movie in the game window's client area
static void MovieWindowSize(void* anyGraphItf, int* w, int* h)
{
    *w = *h = 0;
    if (G->movieW) { *w = *(int*)(g_base + G->movieW); *h = *(int*)(g_base + G->movieH); return; }
    static const GUID kIID_IVideoWindow = {0x56a868b4,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
    void* vw = NULL; OAHWND_ owner = 0; RECT r;
    if (FAILED(((QI_t)VTBL(anyGraphItf)[0])(anyGraphItf, &kIID_IVideoWindow, &vw)) || !vw) return;
    typedef HRESULT (__stdcall *GetOwner_t)(void*, OAHWND_*);
    if (SUCCEEDED(((GetOwner_t)VTBL(vw)[30])(vw, &owner)) && owner && GetClientRect((HWND)owner, &r)) { *w = r.right; *h = r.bottom; }
    ((Release_t)VTBL(vw)[2])(vw);
}
static void ConnectSubtitles(void* anyGraphItf)
{
    void* fg = NULL; void* en = NULL; void* f = NULL; ULONG got = 0;
    if (FAILED(((QI_t)VTBL(anyGraphItf)[0])(anyGraphItf, &kIID_IFilterGraph, &fg)) || !fg) return;
    if (SUCCEEDED(((EnumF_t)VTBL(fg)[5])(fg, &en)) && en) {
        while (((Next_t)VTBL(en)[3])(en, 1, &f, &got) == S_OK && got) {
            void* c = NULL;
            if (SUCCEEDED(((QI_t)VTBL(f)[0])(f, &kIID_SubRenderConsumer, &c)) && c) {
                MovieWindowSize(anyGraphItf, &g_subW, &g_subH);
                g_subConsumer = c;
                typedef HRESULT (__stdcall *Connect_t)(void*, void*);
                HRESULT hr = ((Connect_t)VTBL(c)[20])(c, &g_subProvider);
                // keep no reference: the renderer calls the provider only while it exists (and disconnects it when
                // it goes). A reference kept here kept the renderer alive after its movie, and MPC Video Renderer
                // refuses to create a second instance while one exists: the next movie fell back to the system renderer.
                ((Release_t)VTBL(c)[2])(c);
                logf_("movies: subtitle provider connected to the renderer (%08lX), frame %dx%d", hr, g_subW, g_subH);
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
    ConnectSubtitles(vw);
    // The game sizes the video window to the picture (letterbox strips are its parent window). With
    // MovieFullWindow=1 the renderer window covers the whole movie window and draws the letterbox itself:
    // a flip-model swap chain that covers the screen is shown by Windows directly (independent flip),
    // as in a fullscreen video player, instead of being composited as a smaller window.
    long ww = w, wh = h, dl = 0, dt = 0;
    int mw, mh; MovieWindowSize(vw, &mw, &mh);
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
    if (CodeIs(&G->movieSize) && CodeIs(&G->moviePos))
        WriteMem(g_base + G->movieSize.rva, G->movieSizeNew, G->movieSize.len);   // (see the address table)
    else { logf_("movies: movie window code not recognised, MPC Video Renderer off"); g_mpcvrGetClass = NULL; g_mpcvrPath[0] = 0; return; }
    // MPC Video Renderer draws into the rectangle given by IBasicVideo::SetDestinationPosition, which the game
    // never calls (the system renderer defaults to the whole window). Route the game's SetWindowPosition call
    // through a wrapper that also sets the destination to the whole window.
    {
        u32 f = (u32)SetWinPos_wrap;
        if (G->moviePos.len == 9) {                                // 1.26: mov eax,[edi+9Ch] -> mov eax, SetWinPos_wrap ; nop
            uint8_t p[6] = { 0xb8, 0, 0, 0, 0, 0x90 }; memcpy(p + 1, &f, 4);
            WriteMem(g_base + G->moviePos.rva, p, 6);
        } else if (G->moviePos.len == 6) {                         // 1.28: call [esi+9Ch] -> call SetWinPos_wrap ; nop
            uint8_t p[6] = { 0xe8, 0, 0, 0, 0, 0x90 }; int32_t rel = (int32_t)(f - (g_base + G->moviePos.rva + 5)); memcpy(p + 1, &rel, 4);
            WriteMem(g_base + G->moviePos.rva, p, 6);
        } else {                                                   // 1.27: mov edx,[ecx] ; call [edx+9Ch] -> mov eax, SetWinPos_wrap ; call eax ; nop
            uint8_t p[8] = { 0xb8, 0, 0, 0, 0, 0xff, 0xd0, 0x90 }; memcpy(p + 1, &f, 4);
            WriteMem(g_base + G->moviePos.rva, p, 8);
        }
    }
    u32 iat = FindImport("ole32.dll", "CoCreateInstance");
    if (!iat) { logf_("movies: CoCreateInstance import not found, MPC Video Renderer off"); g_mpcvrGetClass = NULL; g_mpcvrPath[0] = 0; return; }
    if (*(u32*)iat != (u32)GetProcAddress(GetModuleHandleA("ole32.dll"), "CoCreateInstance") &&
        *(u32*)iat != (u32)GetProcAddress(GetModuleHandleA("combase.dll"), "CoCreateInstance"))
        dlogf_("movies: CoCreateInstance import is already hooked by something else, chaining");
    orig_CoCreate = (CoCreate_t)*(u32*)iat;
    u32 hook = (u32)CoCreate_hook;
    WriteMem(iat, &hook, 4);
    if (!InstallPlayHook()) dlogf_("movies: play function not recognised, the renderer is not released after a movie");
    logf_("movies: MPC Video Renderer on (loaded at the first movie)");
}

// ---- cinematics. For every movie the game switches the display to 800x600 (ChangeDisplaySettings) and
//      brightens the whole screen with a gamma ramp of x^(1/2.2): on a modern display the movie ends up
//      low-resolution, washed out, and the near-black noise of these DivX streams is lifted into visible
//      blotches. MovieNativeMode=1 keeps the desktop mode and the gamma untouched; the movie window then
//      covers the native screen (the game already scales the picture to the window width). ----
static void PatchCallGamma(u32 at)     // call [SetDeviceGammaRamp]  ->  add esp,8 (drop its 2 arguments)
{
    uint8_t want[6] = { 0xff, 0x15 }; u32 slot = FindImport("gdi32.dll", "SetDeviceGammaRamp"); memcpy(want + 2, &slot, 4);
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
    if (g_dotaMode) return ((SBSetTex_t)(g_base + G->sbSetTex))(self, edx, a1, a2);
    static const uint8_t bg = 0x00, art = 0x02;
    WriteMem(g_base + G->sbLayerPush + 3, &bg, 1);       // CSimpleTexture(parent, layer 2 -> 0, ...)
    int r = ((SBSetTex_t)(g_base + G->sbSetTex))(self, edx, a1, a2);
    WriteMem(g_base + G->sbLayerPush + 3, &art, 1);
    return r;
}
// The simple UI is drawn level by level (depth in the frame tree), each level layer by layer, frames of one
// level in the order they were shown. Unit bars and the console are both top-level (level 0) frames and the
// console was shown first, so it is drawn under the bars. Whenever a unit bar is shown, move the other
// top-level frames behind it in that order (the engine's own register call appends at the end).
typedef void (__fastcall *TopRegister_t)(u32 top, u32 edx, u32 frame, u32 level);
static void BarsFirst(void)
{
    if (g_dotaMode) return;
    u32 top = *(u32*)(g_base + G->simpleTop);                      // CSimpleTop
    if (!top || IsBadReadPtr((void*)top, 0x200)) return;
    u32 others[96]; int n = 0;
    int node = *(int*)(top + 0x194);                            // level 0 list: first node
    while (node > 0 && n < 96 && !IsBadReadPtr((void*)node, 12)) {
        u32 f = *(u32*)(node + 8);
        if (f && VtRva(f) != VT(StatBar) && f != *(u32*)(top + 0x16C) && !IsOccupTree(f)) others[n++] = f;   // (occupancy marks too)   // not bars, not the mouse focus
        node = *(int*)(node + 4);
    }
    TopRegister_t reg = (TopRegister_t)(g_base + G->topRegister.rva);
    for (int i = 0; i < n; i++) reg(top, 0, others[i], 0);
    static int logged; if (logged++ < 3) dlogf_("health bars first: %d other top-level frames moved behind", n);
}
static int __fastcall StatBarShow_hook(u32 self, u32 edx)
{
    int wasShown = *(u32*)(self + 0x94);
    int r = ((FrameShow_t)(g_base + G->frameShow))(self, edx);
    if (!wasShown && *(u32*)(self + 0x94) && *(u32*)(self + 0x84) == 0) BarsFirst();
    return r;
}
typedef u32 (__fastcall *StatBarCtor_t)(u32 self, u32 edx, u32 a1, u32 a2, u32 a3);
static u32 __fastcall StatBarCtor_wrap(u32 self, u32 edx, u32 a1, u32 a2, u32 a3)
{
    u32 r = ((StatBarCtor_t)(g_base + G->statBarCtor))(self, edx, a1, a2, a3);
    if (*(u32*)(self + 0x94) && *(u32*)(self + 0x84) == 0) BarsFirst();
    static int n; if (n++ < 3) dlogf_("health bar created %08X (level %u, shown %u)", self, *(u32*)(self + 0x84), *(u32*)(self + 0x94));
    return r;
}
static void DumpSimpleLevels(u32 bar)
{
    u32 top = *(u32*)(g_base + G->simpleTop);
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
// ---- occupancy marks (COccupUI: a frame with a background texture, its COccupGrid child at +0x128, whose marks are
//      status bars, at +0x12C, their count at +0x134) under the console, as the health bars: all of them on the
//      top level (0) with their fills in the BACKGROUND layer, drawn before the console (BarsFirst)
static const u32 kOccupTexCall[3] = { 0x362AB7, 0x3CEE44, 0x402F44 };   // a mark's fill: CSimpleStatusBar SetTexture
static int IsOccupTree(u32 f)
{
    for (int i = 0; i < 4 && f && !IsBadReadPtr((void*)f, 0x70); i++) {
        if (VtRva(f) == kOccupVt[BuildIdx()]) return 1;
        f = *(u32*)(f + 0x6C);
    }
    return 0;
}
static int OccupFrames(u32 ui, u32* out, int max)
{
    int n = 0;
    out[n++] = ui;
    u32 grid = *(u32*)(ui + 0x128);
    if (!grid || IsBadReadPtr((void*)grid, 0x138)) return n;
    out[n++] = grid;
    u32 arr = *(u32*)(grid + 0x12C), cnt = *(u32*)(grid + 0x134);
    if (!arr || cnt > 64 || IsBadReadPtr((void*)arr, cnt * 4)) return n;
    for (u32 i = 0; i < cnt && n < max; i++) {
        u32 c = ((u32*)arr)[i];
        if (c && !IsBadReadPtr((void*)c, 0x98)) out[n++] = c;
    }
    return n;
}
static void OccupUnder(u32 ui)
{
    u32 f[70]; int n = OccupFrames(ui, f, 70), raised = 0;
    for (int i = 0; i < n; i++) if (*(u32*)(f[i] + 0x84) != 0) raised = 1;
    if (!raised) return;
    int shown = *(u32*)(ui + 0x94);
    // as the game's SetFrameLevel: hide (the children too), change the levels, show again (registered anew)
    if (shown) { *(u32*)(ui + 0x90) = 0; ((FrameShow_t)(*(u32**)ui)[0x64 / 4])(ui, 0); }
    for (int i = 0; i < n; i++) *(u32*)(f[i] + 0x84) = 0;
    if (shown) { *(u32*)(ui + 0x90) = 1; ((FrameShow_t)(*(u32**)ui)[0x68 / 4])(ui, 0); }
    static int k; if (k++ < 3) dlogf_("occupancy marks %08X: %d frames moved to the top level", ui, n);
}
static int __fastcall OccupShow_hook(u32 self, u32 edx)
{
    int wasShown = *(u32*)(self + 0x94);
    int r = ((FrameShow_t)(g_base + G->frameShow))(self, edx);
    if (!wasShown && *(u32*)(self + 0x94)) BarsFirst();
    return r;
}
static void InstallOccupUnder(void)
{
    u32 at = g_base + kOccupTexCall[BuildIdx()], slot = g_base + kOccupVt[BuildIdx()] + 0x68;
    if (*(uint8_t*)at != 0xE8 || at + 5 + *(int32_t*)(at + 1) != g_base + G->sbSetTex || *(u32*)slot != g_base + G->frameShow) {
        logf_("occupancy marks code not recognised"); return;
    }
    uint8_t c[5] = { 0xE8 }; int32_t rel = (int32_t)((u32)StatBarSetTexture_wrap - (at + 5)); memcpy(c + 1, &rel, 4);
    WriteMem(at, c, 5);
    u32 h = (u32)OccupShow_hook; WriteMem(slot, &h, 4);
    g_occupUnder = 1;
    logf_("occupancy marks drawn under the console");
}
static void InstallHealthBarLayer(void)
{
    if (!g_hpUnder || !g_worldFull) return;
    u32 at = g_base + G->sbSetTexCall;
    if (*(uint8_t*)at != 0xE8 || at + 5 + *(int32_t*)(at + 1) != g_base + G->sbSetTex ||
        memcmp((void*)(g_base + G->sbLayerPush), "\x6a\x01\x6a\x02\x6a\x00", 6)) { logf_("health bar code not recognised"); g_hpUnder = 0; return; }
    uint8_t c[5] = { 0xE8 }; int32_t rel = (int32_t)((u32)StatBarSetTexture_wrap - (at + 5)); memcpy(c + 1, &rel, 4);
    WriteMem(at, c, 5);
    // right after creating a unit bar the game raises its frame level by one (1.26: add eax,1 / 1.27: inc eax before
    // SetFrameLevel) so that it is drawn after the top-level console: keep it at the console's level instead
    if (CodeIs(&G->barRaise)) { WriteMem(g_base + G->barRaiseAt, &G->barRaiseOff, 1); g_barLevelPatched = 1; }
    else logf_("health bar level code not recognised");
    u32 slot = g_base + VT(StatBar) + 0x68;                     // CStatBar vtable: Show
    if (*(u32*)slot == g_base + G->frameShow && CodeIs(&G->topRegister)) {
        u32 h = (u32)StatBarShow_hook; WriteMem(slot, &h, 4);
        for (size_t i = 0; i < sizeof G->ctorCalls / sizeof G->ctorCalls[0]; i++) {
            u32 a = g_base + G->ctorCalls[i];
            if (*(uint8_t*)a != 0xE8 || a + 5 + *(int32_t*)(a + 1) != g_base + G->statBarCtor) { logf_("health bar call %06X not recognised", G->ctorCalls[i]); continue; }
            uint8_t cc[5] = { 0xE8 }; int32_t rr = (int32_t)((u32)StatBarCtor_wrap - (a + 5)); memcpy(cc + 1, &rr, 4);
            WriteMem(a, cc, 5);
        }
    } else logf_("health bar order code not recognised");
    logf_("unit health bars drawn under the console");
    if (*(u32*)slot == (u32)StatBarShow_hook) InstallOccupUnder();
}

static void InstallMovieNative(void)
{
    if (!g_movieNative) return;
    if (!CodeIs(&G->modeLoop) || !CodeIs(&G->modeRestore)) {
        logf_("movies: display mode code not recognised, native mode off"); return;
    }
    // the mode search runs after the current desktop mode has been stored as the movie size: skip the search
    uint8_t j[8] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((g_base + G->modeLoopTo) - (g_base + G->modeLoop.rva + 5)); memcpy(j + 1, &rel, 4);
    WriteMem(g_base + G->modeLoop.rva, j, G->modeLoop.len);
    static const uint8_t nop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    WriteMem(g_base + G->modeRestore.rva + 4, nop5, 5);   // ...and nothing to restore afterwards
    PatchCallGamma(g_base + G->gammaStart);              // movie start: x^(1/2.2) ramp
    PatchCallGamma(g_base + G->gammaEnd);                // movie end: restore of the ramp saved at the start
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
// play a movie: edx = file, ecx = window, stack arguments (1.26: 3, 1.27: 5), callee cleans = __fastcall
typedef int (__attribute__((fastcall)) *PlayMovie_t)(void*, const char*, int, int, int);
typedef int (__attribute__((fastcall)) *PlayMovie5_t)(void*, const char*, int, int, int, int, int);
static u32 orig_PlayMovie;
static int __attribute__((fastcall)) PlayMovie_hook(void* c, const char* file, int a, int b, int d)
{
    if (g_moviePlayer[0] && file && file[0] && PlayExternal(file)) return 0;
    int r = ((PlayMovie_t)orig_PlayMovie)(c, file, a, b, d);
    MovieEnded();
    return r;
}
static int __attribute__((fastcall)) PlayMovie5_hook(void* c, const char* file, int a, int b, int d, int e, int f)
{
    if (g_moviePlayer[0] && file && file[0] && PlayExternal(file)) return 0;
    int r = ((PlayMovie5_t)orig_PlayMovie)(c, file, a, b, d, e, f);
    MovieEnded();
    return r;
}
static int InstallPlayHook(void)
{
    if (orig_PlayMovie) return 1;
    u32 at = g_base + G->playCall;
    if (!G->playCall || *(uint8_t*)at != 0xe8 || at + 5 + *(int32_t*)(at + 1) != g_base + G->playFn) return 0;
    orig_PlayMovie = g_base + G->playFn;
    u32 hook = G->playArgs == 5 ? (u32)PlayMovie5_hook : (u32)PlayMovie_hook;
    uint8_t c[5] = { 0xe8 }; int32_t rel = (int32_t)(hook - (at + 5)); memcpy(c + 1, &rel, 4);
    WriteMem(at, c, 5);
    return 1;
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
    // call <play movie>  ->  call PlayMovie_hook
    if (!InstallPlayHook()) { logf_("movies: movie code not recognised, external player off"); g_moviePlayer[0] = 0; return; }
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
    if (!g_pathQuery || !G->protectCall) return;   // (1.27 does not protect the process)
    u32 at = g_base + G->protectCall;   // call <protect> in GameMain; usually done already (Miles loads the .mix later)
    if (*(uint8_t*)at == 0xe8 && at + 5 + *(int32_t*)(at + 1) == g_base + G->protectFn) {
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
u32 g_renderTr;
static LARGE_INTEGER g_lastFrame; static double g_qpcToMs, g_minFrameMs;
// GetTickCount itself only moves every 15.625 ms on current Windows, timeBeginPeriod or not, so give Game.dll a
// 1 ms version: the system counter plus the performance counter's progress since we started. Never behind the
// real counter, never going backwards; other modules keep calling the real one through their own import tables.
static u32 g_gtcSlot;   // Game.dll's import of GetTickCount
typedef DWORD (WINAPI *GetTickCount_t)(void);
static GetTickCount_t g_realGTC;
static DWORD g_tickBase, g_lastHires; static LARGE_INTEGER g_qpcBase;
static DWORD WINAPI GetTickCount_hires(void)
{
    // the tick gate (0x62D710) fires the game tick whenever this value changes: keep it on the real 15.625 ms
    // counter so the tick rate (and everything stepped per tick) stays exactly as in the original game
    if ((u32)__builtin_return_address(0) - g_base == G->tickGateRet) return g_realGTC();
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
    u32 slot = g_gtcSlot = FindImport("kernel32.dll", "GetTickCount");
    if (!slot) { logf_("fps limit: GetTickCount import not found"); return; }
    g_realGTC = *(GetTickCount_t*)slot;
    if (!g_realGTC) { logf_("fps limit: GetTickCount import not found"); return; }
    g_tickBase = g_realGTC(); QueryPerformanceCounter(&g_qpcBase); g_lastHires = g_tickBase;
    void* h = (void*)GetTickCount_hires; WriteMem(slot, &h, 4);
    logf_("fps limit: 1 ms GetTickCount for Game.dll (base %lu)", g_tickBase);
}
static void RemoveHiresTicks(void)
{
    if (!g_realGTC) return;
    void* r = (void*)g_realGTC; WriteMem(g_gtcSlot, &r, 4); g_realGTC = NULL;
}
__attribute__((used, cdecl)) void RenderGate(u32 self)
{
    // self = the frame object; bit 4 of +0x44 = "render pending", which the original dispatcher consumes.
    // The tick sets it 64 times a second; we take it over: raise it on our schedule, drop it when too soon.
    if (g_dotaMode) return;
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
static void __attribute__((naked)) RenderEventEcx_det(void)    // 1.27: thiscall, the frame object in ecx
{
    asm volatile(
        "pushal\n" "push %%ecx\n" "call _RenderGate\n" "add $4, %%esp\n" "popal\n"
        "jmp *_g_renderTr\n" ::: "memory");
}
static void InstallFpsLimit(void)
{
    if (!g_fpsLimit) return;
    u32 a = g_base + G->renderEvent.rva;
    if (!CodeIs(&G->renderEvent)) { logf_("fps limit: render event code not recognised, off"); return; }
    LARGE_INTEGER f; if (!QueryPerformanceFrequency(&f) || !f.QuadPart) { logf_("fps limit: no performance counter, off"); return; }
    g_qpcToMs = 1000.0 / (double)f.QuadPart;
    g_minFrameMs = 1000.0 / (double)g_fpsLimit;
    g_renderTr = MakeTrampoline(a, G->renderEvent.len);
    if (!g_renderTr) { logf_("fps limit: trampoline failed, off"); return; }
    WriteJmp(a, G->renderSelfEcx ? (u32)RenderEventEcx_det : (u32)RenderEvent_det);
    InstallHiresTicks();
    logf_("fps limit: %d fps (min frame %.3f ms)", g_fpsLimit, g_minFrameMs);
    // Fullscreen: the game sets the display mode at its 'refreshrate' setting (Video, in the registry; 60 or 75
    // unless changed) and waits for vsync, so that is the real cap there ('maxfps' does nothing). When it is below
    // FpsLimit, raise it to the monitor's highest rate at the game's resolution, through the game's own settings
    // (CGameSettings: get / set value / store in the registry), before the display is set up.
    HKEY k; DWORD rr = 0, rw = 0, rh = 0, sz = 4, ty = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Blizzard Entertainment\\Warcraft III\\Video", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "refreshrate", NULL, &ty, (BYTE*)&rr, &sz) != ERROR_SUCCESS || ty != REG_DWORD) rr = 0;
        sz = 4; RegQueryValueExA(k, "reswidth", NULL, NULL, (BYTE*)&rw, &sz);
        sz = 4; RegQueryValueExA(k, "resheight", NULL, NULL, (BYTE*)&rh, &sz);
        RegCloseKey(k);
    }
    DEVMODEA dm; DWORD best = 0, bestAny = 0;
    for (DWORD i = 0; ; i++) {
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(NULL, i, &dm)) break;
        if (dm.dmDisplayFrequency > bestAny) bestAny = dm.dmDisplayFrequency;
        if (dm.dmPelsWidth == rw && dm.dmPelsHeight == rh && dm.dmDisplayFrequency > best) best = dm.dmDisplayFrequency;
    }
    if (!best) best = bestAny;
    static const u32 kSettings[3][3] = { { 0x5720, 0x57F0, 0x5BB0 }, { 0x40ED0, 0x42B40, 0x40310 }, { 0x6BF60, 0x6DBC0, 0x6B3A0 } };   // get, set, store
    const u32* fn = kSettings[BuildIdx()];
    if (best > rr && (int)rr < g_fpsLimit && best < 1000) {
        typedef u32 (__cdecl *GetSettings_t)(void);
        typedef int (__thiscall *SetInt_t)(u32, int, int);
        typedef int (__thiscall *Store_t)(u32, int);
        u32 st = ((GetSettings_t)(g_base + fn[0]))();
        if (st) {
            ((SetInt_t)(g_base + fn[1]))(st, 4, (int)best);       // 4: refreshrate
            ((Store_t)(g_base + fn[2]))(st, 4);
            // and in the registry itself: the game may read its settings from there after this point (and write
            // them back on exit)
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Blizzard Entertainment\\Warcraft III\\Video", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                RegSetValueExA(k, "refreshrate", 0, REG_DWORD, (const BYTE*)&best, 4);
                RegCloseKey(k);
            }
            logf_("fps limit: refreshrate %lu -> %lu Hz (the monitor's highest at %lux%lu): fullscreen vsync no longer caps the frame rate below %d",
                  rr, best, rw, rh, g_fpsLimit);
        } else logf_("fps limit: game settings not available, refreshrate stays %lu", rr);
    } else if (rr) logf_("fps limit: refreshrate %lu (monitor's highest %lu)", rr, best);
}

static void Install(void)
{
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *(s + 1) = 0;
    strcat(path, "W3TrueWidescreen.log");
    g_log = fopen(path, "w");

    g_base = (u32)GetModuleHandleA("Game.dll");
    u32 build = GetGameBuild();
    logf_("W3TrueWidescreen 1.17  Game.dll build %u", build);
    for (size_t i = 0; i < sizeof kGames / sizeof kGames[0]; i++) if (kGames[i].build == build) G = &kGames[i];
    if (!g_base || !G) { logf_("unsupported game version, doing nothing (need 1.26a / 6401, 1.27b / 7085 or 1.28.5 / 7680)"); return; }
    logf_("Warcraft III %s", G->name);
    g_keepVt[0] = VT(SimpleTop); g_keepVt[1] = VT(Cursor);

    char src[128];
    double aspect = ReadAspect(src, sizeof src);
    logf_("aspect %.4f (%s)", aspect, src);
    InstallPathQuery();
    if (aspect < 1.34) {
        // nothing to widen, but the movie and frame rate options do not depend on the aspect
        logf_("aspect is 4:3 or narrower: interface and view left as they are");
        InstallMovieNative();
        InstallMovieRenderer();
        InstallExternalPlayer(g_moviePlayerOpt);
        InstallFpsLimit();
        return;
    }

    g_aspect = aspect;
    g_uiW_d = g_uiH_d * aspect;
    g_uiW_f = (float)g_uiW_d;
    g_e = (float)((g_uiW_d - 0.8) * 0.5);
    if (g_uiScale != 1.0f) logf_("UI scale %.0f%% in game: virtual screen %.4f x %.4f", g_uiScale * 100.0f, 0.6 / g_uiScale * aspect, 0.6 / g_uiScale);
    dlogf_("UI width %.4f, HUD shift %.4f", g_uiW_d, g_e);

    // 1) UI <-> screen conversions use our width (1.26: double operands, 1.27: float operands)
    for (size_t i = 0; i < sizeof G->widthSites / sizeof G->widthSites[0] && G->widthSites[i]; i++) {
        u32 cur = *(u32*)(g_base + G->widthSites[i]);
        if (cur != g_base + G->widthConst) { logf_("site %06X unexpected operand %08X, abort", G->widthSites[i], cur); return; }
    }
    if (G->orthoImm ? *(u32*)(g_base + G->orthoW) != G->orthoWConst : *(u32*)(g_base + G->orthoW) != g_base + G->orthoWConst) { logf_("ortho site mismatch, abort"); return; }
    if (!CodeIs(&G->setFramePoint)) { logf_("SetFramePoint prologue mismatch, abort"); return; }
    if (!CodeIs(&G->setAllPoints)) { logf_("SetAllPoints prologue mismatch, abort"); return; }

    u32 pw = G->sse ? (u32)&g_uiW_f : (u32)&g_uiW_d;
    for (size_t i = 0; i < sizeof G->widthSites / sizeof G->widthSites[0] && G->widthSites[i]; i++)
        WriteMem(g_base + G->widthSites[i], &pw, 4);
    if (g_uiScale != 1.0f) {
        int ok = G->orthoImm ? *(u32*)(g_base + G->orthoH) == G->orthoHConst : *(u32*)(g_base + G->orthoH) == g_base + G->orthoHConst;   // ortho top
        for (size_t i = 0; i < sizeof G->heightSites / sizeof G->heightSites[0] && G->heightSites[i]; i++)
            ok &= *(u32*)(g_base + G->heightSites[i]) == g_base + G->heightConst;
        if (ok) {
            u32 ph = G->sse ? (u32)&g_uiH_f : (u32)&g_uiH_d, pf = (u32)&g_uiH_f;
            for (size_t i = 0; i < sizeof G->heightSites / sizeof G->heightSites[0] && G->heightSites[i]; i++) WriteMem(g_base + G->heightSites[i], &ph, 4);
            if (!G->orthoImm) WriteMem(g_base + G->orthoH, &pf, 4);
        } else { logf_("UI scale: height sites not recognised, scale off"); g_uiScale = 1.0f; }
    }

    // 2) UI ortho projection: right edge = our width
    if (G->orthoImm) { g_orthoPatched = 1; SyncImmediates(); }
    else { u32 pf = (u32)&g_uiW_f; WriteMem(g_base + G->orthoW, &pf, 4); }
    // 2b) map-click gate: let clicks reach the world in the strip beside the top bar (WorldFullHeight)
    if (*(u32*)(g_base + G->pickTop) == g_base + G->pickTopConst) {
        g_pickTop = g_worldFull ? 1.0f : 0.577f;
        u32 pp = (u32)&g_pickTop;
        WriteMem(g_base + G->pickTop, &pp, 4);
        logf_("map-click gate: top %.3f", g_pickTop);
    } else logf_("map-click gate code not recognised, left as is");

    // 3) keep HUD frames in the centered 4:3 area
    ClearAllPoints = (ClearAllPoints_t)(g_base + G->clearAllPoints);
    orig_SetFramePoint = (SetFramePoint_t)MakeTrampoline(g_base + G->setFramePoint.rva, G->setFramePoint.len);
    if (!orig_SetFramePoint) { logf_("trampoline alloc failed"); return; }
    WriteJmp(g_base + G->setFramePoint.rva, (u32)SetFramePoint_hook);
    WriteJmp(g_base + G->setAllPoints.rva, (u32)SetAllPoints_hook);
    if (CodeIs(&G->setAbsPoint)) {
        orig_SetAbsPoint = (SetAbsPoint_t)MakeTrampoline(g_base + G->setAbsPoint.rva, G->setAbsPoint.len);
        if (orig_SetAbsPoint) WriteJmp(g_base + G->setAbsPoint.rva, (u32)SetAbsPoint_hook);
    } else logf_("SetAbsPoint code not recognised");

    if (CodeIs(&G->persp)) {
        orig_Persp = (Persp_t)MakeTrampoline(g_base + G->persp.rva, G->persp.len);
        if (orig_Persp) WriteJmp(g_base + G->persp.rva, (u32)Persp_hook);
        if (!orig_Persp) logf_("perspective hook failed");
    } else logf_("perspective code not recognised, field-of-view fix off");
    if (g_worldFull && CodeIs(&G->worldPick)) {
        orig_WorldPick = (WorldPick_t)MakeTrampoline(g_base + G->worldPick.rva, G->worldPick.len);
        if (orig_WorldPick) WriteJmp(g_base + G->worldPick.rva, (u32)WorldPick_hook);
    } else if (g_worldFull) logf_("world pick code not recognised, clicks on the console reach the world");
    if (CodeIs(&G->renderWorld)) {
        orig_RenderWorld = (RenderWorld_t)MakeTrampoline(g_base + G->renderWorld.rva, G->renderWorld.len);
        if (orig_RenderWorld) WriteJmp(g_base + G->renderWorld.rva, (u32)RenderWorld_hook);
    } else logf_("world render code not recognised, console backing off");
    InstallFadeFix();
    InstallTextShadow();
    InstallHealthBarLayer();
    InstallMovieNative();
    InstallMovieRenderer();
    InstallExternalPlayer(g_moviePlayerOpt);
    InstallFpsLimit();
    g_wideD = g_uiW_d; g_wideE = g_e; g_pickTopWide = g_pickTop;
    if (g_glue && g_menuEdges >= 1 && g_menuAspectOpt > 1.33 && g_aspect > g_menuAspectOpt + 0.01) {
        g_menuCut = (float)((g_aspect - g_menuAspectOpt) * 0.6 * 0.5);
        logf_("menus in a centred %.4f area (MenuAspect): edges %.4f in from the screen edges", g_menuAspectOpt, g_menuCut);
    }
    if (g_glue && g_menuEdges >= 2 && g_aspect > 1.34) g_menuWide = 1;
    g_enabled = 1;
    if (g_glue && !g_menuEdges && g_aspect > 1.34) { g_narrowWant = 1; SetGlueNarrow(1); }
    logf_("active");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_DETACH) RemoveHiresTicks();
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // the Miles sound library loads the .mix files and unloads / reloads them when the sound provider changes
        // (e.g. EAX switched on); the patches in Game.dll point into this module, so it must stay loaded
        { HMODULE self; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &self); }
        Install();
    }
    return TRUE;
}
