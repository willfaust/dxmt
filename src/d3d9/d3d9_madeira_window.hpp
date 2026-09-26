/*
 * d3d9_madeira_window.hpp -- the window seam for the native Madeira build.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(d), section 8.5).  New file,
 * GPL-3.0-or-later; see research/dxmt/LICENSE-MADEIRA.md.
 *
 * "All user32/gdi32 stays in the shim": the fullscreen restyle, the focus
 * window subclass and the activation reposition all deliver messages to the
 * application SYNCHRONOUSLY and must therefore run on the guest's own thread,
 * which is a thing the native frontend does not have.  So the frontend keeps
 * the decisions -- when to go fullscreen, when a device is Lost, which
 * monitor -- and hands the window work across this seam.
 *
 * Step 1 implements every entry point as a no-op in
 * src/d3d9/unix/d3d9_native_glue.cpp; step 3 gives them a shim call.  They
 * are declared out of line rather than inlined as empty bodies on purpose:
 * an unimplemented seam should be greppable in the archive's symbol table,
 * not compiled away.
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

#pragma once

#ifdef DXMT_MADEIRA

#include <windows.h>

#include <cstdint>

namespace dxmt {

/* wined3d_swapchain_state_setup_fullscreen / _restore_from_fullscreen: the
 * borderless restyle and the reposition.  The frontend still tracks WHICH
 * window is fullscreen and at what size, because Reset and the swapchain read
 * that back; only the window calls cross. */
void madeira_window_enter_fullscreen(HWND window, uint32_t width, uint32_t height);
void madeira_window_leave_fullscreen(HWND window, bool restore_rect);

/* wined3d_device_acquire_focus_window plus the WM_ACTIVATEAPP subclass.
 * `device` is the MTLD3D9Device the transitions belong to; the shim hands it
 * back with the activation so the native side can run onFocusActivation. */
void madeira_window_hook_focus(HWND window, void *device);
void madeira_window_unhook_focus(HWND window, void *device);

/* wined3d_swapchain_activate's two window touches. */
void madeira_window_minimize(HWND window);
void madeira_window_reposition(HWND window, uint32_t width, uint32_t height);

/* IsWindowVisible: the guard in front of the minimize. */
bool madeira_window_is_visible(HWND window);

} // namespace dxmt

#endif /* DXMT_MADEIRA */
