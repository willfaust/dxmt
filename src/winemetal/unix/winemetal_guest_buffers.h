/* Metal may release a buffer on a driver thread without Wine thread state.
 * Queue its storage there, then free it on a caller in the owning guest window.
 * The no-copy deallocator runs after Metal's LAST reference, including GPU work.
 */
#if TARGET_OS_IOS
#include <pthread.h>
#include <unistd.h>

extern NTSTATUS NtAllocateVirtualMemory(void *, void **, uintptr_t, size_t *, unsigned, unsigned);
extern NTSTATUS NtFreeVirtualMemory(void *, void **, size_t *, unsigned);
/* Optional host hook: zero outside a guest window, otherwise unique for each
 * reservation, including when a later process reuses the same base address. */
extern uint64_t ios_wow_window_identity(void) __attribute__((weak_import));

struct wmt_guest_storage {
  struct wmt_guest_storage *next;
  uint64_t owner;
  void *memory;
};
static pthread_mutex_t wmt_guest_storage_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wmt_guest_storage *wmt_guest_storage_retired;

static void wmt_guest_storage_retire(struct wmt_guest_storage *storage)
{
  pthread_mutex_lock(&wmt_guest_storage_lock);
  storage->next = wmt_guest_storage_retired;
  wmt_guest_storage_retired = storage;
  pthread_mutex_unlock(&wmt_guest_storage_lock);
}

static void wmt_guest_storage_reclaim(void)
{
  uint64_t owner = ios_wow_window_identity ? ios_wow_window_identity() : 0;
  struct wmt_guest_storage **link, *storage, *ready = NULL;
  if (!owner) return;
  pthread_mutex_lock(&wmt_guest_storage_lock);
  link = &wmt_guest_storage_retired;
  while ((storage = *link)) {
    if (storage->owner != owner) { link = &storage->next; continue; }
    *link = storage->next;
    storage->next = ready;
    ready = storage;
  }
  pthread_mutex_unlock(&wmt_guest_storage_lock);
  while ((storage = ready)) {
    size_t size = 0;
    ready = storage->next;
    if (NtFreeVirtualMemory((void *)-1, &storage->memory, &size, 0x8000 /* MEM_RELEASE */))
      wmt_guest_storage_retire(storage);
    else free(storage);
  }
}

static NTSTATUS wmt_guest_buffer_new(struct unixcall_mtldevice_newbuffer *params,
                                    struct WMTBufferInfo *info)
{
  uintptr_t base = ios_wow_base();
  uint64_t owner = ios_wow_window_identity ? ios_wow_window_identity() : 0;
  size_t page = (size_t)getpagesize(), size;
  struct wmt_guest_storage *storage;
  id<MTLBuffer> buffer;
  NTSTATUS status;
  params->ret = 0;
  info->memory.ptr = NULL;
  info->gpu_address = 0;
  /* Remote buffers have a separate lifetime protocol. Keep its existing refusal. */
  if (!base || !owner || wmtr_enabled()) return STATUS_INVALID_ADDRESS;
  if (!info->length || info->length > 0x7fffffffULL - (page - 1))
    return STATUS_INVALID_PARAMETER;
  size = (size_t)((info->length + page - 1) & ~(page - 1));
  storage = calloc(1, sizeof *storage);
  if (!storage) return STATUS_UNSUCCESSFUL;
  storage->owner = owner;
  status = NtAllocateVirtualMemory((void *)-1, &storage->memory, 1, &size,
                                  0x3000 /* COMMIT | RESERVE */, 4 /* READWRITE */);
  if (status) { free(storage); return status; }
  uintptr_t address = (uintptr_t)storage->memory;
  if (address < base || address - base > UINT32_MAX ||
      size > 0x100000000ULL - (address - base)) {
    wmt_guest_storage_retire(storage);
    wmt_guest_storage_reclaim();
    return STATUS_INVALID_ADDRESS;
  }
  buffer = [(id<MTLDevice>)params->device newBufferWithBytesNoCopy:storage->memory
                     length:size options:(enum MTLResourceOptions)info->options
                     deallocator:^(void *memory, NSUInteger length) {
                       (void)memory; (void)length;
                       wmt_guest_storage_retire(storage);
                     }];
  if (!buffer) {
    wmt_guest_storage_retire(storage);
    wmt_guest_storage_reclaim();
    return STATUS_UNSUCCESSFUL;
  }
  info->memory.ptr = (void *)(address - base);
  info->gpu_address = [buffer gpuAddress];
  params->ret = (obj_handle_t)buffer;
  if (wmt_stale_probe_on()) wmt_freed_set((uintptr_t)buffer, 0);
  return STATUS_SUCCESS;
}
#else
static void wmt_guest_storage_reclaim(void) {}
#endif
