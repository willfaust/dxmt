#pragma once
/* ============================================================================
 * ml679 -- complete, spec-accurate BCn block decoders.
 *
 * A15 cannot sample any BC format (supportsBCTextureCompression = NO), so every
 * BC resource is remapped to an uncompressed one and we must supply the texels.
 * BC1 and BC3 already decoded; BC2/BC4/BC5/BC7 were being filled with synthetic
 * patterns, which is why foliage alpha masks (BC4) came out as checkerboards and
 * normals (BC5) as flat blue.
 *
 * Layout follows D3D's BCn spec exactly, including the cases that are easy to
 * get subtly wrong and that show up as edge artefacts:
 *   - BC1 3-colour (punch-through) mode when c0 <= c1: index 3 is transparent
 *     BLACK, and the third colour is a 1/2 blend, not 1/3-2/3.
 *   - BC4/BC5 6-interpolant mode when a0 <= a1: indices 6 and 7 are hard 0 and 1.
 *   - Signed BC4/BC5 (-1..1) are distinct formats from unsigned; -128 clamps to
 *     -127 so the range stays symmetric.
 *   - BC7 anchor indices: the first index of each subset is stored with one bit
 *     fewer, because its high bit is implied zero.
 * ==========================================================================*/
#include <cstdint>
#include <cstring>

namespace dxmt {

/* ---- shared helpers ---------------------------------------------------- */

static inline void bcn_rgb565(uint16_t c, uint8_t *out) {
  uint8_t r = (uint8_t)((c >> 11) & 0x1F), g = (uint8_t)((c >> 5) & 0x3F), b = (uint8_t)(c & 0x1F);
  out[0] = (uint8_t)((r << 3) | (r >> 2));
  out[1] = (uint8_t)((g << 2) | (g >> 4));
  out[2] = (uint8_t)((b << 3) | (b >> 2));
}

/* The BC4/BC5 alpha/scalar sub-block, shared by BC3's alpha too. */
static inline void bcn_scalar_block(const uint8_t *blk, uint8_t out[16]) {
  uint8_t a[8];
  a[0] = blk[0]; a[1] = blk[1];
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1] + 2) / 5);
    a[6] = 0; a[7] = 255;          /* 6-interpolant mode: hard endpoints */
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
  for (int t = 0; t < 16; t++) out[t] = a[(bits >> (t * 3)) & 7];
}

/* Signed variant: endpoints are int8 in -127..127, output re-biased to 0..255
 * so it can live in an R8Snorm/RG8Snorm texture unchanged. */
static inline void bcn_scalar_block_signed(const uint8_t *blk, int8_t out[16]) {
  int8_t a[8];
  int8_t e0 = (int8_t)blk[0], e1 = (int8_t)blk[1];
  if (e0 == -128) e0 = -127;
  if (e1 == -128) e1 = -127;
  a[0] = e0; a[1] = e1;
  if (e0 > e1) {
    for (int i = 1; i < 7; i++) a[i + 1] = (int8_t)(((7 - i) * e0 + i * e1) / 7);
  } else {
    for (int i = 1; i < 5; i++) a[i + 1] = (int8_t)(((5 - i) * e0 + i * e1) / 5);
    a[6] = -127; a[7] = 127;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
  for (int t = 0; t < 16; t++) out[t] = a[(bits >> (t * 3)) & 7];
}

/* ---- BC1 / BC2 / BC3 ---------------------------------------------------- */

static inline void bcn_bc1_block(const uint8_t *blk, uint8_t out[64], bool punchthrough) {
  uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
  uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
  uint8_t p[4][4];
  bcn_rgb565(c0, p[0]); p[0][3] = 255;
  bcn_rgb565(c1, p[1]); p[1][3] = 255;
  if (!punchthrough || c0 > c1) {
    for (int i = 0; i < 3; i++) {
      p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3);
      p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int i = 0; i < 3; i++) {
      p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2);
      p[3][i] = 0;
    }
    p[2][3] = 255;
    p[3][3] = 0;
  }
  uint32_t idx = (uint32_t)(blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24));
  for (int t = 0; t < 16; t++) {
    const uint8_t *s = p[(idx >> (t * 2)) & 3];
    out[t * 4 + 0] = s[0]; out[t * 4 + 1] = s[1]; out[t * 4 + 2] = s[2]; out[t * 4 + 3] = s[3];
  }
}

/* BC2 = 4-bit explicit alpha + a BC1 colour block always in 4-colour mode. */
static inline void bcn_bc2_block(const uint8_t *blk, uint8_t out[64]) {
  bcn_bc1_block(blk + 8, out, false);
  for (int t = 0; t < 16; t++) {
    uint8_t nib = (uint8_t)((blk[t >> 1] >> ((t & 1) * 4)) & 0xF);
    out[t * 4 + 3] = (uint8_t)((nib << 4) | nib);
  }
}

static inline void bcn_bc3_block(const uint8_t *blk, uint8_t out[64]) {
  bcn_bc1_block(blk + 8, out, false);
  uint8_t a[16];
  bcn_scalar_block(blk, a);
  for (int t = 0; t < 16; t++) out[t * 4 + 3] = a[t];
}

/* ---- BC4 / BC5 ---------------------------------------------------------- */

static inline void bcn_bc4_block(const uint8_t *blk, uint8_t out[16], bool is_signed) {
  if (is_signed) {
    int8_t s[16];
    bcn_scalar_block_signed(blk, s);
    for (int t = 0; t < 16; t++) out[t] = (uint8_t)s[t];
  } else {
    bcn_scalar_block(blk, out);
  }
}

static inline void bcn_bc5_block(const uint8_t *blk, uint8_t out[32], bool is_signed) {
  uint8_t r[16], g[16];
  bcn_bc4_block(blk, r, is_signed);
  bcn_bc4_block(blk + 8, g, is_signed);
  for (int t = 0; t < 16; t++) { out[t * 2 + 0] = r[t]; out[t * 2 + 1] = g[t]; }
}

/* ---- BC7 ---------------------------------------------------------------- */

void bcn_bc7_block(const uint8_t *blk, uint8_t out[64]);

} // namespace dxmt
