#include "d3d9_query.hpp"

#include "d3d9_device.hpp"
#include "d3d9_query_contract.hpp"
#include "dxmt_command_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>

#include "d3d9_census.hpp"

namespace dxmt {

MTLD3D9Query::MTLD3D9Query(MTLD3D9Device *device, D3DQUERYTYPE type) : m_device(device), m_type(type) {
  AddRefPrivate();
}

MTLD3D9Query::~MTLD3D9Query() = default;

void
MTLD3D9Query::endOcclusionIfActive() {
  if (m_type != D3DQUERYTYPE_OCCLUSION || !m_visibility_query || m_ended)
    return;
  m_device->FlushDrawBatch();
  auto &queue = m_device->dxmtQueue();
  auto *chunk = queue.CurrentChunk();
  chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
    ctx.endVisibilityResultQuery(std::move(query));
  });
  m_event_seq = queue.CurrentSeqId();
  m_ended = true;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Query::AddRef() {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_AddRef);
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Query::Release() {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_Release);
  // D3D9 Release-at-0 clamp: handed out at public 0 while self-pinned / bound
  // (DXVK clamps every device child); guard the underflow before the decrement.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // App is releasing the query without first issuing END. Synthesize
    // the End BEFORE the device public-refcount drop below, because
    // m_device may be torn down before this query's dtor runs (its
    // own priv ref to us only releases inside the device dtor); by
    // then m_device->FlushDrawBatch / dxmtQueue would UAF.
    {
      // endOcclusionIfActive mutates device draw-batch + queue state, unlike
      // the rest of this Release (atomic-only), so it must hold the device lock
      // under D3DCREATE_MULTITHREADED or it races a concurrent draw thread.
      // Scoped so the lock is dropped before the device pub-ref release below,
      // which on the last ref tears the device (and its lock) down.
      D9DeviceLock lock = m_device->LockDevice();
      endOcclusionIfActive();
    }
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::QueryInterface(REFIID riid, void **ppvObject) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_QueryInterface);
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
    *ppvObject = static_cast<IDirect3DQuery9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetDevice(IDirect3DDevice9 **ppDevice) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_GetDevice);
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

D3DQUERYTYPE STDMETHODCALLTYPE
MTLD3D9Query::GetType() {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_GetType);
  D9DeviceLock lock = m_device->LockDevice();
  return m_type;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Query::GetDataSize() {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_GetDataSize);
  // MADEIRA: constant time, and deliberately lock-free. m_type is written once
  // in the constructor and never again, so there is nothing for the device
  // lock to protect here -- and this is the single hottest const accessor in
  // the frontend: a fence-polling render thread calls it once per GetData.
  // Under D3DCREATE_MULTITHREADED the lock it used to take was a recursive
  // spinlock acquire/release (an atomic CAS plus a release store) for a value
  // that cannot change, on a path the application executes tens of thousands
  // of times a frame.
  //
  // It stays defined here rather than moving to the header as a true inline:
  // gen_d3d9_census.py scans the .cpp files and the census code IS the index
  // into d3d9_census_names[], so lifting one definition out would renumber
  // every method after it.
  //
  // Pure per-type table. Getting these wrong
  // corrupts memory when the app passes a buffer sized to the documented type.
  return d3d9_query_data_size(m_type);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::Issue(DWORD dwIssueFlags) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_Issue);
  D9DeviceLock lock = m_device->LockDevice();
  // D3DISSUE_BEGIN starts a query (only OCCLUSION uses BEGIN; EVENT
  // and TIMESTAMP are END-only). D3DISSUE_END signals the GPU to
  // capture the current value. wine dlls/d3d9 query.c d3d9_query_Issue
  // accepts both flags as a no-op on unsupported types.
  if (dwIssueFlags & D3DISSUE_BEGIN) {
    if (m_type == D3DQUERYTYPE_OCCLUSION) {
      // Begin-after-Begin: drain the in-flight query before starting a new one.
      // Without this, previous query leaks in pending_queries_ (seq_id_end stays
      // ~0uLL so erase-if never matches) and visibility slots burn.
      // d3d11 query.cpp documents the same hazard.
      endOcclusionIfActive();

      // Fresh VisibilityResultQuery on each Begin. Must FlushDrawBatch before emitcc:
      // d3d9 batches draws on calling-thread; FIFO becomes [pre-Begin batch -> begin -> subsequent batches].
      // If begin emitted before flush: [begin -> end -> draws-bundle] coalesces begin==end to same slot,
      // GPU counter never increments. d3d11 has no batching layer, so doesn't need this.
      m_device->FlushDrawBatch();
      m_visibility_query = new VisibilityResultQuery();
      auto &queue = m_device->dxmtQueue();
      auto *chunk = queue.CurrentChunk();
      chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
        ctx.beginVisibilityResultQuery(std::move(query));
      });
    }
    // wined3d sets state = QUERY_BUILDING on any BEGIN regardless of type, so
    // GetData reports S_FALSE until the matching END. Only OCCLUSION opens a
    // GPU counter above; for the other types BEGIN is just the state move.
    m_began = true;
    m_ended = false;
  }
  if (dwIssueFlags & D3DISSUE_END) {
    if (m_type == D3DQUERYTYPE_OCCLUSION && m_visibility_query && !m_ended) {
      // Same drain shape as Begin; flush the in-window batch into
      // the chunk before the end lambda lands, otherwise the visibility
      // window collapses to zero draws.
      m_device->FlushDrawBatch();
      auto &queue = m_device->dxmtQueue();
      auto *chunk = queue.CurrentChunk();
      chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
        ctx.endVisibilityResultQuery(std::move(query));
      });
      // Snapshot the chunk-in-flight seq id so GetData(FLUSH) knows
      // whether the issuing chunk has been committed yet. The
      // VisibilityResultQuery's getValue() handles the GPU-completion
      // check itself via the readback issue path.
      m_event_seq = queue.CurrentSeqId();
    }
    m_ended = true;
    // MADEIRA: a fresh result window. The next GetData owns the one-off
    // D3DGETDATA_FLUSH submit and starts a fresh poll ramp; see the
    // m_flushed_since_issue / kPollsBeforePark comments in the header.
    m_flushed_since_issue = false;
    m_polls_since_issue = 0;
    m_last_pending_poll_ns = 0;
    m_poll_burst = 0;
    if (m_type == D3DQUERYTYPE_EVENT || m_type == D3DQUERYTYPE_OCCLUSION) {
      m_issue_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      census::queryIssued();
    }
    if (m_type == D3DQUERYTYPE_EVENT) {
      // Snapshot the chunk-in-flight's seq id. All prior calling-thread
      // work landed on this chunk (or earlier); once cpu_coherent
      // advances past m_event_seq, the GPU has completed the work the
      // app is waiting on. dxmt's queue advances ready_for_encode on
      // CommitCurrentChunk and cpu_coherent on GPU-retire; matches the
      // DXVK DxvkGpuEvent + d3d11's EventQuery shape.
      m_event_seq = m_device->dxmtQueue().CurrentSeqId();
    }
    if (m_type == D3DQUERYTYPE_TIMESTAMP) {
      // Host-side approximation: monotonic ns since epoch. GetData
      // returns this raw value; TIMESTAMPFREQ reports 1 GHz so apps
      // computing (end - start) / freq see seconds-elapsed directly.
      m_timestamp_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
    }
  }
  return D3D_OK;
}

// MADEIRA: [d3d9-query] bookkeeping for the moment a query's result becomes
// available. Folded out of GetData because both of the exits that can observe
// it (the immediate hit and the post-park retry) have to run it. The poll
// itself is counted by the caller, once per GetData call, so that a parked
// poll that then succeeds is one poll and not two.
void
MTLD3D9Query::noteCompletion() {
  if (!m_issue_ns)
    return;
  uint64_t now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  census::queryCompleted(now - m_issue_ns, m_polls_since_issue);
  // One completion is reported once: a caller that keeps polling a finished
  // query (several of them do, to re-read the pixel count) must not multiply
  // the completion count and drag the average latency to zero.
  m_issue_ns = 0;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  D3D9_CENSUS(D3D9_CENSUS_MTLD3D9Query_GetData);
  // MADEIRA: the shape of this function is the fix for the census finding that
  // a 32-bit title spent ~85k GetData calls per frame (at ~30fps, i.e. the
  // single busiest slot in the whole frontend) spinning on a GPU fence.
  //
  // Two things were wrong, and only the second one is dxmt's own doing:
  //
  //  1. The poll was cheap but not free -- a device-lock acquire, a census
  //     add, a nested virtual GetDataSize (which took the lock AGAIN and
  //     counted itself, which is why GetDataSize's census count matched
  //     GetData's exactly: the application was barely calling it at all), and
  //     a re-read of the queue's submission state. Multiplied by 85k that is
  //     real frame time. getDataImpl no longer calls the virtual, and
  //     GetDataSize no longer locks.
  //
  //  2. The loop had no back-pressure at all. Between the FLUSH submit and
  //     the GPU retiring that command buffer there is nothing the calling
  //     thread can usefully do, but it burned a core asking, on a device
  //     where the encode and finish threads want that core. GetData may not
  //     block -- S_FALSE while the GPU is busy is the contract, and an
  //     application is free to go do something else on it -- so the answer is
  //     a CAPPED park, not a wait: after kPollsBeforePark consecutive
  //     S_FALSEs, hand the core away for at most kPollParkNanos and then
  //     return S_FALSE regardless. The caller keeps polling; it just does so
  //     a few hundred times a frame instead of a hundred thousand, and it
  //     sees the completion within tens of microseconds of it happening.
  //
  // The park happens with the device lock DROPPED. Holding an API lock across
  // it would be a deadlock hazard in the other direction: under
  // D3DCREATE_MULTITHREADED a second application thread trying to enter any
  // entry point would spin on D9RecursiveSpinlock for the whole park.
  uint64_t park_seq = 0;
  {
    D9DeviceLock lock = m_device->LockDevice();
    HRESULT hr = getDataImpl(pData, dwSize, dwGetDataFlags);
    if (hr != S_FALSE) {
      census::queryPoll(true, false);
      noteCompletion();
      return hr;
    }
    m_polls_since_issue++;
    // ml1150: the old lifetime count penalized an asynchronous culling loop
    // after its third check, even if those checks were a frame apart. Keep
    // cooperative back-pressure for tight polling only. A 250us gap starts
    // a new burst; completed queries never pay for this clock read.
    static const bool adaptive = [] {
      const char *value = std::getenv("DXMT_D9_QUERY_ADAPTIVE");
      bool enabled = !value || std::strcmp(value, "0");
      Logger::warn(str::format("[query-pacing] ml1150 burst-aware=", enabled,
                              " (DXMT_D9_QUERY_ADAPTIVE=0 restores lifetime polling)"));
      return enabled;
    }();
    uint32_t pending_polls = m_polls_since_issue;
    if (adaptive) {
      uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      m_poll_burst = m_last_pending_poll_ns && now - m_last_pending_poll_ns <= 250000
          ? std::min(m_poll_burst + 1, kPollsBeforePark + 1) : 1;
      m_last_pending_poll_ns = now;
      pending_polls = m_poll_burst;
    }
    // Only the two GPU-backed types have a fence to park on. m_event_seq == 0
    // means Issue(END) never ran for this type (TIMESTAMP and friends resolve
    // on the calling thread), so there is nothing to wait for.
    if (pending_polls <= kPollsBeforePark || !m_event_seq ||
        (m_type != D3DQUERYTYPE_EVENT && m_type != D3DQUERYTYPE_OCCLUSION)) {
      census::queryPoll(false, false);
      return S_FALSE;
    }
    park_seq = m_event_seq;
  }

  // Both backends resolve off the same watermark: the finish thread retires
  // command buffer N (which is what publishes an OCCLUSION query's readback,
  // in CommandChunk::reset) and only then signals cpu_coherent to N, so
  // CoherentSeqId() >= m_event_seq is exactly "the work this query was issued
  // behind is done" for EVENT and OCCLUSION alike.
  bool ready = m_device->dxmtQueue().WaitCPUFenceBounded(park_seq, kPollParkNanos);
  if (!ready) {
    census::queryPoll(false, true);
    return S_FALSE;
  }

  // The fence landed inside the budget, so answer now instead of making the
  // caller come back for it -- that is the difference between "the spin is
  // bounded" and "the spin is bounded AND the result is timely". getDataImpl
  // cannot re-submit here: the issuing chunk was committed on the first poll.
  D9DeviceLock lock = m_device->LockDevice();
  HRESULT hr = getDataImpl(pData, dwSize, dwGetDataFlags);
  census::queryPoll(hr != S_FALSE, true);
  if (hr != S_FALSE)
    noteCompletion();
  return hr;
}

HRESULT
MTLD3D9Query::getDataImpl(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  // MADEIRA: the free inline table, not the virtual GetDataSize override. The
  // override is a counted census slot and used to re-take the device lock, so
  // calling it from here made every poll pay for a second locked virtual call
  // and double-counted itself in the census (see GetDataSize).
  const DWORD data_size = d3d9_query_data_size(m_type);

  // wined3d query.c state machine, surfaced through dlls/d3d9 query.c GetData.
  // QUERY_BUILDING (BEGIN issued, END pending): the result window is still
  // open, so the value is not yet available: S_FALSE.
  if (m_began && !m_ended)
    return S_FALSE;

  // QUERY_CREATED (never issued END): wined3d returns INVALIDCALL, which the
  // d3d9 wrapper rewrites to S_OK after poisoning the caller buffer: zero the
  // whole span, then 0xdd the leading data_size bytes. An app polling a freshly
  // created query must observe S_OK, not a fatal INVALIDCALL.
  if (!m_ended) {
    if (pData)
      d3d9_poison_created_query(pData, dwSize, data_size);
    return S_OK;
  }

  // QUERY_SIGNALLED: END issued. Per-type readiness + result copy follow.
  // pData=NULL probes readiness; forward-commit EVENT/OCCLUSION regardless of
  // D3DGETDATA_FLUSH to prevent spin-loop deadlock.

  // EVENT queries are now backed by the queue's coherent-seq counter.
  // Commit the recorded chunk whenever it hasn't submitted yet (event_seq
  // still == CurrentSeqId), regardless of D3DGETDATA_FLUSH: an app that issues
  // an EVENT then immediately polls without the flag (and does no other GPU
  // work) would otherwise spin forever on a chunk that never submits. The
  // readiness model mirrors DXVK's DxvkGpuEvent::test; the unconditional submit
  // is dxmt's own (DXVK's forward-progress submit is FLUSH-gated, but its CS
  // thread submits independently so a non-FLUSH spin-poll cannot deadlock it).
  if (m_type == D3DQUERYTYPE_EVENT && m_event_seq != 0) {
    auto &queue = m_device->dxmtQueue();
    // Submit issuing chunk for forward progress regardless of FLUSH flag.
    // Apps polling without D3DGETDATA_FLUSH would deadlock: chunk never
    // submits, 100% CPU spin with encode/finish threads idle. D3D9 contract.
    //
    // MADEIRA: once per Issue, not once per poll. Present is the only commit
    // point a normal d3d9 frame otherwise has (d3d9_swapchain.cpp:1114; the
    // rest are teardown, Reset, forceFlushAndCommit and the synchronous
    // readbacks), so without this submit a fence issued mid-frame could not
    // possibly retire before the frame ended -- the poll loop would spin for a
    // whole frame by construction. With it, the very first poll puts the work
    // in flight. Repeating the submit on later polls does no good and some
    // harm: each one cuts the chunk the application is still filling into
    // another command buffer, and can block the calling thread on the
    // 32-entry chunk ring (CommitCurrentChunk's chunk_ongoing.wait).
    if (!m_flushed_since_issue) {
      m_flushed_since_issue = true;
      if (queue.CurrentSeqId() == m_event_seq) {
        m_device->FlushDrawBatch();
        m_device->commitCurrentChunkTimed(2);
        census::queryFlushed();
      }
    }
    if (queue.CoherentSeqId() < m_event_seq) {
      // Caller polling readiness with no-buffer call: S_FALSE on
      // still-pending matches the per-spec contract.
      if (pData == nullptr || dwSize == 0)
        return S_FALSE;
      BOOL signaled = FALSE;
      std::memcpy(pData, &signaled, dwSize < sizeof(signaled) ? dwSize : sizeof(signaled));
      return S_FALSE;
    }
  }

  // OCCLUSION FLUSH + readiness gate: if the issuing chunk hasn't
  // been committed yet, commit it so the GPU has a chance to retire
  // the visibility-result readback. Without this an app that issues
  // an occlusion query and immediately polls; without doing any
  // other GPU work; would spin forever waiting for the chunk that
  // never committed. Same shape as the EVENT flush above. The
  // readiness probe (pData==null) returns S_FALSE when the query
  // isn't done: checked before the null-buffer short-circuit below.
  if (m_type == D3DQUERYTYPE_OCCLUSION && m_visibility_query) {
    auto &queue = m_device->dxmtQueue();
    // Same forward-progress fix as the EVENT path above: commit the issuing
    // chunk regardless of D3DGETDATA_FLUSH so a spin-poll without the flag
    // can't deadlock on a chunk that never submits. Flush first, like the
    // EVENT arm: draws queued AFTER Issue(END) (the standard cull loop keeps
    // drawing before it polls) are still pending with pod snapshots on the
    // current chunk. Committing without draining them retires that chunk while
    // those snapshots are live, and the encode thread reads freed ring memory
    // when it resolves the draws on the next chunk.
    //
    // MADEIRA: one submit per Issue, for the reasons spelled out on the EVENT
    // arm above. It matters more here: a visibility-cull loop issues hundreds
    // of occlusion queries per frame and polls them interleaved, so a per-poll
    // submit would have turned one frame into hundreds of command buffers.
    if (!m_flushed_since_issue) {
      m_flushed_since_issue = true;
      if (queue.CurrentSeqId() == m_event_seq) {
        m_device->FlushDrawBatch();
        m_device->commitCurrentChunkTimed(2);
        census::queryFlushed();
      }
    }
    uint64_t probe = 0;
    if (!m_visibility_query->getValue(&probe)) {
      if (pData == nullptr || dwSize == 0)
        return S_FALSE;
    }
  }

  // Caller can pass pData=null + dwSize=0 to poll readiness without
  // copying the result. D3D_OK means "result is available". Real
  // backed queries would return S_FALSE while the GPU is still
  // running; the stub is always-ready for non-EVENT (we have no async
  // work).
  if (pData == nullptr || dwSize == 0)
    return D3D_OK;

  // Same per-type table as GetDataSize. Copy the smaller of dwSize
  // and the type's size; wined3d truncates if the app passes a
  // smaller buffer.
  switch (m_type) {
  case D3DQUERYTYPE_OCCLUSION: {
    // GPU-backed sample count via MTLVisibilityResultMode. The
    // VisibilityResultQuery accumulates the counter across all encoders the
    // begin/end straddles; getValue returns true once the issuing chunk's
    // readback has fired. wined3d stores the count as a uint64 and GetData
    // copies the full width (min(size, 8)); GetDataSize still reports the
    // documented sizeof(DWORD), so an app reading 8 bytes sees the high dword.
    uint64_t pixels64 = 0;
    if (m_visibility_query && !m_visibility_query->getValue(&pixels64)) {
      // Not ready yet. FLUSH already committed above; spin-poll is the app's
      // responsibility per the D3D9 contract. Leave the caller's buffer
      // untouched (wined3d and DXVK write only on the S_OK path): an app that
      // reuses last frame's pixel count on S_FALSE, a common conservative-cull
      // pattern, must not see a spurious 0. pData is non-null here (the null /
      // zero-size poll returned above).
      return S_FALSE;
    }
    std::memcpy(pData, &pixels64, std::min<DWORD>(dwSize, sizeof(pixels64)));
    return D3D_OK;
  }
  case D3DQUERYTYPE_EVENT: {
    BOOL signaled = TRUE;
    std::memcpy(pData, &signaled, dwSize < sizeof(signaled) ? dwSize : sizeof(signaled));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMP: {
    // Host-side monotonic ns capture from Issue(END). Real GPU
    // timestamps via MTLCounterSampleBuffer are a follow-up; the
    // host-side delta is within a few-ms of GPU delta for typical
    // frame-paced work; apps use this for profiling overlays and
    // FPS counters which tolerate the imprecision.
    UINT64 ticks = m_timestamp_ns;
    std::memcpy(pData, &ticks, dwSize < sizeof(ticks) ? dwSize : sizeof(ticks));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMPDISJOINT: {
    // Host steady_clock is monotonic + non-disjoint by construction;
    // FALSE always. Real GPU timestamps could report TRUE if the GPU
    // clock skipped (power-state transitions, etc.); not relevant on
    // the host-side path.
    BOOL disjoint = FALSE;
    std::memcpy(pData, &disjoint, dwSize < sizeof(disjoint) ? dwSize : sizeof(disjoint));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMPFREQ: {
    // 1 GHz, which follows from TIMESTAMP being a host nanosecond clock here
    // rather than a GPU tick counter: reporting the frequency the timestamps
    // are actually in is what makes a TIMESTAMP delta divide out to seconds.
    // A reference with real GPU timestamps derives this from the hardware
    // tick period instead.
    UINT64 freq = 1000000000ull;
    std::memcpy(pData, &freq, dwSize < sizeof(freq) ? dwSize : sizeof(freq));
    return D3D_OK;
  }
  default:
    return D3DERR_INVALIDCALL;
  }
}

} // namespace dxmt
