/* SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Madeira-Converter-Exception */
#pragma once

/* A remote heap uses one guest reservation so overlapping placed buffers alias. */
struct wmt_heap_arena {
  obj_handle_t handle;
  uint64_t size;
  unsigned refs, buffers;
  uint8_t *base;
  struct wmt_heap_arena *next;
};
struct wmt_heap_buffer {
  obj_handle_t handle;
  uint64_t offset, length;
  unsigned refs;
  struct wmt_heap_arena *arena;
  struct wmt_heap_buffer *next;
};
static SRWLOCK wmt_heap_lock = SRWLOCK_INIT;
static struct wmt_heap_arena *wmt_heap_arenas;
static struct wmt_heap_buffer *wmt_heap_buffers;

static void wmt_heap_dispose_locked(struct wmt_heap_arena *arena) {
  if (arena->refs || arena->buffers) return;
  struct wmt_heap_arena **link = &wmt_heap_arenas;
  while (*link && *link != arena) link = &(*link)->next;
  if (*link) *link = arena->next;
  VirtualFree(arena->base, 0, MEM_RELEASE);
  free(arena);
}

static int wmt_heap_create(obj_handle_t handle, uint64_t size) {
  if (!handle || !size || size > SIZE_MAX || size >= WMT_HEAP_AUTO_SHADOW) return 0;
  struct wmt_heap_arena *arena = calloc(1, sizeof(*arena));
  if (!arena) return 0;
  arena->base = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
  if (!arena->base) { free(arena); return 0; }
  arena->handle = handle; arena->size = size; arena->refs = 1;
  AcquireSRWLockExclusive(&wmt_heap_lock);
  arena->next = wmt_heap_arenas; wmt_heap_arenas = arena;
  ReleaseSRWLockExclusive(&wmt_heap_lock);
  return 1;
}

/* Begin holds the lock until finish, so the reservation survives the unix call. */
static int wmt_heap_begin(obj_handle_t heap, uint64_t offset, uint64_t length,
                          void **memory, struct wmt_heap_buffer **token) {
  AcquireSRWLockExclusive(&wmt_heap_lock);
  struct wmt_heap_arena *arena = wmt_heap_arenas;
  while (arena && arena->handle != heap) arena = arena->next;
  if (!arena) { ReleaseSRWLockExclusive(&wmt_heap_lock); return 0; }
  if (!arena->refs || !length || length > arena->size) { ReleaseSRWLockExclusive(&wmt_heap_lock); return -1; }
  if (offset == UINT64_MAX) {
    offset = 0;
    for (;;) {
      uint64_t next = offset;
      for (struct wmt_heap_buffer *b=wmt_heap_buffers; b; b=b->next)
        if (b->arena == arena && offset < b->offset+b->length && b->offset < offset+length && b->offset+b->length > next)
          next=b->offset+b->length;
      if (next == offset) break;
      offset=next;
      if (offset > arena->size || length > arena->size-offset) break;
    }
  }
  if (!arena->refs || !length || offset > arena->size || length > arena->size - offset) {
    ReleaseSRWLockExclusive(&wmt_heap_lock); return -1;
  }
  struct wmt_heap_buffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer || !VirtualAlloc(arena->base + (size_t)offset, (SIZE_T)length, MEM_COMMIT, PAGE_READWRITE)) {
    free(buffer); ReleaseSRWLockExclusive(&wmt_heap_lock); return -1;
  }
  buffer->arena = arena; buffer->refs = 1; buffer->offset = offset; buffer->length = length;
  *memory = arena->base + (size_t)offset; *token = buffer;
  return 1;
}

static void wmt_heap_finish(struct wmt_heap_buffer *buffer, obj_handle_t handle) {
  if (handle) {
    buffer->handle = handle; buffer->arena->buffers++;
    buffer->next = wmt_heap_buffers; wmt_heap_buffers = buffer;
  } else free(buffer);
  ReleaseSRWLockExclusive(&wmt_heap_lock);
}

static void wmt_heap_reference(obj_handle_t handle, int retain) {
  AcquireSRWLockExclusive(&wmt_heap_lock);
  struct wmt_heap_buffer **link = &wmt_heap_buffers;
  while (*link && (*link)->handle != handle) link = &(*link)->next;
  if (*link) {
    struct wmt_heap_buffer *buffer = *link;
    if (retain) buffer->refs++;
    else if (--buffer->refs == 0) {
      struct wmt_heap_arena *arena = buffer->arena;
      *link = buffer->next; free(buffer); arena->buffers--;
      wmt_heap_dispose_locked(arena);
    }
  } else {
    struct wmt_heap_arena *arena = wmt_heap_arenas;
    while (arena && arena->handle != handle) arena = arena->next;
    if (arena) {
      if (retain) arena->refs++;
      else if (arena->refs) { arena->refs--; wmt_heap_dispose_locked(arena); }
    }
  }
  ReleaseSRWLockExclusive(&wmt_heap_lock);
}
