/*
 * util_futex.cpp -- backend selection and the platform address-wait calls.
 *
 * MADEIRA (WOW64_DESIGN.md, ml1070).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md and the long comment in util_futex.hpp for
 * why DXMT does not use std::atomic<T>::wait on a PE target.
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

#include "util_futex.hpp"

#include "log/log.hpp"
#include "util_env.hpp"

#include <string>

#if defined(_WIN32)
#include <windows.h>

/* Declared here rather than pulled in from winternl.h: mingw's winternl.h
 * does not carry these, and a four-line declaration is easier to check than
 * an include that also redefines half of NTSTATUS.  The @16/@4 stdcall
 * decorations these produce on i386 are exactly the ones libntdll.a exports,
 * so the linker resolves them against the real ntdll import descriptor. */
extern "C" {
LONG WINAPI RtlWaitOnAddress(const void *addr, const void *cmp, SIZE_T size, const LARGE_INTEGER *timeout);
void WINAPI RtlWakeAddressAll(const void *addr);
void WINAPI RtlWakeAddressSingle(const void *addr);
}
#endif

namespace dxmt::futex {

/* Set by the one-time initialiser below and read by every wait and every
 * notify.  Not thread_local, not a pointer into a module: a plain function
 * scoped static, so the pair of threads on either side of a hand-off cannot
 * end up on different backends. */
static WaitBackend
select_backend() {
#if defined(_WIN32)
  constexpr bool have_address_wait = true;
#else
  /* Darwin: libc++'s std::atomic<T>::wait is already __ulock_wait, which is
   * the platform futex.  There is nothing here to improve and a wrapper would
   * only add a branch, so the native (-DDXMT_MADEIRA) build keeps what it
   * has -- that is the no-regression requirement, expressed as a branch that
   * is decided once instead of as an #ifdef at every call site. */
  constexpr bool have_address_wait = false;
#endif

  std::string knob = env::getEnvVar("DXMT_WAIT_ON_ADDRESS");
  bool disabled = (knob == "0" || knob == "off" || knob == "no");

  if (!have_address_wait) {
    Logger::warn("[dxmt-wait] ml1070 backend=std::atomic (platform wait/notify is already a futex)");
    return WaitBackend::StdAtomic;
  }
  if (disabled) {
    Logger::warn("[dxmt-wait] ml1070 backend=std::atomic -- DISABLED by DXMT_WAIT_ON_ADDRESS=0 "
                 "(libc++ hashed contention table, spin/yield/sleep ladder)");
    return WaitBackend::StdAtomic;
  }
  Logger::warn("[dxmt-wait] ml1070 backend=wait-on-address on the object's own word "
               "(DXMT_WAIT_ON_ADDRESS=0 reverts to std::atomic wait/notify)");
  return WaitBackend::AddressWait;
}

WaitBackend
backend() {
  static const WaitBackend selected = select_backend();
  return selected;
}

#if defined(_WIN32)

void
address_wait(const void *addr, const void *compare, size_t size) {
  RtlWaitOnAddress(addr, compare, (SIZE_T)size, nullptr);
}

void
address_wake_one(const void *addr) {
  RtlWakeAddressSingle(addr);
}

void
address_wake_all(const void *addr) {
  RtlWakeAddressAll(addr);
}

#else

/* Unreachable: backend() is StdAtomic on every target that gets here, and
 * dxmt::atomic_wait/notify take the std::atomic arm before they can call
 * these.  They exist because the branch is a runtime one -- deliberately, so
 * that the native and PE halves compile the same code -- and a runtime branch
 * still needs both arms to link. */
void
address_wait(const void *, const void *, size_t) {}
void
address_wake_one(const void *) {}
void
address_wake_all(const void *) {}

#endif

} // namespace dxmt::futex
