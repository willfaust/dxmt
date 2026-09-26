<!--
Copyright 2026 Will Faust
SPDX-License-Identifier: GPL-3.0-or-later
-->

# `d3d9shim` — the generated half of the native D3D9 path

`WOW64_DESIGN.md` §8 replaces the emulated i386 `d3d9.dll` with two modules:
a thin i386 **shim** that owns the guest-visible object model, and the
**native ARM64** DXMT D3D9 frontend living in `libdxmt_unix.a`. This
directory holds the single description of the boundary between them and the
generator that emits both sides of it.

The shim half builds and links as `d3d9shim.dll` and is installed as
`d3d9.dll`; the unix half is implemented against `MTLD3D9*` and bound by
`virtual_ios.c`'s `d3d9shim` branch of `load_builtin_unixlib()`.

**Installed as `d3d9.dll` is not the same thing as "the native frontend is
on".** With no knob set the shim's `DllMain` forwards all ten exports to
`d3d9-emulated.dll`, so the shipped default is the emulated frontend, byte for
byte. `Documents/madeira-d3d9.txt` = `native` (exported by the app as
`MADEIRA_D3D9`) is what switches the process onto the native ARM64 frontend;
`emulated` spells the default out.

## Files

| File | Owner | What it is |
|---|---|---|
| `d3d9_api.py` | **hand-written** | The description: 15 interfaces, 320 vtable slots, the three transport slots, every argument's shape, every method's disposition, the five mirror structs, the shim object model. Run it (`python3 d3d9_api.py`) for a summary and a validation pass. |
| `gen_d3d9_thunks.py` | **hand-written** | The generator. |
| `d3d9shim_ops.h` | generated | Opcode enum, 320 parameter blocks plus the three transport blocks, mirror structs, every `_Static_assert`. Included by **both** sides. |
| `d3d9shim_objects_gen.h` | generated | The shim object header, per-kind state, and the extern hook contract. |
| `d3d9shim_thunks.c` | generated | 15 vtables, 320 method bodies (i386 PE). |
| `../d3d9/unix/d3d9_native_hooks.h` | generated | One `d3d9_native_<Iface>_<Method>` prototype per slot. |
| `../d3d9/unix/d3d9_unix.c` | generated | 324 unix entries, the mirror conversions, the ring-replay switch. |
| `../d3d9/unix/d3d9_unix_table.c` | generated | Both dispatch tables. |
| `../d3d9/unix/d3d9_native_gen.inc` | generated | 314 of the 320 native hooks: look the receiver up, look every interface argument up, forward to the `MTLD3D9*` object, intern whatever came back, `catch (...)`. Included once, by `d3d9_native_glue.cpp`. Also carries the opcode NAME table the `[d3d9-native-census]` summary prints. |

Hand-written and landed in step 3 (§8.5): `d3d9shim_main.c` (DllMain, the
twelve exports, the transport, the `madeira-d3d9.txt` A/B knob, `D3DPERF_*`
and the shader validator), `d3d9shim_object.c/.h` (the object model, identity,
refcounts, the shadow), `d3d9shim_custom.c` (the eleven custom bodies and the
two cursor bodies), `d3d9shim_window.c`, `d3d9shim_fpu.c`, `d3d9shim_arena.c`,
`d3d9shim_lock.c`, `d3d9.def`, `meson.build`. Hand-written and landed in
step 4: `d3d9_unix_glue.h` + `d3d9_native_glue.cpp`.

**Why 314 of the 320 native bodies are generated.** The unix entry has
already converted, validated and (for the five mirrors) expanded everything
by the time a hook is called, so what is left is mechanical in every case
where the argument shapes carry enough type information — which is all but
six. Writing those 314 by hand is the same drift the slot table itself is
generated to prevent. The six exceptions are listed in
`NATIVE_HANDWRITTEN` in the generator, printed into the top of
`d3d9_native_gen.inc`, and implemented in `d3d9_native_glue.cpp`:
`IDirect3D9Ex::CreateDevice` / `CreateDeviceEx` (they strip
`D3DCREATE_MULTITHREADED`, because §8.2(d) gives that lock to the shim and
`MTLD3D9Device` derives `is_protected` from the flags it is handed, and they
seed the window-size cache), `IDirect3DDevice9Ex::GetCreationParameters`
(puts that flag back), `IDirect3DDevice9Ex::CheckResourceResidency` (its
array holds guest interface pointers, which have no native meaning), and
`IDirect3DSurface9::GetDC` / `ReleaseDC` (shim-local: gdi32 on the guest's
own thread). `generator_self_check()` refuses to emit if a slot whose shape
cannot be forwarded mechanically is neither `local` nor in that list.

Three slots are **transport**, not vtable methods: `d3d9_api.py` describes
them in `TRANSPORT_SLOTS` rather than in `INTERFACES`, and the generator emits
their blocks, their opcodes, their native hooks, their unix entries and their
table rows exactly as it does for a method.

They used to be hand-written in `d3d9shim_object.h` and numbered
`D3D9SHIM_OP_COUNT + n` — which put them **past the end of both dispatch
tables**, since those are sized `D3D9SHIM_OP_COUNT`, so no call on one of them
could ever bind. Absorbing them is what gives them entries. The numbers and
the block layouts are unchanged, byte for byte; `D3D9SHIM_OP_COUNT` is now 324
and includes them.

| Slot | Block | Native hook | What it is |
|---|---|---|---|
| 321 `D3D9SHIM_OP_arena_register` | `struct d3d9_arena_register_params` (16) | `int d3d9_native_arena_register(uint32_t guest_base, uint64_t size)` | hands the native sub-allocator one `VirtualAlloc`'d guest arena chunk (§8.2(c)). 0 means success, which is why the entry maps it to `D3D_OK`/`E_FAIL` |
| 322 `D3D9SHIM_OP_window_state` | `struct d3d9_window_state_params` (24) | `HRESULT d3d9_native_window_state(HWND, uint32_t width, uint32_t height, uint32_t flags)` | the per-HWND client size, visibility and foreground state `wsi_window_madeira.cpp` answers from (§8.2(d)) |
| 323 `D3D9SHIM_OP_create_interface` | `struct d3d9_create_interface_params` (24) | `HRESULT d3d9_native_create_interface(d3d9_native_handle *iface, uint32_t sdk_version, uint32_t is_ex)` | creates the native `IDirect3D9(Ex)` — the handle every other call's `self` descends from. `Direct3DCreate9` is a DLL export rather than a vtable slot, so no interface in the description produces it |

`D3D9SHIM_WINDOW_VISIBLE`/`_FOREGROUND`/`_FULLSCREEN`/`_GONE` are emitted with
them, so the two halves cannot disagree about the flag values either. The
absolute slot numbers are pinned in the description and checked, so a 321st
vtable slot cannot silently renumber the transport ABI.

## Regenerating

```sh
python3 research/dxmt/src/d3d9shim/gen_d3d9_thunks.py            # write
python3 research/dxmt/src/d3d9shim/gen_d3d9_thunks.py --check    # CI: stale?
```

Every generated file carries a "REGENERATE, DO NOT EDIT" banner. Output is
deterministic — no timestamps, no paths, no dict iteration order — so
`--check` is a clean staleness gate for a pre-commit hook or CI.

The generator **refuses to emit anything** if `d3d9_api.validate()` or its own
self-check reports a problem. Each of these has been shown to fire:

- duplicate or non-dense slot numbers within an interface
- a slot added or dropped (the per-interface counts are checked against the
  table in `WOW64_DESIGN.md` §8.1, and the total against 320)
- a duplicate method name in one interface
- an unknown disposition, or a `defer` with no validation predicate or no
  constant return, or a `local` with no local kind, or a `sync` that does not
  flush
- a mirror whose declared 32-bit image is not exactly 4-byte packed, or whose
  field offsets do not add up to its declared `size32`
- a predicate or count expression naming something that does not resolve — an
  argument that does not exist, a `self.<field>` not in `OBJECT_STATE`, an
  unknown helper call
- an unknown identity helper, an interface with no object kind, a return type
  with no storage class, a parameter block whose size is not a multiple of 8
  (reachable where the size is declared by hand — the transport blocks;
  a vtable block's size is a multiple of 8 by construction)
- a `resolve` with no identity helper, with no `iface_out`, or whose resolved
  handle is not the block's second 64-bit word
- a `guest_ptr_out` with no `HRESULT` to refuse a NULL out-parameter with, or
  two of them in one block
- a transport slot that has moved off its pinned opcode number, whose block
  no longer has the layout the hand-written half ships against, whose 64-bit
  field is not first, or whose field role is unknown

The two dispatch tables are emitted from one list, so they cannot differ in
length; `d3d9_unix_table.c` asserts that in C anyway, and each vtable asserts
its own length.

## The ABI rules

**The slot number is the ABI.** Slot order comes from the SDK header
`include/native/directx/d3d9.h`, which is also what `src/d3d9/*.hpp` declares
`override` against — so the description and the implementation cannot drift
without a C++ compile error. A line inserted or dropped silently sends every
later call to the wrong function, exactly as `gen_remote_guard.py` warns about
the winemetal table. All 320 slot names were cross-checked against the
implementing classes; only `AddRef`/`Release` on two interfaces are not
textually present, because they come from `ComObject`.

**Parameter blocks are fixed width.** Guest pointers are `uint32_t`, handles
and native object references `uint64_t`, scalars `uint32_t` (or `float` where
the C type is float — 4 bytes with the same representation on both). The
64-bit fields are emitted first, so each lands on a multiple of 8 whatever the
target's alignment rule for 64-bit types happens to be. One struct definition
is therefore correct on i386 and on LP64 and there is **no `*_params32`
mirror**: this is the `WMTMemoryPointer` trick (`winemetal.h:132-200`) applied
to the whole block.

**That is also why both unix tables point at the same functions.** A block's
pointer fields are `uint32_t`, so a block can only have been written by a
32-bit caller and there is exactly one correct way to read one — a `_32`
variant would have nothing to do differently. The 64-bit table exists because
`load_builtin_unixlib` binds a *pair* of equal length (§7.4 rule 3);
`_d3d9_init` refuses any caller whose `sizeof(void *)` is not 4, so the
64-bit table can be entered but never lies about a result (§7.4 rule 4).

**Pointers.** Every embedded pointer is a GUEST address and is converted with
`ios_wow_host_ptr()` semantics, NULL-preserving, before any dereference; every
converted pointer is range-checked before the frontend sees it, so a bad guest
pointer is `D3DERR_INVALIDCALL` and not a host fault the application's SEH can
never catch (§8.9-4). **Three fields** are ever written back, all of them the
same thing — the address of mapped resource memory, always inside the guest
arena, converted with `ios_wow_guest_ptr32()`: `D3DLOCKED_RECT::pBits`,
`D3DLOCKED_BOX::pBits`, and the `ppbData` of
`IDirect3DVertexBuffer9::Lock` / `IDirect3DIndexBuffer9::Lock`, which is the
`guest_ptr_out` shape — `pBits` with no struct around it. (It was described as
`iface_out:IUnknown` until the shim implementer pointed out that a buffer
mapping is not an interface: the shim was building a guest object wrapper
around a memory address.) Sizes, enums, `HWND`/`HMONITOR`/`HDC`/`HANDLE` and
native handles are never offset (invariant 4). Nesting is at most two levels:
`pSharedHandle`'s user-memory idiom, `DrawIndexedPrimitiveUP`'s two buffers,
`CheckResourceResidency`'s array of guest interface pointers, and `pBits`.

**Structs.** Measured with `i686-w64-mingw32-clang` and with a 64-bit `gcc`
against the same headers, not assumed:

- **five mirrors** (i386 and LP64 layouts differ):
  `D3DPRESENT_PARAMETERS` 56/64, `D3DDEVICE_CREATION_PARAMETERS` 16/24,
  `D3DLOCKED_RECT` 8/16, `D3DLOCKED_BOX` 12/16, and — **not in §8.2(c)** —
  `D3DPRESENTSTATS` 28/32, where `LARGE_INTEGER` takes 4-byte alignment on
  i386 and 8 on LP64, moving `SyncQPCTime` from +12 to +16. Reached only by
  `IDirect3DSwapChain9Ex::GetPresentStats`.
- **26 layout-identical** structs, pointed at in place after one `+B`.
- **one padding-only** struct, `D3DADAPTER_IDENTIFIER9`: every field at the
  same offset, but `sizeof()` is 1100 on i386 and 1104 on LP64 —
  `d3d9types.h` opens with `#pragma pack(push,4)`, which caps the
  `LARGE_INTEGER DriverVersion` member's alignment at 4 on i386 and leaves it
  at 8 on LP64, so the struct's tail padding differs. It **bounces**, exactly
  like a mirror: the entry declares a host-layout local, the frontend writes
  that, and `D3D9_COPY32_OUT` copies back `D3D9SHIM_SIZE32_*` bytes and not
  one more. It used to be pointed at in place — which handed the frontend a
  host-typed pointer into a 1100-byte guest buffer, and
  `MTLD3D9Interface::GetAdapterIdentifier` opens with
  `memset(pIdentifier, 0, sizeof(*pIdentifier))`. Four bytes past the end of
  an application's stack local is its `/GS` cookie, so the title died of
  `0xC0000409` in its own epilogue with no D3D9 frame anywhere near the
  faulting address. `D3D9_COPY32_OUT`/`_IN` carry their own `_Static_assert`
  and are the only copy path for these; `generator_self_check()` refuses any
  `out_struct`/`inout_struct` whose target is not a mirror, a padding-only
  bounce, or a declared layout-identical struct, so a new struct cannot be
  written back at host size by default.

The mirror wire form is the i386 *memory image*, so every mirror field is
4 bytes wide and the two `LARGE_INTEGER`s cross as explicit lo/hi halves; a
`uint64_t` field would silently re-align the mirror and break it.

**Handshake.** `d3d9_api.py` hashes its own canonical form — slot order,
names, shapes, dispositions, predicates, block layouts, mirrors, and the
transport slots (number, hook, block layout) plus the window-state flags, but
not comments or formatting — into `D3D9SHIM_API_HASH`, compiled into both
halves.
`_d3d9_init` (unix slot 0) refuses a mismatch with `STATUS_REVISION_MISMATCH`
and reports the native side's own hash so the log says which is stale.

## Dispositions

| | slots | |
|---|---:|---|
| `local` | 62 | never crosses: 15 `QueryInterface`, 15 `AddRef`, 7 `GetType`, 22 identity getters, `RegisterSoftwareDevice`, `SetCursorPosition`, `ShowCursor` |
| `resolve` | 8 | answered from the identity cache; the cache is filled by ONE crossing on this same slot |
| `sync` | 175 | flush the ring, call, wait |
| `defer` | 75 | ring-append and return a constant, if the predicate holds (~40 distinct op names; the count is higher because the resource ops repeat across six interfaces and every final `Release` is one) |

`Release` is `defer` because only the **final** release crosses; the
hand-written `d3d9shim_obj_release()` owns the decrement and builds the
`D3D9OP_*_Release` record itself.

**`resolve` is `local` on the shim side and `sync` on the unix side**, and it
exists because a child's native handle is produced by nothing but the method
that hands the child out. `Texture9::GetSurfaceLevel`,
`CubeTexture9::GetCubeMapSurface`, `VolumeTexture9::GetVolumeLevel`,
`Device9Ex::GetSwapChain`, `Device9Ex::GetBackBuffer`,
`SwapChain9Ex::GetBackBuffer`, `Device9Ex::GetRenderTarget` and
`Device9Ex::GetDepthStencilSurface` all hand out an object the shim has no
other way to learn the identity of — an implicit swapchain, a back buffer, a
mip level, a cube face, a volume level, and the render target and depth
stencil the device binds to itself at creation (`d3d9_device.cpp:652`).
The last two were plain `identity:` locals reading the `SetRenderTarget` /
`SetDepthStencilSurface` shadow, which is only ever right AFTER the
application has bound something — before that the shadow is empty and the
answer was `D3DERR_NOTFOUND` for surfaces that exist. Classified `local`,
their unix entries were
`STATUS_NOT_IMPLEMENTED` stubs and the identity cache could never be filled at
all. The generated shim body is the identity body (call
`d3d9shim_<helper>()`, `AddRef`, store); what changes is that the generated
**unix** entry is real and answers the child's handle in the block.

Phase 1 (§8.5) is synchronous-everything. The generated deferred bodies carry
**both** arms, selected by `-DD3D9SHIM_PHASE=2`; both compile. The shadow is
applied on **both** arms — see `d3d9shim_shadow_apply` below.

## The hook contract

Everything the generated code calls, and nothing else. Full declarations are
in `d3d9shim_objects_gen.h` (shim) and the prologue of `d3d9_unix.c` (unix).

### Shim side — implemented by the hand-written `d3d9shim_*.c`

Transport:

| Hook | Contract |
|---|---|
| `uint32_t d3d9shim_native_call(slot, block, size)` | 0 on success; any non-zero is a transport failure and the thunk returns `E_FAIL` without touching the block's out-parameters. |
| `int d3d9shim_ring_append(dev, op, block, size)` | non-zero if it fit; 0 means the caller flushes and calls synchronously. |
| `HRESULT d3d9shim_flush(dev)` | replay everything queued. |
| `void d3d9shim_shadow_apply(dev, op, block)` | update the shim's shadow of whatever the deferred op changes, so the `local` getters stay correct while the record is queued. One hook, switching on the opcode. **The generated deferred bodies are the only caller**, on both arms: the phase-2 arm calls it before the ring append, and the phase-1 arm calls it after a successful synchronous call (`SUCCEEDED(ret)`, where there is an `HRESULT` to test). It must therefore be safe to call with no lock held, and it must take whatever internal lock it needs itself. |

Objects:

`d3d9shim_obj_from_native(kind, handle)` (create-or-find the guest wrapper),
`d3d9shim_obj_native(iface)`, `d3d9shim_obj_addref`, `d3d9shim_obj_release`,
`d3d9shim_obj_query_interface`, `d3d9shim_device_of`, `d3d9shim_same_device`,
`d3d9shim_has_usage`, `d3d9_transform_index`.

Identity helpers (return a **borrowed** reference or NULL; the generated body
does the `AddRef` and the store): `d3d9shim_device_back_buffer`,
`d3d9shim_device_texture` (applies `texture_stage_to_slot`),
`d3d9shim_device_stream_source` (also fills `offset`/`stride`),
`d3d9shim_device_swapchain`, `d3d9shim_swapchain_back_buffer`,
`d3d9shim_texture_sublevel`, `d3d9shim_cube_surface`, `d3d9shim_container`.

A helper named by a `resolve` slot owes one thing more: on a cache **miss** it
makes the single synchronous crossing on that slot's own opcode —
`d3d9shim_native_call(op, &block, sizeof(block))` with `block.self` set to the
receiver — and adopts the native handle the unix entry writes into the block.
The generator checks that the handle is the block's second 64-bit word, which
is what the shim's one generic reader assumes.

Arena (§7.5 / §8.2(c)): `d3d9shim_arena_alloc`, `d3d9shim_arena_free`,
`d3d9shim_arena_grow`. `d3d9shim_arena_init()` reserves and registers the
first 64 MB chunk from the transport handshake, before any D3D9 object can
exist. It must: the arena's consumer is `dxmt::guest_alloc()` on the *native*
side, and the only caller of `d3d9shim_arena_grow()` is the shim's answer to
`D3D9SHIM_STATUS_ARENA_EXHAUSTED` — so an arena that starts empty stays empty,
every app-visible allocation from the first `CreateDevice` onward returns NULL,
and the create call it was made for still reports `S_OK`. Lock (§8.2(d)): `d3d9shim_lock` / `d3d9shim_unlock`,
recursive, keyed by thread id; empty for a device without
`D3DCREATE_MULTITHREADED`. Diagnostics: `d3d9shim_log_once`.

Shim-local bodies, because they are user32/gdi32 work that must run on the
guest's own thread: `d3d9shim_shim_cursor_set_position`,
`d3d9shim_shim_cursor_show`.

Custom bodies, 11 slots whose guest-side half is not mechanical — `setupFpu`,
the focus-window hook, fullscreen styles, the cursor bitmap, `GetDC`'s
`D3DKMTCreateDCFromMemory`, the per-HWND client-size cache:
`CreateDevice`, `CreateDeviceEx`, `Reset`, `ResetEx`, `Present`, `PresentEx`,
`SwapChain9Ex::Present`, `SetCursorProperties`, `SetDialogBoxMode`,
`Surface9::GetDC`, `Surface9::ReleaseDC`. Each is
`d3d9shim_custom_<Iface>_<Method>` with the method's own signature. A `local`
method may not also be `custom` — the generator refuses it, because a local
body that needs hand-written help already has a `shim:` hook.

### Unix side — implemented by `d3d9_unix_glue.h` + `d3d9_native_glue.cpp`

Macros `d3d9_unix.c` requires:

| Macro | Contract |
|---|---|
| `D3D9_HOST_PTR(u32)` | `ios_wow_host_ptr()`: `+B`, NULL-preserving. |
| `D3D9_GUEST_PTR32(void *)` | `ios_wow_guest_ptr32()` — and it asserts the window first. The three fields this is ever applied to are mapped resource memory, which `dxmt::guest_alloc()` puts in the arena by construction; a host-heap pointer here would mean an app-visible allocation site was missed, and the plain subtraction would hand the application a plausible 32-bit number pointing at unrelated guest memory. It refuses with a once-only line and a 0 instead. |
| `D3D9_IN_WINDOW(p, bytes)` | false for anything not wholly inside `[B, B+4G)`; must still validate the base address when `bytes` is 0. |
| `D3D9_DEREF32(p)` | the `ULONG` at an already-converted pointer; **NULL- and window-safe**, because a size-inout count is read before that argument's own validation runs. |
| `D3D9_SHARED_IN(slot, pool)` / `D3D9_SHARED_OUT(slot, h, pool)` | the `pSharedHandle` two-level rule: convert only for `D3DPOOL_SYSTEMMEM` with a non-NULL target (the user-memory idiom); otherwise opaque. `pool` is `D3D9_NO_POOL` for the four create paths that have no pool argument. |
| `D3D9_LOG(msg)` | one-line diagnostic. |
| `D3D9_ARENA_TAKE_STARVED()` | non-zero if an app-visible allocation on **this thread** could not be served since the last call; reading it clears it. Every entry whose method returns `HRESULT` takes it after the call and, if the call also failed, returns `D3D9SHIM_STATUS_ARENA_EXHAUSTED` so the shim grows the arena and retries the same block once. `dxmt::guest_alloc()` is reached from deep inside the frontend and can only answer NULL, so without this mark the grow-and-retry path of §8.2(c) was unreachable and an exhausted arena stayed exhausted. Only on a failed `HRESULT`, so a retry can never repeat work that succeeded. |
| `NTSTATUS`, `STATUS_*` | as ntdll. |

Variable-length input scanners (each walks GUEST memory, so each must
window-check as it walks and return 0 for anything it cannot follow):
`d3d9_decl_element_count`, `d3d9_shader_token_count`, `d3d9_up_vertex_bytes`,
`d3d9_up_index_bytes`.

Lifecycle: `int d3d9_native_init(void)` (called from `_d3d9_init` after the
hash check) and `void d3d9_native_process_teardown(void *peb)` — called from
`ios_wow_reclaim_dead_windows()` **before** the `PROT_NONE` replace and before
`ios_jit_purge_window()`, because native objects hold host pointers *into* the
arena (§8.9-5).

Per-method: `d3d9_native_<Iface>_<Method>(...)`, one per slot including the
`local` ones, so a later reclassification is a one-line change in
`d3d9_api.py`. The receiver and any interface argument arrive as
`d3d9_native_handle` (a `uint64_t` index+generation, never a host pointer);
struct and array arguments arrive as converted, validated host pointers.
314 of the 320 are generated into `d3d9_native_gen.inc`; the six that are not
are listed under **Files** above. Every one of them looks its receiver up
(wrong kind → `D3DERR_INVALIDCALL`, never a wild cast), looks every interface
argument up the same way, forwards, interns any interface it is handed back
with FIND-BEFORE-CREATE so identity holds, and wraps the whole thing in
`catch (...)` so no C++ exception escapes into 32-bit code (§8.9-4).

The handle table is also the identity map. Asking twice for the same child
has to give the same handle, because the shim builds its guest wrapper — and
that wrapper's refcount — from it; two handles for one native surface would be
two guest objects for one surface, which is exactly the identity D3D9
applications compare pointers for. The table holds exactly one native
reference per object, for the life of the handle, and the final guest
`Release` retires it.

`[d3d9-native-census]`: one relaxed 32-bit add at the top of every hook, and
a windowed summary on the same 1/100/1000-then-every-5000 present cadence as
`[d3d9-census]`, naming the ten busiest slots. The two answer different
questions: `[d3d9-census]` counts frontend METHODS (and is compiled into this
archive too, so with the native path on it counts them again on this side),
while this counts CROSSINGS — the number §8.6's ring exists to reduce, and
the one no per-method counter can produce. `MADEIRA_D3D9_NATIVE_CENSUS=0`
disables it; `MADEIRA_D3D9_NATIVE_CENSUS_EVERY` overrides the interval.

`[d3d9-last]`: the census summary only prints on a Present cadence, so a title
that dies during device initialisation produces no census at all — and a fault
in the *application's* own code (a `/GS` fast-fail, a bad pointer) carries no
D3D9 frame, so nothing in the log says which call preceded it. The same
counter site therefore also records a per-thread last opcode, printed by every
census summary; `MADEIRA_D3D9_TRACE=1` prints one `[d3d9-last]` line per
crossing, and the last one in the log is the call the guest was in when it
died. That is the bisection tool for anything that faults on the guest side.

`[d3d9-lockcheck]`: `MADEIRA_D3D9_LOCKCHECK=1` arms a check on every pointer
written back to the guest — the two `pBits` and the two buffer `Lock()`s'
`ppbData`, all of which funnel through `d3d9_guest_ptr32()`. In-window is not
the same thing as writable: a reservation with no commit, a page an arena
chunk never faulted in, or a range some other owner re-protected all pass the
window test and then fault inside translated guest code with no provenance at
all. Armed, the pointer is looked up in the arena (an app-visible allocation
may come from nowhere else — a miss is a named line and a missed
`dxmt::guest_alloc()` site) and the first and last byte of its allocation are
read and written back unchanged, so a non-writable mapping faults *here*, on
the guest's own thread, inside a named function, with the allocation printed.

Ring replay: `NTSTATUS d3d9_ring_replay(base, bytes, seq_io)` walks
`{u16 op; u16 len; u32 seq;}` records and calls the *same* `d3d9_call_*` the
synchronous entry does — the ring is a transport, not a re-implementation. It
refuses a non-monotonic `seq`, a misaligned base or length, a record whose
length does not match its opcode's block, and any opcode that is not
`defer`-classified.

## Host validation

`i686-w64-mingw32-clang` from `.xtool/toolchains/llvm-mingw`, against the real
mingw-w64 `d3d9.h`:

```
i686-w64-mingw32-clang -fsyntax-only -std=c11 -Wall -Wextra \
    -I src/d3d9shim src/d3d9shim/d3d9shim_thunks.c            # and -DD3D9SHIM_PHASE=2
```

and the unix side, LP64, against DXMT's own native headers (plus the
hand-written `d3d9_unix_glue.h`):

```
gcc -fsyntax-only -std=c11 -Wall -Wextra \
    -I research/dxmt/include/native/windows \
    -I research/dxmt/include/native/directx \
    -I src/d3d9shim -I src/d3d9/unix src/d3d9/unix/d3d9_unix.c
```

Both pass clean at `-Wall -Wextra`, with no warnings at all from generated
code — including no `-Wunused-parameter`. That compile is also what
proves the mirrors: `d3d9shim_ops.h`'s `_Static_assert`s check the i386 image
against the SDK typedef on the 32-bit build and the host layout on the 64-bit
one, so a header change on either side is a build failure rather than a
silently wrong frame.
