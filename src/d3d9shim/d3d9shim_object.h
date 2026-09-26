/*
 * d3d9shim_object.h -- private declarations shared by the hand-written half
 *                      of the i386 D3D9 shim
 *
 * Nothing here is part of the generated hook contract: d3d9shim_objects_gen.h
 * declares everything the generated thunks call, and this header declares what
 * the hand-written files (d3d9shim_object.c, d3d9shim_arena.c,
 * d3d9shim_lock.c, d3d9shim_window.c, d3d9shim_fpu.c, d3d9shim_custom.c,
 * d3d9shim_main.c) need from each other.
 *
 * Copyright 2026 Will Faust
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __MADEIRA_D3D9SHIM_OBJECT_H
#define __MADEIRA_D3D9SHIM_OBJECT_H

#include "d3d9shim_objects_gen.h"

/* ------------------------------------------------------------------------
 * The three transport slots (WOW64_DESIGN.md 8.2(c), 8.2(d)) used to be
 * declared here, because d3d9_api.py described only the 320 vtable slots plus
 * `_d3d9_init` and had no way to spell an opcode that is not a vtable method.
 *
 * They are now in the DESCRIPTION (d3d9_api.py's TRANSPORT_SLOTS), which is
 * what finally gets them entries in the two unix dispatch tables: those are
 * sized D3D9SHIM_OP_COUNT, so while the numbers were D3D9SHIM_OP_COUNT + n
 * they sat past the end of both tables and every call on one failed to bind.
 * D3D9SHIM_OP_COUNT is now 324 and includes them.
 *
 * The opcodes (D3D9SHIM_OP_arena_register 321, D3D9SHIM_OP_window_state 322,
 * D3D9SHIM_OP_create_interface 323), the three parameter blocks and the
 * D3D9SHIM_WINDOW_* flags are all emitted into d3d9shim_ops.h under the same
 * names and byte-for-byte the same layouts, so nothing that used them here
 * had to change.  The layout asserts below are kept deliberately: they are
 * now a check ON the generated structs, so a description edit that re-lays
 * one of these blocks out fails THIS file's build, which is the half that
 * writes them.
 * ------------------------------------------------------------------------ */
_Static_assert(sizeof(struct d3d9_arena_register_params) == 16,
               "d3d9_arena_register_params");
_Static_assert(sizeof(struct d3d9_window_state_params) == 24,
               "d3d9_window_state_params");
_Static_assert(sizeof(struct d3d9_create_interface_params) == 24,
               "d3d9_create_interface_params");
_Static_assert(D3D9SHIM_OP_arena_register == 321
               && D3D9SHIM_OP_window_state == 322
               && D3D9SHIM_OP_create_interface == 323,
               "the transport opcodes moved");

/* The distinguished status the native sub-allocator returns when the arena is
 * exhausted; d3d9shim_native_call() grows and retries exactly once rather than
 * turning it into an opaque E_OUTOFMEMORY (8.9-6).  Generated into
 * d3d9shim_ops.h so both halves read it from one place; kept here as a
 * fallback so this header still stands alone. */
#ifndef D3D9SHIM_STATUS_ARENA_EXHAUSTED
#define D3D9SHIM_STATUS_ARENA_EXHAUSTED  0xC0000017u  /* STATUS_NO_MEMORY */
#endif

/* ------------------------------------------------------------------------
 * The object allocation.
 *
 * The generated code casts an interface pointer straight to the per-kind
 * struct, so the union MUST be first and MUST keep those layouts; everything
 * after it is private to the hand-written files and invisible to the thunks.
 * ------------------------------------------------------------------------ */

#define D3D9SHIM_F_SUBRESOURCE   0x0001u /* mip level / cube face / volume:
                                          * AddRef+Release delegate to the
                                          * container (d3d9_surface.cpp:
                                          * MTLD3D9Surface::AddRef) */
#define D3D9SHIM_F_IMPLICIT      0x0002u /* handed out at public 0 and kept by
                                          * a private reference; Release
                                          * clamps at 0 */
#define D3D9SHIM_F_DEVICE_CHILD  0x0004u /* pins the device on the public
                                          * 0 -> 1 edge */
#define D3D9SHIM_F_CHILDREN_DONE 0x0008u /* the child cache is populated */
#define D3D9SHIM_F_IS_EX         0x0010u /* created through the Ex entry */
#define D3D9SHIM_F_DEAD          0x0020u /* destruction in progress */

struct d3d9shim_device_extra;

struct d3d9shim_impl {
    union {
        struct d3d9shim_object        hdr;
        struct d3d9shim_d3d9          d3d9;
        struct d3d9shim_device        device;
        struct d3d9shim_swapchain     swapchain;
        struct d3d9shim_surface       surface;
        struct d3d9shim_texture       texture;
        struct d3d9shim_cubetexture   cubetexture;
        struct d3d9shim_volumetexture volumetexture;
        struct d3d9shim_volume        volume;
        struct d3d9shim_vertexbuffer  vertexbuffer;
        struct d3d9shim_indexbuffer   indexbuffer;
        struct d3d9shim_vertexdecl    vertexdecl;
        struct d3d9shim_vertexshader  vertexshader;
        struct d3d9shim_pixelshader   pixelshader;
        struct d3d9shim_stateblock    stateblock;
        struct d3d9shim_query         query;
    } o;

    struct d3d9shim_impl *         hash_next;   /* native handle -> object */
    uint32_t                       flags;
    uint32_t                       sublevel_slots; /* allocated entries */
    struct d3d9shim_device_extra * extra;        /* devices only */
    HDC                            gdi_dc;       /* surfaces only: GetDC */
    HANDLE                         gdi_bitmap;
};

#define D3D9SHIM_IMPL(obj) ((struct d3d9shim_impl *)(void *)(obj))

/* Per-device state that is neither guest-visible nor part of the generated
 * shadow: the MULTITHREADED lock, the window machine and the cursor. */
struct d3d9shim_device_extra {
    /* d3d9shim_lock.c -- recursive, keyed by GetCurrentThreadId (8.2(d)) */
    volatile LONG  lock_owner;
    LONG           lock_depth;

    /* d3d9shim_window.c -- the moved user32/gdi32 state */
    HWND           focus_window;
    HWND           device_window;
    HWND           fullscreen_window;
    LONG           saved_style;
    LONG           saved_ex_style;
    RECT           saved_rect;
    HMONITOR       fullscreen_monitor;
    int            focus_hooked;
    int            focus_unicode;
    volatile LONG  focus_filtered;
    HCURSOR        hw_cursor;
    int            cursor_visible;
    int            cursor_image_set;

    /* the present parameters as the runtime last accepted them */
    UINT           backbuffer_width;
    UINT           backbuffer_height;
    int            windowed;
    int            is_ex;
};

/* ---- d3d9shim_object.c ------------------------------------------------- */
struct d3d9shim_impl *d3d9shim_obj_alloc(unsigned int kind, uint64_t native,
                                         struct d3d9shim_object *parent,
                                         uint32_t flags);
struct d3d9shim_impl *d3d9shim_obj_lookup(uint64_t native);
void  d3d9shim_obj_addref_private(struct d3d9shim_object *obj);
void  d3d9shim_obj_release_private(struct d3d9shim_object *obj);
/* Remembers the receiver of the call currently in flight so that an object
 * created by it can find its parent device without a second crossing. */
void  d3d9shim_obj_note_call(unsigned int op, const void *block);
/* Reset invalidates every cached implicit child identity. */
void  d3d9shim_device_invalidate_children(struct d3d9shim_device *dev);
struct d3d9shim_device_extra *d3d9shim_extra(struct d3d9shim_device *dev);
const void *d3d9shim_kind_vtbl(unsigned int kind);

/* ---- d3d9shim_main.c (transport) --------------------------------------- */
int   d3d9shim_transport_init(void);
int   d3d9shim_transport_ready(void);
void  d3d9shim_trace(const char *msg);

/* ---- d3d9shim_arena.c -------------------------------------------------- */
int   d3d9shim_arena_init(void);
void  d3d9shim_arena_report(void);

/* ---- d3d9shim_fpu.c ---------------------------------------------------- */
void  d3d9shim_setup_fpu(void);

/* ---- d3d9shim_window.c ------------------------------------------------- */
void  d3d9shim_window_push(HWND hwnd, int fullscreen);
void  d3d9shim_window_forget(HWND hwnd);
void  d3d9shim_window_enter_fullscreen(struct d3d9shim_device *dev, HWND window,
                                       UINT width, UINT height);
void  d3d9shim_window_leave_fullscreen(struct d3d9shim_device *dev);
void  d3d9shim_window_hook_focus(struct d3d9shim_device *dev, HWND fallback);
void  d3d9shim_window_unhook_focus(struct d3d9shim_device *dev);
HRESULT d3d9shim_window_set_cursor(struct d3d9shim_device *dev, UINT hotspot_x,
                                   UINT hotspot_y, IDirect3DSurface9 *bitmap);
void  d3d9shim_window_cursor_set_position(struct d3d9shim_device *dev, int x, int y);
WINBOOL d3d9shim_window_cursor_show(struct d3d9shim_device *dev, WINBOOL show);
void  d3d9shim_window_on_device_reset(struct d3d9shim_device *dev,
                                      const D3DPRESENT_PARAMETERS *params);
void  d3d9shim_window_on_device_destroy(struct d3d9shim_device *dev);

/* ---- d3d9shim_custom.c ------------------------------------------------- */
HRESULT d3d9shim_custom_create_device(IDirect3D9Ex *iface, UINT adapter_idx,
                                      D3DDEVTYPE device_type, HWND focus_window,
                                      DWORD flags,
                                      D3DPRESENT_PARAMETERS *parameters,
                                      D3DDISPLAYMODEEX *mode, int is_ex,
                                      void **device_out);

#endif /* __MADEIRA_D3D9SHIM_OBJECT_H */
