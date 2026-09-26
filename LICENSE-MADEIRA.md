# Licensing of Madeira's modifications

This repository is a fork of [DXMT](https://github.com/3Shain/DXMT). **The upstream licence
is unchanged and continues to apply to all upstream code.** See `LICENSE`.

## What is licensed how

| Code | Licence |
|---|---|
| All upstream DXMT code | as in `LICENSE` (MIT) — unchanged |
| Code imported from the `v0.4-d3d9` tag of `dacevedo12/dxmt` (see below) | **LGPL-2.1-or-later**, as received (`COPYING.LIB`) |
| Modifications and new files authored for **Madeira** by Will Faust | **GPL-3.0-or-later** |
| Modifications and new files contributed for **Madeira** by 125hz | **GPL-3.0-or-later**, with the Madeira Converter Exception below |

## The Direct3D 9 / DXSO import (LGPL-2.1-or-later)

The Direct3D 9 frontend and its DXSO/fixed-function shader translator were
imported from a different fork of the same upstream:

- **Origin:** `https://github.com/dacevedo12/dxmt.git`
- **Tag:** `v0.4-d3d9`, commit `e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b`
- **Upstream licence at that tag:** **LGPL-2.1-or-later**
  ("Copyright (c) 2023-2026 Feifan He for CodeWeavers"), full text in
  `COPYING.LIB`. That repository's `LICENSE.OLD` records that releases up to
  v0.80 were MIT; this fork branched in the MIT era, which is why the `LICENSE`
  here is still the MIT one and why it stays that way — it states the terms of
  the code this fork actually took from upstream.

The imported code is kept under **LGPL-2.1-or-later**, exactly as received.
LGPL-2.1 **§3** would let a recipient distribute a copy under the ordinary GNU
GPL instead (the main repository did that for its Wine fork), but that option
is **not** exercised here: whether to convert the imported code, or to keep it
as a separately licensed LGPL-2.1-or-later part of this repository, is left to
the maintainer. Nothing in this repository changes its licence, and every
upstream copyright and licence notice is kept intact.

### Files imported from that tag

Whole files, unmodified except where a compile fix is noted in the source:

| Path | Files |
|---|---|
| `src/d3d9/**` | 71 (the whole directory, including `meson.build`, `d3d9.def`, `version.rc`) |
| `src/airconv/dxso_header.hpp`, `dxso_decoder.hpp`, `dxso_compile.{hpp,cpp}`, `ffp_compile.{hpp,cpp}` | 6 |

Blocks spliced into files this fork already had (the imported blocks remain
LGPL-2.1-or-later; the surrounding code stays under its existing terms):

| Path | What was taken |
|---|---|
| `src/airconv/airconv_public.h` | the DXSO public API: `dxso_shader_t`/`dxso_bitcode_t`, the `DXSO_*` argument structs and enums, and the five `DXSO*` entry points. The SM50 half of the reference header (its `AIRCONV_VERSION`, `SM50_BINDING_INDEX`, `SM50_SHADER_FLAG`, root-signature argument) was deliberately **not** taken — it would change the SM50 wire format this fork's d3d11 already uses. |
| `src/airconv/air_signature.{hpp,cpp}` | `air::InputPointCoord` and the `OutputPointSize` function output, with their AIR metadata arms |
| `src/airconv/nt/air_builder.{hpp,cpp}` | `AIRBuilder::FPBinOp::pow` |
| `src/winemetal/airconv_thunks.{h,c}` | the DXSO unix-call slot numbers, parameter structs (and their `*32` mirrors) and PE-side thunks |
| `src/winemetal/unix/winemetal_unix.c` | the DXSO 32-bit argument-chain converter (`dxso_compilation_argument32_convert`/`_free`) and the `thunk_DXSO*` / `thunk32_DXSO*` handlers |
| `src/dxmt/dxmt_command.metal` | the shader-resolve and stretch-blit shaders and their metadata structs: `resolve_data`, `DXMTResolveMetadata`, `vs_resolve_msaa`, `fs_resolve_msaa_average`, `resolve_depth_output`, `fs_resolve_msaa_depth`, `blit_data`, `DXMTStretchBlitMetadata`, `vs_blit_quad`, `fs_blit_quad` |
| `src/dxmt/dxmt_command.{hpp,cpp}` | `ResolveTextureMode`, `ResolveTextureContext` and `StretchBlitContext` (declarations and definitions, including their PSO/sampler caches) |
| `src/dxmt/dxmt_context.{hpp,cpp}` | `ArgumentEncodingContext::resolveDepthTexture`, `stretchBlit`, `copyTexture`, `optimizeTextureForGPUAccess`, `signalEventByHandle`, the `Rc<BufferAllocation>` overload of `access`, `StretchBlitEncoderData`, the extra `ResolveEncoderData` fields, and the `EncoderType::Resolve` depth/shader branches and `EncoderType::StretchBlit` encode body |
| `src/dxmt/dxmt_command_queue.{hpp,cpp}` | `GpuCompletionStatus`, `GpuCompletionTarget`, `CommandChunk::addCompletionTarget` and the finish-thread completion dispatch, `CommandQueue::HasDeviceError`/`MarkDeviceError`/`FrameLatencySignaled`/`WaitFrameLatency`, `CommandQueue::WaitCPUFenceBounded` |
| `src/dxmt/dxmt_ring_bump_allocator.hpp` | `RingBumpState::preallocate`, `seal_latest`, the `single_writer` constructor argument and its `note_single_writer` assertion, and the `__i386__` `kStagingBlockSize` |
| `src/dxmt/dxmt_format.hpp` | `Recall_sRGB_ForRenderTarget` |
| `src/dxmt/dxmt_shader_cache.{hpp,cpp}` | `GetDXMTShaderCacheDirectory` |
| `src/dxmt/dxmt_buffer.hpp` | `BufferAllocation::length` |
| `src/dxmt/dxmt_texture.{hpp,cpp}` | `TextureAllocation::buffer`, `TextureViewDescriptor::swizzle`, `Texture::miplevelCount`, `checkViewUseSwizzle`, `checkViewUseMipRange` |
| `src/dxmt/dxmt_dynamic.{hpp,cpp}` | the `out_minted_fresh` argument of `DynamicBuffer::allocate` |
| `src/dxmt/dxmt_presenter.{hpp,cpp}` | `Presenter::setDisplaySyncEnabled` |
| `src/util/wsi_window.hpp`, `wsi_window_win32.cpp`, `wsi_window_headless.cpp` | `wsi::foregroundWindow` |
| `src/winemetal/winemetal.h` | `wmtcmd_render_setsamplerstate`, `wmtcmd_blit_optimize_contents`, and the appended `WMTRenderCommandSetBlendFactor` / `SetFragmentSamplerState` / `SetVertexTexture` / `SetVertexSamplerState` and `WMTBlitCommandOptimizeContentsForGPUAccess` enumerators |

The guest-window fix in `src/dxmt/dxmt_buffer.cpp` (`Buffer::allocate`'s
`#ifdef __i386__` `CpuPlaced`) is Madeira's own work: the reference has no
equivalent, because it does not have a shifted guest window to satisfy.

The `[d3d9-census]` instrumentation (WOW64_DESIGN.md §8.4) is Madeira's own
work under GPL-3.0-or-later, and the reference has no equivalent. New files:
`src/d3d9/gen_d3d9_census.py`, `src/d3d9/d3d9_census.{hpp,cpp}` and the
generated `src/d3d9/d3d9_census_names.h`. Added to files imported from the tag:

| Path | What was added |
|---|---|
| `src/d3d9/d3d9_{buffer,cube_texture,device,interface,query,shader,state_block,surface,swapchain,texture,vertex_declaration,volume,volume_texture}.cpp` | one `#include "d3d9_census.hpp"` and one generated `D3D9_CENSUS(...)` / `D3D9_CENSUS_FRAME(...)` line at the top of each of the 317 `STDMETHODCALLTYPE` definitions, all emitted by `gen_d3d9_census.py`; plus three hand-placed histogram calls (`census::shaderConstF` in `d3d9_device.cpp`'s `Set{Vertex,Pixel}ShaderConstantF`, `census::lockBytes` in `d3d9_buffer.cpp`'s two `Lock`s) |
| `src/d3d9/meson.build` | `d3d9_census.cpp` in `d3d9_src` |

The empty unix-call slot the §8.4 benchmark times (`_d3d9_nop` at slot 150,
`WMTNop`, `struct unixcall_d3d9_nop`, and the `gen_remote_guard.py` `LOCAL_OK`
entry, in `src/winemetal/`) is likewise Madeira's own work; it exists only to
be measured and has no upstream counterpart.

The `Reserved*` enumerators alongside those appended command types, the
`_MTLRenderCommandEncoder_encodeCommands` / `_MTLBlitCommandEncoder_encodeCommands`
cases that decode them, and `Texture::fullView`'s mapping onto this fork's view-0
model are Madeira's own work: the reference expresses them differently (a wider
command set, and a `TextureViewKey` carrying a descriptor rather than an index).

Everything else in the D3D9 path — the guest-window pointer conversions, the
32-bit dispatch-table variants, the iOS build stages — is Madeira's own work
under GPL-3.0-or-later.

Where a file contains both, the file as a whole may only be distributed under
terms compatible with GPL-3.0-or-later, because the GPL-covered contributions
cannot be separated from it. The underlying upstream code remains available
under MIT **from upstream**, and nothing here withdraws that.

### The native ARM64 D3D9 frontend (`DXMT_MADEIRA`, WOW64_DESIGN.md §8)

Madeira's own work under **GPL-3.0-or-later**; the reference has no
equivalent, because it has no shim, no guest window and no arena. New files:

| Path | What it is |
|---|---|
| `src/util/util_madeira_compat.h` | real `GetCurrentThreadId` / `SwitchToThread` / `SetThreadPriority` / `GetCurrentProcessId` and explicit sentinels, replacing `util_win32_compat.h`'s warn-and-fail stubs in this mode (§8.2(d)) |
| `src/util/wsi_platform_madeira.cpp` | the HOST aligned allocator |
| `src/util/wsi_window_madeira.{hpp,cpp}` | the per-HWND client-size cache and the headless-shaped fullscreen answers |
| `src/d3d9/d3d9_guest_alloc.hpp` | `dxmt::guest_alloc`/`guest_free`/`guest_calloc`, defined to `wsi::aligned_malloc`/`aligned_free` off-Madeira (§8.2(c)) |
| `src/d3d9/d3d9_madeira_window.hpp` | the `madeira_window_*` seam the user32 work leaves through |
| `src/d3d9/unix/d3d9_unix_glue.h` | the pointer macros and NTSTATUS vocabulary the generated unix entries require |
| `src/d3d9/unix/d3d9_native_glue.cpp` | the handle table (and the object-identity map over it), the per-guest-process teardown, the guest arena and sub-allocator, the four input scanners, the window seam, the three transport hooks, the `[d3d9-native-census]` crossing counters, and the six per-method hooks whose shape is not mechanical |
| `src/d3d9/unix/d3d9_native_gen.inc` | generated by `src/d3d9shim/gen_d3d9_thunks.py`: the other 314 per-method hooks, each one a lookup, a forward to the `MTLD3D9*` object and a `catch (...)` |

Changes to files imported from the tag, every one gated on `DXMT_MADEIRA` so
a diff against `v0.4-d3d9` stays readable (§8.9 risk 2):

| Path | Gate sites | What changes |
|---|---|---|
| `src/util/util_win32_compat.h` | `:10` | hands the whole surface to `util_madeira_compat.h` |
| `src/util/wsi_monitor_headless.cpp` | `:19`, `:43` | `getScreenSize` answers from the client-size cache instead of `::GetSystemMetrics` |
| `src/dxmt/dxmt_buffer.cpp` | `:171` | the `CpuPlaced` arm becomes `__i386__ && !DXMT_MADEIRA` — natively there is no guest in that path, so the flag would only add a malloc and a copy |
| `src/dxmt/dxmt_ring_bump_allocator.hpp` | `:25` | same, for the 8 MB `kStagingBlockSize`: reverts to the 32 MB block that suits every other 64-bit target |
| `src/d3d9/d3d9.cpp` | `:12` | includes `util_madeira_compat.h` for `DWORD_PTR` |
| `src/d3d9/d3d9_multithread.hpp` | `:23` | same, for `GetCurrentThreadId` / `SwitchToThread` / `YieldProcessor` |
| `src/d3d9/d3d9_interface.cpp` | `:12`, `:995` | the zero-extent fallback uses `wsi::getWindowSize` rather than leaving the extent at 0 as the upstream `!_WIN32` arm does |
| `src/d3d9/d3d9_device.cpp` | `:1972-2018`, `:2033-2063`, `:2066-2121`, `:2176-2191`, `:2204-2233`, `:2251-2296`, `:2306-2338` | the user32 blocks — fullscreen restyle, the focus-window subclass and its wndproc, the activation minimize and reposition — leave through the `madeira_window_*` seam; the device-state half of every one of them stays |
| `src/d3d9/d3d9_device.cpp` | `TestCooperativeLevel` (not gated) | drops `LockDevice()`, exactly as `MTLD3D9Query::GetDataSize` already does and for the same reason: everything it reads is `const` or an atomic, and log 41 measured 809 calls per frame. Generic (DXVK's `TestCooperativeLevel` takes no lock either), so it is not behind `DXMT_MADEIRA` |

The allocator split's call sites are **not** gated, on purpose: off-Madeira
`guest_alloc` is `wsi::aligned_malloc`, so `d3d9_buffer.cpp`,
`d3d9_surface.cpp`, `d3d9_swapchain.cpp`, `d3d9_texture.cpp`,
`d3d9_cube_texture.cpp`, `d3d9_device.cpp` and `d3d9_mem.cpp` keep their exact
current behaviour on every other target and the diff stays one identifier per
site.

### The i386 D3D9 shim (`src/d3d9shim/`, WOW64_DESIGN.md §8.5)

The shim is a second, separate module: `d3d9.dll` on the i386 farm is now the
shim, and the emulated frontend above ships beside it as `d3d9-emulated.dll`.
One file name, two provenances, so they are recorded separately.

Madeira's own work under **GPL-3.0-or-later**. New files:

| Path | What it is |
|---|---|
| `src/d3d9shim/d3d9_api.py`, `gen_d3d9_thunks.py` | the single description of the boundary and its generator |
| `src/d3d9shim/d3d9shim_ops.h`, `d3d9shim_objects_gen.h`, `d3d9shim_thunks.c` | generated from it (the 320 vtable bodies, the 15 vtables, the parameter blocks and mirrors) |
| `src/d3d9/unix/d3d9_native_hooks.h`, `d3d9_unix.c`, `d3d9_unix_table.c`, `d3d9_native_gen.inc` | generated from the same description: the unix half of the boundary |
| `src/d3d9shim/d3d9shim_object.{c,h}` | the guest-side object model: header, vtables, identity tables, refcounts, `QueryInterface`, the shadow state |
| `src/d3d9shim/d3d9shim_arena.c` | the guest arena — `VirtualAlloc`'d chunks registered with the native sub-allocator (§8.2(c)) |
| `src/d3d9shim/d3d9shim_lock.c` | the `D3DCREATE_MULTITHREADED` recursive spinlock (§8.2(d)) |
| `src/d3d9shim/d3d9shim_custom.c` | the eleven custom slot bodies and the two cursor bodies |
| `src/d3d9shim/d3d9shim_main.c` | `DllMain`, the twelve exports, the unix-call transport and the `madeira-d3d9.txt` A/B knob |
| `src/d3d9shim/d3d9.def`, `meson.build` | the export list (names **and** ordinals, matching `src/d3d9/d3d9.def`) and the i386-only build |

**Three blocks were moved VERBATIM out of the imported frontend** and keep
their LGPL-2.1-or-later licence, exactly as above. They are the same code, relocated
because it has to run on the guest's own thread; each carries the note in its
file header:

| Moved into | From | What |
|---|---|---|
| `src/d3d9shim/d3d9shim_main.c` | `src/d3d9/d3d9.cpp` (`D3DPERF_*` and `DebugSet*`, the block §8.9-1 calls `:44-67`) | the seven `D3DPERF_*` entry points and the PIX nesting counter, plus `DebugSetLevel`/`DebugSetMute` |
| `src/d3d9shim/d3d9shim_main.c` | `src/d3d9/d3d9.cpp` (the block §8.9-1 calls `:85-316`) | the whole `Direct3DShaderValidatorCreate9` state machine: its message ids, vtable, `Begin`/`Instruction`/`End` and the stuck-error behaviour the wine d3d9 test pins against native |
| `src/d3d9shim/d3d9shim_fpu.c` | `src/d3d9/d3d9_device.cpp` (`static setupFpu()`, the block §8.9-1 calls `d3d9_device.cpp:512-521`) | the x87 control-word reprogram and the comment explaining the mask |

Both files were translated from C++ to C — `std::atomic` became the
`Interlocked*` intrinsics, `new`/`delete` became `HeapAlloc`/`HeapFree`, and
the namespace disappeared — without changing any observable behaviour. The
line numbers above are the ones WOW64_DESIGN.md §8.9-1 records; the blocks
have since shifted, and were re-located by content.

`src/d3d9shim/d3d9shim_window.c` is a **reshaping** rather than a move: the
fullscreen restyle, the focus-window subclass and its wndproc, and the cursor
realisation come from the same `d3d9_device.cpp` blocks (`enterFullscreenWindow`,
`leaveFullscreenWindow`, `focusWindowProc`, `hookFocusWindowProc`,
`unhookFocusWindowProc`, `SetCursorProperties`, `SetCursorPosition`,
`ShowCursor`), rewritten from C++ members over `MTLD3D9Device` into C functions
over `struct d3d9shim_device_extra`, with the device-state half of each left
behind on the native side. `d3d9shim_custom.c`'s `GetDC`/`ReleaseDC` are the
same relationship to `d3d9_surface.cpp`'s, including the
`D3DKMT_CREATEDCFROMMEMORY` declaration the toolchain ships no header for.
Those files carry the same provenance note.

`src/d3d9shim/d3d9shim_object.c`'s `d3d9_transform_index` and
`texture_stage_to_slot` are two-line ports of `d3d9_matrix.hpp`'s
`transform_index` and the anonymous-namespace helper at `d3d9_device.cpp`'s
`GetTexture`; same provenance, noted at the functions.

### The `IDirect3DQuery9::GetData` poll fix (WOW64_DESIGN.md §7, §8.4)

Madeira's own work under GPL-3.0-or-later; the reference has no equivalent
(it spins). The `[d3d9-census]` measurement of a 32-bit title showed
`GetData` as the single busiest vtable slot in the frontend at ~85.7k calls
per frame, the application using an `EVENT` query as a GPU fence and
busy-polling it. Three changes, all generic DXMT behaviour with no
application-specific condition anywhere:

| Path | What was changed |
|---|---|
| `src/dxmt/dxmt_command_queue.{hpp,cpp}` | `CommandQueue::WaitCPUFenceBounded(seq, timeout_ns)` — the bounded, cooperative counterpart of `WaitCPUFence`, for a caller that may not block |
| `src/d3d9/d3d9_query.{hpp,cpp}` | `m_flushed_since_issue` (the forward-progress command-buffer submit is now one-off per `Issue(D3DISSUE_END)` instead of re-evaluated on every poll), `m_polls_since_issue` / `m_issue_ns` / `kPollsBeforePark` / `kPollParkNanos` and the capped park in `GetData` around `getDataImpl`, `notePollComplete`, the lock-free `GetDataSize`, and `getDataImpl` calling the inline `d3d9_query_data_size` instead of the virtual override |
| `src/d3d9/d3d9_census.{hpp,cpp}` | the `[d3d9-query]` instrument: `census::query{Issued,Flushed,Poll,Completed}` and the two-line per-window report (`reportQueries`, `qline`). Hand-placed in one class, so `gen_d3d9_census.py` neither emits nor checks it and `d3d9_census_names.h` is unchanged |

## Why

The intent is that derivatives of this work which are *distributed* remain open
source. MIT permits a proprietary derivative; the GPL does not. MIT is
GPL-compatible, so combining them this way is permitted.

Three limits, stated plainly rather than left implied:

- The GPL constrains **distribution**. It does not restrict private
  modification, internal use, or a separate program that merely invokes this
  one.
- **Modifications published earlier, while this repository presented itself as
  MIT, were granted under MIT. That grant cannot be revoked.** Only
  contributions made from 2026-08-28 onward are GPL-only. Anyone who already has a
  copy keeps their MIT rights to it.
- It cannot stop anyone independently reimplementing the same ideas.

## Contributing

Contributions are accepted under **GPL-3.0-or-later**. See `CONTRIBUTING.md`.

## Additional permission for the Apple Metal Shader Converter (adopted 2026-09-24)

The Madeira-authored modifications and new files in this repository (the
commits by Will Faust) are offered under GPL-3.0-or-later **with** the
following additional permission, reproduced here in full so this grant is
self-contained. Upstream code keeps its own licence and notices and needs
no exception. Prepared 2026-09-16; the copyright holder adopted it on
2026-09-24, as recorded in the adoption line of the top-level Madeira
repository's LICENSE-EXCEPTION.md, before any public push.

The modifications and new files contributed by 125hz (the commits signed off
by 125hz) are offered on the same terms: GPL-3.0-or-later with the
additional permission below.

### Madeira Converter Exception, version 1 (of 2026-09-16; in effect from the adoption recorded in the top-level LICENSE-EXCEPTION.md)

Additional permission under GNU GPL version 3 section 7.

If you modify this Program, or any covered work, by linking or combining it
with the Apple Metal Shader Converter dynamic library
(libmetalirconverter.dylib, in any version) or with Apple's Metal,
Foundation, CoreGraphics, QuartzCore, UIKit, AppKit and related system
frameworks, or with modified versions of those libraries, the licensors of
this Program grant you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination shall
include the source code for the parts of the Program used in the
combination, but need not include the source code of those Apple libraries.
This permission does not extend to those libraries, which remain subject to
Apple's own licence terms. You may remove this additional permission from
copies you convey, as GPL-3.0 section 7 allows.
