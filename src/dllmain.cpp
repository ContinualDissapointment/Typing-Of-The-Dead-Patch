#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include "config.h"
#include "ddraw_proxy.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Real ddraw.dll handle + function pointers
// ---------------------------------------------------------------------------

static HMODULE  g_realDDraw  = nullptr;
static Config   g_cfg        = {};
static HMODULE  g_hSelf      = nullptr;

typedef HRESULT (WINAPI* PFN_DirectDrawCreateEx)(GUID*, LPVOID*, REFIID, IUnknown*);
typedef HRESULT (WINAPI* PFN_DirectDrawCreate)(GUID*, LPDIRECTDRAW*, IUnknown*);
typedef HRESULT (WINAPI* PFN_DirectDrawEnumerateExA)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
typedef HRESULT (WINAPI* PFN_DirectDrawEnumerateExW)(LPDDENUMCALLBACKEXW, LPVOID, DWORD);
typedef HRESULT (WINAPI* PFN_DirectDrawEnumerateA)(LPDDENUMCALLBACKA, LPVOID);
typedef HRESULT (WINAPI* PFN_DirectDrawEnumerateW)(LPDDENUMCALLBACKW, LPVOID);
typedef HRESULT (WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI* PFN_DllCanUnloadNow)();

static PFN_DirectDrawCreateEx      pfn_DirectDrawCreateEx      = nullptr;
static PFN_DirectDrawCreate        pfn_DirectDrawCreate        = nullptr;
static PFN_DirectDrawEnumerateExA  pfn_DirectDrawEnumerateExA  = nullptr;
static PFN_DirectDrawEnumerateExW  pfn_DirectDrawEnumerateExW  = nullptr;
static PFN_DirectDrawEnumerateA    pfn_DirectDrawEnumerateA    = nullptr;
static PFN_DirectDrawEnumerateW    pfn_DirectDrawEnumerateW    = nullptr;
static PFN_DllGetClassObject       pfn_DllGetClassObject       = nullptr;
static PFN_DllCanUnloadNow         pfn_DllCanUnloadNow         = nullptr;

// All remaining exports from the real ddraw.dll — forwarded via trampolines.
// D3DIM700.DLL imports: AcquireDDThreadLock, CompleteCreateSysmemSurface,
//   D3DParseUnknownCommand, DDInternalLock, DDInternalUnlock, ReleaseDDThreadLock.
// Other DLLs / the game may call the rest.
static void* pfn_AcquireDDThreadLock         = nullptr;
static void* pfn_ReleaseDDThreadLock         = nullptr;
static void* pfn_DDInternalLock              = nullptr;
static void* pfn_DDInternalUnlock            = nullptr;
static void* pfn_CompleteCreateSysmemSurface = nullptr;
static void* pfn_DBreakVBLock               = nullptr;
static void* pfn_D3DParseUnknownCommand      = nullptr;
static void* pfn_DDGetAttachedSurfaceLcl     = nullptr;
static void* pfn_DSoundHelp                  = nullptr;
static void* pfn_DirectDrawCreateClipper     = nullptr;
static void* pfn_GetDDSurfaceLocal           = nullptr;
static void* pfn_GetOLEThunkData             = nullptr;
static void* pfn_GetSurfaceFromDC            = nullptr;
static void* pfn_RegisterSpecialCase         = nullptr;
static void* pfn_SetAppCompatData            = nullptr;

static bool LoadRealDDraw()
{
    if (g_realDDraw) return true;

    // Prefer dgvoodoo_ddraw.dll in the game folder.
    // On Windows 10/11, the system ddraw.dll no longer provides a D3D7 HAL device.
    // dgVoodoo2's DDraw.dll re-implements D3D7 on top of D3D11 and restores HAL.
    // Rename dgVoodoo2's DDraw.dll to "dgvoodoo_ddraw.dll" in the game folder.
    char localPath[MAX_PATH];
    GetModuleFileNameA(g_hSelf, localPath, sizeof(localPath));
    char* lastSlash = strrchr(localPath, '\\');
    if (lastSlash)
    {
        strcpy_s(lastSlash + 1,
                 MAX_PATH - (DWORD)(lastSlash - localPath + 1),
                 "dgvoodoo_ddraw.dll");
        g_realDDraw = LoadLibraryA(localPath);
        if (g_realDDraw)
            Log("Using dgvoodoo_ddraw.dll from game folder");
    }

    // Fall back to the system ddraw.dll (works on Windows 7 and earlier)
    if (!g_realDDraw)
    {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, sizeof(sysPath));
        strncat_s(sysPath, "\\ddraw.dll", _TRUNCATE);
        g_realDDraw = LoadLibraryA(sysPath);
        if (g_realDDraw)
            Log("Using system ddraw.dll");
    }

    if (!g_realDDraw) return false;

#define GETPROC(name) pfn_##name = (PFN_##name)GetProcAddress(g_realDDraw, #name)
    GETPROC(DirectDrawCreateEx);
    GETPROC(DirectDrawCreate);
    GETPROC(DirectDrawEnumerateExA);
    GETPROC(DirectDrawEnumerateExW);
    GETPROC(DirectDrawEnumerateA);
    GETPROC(DirectDrawEnumerateW);
    GETPROC(DllGetClassObject);
    GETPROC(DllCanUnloadNow);
#undef GETPROC

#define GETRAW(name) pfn_##name = (void*)GetProcAddress(g_realDDraw, #name)
    GETRAW(AcquireDDThreadLock);
    GETRAW(ReleaseDDThreadLock);
    GETRAW(DDInternalLock);
    GETRAW(DDInternalUnlock);
    GETRAW(CompleteCreateSysmemSurface);
    GETRAW(DBreakVBLock);
    GETRAW(D3DParseUnknownCommand);
    GETRAW(DDGetAttachedSurfaceLcl);
    GETRAW(DSoundHelp);
    GETRAW(DirectDrawCreateClipper);
    GETRAW(GetDDSurfaceLocal);
    GETRAW(GetOLEThunkData);
    GETRAW(GetSurfaceFromDC);
    GETRAW(RegisterSpecialCase);
    GETRAW(SetAppCompatData);
#undef GETRAW

    return true;
}

// ---------------------------------------------------------------------------
// dgVoodoo.conf generator
// ---------------------------------------------------------------------------

static void WriteDgVoodooConf(HMODULE hModule, const Config& cfg)
{
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, sizeof(path));
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (DWORD)(slash - path + 1), "dgVoodoo.conf");

    const bool  isFull = (cfg.mode == MODE_FULLSCREEN);
    const bool  isBL   = (cfg.mode == MODE_BORDERLESS);
    const bool  isWin  = (cfg.mode == MODE_WINDOWED);

    const char* scalingMode = (cfg.stretchToFit || !cfg.keepAspect)
                              ? "stretched" : "centered_ar";

    // AppControlledScreenMode=false always: dgVoodoo2 controls the screen mode
    // based on its own FullScreenMode, ignoring the app's DDSCL flags.
    //
    // Fullscreen: FullScreenMode=true  — dgVoodoo2 takes real exclusive fullscreen.
    // Borderless: FullScreenMode=false — dgVoodoo2 stays windowed; our proxy resizes
    //   the game window to fill the screen (WS_POPUP + SM_CXSCREEN x SM_CYSCREEN)
    //   before dgVoodoo2 creates its swapchain.
    // Windowed:   FullScreenMode=false — dgVoodoo2 stays windowed; our proxy sets
    //   the window to cfg.width x cfg.height, centered.
    // Matches the dgVoodoo2 CPL "windowed borderless" configuration exactly:
    //   AppControlledScreenMode=false: dgVoodoo2 ignores app DDSCL flags
    //   FullScreenMode=false: dgVoodoo2 stays windowed
    //   FullscreenAttributes=fake: intercepts app fullscreen requests as borderless
    // Windowed: same but no FullscreenAttributes.
    // Fullscreen: AppControlledScreenMode=true + FullScreenMode=true.
    const char* fullScreenMode    = isFull ? "true" : "false";
    const char* appCtrlMode       = isFull ? "true" : "false";
    const char* fullscreenAttribs = "";
    const char* windowedAttribs   = "";

    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) return;

    fprintf(f,
        "Version = 0x287\n"
        "\n"
        "[General]\n"
        "OutputAPI              = bestavailable\n"
        "Adapters               = 1\n"
        "FullScreenOutput       = default\n"
        "FullScreenMode         = %s\n"
        "ScalingMode            = %s\n"
        "CaptureMouse           = false\n"
        "CenterAppWindow        = false\n"
        "\n"
        "[GeneralExt]\n"
        "WindowedAttributes     = %s\n"
        "FullscreenAttributes   = %s\n"
        "Resampling             = bilinear\n"
        "\n"
        "[DirectX]\n"
        "AppControlledScreenMode           = %s\n"
        "DisableAltEnterToToggleScreenMode = true\n"
        "dgVoodooWatermark                 = false\n"
        "DisableAndPassThru                = false\n"
        "Resolution                        = %s\n",
        fullScreenMode,      // FullScreenMode
        scalingMode,         // ScalingMode
        windowedAttribs,     // WindowedAttributes
        fullscreenAttribs,   // FullscreenAttributes
        appCtrlMode,         // AppControlledScreenMode
        "unforced"  // Resolution: let dgVoodoo2 use window size
    );

    fclose(f);
    Log("dgVoodoo.conf written (mode=%d FullScreenMode=%s ScalingMode=%s FsAttribs=%s)",
        (int)cfg.mode, fullScreenMode, scalingMode, fullscreenAttribs);
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hInst;
        DisableThreadLibraryCalls(hInst);
        Log_Init(hInst);
        Config_Load(g_cfg, hInst);
        Log("DLL loaded. mode=%d width=%d height=%d stretch=%d aspect=%d",
            (int)g_cfg.mode, g_cfg.width, g_cfg.height,
            (int)g_cfg.stretchToFit, (int)g_cfg.keepAspect);
        WriteDgVoodooConf(hInst, g_cfg);
        LoadRealDDraw();
        Log("Real ddraw loaded: %s", g_realDDraw ? "OK" : "FAILED");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_realDDraw) { FreeLibrary(g_realDDraw); g_realDDraw = nullptr; }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Exported functions
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI DirectDrawCreateEx(
    GUID*   lpGUID,
    LPVOID* lplpDD,
    REFIID  iid,
    IUnknown* pUnkOuter)
{
    if (!pfn_DirectDrawCreateEx) return DDERR_GENERIC;

    // Only intercept IDirectDraw7 — let anything else pass straight through
    if (!IsEqualIID(iid, IID_IDirectDraw7))
    {
        Log("DirectDrawCreateEx: non-IDD7 iid, passing through");
        return pfn_DirectDrawCreateEx(lpGUID, lplpDD, iid, pUnkOuter);
    }

    IDirectDraw7* real = nullptr;
    HRESULT hr = pfn_DirectDrawCreateEx(lpGUID, (LPVOID*)&real, iid, pUnkOuter);
    Log("DirectDrawCreateEx guid=%s hr=0x%08X real=%p",
        lpGUID ? "adapter" : "default", (unsigned)hr, (void*)real);
    if (FAILED(hr)) return hr;

    ProxyDDraw7* proxy = new ProxyDDraw7(real, g_cfg);
    Log("  -> ProxyDDraw7=%p", (void*)proxy);
    *lplpDD = (LPVOID)proxy;
    return DD_OK;
}

extern "C" HRESULT WINAPI DirectDrawCreate(
    GUID*        lpGUID,
    LPDIRECTDRAW* lplpDD,
    IUnknown*     pUnkOuter)
{
    if (!pfn_DirectDrawCreate) return DDERR_GENERIC;
    return pfn_DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExA(
    LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags)
{
    if (!pfn_DirectDrawEnumerateExA) return DDERR_GENERIC;
    return pfn_DirectDrawEnumerateExA(lpCallback, lpContext, dwFlags);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExW(
    LPDDENUMCALLBACKEXW lpCallback, LPVOID lpContext, DWORD dwFlags)
{
    if (!pfn_DirectDrawEnumerateExW) return DDERR_GENERIC;
    return pfn_DirectDrawEnumerateExW(lpCallback, lpContext, dwFlags);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateA(
    LPDDENUMCALLBACKA lpCallback, LPVOID lpContext)
{
    if (!pfn_DirectDrawEnumerateA) return DDERR_GENERIC;
    return pfn_DirectDrawEnumerateA(lpCallback, lpContext);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateW(
    LPDDENUMCALLBACKW lpCallback, LPVOID lpContext)
{
    if (!pfn_DirectDrawEnumerateW) return DDERR_GENERIC;
    return pfn_DirectDrawEnumerateW(lpCallback, lpContext);
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!pfn_DllGetClassObject) return E_FAIL;
    return pfn_DllGetClassObject(rclsid, riid, ppv);
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    if (!pfn_DllCanUnloadNow) return S_FALSE;
    return pfn_DllCanUnloadNow();
}

// ---------------------------------------------------------------------------
// Undocumented exports — naked trampolines to the real ddraw.dll.
// D3DIM700.DLL imports both of these by name.  Without them the game crashes
// at startup with "entry point not found in ddraw.dll".
// ---------------------------------------------------------------------------

extern "C" __declspec(naked) void WINAPI AcquireDDThreadLock()
{
    __asm jmp [pfn_AcquireDDThreadLock]
}

extern "C" __declspec(naked) void WINAPI ReleaseDDThreadLock()
{
    __asm jmp [pfn_ReleaseDDThreadLock]
}

extern "C" __declspec(naked) void WINAPI DDInternalLock()
{
    __asm jmp [pfn_DDInternalLock]
}

extern "C" __declspec(naked) void WINAPI DDInternalUnlock()
{
    __asm jmp [pfn_DDInternalUnlock]
}

extern "C" __declspec(naked) void WINAPI CompleteCreateSysmemSurface()
{
    __asm jmp [pfn_CompleteCreateSysmemSurface]
}

extern "C" __declspec(naked) void WINAPI DBreakVBLock()
{
    __asm jmp [pfn_DBreakVBLock]
}

extern "C" __declspec(naked) void WINAPI D3DParseUnknownCommand()
{
    __asm jmp [pfn_D3DParseUnknownCommand]
}

extern "C" __declspec(naked) void WINAPI DDGetAttachedSurfaceLcl()
{
    __asm jmp [pfn_DDGetAttachedSurfaceLcl]
}

extern "C" __declspec(naked) void WINAPI DSoundHelp()
{
    __asm jmp [pfn_DSoundHelp]
}

// DirectDrawCreateClipper conflicts with ddraw.h declaration; forward via regular wrapper
extern "C" HRESULT WINAPI DirectDrawCreateClipper(DWORD dwFlags, LPDIRECTDRAWCLIPPER* lplpDDClipper, IUnknown* pUnkOuter)
{
    typedef HRESULT (WINAPI* PFN)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*);
    if (!pfn_DirectDrawCreateClipper) return DDERR_GENERIC;
    return ((PFN)pfn_DirectDrawCreateClipper)(dwFlags, lplpDDClipper, pUnkOuter);
}

extern "C" __declspec(naked) void WINAPI GetDDSurfaceLocal()
{
    __asm jmp [pfn_GetDDSurfaceLocal]
}

extern "C" __declspec(naked) void WINAPI GetOLEThunkData()
{
    __asm jmp [pfn_GetOLEThunkData]
}

extern "C" __declspec(naked) void WINAPI GetSurfaceFromDC()
{
    __asm jmp [pfn_GetSurfaceFromDC]
}

extern "C" __declspec(naked) void WINAPI RegisterSpecialCase()
{
    __asm jmp [pfn_RegisterSpecialCase]
}

extern "C" __declspec(naked) void WINAPI SetAppCompatData()
{
    __asm jmp [pfn_SetAppCompatData]
}
