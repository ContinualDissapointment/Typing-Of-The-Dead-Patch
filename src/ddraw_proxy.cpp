#include "ddraw_proxy.h"
#include "surface_proxy.h"
#include "log.h"
#include <cstdio>
#include <d3d.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

// ---------------------------------------------------------------------------
// Window subclass — blocks WM_ACTIVATEAPP minimization in borderless mode
// ---------------------------------------------------------------------------

static WNDPROC g_origWndProc      = nullptr;
static bool    g_pauseOnFocusLoss = false;
static bool    g_muteOnFocusLoss  = false;
extern "C" volatile bool g_paused = false;
static volatile bool     g_focused = true;

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

// Watcher thread: when g_paused is set, suspends the main thread.
// Polls GetForegroundWindow to detect focus return, then resumes.

static LRESULT CALLBACK BorderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATEAPP)
    {
        g_focused = (wParam != 0);
        Log("WM_ACTIVATEAPP(%s)", wParam ? "true" : "false");
        if (!wParam)
        {
            if (g_pauseOnFocusLoss) g_paused = true;
            if (g_muteOnFocusLoss)  SetProcessAudioMute(true);
            return 0;  // eat — prevents DirectDraw minimizing window
        }
        else
        {
            g_paused = false;  // clear pause on focus return
            if (g_muteOnFocusLoss)  SetProcessAudioMute(false);
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
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

    if (SUCCEEDED(hr) && hwnd && m_cfg.mode == MODE_BORDERLESS)
    {
        // Subclass the window to suppress WM_ACTIVATEAPP minimization.
        // DirectDraw sets exclusive mode which causes the game window to minimize
        // on alt-tab — eating that message keeps it visible.
        if (!g_origWndProc)
        {
            g_pauseOnFocusLoss = m_cfg.pauseOnFocusLoss;
            g_muteOnFocusLoss  = m_cfg.muteOnFocusLoss;
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                                        (LONG_PTR)BorderlessWndProc);
            Log("  subclassed HWND (pauseOnFocusLoss=%d muteOnFocusLoss=%d)",
                (int)g_pauseOnFocusLoss, (int)g_muteOnFocusLoss);
        }
        ApplyWindowStyle();
    }

    if (SUCCEEDED(hr) && hwnd && m_cfg.mode == MODE_WINDOWED)
        ApplyWindowStyle();

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
