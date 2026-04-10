#include "ddraw_proxy.h"
#include "surface_proxy.h"
#include "log.h"
#include <cstdio>
#include <d3d.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <imagehlp.h>
#pragma comment(lib, "imagehlp.lib")

// ---------------------------------------------------------------------------
// Window subclass — focus, pause/mute, and monitor-move hotkeys
// ---------------------------------------------------------------------------

static WNDPROC    g_origWndProc      = nullptr;
static bool       g_pauseOnFocusLoss = false;
static bool       g_muteOnFocusLoss  = false;
static WindowMode g_windowMode       = MODE_BORDERLESS;
extern "C" volatile bool g_paused    = false;
static volatile bool     g_focused   = true;

static HWND     g_gameHwnd     = nullptr;
static HHOOK    g_kbHook       = nullptr;
static DWORD    g_hookThreadId = 0;
static bool     g_injecting    = false; // true only while we're calling SendInput
static KeyRemap g_remaps[32];
static int      g_remapCount   = 0;

static DWORD FindRemap(DWORD vk)
{
    for (int i = 0; i < g_remapCount; ++i)
        if (g_remaps[i].src == vk) return g_remaps[i].dst;
    return 0;
}

// ---------------------------------------------------------------------------
// Monitor-walk helper for Win+Shift+Arrow
// ---------------------------------------------------------------------------

struct MonitorSearchCtx {
    RECT     current;
    int      dir;       // 0=left 1=right 2=up 3=down
    HMONITOR best;
    RECT     bestRect;
    int      bestDist;
};

static BOOL CALLBACK FindAdjacentMonitorCb(HMONITOR hMon, HDC, LPRECT pRect, LPARAM lp)
{
    MonitorSearchCtx* c = (MonitorSearchCtx*)lp;
    if (EqualRect(pRect, &c->current)) return TRUE;
    int dist = INT_MAX;
    switch (c->dir)
    {
    case 0: if (pRect->right  <= c->current.left)  dist = c->current.left  - pRect->right;  break;
    case 1: if (pRect->left   >= c->current.right) dist = pRect->left   - c->current.right; break;
    case 2: if (pRect->bottom <= c->current.top)   dist = c->current.top   - pRect->bottom; break;
    case 3: if (pRect->top    >= c->current.bottom)dist = pRect->top    - c->current.bottom;break;
    }
    if (dist < c->bestDist) { c->bestDist = dist; c->best = hMon; c->bestRect = *pRect; }
    return TRUE;
}

static void MoveWindowToAdjacentMonitor(HWND hwnd, int dir)
{
    HMONITOR hCur = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hCur, &mi);

    MonitorSearchCtx ctx = {};
    ctx.current  = mi.rcMonitor;
    ctx.dir      = dir;
    ctx.best     = nullptr;
    ctx.bestDist = INT_MAX;
    EnumDisplayMonitors(nullptr, nullptr, FindAdjacentMonitorCb, (LPARAM)&ctx);

    if (!ctx.best) return;

    MONITORINFO miDst = { sizeof(miDst) };
    GetMonitorInfo(ctx.best, &miDst);

    if (g_windowMode == MODE_BORDERLESS)
    {
        // Fill the destination monitor completely
        SetWindowPos(hwnd, nullptr,
            miDst.rcMonitor.left, miDst.rcMonitor.top,
            miDst.rcMonitor.right  - miDst.rcMonitor.left,
            miDst.rcMonitor.bottom - miDst.rcMonitor.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        // Keep window size, center on destination monitor work area
        RECT wr; GetWindowRect(hwnd, &wr);
        int w = wr.right  - wr.left;
        int h = wr.bottom - wr.top;
        RECT& wa = miDst.rcWork;
        int x = wa.left + ((wa.right  - wa.left) - w) / 2;
        int y = wa.top  + ((wa.bottom - wa.top)  - h) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }
    Log("MoveWindowToAdjacentMonitor dir=%d -> (%d,%d)", dir,
        miDst.rcMonitor.left, miDst.rcMonitor.top);
}

// ---------------------------------------------------------------------------
// Audio mute helper — mutes/unmutes this process's WASAPI audio session(s)
// ---------------------------------------------------------------------------

static void SetProcessAudioMute(bool mute)
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
        return;

    IMMDevice* pDevice = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)))
        { pEnum->Release(); return; }

    IAudioSessionManager2* pMgr = nullptr;
    if (FAILED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                  nullptr, (void**)&pMgr)))
        { pDevice->Release(); pEnum->Release(); return; }

    IAudioSessionEnumerator* pSessEnum = nullptr;
    if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessEnum)))
    {
        int count = 0;
        pSessEnum->GetCount(&count);
        DWORD myPid = GetCurrentProcessId();

        for (int i = 0; i < count; ++i)
        {
            IAudioSessionControl* pCtrl = nullptr;
            if (FAILED(pSessEnum->GetSession(i, &pCtrl))) continue;

            IAudioSessionControl2* pCtrl2 = nullptr;
            if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                 (void**)&pCtrl2)))
            {
                DWORD pid = 0;
                pCtrl2->GetProcessId(&pid);
                if (pid == myPid)
                {
                    ISimpleAudioVolume* pVol = nullptr;
                    if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                         (void**)&pVol)))
                    {
                        pVol->SetMute(mute ? TRUE : FALSE, nullptr);
                        pVol->Release();
                    }
                }
                pCtrl2->Release();
            }
            pCtrl->Release();
        }
        pSessEnum->Release();
    }

    pMgr->Release();
    pDevice->Release();
    pEnum->Release();
    Log("SetProcessAudioMute(%s)", mute ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Low-level keyboard hook — intercepts Win+Shift+Arrow before the system
// ---------------------------------------------------------------------------

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

    if (nCode != HC_ACTION || !g_gameHwnd)
        return CallNextHookEx(g_kbHook, nCode, wParam, lParam);

    // Skip only keys that WE injected — not all injected keys.
    // (dgVoodoo2 re-injects all input via SendInput, so LLKHF_INJECTED
    //  is set on every keystroke the game sees.)
    if (g_injecting)
        return CallNextHookEx(g_kbHook, nCode, wParam, lParam);

    // --- Key remapping --------------------------------------------------
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
        wParam == WM_KEYUP   || wParam == WM_SYSKEYUP)
    {
        DWORD dst = FindRemap(kb->vkCode);
        if (dst)
        {
            Log("Hook: remap 0x%02X -> 0x%02X", kb->vkCode, dst);
            INPUT inp = {};
            inp.type       = INPUT_KEYBOARD;
            inp.ki.wVk     = (WORD)dst;
            inp.ki.dwFlags = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                             ? KEYEVENTF_KEYUP : 0;
            g_injecting = true;
            SendInput(1, &inp, sizeof(INPUT));
            g_injecting = false;
            return 1;
        }
    }

    // --- Monitor switching (Ctrl+Shift+Arrow) ---------------------------
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
    {
        bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shiftDown = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;

        if (ctrlDown && shiftDown)
        {
            int dir = -1;
            if (kb->vkCode == VK_LEFT)  dir = 0;
            if (kb->vkCode == VK_RIGHT) dir = 1;
            if (kb->vkCode == VK_UP)    dir = 2;
            if (kb->vkCode == VK_DOWN)  dir = 3;

            if (dir >= 0)
            {
                Log("Hook: Ctrl+Shift+Arrow dir=%d -> posting WM_APP+1", dir);
                PostMessageA(g_gameHwnd, WM_APP + 1, (WPARAM)dir, 0);
                return 1;
            }
        }
    }

    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// Dedicated thread: installs the LL hook then sits in GetMessage.
// WH_KEYBOARD_LL requires its installing thread to have a live message loop —
// if the game thread stalls, the hook times out and stops firing.
static DWORD WINAPI HookThread(LPVOID)
{
    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    Log("HookThread: kbhook=%s", g_kbHook ? "ok" : "FAILED");
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    return 0;
}

static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATEAPP)
    {
        g_focused = (wParam != 0);
        Log("WM_ACTIVATEAPP(%s)", wParam ? "true" : "false");
        if (!wParam)
        {
            if (g_pauseOnFocusLoss) g_paused = true;
            if (g_muteOnFocusLoss)  SetProcessAudioMute(true);
            // Borderless only: eat the message to prevent DirectDraw minimizing the window
            if (g_windowMode == MODE_BORDERLESS) return 0;
        }
        else
        {
            g_paused = false;
            if (g_muteOnFocusLoss) SetProcessAudioMute(false);
        }
    }

    if (msg == WM_APP + 1)
    {
        Log("GameWndProc: WM_APP+1 dir=%d", (int)wParam);
        MoveWindowToAdjacentMonitor(hwnd, (int)wParam);
        return 0;
    }

    if (msg == WM_DESTROY)
    {
        Log("GameWndProc: WM_DESTROY — killing hook thread");
        if (g_hookThreadId) { PostThreadMessageA(g_hookThreadId, WM_QUIT, 0, 0); g_hookThreadId = 0; }
        g_gameHwnd = nullptr;
    }

    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// IAT patching — redirect GetAsyncKeyState / GetKeyState in the game exe
// ---------------------------------------------------------------------------

typedef SHORT (WINAPI* PFN_GetAsyncKeyState)(int);
typedef SHORT (WINAPI* PFN_GetKeyState)(int);

static PFN_GetAsyncKeyState g_realGetAsyncKeyState = nullptr;
static PFN_GetKeyState      g_realGetKeyState      = nullptr;

static SHORT WINAPI Hook_GetAsyncKeyState(int vKey)
{
    DWORD dst = FindRemap((DWORD)vKey);
    if (dst) vKey = (int)dst;
    return g_realGetAsyncKeyState(vKey);
}

static SHORT WINAPI Hook_GetKeyState(int vKey)
{
    DWORD dst = FindRemap((DWORD)vKey);
    if (dst) vKey = (int)dst;
    return g_realGetKeyState(vKey);
}

// Patch one IAT entry in hMod's import table.
// Finds the first thunk for dllName!procName and overwrites it with newFn.
// If oldFn is non-null, stores the original pointer there.
// Returns true on success.
static bool PatchIATEntry(HMODULE hMod, const char* dllName, const char* procName,
                          void* newFn, void** oldFn)
{
    ULONG size = 0;
    PIMAGE_IMPORT_DESCRIPTOR pImport = (PIMAGE_IMPORT_DESCRIPTOR)
        ImageDirectoryEntryToData(hMod, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);
    if (!pImport) return false;

    BYTE* base = (BYTE*)hMod;

    for (; pImport->Name; ++pImport)
    {
        const char* name = (const char*)(base + pImport->Name);
        if (_stricmp(name, dllName) != 0) continue;

        PIMAGE_THUNK_DATA pOrig = (PIMAGE_THUNK_DATA)(base + pImport->OriginalFirstThunk);
        PIMAGE_THUNK_DATA pThunk= (PIMAGE_THUNK_DATA)(base + pImport->FirstThunk);

        for (; pOrig->u1.AddressOfData; ++pOrig, ++pThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(pOrig->u1.Ordinal)) continue;
            PIMAGE_IMPORT_BY_NAME pByName =
                (PIMAGE_IMPORT_BY_NAME)(base + pOrig->u1.AddressOfData);
            if (_stricmp((const char*)pByName->Name, procName) != 0) continue;

            // Found it — unprotect the page and overwrite
            DWORD oldProt = 0;
            VirtualProtect(&pThunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            if (oldFn) *oldFn = (void*)pThunk->u1.Function;
            pThunk->u1.Function = (ULONG_PTR)newFn;
            VirtualProtect(&pThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            Log("PatchIAT: %s!%s -> %p (was %p)", dllName, procName, newFn,
                oldFn ? *oldFn : nullptr);
            return true;
        }
    }
    return false;
}

void ApplyInputRemaps(const Config& cfg)
{
    if (cfg.remapCount == 0) return;

    // Populate globals so Hook_* can use FindRemap
    g_remapCount = cfg.remapCount;
    for (int i = 0; i < cfg.remapCount; ++i) g_remaps[i] = cfg.remaps[i];

    HMODULE hExe = GetModuleHandleA(nullptr);

    bool ok1 = PatchIATEntry(hExe, "user32.dll", "GetAsyncKeyState",
                             (void*)Hook_GetAsyncKeyState, (void**)&g_realGetAsyncKeyState);
    bool ok2 = PatchIATEntry(hExe, "user32.dll", "GetKeyState",
                             (void*)Hook_GetKeyState,      (void**)&g_realGetKeyState);

    // Fallback: if the game imports from a differently-cased DLL name
    if (!ok1) ok1 = PatchIATEntry(hExe, "USER32.DLL", "GetAsyncKeyState",
                                  (void*)Hook_GetAsyncKeyState, (void**)&g_realGetAsyncKeyState);
    if (!ok2) ok2 = PatchIATEntry(hExe, "USER32.DLL", "GetKeyState",
                                  (void*)Hook_GetKeyState,      (void**)&g_realGetKeyState);

    Log("ApplyInputRemaps: remaps=%d GetAsyncKeyState=%s GetKeyState=%s",
        cfg.remapCount, ok1 ? "patched" : "not found", ok2 ? "patched" : "not found");
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProxyDDraw7::ProxyDDraw7(IDirectDraw7* real, const Config& cfg)
    : m_real(real), m_cfg(cfg), m_hwnd(nullptr),
      m_refCount(1), m_windowStyleApplied(false), m_initialized(false), m_srcW(640), m_srcH(480)
{}

ProxyDDraw7::~ProxyDDraw7()
{
    Log("~ProxyDDraw7 %p (m_real=%p)", (void*)this, (void*)m_real);
    if (m_real) { m_real->Release(); m_real = nullptr; }
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxyDDraw7::QueryInterface(REFIID riid, void** ppv)
{
    HRESULT hr = m_real->QueryInterface(riid, ppv);
    // Log IIDs we care about (IDirect3D7 = {f5049e77-...})
    if (IsEqualIID(riid, IID_IDirect3D7))
        Log("QI(IDirect3D7) -> hr=0x%08X ptr=%p", (unsigned)hr, ppv ? *ppv : nullptr);
    else if (IsEqualIID(riid, IID_IDirectDraw7))
        Log("QI(IDirectDraw7) -> hr=0x%08X ptr=%p", (unsigned)hr, ppv ? *ppv : nullptr);
    return hr;
}
ULONG STDMETHODCALLTYPE ProxyDDraw7::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}
ULONG STDMETHODCALLTYPE ProxyDDraw7::Release()
{
    LONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) delete this;
    return (ULONG)ref;
}

// ---------------------------------------------------------------------------
// Window style helper
// ---------------------------------------------------------------------------

void ProxyDDraw7::ApplyWindowStyle()
{
    if (!m_hwnd) return;

    LONG style, exStyle;

    if (m_cfg.mode == MODE_BORDERLESS)
    {
        // Borderless windowed: no caption, no border
        style   = WS_POPUP | WS_VISIBLE;
        exStyle = WS_EX_APPWINDOW;
    }
    else if (m_cfg.mode == MODE_WINDOWED)
    {
        // Normal bordered window, resizable
        style   = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        exStyle = WS_EX_APPWINDOW;
    }
    else
    {
        // Fullscreen -- let the real cooperative level handle the window
        return;
    }

    SetWindowLongA(m_hwnd, GWL_STYLE,   style);
    SetWindowLongA(m_hwnd, GWL_EXSTYLE, exStyle);

    int x = 0, y = 0;
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    if (m_cfg.mode == MODE_BORDERLESS)
    {
        // Use cfg.width x cfg.height (e.g. 1920x1080) centered on screen.
        // A WS_POPUP smaller than the native display won't trigger DXGI's
        // automatic exclusive-fullscreen promotion heuristic.
        w = m_cfg.width;
        h = m_cfg.height;
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        x = (sw - w) / 2;
        y = (sh - h) / 2;
    }
    else if (m_cfg.mode == MODE_WINDOWED)
    {
        // Adjust so client area == requested resolution
        RECT rc = { 0, 0, m_cfg.width, m_cfg.height };
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        w = rc.right  - rc.left;
        h = rc.bottom - rc.top;

        // Center on the primary monitor
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        x = (sw - w) / 2;
        y = (sh - h) / 2;
    }

    SetWindowPos(m_hwnd, HWND_TOP, x, y, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOZORDER);
    ShowCursor(TRUE);
}

// ---------------------------------------------------------------------------
// SetCooperativeLevel
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxyDDraw7::SetCooperativeLevel(HWND hwnd, DWORD flags)
{
    m_hwnd = hwnd;
    Log("SetCooperativeLevel hwnd=%p flags=0x%08X", (void*)hwnd, flags);

    // dgVoodoo2 needs a valid HWND to create its D3D11 device.
    // During D3DX enumeration hwnd is NULL — substitute the desktop window.
    HWND hwndCoop = hwnd ? hwnd : GetDesktopWindow();

    HRESULT hr = m_real->SetCooperativeLevel(hwndCoop, flags);
    Log("  flags=0x%08X -> 0x%08X", flags, (unsigned)hr);

    if (SUCCEEDED(hr) && hwnd && m_cfg.mode != MODE_FULLSCREEN)
    {
        if (!g_origWndProc)
        {
            g_pauseOnFocusLoss = m_cfg.pauseOnFocusLoss;
            g_muteOnFocusLoss  = m_cfg.muteOnFocusLoss;
            g_windowMode       = m_cfg.mode;
            g_gameHwnd    = hwnd;
            g_remapCount  = m_cfg.remapCount;
            for (int i = 0; i < m_cfg.remapCount; ++i) g_remaps[i] = m_cfg.remaps[i];
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                                        (LONG_PTR)GameWndProc);
            HANDLE hThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
            if (hThread) CloseHandle(hThread);
            Log("  subclassed HWND mode=%d (pause=%d mute=%d remaps=%d hookThread=%u)",
                (int)m_cfg.mode, (int)g_pauseOnFocusLoss, (int)g_muteOnFocusLoss,
                g_remapCount, g_hookThreadId);
        }
        ApplyWindowStyle();
    }

    ShowCursor(TRUE);
    return hr;
}

// ---------------------------------------------------------------------------
// SetDisplayMode
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxyDDraw7::SetDisplayMode(
    DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    m_srcW = (int)w;
    m_srcH = (int)h;
    Log("SetDisplayMode %dx%dx%d refresh=%d", w, h, bpp, refresh);
    // Pass through — dgVoodoo2 handles the actual mode change (or suppresses it
    // in windowed mode based on its conf).
    HRESULT hr = m_real->SetDisplayMode(w, h, bpp, refresh, flags);
    Log("  -> 0x%08X", (unsigned)hr);
    return hr;
}

// ---------------------------------------------------------------------------
// CreateSurface
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxyDDraw7::CreateSurface(
    LPDDSURFACEDESC2       lpDDSD,
    LPDIRECTDRAWSURFACE7*  lplpDDS,
    IUnknown*              pUnk)
{
    DWORD caps = lpDDSD ? lpDDSD->ddsCaps.dwCaps : 0;
    Log("CreateSurface caps=0x%08X", caps);
    HRESULT hr = m_real->CreateSurface(lpDDSD, lplpDDS, pUnk);
    Log("  -> 0x%08X ptr=%p", (unsigned)hr, lplpDDS ? (void*)*lplpDDS : nullptr);

    // Wrap the primary surface so we can intercept Flip for pause support
    if (SUCCEEDED(hr) && lplpDDS && *lplpDDS && (caps & DDSCAPS_PRIMARYSURFACE))
    {
        *lplpDDS = new ProxySurface7(*lplpDDS, nullptr, m_real, m_hwnd, m_cfg, m_srcW, m_srcH);
        Log("  wrapped primary in ProxySurface7");
        if (caps & DDSCAPS_FLIP)
            m_initialized = true;
    }

    return hr;
}

// ---------------------------------------------------------------------------
// Delegation stubs
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxyDDraw7::Compact()                                         { return m_real->Compact(); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::CreateClipper(DWORD a,LPDIRECTDRAWCLIPPER* b,IUnknown* c) { return m_real->CreateClipper(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::CreatePalette(DWORD a,LPPALETTEENTRY b,LPDIRECTDRAWPALETTE* c,IUnknown* d) { return m_real->CreatePalette(a,b,c,d); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::DuplicateSurface(LPDIRECTDRAWSURFACE7 a,LPDIRECTDRAWSURFACE7* b) { return m_real->DuplicateSurface(a,b); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::EnumDisplayModes(DWORD a,LPDDSURFACEDESC2 b,LPVOID c,LPDDENUMMODESCALLBACK2 d) { return m_real->EnumDisplayModes(a,b,c,d); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::EnumSurfaces(DWORD a,LPDDSURFACEDESC2 b,LPVOID c,LPDDENUMSURFACESCALLBACK7 d) { return m_real->EnumSurfaces(a,b,c,d); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::FlipToGDISurface()                                { return m_real->FlipToGDISurface(); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetCaps(LPDDCAPS a,LPDDCAPS b)                    { return m_real->GetCaps(a,b); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetDisplayMode(LPDDSURFACEDESC2 a)                { return m_real->GetDisplayMode(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetFourCCCodes(LPDWORD a,LPDWORD b)               { return m_real->GetFourCCCodes(a,b); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetGDISurface(LPDIRECTDRAWSURFACE7* a)            { return m_real->GetGDISurface(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetMonitorFrequency(LPDWORD a)                    { return m_real->GetMonitorFrequency(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetScanLine(LPDWORD a)                            { return m_real->GetScanLine(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetVerticalBlankStatus(LPBOOL a)                  { return m_real->GetVerticalBlankStatus(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::Initialize(GUID* a)                               { return m_real->Initialize(a); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::RestoreDisplayMode()                              { return m_real->RestoreDisplayMode(); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::RestoreAllSurfaces()
{
    static int s_count = 0;
    if (++s_count <= 5) Log("RestoreAllSurfaces called");
    return m_real->RestoreAllSurfaces();
}
HRESULT STDMETHODCALLTYPE ProxyDDraw7::TestCooperativeLevel()
{
    static int s_count = 0;
    if (++s_count <= 5) Log("TestCooperativeLevel called");
    if (g_paused)
    {
        Log("  paused — pumping messages until focus returns");
        MSG m;
        while (g_paused)
        {
            if (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            else
                Sleep(16);
        }
        Log("  focus returned");
    }
    return m_real->TestCooperativeLevel();
}
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 a,DWORD b) { return m_real->GetDeviceIdentifier(a,b); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::StartModeTest(LPSIZE a,DWORD b,DWORD c)                    { return m_real->StartModeTest(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::EvaluateMode(DWORD a,DWORD* b)                            { return m_real->EvaluateMode(a,b); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::WaitForVerticalBlank(DWORD a, HANDLE b)
{
    static int s_count = 0;
    if (++s_count <= 5) Log("WaitForVerticalBlank called");
    return m_real->WaitForVerticalBlank(a, b);
}
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetAvailableVidMem(LPDDSCAPS2 a,LPDWORD b,LPDWORD c)      { return m_real->GetAvailableVidMem(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxyDDraw7::GetSurfaceFromDC(HDC a,LPDIRECTDRAWSURFACE7* b)           { return m_real->GetSurfaceFromDC(a,b); }
