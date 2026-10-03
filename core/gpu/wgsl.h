/**
 * Maxwell pixel programs -> WGSL (§13, the WebGPU renderer).
 *
 * The CPU does everything up to the rasteriser (vertex programs, assembly,
 * clipping, the viewport transform) and sends screen-space triangles whose
 * varyings are already divided by w where the program interpolates them
 * perspective-correct (gpu_records.h); the generated vertex stage passes
 * them through, and the fragment stage is the pixel program translated
 * instruction by instruction with the interpreter's semantics
 * (maxwell_shader.c is the reference):
 *
 *   - registers, predicates and condition codes are function variables;
 *   - straight-line programs translate directly; anything with branches,
 *     SSY/SYNC, PBK/BRK, PCNT/CONT or CAL/RET runs in a pc-dispatch loop
 *     (one invocation is one lane, so the flow stacks are plain arrays);
 *   - IPA reads the screen-linear varyings (as attr_in does); position
 *     x/y come from @builtin(position) (pixel centres), z likewise, w is
 *     the interpolated 1/w varying;
 *   - quad operations (SHFL.BFLY 1/2, FSWZADD) become dpdxFine/dpdyFine on
 *     the quad's pixel parity;
 *   - textures are bound per draw: each texture instruction reads the
 *     binding the caller resolved for it (Wgsl_Program_Desc.binding_of).
 *     Every texture is a texture_2d_array (cube faces and 3D slices are
 *     layers, as the reference decodes them) read with textureLoad; wrap,
 *     border, bilinear filtering, depth compare and the swizzle are done
 *     in the shader from the draw constants, exactly as texture.c does,
 *     so sampler state never changes the WGSL (level 0, as the reference).
 *     Where the descriptor allows (hw_sample_mask), plain samples use a
 *     hardware sampler instead - one tap rather than four loads.
 *
 * Bindings (group 0): 0 = the draw's data (one read-only storage buffer of
 * u32: the draw constants below, then the constant buffers the program
 * reads, wherever the constant-buffer table says - one binding however many
 * slots are used, under WebGPU's 8-storage-buffer default);
 * WGSL_TEXTURE_BINDING_BASE + i = texture i.
 *
 * Draw constants (u32 words):
 *   0 surface height (f32)   1 flags (WGSL_DRAW_LOWER_LEFT)
 *   2 alpha test: 0 off, else 1 + function (0 NEVER .. 7 ALWAYS: alpha OP ref)
 *   3 alpha reference (f32)
 *   WGSL_DRAW_TEXTURE_PARAMS + WGSL_TEX_PARAM_WORDS * i: texture i's sampler
 *   state - flags (WGSL_TEXP_*), wrap (TSC modes, 4 bits each: u, v, p),
 *   swizzle (TEX_SOURCE_* 4 bits each: x, y, z, w), mip levels, compare
 *   function (0 NEVER .. 7 ALWAYS: ref OP texel), LOD bias, min LOD, max LOD
 *   (f32), border (f32 x 4).
 *   WGSL_DRAW_CBUF_TABLE + 2 * slot: constant buffer `slot`'s first word in
 *   this buffer and its size in words (0: unbound, reads 0).
 */
#ifndef SWITCH_GPU_WGSL_H
#define SWITCH_GPU_WGSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/maxwell_shader.h"

#define WGSL_MAX_TEXTURES 16u
#define WGSL_DATA_BINDING 0u
#define WGSL_TEXTURE_BINDING_BASE 1u
#define WGSL_SAMPLER_BINDING_BASE (WGSL_TEXTURE_BINDING_BASE + WGSL_MAX_TEXTURES) /* + i: texture i's sampler */
#define WGSL_NO_BINDING 0xffu
#define WGSL_MAX_TARGETS 8u
#define WGSL_DRAW_SURFACE_HEIGHT 0u /* draw-constant words */
#define WGSL_DRAW_FLAGS 1u
#define WGSL_DRAW_ALPHA_FUNC 2u
#define WGSL_DRAW_ALPHA_REF 3u
#define WGSL_DRAW_TEXTURE_PARAMS 4u
#define WGSL_TEX_PARAM_WORDS 12u
#define WGSL_DRAW_CBUF_TABLE (WGSL_DRAW_TEXTURE_PARAMS + WGSL_TEX_PARAM_WORDS * WGSL_MAX_TEXTURES)
#define WGSL_DRAW_CONSTANT_WORDS (WGSL_DRAW_CBUF_TABLE + 2u * SM_CBUF_SLOTS)
#define WGSL_MAX_VARYINGS 14u /* + 1/w, within WebGPU's 16 inter-stage vectors */
#define WGSL_DRAW_LOWER_LEFT 1u /* position y counts from the bottom */

/* Texture parameter words. */
#define WGSL_TEXP_FLAGS 0u
#define WGSL_TEXP_WRAP 1u
#define WGSL_TEXP_SWIZZLE 2u
#define WGSL_TEXP_LEVELS 3u
#define WGSL_TEXP_COMPARE 4u
#define WGSL_TEXP_LOD_BIAS 5u /* f32: the sampler's bias, in levels */
#define WGSL_TEXP_MIN_LOD 6u  /* f32: the sampler's level clamps (0, 0: level 0 only) */
#define WGSL_TEXP_MAX_LOD 7u
#define WGSL_TEXP_BORDER 8u
#define WGSL_TEXP_SCALE 1u   /* coordinates are normalized (scale by the size) */
#define WGSL_TEXP_LINEAR 2u  /* bilinear (float formats only) */
#define WGSL_TEXP_CUBE 4u    /* cube map: direction -> face layer */
#define WGSL_TEXP_DEPTH_COMPARE 8u /* the sampler compares (shadow instructions) */
#define WGSL_TEXP_MIN_LINEAR 16u   /* bilinear below level 0 (minified, with a mip chain) */

typedef enum Wgsl_Sample_Type {
  WGSL_SAMPLE_FLOAT,
  WGSL_SAMPLE_UINT,
  WGSL_SAMPLE_SINT,
} Wgsl_Sample_Type;

/* Everything the WGSL depends on beyond the program itself; equal
 * descriptors give equal text (the caller caches by it). */
typedef struct Wgsl_Program_Desc {
  /* Texture instruction at word w samples binding binding_of[w]
   * (WGSL_NO_BINDING: reads 0, 0, 0, 1). */
  uint8_t binding_of[SM_MAX_WORDS];
  uint32_t texture_count;
  uint8_t sample_type[WGSL_MAX_TEXTURES]; /* Wgsl_Sample_Type per texture */
  /* Bit per texture: plain samples (no depth compare) go through a
   * hardware sampler at WGSL_SAMPLER_BINDING_BASE + i - the texture is
   * filterable and its wrap modes are WebGPU's (repeat, mirror, clamp to
   * edge), so the result is the reference's (filtering weights aside). */
  uint32_t hw_sample_mask;
  /* Varyings: generic input vector v arrives at location varying_location[v]
   * (0xff: not passed); flat_mask bit per location (not interpolated). */
  uint8_t varying_location[SM_ATTR_GENERIC_COUNT];
  uint32_t varying_count;
  uint32_t flat_mask;
  /* Render targets: target t receives colour (0 = no target), as integers
   * when target_int bit t is set. */
  uint32_t target_count;
  uint32_t target_int_mask;
  uint32_t target_sint_mask;
  bool mrt;               /* each target gets its own colour (else target 0's) */
} Wgsl_Program_Desc;

typedef struct Wgsl_Result {
  char *text;          /* NUL-terminated; in the caller's buffer */
  size_t length;
  bool ok;             /* false: the program uses something not translated */
  char reason[96];     /* why not, when !ok */
  uint32_t cbuf_slots; /* bit per constant buffer the WGSL reads */
} Wgsl_Result;

/* Translates `program` (a pixel program) for `desc` into `buffer`. */
Wgsl_Result wgsl_translate(const Sm_Program *program, const Wgsl_Program_Desc *desc, char *buffer, size_t capacity);

/* A descriptor from the program alone (tests, diagnostics, and the start
 * of a draw's): every generic input vector the SPH reads gets the next
 * location (flat when all its used components are constant-interpolated),
 * each texture instruction its own float binding while they last, and one
 * float target per render target the output map writes. */
void wgsl_default_desc(const Sm_Program *program, Wgsl_Program_Desc *desc);

/* FNV-1a over the descriptor's meaningful bytes (the WGSL cache key, with
 * the program's hash). */
uint64_t wgsl_desc_hash(const Wgsl_Program_Desc *desc, const Sm_Program *program);

#endif /* SWITCH_GPU_WGSL_H */
