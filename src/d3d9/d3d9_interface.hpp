#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "d3d9.h"
#include "d3d9_present_validation.hpp"

namespace dxmt {

// PresentParamsRejectReason (the D3DPRESENT_PARAMETERS accept/reject matrix)
// lives in d3d9_present_validation.hpp, and is in scope here through that
// include.

// Resolve spec-permitted "use the runtime default" placeholders
// (BackBufferCount=0 -> 1; BackBufferFormat=UNKNOWN -> X8R8G8B8;
// AutoDepthStencilFormat=UNKNOWN -> D24S8; Windowed + zero extent ->
// GetClientRect of hwndFallback, 8px floor). Mutates p in place.
// Returns false on a value that's invalid in a way PresentParamsRejectReason
// doesn't catch (unmappable BackBufferFormat, zero extent on a hwndless
// chain, or a fullscreen extent the adapter cannot present).
// Reference: wined3d swapchain.c.
bool CanonicalisePresentParams(D3DPRESENT_PARAMETERS &p, HWND hwndFallback, UINT adapter);

// MADEIRA: the [d3d9-modes] trace. Two things, and only these two, because the
// question they answer is "which mode did the application ask for, and why was
// it refused" -- the one thing a log of a failed renderer init never showed.
//
//  - LogAdapterModesOnce() dumps, once per process, the adapter's mode count
//    and its first eight modes. Called from the mode-enumeration entry points,
//    so it fires for an application that never reaches CreateDevice.
//  - LogPresentRequest() logs every CreateDevice / CreateDeviceEx / Reset /
//    ResetEx / CreateAdditionalSwapChain request as
//    "WxH fmt refresh windowed=" with the HRESULT it returned.
//
// Both write through Logger::info, which reaches the Wine debug channel.
void LogAdapterModesOnce(UINT adapter);
void LogPresentRequest(const char *what, const D3DPRESENT_PARAMETERS &p, HRESULT hr);

class MTLD3D9Interface final : public ComObject<IDirect3D9Ex> {
public:
  MTLD3D9Interface(UINT SDKVersion, bool isEx);
  ~MTLD3D9Interface();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) override;
  UINT STDMETHODCALLTYPE GetAdapterCount() override;
  HRESULT STDMETHODCALLTYPE
  GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) override;
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) override;
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceType(
      UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
  ) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
      D3DFORMAT CheckFormat
  ) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
      DWORD *pQualityLevels
  ) override;
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
      D3DFORMAT DepthStencilFormat
  ) override;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
  ) override;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) override;
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override;
  HRESULT STDMETHODCALLTYPE CreateDevice(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
      D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
  ) override;

  UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) override;
  HRESULT STDMETHODCALLTYPE
  EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode) override;
  HRESULT STDMETHODCALLTYPE
  GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) override;
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
      D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
      IDirect3DDevice9Ex **ppReturnedDeviceInterface
  ) override;
  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID *pLUID) override;

private:
  // MADEIRA [d3d9-caps]: the bodies of the five format/capability probes.
  //
  // The public methods above are thin wrappers that count the call, run one
  // of these, and print the query and its HRESULT once per distinct tuple.
  // The split exists because CheckDeviceFormat alone returns from two dozen
  // places, and a trace threaded through all of them would drift the first
  // time one moved.
  //
  // These are deliberately NOT declared STDMETHODCALLTYPE: gen_d3d9_census.py
  // scans for that keyword to place the per-method counters and to build the
  // code table, so a second definition carrying it would renumber every
  // method after it. Same reason they are not in the vtable.
  HRESULT CheckDeviceTypeProbe(
      UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
  );
  HRESULT CheckDeviceFormatProbe(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
      D3DFORMAT CheckFormat
  );
  HRESULT CheckDeviceMultiSampleTypeProbe(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
      D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels
  );
  HRESULT CheckDepthStencilMatchProbe(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
      D3DFORMAT DepthStencilFormat
  );
  HRESULT CheckDeviceFormatConversionProbe(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
  );

  const UINT m_sdkVersion;
  const bool m_isEx;
  // Adapter list cached at construction. macOS GPUs do not hotplug
  // within a process (the array is a snapshot of the Metal device
  // registry at first query), and wined3d / DXVK both snap their
  // adapter set at IDirect3D9 creation. Caching dodges a wine_unix_call
  // round-trip on every per-method validation gate below.
  WMT::Reference<WMT::Array<WMT::Device>> m_adapters;
  const UINT m_adapterCount;
};

} // namespace dxmt
