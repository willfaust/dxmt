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
#include <atomic> /* madeira-bcd: budget events */
#include <string>
#include <utility>
#include <vector>

namespace dxmt {

const GUID kWineDXGIAdapterUUID = {0x17399d75,
                                   0x964e,
                                   0x4c03,
                                   {0x99, 0xf8, 0x9d, 0x4f, 0xd1, 0x96, 0xdd, 0x62}};

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

/* madeira-bcd: video-memory budget events (tools/patch-dxgi-budget-events.py),
 * opt-in with env.MADEIRA_DXGI_BUDGET_EVENTS = 1. The budget below already
 * shrinks near the app's memory limit (winemetal ml1075); these events make a
 * game that waits for them (Red Dead Redemption 2) ask for it again. */
struct MadeiraBudgetEvent {
  DWORD cookie;
  HANDLE handle; /* our duplicate of the caller's event, or the caller's own */
  bool own;
};

struct MadeiraBudgetEvents {
  std::mutex lock;
  std::vector<MadeiraBudgetEvent> events;
  DWORD next_cookie = 0x4d420000; /* apart from the per-adapter cookies */
  obj_handle_t device = 0;        /* retained once, never released */
  uint64_t signalled = 0;         /* the budget the events last announced */
  unsigned signals = 0;
  bool watching = false;
  std::atomic<int> unanswered{0}; /* signals not yet followed by a query */
};

static MadeiraBudgetEvents *madeira_budget() {
  /* Never freed: the watcher thread outlives every adapter object. */
  static MadeiraBudgetEvents *state = new MadeiraBudgetEvents;
  return state;
}

static bool madeira_budget_events_switch() {
  static const bool on = [] {
    char v[8] = {};
    DWORD n = GetEnvironmentVariableA("MADEIRA_DXGI_BUDGET_EVENTS", v, sizeof(v));
    if (n == 0 || n >= sizeof(v))
      return false;
    return !strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") ||
           !_stricmp(v, "yes");
  }();
  return on;
}

static DWORD WINAPI madeira_budget_watch(void *) {
  MadeiraBudgetEvents *s = madeira_budget();
  unsigned quiet = 0, retries = 0;
  for (;;) {
    Sleep(500);
    WMT::Device device;
    device.handle = s->device;
    uint64_t budget = device.recommendedMaxWorkingSetSize();
    std::lock_guard<std::mutex> guard(s->lock);
    if (s->events.empty() || !budget)
      continue;
    uint64_t moved = budget > s->signalled ? budget - s->signalled : s->signalled - budget;
    /* A signal nobody answered with a query within 2 s is sent again (at most
     * 30 times in a row): a game that reset its event before waiting on it
     * would otherwise wait forever, as RDR2 did (rdr77). */
    bool again = false;
    if (s->unanswered.load() == 0)
      quiet = retries = 0;
    else if (++quiet >= 4 && retries < 30)
      again = true;
    if (moved < (32ull << 20) && !again)
      continue;
    quiet = 0;
    retries = moved < (32ull << 20) ? retries + 1 : 0;
    for (auto &e : s->events)
      SetEvent(e.handle);
    unsigned n = ++s->signals;
    s->unanswered.fetch_add(1);
    if (n <= 64 || n % 50 == 0)
      Logger::info(str::format("[budget-event] budget ", budget >> 20, " MB (was ",
                               s->signalled >> 20, " MB), usage ",
                               device.currentAllocatedSize() >> 20, " MB: signalled ",
                               s->events.size(), " event(s), signal #", n,
                               moved < (32ull << 20) ? " (again: no query since the last one)" : ""));
    s->signalled = budget;
  }
  return 0;
}

static HRESULT madeira_budget_register(obj_handle_t device_handle, HANDLE event,
                                       DWORD *cookie) {
  MadeiraBudgetEvents *s = madeira_budget();
  if (!event)
    return E_INVALIDARG;
  HANDLE dup = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &dup, 0,
                       FALSE, DUPLICATE_SAME_ACCESS))
    dup = nullptr;
  WMT::Device device;
  device.handle = device_handle;
  uint64_t budget = device.recommendedMaxWorkingSetSize();
  bool start = false;
  {
    std::lock_guard<std::mutex> guard(s->lock);
    if (!s->device) {
      device.retain();
      s->device = device_handle;
    }
    DWORD assigned = ++s->next_cookie;
    s->events.push_back({assigned, dup ? dup : event, dup != nullptr});
    *cookie = assigned;
    if (!s->signalled)
      s->signalled = budget;
    start = !s->watching;
    s->watching = true;
    /* Windows signals a new registration: the caller may wait for it. */
    SetEvent(dup ? dup : event);
    s->unanswered.fetch_add(1);
    Logger::info(str::format("[budget-event] registered cookie=", assigned,
                             " (MADEIRA_DXGI_BUDGET_EVENTS=1", dup ? "" : ", caller's handle: duplicate failed",
                             "): signalled now; budget ", budget >> 20, " MB, usage ",
                             device.currentAllocatedSize() >> 20,
                             " MB; signalled again when the budget moves by 32 MB"));
  }
  if (start) {
    HMODULE self = nullptr;
    /* The watcher runs this DLL's code for good: keep it loaded. */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)&madeira_budget_watch, &self);
    HANDLE thread = CreateThread(nullptr, 256 * 1024, madeira_budget_watch, nullptr,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread)
      CloseHandle(thread);
    else
      Logger::warn(str::format("[budget-event] the watcher thread did not start (error ",
                               GetLastError(), "): only the first signal is sent"));
  }
  return S_OK;
}

static bool madeira_budget_unregister(DWORD cookie) {
  if (!madeira_budget_events_switch())
    return false;
  MadeiraBudgetEvents *s = madeira_budget();
  std::lock_guard<std::mutex> guard(s->lock);
  for (auto it = s->events.begin(); it != s->events.end(); ++it) {
    if (it->cookie != cookie)
      continue;
    if (it->own)
      CloseHandle(it->handle);
    s->events.erase(it);
    Logger::info(str::format("[budget-event] unregistered cookie=", cookie, ", ",
                             s->events.size(), " left"));
    return true;
  }
  return false;
}

static void madeira_budget_queried(uint64_t budget, uint64_t usage) {
  if (!madeira_budget_events_switch())
    return;
  int pending = madeira_budget()->unanswered.exchange(0);
  if (!pending)
    return;
  static std::atomic<int> said{0};
  int n = said.fetch_add(1);
  if (n < 64 || n % 50 == 0)
    Logger::info(str::format("[budget-event] the game asked after ", pending,
                             " signal(s): budget ", budget >> 20, " MB, usage ",
                             usage >> 20, " MB"));
}

LUID GetAdapterLuid(WMT::Device device) {
    // NOTE: use big-endian registryID, be consistent with MVK
  return std::bit_cast<LUID>(__builtin_bswap64(device.registryID()));
}

class MTLDXGIAdapter : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdapter(WMT::Device device, IDXGIFactory *factory, Config &config)
      : device_(device), factory_(factory), options_(config) {
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = GetAdapterLuid(device_);
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdapter() {
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

    if (riid == kWineDXGIAdapterUUID)
      return E_NOINTERFACE;

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
    madeira_budget_queried(pVideoMemoryInfo->Budget, pVideoMemoryInfo->CurrentUsage);
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

    if (madeira_budget_events_switch())
      return madeira_budget_register(device_.handle, event, cookie);

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
    if (madeira_budget_unregister(cookie))
      return;
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
      new MTLDXGIAdapter(Device, pFactory, config));
}

} // namespace dxmt