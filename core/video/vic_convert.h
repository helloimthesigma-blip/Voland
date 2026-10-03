/**
 * VIC's colour conversion (DESIGN §13 "VIC HLE"): a decoded NV12 picture
 * to the RGBA8 surface the guest samples. Host memory in, host memory
 * out; the caller moves the result into guest memory through vmm.
 */
#ifndef SWITCH_VIDEO_VIC_CONVERT_H
#define SWITCH_VIDEO_VIC_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum Vic_Matrix {
  VIC_MATRIX_BT601 = 0,
  VIC_MATRIX_BT709 = 1,
} Vic_Matrix;

typedef struct Vic_Nv12 {
  const uint8_t *luma;
  const uint8_t *chroma; /* interleaved Cb,Cr at half resolution */
  uint32_t width, height, pitch;
} Vic_Nv12;

typedef struct Vic_Target {
  uint8_t *pixels;
  uint32_t width, height;   /* pixels */
  uint32_t pitch;           /* bytes per row (pitch-linear) */
  bool block_linear;        /* else pitch-linear */
  uint32_t block_height_log2;
  bool bgra;                /* B,G,R,A byte order instead of R,G,B,A */
  uint8_t alpha;
} Vic_Target;

/* Bytes the target occupies. */
uint64_t vic_target_bytes(const Vic_Target *target);

/* Converts (limited-range YCbCr) and scales nearest-neighbour from the
 * whole source to the whole target. */
void vic_convert_nv12(const Vic_Nv12 *source, const Vic_Target *target, Vic_Matrix matrix);

#endif /* SWITCH_VIDEO_VIC_CONVERT_H */
