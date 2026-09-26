/*
 * wsi_window_madeira.cpp -- wsi window services for the native Madeira build.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(d)).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * wsi_window_headless.cpp is the shape this follows -- every fullscreen entry
 * point returns true so a game's mode negotiation does not abort -- but it
 * cannot be reused: it calls ::GetClientRect and ::GetSystemMetrics, and
 * neither exists below the Win32 boundary.  The client size comes from a
 * per-HWND cache the shim fills instead (wsi_window_madeira.hpp).
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

#include "wsi_window.hpp"
#include "wsi_window_madeira.hpp"
#include "wsi_monitor.hpp"

#include <mutex>
#include <unordered_map>

namespace dxmt::wsi {

/* Same synthetic handle as wsi_monitor_headless.cpp -- must match. */
static HMONITOR const kSyntheticMonitor = reinterpret_cast<HMONITOR>(1);

namespace {

struct ClientSize {
  uint32_t width;
  uint32_t height;
};

/* A plain std::mutex, not dxmt::mutex: the map is touched once per
 * CreateDevice / Reset / Present and once per swapchain query, never in the
 * encode path, and the SRW specialisation of dxmt::mutex does not exist on
 * this side of the boundary anyway. */
std::mutex &
cache_mutex() {
  static std::mutex m;
  return m;
}

std::unordered_map<HWND, ClientSize> &
cache() {
  static std::unordered_map<HWND, ClientSize> map;
  return map;
}

ClientSize g_default = {1024, 768};

} // namespace

void
madeira_set_client_size(HWND window, uint32_t width, uint32_t height) {
  if (!width || !height)
    return;
  std::lock_guard<std::mutex> guard(cache_mutex());
  if (!window) {
    g_default = {width, height};
    return;
  }
  cache()[window] = {width, height};
}

void
madeira_set_default_client_size(uint32_t width, uint32_t height) {
  if (!width || !height)
    return;
  std::lock_guard<std::mutex> guard(cache_mutex());
  g_default = {width, height};
}

void
madeira_forget_window(HWND window) {
  std::lock_guard<std::mutex> guard(cache_mutex());
  cache().erase(window);
}

void
madeira_forget_all_windows() {
  std::lock_guard<std::mutex> guard(cache_mutex());
  cache().clear();
}

void
getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  ClientSize size;
  {
    std::lock_guard<std::mutex> guard(cache_mutex());
    auto it = cache().find(hWindow);
    size = (it != cache().end()) ? it->second : g_default;
  }
  if (pWidth)
    *pWidth = size.width;
  if (pHeight)
    *pHeight = size.height;
}

/* The window is the guest's; only the shim may resize it. */
void
resizeWindow(HWND hWindow, DXMTWindowState *pState, uint32_t width, uint32_t height) {
  (void)hWindow;
  (void)pState;
  (void)width;
  (void)height;
}

/* Headless no-op fullscreen success, exactly as wsi_window_headless.cpp:
 * games negotiate a mode and abort on a failure here.  There is one
 * Swift-owned CAMetalLayer for every HWND (section 7.1), so a mode switch has
 * nothing to switch. */
bool
setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  (void)hMonitor;
  (void)hWindow;
  (void)mode;
  return true;
}

bool
enterFullscreenMode(HMONITOR hMonitor, HWND hWindow, DXMTWindowState *pState,
                    [[maybe_unused]] bool modeSwitch) {
  (void)hMonitor;
  (void)hWindow;
  (void)pState;
  return true;
}

bool
leaveFullscreenMode(HWND hWindow, DXMTWindowState *pState, bool restoreCoordinates) {
  (void)hWindow;
  (void)pState;
  (void)restoreCoordinates;
  return true;
}

bool
restoreDisplayMode(HMONITOR hMonitor) {
  (void)hMonitor;
  return true;
}

HMONITOR
getWindowMonitor(HWND hWindow) {
  (void)hWindow;
  return kSyntheticMonitor;
}

bool
isWindow(HWND hWindow) {
  (void)hWindow;
  return true;
}

void
updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, bool forceTopmost) {
  (void)hMonitor;
  (void)hWindow;
  (void)forceTopmost;
}

/* The app is the only thing on screen when it is running at all, and the
 * shim owns the real activation state.  Reporting "foregrounded, not
 * minimized" unconditionally is what d3d11_swapchain.cpp already does under
 * DXMT_IOS (section 7.7 risk 7). */
bool
isForeground(HWND hWindow) {
  (void)hWindow;
  return true;
}

HWND
foregroundWindow() {
  return nullptr;
}

bool
isMinimized(HWND hWindow) {
  (void)hWindow;
  return false;
}

} // namespace dxmt::wsi
