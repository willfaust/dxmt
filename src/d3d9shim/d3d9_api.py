#!/usr/bin/env python3
#
# d3d9_api.py -- the single description of the Direct3D 9 interface ABI that
# the Madeira i386 shim and the native ARM64 D3D9 frontend share.
#
# Copyright 2026 Will Faust
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""The D3D9 interface description (WOW64_DESIGN.md sections 8.5 / 8.6 / 8.7).

15 interfaces, 320 vtable slots.  THE SLOT ORDER IS THE ABI: it is taken from
the SDK header `research/dxmt/include/native/directx/d3d9.h`, which is also
what `research/dxmt/src/d3d9/*.hpp` declares `override` against, so the two
cannot drift without a C++ compile error.  A slot inserted or dropped here
silently sends every later call to the wrong function, exactly as
`src/winemetal/gen_remote_guard.py` warns about the unix-call table.

Nothing in this file is generated.  `gen_d3d9_thunks.py` reads it and emits
every line of C on both sides of the boundary; the hash of the canonical form
below is compiled into both halves and checked at init.

Vocabulary of argument shapes
-----------------------------
  u32                      4-byte scalar passed by value (UINT, DWORD, INT,
                           any D3D enum, float, WINBOOL).  Stored as uint32_t,
                           or `float` when the C type is float -- both are
                           4 bytes with the same representation on i386 and
                           on ARM64, so the block layout is unaffected.
  u64                      8-byte scalar passed by value.
  iface_in:<Iface>         interface pointer IN.  Crosses as the shim object's
                           native handle (uint64_t), never as a pointer.
  iface_out:<Iface>        Iface** OUT.  Crosses as a native handle out; the
                           shim wraps it in a guest object.
  in_struct:<T>            const T* IN, pointed at in place after one +B.
  out_struct:<T>           T* OUT.
  inout_struct:<T>         T* the runtime reads AND writes back.
  in_array:<T>:<count>     const T[] IN; <count> is a C expression over the
                           other argument names of the same method.
  out_array:<T>:<count>    T[] OUT; `*name` in <count> means "the count is
                           read from, and written back to, that out_ptr arg"
                           (the D3D9 query-size-then-fill idiom).
  out_ptr:<T>              pointer to a single scalar OUT.
  locked_rect_out          D3DLOCKED_RECT* OUT -- mirrored, `pBits` converted.
  locked_box_out           D3DLOCKED_BOX* OUT  -- mirrored, `pBits` converted.
  guest_ptr_out            void ** OUT whose target is a MAPPED-MEMORY guest
                           pointer: the two buffer Lock()s.  It is NOT an
                           interface -- it is D3DLOCKED_RECT::pBits with no
                           struct around it, so the block field is the uint32
                           guest address itself, written back by the unix
                           entry with ios_wow_guest_ptr32() and stored into
                           *ppbData by the shim body.  Nothing is packed on
                           the way in; a NULL out-parameter is the shim's own
                           D3DERR_INVALIDCALL.
  handle                   HMONITOR / HDC / HANDLE by value; crosses as
                           uint64_t and is NEVER offset (invariant 4).
  hwnd                     HWND by value; uint64_t, never offset.
  shared_handle_inout      HANDLE* pSharedHandle.  Two levels: convert the
                           outer pointer only when the create is SYSTEMMEM
                           with a non-NULL target (the user-memory idiom,
                           WOW64_DESIGN.md 8.2(c)); otherwise it is an opaque
                           out-parameter.
  user_mem_in:<size>       app-owned scratch memory read for the duration of
                           one call (the two Draw*UP buffers).  <size> is a C
                           expression over the other argument names.

`out_array` and `inout_struct` are additions to the tag list sketched in
8.5; 8.5 has no way to spell "T[] OUT with a caller-supplied capacity"
(GetDeclaration, GetFunction, GetPrivateData, Get*ShaderConstant*,
GetPaletteEntries, GetClipPlane, Query9::GetData -- 19 arguments) nor
"the runtime adjusts the struct it was given" (D3DPRESENT_PARAMETERS on the
five create/reset paths).  Spelling those as plain out_struct/out_ptr would
have lost, respectively, the capacity and the write-back.

Dispositions
------------
  local   answered entirely inside the shim; no unix call, ever.
  resolve answered from the shim's identity cache, which is populated by ONE
          synchronous call on this same slot the first time the child is
          asked for.  The shim half is a `local` identity body -- it calls
          `d3d9shim_<helper>()`, which returns a borrowed reference and, on a
          cache miss, crosses on this opcode and adopts the native handle the
          unix entry writes into the block's iface_out field.  The unix half
          is therefore a REAL entry, not the STATUS_NOT_IMPLEMENTED stub a
          `local` slot gets: a child's native handle is only ever produced by
          the method that hands the child out, so without it the shim has no
          way to learn the identity of an implicit swapchain, back buffer,
          render target, depth stencil, mip level, cube face or volume level.
  sync    flush the ring, then make the unix call and wait.
  defer   append to the guest command ring and return `ret_const`, provided
          `pred` holds.  `pred` is a Python-syntax boolean expression over the
          method's own argument names and named shim state; the generator
          translates it to C for the shim and, under DXMT_DEBUG, re-checks it
          natively at replay.  When `pred` is false the shim returns
          D3DERR_INVALIDCALL and appends nothing.

Phase 1 (8.5) is synchronous-everything: `disp == 'defer'` still describes the
predicate and the constant return, but `gen_d3d9_thunks.py --phase 1` emits a
flush-and-call body for them.  Phase 2 flips the same table on.
"""

import hashlib
import re
import sys
from collections import Counter

# Bumped whenever the MEANING of a field changes (not when a slot's data
# changes -- the hash covers that).
API_VERSION = 1

SHAPE_TAGS = (
    "u32", "u64", "iface_in", "iface_out", "in_struct", "out_struct",
    "inout_struct", "in_array", "out_array", "out_ptr", "locked_rect_out",
    "locked_box_out", "guest_ptr_out", "handle", "hwnd",
    "shared_handle_inout", "user_mem_in",
)

DISPOSITIONS = ("local", "resolve", "sync", "defer")


def A(shape, ctype, name):
    """One argument."""
    tag = shape.split(":", 1)[0]
    if tag not in SHAPE_TAGS:
        raise ValueError("unknown shape tag %r" % shape)
    return {"shape": shape, "tag": tag, "ctype": ctype, "name": name}


def M(slot, name, ret, args, disp="sync", local=None, pred=None,
      ret_const=None, flush=False, custom=False, null_hr="D3D_OK",
      unimplemented=False, note=None):
    """One vtable slot.  `slot` IS the ABI.

    null_hr      for an `identity:` local: what to return when the shim has
                 no such child.  D3D9 is not uniform about this -- an unbound
                 texture stage is D3D_OK with a NULL out, an unbound render
                 target is D3DERR_NOTFOUND, an out-of-range index is
                 D3DERR_INVALIDCALL.
    unimplemented  emit an E_NOTIMPL stub that logs once instead of a body.
                 Nothing sets it today and nothing should: every comment in
                 d3d9_device.cpp's own stubs (SetConvolutionMonoKernel,
                 DrawRectPatch, DeletePatch) says E_NOTIMPL sends hr-strict
                 app initialisation into unrecoverable failure, so those
                 slots return a per-spec HRESULT from the NATIVE side instead.
                 The flag exists so a vtable hole is impossible by
                 construction, not so it gets used.
    """
    if disp not in DISPOSITIONS:
        raise ValueError("unknown disposition %r" % disp)
    return {"slot": slot, "name": name, "ret": ret, "args": args,
            "disp": disp, "local": local, "pred": pred, "ret_const": ret_const,
            "flush": flush, "custom": custom, "null_hr": null_hr,
            "unimplemented": unimplemented, "note": note}


def I(iface, short, kind, methods):
    """One interface.  `short` names it in generated C symbols."""
    return {"iface": iface, "short": short, "kind": kind, "methods": methods}


# --------------------------------------------------------------------------
# Storage classes.  Every parameter-block field is fixed width, so one struct
# definition is correct on both sides and no *_params32 mirror is needed --
# this is the WMTMemoryPointer trick (winemetal.h:132-200) applied to the
# whole block.  Field ORDER in the emitted struct is: uint64_t fields first
# (so every one of them lands on a multiple of 8 under both ABIs, whatever
# the target's alignment rule for 64-bit types is), then 4-byte fields, then
# the return field, then explicit padding to a multiple of 8.
# --------------------------------------------------------------------------

# shape tag -> ("u64" | "u32" | "f32"), i.e. which half of the block it lands in
SHAPE_STORAGE = {
    "u32": "u32", "u64": "u64",
    "iface_in": "u64", "iface_out": "u64",
    "in_struct": "u32", "out_struct": "u32", "inout_struct": "u32",
    "in_array": "u32", "out_array": "u32", "out_ptr": "u32",
    "locked_rect_out": "u32", "locked_box_out": "u32", "guest_ptr_out": "u32",
    "handle": "u64", "hwnd": "u64",
    "shared_handle_inout": "u32", "user_mem_in": "u32",
}

# C return type -> parameter-block storage for the result (None = void)
RET_STORAGE = {
    "HRESULT": "int32_t",
    "ULONG": "uint32_t",
    "UINT": "uint32_t",
    "DWORD": "uint32_t",
    "WINBOOL": "int32_t",
    "float": "float",
    "HMONITOR": "uint64_t",
    "D3DRESOURCETYPE": "uint32_t",
    "D3DTEXTUREFILTERTYPE": "uint32_t",
    "D3DQUERYTYPE": "uint32_t",
    "int": "int32_t",
    "void": None,
}

# --------------------------------------------------------------------------
# Structs that cross the boundary.
#
# MIRRORS: i386 and LP64 layouts differ, so the wire form is the i386 MEMORY
# IMAGE of the struct and the unix entry expands it into a host one.  Every
# mirror field is therefore 4 bytes wide -- nothing in these five structs is
# wider than a DWORD on i386, and the two LARGE_INTEGERs are carried as
# explicit lo/hi halves rather than as a uint64_t, because LARGE_INTEGER's
# first union member is a pair of DWORDs and so takes 4-byte alignment on
# i386 (measured: D3DPRESENTSTATS::SyncQPCTime is at +12, not +16).  A
# uint64_t field would silently re-align the mirror and break it.
#
# `fields` is (C type, name, offset32, offset64, role) in order; offset64 is
# the same field's offset in the HOST struct, and role is one of
#   scalar     copy across unchanged
#   hwnd       widen to HWND / narrow back; a handle, so NEVER offset
#   guest_ptr  a guest address: ios_wow_host_ptr() in, ios_wow_guest_ptr32()
#              out.  There is exactly one such field in the whole API, pBits.
#   u64_lo/u64_hi  the two halves of one LARGE_INTEGER
# All numbers below were measured, not assumed; see d3d9shim/README.md.
# --------------------------------------------------------------------------

MIRROR_STRUCTS = [
    {
        "name": "D3DPRESENT_PARAMETERS", "size32": 56, "size64": 64,
        "fields": [
            ("uint32_t", "BackBufferWidth", 0, 0, "scalar"),
            ("uint32_t", "BackBufferHeight", 4, 4, "scalar"),
            ("uint32_t", "BackBufferFormat", 8, 8, "scalar"),
            ("uint32_t", "BackBufferCount", 12, 12, "scalar"),
            ("uint32_t", "MultiSampleType", 16, 16, "scalar"),
            ("uint32_t", "MultiSampleQuality", 20, 20, "scalar"),
            ("uint32_t", "SwapEffect", 24, 24, "scalar"),
            ("uint32_t", "hDeviceWindow", 28, 32, "hwnd"),
            ("int32_t", "Windowed", 32, 40, "scalar"),
            ("int32_t", "EnableAutoDepthStencil", 36, 44, "scalar"),
            ("uint32_t", "AutoDepthStencilFormat", 40, 48, "scalar"),
            ("uint32_t", "Flags", 44, 52, "scalar"),
            ("uint32_t", "FullScreen_RefreshRateInHz", 48, 56, "scalar"),
            ("uint32_t", "PresentationInterval", 52, 60, "scalar"),
        ],
        "note": "INOUT on the five create/reset paths; the runtime writes the "
                "adjusted mode back.  hDeviceWindow is an HWND: 4 bytes on "
                "i386, 8 on LP64, zero-extended and never offset.",
    },
    {
        "name": "D3DDEVICE_CREATION_PARAMETERS", "size32": 16, "size64": 24,
        "fields": [
            ("uint32_t", "AdapterOrdinal", 0, 0, "scalar"),
            ("uint32_t", "DeviceType", 4, 4, "scalar"),
            ("uint32_t", "hFocusWindow", 8, 8, "hwnd"),
            ("uint32_t", "BehaviorFlags", 12, 16, "scalar"),
        ],
        "note": "OUT only (GetCreationParameters).",
    },
    {
        "name": "D3DLOCKED_RECT", "size32": 8, "size64": 16,
        "fields": [
            ("int32_t", "Pitch", 0, 0, "scalar"),
            ("uint32_t", "pBits", 4, 8, "guest_ptr"),
        ],
        "note": "pBits is THE one OUT pointer in the whole API "
                "(WOW64_DESIGN.md 8.2(c)); written back with "
                "ios_wow_guest_ptr32() and always inside the guest arena.",
    },
    {
        "name": "D3DLOCKED_BOX", "size32": 12, "size64": 16,
        "fields": [
            ("int32_t", "RowPitch", 0, 0, "scalar"),
            ("int32_t", "SlicePitch", 4, 4, "scalar"),
            ("uint32_t", "pBits", 8, 8, "guest_ptr"),
        ],
        "note": "same pBits rule as D3DLOCKED_RECT.  pBits happens to sit at "
                "+8 on both ABIs; the SIZE still differs (12 vs 16), so it is "
                "a mirror.",
    },
    {
        "name": "D3DPRESENTSTATS", "size32": 28, "size64": 32,
        "fields": [
            ("uint32_t", "PresentCount", 0, 0, "scalar"),
            ("uint32_t", "PresentRefreshCount", 4, 4, "scalar"),
            ("uint32_t", "SyncRefreshCount", 8, 8, "scalar"),
            ("uint32_t", "SyncQPCTime_lo", 12, 16, "u64_lo"),
            ("uint32_t", "SyncQPCTime_hi", 16, 20, "u64_hi"),
            ("uint32_t", "SyncGPUTime_lo", 20, 24, "u64_lo"),
            ("uint32_t", "SyncGPUTime_hi", 24, 28, "u64_hi"),
        ],
        "note": "NOT among the four mirrors WOW64_DESIGN.md 8.2(c) lists -- "
                "found by measuring every struct that crosses.  LARGE_INTEGER "
                "takes 4-byte alignment on i386 and 8 on LP64, so SyncQPCTime "
                "sits at +12 / +16 and SyncGPUTime at +20 / +24.  Reached "
                "only by IDirect3DSwapChain9Ex::GetPresentStats.",
    },
]

# Layout-identical: same size AND same field offsets on both ABIs, so the
# unix entry points at the converted guest address in place.  Sizes measured
# with i686-w64-mingw32-clang and with gcc on LP64.
IDENTICAL_STRUCTS = [
    ("D3DBOX", 24), ("D3DCAPS9", 304), ("D3DCLIPSTATUS9", 8),
    ("D3DDISPLAYMODE", 16), ("D3DDISPLAYMODEEX", 24),
    ("D3DDISPLAYMODEFILTER", 12), ("D3DGAMMARAMP", 1536),
    ("D3DINDEXBUFFER_DESC", 20), ("D3DLIGHT9", 104), ("D3DMATERIAL9", 68),
    ("D3DMATRIX", 64), ("D3DRASTER_STATUS", 8), ("D3DRECT", 16),
    ("D3DRECTPATCH_INFO", 28), ("D3DSURFACE_DESC", 32),
    ("D3DTRIPATCH_INFO", 16), ("D3DVERTEXBUFFER_DESC", 24),
    ("D3DVERTEXELEMENT9", 8), ("D3DVIEWPORT9", 24), ("D3DVOLUME_DESC", 28),
    ("GUID", 16), ("LUID", 8), ("PALETTEENTRY", 4), ("POINT", 8),
    ("RECT", 16), ("RGNDATA", 36),
]

# Field offsets are identical but the TAIL PADDING is not: LARGE_INTEGER
# DriverVersion gives the struct 8-byte alignment on LP64 and 4 on i386, so
# sizeof() is 1104 there and 1100 here.  Every field is at the same offset,
# so it is still pointed at in place -- but the generator must assert
# offsets, not sizeof, and a copy must use the SMALLER size.
PADDING_ONLY_STRUCTS = [
    ("D3DADAPTER_IDENTIFIER9", 1100, 1104,
     [("DriverVersion", 1056), ("VendorId", 1064),
      ("DeviceIdentifier", 1080), ("WHQLLevel", 1096)]),
]

# --------------------------------------------------------------------------
# Shim object model.  gen_d3d9_thunks.py emits d3d9shim_objects_gen.h from
# this; the hand-written d3d9shim_object.c implements the hooks.
# --------------------------------------------------------------------------

OBJECT_HEADER = [
    ("const void *", "vtbl", "guest vtable -- MUST be first; apps cache and patch it"),
    ("int32_t", "refcount", "public refcount; entirely guest-side"),
    ("int32_t", "priv_refcount", "D3D9 private references (a container's children)"),
    ("uint32_t", "kind", "enum d3d9shim_kind"),
    ("uint32_t", "type_value", "D3DRESOURCETYPE / D3DQUERYTYPE, for local GetType()"),
    ("uint64_t", "native", "native handle (index + generation); 0 = none"),
    ("struct d3d9shim_object *", "parent", "owning device, or the interface"),
]

LIMITS = {
    "D3D9SHIM_MAX_SWAPCHAINS": 8,
    "D3D9SHIM_MAX_BACK_BUFFERS": 8,
    "D3D9SHIM_MAX_TEXTURE_SLOTS": 20,
    "D3D_MAX_SIMULTANEOUS_RENDERTARGETS": 4,
    "D3D9_MAX_VERTEX_STREAMS": 16,
    # d3d9_matrix.hpp:15 -- kTransformStateCount is 10 + 256, not 10: the ten
    # named transforms plus D3DTS_WORLDMATRIX(0..255), which transform_index()
    # compacts onto [10, 266).  At 10 the deferred SetTransform /
    # MultiplyTransform predicate rejected every D3DTS_WORLD(MATRIX) with
    # D3DERR_INVALIDCALL -- i.e. all fixed-function world matrices.
    "D3D9_MAX_TRANSFORMS": 10 + 256,
    "D3D9_MAX_VS_CONST_I": 16,
    "D3D9_MAX_VS_CONST_B": 16,
    "D3D9_MAX_PS_CONST_F": 224,
    "D3D9_MAX_PS_CONST_I": 16,
    "D3D9_MAX_PS_CONST_B": 16,
}

# Per-kind shim state.  Only what a `local` body reads or a `defer` predicate
# names may live here: everything else is the native side's business.
OBJECT_STATE = {
    "D3D9": [],
    "DEVICE": [
        ("struct d3d9shim_object *", "d3d9", "GetDirect3D"),
        ("struct d3d9shim_object *", "swapchains[D3D9SHIM_MAX_SWAPCHAINS]", "GetSwapChain"),
        ("uint32_t", "swapchain_count", "GetNumberOfSwapChains cross-check"),
        ("struct d3d9shim_object *", "render_targets[D3D_MAX_SIMULTANEOUS_RENDERTARGETS]", "GetRenderTarget"),
        ("struct d3d9shim_object *", "depth_stencil", "GetDepthStencilSurface"),
        ("struct d3d9shim_object *", "textures[D3D9SHIM_MAX_TEXTURE_SLOTS]", "GetTexture"),
        ("struct d3d9shim_object *", "vertex_declaration", "GetVertexDeclaration, Draw* predicate"),
        ("struct d3d9shim_object *", "vertex_shader", "GetVertexShader"),
        ("struct d3d9shim_object *", "pixel_shader", "GetPixelShader"),
        ("struct d3d9shim_object *", "stream_buffers[D3D9_MAX_VERTEX_STREAMS]", "GetStreamSource"),
        ("uint32_t", "stream_offsets[D3D9_MAX_VERTEX_STREAMS]", "GetStreamSource"),
        ("uint32_t", "stream_strides[D3D9_MAX_VERTEX_STREAMS]", "GetStreamSource"),
        ("struct d3d9shim_object *", "index_buffer", "GetIndices, DrawIndexed* predicate"),
        ("uint32_t", "vs_const_f_count", "256 on a HW-VP device, 8192 on SW/MIXED (d3d9_device.cpp:11310)"),
        ("int", "in_scene", "Begin/EndScene predicate"),
        ("int", "can_software_vp", "created with SOFTWARE or MIXED"),
        ("int", "can_hardware_vp", "created without SOFTWARE"),
        ("int", "multithreaded", "D3DCREATE_MULTITHREADED -- the shim owns the lock"),
        ("uint32_t", "behavior_flags", "the CreateDevice flags, verbatim"),
    ],
    "SWAPCHAIN": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object *", "back_buffers[D3D9SHIM_MAX_BACK_BUFFERS]", "GetBackBuffer"),
        ("uint32_t", "back_buffer_count", "GetBackBuffer bound"),
    ],
    "SURFACE": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object *", "container", "GetContainer"),
        ("uint32_t", "usage", "SetRenderTarget / SetDepthStencilSurface predicates"),
        ("uint32_t", "priority", "SetPriority returns the PREVIOUS value"),
    ],
    "TEXTURE": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object **", "sublevels", "GetSurfaceLevel"),
        ("uint32_t", "level_count", "GetSurfaceLevel bound"),
        ("uint32_t", "usage", ""),
        ("uint32_t", "priority", "SetPriority returns the PREVIOUS value"),
        ("uint32_t", "lod", "SetLOD returns the PREVIOUS value"),
    ],
    "CUBETEXTURE": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object **", "sublevels", "GetCubeMapSurface, 6 * level_count"),
        ("uint32_t", "level_count", ""),
        ("uint32_t", "usage", ""),
        ("uint32_t", "priority", ""),
        ("uint32_t", "lod", ""),
    ],
    "VOLUMETEXTURE": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object **", "sublevels", "GetVolumeLevel"),
        ("uint32_t", "level_count", ""),
        ("uint32_t", "usage", ""),
        ("uint32_t", "priority", ""),
        ("uint32_t", "lod", ""),
    ],
    "VOLUME": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("struct d3d9shim_object *", "container", "GetContainer"),
    ],
    "VERTEXBUFFER": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("uint32_t", "usage", ""),
        ("uint32_t", "priority", ""),
    ],
    "INDEXBUFFER": [
        ("struct d3d9shim_object *", "device", "GetDevice"),
        ("uint32_t", "usage", ""),
        ("uint32_t", "priority", ""),
    ],
    "VERTEXDECL": [("struct d3d9shim_object *", "device", "GetDevice")],
    "VERTEXSHADER": [("struct d3d9shim_object *", "device", "GetDevice")],
    "PIXELSHADER": [("struct d3d9shim_object *", "device", "GetDevice")],
    "STATEBLOCK": [("struct d3d9shim_object *", "device", "GetDevice")],
    "QUERY": [("struct d3d9shim_object *", "device", "GetDevice")],
}

# Hand-written helpers a `local='identity:helper:X'` body calls.  Each takes
# the receiving object plus the method's own arguments and returns a borrowed
# `struct d3d9shim_object *` (NULL = not available); the generated body does
# the AddRef and the store.  Declared in d3d9shim_objects_gen.h, implemented
# in d3d9shim_object.c.
#
# A helper on a `resolve` slot carries one extra obligation: on a cache MISS
# it makes the single synchronous crossing on that slot's own opcode and
# adopts the native handle the unix entry returns, because nothing else in
# the system can tell the shim what a child's handle is.
IDENTITY_HELPERS = {
    "device_back_buffer": "(device, swapchain_idx, backbuffer_idx, backbuffer_type)",
    "device_texture": "(device, stage)  -- applies texture_stage_to_slot()",
    "device_stream_source": "(device, stream_idx, &offset, &stride)",
    "device_swapchain": "(device, swapchain_idx)  -- resolves on a miss",
    "device_render_target": "(device, idx)  -- resolves on a miss",
    "device_depth_stencil": "(device)  -- resolves on a miss",
    "swapchain_back_buffer": "(swapchain, backbuffer_idx, backbuffer_type)",
    "texture_sublevel": "(texture, level)",
    "cube_surface": "(cube, face, level)",
    "container": "(object, riid)  -- QIs the container to riid",
}

# The per-interface slot counts WOW64_DESIGN.md 8.1 states.  validate()
# refuses to hand the table to the generator unless they still match.
EXPECTED_SLOT_COUNTS = {
    "IDirect3D9Ex": 22, "IDirect3DDevice9Ex": 134, "IDirect3DSwapChain9Ex": 13,
    "IDirect3DSurface9": 17, "IDirect3DTexture9": 22,
    "IDirect3DCubeTexture9": 22, "IDirect3DVolumeTexture9": 22,
    "IDirect3DVolume9": 11, "IDirect3DVertexBuffer9": 14,
    "IDirect3DIndexBuffer9": 14, "IDirect3DVertexDeclaration9": 5,
    "IDirect3DVertexShader9": 5, "IDirect3DPixelShader9": 5,
    "IDirect3DStateBlock9": 6, "IDirect3DQuery9": 8,
}
EXPECTED_TOTAL_SLOTS = 320

# The DLL exports the shim must provide, name and ORDINAL, copied from
# research/dxmt/src/d3d9/d3d9.def -- an app that imports d3d9 by ordinal gets
# the wrong function otherwise.  WOW64_DESIGN.md 8.1 says "10 DLL exports";
# the .def has TWELVE (DebugSetLevel and DebugSetMute were not counted).
# None of them reaches the unix side by itself: 8.2(b) keeps D3DPERF_*,
# DebugSet* and the whole shader-validator state machine shim-local.
DLL_EXPORTS = [
    ("Direct3DCreate9", 16), ("Direct3DCreate9Ex", 20),
    ("D3DPERF_BeginEvent", 23), ("D3DPERF_EndEvent", 24),
    ("D3DPERF_GetStatus", 25), ("D3DPERF_QueryRepeatFrame", 26),
    ("D3DPERF_SetMarker", 27), ("D3DPERF_SetOptions", 28),
    ("D3DPERF_SetRegion", 29), ("DebugSetLevel", 30), ("DebugSetMute", 31),
    ("Direct3DShaderValidatorCreate9", 32),
]

# --------------------------------------------------------------------------
# Transport slots.  Three things the boundary needs that are not vtable
# methods, so nothing in INTERFACES can produce them.  They were hand-written
# in d3d9shim_object.h and had NO unix table entry at all -- the table is
# sized D3D9SHIM_OP_COUNT and they sat past its end, which is why the shim
# logged "is the slot bound?" every time.  Absorbing them here keeps their
# numbers (321, 322, 323 -- immediately after init plus the 320 vtable slots)
# and their layouts byte for byte, and gets them entries in both tables.
#
# `fields` is (C type, name, role) in emission order and follows the same
# rule as a vtable block: uint64_t first, then the 4-byte fields, then `ret`,
# then padding to a multiple of 8.  Roles:
# `slot` is the absolute opcode number, checked against the position the
# generator actually puts it at.
#   scalar:<ctype>      pass by value, cast to <ctype>
#   hwnd32              a 32-bit HWND: widen, never offset (invariant 4)
#   guest_ptr:<size>    a guest address IN, window-checked for <size> bytes
#                       (a field name, or a number) and passed as void *
#   handle_out          a d3d9_native_handle OUT, written back into the block
# `ret` is the C type of the hook's result and `ret_zero_ok` says the hook
# reports success as 0 (the arena registrar already does) rather than as an
# HRESULT.
TRANSPORT_SLOTS = [
    {
        "name": "arena_register",
        "slot": 321,
        "op": "D3D9SHIM_OP_arena_register",
        "block": "d3d9_arena_register_params",
        "hook": "d3d9_native_arena_register",
        "size": 16,
        "ret": "int", "ret_zero_ok": True,
        "fields": [
            ("uint32_t", "guest_base", "scalar:uint32_t"),
            ("uint32_t", "size", "scalar:uint64_t"),
        ],
        "note": "hands the native sub-allocator one VirtualAlloc'd guest "
                "arena chunk (WOW64_DESIGN.md 8.2(c)).  guest_base is a GUEST "
                "address the window chokepoint has already placed inside "
                "[B, B+4G); the native side owns the sub-allocation, so the "
                "entry does NOT convert it.",
    },
    {
        "name": "window_state",
        "slot": 322,
        "op": "D3D9SHIM_OP_window_state",
        "block": "d3d9_window_state_params",
        "hook": "d3d9_native_window_state",
        "size": 24,
        "ret": "HRESULT", "ret_zero_ok": False,
        "fields": [
            ("uint32_t", "hwnd", "hwnd32"),
            ("uint32_t", "width", "scalar:uint32_t"),
            ("uint32_t", "height", "scalar:uint32_t"),
            ("uint32_t", "flags", "scalar:uint32_t"),
        ],
        "note": "the per-HWND client size, visibility and foreground state "
                "wsi_window_madeira.cpp answers from (8.2(d)).  Pushed at "
                "CreateDevice, Reset, Present and from the focus window proc.",
    },
    {
        "name": "create_interface",
        "slot": 323,
        "op": "D3D9SHIM_OP_create_interface",
        "block": "d3d9_create_interface_params",
        "hook": "d3d9_native_create_interface",
        "size": 24,
        "ret": "HRESULT", "ret_zero_ok": False,
        "fields": [
            ("uint64_t", "iface", "handle_out"),
            ("uint32_t", "sdk_version", "scalar:uint32_t"),
            ("uint32_t", "is_ex", "scalar:uint32_t"),
        ],
        "note": "creates the native IDirect3D9(Ex) -- the handle every other "
                "call's `self` descends from.  Direct3DCreate9(Ex) is a DLL "
                "export rather than a vtable slot, so no method in INTERFACES "
                "produces it and there is no iface_out anywhere for it.",
    },
]

# The flags d3d9shim_window.c packs into d3d9_window_state_params::flags.
# Emitted as #defines so the two halves cannot disagree about them.
WINDOW_STATE_FLAGS = [
    ("D3D9SHIM_WINDOW_VISIBLE", 0x1),
    ("D3D9SHIM_WINDOW_FOREGROUND", 0x2),
    ("D3D9SHIM_WINDOW_FULLSCREEN", 0x4),
    ("D3D9SHIM_WINDOW_GONE", 0x8),      # destroyed; drop the entry
]

INTERFACES = [
    I(
        'IDirect3D9Ex', 'D3D9Ex', 'D3D9',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'iid'), A('iface_out:IUnknown', 'void **', 'out')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'RegisterSoftwareDevice', 'HRESULT', [A('u32', 'void *', 'init')],
               disp='local', local='const:D3DERR_NOTAVAILABLE'),
            M(4, 'GetAdapterCount', 'UINT', [],
               disp='sync', flush=True),
            M(5, 'GetAdapterIdentifier', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'DWORD', 'flags'), A('out_struct:D3DADAPTER_IDENTIFIER9', 'D3DADAPTER_IDENTIFIER9 *', 'identifier')],
               disp='sync', flush=True),
            M(6, 'GetAdapterModeCount', 'UINT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DFORMAT', 'format')],
               disp='sync', flush=True),
            M(7, 'EnumAdapterModes', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'UINT', 'mode_idx'), A('out_struct:D3DDISPLAYMODE', 'D3DDISPLAYMODE *', 'mode')],
               disp='sync', flush=True),
            M(8, 'GetAdapterDisplayMode', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('out_struct:D3DDISPLAYMODE', 'D3DDISPLAYMODE *', 'mode')],
               disp='sync', flush=True),
            M(9, 'CheckDeviceType', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('u32', 'D3DFORMAT', 'display_format'), A('u32', 'D3DFORMAT', 'backbuffer_format'), A('u32', 'WINBOOL', 'windowed')],
               disp='sync', flush=True),
            M(10, 'CheckDeviceFormat', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('u32', 'D3DFORMAT', 'adapter_format'), A('u32', 'DWORD', 'usage'), A('u32', 'D3DRESOURCETYPE', 'resource_type'), A('u32', 'D3DFORMAT', 'format')],
               disp='sync', flush=True),
            M(11, 'CheckDeviceMultiSampleType', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('u32', 'D3DFORMAT', 'surface_format'), A('u32', 'WINBOOL', 'windowed'), A('u32', 'D3DMULTISAMPLE_TYPE', 'multisample_type'), A('out_ptr:DWORD', 'DWORD *', 'quality_levels')],
               disp='sync', flush=True),
            M(12, 'CheckDepthStencilMatch', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('u32', 'D3DFORMAT', 'adapter_format'), A('u32', 'D3DFORMAT', 'rt_format'), A('u32', 'D3DFORMAT', 'ds_format')],
               disp='sync', flush=True),
            M(13, 'CheckDeviceFormatConversion', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('u32', 'D3DFORMAT', 'src_format'), A('u32', 'D3DFORMAT', 'dst_format')],
               disp='sync', flush=True),
            M(14, 'GetDeviceCaps', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('out_struct:D3DCAPS9', 'D3DCAPS9 *', 'caps')],
               disp='sync', flush=True),
            M(15, 'GetAdapterMonitor', 'HMONITOR', [A('u32', 'UINT', 'adapter_idx')],
               disp='sync', flush=True),
            M(16, 'CreateDevice', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('hwnd', 'HWND', 'focus_window'), A('u32', 'DWORD', 'flags'), A('inout_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters'), A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9 **', 'device')],
               disp='sync', flush=True, custom=True),
            M(17, 'GetAdapterModeCountEx', 'UINT', [A('u32', 'UINT', 'adapter_idx'), A('in_struct:D3DDISPLAYMODEFILTER', 'const D3DDISPLAYMODEFILTER *', 'filter')],
               disp='sync', flush=True),
            M(18, 'EnumAdapterModesEx', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('in_struct:D3DDISPLAYMODEFILTER', 'const D3DDISPLAYMODEFILTER *', 'filter'), A('u32', 'UINT', 'mode_idx'), A('out_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode')],
               disp='sync', flush=True),
            M(19, 'GetAdapterDisplayModeEx', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('out_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode'), A('out_ptr:D3DDISPLAYROTATION', 'D3DDISPLAYROTATION *', 'rotation')],
               disp='sync', flush=True),
            M(20, 'CreateDeviceEx', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('u32', 'D3DDEVTYPE', 'device_type'), A('hwnd', 'HWND', 'focus_window'), A('u32', 'DWORD', 'flags'), A('inout_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters'), A('in_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode'), A('iface_out:IDirect3DDevice9Ex', 'struct IDirect3DDevice9Ex **', 'device')],
               disp='sync', flush=True, custom=True),
            M(21, 'GetAdapterLUID', 'HRESULT', [A('u32', 'UINT', 'adapter_idx'), A('out_struct:LUID', 'LUID *', 'luid')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DDevice9Ex', 'Device9Ex', 'DEVICE',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'iid'), A('iface_out:IUnknown', 'void **', 'out')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'TestCooperativeLevel', 'HRESULT', [],
               disp='sync', flush=True),
            M(4, 'GetAvailableTextureMem', 'UINT', [],
               disp='sync', flush=True),
            M(5, 'EvictManagedResources', 'HRESULT', [],
               disp='sync', flush=True),
            M(6, 'GetDirect3D', 'HRESULT', [A('iface_out:IDirect3D9', 'IDirect3D9 **', 'd3d9')],
               disp='local', local='identity:d3d9'),
            M(7, 'GetDeviceCaps', 'HRESULT', [A('out_struct:D3DCAPS9', 'D3DCAPS9 *', 'caps')],
               disp='sync', flush=True),
            M(8, 'GetDisplayMode', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('out_struct:D3DDISPLAYMODE', 'D3DDISPLAYMODE *', 'mode')],
               disp='sync', flush=True),
            M(9, 'GetCreationParameters', 'HRESULT', [A('out_struct:D3DDEVICE_CREATION_PARAMETERS', 'D3DDEVICE_CREATION_PARAMETERS *', 'parameters')],
               disp='sync', flush=True),
            M(10, 'SetCursorProperties', 'HRESULT', [A('u32', 'UINT', 'hotspot_x'), A('u32', 'UINT', 'hotspot_y'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'bitmap')],
               disp='sync', flush=True, custom=True),
            M(11, 'SetCursorPosition', 'void', [A('u32', 'int', 'x'), A('u32', 'int', 'y'), A('u32', 'DWORD', 'flags')],
               disp='local', local='shim:cursor_set_position'),
            M(12, 'ShowCursor', 'WINBOOL', [A('u32', 'WINBOOL', 'show')],
               disp='local', local='shim:cursor_show'),
            M(13, 'CreateAdditionalSwapChain', 'HRESULT', [A('inout_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters'), A('iface_out:IDirect3DSwapChain9', 'IDirect3DSwapChain9 **', 'swapchain')],
               disp='sync', flush=True),
            M(14, 'GetSwapChain', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('iface_out:IDirect3DSwapChain9', 'IDirect3DSwapChain9 **', 'swapchain')],
               disp='resolve', local='identity:helper:device_swapchain', null_hr='D3DERR_INVALIDCALL'),
            M(15, 'GetNumberOfSwapChains', 'UINT', [],
               disp='sync', flush=True),
            M(16, 'Reset', 'HRESULT', [A('inout_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters')],
               disp='sync', flush=True, custom=True),
            M(17, 'Present', 'HRESULT', [A('in_struct:RECT', 'const RECT *', 'src_rect'), A('in_struct:RECT', 'const RECT *', 'dst_rect'), A('hwnd', 'HWND', 'dst_window_override'), A('in_struct:RGNDATA', 'const RGNDATA *', 'dirty_region')],
               disp='sync', flush=True, custom=True),
            M(18, 'GetBackBuffer', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('u32', 'UINT', 'backbuffer_idx'), A('u32', 'D3DBACKBUFFER_TYPE', 'backbuffer_type'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'backbuffer')],
               disp='resolve', local='identity:helper:device_back_buffer', null_hr='D3DERR_INVALIDCALL'),
            M(19, 'GetRasterStatus', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('out_struct:D3DRASTER_STATUS', 'D3DRASTER_STATUS *', 'raster_status')],
               disp='sync', flush=True),
            M(20, 'SetDialogBoxMode', 'HRESULT', [A('u32', 'WINBOOL', 'enable')],
               disp='sync', flush=True, custom=True),
            M(21, 'SetGammaRamp', 'void', [A('u32', 'UINT', 'swapchain_idx'), A('u32', 'DWORD', 'flags'), A('in_struct:D3DGAMMARAMP', 'const D3DGAMMARAMP *', 'ramp')],
               disp='sync', flush=True),
            M(22, 'GetGammaRamp', 'void', [A('u32', 'UINT', 'swapchain_idx'), A('out_struct:D3DGAMMARAMP', 'D3DGAMMARAMP *', 'ramp')],
               disp='sync', flush=True),
            M(23, 'CreateTexture', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'UINT', 'levels'), A('u32', 'DWORD', 'usage'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DTexture9', 'IDirect3DTexture9 **', 'texture'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(24, 'CreateVolumeTexture', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'UINT', 'depth'), A('u32', 'UINT', 'levels'), A('u32', 'DWORD', 'usage'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DVolumeTexture9', 'IDirect3DVolumeTexture9 **', 'texture'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(25, 'CreateCubeTexture', 'HRESULT', [A('u32', 'UINT', 'edge_length'), A('u32', 'UINT', 'levels'), A('u32', 'DWORD', 'usage'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DCubeTexture9', 'IDirect3DCubeTexture9 **', 'texture'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(26, 'CreateVertexBuffer', 'HRESULT', [A('u32', 'UINT', 'size'), A('u32', 'DWORD', 'usage'), A('u32', 'DWORD', 'fvf'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 **', 'buffer'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(27, 'CreateIndexBuffer', 'HRESULT', [A('u32', 'UINT', 'size'), A('u32', 'DWORD', 'usage'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DIndexBuffer9', 'IDirect3DIndexBuffer9 **', 'buffer'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(28, 'CreateRenderTarget', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DMULTISAMPLE_TYPE', 'multisample_type'), A('u32', 'DWORD', 'multisample_quality'), A('u32', 'WINBOOL', 'lockable'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(29, 'CreateDepthStencilSurface', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DMULTISAMPLE_TYPE', 'multisample_type'), A('u32', 'DWORD', 'multisample_quality'), A('u32', 'WINBOOL', 'discard'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(30, 'UpdateSurface', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'src_surface'), A('in_struct:RECT', 'const RECT *', 'src_rect'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'dst_surface'), A('in_struct:POINT', 'const POINT *', 'dst_point')],
               disp='sync', flush=True),
            M(31, 'UpdateTexture', 'HRESULT', [A('iface_in:IDirect3DBaseTexture9', 'IDirect3DBaseTexture9 *', 'src_texture'), A('iface_in:IDirect3DBaseTexture9', 'IDirect3DBaseTexture9 *', 'dst_texture')],
               disp='sync', flush=True),
            M(32, 'GetRenderTargetData', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'render_target'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'dst_surface')],
               disp='sync', flush=True),
            M(33, 'GetFrontBufferData', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'dst_surface')],
               disp='sync', flush=True),
            M(34, 'StretchRect', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'src_surface'), A('in_struct:RECT', 'const RECT *', 'src_rect'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'dst_surface'), A('in_struct:RECT', 'const RECT *', 'dst_rect'), A('u32', 'D3DTEXTUREFILTERTYPE', 'filter')],
               disp='sync', flush=True),
            M(35, 'ColorFill', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'surface'), A('in_struct:RECT', 'const RECT *', 'rect'), A('u32', 'D3DCOLOR', 'colour')],
               disp='sync', flush=True),
            M(36, 'CreateOffscreenPlainSurface', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle')],
               disp='sync', flush=True),
            M(37, 'SetRenderTarget', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'surface')],
               disp='defer',
               pred='idx < D3D_MAX_SIMULTANEOUS_RENDERTARGETS and not (idx == 0 and is_null(surface)) and same_device(surface) and (is_null(surface) or has_usage(surface, D3DUSAGE_RENDERTARGET))', ret_const='D3D_OK'),
            M(38, 'GetRenderTarget', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface')],
               disp='resolve', local='identity:helper:device_render_target', null_hr='D3DERR_NOTFOUND'),
            M(39, 'SetDepthStencilSurface', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'depth_stencil')],
               disp='defer',
               pred='same_device(depth_stencil) and (is_null(depth_stencil) or has_usage(depth_stencil, D3DUSAGE_DEPTHSTENCIL))', ret_const='D3D_OK'),
            M(40, 'GetDepthStencilSurface', 'HRESULT', [A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'depth_stencil')],
               disp='resolve', local='identity:helper:device_depth_stencil', null_hr='D3DERR_NOTFOUND'),
            M(41, 'BeginScene', 'HRESULT', [],
               disp='defer',
               pred='not self.in_scene', ret_const='D3D_OK'),
            M(42, 'EndScene', 'HRESULT', [],
               disp='defer',
               pred='self.in_scene', ret_const='D3D_OK', flush=True),
            M(43, 'Clear', 'HRESULT', [A('u32', 'DWORD', 'rect_count'), A('in_array:D3DRECT:rect_count', 'const D3DRECT *', 'rects'), A('u32', 'DWORD', 'flags'), A('u32', 'D3DCOLOR', 'colour'), A('u32', 'float', 'z'), A('u32', 'DWORD', 'stencil')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(44, 'SetTransform', 'HRESULT', [A('u32', 'D3DTRANSFORMSTATETYPE', 'state'), A('in_struct:D3DMATRIX', 'const D3DMATRIX *', 'matrix')],
               disp='defer',
               pred='not is_null(matrix) and d3d9_transform_index(state) < D3D9_MAX_TRANSFORMS', ret_const='D3D_OK'),
            M(45, 'GetTransform', 'HRESULT', [A('u32', 'D3DTRANSFORMSTATETYPE', 'State'), A('out_struct:D3DMATRIX', 'D3DMATRIX *', 'matrix')],
               disp='sync', flush=True),
            M(46, 'MultiplyTransform', 'HRESULT', [A('u32', 'D3DTRANSFORMSTATETYPE', 'state'), A('in_struct:D3DMATRIX', 'const D3DMATRIX *', 'matrix')],
               disp='defer',
               pred='not is_null(matrix) and d3d9_transform_index(state) < D3D9_MAX_TRANSFORMS', ret_const='D3D_OK'),
            M(47, 'SetViewport', 'HRESULT', [A('in_struct:D3DVIEWPORT9', 'const D3DVIEWPORT9 *', 'viewport')],
               disp='defer',
               pred='not is_null(viewport)', ret_const='D3D_OK'),
            M(48, 'GetViewport', 'HRESULT', [A('out_struct:D3DVIEWPORT9', 'D3DVIEWPORT9 *', 'viewport')],
               disp='sync', flush=True),
            M(49, 'SetMaterial', 'HRESULT', [A('in_struct:D3DMATERIAL9', 'const D3DMATERIAL9 *', 'material')],
               disp='defer',
               pred='not is_null(material)', ret_const='D3D_OK'),
            M(50, 'GetMaterial', 'HRESULT', [A('out_struct:D3DMATERIAL9', 'D3DMATERIAL9 *', 'material')],
               disp='sync', flush=True),
            M(51, 'SetLight', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('in_struct:D3DLIGHT9', 'const D3DLIGHT9 *', 'light')],
               disp='defer',
               pred='not is_null(light)', ret_const='D3D_OK'),
            M(52, 'GetLight', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('out_struct:D3DLIGHT9', 'D3DLIGHT9 *', 'light')],
               disp='sync', flush=True),
            M(53, 'LightEnable', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('u32', 'WINBOOL', 'enable')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(54, 'GetLightEnable', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('out_ptr:WINBOOL', 'WINBOOL *', 'enable')],
               disp='sync', flush=True),
            M(55, 'SetClipPlane', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('in_array:float:4', 'const float *', 'plane')],
               disp='defer',
               pred='not is_null(plane)', ret_const='D3D_OK'),
            M(56, 'GetClipPlane', 'HRESULT', [A('u32', 'DWORD', 'idx'), A('out_array:float:4', 'float *', 'plane')],
               disp='sync', flush=True),
            M(57, 'SetRenderState', 'HRESULT', [A('u32', 'D3DRENDERSTATETYPE', 'state'), A('u32', 'DWORD', 'value')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(58, 'GetRenderState', 'HRESULT', [A('u32', 'D3DRENDERSTATETYPE', 'state'), A('out_ptr:DWORD', 'DWORD *', 'value')],
               disp='sync', flush=True),
            M(59, 'CreateStateBlock', 'HRESULT', [A('u32', 'D3DSTATEBLOCKTYPE', 'type'), A('iface_out:IDirect3DStateBlock9', 'IDirect3DStateBlock9 **', 'stateblock')],
               disp='sync', flush=True),
            M(60, 'BeginStateBlock', 'HRESULT', [],
               disp='sync', flush=True),
            M(61, 'EndStateBlock', 'HRESULT', [A('iface_out:IDirect3DStateBlock9', 'IDirect3DStateBlock9 **', 'stateblock')],
               disp='sync', flush=True),
            M(62, 'SetClipStatus', 'HRESULT', [A('in_struct:D3DCLIPSTATUS9', 'const D3DCLIPSTATUS9 *', 'clip_status')],
               disp='defer',
               pred='not is_null(clip_status)', ret_const='D3D_OK'),
            M(63, 'GetClipStatus', 'HRESULT', [A('out_struct:D3DCLIPSTATUS9', 'D3DCLIPSTATUS9 *', 'clip_status')],
               disp='sync', flush=True),
            M(64, 'GetTexture', 'HRESULT', [A('u32', 'DWORD', 'stage'), A('iface_out:IDirect3DBaseTexture9', 'IDirect3DBaseTexture9 **', 'texture')],
               disp='local', local='identity:helper:device_texture'),
            M(65, 'SetTexture', 'HRESULT', [A('u32', 'DWORD', 'stage'), A('iface_in:IDirect3DBaseTexture9', 'IDirect3DBaseTexture9 *', 'texture')],
               disp='defer',
               pred='same_device(texture)', ret_const='D3D_OK'),
            M(66, 'GetTextureStageState', 'HRESULT', [A('u32', 'DWORD', 'stage'), A('u32', 'D3DTEXTURESTAGESTATETYPE', 'state'), A('out_ptr:DWORD', 'DWORD *', 'value')],
               disp='sync', flush=True),
            M(67, 'SetTextureStageState', 'HRESULT', [A('u32', 'DWORD', 'stage'), A('u32', 'D3DTEXTURESTAGESTATETYPE', 'state'), A('u32', 'DWORD', 'value')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(68, 'GetSamplerState', 'HRESULT', [A('u32', 'DWORD', 'sampler_idx'), A('u32', 'D3DSAMPLERSTATETYPE', 'state'), A('out_ptr:DWORD', 'DWORD *', 'value')],
               disp='sync', flush=True),
            M(69, 'SetSamplerState', 'HRESULT', [A('u32', 'DWORD', 'sampler_idx'), A('u32', 'D3DSAMPLERSTATETYPE', 'state'), A('u32', 'DWORD', 'value')],
               disp='defer',
               pred='state <= D3DSAMP_DMAPOFFSET', ret_const='D3D_OK'),
            M(70, 'ValidateDevice', 'HRESULT', [A('out_ptr:DWORD', 'DWORD *', 'pass_count')],
               disp='sync', flush=True),
            M(71, 'SetPaletteEntries', 'HRESULT', [A('u32', 'UINT', 'palette_idx'), A('in_array:PALETTEENTRY:256', 'const PALETTEENTRY *', 'entries')],
               disp='defer',
               pred='not is_null(entries)', ret_const='D3D_OK'),
            M(72, 'GetPaletteEntries', 'HRESULT', [A('u32', 'UINT', 'palette_idx'), A('out_array:PALETTEENTRY:256', 'PALETTEENTRY *', 'entries')],
               disp='sync', flush=True),
            M(73, 'SetCurrentTexturePalette', 'HRESULT', [A('u32', 'UINT', 'palette_idx')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(74, 'GetCurrentTexturePalette', 'HRESULT', [A('out_ptr:UINT', 'UINT *', 'palette_idx')],
               disp='sync', flush=True),
            M(75, 'SetScissorRect', 'HRESULT', [A('in_struct:RECT', 'const RECT *', 'rect')],
               disp='defer',
               pred='not is_null(rect)', ret_const='D3D_OK'),
            M(76, 'GetScissorRect', 'HRESULT', [A('out_struct:RECT', 'RECT *', 'rect')],
               disp='sync', flush=True),
            M(77, 'SetSoftwareVertexProcessing', 'HRESULT', [A('u32', 'WINBOOL', 'software')],
               disp='defer',
               pred='(software and self.can_software_vp) or (not software and self.can_hardware_vp)', ret_const='D3D_OK'),
            M(78, 'GetSoftwareVertexProcessing', 'WINBOOL', [],
               disp='sync', flush=True),
            M(79, 'SetNPatchMode', 'HRESULT', [A('u32', 'float', 'segment_count')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(80, 'GetNPatchMode', 'float', [],
               disp='sync', flush=True),
            M(81, 'DrawPrimitive', 'HRESULT', [A('u32', 'D3DPRIMITIVETYPE', 'primitive_type'), A('u32', 'UINT', 'start_vertex'), A('u32', 'UINT', 'primitive_count')],
               disp='defer',
               pred='not is_null(self.vertex_declaration)', ret_const='D3D_OK'),
            M(82, 'DrawIndexedPrimitive', 'HRESULT', [A('u32', 'D3DPRIMITIVETYPE', 'primitive_type'), A('u32', 'INT', 'base_vertex_idx'), A('u32', 'UINT', 'min_vertex_idx'), A('u32', 'UINT', 'vertex_count'), A('u32', 'UINT', 'start_idx'), A('u32', 'UINT', 'primitive_count')],
               disp='defer',
               pred='not is_null(self.vertex_declaration) and not is_null(self.index_buffer)', ret_const='D3D_OK'),
            M(83, 'DrawPrimitiveUP', 'HRESULT', [A('u32', 'D3DPRIMITIVETYPE', 'primitive_type'), A('u32', 'UINT', 'primitive_count'), A('user_mem_in:d3d9_up_vertex_bytes(primitive_type, primitive_count, stride)', 'const void *', 'data'), A('u32', 'UINT', 'stride')],
               disp='sync', flush=True),
            M(84, 'DrawIndexedPrimitiveUP', 'HRESULT', [A('u32', 'D3DPRIMITIVETYPE', 'primitive_type'), A('u32', 'UINT', 'min_vertex_idx'), A('u32', 'UINT', 'vertex_count'), A('u32', 'UINT', 'primitive_count'), A('user_mem_in:d3d9_up_index_bytes(primitive_type, primitive_count, index_format)', 'const void *', 'index_data'), A('u32', 'D3DFORMAT', 'index_format'), A('user_mem_in:(min_vertex_idx + vertex_count) * stride', 'const void *', 'data'), A('u32', 'UINT', 'stride')],
               disp='sync', flush=True),
            M(85, 'ProcessVertices', 'HRESULT', [A('u32', 'UINT', 'src_start_idx'), A('u32', 'UINT', 'dst_idx'), A('u32', 'UINT', 'vertex_count'), A('iface_in:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 *', 'dst_buffer'), A('iface_in:IDirect3DVertexDeclaration9', 'IDirect3DVertexDeclaration9 *', 'declaration'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(86, 'CreateVertexDeclaration', 'HRESULT', [A('in_array:D3DVERTEXELEMENT9:d3d9_decl_element_count(elements)', 'const D3DVERTEXELEMENT9 *', 'elements'), A('iface_out:IDirect3DVertexDeclaration9', 'IDirect3DVertexDeclaration9 **', 'declaration')],
               disp='sync', flush=True),
            M(87, 'SetVertexDeclaration', 'HRESULT', [A('iface_in:IDirect3DVertexDeclaration9', 'IDirect3DVertexDeclaration9 *', 'declaration')],
               disp='defer',
               pred='same_device(declaration)', ret_const='D3D_OK'),
            M(88, 'GetVertexDeclaration', 'HRESULT', [A('iface_out:IDirect3DVertexDeclaration9', 'IDirect3DVertexDeclaration9 **', 'declaration')],
               disp='local', local='identity:vertex_declaration'),
            M(89, 'SetFVF', 'HRESULT', [A('u32', 'DWORD', 'fvf')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(90, 'GetFVF', 'HRESULT', [A('out_ptr:DWORD', 'DWORD *', 'fvf')],
               disp='sync', flush=True),
            M(91, 'CreateVertexShader', 'HRESULT', [A('in_array:DWORD:d3d9_shader_token_count(byte_code)', 'const DWORD *', 'byte_code'), A('iface_out:IDirect3DVertexShader9', 'IDirect3DVertexShader9 **', 'shader')],
               disp='sync', flush=True),
            M(92, 'SetVertexShader', 'HRESULT', [A('iface_in:IDirect3DVertexShader9', 'IDirect3DVertexShader9 *', 'shader')],
               disp='defer',
               pred='same_device(shader)', ret_const='D3D_OK'),
            M(93, 'GetVertexShader', 'HRESULT', [A('iface_out:IDirect3DVertexShader9', 'IDirect3DVertexShader9 **', 'shader')],
               disp='local', local='identity:vertex_shader'),
            M(94, 'SetVertexShaderConstantF', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:float:count * 4', 'const float *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= self.vs_const_f_count and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(95, 'GetVertexShaderConstantF', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:float:count * 4', 'float *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(96, 'SetVertexShaderConstantI', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:int:count * 4', 'const int *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= D3D9_MAX_VS_CONST_I and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(97, 'GetVertexShaderConstantI', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:int:count * 4', 'int *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(98, 'SetVertexShaderConstantB', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:WINBOOL:count', 'const WINBOOL *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= D3D9_MAX_VS_CONST_B and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(99, 'GetVertexShaderConstantB', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:WINBOOL:count', 'WINBOOL *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(100, 'SetStreamSource', 'HRESULT', [A('u32', 'UINT', 'stream_idx'), A('iface_in:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 *', 'buffer'), A('u32', 'UINT', 'offset'), A('u32', 'UINT', 'stride')],
               disp='defer',
               pred='stream_idx < D3D9_MAX_VERTEX_STREAMS and same_device(buffer)', ret_const='D3D_OK'),
            M(101, 'GetStreamSource', 'HRESULT', [A('u32', 'UINT', 'stream_idx'), A('iface_out:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 **', 'buffer'), A('out_ptr:UINT', 'UINT *', 'offset'), A('out_ptr:UINT', 'UINT *', 'stride')],
               disp='local', local='identity:helper:device_stream_source'),
            M(102, 'SetStreamSourceFreq', 'HRESULT', [A('u32', 'UINT', 'stream_idx'), A('u32', 'UINT', 'frequency')],
               disp='defer',
               pred='stream_idx < D3D9_MAX_VERTEX_STREAMS and frequency != 0 and not (frequency & D3DSTREAMSOURCE_INDEXEDDATA and frequency & D3DSTREAMSOURCE_INSTANCEDATA) and not (stream_idx == 0 and frequency & D3DSTREAMSOURCE_INSTANCEDATA)', ret_const='D3D_OK'),
            M(103, 'GetStreamSourceFreq', 'HRESULT', [A('u32', 'UINT', 'stream_idx'), A('out_ptr:UINT', 'UINT *', 'frequency')],
               disp='sync', flush=True),
            M(104, 'SetIndices', 'HRESULT', [A('iface_in:IDirect3DIndexBuffer9', 'IDirect3DIndexBuffer9 *', 'buffer')],
               disp='defer',
               pred='same_device(buffer)', ret_const='D3D_OK'),
            M(105, 'GetIndices', 'HRESULT', [A('iface_out:IDirect3DIndexBuffer9', 'IDirect3DIndexBuffer9 **', 'buffer')],
               disp='local', local='identity:index_buffer'),
            M(106, 'CreatePixelShader', 'HRESULT', [A('in_array:DWORD:d3d9_shader_token_count(byte_code)', 'const DWORD *', 'byte_code'), A('iface_out:IDirect3DPixelShader9', 'IDirect3DPixelShader9 **', 'shader')],
               disp='sync', flush=True),
            M(107, 'SetPixelShader', 'HRESULT', [A('iface_in:IDirect3DPixelShader9', 'IDirect3DPixelShader9 *', 'shader')],
               disp='defer',
               pred='same_device(shader)', ret_const='D3D_OK'),
            M(108, 'GetPixelShader', 'HRESULT', [A('iface_out:IDirect3DPixelShader9', 'IDirect3DPixelShader9 **', 'shader')],
               disp='local', local='identity:pixel_shader'),
            M(109, 'SetPixelShaderConstantF', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:float:count * 4', 'const float *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= D3D9_MAX_PS_CONST_F and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(110, 'GetPixelShaderConstantF', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:float:count * 4', 'float *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(111, 'SetPixelShaderConstantI', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:int:count * 4', 'const int *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= D3D9_MAX_PS_CONST_I and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(112, 'GetPixelShaderConstantI', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:int:count * 4', 'int *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(113, 'SetPixelShaderConstantB', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('in_array:WINBOOL:count', 'const WINBOOL *', 'data'), A('u32', 'UINT', 'count')],
               disp='defer',
               pred='reg_idx <= UINT_MAX - count and reg_idx + count <= D3D9_MAX_PS_CONST_B and (count == 0 or not is_null(data))', ret_const='D3D_OK'),
            M(114, 'GetPixelShaderConstantB', 'HRESULT', [A('u32', 'UINT', 'reg_idx'), A('out_array:WINBOOL:count', 'WINBOOL *', 'data'), A('u32', 'UINT', 'count')],
               disp='sync', flush=True),
            M(115, 'DrawRectPatch', 'HRESULT', [A('u32', 'UINT', 'handle'), A('in_array:float:4', 'const float *', 'segment_count'), A('in_struct:D3DRECTPATCH_INFO', 'const D3DRECTPATCH_INFO *', 'patch_info')],
               disp='sync', flush=True),
            M(116, 'DrawTriPatch', 'HRESULT', [A('u32', 'UINT', 'handle'), A('in_array:float:3', 'const float *', 'segment_count'), A('in_struct:D3DTRIPATCH_INFO', 'const D3DTRIPATCH_INFO *', 'patch_info')],
               disp='sync', flush=True),
            M(117, 'DeletePatch', 'HRESULT', [A('u32', 'UINT', 'handle')],
               disp='sync', flush=True),
            M(118, 'CreateQuery', 'HRESULT', [A('u32', 'D3DQUERYTYPE', 'type'), A('iface_out:IDirect3DQuery9', 'IDirect3DQuery9 **', 'query')],
               disp='sync', flush=True),
            M(119, 'SetConvolutionMonoKernel', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('in_array:float:width', 'float *', 'rows'), A('in_array:float:height', 'float *', 'columns')],
               disp='sync', flush=True),
            M(120, 'ComposeRects', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'src_surface'), A('iface_in:IDirect3DSurface9', 'IDirect3DSurface9 *', 'dst_surface'), A('iface_in:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 *', 'src_descs'), A('u32', 'UINT', 'rect_count'), A('iface_in:IDirect3DVertexBuffer9', 'IDirect3DVertexBuffer9 *', 'dst_descs'), A('u32', 'D3DCOMPOSERECTSOP', 'operation'), A('u32', 'INT', 'offset_x'), A('u32', 'INT', 'offset_y')],
               disp='sync', flush=True),
            M(121, 'PresentEx', 'HRESULT', [A('in_struct:RECT', 'const RECT *', 'src_rect'), A('in_struct:RECT', 'const RECT *', 'dst_rect'), A('hwnd', 'HWND', 'dst_window_override'), A('in_struct:RGNDATA', 'const RGNDATA *', 'dirty_region'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True, custom=True),
            M(122, 'GetGPUThreadPriority', 'HRESULT', [A('out_ptr:INT', 'INT *', 'priority')],
               disp='sync', flush=True),
            M(123, 'SetGPUThreadPriority', 'HRESULT', [A('u32', 'INT', 'priority')],
               disp='sync', flush=True),
            M(124, 'WaitForVBlank', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx')],
               disp='sync', flush=True),
            M(125, 'CheckResourceResidency', 'HRESULT', [A('in_array:IDirect3DResource9:resource_count', 'IDirect3DResource9 **', 'resources'), A('u32', 'UINT32', 'resource_count')],
               disp='sync', flush=True),
            M(126, 'SetMaximumFrameLatency', 'HRESULT', [A('u32', 'UINT', 'max_latency')],
               disp='sync', flush=True),
            M(127, 'GetMaximumFrameLatency', 'HRESULT', [A('out_ptr:UINT', 'UINT *', 'max_latency')],
               disp='sync', flush=True),
            M(128, 'CheckDeviceState', 'HRESULT', [A('hwnd', 'HWND', 'dst_window')],
               disp='sync', flush=True),
            M(129, 'CreateRenderTargetEx', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DMULTISAMPLE_TYPE', 'multisample_type'), A('u32', 'DWORD', 'multisample_quality'), A('u32', 'WINBOOL', 'lockable'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle'), A('u32', 'DWORD', 'usage')],
               disp='sync', flush=True),
            M(130, 'CreateOffscreenPlainSurfaceEx', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'Height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DPOOL', 'pool'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle'), A('u32', 'DWORD', 'usage')],
               disp='sync', flush=True),
            M(131, 'CreateDepthStencilSurfaceEx', 'HRESULT', [A('u32', 'UINT', 'width'), A('u32', 'UINT', 'height'), A('u32', 'D3DFORMAT', 'format'), A('u32', 'D3DMULTISAMPLE_TYPE', 'multisample_type'), A('u32', 'DWORD', 'multisample_quality'), A('u32', 'WINBOOL', 'discard'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9 **', 'surface'), A('shared_handle_inout', 'HANDLE *', 'shared_handle'), A('u32', 'DWORD', 'usage')],
               disp='sync', flush=True),
            M(132, 'ResetEx', 'HRESULT', [A('inout_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters'), A('in_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode')],
               disp='sync', flush=True, custom=True),
            M(133, 'GetDisplayModeEx', 'HRESULT', [A('u32', 'UINT', 'swapchain_idx'), A('out_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode'), A('out_ptr:D3DDISPLAYROTATION', 'D3DDISPLAYROTATION *', 'rotation')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DSwapChain9Ex', 'SwapChain9Ex', 'SWAPCHAIN',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'iid'), A('iface_out:IUnknown', 'void **', 'out')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'Present', 'HRESULT', [A('in_struct:RECT', 'const RECT *', 'src_rect'), A('in_struct:RECT', 'const RECT *', 'dst_rect'), A('hwnd', 'HWND', 'dst_window_override'), A('in_struct:RGNDATA', 'const RGNDATA *', 'dirty_region'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True, custom=True),
            M(4, 'GetFrontBufferData', 'HRESULT', [A('iface_in:IDirect3DSurface9', 'struct IDirect3DSurface9 *', 'dst_surface')],
               disp='sync', flush=True),
            M(5, 'GetBackBuffer', 'HRESULT', [A('u32', 'UINT', 'backbuffer_idx'), A('u32', 'D3DBACKBUFFER_TYPE', 'backbuffer_type'), A('iface_out:IDirect3DSurface9', 'struct IDirect3DSurface9 **', 'backbuffer')],
               disp='resolve', local='identity:helper:swapchain_back_buffer', null_hr='D3DERR_INVALIDCALL'),
            M(6, 'GetRasterStatus', 'HRESULT', [A('out_struct:D3DRASTER_STATUS', 'D3DRASTER_STATUS *', 'raster_status')],
               disp='sync', flush=True),
            M(7, 'GetDisplayMode', 'HRESULT', [A('out_struct:D3DDISPLAYMODE', 'D3DDISPLAYMODE *', 'mode')],
               disp='sync', flush=True),
            M(8, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9 **', 'device')],
               disp='local', local='identity:device'),
            M(9, 'GetPresentParameters', 'HRESULT', [A('out_struct:D3DPRESENT_PARAMETERS', 'D3DPRESENT_PARAMETERS *', 'parameters')],
               disp='sync', flush=True),
            M(10, 'GetLastPresentCount', 'HRESULT', [A('out_ptr:UINT', 'UINT *', 'last_present_count')],
               disp='sync', flush=True),
            M(11, 'GetPresentStats', 'HRESULT', [A('out_struct:D3DPRESENTSTATS', 'D3DPRESENTSTATS *', 'stats')],
               disp='sync', flush=True),
            M(12, 'GetDisplayModeEx', 'HRESULT', [A('out_struct:D3DDISPLAYMODEEX', 'D3DDISPLAYMODEEX *', 'mode'), A('out_ptr:D3DDISPLAYROTATION', 'D3DDISPLAYROTATION *', 'rotation')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DSurface9', 'Surface9', 'SURFACE',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'GetContainer', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppContainer')],
               disp='local', local='identity:helper:container', null_hr='D3DERR_INVALIDCALL'),
            M(12, 'GetDesc', 'HRESULT', [A('out_struct:D3DSURFACE_DESC', 'D3DSURFACE_DESC*', 'pDesc')],
               disp='sync', flush=True),
            M(13, 'LockRect', 'HRESULT', [A('locked_rect_out', 'D3DLOCKED_RECT *', 'locked_rect'), A('in_struct:RECT', 'const RECT *', 'rect'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(14, 'UnlockRect', 'HRESULT', [],
               disp='sync', flush=True),
            M(15, 'GetDC', 'HRESULT', [A('handle', 'HDC*', 'phdc')],
               disp='sync', flush=True, custom=True),
            M(16, 'ReleaseDC', 'HRESULT', [A('handle', 'HDC', 'hdc')],
               disp='sync', flush=True, custom=True),
        ]),
    I(
        'IDirect3DTexture9', 'Texture9', 'TEXTURE',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'SetLOD', 'DWORD', [A('u32', 'DWORD', 'LODNew')],
               disp='defer',
               pred='True', ret_const='shadow:lod'),
            M(12, 'GetLOD', 'DWORD', [],
               disp='sync', flush=True),
            M(13, 'GetLevelCount', 'DWORD', [],
               disp='sync', flush=True),
            M(14, 'SetAutoGenFilterType', 'HRESULT', [A('u32', 'D3DTEXTUREFILTERTYPE', 'FilterType')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(15, 'GetAutoGenFilterType', 'D3DTEXTUREFILTERTYPE', [],
               disp='sync', flush=True),
            M(16, 'GenerateMipSubLevels', 'void', [],
               disp='sync', flush=True),
            M(17, 'GetLevelDesc', 'HRESULT', [A('u32', 'UINT', 'Level'), A('out_struct:D3DSURFACE_DESC', 'D3DSURFACE_DESC*', 'pDesc')],
               disp='sync', flush=True),
            M(18, 'GetSurfaceLevel', 'HRESULT', [A('u32', 'UINT', 'Level'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9**', 'ppSurfaceLevel')],
               disp='resolve', local='identity:helper:texture_sublevel', null_hr='D3DERR_INVALIDCALL'),
            M(19, 'LockRect', 'HRESULT', [A('u32', 'UINT', 'level'), A('locked_rect_out', 'D3DLOCKED_RECT *', 'locked_rect'), A('in_struct:RECT', 'const RECT *', 'rect'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(20, 'UnlockRect', 'HRESULT', [A('u32', 'UINT', 'Level')],
               disp='sync', flush=True),
            M(21, 'AddDirtyRect', 'HRESULT', [A('in_struct:RECT', 'const RECT *', 'dirty_rect')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
        ]),
    I(
        'IDirect3DCubeTexture9', 'CubeTexture9', 'CUBETEXTURE',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'SetLOD', 'DWORD', [A('u32', 'DWORD', 'LODNew')],
               disp='defer',
               pred='True', ret_const='shadow:lod'),
            M(12, 'GetLOD', 'DWORD', [],
               disp='sync', flush=True),
            M(13, 'GetLevelCount', 'DWORD', [],
               disp='sync', flush=True),
            M(14, 'SetAutoGenFilterType', 'HRESULT', [A('u32', 'D3DTEXTUREFILTERTYPE', 'FilterType')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(15, 'GetAutoGenFilterType', 'D3DTEXTUREFILTERTYPE', [],
               disp='sync', flush=True),
            M(16, 'GenerateMipSubLevels', 'void', [],
               disp='sync', flush=True),
            M(17, 'GetLevelDesc', 'HRESULT', [A('u32', 'UINT', 'Level'), A('out_struct:D3DSURFACE_DESC', 'D3DSURFACE_DESC*', 'pDesc')],
               disp='sync', flush=True),
            M(18, 'GetCubeMapSurface', 'HRESULT', [A('u32', 'D3DCUBEMAP_FACES', 'FaceType'), A('u32', 'UINT', 'Level'), A('iface_out:IDirect3DSurface9', 'IDirect3DSurface9**', 'ppCubeMapSurface')],
               disp='resolve', local='identity:helper:cube_surface', null_hr='D3DERR_INVALIDCALL'),
            M(19, 'LockRect', 'HRESULT', [A('u32', 'D3DCUBEMAP_FACES', 'face'), A('u32', 'UINT', 'level'), A('locked_rect_out', 'D3DLOCKED_RECT *', 'locked_rect'), A('in_struct:RECT', 'const RECT *', 'rect'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(20, 'UnlockRect', 'HRESULT', [A('u32', 'D3DCUBEMAP_FACES', 'FaceType'), A('u32', 'UINT', 'Level')],
               disp='sync', flush=True),
            M(21, 'AddDirtyRect', 'HRESULT', [A('u32', 'D3DCUBEMAP_FACES', 'face'), A('in_struct:RECT', 'const RECT *', 'dirty_rect')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
        ]),
    I(
        'IDirect3DVolumeTexture9', 'VolumeTexture9', 'VOLUMETEXTURE',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'SetLOD', 'DWORD', [A('u32', 'DWORD', 'LODNew')],
               disp='defer',
               pred='True', ret_const='shadow:lod'),
            M(12, 'GetLOD', 'DWORD', [],
               disp='sync', flush=True),
            M(13, 'GetLevelCount', 'DWORD', [],
               disp='sync', flush=True),
            M(14, 'SetAutoGenFilterType', 'HRESULT', [A('u32', 'D3DTEXTUREFILTERTYPE', 'FilterType')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(15, 'GetAutoGenFilterType', 'D3DTEXTUREFILTERTYPE', [],
               disp='sync', flush=True),
            M(16, 'GenerateMipSubLevels', 'void', [],
               disp='sync', flush=True),
            M(17, 'GetLevelDesc', 'HRESULT', [A('u32', 'UINT', 'Level'), A('out_struct:D3DVOLUME_DESC', 'D3DVOLUME_DESC *', 'pDesc')],
               disp='sync', flush=True),
            M(18, 'GetVolumeLevel', 'HRESULT', [A('u32', 'UINT', 'Level'), A('iface_out:IDirect3DVolume9', 'IDirect3DVolume9**', 'ppVolumeLevel')],
               disp='resolve', local='identity:helper:texture_sublevel', null_hr='D3DERR_INVALIDCALL'),
            M(19, 'LockBox', 'HRESULT', [A('u32', 'UINT', 'level'), A('locked_box_out', 'D3DLOCKED_BOX *', 'locked_box'), A('in_struct:D3DBOX', 'const D3DBOX *', 'box'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(20, 'UnlockBox', 'HRESULT', [A('u32', 'UINT', 'Level')],
               disp='sync', flush=True),
            M(21, 'AddDirtyBox', 'HRESULT', [A('in_struct:D3DBOX', 'const D3DBOX *', 'dirty_box')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
        ]),
    I(
        'IDirect3DVolume9', 'Volume9', 'VOLUME',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'GetContainer', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppContainer')],
               disp='local', local='identity:helper:container', null_hr='D3DERR_INVALIDCALL'),
            M(8, 'GetDesc', 'HRESULT', [A('out_struct:D3DVOLUME_DESC', 'D3DVOLUME_DESC*', 'pDesc')],
               disp='sync', flush=True),
            M(9, 'LockBox', 'HRESULT', [A('locked_box_out', 'D3DLOCKED_BOX *', 'locked_box'), A('in_struct:D3DBOX', 'const D3DBOX *', 'box'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(10, 'UnlockBox', 'HRESULT', [],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DVertexBuffer9', 'VertexBuffer9', 'VERTEXBUFFER',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'Lock', 'HRESULT', [A('u32', 'UINT', 'OffsetToLock'), A('u32', 'UINT', 'SizeToLock'), A('guest_ptr_out', 'void**', 'ppbData'), A('u32', 'DWORD', 'Flags')],
               disp='sync', flush=True),
            M(12, 'Unlock', 'HRESULT', [],
               disp='sync', flush=True),
            M(13, 'GetDesc', 'HRESULT', [A('out_struct:D3DVERTEXBUFFER_DESC', 'D3DVERTEXBUFFER_DESC*', 'pDesc')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DIndexBuffer9', 'IndexBuffer9', 'INDEXBUFFER',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'SetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'guid'), A('in_array:BYTE:data_size', 'const void *', 'data'), A('u32', 'DWORD', 'data_size'), A('u32', 'DWORD', 'flags')],
               disp='sync', flush=True),
            M(5, 'GetPrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid'), A('out_array:BYTE:*pSizeOfData', 'void*', 'pData'), A('out_ptr:DWORD', 'DWORD*', 'pSizeOfData')],
               disp='sync', flush=True),
            M(6, 'FreePrivateData', 'HRESULT', [A('in_struct:GUID', 'REFGUID', 'refguid')],
               disp='sync', flush=True),
            M(7, 'SetPriority', 'DWORD', [A('u32', 'DWORD', 'PriorityNew')],
               disp='defer',
               pred='True', ret_const='shadow:priority'),
            M(8, 'GetPriority', 'DWORD', [],
               disp='sync', flush=True),
            M(9, 'PreLoad', 'void', [],
               disp='defer',
               pred='True'),
            M(10, 'GetType', 'D3DRESOURCETYPE', [],
               disp='local', local='gettype'),
            M(11, 'Lock', 'HRESULT', [A('u32', 'UINT', 'OffsetToLock'), A('u32', 'UINT', 'SizeToLock'), A('guest_ptr_out', 'void**', 'ppbData'), A('u32', 'DWORD', 'Flags')],
               disp='sync', flush=True),
            M(12, 'Unlock', 'HRESULT', [],
               disp='sync', flush=True),
            M(13, 'GetDesc', 'HRESULT', [A('out_struct:D3DINDEXBUFFER_DESC', 'D3DINDEXBUFFER_DESC*', 'pDesc')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DVertexDeclaration9', 'VertexDeclaration9', 'VERTEXDECL',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'GetDeclaration', 'HRESULT', [A('out_array:D3DVERTEXELEMENT9:*pNumElements', 'D3DVERTEXELEMENT9*', 'arg'), A('out_ptr:UINT', 'UINT*', 'pNumElements')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DVertexShader9', 'VertexShader9', 'VERTEXSHADER',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'GetFunction', 'HRESULT', [A('out_array:BYTE:*pSizeOfData', 'void*', 'arg'), A('out_ptr:UINT', 'UINT*', 'pSizeOfData')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DPixelShader9', 'PixelShader9', 'PIXELSHADER',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'GetFunction', 'HRESULT', [A('out_array:BYTE:*pSizeOfData', 'void*', 'arg'), A('out_ptr:UINT', 'UINT*', 'pSizeOfData')],
               disp='sync', flush=True),
        ]),
    I(
        'IDirect3DStateBlock9', 'StateBlock9', 'STATEBLOCK',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'Capture', 'HRESULT', [],
               disp='sync', flush=True),
            M(5, 'Apply', 'HRESULT', [],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
        ]),
    I(
        'IDirect3DQuery9', 'Query9', 'QUERY',
        [
            M(0, 'QueryInterface', 'HRESULT', [A('in_struct:GUID', 'REFIID', 'riid'), A('iface_out:IUnknown', 'void**', 'ppvObject')],
               disp='local', local='qi'),
            M(1, 'AddRef', 'ULONG', [],
               disp='local', local='addref'),
            M(2, 'Release', 'ULONG', [],
               disp='defer',
               pred='True', ret_const='refcount'),
            M(3, 'GetDevice', 'HRESULT', [A('iface_out:IDirect3DDevice9', 'struct IDirect3DDevice9**', 'ppDevice')],
               disp='local', local='identity:device'),
            M(4, 'GetType', 'D3DQUERYTYPE', [],
               disp='local', local='gettype'),
            M(5, 'GetDataSize', 'DWORD', [],
               disp='sync', flush=True),
            M(6, 'Issue', 'HRESULT', [A('u32', 'DWORD', 'dwIssueFlags')],
               disp='defer',
               pred='True', ret_const='D3D_OK'),
            M(7, 'GetData', 'HRESULT', [A('out_array:BYTE:dwSize', 'void*', 'pData'), A('u32', 'DWORD', 'dwSize'), A('u32', 'DWORD', 'dwGetDataFlags')],
               disp='sync', flush=True),
        ]),
]


# --------------------------------------------------------------------------
# Derived views
# --------------------------------------------------------------------------

def iter_methods():
    """(interface, method) in canonical order: interface order, then slot."""
    for it in INTERFACES:
        for m in it["methods"]:
            yield it, m


def opcode_name(it, m):
    return "D3D9OP_%s_%s" % (it["short"], m["name"])


def symbol(it, m):
    return "%s_%s" % (it["short"], m["name"])


def block_struct(it, m):
    return "d3d9_%s_%s_params" % (it["short"], m["name"])


def block_fields(m):
    """The parameter block's fields, in emission order.

    uint64_t first (every one then sits on a multiple of 8 whatever the
    target's alignment rule for 64-bit types is), then 4-byte fields, then
    the result.  Returns [(c_type, name, role)] where role is one of
    'self', 'arg', 'ret'.
    """
    wide, narrow = [], []
    for a in m["args"]:
        store = SHAPE_STORAGE[a["tag"]]
        if store == "u64":
            wide.append(("uint64_t", a["name"], "arg"))
        elif a["ctype"].strip() == "float":
            narrow.append(("float", a["name"], "arg"))
        else:
            narrow.append(("uint32_t", a["name"], "arg"))
    out = [("uint64_t", "self", "self")] + wide + narrow
    ret = RET_STORAGE[m["ret"]]
    if ret is not None:
        if ret == "uint64_t":
            out.insert(1 + len(wide), ("uint64_t", "ret", "ret"))
        else:
            out.append((ret, "ret", "ret"))
    return out


def transport_fields(t):
    """A transport block's fields, in emission order: the described ones (the
    u64s already first), then `ret`, then nothing -- pad_words fills the rest.
    Returns [(c_type, name, role)] with the same role vocabulary as
    block_fields(): 'arg' or 'ret'."""
    out = [(ctype, name, "arg") for ctype, name, _role in t["fields"]]
    out.append((RET_STORAGE[t["ret"]], "ret", "ret"))
    return out


def _sizeof(fields):
    off = 0
    for ctype, _name, _role in fields:
        width = 8 if ctype == "uint64_t" else 4
        off = (off + width - 1) // width * width
        off += width
    return (off + 7) // 8 * 8, off


def transport_size(t):
    return _sizeof(transport_fields(t))[0]


def transport_pad_words(t):
    total, off = _sizeof(transport_fields(t))
    return (total - off) // 4


def block_size(m):
    """sizeof() of the emitted parameter block, identical on both ABIs."""
    fields = block_fields(m)
    off = 0
    for ctype, _name, _role in fields:
        width = 8 if ctype == "uint64_t" else 4
        off = (off + width - 1) // width * width
        off += width
    return (off + 7) // 8 * 8


def pad_words(m):
    """uint32_t padding words the emitted struct needs to reach block_size."""
    fields = block_fields(m)
    off = 0
    for ctype, _name, _role in fields:
        width = 8 if ctype == "uint64_t" else 4
        off = (off + width - 1) // width * width
        off += width
    return (block_size(m) - off) // 4


# --------------------------------------------------------------------------
# Guard rails (WOW64_DESIGN.md 8.7): the generator refuses to emit on ANY
# inconsistency.  Returns a list of problem strings; empty means good.
# --------------------------------------------------------------------------

# Callables a predicate or a count expression may name.  Anything else that
# looks like a function call is a typo waiting to become a silent miscompare.
EXPR_CALLS = {
    "is_null": "((x) == NULL)",
    "same_device": "the receiver's device owns x, or x is NULL",
    "has_usage": "x's D3DUSAGE bits include the given flag",
    "d3d9_transform_index": "d3d9_matrix.hpp transform_index()",
    "d3d9_decl_element_count": "elements up to and including D3DDECL_END()",
    "d3d9_shader_token_count": "tokens up to and including 0x0000ffff",
    "d3d9_up_vertex_bytes": "vertex bytes a Draw*UP call reads",
    "d3d9_up_index_bytes": "index bytes a DrawIndexedPrimitiveUP call reads",
}

_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_PYKEYWORDS = {"and", "or", "not", "True", "False"}


def _check_expr(where, what, expr, it, argnames):
    """Every name in a predicate / count expression must resolve.

    Resolvable: a Python keyword, an argument of this method, `self.<field>`
    where <field> is declared in OBJECT_STATE for this interface's kind, a
    call in EXPR_CALLS, or an ALL_CAPS constant (a D3D9 #define or a LIMITS
    entry).  This is the guard rail that stops `Type <= D3DSAMP_DMAPOFFSET`
    from shipping when the argument is actually called `state`.
    """
    if not expr:
        return []
    bad = []
    fields = {f[1].split("[")[0] for f in OBJECT_STATE.get(it["kind"], ())}
    for m in re.finditer(r"(self\.)?(" + _IDENT.pattern + r")\s*(\()?", expr):
        qualified, name, call = m.group(1), m.group(2), m.group(3)
        if qualified:
            if name not in fields:
                bad.append("%s: %s names self.%s, not in OBJECT_STATE[%s]"
                           % (where, what, name, it["kind"]))
            continue
        if call and name not in _PYKEYWORDS:
            if name not in EXPR_CALLS:
                bad.append("%s: %s calls unknown %s()" % (where, what, name))
            continue
        if name in _PYKEYWORDS or name in argnames or name in LIMITS:
            continue
        if name == name.upper():          # a D3D9 #define
            continue
        bad.append("%s: %s names %r, which is not an argument, self.<field>, "
                   "a LIMITS entry or an ALL_CAPS constant" % (where, what, name))
    return bad


def validate():
    problems = []
    seen_short = set()
    seen_iface = set()
    total = 0

    for it in INTERFACES:
        if it["iface"] in seen_iface:
            problems.append("duplicate interface %s" % it["iface"])
        seen_iface.add(it["iface"])
        if it["short"] in seen_short:
            problems.append("duplicate short name %s" % it["short"])
        seen_short.add(it["short"])
        if it["kind"] not in OBJECT_STATE:
            problems.append("%s: no OBJECT_STATE for kind %s" % (it["iface"], it["kind"]))

        slots = [m["slot"] for m in it["methods"]]
        if slots != list(range(len(slots))):
            problems.append("%s: slot numbers are not 0..n-1 (%r)" % (it["iface"], slots))
        names = [m["name"] for m in it["methods"]]
        if len(set(names)) != len(names):
            dup = [n for n in set(names) if names.count(n) > 1]
            problems.append("%s: duplicate method name(s) %r" % (it["iface"], dup))

        want = EXPECTED_SLOT_COUNTS.get(it["iface"])
        if want is None:
            problems.append("%s: not in EXPECTED_SLOT_COUNTS" % it["iface"])
        elif want != len(it["methods"]):
            problems.append("%s: %d slots, WOW64_DESIGN.md 8.1 says %d"
                            % (it["iface"], len(it["methods"]), want))
        total += len(it["methods"])

        for m in it["methods"]:
            where = "%s::%s" % (it["iface"], m["name"])
            if m["disp"] not in DISPOSITIONS:
                problems.append("%s: bad disposition %r" % (where, m["disp"]))
            if m["ret"] not in RET_STORAGE:
                problems.append("%s: return type %r has no storage class"
                                % (where, m["ret"]))
            if m["disp"] == "defer" and not m["pred"]:
                problems.append("%s: deferred with no validation predicate" % where)
            if m["disp"] == "defer" and m["ret"] != "void" and not m["ret_const"]:
                problems.append("%s: deferred with no constant return" % where)
            if m["disp"] in ("local", "resolve") and not m["local"]:
                problems.append("%s: %s with no local kind" % (where, m["disp"]))
            if m["disp"] not in ("local", "resolve") and m["local"]:
                problems.append("%s: local kind on a non-local method" % where)
            if m["disp"] in ("local", "resolve") and m["custom"]:
                problems.append("%s: local and custom both set -- a local body "
                                "already has a shim: hook if it needs one" % where)
            if m["disp"] == "sync" and not m["flush"]:
                problems.append("%s: sync must flush the ring" % where)
            # A resolve is a local body whose helper may CROSS on a cache
            # miss, so the identity has to come from a helper (a plain field
            # cannot resolve anything) and there has to be an iface_out for
            # the unix entry to write the child's handle into.
            if m["disp"] == "resolve":
                if not (m["local"] or "").startswith("identity:helper:"):
                    problems.append("%s: resolve without an identity helper -- "
                                    "a plain field cannot resolve a miss" % where)
                if not any(a["tag"] == "iface_out" for a in m["args"]):
                    problems.append("%s: resolve with no iface_out argument "
                                    "for the unix entry to answer into" % where)
                if m["ret"] != "HRESULT":
                    problems.append("%s: resolve must return HRESULT" % where)
            # guest_ptr_out is the mapped-memory OUT pointer; the shim body
            # refuses a NULL one itself, so the method has to have an HRESULT
            # to refuse with, and two of them in one block would make
            # "which pointer was written back" ambiguous.
            gp = [a for a in m["args"] if a["tag"] == "guest_ptr_out"]
            if gp and m["ret"] != "HRESULT":
                problems.append("%s: guest_ptr_out needs an HRESULT return"
                                % where)
            if len(gp) > 1:
                problems.append("%s: more than one guest_ptr_out" % where)
            if m["local"] and m["local"].startswith("identity:helper:"):
                helper = m["local"].split(":", 2)[2]
                if helper not in IDENTITY_HELPERS:
                    problems.append("%s: unknown identity helper %r" % (where, helper))
            argnames = [a["name"] for a in m["args"]]
            if len(set(argnames)) != len(argnames):
                problems.append("%s: duplicate argument name" % where)
            if "self" in argnames or "ret" in argnames:
                problems.append("%s: argument shadows a block field" % where)
            problems += _check_expr(where, "predicate", m["pred"], it, argnames)
            for a in m["args"]:
                expr = None
                if a["tag"] in ("in_array", "out_array"):
                    expr = a["shape"].split(":", 2)[2]
                elif a["tag"] == "user_mem_in":
                    expr = a["shape"].split(":", 1)[1]
                problems += _check_expr(where, "count/size for " + a["name"],
                                        expr, it, argnames)
            for a in m["args"]:
                if a["tag"] not in SHAPE_STORAGE:
                    problems.append("%s: shape %r has no storage class"
                                    % (where, a["shape"]))
            # guarded: block_size() needs a known return storage class, and a
            # bad one is already reported above.
            if m["ret"] in RET_STORAGE and block_size(m) % 8:
                problems.append("%s: block size %d is not a multiple of 8"
                                % (where, block_size(m)))

    if total != EXPECTED_TOTAL_SLOTS:
        problems.append("%d slots total, WOW64_DESIGN.md 8.1 says %d"
                        % (total, EXPECTED_TOTAL_SLOTS))

    # Mirrors: the described i386 image must be exactly 4-byte packed, with
    # every declared offset32 landing where a plain C struct of those fields
    # would put it, and the offsets adding up to size32.  If that ever stops
    # holding, the wire form is no longer the i386 memory image and every
    # generated conversion is wrong.
    MIRROR_ROLES = ("scalar", "hwnd", "guest_ptr", "u64_lo", "u64_hi")
    for s in MIRROR_STRUCTS:
        off = 0
        prev64 = -1
        for ctype, name, off32, off64, role in s["fields"]:
            where = "%s.%s" % (s["name"], name)
            if ctype not in ("uint32_t", "int32_t"):
                problems.append("%s: mirror fields must be 4 bytes, got %s"
                                % (where, ctype))
            if role not in MIRROR_ROLES:
                problems.append("%s: unknown mirror role %r" % (where, role))
            if off32 != off:
                problems.append("%s: offset32 %d, 4-byte packing says %d"
                                % (where, off32, off))
            if off64 <= prev64:
                problems.append("%s: offset64 %d does not advance" % (where, off64))
            prev64 = off64
            off += 4
        if off != s["size32"]:
            problems.append("%s: fields total %d but size32 is %d"
                            % (s["name"], off, s["size32"]))
        if s["size64"] < s["size32"]:
            problems.append("%s: size64 %d < size32 %d"
                            % (s["name"], s["size64"], s["size32"]))
        if s["size32"] % 4 or s["size64"] % 4:
            problems.append("%s: mirror size is not 4-byte aligned" % s["name"])

    # Transport slots.  Their numbers and their layouts are an ABI the
    # hand-written half already ships against, so both are asserted here and
    # again in C; `size` is the number d3d9shim_object.h _Static_asserted.
    TRANSPORT_ROLES = ("hwnd32", "handle_out")
    t_names = [t["name"] for t in TRANSPORT_SLOTS]
    if len(set(t_names)) != len(t_names):
        problems.append("duplicate transport slot")
    for i, t in enumerate(TRANSPORT_SLOTS):
        where = "transport %s" % t["name"]
        if t["slot"] != 1 + EXPECTED_TOTAL_SLOTS + i:
            problems.append("%s: declared slot %d, but it lands at %d -- the "
                            "transport numbers are an ABI the hand-written "
                            "half already calls with"
                            % (where, t["slot"], 1 + EXPECTED_TOTAL_SLOTS + i))
        if t["op"] != "D3D9SHIM_OP_" + t["name"]:
            problems.append("%s: opcode name %r does not match" % (where, t["op"]))
        if t["ret"] not in RET_STORAGE or RET_STORAGE[t["ret"]] is None:
            problems.append("%s: return type %r has no storage class"
                            % (where, t["ret"]))
        fnames = [f[1] for f in t["fields"]]
        if len(set(fnames)) != len(fnames) or "ret" in fnames:
            problems.append("%s: duplicate or reserved field name" % where)
        seen_narrow = False
        for ctype, name, role in t["fields"]:
            if ctype not in ("uint32_t", "int32_t", "uint64_t"):
                problems.append("%s.%s: field type %r is not fixed width"
                                % (where, name, ctype))
            if ctype == "uint64_t" and seen_narrow:
                problems.append("%s.%s: uint64_t after a 4-byte field -- the "
                                "block layout rule is 64-bit fields FIRST"
                                % (where, name))
            if ctype != "uint64_t":
                seen_narrow = True
            base = role.split(":", 1)[0]
            if base not in TRANSPORT_ROLES and base not in ("scalar", "guest_ptr"):
                problems.append("%s.%s: unknown role %r" % (where, name, role))
            if base in ("scalar", "guest_ptr") and ":" not in role:
                problems.append("%s.%s: role %r needs an argument"
                                % (where, name, role))
            if base == "guest_ptr":
                size = role.split(":", 1)[1]
                if not size.isdigit() and size not in fnames:
                    problems.append("%s.%s: window size %r is neither a number "
                                    "nor another field" % (where, name, size))
            if base == "handle_out" and ctype != "uint64_t":
                problems.append("%s.%s: a handle_out must be uint64_t"
                                % (where, name))
        if transport_size(t) != t["size"]:
            problems.append("%s: computed block size %d, declared %d -- the "
                            "hand-written _Static_assert says %d"
                            % (where, transport_size(t), t["size"], t["size"]))
        if t["size"] % 8:
            problems.append("%s: block size %d is not a multiple of 8"
                            % (where, t["size"]))

    names = [s["name"] for s in MIRROR_STRUCTS]
    if len(set(names)) != len(names):
        problems.append("duplicate mirror struct")
    ident = [n for n, _ in IDENTICAL_STRUCTS]
    if len(set(ident)) != len(ident):
        problems.append("duplicate layout-identical struct")
    both = set(names) & set(ident)
    if both:
        problems.append("struct(s) both mirrored and layout-identical: %r"
                        % sorted(both))

    return problems


# --------------------------------------------------------------------------
# The handshake hash.  Covers everything that changes the wire format: slot
# order, names, shapes, dispositions, predicates, block layouts, mirrors.
# It deliberately does NOT cover comments or this file's formatting.
# --------------------------------------------------------------------------

def canonical_text():
    out = ["d3d9shim-api v%d" % API_VERSION]
    for it in INTERFACES:
        out.append("I %s %s %s %d" % (it["iface"], it["short"], it["kind"],
                                      len(it["methods"])))
        for m in it["methods"]:
            out.append("M %d %s %s %s %s %s %s %d %d" % (
                m["slot"], m["name"], m["ret"], m["disp"],
                m["local"] or "-", m["pred"] or "-", m["ret_const"] or "-",
                int(m["flush"]), int(m["custom"])))
            for a in m["args"]:
                out.append("A %s %s" % (a["shape"], a["name"]))
            for ctype, name, role in block_fields(m):
                out.append("F %s %s %s" % (ctype, name, role))
            out.append("Z %d" % block_size(m))
    for s in MIRROR_STRUCTS:
        out.append("R %s %d %d" % (s["name"], s["size32"], s["size64"]))
        for ctype, name, off32, off64, role in s["fields"]:
            out.append("RF %s %s %d %d %s" % (ctype, name, off32, off64, role))
    for name, size in IDENTICAL_STRUCTS:
        out.append("S %s %d" % (name, size))
    for name, size32, size64, offs in PADDING_ONLY_STRUCTS:
        out.append("P %s %d %d %s" % (name, size32, size64,
                                      ",".join("%s=%d" % o for o in offs)))
    # The three transport slots are part of the wire format: their numbers
    # follow the vtable range, so anything that moves them, renames them or
    # re-lays-out one of their blocks has to fail the handshake too.
    for i, t in enumerate(TRANSPORT_SLOTS):
        out.append("T %d %d %s %s %s %s %d" % (i, t["slot"], t["name"], t["op"],
                                               t["hook"], t["ret"], t["size"]))
        for ctype, name, role in t["fields"]:
            out.append("TF %s %s %s" % (ctype, name, role))
        for ctype, name, role in transport_fields(t):
            out.append("TL %s %s %s" % (ctype, name, role))
    for name, value in WINDOW_STATE_FLAGS:
        out.append("W %s %d" % (name, value))
    return "\n".join(out) + "\n"


def api_hash():
    """The 64-bit handshake value compiled into both halves."""
    digest = hashlib.sha256(canonical_text().encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


# --------------------------------------------------------------------------

def _main():
    problems = validate()
    print("d3d9_api.py -- %d interfaces" % len(INTERFACES))
    print()
    print("%-32s %6s %6s %7s %6s %6s"
          % ("interface", "slots", "local", "resolve", "sync", "defer"))
    total = Counter()
    for it in INTERFACES:
        c = Counter(m["disp"] for m in it["methods"])
        total.update(c)
        print("%-32s %6d %6d %7d %6d %6d"
              % (it["iface"], len(it["methods"]), c["local"], c["resolve"],
                 c["sync"], c["defer"]))
    print("%-32s %6d %6d %7d %6d %6d"
          % ("TOTAL", sum(len(i["methods"]) for i in INTERFACES),
             total["local"], total["resolve"], total["sync"], total["defer"]))
    print()
    print("dispositions")
    for k in DISPOSITIONS:
        print("  %-12s %4d" % (k, total[k]))
    print()
    print("argument shapes (%d arguments over %d slots)"
          % (sum(len(m["args"]) for _, m in iter_methods()),
             sum(1 for _ in iter_methods())))
    shapes = Counter(a["tag"] for _, m in iter_methods() for a in m["args"])
    for k in sorted(shapes, key=lambda x: (-shapes[x], x)):
        print("  %-22s %4d" % (k, shapes[k]))
    print()
    print("return types")
    rets = Counter(m["ret"] for _, m in iter_methods())
    for k in sorted(rets, key=lambda x: (-rets[x], x)):
        print("  %-22s %4d   -> %s" % (k, rets[k], RET_STORAGE[k] or "void"))
    print()
    sizes = [block_size(m) for _, m in iter_methods()]
    print("parameter blocks: %d..%d bytes, %d bytes total"
          % (min(sizes), max(sizes), sum(sizes)))
    print("transport slots: %d (%s), %d bytes of block"
          % (len(TRANSPORT_SLOTS),
             ", ".join("%s=%d" % (t["name"], 1 + EXPECTED_TOTAL_SLOTS + i)
                       for i, t in enumerate(TRANSPORT_SLOTS)),
             sum(transport_size(t) for t in TRANSPORT_SLOTS)))
    print("mirrors: %d, layout-identical: %d, padding-only: %d"
          % (len(MIRROR_STRUCTS), len(IDENTICAL_STRUCTS), len(PADDING_ONLY_STRUCTS)))
    print("flush on return: %d slots, custom shim body: %d slots"
          % (sum(1 for _, m in iter_methods() if m["flush"]),
             sum(1 for _, m in iter_methods() if m["custom"])))
    print()
    print("D3D9SHIM_API_HASH = 0x%016x" % api_hash())
    print()
    if problems:
        print("VALIDATION FAILED (%d):" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    print("validation: OK")
    return 0


if __name__ == "__main__":
    sys.exit(_main())


