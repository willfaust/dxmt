#include "dxmt_dynamic.hpp"
#include "dxmt_texture.hpp"
#include "dxmt_mem_census.hpp"

namespace dxmt {
DynamicBuffer::DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags) :
    buffer(buffer),
    flags_(flags),
    name_(buffer->current()) {
  /* ml681 */
  g_live_dynbuf.created.fetch_add(1, std::memory_order_relaxed);   /* ml684 */
  census_id_ = g_dyn_next_id.fetch_add(1, std::memory_order_relaxed);
  if (census_id_ < DYN_CENSUS_SLOTS)
    g_dyn_stats[census_id_].length.store(buffer ? buffer->length() : 0, std::memory_order_relaxed);
}

void
DynamicBuffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicBuffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u) {
    g_live_dynbuf.destroyed.fetch_add(1, std::memory_order_relaxed);   /* ml684 */
    delete this;
  }
};

Rc<BufferAllocation>
DynamicBuffer::allocate(uint64_t coherent_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  /* ml685 TRIM REGRET: did we release something and then immediately have to
   * allocate again? That -- not the raw churn count -- is what says the 64-entry
   * reserve is too tight. Cleared every call, set only by an actual trim. */
  const bool was_trimmed = trimmed_last_;
  trimmed_last_ = false;
  Rc<BufferAllocation> ret;
  for (;;) {
    if (fifo.empty()) {
      break;
    }
    auto entry = fifo.front();
    if (entry.will_free_at > coherent_seq_id) {
      break;
    }
    ret = std::move(entry.allocation);
    fifo.pop_front();
    break;
  }
  /* ml681: walk the retained set while we already hold the lock. eligible vs
   * ineligible is the discriminator; min/max will_free_at shows whether the
   * tail is fenced by one burst or spread across many sequences. */
  /* ml685 DESIGN BUG, found by accident: this whole block -- INCLUDING THE TRIM
   * -- used to sit behind `census_id_ < DYN_CENSUS_SLOTS`. Allocator behaviour
   * was therefore decided by whether an object happened to fit in a DIAGNOSTIC
   * table. With the table at 512 the trim only ever saw tiny constant buffers,
   * freed nothing, and I read that as "nothing to free". Raising the table to
   * 8192 for observability silently switched the trim on for the instances that
   * held ~1GB, which is the entire reason memory dropped.
   *
   * The walk and the trim now run for EVERY instance; only the statistics are
   * gated. Behaviour must never depend on diagnostic capacity. */
  {
    const bool stats = census_id_ < DYN_CENSUS_SLOTS;
    uint64_t en = 0, eb = 0, in_n = 0, in_b = 0, lo = ~0ull, hi = 0, dup = 0;
    const void *prev_seen[8] = {nullptr};
    unsigned pi = 0;
    for (const auto &e : fifo) {
      uint64_t bytes = e.allocation.ptr() ? e.allocation->census_bytes_ : 0;
      if (e.will_free_at <= coherent_seq_id) { en++; eb += bytes; }
      else                                   { in_n++; in_b += bytes; }
      if (e.will_free_at < lo) lo = e.will_free_at;
      if (e.will_free_at > hi) hi = e.will_free_at;
      for (unsigned k = 0; k < 8; k++)
        if (prev_seen[k] && prev_seen[k] == (const void *)e.allocation.ptr()) { dup++; break; }
      prev_seen[pi++ & 7] = (const void *)e.allocation.ptr();
    }
    if (stats) {
      DynStat &st = g_dyn_stats[census_id_];
      st.depth.store(fifo.size(), std::memory_order_relaxed);
      st.eligible_n.store(en, std::memory_order_relaxed);
      st.eligible_bytes.store(eb, std::memory_order_relaxed);
      st.inelig_n.store(in_n, std::memory_order_relaxed);
      st.inelig_bytes.store(in_b, std::memory_order_relaxed);
      st.wfa_min.store(lo == ~0ull ? 0 : lo, std::memory_order_relaxed);
      st.wfa_max.store(hi, std::memory_order_relaxed);
      if (dup) st.dup_ptrs.fetch_add(dup, std::memory_order_relaxed);
    }

    /* ml681 SAFE TRIM. Release only entries the GPU has demonstrably finished
     * with (will_free_at <= coherent_seq_id), and only the surplus beyond a
     * working reserve. Entries are pushed in non-decreasing will_free_at order,
     * so the eligible ones are exactly the front run -- stopping at the first
     * ineligible entry is what makes this safe.
     *
     * A blanket depth cap would be a use-after-free: an entry whose
     * will_free_at still exceeds coherent_seq_id can be in GPU use right now. */
    enum { DYN_KEEP_ELIGIBLE = 64 };
    if (en > DYN_KEEP_ELIGIBLE) {
      uint64_t drop = en - DYN_KEEP_ELIGIBLE, freed = 0, freed_n = 0;
      while (drop && !fifo.empty()) {
        const QueueEntry &f = fifo.front();
        if (f.will_free_at > coherent_seq_id) break;   /* never touch fenced */
        freed += f.allocation.ptr() ? f.allocation->census_bytes_ : 0;
        freed_n++;
        fifo.pop_front();
        drop--;
      }
      if (freed_n) {
        trimmed_last_ = true;                  /* ml685: for trim-regret */
        g_trim_total_n.fetch_add(freed_n, std::memory_order_relaxed);
        g_trim_total_bytes.fetch_add(freed, std::memory_order_relaxed);
        if (stats) {
          DynStat &st = g_dyn_stats[census_id_];
          st.trimmed_n.fetch_add(freed_n, std::memory_order_relaxed);
          st.trimmed_bytes.fetch_add(freed, std::memory_order_relaxed);
        }
      }
    }
  }

  /* ml684: report on SEQUENCE ADVANCEMENT, not only on memory high-water marks.
   * Every prior census fired on a rising HWM, so once the footprint plateaued
   * near jetsam the reports stopped -- exactly when the retained set was most
   * interesting. Sampling on the fence means the burst is observed while it is
   * happening. */
  {
    static std::atomic<uint64_t> last_reported{0};
    uint64_t lr = last_reported.load(std::memory_order_relaxed);
    if (coherent_seq_id >= lr + 30 &&
        last_reported.compare_exchange_strong(lr, coherent_seq_id, std::memory_order_relaxed))
      mem_census_report("seq");
  }

  /* ml680 */
  g_mem_census.dyn_seq_coherent.store(coherent_seq_id, std::memory_order_relaxed);
  g_mem_census.dyn_fifo_depth.store(fifo.size(), std::memory_order_relaxed);
  {
    uint64_t d = fifo.size(), mx = g_mem_census.dyn_fifo_depth_max.load(std::memory_order_relaxed);
    while (d > mx && !g_mem_census.dyn_fifo_depth_max.compare_exchange_weak(mx, d, std::memory_order_relaxed)) {}
    uint64_t cur = g_mem_census.dyn_seq_current.load(std::memory_order_relaxed);
    if (cur > coherent_seq_id) {
      uint64_t lag = cur - coherent_seq_id, lm = g_mem_census.dyn_seq_lag_max.load(std::memory_order_relaxed);
      while (lag > lm && !g_mem_census.dyn_seq_lag_max.compare_exchange_weak(lm, lag, std::memory_order_relaxed)) {}
    }
  }
  if (!ret.ptr()) {
    g_mem_census.dyn_reuse_miss.fetch_add(1, std::memory_order_relaxed);
    if (was_trimmed) g_trim_regret.fetch_add(1, std::memory_order_relaxed);   /* ml685 */
    if (census_id_ < DYN_CENSUS_SLOTS) g_dyn_stats[census_id_].reuse_miss.fetch_add(1, std::memory_order_relaxed);
    ret = buffer->allocate(flags_, "DynamicBuffer::allocate");
  } else {
    g_mem_census.dyn_reuse_hit.fetch_add(1, std::memory_order_relaxed);
    if (census_id_ < DYN_CENSUS_SLOTS) {
      g_dyn_stats[census_id_].reuse_hit.fetch_add(1, std::memory_order_relaxed);
      g_dyn_stats[census_id_].pops.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return ret;
}

void
DynamicBuffer::updateImmediateName(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation, uint32_t suballocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  g_mem_census.dyn_seq_current.store(current_seq_id, std::memory_order_relaxed);   /* ml680 */
  if (!owned_by_command_list_ && census_id_ < DYN_CENSUS_SLOTS)
    g_dyn_stats[census_id_].push_update.fetch_add(1, std::memory_order_relaxed);   /* ml681 */
  if (!owned_by_command_list_)
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  name_suballocation_ = suballocation;
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicBuffer::recycle(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  if (census_id_ < DYN_CENSUS_SLOTS)
    g_dyn_stats[census_id_].push_recycle.fetch_add(1, std::memory_order_relaxed);   /* ml681 */
  fifo.push_back(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

uint32_t
DynamicBuffer::nextSuballocation() {
  if (name_->hasSuballocatoin(name_suballocation_ + 1)) {
    return ++name_suballocation_;
  }
  return 0;
}

DynamicLinearTexture::DynamicLinearTexture(Texture *texture, Flags<TextureAllocationFlag> flags) :
    texture(texture),
    flags_(flags),
    name_(texture->current()) {}

void
DynamicLinearTexture::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicLinearTexture::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<TextureAllocation>
DynamicLinearTexture::allocate(uint64_t coherent_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  Rc<TextureAllocation> ret;
  for (;;) {
    if (fifo.empty()) {
      break;
    }
    auto entry = fifo.front();
    if (entry.will_free_at > coherent_seq_id) {
      break;
    }
    ret = std::move(entry.allocation);
    fifo.pop_front();
    break;
  }
  if (!ret.ptr())
    ret = texture->allocate(flags_);
  return ret;
}

void
DynamicLinearTexture::updateImmediateName(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!owned_by_command_list_)
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicLinearTexture::recycle(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  fifo.push_back(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

} // namespace dxmt
