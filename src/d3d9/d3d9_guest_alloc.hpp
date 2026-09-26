/*
 * d3d9_guest_alloc.hpp -- the app-visible half of the allocator split.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(c)).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * Every pointer D3D9 hands back through Lock / LockRect / LockBox, and every
 * CPU mirror the application can reach, must live inside the guest window
 * [B, B+4G) -- a 32-bit application cannot name anything else, and
 * d3d9_buffer_map.hpp:13-54 states why the mirror can never be Metal memory.
 * Natively wsi::aligned_malloc returns host heap, which is above 4 GB, so the
 * app-visible sites allocate from the guest arena instead: the shim
 * VirtualAllocs 64 MB chunks (the window chokepoint puts them in range by
 * construction), registers them, and the unix glue sub-allocates.
 *
 * Off-Madeira this is exactly wsi::aligned_malloc / aligned_free, so every
 * converted call site keeps its current meaning on every other target.
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

#include <cstddef>

#include "wsi_platform.hpp"

namespace dxmt {

#ifdef DXMT_MADEIRA

/* Implemented by src/d3d9/unix/d3d9_native_glue.cpp against the registered
 * arena chunks.  Returns NULL on exhaustion -- the caller must treat that as
 * D3DERR_OUTOFVIDEOMEMORY / E_OUTOFMEMORY exactly as it treats an
 * aligned_malloc failure today; the shim grows the arena and the next
 * attempt succeeds (section 8.2(c): no upcall machinery). */
void *guest_alloc(size_t size, size_t alignment);
void guest_free(void *ptr);

/* Zeroing variant: the only current caller is d3d9_mem.cpp, whose mirrors
 * are std::calloc'd today and whose contents an app may read before writing. */
void *guest_calloc(size_t size, size_t alignment);

/* The arena's alignment granularity: the REAL host page (16 KB on iOS), not
 * DXMT_PAGE_SIZE, which meson defines as 4096 unconditionally
 * (meson.build:155).  Asserted against getpagesize() in the glue. */
size_t guest_page_size();

#else

inline void *
guest_alloc(size_t size, size_t alignment) {
  return wsi::aligned_malloc(size, alignment);
}

inline void
guest_free(void *ptr) {
  wsi::aligned_free(ptr);
}

inline void *
guest_calloc(size_t size, size_t alignment) {
  void *ptr = wsi::aligned_malloc(size, alignment);
  if (ptr)
    __builtin_memset(ptr, 0, size);
  return ptr;
}

inline size_t
guest_page_size() {
  return DXMT_PAGE_SIZE;
}

#endif

} // namespace dxmt
