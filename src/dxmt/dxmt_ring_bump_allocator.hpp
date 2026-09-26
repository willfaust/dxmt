#pragma once

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_math.hpp"
#include "util_env.hpp"
#include <cassert>
#include <mutex>
#include <queue>
#include "dxmt_mem_census.hpp"

namespace dxmt {

// Staging ring block size. An i386 (WoW64) module shares a 32-bit guest's few
// GB of address space with the application, and every block is guest VA plus
// host RAM plus a Metal buffer registration, so there the ring uses 8 MB
// blocks; every 64-bit target keeps 32 MB.
// MADEIRA (WOW64_DESIGN.md section 8.2(a)): the !DXMT_MADEIRA half reverts
// this toward the upstream tag for the native frontend. The 8 MB block is a
// concession to a 32-bit guest's VA ceiling, and natively these blocks leave
// the guest window entirely -- they are host allocations the application
// never sees -- so the 32 MB block that suits every other 64-bit target
// suits this one too.
#if defined(__i386__) && !defined(DXMT_MADEIRA)
constexpr size_t kStagingBlockSize = 0x800000; // 8MB
#else
constexpr size_t kStagingBlockSize = 0x2000000; // 32MB
#endif
constexpr size_t kStagingBlockSizeForDeferredContext = 0x200000; // 2MB
constexpr size_t kStagingBlockLifetime = 300;

inline bool ringOversizeReuseEnabled() {
  static const bool enabled = [] {
    const bool value = env::getEnvVar("DXMT_RING_OVERSIZE_REUSE") != "0";
    WARN("[ring-reuse] ml1180 enabled=", value, " retained-extra-blocks=2 max-block=16777216");
    return value;
  }();
  return enabled;
}

template <typename Allocator, size_t BlockSize = kStagingBlockSize, class mutex = dxmt::mutex> class RingBumpState {

public:

  static constexpr size_t block_size = BlockSize;

  // single_writer engages a DXMT_DEBUG only assertion that every allocate()
  // and seal_latest() runs on one fixed thread. Set it only for a ring whose
  // writer is a single persistent thread. A ring written by whichever thread
  // drives the API leaves it false: the OS thread id can change across calls
  // even where a device lock serialises them. free_blocks() and preallocate()
  // stay outside the check, since they may run off the writer thread under
  // mutex_.
  RingBumpState(Allocator &&allocator, bool single_writer = false) : allocator_(std::move(allocator)) {
#ifdef DXMT_DEBUG
    single_writer_ = single_writer;
#else
    (void)single_writer;
#endif
  }

  // Push count blocks of the ring's natural BlockSize onto the FIFO with
  // last_used_seq_id 0, so the first allocate() reuses them instead of
  // allocating on the calling thread. First touch of a freshly registered
  // block is expensive enough under translation to show up as a stutter if it
  // lands on a draw or a resource create, so this is called at device
  // construction where the cost hides in startup. No locks: the caller
  // serialises against any concurrent allocate(), which construction does by
  // not having published the device yet.
  void
  preallocate(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      fifo.push({
          .allocated_size = 0,
          .total_size = BlockSize,
          .last_used_seq_id = 0,
          .inc_time_to_live = 0,
          .block = allocator_.allocate(BlockSize),
      });
    }
  }

  // Retire the current back block so the next allocate() rotates onto a
  // different one instead of suballocating more of it. A CPU store into a
  // placed buffer that an in-flight command buffer references faults on every
  // store under x86 translation, and the rings otherwise hand one shared
  // latest block to many command buffers. Called at each submission boundary
  // this keeps every block referenced by a single submission. Append only: it
  // repurposes nothing and touches no pointer already handed out, so live
  // suballocations from the sealed block stay valid.
  void
  seal_latest() {
    std::lock_guard<mutex> lock(mutex_);
    note_single_writer();
    if (fifo.empty())
      return;
    auto &latest = fifo.back();
    latest.allocated_size = latest.total_size;
  }

  std::pair<typename Allocator::Block &, uint64_t>
  allocate(uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment);

  void free_blocks(uint64_t coherent_id);

private:
  struct Allocation {
    size_t allocated_size;
    size_t total_size;
    uint64_t last_used_seq_id;
    uint64_t inc_time_to_live;
    bool reusable_oversize = false;
    Allocator::Block block;
  };

  Allocation &allocate_or_reuse_block(uint64_t seq_id, uint64_t coherent_id, size_t block_size);

  std::pair<typename Allocator::Block &, uint64_t>
  suballocate(Allocation &allocation, size_t size, size_t alignment) {
    auto offset = align(allocation.allocated_size, alignment);
    allocation.allocated_size = offset + size;
    return {allocation.block, offset};
  };

#ifdef DXMT_DEBUG
  // Records the first allocate() / seal_latest() thread and asserts every
  // later call runs on it. A no-op unless single_writer_ was set at
  // construction. Callers already hold mutex_.
  void
  note_single_writer() {
    if (!single_writer_)
      return;
    uint32_t tid = this_thread::get_id();
    if (!writer_tid_)
      writer_tid_ = tid;
    assert(writer_tid_ == tid && "RingBumpState single-writer invariant violated");
  }
  bool single_writer_ = false;
  uint32_t writer_tid_ = 0;
#else
  void note_single_writer() {}
#endif

  std::queue<Allocation> fifo;
  unsigned reusable_oversize_blocks_ = 0;
  uint64_t oversize_reuses_ = 0;
  mutex mutex_;
  Allocator allocator_;
};

class GpuPrivateBufferBlockAllocator {
public:
  GpuPrivateBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options) {
    device_ = device;
    buffer_info_.memory.set(nullptr);
    buffer_info_.options = block_options;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;

    Block() = default;
    Block(const Block &copy) = delete;
    Block(Block &&move) = default;
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    buffer_info_.length = block_size;
    block.buffer = device_.newBuffer(buffer_info_);
    block.gpu_address = buffer_info_.gpu_address;
    return block;
  };

private:
  WMT::Device device_;
  WMTBufferInfo buffer_info_;
};

class StagingBufferBlockAllocator {
public:
  StagingBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options, bool placed_buffer = true) {
    device_ = device;
    buffer_info_ = block_options;
    placed_buffer_ = placed_buffer;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;
    void *mapped_address;

    ~Block() {
      mem_census_sub(MEMOWN_STAGING_RING, census_bytes);   /* ml677 */
      census_bytes = 0;
      /* ml789: RELEASE THE BUFFER BEFORE FREEING ITS MEMORY.
       *
       * mapped_address is handed to newBuffer as WMTBufferInfo.memory, so a
       * backend may hold it for the buffer's whole lifetime -- the remote
       * backend keeps it as the shadow it uploads from, and hashes it every
       * frame. Freeing it first left that backend reading a dead allocation
       * from another thread, which faulted mid-frame on an unmapped address.
       *
       * Member destruction would release `buffer` AFTER this body runs, which
       * is exactly the wrong order, so release it explicitly first. */
      buffer = nullptr;
      if (mapped_address) {
        free(mapped_address);
        mapped_address = nullptr;
      }
    };
    uint64_t census_bytes = 0;                             /* ml677 */

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      buffer = std::move(move.buffer);
      gpu_address = move.gpu_address;
      mapped_address = move.mapped_address;
      census_bytes = move.census_bytes;                    /* ml677: move the debt too */
      move.census_bytes = 0;
      move.mapped_address = nullptr;
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    bool placed = placed_buffer_;
#if defined(__i386__) && !defined(DXMT_MADEIRA)
    /* MADEIRA (WOW64_DESIGN.md section 7.5): placed_buffer=false means "let
     * Metal allocate and never look at the memory again".  That is fine for a
     * 64-bit caller, but for a 32-bit guest the unix side would have to write
     * [buffer contents] -- a pointer in Metal's own heap, outside the guest
     * window -- back into info.memory, which has no 32-bit address, so
     * _MTLDevice_newBuffer32 refuses the call and hands back a NULL buffer.
     *
     * The three placed_buffer=false rings are all on the D3D11 path and all
     * CPU-visible (Managed, which DXMT_IOS remaps to Shared):
     * CommandQueue::staging_allocator, MTLD3D11CommandList::staging_allocator
     * and ResourceInitializer::gpu_command_heap_allocator.  Their contents are
     * filled through MTLBuffer_updateContents, which memcpys into
     * [buffer contents] -- so Private storage is not an alternative either.
     * Supplying the backing from this PE module's own heap is: the allocation
     * goes through the guest window chokepoint by construction, exactly as it
     * does for Buffer::allocate's CpuPlaced (dxmt_buffer.cpp) and
     * Texture::allocate (dxmt_texture.cpp).  Nothing reads mapped_address on
     * these rings, so the only cost is the guest VA -- which is why
     * kStagingBlockSize is already 8 MB rather than 32 MB on i386. */
    placed = true;
#endif
    block.mapped_address = placed ? malloc(block_size) : nullptr;
    WMTBufferInfo info;
    info.options = buffer_info_;
    info.memory.set(block.mapped_address);
    info.length = block_size;
    block.buffer = device_.newBuffer(info);
    block.gpu_address = info.gpu_address;
    block.census_bytes = block_size;                       /* ml677 */
    mem_census_add(MEMOWN_STAGING_RING, block_size);
    return block;
  };

private:
  WMT::Device device_;
  WMTResourceOptions buffer_info_;
  bool placed_buffer_;
};

class HostBufferBlockAllocator {
public:
  class Block {
  public:
    void *ptr;

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      ptr = move.ptr;
      move.ptr = nullptr;
    };

    ~Block() {
      if (ptr) {
        free(ptr);
        ptr = nullptr;
      }
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.ptr = malloc(block_size);
    return block;
  };
};

template <typename Allocator, size_t BlockSize, class mutex>
std::pair<typename Allocator::Block &, uint64_t>
RingBumpState<Allocator, BlockSize, mutex>::allocate(
    uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment
) {
  std::lock_guard<mutex> lock(mutex_);
  note_single_writer();
  while (!fifo.empty()) {
    auto &latest = fifo.back();
    if ((align(latest.allocated_size, alignment) + size) > latest.total_size) {
      break;
    }
    latest.last_used_seq_id = seq_id;
    return suballocate(latest, size, alignment);
  }
  return suballocate(
      allocate_or_reuse_block(
          seq_id, coherent_id, std::max(size, BlockSize) // in case required size is larger than block size
      ),
      size, alignment
  );
};

template <typename Allocator, size_t BlockSize, class mutex>
void
RingBumpState<Allocator, BlockSize, mutex>::free_blocks(uint64_t coherent_id) {
  std::lock_guard<mutex> lock(mutex_);
  while (!fifo.empty()) {
    auto &front = fifo.front();
    if (front.last_used_seq_id > coherent_id)
      break;
    auto expired = (coherent_id - front.last_used_seq_id) > kStagingBlockLifetime ||
                   front.inc_time_to_live > kStagingBlockLifetime || coherent_id == -1ull;
    auto adhoc = front.total_size != BlockSize && !front.reusable_oversize;
    if (expired || adhoc) {
      // can be deallocated
      if (front.reusable_oversize) --reusable_oversize_blocks_;
      fifo.pop();
      continue;
    }
    front.inc_time_to_live++;
    break;
  }
};

template <typename Allocator, size_t BlockSize, class mutex>
RingBumpState<Allocator, BlockSize, mutex>::Allocation &
RingBumpState<Allocator, BlockSize, mutex>::allocate_or_reuse_block(
    uint64_t seq_id, uint64_t coherent_id, size_t block_size
) {
  while (!fifo.empty()) {
    auto &front = fifo.front();
    if (front.last_used_seq_id < coherent_id) {
      if (ringOversizeReuseEnabled() && front.total_size < block_size) {
        // A completed small block must not strand usable blocks behind it.
        // No pointer into it remains in flight at this coherent sequence.
        if (front.reusable_oversize) --reusable_oversize_blocks_;
        fifo.pop();
        continue;
      }
      if (front.total_size != BlockSize && !front.reusable_oversize) {
        fifo.pop();
        continue;
      } else if (front.total_size >= block_size) {
        if (front.reusable_oversize && ++oversize_reuses_ == 1)
          WARN("[ring-reuse] ml1180 recycled oversized block bytes=", front.total_size);
        front.last_used_seq_id = seq_id;
        front.allocated_size = 0;
        front.inc_time_to_live = 0;
        fifo.push(std::move(front));
        fifo.pop();
        return fifo.back();
      }
      WARN("forced to allocate new block of size ", block_size);
    }
    break;
  }
  // Repeated image uploads just above the normal block size otherwise create
  // and destroy a Metal buffer every frame. Retain at most two modest oversize
  // blocks per ring, with the same completion checks and expiry as normal ones.
  const bool reusable_oversize = ringOversizeReuseEnabled() && block_size > BlockSize &&
      block_size <= 16u * 1024u * 1024u && reusable_oversize_blocks_ < 2;
  if (reusable_oversize) ++reusable_oversize_blocks_;
  fifo.push({
      .allocated_size = 0,
      .total_size = block_size,
      .last_used_seq_id = seq_id,
      .inc_time_to_live = 0,
      .reusable_oversize = reusable_oversize,
      .block = allocator_.allocate(block_size),
  });
  return fifo.back();
};

} // namespace dxmt
