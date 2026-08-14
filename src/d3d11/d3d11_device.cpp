#include "com/com_guid.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_interfaces.hpp"
#include "d3d11_multithread.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_class_linkage.hpp"
#include "d3d11_inspection.hpp"
#include "d3d11_context.hpp"
#include "d3d11_context_state.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline_cache.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "d3d11_swapchain.hpp"
#include "d3d11_state_object.hpp"
#include "dxgi_interfaces.h"
#include "../d3d10/d3d10_device.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_device.hpp"
#include "dxmt_format.hpp"
#include "ftl.hpp"
#include "d3d11_resource.hpp"
#include "dxgi_object.hpp"
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "config/config.hpp"   /* ml670: d3d11.mipClampBC */
#include "d3d11_4.h"
#include "util_win32_compat.h"

namespace dxmt {

/* ============================ ml652 BC FORMAT CENSUS ============================
 * Read-only. Changes NO behaviour on any device — it exists to turn "implement
 * BCn" into a specific, much smaller format set.
 *
 * Placed at the D3D layer on purpose. The BC identity does survive down to
 * winemetal (to_metal_pixel_format() is where remap_unsupported_bc() runs), but
 * usage, bind flags, CPU access, typeless/view relationships, initial-data
 * presence and logical pitches only exist HERE. Counting at the Metal boundary
 * would lose exactly the information needed to design the abstraction.
 *
 * Aggregated only — never a line per texture. Snapshots every N resources
 * rather than at shutdown, because these runs frequently die abnormally and a
 * shutdown-only census would be lost precisely when it matters.
 *
 * Existing behaviour for reference: iPhone GPUs report
 * supportsBCTextureCompression = NO, so remap_unsupported_bc() swaps BC->RGBA8
 * to make the descriptor validate, and texture_upload_pitch_ok() then DROPS the
 * upload whose BC row pitch Metal would reject. The texture is created and
 * never filled — that is Thumper's static/missing art. */
namespace {

struct BCCensusEntry {
  uint32_t count;
  uint64_t logical_bytes;
  uint32_t max_w, max_h, max_mips, max_array;
  uint32_t usage_default, usage_immutable, usage_dynamic, usage_staging;
  uint32_t with_data, without_data;
  uint32_t cpu_read, cpu_write;
  uint32_t bind_srv, bind_rt, bind_uav;
  uint32_t is_cube;
};

constexpr uint32_t kDxgiMax = 132;
BCCensusEntry g_bc_census[kDxgiMax] {};
/* ml653: runtime-operation counters. ml652 recorded creation-time properties ONLY,
 * which is why "all DEFAULT" could not be narrowed to "only written at creation" —
 * DEFAULT textures may still take UpdateSubresource/copies, and we had no way to see it. */
uint64_t g_bc_op_update = 0, g_bc_op_copyres = 0, g_bc_op_copyregion = 0, g_bc_op_map = 0;
uint32_t g_bc_srv_formats[kDxgiMax] {};
uint64_t g_bc_resources_total = 0;
uint64_t g_bc_subresources_total = 0;
std::mutex g_bc_census_mutex;

inline bool IsBCFormat(uint32_t f) { return (f >= 70 && f <= 84) || (f >= 94 && f <= 99); }

/* 8 bytes per 4x4 block for BC1/BC4; 16 for BC2/BC3/BC5/BC6H/BC7. */
inline uint32_t BCBlockBytes(uint32_t f) {
  if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return 8;
  return 16;
}

const char *BCName(uint32_t f) {
  switch (f) {
  case 70: return "BC1_TYPELESS";   case 71: return "BC1_UNORM";  case 72: return "BC1_UNORM_SRGB";
  case 73: return "BC2_TYPELESS";   case 74: return "BC2_UNORM";  case 75: return "BC2_UNORM_SRGB";
  case 76: return "BC3_TYPELESS";   case 77: return "BC3_UNORM";  case 78: return "BC3_UNORM_SRGB";
  case 79: return "BC4_TYPELESS";   case 80: return "BC4_UNORM";  case 81: return "BC4_SNORM";
  case 82: return "BC5_TYPELESS";   case 83: return "BC5_UNORM";  case 84: return "BC5_SNORM";
  case 94: return "BC6H_TYPELESS";  case 95: return "BC6H_UF16";  case 96: return "BC6H_SF16";
  case 97: return "BC7_TYPELESS";   case 98: return "BC7_UNORM";  case 99: return "BC7_UNORM_SRGB";
  default: return "?";
  }
}

void BCCensusDump(const char *why) {
  /* ml653: DXMT's ERR() is str::format(...), which CONCATENATES its arguments — it is
   * NOT printf. ml652 passed "%s: %llu" templates, so the placeholders printed
   * literally and every value piled up at the end of the line. Interleave literals
   * and values instead. */
  ERR("[bc-census] ml653 ", why, ": ", g_bc_resources_total, " BC resources, ",
      g_bc_subresources_total, " subresources");
  uint64_t grand = 0;
  for (uint32_t f = 0; f < kDxgiMax; f++) {
    auto &e = g_bc_census[f];
    if (!e.count) continue;
    grand += e.logical_bytes;
    ERR("[bc-census] ml653   ", BCName(f), " n=", e.count, " ", (e.logical_bytes >> 10),
        "KB max=", e.max_w, "x", e.max_h, " mips=", e.max_mips, " arr=", e.max_array,
        " cube=", e.is_cube, " | use def=", e.usage_default, " imm=", e.usage_immutable,
        " dyn=", e.usage_dynamic, " stg=", e.usage_staging, " | data y=", e.with_data,
        " n=", e.without_data, " | cpu r=", e.cpu_read, " w=", e.cpu_write,
        " | bind srv=", e.bind_srv, " rt=", e.bind_rt, " uav=", e.bind_uav);
  }
  for (uint32_t f = 0; f < kDxgiMax; f++)
    if (g_bc_srv_formats[f])
      ERR("[bc-census] ml653   SRV view fmt ", f, " (", BCName(f), ") x", g_bc_srv_formats[f]);
  /* THE question this build exists to answer. */
  ERR("[bc-census] ml653   RUNTIME OPS on BC textures: UpdateSubresource=", g_bc_op_update,
      " CopyResource=", g_bc_op_copyres, " CopySubresourceRegion=", g_bc_op_copyregion,
      " Map=", g_bc_op_map);
  ERR("[bc-census] ml653   TOTAL logical BC bytes = ", (grand >> 10), "KB");
}

void BCCensusRecord(const D3D11_TEXTURE2D_DESC1 *d, const D3D11_SUBRESOURCE_DATA *data) {
  const uint32_t f = (uint32_t)d->Format;
  if (!IsBCFormat(f) || f >= kDxgiMax) return;

  const uint32_t bb = BCBlockBytes(f);
  const uint32_t mips = d->MipLevels ? d->MipLevels : 1;
  const uint32_t arr = d->ArraySize ? d->ArraySize : 1;
  uint64_t bytes = 0;
  for (uint32_t m = 0; m < mips; m++) {
    uint32_t w = d->Width >> m, h = d->Height >> m;
    if (!w) w = 1;
    if (!h) h = 1;
    bytes += (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
  }
  bytes *= arr;

  std::lock_guard<std::mutex> lk(g_bc_census_mutex);
  auto &e = g_bc_census[f];
  e.count++;
  e.logical_bytes += bytes;
  if (d->Width > e.max_w) e.max_w = d->Width;
  if (d->Height > e.max_h) e.max_h = d->Height;
  if (mips > e.max_mips) e.max_mips = mips;
  if (arr > e.max_array) e.max_array = arr;
  if (d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) e.is_cube++;
  switch (d->Usage) {
  case D3D11_USAGE_DEFAULT:   e.usage_default++; break;
  case D3D11_USAGE_IMMUTABLE: e.usage_immutable++; break;
  case D3D11_USAGE_DYNAMIC:   e.usage_dynamic++; break;
  case D3D11_USAGE_STAGING:   e.usage_staging++; break;
  }
  if (data) e.with_data++; else e.without_data++;
  if (d->CPUAccessFlags & D3D11_CPU_ACCESS_READ)  e.cpu_read++;
  if (d->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) e.cpu_write++;
  if (d->BindFlags & D3D11_BIND_SHADER_RESOURCE)  e.bind_srv++;
  if (d->BindFlags & D3D11_BIND_RENDER_TARGET)    e.bind_rt++;
  if (d->BindFlags & D3D11_BIND_UNORDERED_ACCESS) e.bind_uav++;

  g_bc_resources_total++;
  g_bc_subresources_total += (uint64_t)mips * arr;
  if ((g_bc_resources_total % 64) == 0) BCCensusDump("periodic");
}

} // namespace

/* ---- ml670: PHYSICAL MIP CLAMP for BC textures -------------------------
 *
 * A15 cannot sample BC, so DXMT remaps BC -> RGBA8 and decodes at upload. That
 * is lossless but costs 4-8x the memory: Book of the Dead's census reaches
 * 351,197 KB of LOGICAL BC, which lands as ~1.79 GB of RGBA8 backing and
 * jetsams the app mid-load.
 *
 * Dropping the top mip leaves 1/4 of the pixels, so that backing falls to
 * ~0.45 GB. The texture keeps its full logical identity from the app's point of
 * view -- we shrink the PHYSICAL resource and then translate views so shader
 * mip indices still mean what the app intended.
 *
 * Eligibility is deliberately narrow. Anything that could observe the physical
 * layout (render target, UAV, CPU access, staging/dynamic, no initial data,
 * single mip) is left completely alone.
 *
 * ⚠️ D3D11 subresource index = mip + slice * MipLevels. Dropping a mip shifts
 * EVERY index, so initial data must be repacked per array slice -- a plain
 * pointer bump silently corrupts slice 1 onward. Cubes are just ArraySize 6 and
 * fall out of the same per-slice loop.
 *
 * ⚠️ USAGE_DEFAULT permits UpdateSubresource/CopySubresourceRegion, whose
 * indices we do NOT translate. This game's census shows zero such calls, but a
 * different one could, so those paths LOUDLY report a clamped target instead of
 * silently writing the wrong mip. Restricting to IMMUTABLE instead would be
 * safe by contract but would clamp nothing -- all 128 observed textures are
 * DEFAULT. */
namespace {

std::mutex g_mipclamp_mutex;
std::unordered_map<const void *, uint32_t> g_mipclamp;   /* resource -> levels dropped */
uint64_t g_mipclamp_saved_kb = 0;
uint32_t g_mipclamp_count = 0;

uint32_t MipClampLevels() {
  static int cached = -1;
  if (cached < 0)
    cached = std::max(0, std::min(4, Config::getInstance().getOption<int>("d3d11.mipClampBC", 0)));
  return (uint32_t)cached;
}

bool MipClampEligible(const D3D11_TEXTURE2D_DESC1 *d, const D3D11_SUBRESOURCE_DATA *data) {
  if (!MipClampLevels() || !data) return false;
  if (!IsBCFormat((uint32_t)d->Format)) return false;
  if (d->Usage != D3D11_USAGE_DEFAULT && d->Usage != D3D11_USAGE_IMMUTABLE) return false;
  if (d->BindFlags != D3D11_BIND_SHADER_RESOURCE) return false;   /* no RT/UAV/DS */
  if (d->CPUAccessFlags) return false;
  if (d->MipLevels < 2) return false;              /* MipLevels==0 means "generate"; also excluded */
  if (d->SampleDesc.Count > 1) return false;
  if (d->Width < 8 || d->Height < 8) return false; /* keep the clamped top >= one 4x4 block */
  return true;
}

uint32_t MipClampFor(const void *res) {
  std::lock_guard<std::mutex> lk(g_mipclamp_mutex);
  auto it = g_mipclamp.find(res);
  return it == g_mipclamp.end() ? 0 : it->second;
}

} // namespace

extern "C" void MipClampWarnRuntimeOp(const void *res, const char *op) {
  if (!MipClampFor(res)) return;
  static int warned;
  if (warned++ < 16)
    ERR("[mip-clamp] ml670 ", op, " on a CLAMPED resource ", res,
        " -- subresource indices are NOT translated on this path; expect a wrong mip. "
        "Add index translation or widen the eligibility exclusion.");
}

/* ml653: reachable from d3d11_context_impl.cpp (different TU). */
extern "C" void BCCensusRecordSRV(unsigned int fmt) {
  if (!IsBCFormat(fmt) || fmt >= kDxgiMax) return;
  std::lock_guard<std::mutex> lk(g_bc_census_mutex);
  g_bc_srv_formats[fmt]++;
}

extern "C" void BCCensusRecordOp(unsigned int fmt, int op) {
  if (!IsBCFormat(fmt)) return;
  std::lock_guard<std::mutex> lk(g_bc_census_mutex);
  switch (op) {
  case 0: g_bc_op_update++; break;
  case 1: g_bc_op_copyres++; break;
  case 2: g_bc_op_copyregion++; break;
  case 3: g_bc_op_map++; break;
  }
}
/* ========================== end ml652 BC FORMAT CENSUS ========================= */


const GUID kRenderdocUUID = {0xa7aa6116,
                             0x9c8d,
                             0x4bba,
                             {0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78}};
const GUID kPixUUID = {0x9f251514,
                       0x9d4d,
                       0x4902,
                       {0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5}};
const GUID kGpaUUID = {0xccffef16,
                       0x7b69,
                       0x468f,
                       {0xbc, 0xe3, 0xcd, 0x95, 0x33, 0x69, 0xa3, 0x9a}};

class MTLD3D11DeviceImpl final : public MTLD3D11Device, public IMTLD3D11DeviceExt {
friend class MTLD3D11DXGIDevice;
public:
  MTLD3D11DeviceImpl(
      MTLDXGIObject<IMTLDXGIDevice> *container, IMTLDXGIAdapter *pAdapter, D3D_FEATURE_LEVEL FeatureLevel,
      UINT FeatureFlags, Device &device
  ) :
      container_(container),
      feature_level_(FeatureLevel),
      feature_flags_(FeatureFlags),
      features_(container->GetMTLDevice()),
      sampler_states_(this),
      rasterizer_states_(this),
      depthstencil_states_(this),
      device_(device),
      d3dmt_(static_cast<ID3D11Device *>(this), mutex) {
    commandlist_pool_ = InitializeCommandListPool(this);
    pipeline_cache_ = InitializePipelineCache(this);
    context_ = InitializeImmediateContext(this, device_.queue());
    d3d10_ = std::make_unique<MTLD3D10Device>(this, context_.get());
    is_traced_ = !!::GetModuleHandle("dxgitrace.dll");
    format_inspector_.Inspect(GetMTLDevice());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return container_->Release(); }

  bool IsTraced() override { return is_traced_; }

  HRESULT STDMETHODCALLTYPE
  CreateBuffer(const D3D11_BUFFER_DESC *pDesc,
               const D3D11_SUBRESOURCE_DATA *pInitialData,
               ID3D11Buffer **ppBuffer) override {
    InitReturnPtr(ppBuffer);

    if (pDesc->ByteWidth == 0 && !(pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILE_POOL))
      return E_INVALIDARG; 

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
      case D3D11_USAGE_DYNAMIC:
        return dxmt::CreateBuffer(this, pDesc, pInitialData, ppBuffer);
      case D3D11_USAGE_STAGING:
        return CreateStagingBuffer(this, pDesc, pInitialData, ppBuffer);
      default:
        DXMT_UNREACHABLE
      }
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture1D **ppTexture1D) override {

    InitReturnPtr(ppTexture1D);

    if (!pDesc)
      return E_INVALIDARG;

    if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED)
      return E_INVALIDARG; // not supported yet

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture1D(this, pDesc, pInitialData, ppTexture1D);
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture1D(this, pDesc, pInitialData, ppTexture1D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture2D **ppTexture2D) override {
    D3D11_TEXTURE2D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture2D1(&desc1, pInitialData,
                            (ID3D11Texture2D1 **)ppTexture2D);
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture3D **ppTexture3D) override {
    D3D11_TEXTURE3D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture3D1(&desc1, pInitialData,
                            (ID3D11Texture3D1 **)ppTexture3D);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
      ID3D11ShaderResourceView **ppSRView) override {
    if (pDesc) {
      D3D11_SHADER_RESOURCE_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateShaderResourceView1(pResource, &desc1,
                                       (ID3D11ShaderResourceView1 **)ppSRView);
    }
    return CreateShaderResourceView1(pResource, nullptr,
                                     (ID3D11ShaderResourceView1 **)ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      ID3D11UnorderedAccessView **ppUAView) override {
    if (pDesc) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateUnorderedAccessView1(
          pResource, &desc1, (ID3D11UnorderedAccessView1 **)ppUAView);
    }
    return CreateUnorderedAccessView1(pResource, nullptr,
                                      (ID3D11UnorderedAccessView1 **)ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
      ID3D11RenderTargetView **ppRTView) override {
    if (pDesc) {
      D3D11_RENDER_TARGET_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateRenderTargetView1(pResource, &desc1,
                                     (ID3D11RenderTargetView1 **)ppRTView);
    }
    return CreateRenderTargetView1(pResource, nullptr,
                                   (ID3D11RenderTargetView1 **)ppRTView);
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(
      ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
      ID3D11DepthStencilView **ppDepthStencilView) override {
    InitReturnPtr(ppDepthStencilView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppDepthStencilView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateDepthStencilView(pDesc, ppDepthStencilView);
  }

  HRESULT STDMETHODCALLTYPE CreateInputLayout(
      const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements,
      const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
      ID3D11InputLayout **ppInputLayout) override {
    InitReturnPtr(ppInputLayout);

    if (!pInputElementDescs)
      return E_INVALIDARG;
    if (!pShaderBytecodeWithInputSignature)
      return E_INVALIDARG;

    // TODO: must get shader reflection info

    if (!ppInputLayout) {
      return S_FALSE;
    }

    return pipeline_cache_->AddInputLayout(
        pShaderBytecodeWithInputSignature, pInputElementDescs, NumElements,
        (IMTLD3D11InputLayout **)ppInputLayout);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11VertexShader **ppVertexShader) override {
    InitReturnPtr(ppVertexShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddVertexShader(pShaderBytecode, BytecodeLength,
                                            ppVertexShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                       ID3D11ClassLinkage *pClassLinkage,
                       ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppGeometryShader)
      return S_FALSE;

    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(
      const void *pShaderBytecode, SIZE_T BytecodeLength,
      const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries,
      const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream,
      ID3D11ClassLinkage *pClassLinkage,
      ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (NumEntries > 0 && RasterizedStream == D3D11_SO_NO_RASTERIZED_STREAM &&
        ((char *)pShaderBytecode)[0] == 'D' &&
        ((char *)pShaderBytecode)[1] == 'X' &&
        ((char *)pShaderBytecode)[2] == 'B' &&
        ((char *)pShaderBytecode)[3] == 'C') {
      // FIXME: ensure the input shader is a vertex shader
      WARN("Emulate stream output");

      Com<IMTLD3D11StreamOutputLayout> so_layout;
      HRESULT hr = pipeline_cache_->AddStreamOutputLayout(
          pShaderBytecode, NumEntries, pSODeclaration, NumStrides,
          pBufferStrides, RasterizedStream, &so_layout);
      if (FAILED(hr))
        return hr;
      return so_layout->QueryInterface(IID_PPV_ARGS(ppGeometryShader));
    }
    ERR("CreateGeometryShaderWithStreamOutput: not supported, expect problem");
    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                    ID3D11ClassLinkage *pClassLinkage,
                    ID3D11PixelShader **ppPixelShader) override {
    InitReturnPtr(ppPixelShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddPixelShader(pShaderBytecode, BytecodeLength,
                                           ppPixelShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                   ID3D11ClassLinkage *pClassLinkage,
                   ID3D11HullShader **ppHullShader) override {
    InitReturnPtr(ppHullShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppHullShader)
      return S_FALSE;

    return pipeline_cache_->AddHullShader(pShaderBytecode, BytecodeLength,
                                          ppHullShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11DomainShader **ppDomainShader) override {
    InitReturnPtr(ppDomainShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppDomainShader)
      return S_FALSE;

    return pipeline_cache_->AddDomainShader(pShaderBytecode, BytecodeLength,
                                            ppDomainShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                      ID3D11ClassLinkage *pClassLinkage,
                      ID3D11ComputeShader **ppComputeShader) override {
    InitReturnPtr(ppComputeShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddComputeShader(pShaderBytecode, BytecodeLength,
                                             ppComputeShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) override {
    *ppLinkage = ref(new MTLD3D11ClassLinkage(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc,
                   ID3D11BlendState **ppBlendState) override {
    ID3D11BlendState1 *pBlendState1;
    D3D11_BLEND_DESC1 desc;
    desc.AlphaToCoverageEnable = pBlendStateDesc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = pBlendStateDesc->IndependentBlendEnable;

    for (uint32_t i = 0; i < 8; i++) {
      desc.RenderTarget[i].BlendEnable =
          pBlendStateDesc->RenderTarget[i].BlendEnable;
      desc.RenderTarget[i].LogicOpEnable = FALSE;
      desc.RenderTarget[i].LogicOp = D3D11_LOGIC_OP_NOOP;
      desc.RenderTarget[i].SrcBlend = pBlendStateDesc->RenderTarget[i].SrcBlend;
      desc.RenderTarget[i].DestBlend =
          pBlendStateDesc->RenderTarget[i].DestBlend;
      desc.RenderTarget[i].BlendOp = pBlendStateDesc->RenderTarget[i].BlendOp;
      desc.RenderTarget[i].SrcBlendAlpha =
          pBlendStateDesc->RenderTarget[i].SrcBlendAlpha;
      desc.RenderTarget[i].DestBlendAlpha =
          pBlendStateDesc->RenderTarget[i].DestBlendAlpha;
      desc.RenderTarget[i].BlendOpAlpha =
          pBlendStateDesc->RenderTarget[i].BlendOpAlpha;
      desc.RenderTarget[i].RenderTargetWriteMask =
          pBlendStateDesc->RenderTarget[i].RenderTargetWriteMask;
    }
    auto hr = CreateBlendState1(&desc, &pBlendState1);
    *ppBlendState = pBlendState1;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilState(
      const D3D11_DEPTH_STENCIL_DESC *pDesc,
      ID3D11DepthStencilState **ppDepthStencilState) override {
    return depthstencil_states_.CreateStateObject(
        pDesc, (IMTLD3D11DepthStencilState **)ppDepthStencilState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc,
                        ID3D11RasterizerState **ppRasterizerState) override {
    ID3D11RasterizerState2 *pRasterizerState;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, &pRasterizerState);
    *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc,
                     ID3D11SamplerState **ppSamplerState) override {
    return sampler_states_.CreateStateObject(
        pSamplerDesc, (D3D11SamplerState **)ppSamplerState);
  }

  HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *pQueryDesc,
                                        ID3D11Query **ppQuery) override {
    InitReturnPtr(ppQuery);

    if (!pQueryDesc)
      return E_INVALIDARG;

    switch (pQueryDesc->Query) {
    case D3D11_QUERY_EVENT:
      *ppQuery = ref(new MTLD3D11EventQueryImpl<BOOL>(this, pQueryDesc));
      return S_OK;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
      return CreateOcculusionQuery(this, pQueryDesc, ppQuery);
    case D3D11_QUERY_TIMESTAMP:
      *ppQuery = ref(new MTLD3D11EventQueryImpl<UINT64>(this, pQueryDesc));
      return S_OK;
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      *ppQuery = ref(new MTLD3D11EventQueryImpl<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      *ppQuery =
          ref(new MTLD3D11DummyQuery<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(
              this, pQueryDesc));
      return S_OK;
    }
    default:
      ERR("CreateQuery: query type not implemented: ", pQueryDesc->Query);
      return E_NOTIMPL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc,
                  ID3D11Predicate **ppPredicate) override {
    return CreateQuery(pPredicateDesc,
                       reinterpret_cast<ID3D11Query **>(ppPredicate));
  }

  HRESULT STDMETHODCALLTYPE
  CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc,
                ID3D11Counter **ppCounter) override {
    InitReturnPtr(ppCounter);
    WARN("Not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext(
      UINT ContextFlags,
      ID3D11DeviceContext **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTexture(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
      CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override {

    if (pFormatSupport) {
      *pFormatSupport = 0;
    }

    if (Format == DXGI_FORMAT_UNKNOWN) {
      *pFormatSupport =
          D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
      return S_OK;
    }

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, metal_format))) {
      return E_INVALIDARG;
    }

    UINT outFormatSupport = 0;

    if (metal_format.PixelFormat) {
      // All graphics and compute kernels can read or sample a texture with any pixel format.
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
                          D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;

      /* UNCHECKED */
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D |
                          D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_MIP |
                          D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | // ?
                          D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;

      if (!(metal_format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_BUFFER;
      }

      if (metal_format.Flag & MTL_DXGI_FORMAT_BACKBUFFER) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_DISPLAY;
      }
    }

    if (metal_format.AttributeFormat) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    }

    if (metal_format.PixelFormat == WMTPixelFormatR32Uint ||
        metal_format.PixelFormat == WMTPixelFormatR16Uint) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
    }

    auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

    if (any_bit_set(Capability & FormatCapability::Color)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Blend)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    }

    if (any_bit_set(Capability & FormatCapability::DepthStencil)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL |
                          D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON;
    }

    if (any_bit_set(Capability & FormatCapability::Resolve)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    }

    if (any_bit_set(Capability & FormatCapability::MSAA)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Write)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    }

    if (Format == DXGI_FORMAT_R32_FLOAT || Format == DXGI_FORMAT_R32_UINT ||
        Format == DXGI_FORMAT_R32_SINT || Format == DXGI_FORMAT_R32G32_FLOAT ||
        Format == DXGI_FORMAT_R32G32_UINT ||
        Format == DXGI_FORMAT_R32G32_SINT ||
        Format == DXGI_FORMAT_R32G32B32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32_UINT ||
        Format == DXGI_FORMAT_R32G32B32_SINT ||
        Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32A32_UINT ||
        Format == DXGI_FORMAT_R32G32B32A32_SINT)
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SO_BUFFER;

    if (pFormatSupport) {
      *pFormatSupport = outFormatSupport;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(
      DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) override {
    return CheckMultisampleQualityLevels1(Format, SampleCount, 0,
                                          pNumQualityLevels);
  }

  void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo)
      override { // We basically don't support counters
    pCounterInfo->LastDeviceDependentCounter = D3D11_COUNTER(0);
    pCounterInfo->NumSimultaneousCounters = 0;
    pCounterInfo->NumDetectableParallelUnits = 0;
  }

  HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc,
                                         D3D11_COUNTER_TYPE *pType,
                                         UINT *pActiveCounters, LPSTR szName,
                                         UINT *pNameLength, LPSTR szUnits,
                                         UINT *pUnitsLength,
                                         LPSTR szDescription,
                                         UINT *pDescriptionLength) override {
    WARN("Not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    // Format support queries are special in that they use in-out
    // structs
    case D3D11_FEATURE_FORMAT_SUPPORT: {
      auto info =
          static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;

      return CheckFormatSupport(info->InFormat, &info->OutFormatSupport);
    }
    case D3D11_FEATURE_FORMAT_SUPPORT2: {
      auto info = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2 *>(
          pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;
      info->OutFormatSupport2 = 0;

      if (info->InFormat == DXGI_FORMAT_UNKNOWN) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC metal_format;
      if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), info->InFormat,
                                    metal_format))) {
        return E_INVALIDARG;
      }
      auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

      if (any_bit_set(Capability & FormatCapability::TextureBufferRead)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferReadWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                                   D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::Atomic)) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
      }

#ifndef DXMT_NO_PRIVATE_API
      if (any_bit_set(Capability & FormatCapability::Blend)) {
        /* UNCHECKED */
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP;
      }
#endif

      if (any_bit_set(Capability & FormatCapability::Sparse)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_TILED;
      }

      return S_OK;
    }
    default:
      // For everything else, we can use the device feature struct
      // that we already initialized during device creation.
      return features_.GetFeatureData(Feature, FeatureSupportDataSize,
                                       pFeatureSupportData);
    }
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize,
                                           void *pData) override {
    return container_->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize,
                                           const void *pData) override {
    return container_->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override {
    return container_->SetPrivateDataInterface(guid, pData);
  }

  D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override {
    return feature_level_;
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return feature_flags_ & 0x7fffffff; }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    // unless we are deal with eGPU, this method should awalys return S_OK?
    return S_OK;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  UINT STDMETHODCALLTYPE GetExceptionMode() override {
    ERR("Not implemented");
    return 0;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext1(ID3D11DeviceContext1 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext1(
      UINT ContextFlags,
      ID3D11DeviceContext1 **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext1 *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
      CreateBlendState1(const D3D11_BLEND_DESC1 *pBlendStateDesc,
                        ID3D11BlendState1 **ppBlendState) override {
    return pipeline_cache_->AddBlendState(pBlendStateDesc,
                                          (IMTLD3D11BlendState **)ppBlendState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState1(const D3D11_RASTERIZER_DESC1 *pRasterizerDesc,
                         ID3D11RasterizerState1 **ppRasterizerState) override {
    ID3D11RasterizerState2 *pRasterizerState;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = pRasterizerDesc->ForcedSampleCount;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, &pRasterizerState);
    *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDeviceContextState(
      UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, REFIID EmulatedInterface,
      D3D_FEATURE_LEVEL *pChosenFeatureLevel,
      ID3DDeviceContextState **ppContextState) override {
    InitReturnPtr(ppContextState);

    // TODO: validation

    if (ppContextState == nullptr) {
      return S_FALSE;
    }
    *ppContextState = ref(new MTLD3D11DeviceContextState(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource1(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTextureFromNtHandle(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID ReturnedInterface, void **ppResource)
      override {
    return ImportSharedTextureByName(this, lpName, dwDesiredAccess, ReturnedInterface, ppResource);
  }

  void STDMETHODCALLTYPE
  GetImmediateContext2(ID3D11DeviceContext2 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE
  CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2 **ppDeferredContext) override {
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext2 *>(ppDeferredContext3);
    return hr;
  }

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D11Resource *resource, UINT *tile_count,
      D3D11_PACKED_MIP_DESC *mip_desc, D3D11_TILE_SHAPE *tile_shape,
      UINT *subresource_tiling_count, UINT first_subresource_tiling,
      D3D11_SUBRESOURCE_TILING *subresource_tiling) override{IMPLEMENT_ME}

  HRESULT STDMETHODCALLTYPE
      CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount,
                                     UINT Flags,
                                     UINT *pNumQualityLevels) override {
    if (Flags) {
      IMPLEMENT_ME;
    }
    *pNumQualityLevels = 0;
    MTL_DXGI_FORMAT_DESC desc;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, desc)) ||
        desc.PixelFormat == WMTPixelFormatInvalid) {
      return E_INVALIDARG;
    }

    // MSDN:
    // FEATURE_LEVEL_11_0 devices are required to support 4x MSAA for all render
    // target formats, and 8x MSAA for all render target formats except
    // R32G32B32A32 formats.

    // seems some pixel format doesn't support MSAA
    // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf

    if (GetMTLDevice().supportsTextureSampleCount(SampleCount)) {
      *pNumQualityLevels = 1; // always 1: in metal there is no concept of
                              // Quality Level (so is it in vulkan iirc)
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D1(const D3D11_TEXTURE2D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture2D1 **ppTexture2D) override {
    InitReturnPtr(ppTexture2D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    BCCensusRecord(pDesc, pInitialData);   /* ml652: read-only, no behaviour change */

    /* ml670: shrink the PHYSICAL resource by dropping top mips. */
    D3D11_TEXTURE2D_DESC1 clamped_desc;
    std::vector<D3D11_SUBRESOURCE_DATA> clamped_data;
    uint32_t clamp = 0;
    if (MipClampEligible(pDesc, pInitialData)) {
      const uint32_t mips = pDesc->MipLevels;
      const uint32_t arr = pDesc->ArraySize ? pDesc->ArraySize : 1;
      clamp = std::min(MipClampLevels(), mips - 1);          /* always keep >= 1 mip */
      while (clamp && ((pDesc->Width >> clamp) < 4 || (pDesc->Height >> clamp) < 4))
        clamp--;                                             /* never go below one block */
      if (clamp) {
        clamped_desc = *pDesc;
        clamped_desc.Width  = std::max(1u, pDesc->Width  >> clamp);
        clamped_desc.Height = std::max(1u, pDesc->Height >> clamp);
        clamped_desc.MipLevels = mips - clamp;
        /* index = mip + slice * MipLevels, so repack per slice -- a flat
         * pointer bump would corrupt every slice after the first. */
        clamped_data.resize((size_t)clamped_desc.MipLevels * arr);
        for (uint32_t a = 0; a < arr; a++)
          for (uint32_t m = 0; m < clamped_desc.MipLevels; m++)
            clamped_data[m + (size_t)a * clamped_desc.MipLevels] =
                pInitialData[(m + clamp) + (size_t)a * mips];
        pDesc = &clamped_desc;
        pInitialData = clamped_data.data();
      }
    }

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE: {
        HRESULT hr = CreateDeviceTexture2D(this, pDesc, pInitialData, ppTexture2D);
        if (SUCCEEDED(hr) && clamp && ppTexture2D && *ppTexture2D) {
          std::lock_guard<std::mutex> lk(g_mipclamp_mutex);
          g_mipclamp[(const void *)*ppTexture2D] = clamp;
          g_mipclamp_count++;
          if (g_mipclamp_count <= 8 || (g_mipclamp_count % 64) == 0)
            ERR("[mip-clamp] ml670 #", g_mipclamp_count, " dropped ", clamp,
                " mip(s): ", clamped_desc.Width << clamp, "x", clamped_desc.Height << clamp,
                " -> ", clamped_desc.Width, "x", clamped_desc.Height,
                " mips ", clamped_desc.MipLevels + clamp, "->", clamped_desc.MipLevels,
                " arr=", clamped_desc.ArraySize);
        }
        return hr;
      }
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture2D(this, pDesc, pInitialData, ppTexture2D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D1(const D3D11_TEXTURE3D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture3D1 **ppTexture3D) override {
    InitReturnPtr(ppTexture3D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_DYNAMIC:
        return CreateDynamicTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture3D(this, pDesc, pInitialData, ppTexture3D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState2(const D3D11_RASTERIZER_DESC2 *pRasterizerDesc,
                         ID3D11RasterizerState2 **ppRasterizerState) override {
    return rasterizer_states_.CreateStateObject(
        pRasterizerDesc, (IMTLD3D11RasterizerState **)ppRasterizerState);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
      ID3D11ShaderResourceView1 **ppSRView) override {
    /* ml653: ml652 declared g_bc_srv_formats but NEVER incremented it, so the
     * typeless-resource vs view-format question went unanswered. */
    if (pDesc) BCCensusRecordSRV((unsigned int)pDesc->Format);

    InitReturnPtr(ppSRView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppSRView)
      return S_FALSE;

    /* ml670: the app still describes views in LOGICAL mip numbers, but the
     * physical resource lost its top level(s). Shift MostDetailedMip down and
     * shrink MipLevels to match. A null pDesc needs nothing -- the default view
     * already spans exactly the mips the resource physically has. */
    D3D11_SHADER_RESOURCE_VIEW_DESC1 xlated;
    if (pDesc) {
      uint32_t clamp = MipClampFor((const void *)pResource);
      if (clamp && (pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ||
                    pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY ||
                    pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE ||
                    pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY)) {
        xlated = *pDesc;
        /* every dimension above keeps MostDetailedMip/MipLevels at the same
         * offsets in the union, so Texture2D is a valid accessor for all. */
        uint32_t most = xlated.Texture2D.MostDetailedMip;
        uint32_t count = xlated.Texture2D.MipLevels;
        xlated.Texture2D.MostDetailedMip = most > clamp ? most - clamp : 0;
        if (count != (uint32_t)-1) {
          uint32_t drop = most < clamp ? clamp - most : 0;
          xlated.Texture2D.MipLevels = count > drop ? count - drop : 1;
        }
        pDesc = &xlated;
      }
    }

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateShaderResourceView(pDesc, ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
      ID3D11UnorderedAccessView1 **ppUAView) override {
    InitReturnPtr(ppUAView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppUAView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateUnorderedAccessView(pDesc, ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
      ID3D11RenderTargetView1 **ppRTView) override {
    InitReturnPtr(ppRTView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppRTView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateRenderTargetView(pDesc, ppRTView);
  }

  HRESULT STDMETHODCALLTYPE CreateQuery1(const D3D11_QUERY_DESC1 *desc,
                                         ID3D11Query1 **query) override {
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  GetImmediateContext3(ID3D11DeviceContext3 **ppImmediateContext) override{
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext3(
    UINT ContextFlags, ID3D11DeviceContext3 **ppDeferredContext) override {
    *ppDeferredContext = std::move(dxmt::CreateDeferredContext(this, ContextFlags));
    return S_OK;
  }

  void STDMETHODCALLTYPE WriteToSubresource(ID3D11Resource *dst_resource,
                                            UINT dst_subresource,
                                            const D3D11_BOX *dst_box,
                                            const void *src_data,
                                            UINT src_row_pitch,
                                            UINT src_depth_pitch) override {
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  ReadFromSubresource(void *dst_data, UINT dst_row_pitch, UINT dst_depth_pitch,
                      ID3D11Resource *src_resource, UINT src_subresource,
                      const D3D11_BOX *src_box) override{IMPLEMENT_ME}

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return container_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return container_->GetLocalD3DKMT();
  }

  HRESULT
  CreateGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGraphicsPipeline **ppPipeline) override {
    pipeline_cache_->GetGraphicsPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                        MTLCompiledComputePipeline **ppPipeline) override {
    pipeline_cache_->GetComputePipeline(pDesc, ppPipeline);
    return S_OK;
  };

  virtual HRESULT
  CreateGeometryPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGeometryPipeline **ppPipeline) override {
    pipeline_cache_->GetGeometryPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateTessellationMeshPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledTessellationMeshPipeline **ppPipeline) override {
    pipeline_cache_->GetTessellationPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  Device &GetDXMTDevice() override { return device_; };

  void CreateCommandList(ID3D11CommandList** pCommandList) final {
    commandlist_pool_->CreateCommandList(pCommandList);
  };

  virtual void STDMETHODCALLTYPE SetShaderExtensionSlot(UINT Slot) final {
    // TODO
  };

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };

  virtual IMTLD3D11DeviceContext *GetImmediateContextPrivate() final {
    return context_.get();
  };

  virtual unsigned int GetDirectXVersion() final {
    return feature_flags_ & 0x80000000 ? 10 : 11;
  };

  virtual HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE Event,
                                                               DWORD *pCookie) final {
    // no device to remove
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD Cookie) final {
    // just do nothing
  };

  virtual HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE Handle, REFIID riid,
                                                    void **ppFence) final {
    return dxmt::OpenSharedFence(this, Handle, riid, ppFence);
  };

  virtual HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue,
                                                D3D11_FENCE_FLAG Flags,
                                                REFIID riid, void **ppFence) final {
    if (feature_flags_ & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)
      return E_FAIL;
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

private:
  MTLDXGIObject<IMTLDXGIDevice> *container_;
  D3D_FEATURE_LEVEL feature_level_;
  UINT feature_flags_;
  MTLD3D11Inspection features_;
  FormatCapabilityInspector format_inspector_;

  bool is_traced_;

  StateObjectCache<D3D11_SAMPLER_DESC, D3D11SamplerState> sampler_states_;
  StateObjectCache<D3D11_RASTERIZER_DESC2, IMTLD3D11RasterizerState> rasterizer_states_;
  StateObjectCache<D3D11_DEPTH_STENCIL_DESC, IMTLD3D11DepthStencilState> depthstencil_states_;

  std::unique_ptr<MTLD3D11CommandListPoolBase> commandlist_pool_;
  std::unique_ptr<MTLD3D11PipelineCacheBase> pipeline_cache_;

  Device& device_;
  /** ensure destructor called first */
  std::unique_ptr<MTLD3D11DeviceContextBase> context_;
  std::unique_ptr<MTLD3D10Device> d3d10_;
  D3D11Multithread d3dmt_;
};

/**
 * \brief D3D11 device container
 *
 * Stores all the objects that contribute to the D3D11
 * device implementation, including the DXGI device.
 */
class MTLD3D11DXGIDevice final : public MTLDXGIObject<IMTLDXGIDevice> {
public:
  friend class MTLDXGIMetalLayerFactory;

  MTLD3D11DXGIDevice(std::unique_ptr<Device> &&device, IMTLDXGIAdapter *adapter,
                     D3D_FEATURE_LEVEL feature_level, UINT feature_flags)
      : adapter_(adapter), device(std::move(device)),
        cmd_queue_(this->device->queue()),
        d3d11_device_(this, adapter, feature_level, feature_flags,
                      *this->device.get()) {
    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("Failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~MTLD3D11DXGIDevice() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) ||
        riid == __uuidof(IDXGIDevice2) || riid == __uuidof(IDXGIDevice3) ||
        riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) ||
        riid == __uuidof(ID3D11Device2) || riid == __uuidof(ID3D11Device3) ||
        riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5)) {
      *ppvObject = ref_and_cast<ID3D11Device>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1)) {
      *ppvObject = ref_and_cast<ID3D10Device>(d3d11_device_.d3d10_.get());
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11DeviceExt)) {
      *ppvObject = ref_and_cast<IMTLD3D11DeviceExt>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref_and_cast<ID3D11Multithread>(&d3d11_device_.d3dmt_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Debug))
      return E_NOINTERFACE;

    if (riid == kRenderdocUUID || riid == kPixUUID || riid == kGpaUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(IMTLDXGIDevice), riid)) {
      WARN("D3D11Device: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (pAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pAdapter = adapter_.ref();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                DXGI_USAGE usage, const DXGI_SHARED_RESOURCE *shared_resource,
                IDXGISurface **surface) override{IMPLEMENT_ME}

  HRESULT STDMETHODCALLTYPE
      QueryResourceResidency(IUnknown *const *resources,
                             DXGI_RESIDENCY *residency,
                             UINT resource_count) override{IMPLEMENT_ME}

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override {
    if (Priority < -7 || Priority > 7)
      return E_INVALIDARG;

    WARN("SetGPUThreadPriority: Ignoring");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override {
    *pPriority = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
    cmd_queue_.SetMaxLatency(MaxLatency);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override {
    if (pMaxLatency) {
      *pMaxLatency = cmd_queue_.GetMaxLatency();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
      OfferResources(UINT NumResources, IDXGIResource *const *ppResources,
                     DXGI_OFFER_RESOURCE_PRIORITY Priority) override {
    // stub
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources,
                                             IDXGIResource *const *ppResources,
                                             WINBOOL *pDiscarded) override {
    // stub
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) override {
    return E_FAIL;
  }

  void STDMETHODCALLTYPE Trim() override { WARN("DXGIDevice3::Trim: no-op"); };

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) override {

    return dxmt::CreateSwapChain(pFactory, &d3d11_device_, hWnd, pDesc, pFullscreenDesc,
                                 ppSwapChain);
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  Com<IMTLDXGIAdapter> adapter_;
  D3DKMT_HANDLE local_kmt_ = 0;
  std::unique_ptr<Device> device;
  CommandQueue &cmd_queue_;
  MTLD3D11DeviceImpl d3d11_device_;
};

Com<IMTLDXGIDevice> CreateD3D11Device(std::unique_ptr<Device> &&device,
                                      IMTLDXGIAdapter *adapter,
                                      D3D_FEATURE_LEVEL feature_level,
                                      UINT feature_flags) {
  return Com<IMTLDXGIDevice>::transfer(new MTLD3D11DXGIDevice(
      std::move(device), adapter, feature_level, feature_flags));
};
} // namespace dxmt