/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 *
 * iOS-Mythic note 2026-05-13: Was all-nullptr stub which caused
 * IDXGIAdapter::EnumOutputs to return DXGI_ERROR_NOT_FOUND. Thumper
 * then stored NULL in a global IDXGIOutput slot and crashed calling
 * vtable[7] (GetDesc) through NULL. Implements a synthetic single
 * 1024x768 @ 60Hz monitor so the DXGI output chain succeeds.
 */

#include "wsi_monitor.hpp"

namespace dxmt::wsi {

/* Synthetic singleton monitor handle. Non-NULL so EnumOutputs sees a
 * monitor exists; opaque value (1) since headless code paths never
 * dereference HMONITOR. */
static HMONITOR const kSyntheticMonitor = reinterpret_cast<HMONITOR>(1);

HMONITOR getDefaultMonitor() {
  return kSyntheticMonitor;
}

HMONITOR enumMonitors(uint32_t index) {
  /* Only one synthetic monitor exists. */
  if (index != 0)
    return nullptr;
  return kSyntheticMonitor;
}

bool getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
  if (hMonitor != kSyntheticMonitor)
    return false;
  /* Standard Windows display device name "\\.\DISPLAY1". */
  static const WCHAR kName[] = {'\\','\\','.','\\','D','I','S','P','L','A','Y','1', 0};
  for (size_t i = 0; i < 32; i++)
    Name[i] = (i < sizeof(kName)/sizeof(WCHAR)) ? kName[i] : (WCHAR)0;
  return true;
}

bool getDesktopCoordinates(HMONITOR hMonitor, RECT *pRect) {
  if (hMonitor != kSyntheticMonitor || !pRect)
    return false;
  /* 1024x768 at origin — matches Wine iOS sysparams_ios.c + driver_ios.c
   * which advertise 1024x768. Mismatch caused NtUserChangeDisplaySettings
   * to return DISP_CHANGE_BADMODE and a downstream fault loop. */
  pRect->left = 0;
  pRect->top = 0;
  pRect->right = 1024;
  pRect->bottom = 768;
  return true;
}

/* Single supported mode: 1024x768 @ 60Hz, 32bpp, non-interlaced. */
static inline bool retrieveDisplayMode(HMONITOR hMonitor, DWORD modeNumber,
                                       WsiMode *pMode) {
  if (hMonitor != kSyntheticMonitor || !pMode)
    return false;
  pMode->width = 1024;
  pMode->height = 768;
  pMode->refreshRate.numerator = 60;
  pMode->refreshRate.denominator = 1;
  pMode->bitsPerPixel = 32;
  pMode->interlaced = false;
  return true;
}

bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode *pMode) {
  if (modeNumber != 0)
    return false;  /* only one mode exposed */
  return retrieveDisplayMode(hMonitor, modeNumber, pMode);
}

bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  return retrieveDisplayMode(hMonitor, ENUM_CURRENT_SETTINGS, pMode);
}

bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  return retrieveDisplayMode(hMonitor, ENUM_REGISTRY_SETTINGS, pMode);
}

} // namespace dxmt::wsi
