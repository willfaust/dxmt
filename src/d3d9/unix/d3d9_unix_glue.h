/*
 * d3d9_unix_glue.h -- the hand-written half of the generated unix entries.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.5).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * d3d9_unix.c is generated and states its requirements in its own prologue:
 * an NTSTATUS vocabulary, the four pointer macros, the pSharedHandle rule and
 * a log line.  This file supplies exactly those and nothing else -- the
 * object model, the handle table, the arena and the per-method hooks live in
 * d3d9_native_glue.cpp.
 *
 * Why the conversions are declared here rather than included from
 * build/ntdll-unix/ios_wow.h: DXMT's unix half is statically linked into the
 * same iOS binary as ntdll's, so `ios_wow_base` resolves at app link time.
 * winemetal_unix.c already takes exactly this route for UInt32ToPtr
 * (WOW64_DESIGN.md section 7.8), and it keeps this submodule free of a build
 * dependency on the Wine tree.
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

#ifndef __MADEIRA_D3D9_UNIX_GLUE_H
#define __MADEIRA_D3D9_UNIX_GLUE_H

#include <stdint.h>
#include <stdio.h>

#include "d3d9shim_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the ntdll vocabulary ---------------------------------------------- */

/* Deliberately not <winternl.h>: this compiles as part of libdxmt_unix.a,
 * outside the Wine build, and these six values are the whole surface
 * d3d9_unix.c uses.  Values are ntdll's own. */
#ifndef __MADEIRA_D3D9_NTSTATUS
#define __MADEIRA_D3D9_NTSTATUS
typedef int NTSTATUS;
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xc0000002)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#endif
#ifndef STATUS_REVISION_MISMATCH
#define STATUS_REVISION_MISMATCH ((NTSTATUS)0xc0000059)
#endif

/* ---- the guest window -------------------------------------------------- */

/* Resolved at app link time from build/ntdll-unix/virtual_ios.c.  B for the
 * CALLING THREAD's pseudo-process; 0 off a WoW thread, which is why every
 * conversion below degrades to "not a usable guest pointer" rather than to
 * the identity.  A unix entry always runs on the guest's own calling thread,
 * so B is available exactly where it is needed. */
extern unsigned long ios_wow_base(void);

#define D3D9_WINDOW_SIZE ((uint64_t)1 << 32)

static inline void *
d3d9_host_ptr(uint32_t addr) {
  unsigned long base = ios_wow_base();
  if (!addr || !base)
    return NULL;
  return (void *)(uintptr_t)((uint64_t)base + (uint64_t)addr);
}

/* Named by the three write-back fields and by nothing else.  Refusing rather
 * than truncating is the point: see d3d9_guest_ptr32() below. */
extern void d3d9_guest_ptr32_refused(const void *host);

/* MADEIRA_D3D9_LOCKCHECK=1 (read once in d3d9_native_census_configure).  In
 * window is not the same thing as writable: a reservation with no commit, a
 * page the arena chunk never faulted in, or a range some other owner
 * re-protected all pass the window test and then fault inside translated
 * guest code with no provenance.  When armed, every pointer written back to
 * the guest is looked up in the arena and its first and last byte are
 * read-modify-written, so a non-writable mapping faults here, named, instead
 * of thousands of instructions later inside the application. */
extern int d3d9_native_lockcheck_on;
extern void d3d9_native_lockcheck(const void *host);

/* Forward declaration: the window check below is what makes the write-back
 * safe, and it is defined a few lines down. */
static inline int d3d9_in_window(const void *p, size_t bytes);

/* The ONLY direction in which a host pointer ever becomes a guest one, and
 * only ever for mapped resource memory: D3DLOCKED_RECT::pBits,
 * D3DLOCKED_BOX::pBits and the two buffer Lock()s' ppbData (8.2(c)).
 *
 * Every one of those MUST already be inside [B, B+4G): dxmt::guest_alloc()
 * routes every app-visible allocation to the guest arena precisely so that it
 * is.  A host-heap pointer arriving here would mean an allocation site was
 * missed -- and the subtraction would then hand the application a plausible
 * 32-bit number pointing at unrelated guest memory, which it would write
 * through.  So this asserts the window and refuses with a loud, once-only
 * line and a NULL rather than producing that number.  The generated entries
 * already treat a 0 as "no mapping". */
static inline ULONG
d3d9_guest_ptr32(const void *host) {
  unsigned long base = ios_wow_base();
  if (!host)
    return 0;
  if (!base || !d3d9_in_window(host, 1)) {
    d3d9_guest_ptr32_refused(host);
    return 0;
  }
  if (d3d9_native_lockcheck_on)
    d3d9_native_lockcheck(host);
  return (ULONG)((uint64_t)(uintptr_t)host - (uint64_t)base);
}

/* Whole-range check, not just the base: a struct that starts one byte inside
 * the window and runs past its end is exactly the case a base-only test lets
 * through.  `bytes == 0` still validates the base address (the generated code
 * calls it that way for a size-inout argument whose count it has not read
 * yet). */
static inline int
d3d9_in_window(const void *p, size_t bytes) {
  unsigned long base = ios_wow_base();
  uint64_t addr = (uint64_t)(uintptr_t)p;
  uint64_t lo = (uint64_t)base;

  if (!base || !p)
    return 0;
  if (addr < lo || addr >= lo + D3D9_WINDOW_SIZE)
    return 0;
  if (bytes && (uint64_t)bytes > lo + D3D9_WINDOW_SIZE - addr)
    return 0;
  return 1;
}

/* Reads a count that the generated code needs BEFORE it has validated the
 * argument that carries it, so this one has to be safe on its own. */
static inline ULONG
d3d9_deref32(const void *p) {
  if (!d3d9_in_window(p, sizeof(ULONG)))
    return 0;
  return *(const ULONG *)p;
}

#define D3D9_HOST_PTR(u32) d3d9_host_ptr((uint32_t)(u32))
#define D3D9_GUEST_PTR32(p) d3d9_guest_ptr32((const void *)(p))
#define D3D9_IN_WINDOW(p, bytes) d3d9_in_window((const void *)(p), (size_t)(bytes))
#define D3D9_DEREF32(p) d3d9_deref32((const void *)(p))

/* ---- pSharedHandle ----------------------------------------------------- */

/* The one two-level pointer with a conditional rule (section 8.2(c)): for
 * D3DPOOL_SYSTEMMEM a non-NULL *pSharedHandle is the user-memory idiom -- the
 * application's own buffer, a GUEST address that must be converted.  For
 * every other pool it is an opaque sharing token and offsetting it would
 * corrupt it (invariant 4).  D3D9_NO_POOL is the four create paths that take
 * no pool argument at all; those are never the user-memory idiom. */
#define D3D9_NO_POOL ((D3DPOOL)0x7fffffff)

static inline HANDLE
d3d9_shared_in(const ULONG *slot, D3DPOOL pool) {
  ULONG value = slot ? *slot : 0;
  if (!value)
    return NULL;
  if (pool == D3DPOOL_SYSTEMMEM)
    return (HANDLE)d3d9_host_ptr((uint32_t)value);
  return (HANDLE)(ULONG_PTR)value;
}

static inline void
d3d9_shared_out(ULONG *slot, HANDLE handle, D3DPOOL pool) {
  if (!slot)
    return;
  if (!handle) {
    *slot = 0;
    return;
  }
  if (pool == D3DPOOL_SYSTEMMEM) {
    *slot = d3d9_guest_ptr32(handle);
    return;
  }
  *slot = (ULONG)(ULONG_PTR)handle;
}

#define D3D9_SHARED_IN(slot, pool) d3d9_shared_in((const ULONG *)(slot), (D3DPOOL)(pool))
#define D3D9_SHARED_OUT(slot, h, pool) d3d9_shared_out((ULONG *)(slot), (HANDLE)(h), (D3DPOOL)(pool))

/* ---- the arena's starvation mark ---------------------------------------- */

/* dxmt::guest_alloc() has no way to say "grow the arena": it is reached from
 * deep inside the frontend and its contract is a NULL return.  So a failure
 * leaves a per-thread mark, and the generated unix entry turns
 * "this call failed AND the arena starved on this thread" into
 * D3D9SHIM_STATUS_ARENA_EXHAUSTED.  The shim answers that by growing the
 * arena and retrying the same block once (8.2(c)).
 *
 * Per-thread on purpose: a DXMT worker pthread that starves must not make an
 * unrelated guest call on another thread look like an exhaustion.  Reading it
 * clears it, so a mark can never survive into the next call. */
extern int d3d9_native_arena_take_starved(void);

#define D3D9_ARENA_TAKE_STARVED() d3d9_native_arena_take_starved()

/* ---- diagnostics ------------------------------------------------------- */

#define D3D9_LOG(msg) fprintf(stderr, "[d3d9-native] %s\n", (msg))

#ifdef __cplusplus
}
#endif

#endif /* __MADEIRA_D3D9_UNIX_GLUE_H */
