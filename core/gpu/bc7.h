/**
 * BC7 (BPTC UNORM) block decoding (§13 textures), from the Khronos Data
 * Format Specification 1.3 §"BC7": eight modes of 1-3 subsets, partition
 * and anchor tables (bc7_tables.inc, extracted from that specification),
 * endpoint p-bits, 2/3/4-bit interpolation, and the mode 4/5 channel
 * rotation and index selection. Titles use it for UI and character art
 * (Super Smash Bros. Ultimate's title screen); it used to decode as
 * magenta.
 */
#ifndef SWITCH_GPU_BC7_H
#define SWITCH_GPU_BC7_H

#include <stdint.h>

#define BC7_BLOCK_BYTES 16u
#define BC7_TEXELS 16u

/* Decodes one 4x4 block into RGBA8 texels in row-major order. A block
 * with no valid mode decodes to transparent black, as the spec requires. */
void bc7_decode_block(const uint8_t block[BC7_BLOCK_BYTES], uint8_t out[BC7_TEXELS][4]);

#endif /* SWITCH_GPU_BC7_H */
