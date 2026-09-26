/*
 * util_futex.hpp -- a real wait-on-address backend for DXMT's cross-thread
 * hand-offs.
 *
 * MADEIRA (WOW64_DESIGN.md, ml1070).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * WHY THIS EXISTS, stated from the object code rather than from the headers.
 *
 * Every cross-thread hand-off in DXMT is an `std::atomic<T>::wait()` paired
 * with `notify_one()`/`notify_all()`: CpuFence, the chunk ring between the
 * API thread and the encode thread, the commit ring between the encode thread
 * and the finish thread, the frame-latency fence, and the shader-ready flags.
 * On a Windows target built with llvm-mingw's libc++ those compile to
 *
 *   std::__libcpp_thread_poll_with_backoff( poll, __atomic_wait_backoff_impl )
 *       -> 64 bare poll iterations
 *       -> chrono::steady_clock::now()   (twice per backoff round)
 *       -> std::__atomic_monitor_global( addr )        \  a 256-entry GLOBAL
 *       -> std::__atomic_wait_global_table( addr, v )  /  contention table,
 *                                                         hashed by address
 *       -> std::__contention_wait<8, NoTimeout>
 *            -> WaitOnAddress, resolved LAZILY by
 *               GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll") +
 *               GetProcAddress -- and if that resolve ever returns null, a
 *               spin / SwitchToThread / sleep_for(elapsed/2) ladder that caps
 *               at 8 ms once a wait has been running for 128 ms.
 *
 * Three separate costs, all of them paid per hand-off, and the third one is
 * the dangerous one:
 *
 *  1. The object is NEVER waited on directly.  `__has_native_atomic_wait<T>`
 *     is `is_same_v<T, __cxx_contention_t>` unless the unstable-ABI macro
 *     _LIBCPP_ABI_ATOMIC_WAIT_NATIVE_BY_SIZE is on, and the shipped llvm-mingw
 *     libc++ is ABI v1, so it is off.  __cxx_contention_t is `int64_t`;
 *     `uint64_t` is not the same type and `bool` certainly is not.  So every
 *     one of DXMT's waits lands in the hashed global table instead of on its
 *     own word -- verified by compiling both `std::atomic<uint64_t>::wait` and
 *     `std::atomic<bool>::wait` for i686-w64-mingw32 and aarch64-w64-mingw32
 *     and reading the relocations.  Two unrelated atomics that hash to the
 *     same slot wake each other, and notify_all() on the table slot is a
 *     WakeByAddressAll over every waiter in that slot.
 *  2. Two `steady_clock::now()` calls per backoff round.  Under emulation that
 *     is two guest calls into the performance counter before the thread has
 *     even decided to park.
 *  3. The WaitOnAddress pointer is resolved with GetModuleHandleW, NOT
 *     LoadLibraryW, against an API-set name.  If that module is not resolvable
 *     as a loaded module at the moment of the first wait, the static resolves
 *     to null ONCE and the process spends the rest of its life on the
 *     sleep_for ladder -- silently, with no log line anywhere, and with a wake
 *     latency quantised by the host timer instead of by the signaller.
 *
 * WHAT THIS HEADER DOES INSTEAD.  A short relaxed-load spin, then a wait on
 * the atomic's OWN address through the platform's address-wait primitive, in
 * a re-check loop.  On a PE target that is ntdll's RtlWaitOnAddress /
 * RtlWakeAddressSingle / RtlWakeAddressAll -- the same functions kernelbase's
 * WaitOnAddress / WakeByAddressSingle / WakeByAddressAll are (WaitOnAddress
 * calls RtlWaitOnAddress; the two Wake entry points are literal forwards to
 * the Rtl ones).  They are taken from ntdll rather than from the API set
 * because ntdll.dll is always present under its own name in the module list,
 * while the API-set name has to be resolved through the apiset schema -- which
 * is exactly the resolution whose silent failure is failure mode 3 above.
 *
 * On the native (-DDXMT_MADEIRA) half nothing changes: there libc++ is
 * Apple's, `std::atomic<T>::wait` already has a __ulock backend, and this
 * header forwards straight back to it.
 *
 * KNOB: DXMT_WAIT_ON_ADDRESS=0 restores `std::atomic<T>::wait/notify` on every
 * target.  The chosen backend is announced once, on first use.
 *
 * LOST WAKEUPS.  Every waiter here is a value re-check loop and every
 * signaller stores before it notifies, so the pairing is the ordinary one:
 * RtlWaitOnAddress re-compares the word inside the queue spinlock that
 * RtlWakeAddress* takes, so a store that lands before the compare is seen and
 * a store that lands after it finds the waiter already queued; and the wake
 * itself is a per-thread auto-reset alert, so an alert delivered before the
 * park completes is not lost either.  A spurious return -- including one
 * caused by the non-atomic 8-byte compare RtlWaitOnAddress does on a 32-bit
 * target -- costs one extra loop iteration and nothing else, because the loop
 * re-reads the value with a proper atomic load before it believes anything.
 * build/dxmt-tests/futex-host-test.cpp is the producer/consumer model.
 *
 * Copyright 2026 Will Faust
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dxmt {

enum class WaitBackend : uint32_t {
  /* The atomic's own address, through the platform address-wait primitive. */
  AddressWait = 0,
  /* std::atomic<T>::wait / notify, whatever the standard library does. */
  StdAtomic = 1,
};

namespace futex {

/**
 * \brief Backend in use, decided once and logged once.
 *
 * Reads DXMT_WAIT_ON_ADDRESS on first call.  Never changes afterwards, so a
 * waiter and a signaller can never disagree about which mechanism they are on.
 */
WaitBackend backend();

/**
 * \brief Park until the \c size bytes at \c addr differ from \c compare.
 *
 * May return spuriously.  Only called when backend() == AddressWait.
 */
void address_wait(const void *addr, const void *compare, size_t size);

void address_wake_one(const void *addr);
void address_wake_all(const void *addr);

/**
 * \brief One relaxed pause, for the pre-park spin.
 */
inline void
spin_hint() {
  /* arm64ec is checked first on purpose: it is ARM64 code that also defines
   * the x86_64 predefines for source compatibility, so the x86 arm would
   * otherwise win and __builtin_ia32_pause would not compile. */
#if defined(_M_ARM64EC) || defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

/* The same count, and the same reason, as CommandQueue::WaitCPUFenceBounded's
 * phase 1: enough relaxed loads to catch a hand-off that is already in flight
 * on another core, few enough that a wait which is genuinely going to be long
 * has not paid for anything.  Deliberately NOT a timed spin -- the timed,
 * credit-metered one lives in madeira_fast_wait one layer down, where it can
 * see the park-to-satisfied histogram that says whether it is paying off. */
static constexpr unsigned kSpinIterations = 64;

} // namespace futex

/**
 * \brief \c a.wait(old, order), on a real futex where there is one.
 *
 * Returns only once the value differs from \c old, exactly like
 * std::atomic<T>::wait -- callers in this tree rely on that (the encode and
 * finish threads act on the sequence number they woke for without re-testing
 * it), so a spurious platform wake must not become a spurious return.
 */
template <typename T>
inline void
atomic_wait(std::atomic<T> &a, T old, std::memory_order order = std::memory_order_acquire) {
  if (futex::backend() == WaitBackend::StdAtomic) {
    a.wait(old, order);
    return;
  }
  for (unsigned i = 0; i < futex::kSpinIterations; i++) {
    if (a.load(order) != old)
      return;
    futex::spin_hint();
  }
  for (;;) {
    T current = a.load(order);
    if (current != old)
      return;
    /* `current` and `old` are equal here, so either is a valid comparand; the
     * platform re-reads the word under its own lock anyway. */
    futex::address_wait(std::addressof(a), std::addressof(current), sizeof(T));
  }
}

template <typename T>
inline void
atomic_notify_one(std::atomic<T> &a) {
  if (futex::backend() == WaitBackend::StdAtomic) {
    a.notify_one();
    return;
  }
  futex::address_wake_one(std::addressof(a));
}

template <typename T>
inline void
atomic_notify_all(std::atomic<T> &a) {
  if (futex::backend() == WaitBackend::StdAtomic) {
    a.notify_all();
    return;
  }
  futex::address_wake_all(std::addressof(a));
}

} // namespace dxmt
