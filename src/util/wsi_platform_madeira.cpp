/*
 * wsi_platform_madeira.cpp -- host-heap aligned allocation for the native
 * Madeira build.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(c)).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * This is the HOST allocator: everything DXMT allocates that the application
 * never dereferences (argument buffers, encoder scratch, occlusion-query
 * backings, staging rings).  Anything the application *can* dereference has
 * to come out of the guest arena instead -- dxmt::guest_alloc, see
 * src/d3d9/d3d9_guest_alloc.hpp.  The split is the whole point of section
 * 8.2(c): a host pointer is above 4 GB and a 32-bit guest cannot name it.
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

#include "wsi_platform.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt::wsi {

/* posix_memalign rather than aligned_alloc: C11 aligned_alloc requires the
 * size to be a multiple of the alignment, which DXMT's callers do not
 * guarantee (a 4 KB-aligned 100-byte constant block is normal).  Note that
 * wsi_platform_darwin.cpp passes aligned_alloc's two arguments the other way
 * round; not copied. */
void *
aligned_malloc(size_t size, size_t alignment) {
  if (alignment < sizeof(void *))
    alignment = sizeof(void *);
  /* posix_memalign requires a power-of-two multiple of sizeof(void *). */
  if (alignment & (alignment - 1))
    return std::malloc(size);
  if (size == 0)
    size = 1;

  void *ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size) != 0)
    return nullptr;
  return ptr;
}

void
aligned_free(void *ptr) {
  std::free(ptr);
}

} // namespace dxmt::wsi
