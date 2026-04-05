#include "surface_proxy.h"
#include "log.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProxySurface7::ProxySurface7(IDirectDrawSurface7* real,
                             IDirectDrawSurface7* backBuffer,
                             IDirectDraw7*        dd7,
                             HWND                 hwnd,
                             const Config&        cfg,
                             int                  srcW,
                             int                  srcH)
    : m_real(real), m_backBuffer(backBuffer), m_dd7(dd7), m_hwnd(hwnd), m_cfg(cfg),
      m_srcW(srcW), m_srcH(srcH), m_refCount(1),
      m_realPrimary(nullptr), m_clipper(nullptr)
{
    // Surface/clipper creation only needed for StretchBlt path (not used here).
}

ProxySurface7::~ProxySurface7()
{
    if (m_clipper)     { m_clipper->Release();     m_clipper = nullptr; }
    if (m_realPrimary) { m_realPrimary->Release();  m_realPrimary = nullptr; }
    if (m_backBuffer)  { m_backBuffer->Release();   m_backBuffer = nullptr; }
    if (m_real)        { m_real->Release();         m_real = nullptr; }
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxySurface7::QueryInterface(REFIID riid, void** ppv)
{
    return m_real->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE ProxySurface7::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}
ULONG STDMETHODCALLTYPE ProxySurface7::Release()
{
    LONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) delete this;
    return (ULONG)ref;
}

// ---------------------------------------------------------------------------
// Destination rect calculation (letterbox / pillarbox / stretch)
// ---------------------------------------------------------------------------

RECT ProxySurface7::CalcDestRect() const
{
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    int dstW = client.right  - client.left;
    int dstH = client.bottom - client.top;

    if (m_cfg.stretchToFit || !m_cfg.keepAspect)
    {
        // Fill the entire window
        POINT tl = {0, 0};
        ClientToScreen(m_hwnd, &tl);
        RECT r;
        r.left   = tl.x;
        r.top    = tl.y;
        r.right  = tl.x + dstW;
        r.bottom = tl.y + dstH;
        return r;
    }

    // Letterbox / pillarbox
    double srcAR = (double)m_srcW / m_srcH;
    double dstAR = (double)dstW   / dstH;

    int fitW, fitH;
    if (dstAR > srcAR) {
        fitH = dstH;
        fitW = (int)(dstH * srcAR + 0.5);
    } else {
        fitW = dstW;
        fitH = (int)(dstW / srcAR + 0.5);
    }

    POINT tl = { (dstW - fitW) / 2, (dstH - fitH) / 2 };
    ClientToScreen(m_hwnd, &tl);

    RECT r;
    r.left   = tl.x;
    r.top    = tl.y;
    r.right  = tl.x + fitW;
    r.bottom = tl.y + fitH;
    return r;
}

// ---------------------------------------------------------------------------
// Present rendered back-buffer to the window via StretchBlt
// ---------------------------------------------------------------------------

void ProxySurface7::PresentToWindow(IDirectDrawSurface7* src)
{
    if (!m_realPrimary || !m_hwnd) return;

    // If primary was lost (alt-tab etc.), restore and recreate
    if (m_realPrimary->IsLost() == DDERR_SURFACELOST)
    {
        m_realPrimary->Restore();
        if (m_clipper && m_hwnd)
            m_clipper->SetHWnd(0, m_hwnd);
        m_realPrimary->SetClipper(m_clipper);
    }

    RECT srcRect = { 0, 0, m_srcW, m_srcH };
    RECT dstRect = CalcDestRect();

    m_realPrimary->Blt(&dstRect, src, &srcRect, DDBLT_WAIT, nullptr);
}

// ---------------------------------------------------------------------------
// Flip -- the key intercept
// ---------------------------------------------------------------------------

extern "C" volatile bool g_paused;  // defined in ddraw_proxy.cpp

HRESULT STDMETHODCALLTYPE ProxySurface7::Flip(LPDIRECTDRAWSURFACE7 pSurf, DWORD dwFlags)
{
    if (g_paused)
    {
        // Pump messages so WM_ACTIVATEAPP(true) can clear g_paused
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
        // Restore surfaces after focus returns — dgVoodoo2 marks them lost on deactivation
        if (m_dd7) m_dd7->RestoreAllSurfaces();
        m_real->Restore();
    }
    HRESULT hr = m_real->Flip(pSurf, dwFlags);
    // If still lost, restore and skip this frame
    if (hr == DDERR_SURFACELOST)
    {
        if (m_dd7) m_dd7->RestoreAllSurfaces();
        m_real->Restore();
        hr = DD_OK;
    }
    return hr;
}

// ---------------------------------------------------------------------------
// All other methods delegate straight to the real surface
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ProxySurface7::AddAttachedSurface(LPDIRECTDRAWSURFACE7 p)       { return m_real->AddAttachedSurface(p); }
HRESULT STDMETHODCALLTYPE ProxySurface7::AddOverlayDirtyRect(LPRECT p)                    { return m_real->AddOverlayDirtyRect(p); }
HRESULT STDMETHODCALLTYPE ProxySurface7::Blt(LPRECT a,LPDIRECTDRAWSURFACE7 b,LPRECT c,DWORD d,LPDDBLTFX e) { return m_real->Blt(a,b,c,d,e); }
HRESULT STDMETHODCALLTYPE ProxySurface7::BltBatch(LPDDBLTBATCH a,DWORD b,DWORD c)         { return m_real->BltBatch(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxySurface7::BltFast(DWORD a,DWORD b,LPDIRECTDRAWSURFACE7 c,LPRECT d,DWORD e) { return m_real->BltFast(a,b,c,d,e); }
HRESULT STDMETHODCALLTYPE ProxySurface7::DeleteAttachedSurface(DWORD a,LPDIRECTDRAWSURFACE7 b) { return m_real->DeleteAttachedSurface(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::EnumAttachedSurfaces(LPVOID a,LPDDENUMSURFACESCALLBACK7 b) { return m_real->EnumAttachedSurfaces(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::EnumOverlayZOrders(DWORD a,LPVOID b,LPDDENUMSURFACESCALLBACK7 c) { return m_real->EnumOverlayZOrders(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* ppSurface)
{
    DWORD reqCaps = caps ? caps->dwCaps : 0;
    Log("GetAttachedSurface caps=0x%08X backbuf=%p", reqCaps, (void*)m_backBuffer);

    // D3DX calls GetAttachedSurface(DDSCAPS_BACKBUFFER) on the fake primary to get
    // the render target surface.  Return our fake back buffer so D3DX can create
    // a hardware D3D device on it.
    if ((reqCaps & DDSCAPS_BACKBUFFER) && m_backBuffer)
    {
        m_backBuffer->AddRef();
        *ppSurface = m_backBuffer;
        Log("  -> returning fake backbuf %p", (void*)m_backBuffer);
        return DD_OK;
    }
    HRESULT hr = m_real->GetAttachedSurface(caps, ppSurface);
    Log("  -> delegated, result=0x%08X", (unsigned)hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE ProxySurface7::GetBltStatus(DWORD a)                            { return m_real->GetBltStatus(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetCaps(LPDDSCAPS2 a)                            { return m_real->GetCaps(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetClipper(LPDIRECTDRAWCLIPPER* a)               { return m_real->GetClipper(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetColorKey(DWORD a,LPDDCOLORKEY b)              { return m_real->GetColorKey(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetDC(HDC* a)                                    { return m_real->GetDC(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetFlipStatus(DWORD a)                           { return m_real->GetFlipStatus(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetOverlayPosition(LPLONG a,LPLONG b)            { return m_real->GetOverlayPosition(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetPalette(LPDIRECTDRAWPALETTE* a)               { return m_real->GetPalette(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetPixelFormat(LPDDPIXELFORMAT a)                { return m_real->GetPixelFormat(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetSurfaceDesc(LPDDSURFACEDESC2 a)               { return m_real->GetSurfaceDesc(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::Initialize(LPDIRECTDRAW a,LPDDSURFACEDESC2 b)    { return m_real->Initialize(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::IsLost()                                         { return m_real->IsLost(); }
HRESULT STDMETHODCALLTYPE ProxySurface7::Lock(LPRECT a,LPDDSURFACEDESC2 b,DWORD c,HANDLE d) { return m_real->Lock(a,b,c,d); }
HRESULT STDMETHODCALLTYPE ProxySurface7::ReleaseDC(HDC a)                                 { return m_real->ReleaseDC(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::Restore()                                        { return m_real->Restore(); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetClipper(LPDIRECTDRAWCLIPPER a)                { return m_real->SetClipper(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetColorKey(DWORD a,LPDDCOLORKEY b)              { return m_real->SetColorKey(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetOverlayPosition(LONG a,LONG b)                { return m_real->SetOverlayPosition(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetPalette(LPDIRECTDRAWPALETTE a)                { return m_real->SetPalette(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::Unlock(LPRECT a)                                 { return m_real->Unlock(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::UpdateOverlay(LPRECT a,LPDIRECTDRAWSURFACE7 b,LPRECT c,DWORD d,LPDDOVERLAYFX e) { return m_real->UpdateOverlay(a,b,c,d,e); }
HRESULT STDMETHODCALLTYPE ProxySurface7::UpdateOverlayDisplay(DWORD a)                    { return m_real->UpdateOverlayDisplay(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::UpdateOverlayZOrder(DWORD a,LPDIRECTDRAWSURFACE7 b) { return m_real->UpdateOverlayZOrder(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetDDInterface(LPVOID* a)                        { return m_real->GetDDInterface(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::PageLock(DWORD a)                                { return m_real->PageLock(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::PageUnlock(DWORD a)                              { return m_real->PageUnlock(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetSurfaceDesc(LPDDSURFACEDESC2 a,DWORD b)       { return m_real->SetSurfaceDesc(a,b); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetPrivateData(REFGUID a,LPVOID b,DWORD c,DWORD d) { return m_real->SetPrivateData(a,b,c,d); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetPrivateData(REFGUID a,LPVOID b,LPDWORD c)     { return m_real->GetPrivateData(a,b,c); }
HRESULT STDMETHODCALLTYPE ProxySurface7::FreePrivateData(REFGUID a)                       { return m_real->FreePrivateData(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetUniquenessValue(LPDWORD a)                    { return m_real->GetUniquenessValue(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::ChangeUniquenessValue()                          { return m_real->ChangeUniquenessValue(); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetPriority(DWORD a)                             { return m_real->SetPriority(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetPriority(LPDWORD a)                           { return m_real->GetPriority(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::SetLOD(DWORD a)                                  { return m_real->SetLOD(a); }
HRESULT STDMETHODCALLTYPE ProxySurface7::GetLOD(LPDWORD a)                                { return m_real->GetLOD(a); }
