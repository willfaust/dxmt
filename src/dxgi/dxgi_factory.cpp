#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "dxgi_vk_interfaces.h"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include "Metal.hpp"
/* madeira x64 graphics factory: the header madeira_d3d12 uses for its own x64
 * entries (madeira-d3d12/src/pe in the Madeira tree, which this submodule sits
 * in, as winemetal's unix side finds build/madeira_cfg.h). */
#include "../../../madeira-d3d12/src/pe/madeira_graphics_entry.h"
#include <atomic>

namespace dxmt {

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config);
LUID GetAdapterLuid(WMT::Device device);

bool VkSwapChainEnabled();
HRESULT CreateVkSwapChain(IDXGIFactory1 *pFactory, IUnknown *pDevice,
                          IDXGIVkSwapChainFactory *pVkFactory, HWND hWnd,
                          const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
                          IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain);

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory7> {

public:
  MTLDXGIFactory(UINT Flags) : flags_(Flags) {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
        riid == __uuidof(IDXGIFactory7)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) override {
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(pDevice, pDesc->OutputWindow, &desc,
                                        &descFs, nullptr, &swapChain);

    *ppSwapChain = swapChain;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {
    InitReturnPtr(ppSwapChain);

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice)
      return DXGI_ERROR_INVALID_CALL;

    // A device of DXMT's own, or a Direct3D 12 queue that presents through
    // Vulkan (vkd3d-proton); see dxgi_swapchain_vk.cpp
    Com<IMTLDXGIDevice> metal_dxgi_device;
    Com<IDXGIVkSwapChainFactory> vk_swapchain_factory;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device))) &&
        (!VkSwapChainEnabled() ||
         FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&vk_swapchain_factory))))) {
      ERR("Unsupported device type");
      return DXGI_ERROR_UNSUPPORTED;
    }

    // Make sure the back buffer size is not zero
    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    if (metal_dxgi_device == nullptr)
      return CreateVkSwapChain(this, pDevice, vk_swapchain_factory.ptr(), hWnd,
                               &desc, &fsDesc, pRestrictToOutput, ppSwapChain);

    return metal_dxgi_device->CreateSwapChain(this, hWnd, &desc, &fsDesc,
                                              ppSwapChain);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    auto devices = WMT::CopyAllDevices();
    UINT adapter_count = devices.count();

    if (Adapter >= adapter_count)
      return DXGI_ERROR_NOT_FOUND;

    UINT adjusted_adapter = Adapter;
    if (adapter_count > 1) {
      UINT preferred_adapter = 0;
      for (unsigned i = 0; i < adapter_count; i++) {
        if (!devices.object(i).hasUnifiedMemory())
          preferred_adapter = i;
      }
      if (Adapter == 0)
        adjusted_adapter = preferred_adapter;
      else
        adjusted_adapter = Adapter <= preferred_adapter ? Adapter - 1 : Adapter;
    }

    auto device = devices.object(adjusted_adapter);

    *ppAdapter = CreateAdapter(device, this, Config::getInstance());
    // devices->release(); // no you should not release it...
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pWindowHandle = associated_window_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) override {
    if (Flags) {
      WARN("MakeWindowAssociation: Ignoring flags ", Flags);
    }
    associated_window_ = WindowHandle;
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return TRUE; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    ERR("Not implemented");
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    ERR("Not implemented");
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid,
                                              void **adapter) override {
    InitReturnPtr(adapter);

    if (adapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    auto devices = WMT::CopyAllDevices();
    for (unsigned i = 0; i < devices.count(); i++) {
      auto device = devices.object(i);
      LUID device_luid = GetAdapterLuid(device);
      if (device_luid.LowPart == luid.LowPart && device_luid.HighPart == luid.HighPart) {
        Com<IMTLDXGIAdapter> found = CreateAdapter(device, this, Config::getInstance());
        return found->QueryInterface(iid, adapter);
      }
    }

    WARN("DXGIFactory::EnumAdapterByLuid: no adapter with LUID ", luid.HighPart, ":", luid.LowPart);
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    ERR("DXGIFactory::EnumWrapAdapter: not implemented");
    return DXGI_ERROR_NOT_FOUND;
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;

      *info = TRUE;
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      return E_INVALIDARG;
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    // GpuPreference ignored, since Apple Silicon has only 1 GPU anyway
    // FIXME: support Intel Mac with dedicated GPU
    Com<IDXGIAdapter1> adapter;
    HRESULT hr = this->EnumAdapters1(Adapter, &adapter);

    if (FAILED(hr))
      return hr;
    return adapter->QueryInterface(riid, ppvAdapter);
  };

  HRESULT STDMETHODCALLTYPE
  RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD *pdwCookie) override {
    if (hEvent == nullptr || pdwCookie == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    // Metal's adapters do not change while a process runs, so the event is
    // never signalled; registration still succeeds, with a nonzero cookie
    static std::atomic<DWORD> next_cookie = 0;
    DWORD cookie;
    do {
      cookie = ++next_cookie;
    } while (cookie == 0);
    *pdwCookie = cookie;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  UnregisterAdaptersChangedEvent(DWORD dwCookie) override {
    return S_OK;
  }

private:
  UINT flags_;

  HWND associated_window_ = nullptr;
};


/* madeira x64 graphics factory begin */
class MTLDXGIHookFactory : public MTLDXGIFactory {
public:
  explicit MTLDXGIHookFactory(UINT flags) : MTLDXGIFactory(flags) {}
  void InitializeEntries() {
    void **original, **replacement = entry_vtable_ + 2;
    void *com_object = static_cast<IDXGIFactory7 *>(this);
    __builtin_memcpy(&original, com_object, sizeof(original));
    mad_x64_graphics_factory_table(original, entry_vtable_, &__ImageBase);
    __builtin_memcpy(com_object, &replacement, sizeof(replacement));
  }
  MAD_X64_GRAPHICS_ENTRY HRESULT STDMETHODCALLTYPE
  MakeWindowAssociation(HWND window, UINT flags) override;
  MAD_X64_GRAPHICS_ENTRY HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
                  IDXGISwapChain **out) override;
  MAD_X64_GRAPHICS_ENTRY HRESULT STDMETHODCALLTYPE
  CreateSwapChainForHwnd(IUnknown *device, HWND window,
      const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
      IDXGIOutput *restrict_output, IDXGISwapChain1 **out) override;
  MAD_X64_GRAPHICS_ENTRY HRESULT STDMETHODCALLTYPE
  CreateSwapChainForCoreWindow(IUnknown *device, IUnknown *window,
      const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_output,
      IDXGISwapChain1 **out) override;
  MAD_X64_GRAPHICS_ENTRY HRESULT STDMETHODCALLTYPE
  CreateSwapChainForComposition(IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
      IDXGIOutput *restrict_output, IDXGISwapChain1 **out) override;
private:
  void *entry_vtable_[36];
};

HRESULT STDMETHODCALLTYPE
MTLDXGIHookFactory::MakeWindowAssociation(HWND window, UINT flags) {
  return MTLDXGIFactory::MakeWindowAssociation(window, flags);
}
HRESULT STDMETHODCALLTYPE
MTLDXGIHookFactory::CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
                                 IDXGISwapChain **out) {
  return MTLDXGIFactory::CreateSwapChain(device, desc, out);
}
HRESULT STDMETHODCALLTYPE
MTLDXGIHookFactory::CreateSwapChainForHwnd(IUnknown *device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
    IDXGIOutput *restrict_output, IDXGISwapChain1 **out) {
  return MTLDXGIFactory::CreateSwapChainForHwnd(device, window, desc, fs, restrict_output, out);
}
HRESULT STDMETHODCALLTYPE
MTLDXGIHookFactory::CreateSwapChainForCoreWindow(IUnknown *device, IUnknown *window,
    const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_output,
    IDXGISwapChain1 **out) {
  return MTLDXGIFactory::CreateSwapChainForCoreWindow(device, window, desc, restrict_output, out);
}
HRESULT STDMETHODCALLTYPE
MTLDXGIHookFactory::CreateSwapChainForComposition(IUnknown *device,
    const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_output,
    IDXGISwapChain1 **out) {
  return MTLDXGIFactory::CreateSwapChainForComposition(device, desc, restrict_output, out);
}
/* madeira x64 graphics factory end */

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid,
                                                void **ppFactory) {
  try {
    const bool patchable = mad_x64_graphics_entry_enabled();
    MTLDXGIFactory* factory;
    if (patchable) {
      auto* hook_factory = new MTLDXGIHookFactory(Flags);
      hook_factory->InitializeEntries();
      factory = hook_factory;
    } else {
      factory = new MTLDXGIFactory(Flags);
    }
    static std::atomic<bool> noted{false};
    if (patchable && !noted.exchange(true))
      Logger::info("[graphics-entry] x64 patchable DXGI factory methods enabled (PE entries)");
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    if (FAILED(hr))
      return hr;

    return S_OK;
  } catch (const MTLD3DError &e) {
    Logger::err(e.message());
    return E_FAIL;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  return CreateDXGIFactory2(0, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  return CreateDXGIFactory2(0, riid, factory);
}

} // namespace dxmt