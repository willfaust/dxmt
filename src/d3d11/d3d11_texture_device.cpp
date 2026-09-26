#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_enumerable.hpp"
#include "d3d11_view.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_texture.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#include "config/config.hpp"   /* ml675: d3d11.mipClampBC */
#include "d3d11_mip_clamp_policy.hpp"   /* ml2000: d3d11.mipClampAuto */
#include "util_env.hpp"
#include "winemetal.h"
#include <atomic>
#include <cstring>

namespace dxmt {

#pragma region DeviceTexture

template <typename tag_texture>
class DeviceTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  Rc<Texture> underlying_texture_;
  Rc<RenamableTexturePool> renamable_;
  /* ml675: top mip levels dropped from the PHYSICAL Metal texture. The D3D
   * descriptor this object reports stays LOGICAL, so GetDesc keeps telling the
   * application the size it asked for. ml670 got this wrong -- it handed the
   * shrunken descriptor to the resource, so a 2048x2048 texture reported itself
   * as 512x512 and any UI computing layout from GetDesc came out wrong. */
  uint32_t mip_bias_ = 0;
  float min_lod = 0.0;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DeviceTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  private:
    TextureViewKey view_key_;

  public:
    TextureSRV(TextureViewKey view_key,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice), view_key_(view_key) {}

    Rc<Buffer> buffer() final { return {}; };
    Rc<Texture> texture() final { return this->resource->underlying_texture_; };
    unsigned viewId() final { return view_key_;};
    BufferSlice bufferSlice() final { return {};}
  };

  using UAVBase =
      TResourceViewBase<tag_unordered_access_view<DeviceTexture<tag_texture>>>;
  class TextureUAV : public UAVBase {
  private:
    TextureViewKey view_key_;

  public:
    TextureUAV(TextureViewKey view_key,
               const tag_unordered_access_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : UAVBase(pDesc, pResource, pDevice), view_key_(view_key){}

    Rc<Buffer> buffer() final { return {}; };
    Rc<Texture> texture() final { return this->resource->underlying_texture_; };
    unsigned viewId() final { return view_key_;};
    BufferSlice bufferSlice() final { return {};}
    Rc<Buffer> counter() final { return {}; };
  };

  using RTVBase =
      TResourceViewBase<tag_render_target_view<DeviceTexture<tag_texture>>>;
  class TextureRTV : public RTVBase {
  private:
    TextureViewKey view_key_;
    WMTPixelFormat view_format_;
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;

  public:
    TextureRTV(
        TextureViewKey view_key, WMTPixelFormat view_format, const tag_render_target_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &mtl_rtv_desc
    ) :
        RTVBase(pDesc, pResource, pDevice),
        view_key_(view_key),
        view_format_(view_format),
        attachment_desc(mtl_rtv_desc) {}

    WMTPixelFormat
    pixelFormat() final {
      return view_format_;
    }

    MTL_RENDER_PASS_ATTACHMENT_DESC &description() final {
      return attachment_desc;
    };

    Rc<Texture> texture() final {
      return this->resource->underlying_texture_;
    }

    unsigned viewId() final {
      return view_key_;
    }
  };

  using DSVBase =
      TResourceViewBase<tag_depth_stencil_view<DeviceTexture<tag_texture>>>;
  class TextureDSV : public DSVBase {
  private:
    TextureViewKey view_key_;
    WMTPixelFormat view_format_;
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;

  public:
    TextureDSV(
        TextureViewKey view_key, WMTPixelFormat view_format, const tag_depth_stencil_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment_desc
    ) :
        DSVBase(pDesc, pResource, pDevice),
        view_key_(view_key),
        view_format_(view_format),
        attachment_desc(attachment_desc) {}

    WMTPixelFormat
    pixelFormat() final {
      return view_format_;
    }

    MTL_RENDER_PASS_ATTACHMENT_DESC &description() final {
      return attachment_desc;
    };

    UINT readonlyFlags() final {
      return this->desc.Flags;
    };

    Rc<Texture> texture() final {
      return this->resource->underlying_texture_;
    }

    unsigned viewId() final {
      return view_key_;
    }

    dxmt::Rc<dxmt::RenamableTexturePool> renamable() final {
      return this->resource->renamable_;
    }
  };

public:
  DeviceTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)) {}

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)),
      renamable_(std::move(renamable)) {}

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, D3DKMT_HANDLE localHandle, D3DKMT_HANDLE globalHandle,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)), local_kmt_(localHandle), global_kmt_(globalHandle) {}

  /* ml675: the bias lives on the resource, not in a process-global pointer map.
   * ml670 keyed it on the COM pointer and never erased on destruction, so a
   * reused address inherited a stale clamp. Ownership here makes that
   * impossible by construction. */
  uint32_t MipBias() const { return mip_bias_; }
  void SetMipBias(uint32_t bias) { mip_bias_ = bias; }

  /* Logical subresource -> physical, or ~0u when the level was clamped away.
   * USAGE_DEFAULT legally permits UpdateSubresource/CopySubresourceRegion, and
   * those indices shift when mips are dropped. Book of the Dead measured zero
   * such calls, but "measured zero" is not "cannot happen". */
  uint32_t TranslateSubresource(uint32_t logical, uint32_t logicalMips) const {
    if (!mip_bias_) return logical;
    uint32_t mip = logical % logicalMips, slice = logical / logicalMips;
    if (mip < mip_bias_) return ~0u;                 /* level does not exist physically */
    return (mip - mip_bias_) + slice * (logicalMips - mip_bias_);
  }

  ~DeviceTexture() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
  }

  Rc<Buffer> buffer() final { return {}; };
  Rc<Texture> texture() final { return this->underlying_texture_; };
  BufferSlice bufferSlice() final { return {};}
  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                                 ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureRTV(key, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                                 ID3D11DepthStencilView **ppView) override {
    D3D11_DEPTH_STENCIL_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureDSV(key, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      ERR("DeviceTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    /* ml675: TWO descriptors. finalDesc is LOGICAL and is what the view reports
     * from GetDesc; physDesc is the translated copy that addresses the smaller
     * Metal texture. Storing the translated one in the COM view would just move
     * ml670's abstraction leak from resource GetDesc to SRV GetDesc.
     *
     * A null caller descriptor has already been expanded by
     * ExtractEntireResourceViewDescription into a full LOGICAL view, so it needs
     * translating too -- it is not a pass-through case. */
    D3D11_SHADER_RESOURCE_VIEW_DESC1 physDesc = finalDesc;
    uint32_t physMips = this->desc.MipLevels;
    if (mip_bias_) {
      physMips = this->desc.MipLevels > mip_bias_ ? this->desc.MipLevels - mip_bias_ : 1;
      uint32_t most = physDesc.Texture2D.MostDetailedMip;
      uint32_t count = physDesc.Texture2D.MipLevels;
      physDesc.Texture2D.MostDetailedMip = most > mip_bias_ ? most - mip_bias_ : 0;
      if (count != (uint32_t)-1) {
        uint32_t drop = most < mip_bias_ ? mip_bias_ - most : 0;
        physDesc.Texture2D.MipLevels = count > drop ? count - drop : 1;
      }
      if (physDesc.Texture2D.MostDetailedMip >= physMips) {
        static uint32_t oob_n;
        if (oob_n++ < 8)
          ERR("[mip-clamp] ml675 SRV MostDetailedMip ", physDesc.Texture2D.MostDetailedMip,
              " >= physical mips ", physMips, " after bias ", mip_bias_, " -- clamping");
        physDesc.Texture2D.MostDetailedMip = physMips - 1;
        physDesc.Texture2D.MipLevels = 1;
      }
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, physMips, arraySize, this->underlying_texture_.ptr(), physDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture SRV");
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureSRV(key, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture UAV");
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureUAV(key, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (pSharedHandle == nullptr || (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
      return E_INVALIDARG;
    }

    if (!(this->desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
      *pSharedHandle = NULL;
      return S_OK;
    }

    if (!global_kmt_) {
      return E_INVALIDARG;
    }

    *pSharedHandle = reinterpret_cast<HANDLE>(global_kmt_);
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    InitReturnPtr(pNTHandle);
    if (!local_kmt_)
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(Attributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (pName) {
      DWORD session, len, name_len = wcslen(pName);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    if (D3DKMTShareObjects(1, &local_kmt_, &attr, Access, pNTHandle)) {
      ERR("DeviceTexture: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

struct SharedResourceData {
  char mach_port_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
};


/* ml675: BC formats DXGI-wise. Kept local -- the census copy in d3d11_device.cpp
 * lives in an anonymous namespace and is not linkable from here. */
static inline bool IsBCFormatForClamp(uint32_t f) {
  return (f >= 70 && f <= 84) ||    /* BC1..BC5 incl. typeless/sRGB */
         (f >= 94 && f <= 99);      /* BC6H, BC7 */
}

/* ---- ml2000: memory-pressure-aware automatic clamp (d3d11.mipClampAuto) ----
 *
 * A D3D11 scene load created ~500 SRV-only BC textures (mostly 2048/4096
 * square, 1.23 GB logical) and ran the process from 4.35 GB to the 6.1 GB
 * jetsam limit in ~30 s. mipClampBC would have saved it, but it is a blanket
 * setting that costs resolution even with memory to spare. This applies the
 * same, already-proven clamp -- bias 1, large textures only -- to textures
 * created while the process is within d3d11.mipClampAutoMB (default 1536) of
 * its limit, as reported by os_proc_available_memory() through MadeiraCtl op 7.
 *
 * Rollback: d3d11.mipClampAuto=0 (madeira.cfg dxmt=...) or env
 * MADEIRA_MIP_CLAMP_AUTO=0. An explicit d3d11.mipClampBC=N>0 takes precedence.
 * Where the query is unavailable (remote Metal, an older unix side) the
 * reading is marked unavailable once and nothing is ever auto-clamped. */
struct MipClampAutoConfig {
  bool enabled;
  uint32_t threshold_mb;
};

static const MipClampAutoConfig &
GetMipClampAutoConfig() {
  static const MipClampAutoConfig cfg = [] {
    MipClampAutoConfig c;
    const std::string opt = Config::getInstance().getOption<std::string>("d3d11.mipClampAuto", "");
    const std::string env_v = env::getEnvVar("MADEIRA_MIP_CLAMP_AUTO");
    c.enabled = !mip_clamp::IsOffValue(opt.c_str()) && !mip_clamp::IsOffValue(env_v.c_str());
    c.threshold_mb = (uint32_t)std::max(0, Config::getInstance().getOption<int>("d3d11.mipClampAutoMB", 1536));
    ERR("[mip-clamp] ml2000 auto ", c.enabled ? "on" : "off", " threshold=", c.threshold_mb,
        " MB (d3d11.mipClampAuto=0 or MADEIRA_MIP_CLAMP_AUTO=0 disables; d3d11.mipClampAutoMB sets the threshold)");
    return c;
  }();
  return cfg;
}

/* MadeiraCtl op 7: headroom and footprint in MB, or kHeadroomUnavailable. */
static int64_t
QueryHeadroomMB(int64_t *footprint_mb) {
  struct madeira_ctl_args a;
  memset(&a, 0, sizeof a);
  a.op = 7;
  MadeiraCtl(&a);
  if (a.ret != 1)
    return mip_clamp::kHeadroomUnavailable;
  *footprint_mb = (int64_t)(a.ptr >> 20);
  return (int64_t)(a.len >> 20);
}

static std::atomic<uint64_t> g_auto_candidates{0};
static std::atomic<uint64_t> g_auto_clamped{0};
static std::atomic<int64_t> g_auto_headroom_mb{mip_clamp::kHeadroomUnknown};
static std::atomic<int64_t> g_auto_footprint_mb{0};
static std::atomic<bool> g_auto_pressure_logged{false};

/* Bias for an auto candidate already known to pass the mipClampBC filter. */
static uint32_t
MipClampAutoBias(uint32_t width, uint32_t height, uint32_t mip_levels) {
  const MipClampAutoConfig &cfg = GetMipClampAutoConfig();
  if (!cfg.enabled || !cfg.threshold_mb || !mip_clamp::AutoSizeEligible(width, height))
    return 0;
  const uint32_t bias = mip_clamp::ClampBias(1, width, height, mip_levels, true);
  if (!bias)
    return 0;

  const uint64_t n = g_auto_candidates.fetch_add(1, std::memory_order_relaxed) + 1;
  int64_t headroom = g_auto_headroom_mb.load(std::memory_order_relaxed);
  if (mip_clamp::ShouldRequery(n, headroom, cfg.threshold_mb)) {
    int64_t foot = 0;
    const int64_t fresh = QueryHeadroomMB(&foot);
    if (fresh == mip_clamp::kHeadroomUnavailable && headroom == mip_clamp::kHeadroomUnknown)
      ERR("[mip-clamp] ml2000 auto: memory headroom unavailable here; auto clamp inactive");
    headroom = fresh;
    g_auto_headroom_mb.store(headroom, std::memory_order_relaxed);
    g_auto_footprint_mb.store(foot, std::memory_order_relaxed);
  }

  const bool pressure = mip_clamp::UnderPressure(headroom, cfg.threshold_mb);
  /* Log each entry into and exit from the pressure region once. */
  bool was = g_auto_pressure_logged.load(std::memory_order_relaxed);
  if (pressure != was && g_auto_pressure_logged.compare_exchange_strong(was, pressure))
    ERR("[mip-clamp] ml2000 auto headroom=", headroom, " MB footprint=",
        g_auto_footprint_mb.load(std::memory_order_relaxed), " MB -> ", pressure ? "clamping" : "not clamping",
        " large BC textures (threshold ", cfg.threshold_mb, " MB) clamped=",
        g_auto_clamped.load(std::memory_order_relaxed));
  if (!pressure)
    return 0;

  const uint64_t clamped = g_auto_clamped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (mip_clamp::ShouldLogClamp(clamped))
    ERR("[mip-clamp] ml2000 auto headroom=", headroom, " MB clamped=", clamped, " (", width, "x", height,
        " mips ", mip_levels, " -> ", width >> bias, "x", height >> bias, " mips ", mip_levels - bias, ")");
  return bias;
}

template <typename tag>
HRESULT CreateDeviceTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }

  /* ---- ml675: PHYSICAL mip clamp, decided here rather than in the device ----
   *
   * A15 cannot sample BC, so BC formats are decoded to RGBA8 at upload and cost
   * 4-8x their compressed size. Dropping top mips is the cheapest way back under
   * the jetsam limit.
   *
   * This function already had the right abstraction: finalDesc is the LOGICAL
   * normalised descriptor the resource reports, and `info` describes the Metal
   * texture. ml670 clamped one layer up in CreateTexture2D1 and passed the
   * shrunken descriptor onward, which made GetDesc lie to the application.
   * Eligibility and the clamp amount are computed from the normalised LOGICAL
   * descriptor; only `info` and the upload loop see reduced numbers. */
  uint32_t mip_bias = 0;
  typename tag::DESC1 physDesc = finalDesc;
  if constexpr (std::is_same_v<typename tag::DESC1, D3D11_TEXTURE2D_DESC1>) {
    static int cached_clamp = -1;
    if (cached_clamp < 0)
      cached_clamp = std::max(0, std::min(4, Config::getInstance().getOption<int>("d3d11.mipClampBC", 0)));
    /* ml745: pInitialData is NO LONGER required. Requiring it meant the clamp
     * could never fire for a title that creates BC textures empty and streams
     * them later -- the case that actually matters here: 13 textures arrived
     * with initial data against 2,025 streamed updates. Those updates carry
     * LOGICAL mip indices, which UpdateTexture now translates through the bias,
     * dropping levels that no longer exist physically. */
    const bool eligible =
        IsBCFormatForClamp((uint32_t)finalDesc.Format) &&
        (finalDesc.Usage == D3D11_USAGE_DEFAULT || finalDesc.Usage == D3D11_USAGE_IMMUTABLE) &&
        finalDesc.BindFlags == D3D11_BIND_SHADER_RESOURCE && !finalDesc.CPUAccessFlags &&
        finalDesc.MipLevels >= 2 && finalDesc.SampleDesc.Count <= 1 &&
        finalDesc.Width >= 8 && finalDesc.Height >= 8;
    /* ml2000: the automatic clamp additionally refuses any misc flag except
     * TEXTURECUBE / RESOURCE_CLAMP. A shared texture is re-created at LOGICAL
     * size by whoever opens it (ImportSharedTextureInternal) and would then
     * address a smaller allocation; tiled and other exotic resources have no
     * business losing levels behind the application's back. */
    const bool auto_misc_ok =
        !(finalDesc.MiscFlags & ~(UINT)(D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_RESOURCE_CLAMP));
    if (eligible && cached_clamp) {
      mip_bias = mip_clamp::ClampBias((uint32_t)cached_clamp, finalDesc.Width, finalDesc.Height,
                                      finalDesc.MipLevels, false);
      if (mip_bias) {
        static uint32_t clamp_n;
        if (++clamp_n <= 8 || (clamp_n % 256) == 0)
          ERR("[mip-clamp] ml675 #", clamp_n, " logical ", finalDesc.Width, "x", finalDesc.Height,
              " mips ", finalDesc.MipLevels, " -> physical ", finalDesc.Width >> mip_bias, "x",
              finalDesc.Height >> mip_bias, " mips ", finalDesc.MipLevels - mip_bias, " arr=", finalDesc.ArraySize);
      }
    } else if (eligible && auto_misc_ok) {
      /* ml2000: MipLevels >= 2 is part of `eligible`, so a single-level texture
       * is never clamped; arrays and cubes need nothing extra (see the
       * per-slice initial-data remap below and TranslateSubresource). */
      mip_bias = MipClampAutoBias(finalDesc.Width, finalDesc.Height, finalDesc.MipLevels);
    }
    if (mip_bias) {
      physDesc.Width     = std::max(1u, finalDesc.Width  >> mip_bias);
      physDesc.Height    = std::max(1u, finalDesc.Height >> mip_bias);
      physDesc.MipLevels = finalDesc.MipLevels - mip_bias;
      info.width              = physDesc.Width;
      info.height             = physDesc.Height;
      info.mipmap_level_count = physDesc.MipLevels;
    }
  }
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(physDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE);
  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  /* ml745: the immediate context resolves streamed uploads through the dxmt
   * Texture, not the D3D wrapper, so the bias must be visible there too. */
  texture->setMipBias(mip_bias);

  auto &initializer = pDevice->GetDXMTDevice().queue().initializer;

  auto initialize = [&](Rc<TextureAllocation> &&allocation) {
    texture->rename(std::move(allocation));
    if (!pInitialData) {
      for (auto sub : EnumerateSubresources(physDesc)) {
        initializer.initWithZero(texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel);
      }
    } else {
      for (auto sub : EnumerateSubresources(physDesc)) {
        /* ml675: subresource index is mip + slice * MipLevels, so a clamp shifts
         * EVERY index. Walk the PHYSICAL subresources but read the caller's
         * LOGICAL array -- correct for arrays and cubes alike, since D3D counts
         * cube faces in ArraySize. A flat pointer bump would corrupt every slice
         * after the first. */
        auto &data = pInitialData[(sub.MipLevel + mip_bias) +
                                  (size_t)sub.ArraySlice * finalDesc.MipLevels];
        initializer.initWithData(
            texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel, data.pSysMem, data.SysMemPitch,
            data.SysMemSlicePitch
        );
      }
    }
  };

  auto shared_flag =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (finalDesc.MiscFlags & shared_flag) {
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("DeviceTexture: Invalid device handle");
      return E_FAIL;
    }
    // use a dedicated path for now, because there are other works for private storage mode

    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::GpuPrivate);
    if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = texture->allocate(flags);

    mach_port_t mach_port = allocation->machPort;
    if (!mach_port) {
      /* ml866: no shareable port means sharing is unavailable here (the
       * remote backend cannot pass IOSurfaces between machines). The texture
       * itself exists, so keep it and carry on unshared: an engine asking for
       * a shared flag it will never exercise loses nothing, while E_FAIL was
       * a fatal in the caller. */
      static bool said = false;
      if (!said) { said = true; ERR("DeviceTexture: no mach port for a shared texture; continuing unshared"); }
      initialize(std::move(allocation));
      auto *tex = ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice));
      tex->SetMipBias(mip_bias);
      *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(tex);
      return S_OK;
    }
    SharedResourceData runtimeData;
    MakeUniqueSharedName(runtimeData.mach_port_name);
    if (!WMTBootstrapRegister(runtimeData.mach_port_name, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    runtimeData.dimension = tag::dimension;
    memcpy(&runtimeData.desc, pDesc, sizeof(typename tag::DESC1));

    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.pPrivateRuntimeData = &runtimeData;
    create.PrivateRuntimeDataSize = sizeof(runtimeData);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    D3DDDI_ALLOCATIONINFO2 allocationInfo = {};
    create.pAllocationInfo2 = &allocationInfo;
    D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
    create.pStandardAllocation = &standardAllocation;
    standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.Flags.ExistingSysMem = 1;
    D3DDDI_ALLOCATIONINFO systemMem;
    allocationInfo.pSystemMem = &systemMem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;
    create.Flags.NtSecuritySharing = !!(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    if (D3DKMTCreateAllocation2(&create)) {
      ERR("DeviceTexture: Failed to create D3DKMT for shared texture");
      return E_FAIL;
    }

    // TODO: handle keyed mutex

    initialize(std::move(allocation));
    {
      auto *tex = ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), create.hResource,
                                             create.hGlobalShare, pDevice));
      tex->SetMipBias(mip_bias);
      *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(tex);
    }
    return S_OK;
  }

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (single_subresource && (finalDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    Rc<RenamableTexturePool> renamable = new RenamableTexturePool(texture.ptr(), 32, flags);
    initialize(renamable->getNext(0));
    auto *tex = ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(renamable), pDevice));
    tex->SetMipBias(mip_bias);
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(tex);
  } else {
    initialize(texture->allocate(flags));
    auto *tex = ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice));
    tex->SetMipBias(mip_bias);
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(tex);
  }
  return S_OK;
}

HRESULT
CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

template <typename tag>
HRESULT
ImportSharedTextureInternal(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, mach_port_t MachPort, REFIID riid,
    void **ppTexture
) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info)))
    return E_INVALIDARG;

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  auto allocation = texture->import(MachPort);
  if (!allocation)
    return E_FAIL;
  texture->rename(std::move(allocation));

  Com<DeviceTexture<tag>> device_texture = (ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  return device_texture->QueryInterface(riid, ppTexture);
}

HRESULT
ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (!(reinterpret_cast<uintptr_t>(hResource) & 0xc0000000)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfo(&query)) {
    WARN("ImportSharedTexture: Failed to query resource: ", hResource);
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN("ImportSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize);
    return E_INVALIDARG;
  } 

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCE open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

  if (D3DKMTOpenResource2(&open)) {
    WARN("ImportSharedTexture: Failed to open resource: ", hResource);
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  switch (runtimeData.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeData.desc.desc1d, mach_port, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeData.desc.desc2d, mach_port, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeData.desc.desc3d, mach_port, riid, ppTexture
    );
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }
  
  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  char dummy;

  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
  open.pTotalPrivateDriverDataBuffer = &dummy;
  open.TotalPrivateDriverDataBufferSize = 0;

  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  if (open.hSyncObject) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled sync object"));
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroySync = {};
    destroySync.hSyncObject = open.hSyncObject;
    D3DKMTDestroySynchronizationObject(&destroySync);
  }
  if (open.hKeyedMutex) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled keyed mutex"));
    D3DKMT_DESTROYKEYEDMUTEX destroyMutex = {};
    destroyMutex.hKeyedMutex = open.hKeyedMutex;
    D3DKMTDestroyKeyedMutex(&destroyMutex);
  }

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  switch (runtimeData.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeData.desc.desc1d, mach_port, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeData.desc.desc2d, mach_port, riid, ppTexture
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeData.desc.desc3d, mach_port, riid, ppTexture
    );
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  D3DKMT_OPENNTHANDLEFROMNAME openFromName = {};
  openFromName.dwDesiredAccess = dwDesiredAccess;

  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);

  WCHAR buffer[MAX_PATH];
  UNICODE_STRING name_str;
  DWORD session, len, name_len = wcslen(lpName);

  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
  memcpy(buffer + len, lpName, (name_len + 1) * sizeof(WCHAR));
  name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
  name_str.MaximumLength += sizeof(WCHAR);
  name_str.Buffer = buffer;

  attr.ObjectName = &name_str;
  attr.Attributes = OBJ_CASE_INSENSITIVE;
  openFromName.pObjAttrib = &attr;

  if (D3DKMTOpenNtHandleFromName(&openFromName)) {
    WARN(str::format("ImportSharedTextureByName: Failed to open NT handle from name: ", lpName));
    return E_INVALIDARG;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, openFromName.hNtHandle, riid, ppTexture);
  CloseHandle(openFromName.hNtHandle);
  return res;
}

#pragma endregion

} // namespace dxmt