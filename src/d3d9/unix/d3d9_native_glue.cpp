/*
 * d3d9_native_glue.cpp -- the native side of the D3D9 shim boundary.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.5).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * What lives here:
 *
 *   - the handle table (section 8.2(b)): index + generation, never a host
 *     pointer, so a stale or forged handle is a validated failure instead of
 *     a wild dereference.  It is also the IDENTITY map: one handle per
 *     native object, find-before-create, which is what makes
 *     GetSurfaceLevel(n) twice return the same guest pointer;
 *   - the per-guest-process root and d3d9_native_process_teardown(), which
 *     section 8.9-5 calls the MOST IMPORTANT risk in the whole plan -- native
 *     objects hold host pointers INTO the arena, i.e. into the 4 GB range
 *     ios_wow_reclaim_dead_windows() is about to replace with PROT_NONE;
 *   - the guest arena and its sub-allocator (section 8.2(c)), plus
 *     dxmt::guest_alloc / guest_free, which the app-visible d3d9 allocation
 *     sites already call.  The arena is pinned to a per-process ROOT rather
 *     than to ios_wow_base(), which is per-CALLING-THREAD and therefore 0 on
 *     DXMT's own pthreads;
 *   - the four variable-length input scanners d3d9_unix.c declares;
 *   - the three transport hooks (arena registration, the per-HWND window
 *     state, and the creation of the IDirect3D9(Ex) every other handle
 *     descends from);
 *   - the six per-method hooks whose shape is not mechanical, and
 *     d3d9_native_gen.inc -- the other 314, emitted from d3d9_api.py by
 *     gen_d3d9_thunks.py, because a forwarding body written 314 times by
 *     hand is exactly the drift the generator exists to prevent;
 *   - [d3d9-native-census]: how many CROSSINGS a frame costs, which is the
 *     number section 8.6 needs and which the [d3d9-census] counters (one per
 *     frontend method, on either side of the boundary) cannot answer.
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

#include "d3d9_unix_glue.h"
#include "d3d9_native_hooks.h"

#include "d3d9_guest_alloc.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_madeira_window.hpp"
#include "wsi_window_madeira.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

/* B for an explicit pseudo-process, 0 when that process has no window.
 * Resolved at app link time from build/ntdll-unix/virtual_ios.c, the same way
 * ios_wow_base() is (see d3d9_unix_glue.h). */
extern "C" unsigned long ios_wow_base_for_peb(void *peb_id);

/* Defined at the bottom of this file, beside the [d3d9-native-census]
 * summary; called from d3d9_native_init(), which is far above it. */
extern "C" void d3d9_native_census_configure(void);

namespace {

/* ======================================================================
 * Diagnostics
 * ====================================================================== */

std::mutex &
log_mutex() {
  static std::mutex m;
  return m;
}

/* One line per distinct message, so a per-draw stub cannot flood the log and
 * distort the very workload section 8.4 exists to measure. */
void
log_once(const char *message) {
  static std::unordered_set<const char *> seen;
  std::lock_guard<std::mutex> guard(log_mutex());
  if (!seen.insert(message).second)
    return;
  std::fprintf(stderr, "[d3d9-native] %s\n", message);
}

/* ======================================================================
 * The guest arena (WOW64_DESIGN.md section 8.2(c))
 *
 * Every pointer the application can dereference has to be inside [B, B+4G).
 * The shim VirtualAllocs chunks -- which the window chokepoint puts in range
 * by construction -- and registers each one here; this sub-allocator hands
 * out pieces of them.  Nothing here ever calls back into the guest:
 * exhaustion is a NULL return, the caller turns that into the out-of-memory
 * HRESULT it already returns for an aligned_malloc failure, and the shim
 * grows the arena and retries.
 *
 * A deliberately plain first-fit free-list allocator.  The traffic is
 * resource creation and destruction, not per-draw: the per-draw allocator is
 * the ring bump allocator, which is host memory and does not come through
 * here.  A better structure is an optimisation to make when a census says it
 * is one.
 * ====================================================================== */

constexpr size_t kArenaMinAlignment = 16;

struct ArenaBlock {
  uint64_t offset; /* from the chunk base */
  uint64_t size;
};

struct ArenaChunk {
  unsigned long window_base;  /* B of the owning pseudo-process */
  uint32_t guest_base;        /* guest address of the chunk */
  uint64_t size;
  std::vector<ArenaBlock> free_list; /* sorted by offset, coalesced */
};

struct ArenaAllocation {
  size_t chunk;
  uint64_t offset;
  uint64_t size;
};

struct Arena {
  std::mutex mutex;
  /* THE PINNED ROOT (the step-3 hand-off note).  ios_wow_base() answers for
   * the CALLING thread's pseudo-process and is 0 on a DXMT worker pthread --
   * and dxmt::guest_alloc() is reached from the encode thread, the texture
   * upload path and every other native thread, not only from a unix entry.
   * Keying the arena off ios_wow_base() therefore failed every allocation
   * that did not happen to be on the guest's own thread.  The root is
   * captured once, at _d3d9_init or at the first registration, and is what a
   * caller with no pseudo-process of its own is served from. */
  unsigned long root = 0;
  void *root_peb = nullptr;
  bool warned_foreign_thread = false;
  std::vector<ArenaChunk> chunks;
  /* host pointer -> where it came from.  Keyed by host pointer because that
   * is all guest_free() is given; the host pointer is unique because the
   * chunks are disjoint ranges of one process's window. */
  std::unordered_map<void *, ArenaAllocation> live;
  uint64_t in_use = 0;
  uint64_t high_water = 0;
  bool warned_empty = false;
};

Arena &
arena() {
  static Arena a;
  return a;
}

/* Set by every guest_alloc() that could not be served, read and cleared by
 * the generated unix entry's epilogue (D3D9_ARENA_TAKE_STARVED).  Per-thread
 * so a starving DXMT worker cannot make an unrelated guest call on another
 * thread look like an exhaustion. */
thread_local unsigned g_arena_starved;

size_t
host_page_size() {
  static size_t page = []() -> size_t {
    long value = ::getpagesize();
    return value > 0 ? (size_t)value : 16384;
  }();
  return page;
}

void *
chunk_host_ptr(const ArenaChunk &chunk, uint64_t offset) {
  return (void *)(uintptr_t)((uint64_t)chunk.window_base + (uint64_t)chunk.guest_base + offset);
}

/* Insert [offset, offset+size) into a chunk's free list, coalescing with the
 * neighbours.  The list stays sorted, which is what makes first fit cheap and
 * coalescing a two-neighbour check instead of a scan. */
void
free_list_insert(ArenaChunk &chunk, uint64_t offset, uint64_t size) {
  auto it = chunk.free_list.begin();
  while (it != chunk.free_list.end() && it->offset < offset)
    ++it;
  it = chunk.free_list.insert(it, ArenaBlock{offset, size});

  auto next = it + 1;
  if (next != chunk.free_list.end() && it->offset + it->size == next->offset) {
    it->size += next->size;
    chunk.free_list.erase(next);
  }
  if (it != chunk.free_list.begin()) {
    auto prev = it - 1;
    if (prev->offset + prev->size == it->offset) {
      prev->size += it->size;
      chunk.free_list.erase(it);
    }
  }
}

} // namespace

/* ======================================================================
 * The arena's public face
 * ====================================================================== */

extern "C" {

/* Register one guest chunk with the arena.  `guest_base` is a GUEST address
 * -- the shim VirtualAlloc'd it inside its own pseudo-process, so the window
 * chokepoint has already put it in [B, B+4G).  Returns 0 on success, which
 * is why the generated entry maps it to D3D_OK/E_FAIL.
 *
 * Reached on transport slot 321 (D3D9SHIM_OP_arena_register), from
 * d3d9shim_arena.c, on the guest's own thread -- so ios_wow_base() is the
 * registering process's B and is what the root is pinned to. */
int
d3d9_native_arena_register(uint32_t guest_base, uint64_t size) {
  if (!guest_base || !size)
    return -1;

  unsigned long base = ios_wow_base();
  if (!base) {
    log_once("arena: registration from a thread with no guest window; refused");
    return -1;
  }
  /* The chunk must lie wholly inside the window.  A chunk that wraps past
   * 4 GB would hand out pointers outside the caller's own address space. */
  if ((uint64_t)guest_base + size > D3D9_WINDOW_SIZE) {
    log_once("arena: chunk runs past the end of the guest window; refused");
    return -1;
  }
  /* Alignment 16384, the REAL iOS page: DXMT_PAGE_SIZE is 4096
   * unconditionally (meson.build:155) and is the wrong number here. */
  size_t page = host_page_size();
  if ((uint64_t)guest_base % page) {
    log_once("arena: chunk base is not host-page aligned; refused");
    return -1;
  }

  Arena &a = arena();
  std::lock_guard<std::mutex> guard(a.mutex);
  for (const auto &chunk : a.chunks) {
    if (chunk.window_base != base)
      continue;
    uint64_t lo = chunk.guest_base, hi = lo + chunk.size;
    if ((uint64_t)guest_base < hi && lo < (uint64_t)guest_base + size) {
      log_once("arena: chunk overlaps one already registered; refused");
      return -1;
    }
  }

  if (!a.root) {
    a.root = base;
  } else if (a.root != base) {
    /* Two 32-bit pseudo-processes with a live D3D9 at once.  The chunks stay
     * keyed by window base so neither can be served the other's addresses;
     * what cannot be right for both is the root a thread with no
     * pseudo-process falls back to, so say so rather than pick silently. */
    std::fprintf(stderr,
                 "[d3d9-native] arena: a second guest process registered a "
                 "chunk (root B=%p, new B=%p) -- native threads are served "
                 "from the first\n",
                 (void *)a.root, (void *)base);
  }

  ArenaChunk chunk;
  chunk.window_base = base;
  chunk.guest_base = guest_base;
  chunk.size = size;
  chunk.free_list.push_back(ArenaBlock{0, size});
  a.chunks.push_back(std::move(chunk));
  return 0;
}

/* d3d9_unix_glue.h's D3D9_GUEST_PTR32 calls this when the host pointer it was
 * asked to write back is NOT inside the guest window.  That can only mean an
 * app-visible allocation site still goes to the host heap instead of
 * dxmt::guest_alloc(), i.e. a resource the application is about to write
 * through has no address it can express -- so it is worth a line naming the
 * pointer, not a silently truncated one (8.2(c) / 7.5). */
void
d3d9_guest_ptr32_refused(const void *host) {
  static std::atomic<uint32_t> reported;

  if (reported.fetch_add(1, std::memory_order_relaxed) >= 8)
    return;
  std::fprintf(stderr,
               "[d3d9-native] REFUSING to hand the guest %p as a 32-bit "
               "address: it is outside [B, B+4G), so it is host heap and not "
               "arena memory -- an app-visible allocation site is still "
               "calling wsi::aligned_malloc (WOW64_DESIGN.md 8.2(c))\n",
               host);
}

/* High-water mark, for the [d3d9-arena] line section 8.9-6 asks for. */
uint64_t
d3d9_native_arena_high_water(void) {
  Arena &a = arena();
  std::lock_guard<std::mutex> guard(a.mutex);
  return a.high_water;
}

/* Read-and-clear the per-thread starvation mark; see d3d9_unix_glue.h. */
int
d3d9_native_arena_take_starved(void) {
  unsigned n = g_arena_starved;
  g_arena_starved = 0;
  return n != 0;
}

/* ---- MADEIRA_D3D9_LOCKCHECK=1 -------------------------------------------
 *
 * Every pointer this boundary hands back to the application -- the two
 * pBits and the two buffer Lock()s' ppbData -- must be memory the guest can
 * WRITE.  In-window is not enough: a reservation with no commit, a page the
 * arena chunk never faulted in, or a range some other owner re-protected all
 * pass the window test and then fault inside translated guest code, where
 * the log says only "a store faulted at some guest address".
 *
 * So under the knob the pointer is (a) looked up in the arena, which is the
 * only place an app-visible allocation may come from, and (b) touched: the
 * first and last byte of the allocation are read and written back unchanged.
 * A non-writable page then faults HERE, on the guest's own thread, inside a
 * named function, with the allocation printed -- instead of several thousand
 * instructions later with no provenance at all. */
int d3d9_native_lockcheck_on;

void
d3d9_native_lockcheck(const void *host) {
  static std::atomic<uint32_t> reported;
  uint64_t p = (uint64_t)(uintptr_t)host;
  uint64_t base = 0, size = 0;

  if (!host)
    return;

  {
    Arena &a = arena();
    std::lock_guard<std::mutex> guard(a.mutex);
    auto exact = a.live.find(const_cast<void *>(host));
    if (exact != a.live.end()) {
      base = p;
      size = exact->second.size;
    } else {
      /* An interior pointer: a Lock() of a sub-rectangle answers base+offset,
       * so the allocation is the one that CONTAINS it. */
      for (const auto &kv : a.live) {
        uint64_t lo = (uint64_t)(uintptr_t)kv.first;
        if (p >= lo && p < lo + kv.second.size) {
          base = lo;
          size = kv.second.size;
          break;
        }
      }
    }
  }

  if (!size) {
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
      std::fprintf(stderr,
                   "[d3d9-lockcheck] %p is about to be handed to the guest "
                   "but the arena never allocated it -- an app-visible "
                   "allocation site is not going through dxmt::guest_alloc "
                   "(WOW64_DESIGN.md 8.2(c))\n",
                   host);
    return;
  }

  /* Read-modify-write with the same value: it proves the page is writable
   * without changing a byte of the application's data. */
  volatile unsigned char *first = (volatile unsigned char *)(uintptr_t)base;
  volatile unsigned char *last =
      (volatile unsigned char *)(uintptr_t)(base + size - 1);
  unsigned char a0 = *first;
  *first = a0;
  unsigned char a1 = *last;
  *last = a1;

  if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
    std::fprintf(stderr,
                 "[d3d9-lockcheck] ok %p (allocation %p + %llu, offset %llu) "
                 "writable\n",
                 host, (void *)(uintptr_t)base, (unsigned long long)size,
                 (unsigned long long)(p - base));
}

} // extern "C"

namespace dxmt {

void *
guest_alloc(size_t size, size_t alignment) {
  if (!size)
    size = 1;
  if (alignment < kArenaMinAlignment)
    alignment = kArenaMinAlignment;
  if (alignment & (alignment - 1))
    return nullptr; /* not a power of two: a caller bug, not an exhaustion */

  Arena &a = arena();
  std::lock_guard<std::mutex> guard(a.mutex);

  /* The caller is usually a DXMT worker pthread -- the encode thread, a
   * texture upload, the shader compiler -- which has no pseudo-process and
   * whose ios_wow_base() is 0.  Serve it from the pinned root instead of
   * failing, which is what a per-thread base did. */
  unsigned long base = ios_wow_base();
  if (!base) {
    base = a.root;
    if (!base && !a.warned_foreign_thread) {
      a.warned_foreign_thread = true;
      std::fprintf(stderr,
                   "[d3d9-native] arena: an allocation arrived on a thread "
                   "with no guest window and no root has been pinned yet "
                   "(WOW64_DESIGN.md 8.2(c))\n");
    }
  }

  if (a.chunks.empty()) {
    if (!a.warned_empty) {
      a.warned_empty = true;
      std::fprintf(stderr,
                   "[d3d9-native] arena: no chunk registered -- the shim has "
                   "not called d3d9_native_arena_register yet, so every "
                   "app-visible allocation fails (WOW64_DESIGN.md 8.2(c))\n");
    }
    g_arena_starved++;
    return nullptr;
  }

  for (size_t i = 0; i < a.chunks.size(); i++) {
    ArenaChunk &chunk = a.chunks[i];
    /* Only this pseudo-process's chunks: another process's guest addresses
     * mean nothing here, and handing one out would alias its window.  A zero
     * base (a DXMT worker thread, which has no pseudo-process) matches
     * nothing and falls through to the exhaustion path rather than picking an
     * arbitrary window. */
    if (chunk.window_base != base)
      continue;

    for (size_t b = 0; b < chunk.free_list.size(); b++) {
      ArenaBlock block = chunk.free_list[b];
      uint64_t aligned = (block.offset + (alignment - 1)) & ~(uint64_t)(alignment - 1);
      uint64_t pad = aligned - block.offset;
      if (pad + (uint64_t)size > block.size)
        continue;

      chunk.free_list.erase(chunk.free_list.begin() + (long)b);
      if (pad)
        free_list_insert(chunk, block.offset, pad);
      uint64_t tail_offset = aligned + (uint64_t)size;
      uint64_t tail_size = block.offset + block.size - tail_offset;
      if (tail_size)
        free_list_insert(chunk, tail_offset, tail_size);

      void *host = chunk_host_ptr(chunk, aligned);
      a.live[host] = ArenaAllocation{i, aligned, (uint64_t)size};
      a.in_use += (uint64_t)size;
      if (a.in_use > a.high_water)
        a.high_water = a.in_use;
      return host;
    }
  }

  std::fprintf(stderr,
               "[d3d9-native] arena: exhausted at %llu bytes in use -- grow it "
               "(WOW64_DESIGN.md 8.9-6)\n",
               (unsigned long long)a.in_use);
  g_arena_starved++;
  return nullptr;
}

void
guest_free(void *ptr) {
  if (!ptr)
    return;

  Arena &a = arena();
  std::lock_guard<std::mutex> guard(a.mutex);
  auto it = a.live.find(ptr);
  if (it == a.live.end()) {
    log_once("arena: guest_free of a pointer this arena never handed out");
    return;
  }
  ArenaAllocation allocation = it->second;
  a.live.erase(it);
  a.in_use -= allocation.size;
  if (allocation.chunk < a.chunks.size())
    free_list_insert(a.chunks[allocation.chunk], allocation.offset, allocation.size);
}

void *
guest_calloc(size_t size, size_t alignment) {
  void *ptr = guest_alloc(size, alignment);
  if (ptr)
    std::memset(ptr, 0, size);
  return ptr;
}

size_t
guest_page_size() {
  return host_page_size();
}

} // namespace dxmt

/* ======================================================================
 * The handle table (WOW64_DESIGN.md section 8.2(b))
 *
 * A d3d9_native_handle is `(generation << 32) | (index + 1)`, so 0 is "none"
 * and a reused slot does not answer to the handle of the object that used to
 * live in it.  Never a host pointer: invariant 4, and the reason a guest that
 * corrupts one of its own vtables gets D3DERR_INVALIDCALL instead of a host
 * fault its SEH can never see (section 8.9-4).
 *
 * Every entry records the window base of the pseudo-process that created it,
 * which is what makes the per-process sweep in d3d9_native_process_teardown
 * possible at all.
 * ====================================================================== */

namespace {

struct HandleEntry {
  void *object = nullptr;
  void (*destroy)(void *) = nullptr;
  uint32_t generation = 1;
  uint32_t kind = 0;
  unsigned long window_base = 0;
  bool live = false;
};

struct HandleTable {
  std::mutex mutex;
  std::vector<HandleEntry> entries;
  std::vector<uint32_t> free_indices;
  /* IDENTITY.  D3D9 promises that asking twice for the same child hands back
   * the same pointer (GetSurfaceLevel(2) twice, GetBackBuffer(0) twice,
   * GetSwapChain(0) twice), and the shim builds its guest wrapper from the
   * native handle -- so identity has to hold HERE or the shim wraps one
   * native surface in two guest objects with two refcounts.  Keyed by the
   * canonical interface pointer the kind stores, which is unique because the
   * native object is alive for as long as the handle holds its reference. */
  std::unordered_map<void *, d3d9_native_handle> by_object;
};

HandleTable &
handles() {
  static HandleTable table;
  return table;
}

constexpr uint32_t kMaxHandles = 1u << 22; /* 4M objects; a runaway guest fails loudly */

/* Vertices a primitive count implies.  Shared by both UP scanners. */
size_t
up_vertex_count(D3DPRIMITIVETYPE type, UINT primitive_count) {
  switch (type) {
  case D3DPT_POINTLIST:
    return primitive_count;
  case D3DPT_LINELIST:
    return (size_t)primitive_count * 2;
  case D3DPT_LINESTRIP:
    return primitive_count ? (size_t)primitive_count + 1 : 0;
  case D3DPT_TRIANGLELIST:
    return (size_t)primitive_count * 3;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN:
    return primitive_count ? (size_t)primitive_count + 2 : 0;
  default:
    return 0;
  }
}

} // namespace

extern "C" {

/* Find-before-create.  `*out_created` is 0 when the object already had a
 * handle, which is the caller's signal to drop the reference the D3D9 method
 * just handed it: the table holds exactly one reference per object, for the
 * life of the handle. */
d3d9_native_handle
d3d9_native_handle_intern(void *object, uint32_t kind, void (*destroy)(void *),
                          int *out_created) {
  if (out_created)
    *out_created = 0;
  if (!object)
    return 0;

  HandleTable &table = handles();
  std::lock_guard<std::mutex> guard(table.mutex);

  auto found = table.by_object.find(object);
  if (found != table.by_object.end())
    return found->second;

  uint32_t index;
  if (!table.free_indices.empty()) {
    index = table.free_indices.back();
    table.free_indices.pop_back();
  } else {
    if (table.entries.size() >= kMaxHandles) {
      log_once("handle table exhausted");
      return 0;
    }
    index = (uint32_t)table.entries.size();
    table.entries.push_back(HandleEntry{});
  }

  HandleEntry &entry = table.entries[index];
  entry.object = object;
  entry.destroy = destroy;
  entry.kind = kind;
  /* Which pseudo-process owns it.  0 on a DXMT worker pthread, which is why
   * the arena root exists; an object created there is swept by no teardown,
   * so pin it to the arena root for the same reason. */
  entry.window_base = ios_wow_base();
  if (!entry.window_base) {
    Arena &a = arena();
    std::lock_guard<std::mutex> arena_guard(a.mutex);
    entry.window_base = a.root;
  }
  entry.live = true;

  d3d9_native_handle handle =
      ((uint64_t)entry.generation << 32) | (uint64_t)(index + 1);
  table.by_object[object] = handle;
  if (out_created)
    *out_created = 1;
  return handle;
}

void *
d3d9_native_handle_lookup(d3d9_native_handle handle, uint32_t kind) {
  if (!handle)
    return nullptr;

  uint32_t index = (uint32_t)(handle & 0xffffffffu);
  uint32_t generation = (uint32_t)(handle >> 32);
  if (!index)
    return nullptr;
  index -= 1;

  HandleTable &table = handles();
  std::lock_guard<std::mutex> guard(table.mutex);
  if (index >= table.entries.size())
    return nullptr;
  HandleEntry &entry = table.entries[index];
  if (!entry.live || entry.generation != generation)
    return nullptr;
  /* kind 0 means "any": the caller already knows what it is holding. */
  if (kind && entry.kind != kind)
    return nullptr;
  return entry.object;
}

/* The same lookup, reporting what kind came back.  The generated bodies that
 * take an IDirect3DBaseTexture9 (SetTexture is per-draw) would otherwise have
 * to probe three kinds and take this lock three times. */
void *
d3d9_native_handle_lookup_any(d3d9_native_handle handle, uint32_t *kind_out) {
  if (kind_out)
    *kind_out = 0;
  if (!handle)
    return nullptr;

  uint32_t index = (uint32_t)(handle & 0xffffffffu);
  uint32_t generation = (uint32_t)(handle >> 32);
  if (!index)
    return nullptr;
  index -= 1;

  HandleTable &table = handles();
  std::lock_guard<std::mutex> guard(table.mutex);
  if (index >= table.entries.size())
    return nullptr;
  HandleEntry &entry = table.entries[index];
  if (!entry.live || entry.generation != generation)
    return nullptr;
  if (kind_out)
    *kind_out = entry.kind;
  return entry.object;
}

/* Retire a handle.  The object's own destructor runs through `destroy`, which
 * the creator supplied -- the table never assumes a type. */
void
d3d9_native_handle_destroy(d3d9_native_handle handle) {
  if (!handle)
    return;

  uint32_t index = (uint32_t)(handle & 0xffffffffu);
  uint32_t generation = (uint32_t)(handle >> 32);
  if (!index)
    return;
  index -= 1;

  void *object = nullptr;
  void (*destroy)(void *) = nullptr;

  {
    HandleTable &table = handles();
    std::lock_guard<std::mutex> guard(table.mutex);
    if (index >= table.entries.size())
      return;
    HandleEntry &entry = table.entries[index];
    if (!entry.live || entry.generation != generation)
      return;
    object = entry.object;
    destroy = entry.destroy;
    table.by_object.erase(object);
    entry.object = nullptr;
    entry.destroy = nullptr;
    entry.live = false;
    entry.kind = 0;
    entry.window_base = 0;
    /* Wrapping the generation would make an ancient handle valid again, so
     * skip 0 and keep going rather than reusing the slot. */
    entry.generation = entry.generation + 1 ? entry.generation + 1 : 1;
    table.free_indices.push_back(index);
  }

  if (destroy)
    destroy(object);
}

/* ======================================================================
 * Lifecycle
 * ====================================================================== */

int
d3d9_native_init(void) {
  /* The arena hands out 16384-aligned pieces; assert that against the real
   * host page rather than DXMT_PAGE_SIZE, which meson defines as 4096
   * unconditionally (section 8.2(c)). */
  size_t page = host_page_size();
  if (page < 4096 || (page & (page - 1))) {
    std::fprintf(stderr, "[d3d9-native] refusing to initialise: getpagesize() is %zu\n", page);
    return 0;
  }

  /* _d3d9_init runs on the guest's own thread, which is the one moment the
   * arena root can be learned before any allocation happens.  The first
   * arena registration pins it too; whichever comes first wins. */
  {
    Arena &a = arena();
    std::lock_guard<std::mutex> guard(a.mutex);
    if (!a.root)
      a.root = ios_wow_base();
  }

  d3d9_native_census_configure();

  static bool announced = false;
  if (!announced) {
    announced = true;
    std::fprintf(stderr,
                 "[d3d9-native] ready: %u slots, host page %zu, arena root "
                 "B=%p\n",
                 (unsigned)D3D9SHIM_OP_COUNT, page,
                 (void *)ios_wow_base());
  }
  return 1;
}

/* Called from ios_wow_reclaim_dead_windows() BEFORE the PROT_NONE replace and
 * before ios_jit_purge_window().  The order below IS the contract of section
 * 8.9-5, and it is asserted by construction rather than by comment:
 *
 *   1. drop every native object this pseudo-process created -- that is what
 *      releases the Metal objects, and their destructors are the last thing
 *      that may touch arena memory;
 *   2. drop the arena chunks, so no host pointer into the window survives;
 *   3. drop the window's entry in the client-size cache;
 *
 * and only then does the caller remap the range. */
void
d3d9_native_process_teardown(void *peb) {
  unsigned long base = ios_wow_base_for_peb(peb);
  if (!base)
    return;

  /* 1. objects */
  for (;;) {
    void *object = nullptr;
    void (*destroy)(void *) = nullptr;
    {
      HandleTable &table = handles();
      std::lock_guard<std::mutex> guard(table.mutex);
      bool found = false;
      for (uint32_t i = 0; i < (uint32_t)table.entries.size(); i++) {
        HandleEntry &entry = table.entries[i];
        if (!entry.live || entry.window_base != base)
          continue;
        object = entry.object;
        destroy = entry.destroy;
        table.by_object.erase(object);
        entry.object = nullptr;
        entry.destroy = nullptr;
        entry.live = false;
        entry.kind = 0;
        entry.window_base = 0;
        entry.generation = entry.generation + 1 ? entry.generation + 1 : 1;
        table.free_indices.push_back(i);
        found = true;
        break;
      }
      if (!found)
        break;
    }
    /* Outside the lock: a destructor may retire child handles of its own. */
    if (destroy)
      destroy(object);
  }

  /* 2. arena */
  {
    Arena &a = arena();
    std::lock_guard<std::mutex> guard(a.mutex);
    std::vector<size_t> dropped;
    for (size_t i = 0; i < a.chunks.size(); i++)
      if (a.chunks[i].window_base == base)
        dropped.push_back(i);

    for (auto it = a.live.begin(); it != a.live.end();) {
      if (it->second.chunk < a.chunks.size() && a.chunks[it->second.chunk].window_base == base) {
        a.in_use -= it->second.size;
        it = a.live.erase(it);
      } else {
        ++it;
      }
    }
    /* Erasing chunks would renumber the indices every surviving live[] entry
     * holds, so retire them in place instead: an empty free list matches no
     * allocation and a zero window base matches no caller. */
    for (size_t i : dropped) {
      a.chunks[i].free_list.clear();
      a.chunks[i].window_base = 0;
      a.chunks[i].size = 0;
    }
    /* The pinned root is this window; unpin it, and re-pin to whatever
     * chunk of another live process survives, so a native thread of a
     * SECOND guest process is not served addresses from a range that is
     * about to become PROT_NONE. */
    if (a.root == base) {
      a.root = 0;
      a.root_peb = nullptr;
      for (const auto &chunk : a.chunks) {
        if (chunk.window_base) {
          a.root = chunk.window_base;
          break;
        }
      }
    }
  }

  /* 3. window sizes */
  dxmt::wsi::madeira_forget_all_windows();
}

/* ======================================================================
 * Variable-length input scanners
 *
 * Each of these walks GUEST memory that has not been validated yet -- that is
 * the point: d3d9_unix.c calls them to work out how much to validate.  So
 * each window-checks every step it takes and returns 0 the moment it cannot
 * follow, and D3D9_IN_WINDOW(p, 0) still checks the base address, so a 0 does
 * not weaken the check that follows it.
 * ====================================================================== */

size_t
d3d9_decl_element_count(const D3DVERTEXELEMENT9 *elements) {
  /* D3DMAXDECLLENGTH is 64 elements plus the D3DDECL_END() marker; refuse to
   * walk further than the API can express. */
  constexpr size_t kMaxElements = 65;

  if (!elements)
    return 0;
  for (size_t i = 0; i < kMaxElements; i++) {
    if (!d3d9_in_window(&elements[i], sizeof(D3DVERTEXELEMENT9)))
      return 0;
    if (elements[i].Stream == 0xff) /* D3DDECL_END() */
      return i + 1;                 /* the marker is part of the array */
  }
  return 0;
}

size_t
d3d9_shader_token_count(const DWORD *byte_code) {
  /* No token stream this runtime accepts is anywhere near this long; the cap
   * exists so a corrupt stream terminates the walk instead of the window. */
  constexpr size_t kMaxTokens = 1u << 20;

  if (!byte_code)
    return 0;
  for (size_t i = 0; i < kMaxTokens; i++) {
    if (!d3d9_in_window(&byte_code[i], sizeof(DWORD)))
      return 0;
    if (byte_code[i] == 0x0000ffffu) /* D3DSIO_END */
      return i + 1;
  }
  return 0;
}

size_t
d3d9_up_vertex_bytes(D3DPRIMITIVETYPE type, UINT primitive_count, UINT stride) {
  size_t vertices = up_vertex_count(type, primitive_count);
  if (!vertices || !stride)
    return 0;
  if (vertices > (size_t)~(size_t)0 / stride)
    return 0;
  return vertices * stride;
}

size_t
d3d9_up_index_bytes(D3DPRIMITIVETYPE type, UINT primitive_count, D3DFORMAT index_format) {
  size_t indices = up_vertex_count(type, primitive_count);
  size_t width;

  switch (index_format) {
  case D3DFMT_INDEX16:
    width = 2;
    break;
  case D3DFMT_INDEX32:
    width = 4;
    break;
  default:
    return 0;
  }
  if (!indices)
    return 0;
  return indices * width;
}

} // extern "C"

/* ======================================================================
 * The window seam (d3d9_madeira_window.hpp)
 *
 * No-ops for step 1, and correct ones rather than placeholders: with one
 * Swift-owned CAMetalLayer behind every HWND (section 7.1) there is no
 * restyle to perform, no Z-order to change and no monitor to move between --
 * the layer is the whole screen either way. What these WILL carry in step 3
 * is the message traffic: some titles resume drawing only once they see a
 * WM_WINDOWPOSCHANGED on the device window, and only the shim, running on the
 * guest's own thread, can deliver one.
 * ====================================================================== */

namespace dxmt {

void
madeira_window_enter_fullscreen(HWND window, uint32_t width, uint32_t height) {
  /* Record the size the frontend believes the window has, so a later
   * getWindowSize() agrees with the swapchain it just built. */
  wsi::madeira_set_client_size(window, width, height);
  log_once("window seam: enter_fullscreen is a no-op until the shim lands (8.2(d))");
}

void
madeira_window_leave_fullscreen(HWND window, bool restore_rect) {
  (void)window;
  (void)restore_rect;
  log_once("window seam: leave_fullscreen is a no-op until the shim lands (8.2(d))");
}

void
madeira_window_hook_focus(HWND window, void *device) {
  (void)window;
  (void)device;
  log_once("window seam: the focus-window subclass belongs to the shim (8.2(d))");
}

void
madeira_window_unhook_focus(HWND window, void *device) {
  (void)window;
  (void)device;
}

void
madeira_window_minimize(HWND window) {
  (void)window;
  log_once("window seam: minimize is a no-op until the shim lands (8.2(d))");
}

void
madeira_window_reposition(HWND window, uint32_t width, uint32_t height) {
  wsi::madeira_set_client_size(window, width, height);
  log_once("window seam: reposition is a no-op until the shim lands (8.2(d))");
}

/* The app is the only thing on screen while it runs; wsi_window_madeira.cpp
 * answers isForeground()/isMinimized() the same way and for the same reason. */
bool
madeira_window_is_visible(HWND window) {
  (void)window;
  return true;
}

} // namespace dxmt

/* ======================================================================
 * [d3d9-native-census] -- how many CROSSINGS a frame costs
 *
 * [d3d9-census] (d3d9_census.cpp) counts frontend METHODS, and it is compiled
 * into this archive too, so with the native path on it counts the same calls
 * again on this side of the boundary.  What it cannot answer is the number
 * section 8.6 is actually about: how many of those became a unix call.  That
 * is what this counts, at the one place every crossing passes through -- the
 * top of each native hook -- with the same windowed summary cadence and the
 * same relaxed 32-bit add.
 * ====================================================================== */

namespace {

bool g_census_on = true;
unsigned g_census_interval = 5000;

std::atomic<uint32_t> g_census_calls[D3D9SHIM_OP_COUNT];
std::atomic<uint32_t> g_census_frames;
std::atomic<bool> g_census_reporting;

/* Touched only under g_census_reporting, so plain types are enough. */
uint32_t g_census_prev[D3D9SHIM_OP_COUNT];
uint32_t g_census_prev_frames;
unsigned g_census_seq;

void census_report(); /* below d3d9_native_gen.inc: it names the op table */

/* ---- [d3d9-last]: what was the last crossing before the guest died ------
 *
 * A fault in the APPLICATION's own code -- a /GS fast-fail, a bad pointer --
 * carries no D3D9 frame, so the log has to name the last crossing on that
 * thread separately.  The census summary cannot: it only prints on a Present
 * cadence, and a title that dies during device init never presents at all.
 *
 * Always-on: a per-thread last opcode, printed by the teardown line and by
 * every census summary.  MADEIRA_D3D9_TRACE=1 additionally prints one
 * [d3d9-last] line per crossing, which is what bisects a guest-side fault to
 * the call that preceded it. */
bool g_trace_on = false;
thread_local unsigned g_last_op = ~0u;
thread_local unsigned long long g_last_seq;
std::atomic<unsigned> g_last_op_any{~0u};

void trace_line(unsigned op); /* below: it names the op table */

inline void
census_tick(unsigned op) {
  g_last_op = op;
  g_last_seq++;
  g_last_op_any.store(op, std::memory_order_relaxed);
  if (g_trace_on)
    trace_line(op);
  if (!g_census_on || op >= D3D9SHIM_OP_COUNT)
    return;
  g_census_calls[op].fetch_add(1, std::memory_order_relaxed);
}

/* The frame clock.  Any of the three Present slots ticks it: an application
 * presents through exactly one of them, and which one is not knowable here.
 * (MTLD3D9Device::Present forwards to the swapchain NATIVELY, below this
 * boundary, so it is one crossing and not two.) */
void
census_frame() {
  if (!g_census_on)
    return;
  uint32_t frames = g_census_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  bool due = frames == 1 || frames == 100 || frames == 1000
             || (g_census_interval && frames % g_census_interval == 0);
  if (!due)
    return;
  bool expected = false;
  if (!g_census_reporting.compare_exchange_strong(expected, true))
    return;
  census_report();
  g_census_reporting.store(false);
}

/* ======================================================================
 * Per-device and per-window records the native objects do not keep
 * ====================================================================== */

struct WindowState {
  uint32_t width;
  uint32_t height;
  uint32_t flags;
};

std::mutex &
window_mutex() {
  static std::mutex m;
  return m;
}

std::unordered_map<HWND, WindowState> &
window_states() {
  static std::unordered_map<HWND, WindowState> map;
  return map;
}

/* The BehaviorFlags the application passed, as opposed to the ones the native
 * device was built with.  CreateDevice strips D3DCREATE_MULTITHREADED because
 * the shim owns that lock (8.2(d): the native device is constructed
 * `is_protected=false`), and MTLD3D9Device derives m_creationParams from what
 * it is handed -- so GetCreationParameters would otherwise report a flag set
 * the application never chose.  Keyed by the device pointer, dropped when the
 * device's handle is retired. */
std::mutex &
device_flags_mutex() {
  static std::mutex m;
  return m;
}

std::unordered_map<void *, DWORD> &
device_flags() {
  static std::unordered_map<void *, DWORD> map;
  return map;
}

} // namespace

/* Every hook starts with this.  `op` is a compile-time constant at each call
 * site, so the Present comparison folds away in every other body. */
#define D3D9_NATIVE_ENTER(op)                                                  \
  do {                                                                         \
    census_tick((unsigned)(op));                                               \
    if ((op) == D3D9OP_Device9Ex_Present || (op) == D3D9OP_Device9Ex_PresentEx \
        || (op) == D3D9OP_SwapChain9Ex_Present)                                \
      census_frame();                                                          \
  } while (0)

/* A slot d3d9_api.py classifies `local` -- answered inside the shim, with no
 * crossing at all -- was nevertheless crossed.  That is a shim bug or a
 * half-finished reclassification, never something to absorb silently. */
#define D3D9_NATIVE_LOCAL(name)                                                \
  log_once(name " crossed, but the shim answers it locally (d3d9_api.py "      \
                "classifies it `local`)")

/* No C++ exception may escape into 32-bit code: the guest's SEH cannot see an
 * Itanium unwind and FEX's JIT frames are not unwindable (8.9-4). */
#define D3D9_NATIVE_CAUGHT(name)                                               \
  log_once("a C++ exception escaped " name "; reported as a failure HRESULT "  \
           "(WOW64_DESIGN.md 8.9-4)")

/* ======================================================================
 * Handles <-> native objects
 *
 * These live outside the extern "C" block below because a template may not
 * be given C language linkage.  Calling one from inside that block is fine;
 * declaring one inside it is ill-formed.
 * ====================================================================== */

/* The handle table stores the CANONICAL interface pointer for each kind (the
 * one enum d3d9_native_kind names), and the kind is checked on the way out,
 * so this cast is the one the creator made.  A handle of the wrong kind comes
 * back NULL and the caller answers D3DERR_INVALIDCALL -- a validated failure,
 * which is the whole reason handles and not host pointers are on the wire
 * (invariant 4 / 8.9-4). */
template <typename T>
static inline T *
d3d9_native_lookup(d3d9_native_handle handle, uint32_t kind) {
  return static_cast<T *>(d3d9_native_handle_lookup(handle, kind));
}

/* The one reference the table owns, given back. */
template <typename Canon>
static void
d3d9_native_destroy_object(void *object) {
  static_cast<Canon *>(object)->Release();
}

/* Take the reference a D3D9 method just returned and turn it into a handle.
 *
 * FIND BEFORE CREATE.  Asking twice for the same child (GetSurfaceLevel(2),
 * GetBackBuffer(0), GetSwapChain(0)) must yield the same handle, because the
 * shim builds its guest wrapper -- and that wrapper's refcount -- from it;
 * two handles for one native object would be two guest objects for one
 * surface, which is exactly the identity D3D9 applications compare pointers
 * for.  When the object already has a handle the reference this call was
 * given is dropped: the table holds exactly one, for the life of the handle.
 *
 * `Canon` is the interface the kind stores.  A method may hand out a BASE of
 * it -- GetSwapChain is declared to return IDirect3DSwapChain9 while every
 * DXMT swapchain is an IDirect3DSwapChain9Ex -- so the cast down is what
 * keeps one object to one pointer value, and therefore to one handle. */
template <typename Canon, typename T>
static inline void
d3d9_native_adopt(T *object, uint32_t kind, d3d9_native_handle *out, bool ok) {
  if (!object)
    return;
  Canon *canon = static_cast<Canon *>(object);
  if (!ok || !out) {
    canon->Release();
    return;
  }
  int created = 0;
  d3d9_native_handle handle =
      d3d9_native_handle_intern(static_cast<void *>(canon), kind,
                                &d3d9_native_destroy_object<Canon>, &created);
  if (!handle || !created)
    canon->Release();
  *out = handle;
}

/* The guest's last Release.  Retiring the handle runs the destroy hook above,
 * which is what drops the native reference; the guest-side refcount is the
 * shim's business and only the FINAL release ever crosses (8.6). */
static ULONG
d3d9_native_retire(d3d9_native_handle handle, uint32_t kind) {
  void *object;

  if (!handle)
    return 0;
  object = d3d9_native_handle_lookup(handle, kind);
  if (!object) {
    log_once("Release on a handle that is already retired or of another kind");
    return 0;
  }
  if (kind == D3D9_NATIVE_KIND_DEVICE) {
    std::lock_guard<std::mutex> guard(device_flags_mutex());
    device_flags().erase(object);
  }
  d3d9_native_handle_destroy(handle);
  return 0;
}

/* IDirect3DBaseTexture9 is not a kind: three concrete kinds answer to it and
 * the handle says which.  One lookup rather than three, because SetTexture is
 * a per-draw call. */
static IDirect3DBaseTexture9 *
d3d9_native_as_base_texture(d3d9_native_handle handle) {
  uint32_t kind = 0;
  void *object = d3d9_native_handle_lookup_any(handle, &kind);

  if (!object)
    return nullptr;
  switch (kind) {
  case D3D9_NATIVE_KIND_TEXTURE:
    return static_cast<IDirect3DTexture9 *>(object);
  case D3D9_NATIVE_KIND_CUBETEXTURE:
    return static_cast<IDirect3DCubeTexture9 *>(object);
  case D3D9_NATIVE_KIND_VOLUMETEXTURE:
    return static_cast<IDirect3DVolumeTexture9 *>(object);
  default:
    log_once("a base-texture argument named a handle that is not a texture");
    return nullptr;
  }
}

/* ======================================================================
 * The hooks
 * ====================================================================== */

extern "C" {

/* ---- transport: the per-HWND state the shim pushes (slot 322) ---------- */

/* The native frontend has no user32: an HWND is an opaque guest token here
 * and ::GetClientRect does not exist.  The shim runs on the guest's own
 * thread, where the window really lives, and reports the client size at
 * CreateDevice / Reset / Present; wsi_window_madeira.cpp answers
 * getWindowSize() from what lands here (8.2(d)). */
HRESULT
d3d9_native_window_state(HWND hwnd, uint32_t width, uint32_t height, uint32_t flags) {
  D3D9_NATIVE_ENTER(D3D9SHIM_OP_window_state);
  try {
    if (flags & D3D9SHIM_WINDOW_GONE) {
      {
        std::lock_guard<std::mutex> guard(window_mutex());
        window_states().erase(hwnd);
      }
      dxmt::wsi::madeira_forget_window(hwnd);
      return D3D_OK;
    }

    /* A NULL hwnd sets the default every window with no entry of its own
     * falls back to, which is what madeira_set_client_size already means. */
    if (width && height)
      dxmt::wsi::madeira_set_client_size(hwnd, width, height);
    if (hwnd) {
      std::lock_guard<std::mutex> guard(window_mutex());
      WindowState &state = window_states()[hwnd];
      if (width && height) {
        state.width = width;
        state.height = height;
      }
      state.flags = flags;
    }
    return D3D_OK;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("window_state");
    return E_FAIL;
  }
}

/* ---- transport: the root interface (slot 323) -------------------------- */

/* Direct3DCreate9(Ex) is a DLL export rather than a vtable slot, so no
 * interface in the description produces it; this is the handle every other
 * handle in the process descends from. */
HRESULT
d3d9_native_create_interface(d3d9_native_handle *iface, uint32_t sdk_version, uint32_t is_ex) {
  D3D9_NATIVE_ENTER(D3D9SHIM_OP_create_interface);
  try {
    if (!iface)
      return D3DERR_INVALIDCALL;
    *iface = 0;

    auto *created = new dxmt::MTLD3D9Interface(sdk_version, is_ex != 0);
    created->AddRef();
    d3d9_native_adopt<IDirect3D9Ex>(static_cast<IDirect3D9Ex *>(created),
                                    D3D9_NATIVE_KIND_D3D9, iface, true);
    return *iface ? D3D_OK : E_OUTOFMEMORY;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("create_interface");
    return E_FAIL;
  }
}

/* ---- the six bodies that are not mechanical --------------------------- */

/* CreateDevice / CreateDeviceEx owe two things a forwarding body cannot:
 *
 *  1. D3DCREATE_MULTITHREADED is stripped.  8.2(d) moves that lock into the
 *     shim -- a recursive spinlock keyed by GetCurrentThreadId, taken on the
 *     guest's own thread -- and says the native device is constructed
 *     `is_protected=false`.  MTLD3D9Device derives is_protected from the
 *     BehaviorFlags it is handed (d3d9_device.cpp:533), so clearing the bit
 *     HERE is how that is expressed without reaching into the device.
 *     Leaving it set would take a second, redundant recursive lock at every
 *     one of the 320 entry points, on the hot path, for a race the shim has
 *     already serialised.  What the application passed is remembered so
 *     GetCreationParameters still reports it.
 *  2. The window-size cache is seeded.  The shim pushes the real client size
 *     on slot 322 before it calls, but a device created with a zero extent
 *     and a non-NULL hDeviceWindow makes CanonicalisePresentParams ask
 *     getWindowSize() during THIS call; if nothing has been pushed for that
 *     window yet, the presentation parameters are the best answer available.
 */
static void
seed_window_size(HWND window, const D3DPRESENT_PARAMETERS *parameters) {
  if (!window || !parameters || !parameters->BackBufferWidth || !parameters->BackBufferHeight)
    return;
  std::lock_guard<std::mutex> guard(window_mutex());
  if (window_states().count(window))
    return; /* the shim has already said what this window really is */
  window_states()[window] =
      WindowState{parameters->BackBufferWidth, parameters->BackBufferHeight, 0u};
  dxmt::wsi::madeira_set_client_size(window, parameters->BackBufferWidth,
                                     parameters->BackBufferHeight);
}

static void
remember_behavior_flags(void *device, DWORD flags) {
  if (!device)
    return;
  std::lock_guard<std::mutex> guard(device_flags_mutex());
  device_flags()[device] = flags;
}

/* IDirect3D9Ex::CreateDevice -- slot 16, sync */
HRESULT
d3d9_native_D3D9Ex_CreateDevice(d3d9_native_handle self, UINT adapter_idx, D3DDEVTYPE device_type,
                                HWND focus_window, DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                                d3d9_native_handle *device) {
  D3D9_NATIVE_ENTER(D3D9OP_D3D9Ex_CreateDevice);
  try {
    IDirect3D9Ex *_self = d3d9_native_lookup<IDirect3D9Ex>(self, D3D9_NATIVE_KIND_D3D9);
    if (!_self)
      return D3DERR_INVALIDCALL;
    if (device)
      *device = 0;

    seed_window_size(parameters ? parameters->hDeviceWindow : (HWND)NULL, parameters);
    seed_window_size(focus_window, parameters);

    IDirect3DDevice9 *created = nullptr;
    HRESULT hr = _self->CreateDevice(adapter_idx, device_type, focus_window,
                                     flags & ~(DWORD)D3DCREATE_MULTITHREADED,
                                     parameters, &created);
    if (SUCCEEDED(hr) && created)
      remember_behavior_flags(
          static_cast<void *>(static_cast<IDirect3DDevice9Ex *>(created)), flags);
    d3d9_native_adopt<IDirect3DDevice9Ex>(created, D3D9_NATIVE_KIND_DEVICE, device,
                                          SUCCEEDED(hr));
    return hr;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("IDirect3D9Ex::CreateDevice");
    return E_FAIL;
  }
}

/* IDirect3D9Ex::CreateDeviceEx -- slot 20, sync */
HRESULT
d3d9_native_D3D9Ex_CreateDeviceEx(d3d9_native_handle self, UINT adapter_idx, D3DDEVTYPE device_type,
                                  HWND focus_window, DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                                  const D3DDISPLAYMODEEX *mode, d3d9_native_handle *device) {
  D3D9_NATIVE_ENTER(D3D9OP_D3D9Ex_CreateDeviceEx);
  try {
    IDirect3D9Ex *_self = d3d9_native_lookup<IDirect3D9Ex>(self, D3D9_NATIVE_KIND_D3D9);
    if (!_self)
      return D3DERR_INVALIDCALL;
    if (device)
      *device = 0;

    seed_window_size(parameters ? parameters->hDeviceWindow : (HWND)NULL, parameters);
    seed_window_size(focus_window, parameters);

    IDirect3DDevice9Ex *created = nullptr;
    /* The SDK declares pFullscreenDisplayMode non-const while the runtime
     * only reads it; d3d9_api.py describes it as the input it is. */
    HRESULT hr = _self->CreateDeviceEx(adapter_idx, device_type, focus_window,
                                       flags & ~(DWORD)D3DCREATE_MULTITHREADED, parameters,
                                       const_cast<D3DDISPLAYMODEEX *>(mode), &created);
    if (SUCCEEDED(hr) && created)
      remember_behavior_flags(static_cast<void *>(created), flags);
    d3d9_native_adopt<IDirect3DDevice9Ex>(created, D3D9_NATIVE_KIND_DEVICE, device,
                                          SUCCEEDED(hr));
    return hr;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("IDirect3D9Ex::CreateDeviceEx");
    return E_FAIL;
  }
}

/* IDirect3DDevice9Ex::GetCreationParameters -- slot 9, sync.
 * Puts back the D3DCREATE_MULTITHREADED bit CreateDevice stripped, so the
 * application reads back the flags it actually passed. */
HRESULT
d3d9_native_Device9Ex_GetCreationParameters(d3d9_native_handle self,
                                            D3DDEVICE_CREATION_PARAMETERS *parameters) {
  D3D9_NATIVE_ENTER(D3D9OP_Device9Ex_GetCreationParameters);
  try {
    IDirect3DDevice9Ex *_self =
        d3d9_native_lookup<IDirect3DDevice9Ex>(self, D3D9_NATIVE_KIND_DEVICE);
    if (!_self)
      return D3DERR_INVALIDCALL;
    HRESULT hr = _self->GetCreationParameters(parameters);
    if (SUCCEEDED(hr) && parameters) {
      std::lock_guard<std::mutex> guard(device_flags_mutex());
      auto it = device_flags().find(static_cast<void *>(_self));
      if (it != device_flags().end())
        parameters->BehaviorFlags = it->second;
    }
    return hr;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("IDirect3DDevice9Ex::GetCreationParameters");
    return E_FAIL;
  }
}

/* IDirect3DDevice9Ex::CheckResourceResidency -- slot 125, sync.
 *
 * The array holds GUEST interface pointers -- the application's own
 * IDirect3DResource9 *, which are shim objects and mean nothing on this side
 * of the boundary; there is no handle in them to look up.  The native body
 * (d3d9_device.cpp:12201) does not walk the array either: on unified memory
 * every resource is always resident, so the only rule it enforces is MSDN's
 * "D3DERR_INVALIDCALL for a NULL array with a non-zero count" -- and that is
 * answerable from the count and the converted array pointer alone. */
HRESULT
d3d9_native_Device9Ex_CheckResourceResidency(d3d9_native_handle self, const uint32_t *resources,
                                             UINT32 resource_count) {
  D3D9_NATIVE_ENTER(D3D9OP_Device9Ex_CheckResourceResidency);
  try {
    IDirect3DDevice9Ex *_self =
        d3d9_native_lookup<IDirect3DDevice9Ex>(self, D3D9_NATIVE_KIND_DEVICE);
    if (!_self)
      return D3DERR_INVALIDCALL;
    if (resource_count > 0 && !resources)
      return D3DERR_INVALIDCALL;
    return D3D_OK;
  } catch (...) {
    D3D9_NATIVE_CAUGHT("IDirect3DDevice9Ex::CheckResourceResidency");
    return E_FAIL;
  }
}

/* IDirect3DSurface9::GetDC / ReleaseDC -- slots 15 and 16.
 *
 * SHIM-LOCAL.  8.2(d) keeps every user32/gdi32 call on the guest's own
 * thread, and d3d9shim_custom.c does the whole thing there: GetDesc,
 * LockRect, D3DKMTCreateDCFromMemory over the mapped pBits, and the matching
 * unlock in ReleaseDC.  Nothing crosses -- and if something ever did, an HDC
 * minted on this side would be a host handle no guest gdi32 could use, which
 * is why these say so rather than try. */
HRESULT
d3d9_native_Surface9_GetDC(d3d9_native_handle self, HANDLE phdc) {
  D3D9_NATIVE_ENTER(D3D9OP_Surface9_GetDC);
  (void)self;
  (void)phdc;
  D3D9_NATIVE_LOCAL("IDirect3DSurface9::GetDC");
  return E_NOTIMPL;
}

HRESULT
d3d9_native_Surface9_ReleaseDC(d3d9_native_handle self, HANDLE hdc) {
  D3D9_NATIVE_ENTER(D3D9OP_Surface9_ReleaseDC);
  (void)self;
  (void)hdc;
  D3D9_NATIVE_LOCAL("IDirect3DSurface9::ReleaseDC");
  return E_NOTIMPL;
}

/* ---- and the other 314, emitted from d3d9_api.py ---------------------- */

#include "d3d9_native_gen.inc"

} /* extern "C" */

/* ======================================================================
 * The census summary
 *
 * Below the include because it names d3d9_native_op_names[], which is emitted
 * there so a counter cannot label itself with a slot it did not count.
 * ====================================================================== */

namespace {

const char *
op_name(unsigned op) {
  return op < D3D9SHIM_OP_COUNT ? d3d9_native_op_names[op] : "(none)";
}

/* One line per crossing under MADEIRA_D3D9_TRACE=1.  Deliberately the last
 * thing printed before the frontend runs, so a guest-side fault with no D3D9
 * frame in it still says which call it followed. */
void
trace_line(unsigned op) {
  std::fprintf(stderr, "[d3d9-last] #%llu %s\n",
               (unsigned long long)g_last_seq, op_name(op));
}

void
last_call_line(const char *why) {
  std::fprintf(stderr,
               "[d3d9-last] %s: this thread %llu crossings, last %s; any "
               "thread last %s\n",
               why, (unsigned long long)g_last_seq, op_name(g_last_op),
               op_name(g_last_op_any.load(std::memory_order_relaxed)));
}

void
census_report() {
  uint32_t frames = g_census_frames.load(std::memory_order_relaxed);
  uint32_t window_frames = frames - g_census_prev_frames;
  unsigned long long total = 0, window = 0;
  unsigned used = 0;

  if (!window_frames)
    window_frames = 1;

  static uint32_t current[D3D9SHIM_OP_COUNT];
  for (unsigned i = 0; i < D3D9SHIM_OP_COUNT; i++) {
    current[i] = g_census_calls[i].load(std::memory_order_relaxed);
    total += current[i];
    /* unsigned wrap is the right arithmetic for a windowed delta */
    window += (uint32_t)(current[i] - g_census_prev[i]);
    if (current[i])
      used++;
  }

  if (!g_census_seq)
    std::fprintf(stderr,
                 "[d3d9-native-census] armed: %u slots, summaries at present "
                 "1/100/1000 then every %u (MADEIRA_D3D9_NATIVE_CENSUS=0 "
                 "disables, MADEIRA_D3D9_NATIVE_CENSUS_EVERY overrides)\n",
                 (unsigned)D3D9SHIM_OP_COUNT, g_census_interval);
  g_census_seq++;

  std::fprintf(stderr,
               "[d3d9-native-census] ---- summary %u: present=%u frames=%u ----\n",
               g_census_seq, frames, window_frames);
  std::fprintf(stderr,
               "[d3d9-native-census] crossings: window=%llu total=%llu "
               "per_frame=%.1f used=%u/%u\n",
               window, total, (double)window / (double)window_frames, used,
               (unsigned)D3D9SHIM_OP_COUNT);

  /* The ten busiest slots of the window.  In phase 1 this list IS the phase-2
   * work list: every one of them is a crossing the command ring exists to
   * take away (8.6). */
  for (int rank = 0; rank < 10; rank++) {
    unsigned best = 0;
    uint32_t best_count = 0;
    for (unsigned i = 0; i < D3D9SHIM_OP_COUNT; i++) {
      uint32_t delta = (uint32_t)(current[i] - g_census_prev[i]);
      if (delta > best_count) {
        best_count = delta;
        best = i;
      }
    }
    if (!best_count)
      break;
    std::fprintf(stderr, "[d3d9-native-census]   %-46s %8u  %7.1f/frame\n",
                 d3d9_native_op_names[best], best_count,
                 (double)best_count / (double)window_frames);
    g_census_prev[best] = current[best]; /* consumed; do not rank it twice */
  }

  for (unsigned i = 0; i < D3D9SHIM_OP_COUNT; i++)
    g_census_prev[i] = current[i];
  g_census_prev_frames = frames;

  std::fprintf(stderr, "[d3d9-native-census] arena: %llu bytes high water\n",
               (unsigned long long)d3d9_native_arena_high_water());
  last_call_line("census");
}

} // namespace

/* Read the two knobs once, from _d3d9_init, on the guest's own thread. */
extern "C" void
d3d9_native_census_configure(void) {
  const char *off = std::getenv("MADEIRA_D3D9_NATIVE_CENSUS");
  const char *every = std::getenv("MADEIRA_D3D9_NATIVE_CENSUS_EVERY");

  if (off
      && (!std::strcmp(off, "0") || !std::strcmp(off, "off") || !std::strcmp(off, "no")
          || !std::strcmp(off, "false")))
    g_census_on = false;
  if (every) {
    long value = std::strtol(every, nullptr, 10);
    if (value > 0)
      g_census_interval = (unsigned)value;
  }

  const char *trace = std::getenv("MADEIRA_D3D9_TRACE");
  if (trace && std::strcmp(trace, "0") && std::strcmp(trace, "off")
      && std::strcmp(trace, "no") && std::strcmp(trace, "false")) {
    g_trace_on = true;
    std::fprintf(stderr,
                 "[d3d9-last] MADEIRA_D3D9_TRACE=%s: one line per crossing. "
                 "The last one printed is the call the guest was in when it "
                 "died.\n",
                 trace);
  }

  const char *lockcheck = std::getenv("MADEIRA_D3D9_LOCKCHECK");
  if (lockcheck && std::strcmp(lockcheck, "0") && std::strcmp(lockcheck, "off")
      && std::strcmp(lockcheck, "no") && std::strcmp(lockcheck, "false")) {
    d3d9_native_lockcheck_on = 1;
    std::fprintf(stderr,
                 "[d3d9-lockcheck] armed: every pointer handed back to the "
                 "guest (pBits, ppbData) is looked up in the arena and its "
                 "first and last byte are read-modify-written before the "
                 "application sees it (WOW64_DESIGN.md 8.2(c))\n");
  }
}
