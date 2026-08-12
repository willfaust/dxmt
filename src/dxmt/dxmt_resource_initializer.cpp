#include <memory>
#include "Metal.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_format.hpp"
#include "util_math.hpp"
#include <cstdint>
#include <mutex>

namespace dxmt {

#define ALLOC_BLIT(type, cmd)                                                                                          \
  type *cmd = nullptr;                                                                                                 \
  if (!allocateBlit(&cmd)) {                                                                                           \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_CLEAR(info)                                                                                              \
  WMTRenderPassInfo *info;                                                                                             \
  if (!allocateClear(&info)) {                                                                                         \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_GPU(buffer, size)                                                                                        \
  WMT::Buffer buffer;                                                                                                  \
  size_t buffer##_offset;                                                                                              \
  if (!(buffer = allocateGpuHeap(size, buffer##_offset))) {                                                            \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_ZERO(buffer, size)                                                                                       \
  WMT::Buffer buffer;                                                                                                  \
  if (!(buffer = allocateZeroBuffer(size))) {                                                                          \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define RETAIN(allocation)                                                                                             \
  if (!retainAllocation(allocation)) {                                                                                 \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

ResourceInitializer::ResourceInitializer(WMT::Device device) :
    device_(device),
    gpu_command_heap_allocator(StagingBufferBlockAllocator(
        device, WMTResourceStorageModeManaged | WMTResourceHazardTrackingModeUntracked, false
    )) {
  upload_queue_ = device.newCommandQueue(kResourceInitializerChunks);
  upload_queue_event_ = device.newSharedEvent();

  cpu_command_heap_size = kResourceInitializerCpuCommandHeapSize;
  cpu_command_heap = malloc(cpu_command_heap_size);
  reset();
}

ResourceInitializer::~ResourceInitializer() {
  free(cpu_command_heap);
}

uint64_t
ResourceInitializer::initWithZero(BufferAllocation *buffer, uint64_t offset, uint64_t length) {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    RETAIN(buffer);
    ALLOC_BLIT(wmtcmd_blit_fillbuffer, fill);
    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = buffer->buffer();
    fill->offset = offset;
    fill->length = length;
    fill->value = 0;

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initDepthStencilWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, uint32_t dsv_planar
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length = 0;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    if (dsv_planar & 1) {
      info->depth.texture = allocation->texture();
      info->depth.clear_depth = 0;
      info->depth.load_action = WMTLoadActionClear;
      info->depth.store_action = WMTStoreActionStore;
      info->depth.slice = slice;
      info->depth.level = level;
    }
    if (dsv_planar & 2) {
      info->stencil.texture = allocation->texture();
      info->stencil.clear_stencil = 0;
      info->stencil.load_action = WMTLoadActionClear;
      info->stencil.store_action = WMTStoreActionStore;
      info->stencil.slice = slice;
      info->stencil.level = level;
    }

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initRenderTargetWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length = texture->textureType() == WMTTextureType3D ? texture->depth() : 0;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    info->colors[0].texture = allocation->texture();
    info->colors[0].load_action = WMTLoadActionClear;
    info->colors[0].clear_color = {0, 0, 0, 0};
    info->colors[0].store_action = WMTStoreActionStore;
    info->colors[0].slice = slice;
    info->colors[0].level = level;

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  if (auto dsv_planar = DepthStencilPlanarFlags(texture->pixelFormat())) {
    return initDepthStencilWithZero(texture, allocation, slice, level, dsv_planar);
  }
  if (texture->usage() & WMTTextureUsageRenderTarget) {
    return initRenderTargetWithZero(texture, allocation, slice, level);
  }

  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());
  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    ALLOC_ZERO(zero, total_bytes_needed);
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = zero;
    copy->src_offset = 0;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};

  } while (0);

  return current_seq_id_;
}


/* ======================= ml654 BC1/BC3 -> RGBA8 DECODE =======================
 * WHY: A15 reports supportsBCTextureCompression = NO. remap_unsupported_bc()
 * (winemetal_unix.c) therefore creates these textures as RGBA8 so the Metal
 * descriptor validates — but the UPLOAD still carries BC-shaped pitches, so
 * texture_upload_pitch_ok() drops it and the texture is never filled. That is
 * Thumper's static/missing art.
 *
 * The fix belongs HERE because initWithData computes its whole copy layout from
 * texture->pixelFormat(), which is still LOGICALLY BC. Decoding the data without
 * also switching the layout to the PHYSICAL format would still stage BC-sized
 * rows into an RGBA8 texture.
 *
 * ⚠️ LOGICAL vs PHYSICAL: the D3D resource keeps reporting BC everywhere — its
 * description, pitches, subresources and SRVs are untouched. Only this upload
 * path uses the physical RGBA8 layout. Surfacing RGBA8 to the Windows app would
 * violate D3D semantics even where the game would not notice.
 *
 * ml653 census justifies the narrow scope: Thumper is BC1+BC3 only, 128 textures /
 * 1,234 subresources / 30MB, all USAGE_DEFAULT written once at creation, zero
 * UpdateSubresource/Copy, zero CPU access. Decode costs ~+119MB of RGBA8 (peak
 * ~3.13GB vs a 4096MB jetsam) and is LOSSLESS, unlike an ETC2 re-encode. */

static inline void bc_rgb565(uint16_t c, uint8_t *out) {
  uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
  out[0] = (uint8_t)((r << 3) | (r >> 2));
  out[1] = (uint8_t)((g << 2) | (g >> 4));
  out[2] = (uint8_t)((b << 3) | (b >> 2));
}

/* One BC1 colour block -> 16 RGBA8 texels. `punchthrough` selects the 3-colour
 * mode with a transparent index-3; BC3's embedded colour block never uses it. */
static void bc1_block(const uint8_t *blk, uint8_t out[64], bool punchthrough) {
  uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
  uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
  uint8_t p[4][4];
  bc_rgb565(c0, p[0]); p[0][3] = 255;
  bc_rgb565(c1, p[1]); p[1][3] = 255;
  if (!punchthrough || c0 > c1) {
    for (int i = 0; i < 3; i++) {
      p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3);
      p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int i = 0; i < 3; i++) {
      p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2);
      p[3][i] = 0;
    }
    p[2][3] = 255;
    p[3][3] = 0;   /* the punch-through texel */
  }
  uint32_t idx = (uint32_t)(blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24));
  for (int t = 0; t < 16; t++) {
    const uint8_t *src = p[(idx >> (t * 2)) & 3];
    out[t * 4 + 0] = src[0]; out[t * 4 + 1] = src[1];
    out[t * 4 + 2] = src[2]; out[t * 4 + 3] = src[3];
  }
}

/* BC3 = 8-byte BC4-style alpha block + a BC1 colour block that is ALWAYS in
 * 4-colour mode (no punch-through). */
static void bc3_block(const uint8_t *blk, uint8_t out[64]) {
  bc1_block(blk + 8, out, /*punchthrough=*/false);
  uint8_t a[8];
  a[0] = blk[0]; a[1] = blk[1];
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1] + 2) / 5);
    a[6] = 0; a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
  for (int t = 0; t < 16; t++) out[t * 4 + 3] = a[(bits >> (t * 3)) & 7];
}

/* Decode a whole subresource. src_pitch is the guest's BC row pitch (bytes per
 * ROW OF BLOCKS); dst is tightly packed RGBA8 at width*4. Edge blocks are
 * decoded in full and clipped, which is what the BC spec requires for
 * non-multiple-of-4 dimensions. */
static void bc_decode_image(const uint8_t *src, size_t src_pitch, uint8_t *dst, uint32_t width,
                            uint32_t height, bool is_bc3) {
  const uint32_t bx_n = (width + 3) / 4, by_n = (height + 3) / 4;
  const size_t blk_bytes = is_bc3 ? 16 : 8;
  const size_t dst_pitch = (size_t)width * 4;
  uint8_t texels[64];
  for (uint32_t by = 0; by < by_n; by++) {
    const uint8_t *row = src + (size_t)by * src_pitch;
    for (uint32_t bx = 0; bx < bx_n; bx++) {
      if (is_bc3) bc3_block(row + bx * blk_bytes, texels);
      else        bc1_block(row + bx * blk_bytes, texels, /*punchthrough=*/true);
      for (uint32_t ty = 0; ty < 4; ty++) {
        const uint32_t y = by * 4 + ty;
        if (y >= height) break;
        for (uint32_t tx = 0; tx < 4; tx++) {
          const uint32_t x = bx * 4 + tx;
          if (x >= width) break;
          memcpy(dst + (size_t)y * dst_pitch + (size_t)x * 4, texels + (ty * 4 + tx) * 4, 4);
        }
      }
    }
  }
}

static inline int bc_decode_kind(enum WMTPixelFormat f) {
  switch (f) {
  case WMTPixelFormatBC1_RGBA: case WMTPixelFormatBC1_RGBA_sRGB: return 1;
  case WMTPixelFormatBC3_RGBA: case WMTPixelFormatBC3_RGBA_sRGB: return 3;
  default: return 0;
  }
}
/* ===================== end ml654 BC1/BC3 -> RGBA8 DECODE ==================== */

uint64_t
ResourceInitializer::initWithData(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, const void *data,
    size_t row_pitch, size_t depth_pitch
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_1d_tex = (texture->textureType() == WMTTextureType1D) || (texture->textureType() == WMTTextureType1DArray);
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());

  /* ml654: BC decode. The Metal texture is PHYSICALLY RGBA8 (remap_unsupported_bc);
   * only the D3D-visible format stays BC. Switch the layout to the physical format
   * and feed decoded texels, or we stage BC-sized rows into an RGBA8 texture and
   * the upload gets dropped. 3D is excluded — no BC volume textures observed, and
   * guessing at slice pitches here is how you get silent corruption. */
  std::unique_ptr<uint8_t[]> decoded;
  const int bc_kind = bc_decode_kind(texture->pixelFormat());
  if (bc_kind && !is_3d_tex && !device_.supportsBCTextureCompression()) {
    const size_t out_bytes = (size_t)width_sub * height_sub * 4;
    decoded.reset(new (std::nothrow) uint8_t[out_bytes]);
    if (decoded) {
      bc_decode_image((const uint8_t *)data, row_pitch, decoded.get(), width_sub, height_sub,
                      bc_kind == 3);
      data = decoded.get();
      block_size = 1u;                 /* physical RGBA8 is not block-compressed */
      texel_size = 4u;
      row_pitch = (size_t)width_sub * 4;
      depth_pitch = out_bytes;
      static unsigned n;
      if (n < 4 || (n & 0x7f) == 0)
        ERR("[bc-decode] ml654 #", n, " ", (bc_kind == 3 ? "BC3" : "BC1"), " ", width_sub, "x",
            height_sub, " level=", level, " slice=", slice, " -> RGBA8 ", out_bytes, "B");
      n++;
    } else {
      ERR("[bc-decode] ml654 OOM decoding ", width_sub, "x", height_sub, " — upload skipped");
    }
  }

  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_row_increment = is_1d_tex ? bytes_per_row_needed : row_pitch;
  size_t bytes_per_row_valid = is_1d_tex ? bytes_per_row_needed : std::min(row_pitch, bytes_per_row_needed);
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t bytes_per_image_increment = is_3d_tex ? depth_pitch : bytes_per_image_needed;
  size_t bytes_per_image_valid = is_3d_tex ? std::min(depth_pitch, bytes_per_image_needed) : bytes_per_image_needed;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);
    ALLOC_GPU(temp, total_bytes_needed);

    for (auto depth = 0u; depth < depth_sub; depth++) {
      if (bytes_per_row_increment != bytes_per_row_needed) {
        for (auto row = 0u; row < (align(height_sub, block_size) / block_size); row++) {
          auto offset = temp_offset + depth * bytes_per_image_needed + row * bytes_per_row_needed;
          auto length = bytes_per_row_valid;
          auto src_data = ptr_add(data, depth * bytes_per_image_increment + row * bytes_per_row_increment);
          temp.updateContents(offset, src_data, length);
        }
      } else {
        auto offset = temp_offset + depth * bytes_per_image_needed;
        auto length = bytes_per_image_valid;
        auto src_data = ptr_add(data, depth * bytes_per_image_increment);
        temp.updateContents(offset, src_data, length);
      }
    }

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = temp;
    copy->src_offset = temp_offset;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};

  } while (0);

  return current_seq_id_;
}

std::uint64_t
ResourceInitializer::flushInternal() {
  auto pool = WMT::MakeAutoreleasePool();

  auto seq_id = current_seq_id_++;
  auto cmdbuf = upload_queue_.commandBuffer();
  encode(cmdbuf);
  cmdbuf.encodeSignalEvent(upload_queue_event_, seq_id);
  cmdbuf.commit();
  reset();
  cached_coherent_seq_id = upload_queue_event_.signaledValue();
  gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
  return seq_id;
}

uint64_t
ResourceInitializer::flushToWait() {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  if (idle()) {
    gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    cached_coherent_seq_id = upload_queue_event_.signaledValue();
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    return current_seq_id_ - 1;
  }

  return flushInternal();
}

void
ResourceInitializer::reset() {
  cpu_command_heap_offset = 0;

  clear_render_pass_head.next = nullptr;
  clear_render_pass_tail = &clear_render_pass_head;

  blit_cmd_head.type = WMTBlitCommandNop;
  blit_cmd_head.next.set(nullptr);
  blit_cmd_tail = (wmtcmd_base *)&blit_cmd_head;

  ref_tracker.clear();
}

void
ResourceInitializer::encode(WMT::CommandBuffer cmdbuf) {

  auto clear_pass = clear_render_pass_head.next;
  while (clear_pass) {
    auto r = cmdbuf.renderCommandEncoder(clear_pass->info);
    r.endEncoding();
    clear_pass = clear_pass->next;
  }

  if (blit_cmd_head.next.ptr) {
    auto b = cmdbuf.blitCommandEncoder();
    b.encodeCommands(&blit_cmd_head);
    b.endEncoding();
  }
}

WMT::Buffer
ResourceInitializer::allocateGpuHeap(size_t size, size_t &offset) {
  auto [block, offset_] = gpu_command_heap_allocator.allocate(
      current_seq_id_, cached_coherent_seq_id, size, kResourceInitializerGpuUploadHeapAlignment
  );
  offset = offset_;
  return block.buffer;
}

bool
ResourceInitializer::retainAllocation(Allocation *allocation) {
  constexpr size_t block_size = 0x20;
  while (unlikely(!ref_tracker.track(allocation))) {
    auto temp = allocateCpuHeap<intptr_t[block_size]>();
    if (!temp)
      return false;
    ref_tracker.addStorage(temp, block_size * sizeof(intptr_t));
  }
  return true;
}

WMT::Buffer
ResourceInitializer::allocateZeroBuffer(size_t size) {
  if (zero_buffer_size_ < size) {
    if (zero_buffer_size_) {
      flushInternal(); // keep a reference of old zero buffer
      zero_buffer_size_ = 0;
      return {};
    }

    wmtcmd_blit_fillbuffer *fill = nullptr;
    if (!allocateBlit(&fill)) {
      return {};
    }

    WMTBufferInfo buffer_info;
    buffer_info.gpu_address = 0;
    buffer_info.length = size;
    buffer_info.memory.set(nullptr);
    buffer_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    zero_buffer_ = device_.newBuffer(buffer_info);
    zero_buffer_size_ = size;

    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = zero_buffer_;
    fill->length = size;
    fill->offset = 0;
    fill->value = 0;
  }
  return zero_buffer_;
}

} // namespace dxmt