/**
 * VIC colour conversion. See vic_convert.h. Limited-range YCbCr to RGB
 * in 16.16 fixed point (ITU-R BT.601 / BT.709 coefficients).
 */
#include "video/vic_convert.h"

#include "gpu/block_linear.h"

#define FIX_SHIFT 16
#define FIX_ONE (1 << FIX_SHIFT)
#define FIX_HALF (1 << (FIX_SHIFT - 1))
#define LUMA_OFFSET 16
#define CHROMA_OFFSET 128
#define BYTES_PER_PIXEL 4u

/* Coefficients x 65536: Y scale 255/219; Cr->R, Cb->G, Cr->G, Cb->B. */
typedef struct Coefficients {
  int32_t y, cr_r, cb_g, cr_g, cb_b;
} Coefficients;

static const Coefficients k_coefficients[2] = {
    /* BT.601 */ {76309, 104597, -25675, -53279, 132201},
    /* BT.709 */ {76309, 117489, -13975, -34925, 138438},
};

static uint8_t clamp8(int32_t v) {
  v = (v + FIX_HALF) >> FIX_SHIFT;
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

uint64_t vic_target_bytes(const Vic_Target *t) {
  if (t->block_linear) return block_linear_size(t->width * BYTES_PER_PIXEL, t->height, t->block_height_log2);
  return (uint64_t)t->pitch * t->height;
}

void vic_convert_nv12(const Vic_Nv12 *s, const Vic_Target *t, Vic_Matrix matrix) {
  const Coefficients *k = &k_coefficients[matrix == VIC_MATRIX_BT709 ? 1 : 0];
  if (!s->width || !s->height || !t->width || !t->height) return;
  const uint32_t step_x = (uint32_t)(((uint64_t)s->width << FIX_SHIFT) / t->width);
  const uint32_t step_y = (uint32_t)(((uint64_t)s->height << FIX_SHIFT) / t->height);
  for (uint32_t y = 0; y < t->height; y++) {
    const uint32_t sy = (uint32_t)(((uint64_t)y * step_y) >> FIX_SHIFT);
    const uint8_t *luma_row = s->luma + (uint64_t)sy * s->pitch;
    const uint8_t *chroma_row = s->chroma + (uint64_t)(sy >> 1) * s->pitch;
    for (uint32_t x = 0; x < t->width; x++) {
      const uint32_t sx = (uint32_t)(((uint64_t)x * step_x) >> FIX_SHIFT);
      const int32_t luma = ((int32_t)luma_row[sx] - LUMA_OFFSET) * k->y;
      const int32_t cb = (int32_t)chroma_row[sx & ~1u] - CHROMA_OFFSET;
      const int32_t cr = (int32_t)chroma_row[(sx & ~1u) + 1u] - CHROMA_OFFSET;
      const uint8_t r = clamp8(luma + cr * k->cr_r);
      const uint8_t g = clamp8(luma + cb * k->cb_g + cr * k->cr_g);
      const uint8_t b = clamp8(luma + cb * k->cb_b);
      const uint64_t at = t->block_linear
                              ? block_linear_offset(x * BYTES_PER_PIXEL, y, t->width * BYTES_PER_PIXEL,
                                                    t->block_height_log2)
                              : (uint64_t)y * t->pitch + (uint64_t)x * BYTES_PER_PIXEL;
      uint8_t *p = t->pixels + at;
      p[0] = t->bgra ? b : r;
      p[1] = g;
      p[2] = t->bgra ? r : b;
      p[3] = t->alpha;
    }
  }
}
