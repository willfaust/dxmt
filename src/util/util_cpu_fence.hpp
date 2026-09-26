#pragma once
#include "util_futex.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

/* MADEIRA (WOW64_DESIGN.md, ml1070): the wait and the notify go through
 * dxmt::atomic_wait / dxmt::atomic_notify_all rather than through
 * std::atomic<uint64_t>::wait / notify_all.  The semantics are identical --
 * wait returns only once the value differs, notify happens after the store --
 * but on a PE target the standard-library pair parks on a hashed slot of a
 * 256-entry global contention table and can silently degrade to a sleep
 * ladder; see the header comment in util_futex.hpp.  DXMT_WAIT_ON_ADDRESS=0
 * puts this class back on std::atomic exactly. */
class CpuFence {
public:
  void wait(uint64_t value) {
    auto current = value_.load(std::memory_order_acquire);
    while (current < value) {
      dxmt::atomic_wait(value_, current);
      current = value_.load(std::memory_order_acquire);
    }
  }

  void signal(uint64_t value) {
    auto current = value_.load(std::memory_order_relaxed);
    do {
      if (value <= current)
        return;
    } while (!value_.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed));
    dxmt::atomic_notify_all(value_);
  }

  uint64_t signaledValue() {
    return value_.load(std::memory_order_acquire);
  }

  CpuFence(): value_(0) {}
  CpuFence(uint64_t initial_value): value_(initial_value) {}

private:
  std::atomic<uint64_t> value_;
};
} // namespace dxmt
