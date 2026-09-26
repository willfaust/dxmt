#pragma once

#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_capture.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_list.hpp"
#include "dxmt_context.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include "util_futex.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace dxmt {

enum class GpuCompletionStatus : uint8_t {
  Pending,
  Complete,
  Failed,
};

// Notified once the command buffer a chunk was encoded into has finished, so
// an object can retire whatever it was keeping alive for that GPU work.
class GpuCompletionTarget {
public:
  GpuCompletionTarget() = default;
  GpuCompletionTarget(const GpuCompletionTarget &) = delete;
  GpuCompletionTarget &operator=(const GpuCompletionTarget &) = delete;
  virtual ~GpuCompletionTarget() noexcept = default;
  virtual void CompleteGpuWork(GpuCompletionStatus status) noexcept = 0;
};

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

constexpr uint32_t kCommandChunkCount = 32;

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *
  allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
    enc.$$setEncodingContext(
      chunk_id,
      frame_
    );
    auto& statistics = enc.currentFrameStatistics();
    auto t0 = clock::now();
    list_enc.execute(enc);
    attached_cmdbuf = cmdbuf;
    auto t1 = clock::now();
    visibility_readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id);
    auto t2 = clock::now();
    statistics.encode_prepare_interval += (t1 - t0);
    statistics.encode_flush_interval += (t2 - t1);
  };

  void
  addCompletionTarget(std::shared_ptr<GpuCompletionTarget> target) {
    if (!target)
      return;
    std::lock_guard<dxmt::mutex> lock(completion_targets_mutex_);
    completion_targets.push_back(std::move(target));
  }

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  std::unique_ptr<VisibilityResultReadback> visibility_readback;
  uint64_t resource_initializer_event_id;
  std::vector<std::shared_ptr<GpuCompletionTarget>> completion_targets;
  dxmt::mutex completion_targets_mutex_;

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;
  
  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() noexcept {
    signal_frame_latency_fence_ = ~0ull;
    visibility_readback = {};
    completion_targets.clear();
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
  }
};

class CommandQueue {

private:
  std::atomic<bool> device_error_ = false;
  void CommitChunkInternal(CommandChunk &chunk, uint64_t seq);

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped;

  std::array<CommandChunk, kCommandChunkCount> chunks;
  uint64_t encoder_seq = 1;
  uint64_t frame_count = 0;
  uint32_t max_latency_ = 3;

  dxmt::thread encodeThread;
  dxmt::thread finishThread;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

  obj_handle_t shared_event_listener;
  dxmt::thread event_listener_thread;

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  RingBumpState<StagingBufferBlockAllocator> staging_allocator;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize, dxmt::null_mutex> cpu_command_allocator;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_allocator;
  CaptureState capture_state;

public:
  InternalCommandLibrary cmd_library;
  ArgumentEncodingContext argument_encoding_ctx;
  WMT::Reference<WMT::SharedEvent> event;
  std::uint64_t current_event_seq_id = 0;
  FrameStatisticsContainer statistics;
  ResourceInitializer initializer;

  CommandQueue(WMT::Device device);

  // Sticky: set once a command buffer retires in the Error status, so a
  // frontend can report a lost/removed device instead of continuing to submit
  // work the driver has already given up on.
  bool HasDeviceError() const {
    return device_error_.load(std::memory_order_acquire);
  }

  void MarkDeviceError();

  ~CommandQueue();

  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % kCommandChunkCount];
  };

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  };

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  };

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  };

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  };


  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  };

  obj_handle_t GetSharedEventListener() {
    return shared_event_listener;
  }

  /**
  This is not thread-safe!
  CurrentChunk & CommitCurrentChunk should be called on the same thread

  */
  void CommitCurrentChunk();

  uint64_t CurrentFrameSeq() {
    return frame_count + 1;
  }

  FrameStatistics& CurrentFrameStatistics() {
    return statistics.at(frame_count);
  }

  void
  PresentBoundary() {
    statistics.compute(frame_count);
    frame_count++;
    statistics.at(frame_count).reset();
    // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering 
    if (likely(frame_count > max_latency_)) {
      auto t0 = clock::now();
      frame_latency_fence_.wait(frame_count - max_latency_);
      auto t1 = clock::now();
      statistics.at(frame_count).present_lantency_interval += (t1 - t0);
    }
    statistics.at(frame_count).latency = max_latency_;
  }

  uint32_t GetMaxLatency() { return max_latency_; }

  void SetMaxLatency(uint32_t value) { max_latency_ = value; };

  // Highest frame seq whose present chunk has retired, signaled on the frame
  // latency fence. d3d9's D3DPRESENT_DONOTWAIT probe peeks this to decide
  // whether the end-of-Present frame-latency throttle would block, and returns
  // D3DERR_WASSTILLDRAWING instead of entering the wait.
  uint64_t FrameLatencySignaled() { return frame_latency_fence_.signaledValue(); }

  // Block until the present chunk from max_latency frames back has retired,
  // capping how far the calling thread runs ahead of the GPU. Present pacing is
  // applied per frontend rather than in PresentBoundary; the other back ends
  // pace through their own swapchain fence or present semaphore, so d3d9 rides
  // the frame-latency fence it stamps on each present chunk.
  void WaitFrameLatency(uint64_t frame_seq) {
    if (frame_seq > max_latency_)
      frame_latency_fence_.wait(frame_seq - max_latency_);
  }

  void
  WaitCPUFence(uint64_t seq) {
    cpu_coherent.wait(seq);
  };

  // MADEIRA: bounded, cooperative counterpart of WaitCPUFence, for a caller
  // that is NOT allowed to block. D3D9's IDirect3DQuery9::GetData is the
  // motivating one: its contract is "report, never stall", so it has to come
  // back with S_FALSE while the GPU is still running -- but an application
  // that builds a GPU fence out of
  // `while (GetData(..., D3DGETDATA_FLUSH) == S_FALSE) {}` then spins its
  // render thread at full speed for a whole frame. Parking that thread here
  // for a capped slice turns tens of thousands of no-op polls per frame into
  // a few hundred and observes the completion within microseconds of it
  // happening, instead of whenever the caller next happens to ask.
  //
  // Never waits longer than timeout_ns, so it cannot turn a non-blocking API
  // into a blocking one. Two phases, the same shape D9RecursiveSpinlock uses:
  // a short load-spin catches a completion that is already microseconds away
  // without paying for a context switch, then the core is handed to whoever
  // else is runnable -- which is exactly the encode and finish threads, the
  // only threads that can move this watermark at all. Returns true iff the
  // watermark reached seq before the deadline.
  bool WaitCPUFenceBounded(uint64_t seq, uint64_t timeout_ns);

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, size_t size) {
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset};
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] =
        cpu_command_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return ptr_add(block.ptr, offset);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace dxmt