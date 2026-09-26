#pragma once
/* ml2000: policy half of the BC mip clamp, kept free of Windows/Metal types so
 * build/host-tests/check-mip-clamp-auto.py can compile it on the host.
 *
 * The clamp itself (ml675/ml745/ml746) creates an eligible texture with its top
 * `bias` levels missing from the PHYSICAL Metal texture while the D3D
 * descriptor stays LOGICAL. d3d11.mipClampBC=N applies it to every eligible
 * texture. d3d11.mipClampAuto (default on) applies bias 1 only to LARGE ones,
 * and only while the process is close to its memory limit: a scene load that
 * would otherwise run the footprint into jetsam gets half-resolution top mips
 * for the textures it creates from then on, and a process with room to spare
 * is left untouched. */
#include <cstdint>

namespace dxmt::mip_clamp {

/* Auto clamp considers a texture only when Width or Height reaches this. */
constexpr uint32_t kAutoMinDimension = 1024;
/* Headroom is re-read at least once per this many auto candidates. */
constexpr uint32_t kRequeryInterval = 16;

/* Headroom sentinels (MB values are >= 0). */
constexpr int64_t kHeadroomUnknown = -1;      /* never measured */
constexpr int64_t kHeadroomUnavailable = -2;  /* the query does not work here: stop asking */

/* Largest bias <= `requested` that leaves at least one level and a physical
 * top of at least 4x4 texels (one BC block). With `block_aligned`, the
 * physical top must also be a whole number of 4x4 blocks, so the smaller
 * texture is still a valid BC texture even for odd non-power-of-two sizes.
 * Physical mip p of the result is exactly logical mip p+bias:
 * (W >> bias) >> p == W >> (bias + p). */
inline uint32_t ClampBias(uint32_t requested, uint32_t width, uint32_t height, uint32_t mip_levels,
                          bool block_aligned) {
  if (mip_levels < 2)
    return 0;   /* a single-level texture has nothing to drop */
  uint32_t bias = requested < mip_levels - 1 ? requested : mip_levels - 1;
  while (bias && ((width >> bias) < 4 || (height >> bias) < 4 ||
                  (block_aligned && (((width >> bias) & 3u) || ((height >> bias) & 3u)))))
    bias--;
  return bias;
}

inline bool AutoSizeEligible(uint32_t width, uint32_t height) {
  return width >= kAutoMinDimension || height >= kAutoMinDimension;
}

/* Must the headroom be re-read before deciding auto candidate number `n`
 * (1-based)? Always on the first one, every kRequeryInterval-th one, and on
 * every one while the last reading was already within 2x the threshold -- the
 * region where a scene load crosses the line within a few textures. Never
 * again once the query proved unavailable. */
inline bool ShouldRequery(uint64_t n, int64_t last_headroom_mb, uint32_t threshold_mb) {
  if (last_headroom_mb == kHeadroomUnavailable)
    return false;
  if (last_headroom_mb < 0)
    return true;
  if (n % kRequeryInterval == 0)
    return true;
  return (uint64_t)last_headroom_mb < 2ull * threshold_mb;
}

/* Clamp while the measured headroom is below the threshold. An unknown or
 * unavailable reading never clamps; threshold 0 never clamps. */
inline bool UnderPressure(int64_t headroom_mb, uint32_t threshold_mb) {
  return headroom_mb >= 0 && (uint64_t)headroom_mb < threshold_mb;
}

/* Rate limit for the per-clamp log line: the first 8, then every 64th. */
inline bool ShouldLogClamp(uint64_t clamped_n) {
  return clamped_n <= 8 || (clamped_n % 64) == 0;
}

/* Is an explicit "off" value? Accepts 0/false/off/no in any case. */
inline bool IsOffValue(const char *v) {
  if (!v)
    return false;
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
  auto eq = [&](const char *want) {
    const char *p = v;
    while (*p == ' ' || *p == '\t') p++;
    while (*want && lower(*p) == *want) { p++; want++; }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return !*want && !*p;
  };
  return eq("0") || eq("false") || eq("off") || eq("no");
}

} // namespace dxmt::mip_clamp
