/*
 * d3d9shim_object.c -- the guest-side object model of the i386 D3D9 shim
 *
 * WOW64_DESIGN.md 8.2(b): objects and vtables live in guest memory by
 * construction (the image is mapped inside the window and HeapAlloc goes
 * through the window chokepoint), the vtables are stable for the process
 * lifetime because applications cache and patch them, `native` is a HANDLE and
 * never a host pointer (invariant 4), refcounting is entirely guest-side with
 * exactly one native release at zero, and identity is answered LOCALLY -- the
 * ~45 `**` getters that just hand back an object the shim already knows.
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

#define CINTERFACE
#define COBJMACROS
/* One translation unit defines the D3D9 IIDs, so the module does not need
 * dxguid and its import set stays KERNEL32/USER32/GDI32/api-ms-win-crt-*. */
#define INITGUID

#include <stdlib.h>
#include <string.h>

#include "d3d9shim_object.h"

/* ------------------------------------------------------------------------
 * per-kind tables
 * ------------------------------------------------------------------------ */

static const struct {
    const void *const *vtbl;
    unsigned int       release_op;
    uint32_t           type_value;   /* D3DRESOURCETYPE, 0 = not a resource */
} d3d9shim_kinds[D3D9SHIM_KIND_COUNT] = {
    /* D3D9          */ { d3d9shim_vtbl_D3D9Ex,              D3D9OP_D3D9Ex_Release,              0 },
    /* DEVICE        */ { d3d9shim_vtbl_Device9Ex,           D3D9OP_Device9Ex_Release,           0 },
    /* SWAPCHAIN     */ { d3d9shim_vtbl_SwapChain9Ex,        D3D9OP_SwapChain9Ex_Release,        0 },
    /* SURFACE       */ { d3d9shim_vtbl_Surface9,            D3D9OP_Surface9_Release,            D3DRTYPE_SURFACE },
    /* TEXTURE       */ { d3d9shim_vtbl_Texture9,            D3D9OP_Texture9_Release,            D3DRTYPE_TEXTURE },
    /* CUBETEXTURE   */ { d3d9shim_vtbl_CubeTexture9,        D3D9OP_CubeTexture9_Release,        D3DRTYPE_CUBETEXTURE },
    /* VOLUMETEXTURE */ { d3d9shim_vtbl_VolumeTexture9,      D3D9OP_VolumeTexture9_Release,      D3DRTYPE_VOLUMETEXTURE },
    /* VOLUME        */ { d3d9shim_vtbl_Volume9,             D3D9OP_Volume9_Release,             D3DRTYPE_VOLUME },
    /* VERTEXBUFFER  */ { d3d9shim_vtbl_VertexBuffer9,       D3D9OP_VertexBuffer9_Release,       D3DRTYPE_VERTEXBUFFER },
    /* INDEXBUFFER   */ { d3d9shim_vtbl_IndexBuffer9,        D3D9OP_IndexBuffer9_Release,        D3DRTYPE_INDEXBUFFER },
    /* VERTEXDECL    */ { d3d9shim_vtbl_VertexDeclaration9,  D3D9OP_VertexDeclaration9_Release,  0 },
    /* VERTEXSHADER  */ { d3d9shim_vtbl_VertexShader9,       D3D9OP_VertexShader9_Release,       0 },
    /* PIXELSHADER   */ { d3d9shim_vtbl_PixelShader9,        D3D9OP_PixelShader9_Release,        0 },
    /* STATEBLOCK    */ { d3d9shim_vtbl_StateBlock9,         D3D9OP_StateBlock9_Release,         0 },
    /* QUERY         */ { d3d9shim_vtbl_Query9,              D3D9OP_Query9_Release,              0 },
};

const void *
d3d9shim_kind_vtbl(unsigned int kind)
{
    if (kind >= D3D9SHIM_KIND_COUNT)
        return NULL;
    return (const void *)d3d9shim_kinds[kind].vtbl;
}

/* Every generated *_Release parameter block has this shape (self, ret, pad),
 * which is what lets one hand-written release path serve all 15 kinds. */
struct d3d9shim_release_block {
    uint64_t self;
    uint32_t ret;
    uint32_t _pad0;
};
_Static_assert(sizeof(struct d3d9shim_release_block)
               == sizeof(struct d3d9_Texture9_Release_params),
               "release block shape");

/* ------------------------------------------------------------------------
 * the native-handle -> wrapper table, and the lock that guards it
 *
 * The table is process-wide, so it is guarded whether or not any device was
 * created D3DCREATE_MULTITHREADED: d3d9shim_lock() is the API-level lock the
 * application asked for, this one is an implementation detail.
 * ------------------------------------------------------------------------ */

#define D3D9SHIM_HASH_BUCKETS 257u

static struct d3d9shim_impl *d3d9shim_hash[D3D9SHIM_HASH_BUCKETS];
static CRITICAL_SECTION      d3d9shim_table_cs;
static LONG                  d3d9shim_table_ready;

static void
table_init(void)
{
    if (InterlockedCompareExchange(&d3d9shim_table_ready, 1, 0) == 0)
        InitializeCriticalSection(&d3d9shim_table_cs);
}

static void
table_lock(void)
{
    table_init();
    EnterCriticalSection(&d3d9shim_table_cs);
}

static void
table_unlock(void)
{
    LeaveCriticalSection(&d3d9shim_table_cs);
}

static unsigned int
hash_of(uint64_t native)
{
    uint64_t h = native;

    h ^= h >> 32;
    h *= 0x9e3779b1u;
    h ^= h >> 16;
    return (unsigned int)(h % D3D9SHIM_HASH_BUCKETS);
}

struct d3d9shim_impl *
d3d9shim_obj_lookup(uint64_t native)
{
    struct d3d9shim_impl *it;

    if (!native)
        return NULL;
    for (it = d3d9shim_hash[hash_of(native)]; it; it = it->hash_next)
        if (it->o.hdr.native == native)
            return it;
    return NULL;
}

static void
table_insert(struct d3d9shim_impl *impl)
{
    unsigned int b = hash_of(impl->o.hdr.native);

    impl->hash_next = d3d9shim_hash[b];
    d3d9shim_hash[b] = impl;
}

static void
table_remove(struct d3d9shim_impl *impl)
{
    unsigned int b;
    struct d3d9shim_impl **pp;

    if (!impl->o.hdr.native)
        return;
    b = hash_of(impl->o.hdr.native);
    for (pp = &d3d9shim_hash[b]; *pp; pp = &(*pp)->hash_next) {
        if (*pp == impl) {
            *pp = impl->hash_next;
            impl->hash_next = NULL;
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * allocation
 * ------------------------------------------------------------------------ */

static struct d3d9shim_impl *
obj_alloc_nolock(unsigned int kind, uint64_t native,
                 struct d3d9shim_object *parent, uint32_t flags)
{
    struct d3d9shim_impl *impl;

    if (kind >= D3D9SHIM_KIND_COUNT)
        return NULL;
    /* HeapAlloc, so the object and its vtable pointer are guest memory that
     * went through the window chokepoint (8.2(b)). */
    impl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*impl));
    if (!impl)
        return NULL;

    impl->o.hdr.vtbl = (const void *)d3d9shim_kinds[kind].vtbl;
    impl->o.hdr.refcount = 1;
    impl->o.hdr.priv_refcount = 0;
    impl->o.hdr.kind = kind;
    impl->o.hdr.type_value = d3d9shim_kinds[kind].type_value;
    impl->o.hdr.native = native;
    impl->o.hdr.parent = parent;
    impl->flags = flags;

    if (kind == D3D9SHIM_KIND_DEVICE) {
        impl->extra = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*impl->extra));
        if (!impl->extra) {
            HeapFree(GetProcessHeap(), 0, impl);
            return NULL;
        }
    }

    if (native)
        table_insert(impl);
    return impl;
}

/* The locking entry point, for the hand-written bodies outside this file. */
struct d3d9shim_impl *
d3d9shim_obj_alloc(unsigned int kind, uint64_t native,
                   struct d3d9shim_object *parent, uint32_t flags)
{
    struct d3d9shim_impl *impl;

    table_lock();
    impl = obj_alloc_nolock(kind, native, parent, flags);
    table_unlock();
    return impl;
}

/* The per-kind `device` field is at a different offset in every struct, so
 * reach it through the one thing they share: the header's parent chain. */
static struct d3d9shim_object *
device_field(struct d3d9shim_object *obj)
{
    switch (obj->kind) {
    case D3D9SHIM_KIND_SWAPCHAIN:     return D3D9SHIM_IMPL(obj)->o.swapchain.device;
    case D3D9SHIM_KIND_SURFACE:       return D3D9SHIM_IMPL(obj)->o.surface.device;
    case D3D9SHIM_KIND_TEXTURE:       return D3D9SHIM_IMPL(obj)->o.texture.device;
    case D3D9SHIM_KIND_CUBETEXTURE:   return D3D9SHIM_IMPL(obj)->o.cubetexture.device;
    case D3D9SHIM_KIND_VOLUMETEXTURE: return D3D9SHIM_IMPL(obj)->o.volumetexture.device;
    case D3D9SHIM_KIND_VOLUME:        return D3D9SHIM_IMPL(obj)->o.volume.device;
    case D3D9SHIM_KIND_VERTEXBUFFER:  return D3D9SHIM_IMPL(obj)->o.vertexbuffer.device;
    case D3D9SHIM_KIND_INDEXBUFFER:   return D3D9SHIM_IMPL(obj)->o.indexbuffer.device;
    case D3D9SHIM_KIND_VERTEXDECL:    return D3D9SHIM_IMPL(obj)->o.vertexdecl.device;
    case D3D9SHIM_KIND_VERTEXSHADER:  return D3D9SHIM_IMPL(obj)->o.vertexshader.device;
    case D3D9SHIM_KIND_PIXELSHADER:   return D3D9SHIM_IMPL(obj)->o.pixelshader.device;
    case D3D9SHIM_KIND_STATEBLOCK:    return D3D9SHIM_IMPL(obj)->o.stateblock.device;
    case D3D9SHIM_KIND_QUERY:         return D3D9SHIM_IMPL(obj)->o.query.device;
    case D3D9SHIM_KIND_DEVICE:        return obj;
    default:                          return NULL;
    }
}

static void
set_device_field(struct d3d9shim_object *obj, struct d3d9shim_object *dev)
{
    switch (obj->kind) {
    case D3D9SHIM_KIND_SWAPCHAIN:     D3D9SHIM_IMPL(obj)->o.swapchain.device = dev; break;
    case D3D9SHIM_KIND_SURFACE:       D3D9SHIM_IMPL(obj)->o.surface.device = dev; break;
    case D3D9SHIM_KIND_TEXTURE:       D3D9SHIM_IMPL(obj)->o.texture.device = dev; break;
    case D3D9SHIM_KIND_CUBETEXTURE:   D3D9SHIM_IMPL(obj)->o.cubetexture.device = dev; break;
    case D3D9SHIM_KIND_VOLUMETEXTURE: D3D9SHIM_IMPL(obj)->o.volumetexture.device = dev; break;
    case D3D9SHIM_KIND_VOLUME:        D3D9SHIM_IMPL(obj)->o.volume.device = dev; break;
    case D3D9SHIM_KIND_VERTEXBUFFER:  D3D9SHIM_IMPL(obj)->o.vertexbuffer.device = dev; break;
    case D3D9SHIM_KIND_INDEXBUFFER:   D3D9SHIM_IMPL(obj)->o.indexbuffer.device = dev; break;
    case D3D9SHIM_KIND_VERTEXDECL:    D3D9SHIM_IMPL(obj)->o.vertexdecl.device = dev; break;
    case D3D9SHIM_KIND_VERTEXSHADER:  D3D9SHIM_IMPL(obj)->o.vertexshader.device = dev; break;
    case D3D9SHIM_KIND_PIXELSHADER:   D3D9SHIM_IMPL(obj)->o.pixelshader.device = dev; break;
    case D3D9SHIM_KIND_STATEBLOCK:    D3D9SHIM_IMPL(obj)->o.stateblock.device = dev; break;
    case D3D9SHIM_KIND_QUERY:         D3D9SHIM_IMPL(obj)->o.query.device = dev; break;
    default: break;
    }
}

static void
set_usage(struct d3d9shim_object *obj, uint32_t usage)
{
    switch (obj->kind) {
    case D3D9SHIM_KIND_SURFACE:       D3D9SHIM_IMPL(obj)->o.surface.usage = usage; break;
    case D3D9SHIM_KIND_TEXTURE:       D3D9SHIM_IMPL(obj)->o.texture.usage = usage; break;
    case D3D9SHIM_KIND_CUBETEXTURE:   D3D9SHIM_IMPL(obj)->o.cubetexture.usage = usage; break;
    case D3D9SHIM_KIND_VOLUMETEXTURE: D3D9SHIM_IMPL(obj)->o.volumetexture.usage = usage; break;
    case D3D9SHIM_KIND_VERTEXBUFFER:  D3D9SHIM_IMPL(obj)->o.vertexbuffer.usage = usage; break;
    case D3D9SHIM_KIND_INDEXBUFFER:   D3D9SHIM_IMPL(obj)->o.indexbuffer.usage = usage; break;
    default: break;
    }
}

static uint32_t
get_usage(struct d3d9shim_object *obj)
{
    switch (obj->kind) {
    case D3D9SHIM_KIND_SURFACE:       return D3D9SHIM_IMPL(obj)->o.surface.usage;
    case D3D9SHIM_KIND_TEXTURE:       return D3D9SHIM_IMPL(obj)->o.texture.usage;
    case D3D9SHIM_KIND_CUBETEXTURE:   return D3D9SHIM_IMPL(obj)->o.cubetexture.usage;
    case D3D9SHIM_KIND_VOLUMETEXTURE: return D3D9SHIM_IMPL(obj)->o.volumetexture.usage;
    case D3D9SHIM_KIND_VERTEXBUFFER:  return D3D9SHIM_IMPL(obj)->o.vertexbuffer.usage;
    case D3D9SHIM_KIND_INDEXBUFFER:   return D3D9SHIM_IMPL(obj)->o.indexbuffer.usage;
    default:                          return 0;
    }
}

/* The three container kinds keep their children in a flat `sublevels` array;
 * only the indexing differs (level, face * levels + level, level). */
static struct d3d9shim_object ***
sublevels_of(struct d3d9shim_object *obj, uint32_t **count)
{
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(obj);

    switch (obj->kind) {
    case D3D9SHIM_KIND_TEXTURE:
        *count = &impl->o.texture.level_count;
        return &impl->o.texture.sublevels;
    case D3D9SHIM_KIND_CUBETEXTURE:
        *count = &impl->o.cubetexture.level_count;
        return &impl->o.cubetexture.sublevels;
    case D3D9SHIM_KIND_VOLUMETEXTURE:
        *count = &impl->o.volumetexture.level_count;
        return &impl->o.volumetexture.sublevels;
    default:
        *count = NULL;
        return NULL;
    }
}

/* ------------------------------------------------------------------------
 * the call note: what the generated thunk is doing right now
 *
 * d3d9shim_obj_from_native() is handed a kind and a handle and nothing else,
 * so on its own it cannot know which device owns the object it is about to
 * wrap, what D3DRESOURCETYPE a query is, or what usage a texture was created
 * with.  d3d9shim_native_call() records the opcode and parameter block of the
 * call in flight (per thread), and the creation path below reads them.  No
 * extra crossing, and nothing generated has to change.
 * ------------------------------------------------------------------------ */

static DWORD tls_note_op = TLS_OUT_OF_INDEXES;
static DWORD tls_note_block = TLS_OUT_OF_INDEXES;

static void
note_slots_init(void)
{
    if (tls_note_op == TLS_OUT_OF_INDEXES)
        tls_note_op = TlsAlloc();
    if (tls_note_block == TLS_OUT_OF_INDEXES)
        tls_note_block = TlsAlloc();
}

void
d3d9shim_obj_note_call(unsigned int op, const void *block)
{
    note_slots_init();
    if (tls_note_op == TLS_OUT_OF_INDEXES || tls_note_block == TLS_OUT_OF_INDEXES)
        return;
    TlsSetValue(tls_note_op, (LPVOID)(ULONG_PTR)(op + 1u));
    TlsSetValue(tls_note_block, (LPVOID)block);
}

static unsigned int
note_op(void)
{
    ULONG_PTR v;

    if (tls_note_op == TLS_OUT_OF_INDEXES)
        return D3D9SHIM_OP_COUNT;
    v = (ULONG_PTR)TlsGetValue(tls_note_op);
    return v ? (unsigned int)(v - 1u) : D3D9SHIM_OP_COUNT;
}

static const void *
note_block(void)
{
    if (tls_note_block == TLS_OUT_OF_INDEXES)
        return NULL;
    return TlsGetValue(tls_note_block);
}

/* Every parameter block starts with the receiver's native handle. */
static struct d3d9shim_object *
note_receiver(void)
{
    const uint64_t *self = note_block();
    struct d3d9shim_impl *impl;

    if (!self)
        return NULL;
    impl = d3d9shim_obj_lookup(*self);
    return impl ? &impl->o.hdr : NULL;
}

/* ------------------------------------------------------------------------
 * refcounting (8.2(b)); the sub-resource and implicit rules follow
 * d3d9_surface.cpp MTLD3D9Surface::AddRef / ::Release exactly
 * ------------------------------------------------------------------------ */

static void obj_destroy(struct d3d9shim_impl *impl);

static ULONG
obj_addref_locked(struct d3d9shim_object *obj)
{
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(obj);
    ULONG ref;

    if ((impl->flags & D3D9SHIM_F_SUBRESOURCE) && obj->parent)
        return obj_addref_locked(obj->parent);

    ref = (ULONG)(++obj->refcount);
    if (ref == 1 && (impl->flags & D3D9SHIM_F_DEVICE_CHILD)) {
        struct d3d9shim_object *dev = device_field(obj);

        if (dev && dev != obj)
            obj_addref_locked(dev);
    }
    return ref;
}

static ULONG obj_release_locked(struct d3d9shim_object *obj);

static void
obj_maybe_destroy(struct d3d9shim_impl *impl)
{
    if (impl->o.hdr.refcount == 0 && impl->o.hdr.priv_refcount == 0
        && !(impl->flags & D3D9SHIM_F_DEAD))
        obj_destroy(impl);
}

static ULONG
obj_release_locked(struct d3d9shim_object *obj)
{
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(obj);
    ULONG ref;

    if ((impl->flags & D3D9SHIM_F_SUBRESOURCE) && obj->parent)
        return obj_release_locked(obj->parent);

    /* D3D9 clamps Release-at-0: an implicit resource is handed out at public
     * refcount 0 and an over-release must not wrap the counter. */
    if (obj->refcount <= 0)
        return 0;

    ref = (ULONG)(--obj->refcount);
    if (ref == 0) {
        if (impl->flags & D3D9SHIM_F_DEVICE_CHILD) {
            struct d3d9shim_object *dev = device_field(obj);

            if (dev && dev != obj)
                obj_release_locked(dev);
        }
        obj_maybe_destroy(impl);
    }
    return ref;
}

void
d3d9shim_obj_addref_private(struct d3d9shim_object *obj)
{
    if (obj)
        obj->priv_refcount++;
}

void
d3d9shim_obj_release_private(struct d3d9shim_object *obj)
{
    if (!obj)
        return;
    if (obj->priv_refcount > 0)
        obj->priv_refcount--;
    obj_maybe_destroy(D3D9SHIM_IMPL(obj));
}

/* The one native release, at zero (8.2(b)).  Phase 1 issues it directly;
 * phase 2 appends the same record to the ring, which is what "every final
 * Release is deferred" means. */
static void
obj_native_release(struct d3d9shim_impl *impl)
{
    struct d3d9shim_release_block block;
    unsigned int op;

    if (!impl->o.hdr.native)
        return;
    op = d3d9shim_kinds[impl->o.hdr.kind].release_op;
    memset(&block, 0, sizeof(block));
    block.self = impl->o.hdr.native;
#if D3D9SHIM_PHASE >= 2
    {
        struct d3d9shim_object *dev = device_field(&impl->o.hdr);

        if (dev && dev->kind == D3D9SHIM_KIND_DEVICE
            && d3d9shim_ring_append((struct d3d9shim_device *)dev, op,
                                    &block, sizeof(block)))
            return;
    }
#endif
    if (d3d9shim_native_call(op, &block, sizeof(block)))
        d3d9shim_log_once("unix call failed: final Release");
}

static void
drop_child(struct d3d9shim_object **slot)
{
    struct d3d9shim_object *child = *slot;

    if (!child)
        return;
    *slot = NULL;
    d3d9shim_obj_release_private(child);
}

static void
obj_destroy(struct d3d9shim_impl *impl)
{
    struct d3d9shim_object *obj = &impl->o.hdr;
    struct d3d9shim_object ***sublevels;
    uint32_t *level_count;
    unsigned int i;

    impl->flags |= D3D9SHIM_F_DEAD;

    /* Children first: a container owns a private reference on each of them
     * and they hold one on it, so the cache has to be emptied before the
     * container's own release crosses. */
    sublevels = sublevels_of(obj, &level_count);
    if (sublevels && *sublevels) {
        for (i = 0; i < impl->sublevel_slots; i++)
            drop_child(&(*sublevels)[i]);
        HeapFree(GetProcessHeap(), 0, *sublevels);
        *sublevels = NULL;
        impl->sublevel_slots = 0;
    }
    if (obj->kind == D3D9SHIM_KIND_SWAPCHAIN) {
        for (i = 0; i < D3D9SHIM_MAX_BACK_BUFFERS; i++)
            drop_child(&impl->o.swapchain.back_buffers[i]);
    }
    if (obj->kind == D3D9SHIM_KIND_DEVICE) {
        for (i = 0; i < D3D9SHIM_MAX_SWAPCHAINS; i++)
            drop_child(&impl->o.device.swapchains[i]);
        for (i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; i++)
            drop_child(&impl->o.device.render_targets[i]);
        drop_child(&impl->o.device.depth_stencil);
        for (i = 0; i < D3D9SHIM_MAX_TEXTURE_SLOTS; i++)
            drop_child(&impl->o.device.textures[i]);
        for (i = 0; i < D3D9_MAX_VERTEX_STREAMS; i++)
            drop_child(&impl->o.device.stream_buffers[i]);
        drop_child(&impl->o.device.index_buffer);
        drop_child(&impl->o.device.vertex_declaration);
        drop_child(&impl->o.device.vertex_shader);
        drop_child(&impl->o.device.pixel_shader);
        d3d9shim_window_on_device_destroy(&impl->o.device);
        if (impl->o.device.d3d9) {
            struct d3d9shim_object *parent = impl->o.device.d3d9;

            impl->o.device.d3d9 = NULL;
            obj_release_locked(parent);
        }
    }

    /* A sub-resource does not hold a private reference on its container (the
     * container owns it); a standalone child of a container does. */
    if (obj->parent && !(impl->flags & D3D9SHIM_F_SUBRESOURCE)
        && obj->parent->kind != D3D9SHIM_KIND_DEVICE
        && obj->parent->kind != D3D9SHIM_KIND_D3D9)
        d3d9shim_obj_release_private(obj->parent);

    obj_native_release(impl);
    table_remove(impl);
    if (impl->extra)
        HeapFree(GetProcessHeap(), 0, impl->extra);
    HeapFree(GetProcessHeap(), 0, impl);
}

ULONG
d3d9shim_obj_addref(struct d3d9shim_object *obj)
{
    ULONG ref;

    if (!obj)
        return 0;
    table_lock();
    ref = obj_addref_locked(obj);
    table_unlock();
    return ref;
}

ULONG
d3d9shim_obj_release(struct d3d9shim_object *obj)
{
    ULONG ref;

    if (!obj)
        return 0;
    table_lock();
    ref = obj_release_locked(obj);
    table_unlock();
    return ref;
}

uint64_t
d3d9shim_obj_native(void *iface)
{
    struct d3d9shim_object *obj = iface;

    return obj ? obj->native : 0;
}

/* ------------------------------------------------------------------------
 * creation from a native handle
 * ------------------------------------------------------------------------ */

static struct d3d9shim_object *
create_wrapper(unsigned int kind, uint64_t handle)
{
    struct d3d9shim_object *receiver = note_receiver();
    struct d3d9shim_object *device = NULL;
    struct d3d9shim_impl *impl;
    const void *block = note_block();
    uint32_t flags = 0;
    uint32_t usage = 0;
    uint32_t type_value = 0;

    if (receiver)
        device = device_field(receiver);

    switch (note_op()) {
    case D3D9OP_Device9Ex_CreateTexture:
        usage = ((const struct d3d9_Device9Ex_CreateTexture_params *)block)->usage;
        break;
    case D3D9OP_Device9Ex_CreateCubeTexture:
        usage = ((const struct d3d9_Device9Ex_CreateCubeTexture_params *)block)->usage;
        break;
    case D3D9OP_Device9Ex_CreateVolumeTexture:
        usage = ((const struct d3d9_Device9Ex_CreateVolumeTexture_params *)block)->usage;
        break;
    case D3D9OP_Device9Ex_CreateVertexBuffer:
        usage = ((const struct d3d9_Device9Ex_CreateVertexBuffer_params *)block)->usage;
        break;
    case D3D9OP_Device9Ex_CreateIndexBuffer:
        usage = ((const struct d3d9_Device9Ex_CreateIndexBuffer_params *)block)->usage;
        break;
    case D3D9OP_Device9Ex_CreateRenderTarget:
    case D3D9OP_Device9Ex_CreateRenderTargetEx:
        usage = D3DUSAGE_RENDERTARGET;
        break;
    case D3D9OP_Device9Ex_CreateDepthStencilSurface:
    case D3D9OP_Device9Ex_CreateDepthStencilSurfaceEx:
        usage = D3DUSAGE_DEPTHSTENCIL;
        break;
    case D3D9OP_Device9Ex_CreateQuery:
        type_value = ((const struct d3d9_Device9Ex_CreateQuery_params *)block)->type;
        break;
    default:
        break;
    }

    /* Everything except the interface and the device itself pins the device
     * on its public 0 -> 1 edge, like every DXVK D3D9DeviceChild. */
    if (kind != D3D9SHIM_KIND_D3D9 && kind != D3D9SHIM_KIND_DEVICE)
        flags |= D3D9SHIM_F_DEVICE_CHILD;

    impl = obj_alloc_nolock(kind, handle, receiver, flags);
    if (!impl)
        return NULL;
    set_device_field(&impl->o.hdr, device);
    set_usage(&impl->o.hdr, usage);
    if (type_value)
        impl->o.hdr.type_value = type_value;
    return &impl->o.hdr;
}

struct d3d9shim_object *
d3d9shim_obj_from_native(unsigned int kind, uint64_t handle)
{
    struct d3d9shim_impl *impl;
    struct d3d9shim_object *obj;

    if (!handle)
        return NULL;
    table_lock();
    /* Identity is find-before-create: the same native handle must always map
     * to the same guest object, because applications compare the pointers
     * (that is why SetTexture identifies textures through a registry). */
    impl = d3d9shim_obj_lookup(handle);
    if (impl) {
        obj = &impl->o.hdr;
        obj_addref_locked(obj);
        table_unlock();
        return obj;
    }
    if (kind == D3D9SHIM_KIND_ANY) {
        /* Only a lookup is meaningful: a handle we have never seen is not an
         * object of ours (GetPrivateData's stored IUnknown). */
        table_unlock();
        return NULL;
    }
    obj = create_wrapper(kind, handle);
    table_unlock();
    return obj;
}

/* ------------------------------------------------------------------------
 * QueryInterface (8.2(b): answered locally, no call)
 * ------------------------------------------------------------------------ */

static int
kind_is(unsigned int kind, REFIID riid)
{
    switch (kind) {
    case D3D9SHIM_KIND_D3D9:
        return IsEqualGUID(riid, &IID_IDirect3D9) || IsEqualGUID(riid, &IID_IDirect3D9Ex);
    case D3D9SHIM_KIND_DEVICE:
        return IsEqualGUID(riid, &IID_IDirect3DDevice9) || IsEqualGUID(riid, &IID_IDirect3DDevice9Ex);
    case D3D9SHIM_KIND_SWAPCHAIN:
        return IsEqualGUID(riid, &IID_IDirect3DSwapChain9) || IsEqualGUID(riid, &IID_IDirect3DSwapChain9Ex);
    case D3D9SHIM_KIND_SURFACE:
        return IsEqualGUID(riid, &IID_IDirect3DSurface9) || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_TEXTURE:
        return IsEqualGUID(riid, &IID_IDirect3DTexture9) || IsEqualGUID(riid, &IID_IDirect3DBaseTexture9)
               || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_CUBETEXTURE:
        return IsEqualGUID(riid, &IID_IDirect3DCubeTexture9) || IsEqualGUID(riid, &IID_IDirect3DBaseTexture9)
               || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_VOLUMETEXTURE:
        return IsEqualGUID(riid, &IID_IDirect3DVolumeTexture9) || IsEqualGUID(riid, &IID_IDirect3DBaseTexture9)
               || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_VOLUME:
        /* IDirect3DVolume9 is NOT an IDirect3DResource9 (it has no GetType);
         * wined3d and DXVK both answer E_NOINTERFACE for that query. */
        return IsEqualGUID(riid, &IID_IDirect3DVolume9);
    case D3D9SHIM_KIND_VERTEXBUFFER:
        return IsEqualGUID(riid, &IID_IDirect3DVertexBuffer9) || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_INDEXBUFFER:
        return IsEqualGUID(riid, &IID_IDirect3DIndexBuffer9) || IsEqualGUID(riid, &IID_IDirect3DResource9);
    case D3D9SHIM_KIND_VERTEXDECL:
        return IsEqualGUID(riid, &IID_IDirect3DVertexDeclaration9);
    case D3D9SHIM_KIND_VERTEXSHADER:
        return IsEqualGUID(riid, &IID_IDirect3DVertexShader9);
    case D3D9SHIM_KIND_PIXELSHADER:
        return IsEqualGUID(riid, &IID_IDirect3DPixelShader9);
    case D3D9SHIM_KIND_STATEBLOCK:
        return IsEqualGUID(riid, &IID_IDirect3DStateBlock9);
    case D3D9SHIM_KIND_QUERY:
        return IsEqualGUID(riid, &IID_IDirect3DQuery9);
    default:
        return 0;
    }
}

HRESULT
d3d9shim_obj_query_interface(struct d3d9shim_object *obj, REFIID riid, void **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!obj || !riid)
        return D3DERR_INVALIDCALL;

    /* The Ex interfaces are extensions of the base ones and this shim has one
     * object per pair, so a plain-D3D9 object answering the Ex IID would lie
     * about the runtime the application asked for. */
    if (IsEqualGUID(riid, &IID_IDirect3D9Ex) || IsEqualGUID(riid, &IID_IDirect3DDevice9Ex)
        || IsEqualGUID(riid, &IID_IDirect3DSwapChain9Ex)) {
        if (!(D3D9SHIM_IMPL(obj)->flags & D3D9SHIM_F_IS_EX))
            return E_NOINTERFACE;
    }

    if (IsEqualGUID(riid, &IID_IUnknown) || kind_is(obj->kind, riid)) {
        d3d9shim_obj_addref(obj);
        *out = obj;
        return S_OK;
    }
    return E_NOINTERFACE;
}

/* ------------------------------------------------------------------------
 * small local predicates the generated bodies use
 * ------------------------------------------------------------------------ */

struct d3d9shim_device *
d3d9shim_device_of(struct d3d9shim_object *obj)
{
    struct d3d9shim_object *dev;

    if (!obj)
        return NULL;
    dev = device_field(obj);
    if (dev && dev->kind == D3D9SHIM_KIND_DEVICE)
        return (struct d3d9shim_device *)dev;
    return NULL;
}

int
d3d9shim_same_device(struct d3d9shim_object *self, struct d3d9shim_object *other)
{
    /* NULL is always allowed: unbinding is not a cross-device call. */
    if (!other)
        return 1;
    if (!self)
        return 0;
    return d3d9shim_device_of(self) == d3d9shim_device_of(other);
}

int
d3d9shim_has_usage(struct d3d9shim_object *obj, uint32_t usage)
{
    if (!obj)
        return 0;
    return (get_usage(obj) & usage) == usage;
}

/* d3d9_matrix.hpp transform_index(), verbatim in C.  The table is
 * 10 + 256 entries; a state outside the defined space maps at or beyond the
 * count through unsigned underflow, so the caller's bound rejects it. */
uint32_t
d3d9_transform_index(uint32_t state)
{
    if (state == D3DTS_VIEW)
        return 0;
    if (state == D3DTS_PROJECTION)
        return 1;
    if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
        return 2 + (state - D3DTS_TEXTURE0);
    return 10 + (state - D3DTS_WORLD);
}

/* d3d9_device.cpp:6541 texture_stage_to_slot(): 0..15 for the pixel samplers,
 * 16..19 for D3DVERTEXTEXTURESAMPLER0..3, UINT32_MAX for anything the runtime
 * ignores (D3DDMAPSAMPLER and out-of-range values). */
static uint32_t
texture_stage_to_slot(DWORD stage)
{
    if (stage < 16)
        return stage;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
        return 16 + (uint32_t)(stage - D3DVERTEXTEXTURESAMPLER0);
    return 0xffffffffu;
}

/* ------------------------------------------------------------------------
 * identity helpers -- borrowed references, and the lazy resolve behind them
 *
 * A child's native handle is only ever produced by the method that hands the
 * child out, and those methods are classified `local`.  The shim therefore
 * resolves each child ONCE through that method's own slot and caches the
 * wrapper; every later call is answered from the cache with no crossing,
 * which is what 8.2(b) asks for.  See the contract note in the step-3 report:
 * the generated unix entry for a `local` slot is currently a
 * STATUS_NOT_IMPLEMENTED stub, so the resolve fails until the generator emits
 * a real body for it -- the shim already asks the right question.
 * ------------------------------------------------------------------------ */

static struct d3d9shim_object *
adopt_child(struct d3d9shim_object *container, unsigned int kind,
            uint64_t handle, uint32_t extra_flags)
{
    struct d3d9shim_impl *impl;
    struct d3d9shim_object *child;

    if (!handle)
        return NULL;
    impl = d3d9shim_obj_lookup(handle);
    if (impl)
        child = &impl->o.hdr;
    else {
        impl = obj_alloc_nolock(kind, handle, container,
                                  D3D9SHIM_F_DEVICE_CHILD | extra_flags);
        if (!impl)
            return NULL;
        child = &impl->o.hdr;
        /* Handed out at public 0: the cache's private reference is what keeps
         * it alive, exactly like an implicit MTLD3D9Surface. */
        child->refcount = 0;
        set_device_field(child, device_field(container));
        set_usage(child, get_usage(container));
        /* GetContainer identity: the texture for a mip level or cube face,
         * the swapchain for a back buffer (its REFERENCE still pins the
         * device -- d3d9_surface.cpp says so in as many words). */
        if (child->kind == D3D9SHIM_KIND_SURFACE)
            impl->o.surface.container = container;
        else if (child->kind == D3D9SHIM_KIND_VOLUME)
            impl->o.volume.container = container;
    }
    d3d9shim_obj_addref_private(child);
    if (!(D3D9SHIM_IMPL(child)->flags & D3D9SHIM_F_SUBRESOURCE))
        d3d9shim_obj_addref_private(container);
    return child;
}

/* One synchronous crossing on the child's own slot. */
static uint64_t
resolve_child(unsigned int op, void *block, unsigned int size)
{
    const uint64_t *out;

    if (d3d9shim_native_call(op, block, size)) {
        d3d9shim_log_once("d3d9shim: child identity resolve failed "
                          "(the unix entry for this local slot is a stub)");
        return 0;
    }
    /* every one of these blocks is { self, <iface_out>, ... } */
    out = (const uint64_t *)block + 1;
    return *out;
}

static struct d3d9shim_object **
sublevel_slot(struct d3d9shim_object *tex, uint32_t index, uint32_t want_slots)
{
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(tex);
    uint32_t *count;
    struct d3d9shim_object ***table = sublevels_of(tex, &count);
    struct d3d9shim_object **fresh;

    if (!table)
        return NULL;
    if (index >= want_slots)
        return NULL;
    if (!*table || impl->sublevel_slots < want_slots) {
        fresh = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                          want_slots * sizeof(*fresh));
        if (!fresh)
            return NULL;
        if (*table) {
            memcpy(fresh, *table, impl->sublevel_slots * sizeof(*fresh));
            HeapFree(GetProcessHeap(), 0, *table);
        }
        *table = fresh;
        impl->sublevel_slots = want_slots;
    }
    return &(*table)[index];
}

struct d3d9shim_object *
d3d9shim_texture_sublevel(struct d3d9shim_object *tex, UINT level)
{
    struct d3d9shim_object **slot;
    uint64_t handle;

    if (!tex)
        return NULL;
    /* The child cache is shared state, so it is guarded by the table lock and
     * not by the device lock: an application without D3DCREATE_MULTITHREADED
     * takes no device lock at all.  The lock is recursive, so the resolve
     * below can re-enter it through d3d9shim_native_call(). */
    table_lock();
    slot = sublevel_slot(tex, level, level + 1u);
    if (!slot || *slot) {
        struct d3d9shim_object *out = slot ? *slot : NULL;

        table_unlock();
        return out;
    }

    if (tex->kind == D3D9SHIM_KIND_VOLUMETEXTURE) {
        struct d3d9_VolumeTexture9_GetVolumeLevel_params p;

        memset(&p, 0, sizeof(p));
        p.self = tex->native;
        p.Level = level;
        handle = resolve_child(D3D9OP_VolumeTexture9_GetVolumeLevel, &p, sizeof(p));
        *slot = adopt_child(tex, D3D9SHIM_KIND_VOLUME, handle,
                            D3D9SHIM_F_SUBRESOURCE);
    } else {
        struct d3d9_Texture9_GetSurfaceLevel_params p;

        memset(&p, 0, sizeof(p));
        p.self = tex->native;
        p.Level = level;
        handle = resolve_child(D3D9OP_Texture9_GetSurfaceLevel, &p, sizeof(p));
        *slot = adopt_child(tex, D3D9SHIM_KIND_SURFACE, handle,
                            D3D9SHIM_F_SUBRESOURCE);
    }
    table_unlock();
    return *slot;
}

struct d3d9shim_object *
d3d9shim_cube_surface(struct d3d9shim_cubetexture *tex, D3DCUBEMAP_FACES face,
                      UINT level)
{
    struct d3d9_CubeTexture9_GetCubeMapSurface_params p;
    struct d3d9shim_object **slot;
    uint32_t index;
    uint64_t handle;

    if (!tex || (unsigned int)face >= 6u)
        return NULL;
    /* 6 * level_count entries; the level count is not known until the first
     * resolve, so grow to whatever this call needs. */
    index = (uint32_t)face * 16u + level;
    table_lock();
    slot = sublevel_slot(&tex->hdr, index, index + 1u);
    if (!slot || *slot) {
        struct d3d9shim_object *out = slot ? *slot : NULL;

        table_unlock();
        return out;
    }

    memset(&p, 0, sizeof(p));
    p.self = tex->hdr.native;
    p.FaceType = (uint32_t)face;
    p.Level = level;
    handle = resolve_child(D3D9OP_CubeTexture9_GetCubeMapSurface, &p, sizeof(p));
    *slot = adopt_child(&tex->hdr, D3D9SHIM_KIND_SURFACE, handle,
                        D3D9SHIM_F_SUBRESOURCE);
    table_unlock();
    return *slot;
}

struct d3d9shim_object *
d3d9shim_swapchain_back_buffer(struct d3d9shim_swapchain *sc, UINT idx,
                               D3DBACKBUFFER_TYPE type)
{
    struct d3d9_SwapChain9Ex_GetBackBuffer_params p;
    uint64_t handle;

    if (!sc || idx >= D3D9SHIM_MAX_BACK_BUFFERS)
        return NULL;
    table_lock();
    if (sc->back_buffers[idx]) {
        struct d3d9shim_object *out = sc->back_buffers[idx];

        table_unlock();
        return out;
    }

    memset(&p, 0, sizeof(p));
    p.self = sc->hdr.native;
    p.backbuffer_idx = idx;
    p.backbuffer_type = (uint32_t)type;
    handle = resolve_child(D3D9OP_SwapChain9Ex_GetBackBuffer, &p, sizeof(p));
    sc->back_buffers[idx] = adopt_child(&sc->hdr, D3D9SHIM_KIND_SURFACE, handle,
                                        D3D9SHIM_F_IMPLICIT);
    if (sc->back_buffers[idx]) {
        set_usage(sc->back_buffers[idx], D3DUSAGE_RENDERTARGET);
        if (idx + 1u > sc->back_buffer_count)
            sc->back_buffer_count = idx + 1u;
    }
    table_unlock();
    return sc->back_buffers[idx];
}

/* Named by IDirect3DDevice9Ex::GetSwapChain, which d3d9_api.py classifies
 * `resolve`, so the generated body calls it by the contract name -- it is the
 * hook, not a file-static helper (the step-3 report's contract item (a)). */
struct d3d9shim_object *
d3d9shim_device_swapchain(struct d3d9shim_device *dev, UINT idx)
{
    struct d3d9_Device9Ex_GetSwapChain_params p;
    uint64_t handle;

    if (!dev || idx >= D3D9SHIM_MAX_SWAPCHAINS)
        return NULL;
    table_lock();
    if (dev->swapchains[idx]) {
        struct d3d9shim_object *out = dev->swapchains[idx];

        table_unlock();
        return out;
    }

    memset(&p, 0, sizeof(p));
    p.self = dev->hdr.native;
    p.swapchain_idx = idx;
    handle = resolve_child(D3D9OP_Device9Ex_GetSwapChain, &p, sizeof(p));
    dev->swapchains[idx] = adopt_child(&dev->hdr, D3D9SHIM_KIND_SWAPCHAIN,
                                       handle, D3D9SHIM_F_IMPLICIT);
    if (dev->swapchains[idx]) {
        D3D9SHIM_IMPL(dev->swapchains[idx])->flags |=
            (D3D9SHIM_IMPL(&dev->hdr)->flags & D3D9SHIM_F_IS_EX);
        if (idx + 1u > dev->swapchain_count)
            dev->swapchain_count = idx + 1u;
    }
    table_unlock();
    return dev->swapchains[idx];
}

struct d3d9shim_object *
d3d9shim_device_back_buffer(struct d3d9shim_device *dev, UINT swapchain_idx,
                            UINT idx, D3DBACKBUFFER_TYPE type)
{
    struct d3d9shim_object *sc = d3d9shim_device_swapchain(dev, swapchain_idx);

    if (!sc)
        return NULL;
    return d3d9shim_swapchain_back_buffer((struct d3d9shim_swapchain *)sc, idx, type);
}

/* GetRenderTarget / GetDepthStencilSurface.
 *
 * Both used to be plain `identity:` locals reading dev->render_targets[] and
 * dev->depth_stencil, which the SetRenderTarget / SetDepthStencilSurface
 * shadow fills.  That is only ever right AFTER the application has bound
 * something: at device creation the frontend binds the implicit back buffer
 * and the auto depth stencil itself (d3d9_device.cpp:652), and the shim has
 * no way to learn those two identities -- exactly the gap `resolve` exists
 * for.  A slot that is explicitly cleared (SetRenderTarget(idx, NULL) for
 * idx > 0) re-resolves and the native side answers D3DERR_NOTFOUND, which
 * is the same answer as before, at the cost of one crossing. */
struct d3d9shim_object *
d3d9shim_device_render_target(struct d3d9shim_device *dev, DWORD idx)
{
    struct d3d9_Device9Ex_GetRenderTarget_params p;
    uint64_t handle;

    if (!dev || idx >= D3D_MAX_SIMULTANEOUS_RENDERTARGETS)
        return NULL;
    table_lock();
    if (dev->render_targets[idx]) {
        struct d3d9shim_object *out = dev->render_targets[idx];

        table_unlock();
        return out;
    }

    memset(&p, 0, sizeof(p));
    p.self = dev->hdr.native;
    p.idx = idx;
    handle = resolve_child(D3D9OP_Device9Ex_GetRenderTarget, &p, sizeof(p));
    dev->render_targets[idx] = adopt_child(&dev->hdr, D3D9SHIM_KIND_SURFACE,
                                           handle, D3D9SHIM_F_IMPLICIT);
    if (dev->render_targets[idx])
        set_usage(dev->render_targets[idx], D3DUSAGE_RENDERTARGET);
    table_unlock();
    return dev->render_targets[idx];
}

struct d3d9shim_object *
d3d9shim_device_depth_stencil(struct d3d9shim_device *dev)
{
    struct d3d9_Device9Ex_GetDepthStencilSurface_params p;
    uint64_t handle;

    if (!dev)
        return NULL;
    table_lock();
    if (dev->depth_stencil) {
        struct d3d9shim_object *out = dev->depth_stencil;

        table_unlock();
        return out;
    }

    memset(&p, 0, sizeof(p));
    p.self = dev->hdr.native;
    handle = resolve_child(D3D9OP_Device9Ex_GetDepthStencilSurface, &p, sizeof(p));
    dev->depth_stencil = adopt_child(&dev->hdr, D3D9SHIM_KIND_SURFACE, handle,
                                     D3D9SHIM_F_IMPLICIT);
    if (dev->depth_stencil)
        set_usage(dev->depth_stencil, D3DUSAGE_DEPTHSTENCIL);
    table_unlock();
    return dev->depth_stencil;
}

struct d3d9shim_object *
d3d9shim_device_texture(struct d3d9shim_device *dev, DWORD stage)
{
    uint32_t slot = texture_stage_to_slot(stage);

    if (!dev || slot >= D3D9SHIM_MAX_TEXTURE_SLOTS)
        return NULL;
    return dev->textures[slot];
}

struct d3d9shim_object *
d3d9shim_device_stream_source(struct d3d9shim_device *dev, UINT stream_idx,
                              UINT *offset, UINT *stride)
{
    if (!dev || stream_idx >= D3D9_MAX_VERTEX_STREAMS)
        return NULL;
    if (offset)
        *offset = dev->stream_offsets[stream_idx];
    if (stride)
        *stride = dev->stream_strides[stream_idx];
    return dev->stream_buffers[stream_idx];
}

struct d3d9shim_object *
d3d9shim_container(struct d3d9shim_object *obj, REFIID riid)
{
    struct d3d9shim_object *container;

    if (!obj)
        return NULL;
    switch (obj->kind) {
    case D3D9SHIM_KIND_SURFACE: container = D3D9SHIM_IMPL(obj)->o.surface.container; break;
    case D3D9SHIM_KIND_VOLUME:  container = D3D9SHIM_IMPL(obj)->o.volume.container; break;
    default:                    return NULL;
    }
    /* GetContainer identity falls back to the device for a standalone
     * surface, and the answer must satisfy the requested interface. */
    if (!container)
        container = device_field(obj);
    if (!container)
        return NULL;
    if (!riid || IsEqualGUID(riid, &IID_IUnknown) || kind_is(container->kind, riid))
        return container;
    return NULL;
}

/* ------------------------------------------------------------------------
 * the shim's shadow of the deferred state
 * ------------------------------------------------------------------------ */

static void
bind_slot(struct d3d9shim_object **slot, uint64_t handle)
{
    struct d3d9shim_impl *impl = d3d9shim_obj_lookup(handle);
    struct d3d9shim_object *now = impl ? &impl->o.hdr : NULL;
    struct d3d9shim_object *was = *slot;

    if (was == now)
        return;
    /* The runtime holds a reference on a bound resource, so the shadow holds a
     * PRIVATE one: the application's own public count keeps reading back the
     * value it expects while the wrapper survives an app Release. */
    if (now)
        d3d9shim_obj_addref_private(now);
    *slot = now;
    if (was)
        d3d9shim_obj_release_private(was);
}

/* The ONE shadow hook, and the only caller is the generated deferred body --
 * on BOTH arms: the phase-2 arm calls it before the ring append, the phase-1
 * arm after a successful synchronous call.  It is therefore reached both with
 * the device lock held (phase 2) and with no lock at all (phase 1), so it
 * takes its own; the table's CRITICAL_SECTION is recursive, which is what
 * lets a caller that already holds it re-enter.
 *
 * It replaces d3d9shim_shadow_after_call(), which d3d9shim_native_call() used
 * to invoke for every crossing: the generated bodies now apply the shadow on
 * both arms themselves, so that hook applied it a SECOND time, on the same
 * block, for every deferred op -- and on the resolve crossings as well, where
 * it matched no case at all. */
void
d3d9shim_shadow_apply(struct d3d9shim_device *dev, unsigned int op,
                      const void *block)
{
    if (!dev || !block)
        return;

    table_lock();
    switch (op) {
    case D3D9OP_Device9Ex_SetTexture: {
        const struct d3d9_Device9Ex_SetTexture_params *p = block;
        uint32_t slot = texture_stage_to_slot(p->stage);

        if (slot < D3D9SHIM_MAX_TEXTURE_SLOTS)
            bind_slot(&dev->textures[slot], p->texture);
        break;
    }
    case D3D9OP_Device9Ex_SetRenderTarget: {
        const struct d3d9_Device9Ex_SetRenderTarget_params *p = block;

        if (p->idx < D3D_MAX_SIMULTANEOUS_RENDERTARGETS)
            bind_slot(&dev->render_targets[p->idx], p->surface);
        break;
    }
    case D3D9OP_Device9Ex_SetDepthStencilSurface: {
        const struct d3d9_Device9Ex_SetDepthStencilSurface_params *p = block;

        bind_slot(&dev->depth_stencil, p->depth_stencil);
        break;
    }
    case D3D9OP_Device9Ex_SetStreamSource: {
        const struct d3d9_Device9Ex_SetStreamSource_params *p = block;

        if (p->stream_idx < D3D9_MAX_VERTEX_STREAMS) {
            bind_slot(&dev->stream_buffers[p->stream_idx], p->buffer);
            dev->stream_offsets[p->stream_idx] = p->offset;
            dev->stream_strides[p->stream_idx] = p->stride;
        }
        break;
    }
    case D3D9OP_Device9Ex_SetIndices: {
        const struct d3d9_Device9Ex_SetIndices_params *p = block;

        bind_slot(&dev->index_buffer, p->buffer);
        break;
    }
    case D3D9OP_Device9Ex_SetVertexDeclaration: {
        const struct d3d9_Device9Ex_SetVertexDeclaration_params *p = block;

        bind_slot(&dev->vertex_declaration, p->declaration);
        break;
    }
    case D3D9OP_Device9Ex_SetFVF:
        /* An FVF replaces whatever declaration was bound; the runtime builds
         * one of its own, which the shim has no wrapper for. */
        bind_slot(&dev->vertex_declaration, 0);
        break;
    case D3D9OP_Device9Ex_SetVertexShader: {
        const struct d3d9_Device9Ex_SetVertexShader_params *p = block;

        bind_slot(&dev->vertex_shader, p->shader);
        break;
    }
    case D3D9OP_Device9Ex_SetPixelShader: {
        const struct d3d9_Device9Ex_SetPixelShader_params *p = block;

        bind_slot(&dev->pixel_shader, p->shader);
        break;
    }
    case D3D9OP_Device9Ex_BeginScene:
        dev->in_scene = 1;
        break;
    case D3D9OP_Device9Ex_EndScene:
        dev->in_scene = 0;
        break;

    /* priority / LOD shadows: SetPriority and SetLOD return the PREVIOUS
     * value, which the deferred arm can only answer from here. */
    case D3D9OP_Surface9_SetPriority: {
        const struct d3d9_Surface9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.surface.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_Texture9_SetPriority: {
        const struct d3d9_Texture9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.texture.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_CubeTexture9_SetPriority: {
        const struct d3d9_CubeTexture9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.cubetexture.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_VolumeTexture9_SetPriority: {
        const struct d3d9_VolumeTexture9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.volumetexture.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_VertexBuffer9_SetPriority: {
        const struct d3d9_VertexBuffer9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.vertexbuffer.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_IndexBuffer9_SetPriority: {
        const struct d3d9_IndexBuffer9_SetPriority_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.indexbuffer.priority = p->PriorityNew;
        break;
    }
    case D3D9OP_Texture9_SetLOD: {
        const struct d3d9_Texture9_SetLOD_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.texture.lod = p->LODNew;
        break;
    }
    case D3D9OP_CubeTexture9_SetLOD: {
        const struct d3d9_CubeTexture9_SetLOD_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.cubetexture.lod = p->LODNew;
        break;
    }
    case D3D9OP_VolumeTexture9_SetLOD: {
        const struct d3d9_VolumeTexture9_SetLOD_params *p = block;
        struct d3d9shim_impl *impl = d3d9shim_obj_lookup(p->self);

        if (impl)
            impl->o.volumetexture.lod = p->LODNew;
        break;
    }
    default:
        break;
    }
    table_unlock();
}

/* Reset destroys and recreates the implicit swapchain and its back buffers,
 * so every cached child identity is stale.  Dropping the cache's private
 * references is what lets the next GetSwapChain / GetBackBuffer resolve fresh
 * handles; an application that still holds one of the old objects keeps a
 * valid wrapper whose native handle no longer resolves, which is a validated
 * failure rather than a wild dereference (8.2(b), invariant 4). */
void
d3d9shim_device_invalidate_children(struct d3d9shim_device *dev)
{
    unsigned int i, j;

    if (!dev)
        return;
    table_lock();
    for (i = 0; i < D3D9SHIM_MAX_SWAPCHAINS; i++) {
        struct d3d9shim_object *sc = dev->swapchains[i];

        if (!sc)
            continue;
        for (j = 0; j < D3D9SHIM_MAX_BACK_BUFFERS; j++)
            drop_child(&D3D9SHIM_IMPL(sc)->o.swapchain.back_buffers[j]);
        D3D9SHIM_IMPL(sc)->o.swapchain.back_buffer_count = 0;
        drop_child(&dev->swapchains[i]);
    }
    dev->swapchain_count = 0;
    for (i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; i++)
        drop_child(&dev->render_targets[i]);
    drop_child(&dev->depth_stencil);
    table_unlock();
}

struct d3d9shim_device_extra *
d3d9shim_extra(struct d3d9shim_device *dev)
{
    return dev ? D3D9SHIM_IMPL(&dev->hdr)->extra : NULL;
}
