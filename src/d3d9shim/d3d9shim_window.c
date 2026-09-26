/*
 * d3d9shim_window.c -- all the user32/gdi32 work, moved into the shim
 *
 * WOW64_DESIGN.md 8.2(d): "All user32/gdi32 stays in the shim" -- the cursor
 * (d3d9_device.cpp:1744-1844), the fullscreen styles and positioning
 * (:1955-2040), the focus hook and focusWindowProc (:2042-2094, :2198-2276),
 * IsWindowVisible / ShowWindow, and GetClientRect, whose result is pushed to
 * the native side so wsi_window_madeira.cpp can answer a client-size query
 * without a window of its own.  None of it can run natively: the window
 * belongs to the guest process and its messages are pumped by a guest thread.
 *
 * The pieces of the wined3d focus machine that are NOT window work -- marking
 * the device lost, restoring a display mode, the occlusion latch -- stay on
 * the native side, which owns the device state; the shim tells it what the
 * window did through the window-state slot.
 *
 * PROVENANCE: the fullscreen restyle, the focus window proc and the cursor
 * realisation are moved from research/dxmt/src/d3d9/d3d9_device.cpp (DXMT,
 * LGPL-2.1-or-later, COPYING.LIB) and reshaped from C++ members into C
 * functions over struct d3d9shim_device_extra.  The moved blocks keep that
 * licence; the rest of this file is GPL-3.0-or-later.  See
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * Copyright 2023-2026 Feifan He for CodeWeavers (the moved blocks)
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
 * SPDX-License-Identifier: GPL-3.0-or-later AND LGPL-2.1-or-later
 */

#define CINTERFACE
#define COBJMACROS

#include <string.h>

#include "d3d9shim_object.h"

/* Window properties held on the focus window while the shim has it
 * subclassed: the application's original wndproc, and the device the
 * transitions belong to.  The forwarding proc reads both per message;
 * hook/unhook own them. */
static const WCHAR kFocusProcProp[]   = L"MadeiraD3D9ShimOrigProc";
static const WCHAR kFocusDeviceProp[] = L"MadeiraD3D9ShimDevice";

/* ml999: THE PROPERTIES ARE A WINESERVER ROUND TRIP, AND WE ALREADY KNOW THE
 * ANSWER.
 *
 * focusWindowProc opened EVERY message with two GetPropW calls, and on this
 * port a window property lives in the wineserver: [srv-stats] measured
 * `get_window_property: NtUserGetProp+0x7c' at 2088-2994 per 10 s -- the third
 * largest server request kind in the whole process -- at 42 us each, purely to
 * re-read two values this DLL set itself in hook_focus.
 *
 * So cache them here, in the process that owns them. The properties stay: they
 * are the cross-DLL contract (the dedup check in hook_focus reads one, and the
 * WM_NCDESTROY self-heal needs them if the app re-subclasses on top of us), and
 * they remain the fallback whenever the cache misses, so behaviour is identical
 * to before in every case -- the cache can only make it faster, never different.
 *
 * Eight slots: a process with more than eight simultaneously subclassed focus
 * windows falls back to the properties for the extras and is merely as slow as
 * it was. Publication is release/acquire on the HWND, with the payload written
 * before the key and read after it, so a reader on the window's own thread
 * never sees a slot whose proc/device have not landed yet. */
#define FOCUS_CACHE_MAX 8
static struct {
    HWND hwnd;                          /* NULL = free */
    WNDPROC orig;
    struct d3d9shim_device *device;
} g_focus_cache[FOCUS_CACHE_MAX];

static void
focus_cache_put(HWND hwnd, WNDPROC orig, struct d3d9shim_device *device)
{
    int i;
    for (i = 0; i < FOCUS_CACHE_MAX; i++) {
        HWND cur = __atomic_load_n(&g_focus_cache[i].hwnd, __ATOMIC_RELAXED);
        if (cur && cur != hwnd)
            continue;
        g_focus_cache[i].orig = orig;
        g_focus_cache[i].device = device;
        __atomic_store_n(&g_focus_cache[i].hwnd, hwnd, __ATOMIC_RELEASE);
        return;
    }
    /* Full: the properties still carry it. */
}

static void
focus_cache_drop(HWND hwnd)
{
    int i;
    for (i = 0; i < FOCUS_CACHE_MAX; i++)
        if (__atomic_load_n(&g_focus_cache[i].hwnd, __ATOMIC_RELAXED) == hwnd)
            __atomic_store_n(&g_focus_cache[i].hwnd, (HWND)NULL, __ATOMIC_RELEASE);
}

/* 1 on a hit. Only writes through the out pointers when it hits, so a miss
 * leaves the caller's GetPropW fallback in charge. */
static int
focus_cache_get(HWND hwnd, WNDPROC *orig, struct d3d9shim_device **device)
{
    int i;
    for (i = 0; i < FOCUS_CACHE_MAX; i++) {
        if (__atomic_load_n(&g_focus_cache[i].hwnd, __ATOMIC_ACQUIRE) != hwnd)
            continue;
        *orig = g_focus_cache[i].orig;
        *device = g_focus_cache[i].device;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * the per-HWND client-size cache the native wsi reads
 * ------------------------------------------------------------------------ */

/* Present pushes the client size every frame, and the size almost never
 * changes, so the last pushed value is remembered per window and an unchanged
 * push costs no crossing at all.  Eight windows is more than any D3D9 title
 * has device or focus windows; a ninth simply always pushes. */
#define D3D9SHIM_WINDOW_CACHE 8

static struct {
    HWND     hwnd;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} window_cache[D3D9SHIM_WINDOW_CACHE];
static LONG window_cache_next;

static int
window_state_changed(HWND hwnd, uint32_t width, uint32_t height, uint32_t flags)
{
    int i, slot;

    for (i = 0; i < D3D9SHIM_WINDOW_CACHE; i++) {
        if (window_cache[i].hwnd != hwnd)
            continue;
        if (flags & D3D9SHIM_WINDOW_GONE) {
            /* Forget the entry outright: the HWND value can be reused. */
            window_cache[i].hwnd = NULL;
            return 1;
        }
        if (window_cache[i].width == width && window_cache[i].height == height
            && window_cache[i].flags == flags)
            return 0;
        window_cache[i].width = width;
        window_cache[i].height = height;
        window_cache[i].flags = flags;
        return 1;
    }
    slot = (int)(InterlockedIncrement(&window_cache_next) - 1) % D3D9SHIM_WINDOW_CACHE;
    window_cache[slot].hwnd = hwnd;
    window_cache[slot].width = width;
    window_cache[slot].height = height;
    window_cache[slot].flags = flags;
    return 1;
}

static void
push_window_state(HWND hwnd, uint32_t width, uint32_t height, uint32_t flags)
{
    struct d3d9_window_state_params params;

    if (!hwnd)
        return;
    if (!window_state_changed(hwnd, width, height, flags))
        return;
    memset(&params, 0, sizeof(params));
    /* An HWND is a handle: zero-extended, never offset (invariant 4). */
    params.hwnd = (uint32_t)(ULONG_PTR)hwnd;
    params.width = width;
    params.height = height;
    params.flags = flags;
    if (d3d9shim_native_call(D3D9SHIM_OP_window_state, &params, sizeof(params)))
        d3d9shim_log_once("d3d9shim: the window-state slot is not bound yet");
}

void
d3d9shim_window_push(HWND hwnd, int fullscreen)
{
    RECT rc;
    uint32_t flags = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    if (!hwnd)
        return;
    if (!IsWindow(hwnd)) {
        push_window_state(hwnd, 0, 0, D3D9SHIM_WINDOW_GONE);
        return;
    }
    /* d3d9_interface.cpp:996 / wsi_window_headless.cpp:32: the client rect is
     * what a zero-extent D3DPRESENT_PARAMETERS is filled from, and it is the
     * one window fact the native side cannot observe for itself. */
    if (GetClientRect(hwnd, &rc)) {
        width = (uint32_t)(rc.right - rc.left);
        height = (uint32_t)(rc.bottom - rc.top);
    }
    if (IsWindowVisible(hwnd))
        flags |= D3D9SHIM_WINDOW_VISIBLE;
    if (GetForegroundWindow() == hwnd)
        flags |= D3D9SHIM_WINDOW_FOREGROUND;
    if (fullscreen)
        flags |= D3D9SHIM_WINDOW_FULLSCREEN;
    push_window_state(hwnd, width, height, flags);
}

void
d3d9shim_window_forget(HWND hwnd)
{
    push_window_state(hwnd, 0, 0, D3D9SHIM_WINDOW_GONE);
}

/* ------------------------------------------------------------------------
 * fullscreen styles and positioning (d3d9_device.cpp:1962-2040)
 * ------------------------------------------------------------------------ */

void
d3d9shim_window_enter_fullscreen(struct d3d9shim_device *dev, HWND window,
                                 UINT width, UINT height)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HMONITOR monitor;
    MONITORINFO mi;
    LONG x = 0, y = 0;

    if (!extra || !window)
        return;
    /* The app asked dxmt not to touch its window; leave it exactly as is. */
    if (dev->behavior_flags & D3DCREATE_NOWINDOWCHANGES)
        return;
    InterlockedExchange(&extra->focus_filtered, 1);

    /* Fullscreen rect: the window's monitor origin plus the backbuffer extent.
     * Single-monitor desktops sit at (0, 0); a read-only MonitorFromWindow
     * keeps multi-monitor correct without a display-mode switch. */
    monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
    }
    /* Remembered for the focus-gain reposition, which runs while the window is
     * still minimized and so cannot ask the window which output it is on. */
    extra->fullscreen_monitor = monitor;

    if (extra->fullscreen_window != window) {
        LONG style, ex_style;

        /* First entry (or a newly designated device window). Save the windowed
         * style/exstyle/rect, then restyle to a borderless popup: wined3d
         * fullscreen_style / fullscreen_exstyle. */
        extra->fullscreen_window = window;
        extra->saved_style = GetWindowLongW(window, GWL_STYLE);
        extra->saved_ex_style = GetWindowLongW(window, GWL_EXSTYLE);
        GetWindowRect(window, &extra->saved_rect);
        style = (extra->saved_style | WS_POPUP | WS_SYSMENU) & ~(WS_CAPTION | WS_THICKFRAME);
        ex_style = extra->saved_ex_style & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
        SetWindowLongW(window, GWL_STYLE, style);
        SetWindowLongW(window, GWL_EXSTYLE, ex_style);
    }
    /* wined3d uses HWND_TOPMOST + SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW. */
    SetWindowPos(window, HWND_TOPMOST, x, y, (int)width, (int)height,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    InterlockedExchange(&extra->focus_filtered, 0);
    d3d9shim_window_push(window, 1);
}

/* Port of wined3d_swapchain_state_restore_from_fullscreen.  Restores the saved
 * style (preserving the live WS_VISIBLE / WS_EX_TOPMOST bits, as wined3d does
 * so it never hides or un-tops a window the app is driving).  Non-Ex d3d9 is
 * style-only; Ex also restores the saved window rect. */
void
d3d9shim_window_leave_fullscreen(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND window;
    LONG live_style, live_ex_style, style, ex_style;

    if (!extra)
        return;
    window = extra->fullscreen_window;
    if (!window)
        return;
    extra->fullscreen_window = NULL;
    InterlockedExchange(&extra->focus_filtered, 1);

    live_style = GetWindowLongW(window, GWL_STYLE);
    live_ex_style = GetWindowLongW(window, GWL_EXSTYLE);
    style = (extra->saved_style & ~WS_VISIBLE) | (live_style & WS_VISIBLE);
    ex_style = (extra->saved_ex_style & ~WS_EX_TOPMOST) | (live_ex_style & WS_EX_TOPMOST);
    SetWindowLongW(window, GWL_STYLE, style);
    SetWindowLongW(window, GWL_EXSTYLE, ex_style);
    if (extra->is_ex)
        SetWindowPos(window, HWND_NOTOPMOST, extra->saved_rect.left, extra->saved_rect.top,
                     extra->saved_rect.right - extra->saved_rect.left,
                     extra->saved_rect.bottom - extra->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    else
        SetWindowPos(window, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);

    extra->saved_style = 0;
    extra->saved_ex_style = 0;
    memset(&extra->saved_rect, 0, sizeof(extra->saved_rect));
    extra->fullscreen_monitor = NULL;

    InterlockedExchange(&extra->focus_filtered, 0);
    d3d9shim_window_push(window, 0);
}

/* ------------------------------------------------------------------------
 * the focus window proc (d3d9_device.cpp:2042-2094, :2103-2196)
 * ------------------------------------------------------------------------ */

/* The window half of wined3d_swapchain_activate.  The two directions are
 * deliberately asymmetric, matching the reference: losing focus minimizes,
 * regaining it only repositions.  There is no un-minimize anywhere in wined3d.
 * The device-state half (Lost / NotReset, the occlusion latch, the display
 * mode) belongs to the native frontend, which learns what happened from the
 * window-state push at the end. */
static void
focus_activation(struct d3d9shim_device *dev, int activated)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND device_window;
    int may_touch_window;

    if (!extra)
        return;
    device_window = extra->device_window;
    /* Windowed devices never subclass the focus window; guard anyway, since a
     * Reset can flip a device windowed under the hook. */
    if (extra->windowed)
        return;
    /* D3DCREATE_NOWINDOWCHANGES suppresses only the window moves.  wined3d
     * consults it at exactly the two window-touching sites. */
    may_touch_window = !(dev->behavior_flags & D3DCREATE_NOWINDOWCHANGES);

    /* The window traffic below is ours, not the application's. */
    InterlockedExchange(&extra->focus_filtered, 1);

    if (!activated) {
        if (may_touch_window && device_window && IsWindowVisible(device_window)) {
            /* SW_MINIMIZE rather than native's SW_SHOWMINIMIZED, following
             * wined3d: under a window manager SW_SHOWMINIMIZED leaves the
             * device window active and breaks reactivation, and that is the
             * environment this runs in. */
            ShowWindow(device_window, SW_MINIMIZE);
        }
    } else if (may_touch_window && device_window) {
        /* Size from the backbuffer, origin from the monitor the device went
         * fullscreen on, and explicitly no activate and no Z-order change.
         * The monitor is the saved one because the window is normally still
         * minimized here, and a minimized window's rect answers for the
         * primary monitor rather than its own.  Some titles resume drawing
         * only once they see a WM_WINDOWPOSCHANGED on the device window,
         * which is the whole reason this runs even when the geometry is
         * unchanged. */
        MONITORINFO mi;
        HMONITOR monitor;
        LONG x = 0, y = 0;

        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        monitor = extra->fullscreen_monitor;
        if (!monitor)
            monitor = MonitorFromWindow(device_window, MONITOR_DEFAULTTOPRIMARY);
        if (monitor && GetMonitorInfoW(monitor, &mi)) {
            x = mi.rcMonitor.left;
            y = mi.rcMonitor.top;
        }
        SetWindowPos(device_window, NULL, x, y, (int)extra->backbuffer_width,
                     (int)extra->backbuffer_height, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    InterlockedExchange(&extra->focus_filtered, 0);
    /* Tell the native side what the window is now; that is what its own
     * device-state gate reads. */
    d3d9shim_window_push(device_window, !extra->windowed);
}

static LRESULT CALLBACK
focusWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig;
    struct d3d9shim_device *device;
    BOOL unicode;

    /* ml999: the cache, or the properties if it misses. See focus_cache_put. */
    if (!focus_cache_get(hwnd, &orig, &device)) {
        orig = (WNDPROC)GetPropW(hwnd, kFocusProcProp);
        device = GetPropW(hwnd, kFocusDeviceProp);
    }
    unicode = IsWindowUnicode(hwnd);

    /* The focus window is being torn down while still subclassed: drop our
     * proc so neither the property nor our function pointer outlives it. */
    if (message == WM_NCDESTROY && orig) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
        RemovePropW(hwnd, kFocusProcProp);
        RemovePropW(hwnd, kFocusDeviceProp);
        focus_cache_drop(hwnd);          /* ml999 */
        d3d9shim_window_forget(hwnd);
        /* The window is going away, so nothing below applies and the
         * application must still receive this. */
        return unicode ? CallWindowProcW(orig, hwnd, message, wparam, lparam)
                       : CallWindowProcA(orig, hwnd, message, wparam, lparam);
    }

    if (device) {
        struct d3d9shim_device_extra *extra = d3d9shim_extra(device);

        /* The filter is checked first so that the window traffic the shim
         * generates cannot re-enter this handler and drive a second
         * transition.  WM_DISPLAYCHANGE is exempt: the application is
         * entitled to see a mode change it did not ask for. */
        if (extra && extra->focus_filtered && message != WM_DISPLAYCHANGE)
            return unicode ? DefWindowProcW(hwnd, message, wparam, lparam)
                           : DefWindowProcA(hwnd, message, wparam, lparam);
        /* Every side effect runs before the application's proc sees the
         * message, so the app observes the device window already minimized.
         * wined3d device.c device_process_message. */
        if (message == WM_ACTIVATEAPP)
            focus_activation(device, wparam != FALSE);
        /* wined3d answers SC_RESTORE itself and then still forwards, so the
         * application sees WM_SYSCOMMAND after the restore rather than before. */
        if (message == WM_SYSCOMMAND && wparam == SC_RESTORE) {
            if (unicode)
                DefWindowProcW(hwnd, message, wparam, lparam);
            else
                DefWindowProcA(hwnd, message, wparam, lparam);
        }
        if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED)
            d3d9shim_window_push(hwnd, extra && !extra->windowed);
    }

    if (!orig)
        return unicode ? DefWindowProcW(hwnd, message, wparam, lparam)
                       : DefWindowProcA(hwnd, message, wparam, lparam);
    return unicode ? CallWindowProcW(orig, hwnd, message, wparam, lparam)
                   : CallWindowProcA(orig, hwnd, message, wparam, lparam);
}

void
d3d9shim_window_hook_focus(struct d3d9shim_device *dev, HWND fallback)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND focus;
    LONG_PTR orig;

    if (!extra || extra->focus_hooked)
        return;
    /* wined3d installs the subclass unconditionally on the fullscreen
     * transition; D3DCREATE_NOWINDOWCHANGES suppresses only the window
     * restyle, not the proc hook.  The focus window defaults to the device
     * window when none was given. */
    focus = extra->focus_window;
    if (!focus)
        focus = fallback;
    if (!focus || !IsWindow(focus))
        return;
    /* A second device on the same focus window would save our own proc as the
     * one to chain to and recurse without end.  wined3d dedups the same way. */
    if (GetPropW(focus, kFocusProcProp))
        return;
    /* Activate the focus window as it is acquired, which is what
     * wined3d_device_acquire_focus_window does: its SetWindowPos carries
     * neither SWP_NOACTIVATE nor SWP_NOZORDER, so the window is raised and the
     * process becomes the active application.  Skipping it costs the whole
     * focus-loss machine: an application that was never active is sent no
     * WM_ACTIVATEAPP when the foreground moves away. */
    SetWindowPos(focus, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    /* This runs BEFORE the subclass below: SetWindowPos delivers the
     * activation messages synchronously, so they reach the application's own
     * proc rather than re-entering a device that is still being built.
     * Match the window's existing ANSI/Unicode flavour and cache it, so the
     * unhook restores through the same slot. */
    extra->focus_unicode = IsWindowUnicode(focus);
    orig = extra->focus_unicode
               ? SetWindowLongPtrW(focus, GWLP_WNDPROC, (LONG_PTR)focusWindowProc)
               : SetWindowLongPtrA(focus, GWLP_WNDPROC, (LONG_PTR)focusWindowProc);
    SetPropW(focus, kFocusProcProp, (HANDLE)orig);
    SetPropW(focus, kFocusDeviceProp, (HANDLE)dev);
    focus_cache_put(focus, (WNDPROC)orig, dev);   /* ml999 */
    extra->focus_window = focus;
    extra->focus_hooked = 1;
}

void
d3d9shim_window_unhook_focus(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND focus;
    WNDPROC orig, current;

    if (!extra || !extra->focus_hooked)
        return;
    focus = extra->focus_window;
    extra->focus_hooked = 0;
    if (!focus || !IsWindow(focus))
        return;
    /* Drop the device property unconditionally and first: a device pointer
     * that outlives its device would be dereferenced by the next activation
     * message, and the proc treats its absence as "forward only". */
    RemovePropW(focus, kFocusDeviceProp);
    /* ml999: the cached device pointer must die with the property, and it must
     * die FIRST -- the proc reads the cache before the properties, so a stale
     * entry here is exactly the dangling device this ordering exists to stop.
     * Dropping the whole entry is the conservative move: the fallback path is
     * always correct, so a miss costs speed and never correctness. */
    focus_cache_drop(focus);
    orig = (WNDPROC)GetPropW(focus, kFocusProcProp);
    if (!orig)
        return;
    /* Restore only while our proc is still the installed one; an app that
     * re-subclassed on top of us keeps its proc.  Leave the property in place
     * if someone else owns the proc: its WM_NCDESTROY self-heal needs it. */
    current = (WNDPROC)(extra->focus_unicode ? GetWindowLongPtrW(focus, GWLP_WNDPROC)
                                             : GetWindowLongPtrA(focus, GWLP_WNDPROC));
    if (current == focusWindowProc) {
        if (extra->focus_unicode)
            SetWindowLongPtrW(focus, GWLP_WNDPROC, (LONG_PTR)orig);
        else
            SetWindowLongPtrA(focus, GWLP_WNDPROC, (LONG_PTR)orig);
        RemovePropW(focus, kFocusProcProp);
    } else {
        /* ml999: someone else owns the proc, so kFocusProcProp stays for their
         * WM_NCDESTROY self-heal. Put the cache back in step with it -- orig
         * still valid, device gone -- rather than leaving a permanent miss. */
        focus_cache_put(focus, orig, NULL);
    }
}

/* ------------------------------------------------------------------------
 * the cursor (d3d9_device.cpp:1755-1861)
 * ------------------------------------------------------------------------ */

HRESULT
d3d9shim_window_set_cursor(struct d3d9shim_device *dev, UINT hotspot_x,
                           UINT hotspot_y, IDirect3DSurface9 *bitmap)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    uint32_t pixels[32 * 32];
    uint8_t mask[32 * 32 / 8];
    ICONINFO info;
    HCURSOR cursor;
    UINT copy_w, copy_h, row;

    if (!extra || !bitmap)
        return D3DERR_INVALIDCALL;
    memset(&desc, 0, sizeof(desc));
    if (FAILED(IDirect3DSurface9_GetDesc(bitmap, &desc)))
        return D3DERR_INVALIDCALL;

    memset(&locked, 0, sizeof(locked));
    if (FAILED(IDirect3DSurface9_LockRect(bitmap, &locked, NULL, D3DLOCK_READONLY)))
        return D3DERR_INVALIDCALL;
    /* Crop/clamp any bitmap into the 32x32 hardware cursor; a 32x32 bitmap
     * copies whole, a larger one is truncated to the top-left 32x32. */
    memset(pixels, 0, sizeof(pixels));
    copy_w = (desc.Width < 32u ? desc.Width : 32u) * 4u;
    copy_h = desc.Height < 32u ? desc.Height : 32u;
    for (row = 0; row < copy_h; row++)
        memcpy(&pixels[row * 32], (const uint8_t *)locked.pBits + row * locked.Pitch, copy_w);
    IDirect3DSurface9_UnlockRect(bitmap);

    /* 32-bit user32 cursors fall back to the mono mask when the alpha channel
     * is all zeroes; an all-ones mask keeps such bitmaps fully transparent
     * instead (wined3d). */
    memset(mask, 0xff, sizeof(mask));
    memset(&info, 0, sizeof(info));
    info.fIcon = FALSE;
    info.xHotspot = hotspot_x;
    info.yHotspot = hotspot_y;
    info.hbmMask = CreateBitmap(32, 32, 1, 1, mask);
    info.hbmColor = CreateBitmap(32, 32, 1, 32, pixels);
    cursor = CreateIconIndirect(&info);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    if (extra->hw_cursor)
        DestroyCursor(extra->hw_cursor);
    extra->hw_cursor = cursor;
    if (extra->cursor_visible)
        SetCursor(extra->hw_cursor);
    extra->cursor_image_set = 1;
    return D3D_OK;
}

/* The two cursor bodies themselves live in d3d9shim_custom.c, next to the
 * other hand-written slot bodies; these are their user32 halves. */
void
d3d9shim_window_cursor_set_position(struct d3d9shim_device *dev, int x, int y)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    POINT pt;

    /* wined3d device.c warps the OS pointer only when a hardware cursor is
     * realised, and skips the call when the position is unchanged (apps echo
     * back the position they just read every frame). */
    if (!extra || !extra->hw_cursor)
        return;
    if (GetCursorPos(&pt) && pt.x == x && pt.y == y)
        return;
    SetCursorPos(x, y);
}

WINBOOL
d3d9shim_window_cursor_show(struct d3d9shim_device *dev, WINBOOL show)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    WINBOOL previous;

    if (!extra)
        return FALSE;
    /* Returns the previous visibility per the wined3d_device_show_cursor
     * contract; UI toggle code reads the return to drive its own state.
     * Visibility latches only once a cursor image has been set. */
    previous = extra->cursor_visible;
    if (extra->cursor_image_set)
        extra->cursor_visible = show;
    if (extra->hw_cursor)
        SetCursor(show ? extra->hw_cursor : NULL);
    return previous;
}

/* ------------------------------------------------------------------------
 * device lifecycle
 * ------------------------------------------------------------------------ */

void
d3d9shim_window_on_device_reset(struct d3d9shim_device *dev,
                                const D3DPRESENT_PARAMETERS *params)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);
    HWND window;

    if (!extra || !params)
        return;
    window = params->hDeviceWindow ? params->hDeviceWindow : extra->focus_window;
    extra->device_window = window;
    extra->backbuffer_width = params->BackBufferWidth;
    extra->backbuffer_height = params->BackBufferHeight;
    extra->windowed = params->Windowed != 0;

    if (!extra->windowed) {
        d3d9shim_window_enter_fullscreen(dev, window, params->BackBufferWidth,
                                         params->BackBufferHeight);
        d3d9shim_window_hook_focus(dev, window);
    } else {
        d3d9shim_window_leave_fullscreen(dev);
        d3d9shim_window_push(window, 0);
    }
}

void
d3d9shim_window_on_device_destroy(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra = d3d9shim_extra(dev);

    if (!extra)
        return;
    d3d9shim_window_unhook_focus(dev);
    d3d9shim_window_leave_fullscreen(dev);
    if (extra->hw_cursor) {
        DestroyCursor(extra->hw_cursor);
        extra->hw_cursor = NULL;
    }
    if (extra->device_window)
        d3d9shim_window_forget(extra->device_window);
}
