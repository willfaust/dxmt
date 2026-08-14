#pragma once
/* ============================================================================
 * ml677 -- WHO OWNS THE GIGABYTE?
 *
 * The ml676 run reported `tag 100 total dirty=1380 MB` and I turned that into
 * "1.38 GB of pinned GPU texture memory". Both halves were wrong:
 *
 *   - dirty is not residency, and
 *   - tag 100 (VM_MEMORY_IOACCELERATOR) covers EVERY Metal allocation --
 *     textures, buffers, ring blocks, upload heaps -- not textures alone.
 *
 * The BC textures cannot account for it either: under mip clamp 2 their whole
 * physical backing is only ~210-250 MB. So something else holds the bulk, and
 * until it is named, transcoding is a guess rather than a fix.
 *
 * This census attributes bytes to the DXMT owner that requested them, which is
 * the one thing we can state exactly. Kernel residency is NOT attributable per
 * owner here (Metal owns the pages), so it is deliberately not claimed --
 * MTLDevice_currentAllocatedSize is printed alongside as Metal's own total, to
 * reconcile the sum of our owners against the driver's view and against tag 100.
 * ==========================================================================*/
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "winemetal.h"
#include "Metal.hpp"

namespace dxmt {

enum MemOwner {
  MEMOWN_TEX_PRIVATE = 0,  /* device.newTexture / newSharedTexture   */
  MEMOWN_TEX_LINEAR,       /* buffer-backed textures                 */
  MEMOWN_BUFFER,           /* BufferAllocation (vertex/index/const)  */
  MEMOWN_STAGING_RING,     /* RingBumpState 32MB staging blocks      */
  MEMOWN_INIT_UPLOAD,      /* resource-initializer upload/zero heap  */
  MEMOWN_PRESENTER,        /* gamma LUT etc.                         */
  MEMOWN_CONTEXT,          /* dummy cbuffer etc.                     */
  MEMOWN_COUNT
};

static const char *const kMemOwnerName[MEMOWN_COUNT] = {
  "tex-private", "tex-linear", "buffer", "staging-ring",
  "init-upload", "presenter", "context"
};

/* ml678: 16,597 buffer allocs against 4,802 frees, 1194MB live. That could be
 * genuine geometry, suballocation rounding, or retained rename generations --
 * the aggregate cannot tell them apart. Split by the axes that discriminate:
 * storage mode (private/managed/shared decides WHERE the bytes live), the
 * allocation flags, a size bucket, and how many of them are renames of an
 * existing buffer rather than fresh resources. */
enum BufSizeBucket {
  BUFB_LE_4K = 0, BUFB_LE_64K, BUFB_LE_1M, BUFB_LE_16M, BUFB_GT_16M, BUFB_COUNT
};
static const char *const kBufBucketName[BUFB_COUNT] = {
  "<=4K", "<=64K", "<=1M", "<=16M", ">16M"
};

struct MemCensus {
  std::atomic<uint64_t> requested[MEMOWN_COUNT];  /* cumulative bytes ever asked for */
  std::atomic<uint64_t> live[MEMOWN_COUNT];       /* bytes currently outstanding     */
  std::atomic<uint64_t> peak[MEMOWN_COUNT];       /* high-water of live              */
  std::atomic<uint64_t> allocs[MEMOWN_COUNT];     /* event counts, not log lines     */
  std::atomic<uint64_t> frees[MEMOWN_COUNT];
  std::atomic<uint64_t> report_hwm;               /* last total-live we reported at  */

  /* buffer breakdown (ml678) */
  std::atomic<uint64_t> buf_bucket_live[BUFB_COUNT];
  std::atomic<uint64_t> buf_bucket_n[BUFB_COUNT];
  std::atomic<uint64_t> buf_storage_live[4];     /* WMTResourceStorageMode 0..3 */
  std::atomic<uint64_t> buf_storage_n[4];
  std::atomic<uint64_t> buf_flag_n[8];           /* BufferAllocationFlag bits   */
  std::atomic<uint64_t> buf_renames;             /* Buffer::rename() calls      */
  std::atomic<uint64_t> buf_suballoc_waste;      /* padded bytes we asked for   */

  /* ml680: DynamicBuffer recycling. allocs(16594) ~= renames(19063) says the
   * FIFO almost never hands one back, but that alone cannot distinguish "the
   * GPU is genuinely miles behind" from "the sequence ids are wired wrong".
   * Measuring the lag directly separates them. */
  std::atomic<uint64_t> dyn_reuse_hit;           /* popped a retired allocation */
  std::atomic<uint64_t> dyn_reuse_miss;          /* had to allocate fresh       */
  std::atomic<uint64_t> dyn_fifo_depth;          /* entries waiting             */
  std::atomic<uint64_t> dyn_fifo_depth_max;
  std::atomic<uint64_t> dyn_seq_current;         /* last current_seq_id seen    */
  std::atomic<uint64_t> dyn_seq_coherent;        /* last coherent_seq_id seen   */
  std::atomic<uint64_t> dyn_seq_lag_max;         /* worst current - coherent    */
};

/* ============================ ml681 ====================================
 * ml680 showed the retained set is NOT a simple leak: the FIFO drained
 * repeatedly early (814->125, 958->5, 209->0) and only later began climbing
 * by exactly +204 per sample. So "pop one per call" is not the defect, and a
 * blanket depth cap would be wrong -- an entry whose will_free_at exceeds
 * coherent_seq_id may still be in GPU use, and dropping it is a use-after-free.
 *
 * The open question is WHICH of three shapes this is:
 *   (a) the same allocation inserted twice (duplicate pointers),
 *   (b) a legitimately large in-flight burst fenced by a 1-sequence lag,
 *   (c) an eligible cache that never gets reused after the burst ends.
 * Only (c) is safe to trim, and only the eligible part of it. These
 * per-instance stats separate the three. */
/* ml684: 512 was too small -- ml683 measured 4,659 DynamicBuffer instances, so
 * the table only ever saw the first 512 (all tiny constant buffers) and I twice
 * concluded "the FIFO holds 1MB" from a sample that structurally excluded the
 * dominant instance. Size it above the observed population and report overflow
 * explicitly so the clamp can never masquerade as a census again. */
enum { DYN_CENSUS_SLOTS = 8192 };

struct DynStat {
  std::atomic<uint64_t> length;          /* per-allocation size            */
  std::atomic<uint64_t> push_update;     /* from updateImmediateName()     */
  std::atomic<uint64_t> push_recycle;    /* from recycle()                 */
  std::atomic<uint64_t> pops;
  std::atomic<uint64_t> reuse_hit;
  std::atomic<uint64_t> reuse_miss;
  std::atomic<uint64_t> depth;           /* entries retained now           */
  std::atomic<uint64_t> eligible_n;      /* will_free_at <= coherent       */
  std::atomic<uint64_t> eligible_bytes;
  std::atomic<uint64_t> inelig_n;
  std::atomic<uint64_t> inelig_bytes;
  std::atomic<uint64_t> wfa_min;
  std::atomic<uint64_t> wfa_max;
  std::atomic<uint64_t> dup_ptrs;        /* same allocation seen twice     */
  std::atomic<uint64_t> trimmed_n;       /* released by the eligible trim  */
  std::atomic<uint64_t> trimmed_bytes;
};

/* ============================ ml682 ====================================
 * The retained gigabyte is ~6,769 live buffers of ~165KB. It is NOT the
 * DynamicBuffer recycle FIFOs (those hold 1MB of 16-1136 byte constant
 * buffers) and NOT suballocation rounding (DXMT_PAGE_SIZE is 4096, so the
 * 14,061 rounded buffers cost ~56MB). Two guesses at the owner, both wrong.
 *
 * So stop guessing: record WHERE each BufferAllocation was created from, by
 * return address, and rank live bytes by site. The addresses symbolize offline
 * against d3d11.dll, so this names the caller without threading an enum
 * through every layer. */
enum { BSITE_SLOTS = 64 };
struct BufSite {
  std::atomic<uint64_t> ra;          /* return address of the creator */
  std::atomic<uint64_t> live_bytes;
  std::atomic<uint64_t> live_n;
  std::atomic<uint64_t> total_n;
  std::atomic<uint64_t> total_bytes;
};
extern BufSite g_buf_sites[BSITE_SLOTS];

/* Charge/refund a buffer against its creation site. Linear scan over 64 slots
 * is fine: the number of distinct sites is tiny and fixed by the code. */
void buf_site_add(uint64_t ra, uint64_t bytes);
void buf_site_sub(uint64_t ra, uint64_t bytes);

extern DynStat g_dyn_stats[DYN_CENSUS_SLOTS];
extern std::atomic<uint32_t> g_dyn_next_id;

/* ml684: created vs destroyed vs LIVE. ml683 could only report cumulative
 * creations, which cannot distinguish "the game really has 4,659 dynamic
 * buffers" from "we leak the objects". Those have opposite fixes. */
struct LiveCount {
  std::atomic<uint64_t> created;
  std::atomic<uint64_t> destroyed;
};
extern LiveCount g_live_dynbuf, g_live_buffer, g_live_bufalloc;

/* ml685: trim totals across ALL instances (the per-instance rows only cover the
 * first DYN_CENSUS_SLOTS, and reading a clamped subtotal as a total is exactly
 * how the last three wrong conclusions happened). g_trim_regret counts the
 * allocations that immediately followed a trim on the same instance. */
extern std::atomic<uint64_t> g_trim_total_n, g_trim_total_bytes, g_trim_regret;

void dyn_census_report(void);
void buf_site_report(void);

extern MemCensus g_mem_census;

inline BufSizeBucket
mem_census_bucket(uint64_t n) {
  if (n <= (4ull << 10))  return BUFB_LE_4K;
  if (n <= (64ull << 10)) return BUFB_LE_64K;
  if (n <= (1ull << 20))  return BUFB_LE_1M;
  if (n <= (16ull << 20)) return BUFB_LE_16M;
  return BUFB_GT_16M;
}

/* Charged alongside mem_census_add(MEMOWN_BUFFER, ...) so the split always
 * reconciles against the owner total. */
inline void
mem_census_buffer_detail(uint64_t bytes, uint32_t storage_mode, uint32_t flag_bits, int add) {
  BufSizeBucket b = mem_census_bucket(bytes);
  if (add) {
    g_mem_census.buf_bucket_live[b].fetch_add(bytes, std::memory_order_relaxed);
    g_mem_census.buf_bucket_n[b].fetch_add(1, std::memory_order_relaxed);
    g_mem_census.buf_storage_live[storage_mode & 3].fetch_add(bytes, std::memory_order_relaxed);
    g_mem_census.buf_storage_n[storage_mode & 3].fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 8; i++)
      if (flag_bits & (1u << i)) g_mem_census.buf_flag_n[i].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_mem_census.buf_bucket_live[b].fetch_sub(bytes, std::memory_order_relaxed);
    g_mem_census.buf_storage_live[storage_mode & 3].fetch_sub(bytes, std::memory_order_relaxed);
  }
}

void mem_census_report(const char *why);
void mem_census_set_device(WMT::Device *d);   /* ml678 */

inline void
mem_census_add(MemOwner o, uint64_t bytes) {
  if (!bytes) return;
  g_mem_census.requested[o].fetch_add(bytes, std::memory_order_relaxed);
  g_mem_census.allocs[o].fetch_add(1, std::memory_order_relaxed);
  uint64_t now = g_mem_census.live[o].fetch_add(bytes, std::memory_order_relaxed) + bytes;
  uint64_t prev = g_mem_census.peak[o].load(std::memory_order_relaxed);
  while (now > prev && !g_mem_census.peak[o].compare_exchange_weak(prev, now, std::memory_order_relaxed)) {}

  /* Report on a rising high-water mark so the census is guaranteed to be
   * sampled NEAR THE PEAK -- ml664 quoted a stale lower bound because the run
   * ended between fixed-interval samples. 64MB steps keep it quiet. */
  uint64_t total = 0;
  for (int i = 0; i < MEMOWN_COUNT; i++)
    total += g_mem_census.live[i].load(std::memory_order_relaxed);
  uint64_t hwm = g_mem_census.report_hwm.load(std::memory_order_relaxed);
  if (total > hwm + (64ull << 20) &&
      g_mem_census.report_hwm.compare_exchange_strong(hwm, total, std::memory_order_relaxed))
    mem_census_report("hwm");
}

inline void
mem_census_sub(MemOwner o, uint64_t bytes) {
  if (!bytes) return;
  g_mem_census.frees[o].fetch_add(1, std::memory_order_relaxed);
  g_mem_census.live[o].fetch_sub(bytes, std::memory_order_relaxed);
}

/* Physical bytes a texture will actually occupy. On A15 every BC format is
 * remapped by remap_unsupported_bc() before Metal ever sees it, so the cost is
 * the DECODED size -- which is the entire point of measuring: BC1->RGBA8 is 8x,
 * BC6H->RGBA16F is 8x, BC7->RGBA8 is 4x. Counting compressed bytes here would
 * hide exactly the expansion we are hunting. */
inline uint32_t
mem_census_texel_bytes(enum WMTPixelFormat f) {
  switch (f) {
  case WMTPixelFormatBC4_RUnorm: case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatR8Unorm: case WMTPixelFormatR8Snorm:
  case WMTPixelFormatA8Unorm: case WMTPixelFormatR8Uint: case WMTPixelFormatR8Sint:
    return 1;
  case WMTPixelFormatBC5_RGUnorm: case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatRG8Unorm: case WMTPixelFormatRG8Snorm:
  case WMTPixelFormatR16Float: case WMTPixelFormatR16Uint: case WMTPixelFormatR16Sint:
  case WMTPixelFormatR16Unorm: case WMTPixelFormatR16Snorm:
  case WMTPixelFormatDepth16Unorm:
    return 2;
  case WMTPixelFormatBC6H_RGBFloat: case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatRGBA16Float: case WMTPixelFormatRGBA16Uint:
  case WMTPixelFormatRGBA16Sint: case WMTPixelFormatRGBA16Unorm:
  case WMTPixelFormatRG32Float: case WMTPixelFormatRG32Uint: case WMTPixelFormatRG32Sint:
    return 8;
  case WMTPixelFormatRGBA32Float: case WMTPixelFormatRGBA32Uint: case WMTPixelFormatRGBA32Sint:
    return 16;
  default:
    return 4;   /* RGBA8/BGRA8/BC1/BC2/BC3/BC7-as-RGBA8/depth32/etc. */
  }
}

inline uint64_t
mem_census_texture_bytes(const WMTTextureInfo &info) {
  const uint32_t bpp = mem_census_texel_bytes(info.pixel_format);
  const uint32_t levels = info.mipmap_level_count ? info.mipmap_level_count : 1u;
  const uint32_t slices = info.array_length ? info.array_length : 1u;
  const uint32_t samples = info.sample_count ? info.sample_count : 1u;
  uint64_t total = 0;
  for (uint32_t l = 0; l < levels; l++) {
    uint32_t w = info.width  >> l ? info.width  >> l : 1u;
    uint32_t h = info.height >> l ? info.height >> l : 1u;
    uint32_t d = info.depth  >> l ? info.depth  >> l : 1u;
    total += (uint64_t)w * h * d * bpp;
  }
  return total * slices * samples;
}

} // namespace dxmt
