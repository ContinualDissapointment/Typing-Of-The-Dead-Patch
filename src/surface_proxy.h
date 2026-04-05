#pragma once
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include "config.h"

// Wraps an offscreen IDirectDrawSurface7.
// Intercepts Flip() to StretchBlt the rendered frame into the game window,
// scaled to the user's target resolution.
class ProxySurface7 : public IDirectDrawSurface7
{
public:
    ProxySurface7(IDirectDrawSurface7* real,
                  IDirectDrawSurface7* backBuffer,
                  IDirectDraw7*        dd7,
                  HWND                 hwnd,
                  const Config&        cfg,
                  int                  srcW,
                  int                  srcH);
    virtual ~ProxySurface7();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // IDirectDrawSurface7 -- all delegate to m_real except Flip
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE7) override;
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override;
    HRESULT STDMETHODCALLTYPE Blt(LPRECT,LPDIRECTDRAWSURFACE7,LPRECT,DWORD,LPDDBLTFX) override;
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH,DWORD,DWORD) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD,DWORD,LPDIRECTDRAWSURFACE7,LPRECT,DWORD) override;
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD,LPDIRECTDRAWSURFACE7) override;
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID,LPDDENUMSURFACESCALLBACK7) override;
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD,LPVOID,LPDDENUMSURFACESCALLBACK7) override;
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE7, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS2,LPDIRECTDRAWSURFACE7*) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS2) override;
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER*) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD,LPDDCOLORKEY) override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC*) override;
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override;
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG,LPLONG) override;
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE*) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC2) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW,LPDDSURFACEDESC2) override;
    HRESULT STDMETHODCALLTYPE IsLost() override;
    HRESULT STDMETHODCALLTYPE Lock(LPRECT,LPDDSURFACEDESC2,DWORD,HANDLE) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override;
    HRESULT STDMETHODCALLTYPE Restore() override;
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD,LPDDCOLORKEY) override;
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG,LONG) override;
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE) override;
    HRESULT STDMETHODCALLTYPE Unlock(LPRECT) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT,LPDIRECTDRAWSURFACE7,LPRECT,DWORD,LPDDOVERLAYFX) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD,LPDIRECTDRAWSURFACE7) override;
    HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID*) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD) override;
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override;
    HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC2,DWORD) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,LPVOID,DWORD,DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,LPVOID,LPDWORD) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(LPDWORD) override;
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override;
    HRESULT STDMETHODCALLTYPE SetPriority(DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPriority(LPDWORD) override;
    HRESULT STDMETHODCALLTYPE SetLOD(DWORD) override;
    HRESULT STDMETHODCALLTYPE GetLOD(LPDWORD) override;

    // Allow the ddraw proxy to update the hwnd/config after surface creation
    void SetHwnd(HWND hwnd) { m_hwnd = hwnd; }

private:
    RECT CalcDestRect() const;
    void PresentToWindow(IDirectDrawSurface7* src);

    IDirectDrawSurface7* m_real;       // fake "primary" surface
    IDirectDrawSurface7* m_backBuffer; // fake back buffer — D3DX renders here
    IDirectDraw7*        m_dd7;
    HWND                 m_hwnd;
    Config               m_cfg;
    int                  m_srcW;
    int                  m_srcH;
    LONG                 m_refCount;

    // Real desktop primary + clipper for StretchBlt output
    IDirectDrawSurface7* m_realPrimary;
    IDirectDrawClipper*  m_clipper;
};
