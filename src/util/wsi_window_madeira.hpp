/*
 * wsi_window_madeira.hpp -- the per-HWND client-size cache the shim fills.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(d)).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * The native frontend has no user32: an HWND down here is an opaque guest
 * token, and ::GetClientRect does not exist.  The shim runs on the guest's
 * own thread, where the window really lives, and pushes the client size
 * across at CreateDevice / Reset / Present; the unix glue forwards it here.
 * getWindowSize() then answers from the cache instead of guessing.
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

#include <windows.h>

#include <cstdint>

namespace dxmt::wsi {

/* Called by the unix glue whenever the shim reports a window's client size.
 * A width or height of 0 is rejected: a zero-sized swapchain is never what
 * the caller meant, and the last good size is a better answer than one. */
void madeira_set_client_size(HWND window, uint32_t width, uint32_t height);

/* Drop one window's entry (device teardown), or every entry belonging to a
 * dead guest process (d3d9_native_process_teardown). */
void madeira_forget_window(HWND window);
void madeira_forget_all_windows();

/* The fallback used when a window has no cached size yet -- the virtual
 * desktop the app is presented into.  Set once at init from the same place
 * the layer size comes from; defaults below. */
void madeira_set_default_client_size(uint32_t width, uint32_t height);

} // namespace dxmt::wsi
