/*
 * The DXGI <-> Vulkan swapchain interfaces of vkd3d-proton
 * (include/vkd3d_swapchain_factory.idl at 750061ae), transcribed to C++ for
 * DXMT's DXGI. Interface declarations only; DXVK's DXGI consumes the same
 * interfaces.
 *
 * Copyright 2020 Joshua Ashton for Valve Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */
#pragma once

#include <cstdint>
#include <dxgi1_6.h>
#include "com/com_guid.hpp"

/* The few Vulkan handle types the interfaces name. DXMT's PE side has no
 * Vulkan headers; these match vulkan_core.h when it is not included. */
#ifndef VK_VERSION_1_0
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__) ||            \
    defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
typedef struct VkSurfaceKHR_T *VkSurfaceKHR;
#else
typedef uint64_t VkSurfaceKHR;
#endif
typedef int32_t VkResult;
#endif

typedef struct DXGI_VK_HDR_METADATA {
  DXGI_HDR_METADATA_TYPE Type;
  union {
    DXGI_HDR_METADATA_HDR10 HDR10;
  };
} DXGI_VK_HDR_METADATA;

typedef struct DXGI_VK_FRAME_STATISTICS {
  UINT64 PresentCount;
  UINT64 PresentQPCTime;
} DXGI_VK_FRAME_STATISTICS;

DEFINE_COM_INTERFACE("1e7895a1-1bc3-4f9c-a670-290a4bc9581a", IDXGIVkSurfaceFactory)
    : public IUnknown {
  virtual VkResult STDMETHODCALLTYPE CreateSurface(VkInstance Instance,
                                                   VkPhysicalDevice Adapter,
                                                   VkSurfaceKHR *pSurface) = 0;
};

DEFINE_COM_INTERFACE("e4a9059e-b569-46ab-8de7-501bd2bc7f7a", IDXGIVkSwapChain)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC1 * pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapter(REFIID riid, void **ppvObject) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetImage(UINT BufferId, REFIID riid,
                                             void **ppBuffer) = 0;
  virtual UINT STDMETHODCALLTYPE GetImageIndex() = 0;
  virtual UINT STDMETHODCALLTYPE GetFrameLatency() = 0;
  virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyEvent() = 0;
  virtual HRESULT STDMETHODCALLTYPE
  ChangeProperties(const DXGI_SWAP_CHAIN_DESC1 *pDesc, const UINT *pNodeMasks,
                   IUnknown *const *ppPresentQueues) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPresentRegion(const RECT *pRegion) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  SetGammaControl(UINT NumControlPoints, const DXGI_RGB *pControlPoints) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetFrameLatency(UINT MaxLatency) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  Present(UINT SyncInterval, UINT PresentFlags,
          const DXGI_PRESENT_PARAMETERS *pPresentParameters) = 0;
  virtual UINT STDMETHODCALLTYPE
  CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetColorSpace(DXGI_COLOR_SPACE_TYPE ColorSpace) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  SetHDRMetaData(const DXGI_VK_HDR_METADATA *pMetaData) = 0;
};

DEFINE_COM_INTERFACE("785326d4-b77b-4826-ae70-8d08308ee6d1", IDXGIVkSwapChain1)
    : public IDXGIVkSwapChain {
  virtual void STDMETHODCALLTYPE GetLastPresentCount(UINT64 * pLastPresentCount) = 0;
  virtual void STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_VK_FRAME_STATISTICS * pFrameStatistics) = 0;
};

DEFINE_COM_INTERFACE("aed91093-e02e-458c-bdef-a97da1a7e6d2", IDXGIVkSwapChain2)
    : public IDXGIVkSwapChain1 {
  virtual void STDMETHODCALLTYPE SetTargetFrameRate(double FrameRate) = 0;
};

DEFINE_COM_INTERFACE("e7d6c3ca-23a0-4e08-9f2f-ea5231df6633", IDXGIVkSwapChainFactory)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IDXGIVkSurfaceFactory * pSurfaceFactory,
                  const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                  IDXGIVkSwapChain **ppSwapChain) = 0;
};
