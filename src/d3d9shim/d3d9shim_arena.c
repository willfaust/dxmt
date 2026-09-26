/*
 * d3d9shim_arena.c -- the guest arena (WOW64_DESIGN.md 8.2(c) / 7.5)
 *
 * Every pointer the application may dereference has to be inside [B, B+4G):
 * d3d9_buffer_map.hpp:13-54 states the contract and :36-47 says why it can
 * never change.  Natively, wsi::aligned_malloc returns host heap, which is
 * unusable for that.  So the SHIM reserves the memory -- a VirtualAlloc inside
 * a WoW pseudo-process goes through the window chokepoint, so the result is
 * inside the window by construction -- and registers {guest_base, size} with
 * the native side, which sub-allocates out of it and computes guest addresses
 * trivially.  Exhaustion comes back as a distinguished status and is answered
 * by growing and retrying; there is no upcall machinery anywhere.
 *
 * Alignment is 16384, the real iOS page size: DXMT_PAGE_SIZE is 4096
 * unconditionally (research/dxmt/meson.build:155), which is wrong here.
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

#define CINTERFACE
#define COBJMACROS

#include <stdio.h>
#include <string.h>

#include "d3d9shim_object.h"

#define D3D9SHIM_ARENA_CHUNK   (64u * 1024u * 1024u)
#define D3D9SHIM_ARENA_ALIGN   16384u
#define D3D9SHIM_ARENA_MAGIC   0xd3d9a9e4u

struct arena_block {
    uint32_t             magic;
    uint32_t             size;      /* payload bytes, alignment included */
    struct arena_block * next_free;
    void *               payload;
    uint32_t             _pad0;
};

struct arena_chunk {
    struct arena_chunk * next;
    unsigned char *      base;
    size_t               size;
    size_t               used;      /* bump offset */
    struct arena_block * free_list;
};

static struct arena_chunk *arena_chunks;
static CRITICAL_SECTION    arena_cs;
static LONG                arena_ready;
static size_t              arena_committed;
static size_t              arena_in_use;
static size_t              arena_high_water;

static void
arena_lock(void)
{
    if (InterlockedCompareExchange(&arena_ready, 1, 0) == 0)
        InitializeCriticalSection(&arena_cs);
    EnterCriticalSection(&arena_cs);
}

static void
arena_unlock(void)
{
    LeaveCriticalSection(&arena_cs);
}

static HRESULT arena_add_chunk(size_t need);

/* Reserve and register the FIRST chunk, now, from the transport handshake.
 *
 * This used to do nothing but construct the critical section, on the reading
 * that the arena grows on demand.  It cannot: the consumer is
 * dxmt::guest_alloc() on the NATIVE side, and the only thing that ever calls
 * d3d9shim_arena_grow() is d3d9shim_native_call()'s answer to
 * D3D9SHIM_STATUS_ARENA_EXHAUSTED.  With no chunk registered, guest_alloc()
 * returns NULL for every app-visible allocation from the very first
 * CreateDevice -- the native side says so once ("arena: no chunk registered")
 * and then every Lock mirror, every MANAGED/SYSTEMMEM mirror and every
 * backing-pool block is a failed allocation, while the create call it was
 * made for still reports S_OK.  The first thing the application writes
 * through is then not memory it owns.
 *
 * So the arena is established before the first D3D9 object can exist.  One
 * 64 MB chunk, the size 8.2(c) specifies; growth still happens on demand.
 * A failure here is not fatal -- the native side still names it -- but it is
 * said out loud at the point it can still be understood. */
int
d3d9shim_arena_init(void)
{
    HRESULT hr;

    arena_lock();
    hr = arena_chunks ? D3D_OK : arena_add_chunk(0);
    arena_unlock();
    if (FAILED(hr)) {
        d3d9shim_log_once("[d3d9-arena] could not reserve the first chunk; "
                          "every app-visible allocation will fail");
        return 0;
    }
    return 1;
}

static size_t
round_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Reserve one more chunk and hand the native sub-allocator its guest base.
 * The registration is the hand-written D3D9SHIM_OP_arena_register slot; see
 * d3d9shim_object.h for why it lives after the generated range. */
static HRESULT
arena_add_chunk(size_t need)
{
    struct d3d9_arena_register_params params;
    struct arena_chunk *chunk;
    size_t size = D3D9SHIM_ARENA_CHUNK;
    void *base;

    if (need > size)
        size = round_up(need, D3D9SHIM_ARENA_ALIGN);

    base = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        d3d9shim_trace("[d3d9-arena] VirtualAlloc failed; the guest address "
                       "space is exhausted, not the arena");
        return E_OUTOFMEMORY;
    }
    /* Invariant 4 and 8.2(c): this address IS the guest address, and it is
     * inside the window because the allocation went through the chokepoint.
     * The one thing worth asserting is that the chunk does not run off the
     * end of the 32-bit space, because the native side computes guest
     * addresses by adding an offset to guest_base. */
    if ((uint64_t)(ULONG_PTR)base + (uint64_t)size > 0x100000000ull) {
        d3d9shim_trace("[d3d9-arena] refusing a chunk that would run past 4 GB");
        VirtualFree(base, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }

    chunk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*chunk));
    if (!chunk) {
        VirtualFree(base, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }
    chunk->base = base;
    chunk->size = size;
    chunk->used = 0;
    chunk->next = arena_chunks;
    arena_chunks = chunk;
    arena_committed += size;

    memset(&params, 0, sizeof(params));
    params.guest_base = (uint32_t)(ULONG_PTR)base;
    params.size = (uint32_t)size;
    if (d3d9shim_native_call(D3D9SHIM_OP_arena_register, &params, sizeof(params))
        || FAILED((HRESULT)params.ret)) {
        /* Keep the chunk: the shim's own allocations out of it are still
         * valid guest memory, and saying so once is more useful than
         * pretending the reservation failed. */
        d3d9shim_log_once("[d3d9-arena] the native side did not accept a chunk "
                          "registration");
    }
    {
        char line[160];

        snprintf(line, sizeof(line),
                 "[d3d9-arena] chunk 0x%08x size %u MB, committed %u MB",
                 (unsigned int)(ULONG_PTR)base,
                 (unsigned int)(size >> 20),
                 (unsigned int)(arena_committed >> 20));
        d3d9shim_trace(line);
    }
    return D3D_OK;
}

HRESULT
d3d9shim_arena_grow(size_t hint)
{
    HRESULT hr;

    arena_lock();
    hr = arena_add_chunk(hint);
    arena_unlock();
    return hr;
}

static void *
alloc_from_chunk(struct arena_chunk *chunk, size_t size, size_t align)
{
    struct arena_block **pp;
    struct arena_block *block;
    unsigned char *payload;
    size_t offset;

    /* first fit over the free list */
    for (pp = &chunk->free_list; *pp; pp = &(*pp)->next_free) {
        block = *pp;
        if (block->size < size)
            continue;
        if ((ULONG_PTR)block->payload & (align - 1u))
            continue;
        *pp = block->next_free;
        block->next_free = NULL;
        return block->payload;
    }

    offset = round_up(chunk->used + sizeof(struct arena_block), align);
    if (offset + size > chunk->size)
        return NULL;
    payload = chunk->base + offset;
    block = (struct arena_block *)(payload - sizeof(struct arena_block));
    block->magic = D3D9SHIM_ARENA_MAGIC;
    block->size = (uint32_t)size;
    block->next_free = NULL;
    block->payload = payload;
    chunk->used = offset + size;
    return payload;
}

void *
d3d9shim_arena_alloc(size_t size, size_t align)
{
    struct arena_chunk *chunk;
    void *out = NULL;

    if (!size)
        return NULL;
    if (align < 16u)
        align = 16u;
    size = round_up(size, 16u);

    arena_lock();
    for (chunk = arena_chunks; chunk && !out; chunk = chunk->next)
        out = alloc_from_chunk(chunk, size, align);
    if (!out && SUCCEEDED(arena_add_chunk(size + align))) {
        /* grow-and-retry: the newest chunk is at the head */
        out = alloc_from_chunk(arena_chunks, size, align);
    }
    if (out) {
        arena_in_use += size;
        if (arena_in_use > arena_high_water) {
            arena_high_water = arena_in_use;
            if ((arena_high_water & ((8u << 20) - 1u)) < size) {
                char line[160];

                snprintf(line, sizeof(line),
                         "[d3d9-arena] high water %u MB of %u MB committed",
                         (unsigned int)(arena_high_water >> 20),
                         (unsigned int)(arena_committed >> 20));
                d3d9shim_trace(line);
            }
        }
    }
    arena_unlock();
    return out;
}

void
d3d9shim_arena_free(void *p)
{
    struct arena_block *block;
    struct arena_chunk *chunk;

    if (!p)
        return;
    block = (struct arena_block *)((unsigned char *)p - sizeof(struct arena_block));

    arena_lock();
    if (block->magic != D3D9SHIM_ARENA_MAGIC) {
        arena_unlock();
        d3d9shim_log_once("[d3d9-arena] free of a pointer this arena never "
                          "handed out");
        return;
    }
    for (chunk = arena_chunks; chunk; chunk = chunk->next) {
        if ((unsigned char *)p >= chunk->base
            && (unsigned char *)p < chunk->base + chunk->size) {
            block->next_free = chunk->free_list;
            chunk->free_list = block;
            if (arena_in_use >= block->size)
                arena_in_use -= block->size;
            break;
        }
    }
    arena_unlock();
}

void
d3d9shim_arena_report(void)
{
    char line[160];

    arena_lock();
    snprintf(line, sizeof(line),
             "[d3d9-arena] high water %u MB, in use %u MB, committed %u MB",
             (unsigned int)(arena_high_water >> 20),
             (unsigned int)(arena_in_use >> 20),
             (unsigned int)(arena_committed >> 20));
    arena_unlock();
    d3d9shim_trace(line);
}
