#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_options.hpp"
#include "util_string.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3d10_1.h"
#include "Metal.hpp"
#include <cstdio>     /* ml1007 */
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dxmt {

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

LUID GetAdapterLuid(WMT::Device device) {
    // NOTE: use big-endian registryID, be consistent with MVK
  return std::bit_cast<LUID>(__builtin_bswap64(device.registryID()));
}

class MTLDXGIAdatper : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdatper(WMT::Device device, IDXGIFactory *factory, Config &config)
      : device_(device), factory_(factory), options_(config) {
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = GetAdapterLuid(device_);
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdatper() {
    if (local_kmt_) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = local_kmt_;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT adapter");
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
        riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
        riid == __uuidof(IDXGIAdapter4) || riid == __uuidof(IMTLDXGIAdapter)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIAdapter), riid)) {
      WARN("DXGIAdapter: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    return factory_->QueryInterface(riid, ppParent);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
    }

    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
      pDesc->GraphicsPreemptionGranularity = desc.GraphicsPreemptionGranularity;
      pDesc->ComputePreemptionGranularity = desc.ComputePreemptionGranularity;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    std::memset(pDesc->Description, 0, sizeof(pDesc->Description));

    if (!options_.customDeviceDesc.empty()) {
      str::transcodeString(
          pDesc->Description,
          sizeof(pDesc->Description) / sizeof(pDesc->Description[0]) - 1,
          options_.customDeviceDesc.c_str(), options_.customDeviceDesc.size());
    } else {
      device_.name().getCString((char *)pDesc->Description, sizeof(pDesc->Description), WMTUTF16StringEncoding);
    }

    if (options_.customVendorId >= 0) {
      pDesc->VendorId = options_.customVendorId;
    } else {
      pDesc->VendorId = 0x106B;
      if (g_extension_enabled == VendorExtension::Nvidia) {
        pDesc->VendorId = 0x10DE;
      }
    }

    if (options_.customDeviceId >= 0) {
      pDesc->DeviceId = options_.customDeviceId;
    } else {
      pDesc->DeviceId = 0;
    }

    pDesc->SubSysId = 0;
    pDesc->Revision = 0;
    // ml1042: the unix side now returns an honest per-process video budget (see
    // winemetal_unix.c), so report it whole. The old "/2 on unified memory" was a
    // guess compensating for a number that was far too large; halving a correct
    // number would just starve the application of half its real budget.
    pDesc->DedicatedVideoMemory = device_.recommendedMaxWorkingSetSize();
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = 0;
    pDesc->AdapterLuid = GetAdapterLuid(device_);
    pDesc->Flags = DXGI_ADAPTER_FLAG3_NONE;
    pDesc->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    pDesc->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output,
                                        IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr)
      return E_INVALIDARG;

    HMONITOR monitor = wsi::enumMonitors(Output);
    if (monitor == nullptr)
      return DXGI_ERROR_NOT_FOUND;

    *ppOutput = CreateOutput(this, monitor, options_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CheckInterfaceSupport(const GUID &guid, LARGE_INTEGER *umd_version) final {
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;

    if (guid == __uuidof(IDXGIDevice) || guid == __uuidof(ID3D10Device) ||
        guid == __uuidof(ID3D10Device1))
      hr = S_OK;

    // We can't really reconstruct the version numbers
    // returned by Windows drivers from Metal; report the one
    // the kernel-mode side has for this adapter, if any
    if (SUCCEEDED(hr) && umd_version)
      umd_version->QuadPart = GetUmdDriverVersion();

    if (FAILED(hr)) {
      Logger::err("DXGI: CheckInterfaceSupport: Unsupported interface");
      Logger::err(str::format(guid));
    }

    return hr;
  }

  /* ml1007: the same defect as the budget-notification pair below -- both were
   * `assert(0 && "TODO")` with no return, compiling to a single `brk #1`. The
   * compiler says so plainly ("non-void function does not return a value"), and
   * rdr76 died on the sibling method, so this one is the same landmine one
   * vtable slot away.
   *
   * Unlike the budget notification, this is genuinely UNAVAILABLE rather than
   * merely static: there is no hardware content-protection path on this
   * platform, so there is no teardown to be notified about. DXGI_ERROR_UNSUPPORTED
   * is the truthful answer -- returning S_OK would promise a notification that
   * could never arrive and would mislead a caller that depends on it. */
  HRESULT STDMETHODCALLTYPE
  RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event,
                                                       DWORD *cookie) override {
    (void)event;
    if (!cookie)
      return E_INVALIDARG;
    *cookie = 0;
    Logger::warn("DXGI: RegisterHardwareContentProtectionTeardownStatusEvent -- "
                 "no hardware content protection on this platform, returning "
                 "DXGI_ERROR_UNSUPPORTED");
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override {
    /* Nothing was ever registered; the method returns void and cannot report. */
    Logger::warn(str::format("DXGI: UnregisterHardwareContentProtectionTeardownStatus "
                             "cookie=", cookie, " -- nothing was registered, ignored"));
  }

  HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) override {
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    // we don't actually care about MemorySegmentGroup
    pVideoMemoryInfo->Budget = device_.recommendedMaxWorkingSetSize();
    pVideoMemoryInfo->CurrentUsage = device_.currentAllocatedSize();
    pVideoMemoryInfo->AvailableForReservation = 0;
    pVideoMemoryInfo->CurrentReservation =
        mem_reserved_[uint32_t(MemorySegmentGroup)];
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      UINT64 Reservation) override {
    if (NodeIndex > 0)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    mem_reserved_[uint32_t(MemorySegmentGroup)] = Reservation;
    return S_OK;
  }

  /* ml1007: implement budget-change notification registration.
   *
   * These two were `assert(0 && "TODO")`. The Register variant additionally had
   * no return statement at all, so even with NDEBUG it falls off the end of a
   * non-void function. Compiled, the body is a single `brk #1`, and rdr76
   * traps there: RDR2 reaches DXGI adapter setup, calls this method through the
   * IDXGIAdapter3 vtable, and the process takes STATUS_ILLEGAL_INSTRUCTION
   * (c000001d) which nothing handles.
   *
   * Contract: register `event` to be signalled when the video-memory budget
   * changes, hand back a cookie that Unregister accepts. Registration is done
   * for real -- a unique cookie, the handle retained, and Unregister actually
   * removing it -- so the bookkeeping the caller observes is truthful.
   *
   * We do NOT fabricate notifications. QueryVideoMemoryInfo reports Budget from
   * the Metal device's recommended working-set size, which does not change
   * during a run on this platform, so no budget-change event is due and an
   * event that never fires is the accurate outcome rather than a missing
   * feature. If Budget ever becomes dynamic, signal the registered handles at
   * the point it changes -- that is the only correct place for it.
   *
   * ⛔ MEASURED, rdr77 vs rdr78 -- a clean single-variable A/B on the same
   * build, flipping only the file below:
   *   S_OK + event never fires  -> RDR2 HUNG on an auto-reset Event, infinite
   *                                wait, 8 minutes, no window
   *   DXGI_ERROR_UNSUPPORTED    -> swapchain created, 240 GPU flush cycles, the
   *                                game window appeared
   * So the caller really does wait on this event, and "register successfully and
   * never signal" is NOT an acceptable reading of a static budget -- Astra's
   * warning against returning fake success was correct and my first
   * implementation was wrong. DEFAULT IS THEREFORE THE HONEST FAILURE.
   *
   * Documents/madeira-dxgi-budget.txt = 1 re-enables real registration, for
   * when Budget becomes dynamic and we can actually signal the handles. Do not
   * enable it before there is a signalling path. */
  HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
      HANDLE event, DWORD *cookie) override {
    if (!cookie)
      return E_INVALIDARG;

    if (!budget_notify_enabled()) {
      Logger::warn("DXGI: RegisterVideoMemoryBudgetChangeNotificationEvent "
                   "disabled by madeira-dxgi-budget.txt -> DXGI_ERROR_UNSUPPORTED");
      return DXGI_ERROR_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> lock(budget_mutex_);
    DWORD assigned = ++budget_cookie_seq_;
    budget_events_.emplace_back(assigned, event);
    *cookie = assigned;
    Logger::warn(str::format("DXGI: registered video-memory budget notification "
                             "cookie=", assigned, " (budget is static on this "
                             "platform, so no notification is expected)"));
    return S_OK;
  }

  void STDMETHODCALLTYPE
  UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override {
    std::lock_guard<std::mutex> lock(budget_mutex_);
    for (auto it = budget_events_.begin(); it != budget_events_.end(); ++it) {
      if (it->first == cookie) {
        budget_events_.erase(it);
        Logger::warn(str::format("DXGI: unregistered video-memory budget "
                                 "notification cookie=", cookie));
        return;
      }
    }
    /* Windows ignores an unknown cookie here (the method returns void and
     * cannot report). Say so rather than silently doing nothing. */
    Logger::warn(str::format("DXGI: UnregisterVideoMemoryBudgetChangeNotification "
                             "for unknown cookie=", cookie, " -- ignored"));
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() final { return device_; }
  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  /* ml1007: read once. Absent file (the normal case) leaves this enabled. */
  static bool budget_notify_enabled() {
    /* Default FALSE: see the A/B in the comment above. Only an explicit "1"
     * turns real registration back on. */
    static const bool enabled = [] {
      const char *docs = std::getenv("MADEIRA_DOCS_DIR");
      if (!docs || !*docs)
        return false;
      std::string path = std::string(docs) + "/madeira-dxgi-budget.txt";
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f)
        return false;
      int c = std::fgetc(f);
      std::fclose(f);
      return c == '1';
    }();
    return enabled;
  }

  /* What Windows' DXGI hands out: the UMD version the kernel reports for the
   * adapter (KMTQAITYPE_UMD_DRIVER_VERSION, the registry DriverVersion
   * a.b.c.d as a<<48 | b<<32 | c<<16 | d). ~0 when there is none. */
  UINT64 GetUmdDriverVersion() {
    D3DKMT_UMD_DRIVER_VERSION version = {};
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = local_kmt_;
    query.Type = KMTQAITYPE_UMD_DRIVER_VERSION;
    query.pPrivateDriverData = &version;
    query.PrivateDriverDataSize = sizeof(version);
    if (!local_kmt_ || D3DKMTQueryAdapterInfo(&query) || !version.DriverVersion.QuadPart)
      return ~0ull;
    return version.DriverVersion.QuadPart;
  }

  WMT::Reference<WMT::Device> device_;
  D3DKMT_HANDLE local_kmt_ = 0;
  Com<IDXGIFactory> factory_;
  DxgiOptions options_;
  uint64_t mem_reserved_[2] = {0, 0};
  std::mutex budget_mutex_;
  std::vector<std::pair<DWORD, HANDLE>> budget_events_;
  DWORD budget_cookie_seq_ = 0;
};

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config) {
  return Com<IMTLDXGIAdapter>::transfer(
      new MTLDXGIAdatper(Device, pFactory, config));
}

} // namespace dxmt