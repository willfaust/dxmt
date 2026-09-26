#pragma once

#include "Metal.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"
#include <array>
#include <optional>
#include <unordered_map>

namespace dxmt {

class ArgumentEncodingContext;

class InternalCommandLibrary {
public:
  InternalCommandLibrary(WMT::Device device);

  WMT::Library
  getLibrary() {
    return library_;
  }

private:
  WMT::Reference<WMT::Library> library_;
};

class EmulatedCommandContext {
public:
  EmulatedCommandContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void
  ClearBufferUint(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<uint32_t, 4> &value
  ) {
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferUint(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_buffer_uint_pipeline, {32, 1, 1});
    setComputeTexture(texture, 0);
    struct CLEAR_UINT_DATA {
      const std::array<uint32_t, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearBufferFloat(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<float, 4> &value
  ) {
    // just reinterpret float as uint
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferFloat(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_buffer_float_pipeline, {32, 1, 1});
    struct CLEAR_FLOAT_DATA {
      const std::array<float, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeTexture(texture, 0);
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearTexture2DUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DArrayUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture2DArrayFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture3DFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_3d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.depth()});
  }

  void
  ClearTexture3DUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_3d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.depth()});
  }

  void
  MarshalGSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(gs_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, 0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, 0);
    encoder.setVertexBuffer({}, 0, 1);
  }

  void
  MarshalTSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(ts_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, 0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, 0);
    encoder.setVertexBuffer({}, 0, 1);
  }

private:
  void setComputePipelineState(WMT::ComputePipelineState state, const WMTSize &threadgroup_size);

  void dispatchThreads(const WMTSize &grid_size);

  void setComputeBuffer(WMT::Buffer buffer, uint64_t offset, uint8_t index);

  void setComputeTexture(WMT::Texture texture, uint8_t index);

  void setComputeBytes(const void *buf, uint64_t length, uint8_t index);

  ArgumentEncodingContext &ctx;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_float_pipeline;

  WMT::Reference<WMT::RenderPipelineState> gs_draw_arguments_marshal;
  WMT::Reference<WMT::RenderPipelineState> ts_draw_arguments_marshal;
};

class ClearRenderTargetContext {
public:
  ClearRenderTargetContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void begin(Rc<Texture> texture, TextureViewKey view);

  void clear(uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t height, const std::array<float, 4>& color);

  void end();

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> vs_clear_;
  WMT::Reference<WMT::Function> fs_clear_float_;
  WMT::Reference<WMT::Function> fs_clear_uint_;
  WMT::Reference<WMT::Function> fs_clear_sint_;
  WMT::Reference<WMT::Function> fs_clear_depth_;
  WMT::Reference<WMT::DepthStencilState> depth_write_state_;
  WMT::Reference<WMT::DepthStencilState> depth_readonly_state_;
  std::unordered_map<WMTPixelFormat, WMT::Reference<WMT::RenderPipelineState>> pso_cache_;
  Rc<Texture> clearing_texture_;
  TextureViewKey clearing_texture_view_;
};

class DepthStencilBlitContext {

public:
  DepthStencilBlitContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void copyFromBuffer(
      const Rc<Buffer> &src, uint64_t src_offset, uint64_t src_length, uint32_t bytes_per_row, uint32_t bytes_per_image,
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, bool from_d24s8
  );

  void copyFromTexture(
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, const Rc<Buffer> &dst, uint64_t dst_offset,
      uint64_t dst_length, uint32_t bytes_per_row, uint32_t bytes_per_image, bool to_d24s8
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::DepthStencilState> depth_stencil_state_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d24s8_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d32s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d24s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d32s8_;
};

class ClearResourceKernelContext {
public:
  ClearResourceKernelContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void begin(const std::array<float, 4> &color, Rc<Texture> texture, TextureViewKey view);
  void begin(const std::array<float, 4> &color, Rc<Buffer> buffer, BufferViewKey view);
  void begin(const std::array<float, 4> &color, Rc<Buffer> buffer, bool raw_buffer_is_integer);

  void clear(uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t height);

  void end();

private:
  void setClearColor(const std::array<float, 4> &color, bool is_integer);

  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> cs_clear_buffer_uint_;
  WMT::Reference<WMT::Function> cs_clear_buffer_float_;
  WMT::Reference<WMT::Function> cs_clear_tbuffer_uint_;
  WMT::Reference<WMT::Function> cs_clear_tbuffer_float_;
  WMT::Reference<WMT::Function> cs_clear_texture2d_uint_;
  WMT::Reference<WMT::Function> cs_clear_texture2d_float_;
  WMT::Reference<WMT::Function> cs_clear_texture2d_array_uint_;
  WMT::Reference<WMT::Function> cs_clear_texture2d_array_float_;
  Rc<Texture> clearing_texture_;
  Rc<Buffer> clearing_buffer_;
  TextureViewKey clearing_view_ = 0; // type compatible with BufferViewKey
  uint32_t dispatch_depth_ = 1;

  struct DXMTClearMetadata {
    union {
      float color_f32[4];
      uint32_t color_u32[4];
    };
    uint32_t offset[2];
    uint32_t size[2];
  };
  DXMTClearMetadata meta_temp_;
};

class MTLFXMVScaleContext {
public:
  MTLFXMVScaleContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void dispatch(
      const Rc<Texture> &dilated, TextureViewKey view_dilated, const Rc<Texture> &downscaled,
      TextureViewKey view_downscaled, float mv_scale_x, float mv_scale_y
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::ComputePipelineState> pso_downscale_dilated_mv_;
};

/* MADEIRA (WOW64_DESIGN.md section 7.11): the two shader-driven copy contexts
 * the Direct3D 9 frontend needs. Imported from the upstream v0.4-d3d9 tag (see
 * LICENSE-MADEIRA.md) and unreferenced by d3d11, which resolves only through
 * Metal's own multisample-resolve store action and never scales a copy. */

enum class ResolveTextureMode : uint32_t {
  Average = 0,
  // Point resolve of a multisampled depth surface (sample 0) to a single-sample
  // depth target. Uses a depth-writing pipeline into a depth attachment rather
  // than a colour one; see ResolveTextureContext::resolveDepth.
  DepthPoint = 1,
};

class ResolveTextureContext {
public:
  ResolveTextureContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void resolve(
      Rc<Texture> src, TextureViewKey src_view, Rc<Texture> dst, TextureViewKey dst_view,
      ResolveTextureMode mode, std::optional<WMTScissorRect> src_rect,
      WMTOrigin dst_origin, WMTSize resolve_size
  );

  // Point resolve of a multisampled depth surface (sample 0) into a single-sample
  // depth destination; separate from resolve() because it drives a depth
  // attachment and a depth-write pipeline instead of a colour one.
  void resolveDepth(
      Rc<Texture> src, TextureViewKey src_view, Rc<Texture> dst, TextureViewKey dst_view,
      std::optional<WMTScissorRect> src_rect, WMTOrigin dst_origin, WMTSize resolve_size
  );

private:
  WMT::RenderPipelineState getPSO(WMTPixelFormat format, ResolveTextureMode mode);

  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> vs_resolve_;
  WMT::Reference<WMT::Function> fs_resolve_average_;
  WMT::Reference<WMT::Function> fs_resolve_depth_;

  struct PSOKey {
    WMTPixelFormat format;
    ResolveTextureMode mode;

    bool operator==(const PSOKey &other) const {
      return format == other.format && mode == other.mode;
    }
  };

  struct PSOKeyHash {
    size_t operator()(const PSOKey &key) const noexcept {
      return (size_t(key.format) << 8) ^ size_t(key.mode);
    }
  };

  std::unordered_map<PSOKey, WMT::Reference<WMT::RenderPipelineState>, PSOKeyHash> psos_;
};

// Render-pass sample/store blit for D3D9 StretchRect when the fast
// blit-copy path doesn't fit; different src/dst extents, or pairs of
// format aliases that share storage layout but use distinct Metal
// pixel formats (X8R8G8B8 <-> A8R8G8B8 -> BGRX8Unorm vs BGRA8Unorm). The
// PSO is keyed on dst format only; the sampler is keyed on filter
// (POINT/LINEAR). Mirrors ResolveTextureContext's shape; minimum
// viable additions are a sampler cache and a separate fragment shader
// that calls `source.sample()` instead of `source.read()`.
class StretchBlitContext {
public:
  StretchBlitContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  // Filter is POINT or LINEAR; anything else should be rejected at the
  // d3d9 call site (D3DTEXF_ANISOTROPIC etc. are not legal for
  // StretchRect per the IDL).
  enum class Filter : uint8_t { Point = 0, Linear = 1 };

  void blit(
      Rc<Texture> src, TextureViewKey src_view, Rc<Texture> dst, TextureViewKey dst_view,
      Filter filter, WMTOrigin src_origin, WMTSize src_size,
      WMTOrigin dst_origin, WMTSize dst_size
  );

private:
  WMT::RenderPipelineState getPSO(WMTPixelFormat dst_format, uint32_t sample_count);
  WMT::SamplerState getSampler(Filter filter);

  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> vs_blit_;
  WMT::Reference<WMT::Function> fs_blit_;
  // Keyed on {dst_format, sample_count}. raster_sample_count is baked into the
  // PSO, so a single-sample blit and a single-sample -> multisample broadcast
  // of the same format are distinct pipelines. Composite key = pixel format in
  // the high bits, sample count in the low byte; a single-sample destination
  // keys count = 1, unchanged from the format-only cache.
  std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> psos_;
  WMT::Reference<WMT::SamplerState> sampler_point_;
  WMT::Reference<WMT::SamplerState> sampler_linear_;
};

} // namespace dxmt