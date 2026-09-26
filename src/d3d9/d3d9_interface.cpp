#include "d3d9_interface.hpp"

#include "Metal.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "dxmt_format.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "config/config.hpp"
#include "wsi_monitor.hpp"
#ifdef DXMT_MADEIRA
#include "wsi_window.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "d3d9_census.hpp"

namespace dxmt {

// Pin Metal's own shader and pipeline cache to the per-executable
// directory the disk shader cache uses (the d3d11 backend does the
// same at provider init). Without it the OS cache keys off the wine
// loader's identity and pipeline links never persist across runs.
static void
initializeMetalCachePath() {
  static std::once_flag once;
  std::call_once(once, []() {
    if (env::getEnvVar("DXMT_USE_DEFAULT_METAL_CACHE") == "1")
      return;
    auto path = GetDXMTShaderCacheDirectory() + "com.apple.metal";
    if (path.size() >= 2 && path[1] == ':')
      path = env::getUnixPath(path);
    if (!WMTSetMetalShaderCachePath(path.c_str()))
      Logger::info("Failed to set Metal cache path, fallback to system default");
  });
}

MTLD3D9Interface::MTLD3D9Interface(UINT SDKVersion, bool isEx) :
    m_sdkVersion(SDKVersion),
    m_isEx(isEx),
    m_adapters(WMT::CopyAllDevices()),
    m_adapterCount(m_adapters ? static_cast<UINT>(m_adapters.count()) : 0u) {
  initializeMetalCachePath();
}

// wsi reports refresh as a rational (numerator/denominator). Either
// can be zero on a display where the OS doesn't know the real rate
// (sleep/wake transitions, headless monitors). Match d3d11_swapchain
// and d3d9_swapchain: only divide when both are non-zero; otherwise
// fall back to 60 so apps that gate animation on the rate don't
// start dividing by zero or scheduling at 0 Hz.
static UINT
refreshRateHzOr60(const wsi::WsiMode &wm) {
  if (wm.refreshRate.denominator == 0 || wm.refreshRate.numerator == 0)
    return 60;
  return wm.refreshRate.numerator / wm.refreshRate.denominator;
}

// The adapter's D3D9 mode list: the display's own modes, deduplicated and
// ordered. Backs GetAdapterModeCount / EnumAdapterModes and their Ex variants.
//
// Every entry here must be a mode the platform can actually SET, because that is
// the contract apps rely on: they pick an extent out of this list and hand it to
// ChangeDisplaySettings. Nothing may be added that the display does not really
// offer, however convenient it would be for the create path. wined3d builds the
// equivalent list in directx.c.
static std::vector<wsi::WsiMode>
adapterModes(UINT adapter) {
  std::vector<wsi::WsiMode> modes;
  HMONITOR mon = wsi::enumMonitors(adapter);
  if (!mon)
    return modes;

  // The platform list repeats each extent once per colour depth. D3D9 enumerates
  // one entry per extent per display FORMAT (see isEnumerableDisplayFormat), so
  // without this the same resolution surfaces several times in an app's
  // resolution menu.
  wsi::WsiMode wm{};
  for (UINT i = 0; wsi::getDisplayMode(mon, i, &wm); ++i) {
    const UINT hz = refreshRateHzOr60(wm);
    const bool known = std::any_of(modes.begin(), modes.end(), [&](const wsi::WsiMode &m) {
      return m.width == wm.width && m.height == wm.height && refreshRateHzOr60(m) == hz;
    });
    if (!known)
      modes.push_back(wm);
  }

  std::sort(modes.begin(), modes.end(), [](const wsi::WsiMode &a, const wsi::WsiMode &b) {
    return std::tie(a.width, a.height) < std::tie(b.width, b.height);
  });
  return modes;
}

// The display formats EnumAdapterModes reports modes for.
//
// Was X8R8G8B8 alone, on the reasoning that Apple panels have no 16-bit modes.
// That is true of a real panel and false of this one: the display behind
// EnumAdapterModes here is the win32u virtual monitor, whose
// ChangeDisplaySettings honours DM_PELSWIDTH / DM_PELSHEIGHT and ignores
// dmBitsPerPel entirely (build/win32u-unix/sysparams_ios.c,
// ios_virtual_change_display_settings), and the backbuffer is composited into a
// Metal layer whose own format never depended on the app's display format. So
// every extent really is settable at every one of these formats, which is the
// contract adapterModes() documents above.
//
// It matters because CheckDeviceType gates a FULLSCREEN device on
// GetAdapterModeCount(DisplayFormat) != 0: an application of the D3D8 era that
// offers 16-bit colour, or that probes R5G6B5 first and only falls back to
// 32-bit if that fails, was told the adapter has no modes at all. The set is
// the four D3D9 display formats (wined3d directx.c; DXVK d3d9_adapter.cpp maps
// each to a monitor bpp and enumerates both depths the same way).
static bool
isEnumerableDisplayFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A2R10G10B10:
    return true;
  default:
    return false;
  }
}

// Accepted for a fullscreen present even when the display does not enumerate
// them. There is no display-mode switch behind a fullscreen present here: the
// backbuffer is composited into a layer and scaled to the drawable, which is
// what a fixed-panel display does when asked for a lower mode anyway. The
// platform enumerates only the panel's own scaled set, whose smallest entry can
// sit above the standard VGA extents entirely, so gating on that list alone
// would reject sizes an application is entitled to ask for.
//
// Deliberately NOT added to adapterModes: enumeration promises settable modes,
// and these are not settable here. The two lists differ on purpose.
static const struct {
  UINT width;
  UINT height;
} kPresentableExtents[] = {
    {320, 240},  {512, 384},  {640, 400},  {640, 480},  {720, 480},   {800, 600},
    {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960}, {1280, 1024},
};

// Whether a fullscreen present may name this extent. The refresh rate is matched
// only when the app pins a nonzero FullScreen_RefreshRateInHz, and only against a
// real mode: a scaled extent carries no refresh rate of its own.
static bool
isPresentableFullscreenExtent(UINT adapter, UINT width, UINT height, UINT refreshRate) {
  for (const auto &m : adapterModes(adapter)) {
    if (m.width != width || m.height != height)
      continue;
    if (refreshRate != 0 && refreshRateHzOr60(m) != refreshRate)
      continue;
    return true;
  }
  return std::any_of(std::begin(kPresentableExtents), std::end(kPresentableExtents), [&](const auto &e) {
    return e.width == width && e.height == height;
  });
}

// MADEIRA: the [d3d9-modes] trace (declared in d3d9_interface.hpp).
//
// A failed renderer init used to leave nothing in the log but the application's
// own error box: what it asked the adapter for, what the adapter offered, and
// which of the two did not match were all invisible. These two calls make the
// next log answer that without a rebuild.
static const char *
d3dFormatName(D3DFORMAT f) {
  switch (f) {
  case D3DFMT_UNKNOWN:      return "UNKNOWN";
  case D3DFMT_X8R8G8B8:     return "X8R8G8B8";
  case D3DFMT_A8R8G8B8:     return "A8R8G8B8";
  case D3DFMT_R5G6B5:       return "R5G6B5";
  case D3DFMT_X1R5G5B5:     return "X1R5G5B5";
  case D3DFMT_A1R5G5B5:     return "A1R5G5B5";
  case D3DFMT_A2R10G10B10:  return "A2R10G10B10";
  default:                  return "other";
  }
}

void
LogAdapterModesOnce(UINT adapter) {
  static std::once_flag once;
  std::call_once(once, [adapter]() {
    const auto modes = adapterModes(adapter);

    wsi::WsiMode cur{};
    HMONITOR mon = wsi::enumMonitors(adapter);
    const bool haveCur = mon && wsi::getCurrentDisplayMode(mon, &cur);

    std::string line = str::format("[d3d9-modes] adapter ", adapter, " count=", modes.size(), " current=");
    if (haveCur)
      line += str::format(cur.width, "x", cur.height, "@", refreshRateHzOr60(cur));
    else
      line += "unknown";

    // The first eight are enough to tell a real table from a synthesized stub,
    // and short enough not to flood a log that is already large.
    const size_t shown = modes.size() < 8 ? modes.size() : 8;
    for (size_t i = 0; i < shown; i++)
      line += str::format(" | ", modes[i].width, "x", modes[i].height, "@", refreshRateHzOr60(modes[i]));
    if (modes.size() > shown)
      line += str::format(" | +", modes.size() - shown, " more");

    Logger::info(line);
  });
}

// MADEIRA: the [d3d9-caps] trace.
//
// [d3d9-modes] answered "what extent did it ask for". It cannot answer the
// question a title of the 2000-2010 era actually fails on, which is "which
// capability did it probe, and what did we say". Those titles gate whole
// renderers on a CheckDeviceFormat or a D3DCAPS9 field and then take a silent
// fallback -- an error box we do not render, an intro that never ends, or a
// null they dereference three frames later. A log that shows every distinct
// probe and its HRESULT turns that from a bisect into a read.
//
// Two properties make it affordable to leave on:
//
//  - ONCE PER DISTINCT QUERY. Frame 1 of a real title issues thousands of
//    these (the census measured 2016 EnumAdapterModes and 144 CheckDeviceType
//    calls in a single frame), and the same tuple repeats constantly. The
//    dedup set below is 1024 open-addressed slots of the query's hash; a
//    repeat costs one atomic load and nothing else.
//  - CAPPED AT 256 LINES. A title that probes a genuinely unbounded set (a
//    format loop over all 2^32 FOURCCs, say) cannot turn the log into this
//    trace. The 257th line says the cap was hit and nothing is printed after.
namespace {

constexpr unsigned kCapsLineCap = 256;
constexpr unsigned kCapsSeenSlots = 1024;

std::atomic<unsigned> g_capsLines;
std::atomic<uint64_t> g_capsSeen[kCapsSeenSlots];

// True the first time this exact query is seen. Open addressing with a CAS on
// an empty slot: two threads racing on the same key can both win at most once
// each, which is a duplicate line, not a correctness problem. A full table
// stops deduplicating and lets the 256-line cap do the bounding.
bool
capsFirstTime(uint64_t key) {
  if (!key)
    key = 1; // 0 is the empty marker
  uint64_t h = key * 0x9e3779b97f4a7c15ull;
  for (unsigned probe = 0; probe < 16; probe++) {
    unsigned i = (unsigned)((h >> 32) + probe) & (kCapsSeenSlots - 1);
    uint64_t cur = g_capsSeen[i].load(std::memory_order_relaxed);
    if (cur == key)
      return false;
    if (cur == 0) {
      uint64_t expected = 0;
      if (g_capsSeen[i].compare_exchange_strong(expected, key, std::memory_order_relaxed))
        return true;
      if (expected == key)
        return false;
    }
  }
  return true;
}

// The dedup key. Built by mixing rather than by packing bit ranges: the
// fields a probe carries do not fit in 64 bits side by side (Usage alone is
// 32 bits and a FOURCC format is another 32), and overlapping shifted XORs
// silently alias one query onto another, which shows up as a line the trace
// never prints. A 64-bit FNV-1a over the fields has no such quiet failure --
// a collision is possible but random and rare, rather than systematic.
uint64_t
capsKey(unsigned which, uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0) {
  uint64_t h = 1469598103934665603ull;
  const uint64_t parts[6] = {which, a, b, c, d, e};
  for (uint64_t p : parts) {
    for (int i = 0; i < 8; i++) {
      h ^= (p >> (i * 8)) & 0xff;
      h *= 1099511628211ull;
    }
  }
  return h;
}

void capsLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void
capsLine(const char *fmt, ...) {
  unsigned n = g_capsLines.fetch_add(1, std::memory_order_relaxed);
  if (n > kCapsLineCap)
    return;
  if (n == kCapsLineCap) {
    Logger::info(
        str::format("[d3d9-caps] ... capped at ", kCapsLineCap, " lines; further distinct queries are not printed")
    );
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Logger::info(std::string("[d3d9-caps] ") + buf);
}

// Every D3DFORMAT a title of this era names, plus a numeric/FOURCC fallback so
// a vendor format nobody here has heard of is still identifiable in the log.
// Returned by value: this is a diagnostic path, never a hot one.
std::string
fmtStr(D3DFORMAT f) {
  switch (f) {
  case D3DFMT_UNKNOWN:       return "UNKNOWN";
  case D3DFMT_R8G8B8:        return "R8G8B8";
  case D3DFMT_A8R8G8B8:      return "A8R8G8B8";
  case D3DFMT_X8R8G8B8:      return "X8R8G8B8";
  case D3DFMT_R5G6B5:        return "R5G6B5";
  case D3DFMT_X1R5G5B5:      return "X1R5G5B5";
  case D3DFMT_A1R5G5B5:      return "A1R5G5B5";
  case D3DFMT_A4R4G4B4:      return "A4R4G4B4";
  case D3DFMT_R3G3B2:        return "R3G3B2";
  case D3DFMT_A8:            return "A8";
  case D3DFMT_A8R3G3B2:      return "A8R3G3B2";
  case D3DFMT_X4R4G4B4:      return "X4R4G4B4";
  case D3DFMT_A2B10G10R10:   return "A2B10G10R10";
  case D3DFMT_A8B8G8R8:      return "A8B8G8R8";
  case D3DFMT_X8B8G8R8:      return "X8B8G8R8";
  case D3DFMT_G16R16:        return "G16R16";
  case D3DFMT_A2R10G10B10:   return "A2R10G10B10";
  case D3DFMT_A16B16G16R16:  return "A16B16G16R16";
  case D3DFMT_A8P8:          return "A8P8";
  case D3DFMT_P8:            return "P8";
  case D3DFMT_L8:            return "L8";
  case D3DFMT_A8L8:          return "A8L8";
  case D3DFMT_A4L4:          return "A4L4";
  case D3DFMT_V8U8:          return "V8U8";
  case D3DFMT_L6V5U5:        return "L6V5U5";
  case D3DFMT_X8L8V8U8:      return "X8L8V8U8";
  case D3DFMT_Q8W8V8U8:      return "Q8W8V8U8";
  case D3DFMT_V16U16:        return "V16U16";
  case D3DFMT_A2W10V10U10:   return "A2W10V10U10";
  case D3DFMT_D16_LOCKABLE:  return "D16_LOCKABLE";
  case D3DFMT_D32:           return "D32";
  case D3DFMT_D15S1:         return "D15S1";
  case D3DFMT_D24S8:         return "D24S8";
  case D3DFMT_D24X8:         return "D24X8";
  case D3DFMT_D24X4S4:       return "D24X4S4";
  case D3DFMT_D16:           return "D16";
  case D3DFMT_D32F_LOCKABLE: return "D32F_LOCKABLE";
  case D3DFMT_D24FS8:        return "D24FS8";
  case D3DFMT_L16:           return "L16";
  case D3DFMT_VERTEXDATA:    return "VERTEXDATA";
  case D3DFMT_INDEX16:       return "INDEX16";
  case D3DFMT_INDEX32:       return "INDEX32";
  case D3DFMT_Q16W16V16U16:  return "Q16W16V16U16";
  case D3DFMT_R16F:          return "R16F";
  case D3DFMT_G16R16F:       return "G16R16F";
  case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
  case D3DFMT_R32F:          return "R32F";
  case D3DFMT_G32R32F:       return "G32R32F";
  case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
  case D3DFMT_CxV8U8:        return "CxV8U8";
  default:
    break;
  }
  // FOURCC (DXT1..5, ATI1/2, INTZ, NULL, ...) reads as its four characters;
  // anything else as a number.
  const uint32_t v = (uint32_t)f;
  char buf[32];
  const char c0 = (char)(v & 0xff), c1 = (char)((v >> 8) & 0xff), c2 = (char)((v >> 16) & 0xff),
             c3 = (char)((v >> 24) & 0xff);
  auto printable = [](char c) { return c >= 0x20 && c < 0x7f; };
  if (printable(c0) && printable(c1) && printable(c2) && printable(c3)) {
    snprintf(buf, sizeof(buf), "'%c%c%c%c'", c0, c1, c2, c3);
    return buf;
  }
  snprintf(buf, sizeof(buf), "0x%x", v);
  return buf;
}

const char *
rtypeStr(D3DRESOURCETYPE t) {
  switch (t) {
  case D3DRTYPE_SURFACE:       return "SURFACE";
  case D3DRTYPE_VOLUME:        return "VOLUME";
  case D3DRTYPE_TEXTURE:       return "TEXTURE";
  case D3DRTYPE_VOLUMETEXTURE: return "VOLUMETEXTURE";
  case D3DRTYPE_CUBETEXTURE:   return "CUBETEXTURE";
  case D3DRTYPE_VERTEXBUFFER:  return "VERTEXBUFFER";
  case D3DRTYPE_INDEXBUFFER:   return "INDEXBUFFER";
  default:                     return "?";
  }
}

const char *
devtypeStr(D3DDEVTYPE t) {
  switch (t) {
  case D3DDEVTYPE_HAL:      return "HAL";
  case D3DDEVTYPE_REF:      return "REF";
  case D3DDEVTYPE_SW:       return "SW";
  case D3DDEVTYPE_NULLREF:  return "NULLREF";
  default:                  return "?";
  }
}

// The usage bits a CheckDeviceFormat probe can carry, spelled out: which one
// was refused is the whole answer when a title asks for the same format twice
// with different usage and only one probe fails.
std::string
usageStr(DWORD usage) {
  if (!usage)
    return "0";
  static const struct {
    DWORD bit;
    const char *name;
  } kBits[] = {
      {D3DUSAGE_RENDERTARGET, "RENDERTARGET"},
      {D3DUSAGE_DEPTHSTENCIL, "DEPTHSTENCIL"},
      {D3DUSAGE_DYNAMIC, "DYNAMIC"},
      {D3DUSAGE_AUTOGENMIPMAP, "AUTOGENMIPMAP"},
      {D3DUSAGE_DMAP, "DMAP"},
      {D3DUSAGE_QUERY_LEGACYBUMPMAP, "Q_LEGACYBUMPMAP"},
      {D3DUSAGE_QUERY_SRGBREAD, "Q_SRGBREAD"},
      {D3DUSAGE_QUERY_FILTER, "Q_FILTER"},
      {D3DUSAGE_QUERY_SRGBWRITE, "Q_SRGBWRITE"},
      {D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, "Q_POSTPSBLEND"},
      {D3DUSAGE_QUERY_VERTEXTEXTURE, "Q_VERTEXTEXTURE"},
      {D3DUSAGE_QUERY_WRAPANDMIP, "Q_WRAPANDMIP"},
      {D3DUSAGE_WRITEONLY, "WRITEONLY"},
      {D3DUSAGE_SOFTWAREPROCESSING, "SOFTWAREPROCESSING"},
      {D3DUSAGE_DONOTCLIP, "DONOTCLIP"},
      {D3DUSAGE_POINTS, "POINTS"},
      {D3DUSAGE_RTPATCHES, "RTPATCHES"},
      {D3DUSAGE_NPATCHES, "NPATCHES"},
  };
  std::string out;
  DWORD left = usage;
  for (const auto &b : kBits) {
    if (usage & b.bit) {
      if (!out.empty())
        out += "|";
      out += b.name;
      left &= ~b.bit;
    }
  }
  if (left) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)left);
    if (!out.empty())
      out += "|";
    out += buf;
  }
  return out;
}

} // namespace

void
LogPresentRequest(const char *what, const D3DPRESENT_PARAMETERS &p, HRESULT hr) {
  Logger::info(
      str::format(
          "[d3d9-modes] ", what, " ", p.BackBufferWidth, "x", p.BackBufferHeight, " ",
          d3dFormatName(p.BackBufferFormat), " refresh=", p.FullScreen_RefreshRateInHz,
          " windowed=", p.Windowed ? 1 : 0, " count=", p.BackBufferCount, " -> hr 0x", std::hex, (unsigned)hr
      )
  );
  /* MADEIRA [d3d9-last]: the one place in the frontend where a whole-device
   * request and its HRESULT are already in the same hand, so the ring gets
   * the answer and not just the question. `what` is a string literal at every
   * call site, which is what ringNote requires. */
  census::ringNote(what, hr, p.BackBufferWidth, p.BackBufferHeight);
}

MTLD3D9Interface::~MTLD3D9Interface() = default;

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::QueryInterface(REFIID riid, void **ppvObject) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_QueryInterface);
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
    *ppvObject = static_cast<IDirect3D9 *>(this);
    AddRef();
    return S_OK;
  }
  if (m_isEx && riid == __uuidof(IDirect3D9Ex)) {
    *ppvObject = static_cast<IDirect3D9Ex *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::RegisterSoftwareDevice(void *pInitializeFunction) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_RegisterSoftwareDevice);
  if (!pInitializeFunction)
    return D3DERR_INVALIDCALL;
  return D3DERR_NOTAVAILABLE;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterCount() {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterCount);
  return m_adapterCount;
}

// Canonical D3D9 device UID. wined3d hands this back from
// wined3d_adapter_get_identifier (directx.c, :1805). Some 2005-era
// titles compare DeviceIdentifier against this exact GUID and treat
// any other value as a "non-standard" adapter, falling into a SetupAPI/
// EnumDisplayDevices fallback that walks the registry every frame.
static const GUID kD3DDeviceD3DUID = {0xaeb2cdd4, 0x6e41, 0x43ea, {0x94, 0x1c, 0x83, 0x61, 0xcc, 0x76, 0x07, 0x81}};

// MADEIRA: the adapter identity this port reports, and the one opt-in knob
// over it.
//
// The defaults are honest: 0x106B is Apple's real PCI vendor ID (the same
// value dxgi_adapter.cpp hands D3D11 callers), device 1, and the Metal
// device's own name as the description. A translation layer should say what
// it is, so nothing here impersonates a GPU vendor by default.
//
// The knob exists because a title of this era commonly keeps a hard-coded
// vendor table -- 0x10DE / 0x1002 / 0x8086 and nothing else -- and treats an
// unrecognised vendor the way it treats a broken driver: a reduced renderer,
// a refusal, or a code path nobody tested. That is not a bug we can fix from
// inside the adapter, only one the user can A/B, so it is spelled exactly the
// way DXVK spells it (d3d9.customVendorId / d3d9.customDeviceId in
// DXMT_CONFIG, four hex digits, absent = off) and reachable on device through
// Documents/madeira-dxmt.txt. Overriding the ids alone leaves the description
// saying what the adapter really is; d3d9.customDeviceDesc overrides that too
// for a title that reads the string instead of the numbers.
//
// wined3d has the same pair of settings (VideoPciVendorID / VideoPciDeviceID,
// wined3d_main.c) for the same reason.
namespace {

struct AdapterIdOverride {
  int32_t vendorId;
  int32_t deviceId;
  std::string desc;
};

int32_t
parsePciId(const std::string &s) {
  if (s.size() != 4)
    return -1;
  int32_t id = 0;
  for (char c : s) {
    id *= 16;
    if (c >= '0' && c <= '9')
      id += c - '0';
    else if (c >= 'A' && c <= 'F')
      id += c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      id += c - 'a' + 10;
    else
      return -1;
  }
  return id;
}

const AdapterIdOverride &
adapterIdOverride() {
  static const AdapterIdOverride ov = [] {
    const Config &cfg = Config::getInstance();
    AdapterIdOverride o{};
    o.vendorId = parsePciId(cfg.getOption<std::string>("d3d9.customVendorId", ""));
    o.deviceId = parsePciId(cfg.getOption<std::string>("d3d9.customDeviceId", ""));
    o.desc = cfg.getOption<std::string>("d3d9.customDeviceDesc", "");
    return o;
  }();
  return ov;
}

} // namespace

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterIdentifier);
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  auto device = m_adapters.object(Adapter);

  std::memset(pIdentifier, 0, sizeof(*pIdentifier));
  std::snprintf(pIdentifier->Driver, MAX_DEVICE_IDENTIFIER_STRING, "%s", "DXMT (Metal)");
  device.name().getCString(pIdentifier->Description, MAX_DEVICE_IDENTIFIER_STRING, WMTUTF8StringEncoding);
  std::snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "%s", "\\\\.\\DISPLAY1");

  // 0x106B is Apple's PCI vendor ID, the same value dxgi_adapter.cpp
  // hands back to D3D11 callers. DeviceId / SubSysId / Revision aren't
  // meaningful for Metal but keep DeviceId non-zero; some titles
  // treat 0 as "no adapter" and fall through to a different code path.
  //
  // Both must stay in step with the identity the 2D path reports, or a title
  // that asks D3D9 and DirectDraw for the same adapter sees two different
  // GPUs: wine/dlls/wined3d/directx.c's no3d gpu_description carries the same
  // pair, and build/win32u-unix/sysparams_ios.c puts them in the synthesized
  // EnumDisplayDevices DeviceID string.
  pIdentifier->VendorId = 0x106B;
  pIdentifier->DeviceId = 1;
  // SubSysId / Revision intentionally 0; wined3d does the same.

  const AdapterIdOverride &ov = adapterIdOverride();
  if (ov.vendorId >= 0)
    pIdentifier->VendorId = (DWORD)ov.vendorId;
  if (ov.deviceId >= 0)
    pIdentifier->DeviceId = (DWORD)ov.deviceId;
  if (!ov.desc.empty())
    std::snprintf(pIdentifier->Description, MAX_DEVICE_IDENTIFIER_STRING, "%s", ov.desc.c_str());

  // Apps gate driver-bug workarounds on this version; a low value
  // re-enables ancient workaround paths. DXVK reports INT64_MAX
  // ("newest possible driver", d3d9_adapter.cpp) after games
  // misbehaved on small values; mirror it.
  pIdentifier->DriverVersion.QuadPart = INT64_MAX;

  pIdentifier->DeviceIdentifier = kD3DDeviceD3DUID;
  pIdentifier->WHQLLevel = (Flags & D3DENUM_WHQL_LEVEL) ? 1 : 0;

  if (capsFirstTime(capsKey(6, Adapter, Flags)))
    capsLine(
        "GetAdapterIdentifier adapter=%u flags=0x%x -> hr 0x0  vendor=0x%04x device=0x%04x rev=%u subsys=0x%08x "
        "driver=\"%s\" version=%u.%u.%u.%u desc=\"%s\" devicename=\"%s\"%s",
        Adapter, (unsigned)Flags, (unsigned)pIdentifier->VendorId, (unsigned)pIdentifier->DeviceId,
        (unsigned)pIdentifier->Revision, (unsigned)pIdentifier->SubSysId, pIdentifier->Driver,
        (unsigned)(pIdentifier->DriverVersion.HighPart >> 16), (unsigned)(pIdentifier->DriverVersion.HighPart & 0xffff),
        (unsigned)(pIdentifier->DriverVersion.LowPart >> 16), (unsigned)(pIdentifier->DriverVersion.LowPart & 0xffff),
        pIdentifier->Description, pIdentifier->DeviceName,
        (ov.vendorId >= 0 || ov.deviceId >= 0 || !ov.desc.empty()) ? "  [overridden by DXMT_CONFIG d3d9.custom*]" : ""
    );

  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterModeCount);
  if (Adapter >= m_adapterCount)
    return 0;
  if (!isEnumerableDisplayFormat(Format))
    return 0;
  LogAdapterModesOnce(Adapter);
  return static_cast<UINT>(adapterModes(Adapter).size());
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_EnumAdapterModes);
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (!isEnumerableDisplayFormat(Format))
    return D3DERR_INVALIDCALL;
  LogAdapterModesOnce(Adapter);

  const auto modes = adapterModes(Adapter);
  if (Mode >= modes.size())
    return D3DERR_INVALIDCALL;

  pMode->Width = modes[Mode].width;
  pMode->Height = modes[Mode].height;
  pMode->RefreshRate = refreshRateHzOr60(modes[Mode]);
  // The mode is reported at the format that was asked for: the extent is what
  // varies per entry, the format is the caller's filter (wined3d directx.c
  // wined3d_output_get_mode, DXVK d3d9_adapter.cpp).
  pMode->Format = Format;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterDisplayMode);
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  LogAdapterModesOnce(Adapter);

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = refreshRateHzOr60(wm);
  // D3D9 only ever advertises X8R8G8B8 / R5G6B5 here. Pick X8R8G8B8.
  // matches what wined3d does on a 32-bit GL desktop and what every
  // modern Windows desktop reports.
  pMode->Format = D3DFMT_X8R8G8B8;
  if (capsFirstTime(capsKey(7, Adapter)))
    capsLine(
        "GetAdapterDisplayMode adapter=%u -> hr 0x0  %ux%u@%u %s", Adapter, pMode->Width, pMode->Height,
        pMode->RefreshRate, fmtStr(pMode->Format).c_str()
    );
  return D3D_OK;
}

// MADEIRA [d3d9-caps]: the five format/capability probes are traced by
// wrapping rather than by editing every return. CheckDeviceFormat alone has
// two dozen exits, and a trace threaded through all of them is a trace that
// drifts the first time one of them moves. The wrapper is also where the
// census counter lives (gen_d3d9_census.py injects at the top of every
// STDMETHODCALLTYPE definition, and the *Probe bodies below deliberately are
// not STDMETHODCALLTYPE), so the per-method counts are unchanged.
HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CheckDeviceType);
  HRESULT hr = CheckDeviceTypeProbe(Adapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
  uint64_t key = capsKey(1, Adapter, DevType, (uint32_t)DisplayFormat, (uint32_t)BackBufferFormat, bWindowed ? 1u : 0u);
  if (capsFirstTime(key))
    capsLine(
        "CheckDeviceType adapter=%u %s display=%s backbuffer=%s windowed=%d -> hr 0x%08x", Adapter, devtypeStr(DevType),
        fmtStr(DisplayFormat).c_str(), fmtStr(BackBufferFormat).c_str(), bWindowed ? 1 : 0, (unsigned)hr
    );
  return hr;
}

HRESULT
MTLD3D9Interface::CheckDeviceTypeProbe(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // There are only four display formats, and an alpha format is never one of
  // them. wined3d directx.c wined3d_check_device_type.
  if (DisplayFormat != D3DFMT_X8R8G8B8 && DisplayFormat != D3DFMT_R5G6B5 && DisplayFormat != D3DFMT_X1R5G5B5 &&
      DisplayFormat != D3DFMT_A2R10G10B10)
    return D3DERR_NOTAVAILABLE;

  if (!bWindowed) {
    // Fullscreen requires the display format to actually have enumerable modes.
    if (GetAdapterModeCount(Adapter, DisplayFormat) == 0)
      return D3DERR_NOTAVAILABLE;
  } else if (DisplayFormat == D3DFMT_A2R10G10B10) {
    // A2R10G10B10 is a fullscreen-only display format.
    return D3DERR_NOTAVAILABLE;
  }

  if (bWindowed) {
    // Windowed mode permits the driver to convert the backbuffer to the display
    // format at present. Backbuffer == UNKNOWN means "use the display format",
    // but only here: fullscreen keeps UNKNOWN so the strict match below rejects
    // it (wined3d substitutes inside the conversion branch only).
    if (BackBufferFormat == D3DFMT_UNKNOWN)
      BackBufferFormat = DisplayFormat;
    if (FAILED(CheckDeviceFormatConversion(Adapter, DevType, BackBufferFormat, DisplayFormat)))
      return D3DERR_NOTAVAILABLE;
  } else {
    // Fullscreen: ignoring alpha, the display and backbuffer formats must match
    // exactly, so each display format pairs only with itself or its alpha twin.
    bool match = DisplayFormat == BackBufferFormat ||
                 (DisplayFormat == D3DFMT_X1R5G5B5 && BackBufferFormat == D3DFMT_A1R5G5B5) ||
                 (DisplayFormat == D3DFMT_X8R8G8B8 && BackBufferFormat == D3DFMT_A8R8G8B8);
    if (!match)
      return D3DERR_NOTAVAILABLE;
  }

  // Finally the backbuffer format must be usable as a render target.
  if (FAILED(
          CheckDeviceFormat(Adapter, DevType, DisplayFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, BackBufferFormat)
      ))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

namespace {

// D16_LOCKABLE and D32_LOCKABLE are NOT advertised: a lockable depth surface
// lets the app read depth back via LockRect, which needs a host-visible
// backing dxmt's GPU-private depth textures do not have, so the lock would
// fail. D16_LOCKABLE is AMD-only and D32_LOCKABLE is unsupported everywhere
// (DXVK d3d9_format.cpp). Plain D32 (32-bit unorm depth) is likewise dropped:
// no hardware driver exposes it, so wined3d intentionally refuses it too
// (wined3d utils.c); apps fall back to D24X8 / D16. D32F_LOCKABLE stays: it
// is the only float depth format, used as a plain depth buffer (the unlikely
// lock path is the separate lockable-depth feature).
bool
isDSFormat(D3DFORMAT f) {
  switch (f) {
  case D3DFMT_D16:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D32F_LOCKABLE:
  case D3DFMT_D24FS8:
  case D3DFMT_D15S1:
  // FOURCC sampleable-depth aliases (D3DFormatToMetal maps them).
  // Games probe via CheckDeviceFormat(... DEPTHSTENCIL ...,
  // INTZ) before allocating a shadow-map intermediate; NOTAVAILABLE
  // here would force them off the fast PCF path even though creation
  // would succeed. wined3d's directx.c exposes INTZ whenever the matching
  // native depth format is available; DF16 / DF24 are ATI-specific FOURCCs
  // with no wined3d support at all (they come from d9vk / DXVK), so this set
  // advertises INTZ on the wined3d precedent and DF16 / DF24 on the d9vk one.
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    return true;
  default:
    return false;
  }
}

} // namespace

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
    D3DFORMAT CheckFormat
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CheckDeviceFormat);
  HRESULT hr = CheckDeviceFormatProbe(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
  uint64_t key = capsKey(2, ((uint64_t)Adapter << 32) | DeviceType, (uint32_t)AdapterFormat, Usage, RType, (uint32_t)CheckFormat);
  if (capsFirstTime(key))
    capsLine(
        "CheckDeviceFormat adapter=%u %s adapterfmt=%s usage=%s rtype=%s fmt=%s -> hr 0x%08x", Adapter,
        devtypeStr(DeviceType), fmtStr(AdapterFormat).c_str(), usageStr(Usage).c_str(), rtypeStr(RType),
        fmtStr(CheckFormat).c_str(), (unsigned)hr
    );
  return hr;
}

HRESULT
MTLD3D9Interface::CheckDeviceFormatProbe(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
    D3DFORMAT CheckFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;

  // The adapter-format validation precedes the device-type gate: a
  // D3DFMT_UNKNOWN adapter format is D3DERR_INVALIDCALL for every device
  // type, since the d3d9 runtime validates the adapter format before the
  // device type is ever consumed.
  //
  // The accepted set is the four D3D9 DISPLAY formats, which is exactly the
  // set isEnumerableDisplayFormat enumerates modes for. A2R10G10B10 used to
  // be missing here while being enumerable there, and the two answers
  // contradicted each other in a way an application acts on: CheckDeviceType
  // for a fullscreen device ends by asking CheckDeviceFormat whether the
  // backbuffer is a render target AT THE DISPLAY FORMAT, so every
  // A2R10G10B10 fullscreen probe was refused on an adapter that had just
  // reported fourteen A2R10G10B10 modes. A title that walks the display
  // formats and takes the widest one it is offered got a mode list it could
  // not create a device from, with no second answer telling it to fall back.
  // A2R10G10B10 is a real render target here (isColorRTFormat lists it), so
  // accepting it is also what the create path can honour.
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5 &&
      AdapterFormat != D3DFMT_A2R10G10B10)
    return AdapterFormat ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  const bool wantRT = (Usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool wantDS = (Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
  if (wantRT && wantDS)
    return D3DERR_NOTAVAILABLE;

  // A depth-stencil format on a cube texture is never creatable
  // (validate_texture_create rejects it: no native driver backs a depth cube).
  // Deny the format probe too so CheckDeviceFormat and the create stay in step;
  // a split probe/create trips an app that gates a code path on the query.
  if (RType == D3DRTYPE_CUBETEXTURE && IsDepthStencilFormat(CheckFormat))
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_AUTOGENMIPMAP asks whether the runtime can auto-build a texture's
  // mip chain. It is a texture-only query (a standalone surface or a volume
  // slice has no mip chain to generate), so it is an invalid usage on any other
  // resource type. wined3d allows the bit only in the 2D-texture allowed_usage
  // set (wined3d directx.c).
  const bool wantAutoGen = (Usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
  if (wantAutoGen && RType != D3DRTYPE_TEXTURE && RType != D3DRTYPE_CUBETEXTURE)
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_DMAP is displacement mapping for the N-patch tessellator, which
  // this implementation does not have: D3DDEVCAPS2_DMAPNPATCH is not among the
  // DevCaps2 bits below. Answering OK here would tell an application the
  // feature is available and then fail it at draw, so refuse the probe and keep
  // the two answers consistent. DXVK refuses it for the same reason.
  if (Usage & D3DUSAGE_DMAP)
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_QUERY_SRGBREAD / SRGBWRITE: apps probe sRGB sampling/writing.
  // dxmt aliases to *_sRGB Metal format; return OK for formats with sRGB.
  // Apps getting NOTAVAILABLE fall back to non-sRGB shader path.
  const bool querySrgbRead = (Usage & D3DUSAGE_QUERY_SRGBREAD) != 0;
  const bool querySrgbWrite = (Usage & D3DUSAGE_QUERY_SRGBWRITE) != 0;
  if (querySrgbRead || querySrgbWrite) {
    // sRGB read is a sampler capability, sRGB write a render-target one.
    // A plain surface (no shader-resource role) carries SRGBWRITE only
    // when RENDERTARGET is also requested, and never SRGBREAD; the sampled
    // resource types carry both. wined3d gates the query bits the same way
    // through its per-resource allowed_usage table (wined3d directx.c).
    const bool sampledType = RType == D3DRTYPE_TEXTURE || RType == D3DRTYPE_CUBETEXTURE ||
                             RType == D3DRTYPE_VOLUMETEXTURE || RType == D3DRTYPE_VOLUME;
    if (querySrgbRead && !sampledType)
      return D3DERR_NOTAVAILABLE;
    if (querySrgbWrite && !sampledType && !wantRT)
      return D3DERR_NOTAVAILABLE;

    auto metal_fmt = D3DFormatToMetal(CheckFormat, D3D9FormatUsage::SampleableTexture);
    // D3DFMT_A8L8 lowers to RG8Unorm, which HAS an sRGB Metal sibling, but that
    // sibling gamma-decodes the alpha lane stored in G; native D3D9 and DXVK
    // give A8L8 no sRGB pair (D3D9FormatSuppressSRGBRead), so deny both
    // SRGBREAD and SRGBWRITE for it here just as the sample-bind alias skips it.
    if (metal_fmt == WMTPixelFormatInvalid || Recall_sRGB(metal_fmt) == metal_fmt ||
        D3D9FormatSuppressSRGBRead(CheckFormat))
      return D3DERR_NOTAVAILABLE;
  }

  // D3DUSAGE_QUERY_FILTER asks whether the format can be linearly filtered.
  // Apple-silicon Metal cannot filter the 32-bit-float colour formats, so deny
  // it for them (native ATI / pre-G80 denied it too, and wined3d gates on the
  // per-format FILTERING cap); the sampler translation degrades a LINEAR
  // request over these formats to point anyway. Every other format falls
  // through to the per-type probe below and reports filterable as before.
  if ((Usage & D3DUSAGE_QUERY_FILTER) && IsMetalNonFilterableFormat(CheckFormat))
    return D3DERR_NOTAVAILABLE;

  auto isSamplable2D = [&](D3DFORMAT f) {
    if (isColorRTFormat(f))
      return true;
    switch (f) {
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_A8L8:
    case D3DFMT_L8:
    case D3DFMT_L16:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
    case D3DFMT_ATI1:
    case D3DFMT_ATI2:
    case D3DFMT_V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    // 4-channel signed-norm bump, mapped to RGBA16Snorm (sampler-only, like the
    // other bump formats); advertise it so the TEXTURE probe agrees with the
    // create path (d9vk / DXVK / wined3d all support it).
    case D3DFMT_Q16W16V16U16:
      return true;
    default:
      return false;
    }
  };

  switch (RType) {
  case D3DRTYPE_SURFACE:
    if (wantDS)
      return isDSFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (wantRT) {
      // 'NULL' FOURCC is a vendor-defined sentinel for a colour RT
      // slot the app binds but never writes; accepted as RT-capable
      // here so apps gate creation on the standard ENABLE pattern.
      // dxmt drops the slot at render-pass + PSO build time.
      if (IsNullFormat(CheckFormat))
        return D3D_OK;
      return isColorRTFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    // 3Dc (ATI1N/ATI2N) is a texture-only vendor FOURCC: sampleable as a
    // texture but with no offscreen-plain surface form. CreateOffscreenPlainSurface
    // rejects it in every pool but SCRATCH, so deny the surface cap here to keep
    // the advertisement consistent with the create; the texture arm below still
    // grants it. Native advertises the pair for D3DRTYPE_TEXTURE only.
    if (Is3DcFormat(CheckFormat))
      return D3DERR_NOTAVAILABLE;
    return (isSamplable2D(CheckFormat) || isDSFormat(CheckFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_TEXTURE:
  case D3DRTYPE_CUBETEXTURE:
    if (wantDS) {
      // Depth-stencil CUBE maps: Metal can allocate one and CreateCubeTexture
      // lowers a DS format fine, but neither reference grants the cap (DXVK
      // restricts cube to the color aspect, wined3d strips the DEPTH_STENCIL
      // bind for cubemaps) and 2005-era hardware had no cube depth. Advertising
      // it would send a point-light-shadow app down a Windows-untested path, so
      // the probe denies it. The create path stays permissive (a blind create
      // still works) - a deliberate probe-conservative / create-permissive split.
      if (RType == D3DRTYPE_CUBETEXTURE)
        return D3DERR_NOTAVAILABLE;
      return isDSFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    if (wantRT)
      // The 'NULL' FOURCC is RT-capable as a render-target texture too, not
      // just a standalone surface (see the SURFACE branch); apps query it
      // before binding a write-skipped colour slot backed by a texture.
      return (isColorRTFormat(CheckFormat) || IsNullFormat(CheckFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;
    // Depth formats are sampleable textures, not just depth attachments:
    // wined3d marks every depth format FORMAT_CAP_TEXTURE (ARB_depth_texture),
    // and 2005-era titles create a plain depth texture (D24X8 / D24S8) to read
    // back as a hardware shadow map. dxmt lowers them to a sampleable Metal
    // depth texture, so advertise them texturable here too (not only via the
    // DEPTHSTENCIL probe above), keeping CheckDeviceFormat consistent with the
    // create path.
    if (!isSamplable2D(CheckFormat) && !isDSFormat(CheckFormat))
      return D3DERR_NOTAVAILABLE;
    // Hardware mip generation renders each level down, so it needs a colour-
    // renderable format. dxmt ties autogen-capability to RT-capability: a
    // renderable format autogens (a RENDERTARGET | AUTOGENMIPMAP probe already
    // returned D3D_OK in the wantRT branch above), a non-renderable one is still
    // a valid texture but reports D3DOK_NOAUTOGEN, the success-with-caveat code
    // apps fall back on by building the mips themselves. wined3d gates this on
    // the per-format GEN_MIPMAP cap (wined3d directx.c).
    if (wantAutoGen && !isColorRTFormat(CheckFormat))
      return D3DOK_NOAUTOGEN;
    return D3D_OK;

  case D3DRTYPE_VOLUMETEXTURE:
  case D3DRTYPE_VOLUME:
    // 3D textures: no DS, no RT. The mappable-color set is derived from the
    // same predicate CreateVolumeTexture gates on (IsVolumeTextureFormat), so
    // the probe and the create can never drift; both refs advertise every
    // mapped color format for TEXTURE_3D.
    if (wantDS || wantRT)
      return D3DERR_NOTAVAILABLE;
    return IsVolumeTextureFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_VERTEXBUFFER:
  case D3DRTYPE_INDEXBUFFER:
    // Buffer resource types are not valid CheckDeviceFormat queries: native
    // returns D3DERR_INVALIDCALL for VERTEXDATA / INDEX16 / INDEX32 regardless
    // of format (wine d3d9 test device.c; DXVK rejects buffer rtypes up front
    // in d3d9_adapter.cpp). wined3d itself gets this wrong (todo_wine), so
    // match native/DXVK, not wined3d.
    return D3DERR_INVALIDCALL;

  default:
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CheckDeviceMultiSampleType);
  HRESULT hr =
      CheckDeviceMultiSampleTypeProbe(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
  uint64_t key = capsKey(3, ((uint64_t)Adapter << 32) | DeviceType, (uint32_t)SurfaceFormat, MultiSampleType);
  if (capsFirstTime(key))
    capsLine(
        "CheckDeviceMultiSampleType adapter=%u %s fmt=%s samples=%u -> hr 0x%08x  quality=%u", Adapter,
        devtypeStr(DeviceType), fmtStr(SurfaceFormat).c_str(), (unsigned)MultiSampleType, (unsigned)hr,
        pQualityLevels ? (unsigned)*pQualityLevels : 0u
    );
  return hr;
}

HRESULT
MTLD3D9Interface::CheckDeviceMultiSampleTypeProbe(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d caps the request at 16; anything beyond is invalid input.
  // wine dlls/d3d9/directx.c.
  if (MultiSampleType > D3DMULTISAMPLE_16_SAMPLES)
    return D3DERR_INVALIDCALL;

  // D3DFMT_UNKNOWN is rejected outright, even for D3DMULTISAMPLE_NONE.
  // wined3d_check_device_multisample_type does this before any type or
  // capability check (wined3d directx.c).
  if (SurfaceFormat == D3DFMT_UNKNOWN)
    return D3DERR_INVALIDCALL;

  // The format must be one the create path can actually back with a
  // Metal texture. Deriving the check from D3DFormatToMetal (the same
  // call CreateRenderTarget / CreateDepthStencilSurface make) keeps the
  // probe from rejecting formats the create path accepts, e.g. G16R16,
  // A2B10G10R10, R32F, G32R32F. DXVK gates on ConvertFormatUnfixed for
  // the same reason.
  auto device = m_adapters.object(Adapter);
  const bool is_ds = IsDepthStencilFormat(SurfaceFormat);
  WMTPixelFormat metal_fmt =
      D3DFormatToMetal(SurfaceFormat, is_ds ? D3D9FormatUsage::DepthStencil : D3D9FormatUsage::RenderTarget);
  const bool format_ok = metal_fmt != WMTPixelFormatInvalid || IsNullFormat(SurfaceFormat);

  HRESULT hr = D3D_OK;
  DWORD quality_levels = 1;
  if (MultiSampleType == D3DMULTISAMPLE_NONE) {
    hr = D3D_OK;
  } else if (!format_ok) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (MultiSampleType == D3DMULTISAMPLE_NONMASKABLE) {
    // NONMASKABLE reports how many quality levels exist; level N selects
    // 1 << N samples at create time (the DXVK GetSampleCount mapping).
    // Count the supported power-of-two sample counts so an app's
    // quality-level loop sees exactly the counts the create path honours.
    // Apple's supportsTextureSampleCount: is the device capability Metal
    // exposes (no per-format sample-count table), matching what
    // newTextureDescriptor with sampleCount=N will allow.
    quality_levels = 1; // level 0 = 1 sample, always available
    for (uint8_t n = 2; n <= 16; n <<= 1) {
      if (device.supportsTextureSampleCount(n))
        ++quality_levels;
      else
        break;
    }
    hr = D3D_OK;
  } else {
    // Masked request: the count is the enum value itself. Probe the
    // device for that exact sample count; M-series GPUs vary in 8x
    // support by family. wined3d issues an equivalent
    // vkGetPhysicalDeviceImageFormatProperties probe.
    hr = device.supportsTextureSampleCount(static_cast<uint8_t>(MultiSampleType)) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  // On D3DERR_NOTAVAILABLE the d3d9 runtime still writes a quality count of
  // 1 (wine directx.c: `if (hr == WINED3DERR_NOTAVAILABLE && levels) *levels
  // = 1`). Only D3DERR_INVALIDCALL leaves the out-pointer untouched.
  if (pQualityLevels) {
    if (hr == D3D_OK)
      *pQualityLevels = quality_levels;
    else if (hr == D3DERR_NOTAVAILABLE)
      *pQualityLevels = 1;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
    D3DFORMAT DepthStencilFormat
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CheckDepthStencilMatch);
  HRESULT hr =
      CheckDepthStencilMatchProbe(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
  uint64_t key = capsKey(4, ((uint64_t)Adapter << 32) | DeviceType, (uint32_t)AdapterFormat, (uint32_t)RenderTargetFormat, (uint32_t)DepthStencilFormat);
  if (capsFirstTime(key))
    capsLine(
        "CheckDepthStencilMatch adapter=%u %s adapterfmt=%s rt=%s ds=%s -> hr 0x%08x", Adapter, devtypeStr(DeviceType),
        fmtStr(AdapterFormat).c_str(), fmtStr(RenderTargetFormat).c_str(), fmtStr(DepthStencilFormat).c_str(),
        (unsigned)hr
    );
  return hr;
}

HRESULT
MTLD3D9Interface::CheckDepthStencilMatchProbe(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
    D3DFORMAT DepthStencilFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;
  // Same four display formats CheckDeviceFormat accepts, and for the same
  // reason: an application that probes a depth-stencil pair at the display
  // format it just enumerated must not be told the format does not exist.
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5 &&
      AdapterFormat != D3DFMT_A2R10G10B10)
    return D3DERR_NOTAVAILABLE;

  // Shares CheckDeviceFormat's format predicates so the two probes cannot
  // diverge; apps query one then the other and expect consistent answers.
  // wined3d does real bit-depth pairing; on Metal the depth/stencil
  // attachment is independent of the colour attachment, so any legal
  // (RT, DS) pair is compatible.
  // A 'NULL' colour target pairs with any real depth/stencil: a depth-only
  // pass binds NULL as the colour slot and a genuine DS attachment.
  return ((isColorRTFormat(RenderTargetFormat) || IsNullFormat(RenderTargetFormat)) && isDSFormat(DepthStencilFormat))
             ? D3D_OK
             : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CheckDeviceFormatConversion);
  HRESULT hr = CheckDeviceFormatConversionProbe(Adapter, DeviceType, SourceFormat, TargetFormat);
  uint64_t key = capsKey(5, ((uint64_t)Adapter << 32) | DeviceType, (uint32_t)SourceFormat, (uint32_t)TargetFormat);
  if (capsFirstTime(key))
    capsLine(
        "CheckDeviceFormatConversion adapter=%u %s src=%s dst=%s -> hr 0x%08x", Adapter, devtypeStr(DeviceType),
        fmtStr(SourceFormat).c_str(), fmtStr(TargetFormat).c_str(), (unsigned)hr
    );
  return hr;
}

HRESULT
MTLD3D9Interface::CheckDeviceFormatConversionProbe(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d short-circuits the same-format case before consulting its
  // table. wine dlls/d3d9/directx.c.
  if (SourceFormat == TargetFormat)
    return D3D_OK;

  // Models wined3d_check_device_format_conversion (wined3d directx.c): the
  // source must be a blittable non-depth format and the destination a simple
  // RGB format with a real red channel. Packed YUV is a valid source (StretchRect
  // YUV->RGB, which real hardware performs though WARP does not); the float-color
  // formats carry no fixed-function blit cap and are rejected as a source, while
  // remaining valid RGB destinations. isColorRTFormat is exactly the simple-RGB
  // set for the destination: it excludes luminance, YUV, A8 and the compressed
  // formats.
  const bool src_blittable = SourceFormat == D3DFMT_YUY2 || SourceFormat == D3DFMT_UYVY ||
                             (isColorRTFormat(SourceFormat) && !IsFloatColorFormat(SourceFormat));
  if (!src_blittable || !isColorRTFormat(TargetFormat))
    return D3DERR_NOTAVAILABLE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetDeviceCaps);
  if (!pCaps)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;

  std::memset(pCaps, 0, sizeof(*pCaps));

  // wined3d_caps_from_wined3dcaps masks each field down to the bits
  // D3D9 actually defines. We start from those masks directly; they
  // describe a hardware-T&L SM 3.0 GPU, which is what most D3D9 games
  // expect. wine dlls/d3d9/device.c d3d9_caps_from_wined3dcaps.
  pCaps->DeviceType = DeviceType;
  pCaps->AdapterOrdinal = Adapter;

  pCaps->Caps = D3DCAPS_READ_SCANLINE;
  // D3DCAPS2_CANCALIBRATEGAMMA is deliberately omitted (as wined3d and DXVK do):
  // the SetGammaRamp D3DSGR_CALIBRATE flag is ignored, so advertising it would
  // be a caps lie. D3DCAPS2_CANMANAGERESOURCE and D3DCAPS2_RESERVED are dropped
  // for the same reason: neither reference raises them, and CANMANAGERESOURCE
  // claims driver-side management of the MANAGED pool while dxmt mirrors MANAGED
  // resources in the runtime (the DXVK model), so it is not earned.
  pCaps->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
  // D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION is not advertised: the Present path
  // consumes only D3DPRESENTFLAG_LOCKABLE_BACKBUFFER and never
  // D3DPRESENTFLAG_LINEAR_CONTENT, so a title that renders linear content and
  // relies on a present-time linear->sRGB encode would present washed out.
  // wined3d never sets the bit. The honest upgrade is an sRGB drawable view at
  // the Present blit when LINEAR_CONTENT is set; deferred.
  pCaps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  // Per DXVK d3d9_adapter.cpp; claim the full set the Present
  // path honours. swapchain.cpp already maps INTERVAL_TWO/
  // THREE/FOUR to their N-vsync dwell; pre-port we under-claimed
  // (apps fell back to ONE without knowing TWO+ was available).
  // D3DPRESENT_INTERVAL_DEFAULT (=0) is reportable too; some apps
  // bit-test for it as a "no-vsync hint accepted" probe.
  pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_DEFAULT | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO |
                                 D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR | D3DPRESENT_INTERVAL_IMMEDIATE;
  pCaps->CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;

  pCaps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                   D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
  pCaps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET |
                    D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;

  pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                             D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                             D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP |
                             D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_SEPARATEALPHABLEND |
                             D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
                             D3DPMISCCAPS_FOGVERTEXCLAMPED | D3DPMISCCAPS_FOGANDSPECULARALPHA |
                             D3DPMISCCAPS_POSTBLENDSRGBCONVERT | D3DPMISCCAPS_PERSTAGECONSTANT;
  // CLIPTLVERTS: the generated fixed-function VS clips pre-transformed
  // (POSITIONT) draws against the user clip planes, so advertising the
  // "clips post-transform vertices" cap is honest. Both refs set it.
  // PERSTAGECONSTANT: the fixed-function combiner reads D3DTSS_CONSTANT
  // (D3DTA_CONSTANT) per stage, so the cap must be advertised or a
  // cap-checking app skips per-stage constants and falls back to TFACTOR.
  // wined3d and DXVK both advertise it.
  // FOGANDSPECULARALPHA: the fog factor rides its own FOG0 varying (VS oFog),
  // separate from the specular-alpha channel, in both the FFP generator and the
  // DXSO path, so fog and specular alpha never share storage. DXVK advertises
  // it; wined3d leaves it inside a TODO alongside FOGVERTEXCLAMPED.

  // Both WFOG and ZFOG are honored: table fog picks its coordinate from the
  // projection like wined3d (orthographic projection, 4th column 0,0,0,1 ->
  // device z; otherwise eye w), which is stronger than DXVK's single
  // z-shaped path. FOGRANGE is honored too (D3DRS_RANGEFOGENABLE drives the
  // FFP vertex-fog radial-distance path), so this whole mask is now backed.
  pCaps->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG |
                      D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS;
  // MULTISAMPLE_TOGGLE is deliberately absent. It promises that
  // D3DRS_MULTISAMPLEANTIALIAS can turn antialiasing off on an already
  // multisampled target, and Metal cannot express that: a pipeline's
  // rasterSampleCount must equal the attachment's, so single-sample
  // rasterization into a multisample pass has no representation. Both
  // references advertise it, wined3d honouring it and DXVK carrying a TODO,
  // but the limit here is permanent rather than unfinished work, so claiming
  // it would be a promise no future version keeps. D3DRS_MULTISAMPLEMASK,
  // the half that IS expressible, is honoured through a sample-mask variant.

  // Compare ops, blend factors, alpha-test ops; claim the full set.
  pCaps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                    D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  pCaps->AlphaCmpCaps = pCaps->ZCmpCaps;
  pCaps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA |
                        D3DPBLENDCAPS_BLENDFACTOR;
  // Dual-source factors are a 9Ex-only advertisement; non-Ex devices
  // never see SRCCOLOR2/INVSRCCOLOR2. d3d9_adapter.cpp gates them on
  // IsExtended() and so do we.
  if (m_isEx)
    pCaps->SrcBlendCaps |= D3DPBLENDCAPS_SRCCOLOR2 | D3DPBLENDCAPS_INVSRCCOLOR2;
  pCaps->DestBlendCaps = pCaps->SrcBlendCaps;

  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

  // D3DPTEXTURECAPS_NOPROJECTEDBUMPENV is not advertised: that restriction bit
  // claims projected bump-env lookups are unsupported, but the per-stage
  // PROJECTED divide runs before every stage's sample including the bump-env
  // stages, so the restriction is not real. Neither reference sets it.
  // ALPHAPALETTE is deliberately absent: the palettised formats it describes
  // are SCRATCH-only here and SetCurrentTexturePalette stores a palette that
  // no sampler consults, so CheckDeviceFormat already answers NOTAVAILABLE for
  // P8. Advertising the cap would only let the probe and the capability
  // contradict each other.
  pCaps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE |
                       D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP |
                       D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;

  // Regular texture filter caps: point/linear/anisotropic min+mag, point/linear
  // mip. This mask mirrors DXVK's d3d9_adapter set (wined3d omits the aniso bits
  // from its own TextureFilterCaps).
  const DWORD textureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
                                  D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
                                  D3DPTFILTERCAPS_MAGFANISOTROPIC;
  pCaps->TextureFilterCaps = textureFilterCaps;
  pCaps->CubeTextureFilterCaps = textureFilterCaps;
  pCaps->VolumeTextureFilterCaps = textureFilterCaps;
  // Classic D3D9 vertex texture fetch is point-sampled only in wined3d, which
  // reports just point min/mag here (directx.c SM3 arm); DXVK actually adds the
  // LINEAR bits (d3d9_adapter.cpp). Follow wined3d, the primary reference: a
  // filtering vertex sampler is a Metal capability the SM3-era reference
  // hardware lacked. Advertising less than we honor is the safe direction (the
  // VTF sample path filters if asked, like both refs).
  pCaps->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT;
  // StretchRect uses Metal's blit encoder which supports only point and
  // linear filtering; anisotropic is sampler-only. DXVK d3d9_adapter.cpp
  // reports only POINT|LINEAR. Pre-port dxmt reused the full
  // textureFilterCaps mask, so apps that requested D3DTEXF_ANISOTROPIC
  // on StretchRect silently degraded to LINEAR.
  pCaps->StretchRectFilterCaps =
      D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;

  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
  pCaps->VolumeTextureAddressCaps = pCaps->TextureAddressCaps;

  // No D3DLINECAPS_ANTIALIAS: Metal has no line smoothing and
  // D3DRS_ANTIALIASEDLINEENABLE is inert, so the bit would be a lie. Native
  // Windows drivers do not set it either (wined3d directx.c).
  pCaps->LineCaps =
      D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

  pCaps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                       D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                       D3DSTENCILCAPS_TWOSIDED;

  pCaps->FVFCaps = 8 | D3DFVFCAPS_PSIZE; // 8 = max texture coord count

  // Every advertised op has a real combiner arm in ffp_compile.cpp. The two
  // MODULATEINV* ops are the exact MSDN formulas (arg1 + (1-arg1.a)*arg2 and
  // (1-arg1)*arg2 + arg1.a), and both refs advertise them, so hiding them would
  // send a cap-checking combiner engine down a needless fallback.
  // D3DTEXOPCAPS_PREMODULATE is NOT advertised: the combiner no-ops op 17 (a DX6
  // relic with no well-defined semantics anywhere), and wined3d neither
  // advertises nor implements it; advertising an inert op would be a caps lie.
  pCaps->TextureOpCaps =
      D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
      D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
      D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
      D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM |
      D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
      D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3 |
      D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP | D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;

  // No D3DVTXPCAPS_TWEENING: the generated vertex pipe treats
  // D3DVBF_TWEENING as disabled, and wined3d leaves the bit clear too.
  // No D3DVTXPCAPS_NO_TEXGEN_NONLOCALVIEWER: that restriction bit claims texgen
  // breaks when D3DRS_LOCALVIEWER is FALSE, but texgen is shader-generated and
  // independent of the render state (LOCALVIEWER only steers the specular
  // half-vector), so the restriction is not real. Neither reference sets it.
  pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
  pCaps->MaxActiveLights = 8;
  pCaps->MaxUserClipPlanes = 8;
  pCaps->MaxVertexBlendMatrices = 4;
  pCaps->MaxVertexBlendMatrixIndex = 0;

  // Geometry / format limits. 16384 is the Metal-on-Apple-Silicon
  // texture max for non-MSAA 2D textures.
  pCaps->MaxTextureWidth = 16384;
  pCaps->MaxTextureHeight = 16384;
  pCaps->MaxVolumeExtent = 2048;
  pCaps->MaxTextureRepeat = 8192;
  pCaps->MaxTextureAspectRatio = 16384;
  pCaps->MaxAnisotropy = 16;
  pCaps->MaxVertexW = 1e10f;
  // The guard band tells an app how far outside the viewport it may leave
  // geometry unclipped. Both references report 32768, and nothing here has
  // measured what the tile rasterizer actually tolerates, so report what they
  // report rather than a number chosen for being large.
  pCaps->GuardBandLeft = -32768.0f;
  pCaps->GuardBandTop = -32768.0f;
  pCaps->GuardBandRight = 32768.0f;
  pCaps->GuardBandBottom = 32768.0f;
  pCaps->ExtentsAdjust = 0.0f;
  // Apple Silicon max point size 511.0 per Metal Feature Set Tables.
  // Vulkan pointSizeRange[1] equivalent.
  pCaps->MaxPointSize = 511.0f;
  // These two are the values wined3d and DXVK both report, taken from an AMD
  // Evergreen GPU.
  pCaps->MaxPrimitiveCount = 0x00555555;
  pCaps->MaxVertexIndex = 0x00FFFFFF;
  pCaps->MaxStreams = 16;
  // The references disagree here: wined3d reports 1024, DXVK 508. Follow DXVK,
  // since a stride ceiling only has to be at least what applications use and
  // the lower of the two is the one a shipping translation layer has run on.
  pCaps->MaxStreamStride = 508;
  pCaps->MaxNpatchTessellationLevel = 0.0f;

  pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  pCaps->MaxVertexShaderConst = 256;
  pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  pCaps->PixelShader1xMaxValue = 65504.0f;

  // SM 2.0/3.0 sub-caps: claim full predication / dynamic flow control.
  pCaps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
  pCaps->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
  pCaps->VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
  pCaps->VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                         D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
  pCaps->PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
  pCaps->PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
  pCaps->MaxVShaderInstructionsExecuted = 65535;
  pCaps->MaxPShaderInstructionsExecuted = 65535;
  pCaps->MaxVertexShader30InstructionSlots = 32768;
  pCaps->MaxPixelShader30InstructionSlots = 32768;

  pCaps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N |
                     D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;

  pCaps->NumSimultaneousRTs = 4;

  // Single-adapter group; matches what wined3d does when there's only
  // one output on the wined3d_adapter.
  pCaps->MasterAdapterOrdinal = 0;
  pCaps->AdapterOrdinalInGroup = 0;
  pCaps->NumberOfAdaptersInGroup = 1;

  // MADEIRA [d3d9-caps]: the fields a title of this era actually gates on.
  // Not the whole D3DCAPS9 -- the struct is 300-odd bytes and most of it has
  // never decided anything -- but every field that has been seen to end a
  // renderer: the shader versions, the fixed-function texture limits, the
  // four capability masks, and the geometry ceilings. Three lines, once per
  // (adapter, devtype).
  if (capsFirstTime(capsKey(8, Adapter, DeviceType))) {
    capsLine(
        "GetDeviceCaps adapter=%u %s -> hr 0x0  vs=%u.%u ps=%u.%u MaxVertexShaderConst=%u "
        "MaxVShader30Slots=%u MaxPShader30Slots=%u",
        Adapter, devtypeStr(DeviceType), (unsigned)((pCaps->VertexShaderVersion >> 8) & 0xff),
        (unsigned)(pCaps->VertexShaderVersion & 0xff), (unsigned)((pCaps->PixelShaderVersion >> 8) & 0xff),
        (unsigned)(pCaps->PixelShaderVersion & 0xff), (unsigned)pCaps->MaxVertexShaderConst,
        (unsigned)pCaps->MaxVertexShader30InstructionSlots, (unsigned)pCaps->MaxPixelShader30InstructionSlots
    );
    capsLine(
        "GetDeviceCaps adapter=%u   MaxSimultaneousTextures=%u MaxTextureBlendStages=%u MaxTexture=%ux%u "
        "MaxPrimitiveCount=0x%x MaxVertexIndex=0x%x MaxStreams=%u NumSimultaneousRTs=%u MaxActiveLights=%u "
        "MaxAnisotropy=%u",
        Adapter, (unsigned)pCaps->MaxSimultaneousTextures, (unsigned)pCaps->MaxTextureBlendStages,
        (unsigned)pCaps->MaxTextureWidth, (unsigned)pCaps->MaxTextureHeight, (unsigned)pCaps->MaxPrimitiveCount,
        (unsigned)pCaps->MaxVertexIndex, (unsigned)pCaps->MaxStreams, (unsigned)pCaps->NumSimultaneousRTs,
        (unsigned)pCaps->MaxActiveLights, (unsigned)pCaps->MaxAnisotropy
    );
    capsLine(
        "GetDeviceCaps adapter=%u   Caps=0x%08x Caps2=0x%08x Caps3=0x%08x DevCaps=0x%08x%s%s TextureCaps=0x%08x%s%s "
        "RasterCaps=0x%08x PrimitiveMiscCaps=0x%08x StencilCaps=0x%08x DeclTypes=0x%08x",
        Adapter, (unsigned)pCaps->Caps, (unsigned)pCaps->Caps2, (unsigned)pCaps->Caps3, (unsigned)pCaps->DevCaps,
        (pCaps->DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? " +HWTNL" : " -HWTNL",
        (pCaps->DevCaps & D3DDEVCAPS_PUREDEVICE) ? " +PURE" : " -PURE", (unsigned)pCaps->TextureCaps,
        (pCaps->TextureCaps & D3DPTEXTURECAPS_POW2) ? " +POW2" : " -POW2",
        (pCaps->TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) ? " +NONPOW2COND" : " -NONPOW2COND",
        (unsigned)pCaps->RasterCaps, (unsigned)pCaps->PrimitiveMiscCaps, (unsigned)pCaps->StencilCaps,
        (unsigned)pCaps->DeclTypes
    );
  }

  return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterMonitor(UINT Adapter) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterMonitor);
  if (Adapter >= m_adapterCount)
    return nullptr;
  return wsi::enumMonitors(Adapter);
}

// All D3DPRESENTFLAG_* bits the D3D9 runtime knows about. Anything
// outside this mask is a higher-version or vendor extension we don't
// understand. Mirrors dlls/d3d9/d3d9_private.h D3DPRESENTFLAGS_MASK.
static constexpr DWORD kD3DPresentFlagsMask = 0x00000fffu;

// PresentParamsRejectReason (the accept/reject matrix mirroring
// wined3d_swapchain_desc_from_d3d9) lives in d3d9_present_validation.hpp and is
// in scope through d3d9_interface.hpp.

// Resolve spec "use runtime default" placeholders in params; return false on
// invalid format, zero extent on a hidden window, or a fullscreen extent that
// is not an enumerable adapter mode. hwndFallback is hFocusWindow; spec:
// hDeviceWindow takes precedence (wined3d swapchain.c).
bool
CanonicalisePresentParams(D3DPRESENT_PARAMETERS &p, HWND hwndFallback, UINT adapter) {
  if (p.BackBufferCount == 0)
    p.BackBufferCount = 1;
  if (p.BackBufferFormat == D3DFMT_UNKNOWN)
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
  if (D3DFormatToMetal(p.BackBufferFormat, D3D9FormatUsage::RenderTarget) == WMTPixelFormatInvalid) {
    Logger::warn(
        str::format("D3DPRESENT_PARAMETERS: BackBufferFormat ", p.BackBufferFormat, " is not a render target")
    );
    return false;
  }
  // EnableAutoDepthStencil=TRUE with AutoDepthStencilFormat=UNKNOWN is
  // a spec-permitted "use the runtime default" placeholder per MSDN
  // CreateDevice; wined3d swapchain.c substitutes D24S8. Without
  // this, the auto-DS allocation path at d3d9_device.cpp::createAutoDS
  // would see UNKNOWN, call D3DFormatToMetal -> Invalid, and skip
  // allocating the implicit DS; leaving EnableAutoDepthStencil=TRUE
  // apps with no DS bound on the first frame.
  if (p.EnableAutoDepthStencil && p.AutoDepthStencilFormat == D3DFMT_UNKNOWN)
    p.AutoDepthStencilFormat = D3DFMT_D24S8;

  if (p.Windowed && (p.BackBufferWidth == 0 || p.BackBufferHeight == 0)) {
#if defined(DXMT_MADEIRA)
    /* MADEIRA (WOW64_DESIGN.md section 8.2(d)): no user32 here, and the
     * upstream !_WIN32 arm below leaves the extent at 0, which is not a
     * swapchain any caller can use. wsi::getWindowSize answers from the
     * per-HWND client-size cache the shim fills at CreateDevice / Reset /
     * Present (src/util/wsi_window_madeira.cpp), which is the same number
     * GetClientRect would have produced. */
    {
      HWND deriveFrom = p.hDeviceWindow ? p.hDeviceWindow : hwndFallback;
      uint32_t width = 0, height = 0;
      wsi::getWindowSize(deriveFrom, &width, &height);
      if (p.BackBufferWidth == 0)
        p.BackBufferWidth = width > 0 ? width : 8;
      if (p.BackBufferHeight == 0)
        p.BackBufferHeight = height > 0 ? height : 8;
    }
#elif defined(_WIN32)
    HWND deriveFrom = p.hDeviceWindow ? p.hDeviceWindow : hwndFallback;
    if (deriveFrom) {
      RECT rc{};
      GetClientRect(deriveFrom, &rc);
      if (p.BackBufferWidth == 0)
        p.BackBufferWidth = rc.right > 0 ? static_cast<UINT>(rc.right) : 8;
      if (p.BackBufferHeight == 0)
        p.BackBufferHeight = rc.bottom > 0 ? static_cast<UINT>(rc.bottom) : 8;
    }
#else
    (void)hwndFallback;
#endif
  }

  if (p.BackBufferWidth == 0 || p.BackBufferHeight == 0) {
    Logger::warn("D3DPRESENT_PARAMETERS: zero backbuffer extent");
    return false;
  }
  // Fullscreen present: native fails the create / Reset on an extent the adapter
  // cannot show, and so do we. The accepted set is the adapter's modes plus the
  // standard PC extents (isPresentableFullscreenExtent), so what an app of this
  // era asks for is honoured while a nonsense extent is still rejected.
  if (!p.Windowed &&
      !isPresentableFullscreenExtent(adapter, p.BackBufferWidth, p.BackBufferHeight, p.FullScreen_RefreshRateInHz)) {
    Logger::warn(
        str::format(
            "D3DPRESENT_PARAMETERS: fullscreen ", p.BackBufferWidth, "x", p.BackBufferHeight, "@",
            p.FullScreen_RefreshRateInHz, " is not an adapter mode"
        )
    );
    return false;
  }
  return true;
}

// Log unhandled D3DPRESENTFLAG_* bits the same way wined3d FIXMEs them;
// so contributors adding handling for LOCKABLE_BACKBUFFER, VIDEO,
// NOAUTOROTATE, etc. have an obvious entry point. Reference:
// dlls/d3d9/device.c.
static void
WarnUnhandledPresentFlags(DWORD flags) {
  if (DWORD unhandled = flags & ~kD3DPresentFlagsMask)
    Logger::warn(str::format("Unknown D3DPRESENT_PARAMETERS::Flags bits 0x", std::hex, unhandled));
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CreateDevice);
  if (!ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;
  *ppReturnedDeviceInterface = nullptr;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  // A device's extended-ness follows the parent interface, not the Create
  // method: IDirect3D9Ex::CreateDevice makes an extended device (DXVK routes
  // it straight through CreateDeviceEx), so it validates with the extended
  // backbuffer-count / swap-effect limits and QIs to IDirect3DDevice9Ex. A
  // plain IDirect3D9 stays non-extended.
  LogAdapterModesOnce(Adapter);
  if (const char *reason = PresentParamsRejectReason(*pPresentationParameters, /*isEx=*/m_isEx)) {
    Logger::warn(str::format("CreateDevice: rejected D3DPRESENT_PARAMETERS::", reason));
    LogPresentRequest("CreateDevice", *pPresentationParameters, D3DERR_INVALIDCALL);
    return D3DERR_INVALIDCALL;
  }

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  // Canonicalise the placeholders (zero extent, UNKNOWN format, zero
  // backbuffer count) in place. D3D9 writes the realized values back into the
  // caller's struct, the same as Reset and CreateAdditionalSwapChain, and that
  // is what apps read after CreateDevice.
  if (!CanonicalisePresentParams(*pPresentationParameters, hFocusWindow, Adapter)) {
    LogPresentRequest("CreateDevice", *pPresentationParameters, D3DERR_INVALIDCALL);
    return D3DERR_INVALIDCALL;
  }

  WMT::Reference<WMT::Device> metalDevice = m_adapters.object(Adapter);
  if (!metalDevice.handle) {
    LogPresentRequest("CreateDevice", *pPresentationParameters, D3DERR_OUTOFVIDEOMEMORY);
    return D3DERR_OUTOFVIDEOMEMORY;
  }

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/m_isEx, Adapter, DeviceType, hFocusWindow, BehaviorFlags, *pPresentationParameters,
      std::move(metalDevice)
  );
  device->AddRef();
  *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9 *>(device);
  LogPresentRequest("CreateDevice", *pPresentationParameters, D3D_OK);
  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterModeCountEx);
  if (!pFilter || Adapter >= m_adapterCount)
    return 0;
  if (!isEnumerableDisplayFormat(pFilter->Format))
    return 0;
  if (pFilter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED)
    return 0;
  LogAdapterModesOnce(Adapter);
  return static_cast<UINT>(adapterModes(Adapter).size());
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::EnumAdapterModesEx(
    UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_EnumAdapterModesEx);
  if (!pFilter || !pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (!isEnumerableDisplayFormat(pFilter->Format))
    return D3DERR_INVALIDCALL;
  if (pFilter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED)
    return D3DERR_INVALIDCALL;
  LogAdapterModesOnce(Adapter);

  const auto modes = adapterModes(Adapter);
  if (Mode >= modes.size())
    return D3DERR_INVALIDCALL;

  pMode->Size = sizeof(*pMode);
  pMode->Width = modes[Mode].width;
  pMode->Height = modes[Mode].height;
  pMode->RefreshRate = refreshRateHzOr60(modes[Mode]);
  pMode->Format = pFilter->Format;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterDisplayModeEx);
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (pMode->Size != sizeof(*pMode))
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = refreshRateHzOr60(wm);
  pMode->Format = D3DFMT_X8R8G8B8;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  if (pRotation)
    *pRotation = D3DDISPLAYROTATION_IDENTITY;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CreateDeviceEx(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
    IDirect3DDevice9Ex **ppDevice
) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_CreateDeviceEx);
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = nullptr;
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;
  // The MS contract says pFullscreenDisplayMode must be NULL in
  // windowed mode, but real DX9Ex titles (some WoW expansions, a
  // handful of others) sometimes pass non-null garbage anyway and
  // wined3d tolerates them. Only reject the genuine spec violation
  // (fullscreen request with no mode), match wined3d's silently-
  // tolerate stance for the windowed-with-stale-mode case.
  if (!pPresentationParameters->Windowed && !pFullscreenDisplayMode)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  LogAdapterModesOnce(Adapter);
  if (const char *reason = PresentParamsRejectReason(*pPresentationParameters, /*isEx=*/true)) {
    Logger::warn(str::format("CreateDeviceEx: rejected D3DPRESENT_PARAMETERS::", reason));
    LogPresentRequest("CreateDeviceEx", *pPresentationParameters, D3DERR_INVALIDCALL);
    return D3DERR_INVALIDCALL;
  }

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  // Canonicalise in place so the caller reads back the realized extent /
  // format / count, matching CreateDevice and the Reset path.
  if (!CanonicalisePresentParams(*pPresentationParameters, hFocusWindow, Adapter)) {
    LogPresentRequest("CreateDeviceEx", *pPresentationParameters, D3DERR_INVALIDCALL);
    return D3DERR_INVALIDCALL;
  }

  WMT::Reference<WMT::Device> metalDevice = m_adapters.object(Adapter);
  if (!metalDevice.handle) {
    LogPresentRequest("CreateDeviceEx", *pPresentationParameters, D3DERR_OUTOFVIDEOMEMORY);
    return D3DERR_OUTOFVIDEOMEMORY;
  }

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/true, Adapter, DeviceType, hFocusWindow, BehaviorFlags, *pPresentationParameters,
      std::move(metalDevice)
  );
  device->AddRef();
  *ppDevice = static_cast<IDirect3DDevice9Ex *>(device);
  LogPresentRequest("CreateDeviceEx", *pPresentationParameters, D3D_OK);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterLUID(UINT Adapter, LUID *pLUID) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Interface_GetAdapterLUID);
  if (!pLUID)
    return D3DERR_INVALIDCALL;
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  uint64_t id = m_adapters.object(Adapter).registryID();
  pLUID->LowPart = static_cast<DWORD>(id & 0xFFFFFFFFu);
  pLUID->HighPart = static_cast<LONG>(id >> 32);
  return D3D_OK;
}

} // namespace dxmt
