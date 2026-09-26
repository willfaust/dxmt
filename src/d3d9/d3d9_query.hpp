#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "dxmt_occlusion_query.hpp"
#include "rc/util_rc_ptr.hpp"

namespace dxmt {

class MTLD3D9Device;

// IDirect3DQuery9: only OCCLUSION and EVENT are used in practice.
// OCCLUSION uses MTLVisibilityResultMode; EVENT is backed by
// queue coherent-seq watermark; other types stub out.
class MTLD3D9Query final : public ComObject<IDirect3DQuery9> {
public:
  MTLD3D9Query(MTLD3D9Device *device, D3DQUERYTYPE type);
  ~MTLD3D9Query();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override;
  DWORD STDMETHODCALLTYPE GetDataSize() override;
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override;
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) override;

public:
  // MADEIRA: how long a GetData poll that found the GPU still busy may park
  // the calling thread before it MUST come back with S_FALSE, and how many
  // consecutive S_FALSE polls are answered at full speed first.
  //
  // The cap is what keeps GetData non-blocking: 100us is under a tenth of a
  // 8.3ms frame at 120Hz, so even a caller that treats S_FALSE as "do
  // something else now" loses nothing it could have used, while a caller that
  // spins gets its answer within ~100us of the GPU actually finishing instead
  // of after another 250 full-speed polls. The first few polls are answered
  // immediately because a fence the application issued a moment ago is often
  // already signalled, and paying a yield for that would be a regression.
  static constexpr uint64_t kPollParkNanos = 100 * 1000;
  static constexpr uint32_t kPollsBeforePark = 2;

private:
  // GetData's body; the public override is a thin stall-instrument wrapper
  // around it (poll count at entry, S_FALSE count at exit).
  HRESULT getDataImpl(void *pData, DWORD dwSize, DWORD dwGetDataFlags);
  // MADEIRA: [d3d9-query] bookkeeping for the moment the result lands.
  void noteCompletion();
  // Drains an in-flight OCCLUSION visibility query: emits the same
  // endVisibilityResultQuery chunk lambda an explicit Issue(END) would,
  // matched 1:1 with the prior Begin so dxmt_context's pending_queries_
  // / active_visibility_query_count_ bookkeeping balances. No-op when
  // the query isn't OCCLUSION, isn't currently Begun, or has already
  // been Ended. Called from Issue(BEGIN) (Begin-after-Begin path) and
  // from the destructor (Release-before-End path).
  void endOcclusionIfActive();

  MTLD3D9Device *m_device;
  D3DQUERYTYPE m_type;
  // Query state machine mirrors wined3d query.c: a fresh query is CREATED, a
  // D3DISSUE_BEGIN moves it to BUILDING, a D3DISSUE_END to SIGNALLED. m_began
  // records the BEGIN so GetData can separate a never-issued query (CREATED:
  // the d3d9 wrapper turns wined3d's INVALIDCALL into a poisoned-buffer S_OK)
  // from one mid-window (BUILDING: S_FALSE).
  bool m_began = false;
  // Whether D3DISSUE_END has run (QUERY_SIGNALLED). wined3d query.c enforces
  // the CREATED -> BUILDING -> SIGNALLED order.
  bool m_ended = false;
  // EVENT-query GPU completion seq. Captured at Issue(D3DISSUE_END):
  // the queue's CurrentSeqId: the chunk-in-flight that all-prior work
  // up to the END landed in. GetData polls CoherentSeqId against this
  // value; when GPU-coherent >= captured, the event is signaled. Zero
  // before any END so the EVENT type defaults to "signaled" if the
  // app polls before issuing; matches the prior stub shape.
  uint64_t m_event_seq = 0;
  // TIMESTAMP-query host-side time capture. Real GPU timestamps via
  // MTLCounterSampleBuffer are an infrastructure follow-up; the host-
  // side approximation is a calling-thread monotonic-clock snapshot at
  // Issue(D3DISSUE_END), reported back in nanoseconds. Apps that use
  // timestamps for profiling (frame-rate overlays, in-game
  // counters) compute deltas: a host-side delta is within a few-ms
  // of the GPU delta for typical frame-paced work, vs. the zero-ticks
  // stub that made every elapsed measurement appear instantaneous.
  uint64_t m_timestamp_ns = 0;
  // OCCLUSION-query GPU counter allocated at Issue(BEGIN).
  // beginVisibilityResultQuery/endVisibilityResultQuery carve out
  // heap offset and manage encoder's setVisibilityResultMode.
  // Null until Begin; read returns 0 in that case.
  Rc<VisibilityResultQuery> m_visibility_query;
  // MADEIRA: the issuing chunk has been committed since the last
  // Issue(D3DISSUE_END). D3DGETDATA_FLUSH means "submit the work this query
  // is waiting on", which is a ONE-OFF: once the chunk is in the queue's
  // hands there is nothing left for a later poll to submit, and committing
  // again would cut the *next* chunk short. Before this flag the guard was
  // implicit (`CurrentSeqId() == m_event_seq` can only be true once), which
  // was correct but re-read queue state on every one of tens of thousands of
  // polls per frame and could not be counted. Cleared by Issue(END).
  bool m_flushed_since_issue = false;
  // MADEIRA: consecutive S_FALSE polls since Issue(D3DISSUE_END). Drives the
  // kPollsBeforePark ramp and is reported as polls_per_completion on the
  // [d3d9-query] line.
  uint32_t m_polls_since_issue = 0;
  // ml1150: readiness checks spread across frames are not a busy loop.
  uint64_t m_last_pending_poll_ns = 0;
  uint32_t m_poll_burst = 0;
  // MADEIRA: steady_clock ns at Issue(D3DISSUE_END), so the instrument can
  // report the real Issue -> completion latency (the number that says whether
  // the poll loop is waiting on the GPU or on dxmt's own submission policy).
  uint64_t m_issue_ns = 0;
  // Self-pin shape mirrors MTLD3D9StateBlock: keep `this` alive
  // across the public 1->0 transition long enough for the override
  // to drop the device pin safely.
  bool m_self_pinned = true;
};

} // namespace dxmt
