#include "dxmt_buffer.hpp"
#include "dxmt_format.hpp"
#include "thread.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "wsi_platform.hpp"
#include <cassert>
#include <mutex>
#include "dxmt_mem_census.hpp"

namespace dxmt {

std::atomic_uint64_t global_buffer_seq = {0};

BufferAllocation::BufferAllocation(WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags) :
    info_(info),
    flags_(flags) {
  // (sub)allocate a minimum of 256B buffer so that texture can be created
  info_.length = std::max(info_.length, 256ull);
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage) && suballocation_size_ <= DXMT_PAGE_SIZE) {
    suballocation_size_ = align(info_.length, 16);
    suballocation_count_ = DXMT_PAGE_SIZE / suballocation_size_;
    info_.length = DXMT_PAGE_SIZE;
  }
  if (flags_.test(BufferAllocationFlag::CpuPlaced)) {
    placed_buffer = wsi::aligned_malloc(info_.length, DXMT_PAGE_SIZE);
    info_.memory.set(placed_buffer);
  }
  obj_ = device.newBuffer(info_);
  g_live_bufalloc.created.fetch_add(1, std::memory_order_relaxed);   /* ml684 */
  census_bytes_ = info_.length;                            /* ml677 */
  census_storage_ = ((uint32_t)info_.options >> 4) & 3;    /* ml678 storage mode */
  census_flags_ = flags_.raw();
  /* ml682: level 1 = whoever called Buffer::allocate, which is the interesting
   * frame (level 0 would just be Buffer::allocate itself, identical for all). */
  /* ml683: charging moved to Buffer::allocate, where the SITE LABEL is known.
   * Allocations created outside that path stay uncharged and show up as the
   * gap between mem-census buffer live and the sum of the site rows. */
  mem_census_add(MEMOWN_BUFFER, census_bytes_);
  mem_census_buffer_detail(census_bytes_, census_storage_, census_flags_, 1);
  mem_census_set_device(&device);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
  depkey = EncoderDepSet::generateNewKey(global_buffer_seq.fetch_add(1));
};

BufferAllocation::~BufferAllocation() {
  g_live_bufalloc.destroyed.fetch_add(1, std::memory_order_relaxed);  /* ml684 */
  mem_census_sub(MEMOWN_BUFFER, census_bytes_);            /* ml677 */
  if (census_site_) buf_site_sub((uint64_t)(uintptr_t)census_site_, census_bytes_);  /* ml683 */
  mem_census_buffer_detail(census_bytes_, census_storage_, census_flags_, 0);  /* ml678 */
  if (placed_buffer) {
    wsi::aligned_free(placed_buffer);
    placed_buffer = nullptr;
  }
}

WMT::Texture
Buffer::view(BufferViewKey key) {
  return view(key, current_.ptr());
};

WMT::Texture
Buffer::view(BufferViewKey key, BufferAllocation *allocation) {
  return view_(key, allocation).texture;
};

BufferView const &
Buffer::view_(BufferViewKey key) {
  return view_(key, current_.ptr());
};

BufferView const &
Buffer::view_(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key];
};

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key) {
  return residency(key, current_.ptr());
}

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return allocation->cached_view_[key]->residency;
}

void
Buffer::prepareAllocationViews(BufferAllocation *allocation) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  for (unsigned version = allocation->version_; version < version_; version++) {
    auto format = viewDescriptors_[version].format;
    auto texel_size = MTLGetTexelSize(format);
    assert(texel_size);
    assert(!(allocation->suballocation_size_ & (texel_size - 1)));
    auto total_length = allocation->suballocation_size_ * allocation->suballocation_count_;
    WMTTextureInfo info;
    info.type = WMTTextureTypeTextureBuffer;
    info.width = total_length / (uint64_t)texel_size;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.pixel_format = format;
    info.options = allocation->info_.options;
    auto usage = WMTTextureUsageShaderRead;
    if (!allocation->flags().test(BufferAllocationFlag::GpuReadonly) &&
       ( allocation->flags().test(BufferAllocationFlag::GpuManaged) ||  allocation->flags().test(BufferAllocationFlag::GpuPrivate))) {
      usage |= WMTTextureUsageShaderWrite;
      if (format == WMTPixelFormatR32Uint || format == WMTPixelFormatR32Sint ||
          (format == WMTPixelFormatRG32Uint && device_.supportsFamily(WMTGPUFamilyApple8))) {
        usage |= WMTTextureUsageShaderAtomic;
      }
    }
    info.usage = usage;

    auto view = allocation->obj_.newTexture(info, 0, total_length);

    allocation->cached_view_.push_back(std::make_unique<BufferView>(
        std::move(view), info.gpu_resource_id, allocation->suballocation_size_ / texel_size
    ));
  }
  allocation->version_ = version_;
};

BufferViewKey
Buffer::createView(BufferViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format == descriptor.format) {
      return i;
    }
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return i;
}

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, const char *site) {
  WMTResourceOptions options = WMTResourceStorageModeShared;
  if (flags.test(BufferAllocationFlag::GpuReadonly)) {
    options |= WMTResourceHazardTrackingModeUntracked;
  }
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = options;
  {
    auto *ba = new BufferAllocation(device_, info, flags);
    ba->census_site_ = site;                       /* ml683 */
    buf_site_add((uint64_t)(uintptr_t)site, ba->census_bytes_);
    return ba;
  }
};

Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&newAllocation) {
  g_mem_census.buf_renames.fetch_add(1, std::memory_order_relaxed);   /* ml678 */
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void
Buffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Buffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace dxmt