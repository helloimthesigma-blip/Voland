/**
 * core/gpu/texture tex_sample: shader coordinates that are huge or NaN
 * sample safely (clamped/wrapped like any other coordinate) instead of
 * reading outside the texture - SSBU's first match once crashed the
 * reference renderer this way (x0 saturated to INT32_MAX, x0 + 1 wrapped
 * negative and passed the fast path's bounds check).
 */
#define CHECK_NAME "tex_sample_test"
#include "check.h"

#include "gpu/texture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SIDE 4u

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
  printf("[tex_sample_test] passed\n");
  return 0;
}
