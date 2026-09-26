/*
 * d3d9shim_custom.c -- the slot bodies whose guest-side half is not mechanical
 *
 * d3d9_api.py marks eleven slots `custom` because the generated body cannot
 * express what they have to do on the guest side: setupFpu on the creating
 * thread, the focus-window hook, the fullscreen restyle, the cursor bitmap,
 * GetDC's D3DKMTCreateDCFromMemory, and the per-HWND client-size cache that
 * has to be current BEFORE the native side resolves a zero-extent
 * D3DPRESENT_PARAMETERS.  The two `shim:` cursor bodies are here too.
 *
 * Everything that is not window work still crosses through the generated
 * parameter block for the slot, so the wire format stays the generated one.
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

#include <string.h>

#include "d3d9shim_object.h"

/* ------------------------------------------------------------------------
 * CreateDevice / CreateDeviceEx
 * ------------------------------------------------------------------------ */

HRESULT
d3d9shim_custom_create_device(IDirect3D9Ex *iface, UINT adapter_idx,
                              D3DDEVTYPE device_type, HWND focus_window,
                              DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                              D3DDISPLAYMODEEX *mode, int is_ex,
                              void **device_out)
{
    struct d3d9shim_d3d9 *self = (struct d3d9shim_d3d9 *)iface;
    struct d3d9shim_impl *impl;
    struct d3d9shim_device *dev;
    struct d3d9shim_device_extra *extra;
    HWND device_window;
    uint64_t handle = 0;
    HRESULT hr;

    if (device_out)
        *device_out = NULL;
    if (!parameters || !device_out)
        return D3DERR_INVALIDCALL;

    /* MUST-NOT-FORGET (8.2(b)): match Windows D3D9 float behaviour on the
     * application's creating thread, before any device work.  This is the
     * whole reason CreateDevice is a custom body -- the native ARM64 frontend
     * has no x87 control word to set. */
    if (!(flags & D3DCREATE_FPU_PRESERVE))
        d3d9shim_setup_fpu();

    /* The native side fills a zero extent from the device window's client
     * rect, and it has no window of its own to ask -- so the cache has to be
     * current before the call, not after it (8.2(d)). */
    device_window = parameters->hDeviceWindow ? parameters->hDeviceWindow : focus_window;
    d3d9shim_window_push(device_window, !parameters->Windowed);
    if (focus_window != device_window)
        d3d9shim_window_push(focus_window, 0);

    if (is_ex) {
        struct d3d9_D3D9Ex_CreateDeviceEx_params p;

        memset(&p, 0, sizeof(p));
        p.self = self->hdr.native;
        p.adapter_idx = adapter_idx;
        p.device_type = (uint32_t)device_type;
        p.focus_window = (uint64_t)(ULONG_PTR)focus_window;
        p.flags = flags;
        /* The i386 memory image of D3DPRESENT_PARAMETERS IS the mirror
         * (struct d3d9_D3DPRESENT_PARAMETERS32), so the application's own
         * struct crosses as a guest pointer and the unix entry expands it. */
        p.parameters = (uint32_t)(ULONG_PTR)parameters;
        p.mode = (uint32_t)(ULONG_PTR)mode;
        if (d3d9shim_native_call(D3D9OP_D3D9Ex_CreateDeviceEx, &p, sizeof(p))) {
            d3d9shim_log_once("unix call failed: IDirect3D9Ex::CreateDeviceEx");
            return E_FAIL;
        }
        hr = (HRESULT)p.ret;
        handle = p.device;
    } else {
        struct d3d9_D3D9Ex_CreateDevice_params p;

        memset(&p, 0, sizeof(p));
        p.self = self->hdr.native;
        p.adapter_idx = adapter_idx;
        p.device_type = (uint32_t)device_type;
        p.focus_window = (uint64_t)(ULONG_PTR)focus_window;
        p.flags = flags;
        p.parameters = (uint32_t)(ULONG_PTR)parameters;
        if (d3d9shim_native_call(D3D9OP_D3D9Ex_CreateDevice, &p, sizeof(p))) {
            d3d9shim_log_once("unix call failed: IDirect3D9Ex::CreateDevice");
            return E_FAIL;
        }
        hr = (HRESULT)p.ret;
        handle = p.device;
    }
    if (FAILED(hr) || !handle)
        return FAILED(hr) ? hr : E_FAIL;

    impl = d3d9shim_obj_alloc(D3D9SHIM_KIND_DEVICE, handle, &self->hdr,
                              is_ex ? D3D9SHIM_F_IS_EX : 0);
    if (!impl)
        return E_OUTOFMEMORY;
    dev = &impl->o.device;

    /* GetDirect3D hands the interface back, so the device holds a reference
     * on it; the release happens when the device is destroyed. */
    dev->d3d9 = &self->hdr;
    d3d9shim_obj_addref(&self->hdr);

    dev->behavior_flags = flags;
    dev->multithreaded = (flags & D3DCREATE_MULTITHREADED) != 0;
    dev->can_software_vp = (flags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING
                                     | D3DCREATE_MIXED_VERTEXPROCESSING)) != 0;
    dev->can_hardware_vp = !(flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING);
    /* d3d9_device.cpp:11310: a software or mixed VP device exposes the
     * extended 8192-register float file, independent of the runtime
     * SetSoftwareVertexProcessing toggle. */
    dev->vs_const_f_count = dev->can_software_vp ? 8192u : 256u;

    extra = d3d9shim_extra(dev);
    if (extra) {
        extra->is_ex = is_ex;
        extra->focus_window = focus_window;
        extra->device_window = device_window;
        extra->backbuffer_width = parameters->BackBufferWidth;
        extra->backbuffer_height = parameters->BackBufferHeight;
        extra->windowed = parameters->Windowed != 0;
    }

    /* The window transitions, in the order wined3d does them: acquire and
     * subclass the focus window, then restyle for fullscreen. */
    d3d9shim_window_hook_focus(dev, device_window);
    if (!parameters->Windowed)
        d3d9shim_window_enter_fullscreen(dev, device_window,
                                         parameters->BackBufferWidth,
                                         parameters->BackBufferHeight);
    else
        d3d9shim_window_push(device_window, 0);

    *device_out = dev;
    return hr;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_D3D9Ex_CreateDevice(IDirect3D9Ex *iface, UINT adapter_idx,
                                    D3DDEVTYPE device_type, HWND focus_window,
                                    DWORD flags,
                                    D3DPRESENT_PARAMETERS *parameters,
                                    struct IDirect3DDevice9 **device)
{
    return d3d9shim_custom_create_device(iface, adapter_idx, device_type,
                                         focus_window, flags, parameters, NULL,
                                         0, (void **)device);
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_D3D9Ex_CreateDeviceEx(IDirect3D9Ex *iface, UINT adapter_idx,
                                      D3DDEVTYPE device_type, HWND focus_window,
                                      DWORD flags,
                                      D3DPRESENT_PARAMETERS *parameters,
                                      D3DDISPLAYMODEEX *mode,
                                      struct IDirect3DDevice9Ex **device)
{
    return d3d9shim_custom_create_device(iface, adapter_idx, device_type,
                                         focus_window, flags, parameters, mode,
                                         1, (void **)device);
}

/* ------------------------------------------------------------------------
 * Reset / ResetEx
 * ------------------------------------------------------------------------ */

static HRESULT
custom_reset(IDirect3DDevice9Ex *iface, D3DPRESENT_PARAMETERS *parameters,
             D3DDISPLAYMODEEX *mode, int is_ex)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND device_window;
    HRESULT hr;

    if (!parameters)
        return D3DERR_INVALIDCALL;

    d3d9shim_lock(dev);
    hr = d3d9shim_flush(dev);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }

    device_window = parameters->hDeviceWindow;
    if (!device_window && extra)
        device_window = extra->focus_window;
    /* Same ordering rule as CreateDevice: a zero extent is filled from the
     * client rect, so push the cache first. */
    d3d9shim_window_push(device_window, !parameters->Windowed);

    if (is_ex) {
        struct d3d9_Device9Ex_ResetEx_params p;

        memset(&p, 0, sizeof(p));
        p.self = dev->hdr.native;
        p.parameters = (uint32_t)(ULONG_PTR)parameters;
        p.mode = (uint32_t)(ULONG_PTR)mode;
        if (d3d9shim_native_call(D3D9OP_Device9Ex_ResetEx, &p, sizeof(p))) {
            d3d9shim_log_once("unix call failed: IDirect3DDevice9Ex::ResetEx");
            d3d9shim_unlock(dev);
            return E_FAIL;
        }
        hr = (HRESULT)p.ret;
    } else {
        struct d3d9_Device9Ex_Reset_params p;

        memset(&p, 0, sizeof(p));
        p.self = dev->hdr.native;
        p.parameters = (uint32_t)(ULONG_PTR)parameters;
        if (d3d9shim_native_call(D3D9OP_Device9Ex_Reset, &p, sizeof(p))) {
            d3d9shim_log_once("unix call failed: IDirect3DDevice9Ex::Reset");
            d3d9shim_unlock(dev);
            return E_FAIL;
        }
        hr = (HRESULT)p.ret;
    }

    if (SUCCEEDED(hr)) {
        /* A Reset destroys and recreates the implicit chain and its back
         * buffers, so every cached child identity is stale: drop them and let
         * the next GetBackBuffer / GetSwapChain resolve fresh handles. */
        d3d9shim_device_invalidate_children(dev);
        d3d9shim_window_on_device_reset(dev, parameters);
    }
    d3d9shim_unlock(dev);
    return hr;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_Reset(IDirect3DDevice9Ex *iface,
                                D3DPRESENT_PARAMETERS *parameters)
{
    return custom_reset(iface, parameters, NULL, 0);
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_ResetEx(IDirect3DDevice9Ex *iface,
                                  D3DPRESENT_PARAMETERS *parameters,
                                  D3DDISPLAYMODEEX *mode)
{
    return custom_reset(iface, parameters, mode, 1);
}

/* ------------------------------------------------------------------------
 * Present / PresentEx / SwapChain::Present
 *
 * Custom only because of the window-size push: the layer has to follow a
 * window the user resized, and GetClientRect is guest-side work
 * (d3d9_swapchain.cpp:897 makes the same point about it being cheap).  The
 * push is deduplicated, so a steady-state frame adds no crossing at all.
 * ------------------------------------------------------------------------ */

/* IDirect3DSwapChain9Ex::Present has the same block shape as
 * IDirect3DDevice9Ex::PresentEx, which is what lets one body serve both. */
_Static_assert(sizeof(struct d3d9_SwapChain9Ex_Present_params)
               == sizeof(struct d3d9_Device9Ex_PresentEx_params),
               "SwapChain9Ex::Present block shape");
_Static_assert(offsetof(struct d3d9_SwapChain9Ex_Present_params, flags)
               == offsetof(struct d3d9_Device9Ex_PresentEx_params, flags),
               "SwapChain9Ex::Present flags offset");
_Static_assert(offsetof(struct d3d9_SwapChain9Ex_Present_params, ret)
               == offsetof(struct d3d9_Device9Ex_PresentEx_params, ret),
               "SwapChain9Ex::Present ret offset");

static HRESULT
custom_present(struct d3d9shim_device *dev, uint64_t self, unsigned int op,
               const RECT *src_rect, const RECT *dst_rect,
               HWND dst_window_override, const RGNDATA *dirty_region,
               DWORD flags, int has_flags)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    union {
        struct d3d9_Device9Ex_Present_params    present;
        struct d3d9_Device9Ex_PresentEx_params  present_ex;
    } p;
    unsigned int size;
    HWND window = dst_window_override;
    HRESULT hr;

    if (!window && extra)
        window = extra->device_window;
    d3d9shim_window_push(window, extra && !extra->windowed);

    d3d9shim_lock(dev);
    hr = d3d9shim_flush(dev);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }
    memset(&p, 0, sizeof(p));
    if (has_flags) {
        p.present_ex.self = self;
        p.present_ex.dst_window_override = (uint64_t)(ULONG_PTR)dst_window_override;
        p.present_ex.src_rect = (uint32_t)(ULONG_PTR)src_rect;
        p.present_ex.dst_rect = (uint32_t)(ULONG_PTR)dst_rect;
        p.present_ex.dirty_region = (uint32_t)(ULONG_PTR)dirty_region;
        p.present_ex.flags = flags;
        size = (unsigned int)sizeof(p.present_ex);
    } else {
        p.present.self = self;
        p.present.dst_window_override = (uint64_t)(ULONG_PTR)dst_window_override;
        p.present.src_rect = (uint32_t)(ULONG_PTR)src_rect;
        p.present.dst_rect = (uint32_t)(ULONG_PTR)dst_rect;
        p.present.dirty_region = (uint32_t)(ULONG_PTR)dirty_region;
        size = (unsigned int)sizeof(p.present);
    }

    if (d3d9shim_native_call(op, &p, size)) {
        d3d9shim_log_once("unix call failed: Present");
        d3d9shim_unlock(dev);
        return E_FAIL;
    }
    hr = has_flags ? (HRESULT)p.present_ex.ret : (HRESULT)p.present.ret;
    d3d9shim_unlock(dev);
    return hr;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_Present(IDirect3DDevice9Ex *iface, const RECT *src_rect,
                                  const RECT *dst_rect, HWND dst_window_override,
                                  const RGNDATA *dirty_region)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;

    return custom_present(dev, dev->hdr.native, D3D9OP_Device9Ex_Present,
                          src_rect, dst_rect, dst_window_override, dirty_region,
                          0, 0);
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_PresentEx(IDirect3DDevice9Ex *iface, const RECT *src_rect,
                                    const RECT *dst_rect, HWND dst_window_override,
                                    const RGNDATA *dirty_region, DWORD flags)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;

    return custom_present(dev, dev->hdr.native, D3D9OP_Device9Ex_PresentEx,
                          src_rect, dst_rect, dst_window_override, dirty_region,
                          flags, 1);
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_SwapChain9Ex_Present(IDirect3DSwapChain9Ex *iface,
                                     const RECT *src_rect, const RECT *dst_rect,
                                     HWND dst_window_override,
                                     const RGNDATA *dirty_region, DWORD flags)
{
    struct d3d9shim_swapchain *sc = (struct d3d9shim_swapchain *)iface;
    struct d3d9shim_device *dev = d3d9shim_device_of(&sc->hdr);

    /* The block shapes are identical apart from the opcode, so the same body
     * serves; the receiver is the swapchain. */
    return custom_present(dev, sc->hdr.native, D3D9OP_SwapChain9Ex_Present,
                          src_rect, dst_rect, dst_window_override, dirty_region,
                          flags, 1);
}

/* ------------------------------------------------------------------------
 * SetCursorProperties, SetDialogBoxMode
 * ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_SetCursorProperties(IDirect3DDevice9Ex *iface,
                                              UINT hotspot_x, UINT hotspot_y,
                                              IDirect3DSurface9 *bitmap)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;
    struct d3d9_Device9Ex_SetCursorProperties_params p;
    HRESULT hr;

    if (!bitmap)
        return D3DERR_INVALIDCALL;

    d3d9shim_lock(dev);
    hr = d3d9shim_flush(dev);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }
    /* The reject matrix (format / power-of-two / hotspot / display mode,
     * d3d9_cursor_validation.hpp) stays on the native side, so there is one
     * copy of it; the shim only realises the Win32 cursor afterwards. */
    memset(&p, 0, sizeof(p));
    p.self = dev->hdr.native;
    p.hotspot_x = hotspot_x;
    p.hotspot_y = hotspot_y;
    p.bitmap = d3d9shim_obj_native(bitmap);
    if (d3d9shim_native_call(D3D9OP_Device9Ex_SetCursorProperties, &p, sizeof(p))) {
        d3d9shim_log_once("unix call failed: SetCursorProperties");
        d3d9shim_unlock(dev);
        return E_FAIL;
    }
    hr = (HRESULT)p.ret;
    if (SUCCEEDED(hr))
        hr = d3d9shim_window_set_cursor(dev, hotspot_x, hotspot_y, bitmap);
    d3d9shim_unlock(dev);
    return hr;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Device9Ex_SetDialogBoxMode(IDirect3DDevice9Ex *iface, WINBOOL enable)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    struct d3d9_Device9Ex_SetDialogBoxMode_params p;
    HRESULT hr;

    d3d9shim_lock(dev);
    /* Dialog-box mode means GDI draws over the swapchain, which a fullscreen
     * device does not allow; the rest of the gate (lockable backbuffer, no
     * multisampling) is the native side's. */
    if (enable && extra && !extra->windowed) {
        d3d9shim_unlock(dev);
        return D3DERR_INVALIDCALL;
    }
    hr = d3d9shim_flush(dev);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }
    memset(&p, 0, sizeof(p));
    p.self = dev->hdr.native;
    p.enable = (uint32_t)enable;
    if (d3d9shim_native_call(D3D9OP_Device9Ex_SetDialogBoxMode, &p, sizeof(p))) {
        d3d9shim_log_once("unix call failed: SetDialogBoxMode");
        d3d9shim_unlock(dev);
        return E_FAIL;
    }
    hr = (HRESULT)p.ret;
    d3d9shim_unlock(dev);
    return hr;
}

/* ------------------------------------------------------------------------
 * Surface9::GetDC / ReleaseDC
 *
 * Moved from d3d9_surface.cpp: the DC is backed by a bitmap created directly
 * over the surface's locked CPU bytes (D3DKMTCreateDCFromMemory), the wined3d
 * and DXVK shape, so GDI paints into the exact memory LockRect exposes and
 * UnlockRect uploads.  Those bytes are in the guest arena, which is the whole
 * reason this cannot move to the native side.  The toolchain ships no DDK
 * header for the two entry points, so the stable kernel-thunk ABI is declared
 * here and both are resolved from gdi32 at runtime.
 * ------------------------------------------------------------------------ */

struct D3DKMT_CREATEDCFROMMEMORY {
    void *          pMemory;
    D3DFORMAT       Format;
    UINT            Width;
    UINT            Height;
    UINT            Pitch;
    HDC             hDeviceDc;
    PALETTEENTRY *  pColorTable;
    HDC             hDc;
    HANDLE          hBitmap;
};

struct D3DKMT_DESTROYDCFROMMEMORY {
    HDC     hDc;
    HANDLE  hBitmap;
};

typedef LONG (WINAPI *d3dkmt_create_dc_fn)(struct D3DKMT_CREATEDCFROMMEMORY *);
typedef LONG (WINAPI *d3dkmt_destroy_dc_fn)(const struct D3DKMT_DESTROYDCFROMMEMORY *);

static LONG
d3dkmt_create_dc_from_memory(struct D3DKMT_CREATEDCFROMMEMORY *arg)
{
    static d3dkmt_create_dc_fn fn;
    static int resolved;

    if (!resolved) {
        fn = (d3dkmt_create_dc_fn)(void *)GetProcAddress(GetModuleHandleA("gdi32.dll"),
                                                         "D3DKMTCreateDCFromMemory");
        resolved = 1;
    }
    return fn ? fn(arg) : -1;
}

static LONG
d3dkmt_destroy_dc_from_memory(const struct D3DKMT_DESTROYDCFROMMEMORY *arg)
{
    static d3dkmt_destroy_dc_fn fn;
    static int resolved;

    if (!resolved) {
        fn = (d3dkmt_destroy_dc_fn)(void *)GetProcAddress(GetModuleHandleA("gdi32.dll"),
                                                          "D3DKMTDestroyDCFromMemory");
        resolved = 1;
    }
    return fn ? fn(arg) : -1;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Surface9_GetDC(IDirect3DSurface9 *iface, HDC *phdc)
{
    struct d3d9shim_surface *self = (struct d3d9shim_surface *)iface;
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(&self->hdr);
    struct d3d9shim_device *dev = d3d9shim_device_of(&self->hdr);
    struct D3DKMT_CREATEDCFROMMEMORY create;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    HRESULT hr;
    LONG status;

    /* Leave *phdc untouched on every failure path: D3D9 only writes it on
     * success, so a failed GetDC must not clobber the caller's HDC. */
    if (!phdc)
        return D3DERR_INVALIDCALL;
    d3d9shim_lock(dev);
    if (impl->gdi_dc) {           /* a DC is already open on this surface */
        d3d9shim_unlock(dev);
        return D3DERR_INVALIDCALL;
    }
    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DSurface9_GetDesc(iface, &desc);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }
    memset(&locked, 0, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(iface, &locked, NULL, 0);
    if (FAILED(hr)) {
        d3d9shim_unlock(dev);
        return hr;
    }

    memset(&create, 0, sizeof(create));
    create.pMemory = locked.pBits;
    create.Format = desc.Format;
    create.Width = desc.Width;
    create.Height = desc.Height;
    create.Pitch = (UINT)locked.Pitch;
    create.hDeviceDc = CreateCompatibleDC(NULL);
    create.pColorTable = NULL;
    /* The output bitmap/DC own the memory mapping; the device DC is only
     * needed for the call and is freed regardless of outcome. */
    status = d3dkmt_create_dc_from_memory(&create);
    if (create.hDeviceDc)
        DeleteDC(create.hDeviceDc);
    if (status != 0 || !create.hDc) {
        IDirect3DSurface9_UnlockRect(iface);
        d3d9shim_unlock(dev);
        return D3DERR_INVALIDCALL;
    }
    impl->gdi_dc = create.hDc;
    impl->gdi_bitmap = create.hBitmap;
    *phdc = create.hDc;
    d3d9shim_unlock(dev);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
d3d9shim_custom_Surface9_ReleaseDC(IDirect3DSurface9 *iface, HDC hdc)
{
    struct d3d9shim_surface *self = (struct d3d9shim_surface *)iface;
    struct d3d9shim_impl *impl = D3D9SHIM_IMPL(&self->hdr);
    struct d3d9shim_device *dev = d3d9shim_device_of(&self->hdr);
    struct D3DKMT_DESTROYDCFROMMEMORY destroy;
    HRESULT hr;

    d3d9shim_lock(dev);
    if (!impl->gdi_dc || hdc != impl->gdi_dc) {
        d3d9shim_unlock(dev);
        return D3DERR_INVALIDCALL;
    }
    memset(&destroy, 0, sizeof(destroy));
    destroy.hDc = impl->gdi_dc;
    destroy.hBitmap = impl->gdi_bitmap;
    d3dkmt_destroy_dc_from_memory(&destroy);
    impl->gdi_dc = NULL;
    impl->gdi_bitmap = NULL;
    /* UnlockRect uploads the GDI-modified bytes through the same path a
     * write-Lock uses. */
    hr = IDirect3DSurface9_UnlockRect(iface);
    d3d9shim_unlock(dev);
    return hr;
}

/* ------------------------------------------------------------------------
 * the two `shim:` cursor bodies (8.2(d)): user32 work that never crosses
 * ------------------------------------------------------------------------ */

void STDMETHODCALLTYPE
d3d9shim_shim_cursor_set_position(IDirect3DDevice9Ex *iface, int x, int y, DWORD flags)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;

    /* Flags is documented as a hint set (D3DCURSOR_IMMEDIATE_UPDATE) the
     * runtime is free to ignore, and wined3d does. */
    (void)flags;
    d3d9shim_lock(dev);
    d3d9shim_window_cursor_set_position(dev, x, y);
    d3d9shim_unlock(dev);
}

WINBOOL STDMETHODCALLTYPE
d3d9shim_shim_cursor_show(IDirect3DDevice9Ex *iface, WINBOOL show)
{
    struct d3d9shim_device *dev = (struct d3d9shim_device *)iface;
    WINBOOL previous;

    d3d9shim_lock(dev);
    previous = d3d9shim_window_cursor_show(dev, show);
    d3d9shim_unlock(dev);
    return previous;
}
