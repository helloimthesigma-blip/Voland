/**
 * ASTC LDR block decoding (§13 textures), per the Khronos Data Format
 * Specification ("ASTC Compressed Texture Image Formats"): 2D blocks of
 * any footprint up to 12x12, all LDR colour endpoint modes, 1-4
 * partitions, dual-plane weights and void-extent blocks. HDR endpoint
 * modes and HDR void-extent blocks decode to the error colour (magenta),
 * as an LDR-profile decoder must.
 */
#ifndef SWITCH_GPU_ASTC_H
#define SWITCH_GPU_ASTC_H

#include <stdbool.h>
#include <stdint.h>

#define ASTC_BLOCK_BYTES 16u
#define ASTC_MAX_FOOTPRINT 12u

/* Decodes one 128-bit block of a `block_width` x `block_height`
 * footprint into RGBA8 texels, row-major (`out` holds block_width *
 * block_height entries). `srgb` selects the sRGB endpoint expansion; the
 * result is still the stored (non-linear) value. Returns false for an
 * illegal block (the texels are then the error colour). */
bool astc_decode_block(const uint8_t block[ASTC_BLOCK_BYTES], uint32_t block_width, uint32_t block_height, bool srgb,
                       uint8_t out[][4]);

#endif /* SWITCH_GPU_ASTC_H */
