/*
 * This file is part of DXMT.
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <windows.h>

#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

/* MADEIRA (WOW64_DESIGN.md section 8.2(d)): natively there is no user32 or
 * kernel32 under this file -- GetCurrentThreadId, SwitchToThread and
 * YieldProcessor come from util_madeira_compat.h, which implements the first
 * two for real (a stub returning 0 would make the spinlock below believe
 * every thread is the owner).  See research/dxmt/LICENSE-MADEIRA.md. */
#ifdef DXMT_MADEIRA
#include "util_madeira_compat.h"
#include <unistd.h>
#endif

namespace dxmt {

/* A spinlock the same thread can acquire multiple times, keyed by the
 * Windows thread id. Public D3D9 entry points nest (Reset applies
 * state through the same setters an app calls), so the device lock
 * must be recursive. DXVK sync_recursive.h. */
class D9RecursiveSpinlock {
public:
  void
  lock() {
    // Bounded pause-spin, then yield the core: the holder can be parked
    // on a GPU fence for milliseconds (a synchronizing Lock), and a pure
    // spin would burn a core for that whole window. DXVK's sync::spin
    // takes the same two-phase shape.
    //
    // MADEIRA ml2000: a waiter that has yielded for a long time switches
    // SwitchToThread for Sleep(1) (MADEIRA_D9_LOCK_BACKOFF=0 keeps the pure
    // yield). SwitchToThread only yields to a thread that is ready on the
    // SAME core; with few cores and QoS a preempted owner can starve behind a
    // yielding waiter indefinitely. Acquisition semantics are unchanged. A
    // wait over 1 s is reported once per wait, with the owner, recursion
    // depth and waiter ([d3d9-lock-spin] ml2000; MADEIRA_D9_LOCK_DIAG=0).
    if (try_lock())
      return;
    uint32_t rounds = 0;
    bool reported = false;
    std::chrono::steady_clock::time_point start{};
    while (!try_lock()) {
      for (uint32_t i = 0; i < 2000; i++) {
        YieldProcessor();
        if (try_lock()) {
          if (reported)
            report_acquired(start);
          return;
        }
      }
      rounds++;
      if (rounds == 1)
        start = std::chrono::steady_clock::now();
      if (rounds >= kBackoffRounds && backoff_enabled())
        backoff_sleep();
      else
        ::SwitchToThread();
      if (!reported && (rounds & 15) == 0 && diag_enabled() &&
          std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
        reported = report_spin(start, rounds);
    }
    if (reported)
      report_acquired(start);
  }

  void
  unlock() {
    if (m_counter == 0)
      m_owner.store(0, std::memory_order_release);
    else
      m_counter -= 1;
  }

  bool
  try_lock() {
    uint32_t thread_id = ::GetCurrentThreadId();
    uint32_t expected = 0;

    bool status = m_owner.compare_exchange_weak(expected, thread_id, std::memory_order_acquire);
    if (status)
      return true;

    if (expected != thread_id)
      return false;

    m_counter += 1;
    return true;
  }

private:
  /* ~2000 pauses + a yield per round: 256 rounds is several milliseconds of
   * pure yielding before the waiter starts parking for 1 ms at a time. */
  static constexpr uint32_t kBackoffRounds = 256;

  static bool
  backoff_enabled() {
    static const bool enabled = env::getEnvVar("MADEIRA_D9_LOCK_BACKOFF") != "0";
    return enabled;
  }

  static bool
  diag_enabled() {
    static const bool enabled = env::getEnvVar("MADEIRA_D9_LOCK_DIAG") != "0";
    return enabled;
  }

  static void
  backoff_sleep() {
#ifdef DXMT_MADEIRA
    ::usleep(1000);
#else
    ::Sleep(1);
#endif
  }

  /* Run-wide cap so a lock that is contended for minutes cannot flood the log
   * it is being diagnosed in. */
  static std::atomic<uint32_t> &
  report_count() {
    static std::atomic<uint32_t> count{0u};
    return count;
  }

  bool
  report_spin(std::chrono::steady_clock::time_point start, uint32_t rounds) {
    if (report_count().fetch_add(1, std::memory_order_relaxed) >= 32)
      return false;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    Logger::warn(str::format("[d3d9-lock-spin] ml2000 device lock waited ", ms, " ms: waiter=",
        ::GetCurrentThreadId(), " owner=", m_owner.load(std::memory_order_relaxed),
        " owner-depth=", *static_cast<volatile uint32_t *>(&m_counter), " rounds=", rounds,
        " backoff=", backoff_enabled() ? "sleep1" : "yield",
        " (MADEIRA_D9_LOCK_DIAG=0 silences, MADEIRA_D9_LOCK_BACKOFF=0 pure yield)"));
    return true;
  }

  void
  report_acquired(std::chrono::steady_clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    Logger::warn(str::format("[d3d9-lock-spin] ml2000 waiter=", ::GetCurrentThreadId(),
        " acquired the device lock after ", ms, " ms"));
  }

  std::atomic<uint32_t> m_owner = {0u};
  uint32_t m_counter = {0u};
};

/* RAII device lock, cheaper than std::unique_lock: one pointer, no
 * state flags. Default-constructed = no-op, so the unprotected path
 * costs nothing. DXVK d3d9_multithread.h. */
class D9DeviceLock {
public:
  D9DeviceLock() : m_mutex(nullptr) {}

  D9DeviceLock(D9RecursiveSpinlock &mutex) : m_mutex(&mutex) {
    mutex.lock();
  }

  D9DeviceLock(D9DeviceLock &&other) : m_mutex(other.m_mutex) {
    other.m_mutex = nullptr;
  }

  D9DeviceLock &
  operator=(D9DeviceLock &&other) {
    if (m_mutex)
      m_mutex->unlock();
    m_mutex = other.m_mutex;
    other.m_mutex = nullptr;
    return *this;
  }

  D9DeviceLock(const D9DeviceLock &) = delete;
  D9DeviceLock &operator=(const D9DeviceLock &) = delete;

  ~D9DeviceLock() {
    if (m_mutex != nullptr)
      m_mutex->unlock();
  }

private:
  D9RecursiveSpinlock *m_mutex;
};

/* Serializes the D3D9 API when the app created the device with
 * D3DCREATE_MULTITHREADED: every public entry point takes the lock, so
 * app worker threads (resource streaming) cannot race the render
 * thread through the calling-thread state. Without the flag the app
 * promises single-threaded use and AcquireLock degenerates to a no-op,
 * matching the native runtime and DXVK; wined3d locks unconditionally.
 * Coverage is broader than DXVK's: resource methods and constant
 * getters take the lock too, for uniform greppable coverage; the cost
 * is a recursive re-acquire on the forwarding paths.
 * The queue's encode / finish threads never take this lock: their
 * shared state is either owned by them alone or independently
 * synchronized, and GPU progress must not depend on it. */
class D9Multithread {
public:
  D9Multithread(bool is_protected) : m_protected(is_protected) {}

  D9DeviceLock
  AcquireLock() {
    return m_protected ? D9DeviceLock(m_mutex) : D9DeviceLock();
  }

private:
  bool m_protected;
  D9RecursiveSpinlock m_mutex;
};

} // namespace dxmt
