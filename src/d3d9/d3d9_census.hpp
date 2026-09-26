/*
 * MADEIRA (WOW64_DESIGN.md section 8.4, measurement 1): [d3d9-census].
 *
 * This file is Madeira's own work, distributed under GPL-3.0-or-later.
 * See research/dxmt/LICENSE-MADEIRA.md.
 *
 * Section 8 has to know how many D3D9 vtable calls a real workload makes per
 * frame, and which ones, before it can decide whether a synchronous 32-bit
 * shim over a native ARM64 frontend is viable: the answer is (calls/frame) x
 * (cost of one unix call, measured by unixcall-bench-x86.exe). A COM frontend
 * has no choke point to instrument, so there is one counter at the top of
 * every one of the 317 STDMETHODCALLTYPE definitions in src/d3d9.
 *
 * Cost. ONE relaxed 32-bit atomic add per call, behind a plain-bool load that
 * a branch predictor sees as never-taken-either-way. The counters are
 * uint32_t rather than uint64_t on purpose: on i386 a 64-bit atomic add is a
 * `lock cmpxchg8b` retry loop, which is exactly the kind of overhead that
 * would distort the measurement it exists to take. A single method would have
 * to be called 4.29e9 times to wrap; at the ~13k calls/frame and 20 fps
 * section 8.4 assumes, the busiest one wraps after about 4.5 hours, and the
 * summaries are printed continuously long before that.
 *
 * The call sites and d3d9_census_names.h are BOTH emitted by
 * gen_d3d9_census.py from one scan, so a code cannot name a method other than
 * the one that incremented it -- the same contract the winemetal dispatch
 * table's slot numbers have (see src/winemetal/wmt_api_census.c).
 */
#pragma once

#include "d3d9_census_names.h"

#include <atomic>
#include <cstdint>

namespace dxmt::census {

/* Written once, during this DLL's static initialisation, and only read after
 * that -- so the hot path is a plain load, not an atomic one. */
extern bool g_on;

/* MADEIRA ml999: PER-THREAD COUNTERS, BECAUSE THE INSTRUMENT WAS DISTORTING
 * WHAT IT MEASURED.
 *
 * The file header above budgets "ONE relaxed 32-bit atomic add per call" and
 * calls that negligible.  It is not, on this target.  d3d9-emulated.dll is
 * i386 code running under FEX, so `lock xadd' is not one instruction: it is a
 * TSO-ordered read-modify-write that the JIT lowers to an exclusive-monitor
 * sequence.  [d3d9-census] measures 5540 D3D9 calls per frame in the
 * open-world title, and every one of them paid this add PLUS a second one in
 * ringCall's head bump -- about 440k emulated locked RMWs a second, inside the
 * DLL that [prof] puts at 13-20 % of ALL CPU.
 *
 * The atomic was never needed for correctness of the number.  Each counter is
 * a monotone event tally whose only reader is the 5-second summary; nothing
 * branches on it and nothing needs a total that is consistent at an instant.
 * So the count moves to a per-thread block and the summary sums the blocks.
 *
 * Blocks are allocated on a thread's first D3D9 call, linked into a global
 * list, and NEVER freed -- a thread that exits must keep contributing the
 * calls it made, and the summer must not race a free.  At ~1.3 KB per block
 * and a few dozen threads that is tens of kilobytes, once.
 *
 * The counters stay std::atomic so that a summing read and a counting write
 * are a well-defined relaxed pair rather than a data race, but the write is a
 * load/add/store of a relaxed atomic, NOT fetch_add: on x86 that is three
 * plain instructions with no lock prefix, which is the entire point.  Only the
 * owning thread ever writes its own block, so the non-atomic RMW cannot lose a
 * count. */
/* Buckets in the two shape histograms. Named here rather than only in the .cpp
 * because the per-thread block below has to size them. */
constexpr int kCensusHistBuckets = 10;

/* MADEIRA ml1013: THE HISTOGRAMS WERE STILL PAYING WHAT THE COUNTERS STOPPED
 * PAYING.
 *
 * ml999 moved the 317 method counters off `lock xadd' because an emulated
 * locked RMW is expensive, and then left shaderConstF(), lockBytes() and
 * queryPoll() doing exactly that to file-scope atomics. Those are not rare
 * calls: [d3d9-census] measures Set{Vertex,Pixel}ShaderConstantF at 2602 +
 * 2428 per frame in one open-world title and 1569 + 1087 in another, and
 * shaderConstF runs on every one of them, ahead of the device lock and ahead of
 * the unchanged-value short-circuit that makes the rest of the call cheap. At
 * 5030 per frame that is ~150k emulated locked read-modify-writes a second
 * inside the DLL [prof] puts at 17.4 % of all CPU, spent entirely on a
 * histogram nothing reads until the next summary.
 *
 * Same fix as ml999, same reasoning: these are monotone event tallies summed
 * once per window, so they move into the per-thread block and the writes become
 * a relaxed load/add/store with no lock prefix. The type stays std::atomic so
 * that the summing read and the counting write remain a well-defined relaxed
 * pair rather than a data race. */
struct ThreadCounters {
  std::atomic<uint32_t> n[D3D9_CENSUS_COUNT];
  std::atomic<uint32_t> hist_const[kCensusHistBuckets];
  std::atomic<uint32_t> hist_lock[kCensusHistBuckets];
  std::atomic<uint32_t> q_polls;
  std::atomic<uint32_t> q_polls_complete;
  std::atomic<uint32_t> q_polls_parked;
  ThreadCounters *next;
};

// The pointer has no dynamic initializer. Declare that across translation
// units so ARM64EC does not request an unavailable external TLS-init thunk.
extern thread_local constinit ThreadCounters *g_tls_calls;

/* First call on this thread: allocate and link the block, then count. Out of
 * line and never inlined -- it runs once per thread and keeping it out of the
 * macro keeps the hot path to a TLS load, a test and an increment. */
void countSlow(unsigned code);

/* Summed across every registered block. Report path only. */
uint32_t callCount(unsigned code);

inline void
count(unsigned code) {
  ThreadCounters *c = g_tls_calls;
  if (__builtin_expect(c == nullptr, 0)) {
    countSlow(code);
    return;
  }
  /* Deliberately not fetch_add. See the note above. */
  c->n[code].store(c->n[code].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/* MADEIRA ml999: the [d3d9-last] ring is now OPT-IN (MADEIRA_D3D9_LAST=1).
 *
 * It is crash forensics, not a measurement: nothing in any report reads it, it
 * only ever prints after the process is already dying. Its cost is a second
 * emulated locked RMW on the head plus an out-of-line call and ~8 stores, on
 * the same 5540-calls-per-frame path. Off by default, the census numbers are
 * unchanged and the per-call cost is one TLS load and one increment. When it is
 * off the vectored exception handler is not installed either, so a guest fault
 * does not walk through it. */
extern bool g_ring_on;

/* Present. Counted like any other method AND used as the frame clock: the
 * summary cadence is measured in presented frames, because "calls per frame"
 * is the number section 8.4 needs and because an iOS app is killed rather
 * than exited, so there is never an atexit report to fall back on. */
void frame(unsigned code);

/* Two shape histograms the method counts alone cannot answer: how big the
 * constant uploads are (Set{Vertex,Pixel}ShaderConstantF is expected to be
 * the single busiest slot, and a shim can only batch it if the register
 * counts are small), and how big a mapped-memory Lock is (WOW64_DESIGN.md
 * section 7.5 / 8.2's guest arena is sized off this). */
void shaderConstF(unsigned vec4_count);
void lockBytes(unsigned size_bytes); /* 0 means "to the end of the buffer" */

/* MADEIRA: the query-poll instrument, reported on its own [d3d9-query] line
 * beside the census summary (same window, same frame clock).
 *
 * The census made the problem visible but cannot explain it: a per-method
 * count cannot separate "the application asked 85k times" from "each ask cost
 * a command-buffer submit". These five counters do. They are deliberately
 * NOT emitted by gen_d3d9_census.py -- they are a hand-placed instrument in
 * one class (MTLD3D9Query), not a per-vtable-slot counter, so the generator's
 * "the code IS the index" contract does not apply and the name table is
 * untouched.
 *
 * queryPoll is the only one on the spin path, so it is a single relaxed
 * uint32 add like D3D9_CENSUS; the completion counters run at issue rate
 * (hundreds per frame, not tens of thousands) and can afford 64 bits. */
/* MADEIRA [bc-decode]: what CPU BC decompression costs, on its own line beside
 * the census summary AND on a 10-second wall clock of its own.
 *
 * Two clocks because the two questions differ. The census summary's clock is
 * presented frames (every 5000 by default), which at 30 fps is minutes apart --
 * far too coarse to see a streaming burst. Decode events run at hundreds per
 * second at worst, so a GetTickCount comparison per event is free, and a
 * 10-second line puts the cost next to the frame-rate evidence in the same log.
 *
 * Counted, not sampled: MB_in is what the application handed us in BC form,
 * MB_out is what went to the GPU, and their ratio is the memory multiplier the
 * adapter is paying. ms is wall time spent inside the decoder on the calling
 * thread, which is the number that competes with the frame budget.
 *
 * bcDecodeClockNs is exposed so the caller times the decode itself rather than
 * the census guessing: a caller that is not decoding must not pay for a clock
 * read, and this way the timing brackets exactly the loop. */
uint64_t bcDecodeClockNs();
void bcDecode(bool is_base_level, uint64_t in_bytes, uint64_t out_bytes, uint64_t ns);

void queryIssued();                   /* Issue(D3DISSUE_END) on EVENT/OCCLUSION */
void queryFlushed();                  /* GetData committed the issuing chunk  */
void queryPoll(bool complete, bool parked);
void queryCompleted(uint64_t issue_to_complete_ns, uint32_t polls);

/* MADEIRA [d3d9-last]: the last-call ring.
 *
 * A guest that dies dereferencing something D3D9 handed it leaves an
 * address and nothing else: the log says "read of NULL+4 in game code" and
 * the question "what did it ask us for just before that" has no answer. The
 * census already runs at the top of all 317 vtable slots, so the ring costs
 * one more relaxed store per call on the same already-taken branch.
 *
 * 64 entries, power of two, no lock: a single relaxed fetch_add on the head
 * picks the slot, the fields are then stored plainly. Two threads can
 * interleave inside one slot and a reader can see a half-written entry --
 * accepted deliberately, because the alternative (a seqlock or a mutex) puts
 * real synchronisation on a path that runs thousands of times per frame to
 * serve a diagnostic that only ever runs once, after the process is already
 * dying. The sequence number printed with each entry makes a torn slot
 * obvious rather than silently misleading.
 *
 * Entries are of two kinds. Every slot pushes a CALL on entry (the generated
 * D3D9_CENSUS macro, so no call site had to be touched); the methods whose
 * RESULT is the interesting half -- the creates, the gets that hand back an
 * interface pointer, Reset/Present/TestCooperativeLevel -- also push a RET
 * carrying the HRESULT and up to two scalar arguments, by hand, at the
 * return site. A returned E_* or a zeroed out-pointer is what a null in game
 * code is made of, so the pair is what names it. */
constexpr unsigned kLastRing = 64;

void ringCall(unsigned code);
void ringRet(unsigned code, long hr, uint32_t a0, uint32_t a1);

/* A RET entry that names itself with a literal instead of a census code, for
 * the few places where the interesting event is not one vtable slot: the
 * device create / Reset / additional-swapchain path, which already funnels
 * through LogPresentRequest with its HRESULT in hand. `what` must have static
 * storage duration -- the ring keeps the pointer, not a copy. */
void ringNote(const char *what, long hr, uint32_t a0, uint32_t a1);

/* Print the ring. `why` names the trigger ("guest-exception", "detach",
 * "native-hook"); it is printed on the header line so a log with more than
 * one dump can tell them apart. Safe to call from an exception handler: it
 * takes no lock of its own and allocates nothing. Capped at a handful of
 * dumps per process so a fault inside a fault cannot flood the log. */
void dumpLastCalls(const char *why);

} // namespace dxmt::census

#define D3D9_CENSUS(code)                                                                                              \
  do {                                                                                                                 \
    if (::dxmt::census::g_on) {                                                                                        \
      ::dxmt::census::count((code));                                                                                   \
      if (::dxmt::census::g_ring_on)                                                                                   \
        ::dxmt::census::ringCall((code));                                                                              \
    }                                                                                                                  \
  } while (0)

/* MADEIRA [d3d9-last]: the return half, placed by hand at the return sites
 * that matter. Deliberately NOT emitted by gen_d3d9_census.py: the generator
 * owns the ENTRY of every method ("the code IS the index"), and a return site
 * is not something it can find without parsing control flow. Its spelling
 * cannot collide with the generator's marker regex either -- that matches
 * `D3D9_CENSUS(` / `D3D9_CENSUS_FRAME(` exactly, and `D3D9_CENSUS_RET(` is
 * neither -- so a --strip / re-inject pass leaves these lines alone. */
#define D3D9_CENSUS_RET(code, hr, a0, a1)                                                                              \
  do {                                                                                                                 \
    if (::dxmt::census::g_on && ::dxmt::census::g_ring_on)                                                             \
      ::dxmt::census::ringRet((code), (long)(hr), (uint32_t)(a0), (uint32_t)(a1));                                     \
  } while (0)

/* Present only. Separate macro so the frame clock lives in exactly one place;
 * gen_d3d9_census.py emits it for MTLD3D9SwapChain::Present alone, because
 * MTLD3D9Device::Present and ::PresentEx both forward to it and ticking all
 * three would count one frame as three. */
#define D3D9_CENSUS_FRAME(code)                                                                                        \
  do {                                                                                                                 \
    if (::dxmt::census::g_on)                                                                                          \
      ::dxmt::census::frame((code));                                                                                    \
  } while (0)
