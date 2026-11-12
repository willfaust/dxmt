/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once
#include <atomic>
#include "Metal.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "util_flags.hpp"
#include "util_svector.hpp"

namespace dxmt {

enum class TextureAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  Shared = 5,
  ShaderReadonly = 6,
  AllocatedOnHeap = 7,
};

struct TextureViewDescriptor {
  WMTPixelFormat format    : 24;
  WMTTextureType type      : 8;
  uint32_t firstMiplevel   : 4 = 0;
  uint32_t miplevelCount   : 4 = 1;
  uint32_t firstArraySlice : 12 = 0;
  uint32_t arraySize       : 12 = 1;
  // Per-channel sample swizzle. Identity for every d3d11 view (the frontend
  // never sets it, and the defaults below are what TextureView already passed
  // to newTextureView unconditionally); the Direct3D 9 frontend needs the
  // non-identity cases for its X-channel, luminance and depth-replicate
  // formats. Part of the view identity, so createView() compares it.
  WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed,
      WMTTextureSwizzleGreen,
      WMTTextureSwizzleBlue,
      WMTTextureSwizzleAlpha,
  };
};

inline bool
SwizzleEqual(const WMTTextureSwizzleChannels &a, const WMTTextureSwizzleChannels &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

struct TextureViewKey {
  union {
    struct {
      uint64_t index       : 28;
      uint64_t mip_count   : 4;
      uint64_t mip_start   : 4;
      uint64_t array_start : 12;
      uint64_t mip_end     : 4;
      uint64_t array_end   : 12;
    };
    uint64_t impl_;
  };

  TextureViewKey() {
    impl_ = 0;
  }
  TextureViewKey(const TextureViewDescriptor &descriptor, unsigned index, unsigned total_mip_count) {
    mip_start = descriptor.firstMiplevel;
    array_start = descriptor.firstArraySlice;
    mip_end = descriptor.firstMiplevel + descriptor.miplevelCount;
    array_end = descriptor.firstArraySlice + descriptor.arraySize;
    mip_count = total_mip_count;
    this->index = index;
  }
  TextureViewKey(uint64_t impl) {
    impl_ = impl;
  }
  operator uint64_t() const {
    return impl_;
  }
};

class Texture;
class TextureAllocation;

class TextureView {
public:
  virtual ~TextureView() {};

  void incRef();
  void decRef();

  WMT::Reference<WMT::Texture> texture;
  uint64_t gpuResourceID;
  DXMT_RESOURCE_RESIDENCY_STATE residency{};
  TextureAllocation *allocation; // `TextureAllocation` holds strong reference to `TextureView`
  TextureViewKey key;

  TextureView(const TextureView &) = delete;
  TextureView(TextureView &&) = delete;
  TextureView &operator=(const TextureView &) = delete;
  TextureView &operator=(TextureView &&) = delete;
  TextureView(TextureAllocation *allocation);
  TextureView(TextureAllocation *allocation, unsigned index, TextureViewDescriptor descriptor);

private:
  std::atomic<uint32_t> refcount_ = {0u};
};

class TextureViewRef : public Rc<TextureView> {
public:
  using Rc<TextureView>::Rc;

  WMT::Texture
  texture() const {
    if (!*this)
      return {};
    return ptr()->texture;
  }

  TextureViewRef &
  operator=(TextureView &ref) {
    return (*this = &ref);
  }
};

class TextureAllocation final : public Allocation {
  friend class Texture;

public:
  uint32_t census_owner_ = 0;   /* ml677: MemOwner this allocation was charged to */
  uint64_t census_bytes_ = 0;   /* ml677: bytes to refund on destruction          */


  /**
   * notes on thread-safety:
   * all states in `TextureAllocation` is either immutable or only accessed by `dxmt-encode-thread`
   */

public:

  WMT::Texture texture() const {
    return obj_;
  }

  // The Metal buffer a buffer-backed (linear) allocation wraps, or a null
  // handle for a plain texture allocation. Buffer-backed is the only shape
  // whose texels live in memory the CPU also addresses through mappedMemory,
  // so having the buffer is the universal "buffer-backed" predicate.
  WMT::Buffer buffer() const {
    return buffer_;
  }

  Flags<TextureAllocationFlag>
  flags() const {
    return flags_;
  }

  Texture *descriptor;
  void *mappedMemory;
  uint64_t gpuResourceID;
  mach_port_t machPort;
  small_vector<GenericAccessTracker, 1> fenceTrackers;

private:
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
      unsigned bytes_per_row, Flags<TextureAllocationFlag> flags
  );
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
      Flags<TextureAllocationFlag> flags
  );

  void free();

  TextureAllocation(const TextureAllocation &) = delete;
  TextureAllocation(TextureAllocation &&) = delete;

  WMT::Reference<WMT::Texture> obj_;
  WMT::Reference<WMT::Buffer> buffer_;
  uint32_t version_ = 0;
  Flags<TextureAllocationFlag> flags_;
  small_vector<TextureViewRef, 4> cached_view_;
};

class Texture {

public:
  void incRef();
  void decRef();

  TextureViewKey createView(TextureViewDescriptor const &descriptor);

  constexpr TextureAllocation *
  current() {
    return current_.ptr();
  }

  WMTTextureType
  textureType() const {
    return info_.type;
  }

  WMTPixelFormat
  pixelFormat() const {
    return info_.pixel_format;
  }

  /* ml745: how many top mip levels this texture does NOT physically have.
   *
   * The D3D-visible descriptor keeps its full mip count; only the Metal texture
   * is smaller. Uploads therefore arrive with LOGICAL mip indices and must be
   * shifted down by this much, and uploads to a level below it dropped entirely.
   * The equivalent value on the D3D wrapper is unreachable from the context, and
   * WMTTextureInfo crosses the wine/unix ABI boundary, so it lives here.
   *
   * Logical mip N has exactly the dimensions of physical mip N-bias, so only the
   * INDEX shifts -- extents and pitches computed from the logical descriptor stay
   * correct. */
  uint32_t mipBias() const { return mip_bias_; }
  void setMipBias(uint32_t bias) { mip_bias_ = bias; }
  /* ml1252: PHYSICAL levels a mipClampBC copy could not fill (its source level
   * was clamped away). Bit n = level n, any slice. Sampling skips a lost level
   * at the top (see TextureMetadata's caller) and UpdateTexture refills it from
   * the level above by a CPU box filter when that one is streamed in. */
  uint32_t mipLevelCount() const { return info_.mipmap_level_count; }   /* ml1252: physical */
  uint32_t lostLevels() const { return lost_levels_.load(std::memory_order_relaxed); }
  void markLevelLost(uint32_t level) { if (level < 32) lost_levels_.fetch_or(1u << level, std::memory_order_relaxed); }
  void markLevelFilled(uint32_t level) { if (level < 32) lost_levels_.fetch_and(~(1u << level), std::memory_order_relaxed); }
  /* ml1256: bumped whenever the GAME changes a physical level (copy or upload
   * into it), so a refill computed off-thread from older data is discarded. */
  uint32_t levelGen(uint32_t level) const { return level < 16 ? level_gen_[level].load(std::memory_order_relaxed) : 0; }
  void noteLevelWritten(uint32_t level) { if (level < 16) level_gen_[level].fetch_add(1, std::memory_order_relaxed); }
  /* first PHYSICAL mip a view starts at */
  uint32_t
  firstMiplevel(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view].firstMiplevel;
  }

  WMTTextureType
  textureType(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].type;
  }

  WMTPixelFormat
  pixelFormat(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].format;
  }

  WMTTextureUsage
  usage() const {
    return info_.usage;
  }

  unsigned
  sampleCount() const {
    return info_.sample_count;
  }

  unsigned
  width() const {
    return info_.width;
  }

  unsigned
  height() const {
    return info_.height;
  }

  unsigned
  depth() const {
    return info_.depth;
  }

  unsigned
  width(TextureViewKey view) {
    return std::max(info_.width >> view.mip_start, 1u);
  }

  unsigned
  height(TextureViewKey view) {
    return std::max(info_.height >> view.mip_start, 1u);
  }

  unsigned
  depth(TextureViewKey view) const {
    return std::max(info_.depth >> view.mip_start, 1u);
  }

  /**
  \warning for cube texture, this would be multiple of 6.
  */
  unsigned
  arrayLength() const {
    switch (info_.type) {
    case WMTTextureTypeCubeArray:
    case WMTTextureTypeCube:
      return info_.array_length * 6;
    default:
      break;
    }
    return info_.array_length;
  }

  unsigned
  arrayLength(TextureViewKey view) {
    return view.array_end - view.array_start;
  }

  unsigned
  miplevelCount() const {
    return info_.mipmap_level_count;
  }

  TextureViewKey fullView;

  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags);
  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset = 0);
  Rc<TextureAllocation> import(mach_port_t mach_port);

  // A second device needs its own dependency tracking over the same Metal texture.
  Rc<Texture> shareTo(WMT::Device device);

  TextureView &view(TextureViewKey key);
  TextureView &view(TextureViewKey key, TextureAllocation *allocation);

  TextureViewKey checkViewUseArray(TextureViewKey key, bool isArray);
  TextureViewKey checkViewUseFormat(TextureViewKey key, WMTPixelFormat format);
  // Derive a view that applies a per-channel sample swizzle (d3d9
  // X-channel / luminance / depth-replicate formats). Returns `key`
  // unchanged when the swizzle already matches. See checkViewUseFormat.
  TextureViewKey checkViewUseSwizzle(TextureViewKey key, WMTTextureSwizzleChannels swizzle);
  // Derive a view clamped to mips [firstMiplevel, firstMiplevel+count).
  // d3d9 SetLOD(N) clamps sampling to mips N..(level_count-1). Returns
  // `key` unchanged when the range already matches.
  TextureViewKey checkViewUseMipRange(TextureViewKey key, uint32_t firstMiplevel, uint32_t miplevelCount);

  Rc<TextureAllocation> rename(Rc<TextureAllocation> &&newAllocation);

  Texture(const WMTTextureInfo &info, WMT::Device device);

  Texture(unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &info, WMT::Device device);

private:
  void prepareAllocationViews(TextureAllocation* allocation);

  WMTTextureInfo info_;
  uint32_t mip_bias_ = 0;          /* ml745, see mipBias() */
  std::atomic<uint32_t> lost_levels_{0}; /* ml1252, see lostLevels() */
  std::atomic<uint32_t> level_gen_[16] = {}; /* ml1256, see levelGen() */
  unsigned bytes_per_image_ = 0;
  unsigned bytes_per_row_ = 0;

  Rc<TextureAllocation> current_;
  uint32_t version_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};

  small_vector<TextureViewDescriptor, 4> viewDescriptors_;
  dxmt::shared_mutex mutex_;
  WMT::Device device_;
};

class RenamableTexturePool {
public:
  void incRef();
  void decRef();
  Rc<TextureAllocation> getNext(uint64_t frame_);

  RenamableTexturePool(Texture *texture, size_t capacity,Flags<TextureAllocationFlag> allocation_flags);

private:
  Rc<Texture> texture_;
  std::vector<Rc<TextureAllocation>> allocations_;
  uint64_t last_frame_ = 0;
  Flags<TextureAllocationFlag> allocation_flags_;
  unsigned capacity_;
  unsigned current_index_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace dxmt