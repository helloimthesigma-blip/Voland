/**
 * BC1 / BC3 block encoding (§13 textures): for devices that sample BCn
 * but not ASTC (most desktop GPUs), decoded ASTC textures are re-encoded
 * so they keep a compressed size on the GPU - RGBA8 copies of Tears of
 * the Kingdom's ASTC set passed the texture budget every frame.
 *
 * A range fit: the block's colour bounding box, its diagonal oriented by
 * the channels' covariance and inset slightly, quantised to RGB565; each
 * texel takes the nearest of the four palette colours (BC1's opaque
 * mode). BC3 adds an alpha block of the extremes and the nearest of its
 * eight values.
 */
#ifndef SWITCH_GPU_BC_ENCODE_H
#define SWITCH_GPU_BC_ENCODE_H

#include <stdint.h>

#define BC_BLOCK_DIM 4u
#define BC_BLOCK_TEXELS 16u
#define BC1_BLOCK_BYTES 8u
#define BC3_BLOCK_BYTES 16u

/* `rgba`: the block's texels, row-major, RGBA8. Alpha is ignored. */
void bc1_encode_block(const uint8_t rgba[BC_BLOCK_TEXELS][4], uint8_t out[BC1_BLOCK_BYTES]);

/* The alpha block (8 bytes), then the colour block (8 bytes). */
void bc3_encode_block(const uint8_t rgba[BC_BLOCK_TEXELS][4], uint8_t out[BC3_BLOCK_BYTES]);

#endif /* SWITCH_GPU_BC_ENCODE_H */
