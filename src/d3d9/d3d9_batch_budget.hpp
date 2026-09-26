#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace dxmt {
// ml1960: speculative capacity must not scale with an old scene's largest pass.
constexpr size_t kD9BatchReserveBytes = 64 * 1024;
// ml1970: the pressure submit is charged the bytes a batch actually holds (see
// d9BatchCharge), so the threshold describes real queued work. The ml1960
// capacity charge billed every tiny state/query flush a full 4 x 64 KiB reserve
// and forced ~200 submissions per second during gameplay (periodic freezes).
constexpr size_t kD9BatchCommitBytes = 16 * 1024 * 1024;
constexpr size_t kD9BatchLegacyCommitBytes = 8 * 1024 * 1024;
// ml1970: below this a sparse capture is trimmed before it is queued.
constexpr size_t kD9BatchSmallTrimBytes = 4 * 1024;

template <typename T>
size_t d9BatchReserve(size_t peak, bool bounded) {
  return bounded ? std::min(peak, kD9BatchReserveBytes / sizeof(T)) : peak;
}

template <typename T>
void d9CompactBatch(std::vector<T>& batch, bool bounded, bool trim_small = false) {
  if (!bounded)
    return;
  bool large_sparse = batch.capacity() > kD9BatchReserveBytes / sizeof(T) &&
      batch.size() < batch.capacity() / 2;
  // ml1970: a flush that used a sliver of its reserve would otherwise carry the
  // whole reserve into the queued chunk until the GPU retires it.
  bool small_sparse = trim_small && batch.capacity() * sizeof(T) > kD9BatchSmallTrimBytes &&
      batch.size() < batch.capacity() / 4;
  if (large_sparse || small_sparse) {
    std::vector<T> compact(std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    batch.swap(compact);
  }
}

// ml1970: what a captured batch costs the queue. used=false is the ml1960 rule.
template <typename T>
size_t d9BatchCharge(const std::vector<T>& batch, bool used) {
  return (used ? batch.size() : batch.capacity()) * sizeof(T);
}
}
