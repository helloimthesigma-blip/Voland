/**
 * core/gpu/texture tex_sample: shader coordinates that are huge or NaN
 * sample safely (clamped/wrapped like any other coordinate) instead of
 * reading outside the texture - SSBU's first match once crashed the
 * reference renderer this way (x0 saturated to INT32_MAX, x0 + 1 wrapped
 * negative and passed the fast path's bounds check).
 *
 * Also: a BC6H (unsigned) texture decodes to half-float RGBA texels that
 * read back as its block's values (SSBU lights with BC6H cube maps).
 */
#define CHECK_NAME "tex_sample_test"
#include "check.h"

#include "gpu/bc7.h"
#include "gpu/texture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SIDE 4u
#define BC6H_UF16_FORMAT 0x11u
#define DXN2_FORMAT 0x28u /* BC5 */

static uint8_t g_texels[SIDE * SIDE * 4u];

int main(void) {
  tex_init_tables();
  for (uint32_t i = 0; i < sizeof(g_texels); i++) g_texels[i] = (uint8_t)(i * 7u);
  Tex_Image img;
  memset(&img, 0, sizeof(img));
  img.header.type = TEX_TYPE_2D;
  img.header.normalized = false;
  for (uint32_t c = 0; c < 4u; c++) img.header.swizzle[c] = (uint8_t)(TEX_SOURCE_R + c);
  img.width = img.height = SIDE;
  img.layers = 1;
  img.bytes_per_texel = 4;
  img.row_bytes = SIDE * 4u;
  img.layer_bytes = sizeof(g_texels);
  img.texels = g_texels;
  img.rgba8 = true;
  img.valid = true;
  Tex_Sampler s;
  memset(&s, 0, sizeof(s));
  s.mag_filter = s.min_filter = 2u; /* linear: the fast path */
  const float bad[][2] = {{3.0e9f, 1.0f}, {1.0f, 3.0e9f}, {-3.0e9f, 2.0f}, {NAN, NAN}, {INFINITY, -INFINITY},
                          {2147483520.0f, 2147483520.0f}};
  for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    const float coords[3] = {bad[i][0], bad[i][1], 0.0f};
    uint32_t out[4] = {0};
    tex_sample(&img, &s, coords, 0.0f, 0.0f, false, NULL, out);
    s.mag_filter = 1u; /* and the nearest path */
    tex_sample(&img, &s, coords, 0.0f, 0.0f, false, NULL, out);
    s.mag_filter = 2u;
  }
  /* An ordinary coordinate still reads the right texel. */
  s.mag_filter = 1u;
  const float centre[3] = {1.5f, 2.5f, 0.0f};
  uint32_t out[4];
  tex_sample(&img, &s, centre, 0.0f, 0.0f, false, NULL, out);
  float red;
  memcpy(&red, &out[0], sizeof(red));
  CHECK(fabsf(red - (float)g_texels[(2u * SIDE + 1u) * 4u] / 255.0f) < 1e-6f);
  /* BC6H: one 4x4 block, pitch layout. */
  {
    static const uint8_t block[BC7_BLOCK_BYTES] = {0x03, 0x91, 0x4c, 0x2e, 0x7a, 0x15, 0xc8, 0x63,
                                                   0x0f, 0xb2, 0x55, 0x19, 0xe4, 0x87, 0x3d, 0xa6};
    Tex_Header h;
    memset(&h, 0, sizeof(h));
    h.format = BC6H_UF16_FORMAT;
    h.width = h.height = SIDE;
    h.type = TEX_TYPE_2D;
    h.layout = TEX_LAYOUT_PITCH;
    h.pitch = BC7_BLOCK_BYTES;
    h.levels = 1;
    for (uint32_t c = 0; c < 4u; c++) h.data_type[c] = TEX_DATA_FLOAT;
    static uint8_t decoded[4096];
    CHECK(tex_decoded_bytes(&h) <= sizeof(decoded));
    Tex_Image bc6;
    CHECK(tex_decode(&h, block, decoded, &bc6));
    CHECK(bc6.bytes_per_texel == 8u && !bc6.rgba8);
    uint16_t want[BC7_TEXELS][4];
    bc6h_decode_block(block, false, want);
    uint32_t differ = 0;
    for (uint32_t t = 0; t < BC7_TEXELS; t++) {
      uint32_t texel[4];
      tex_texel(&bc6, t % SIDE, t / SIDE, 0, texel);
      for (uint32_t c = 0; c < 4u; c++) {
        const uint32_t h16 = want[t][c], exponent = (h16 >> 10) & 0x1fu, mantissa = h16 & 0x3ffu;
        const float expect = exponent ? ldexpf((float)(mantissa | 0x400u), (int)exponent - 25) : ldexpf((float)mantissa, -24);
        float got;
        memcpy(&got, &texel[c], sizeof(got));
        if (got != expect) differ++;
      }
    }
    CHECK(differ == 0);
  }
  /* BC5 SNORM: signed endpoints. Red 127 / -127 (eight-value mode) with
   * texels 0 and 1 on the endpoints; green 0 / 64 (six-value mode) with
   * texels 0 and 1 on its fixed -127 and 127. Decoded as unsigned these
   * would read about 0.5 and 0.5, not 1 and -1. */
  {
    static const uint8_t block[16] = {0x7f, 0x81, 0x08, 0, 0, 0, 0, 0, 0x00, 0x40, 0x3e, 0, 0, 0, 0, 0};
    Tex_Header h;
    memset(&h, 0, sizeof(h));
    h.format = DXN2_FORMAT;
    h.width = h.height = SIDE;
    h.type = TEX_TYPE_2D;
    h.layout = TEX_LAYOUT_PITCH;
    h.pitch = sizeof(block);
    h.levels = 1;
    for (uint32_t c = 0; c < 4u; c++) h.data_type[c] = TEX_DATA_SNORM;
    static uint8_t decoded[4096];
    CHECK(tex_decoded_bytes(&h) <= sizeof(decoded));
    Tex_Image bc5;
    CHECK(tex_decode(&h, block, decoded, &bc5));
    CHECK(!bc5.rgba8);
    uint32_t t0[4], t1[4];
    tex_texel(&bc5, 0, 0, 0, t0);
    tex_texel(&bc5, 1, 0, 0, t1);
    float r0, r1, g0, g1, a0;
    memcpy(&r0, &t0[0], 4);
    memcpy(&g0, &t0[1], 4);
    memcpy(&a0, &t0[3], 4);
    memcpy(&r1, &t1[0], 4);
    memcpy(&g1, &t1[1], 4);
    CHECK(r0 == 1.0f && r1 == -1.0f && g0 == -1.0f && g1 == 1.0f && a0 == 1.0f);
  }
  /* SUST encoding: what tex_encode_texel stores, tex_decode_texel reads
   * back (RGBA16F, RGBA8 UNORM, a masked write), and 3D block-linear
   * addresses stay inside the image's bytes and never collide. */
  {
    static const uint8_t float4[4] = {TEX_DATA_FLOAT, TEX_DATA_FLOAT, TEX_DATA_FLOAT, TEX_DATA_FLOAT};
    static const uint8_t unorm4[4] = {TEX_DATA_UNORM, TEX_DATA_UNORM, TEX_DATA_UNORM, TEX_DATA_UNORM};
    const float in_f[4] = {0.25f, -2.0f, 1024.0f, 1.0f};
    uint32_t in[4], back[4];
    memcpy(in, in_f, sizeof(in));
    uint8_t texel[16], keep[16];
    CHECK(tex_encode_texel(0x03u /* R16G16B16A16 */, float4, in, 0xfu, texel, keep));
    tex_decode_texel(0x03u, float4, false, texel, back);
    CHECK(memcmp(back, in, sizeof(in)) == 0);
    const float in8[4] = {0.0f, 0.5f, 1.5f, 1.0f};
    memcpy(in, in8, sizeof(in));
    CHECK(tex_encode_texel(0x08u /* A8B8G8R8 */, unorm4, in, 0x5u, texel, keep));
    CHECK(texel[0] == 0u && texel[2] == 255u && keep[0] == 0xffu && keep[1] == 0u && keep[2] == 0xffu && keep[3] == 0u);
    CHECK(!tex_encode_texel(0x40u /* ASTC */, unorm4, in, 0xfu, texel, keep));
    Tex_Header h3;
    memset(&h3, 0, sizeof(h3));
    h3.format = 0x03u;
    h3.type = TEX_TYPE_3D;
    h3.layout = TEX_LAYOUT_BLOCK_LINEAR;
    h3.width = h3.height = h3.depth = 16u;
    h3.levels = 1;
    h3.block_height_log2 = 1;
    h3.block_depth_log2 = 1;
    static uint8_t seen[16u * 16u * 16u * 8u];
    memset(seen, 0, sizeof(seen));
    uint32_t bad_address = 0;
    for (uint32_t z = 0; z < 16u; z++)
      for (uint32_t y = 0; y < 16u; y++)
        for (uint32_t x = 0; x < 16u; x++) {
          uint64_t off = 0;
          uint32_t bytes = 0;
          if (!tex_texel_address(&h3, x, y, z, &off, &bytes) || bytes != 8u || off + 8u > sizeof(seen) || seen[off]) {
            bad_address++;
            continue;
          }
          seen[off] = 1;
        }
    CHECK(bad_address == 0);
    uint64_t off = 0;
    uint32_t bytes = 0;
    CHECK(!tex_texel_address(&h3, 16u, 0, 0, &off, &bytes));
  }
  printf("[tex_sample_test] passed\n");
  return 0;
}
