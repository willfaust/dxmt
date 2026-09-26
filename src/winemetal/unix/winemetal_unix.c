#include <stdatomic.h>
#include "../../../../../build/madeira_cfg.h"   /* ml1095: one config file */
#include <sys/mman.h>
#include <mach/vm_statistics.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
/* iOS lacks CGDirectDisplay: provide minimal stubs for single-display use. */
typedef uint32_t CGDirectDisplayID;
#define kCGNullDirectDisplay ((CGDirectDisplayID)0)
static inline CGDirectDisplayID CGMainDisplayID(void) { return 1; }
#else
#import <Cocoa/Cocoa.h>
#endif
#if !TARGET_OS_IOS
#import <ColorSync/ColorSync.h>
#endif
#import <CoreFoundation/CFRunLoop.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#if TARGET_OS_IOS
#include <objc/message.h>
#include <objc/runtime.h>
#else
#include "objc/objc-runtime.h"
#endif
#if TARGET_OS_IOS
/* iOS SDK omits bootstrap.h but the functions exist in libsystem. */
typedef char name_t[128];
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
#else
#include <bootstrap.h>
#endif
#include <mach/mach_port.h>
/* ml1050: mach_absolute_time / mach_timebase_info / mach_wait_until for the
 * present limiter and the frame timers. */
#include <mach/mach_time.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../airconv_thunks.h"

/* iOS-Madeira 2026-05-22 draw-call telemetry. Defined further down with
 * the present counter; forward-declared here so the draw command cases
 * (above the definition site in source order) can increment them. */
static _Atomic uint64_t g_madeira_draw_calls;
static _Atomic uint64_t g_madeira_draw_calls_at_last_log;

/* iOS-Madeira ml1050: the [frame] critical-path breakdown.
 *
 * The storage, the accumulators and the reporter all live in
 * build/ntdll-unix/server_ios.c (see build/ntdll-unix/shims/ios_frame_stats.h
 * for the whole design); this file is one of the two producers.  Declared
 * extern here rather than by including that header because this translation
 * unit is compiled by build/dxmt-ios/build.sh, which does not carry the ntdll
 * shims include path -- exactly the way madeira_log_present_cadence() already
 * reaches ios_srv_wait_us further down.  Both halves are statically linked
 * into the one Madeira image, so these are ordinary intra-image symbols.
 *
 * Everything here is a no-op when the instrument is off; the `on` flag is
 * read through the accumulators, so this file never branches on it. */
extern void ios_frame_game_tick(void);
extern void ios_frame_encode_present(int skipped);
extern void ios_frame_drawable_wait(unsigned long long ns);
extern void ios_frame_gpu(unsigned long long gpu_ns, unsigned long long inflight);
extern void ios_frame_note_display(int panel_hz, int intent_hz, int mode);
extern void ios_frame_limiter(unsigned long long ns);

/* Command buffers committed minus command buffers retired: the honest GPU
 * queue depth, which is the number the `[frame]` line reports as qdepth.  Both
 * ends are visible from this file and from nowhere else -- DXMT's own
 * chunk_ongoing counter is PE-side emulated state. */
static _Atomic uint64_t g_madeira_cmdbuf_inflight;
extern int ios_frame_stats_on;
extern void ios_frame_pass(unsigned kind, unsigned loads, unsigned stores, unsigned clears);

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001
#define STATUS_NOT_IMPLEMENTED 0xC0000002
/* MADEIRA (WOW64_DESIGN.md section 7.4, rule 4: no fake success): the 32-bit
 * variants below fail the call when an argument block cannot be converted, so
 * they need the two NT status values for that. */
#define STATUS_INVALID_PARAMETER 0xC000000D
#define STATUS_INVALID_ADDRESS 0xC0000141

/* ml762: remote backend. Included after the NTSTATUS/STATUS_* defines it uses
 * and before the first routed handler; the packer's include sits much further
 * down, past every handler this routes. */
#include "wmt_remote_client.h"

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  /* Dispatch by TAG, not by mode. Guest-local objects legitimately exist in
   * remote mode; only a tagged handle names something on the host. */
  if (wmtr_enabled() && RM_IS_REMOTE((uint64_t)(uintptr_t)*obj)) {
    struct rm_arg_handle a = { (uint64_t)(uintptr_t)*obj };
    struct rm_ret_handle r;
    wmtr_buf_retain(a.handle);   /* ml820: no-op unless it is a registered buffer */
    /* The host returns the SAME handle; identity must not change under retain. */
    wmtr_call(RM_OP_RETAIN, &a, sizeof a, &r, sizeof r, 0);
    return STATUS_SUCCESS;
  }
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj);
/* ml1155: WHICH object did we free? A command buffer's completion crashes in
 * objc_release of an already-freed object (IOGPUMetalCommandBufferStorageReset;
 * ph-valley04/05/14): something we release is released once too often. Every
 * release through here that drops the LAST reference is recorded (pointer and
 * class, lock-free ring); the Mach fault handler looks the crashing pointer up
 * (signal_arm64_ios.c ml1155), so the next crash names the object's class. */
struct wmt_rel_rec { uintptr_t p; const char *cls; };
static struct wmt_rel_rec g_wmt_rel[16384];
static volatile uint64_t g_wmt_rel_n;
__attribute__((visibility("default"))) const char *
madeira_wmt_released_class(uintptr_t addr, uint64_t *releases_ago) {
  uint64_t n = __atomic_load_n(&g_wmt_rel_n, __ATOMIC_RELAXED), i, lim = n > 16384 ? 16384 : n;
  for (i = 0; i < lim; i++) {
    const struct wmt_rel_rec *r = &g_wmt_rel[(n - 1 - i) & 16383];
    if (r->p == addr) { if (releases_ago) *releases_ago = i; return r->cls; }
  }
  return NULL;
}

/* ml1156: the ml1155 answer was an AGXG19FamilyBuffer our release freed, then
 * released again by a completing command buffer. A handle that is dead by the
 * time an encoder binds it would do exactly that (Metal retains the corpse, the
 * command buffer releases it at reset). So: remember every buffer our release
 * frees, forget it when a new buffer is created at that address, and check
 * every binding point; the first hits name the binding path. */
#include <os/lock.h>
#define WMT_FREED_SLOTS 65536
static uintptr_t g_wmt_freed[WMT_FREED_SLOTS];
static os_unfair_lock g_wmt_freed_lock = OS_UNFAIR_LOCK_INIT;
static volatile long g_wmt_stale_hits;
static inline unsigned wmt_freed_hash(uintptr_t p) { return (unsigned)((p >> 4) * 2654435761u) & (WMT_FREED_SLOTS - 1); }
static void wmt_freed_set(uintptr_t p, int add) {
  unsigned i, n;
  if (!p) return;
  os_unfair_lock_lock(&g_wmt_freed_lock);
  for (i = wmt_freed_hash(p), n = 0; n < 64; n++, i = (i + 1) & (WMT_FREED_SLOTS - 1)) {
    if (add) { if (g_wmt_freed[i] == p) break; if (g_wmt_freed[i] <= 1) { g_wmt_freed[i] = p; break; } }
    else { if (g_wmt_freed[i] == p) { g_wmt_freed[i] = 1; break; } if (!g_wmt_freed[i]) break; }
  }
  os_unfair_lock_unlock(&g_wmt_freed_lock);
}
#define wmt_stale_check(o, w) wmt_stale_check_p((uintptr_t)(o), (w))
static int wmt_stale_probe_on(void) {   /* ml1158: the ml1156 probe found its bug (ml1157); off unless madeira.cfg stale-probe = 1 */
  static int on = -1;
  if (on < 0) on = madeira_cfg_int("stale-probe", 0) ? 1 : 0;
  return on;
}
static void wmt_stale_check_p(uintptr_t p, const char *where) {
  const void *obj = (const void *)p;
  if (!wmt_stale_probe_on()) return; unsigned i, n; int hit = 0;
  if (!p) return;
  os_unfair_lock_lock(&g_wmt_freed_lock);
  for (i = wmt_freed_hash(p), n = 0; n < 64; n++, i = (i + 1) & (WMT_FREED_SLOTS - 1)) {
    if (g_wmt_freed[i] == p) { hit = 1; break; }
    if (!g_wmt_freed[i]) break;
  }
  os_unfair_lock_unlock(&g_wmt_freed_lock);
  if (hit) {
    long k = __atomic_add_fetch(&g_wmt_stale_hits, 1, __ATOMIC_RELAXED);
    if (k <= 48 || (k & (k - 1)) == 0) {
      uint64_t ago = 0; const char *cls = madeira_wmt_released_class(p, &ago);
      fprintf(stderr, "[wmt] ml1156 STALE %s: %p was freed by our release (%s, %llu releases ago); hit #%ld\n",
              where, obj, cls ? cls : "?", (unsigned long long)ago, k);
    }
  }
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  if (wmtr_enabled() && RM_IS_REMOTE((uint64_t)(uintptr_t)*obj)) {
    struct rm_arg_handle a = { (uint64_t)(uintptr_t)*obj };
    wmtr_buf_remove(a.handle);   /* ml820: retires only on the LAST guest reference */
    wmtr_call(RM_OP_RELEASE, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  /* ml820: a guest-local autorelease pool going away is the moment the host
   * objects it mirrored (command buffers, encoders) are released too. */
  if (wmtr_enabled() && [*obj isKindOfClass:[NSAutoreleasePool class]]) wmtr_pool_drain();
  if (*obj && [*obj retainCount] == 1) {   /* ml1155: this release frees it */
    uint64_t k = __atomic_fetch_add(&g_wmt_rel_n, 1, __ATOMIC_RELAXED) & 16383;
    g_wmt_rel[k].p = (uintptr_t)*obj; g_wmt_rel[k].cls = object_getClassName(*obj);
    if (wmt_stale_probe_on() && strstr(g_wmt_rel[k].cls, "Buffer")) wmt_freed_set((uintptr_t)*obj, 1);   /* ml1156 */
  }
  [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_ARRAY_OBJECT, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_ARRAY_COUNT, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_COPY_ALL_DEVICES, 0, 0, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

/* ml1042: THE VIDEO MEMORY BUDGET IS WHAT THIS PROCESS CAN AFFORD, NOT WHAT THE
 * GPU COULD ADDRESS.
 *
 * Everything DXGI tells an application about video memory (adapter
 * DedicatedVideoMemory, QueryVideoMemoryInfo.Budget) comes from this one number,
 * and it was Metal's recommendedMaxWorkingSetSize -- on a 12GB phone, most of
 * RAM. An engine budgets system RAM and video RAM as two separate pools: it
 * filled the 4GB of RAM we report (guest band 4070MB dirty) AND streamed
 * textures toward a multi-GB "VRAM" target (IOAccelerator 1983MB and climbing).
 * On this device those are the SAME pool, and the process has one limit. The
 * first run to reach the 3D benchmark sat at 8186MB with 1.5GB in the
 * compressor -- ~2 FPS, scene never finished streaming -- and was jetsammed.
 *
 * So: budget = process limit - the RAM we let the guest believe it has - what
 * our own runtime costs (JIT pool, FEX, Wine, host: ~2.5GB measured), floored at
 * 1GB, never above what Metal recommends. os_proc_available_memory() +
 * phys_footprint gives the real limit, including the Game Mode increase.
 * Documents/madeira-vram-mb.txt overrides the result outright. */
#include <os/proc.h>
#include <mach/mach.h>
static uint64_t madeira_ml1042_video_budget(uint64_t metal_recommended);
/* ml1075: DYNAMIC BUDGET. The number above is a one-off: the game read it once
 * and then grew its heap through a cutscene until jetsam (ph-rdr42: guest heap
 * 2.3 -> 4.0 GB, footprint 4.7 -> 7.9 GB, killed at 8.19). On Windows, DXGI's
 * budget MOVES under memory pressure and RAGE trims its texture pool when it
 * shrinks. So: once the process passes a high-water mark, shrink the advertised
 * budget 1:1 with the excess, floored at 768 MB, recomputed at most every 250 ms,
 * and count the queries -- if the count stays at one, the game does not poll and
 * this cannot help (the budget-change EVENT would be next). */
static uint64_t madeira_ml1075_dynamic_budget(uint64_t base) {
  static uint64_t last_ns, last_budget, last_logged; static unsigned long calls;
  struct timespec ts; uint64_t now;
  calls++;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
  if (last_budget && now - last_ns < 250000000ull) return last_budget;
  last_ns = now;
  {
    task_vm_info_data_t vmi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    uint64_t foot = 0, limit, high, budget = base;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) == KERN_SUCCESS) foot = vmi.phys_footprint;
    limit = (uint64_t)os_proc_available_memory() + foot;
    /* ml1103: madeira.cfg vram-trim-mb = distance below the kill line where the
     * trim starts (default 1536, the ml1075 value); 0 = never trim. ph-rdr56:
     * raising vram-mb to 3072 doubled the snow-scene frame rate, but in the city
     * the footprint reached 7.1 GB, this trim cut the budget to 2625 MB, and the
     * game went back to evicting and re-streaming (the pop-in). */
    { static long long trim_mb = -1; if (trim_mb < 0) { trim_mb = madeira_cfg_int("vram-trim-mb", 1536); if (trim_mb < 0) trim_mb = 1536;
        fprintf(stderr, "[wmt] ml1103 video budget trim starts %lld MB below the kill line (madeira.cfg vram-trim-mb; 0 = never)\n", trim_mb); }
      high = trim_mb ? (limit > ((uint64_t)trim_mb << 20) ? limit - ((uint64_t)trim_mb << 20) : 0) : ~0ull; }
    if (foot > high) budget = base > foot - high ? base - (foot - high) : 0;
    if (budget < (768ull << 20)) budget = 768ull << 20;
    if (budget > base) budget = base;
    if (!last_logged || (last_logged > budget ? last_logged - budget : budget - last_logged) >= (64ull << 20) || (calls % 5000) == 0) {
      fprintf(stderr, "[wmt] ml1075 video budget now %llu MB (base %llu, footprint %llu of %llu MB; %lu queries so far)\n",
              (unsigned long long)(budget >> 20), (unsigned long long)(base >> 20), (unsigned long long)(foot >> 20),
              (unsigned long long)(limit >> 20), calls);
      last_logged = budget;
    }
    last_budget = budget;
    return budget;
  }
}
static uint64_t madeira_ml1042_video_budget(uint64_t metal_recommended) {
  static uint64_t cached;
  if (cached) return madeira_ml1075_dynamic_budget(cached);

  uint64_t budget = 0;
  { long long mb = madeira_cfg_int("vram-mb", 0);   /* ml1095: madeira.cfg vram-mb = N */
    if (mb >= 256) budget = (uint64_t)mb << 20; }
  uint64_t limit = 0, foot = 0;
  if (!budget) {
    task_vm_info_data_t vmi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) == KERN_SUCCESS) foot = vmi.phys_footprint;
    limit = (uint64_t)os_proc_available_memory() + foot;
    const uint64_t guest_ram = 4096ull << 20;      /* what ml992 lets the guest see */
    const uint64_t overhead  = 2560ull << 20;      /* JIT pool + FEX + Wine + host, measured */
    budget = limit > guest_ram + overhead ? limit - guest_ram - overhead : 0;
    if (budget < (1024ull << 20)) budget = 1024ull << 20;
  }
  if (metal_recommended && budget > metal_recommended) budget = metal_recommended;
  cached = budget;
  fprintf(stderr, "[wmt] ml1042 video memory budget = %llu MB (process limit %llu MB, footprint now %llu MB, "
                  "Metal recommends %llu MB)\n",
          (unsigned long long)(budget >> 20), (unsigned long long)(limit >> 20),
          (unsigned long long)(foot >> 20), (unsigned long long)(metal_recommended >> 20));
  return madeira_ml1075_dynamic_budget(cached);
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_DEVICE_MAX_WORKING_SET, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = madeira_ml1042_video_budget([(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize]);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_ALLOCATED_SIZE, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    char buf[256]; uint32_t got = 0;
    if (wmtr_call(RM_OP_DEVICE_NAME, &a, sizeof a, buf, sizeof buf - 1, &got) == RM_OK) {
      if (got >= sizeof buf) got = sizeof buf - 1;
      buf[got] = 0;
      params->ret = (obj_handle_t)[[NSString alloc] initWithUTF8String:buf];
    } else {
      params->ret = (obj_handle_t)[[NSString alloc] initWithUTF8String:"remote device"];
    }
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle name];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandQueue(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_NEW_COMMAND_QUEUE, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandQueueWithMaxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  if (wmtr_enabled()) wmtr_pool_begin();   /* ml820 */
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_COMMAND_BUFFER, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    wmtr_pool_push(params->ret);   /* ml820: autoreleased natively */
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLCommandQueue>)params->handle commandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (wmtr_enabled()) {
    /* Push guest shadows BEFORE the GPU reads them. The app writes into buffer
     * contents with no call of its own, so this is the last point at which the
     * host copy can be made to match. */
    wmtr_flush_buffers();
    struct rm_arg_handle a = { params->handle };
    wmtr_call(RM_OP_COMMIT, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  /* ml1140: retirement belongs to the GPU completion, not waitUntilCompleted.
   * The finish thread skips that wait for buffers already completed, so the
   * old subtraction counted them as queued forever and omitted their GPU time. */
  if (ios_frame_stats_on) {
    atomic_fetch_add_explicit(&g_madeira_cmdbuf_inflight, 1, memory_order_relaxed);
    [(id<MTLCommandBuffer>)params->handle addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      uint64_t depth = atomic_fetch_sub_explicit(&g_madeira_cmdbuf_inflight, 1, memory_order_relaxed);
      double span = buffer.GPUEndTime - buffer.GPUStartTime;
      ios_frame_gpu(buffer.GPUStartTime > 0.0 && span > 0.0 ? (unsigned long long)(span * 1e9) : 0, depth);
    }];
  }
  [(id<MTLCommandBuffer>)params->handle commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    wmtr_call(RM_OP_WAIT_COMPLETED, &a, sizeof a, 0, 0, 0);
    wmtr_rb_drain(params->handle);   /* ml820: GPU results back before the guest reads them */
    return STATUS_SUCCESS;
  }
  [(id<MTLCommandBuffer>)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_CMDBUF_STATUS, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    if (params->ret >= WMTCommandBufferStatusCompleted) wmtr_rb_drain(params->handle);   /* ml820 */
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLCommandBuffer>)params->handle status];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_NEW_SHARED_EVENT, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_SHARED_EVENT_VALUE, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_encode_sig a = { params->handle, params->arg0, params->arg1 };
    wmtr_call(RM_OP_ENCODE_SIGNAL, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  [(id<MTLCommandBuffer>)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBuffer(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  if (wmtr_enabled()) {
    struct WMTBufferInfo *bi = params->info.ptr;
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTBufferInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof *bi; w->extra_count = 0;
    memcpy(buf + sizeof *w, bi, sizeof *bi);
    struct rm_ret_handle_u64 r;
    params->ret = 0;
    if (wmtr_call(RM_OP_NEW_BUFFER_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK && r.handle) {
      params->ret = r.handle;
      bi->gpu_address = r.value;
      /* Private buffers are never touched by the CPU, so they need no shadow. */
      int cpu = ((bi->options & 0x30) != WMTResourceStorageModePrivate);
      /* CRITICAL: if the caller supplied memory, that IS the buffer's storage
       * and must be preserved. The local path passes it to
       * newBufferWithBytesNoCopy; DXMT's ring allocator hands in
       * block.mapped_address and then keeps writing argument-buffer contents
       * and GPU addresses through that same pointer. Substituting our own
       * allocation here meant the app wrote one block while the flush uploaded
       * another, so the shaders read zeros -- draws executed and produced
       * nothing, which is exactly the flat clear with no geometry. */
      void *shadow = bi->memory.ptr;
      int owned = 0;
      if (cpu && !shadow) {
        const size_t want = bi->length ? (size_t)bi->length : 1;
        errno = 0;
        shadow = calloc(1, want);
        owned = 1;
        if (!shadow) {
          /* ml816: retry as a TAGGED anonymous mapping.
           *
           * ml803 already tried a plain mmap here and it failed exactly like the
           * calloc, so I removed it as useless. That was the wrong conclusion:
           * ml815 showed the deciding factor is the allocation TAG, not the
           * call. This kernel picks an address RANGE from the tag and size, so
           * an untagged mapping can be refused while heap-tagged space is free
           * -- 78 otherwise-fatal allocations were recovered that way, with none
           * failing.
           *
           * These are the buffers that matter for what actually renders: the 14
           * failures in the first successful run were 3MB-128MB CPU-visible
           * buffers, ~406MB in total. A shadowless buffer still serves explicit
           * updateContents, but anything the game writes through its MAPPED
           * pointer has nothing to diff, so those bytes never reach the host and
           * the geometry they describe draws as nothing.
           *
           * mmap is zero-filled, so this keeps calloc's semantics. owned==2
           * routes the release through munmap. */
          void *m = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                         VM_MAKE_TAG(VM_MEMORY_MALLOC), 0);
          if (m != MAP_FAILED) {
            shadow = m;
            owned = 2;
            static unsigned long tagn;
            if (++tagn <= 32)
              fprintf(stderr, "[wmt-remote] ml816 shadow RECOVERED by tagged mmap: %zu bytes "
                              "options 0x%x at %p (calloc failed) -- range placement, not "
                              "exhaustion\n", want, bi->options, m);
          }
        }
        if (!shadow) {
          /* ml802: say everything needed to act on this.
           *
           * The old line reported only the size and carried on, registering a
           * NULL shadow -- so the buffer got a valid host handle while every
           * write to it was discarded. An explicit updateContents now uploads
           * directly and does not need this allocation at all; what still breaks
           * is a buffer the app writes through its MAPPED pointer, because there
           * is then nothing to diff and nothing to send. That case is reported
             * above; this buffer never takes a mapped pointer (see ml804). */
          fprintf(stderr, "[wmt-remote] shadow alloc FAILED: %llu bytes, options 0x%x, "
                          "errno %d (%s). ml804: KEEPING the buffer registered shadowless -- "
                          "explicit updateContents upload directly; only writes through a "
                          "MAPPED pointer would be lost, and this allocation takes none\n",
                  (unsigned long long)bi->length, bi->options, errno, strerror(errno));
          /* ml804: KEEP the buffer. Register it shadowless and upload directly.
           *
           * ml803 released the host buffer and returned handle 0 on the theory
           * that a caller cannot use a buffer with a NULL CPU pointer. That was
           * wrong for the allocation that actually fails here. It is
           * ResourceInitializer's staging ring, which builds its allocator with
           * placed_buffer=false, never takes a mapped address, and fills the
           * resource entirely through explicit updateContents calls
           * (dxmt_resource_initializer.cpp:553). Those calls carry the source,
           * offset and length, so the direct-upload path serves them completely.
           *
           * Returning 0 made it strictly worse: DXMT ignored the failure and
           * issued updateContents against handle 0, which were then dropped, so
           * the resource never initialised and the frame never completed.
           *
           * A shadow is needed only for writes through a MAPPED pointer, which
           * this buffer never does. Registering with the real length and options
           * is what makes the direct-upload path's range check pass. */
          owned = 0;
        }
        bi->memory.ptr = shadow;   /* only when the caller supplied none */
      }
      wmtr_buf_add(r.handle, shadow, bi->length, bi->options,
                   cpu && shadow != NULL, owned);
    } else {
      bi->memory.ptr = NULL;
      bi->gpu_address = 0;
    }
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  if (wmt_stale_probe_on()) wmt_freed_set((uintptr_t)buffer, 0);   /* ml1156: a new buffer at a recycled address is alive */
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSamplerState(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTSamplerInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof(struct WMTSamplerInfo); w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTSamplerInfo));
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_NEW_SAMPLER_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) {
      params->ret = r.handle;
      ((struct WMTSamplerInfo *)params->info.ptr)->gpu_resource_id = r.value;
    } else {
      params->ret = 0;
    }
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;

  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newDepthStencilState(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTDepthStencilInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof(struct WMTDepthStencilInfo); w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTDepthStencilInfo));
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_NEW_DSS_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
  [desc release];
  return STATUS_SUCCESS;
}

/* iOS-Madeira 2026-05-13: iPhone GPUs (Apple7/Apple8 = A14/A15) lack native
 * BC (DXT/BPTC) texture support — that's Apple9 / Mac2 only. Games like
 * Thumper unconditionally CreateTexture2D(BC1) on their .pc cache files,
 * which then dies in Metal's MTLTextureDescriptor validateWithDevice.
 *
 * Tier-1 fix: remap BC formats to RGBA8 (or matching narrower format) so
 * the descriptor validates. The uploaded BC blob will be interpreted as
 * RGBA8 garbage — black/noise textures with correct geometry. Acceptable
 * for boot validation; tier-3 CPU decompression will follow once a first
 * frame renders. */
static enum WMTPixelFormat remap_unsupported_bc(enum WMTPixelFormat fmt, bool bc_supported) {
  if (bc_supported)
    return fmt;
  switch (fmt) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC7_RGBAUnorm:
    return WMTPixelFormatRGBA8Unorm;
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    return WMTPixelFormatRGBA8Unorm_sRGB;
  case WMTPixelFormatBC4_RUnorm:
    return WMTPixelFormatR8Unorm;
  case WMTPixelFormatBC4_RSnorm:
    return WMTPixelFormatR8Snorm;
  case WMTPixelFormatBC5_RGUnorm:
    return WMTPixelFormatRG8Unorm;
  case WMTPixelFormatBC5_RGSnorm:
    return WMTPixelFormatRG8Snorm;
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC6H_RGBUfloat:
    return WMTPixelFormatRGBA16Float;
  default:
    return fmt;
  }
}

/* Cached per-device check — set on first to_metal_pixel_format call.
 * Safe because Madeira runs a single MTLDevice. */
static int g_bc_supported_cached = -1;
static bool query_bc_support(void) {
  if (__builtin_expect(g_bc_supported_cached >= 0, 1))
    return g_bc_supported_cached != 0;
  /* In remote mode this must describe the HOST gpu. The local probe below
   * creates an MTLDevice directly -- which both answers for the wrong machine
   * and creates a local Metal object in a mode that is supposed to have none. */
  if (wmtr_enabled()) {
    g_bc_supported_cached = wmtr_host_bc();
    return g_bc_supported_cached != 0;
  }
  bool supported = false;
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev) {
      if ([dev respondsToSelector:@selector(supportsBCTextureCompression)])
        supported = [dev supportsBCTextureCompression];
      [dev release];
    }
  }
  g_bc_supported_cached = supported ? 1 : 0;
  /* iOS-Madeira 2026-05-18: one-shot log so we can see if A15+ supports BC
   * natively (would skip all the BC decoder work). Fires exactly once
   * per process — first call to to_metal_pixel_format. */
  dprintf(STDERR_FILENO,
          "[iOS DXMT] supportsBCTextureCompression = %s\n",
          supported ? "YES" : "NO");
  return supported;
}

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  enum WMTPixelFormat stripped = (enum WMTPixelFormat)ORIGINAL_FORMAT(format);
  {
    /* ml678: prove sRGB survives the BC remap instead of reading the switch and
     * assuming. A BC1_sRGB landing in a linear RGBA8 would wash every albedo
     * out -- one candidate for the flat look that remains after the BC6H fix. */
    enum WMTPixelFormat before = stripped;
    stripped = remap_unsupported_bc(stripped, query_bc_support());
    if (before != stripped) {
      static struct { unsigned s, d, n; } tbl[24];
      static unsigned tn;
      unsigned i;
      for (i = 0; i < tn; i++) if (tbl[i].s == before && tbl[i].d == stripped) break;
      if (i == tn && tn < 24) { tbl[tn].s = before; tbl[tn].d = stripped; tbl[tn].n = 0; tn++; }
      if (i < 24) {
        tbl[i].n++;
        if (tbl[i].n == 1 || (tbl[i].n % 512) == 0)
          fprintf(stderr, "[bc-remap] ml678 %u -> %u  n=%u  (sRGB-in=%d sRGB-out=%d)\n",
                  before, stripped, tbl[i].n,
                  (int)(before == WMTPixelFormatBC1_RGBA_sRGB || before == WMTPixelFormatBC2_RGBA_sRGB ||
                        before == WMTPixelFormatBC3_RGBA_sRGB || before == WMTPixelFormatBC7_RGBAUnorm_sRGB),
                  (int)(stripped == WMTPixelFormatRGBA8Unorm_sRGB));
      }
    }
  }
  return (MTLPixelFormat)stripped;
}

/* iOS-Madeira 2026-05-13: When BC textures are remapped to RGBA8 by
 * to_metal_pixel_format, the game continues to upload BC-compressed bytes
 * with BC row pitch. Metal's replaceRegion/copyFromBuffer validators will
 * abort if bytesPerRow < width * bytes_per_pixel for the (now RGBA8)
 * destination. This helper returns false when the upload would trip that
 * check, letting the call site skip rather than abort. The texture stays
 * zero/garbage — fine for boot validation. */
static bool format_bytes_per_pixel(MTLPixelFormat fmt, size_t *bpp_out) {
  switch (fmt) {
  case MTLPixelFormatA8Unorm:
  case MTLPixelFormatR8Unorm:
  case MTLPixelFormatR8Snorm:
  case MTLPixelFormatR8Uint:
  case MTLPixelFormatR8Sint:
  case MTLPixelFormatStencil8:
    *bpp_out = 1; return true;
  case MTLPixelFormatR16Unorm:
  case MTLPixelFormatR16Snorm:
  case MTLPixelFormatR16Uint:
  case MTLPixelFormatR16Sint:
  case MTLPixelFormatR16Float:
  case MTLPixelFormatRG8Unorm:
  case MTLPixelFormatRG8Snorm:
  case MTLPixelFormatRG8Uint:
  case MTLPixelFormatRG8Sint:
  case MTLPixelFormatDepth16Unorm:
    *bpp_out = 2; return true;
  case MTLPixelFormatRGBA8Unorm:
  case MTLPixelFormatRGBA8Unorm_sRGB:
  case MTLPixelFormatRGBA8Snorm:
  case MTLPixelFormatRGBA8Uint:
  case MTLPixelFormatRGBA8Sint:
  case MTLPixelFormatBGRA8Unorm:
  case MTLPixelFormatBGRA8Unorm_sRGB:
  case MTLPixelFormatRG16Unorm:
  case MTLPixelFormatRG16Snorm:
  case MTLPixelFormatRG16Uint:
  case MTLPixelFormatRG16Sint:
  case MTLPixelFormatRG16Float:
  case MTLPixelFormatR32Uint:
  case MTLPixelFormatR32Sint:
  case MTLPixelFormatR32Float:
  case MTLPixelFormatDepth32Float:
  case MTLPixelFormatRGB10A2Unorm:
  case MTLPixelFormatRGB10A2Uint:
  case MTLPixelFormatBGR10A2Unorm:
  case MTLPixelFormatRG11B10Float:
  case MTLPixelFormatRGB9E5Float:
    *bpp_out = 4; return true;
  case MTLPixelFormatRGBA16Unorm:
  case MTLPixelFormatRGBA16Snorm:
  case MTLPixelFormatRGBA16Uint:
  case MTLPixelFormatRGBA16Sint:
  case MTLPixelFormatRGBA16Float:
  case MTLPixelFormatRG32Uint:
  case MTLPixelFormatRG32Sint:
  case MTLPixelFormatRG32Float:
  case MTLPixelFormatDepth32Float_Stencil8:
    *bpp_out = 8; return true;
  case MTLPixelFormatRGBA32Uint:
  case MTLPixelFormatRGBA32Sint:
  case MTLPixelFormatRGBA32Float:
    *bpp_out = 16; return true;
  default:
    return false;
  }
}

static bool texture_upload_pitch_ok(id<MTLTexture> tex, size_t width, size_t bytes_per_row) {
  if (bytes_per_row == 0)
    return true;
  size_t bpp;
  if (!format_bytes_per_pixel([tex pixelFormat], &bpp))
    return true;
  return bytes_per_row >= width * bpp;
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->reserved = 0;
};

static NTSTATUS
_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  if (wmtr_enabled()) {
    struct WMTTextureInfo *ti = params->info.ptr;
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTTextureInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof *ti; w->extra_count = 0;
    memcpy(buf + sizeof *w, ti, sizeof *ti);
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_NEW_TEXTURE_FULL, buf, sizeof buf, &r, sizeof r, 0) == RM_OK && r.handle) {
      params->ret = r.handle;
      ti->gpu_resource_id = r.value;
    } else {
      params->ret = 0; ti->gpu_resource_id = 0;
      fprintf(stderr, "[wmt-remote] host could not create a %ux%u texture (format %u)\n",
              ti->width, ti->height, (unsigned)ti->pixel_format);
    }
    /* Mach-port texture sharing cannot work across machines. */
    ti->mach_port = 0;
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_buf_texture) + sizeof(struct WMTTextureInfo)];
    struct rm_buf_texture *a = (void *)buf;
    a->buffer = params->buffer; a->offset = params->offset;
    a->bytes_per_row = params->bytes_per_row;
    memcpy(buf + sizeof *a, info, sizeof *info);
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_BUFFER_NEW_TEXTURE, buf, sizeof buf, &r, sizeof r, 0) == RM_OK && r.handle) {
      params->ret = r.handle;
      info->gpu_resource_id = r.value;
    } else {
      params->ret = 0; info->gpu_resource_id = 0;
      fprintf(stderr, "[wmt-remote] host could not make a buffer-backed %ux%u texture\n",
              info->width, info->height);
    }
    info->mach_port = 0;
    return STATUS_SUCCESS;
  }
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  wmt_stale_check(buffer, "buffer newTexture view");   /* ml1156 */
  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  if (format & WMTPixelFormatRGB1Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatR001Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormat0R01Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleOne, (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleOne, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatGBARSwizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, (MTLTextureSwizzle)swizzle.a,
        (MTLTextureSwizzle)swizzle.r
    );
  }
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b,
      (MTLTextureSwizzle)swizzle.a
  );
}

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  if (wmtr_enabled()) {
    /* ml820: the swizzle was sent as 0 and the host built the view without
     * one, so every channel remap DXMT relies on (forced alpha, R001, GBAR,
     * DXGI format flags) was silently dropped. Send the four EFFECTIVE
     * channels exactly as the local path computes them. */
    MTLTextureSwizzleChannels sw = to_metal_swizzle(params->swizzle, params->format);
    uint32_t packed = (uint32_t)sw.red | ((uint32_t)sw.green << 8) |
                      ((uint32_t)sw.blue << 16) | ((uint32_t)sw.alpha << 24);
    struct rm_tex_view a = { params->texture, (uint32_t)params->format,
                             (uint32_t)params->texture_type,
                             params->level_start, params->level_count,
                             params->slice_start, params->slice_count, packed };
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_NEW_TEXTURE_VIEW, &a, sizeof a, &r, sizeof r, 0) == RM_OK) {
      params->ret = r.handle; params->gpu_resource_id = r.value;
    } else { params->ret = 0; params->gpu_resource_id = 0; }
    return STATUS_SUCCESS;
  }
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_MIN_LINEAR_ALIGN, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 256;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibrary(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  if (wmtr_enabled()) {
    /* DispatchData stays guest-local (it is a byte container, not an identity),
     * so the metallib BYTES are what travel. dispatch_data_create_map hands us
     * one contiguous view even when the data is a composite of regions. */
    const void *bytes = NULL; size_t len = 0;
    dispatch_data_t flat = dispatch_data_create_map((dispatch_data_t)params->data, &bytes, &len);
    params->ret_error = 0;
    params->ret_library = 0;
    if (flat && bytes && len && len <= RM_CHUNK_BYTES) {
      /* The host builds its OWN dispatch_data from these bytes, then the
       * library from that. Two round trips for three libraries is nothing. */
      uint8_t *msg = malloc(len);
      if (msg) {
        memcpy(msg, bytes, len);
        struct rm_ret_handle rd_;
        if (wmtr_call(RM_OP_DISPATCH_DATA, msg, (uint32_t)len, &rd_, sizeof rd_, 0) == RM_OK) {
          struct rm_arg_handle_u64 a = { params->device, rd_.handle };
          struct rm_ret_handle rl;
          if (wmtr_call(RM_OP_NEW_LIBRARY_DATA, &a, sizeof a, &rl, sizeof rl, 0) == RM_OK)
            params->ret_library = rl.handle;
          struct rm_arg_handle da = { rd_.handle };   /* the library holds its own ref */
          wmtr_call(RM_OP_RELEASE, &da, sizeof da, 0, 0, 0);
        }
        free(msg);
      }
    } else if (len > RM_CHUNK_BYTES) {
      fprintf(stderr, "[wmt-remote] metallib is %zu bytes, over the %u cap -- not sent\n",
              len, RM_CHUNK_BYTES);
    }
    if (flat) dispatch_release(flat);
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)err;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunction(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  if (wmtr_enabled()) {
    /* arg is a guest pointer to a C string; the NAME crosses, never the pointer. */
    const char *nm = (const char *)params->arg;
    size_t nlen = nm ? strlen(nm) : 0;
    params->ret = 0;
    if (nlen && nlen < 1024) {
      uint8_t buf[sizeof(struct rm_arg_handle) + 1024];
      struct rm_arg_handle *a = (void *)buf;
      a->handle = params->handle;
      memcpy(buf + sizeof *a, nm, nlen);
      struct rm_ret_handle r;
      if (wmtr_call(RM_OP_NEW_FUNCTION, buf, (uint32_t)(sizeof *a + nlen), &r, sizeof r, 0) == RM_OK)
        params->ret = r.handle;
      if (!params->ret)
        fprintf(stderr, "[wmt-remote] newFunction(\"%s\") returned nothing\n", nm);
    }
    return STATUS_SUCCESS;
  }
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSObject *)params->handle description];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTComputePipelineInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof(struct WMTComputePipelineInfo); w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTComputePipelineInfo));
    struct rm_ret_handle r;
    params->ret_error = 0;
    params->ret_pso = (wmtr_call(RM_OP_NEW_COMPUTE_PSO_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_BLIT_ENCODER, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    wmtr_pool_push(params->ret);                 /* ml820: autoreleased natively */
    wmtr_enc_note(params->ret, params->handle);  /* ml820: blit destinations read back per cmdbuf */
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle blitCommandEncoder];
  if (params->ret) ios_frame_pass(1, 0, 0, 0);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  if (wmtr_enabled()) {
    /* arg selects concurrent vs serial dispatch; it changes how Metal may
     * reorder the work, so it travels rather than being defaulted. */
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_COMPUTE_ENCODER, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    wmtr_pool_push(params->ret);   /* ml820 */
    if (!params->ret) fprintf(stderr, "[wmt-remote] host refused a compute encoder\n");
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle
      computeCommandEncoderWithDispatchType:params->arg ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial];
  if (params->ret) ios_frame_pass(2, 0, 0, 0);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  if (wmtr_enabled()) {
    /* The full pass: eight colour attachments with their own load/store
     * actions, levels, slices and resolve targets, plus depth and stencil.
     * The handles inside are already host handles, so they resolve there. */
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTRenderPassInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->handle; w->info_len = sizeof(struct WMTRenderPassInfo); w->extra_count = 0;
    memcpy(buf + sizeof *w, info, sizeof(struct WMTRenderPassInfo));
    struct rm_ret_handle r;
    params->ret = (wmtr_call(RM_OP_RENDER_ENCODER, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    wmtr_pool_push(params->ret);   /* ml820 */
    /* ml820: the GPU writes occlusion counts here; they must come back. */
    if (params->ret && info->visibility_buffer) wmtr_rb_add(params->handle, info->visibility_buffer);
    if (!params->ret) fprintf(stderr, "[wmt-remote] host refused a render encoder\n");
    return STATUS_SUCCESS;
  }
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;

  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor];

  /* Count realized native passes, including clears and the final present.
   * No new PE ABI or emulated hot-path counters are needed. */
  if (params->ret && ios_frame_stats_on) {
    unsigned loads = 0, stores = 0, clears = 0;
    for (unsigned i = 0; i < 8; ++i) if (info->colors[i].texture) {
      loads += info->colors[i].load_action == WMTLoadActionLoad;
      clears += info->colors[i].load_action == WMTLoadActionClear;
      stores += info->colors[i].store_action != WMTStoreActionDontCare;
    }
    if (info->depth.texture) {
      loads += info->depth.load_action == WMTLoadActionLoad;
      clears += info->depth.load_action == WMTLoadActionClear;
      stores += info->depth.store_action != WMTStoreActionDontCare;
    }
    if (info->stencil.texture) {
      loads += info->stencil.load_action == WMTLoadActionLoad;
      clears += info->stencil.load_action == WMTLoadActionClear;
      stores += info->stencil.store_action != WMTStoreActionDontCare;
    }
    ios_frame_pass(0, loads, stores, clears);
  }

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    wmtr_call(RM_OP_END_ENCODING, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  [(id<MTLCommandEncoder>)params->handle endEncoding];
  return STATUS_SUCCESS;
}

#ifndef DXMT_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

static NTSTATUS
_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  if (wmtr_enabled()) {
    /* The WHOLE descriptor crosses: eight colour attachments, blend factors,
     * write masks, depth/stencil formats, sample count, topology and
     * tessellation. A hand-picked subset is how this became a toy before. */
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTRenderPipelineInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof(struct WMTRenderPipelineInfo); w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTRenderPipelineInfo));
    struct rm_ret_handle r;
    params->ret_error = 0;
    params->ret_pso = (wmtr_call(RM_OP_NEW_RENDER_PSO_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newRenderPipelineStateVD(void *obj) {
  struct unixcall_mtldevice_newrenderpso_vd *params = obj;
  const struct WMTVertexDescriptorInfo *vdi = params->vd.ptr;
  if (wmtr_enabled()) {
    /* The WHOLE descriptor crosses: eight colour attachments, blend factors,
     * write masks, depth/stencil formats, sample count, topology and
     * tessellation. A hand-picked subset is how this became a toy before. */
    /* The vertex descriptor rides after the pipeline info; info_len says so. */
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTRenderPipelineInfo) + sizeof(struct WMTVertexDescriptorInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device;
    w->info_len = sizeof(struct WMTRenderPipelineInfo) + sizeof(struct WMTVertexDescriptorInfo);
    w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTRenderPipelineInfo));
    memcpy(buf + sizeof *w + sizeof(struct WMTRenderPipelineInfo), vdi, sizeof(struct WMTVertexDescriptorInfo));
    struct rm_ret_handle r;
    params->ret_error = 0;
    params->ret_pso = (wmtr_call(RM_OP_NEW_RENDER_PSO_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    return STATUS_SUCCESS;
  }
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;
  {
    MTLVertexDescriptor *vdesc = [[MTLVertexDescriptor alloc] init];
    for (unsigned i = 0; i < 31; i++) {
      if (vdi->attribute_mask & (1u << i)) {
        vdesc.attributes[i].format = (MTLVertexFormat)vdi->attributes[i].format;
        vdesc.attributes[i].offset = vdi->attributes[i].offset;
        vdesc.attributes[i].bufferIndex = vdi->attributes[i].buffer_index;
      }
      if (vdi->layout_mask & (1u << i)) {
        vdesc.layouts[i].stride = vdi->layouts[i].stride;
        vdesc.layouts[i].stepFunction = (MTLVertexStepFunction)vdi->layouts[i].step_function;
        /* ml905: Metal requires stepRate 0 for a constant-step layout; 1 otherwise when unset. */
        vdesc.layouts[i].stepRate = vdi->layouts[i].step_function == 0 ? 0 : (vdi->layouts[i].step_rate ? vdi->layouts[i].step_rate : 1);
      }
    }
    descriptor.vertexDescriptor = vdesc;
  }

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  if (wmtr_enabled()) {
    /* The whole descriptor crosses, as the render one does: object/mesh/fragment
     * functions, eight colour attachments, payload length and the immutable
     * buffer masks for all three stages. */
    const struct WMTMeshRenderPipelineInfo *mi = params->info.ptr;
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTMeshRenderPipelineInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof *mi; w->extra_count = 0;
    memcpy(buf + sizeof *w, mi, sizeof *mi);
    struct rm_ret_handle r;
    params->ret_error = 0;
    params->ret_pso = (wmtr_call(RM_OP_NEW_MESH_PSO_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    if (!params->ret_pso)
      fprintf(stderr, "[wmt-remote] host refused a mesh pipeline\n");
    return STATUS_SUCCESS;
  }
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  descriptor.payloadMemoryLength = info->payload_memory_length;

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)err;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

/* ---- ml760: shadow mode -------------------------------------------------
 *
 * Pack and validate every real batch, then throw the result away and render
 * locally as usual. The point is to exercise the packer against live traffic
 * where being wrong costs nothing, before anything depends on it.
 *
 * The check that matters is that packed counts equal census counts. If the
 * packer silently skips a command, the two diverge -- which is the failure a
 * remote replay would otherwise show as a subtly wrong frame on another
 * machine, with nothing pointing at the cause.
 */
#include "wmt_remote_pack.h"
#include "../../../../remote-metal/host/wmt_decode.h"

static int wmt_shadow_on = -1;
static unsigned long wmt_sh_ok, wmt_sh_packfail, wmt_sh_valfail, wmt_sh_records;
static unsigned long wmt_sh_max_recbytes, wmt_sh_max_sidebytes, wmt_sh_max_records;
static unsigned wmt_sh_last_packerr, wmt_sh_last_valerr, wmt_sh_last_opcode;

static void wmt_shadow_report(void) {
    if (wmt_shadow_on != 1) return;
    fprintf(stderr, "[shadow] ml760 packed=%lu packfail=%lu valfail=%lu records=%lu\n",
            wmt_sh_ok, wmt_sh_packfail, wmt_sh_valfail, wmt_sh_records);
    fprintf(stderr, "[shadow] max record-bytes=%lu sidecar-bytes=%lu records/batch=%lu\n",
            wmt_sh_max_recbytes, wmt_sh_max_sidebytes, wmt_sh_max_records);
    if (wmt_sh_packfail)
        fprintf(stderr, "[shadow] last pack failure: %s (opcode %u)\n",
                wmtw_pack_strerror((enum wmtw_pack_status)wmt_sh_last_packerr), wmt_sh_last_opcode);
    if (wmt_sh_valfail)
        fprintf(stderr, "[shadow] last validate failure: %s\n",
                wmtw_dec_strerror((enum wmtw_dec_status)wmt_sh_last_valerr));
}

static void wmt_shadow_batch(const struct wmtcmd_base *head) {
    if (__builtin_expect(wmt_shadow_on < 0, 0)) {
        const char *e = getenv("DXMT_SHADOW_PACK");
        wmt_shadow_on = (e && e[0] == '1') ? 1 : 0;
        if (wmt_shadow_on) fprintf(stderr, "[shadow] ml760 armed\n");
    }
    if (__builtin_expect(wmt_shadow_on != 1, 1)) return;

    /* Static: this runs per batch and must not allocate. Single-threaded use
     * is assumed here because it is a diagnostic, not a shipping path. */
    static uint8_t recbuf[WMTW_MAX_BATCH_BYTES];
    static uint8_t sidebuf[WMTW_MAX_SIDECAR_BYTES];
    static uint8_t payload[sizeof(struct wmtw_batch) + WMTW_MAX_BATCH_BYTES + WMTW_MAX_SIDECAR_BYTES];

    struct wmtw_packer p = { recbuf, sizeof recbuf, 0, sidebuf, sizeof sidebuf, 0, 0 };
    struct wmtw_pack_result pr;
    if (wmtw_pack_render(head, &p, &pr) != WMTW_PACK_OK) {
        wmt_sh_packfail++;
        wmt_sh_last_packerr = pr.status; wmt_sh_last_opcode = pr.opcode;
        return;
    }
    struct wmtw_batch *b = (void *)payload;
    b->magic = WMTW_BATCH_MAGIC; b->version = WMTW_VERSION; b->encoder_kind = 0;
    b->record_bytes = p.rec_len; b->record_count = p.count;
    b->sidecar_bytes = p.side_len; b->reserved = 0;
    memcpy(payload + sizeof *b, recbuf, p.rec_len);
    if (p.side_len) memcpy(payload + sizeof *b + p.rec_len, sidebuf, p.side_len);

    struct wmtw_dec_result dr; struct wmtw_view v;
    uint32_t plen = (uint32_t)(sizeof *b + p.rec_len + p.side_len);
    if (wmtw_validate_batch(payload, plen, &v, &dr) != WMTW_DEC_OK) {
        wmt_sh_valfail++; wmt_sh_last_valerr = dr.status;
        return;
    }
    wmt_sh_ok++;
    wmt_sh_records += p.count;
    if (p.rec_len  > wmt_sh_max_recbytes)  wmt_sh_max_recbytes  = p.rec_len;
    if (p.side_len > wmt_sh_max_sidebytes) wmt_sh_max_sidebytes = p.side_len;
    if (p.count    > wmt_sh_max_records)   wmt_sh_max_records   = p.count;
}

/* ---- ml758: wmtcmd census ----------------------------------------------
 *
 * Before wmtcmd_* lists can be serialised for the remote Metal transport, we
 * need to know which of the 59 command types a real workload actually emits,
 * and how large their sidecar data gets. Serialising all 59 on speculation
 * would be weeks of schema work for commands no title may ever issue.
 *
 * Counters only -- logging every command would change the timing of the thing
 * being measured. Enabled with DXMT_CMD_CENSUS=1; costs one predictable branch
 * per command otherwise.
 */
#define WMT_CENSUS_RENDER 40
#define WMT_CENSUS_COMPUTE 16
#define WMT_CENSUS_BLIT 12

static int wmt_census_on = -1;
static unsigned long wmt_c_render[WMT_CENSUS_RENDER];
static unsigned long wmt_c_compute[WMT_CENSUS_COMPUTE];
static unsigned long wmt_c_blit[WMT_CENSUS_BLIT];
static unsigned long wmt_batches_render, wmt_batches_compute, wmt_batches_blit;
static unsigned long wmt_records_total, wmt_records_max;
static unsigned long wmt_setbytes_calls, wmt_setbytes_bytes, wmt_setbytes_max;
static unsigned long wmt_viewport_calls, wmt_viewport_max;
static unsigned long wmt_scissor_calls, wmt_scissor_max;
/* first bounded opcode sequence, to show the SHAPE of a batch */
static unsigned short wmt_first_seq[64];
static unsigned wmt_first_len;
static int wmt_first_kind = -1;

/* Report PERIODICALLY, not only at exit. iOS apps are killed or backgrounded,
 * not cleanly exited, so an atexit-only summary never fires -- the first run
 * armed the census, counted commands, and printed nothing. */
static void wmt_census_report(void);

static void wmt_census_report(void) {
    if (wmt_census_on != 1) return;
    fprintf(stderr, "\n[cmd-census] ml758 batches render=%lu compute=%lu blit=%lu\n",
            wmt_batches_render, wmt_batches_compute, wmt_batches_blit);
    fprintf(stderr, "[cmd-census] records total=%lu max-per-batch=%lu\n",
            wmt_records_total, wmt_records_max);
    fprintf(stderr, "[cmd-census] setBytes calls=%lu bytes=%lu max=%lu\n",
            wmt_setbytes_calls, wmt_setbytes_bytes, wmt_setbytes_max);
    fprintf(stderr, "[cmd-census] viewports calls=%lu max-count=%lu | scissors calls=%lu max-count=%lu\n",
            wmt_viewport_calls, wmt_viewport_max, wmt_scissor_calls, wmt_scissor_max);
    for (int i = 0; i < WMT_CENSUS_RENDER; i++)
        if (wmt_c_render[i]) fprintf(stderr, "[cmd-census]   render[%2d] %lu\n", i, wmt_c_render[i]);
    for (int i = 0; i < WMT_CENSUS_COMPUTE; i++)
        if (wmt_c_compute[i]) fprintf(stderr, "[cmd-census]   compute[%2d] %lu\n", i, wmt_c_compute[i]);
    for (int i = 0; i < WMT_CENSUS_BLIT; i++)
        if (wmt_c_blit[i]) fprintf(stderr, "[cmd-census]   blit[%2d] %lu\n", i, wmt_c_blit[i]);
    wmt_shadow_report();
    if (wmt_first_len) {
        fprintf(stderr, "[cmd-census] first %s batch shape:", 
                wmt_first_kind == 0 ? "render" : wmt_first_kind == 1 ? "compute" : "blit");
        for (unsigned i = 0; i < wmt_first_len; i++) fprintf(stderr, " %u", wmt_first_seq[i]);
        fprintf(stderr, "\n");
    }
}

static void wmt_census_init(void) {
    const char *e = getenv("DXMT_CMD_CENSUS");
    wmt_census_on = (e && e[0] == '1') ? 1 : 0;
    if (wmt_census_on) { fprintf(stderr, "[cmd-census] ml758 armed\n"); atexit(wmt_census_report); }
}

/* kind: 0 render, 1 compute, 2 blit */
static inline void wmt_census_batch(const struct wmtcmd_base *head, int kind) {
    if (__builtin_expect(wmt_census_on < 0, 0)) wmt_census_init();
    if (__builtin_expect(wmt_census_on != 1, 1)) return;
    unsigned long n = 0;
    int capture = (wmt_first_len == 0);
    if (kind == 0) wmt_batches_render++; else if (kind == 1) wmt_batches_compute++; else wmt_batches_blit++;
    for (const struct wmtcmd_base *c = head; c; ) {
        unsigned t = c->type;
        if (kind == 0 && t < WMT_CENSUS_RENDER) wmt_c_render[t]++;
        else if (kind == 1 && t < WMT_CENSUS_COMPUTE) wmt_c_compute[t]++;
        else if (kind == 2 && t < WMT_CENSUS_BLIT) wmt_c_blit[t]++;
        if (capture && n < 64) { wmt_first_seq[n] = (unsigned short)t; wmt_first_kind = kind; }
        n++;
        c = (const struct wmtcmd_base *)c->next.ptr;
    }
    if (capture) wmt_first_len = (unsigned)(n < 64 ? n : 64);
    wmt_records_total += n;
    if (n > wmt_records_max) wmt_records_max = n;

    /* First batch, then every 512, then at exit. The first tells us the census
     * is live and shows a real batch shape immediately; the cadence keeps a
     * long-running app reporting without flooding the log. */
    {
        static unsigned long ticks;
        unsigned long t = ++ticks;
        if (t == 1 || (t & 0x1FF) == 0) wmt_census_report();
    }
}

static inline void wmt_census_sidecar_bytes(unsigned long len) {
    if (wmt_census_on != 1) return;
    wmt_setbytes_calls++; wmt_setbytes_bytes += len;
    if (len > wmt_setbytes_max) wmt_setbytes_max = len;
}
static inline void wmt_census_viewports(unsigned long n) {
    if (wmt_census_on != 1) return;
    wmt_viewport_calls++; if (n > wmt_viewport_max) wmt_viewport_max = n;
}
static inline void wmt_census_scissors(unsigned long n) {
    if (wmt_census_on != 1) return;
    wmt_scissor_calls++; if (n > wmt_scissor_max) wmt_scissor_max = n;
}

/* Size of one blit command struct, by type. Zero for anything unknown: a
 * guessed size would copy the wrong bytes onto the wire, and a blit that
 * silently copies garbage is far worse than one that reports itself missing. */
static uint32_t wmt_blit_cmd_size(unsigned type) {
    switch (type) {
    case WMTBlitCommandNop:                       return sizeof(struct wmtcmd_blit_nop);
    case WMTBlitCommandCopyFromBufferToBuffer:    return sizeof(struct wmtcmd_blit_copy_from_buffer_to_buffer);
    case WMTBlitCommandCopyFromBufferToTexture:   return sizeof(struct wmtcmd_blit_copy_from_buffer_to_texture);
    case WMTBlitCommandCopyFromTextureToBuffer:   return sizeof(struct wmtcmd_blit_copy_from_texture_to_buffer);
    case WMTBlitCommandCopyFromTextureToTexture:  return sizeof(struct wmtcmd_blit_copy_from_texture_to_texture);
    case WMTBlitCommandGenerateMipmaps:           return sizeof(struct wmtcmd_blit_generate_mipmaps);
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence:               return sizeof(struct wmtcmd_blit_fence_op);
    case WMTBlitCommandFillBuffer:                return sizeof(struct wmtcmd_blit_fillbuffer);
    default:                                      return 0;
    }
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  if (wmtr_enabled()) {
    /* Blit commands carry only handles and scalars, so each struct travels
     * verbatim as {type, size, bytes}; the guest-pointer `next` is not sent.
     * That keeps this in step with the descriptor path rather than inventing a
     * second serialiser to drift. */
    static _Thread_local uint8_t *bbuf;
    if (!bbuf) bbuf = malloc(WMTW_MAX_BATCH_BYTES);
    if (!bbuf) return STATUS_SUCCESS;
    struct rm_arg_handle *ha = (void *)bbuf;
    ha->handle = params->encoder;
    size_t off = sizeof *ha;
    for (const struct wmtcmd_base *c = next; c; c = c->next.ptr) {
      uint32_t sz = wmt_blit_cmd_size(c->type);
      if (!sz) {
        static unsigned told;
        if (told++ < 8)
          fprintf(stderr, "[wmt-remote] blit command type %u has no known size -- NOT sent, "
                          "its destination will be stale\n", (unsigned)c->type);
        continue;
      }
      if (off + 8 + sz > WMTW_MAX_BATCH_BYTES) break;
      /* ml820: a copy INTO a CPU-visible buffer is a readback the guest will
       * consume after completion (staging Map, query resolve). */
      if (c->type == WMTBlitCommandCopyFromTextureToBuffer)
        wmtr_rb_add(wmtr_enc_cmdbuf(params->encoder),
                    ((const struct wmtcmd_blit_copy_from_texture_to_buffer *)c)->dst);
      else if (c->type == WMTBlitCommandCopyFromBufferToBuffer)
        wmtr_rb_add(wmtr_enc_cmdbuf(params->encoder),
                    ((const struct wmtcmd_blit_copy_from_buffer_to_buffer *)c)->dst);
      *(uint32_t *)(bbuf + off) = c->type;
      *(uint32_t *)(bbuf + off + 4) = sz;
      memcpy(bbuf + off + 8, c, sz);
      off += 8 + sz;
    }
    if (off > sizeof *ha) {
      struct rm_ret_u64 rr;
      wmtr_call(RM_OP_BLIT_INTO, bbuf, (uint32_t)off, &rr, sizeof rr, 0);
    }
    return STATUS_SUCCESS;
  }
  wmt_census_batch(next, 2);
  id<MTLBlitCommandEncoder> encoder = (id<MTLBlitCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      wmt_stale_check(body->src, "blit copy src"); wmt_stale_check(body->dst, "blit copy dst");
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      id<MTLTexture> dst = (id<MTLTexture>)body->dst;
      /* iOS-Madeira: skip BC-pitch uploads to remapped RGBA8 textures. */
      if (!texture_upload_pitch_ok(dst, body->size.width, body->bytes_per_row))
        break;
      wmt_stale_check(body->src, "blit copy src"); wmt_stale_check(body->dst, "blit copy dst");
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      id<MTLTexture> src = (id<MTLTexture>)body->src;
      /* iOS-Madeira: skip BC-pitch readback to remapped RGBA8 textures.
       * ml1102: not for an ASPECT copy -- a depth or stencil readback of a
       * Depth32Float_Stencil8 texture is 4 or 1 bytes per pixel, not the
       * combined format's, and this check silently dropped every depth capture
       * (all-zero DepthTarget files in ph-rdr55). */
      if (!body->options && !texture_upload_pitch_ok(src, body->size.width, body->bytes_per_row))
        break;
      [encoder copyFromTexture:src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image
                           options:(MTLBlitOption)body->options];   /* ml1098 */
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      wmt_stale_check(body->buffer, "blit fill");
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

/* Size of one compute command struct, and how many inline bytes it carries.
 * Zero size means "unknown" -- refused by name rather than guessed, since a
 * wrong size copies the wrong bytes onto the wire. */
static uint32_t wmt_compute_cmd_size(unsigned type, const struct wmtcmd_base *c, uint32_t *inline_len) {
  *inline_len = 0;
  switch (type) {
  case WMTComputeCommandNop:               return sizeof(struct wmtcmd_compute_nop);
  case WMTComputeCommandDispatch:
  case WMTComputeCommandDispatchThreads:   return sizeof(struct wmtcmd_compute_dispatch);
  case WMTComputeCommandDispatchIndirect:  return sizeof(struct wmtcmd_compute_dispatch_indirect);
  case WMTComputeCommandSetPSO:            return sizeof(struct wmtcmd_compute_setpso);
  case WMTComputeCommandSetBuffer:         return sizeof(struct wmtcmd_compute_setbuffer);
  case WMTComputeCommandSetBufferOffset:   return sizeof(struct wmtcmd_compute_setbufferoffset);
  case WMTComputeCommandUseResource:       return sizeof(struct wmtcmd_compute_useresource);
  case WMTComputeCommandSetTexture:        return sizeof(struct wmtcmd_compute_settexture);
  case WMTComputeCommandWaitForFence:
  case WMTComputeCommandUpdateFence:       return sizeof(struct wmtcmd_compute_fence_op);
  case WMTComputeCommandSetBytes: {
    /* The only compute command holding a guest pointer: the bytes themselves
     * must travel, not the address. */
    const struct wmtcmd_compute_setbytes *b = (const void *)c;
    *inline_len = (uint32_t)b->length;
    return sizeof(struct wmtcmd_compute_setbytes);
  }
  default:                                 return 0;
  }
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  if (wmtr_enabled()) {
    static _Thread_local uint8_t *cbuf;
    if (!cbuf) cbuf = malloc(WMTW_MAX_BATCH_BYTES);
    if (!cbuf) return STATUS_SUCCESS;
    struct rm_arg_handle *ha = (void *)cbuf;
    ha->handle = params->encoder;
    size_t off = sizeof *ha;
    for (const struct wmtcmd_base *c = params->cmd_head.ptr; c; c = c->next.ptr) {
      uint32_t inl = 0;
      uint32_t sz = wmt_compute_cmd_size(c->type, c, &inl);
      if (!sz) {
        static unsigned told;
        if (told++ < 8)
          fprintf(stderr, "[wmt-remote] compute command type %u has no known size -- NOT sent\n",
                  (unsigned)c->type);
        continue;
      }
      if (off + 12 + sz + inl > WMTW_MAX_BATCH_BYTES) break;
      *(uint32_t *)(cbuf + off)     = c->type;
      *(uint32_t *)(cbuf + off + 4) = sz;
      *(uint32_t *)(cbuf + off + 8) = inl;
      memcpy(cbuf + off + 12, c, sz);
      if (inl) {
        const struct wmtcmd_compute_setbytes *b = (const void *)c;
        memcpy(cbuf + off + 12 + sz, b->bytes.ptr, inl);
      }
      off += 12 + sz + inl;
    }
    if (off > sizeof *ha) {
      struct rm_ret_u64 rr;
      wmtr_call(RM_OP_COMPUTE_INTO, cbuf, (uint32_t)off, &rr, sizeof rr, 0);
    }
    return STATUS_SUCCESS;
  }
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  wmt_census_batch(next, 1);
  id<MTLComputeCommandEncoder> encoder = (id<MTLComputeCommandEncoder>)params->encoder;
  MTLSize threadgroup_size = {0, 0, 0};
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                                threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      wmt_stale_check(body->buffer, "compute setBuffer");
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      wmt_stale_check(body->resource, "compute useResource");
      [encoder useResource:(id<MTLResource>)body->resource usage:(MTLResourceUsage)body->usage];
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      wmt_census_sidecar_bytes(body->length);
      [encoder setBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      [encoder setTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  if (wmtr_enabled()) {
    /* Pack with the SAME packer the wire tests exercise: a second walker here
     * would prove nothing about either. The batch replays into the existing
     * encoder, so Metal state survives across the dozen batches a frame sends. */
    /* Allocated on FIRST USE per thread, not as static thread-local storage.
     * As _Thread_local these two arrays were 1.1MB of TLS in EVERY thread the
     * process creates -- wine and FEX make many, none of which encode -- and
     * the run that introduced them died with a guest allocation returning NULL
     * and DXMT zeroing a structure through it. Only encoding threads pay now. */
    static _Thread_local uint8_t *rec, *side;
    if (!rec) {
      rec = malloc(WMTW_MAX_BATCH_BYTES);
      side = malloc(WMTW_MAX_SIDECAR_BYTES);
      if (!rec || !side) {
        free(rec); free(side); rec = side = NULL;
        static unsigned once;
        if (!once++) fprintf(stderr, "[wmt-remote] no packing buffers -- batches dropped\n");
        return STATUS_SUCCESS;
      }
    }
    struct wmtw_packer pk = { rec, WMTW_MAX_BATCH_BYTES, 0, side, WMTW_MAX_SIDECAR_BYTES, 0, 0 };
    struct wmtw_pack_result pr;
    wmtr_batches_packed++;
    if (wmtw_pack_render(next, &pk, &pr) != WMTW_PACK_OK) {
      /* Report each MISSING OPCODE once, not the first eight failures.
       * A pack failure drops the whole batch, so one unsupported command
       * removes every draw in it -- and reporting only the first occurrence
       * hides the rest behind it, costing a run per gap. */
      static unsigned seen[64]; static unsigned seen_n; static unsigned long dropped;
      dropped++;
      wmtr_batches_dropped++;
      /* ml817: the first-occurrence report above goes quiet after the last new
       * opcode, which left the first in-game log unable to say how many
       * batches were lost in total. Tally periodically as well. */
      if ((dropped % 500) == 0)
        fprintf(stderr, "[wmt-remote] pack: %lu batches dropped so far (last opcode %u, %s)\n",
                dropped, pr.opcode, wmtw_pack_strerror(pr.status));
      unsigned k = 0;
      for (; k < seen_n; k++) if (seen[k] == pr.opcode) break;
      if (k == seen_n && seen_n < 64) {
        seen[seen_n++] = pr.opcode;
        fprintf(stderr, "[wmt-remote] pack: UNSUPPORTED render opcode %u (%s) -- batch dropped, "
                        "%u distinct opcode(s) missing so far, %lu batches lost\n",
                pr.opcode, wmtw_pack_strerror(pr.status), seen_n, dropped);
      }
      return STATUS_SUCCESS;
    }
    uint32_t total = (uint32_t)(sizeof(struct rm_arg_handle) + sizeof(struct wmtw_batch)
                                + pk.rec_len + pk.side_len);
    uint8_t *msg = malloc(total);
    if (msg) {
      struct rm_arg_handle *a = (void *)msg;
      a->handle = params->encoder;
      struct wmtw_batch *b = (void *)(msg + sizeof *a);
      b->magic = WMTW_BATCH_MAGIC; b->version = WMTW_VERSION; b->encoder_kind = 0;
      b->record_bytes = pk.rec_len; b->record_count = pk.count;
      b->sidecar_bytes = pk.side_len; b->reserved = 0;
      memcpy((uint8_t *)(b + 1), rec, pk.rec_len);
      memcpy((uint8_t *)(b + 1) + pk.rec_len, side, pk.side_len);
      struct rm_ret_u64 rr;
      if (wmtr_call(RM_OP_ENCODE_INTO, msg, total, &rr, sizeof rr, 0) != RM_OK) {
        static unsigned bad;
        if (bad++ < 8) fprintf(stderr, "[wmt-remote] host rejected a packed batch\n");
      }
      free(msg);
    }
    return STATUS_SUCCESS;
  }
  /* Shadow BEFORE census: the census report is emitted from inside
   * wmt_census_batch, so with the old order it had counted the current batch
   * while shadow had not, and the two totals differed by exactly one batch
   * forever. Ordering it this way makes "packed == batches" an exact equality
   * that either holds or reveals a real skip. */
  wmt_shadow_batch(next);
  wmt_census_batch(next, 0);
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      wmt_stale_check(body->resource, "render useResource");
      [encoder useResource:(id<MTLResource>)body->resource
                     usage:(MTLResourceUsage)body->usage
                    stages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      wmt_stale_check(body->buffer, "render setVertexBuffer");
      [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      wmt_stale_check(body->buffer, "render setFragmentBuffer");
      [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      wmt_stale_check(body->buffer, "render setMeshBuffer");
      [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      wmt_stale_check(body->buffer, "render setObjectBuffer");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      wmt_census_sidecar_bytes(body->length);
      [encoder setFragmentBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setFragmentTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      wmt_census_viewports(body->viewport_count);
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      wmt_census_scissors(body->rect_count);
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      atomic_fetch_add_explicit(&g_madeira_draw_calls, 1, memory_order_relaxed);
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      atomic_fetch_add_explicit(&g_madeira_draw_calls, 1, memory_order_relaxed);
      wmt_stale_check(body->index_buffer, "draw index buffer");
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      atomic_fetch_add_explicit(&g_madeira_draw_calls, 1, memory_order_relaxed);
      wmt_stale_check(body->indirect_args_buffer, "draw indirect args");
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
          indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      atomic_fetch_add_explicit(&g_madeira_draw_calls, 1, memory_order_relaxed);
      wmt_stale_check(body->index_buffer, "draw index buffer");
      wmt_stale_check(body->indirect_args_buffer, "draw indirect args");
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                      indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                indirectBufferOffset:body->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope
                          afterStages:(MTLRenderStages)body->stages_after
                         beforeStages:(MTLRenderStages)body->stages_before];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      wmt_stale_check(body->index_buffer, "gs index buffer");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      wmt_stale_check(body->indirect_args_buffer, "gs indirect args");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      wmt_stale_check(body->index_buffer, "gs index buffer");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      wmt_stale_check(body->indirect_args_buffer, "gs indirect args");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      wmt_stale_check(body->index_buffer, "gs index buffer");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      wmt_stale_check(body->indirect_args_buffer, "gs indirect args");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      wmt_stale_check(body->index_buffer, "gs index buffer");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      wmt_stale_check(body->indirect_args_buffer, "gs indirect args");
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    params->ret = wmtr_tex_dim(params->handle, 'w');
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    params->ret = wmtr_tex_dim(params->handle, 'h');
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    /* Mip count feeds subresource and format decisions, so a wrong answer is
     * not cosmetic. */
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_TEXTURE_MIPLEVELS, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 1;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  if (wmtr_enabled()) {
    uint64_t rows = params->size.height ? params->size.height : 1;
    uint64_t depth = params->size.depth ? params->size.depth : 1;
    uint64_t bytes = params->bytes_per_row * rows * depth;
    if (bytes && bytes <= RM_CHUNK_BYTES - sizeof(struct rm_tex_replace) && params->data.ptr) {
      uint8_t *msg = malloc(sizeof(struct rm_tex_replace) + bytes);
      if (msg) {
        struct rm_tex_replace *a = (void *)msg;
        a->texture = params->texture;
        a->x = params->origin.x; a->y = params->origin.y; a->z = params->origin.z;
        a->w = params->size.width; a->h = params->size.height; a->d = params->size.depth;
        a->level = (uint32_t)params->level; a->slice = (uint32_t)params->slice;
        a->bytes_per_row = (uint32_t)params->bytes_per_row;
        a->bytes_per_image = (uint32_t)params->bytes_per_image;
        memcpy(msg + sizeof *a, params->data.ptr, bytes);
        wmtr_call(RM_OP_TEXTURE_REPLACE, msg, (uint32_t)(sizeof *a + bytes), 0, 0, 0);
        free(msg);
      }
    } else if (bytes > RM_CHUNK_BYTES - sizeof(struct rm_tex_replace)) {
      fprintf(stderr, "[wmt-remote] texture upload of %llu bytes exceeds the message cap"
                      " -- NOT sent\n", (unsigned long long)bytes);
    }
    return STATUS_SUCCESS;
  }
  id<MTLTexture> tex = (id<MTLTexture>)params->texture;
  /* iOS-Madeira: skip BC-pitch uploads to remapped RGBA8 textures. */
  if (!texture_upload_pitch_ok(tex, params->size.width, params->bytes_per_row))
    return STATUS_SUCCESS;
  [tex replaceRegion:MTLRegionMake3D(
                         params->origin.x, params->origin.y, params->origin.z,
                         params->size.width, params->size.height, params->size.depth
                     )
         mipmapLevel:params->level
               slice:params->slice
           withBytes:params->data.ptr
         bytesPerRow:params->bytes_per_row
       bytesPerImage:params->bytes_per_image];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
#if !TARGET_OS_IOS
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
#else
  (void)params; /* iOS unified memory — no range invalidation needed. */
#endif
  return STATUS_SUCCESS;
}

/* iOS-Madeira 2026-05-13: track Present cadence so we can tell whether the
 * game's render loop is alive (continuous Presents → splash sustained via
 * redraw) or wedged on first frame. Prints once per ~60 frames at ~1Hz. */
static _Atomic uint64_t g_madeira_present_count = 0;
/* iOS-Madeira 2026-05-22: draw-call counter, sampled+reset on each present
 * log line. Tells us if the game is issuing draws between Presents or
 * presenting empty frames. Bumped in WMTRenderCommandDraw{,Indexed,Indirect,
 * IndexedIndirect} cases of the render-command processor.
 * Forward-declared near top of file; definition lives here. */

static inline void madeira_log_present_cadence(const char *path, double after) {
  uint64_t n = atomic_fetch_add_explicit(&g_madeira_present_count, 1, memory_order_relaxed) + 1;
  /* iOS-Madeira quiet mode: counter always ticks (FPS overlay reads it);
   * only the log line is suppressed. At RAW rates this line fires 100+
   * times/s — real I/O + heat. */
  {
    static int quiet = -1;
    if (quiet < 0) quiet = getenv("MADEIRA_QUIET") != NULL;
    if (quiet) return;
  }
  /* 2026-07-03: every-16 cadence (was 60) + monotonic timestamp + the
   * `after` min-duration arg — measures the black-phase ~1 FPS pacing
   * directly from the log (game-phase log lines carry no timestamps under
   * the WINEDEBUG perf default). */
  if (n == 1 || (n % 16) == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* Sample + reset draw counter delta since last log line. */
    uint64_t cur_draws = atomic_load_explicit(&g_madeira_draw_calls, memory_order_relaxed);
    uint64_t last_draws = atomic_exchange_explicit(&g_madeira_draw_calls_at_last_log, cur_draws, memory_order_relaxed);
    uint64_t draws_since_last = cur_draws - last_draws;
    /* 2026-07-03 Mach-exception storm probe: the PC-sampling profiler put
     * ~25% of game-thread time in the exception-resume trampoline. This
     * counter (signal_arm64_ios.c, same binary) exposes exceptions/present.
     * Thousands per present = steady-state trap storm (suspect: guest CALL
     * pushes into trap-mode-protected callret memory at ~70us each). */
    extern volatile int ios_exc_msg_count;
    static int last_exc_count = 0;
    int cur_exc = ios_exc_msg_count;
    int exc_delta = cur_exc - last_exc_count;
    last_exc_count = cur_exc;
    /* iOS-Madeira 2026-07-05: frame anatomy for the locked-60 push —
     * game-thread server_wait wall time + wait/timeout/request counts
     * (server_ios.c, same binary). Deltas cover the 16 presents since
     * the last line. Answers: is the last ~1.5ms/frame server-request
     * WORK or wait-wake LATENCY? */
    extern volatile long long ios_srv_wait_us, ios_srv_wait_req_us;
    extern volatile int ios_srv_wait_count, ios_srv_wait_timeouts, ios_srv_req_count;
    static long long last_wait_us, last_req_us; static int last_wc, last_wt, last_rq;
    long long cur_wait_us = ios_srv_wait_us, cur_req_us = ios_srv_wait_req_us;
    int cur_wc = ios_srv_wait_count, cur_wt = ios_srv_wait_timeouts, cur_rq = ios_srv_req_count;
    dprintf(STDERR_FILENO, "[iOS DXMT] Present #%llu t=%llu.%03lu after=%.4f (draws_since_last=%llu total_draws=%llu machexc_delta=%d srvw=%d/%d w_ms=%.1f wreq_ms=%.1f reqs=%d) [%s]\n",
            (unsigned long long)n,
            (unsigned long long)ts.tv_sec, (unsigned long)(ts.tv_nsec / 1000000),
            after,
            (unsigned long long)draws_since_last,
            (unsigned long long)cur_draws,
            exc_delta,
            cur_wc - last_wc, cur_wt - last_wt,
            (cur_wait_us - last_wait_us) / 1000.0,
            (cur_req_us - last_req_us) / 1000.0,
            cur_rq - last_rq,
            path);
    last_wait_us = cur_wait_us; last_req_us = cur_req_us;
    last_wc = cur_wc; last_wt = cur_wt; last_rq = cur_rq;
  }
}

/* iOS-Madeira 2026-05-18: exposed for SwiftUI FPS overlay. Reads the
 * atomic counter on the calling thread (typically a 100ms Swift Timer). */
uint64_t madeira_get_present_count(void) {
  return atomic_load_explicit(&g_madeira_present_count, memory_order_relaxed);
}

/* NOTE (2026-07-03): a presented-handler probe lived here during the
 * visibility-stall investigation. Removed — presentedTime reported 0.000
 * even for frames provably on glass (the splash), so it carries no signal
 * for this layer. See project memory for the full postmortem. */

/* iOS-Madeira 2026-07-05: runtime present-pacing mode, read per present
 * (live-flippable from the Swift UI):
 *   1 = LOCKED (default): afterMinimumDuration(1/60) — exact 60.
 *   0 = MAX: present every frame, free-run to the display refresh
 *       (120Hz ProMotion with CADisableMinimumFrameDurationOnPhone +
 *       the app-side CADisplayLink intent; thermal governor may cap 60).
 *   2 = RAW: mailbox/frame-skip — the game runs UNTHROTTLED; a real
 *       drawable present is submitted at most every 8ms, other frames
 *       release their drawable unpresented so the pool never blocks.
 *       Measures raw stack throughput independent of the panel; the
 *       present COUNTER counts every game present (incl. skipped) so
 *       the FPS overlay reads true game rate.
 *   3 = LOCKED30 (2026-09-15, device feedback): same mechanism as mode 1,
 *       afterMinimumDuration(1/30) — exact 30. A real cap on THIS present
 *       path, not merely a CADisplayLink hint: the drawable is genuinely
 *       held until the next 1/30s boundary, same as mode 1 is at 1/60s. */
static volatile int g_madeira_vsync_mode = 1;

/* ml1050: what the PANEL can do, published from Swift (only UIKit knows it).
 * Nothing branches on these -- they are printed by the [frame] line, because
 * the panel's refresh IS the grid every presentation snaps to and a log that
 * does not state it cannot tell "the pipeline is slow" from "the pipeline
 * missed a vblank by 3 ms". */
static volatile int g_madeira_panel_hz = 0;
static volatile int g_madeira_intent_hz = 0;
void madeira_set_display_max_fps(int panel_hz, int intent_hz) {
  g_madeira_panel_hz = panel_hz;
  g_madeira_intent_hz = intent_hz;
  ios_frame_note_display(panel_hz, intent_hz, g_madeira_vsync_mode);
  dprintf(STDERR_FILENO, "[iOS DXMT] panel_max=%dHz display_intent=%dHz vsync_mode=%d\n",
          panel_hz, intent_hz, g_madeira_vsync_mode);
}

void madeira_set_vsync_locked(int mode) {
  g_madeira_vsync_mode = mode;
  ios_frame_note_display(g_madeira_panel_hz, g_madeira_intent_hz, mode);
  dprintf(STDERR_FILENO, "[iOS DXMT] vsync_mode=%d (1=locked60 0=max 2=raw 3=locked30)\n", mode);
}
int madeira_get_vsync_locked(void) { return g_madeira_vsync_mode; }

/* ml1050: THE PRESENT LIMITER, AND WHY IT IS NOT A SLEEP LADDER.
 *
 * Until now the 60 and 30 caps were expressed ONLY as
 * presentDrawable:afterMinimumDuration:, which is a constraint on when the
 * DISPLAY may show the frame -- it never holds the producer, and it snaps to
 * the panel's vblank grid. That is the right primitive for a cap and the
 * wrong one for pacing, and it is why the 60 cap quantises a 17-25 ms frame
 * to 33.3 ms on a 60 Hz grid (see FPSOverlay.swift's ProMotionIntent).
 *
 * MADEIRA_PRESENT_LIMITER=1 adds the other half, opt-in, for A/B against the
 * min-duration behaviour: hold the PRODUCER to an exact deadline with
 * mach_wait_until and present with no minimum duration at all. mach_wait_until
 * sleeps to an absolute deadline on the same timebase the scheduler uses, so
 * it does not accumulate the per-iteration error a relative nanosleep ladder
 * does and it does not quantise to a timer tick -- the failure mode the brief
 * calls out, where a limiter sleeping in 16.6 ms quanta turns 45 fps into 30.
 * The deadline is advanced from the PREVIOUS deadline rather than from "now",
 * so a frame that ran long is not paid for twice, and it is re-based whenever
 * it falls more than one period behind so a stall cannot bank credit.
 *
 * Default OFF: the min-duration path is what every previous measurement was
 * taken against, and the [frame] line has to show the limiter's own sleep
 * (`limiter=`) before it becomes the default. */
static int madeira_present_limiter_enabled(void) {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("MADEIRA_PRESENT_LIMITER");
    on = (e && *e && *e != '0') ? 1 : 0;
    dprintf(STDERR_FILENO, "[iOS DXMT] ml1050 present limiter %s "
            "(MADEIRA_PRESENT_LIMITER=1 paces the producer with mach_wait_until "
            "instead of afterMinimumDuration)\n", on ? "ON" : "OFF");
  }
  return on;
}

/* Returns the nanoseconds actually slept. */
static unsigned long long madeira_present_limit_to(double period_s) {
  static mach_timebase_info_data_t tb;
  static uint64_t deadline_abs;      /* encode thread only -- one writer */
  uint64_t now_abs, period_abs, slept_abs;

  if (!tb.denom) mach_timebase_info(&tb);
  period_abs = (uint64_t)(period_s * 1e9 * (double)tb.denom / (double)tb.numer);
  now_abs = mach_absolute_time();
  if (!deadline_abs || now_abs > deadline_abs + period_abs) {
    /* First frame, or we fell more than a whole period behind: re-base rather
     * than fire a burst of catch-up presents. */
    deadline_abs = now_abs + period_abs;
    return 0;
  }
  if (now_abs >= deadline_abs) {
    deadline_abs += period_abs;      /* late: no sleep, no accumulated debt */
    return 0;
  }
  slept_abs = deadline_abs - now_abs;
  mach_wait_until(deadline_abs);
  deadline_abs += period_abs;
  return (unsigned long long)slept_abs * tb.numer / tb.denom;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  if (wmtr_enabled()) {
    /* Presenting hands the drawable back to the host layer; without this the
     * pool leaks one a frame and acquisition eventually blocks forever. */
    struct rm_present a = { params->handle, params->arg };
    wmtr_call(RM_OP_PRESENT_DRAWABLE, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  int mode = g_madeira_vsync_mode;
  /* ml1050: this call IS the frame boundary on the encode thread. */
  ios_frame_encode_present(0);
  if ((mode == 1 || mode == 3) && madeira_present_limiter_enabled()) {
    /* Opt-in precise pacing: hold the producer to the deadline and then ask
     * for the next vblank with no minimum duration, so the cap is the
     * limiter's and the only quantisation left is the panel's own. */
    ios_frame_limiter(madeira_present_limit_to(mode == 3 ? (1.0 / 30.0) : (1.0 / 60.0)));
    madeira_log_present_cadence(mode == 3 ? "presentLimited30" : "presentLimited60", 0.0);
    [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  } else if (mode == 1) {
    madeira_log_present_cadence("presentDrawable60", 0.0);
    [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg
                                     afterMinimumDuration:(1.0 / 60.0)];
  } else if (mode == 3) {
    /* Device feedback (2026-09-15): a real 30fps cap, exact same mechanism
     * as the mode-1 60fps cap just above -- afterMinimumDuration holds the
     * drawable on THIS present path, so this is a genuine cap independent
     * of whatever CADisplayLink/ProMotionIntent is doing on the Swift side. */
    madeira_log_present_cadence("presentDrawable30", 0.0);
    [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg
                                     afterMinimumDuration:(1.0 / 30.0)];
  } else if (mode == 2) {
    /* Frame-skip gating lives in _MetalLayer_nextDrawable (nil return);
     * only real, ≥18ms-spaced frames reach here. */
    madeira_log_present_cadence("presentRaw", 0.0);
    [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  } else {
    madeira_log_present_cadence("presentDrawable", 0.0);
    [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  madeira_log_present_cadence("presentDrawableAfterMinDuration", params->arg1);
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg0
                                   afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_SUPPORTS_FAMILY, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle supportsFamily:(MTLGPUFamily)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_SUPPORTS_BC, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_SUPPORTS_SAMPLE_COUNT, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_DEVICE_UNIFIED_MEM, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
_MTLDevice_newTemporalScaler(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:(id<MTLDevice>)params->device];

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSpatialScaler(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:(id<MTLDevice>)params->device];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)(objc_lookUpClass("_CADeveloperHUDProperties"), @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  params->ret = ((bool (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(addLabel:after:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    /* Already known from the nextDrawable reply -- no round trip needed. */
    params->ret = wmtr_pair_texture(params->handle);
    if (!params->ret)
      fprintf(stderr, "[wmt-remote] texture asked for an unknown drawable 0x%llx\n",
              (unsigned long long)params->handle);
    return STATUS_SUCCESS;
  }
  params->ret = (obj_handle_t)[(id<CAMetalDrawable>)params->handle texture];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_ret_drawable r;
    params->ret = 0;
    if (wmtr_call(RM_OP_NEXT_DRAWABLE, 0, 0, &r, sizeof r, 0) == RM_OK && r.drawable) {
      wmtr_pair_record(r.drawable, r.texture);
      params->ret = r.drawable;
    } else {
      /* Silent before. A frame with no drawable is drawn nowhere and shows as
       * a black flash, so it has to be counted rather than inferred. */
      static unsigned long missed;
      if (++missed <= 4 || (missed % 256) == 0)
        fprintf(stderr, "[wmt-remote] no drawable from the host (%lu times) -- this frame "
                        "will be blank\n", missed);
    }
    return STATUS_SUCCESS;
  }
  /* iOS-Madeira 2026-07-05 RAW mode (mode 2): the mailbox skip lives HERE,
   * before any drawable is consumed. First attempt gated at the
   * presentDrawable thunk — too late: every game frame had already
   * acquired a drawable, and PRESENTED drawables are held until vsync,
   * so a hot-capped 60Hz panel exhausted the 3-drawable pool and
   * throttled "unlocked" RAW right back to 60 (observed 02:35 run).
   * Returning nil here = no drawable touched, no block anywhere; the
   * PE side (Presenter::encodeCommands / flushCommands) skips the blit
   * and present on nil. Real acquires are spaced ≥18ms so presented
   * drawables can never exhaust the pool even on a 60Hz-capped panel.
   * Skipped frames tick the present counter so the FPS overlay shows
   * TRUE game rate. */
  if (g_madeira_vsync_mode == 2) {
    static struct timespec last_acquire; /* encode-thread only */
    struct timespec now;
    double since;
    clock_gettime(CLOCK_MONOTONIC, &now);
    since = (now.tv_sec - last_acquire.tv_sec) + (now.tv_nsec - last_acquire.tv_nsec) / 1e9;
    if (since < 0.018) {
      params->ret = 0;
      ios_frame_encode_present(1);   /* ml1050: a frame that reaches no glass */
      madeira_log_present_cadence("presentSkipped", 0.0);
      return STATUS_SUCCESS;
    }
    last_acquire = now;
  }
  /* 2026-07-03: measure blocking time. When queued presentations never
   * complete (render server not compositing the layer), all 3 pool
   * drawables stay owned by the presentation queue and this call blocks
   * its full 1s timeout — the observed ~1 present/s black-screen pacing. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle nextDrawable];
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  /* ml1050: THE number that separates "the pipeline is slow" from "the display
   * is holding the producer". A drawable pool that is never exhausted reports
   * near zero here; a producer parked waiting for the compositor to hand a
   * drawable back reports the remainder of a refresh interval, every frame. */
  ios_frame_drawable_wait((unsigned long long)((t1.tv_sec - t0.tv_sec) * 1000000000ll
                                               + (t1.tv_nsec - t0.tv_nsec)));
  static _Atomic uint64_t nd_total = 0, nd_slow = 0;
  uint64_t n = atomic_fetch_add_explicit(&nd_total, 1, memory_order_relaxed) + 1;
  if (ms > 50.0) {
    uint64_t s = atomic_fetch_add_explicit(&nd_slow, 1, memory_order_relaxed) + 1;
    if (s <= 16 || (s % 64) == 0)
      dprintf(STDERR_FILENO, "[iOS DXMT] nextDrawable #%llu BLOCKED %.0fms (nil=%d slow_total=%llu)\n",
              (unsigned long long)n, ms, params->ret == 0, (unsigned long long)s);
  } else if (n <= 8) {
    dprintf(STDERR_FILENO, "[iOS DXMT] nextDrawable #%llu took %.2fms\n", (unsigned long long)n, ms);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTLayerProps)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->handle; w->info_len = sizeof(struct WMTLayerProps); w->extra_count = 0;
    memcpy(buf + sizeof *w, params->arg.ptr, sizeof(struct WMTLayerProps));
    wmtr_call(RM_OP_LAYER_SET_PROPS, buf, sizeof buf, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  const struct WMTLayerProps *props = params->arg.ptr;
  execute_on_main(^{
    layer.device = (id<MTLDevice>)props->device;
    layer.opaque = props->opaque;
    layer.framebufferOnly = props->framebuffer_only;
    layer.contentsScale = props->contents_scale;
#if !TARGET_OS_IOS
    layer.displaySyncEnabled = props->display_sync_enabled;
#endif
    layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props->pixel_format);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  if (wmtr_enabled()) {
    struct WMTLayerProps got;
    if (wmtr_call(RM_OP_LAYER_GET_PROPS, 0, 0, &got, sizeof got, 0) == RM_OK) {
      /* The device stays the guest's own handle: the host's layer.device is a
       * host object and returning it would put an untagged id in guest hands. */
      obj_handle_t keep = ((struct WMTLayerProps *)params->arg.ptr)->device;
      memcpy(params->arg.ptr, &got, sizeof got);
      ((struct WMTLayerProps *)params->arg.ptr)->device = keep;
    }
    return STATUS_SUCCESS;
  }
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
#if TARGET_OS_IOS
  /* ml1140: WSI window extents are Windows pixels, not UIKit points. Applying
   * the phone's 3x scale again made a 720p present render into a 4K drawable.
   * Core Animation already maps that drawable onto the host view in points.
   * Keep the correction at the iOS boundary so macOS Retina is unchanged. */
  {
    const char *e = getenv("MADEIRA_PRESENT_PIXELS");
    int enabled = !e || strcmp(e, "0");
    static _Atomic int announced;
    if (!atomic_exchange_explicit(&announced, 1, memory_order_relaxed))
      fprintf(stderr, "[present-size] ml1140 guest-pixels=%d host-scale=%.1f (MADEIRA_PRESENT_PIXELS=0 reverts)\n",
              enabled, (double)layer.contentsScale);
    if (enabled) props->contents_scale = 1.0;
  }
  props->display_sync_enabled = true; /* iOS always syncs to display refresh. */
#else
  props->display_sync_enabled = layer.displaySyncEnabled;
#endif
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;
  if (wmtr_enabled()) {
    /* The HWND names a window in the GUEST's windowing system and is not sent.
     * The surface that gets presented is the host's own layer, which is what
     * the guest actually needs in order to acquire drawables. */
    struct rm_arg_handle a = { params->device };
    struct rm_ret_view r;
    if (wmtr_call(RM_OP_CREATE_VIEW, &a, sizeof a, &r, sizeof r, 0) == RM_OK) {
      params->ret_view = r.view;
      params->ret_layer = r.layer;
    } else {
      params->ret_view = 0; params->ret_layer = 0;
      fprintf(stderr, "[wmt-remote] host refused to bind a view\n");
    }
    return STATUS_SUCCESS;
  }

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    macdrv_metal_view view =
        pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
    params->ret_view = (obj_handle_t)view;
    if (view) {
      params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
    }
    pfn_release_win_data(win_data);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    wmtr_call(RM_OP_RELEASE_VIEW, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret =
      SM50Initialize(params->bytecode, params->bytecode_size, params->shader, params->reflection, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;

  params->ret = SM50Compile(params->shader, params->args, params->func_name, params->bitcode, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, params->hull_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, params->domain_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, params->vertex_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, params->geometry_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  if (wmtr_enabled()) {
    /* The label is a GUEST-LOCAL NSString (NSString_* stays local), so its
     * bytes travel; the encoder handle is already a host handle. */
    const char *lbl = params->arg ? [(NSString *)params->arg UTF8String] : NULL;
    size_t n = lbl ? strlen(lbl) : 0;
    if (n && n < 512) {
      uint8_t buf[sizeof(struct rm_arg_handle) + 512];
      struct rm_arg_handle *a = (void *)buf;
      a->handle = params->handle;
      memcpy(buf + sizeof *a, lbl, n);
      wmtr_call(RM_OP_SET_LABEL, buf, (uint32_t)(sizeof *a + n), 0, 0, 0);
    }
    return STATUS_SUCCESS;
  }
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  /* Skipped on iOS below because the selector raises there -- but in remote
   * mode the device lives on macOS, where it is both valid and useful. */
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    wmtr_call(RM_OP_DEVICE_SET_MAXCC, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
#if !TARGET_OS_IOS
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
#else
  /* setShouldMaximizeConcurrentCompilation: is macOS-only — iOS MTLDevice
   * raises an ObjC exception if we call it. Skip. */
  (void)params;
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

/* MADEIRA, WOW64_DESIGN.md sections 2 and 7 ("shifted guest window").
 *
 * On iOS a 32-bit pseudo-process cannot own the low 4 GB: XNU forces a 4 GB
 * __PAGEZERO, so nothing is ever mapped below 4 GB.  Instead each 32-bit
 * pseudo-process gets a guest window -- one reserved host range [B, B+4G) --
 * and guest address `a` (what the x86 code sees, always < 4 GB) lives at host
 * address B + a.
 *
 * An entry of __wine_unix_call_wow64_funcs receives its OUTER `args` pointer
 * already converted by the WoW64 CPU module, but every pointer EMBEDDED in
 * that 32-bit block is still a GUEST address.  Upstream's UInt32ToPtr is a
 * bare zero-extension, which is correct only under the classic WoW64 identity
 * guest == host.  Here it would produce a sub-4 GB address that is not mapped
 * at all, so the base has to be added.  That makes this function the DXMT
 * counterpart of ios_wow_host_ptr() in build/ntdll-unix/ios_wow.h, and it is
 * the single conversion point for all 22 of its call sites in the thunk32_*
 * handlers below.
 *
 * ios_wow_base() is defined in ntdll's unix side (build/ntdll-unix/
 * virtual_ios.c, declared in ios_wow.h).  DXMT's unix half is statically
 * linked into the very same binary -- libdxmt_combined.a inside Madeira.app --
 * so the symbol resolves at link time with no new dependency.  It returns 0
 * for a process that has no guest window, which collapses this back to the
 * plain zero-extension: a 64-bit caller reaching here, and every non-iOS
 * build, behave exactly as before.  NULL is preserved in both directions. */
#if TARGET_OS_IOS
extern unsigned long ios_wow_base(void);
#endif

static inline void *
UInt32ToPtr(uint32_t v) {
#if TARGET_OS_IOS
  return v ? (void *)(ios_wow_base() + (uint64_t)v) : NULL;
#else
  return (void *)(uint64_t)v;
#endif
}

/* The reverse direction: a host pointer written back into a field that a
 * 32-bit caller will read.  The field is a WMTMemoryPointer (8 bytes on both
 * sides: `void *ptr` plus a `uint32_t high_part` that the i386 side forces to
 * 0), so storing the guest address as a pointer value leaves the high half
 * zero, which is what the i386 accessor asserts on. */
static inline void *
PtrToUInt32Ptr(void *host) {
#if TARGET_OS_IOS
  return host ? (void *)(uint64_t)(uint32_t)((unsigned long)host - ios_wow_base()) : NULL;
#else
  return (void *)(uint64_t)(uint32_t)(uintptr_t)host;
#endif
}

#ifndef DXMT_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  uint32_t elements;
};

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORAMT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

void
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data =
          malloc(sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_output_slots = src->num_output_slots;
      data->num_elements = src->num_elements;
      data->strides[0] = src->strides[0];
      data->strides[1] = src->strides[1];
      data->strides[2] = src->strides[2];
      data->strides[3] = src->strides[3];
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->dual_source_blending = src->dual_source_blending;
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}
#endif /* DXMT_NATIVE */

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle error];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle logs];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumeratable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    CGColorSpaceRelease(ref);
  });
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

#if !TARGET_OS_IOS
  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }
#endif

  return STATUS_SUCCESS;
}

#if !TARGET_OS_IOS
typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t))
    return false;
  icc_XYZ_t *data = (icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (data->sig != 0x205a5958)
    return false;
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}
#endif /* !TARGET_OS_IOS — end of ColorSync/NSScreen block */

#if !TARGET_OS_IOS
NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}
#endif

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
#if TARGET_OS_IOS
  (void)params;
  desc_out->maximum_edr_color_component_value = 1.0;
  desc_out->maximum_reference_edr_color_component_value = 0.0;
  desc_out->maximum_potential_edr_color_component_value = 1.0;
#else
  CGDirectDisplayID display_id = params->handle;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out))
    GetDisplayColorGamut(ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile), desc_out);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
#endif
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

#if !TARGET_OS_IOS
  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;

  value->maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  value->maximum_potential_edr_color_component_value = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
#endif

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  if (wmtr_enabled()) {
    /* Each constant's VALUE is inlined; the pointer inside WMTFunctionConstant
     * is a guest address and cannot cross. */
    const char *nm = (const char *)params->name.ptr;
    size_t nlen = nm ? strlen(nm) : 0;
    const struct WMTFunctionConstant *cs = params->constants.ptr;
    uint8_t buf[4096];
    struct rm_wmt_info *w = (void *)buf;
    size_t off = sizeof *w;
    params->ret = 0; params->ret_error = 0;
    if (nlen && off + nlen < sizeof buf) {
      memcpy(buf + off, nm, nlen); off += nlen;
      w->owner = params->library; w->info_len = (uint32_t)nlen; w->extra_count = 0;
      for (uint64_t k = 0; k < params->num_constants && cs; k++) {
        uint32_t vlen = wmt_const_size(cs[k].type);
        if (!vlen) { fprintf(stderr, "[wmt-remote] unknown function-constant type %u at index %u"
                                     " -- not sent\n", (unsigned)cs[k].type, cs[k].index); continue; }
        if (off + sizeof(struct rm_fn_const) + vlen > sizeof buf) break;
        struct rm_fn_const fc = { (uint16_t)cs[k].type, cs[k].index, vlen };
        memcpy(buf + off, &fc, sizeof fc); off += sizeof fc;
        memcpy(buf + off, cs[k].data.ptr, vlen); off += vlen;
        w->extra_count++;
      }
      struct rm_ret_handle r;
      if (wmtr_call(RM_OP_NEW_FUNCTION_CONSTS, buf, (uint32_t)off, &r, sizeof r, 0) == RM_OK)
        params->ret = r.handle;
    }
    return STATUS_SUCCESS;
  }
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  struct WMTFunctionConstant *constants = (struct WMTFunctionConstant *)params->constants.ptr;
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++)
    [values setConstantValue:constants[i].data.ptr type:(MTLDataType)constants[i].type atIndex:constants[i].index];

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)err;
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  /* ml1050: THE FRAME BOUNDARY ON THE PRESENTING THREAD.
   *
   * Presenter::synchronizeLayerProperties() issues this call exactly once per
   * Present, on the CALLING thread (dxmt_presenter.cpp:119, reached from the
   * d3d9 swapchain's Present before it commits the present chunk), and it is
   * the only per-frame native call that thread makes.  So it is both free and
   * exact as a frame boundary, and it is what claims IOS_FRAME_ROLE_GAME --
   * no heuristic, no thread-name matching, no guessing which of fifty threads
   * is "the game".  Placed before the early return so the iOS path counts. */
  ios_frame_game_tick();

  params->version = 0;
#if TARGET_OS_IOS
  (void)layer;
  return STATUS_SUCCESS;
#else
  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;
  CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

  struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
  *hdr_metadata_out = setting->hdr_metadata;
  params->version = setting->version;
  params->colorspace = setting->colorspace;
  params->edr_value.maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  params->edr_value.maximum_potential_edr_color_component_value =
      screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

  return STATUS_SUCCESS;
#endif
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_encode_sig a = { params->handle, params->arg0, params->arg1 };
    wmtr_call(RM_OP_ENCODE_WAIT, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  [(id<MTLCommandBuffer>)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef DXMT_NATIVE

typedef struct {
  _Atomic(CFRunLoopRef) runloop_ref;
  MTLSharedEventListener *shared_listener;
} *shared_event_listener_t;

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  void *nt_event_handle = (shared_event_listener_t)params->event_handle;
  shared_event_listener_t q = (shared_event_listener_t)params->shared_event_listener;
  [(id<MTLSharedEvent>)params->shared_event
      notifyListener:q->shared_listener
             atValue:params->value
               block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                 // NOTE: must ensure no more notification comes after listener been destroyed.
                 while (!atomic_load_explicit(&q->runloop_ref, memory_order_acquire)) {
#if defined(__x86_64__)
                   _mm_pause();
#elif defined(__aarch64__)
          __asm__ __volatile__("yield");
#endif
                 }
                 CFRunLoopPerformBlock(q->runloop_ref, kCFRunLoopCommonModes, ^{
                   NtSetEvent(nt_event_handle, NULL);
                 });
                 CFRunLoopWakeUp(q->runloop_ref);
               }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  CFRunLoopRef uninited = NULL;
  if (q && atomic_compare_exchange_strong(&q->runloop_ref, &uninited, CFRunLoopGetCurrent())) {
    /* Add a dummy source so the runloop stays running */
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(q->runloop_ref, source, kCFRunLoopCommonModes);
    CFRunLoopRun();
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  shared_event_listener_t q = malloc(sizeof(*q));
  if (q) {
    q->runloop_ref = NULL;
    q->shared_listener = [[MTLSharedEventListener alloc] init];
  }
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q && q->runloop_ref) {
    CFRunLoopStop(q->runloop_ref);
    q->runloop_ref = NULL;
    [q->shared_listener release];
    q->shared_listener = nil;
    free(q);
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
_MTLDevice_newFence(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  if (wmtr_enabled()) {
    /* Copy the entry out under the lock rather than holding a pointer into the
     * table across a memcpy and an RPC. */
    struct wmtr_buf b;
    const int found = wmtr_buf_lookup(params->buffer, &b);
    const int in_range = found && params->offset + params->length <= b.length;

    if (found && b.shadow && in_range) {
      memcpy((char *)b.shadow + params->offset, params->data.ptr, params->length);
      /* ml822: send it now, always.
       *
       * ml821 skipped this and let the pre-submission flush carry the bytes,
       * on the reasoning that the memcpy above is already the snapshot and the
       * flush hashes this shadow before the GPU reads it. That removes a real
       * duplicate -- these bytes were being sent twice, because this path never
       * rebased the page checksums it dirtied -- but it makes delivery depend
       * entirely on the hash noticing every write, and it lost a race with
       * readback: a completing GPU write downloads host bytes over a shadow
       * whose update had not been sent yet, and rebases the checksum, so the
       * write disappears with nothing reporting it.
       *
       * Deduplicating this is still worth doing. It needs the checksum rebased
       * here under the registry lock, and tests through the real entry points
       * for the readback ordering -- not an inference. */
      wmtr_buf_upload(params->buffer, (const char *)b.shadow + params->offset,
                      params->offset, params->length);
    } else if (found && !b.shadow && in_range) {
      /* ml802: a registered-but-shadowless buffer uploads DIRECTLY.
       *
       * The shadow exists to diff a buffer the app writes through a mapped
       * pointer, so the flush can find what changed. An explicit updateContents
       * needs none of that: the caller has already told us the source, the
       * offset and the length, which is exactly what the upload takes. Requiring
       * a persistent shadow merely to service explicit uploads is what made a
       * failed 32MB calloc silently discard every write to that buffer -- the
       * staging ring's sixth block, whose contents then never reached the host. */
      wmtr_buf_upload(params->buffer, (const char *)params->data.ptr,
                      params->offset, params->length);
    } else {
      /* Rate limited: this used to log per update, and one run produced 75,525
       * lines. Name WHICH failure it is -- "unregistered" covered three
       * different causes and was wrong for two of them. */
      static unsigned long unreg;
      if (++unreg <= 4 || (unreg % 4096) == 0)
        fprintf(stderr, "[wmt-remote] updateContents DROPPED on 0x%llx: %s "
                        "(offset %llu length %llu, buffer length %llu) (%lu so far)\n",
                (unsigned long long)params->buffer,
                !found      ? "NOT_FOUND — never registered"
                            : "OUT_OF_RANGE — write exceeds the buffer",
                (unsigned long long)params->offset,
                (unsigned long long)params->length,
                (unsigned long long)(found ? b.length : 0), unreg);
    }
    return STATUS_SUCCESS;
  }
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
#if !TARGET_OS_IOS
  /* Managed storage mode doesn't exist on iOS (unified memory). */
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
#endif
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  if (wmtr_enabled()) {
    struct rm_os_version v;
    if (wmtr_call(RM_OP_OS_VERSION, 0, 0, &v, sizeof v, 0) == RM_OK) {
      params->ret_major = v.major; params->ret_minor = v.minor; params->ret_patch = v.patch;
    } else {
      params->ret_major = params->ret_minor = params->ret_patch = 0;
    }
    return STATUS_SUCCESS;
  }
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBinaryArchive(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)err;
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)err;
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)dispatch_data_create((void *)params->handle, params->arg, NULL, NULL);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  if (wmtr_enabled()) {
    /* A shared texture cannot cross machines; the host makes an ordinary one
     * and no port comes back, which the caller treats as "unshared". */
    struct WMTTextureInfo *ti = params->info.ptr;
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTTextureInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device; w->info_len = sizeof *ti; w->extra_count = 0;
    memcpy(buf + sizeof *w, ti, sizeof *ti);
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_NEW_TEXTURE_FULL, buf, sizeof buf, &r, sizeof r, 0) == RM_OK && r.handle) {
      params->ret = r.handle;
      ti->gpu_resource_id = r.value;
    } else {
      params->ret = 0; ti->gpu_resource_id = 0;
    }
    ti->mach_port = 0;
    return STATUS_SUCCESS;
  }
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

static NTSTATUS
_WMTBootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = params->mach_port;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_register2(bp, params->name, rp, 0) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  return ret;
}

static NTSTATUS
_WMTBootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = 0;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_look_up(bp, params->name, &rp) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  params->mach_port = rp;
  return ret;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
_MTLSharedEvent_createMachPort(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];
  
  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
  
  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEventWithMachPort(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    struct rm_ret_u64 r;
    params->ret = (wmtr_call(RM_OP_DEVICE_REGISTRY_ID, &a, sizeof a, &r, sizeof r, 0) == RM_OK) ? r.value : 0;
    return STATUS_SUCCESS;
  }
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  return STATUS_SUCCESS;
}

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);

#ifndef DXMT_NATIVE
/* ------------------------------------------------------------------------
 * MADEIRA (WOW64_DESIGN.md section 7.4): 32-bit variants for every slot whose
 * argument block carries an EMBEDDED pointer.
 *
 * The outer `args` pointer is converted for us by the WoW64 CPU module; every
 * pointer inside the block is still a GUEST address, and under the shifted
 * guest window (section 2) a guest address is NOT a host address.  Before
 * this, these slots were shared verbatim between the two tables, so a 32-bit
 * caller had its guest pointers dereferenced as host pointers.
 *
 * Why the 64-bit struct is reused instead of a `*_params32` mirror: DXMT wraps
 * every embedded pointer in WMTMemoryPointer / WMTConstMemoryPointer, which is
 * 8 bytes on both sides (`void *ptr` plus an i386-only `uint32_t high_part`
 * the 32-bit side forces to 0).  The blocks are therefore layout-identical and
 * the only thing that differs is the VALUE of the pointer fields, so each
 * variant converts them in place, calls the 64-bit handler, and puts the guest
 * values back.  Restoring matters: the block is the guest's own memory and it
 * reads its own fields again (DXMT reuses the WMTBufferInfo it passed in).
 *
 * Rules followed here (section 7.4): every embedded pointer is converted at
 * every level of nesting before any dereference; NULL stays NULL; an OUT
 * pointer field written back for the guest is converted the other way with
 * PtrToUInt32Ptr; handles, sizes, flags and gpu_address are never touched.
 * ------------------------------------------------------------------------ */

/* Guest -> host for a pointer field an i386 caller wrote.  It stored 4 bytes
 * of guest address and a zero high half, so the 64-bit read of the field is
 * the zero-extended guest address; UInt32ToPtr adds the window base and
 * preserves NULL. */
static inline void *
wow_in(const void *field) {
  return UInt32ToPtr((uint32_t)(uintptr_t)field);
}

/* Host -> guest, for a field the guest will read back. */
static inline void *
wow_out(const void *host) {
  return PtrToUInt32Ptr((void *)host);
}

/* MADEIRA (WOW64_DESIGN.md sections 3 and 7.5).  Guest -> host for a pointer
 * carried in a field that is 64 bits WIDE on both sides -- a raw uint64_t /
 * obj_handle_t, not a WMTMemoryPointer.  Such a field can hold EITHER
 * namespace and the two must be told apart before anything is added to it:
 *
 *  - a GUEST address, which the i386 caller wrote from one of its own 32-bit
 *    pointers.  It is always < 4 GB and needs the window base.
 *  - a HOST address the unix side produced earlier and handed back through a
 *    deliberately 64-bit-wide field, so that it crosses the boundary intact.
 *    airconv's sm50_ptr64_t exists for exactly this (it is `void *` on LP64 and
 *    a `uint64_t` box on i386), and SM50_COMPILED_BITCODE::Data -- the compiled
 *    AIR blob from SM50GetCompiledBitcode / DXSOGetCompiledBitcode -- is one.
 *    Such a value must be passed through UNTOUCHED.  Adding the window base to
 *    it truncates the top half first (UInt32ToPtr takes a uint32_t), which
 *    lands on an unrelated, usually unmapped, address inside the window.
 *
 * The test is EXACT, not a heuristic: XNU reserves a 4 GB __PAGEZERO for every
 * arm64 binary (section 1), so no host mapping can ever exist below 4 GB, and a
 * guest address is below 4 GB by construction (invariant 1).  This is the same
 * discrimination ios_wow_fixup_peb64_ptrs() makes in build/ntdll-unix/env_ios.c.
 * Off iOS, and for a process with no guest window, ios_wow_base() is 0 and both
 * arms collapse to the classic identity. */
static inline uint64_t
wow_in_wide(uint64_t field) {
  if (field >= 0x100000000ull)
    return field; /* already a host address; never rebase it */
  return (uint64_t)(uintptr_t)UInt32ToPtr((uint32_t)field);
}

static NTSTATUS
_NSString_getCString32(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  uint64_t guest = params->buffer_ptr;
  NTSTATUS status;

  /* OUT buffer; the POINTER is IN.  64-bit-wide field, so wow_in_wide. */
  params->buffer_ptr = wow_in_wide(guest);
  status = _NSString_getCString(obj);
  params->buffer_ptr = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newBuffer32(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  void *guest_info = params->info.ptr;
  struct WMTBufferInfo *info = wow_in(guest_info);
  void *guest_memory;
  uint64_t storage_mode;
  NTSTATUS status;

  if (!info) {
    params->ret = 0;
    return STATUS_INVALID_PARAMETER;
  }

  /* MADEIRA (WOW64_DESIGN.md section 7.5).  Two paths exist here:
   *
   *  - caller-supplied memory (info->memory.ptr non-NULL) is DXMT's normal
   *    path -- the ring bump allocator hands in its own PE-side heap block and
   *    keeps writing argument-buffer contents through that same pointer.  For
   *    a 32-bit caller that block came from a VirtualAlloc inside the guest
   *    window, so adding B gives a host address that names the same bytes and
   *    the app can keep using its 32-bit pointer.  This works.
   *
   *  - Metal-allocated memory (info->memory.ptr NULL on a CPU-visible buffer)
   *    makes the handler write [buffer contents] back into the field.  That is
   *    a pointer into Metal's own heap, which is NOT in the guest window, so
   *    there is no 32-bit address that names it: truncating it would hand the
   *    guest a pointer to something else entirely.  Refuse loudly instead.
   *    Private and memoryless buffers are exempt: the CPU never maps them and
   *    the handler leaves the field NULL. */
  storage_mode = (uint64_t)info->options & 0x30;
  if (!info->memory.ptr && storage_mode != WMTResourceStorageModePrivate &&
      storage_mode != (uint64_t)WMTResourceStorageModeMemoryless) {
    fprintf(
        stderr,
        "winemetal: MTLDevice_newBuffer from a 32-bit caller with no caller-supplied memory "
        "(length %llu, options 0x%llx): [buffer contents] has no guest address, refusing. "
        "Route the allocation through the ring allocator (WOW64_DESIGN.md section 7.5).\n",
        (unsigned long long)info->length, (unsigned long long)info->options
    );
    params->ret = 0;
    return STATUS_INVALID_ADDRESS;
  }

  guest_memory = info->memory.ptr;
  info->memory.ptr = wow_in(guest_memory);
  params->info.ptr = info;

  status = _MTLDevice_newBuffer(obj);

  /* gpu_address is a Metal GPU virtual address, never a CPU one -- untouched.
   * memory.ptr goes back to the guest value the caller gave us (the handler
   * leaves it alone on this path, except for private/memoryless buffers where
   * it writes NULL, which is representable). */
  info->memory.ptr = info->memory.ptr ? guest_memory : NULL;
  params->info.ptr = guest_info;
  return status;
}

static NTSTATUS
_MTLDevice_newSamplerState32(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newSamplerState(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newDepthStencilState32(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  const void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newDepthStencilState(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newTexture32(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newTexture(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newSharedTexture32(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newSharedTexture(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLBuffer_newTexture32(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLBuffer_newTexture(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLLibrary_newFunction32(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  uint64_t guest = params->arg;
  NTSTATUS status;

  /* `arg` is a guest pointer to the function name (a C string), not a value. */
  params->arg = wow_in_wide(guest);
  status = _MTLLibrary_newFunction(obj);
  params->arg = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newComputePipelineState32(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  const void *guest_info = params->info.ptr;
  struct WMTComputePipelineInfo *info = wow_in(guest_info);
  const void *guest_archives = NULL;
  NTSTATUS status;

  if (!info) {
    params->ret_pso = 0;
    params->ret_error = 0;
    return STATUS_INVALID_PARAMETER;
  }
  /* Second level: an array of binary-archive HANDLES, reached through a guest
   * pointer.  The handles themselves are host handles and are not converted. */
  guest_archives = info->binary_archives_for_lookup.ptr;
  info->binary_archives_for_lookup.ptr = wow_in(guest_archives);
  params->info.ptr = info;

  status = _MTLDevice_newComputePipelineState(obj);

  info->binary_archives_for_lookup.ptr = guest_archives;
  params->info.ptr = guest_info;
  return status;
}

static NTSTATUS
_MTLDevice_newRenderPipelineState32(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const void *guest_info = params->info.ptr;
  struct WMTRenderPipelineInfo *info = wow_in(guest_info);
  const void *guest_archives = NULL;
  NTSTATUS status;

  if (!info) {
    params->ret_pso = 0;
    params->ret_error = 0;
    return STATUS_INVALID_PARAMETER;
  }
  guest_archives = info->binary_archives_for_lookup.ptr;
  info->binary_archives_for_lookup.ptr = wow_in(guest_archives);
  params->info.ptr = info;

  status = _MTLDevice_newRenderPipelineState(obj);

  info->binary_archives_for_lookup.ptr = guest_archives;
  params->info.ptr = guest_info;
  return status;
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState32(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const void *guest_info = params->info.ptr;
  struct WMTMeshRenderPipelineInfo *info = wow_in(guest_info);
  const void *guest_archives = NULL;
  NTSTATUS status;

  if (!info) {
    params->ret_pso = 0;
    params->ret_error = 0;
    return STATUS_INVALID_PARAMETER;
  }
  guest_archives = info->binary_archives_for_lookup.ptr;
  info->binary_archives_for_lookup.ptr = wow_in(guest_archives);
  params->info.ptr = info;

  status = _MTLDevice_newMeshRenderPipelineState(obj);

  info->binary_archives_for_lookup.ptr = guest_archives;
  params->info.ptr = guest_info;
  return status;
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder32(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  uint64_t guest = params->arg;
  NTSTATUS status;

  /* `arg` is a guest pointer to a WMTRenderPassInfo.  Everything inside it is
   * handles and scalars, so one level is enough. */
  params->arg = wow_in_wide(guest);
  status = _MTLCommandBuffer_renderCommandEncoder(obj);
  params->arg = guest;
  return status;
}

/* The three encodeCommands slots are the hard ones: each walks a
 * caller-allocated singly linked list of wmtcmd_* records whose every `next`
 * is a guest pointer, so the conversion happens at EVERY hop, not once.  Four
 * command kinds also carry a payload pointer.  The chain is converted in
 * place, the handler runs, and the chain is converted back -- the nodes live
 * in the guest's own command heap and it reuses them. */
enum wow_cmd_kind {
  WOW_CMD_BLIT,
  WOW_CMD_COMPUTE,
  WOW_CMD_RENDER,
};

/* Convert one node's payload pointer, if its type carries one. */
static void
wow_cmd_payload(struct wmtcmd_base *node, enum wow_cmd_kind kind, int to_host) {
  struct WMTMemoryPointer *payload = NULL;

  switch (kind) {
  case WOW_CMD_RENDER:
    if (node->type == WMTRenderCommandSetFragmentBytes)
      payload = &((struct wmtcmd_render_setbytes *)node)->bytes;
    else if (node->type == WMTRenderCommandSetViewports)
      payload = &((struct wmtcmd_render_setviewports *)node)->viewports;
    else if (node->type == WMTRenderCommandSetScissorRects)
      payload = &((struct wmtcmd_render_setscissorrects *)node)->scissor_rects;
    /* MADEIRA (WOW64_DESIGN.md section 7.11): any later command that carries
     * a CPU pointer must be added here or the 32-bit path dereferences a
     * guest address. */
    break;
  case WOW_CMD_COMPUTE:
    if (node->type == WMTComputeCommandSetBytes)
      payload = &((struct wmtcmd_compute_setbytes *)node)->bytes;
    break;
  case WOW_CMD_BLIT:
    /* No blit command carries a CPU pointer. */
    break;
  }
  if (payload)
    payload->ptr = to_host ? wow_in(payload->ptr) : wow_out(payload->ptr);
}

/* `head` must already be a host pointer. */
static void
wow_cmd_chain(struct wmtcmd_base *head, enum wow_cmd_kind kind, int to_host) {
  struct wmtcmd_base *node = head;

  while (node) {
    struct wmtcmd_base *next;

    wow_cmd_payload(node, kind, to_host);
    if (to_host) {
      node->next.ptr = wow_in(node->next.ptr);
      next = node->next.ptr;
    } else {
      next = node->next.ptr; /* still a host pointer at this point */
      node->next.ptr = wow_out(node->next.ptr);
    }
    node = next;
  }
}

static NTSTATUS
_encodeCommands32(void *obj, enum wow_cmd_kind kind, NTSTATUS (*handler)(void *)) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const void *guest_head = params->cmd_head.ptr;
  struct wmtcmd_base *head = wow_in(guest_head);
  NTSTATUS status;

  params->cmd_head.ptr = head;
  wow_cmd_chain(head, kind, 1);

  status = handler(obj);

  wow_cmd_chain(head, kind, 0);
  params->cmd_head.ptr = guest_head;
  return status;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands32(void *obj) {
  return _encodeCommands32(obj, WOW_CMD_BLIT, _MTLBlitCommandEncoder_encodeCommands);
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands32(void *obj) {
  return _encodeCommands32(obj, WOW_CMD_COMPUTE, _MTLComputeCommandEncoder_encodeCommands);
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands32(void *obj) {
  return _encodeCommands32(obj, WOW_CMD_RENDER, _MTLRenderCommandEncoder_encodeCommands);
}

static NTSTATUS
_MTLTexture_replaceRegion32(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  void *guest = params->data.ptr;
  NTSTATUS status;

  params->data.ptr = wow_in(guest);
  status = _MTLTexture_replaceRegion(obj);
  params->data.ptr = guest;
  return status;
}

static NTSTATUS
_MTLCaptureManager_startCapture32(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  void *guest_info = params->info.ptr;
  struct WMTCaptureInfo *info = wow_in(guest_info);
  const void *guest_url = NULL;
  NTSTATUS status;

  if (!info) {
    params->ret = 0;
    return STATUS_INVALID_PARAMETER;
  }
  guest_url = info->output_url.ptr;
  info->output_url.ptr = wow_in(guest_url);
  params->info.ptr = info;

  status = _MTLCaptureManager_startCapture(obj);

  info->output_url.ptr = guest_url;
  params->info.ptr = guest_info;
  return status;
}

static NTSTATUS
_MTLDevice_newTemporalScaler32(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  const void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newTemporalScaler(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLDevice_newSpatialScaler32(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  const void *guest = params->info.ptr;
  NTSTATUS status;

  params->info.ptr = wow_in(guest);
  status = _MTLDevice_newSpatialScaler(obj);
  params->info.ptr = guest;
  return status;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale32(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  const void *guest = params->props.ptr;
  NTSTATUS status;

  params->props.ptr = wow_in(guest);
  status = _MTLCommandBuffer_encodeTemporalScale(obj);
  params->props.ptr = guest;
  return status;
}

static NTSTATUS
_NSString_string32(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  const void *guest = params->buffer_ptr.ptr;
  NTSTATUS status;

  params->buffer_ptr.ptr = wow_in(guest);
  status = _NSString_string(obj);
  params->buffer_ptr.ptr = guest;
  return status;
}

static NTSTATUS
_NSString_alloc_init32(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  const void *guest = params->buffer_ptr.ptr;
  NTSTATUS status;

  params->buffer_ptr.ptr = wow_in(guest);
  status = _NSString_alloc_init(obj);
  params->buffer_ptr.ptr = guest;
  return status;
}

static NTSTATUS
_MetalLayer_setProps32(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  const void *guest = params->arg.ptr;
  NTSTATUS status;

  params->arg.ptr = wow_in(guest);
  status = _MetalLayer_setProps(obj);
  params->arg.ptr = guest;
  return status;
}

static NTSTATUS
_MetalLayer_getProps32(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  void *guest = params->arg.ptr;
  NTSTATUS status;

  /* INOUT: the handler reads the requested drawable size and writes the
   * granted one back into the same block, which stays guest memory. */
  params->arg.ptr = wow_in(guest);
  status = _MetalLayer_getProps(obj);
  params->arg.ptr = guest;
  return status;
}

static NTSTATUS
_MTLLogContainer_enumerate32(void *obj) {
  struct unixcall_enumerate *params = obj;
  void *guest = params->buffer.ptr;
  NTSTATUS status;

  /* OUT array of handles; the ARRAY pointer is what needs converting. */
  params->buffer.ptr = wow_in(guest);
  status = _MTLLogContainer_enumerate(obj);
  params->buffer.ptr = guest;
  return status;
}

static NTSTATUS
_WMTGetDisplayDescription32(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  void *guest = params->arg.ptr;
  NTSTATUS status;

  params->arg.ptr = wow_in(guest);
  status = _WMTGetDisplayDescription(obj);
  params->arg.ptr = guest;
  return status;
}

static NTSTATUS
_MetalLayer_getEDRValue32(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  void *guest = params->arg.ptr;
  NTSTATUS status;

  params->arg.ptr = wow_in(guest);
  status = _MetalLayer_getEDRValue(obj);
  params->arg.ptr = guest;
  return status;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants32(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  const void *guest_name = params->name.ptr;
  const void *guest_constants = params->constants.ptr;
  struct WMTFunctionConstant *constants = wow_in(guest_constants);
  uint64_t i;

  NTSTATUS status;

  params->name.ptr = wow_in(guest_name);
  params->constants.ptr = constants;
  /* Two levels: each constant's `data` is its own guest pointer. */
  if (constants) {
    for (i = 0; i < params->num_constants; i++)
      constants[i].data.ptr = wow_in(constants[i].data.ptr);
  }

  status = _MTLLibrary_newFunctionWithConstants(obj);

  if (constants) {
    for (i = 0; i < params->num_constants; i++)
      constants[i].data.ptr = wow_out(constants[i].data.ptr);
  }
  params->constants.ptr = guest_constants;
  params->name.ptr = guest_name;
  return status;
}

static NTSTATUS
_WMTQueryDisplaySetting32(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  void *guest = params->hdr_metadata.ptr;
  NTSTATUS status;

  params->hdr_metadata.ptr = wow_in(guest);
  status = _WMTQueryDisplaySetting(obj);
  params->hdr_metadata.ptr = guest;
  return status;
}

static NTSTATUS
_WMTUpdateDisplaySetting32(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  const void *guest = params->hdr_metadata.ptr;
  NTSTATUS status;

  params->hdr_metadata.ptr = wow_in(guest);
  status = _WMTUpdateDisplaySetting(obj);
  params->hdr_metadata.ptr = guest;
  return status;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer32(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  void *guest = params->hdr_metadata.ptr;
  NTSTATUS status;

  params->hdr_metadata.ptr = wow_in(guest);
  status = _WMTQueryDisplaySettingForLayer(obj);
  params->hdr_metadata.ptr = guest;
  return status;
}

static NTSTATUS
_MTLBuffer_updateContents32(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  const void *guest = params->data.ptr;
  NTSTATUS status;

  params->data.ptr = wow_in(guest);
  status = _MTLBuffer_updateContents(obj);
  params->data.ptr = guest;
  return status;
}

static NTSTATUS
_DispatchData_alloc_init32(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  obj_handle_t guest = params->handle;
  NTSTATUS status;

  /* Despite the struct's name this slot's `handle` is the BYTES pointer
   * (dispatch_data_create(ptr, length)) and `arg` is the length.
   *
   * MADEIRA (WOW64_DESIGN.md section 7.5): the field is 64 bits wide on both
   * sides and both namespaces genuinely reach it, so it must be discriminated
   * rather than rebased unconditionally:
   *
   *  - WMT::Device::newLibrary(const void *bytecode, ...) passes PE-side bytes
   *    (the internal library blob compiled into the module, dxmt_command.cpp) --
   *    a guest address, which needs the window base.
   *  - WMT::Device::newLibraryFromNativeBuffer() and WMT::MakeDispatchData()
   *    pass the unix side's own SM50_COMPILED_BITCODE::Data, the AIR blob that
   *    SM50GetCompiledBitcode / DXSOGetCompiledBitcode allocated in the host
   *    heap.  It is a host address that crossed intact through a 64-bit field
   *    and must NOT be rebased.
   *
   * Rebasing it unconditionally is what produced the d3d9 shader-compile crash:
   * a host bitcode pointer was truncated to 32 bits and then offset by B, so
   * dispatch_data_create() memmove'd from unallocated window space. */
  params->handle = (obj_handle_t)wow_in_wide(guest);
  status = _DispatchData_alloc_init(obj);
  params->handle = guest;
  return status;
}

static NTSTATUS
_CacheReader_alloc_init32(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  const void *guest = params->path.ptr;
  NTSTATUS status;

  params->path.ptr = wow_in(guest);
  status = _CacheReader_alloc_init(obj);
  params->path.ptr = guest;
  return status;
}

static NTSTATUS
_CacheReader_get32(void *obj) {
  struct unixcall_cache_get *params = obj;
  const void *guest = params->key.ptr;
  NTSTATUS status;

  params->key.ptr = wow_in(guest);
  status = _CacheReader_get(obj);
  params->key.ptr = guest;
  return status;
}

static NTSTATUS
_CacheWriter_alloc_init32(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  const void *guest = params->path.ptr;
  NTSTATUS status;

  params->path.ptr = wow_in(guest);
  status = _CacheWriter_alloc_init(obj);
  params->path.ptr = guest;
  return status;
}

static NTSTATUS
_CacheWriter_set32(void *obj) {
  struct unixcall_cache_set *params = obj;
  const void *guest = params->key.ptr;
  NTSTATUS status;

  params->key.ptr = wow_in(guest);
  status = _CacheWriter_set(obj);
  params->key.ptr = guest;
  return status;
}

static NTSTATUS
_WMTSetMetalShaderCachePath32(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  const void *guest = params->path.ptr;
  NTSTATUS status;

  params->path.ptr = wow_in(guest);
  status = _WMTSetMetalShaderCachePath(obj);
  params->path.ptr = guest;
  return status;
}
#endif /* DXMT_NATIVE */

/* MADEIRA (WOW64_DESIGN.md section 8.4, measurement 2): the empty unix call.
 *
 * Deliberately the shortest possible handler. Section 8.4's whole point is
 * that the cost of ONE crossing decides the architecture of section 8.5, and
 * that the 150-400 ns estimate must not be built on -- so what this measures
 * has to be the crossing and nothing else. It touches no Metal object and
 * dereferences nothing, which is also why it is listed in gen_remote_guard.py's
 * LOCAL_OK set: there is no handle here that could belong to another machine,
 * so guarding it in remote mode would only measure the guard.
 *
 * No `_d3d9_nop32`. Rule 1 of section 7.4 exists for argument blocks with an
 * embedded pointer; this one has none, so a 32-bit caller's block is already
 * correct and sharing the handler is the right answer rather than an
 * oversight -- the same reason slots 66/67/47/72 share theirs (section 7.11).
 *
 * Named `_d3d9_nop` after the thing being costed, not after what it does. */
static NTSTATUS
_d3d9_nop(void *obj) {
  (void)obj;
  return STATUS_SUCCESS;
}
/* madeira-d3d12's shader-converter service: runtime DXIL -> metallib. Defined
 * in research/madeira-d3d12/src/unix/ and compiled into the same static
 * library as the rest of this unix side. */
extern int madeira_ir_convert(void *args);
static NTSTATUS _madeira_ir_convert(void *args) { return (NTSTATUS)madeira_ir_convert(args); }

/* The wow64 table must stay the SAME LENGTH as the native one, because the slot
 * number is the ABI. A 32-bit guest would hand us narrowed pointers, so pointing
 * this at the native handler would read the wrong addresses rather than fail.
 * Our ARM64EC guests are 64-bit and never take this path; if one ever does, it
 * gets a named refusal instead of silent corruption. */
static NTSTATUS _madeira_ir_convert_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLDevice_newRenderPipelineStateVD_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLDevice_newResidencySet_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLResidencySet_addAllocation_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLResidencySet_commit_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLCommandQueue_addResidencySet_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLDevice_newGeometryEmulationPipelineState_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLResidencySet_removeAllocation_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLDevice_heapTextureSizeAndAlign_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLDevice_newPlacementHeap_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLHeap_newTextureAtOffset_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }

/* ml1098: runtime control (winemetal.h, struct madeira_ctl_args). */
static volatile int g_madeira_capture_req;
void madeira_capture_request(int frames) {   /* called from the app's UI */
  __sync_fetch_and_add(&g_madeira_capture_req, frames > 0 ? frames : 1);
  fprintf(stderr, "[capture] ml1098 UI requested %d frame(s)\n", frames);
}
/* ml1136: live GPU encoder-sync mode from the overlay (0 = no request). The D3D12
 * runtime polls it once per Present (op 6) and switches at a list boundary, so a
 * mode can be A/B-tested in one spot instead of across runs. */
static volatile int g_madeira_fence_req;
void madeira_set_fence_mode(int mode) {
  g_madeira_fence_req = mode;
  fprintf(stderr, "[fence-mode] ml1136 UI requested fence-chain %d\n", mode);
}
static NTSTATUS _madeira_ctl(void *args) {
  struct madeira_ctl_args *a = args;
  if (!a) return STATUS_SUCCESS;
  a->ret = 0;
  switch (a->op) {
  case 0: {
    int n;
    do { n = g_madeira_capture_req; } while (n && !__sync_bool_compare_and_swap(&g_madeira_capture_req, n, 0));
    a->ret = (uint32_t)n;
    break;
  }
  case 1: {
    const char *docs = getenv("MADEIRA_DOCS_DIR");
    char path[1400];
    int fd;
    if (!docs || !*docs || !a->name[0] || !a->ptr) break;
    snprintf(path, sizeof path, "%s/capture", docs);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/capture/%s", docs, a->name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "[capture] ml1098 cannot create %s: %s\n", path, strerror(errno)); break; }
    {
      const unsigned char *p = (const unsigned char *)(uintptr_t)a->ptr;
      uint64_t left = a->len; int ok = 1;
      while (left) { ssize_t w = write(fd, p, left > (1u << 20) ? (1u << 20) : (size_t)left); if (w <= 0) { ok = 0; break; } p += w; left -= (uint64_t)w; }
      close(fd);
      a->ret = ok ? 1u : 0u;
    }
    break;
  }
  case 3: {   /* ml1108: GPU start/end of a COMPLETED command buffer (seconds, CACurrentMediaTime base) */
    struct { uint64_t cb; double start, end; } *t = (void *)(uintptr_t)a->ptr;
    if (!t || !t->cb || wmtr_enabled()) break;
    t->start = [(id<MTLCommandBuffer>)(uintptr_t)t->cb GPUStartTime];
    t->end = [(id<MTLCommandBuffer>)(uintptr_t)t->cb GPUEndTime];
    a->ret = 1;
    break;
  }
  case 4: {   /* ml1128: probe role for the CALLING thread (runs on it); len = role char, ptr = Windows TID */
    extern void ios_xp_set_role(int role, uint64_t wtid);
    ios_xp_set_role((int)a->len, a->ptr);
    a->ret = 1;
    break;
  }
  case 6:     /* ml1136: requested fence-chain mode (0 = none) */
    a->ret = (uint32_t)g_madeira_fence_req;
    break;
  case 5: {   /* ml1128: the PE counter block the probe samples */
    extern volatile uint64_t ios_xp_pe_block, ios_xp_pe_len;
    ios_xp_pe_len = a->len; ios_xp_pe_block = a->ptr;
    a->ret = 1;
    break;
  }
  case 7: {   /* ml2000: memory headroom for DXMT's automatic mip clamp.
               * len = os_proc_available_memory() bytes, ptr = phys_footprint
               * bytes (0 when task_info fails), ret = 1. ret stays 0 when the
               * limit is unknown (0 from os_proc_available_memory) and in
               * remote mode, where textures live on the other machine. No
               * pointer is read or written, which is why the wow64 entry may
               * forward this op unchanged. */
    task_vm_info_data_t vmi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    uint64_t avail;
    if (wmtr_enabled()) break;
    avail = (uint64_t)os_proc_available_memory();
    if (!avail) break;
    a->len = avail;
    a->ptr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) == KERN_SUCCESS
                 ? (uint64_t)vmi.phys_footprint : 0;
    a->ret = 1;
    break;
  }
  case 2: {
    char v[512];
    if (madeira_cfg_get(a->name, v, sizeof v)) {
      if (a->ptr && a->len) { strncpy((char *)(uintptr_t)a->ptr, v, (size_t)a->len - 1); ((char *)(uintptr_t)a->ptr)[a->len - 1] = 0; }
      a->ret = 1;
    }
    break;
  }
  default: break;
  }
  return STATUS_SUCCESS;
}
/* ml2000: the other ops carry guest pointers in ptr (which would need
 * UInt32ToPtr) and stay unimplemented for 32-bit callers. Op 7 is pointer-free
 * and struct madeira_ctl_args has the same offsets on i386 (two uint32, then
 * uint64s at 8 and 16, name at 24), so the i386 d3d11.dll gets the same
 * headroom reading. */
static NTSTATUS _madeira_ctl_wow64(void *args) {
  struct madeira_ctl_args *a = args;
  if (a && a->op == 7) return _madeira_ctl(args);
  return STATUS_NOT_IMPLEMENTED;
}

#if TARGET_OS_IOS
/* On iOS we statically link DXMT's unix side into the host app (Madeira.app),
 * alongside ntdll's own __wine_unix_call_funcs. Rename ours so the linker
 * doesn't get a duplicate symbol; our ntdll's load_builtin_unixlib picks
 * it up by name when a DLL registers winemetal.so as its unix path.        */
#define __wine_unix_call_funcs dxmt_winemetal_unix_call_funcs
/* MADEIRA (WOW64_DESIGN.md section 7): same treatment for the 32-bit table,
 * so the iOS ntdll has a name to bind to.  It matters because the static-link
 * fallback in load_builtin_unixlib (build/ntdll-unix/virtual_ios.c, the
 * `strstr(match, "winemetal")` branch) currently ignores its `wow` argument
 * and hands every caller the 64-bit table -- see the hand-off note in
 * WOW64_DESIGN.md section 7; that branch has to select this symbol when
 * `wow` is set, exactly as get_unixlib_funcs() does for a real .so.  The
 * naming also matches build/ntdll-unix/build.sh's compile_unixlib, which
 * renames both tables to <prefix>_unix_call{,_wow64}_funcs. */
#define __wine_unix_call_wow64_funcs dxmt_winemetal_unix_call_wow64_funcs
#endif


/* ml880: residency sets (slots 129-132). Local bodies need iOS 18 / macOS 15;
 * on anything older the calls are no-ops and the runtime falls back to its
 * per-draw useResource lists. */
static NTSTATUS
_MTLDevice_newResidencySet(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    struct rm_ret_handle r;
    uint32_t st = wmtr_call(RM_OP_NEW_RESIDENCY_SET, &a, sizeof a, &r, sizeof r, 0);
    params->ret = (st == RM_OK) ? r.handle : 0;
    fprintf(stderr, "[wmt-remote] ml881 newResidencySet: op=%u status=%u handle=%#llx\n",
            (unsigned)RM_OP_NEW_RESIDENCY_SET, st, (unsigned long long)params->ret);
    return STATUS_SUCCESS;
  }
  params->ret = 0;
  if (@available(iOS 18.0, macOS 15.0, *)) {
    MTLResidencySetDescriptor *d = [[MTLResidencySetDescriptor alloc] init];
    d.initialCapacity = (NSUInteger)params->arg;
    NSError *e = nil;
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newResidencySetWithDescriptor:d error:&e];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_addAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    wmtr_call(RM_OP_RESIDENCY_ADD, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  if (@available(iOS 18.0, macOS 15.0, *))
    wmt_stale_check(params->arg, "residency add");   /* ml1156 */
    [(id<MTLResidencySet>)params->handle addAllocation:(id<MTLAllocation>)params->arg];
  return STATUS_SUCCESS;
}

/* ml1050: the set RETAINS what it holds, and the D3D12 runtime only ever added.
 * Every texture, buffer and view the application destroyed therefore stayed
 * allocated for the rest of the run. Removal is staged until the next commit. */
static NTSTATUS
_MTLResidencySet_removeAllocation(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  if (wmtr_enabled()) return STATUS_SUCCESS;   /* the remote host owns its own set */
  if (@available(iOS 18.0, macOS 15.0, *))
    [(id<MTLResidencySet>)params->handle removeAllocation:(id<MTLAllocation>)params->arg];
  return STATUS_SUCCESS;
}

/* ml1072: placement heaps for the D3D12 runtime's small-texture sub-allocator. */
static NTSTATUS
_MTLDevice_heapTextureSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heaptexturesizealign *params = obj;
  params->ret_size = 0; params->ret_align = 0;
  if (wmtr_enabled()) return STATUS_SUCCESS;
  {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, params->info.ptr);
    MTLSizeAndAlign sa = [(id<MTLDevice>)params->device heapTextureSizeAndAlignWithDescriptor:desc];
    params->ret_size = sa.size; params->ret_align = sa.align;
    [desc release];
  }
  return STATUS_SUCCESS;
}
static NTSTATUS
_MTLDevice_newPlacementHeap(void *obj) {
  struct unixcall_mtldevice_newplacementheap *params = obj;
  params->ret = 0;
  if (wmtr_enabled()) return STATUS_SUCCESS;
  {
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    hd.type = MTLHeapTypePlacement;
    hd.size = params->size;
    hd.storageMode = ((params->options & 0x30) == WMTResourceStorageModePrivate) ? MTLStorageModePrivate : MTLStorageModeShared;
    hd.hazardTrackingMode = MTLHazardTrackingModeTracked;
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->device newHeapWithDescriptor:hd];
    [hd release];
  }
  return STATUS_SUCCESS;
}
static NTSTATUS
_MTLHeap_newTextureAtOffset(void *obj) {
  struct unixcall_mtlheap_newtextureatoffset *params = obj;
  struct WMTTextureInfo *info = params->info.ptr;
  params->ret = 0;
  if (wmtr_enabled()) return STATUS_SUCCESS;
  {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> t = [(id<MTLHeap>)params->heap newTextureWithDescriptor:desc offset:params->offset];
    params->ret = (obj_handle_t)t;
    info->gpu_resource_id = t ? [t gpuResourceID]._impl : 0;
    info->mach_port = 0;
    [desc release];
  }
  return STATUS_SUCCESS;
}

/* ml1145: buffers placed in a placement heap (D3D12 placed resources in DEFAULT heaps). */
static NTSTATUS
_MTLDevice_heapBufferSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heapbuffersizealign *params = obj;
  params->ret_size = 0; params->ret_align = 0;
  if (wmtr_enabled()) return STATUS_SUCCESS;
  {
    MTLSizeAndAlign sa = [(id<MTLDevice>)params->device heapBufferSizeAndAlignWithLength:params->length
                                                                                  options:(MTLResourceOptions)params->options];
    params->ret_size = sa.size; params->ret_align = sa.align;
  }
  return STATUS_SUCCESS;
}
static NTSTATUS
_MTLHeap_newBufferAtOffset(void *obj) {
  struct unixcall_mtlheap_newbufferatoffset *params = obj;
  struct WMTBufferInfo *info = params->info.ptr;
  params->ret = 0;
  if (wmtr_enabled()) return STATUS_SUCCESS;
  {
    id<MTLBuffer> b = [(id<MTLHeap>)params->heap newBufferWithLength:info->length
                                                             options:(MTLResourceOptions)info->options
                                                              offset:params->offset];
    params->ret = (obj_handle_t)b;
    if (wmt_stale_probe_on()) wmt_freed_set((uintptr_t)b, 0);   /* ml1156 */
    info->memory.ptr = (b && [b storageMode] != MTLStorageModePrivate) ? [b contents] : NULL;
    info->gpu_address = b ? [b gpuAddress] : 0;
  }
  return STATUS_SUCCESS;
}
static NTSTATUS _MTLDevice_heapBufferSizeAndAlign_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }
static NTSTATUS _MTLHeap_newBufferAtOffset_wow64(void *args) { (void)args; return STATUS_NOT_IMPLEMENTED; }

static NTSTATUS
_MTLResidencySet_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle a = { params->handle };
    wmtr_call(RM_OP_RESIDENCY_COMMIT, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  if (@available(iOS 18.0, macOS 15.0, *)) {
    [(id<MTLResidencySet>)params->handle commit];
    [(id<MTLResidencySet>)params->handle requestResidency];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  if (wmtr_enabled()) {
    struct rm_arg_handle_u64 a = { params->handle, params->arg };
    wmtr_call(RM_OP_QUEUE_ADD_RESIDENCY, &a, sizeof a, 0, 0, 0);
    return STATUS_SUCCESS;
  }
  if (@available(iOS 18.0, macOS 15.0, *))
    [(id<MTLCommandQueue>)params->handle addResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

/* ml927: geometry-shader emulation pipeline (converter mesh emulation). The
 * attachments cross as a WMTMeshRenderPipelineInfo and the libraries, names
 * and configuration as a WMTGeometryEmulationInfo right after it; the host
 * builds the functions (with their function constants and the linked stage-in
 * function) itself. Local backend (ml1138): built here the same way; iPhones
 * with mesh shaders render locally. */
static NTSTATUS
_MTLDevice_newGeometryEmulationPipelineState(void *obj) {
  struct unixcall_mtldevice_newgeompso *params = obj;
  params->ret_error = 0;
  params->ret_pso = 0;
  if (wmtr_enabled()) {
    uint8_t buf[sizeof(struct rm_wmt_info) + sizeof(struct WMTMeshRenderPipelineInfo) + sizeof(struct WMTGeometryEmulationInfo)];
    struct rm_wmt_info *w = (void *)buf;
    w->owner = params->device;
    w->info_len = sizeof(struct WMTMeshRenderPipelineInfo) + sizeof(struct WMTGeometryEmulationInfo);
    w->extra_count = 0;
    memcpy(buf + sizeof *w, params->info.ptr, sizeof(struct WMTMeshRenderPipelineInfo));
    memcpy(buf + sizeof *w + sizeof(struct WMTMeshRenderPipelineInfo), params->ge.ptr, sizeof(struct WMTGeometryEmulationInfo));
    struct rm_ret_handle r;
    params->ret_pso = (wmtr_call(RM_OP_NEW_GEOM_PSO_INFO, buf, sizeof buf, &r, sizeof r, 0) == RM_OK) ? r.handle : 0;
    if (!params->ret_pso)
      fprintf(stderr, "[wmt-remote] host refused a geometry-emulation pipeline\n");
    return STATUS_SUCCESS;
  }
  /* ml1138: LOCAL geometry emulation, exactly what IRRuntimeNewGeometryEmulationPipeline
   * (and rmetald's RM_OP_NEW_GEOM_PSO_INFO) does. Object stage = the vertex
   * shader's converter object entry with tessellation off, with the stage-in
   * function linked in; mesh stage = the geometry shader, told the vertex output
   * size; fragment = the pixel shader. Until now the local backend refused these,
   * the fallback plain pipeline cannot link a VS converted for stage-in
   * ("unresolved visible function reference: irconverter_stage_in_shader"), and a
   * UE5 title froze on the device waiting for its WriteToSlice pipelines. */
  @autoreleasepool {
    const struct WMTMeshRenderPipelineInfo *i = params->info.ptr;
    const struct WMTGeometryEmulationInfo *g = params->ge.ptr;
    id<MTLLibrary> Ls = (id<MTLLibrary>)g->stagein_library, Lv = (id<MTLLibrary>)g->vertex_library;
    id<MTLLibrary> Lg = (id<MTLLibrary>)g->geometry_library, Lf = (id<MTLLibrary>)g->fragment_library;
    MTLMeshRenderPipelineDescriptor *d;
    MTLFunctionConstantValues *cv;
    id<MTLFunction> fsi, fo, fm, ff = nil;
    NSError *e = nil;
    BOOL tess = NO;
    int vsz = (int)g->gs_vertex_size_bytes;
    if (!Ls || !Lv || !Lg || (g->fragment_function[0] && !Lf)) {
      fprintf(stderr, "[winemetal] ml1138 geometry pipeline: missing library (stage-in %d vertex %d geometry %d fragment %d)\n",
              !!Ls, !!Lv, !!Lg, !!Lf);
      return STATUS_SUCCESS;
    }
    d = [[[MTLMeshRenderPipelineDescriptor alloc] init] autorelease];
    for (unsigned c = 0; c < 8; c++) {
      d.colorAttachments[c].pixelFormat = to_metal_pixel_format(i->colors[c].pixel_format);
      d.colorAttachments[c].blendingEnabled = i->colors[c].blending_enabled;
      d.colorAttachments[c].writeMask = (MTLColorWriteMask)i->colors[c].write_mask;
      d.colorAttachments[c].alphaBlendOperation = (MTLBlendOperation)i->colors[c].alpha_blend_operation;
      d.colorAttachments[c].rgbBlendOperation = (MTLBlendOperation)i->colors[c].rgb_blend_operation;
      d.colorAttachments[c].sourceRGBBlendFactor = (MTLBlendFactor)i->colors[c].src_rgb_blend_factor;
      d.colorAttachments[c].sourceAlphaBlendFactor = (MTLBlendFactor)i->colors[c].src_alpha_blend_factor;
      d.colorAttachments[c].destinationRGBBlendFactor = (MTLBlendFactor)i->colors[c].dst_rgb_blend_factor;
      d.colorAttachments[c].destinationAlphaBlendFactor = (MTLBlendFactor)i->colors[c].dst_alpha_blend_factor;
    }
    d.depthAttachmentPixelFormat = to_metal_pixel_format(i->depth_pixel_format);
    d.stencilAttachmentPixelFormat = to_metal_pixel_format(i->stencil_pixel_format);
    d.alphaToCoverageEnabled = i->alpha_to_coverage_enabled;
    d.rasterizationEnabled = i->rasterization_enabled;
    d.rasterSampleCount = i->raster_sample_count ? i->raster_sample_count : 1;

    cv = [[[MTLFunctionConstantValues alloc] init] autorelease];
    fsi = [[Ls newFunctionWithName:Ls.functionNames.firstObject] autorelease];
    [cv setConstantValue:&tess type:MTLDataTypeBool withName:@"tessellationEnabled"];
    fo = [[Lv newFunctionWithName:[NSString stringWithFormat:@"%s.dxil_irconverter_object_shader", g->vertex_function]
                   constantValues:cv error:&e] autorelease];
    if (!fo)
      fprintf(stderr, "[winemetal] ml1138 geometry pipeline: object function '%s.dxil_irconverter_object_shader': %s\n",
              g->vertex_function, e ? [[e localizedDescription] UTF8String] : "?");
    [cv setConstantValue:&vsz type:MTLDataTypeInt withName:@"vertex_shader_output_size_fc"];
    e = nil;
    fm = [[Lg newFunctionWithName:[NSString stringWithUTF8String:g->geometry_function] constantValues:cv error:&e] autorelease];
    if (!fm)
      fprintf(stderr, "[winemetal] ml1138 geometry pipeline: mesh function '%s': %s\n", g->geometry_function,
              e ? [[e localizedDescription] UTF8String] : "?");
    if (g->fragment_function[0])
      ff = [[Lf newFunctionWithName:[NSString stringWithUTF8String:g->fragment_function]] autorelease];
    if (!fsi || !fo || !fm || (g->fragment_function[0] && !ff)) {
      fprintf(stderr, "[winemetal] ml1138 geometry pipeline REFUSED: stage-in %d object %d mesh %d fragment %d\n",
              !!fsi, !!fo, !!fm, !!ff);
      return STATUS_SUCCESS;
    }
    d.objectFunction = fo;
    d.meshFunction = fm;
    d.fragmentFunction = ff;
    {
      MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
      lf.functions = @[ fsi ];
      d.objectLinkedFunctions = lf;
    }
    e = nil;
    params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithMeshDescriptor:d
                                                                                                    options:MTLPipelineOptionNone
                                                                                                 reflection:nil
                                                                                                      error:&e];
    if (!params->ret_pso)
      fprintf(stderr, "[winemetal] ml1138 geometry pipeline: %s\n", e ? [[e localizedDescription] UTF8String] : "?");
    else {
      static unsigned said;
      if (said++ < 4)
        fprintf(stderr, "[winemetal] ml1138 local geometry pipeline OK: vs '%s' gs '%s' ps '%s', vertex %u B\n",
                g->vertex_function, g->geometry_function, g->fragment_function, g->gs_vertex_size_bytes);
    }
  }
  return STATUS_SUCCESS;
}

#include "wmt_remote_guard.h"

const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_rmg_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_rmg_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_rmg_MTLTexture_depth,
    &_rmg_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_rmg_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_rmg_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_rmg_MTLCaptureManager_sharedCaptureManager,
    &_rmg_MTLCaptureManager_startCapture,
    &_rmg_MTLCaptureManager_stopCapture,
    &_rmg_MTLDevice_newTemporalScaler,
    &_rmg_MTLDevice_newSpatialScaler,
    &_rmg_MTLCommandBuffer_encodeTemporalScale,
    &_rmg_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_rmg_MTLDevice_supportsFXSpatialScaler,
    &_rmg_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk_SM50Compile,
    &thunk_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk_SM50CompileGeometryPipelineVertex,
    &thunk_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk_SM50CompileTessellationPipelineHull,
    &thunk_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk_SM50GetArgumentsInfo,
    &_rmg_MTLCommandBuffer_error,
    &_rmg_MTLCommandBuffer_logs,
    &_rmg_MTLLogContainer_enumerate,
    &_rmg_CGColorSpace_checkColorSpaceSupported,
    &_rmg_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_rmg_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_rmg_WMTQueryDisplaySetting,
    &_rmg_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_rmg_MTLSharedEvent_signalValue,
    &_rmg_MTLSharedEvent_setWin32EventAtValue,
    &_rmg_MTLDevice_newFence,
    &_rmg_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_rmg_MTLDevice_newBinaryArchive,
    &_rmg_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_rmg_WMTBootstrapRegister,
    &_rmg_WMTBootstrapLookUp,
    &_rmg_MTLSharedEvent_createMachPort,
    &_rmg_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_rmg_MTLSharedEvent_waitUntilSignaledValue,
    /* madeira-d3d12 runtime DXIL conversion. APPENDED, never inserted: the slot
     * number is the ABI, so an insertion would silently send every later call
     * to the wrong function.
     *
     * Deliberately not remote-guarded. It is a pure byte transform that never
     * dereferences a Metal handle, so it is safe on the guest even when
     * rendering is remote. What must follow the rendering backend is the
     * TARGET, and that is chosen by the caller and passed in. */
    &_madeira_ir_convert,
    &_MTLDevice_newRenderPipelineStateVD,
    &_MTLDevice_newResidencySet,
    &_MTLResidencySet_addAllocation,
    &_MTLResidencySet_commit,
    &_MTLCommandQueue_addResidencySet,
    &_MTLDevice_newGeometryEmulationPipelineState,
    &_rmg_MTLResidencySet_removeAllocation,
    &_rmg_MTLDevice_heapTextureSizeAndAlign,
    &_rmg_MTLDevice_newPlacementHeap,
    &_rmg_MTLHeap_newTextureAtOffset,
    &_madeira_ctl,   /* ml1098 */
    /* 127-138 are madeira-d3d12 (above); 139-140 are the placement-heap buffers; 141-144 stay NULL;
     * 145-149 are reserved for the DXSO (D3D9 shader) compiler; 150 is the unix-call benchmark nop.
     * The slot number is the ABI: never insert, never reuse. */
    &_MTLDevice_heapBufferSizeAndAlign,   /* ml1145: 139 */
    &_MTLHeap_newBufferAtOffset,          /* ml1145: 140 */
    NULL, /* 141 */
    NULL, /* 142 */
    NULL, /* 143 */
    NULL, /* 144 */
    NULL, /* 145: reserved for the DXSO (D3D9 shader) compiler */
    NULL, /* 146 */
    NULL, /* 147 */
    NULL, /* 148 */
    NULL, /* 149 */
    /* MADEIRA (WOW64_DESIGN.md section 8.4, measurement 2): appended at the
     * END of both tables, which is the only place a slot may be added --
     * rules 3 and 4 of section 7.4. 150 in both. */
    &_d3d9_nop,                         /* 150 */
};

#ifndef DXMT_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString32,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer32,
    &_MTLDevice_newSamplerState32,
    &_MTLDevice_newDepthStencilState32,
    &_MTLDevice_newTexture32,
    &_MTLBuffer_newTexture32,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction32,
    &_NSString_lengthOfBytesUsingEncoding,
    &_rmg_NSObject_description,
    &_MTLDevice_newComputePipelineState32,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder32,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState32,
    &_MTLDevice_newMeshRenderPipelineState32,
    &_MTLBlitCommandEncoder_encodeCommands32,
    &_MTLComputeCommandEncoder_encodeCommands32,
    &_MTLRenderCommandEncoder_encodeCommands32,
    &_rmg_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_rmg_MTLTexture_depth,
    &_rmg_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion32,
    &_rmg_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_rmg_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_rmg_MTLCaptureManager_sharedCaptureManager,
    &_rmg_MTLCaptureManager_startCapture32,
    &_rmg_MTLCaptureManager_stopCapture,
    &_rmg_MTLDevice_newTemporalScaler32,
    &_rmg_MTLDevice_newSpatialScaler32,
    &_rmg_MTLCommandBuffer_encodeTemporalScale32,
    &_rmg_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string32,
    &_NSString_alloc_init32,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_rmg_MTLDevice_supportsFXSpatialScaler,
    &_rmg_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps32,
    &_MetalLayer_getProps32,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk32_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk32_SM50Compile,
    &thunk32_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk32_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk32_SM50CompileGeometryPipelineVertex,
    &thunk32_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk32_SM50CompileTessellationPipelineHull,
    &thunk32_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk32_SM50GetArgumentsInfo,
    &_rmg_MTLCommandBuffer_error,
    &_rmg_MTLCommandBuffer_logs,
    &_rmg_MTLLogContainer_enumerate32,
    &_rmg_CGColorSpace_checkColorSpaceSupported,
    &_rmg_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_rmg_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription32,
    &_MetalLayer_getEDRValue32,
    &_MTLLibrary_newFunctionWithConstants32,
    &_rmg_WMTQueryDisplaySetting32,
    &_rmg_WMTUpdateDisplaySetting32,
    &_WMTQueryDisplaySettingForLayer32,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_rmg_MTLSharedEvent_signalValue,
    &_rmg_MTLSharedEvent_setWin32EventAtValue,
    &_rmg_MTLDevice_newFence,
    &_rmg_MTLDevice_newEvent,
    &_MTLBuffer_updateContents32,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_rmg_MTLDevice_newBinaryArchive,
    &_rmg_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init32,
    &_CacheReader_alloc_init32,
    &_CacheReader_get32,
    &_CacheWriter_alloc_init32,
    &_CacheWriter_set32,
    &_WMTSetMetalShaderCachePath32,
    &_MTLDevice_newSharedTexture32,
    &_rmg_WMTBootstrapRegister,
    &_rmg_WMTBootstrapLookUp,
    &_rmg_MTLSharedEvent_createMachPort,
    &_rmg_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_rmg_MTLSharedEvent_waitUntilSignaledValue,
    &_madeira_ir_convert_wow64,
    &_MTLDevice_newRenderPipelineStateVD_wow64,
    &_MTLDevice_newResidencySet_wow64,
    &_MTLResidencySet_addAllocation_wow64,
    &_MTLResidencySet_commit_wow64,
    &_MTLCommandQueue_addResidencySet_wow64,
    &_MTLDevice_newGeometryEmulationPipelineState_wow64,
    &_MTLResidencySet_removeAllocation_wow64,
    &_MTLDevice_heapTextureSizeAndAlign_wow64,
    &_MTLDevice_newPlacementHeap_wow64,
    &_MTLHeap_newTextureAtOffset_wow64,
    &_madeira_ctl_wow64,
    /* 127-138 are madeira-d3d12 (above); 139-140 are the placement-heap buffers; 141-144 stay NULL;
     * 145-149 are reserved for the DXSO (D3D9 shader) compiler; 150 is the unix-call benchmark nop.
     * The slot number is the ABI: never insert, never reuse. */
    &_MTLDevice_heapBufferSizeAndAlign_wow64,   /* 139 */
    &_MTLHeap_newBufferAtOffset_wow64,   /* 140 */
    NULL, /* 141 */
    NULL, /* 142 */
    NULL, /* 143 */
    NULL, /* 144 */
    NULL, /* 145: reserved for the DXSO (D3D9 shader) compiler */
    NULL, /* 146 */
    NULL, /* 147 */
    NULL, /* 148 */
    NULL, /* 149 */
    /* 150: no `_d3d9_nop32`.  The block is two uint64_t with no embedded
     * pointer, so there is nothing for a 32-bit variant to convert and
     * sharing the 64-bit handler is correct, not a gap (section 7.11 says the
     * same about slots 47, 66, 67 and 72). */
    &_d3d9_nop,                         /* 150 */
};
#endif
