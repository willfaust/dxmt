/*
 * DXGI swapchains for Direct3D 12 devices that present through Vulkan.
 *
 * vkd3d-proton's ID3D12CommandQueue does not implement IMTLDXGIDevice; it
 * offers IDXGIVkSwapChainFactory instead and expects DXGI to wrap the
 * IDXGIVkSwapChain it creates in an IDXGISwapChain4, handing it a surface
 * factory for the window. This is the same split DXVK's DXGI uses (the idea
 * is DXVK's, src/dxgi/dxgi_swapchain.cpp); the implementation here is DXMT's
 * own and covers windowed presentation only: fullscreen requests keep the
 * window as it is, which is what the platform's window does anyway.
 */
#include "com/com_pointer.hpp"
#include "dxgi_object.hpp"
#include "dxgi_vk_interfaces.h"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace dxmt {

namespace {

/* VkWin32SurfaceCreateInfoKHR and the two entry points the surface factory
 * needs, as vulkan_core.h / vulkan_win32.h declare them. */
constexpr int32_t kVkStructureTypeWin32SurfaceCreateInfo = 1000009000;
constexpr VkResult kVkErrorInitializationFailed = -3;
constexpr VkResult kVkErrorExtensionNotPresent = -7;

struct Win32SurfaceCreateInfo {
  int32_t sType;
  const void *pNext;
  uint32_t flags;
  HINSTANCE hinstance;
  HWND hwnd;
};

using PFN_VoidFunction = void(__stdcall *)();
using PFN_GetInstanceProcAddr = PFN_VoidFunction(__stdcall *)(VkInstance, const char *);
using PFN_CreateWin32Surface = VkResult(__stdcall *)(VkInstance, const Win32SurfaceCreateInfo *,
                                                     const void *, VkSurfaceKHR *);

/* The Vulkan loader vkd3d-proton created its instance with. d3d12core loads
 * winevulkan.dll first and vulkan-1.dll second and keeps the first that
 * exports vkGetInstanceProcAddr; mirror that order so the instance handle
 * belongs to the loader asked. Both are already loaded when a swapchain is
 * made (the device exists), so this only looks them up. */
PFN_GetInstanceProcAddr FindInstanceProcAddr(const char **pModuleName) {
  static const wchar_t *const kModules[] = {L"winevulkan.dll", L"vulkan-1.dll"};
  static const char *const kNames[] = {"winevulkan.dll", "vulkan-1.dll"};
  for (unsigned i = 0; i < 2; i++) {
    HMODULE module = GetModuleHandleW(kModules[i]);
    if (!module)
      continue;
    auto proc = reinterpret_cast<PFN_GetInstanceProcAddr>(
        reinterpret_cast<void *>(GetProcAddress(module, "vkGetInstanceProcAddr")));
    if (proc) {
      *pModuleName = kNames[i];
      return proc;
    }
  }
  *pModuleName = "none";
  return nullptr;
}

class MTLDXGIVkSurfaceFactory final
    : public ComObjectWithInitialRef<IDXGIVkSurfaceFactory> {
public:
  MTLDXGIVkSurfaceFactory(HWND hWnd) : hwnd_(hWnd) {
    get_instance_proc_addr_ = FindInstanceProcAddr(&loader_name_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIVkSurfaceFactory)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  VkResult STDMETHODCALLTYPE CreateSurface(VkInstance Instance, VkPhysicalDevice Adapter,
                                           VkSurfaceKHR *pSurface) final {
    if (!get_instance_proc_addr_) {
      ERR("[dxgi] ml1320 Vulkan swapchain: no loaded winevulkan.dll or vulkan-1.dll exports "
          "vkGetInstanceProcAddr");
      return kVkErrorInitializationFailed;
    }
    auto create = reinterpret_cast<PFN_CreateWin32Surface>(
        get_instance_proc_addr_(Instance, "vkCreateWin32SurfaceKHR"));
    if (!create) {
      ERR("[dxgi] ml1320 Vulkan swapchain: the instance has no vkCreateWin32SurfaceKHR "
          "(VK_KHR_win32_surface not enabled?)");
      return kVkErrorExtensionNotPresent;
    }
    Win32SurfaceCreateInfo info = {};
    info.sType = kVkStructureTypeWin32SurfaceCreateInfo;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = hwnd_;
    VkResult vr = create(Instance, &info, nullptr, pSurface);
    WARN("[dxgi] ml1320 Vulkan swapchain: vkCreateWin32SurfaceKHR(hwnd=", (void *)hwnd_,
         ") through ", loader_name_, " -> VkResult ", vr);
    return vr;
  }

  const char *LoaderName() const { return loader_name_; }

private:
  HWND hwnd_;
  PFN_GetInstanceProcAddr get_instance_proc_addr_ = nullptr;
  const char *loader_name_ = "none";
};

/* The legacy desc's Windowed = FALSE, SetFullscreenState(TRUE) and friends:
 * report each kind once per process, the calls themselves are frequent. */
void LogOnce(std::atomic<bool> &flag, const char *message) {
  if (!flag.exchange(true))
    WARN("[dxgi] ml1320 Vulkan swapchain: ", message);
}

std::atomic<bool> g_logged_fullscreen{false};
std::atomic<bool> g_logged_source_size{false};
std::atomic<bool> g_logged_first_present{false};
std::atomic<uint32_t> g_present_failures{0};

class MTLDXGIVkSwapChain final : public MTLDXGIObject<IDXGISwapChain4> {
public:
  MTLDXGIVkSwapChain(IDXGIFactory1 *pFactory, IUnknown *pDevice, IDXGIVkSwapChain *pChain,
                     IDXGIVkSurfaceFactory *pSurfaceFactory, HWND hWnd,
                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC &FullscreenDesc,
                     IDXGIOutput *pRestrictToOutput)
      : factory_(pFactory), device_(pDevice), chain_(pChain),
        surface_factory_(pSurfaceFactory), restrict_to_output_(pRestrictToOutput),
        hwnd_(hWnd), fullscreen_desc_(FullscreenDesc) {
    pChain->QueryInterface(IID_PPV_ARGS(&chain1_));
    fullscreen_desc_.Windowed = TRUE;
    DXGI_SWAP_CHAIN_DESC1 desc;
    chain_->GetDesc(&desc);
    creation_flags_ = desc.Flags;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
        riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid)) {
      WARN("DXGISwapChain (Vulkan): Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);
    return factory_->QueryInterface(riid, ppParent);
  }

  /* The device a D3D12 swapchain was made with is the command queue. */
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) final {
    InitReturnPtr(ppDevice);
    return device_->QueryInterface(riid, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) final {
    return Present1(SyncInterval, Flags, nullptr);
  }

  HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                     const DXGI_PRESENT_PARAMETERS *pPresentParameters) final {
    if (SyncInterval > 4)
      return DXGI_ERROR_INVALID_CALL;

    HRESULT hr;
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      hr = chain_->Present(SyncInterval, PresentFlags, pPresentParameters);
      if (hr == S_OK && !(PresentFlags & DXGI_PRESENT_TEST))
        present_count_++;
    }

    if (hr == S_OK && !(PresentFlags & DXGI_PRESENT_TEST) &&
        !g_logged_first_present.exchange(true)) {
      WARN("[dxgi] ml1320 Vulkan swapchain: first present (sync interval ", SyncInterval,
           ", flags 0x", std::hex, PresentFlags, std::dec, ")");
    } else if (FAILED(hr) && g_present_failures.fetch_add(1) < 8) {
      ERR("[dxgi] ml1320 Vulkan swapchain: Present failed, hr 0x", std::hex, (uint32_t)hr);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) final {
    InitReturnPtr(ppSurface);
    if (ppSurface == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    return chain_->GetImage(Buffer, riid, ppSurface);
  }

  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() final {
    return chain_->GetImageIndex();
  }

  /* Windowed only: the window already covers the screen it is on. A request
   * for fullscreen succeeds and leaves it windowed, so GetFullscreenState
   * keeps answering FALSE and the program does not wait for a mode change. */
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) final {
    if (Fullscreen)
      LogOnce(g_logged_fullscreen,
              "fullscreen requested, the swapchain stays windowed (S_OK)");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) final {
    if (pFullscreen)
      *pFullscreen = FALSE;
    if (ppTarget)
      *ppTarget = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_SWAP_CHAIN_DESC1 desc;
    chain_->GetDesc(&desc);

    pDesc->BufferDesc.Width = desc.Width;
    pDesc->BufferDesc.Height = desc.Height;
    pDesc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
    pDesc->BufferDesc.Format = desc.Format;
    pDesc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
    pDesc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
    pDesc->SampleDesc = desc.SampleDesc;
    pDesc->BufferUsage = desc.BufferUsage;
    pDesc->BufferCount = desc.BufferCount;
    pDesc->OutputWindow = hwnd_;
    pDesc->Windowed = TRUE;
    pDesc->SwapEffect = desc.SwapEffect;
    pDesc->Flags = desc.Flags;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;
    return chain_->GetDesc(pDesc);
  }

  HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;
    *pDesc = fullscreen_desc_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) final {
    if (pHwnd == nullptr)
      return E_INVALIDARG;
    *pHwnd = hwnd_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void **ppUnk) final {
    InitReturnPtr(ppUnk);
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                          DXGI_FORMAT NewFormat, UINT SwapChainFlags) final {
    return ResizeBuffers1(BufferCount, Width, Height, NewFormat, SwapChainFlags, nullptr,
                          nullptr);
  }

  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                           DXGI_FORMAT Format, UINT SwapChainFlags,
                                           const UINT *pCreationNodeMask,
                                           IUnknown *const *ppPresentQueue) final {
    if (!wsi::isWindow(hwnd_))
      return DXGI_ERROR_INVALID_CALL;
    if (BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    std::lock_guard<dxmt::mutex> lock(mutex_);

    DXGI_SWAP_CHAIN_DESC1 desc;
    chain_->GetDesc(&desc);

    if (BufferCount)
      desc.BufferCount = BufferCount;
    desc.Width = Width;
    desc.Height = Height;
    /* Zero means the window's client size, as at creation. */
    wsi::getWindowSize(hwnd_, Width ? nullptr : &desc.Width, Height ? nullptr : &desc.Height);
    desc.Width = std::max(desc.Width, 1u);
    desc.Height = std::max(desc.Height, 1u);
    if (Format != DXGI_FORMAT_UNKNOWN)
      desc.Format = Format;
    /* The waitable-object flag is fixed at creation; the rest follows the call. */
    constexpr UINT kFixedFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    desc.Flags = (creation_flags_ & kFixedFlags) | (SwapChainFlags & ~kFixedFlags);

    HRESULT hr = chain_->ChangeProperties(&desc, pCreationNodeMask, ppPresentQueue);
    if (FAILED(hr))
      WARN("[dxgi] ml1320 Vulkan swapchain: ResizeBuffers(", desc.BufferCount, ", ", desc.Width,
           "x", desc.Height, ", format ", desc.Format, ") failed, hr 0x", std::hex,
           (uint32_t)hr);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) final {
    if (pNewTargetParameters == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    if (!wsi::isWindow(hwnd_))
      return DXGI_ERROR_INVALID_CALL;

    if (pNewTargetParameters->RefreshRate.Numerator != 0)
      fullscreen_desc_.RefreshRate = pNewTargetParameters->RefreshRate;
    fullscreen_desc_.ScanlineOrdering = pNewTargetParameters->ScanlineOrdering;
    fullscreen_desc_.Scaling = pNewTargetParameters->Scaling;

    if (pNewTargetParameters->Width && pNewTargetParameters->Height)
      wsi::resizeWindow(hwnd_, &window_state_, pNewTargetParameters->Width,
                        pNewTargetParameters->Height);
    return S_OK;
  }

  /* The output the window is on, from the adapter the device was made on
   * (vkd3d-proton's device parent, which is this DXGI's adapter), else the
   * factory's first adapter; the adapter's first output when no monitor
   * matches. */
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);
    if (ppOutput == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIAdapter> adapter;
    if (FAILED(chain_->GetAdapter(IID_PPV_ARGS(&adapter))) || adapter == nullptr) {
      adapter = nullptr;
      if (FAILED(factory_->EnumAdapters(0, &adapter)))
        return DXGI_ERROR_NOT_FOUND;
    }

    HMONITOR monitor = wsi::isWindow(hwnd_) ? wsi::getWindowMonitor(hwnd_) : nullptr;
    Com<IDXGIOutput> first;
    Com<IDXGIOutput> output;
    for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC desc;
      if (monitor && SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        *ppOutput = output.ref();
        return S_OK;
      }
      if (first == nullptr)
        first = output;
      output = nullptr;
    }

    if (first == nullptr)
      return DXGI_ERROR_NOT_FOUND;
    *ppOutput = first.ref();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) final {
    if (pStats == nullptr)
      return E_INVALIDARG;

    *pStats = {};
    if (chain1_ != nullptr) {
      DXGI_VK_FRAME_STATISTICS vk_stats = {};
      chain1_->GetFrameStatistics(&vk_stats);
      pStats->PresentCount = (UINT)vk_stats.PresentCount;
      pStats->PresentRefreshCount = (UINT)vk_stats.PresentCount;
      pStats->SyncRefreshCount = (UINT)vk_stats.PresentCount;
      pStats->SyncQPCTime.QuadPart = (LONGLONG)vk_stats.PresentQPCTime;
    } else {
      UINT count = present_count_.load();
      pStats->PresentCount = count;
      pStats->PresentRefreshCount = count;
      pStats->SyncRefreshCount = count;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) final {
    if (pLastPresentCount == nullptr)
      return E_INVALIDARG;
    if (chain1_ != nullptr) {
      UINT64 count = 0;
      chain1_->GetLastPresentCount(&count);
      *pLastPresentCount = (UINT)count;
    } else {
      *pLastPresentCount = present_count_.load();
    }
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() final { return FALSE; }

  HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) final {
    if (ppRestrictToOutput == nullptr)
      return E_INVALIDARG;
    *ppRestrictToOutput = restrict_to_output_.ref();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) final {
    if (pColor == nullptr)
      return E_INVALIDARG;
    background_color_ = *pColor;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) final {
    if (pColor == nullptr)
      return E_INVALIDARG;
    *pColor = background_color_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) final {
    return Rotation == DXGI_MODE_ROTATION_IDENTITY ? S_OK : DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) final {
    if (pRotation == nullptr)
      return E_INVALIDARG;
    *pRotation = DXGI_MODE_ROTATION_IDENTITY;
    return S_OK;
  }

  /* Kept and reported back; the whole back buffer is presented (vkd3d-proton
   * has no present region). */
  HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) final {
    DXGI_SWAP_CHAIN_DESC1 desc;
    chain_->GetDesc(&desc);
    if (!Width || !Height || Width > desc.Width || Height > desc.Height)
      return DXGI_ERROR_INVALID_CALL;
    source_width_ = Width;
    source_height_ = Height;
    if (Width != desc.Width || Height != desc.Height)
      LogOnce(g_logged_source_size,
              "SetSourceSize smaller than the buffers, the whole buffer is presented");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) final {
    if (pWidth == nullptr && pHeight == nullptr)
      return E_INVALIDARG;
    DXGI_SWAP_CHAIN_DESC1 desc;
    chain_->GetDesc(&desc);
    if (pWidth)
      *pWidth = source_width_ ? std::min(source_width_, desc.Width) : desc.Width;
    if (pHeight)
      *pHeight = source_height_ ? std::min(source_height_, desc.Height) : desc.Height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) final {
    if (!(creation_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return chain_->SetFrameLatency(MaxLatency);
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) final {
    if (pMaxLatency == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    *pMaxLatency = chain_->GetFrameLatency();
    return S_OK;
  }

  /* vkd3d-proton hands out a handle of its own for each call, as DXGI does. */
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() final {
    if (!(creation_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return nullptr;
    return chain_->GetFrameLatencyEvent();
  }

  HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) final {
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) final {
    return DXGI_ERROR_INVALID_CALL;
  }

  /* Colour spaces and HDR metadata are vkd3d-proton's to judge: it answers
   * from the formats and colour spaces the Vulkan surface reports. */
  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                   UINT *pColorSpaceSupport) final {
    if (pColorSpaceSupport == nullptr)
      return E_INVALIDARG;
    *pColorSpaceSupport = chain_->CheckColorSpaceSupport(ColorSpace);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) final {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return chain_->SetColorSpace(ColorSpace);
  }

  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                           void *pMetaData) final {
    DXGI_VK_HDR_METADATA metadata = {};
    metadata.Type = Type;
    switch (Type) {
    case DXGI_HDR_METADATA_TYPE_NONE:
      break;
    case DXGI_HDR_METADATA_TYPE_HDR10:
      if (pMetaData == nullptr || Size != sizeof(DXGI_HDR_METADATA_HDR10))
        return E_INVALIDARG;
      metadata.HDR10 = *static_cast<const DXGI_HDR_METADATA_HDR10 *>(pMetaData);
      break;
    default:
      return DXGI_ERROR_UNSUPPORTED;
    }
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return chain_->SetHDRMetaData(&metadata);
  }

private:
  Com<IDXGIFactory1> factory_;
  Com<IUnknown> device_;
  Com<IDXGIVkSwapChain> chain_;
  Com<IDXGIVkSwapChain1> chain1_;
  Com<IDXGIVkSurfaceFactory> surface_factory_;
  Com<IDXGIOutput> restrict_to_output_;
  HWND hwnd_;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_;
  UINT creation_flags_ = 0;
  std::atomic<UINT> present_count_{0};
  DXGI_RGBA background_color_ = {};
  UINT source_width_ = 0;
  UINT source_height_ = 0;
  wsi::DXMTWindowState window_state_ = {};
  dxmt::mutex mutex_;
};

} // namespace

/* ml1320: MADEIRA_DXGI_VK_SWAPCHAIN=0 turns off DXGI swapchains for Direct3D 12
 * on Vulkan (vkd3d-proton's IDXGIVkSwapChainFactory); CreateSwapChain then
 * fails with "Unsupported device type" as before. On by default. */
bool VkSwapChainEnabled() {
  static const bool enabled = env::getEnvVar("MADEIRA_DXGI_VK_SWAPCHAIN") != "0";
  return enabled;
}

HRESULT CreateVkSwapChain(IDXGIFactory1 *pFactory, IUnknown *pDevice,
                          IDXGIVkSwapChainFactory *pVkFactory, HWND hWnd,
                          const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
                          IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
  InitReturnPtr(ppSwapChain);

  Com<MTLDXGIVkSurfaceFactory> surface_factory =
      Com<MTLDXGIVkSurfaceFactory>::transfer(new MTLDXGIVkSurfaceFactory(hWnd));

  IDXGIVkSwapChain *raw_chain = nullptr;
  HRESULT hr = pVkFactory->CreateSwapChain(surface_factory.ptr(), pDesc, &raw_chain);
  if (FAILED(hr) || raw_chain == nullptr) {
    ERR("[dxgi] ml1320 swapchain for a Vulkan device (IDXGIVkSwapChainFactory) failed: hr 0x",
        std::hex, (uint32_t)hr, std::dec, ", ", pDesc->Width, "x", pDesc->Height, " format ",
        pDesc->Format, " buffers ", pDesc->BufferCount, ", loader ",
        surface_factory->LoaderName());
    return FAILED(hr) ? hr : E_FAIL;
  }
  Com<IDXGIVkSwapChain> chain = Com<IDXGIVkSwapChain>::transfer(raw_chain);

  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = *pFullscreenDesc;
  if (!fs_desc.Windowed)
    LogOnce(g_logged_fullscreen, "created fullscreen, the swapchain stays windowed");

  WARN("[dxgi] ml1320 swapchain for a Vulkan device (IDXGIVkSwapChainFactory) ", pDesc->Width,
       "x", pDesc->Height, " format ", pDesc->Format, " buffers ", pDesc->BufferCount,
       " swap effect ", pDesc->SwapEffect, " flags 0x", std::hex, pDesc->Flags, std::dec,
       " hwnd ", (void *)hWnd, ", loader ", surface_factory->LoaderName(),
       " (MADEIRA_DXGI_VK_SWAPCHAIN=0 turns this off)");

  *ppSwapChain = new MTLDXGIVkSwapChain(pFactory, pDevice, chain.ptr(), surface_factory.ptr(),
                                        hWnd, fs_desc, pRestrictToOutput);
  return S_OK;
}

} // namespace dxmt
