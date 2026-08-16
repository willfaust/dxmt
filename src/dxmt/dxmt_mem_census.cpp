#include "dxmt_mem_census.hpp"
#include "log/log.hpp"

namespace dxmt {

MemCensus g_mem_census{};
DynStat g_dyn_stats[DYN_CENSUS_SLOTS]{};
BufSite g_buf_sites[BSITE_SLOTS]{};
LiveCount g_live_dynbuf{}, g_live_buffer{}, g_live_bufalloc{};
std::atomic<uint64_t> g_trim_total_n{0}, g_trim_total_bytes{0}, g_trim_regret{0};

static BufSite *
buf_site_find(uint64_t ra) {
  for (int i = 0; i < BSITE_SLOTS; i++) {
    uint64_t cur = g_buf_sites[i].ra.load(std::memory_order_relaxed);
    if (cur == ra) return &g_buf_sites[i];
    if (cur == 0) {
      uint64_t expect = 0;
      if (g_buf_sites[i].ra.compare_exchange_strong(expect, ra, std::memory_order_relaxed))
        return &g_buf_sites[i];
      if (g_buf_sites[i].ra.load(std::memory_order_relaxed) == ra) return &g_buf_sites[i];
    }
  }
  return nullptr;   /* more than 64 distinct sites: report says so via overflow */
}

void
buf_site_add(uint64_t ra, uint64_t bytes) {
  BufSite *s = buf_site_find(ra);
  if (!s) return;
  s->live_bytes.fetch_add(bytes, std::memory_order_relaxed);
  s->live_n.fetch_add(1, std::memory_order_relaxed);
  s->total_n.fetch_add(1, std::memory_order_relaxed);
  s->total_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void
buf_site_sub(uint64_t ra, uint64_t bytes) {
  BufSite *s = buf_site_find(ra);
  if (!s) return;
  s->live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  s->live_n.fetch_sub(1, std::memory_order_relaxed);
}
std::atomic<uint32_t> g_dyn_next_id{0};

/* ml681: top-N by RETAINED ELIGIBLE bytes -- the only category that is both
 * reclaimable and safe to reclaim. Ineligible bytes are reported beside it so
 * "fenced by a burst" is visibly distinguishable from "stranded cache", but
 * they are never candidates for release: an entry with
 * will_free_at > coherent_seq_id may still be in GPU use. */
void
buf_site_report(void) {
  {   /* ml683: sites are LABELS now, not return addresses. Also report what the
       * site rows fail to account for -- if that gap is large, the memory is
       * being created somewhere that never goes through Buffer::allocate. */
    uint64_t sum = 0;
    for (int i = 0; i < BSITE_SLOTS; i++)
      if (g_buf_sites[i].ra.load()) sum += g_buf_sites[i].live_bytes.load();
    uint64_t owner_live = g_mem_census.live[MEMOWN_BUFFER].load(std::memory_order_relaxed);
    ERR("[buf-site] ml683 live buffer bytes by creation site -- sites total ", sum >> 20,
        "MB vs owner census ", owner_live >> 20, "MB (unaccounted ",
        owner_live > sum ? (owner_live - sum) >> 20 : 0, "MB)");
  }
  for (int rank = 0; rank < 10; rank++) {
    int best = -1; uint64_t bestv = 0;
    static int shown[10]; static int nshown;
    if (rank == 0) nshown = 0;
    for (int i = 0; i < BSITE_SLOTS; i++) {
      bool dup = false;
      for (int k = 0; k < nshown; k++) if (shown[k] == i) dup = true;
      if (dup || !g_buf_sites[i].ra.load(std::memory_order_relaxed)) continue;
      uint64_t v = g_buf_sites[i].live_bytes.load(std::memory_order_relaxed);
      if (v > bestv) { bestv = v; best = i; }
    }
    if (best < 0 || !bestv) break;
    shown[nshown++] = best;
    BufSite &s = g_buf_sites[best];
    const char *label = (const char *)(uintptr_t)s.ra.load();
    ERR("[buf-site]   ", label ? label : "?", " live=", s.live_bytes.load() >> 20, "MB / ",
        s.live_n.load(), " buffers  avg=",
        s.live_n.load() ? (s.live_bytes.load() / s.live_n.load()) >> 10 : 0, "KB",
        "  (ever: ", s.total_n.load(), " / ", s.total_bytes.load() >> 20, "MB)");
  }
}

void
dyn_census_report(void) {
  uint32_t n = g_dyn_next_id.load(std::memory_order_relaxed);
  if (n > DYN_CENSUS_SLOTS) n = DYN_CENSUS_SLOTS;
  uint64_t tot_e = 0, tot_i = 0, tot_d = 0, tot_dup = 0, tot_tn = 0, tot_tb = 0;
  for (uint32_t i = 0; i < n; i++) {
    tot_e += g_dyn_stats[i].eligible_bytes.load(std::memory_order_relaxed);
    tot_i += g_dyn_stats[i].inelig_bytes.load(std::memory_order_relaxed);
    tot_d += g_dyn_stats[i].depth.load(std::memory_order_relaxed);
    tot_dup += g_dyn_stats[i].dup_ptrs.load(std::memory_order_relaxed);
    tot_tn += g_dyn_stats[i].trimmed_n.load(std::memory_order_relaxed);
    tot_tb += g_dyn_stats[i].trimmed_bytes.load(std::memory_order_relaxed);
  }
  ERR("[live] ml684 DynamicBuffer created=", g_live_dynbuf.created.load(),
      " destroyed=", g_live_dynbuf.destroyed.load(),
      " LIVE=", g_live_dynbuf.created.load() - g_live_dynbuf.destroyed.load(),
      " | Buffer created=", g_live_buffer.created.load(),
      " destroyed=", g_live_buffer.destroyed.load(),
      " LIVE=", g_live_buffer.created.load() - g_live_buffer.destroyed.load(),
      " | BufferAllocation created=", g_live_bufalloc.created.load(),
      " destroyed=", g_live_bufalloc.destroyed.load(),
      " LIVE=", g_live_bufalloc.created.load() - g_live_bufalloc.destroyed.load());
  {
    uint64_t tn = g_trim_total_n.load(), tb = g_trim_total_bytes.load(), rg = g_trim_regret.load();
    uint64_t miss = g_mem_census.dyn_reuse_miss.load(), hit = g_mem_census.dyn_reuse_hit.load();
    ERR("[trim] ml685 ALL-instance totals: released ", tn, " entries / ", tb >> 20,
        "MB | regret=", rg, " (allocations right after a trim on the same instance) = ",
        miss ? (rg * 100 / miss) : 0, "% of misses | reuse ", hit, "/", hit + miss,
        " = ", (hit + miss) ? (hit * 100 / (hit + miss)) : 0, "%");
  }
  ERR("[dyn-census] ml681 instances-tracked=", n, " instances-TOTAL=",
      g_dyn_next_id.load(std::memory_order_relaxed),
      " (ml682: the first number is CLAMPED to the table size -- ml681 printed"
      " only the clamp and read as if it were the count)",
      " retained entries=", tot_d,
      " eligible=", tot_e >> 20, "MB ineligible=", tot_i >> 20,
      "MB duplicate-ptrs=", tot_dup,
      " | trimmed ", tot_tn, " entries / ", tot_tb >> 20, "MB");
  /* top 8 by eligible bytes */
  for (int rank = 0; rank < 8; rank++) {
    uint32_t best = ~0u; uint64_t bestv = 0;
    static uint32_t shown[8]; static int nshown;
    if (rank == 0) nshown = 0;
    for (uint32_t i = 0; i < n; i++) {
      bool dup = false;
      for (int k = 0; k < nshown; k++) if (shown[k] == i) dup = true;
      if (dup) continue;
      uint64_t v = g_dyn_stats[i].eligible_bytes.load(std::memory_order_relaxed);
      if (v > bestv) { bestv = v; best = i; }
    }
    if (best == ~0u || !bestv) break;
    shown[nshown++] = best;
    DynStat &st = g_dyn_stats[best];
    ERR("[dyn-census]   #", best, " len=", st.length.load(),
        " depth=", st.depth.load(),
        " eligible=", st.eligible_n.load(), "/", st.eligible_bytes.load() >> 20, "MB",
        " inelig=", st.inelig_n.load(), "/", st.inelig_bytes.load() >> 20, "MB",
        " wfa=[", st.wfa_min.load(), "..", st.wfa_max.load(), "]",
        " push(update=", st.push_update.load(), " recycle=", st.push_recycle.load(), ")",
        " reuse=", st.reuse_hit.load(), "/", st.reuse_hit.load() + st.reuse_miss.load(),
        " dup=", st.dup_ptrs.load());
  }
}

/* ml678: ml677's header claimed currentAllocatedSize would be printed "alongside"
 * and then never implemented it, so the census reported REQUESTED capacity while
 * reading as though it were physical footprint. Metal's own number is the only
 * thing that reconciles our owner totals against tag 100. */
/* ml696: ml678 stored &device -- a pointer to a BY-VALUE CONSTRUCTOR PARAMETER,
 * so it dangled the moment BufferAllocation's constructor returned. Every census
 * report since then called currentAllocatedSize() through a stack address that
 * had been reused, which is why this line always printed 0MB and why Thumper
 * died on a SIGTRAP inside a system library on a non-guest thread. WMT::Device
 * is a thin handle wrapper, so hold it by VALUE. */
static WMT::Device g_census_device {};
static bool g_census_device_valid = false;

void
mem_census_set_device(WMT::Device d) { g_census_device = d; g_census_device_valid = true; }

void
mem_census_report(const char *why) {
  uint64_t tl = 0, tp = 0, tr = 0;
  for (int i = 0; i < MEMOWN_COUNT; i++) {
    tl += g_mem_census.live[i].load(std::memory_order_relaxed);
    tp += g_mem_census.peak[i].load(std::memory_order_relaxed);
    tr += g_mem_census.requested[i].load(std::memory_order_relaxed);
  }
  uint64_t metal = g_census_device_valid ? g_census_device.currentAllocatedSize() : 0;
  ERR("[mem-census] ml678 why=", why, " LIVE=", tl >> 20, "MB peak-sum=", tp >> 20,
      "MB requested=", tr >> 20, "MB | METAL currentAllocatedSize=", metal >> 20,
      "MB (authoritative; our LIVE is requested capacity, not residency)");
  for (int i = 0; i < MEMOWN_COUNT; i++) {
    uint64_t l = g_mem_census.live[i].load(std::memory_order_relaxed);
    uint64_t p = g_mem_census.peak[i].load(std::memory_order_relaxed);
    if (!p) continue;
    ERR("[mem-census]   ", kMemOwnerName[i],
        " live=", l >> 20, "MB peak=", p >> 20,
        "MB requested=", g_mem_census.requested[i].load(std::memory_order_relaxed) >> 20,
        "MB allocs=", g_mem_census.allocs[i].load(std::memory_order_relaxed),
        " frees=", g_mem_census.frees[i].load(std::memory_order_relaxed));
  }

  ERR("[mem-census]   buffers by size: ",
      kBufBucketName[0], "=", g_mem_census.buf_bucket_live[0].load() >> 20, "MB/",
      g_mem_census.buf_bucket_n[0].load(), "  ",
      kBufBucketName[1], "=", g_mem_census.buf_bucket_live[1].load() >> 20, "MB/",
      g_mem_census.buf_bucket_n[1].load(), "  ",
      kBufBucketName[2], "=", g_mem_census.buf_bucket_live[2].load() >> 20, "MB/",
      g_mem_census.buf_bucket_n[2].load(), "  ",
      kBufBucketName[3], "=", g_mem_census.buf_bucket_live[3].load() >> 20, "MB/",
      g_mem_census.buf_bucket_n[3].load(), "  ",
      kBufBucketName[4], "=", g_mem_census.buf_bucket_live[4].load() >> 20, "MB/",
      g_mem_census.buf_bucket_n[4].load());
  ERR("[mem-census]   buffers by storage mode: ",
      "shared=", g_mem_census.buf_storage_live[0].load() >> 20, "MB/", g_mem_census.buf_storage_n[0].load(),
      "  managed=", g_mem_census.buf_storage_live[1].load() >> 20, "MB/", g_mem_census.buf_storage_n[1].load(),
      "  private=", g_mem_census.buf_storage_live[2].load() >> 20, "MB/", g_mem_census.buf_storage_n[2].load(),
      "  memoryless=", g_mem_census.buf_storage_live[3].load() >> 20, "MB/", g_mem_census.buf_storage_n[3].load());
  ERR("[mem-census]   buffer flags n[GpuPrivate]=", g_mem_census.buf_flag_n[1].load(),
      " n[CpuWriteCombined]=", g_mem_census.buf_flag_n[2].load(),
      " n[OwnedByCommandList]=", g_mem_census.buf_flag_n[3].load(),
      " n[GpuManaged]=", g_mem_census.buf_flag_n[4].load(),
      " n[SuballocOnePage]=", g_mem_census.buf_flag_n[5].load(),
      " n[CpuPlaced]=", g_mem_census.buf_flag_n[6].load(),
      " | renames=", g_mem_census.buf_renames.load(),
      " suballoc-padding=", g_mem_census.buf_suballoc_waste.load() >> 20, "MB");
  dyn_census_report();
  buf_site_report();
  {
    uint64_t hit = g_mem_census.dyn_reuse_hit.load(), miss = g_mem_census.dyn_reuse_miss.load();
    ERR("[mem-census]   dynamic recycle: reuse-hit=", hit, " fresh=", miss,
        " hit-rate=", (hit + miss) ? (hit * 100 / (hit + miss)) : 0, "%",
        " | fifo depth=", g_mem_census.dyn_fifo_depth.load(),
        " max=", g_mem_census.dyn_fifo_depth_max.load(),
        " | seq current=", g_mem_census.dyn_seq_current.load(),
        " coherent=", g_mem_census.dyn_seq_coherent.load(),
        " lag-max=", g_mem_census.dyn_seq_lag_max.load());
  }
}

} // namespace dxmt
