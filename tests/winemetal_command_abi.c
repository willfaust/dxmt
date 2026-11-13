#include "../src/winemetal/winemetal.h"
#include "../src/airconv/airconv_public.h"

_Static_assert(WMTBlitCommandOptimizeContentsForGPUAccess == 13, "legacy blit command changed");
_Static_assert(WMTBlitCommandCopyTexture != WMTBlitCommandOptimizeContentsForGPUAccess, "blit commands collide");
_Static_assert(WMTRenderCommandSetFragmentSamplerState == 45, "legacy fragment sampler command changed");
_Static_assert(WMTRenderCommandSetVertexTexture == 46, "legacy vertex texture command changed");
_Static_assert(WMTRenderCommandSetVertexSamplerState == 47, "legacy vertex sampler command changed");
_Static_assert(WMTRenderCommandSetDepthStencilState != WMTRenderCommandSetFragmentSamplerState, "render commands collide");

_Static_assert(SM50_SHADER_PSO_VERTEX_INTERFACE != SM50_SHADER_ROOT_SIGNATURE, "shader argument types collide");
_Static_assert(SM50_SHADER_PSO_VERTEX_INTERFACE != SM50_SHADER_ROOT_SIGNATURE2, "shader argument types collide");
