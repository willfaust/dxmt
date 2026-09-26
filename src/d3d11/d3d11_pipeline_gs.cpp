#include "Metal.hpp"
#include "airconv_public.h"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "log/log.hpp"
#include "util_futex.hpp"
#include "config/config.hpp"   /* ml754 */
#include <atomic>

namespace dxmt {

class MTLCompiledGeometryPipelineImpl
    : public MTLCompiledGeometryPipeline {
public:
  MTLCompiledGeometryPipelineImpl(MTLD3D11Device *pDevice,
                              const MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount) {
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(
                                    pDesc->ColorAttachmentFormats[i]))
                                << i);
    }
    VertexShader =
        pDesc->VertexShader->get_shader(ShaderVariantGeometryVertex{
            pDesc->InputLayout, pDesc->GeometryShader,
            pDesc->IndexBufferFormat, pDesc->GSStripTopology});
    GeometryShader = pDesc->GeometryShader->get_shader(
      ShaderVariantGeometry{pDesc->VertexShader, pDesc->GSStripTopology});
   
    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask});
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
  }

  void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pPipeline) final {
    dxmt::atomic_wait(ready_, false, std::memory_order_acquire);
    *pPipeline = {state_mesh_};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs, gs, ps;

    if (!VertexShader->GetShader(&vs)) {
      return VertexShader;
    }
    if (!vs.Function) {
      ERR("Failed to create mesh PSO: Invalid vertex shader.");
      return this;
    }
    if (!GeometryShader->GetShader(&gs)) {
      return GeometryShader;
    }
    if (!gs.Function) {
      ERR("Failed to create mesh PSO: Invalid geometry shader.");
      return this;
    }
    if (PixelShader) {
      if (!PixelShader->GetShader(&ps)) {
        return PixelShader;
      }
      if (!ps.Function) {
        ERR("Failed to create mesh PSO: Invalid pixel shader.");
        return this;
      }
    }

    WMTMeshRenderPipelineInfo info;
    WMT::InitializeMeshRenderPipelineInfo(info);

    info.object_function = vs.Function;
    info.mesh_function = gs.Function;
    info.payload_memory_length = 16256;

    info.immutable_object_buffers = (1 << 16)  | (1 << 21) | (1 << 29) | (1 << 30);
    info.immutable_mesh_buffers = (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    if (PixelShader) {
      info.fragment_function = ps.Function;
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }

    info.raster_sample_count = SampleCount;

    /* ml867: with d3d11.noMeshShaders=1 the mesh pipeline is never compiled.
     * Apple's shader compiler aborted the remote host (SIGABRT inside AGX
     * while compiling the object program of one of these), which kills every
     * other pipeline with it. Leaving the state null takes the existing
     * mesh-fail path: the draws that needed it are skipped, everything else
     * renders. Real devices keep the default and compile as before. */
    {
      static const int no_mesh = Config::getInstance().getOption<int>("d3d11.noMeshShaders", 0);
      if (no_mesh) {
        static std::atomic<uint32_t> suppressed{0};
        uint32_t n = suppressed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & 0x3F) == 0)
          ERR("[mesh-skip] ml867 suppressed ", n, " GEOMETRY mesh pipeline compile(s) (d3d11.noMeshShaders=1)");
        return this;
      }
    }
    state_mesh_ = device_->GetMTLDevice().newRenderPipelineState(info, err);

    if (state_mesh_ == nullptr) {
      /* ml754: say WHICH stage needed the mesh pipeline.
       *
       * DXMT lowers D3D11 geometry shaders AND tessellation onto Metal
       * object/mesh shaders, and the paravirtual Metal device in the research
       * VM implements neither -- not merely hidden behind capability reporting:
       * AppleParavirtDevice has no mesh PSO creation and
       * AppleParavirtRenderCommandEncoder has no setObjectBuffer/setMeshBuffer,
       * so the APV transport carries no mesh packets at all. respondsToSelector:
       * answers YES because Metal DECLARES the selector, which is a metadata
       * trap, not a capability.
       *
       * Whether that matters depends entirely on which stage a title actually
       * needs: a geometry-shader compute fallback is a contained project, while
       * tessellation needs hull/tess-coord/domain/compaction stages. This
       * counter is what decides which one is worth building. */
      static std::atomic<uint32_t> n{0};
      ERR("[mesh-fail] ml754 kind=GEOMETRY #", n.fetch_add(1, std::memory_order_relaxed) + 1,
          " -- ", err.description().getUTF8String());
      return this;
    }
    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    dxmt::atomic_notify_all(ready_);
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_mesh_;
  bool RasterizationEnabled;
  UINT SampleCount;

  CompiledShader *VertexShader;
  CompiledShader *PixelShader;
  CompiledShader *GeometryShader;
};

std::unique_ptr<MTLCompiledGeometryPipeline>
CreateGeometryPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledGeometryPipelineImpl>(pDevice, pDesc);
}

} // namespace dxmt