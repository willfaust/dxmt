#include <memory>
#include "Metal.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_format.hpp"
#include "util_math.hpp"
#include <cstdint>
#include <mutex>
#include "dxmt_mem_census.hpp"
#include "dxmt_bcn.hpp"

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

/* ml1012: the block decoders and the per-image loop BOTH live in dxmt_bcn.hpp
 * now, so the D3D9 upload funnel reaches the same code this path does. What
 * remains here are the two format predicates (which are about the REMAP, not
 * about BC itself) and thin forwarders that keep this file's existing callers
 * and dxmt_resource_initializer.hpp's declarations unchanged. */

/* Physical bytes per texel produced by each decode kind. */
uint32_t bc_decode_texel_size(int kind) {
  return bcn_texel_size(kind);
}

void bc_decode_image(const uint8_t *src, size_t src_pitch, uint8_t *dst, uint32_t width,
                            uint32_t height, int kind) {
  bcn_decode_image(src, src_pitch, dst, /*dst_pitch=*/0, width, height, kind);
}


/* ml676: which REMAPPED formats came from a BC source we cannot decode yet, and
 * what does one physical texel cost. Mirrors remap_unsupported_bc() in
 * winemetal_unix.c -- BC4->R8, BC5->RG8, BC7->RGBA8, BC6H->RGBA16F. BC1/BC3
 * also remap to RGBA8 but they DO decode, so they never reach here. */
static inline bool bc_is_unsupported_src(enum WMTPixelFormat f) {
  /* Test the SOURCE format: texture->pixelFormat() is still the BC format here
   * (remap_unsupported_bc runs later, when the MTLTexture is created), which is
   * exactly why bc_decode_kind can match on BC1/BC3 at all. */
  switch (f) {
  case WMTPixelFormatBC6H_RGBFloat: case WMTPixelFormatBC6H_RGBUfloat:
    return true;                 /* ml679: the only format still synthesised */
  default:
    return false;
  }
}

/* Physical texel size AFTER remap_unsupported_bc: BC4->R8, BC5->RG8,
 * BC6H->RGBA16F, BC7/BC2->RGBA8. */
static inline uint32_t bc_fill_texel_size(enum WMTPixelFormat f) {
  switch (f) {
  case WMTPixelFormatBC4_RUnorm:  case WMTPixelFormatBC4_RSnorm:    return 1;
  case WMTPixelFormatBC5_RGUnorm: case WMTPixelFormatBC5_RGSnorm:   return 2;
  case WMTPixelFormatBC6H_RGBFloat: case WMTPixelFormatBC6H_RGBUfloat: return 8;
  default:                                                          return 4;
  }
}

/* Deterministic, obviously-synthetic, and different per format so the screen
 * names the culprit. Varies slightly per mip so mip selection is visible too. */
static void bc_fill_pattern(enum WMTPixelFormat f, uint8_t *dst, uint32_t w, uint32_t h, uint32_t level) {
  const uint32_t cell = 16u >> (level > 3 ? 3 : level);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const bool on = (((x / (cell ? cell : 1)) ^ (y / (cell ? cell : 1))) & 1u) != 0;
      switch (f) {
      case WMTPixelFormatBC4_RUnorm: case WMTPixelFormatBC4_RSnorm:
        dst[(size_t)y * w + x] = on ? 0xC0 : 0x40;                    /* mid-grey mask */
        break;
      case WMTPixelFormatBC5_RGUnorm: case WMTPixelFormatBC5_RGSnorm: {
        uint8_t *p = dst + ((size_t)y * w + x) * 2;
        p[0] = 0x80; p[1] = 0x80;                                     /* flat normal */
        break;
      }
      case WMTPixelFormatBC6H_RGBFloat: case WMTPixelFormatBC6H_RGBUfloat: {
        /* ml677 A/B: was (0.125, 0.5, 1.0) -- a strong blue. BC6H is Unity's
         * sky and reflection probes, so that fill became the ambient light for
         * the whole scene and everything read blue. Neutral grey (0.25) tests
         * that directly: if the blue cast goes away, BC6H was the cause; if it
         * survives, the tint is coming from somewhere else entirely. */
        uint16_t *p = (uint16_t *)(dst + ((size_t)y * w + x) * 8);
        p[0] = 0x3400; p[1] = 0x3400; p[2] = 0x3400; p[3] = 0x3C00;   /* neutral grey 0.25 */
        break;
      }
      default: {
        uint8_t *p = dst + ((size_t)y * w + x) * 4;
        p[0] = on ? 0xFF : 0x00; p[1] = 0x00; p[2] = on ? 0xFF : 0x00; p[3] = 0xFF; /* magenta checks */
        break;
      }
      }
    }
  }
}

/* ml679: 1=BC1 2=BC2 3=BC3 4=BC4u 5=BC5u 7=BC7 14=BC4s 15=BC5s. BC6H returns 0
 * and is still handled by the deterministic fill above. */
int bc_decode_kind(enum WMTPixelFormat f) {
  switch (f) {
  case WMTPixelFormatBC1_RGBA: case WMTPixelFormatBC1_RGBA_sRGB: return 1;
  case WMTPixelFormatBC2_RGBA: case WMTPixelFormatBC2_RGBA_sRGB: return 2;
  case WMTPixelFormatBC3_RGBA: case WMTPixelFormatBC3_RGBA_sRGB: return 3;
  case WMTPixelFormatBC4_RUnorm:  return 4;
  case WMTPixelFormatBC4_RSnorm:  return 14;
  case WMTPixelFormatBC5_RGUnorm: return 5;
  case WMTPixelFormatBC5_RGSnorm: return 15;
  case WMTPixelFormatBC7_RGBAUnorm: case WMTPixelFormatBC7_RGBAUnorm_sRGB: return 7;
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
  /* ---- ml676: NEVER UPLOAD RAW BC BYTES AS UNCOMPRESSED TEXELS ----------
   *
   * remap_unsupported_bc() swaps every BC format for an uncompressed one on
   * A15, but bc_decode_kind() only knows BC1 and BC3. BC4/BC5/BC6H/BC7 fell
   * straight through this branch and were uploaded ANYWAY -- compressed block
   * bytes written into a texture that Metal now believes is RGBA8/R8/RG8/
   * RGBA16F.
   *
   * That is not merely ugly. The staging suballocation is sized from the
   * COMPRESSED byte count while the copy is issued for the DECODED extent, so
   * Metal reads past the allocation: 4x over for BC7, 4x for BC4 (1 byte/px
   * physical vs 0.5 compressed), 8x for BC6H. It stays inside the 32MB staging
   * block, but it crosses into neighbouring or stale texture contents -- an
   * out-of-bounds read, and the source of the striped garbage seen in Book of
   * the Dead's control overlay (a BC7 block-row is exactly one RGBA8 pixel row,
   * so the data filled full width at quarter height).
   *
   * Until each format has a real decoder, synthesise a full-size deterministic
   * pattern in the CORRECT physical format. Bounded, obviously synthetic, and
   * diagnostic: if geometry lights up in these colours then missing BC formats
   * are the whole story, and if it stays black the problem is elsewhere. */
  if (!is_3d_tex && !device_.supportsBCTextureCompression() &&
      bc_decode_kind(texture->pixelFormat()) == 0 && bc_is_unsupported_src(texture->pixelFormat())) {
    /* ml679: BC1/BC2/BC3/BC4/BC5/BC7 all decode for real now, so only BC6H
     * still reaches this fill. Keeping the path (rather than deleting it) is
     * what guarantees no raw BC bytes are ever uploaded for a format we have
     * not yet implemented -- that was a memory-safety bug, not a cosmetic one. */
    const uint32_t fill_texel = bc_fill_texel_size(texture->pixelFormat());
    const size_t out_bytes = (size_t)width_sub * height_sub * fill_texel;
    decoded.reset(new (std::nothrow) uint8_t[out_bytes]);
    if (!decoded) {
      ERR("[bc-fill] ml676 OOM for ", width_sub, "x", height_sub, " -- upload SKIPPED (was unsafe)");
      return 0;
    }
    bc_fill_pattern(texture->pixelFormat(), decoded.get(), width_sub, height_sub, level);
    data = decoded.get();
    block_size = 1u;
    texel_size = fill_texel;
    row_pitch = (size_t)width_sub * fill_texel;
    depth_pitch = out_bytes;
    static unsigned fn;
    if (fn < 6 || (fn & 0xff) == 0)
      ERR("[bc-fill] ml676 #", fn, " unsupported BC fmt=", (unsigned)texture->pixelFormat(), " ",
          width_sub, "x", height_sub, " level=", level, " -> ", fill_texel, "B/texel pattern (",
          out_bytes, "B). Raw-BC upload suppressed.");
    fn++;
  }

  const int bc_kind = bc_decode_kind(texture->pixelFormat());
  if (bc_kind && !is_3d_tex && !device_.supportsBCTextureCompression()) {
    const uint32_t out_texel = bc_decode_texel_size(bc_kind);
    const size_t out_bytes = (size_t)width_sub * height_sub * out_texel;
    decoded.reset(new (std::nothrow) uint8_t[out_bytes]);
    if (decoded) {
      bc_decode_image((const uint8_t *)data, row_pitch, decoded.get(), width_sub, height_sub, bc_kind);
      data = decoded.get();
      block_size = 1u;                 /* decoded output is not block-compressed */
      texel_size = out_texel;
      row_pitch = (size_t)width_sub * out_texel;
      depth_pitch = out_bytes;
      static const char *const kName[] = {
        "?", "BC1", "BC2", "BC3", "BC4u", "BC5u", "?", "BC7",
        "?", "?", "?", "?", "?", "?", "BC4s", "BC5s"
      };
      static unsigned n;
      if (n < 6 || (n & 0x1ff) == 0)
        ERR("[bc-decode] ml679 #", n, " ", kName[bc_kind & 15], " ", width_sub, "x",
            height_sub, " level=", level, " slice=", slice, " -> ", out_texel, "B/texel ",
            out_bytes, "B");
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

    /* MADEIRA ml1490: grow to the next power of two (at least 1 MB) instead of
     * the exact request. Each growth above costs a flushInternal(), and a
     * loading screen asking for slowly rising sizes regrew it 322 times on
     * device. DXMT_ZERO_BUFFER_POW2=0 restores exact sizing. */
    static const bool pow2 = [] {
      const bool value = env::getEnvVar("DXMT_ZERO_BUFFER_POW2") != "0";
      WARN("[zero-buffer] ml1490 pow2-growth=", value, " (DXMT_ZERO_BUFFER_POW2=0 disables)");
      return value;
    }();
    size_t length = size;
    if (pow2) {
      length = size_t(1) << 20;
      while (length < size)
        length <<= 1;
    }
    WMTBufferInfo buffer_info;
    buffer_info.gpu_address = 0;
    buffer_info.length = length;
    buffer_info.memory.set(nullptr);
    buffer_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    /* ml1490: the buffer being replaced is released with this assignment, so
     * the census drops it here; it counted every buffer ever made as live. */
    if (zero_buffer_census_)
      mem_census_sub(MEMOWN_INIT_UPLOAD, zero_buffer_census_);
    zero_buffer_ = device_.newBuffer(buffer_info);
    mem_census_add(MEMOWN_INIT_UPLOAD, buffer_info.length);  /* ml677 */
    zero_buffer_census_ = buffer_info.length;
    zero_buffer_size_ = length;

    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = zero_buffer_;
    fill->length = size;
    fill->offset = 0;
    fill->value = 0;
  }
  return zero_buffer_;
}

} // namespace dxmt