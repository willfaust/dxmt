/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 *
 * iOS-Madeira 2026-05-13: Synthetic monitor / no-op fullscreen so Thumper-
 * style games can negotiate display mode without DISP_CHANGE_BADMODE.
 * Pairs with wsi_monitor_headless.cpp (1024x768 @ 60Hz synthetic monitor).
 */

#include "wsi_window.hpp"
#include "wsi_monitor.hpp"

#include "util_string.hpp"
#include "log/log.hpp"

namespace dxmt::wsi {

/* Same synthetic handle as wsi_monitor_headless.cpp — must match. */
static HMONITOR const kSyntheticMonitor = reinterpret_cast<HMONITOR>(1);

void getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  /* iOS-Madeira 2026-07-07 (task #24): report the REAL client size (user32
   * works fine now) so swapchains match the window instead of a hardcoded
   * 1024x768 that predates the 960x540 virtual desktop. Fall back to the
   * screen size if the window query fails. */
  RECT rect;
  if (hWindow && ::GetClientRect(hWindow, &rect) &&
      rect.right > rect.left && rect.bottom > rect.top) {
    if (pWidth)
      *pWidth = (uint32_t)(rect.right - rect.left);
    if (pHeight)
      *pHeight = (uint32_t)(rect.bottom - rect.top);
    return;
  }
  int sw = ::GetSystemMetrics(SM_CXSCREEN);
  int sh = ::GetSystemMetrics(SM_CYSCREEN);
  if (pWidth)
    *pWidth = (sw > 0) ? (uint32_t)sw : 1024;
  if (pHeight)
    *pHeight = (sh > 0) ? (uint32_t)sh : 768;
}

void resizeWindow(HWND hWindow, DXMTWindowState *pState, uint32_t width,
                  uint32_t height) {
}

/* Headless no-op fullscreen success. Games negotiate a mode and expect
 * setWindowMode/enterFullscreenMode to return true; failing here makes
 * games abort with DISP_CHANGE_BADMODE-equivalent error paths. */
bool setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  return true;
}

bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow,
                         DXMTWindowState *pState,
                         [[maybe_unused]] bool modeSwitch) {
  return true;
}

bool leaveFullscreenMode(HWND hWindow, DXMTWindowState *pState,
                         bool restoreCoordinates) {
  return true;
}

bool restoreDisplayMode(HMONITOR hMonitor) {
  return true;
}

HMONITOR getWindowMonitor(HWND hWindow) {
  /* Return the same synthetic monitor everything else uses. */
  return kSyntheticMonitor;
}

bool isWindow(HWND hWindow) { return true; }

void updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow,
                            bool forceTopmost) {
}

bool isForeground(HWND hWindow) { return true; }

bool isMinimized(HWND hWindow) { return false; }

} // namespace dxmt::wsi
