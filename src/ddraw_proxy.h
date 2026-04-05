#pragma once
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include "config.h"

// Wraps IDirectDraw7.
// Key intercepts:
//   SetCooperativeLevel  -- ensures a valid HWND reaches dgVoodoo2 during enumeration;
//                           applies window style for windowed mode
//   SetDisplayMode       -- pass-through (dgVoodoo2 conf controls actual mode)
//   CreateSurface        -- pass-through (dgVoodoo2 owns the flip chain)
class ProxyDDraw7 : public IDirectDraw7
{
public:
    ProxyDDraw7(IDirectDraw7* real, const Config& cfg);
    virtual ~ProxyDDraw7();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // Key intercepts
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC2 lpDDSurfaceDesc, LPDIRECTDRAWSURFACE7* lplpDDSurface, IUnknown* pUnkOuter) override;

    // All others delegate to m_real
    HRESULT STDMETHODCALLTYPE Compact() override;
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*) override;
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMMODESCALLBACK2) override;
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) override;
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS, LPDDCAPS) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC2) override;
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD, LPDWORD) override;
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE7*) override;
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD) override;
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD) override;
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL) override;
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override;
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override;
    HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override;
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2, DWORD) override;
    HRESULT STDMETHODCALLTYPE StartModeTest(LPSIZE, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE EvaluateMode(DWORD, DWORD*) override;
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override;
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2, LPDWORD, LPDWORD) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7*) override;

private:
    void ApplyWindowStyle();

    IDirectDraw7* m_real;
    Config        m_cfg;
    HWND          m_hwnd;
    LONG          m_refCount;
    bool          m_windowStyleApplied;  // set after first flip-chain CreateSurface
    bool          m_initialized;         // set after flip-chain surface created — safe to pause

    // Saved game source resolution (always 640x480)
    int           m_srcW;
    int           m_srcH;
};
