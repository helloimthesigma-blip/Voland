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
 *   - textures are bound per draw: each texture instruction samples the
 *     binding the caller resolved for it (wgsl_program_desc), level 0 as
 *     the reference renderer does.
 *
 * Bindings (group 0): 0 = draw constants (array<u32>: surface height as
 * f32 bits, ...); 1 + slot = constant buffer `slot` (array<u32>);
 * WGSL_TEXTURE_BINDING_BASE + 2i / + 2i + 1 = texture i and its sampler.
 */
#ifndef SWITCH_GPU_WGSL_H
#define SWITCH_GPU_WGSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/maxwell_shader.h"

#define WGSL_MAX_TEXTURES 16u
#define WGSL_TEXTURE_BINDING_BASE 32u
#define WGSL_NO_BINDING 0xffu
#define WGSL_MAX_TARGETS 8u
#define WGSL_DRAW_CONSTANTS_BINDING 0u
#define WGSL_DRAW_SURFACE_HEIGHT 0u /* draw-constant words */
#define WGSL_DRAW_CONSTANT_WORDS 4u

typedef enum Wgsl_Texture_Kind {
  WGSL_TEX_2D,
  WGSL_TEX_2D_ARRAY,
  WGSL_TEX_CUBE,
  WGSL_TEX_3D,
} Wgsl_Texture_Kind;

typedef enum Wgsl_Sample_Type {
  WGSL_SAMPLE_FLOAT,
  WGSL_SAMPLE_UINT,
  WGSL_SAMPLE_SINT,
} Wgsl_Sample_Type;

typedef struct Wgsl_Texture {
  uint8_t kind;        /* Wgsl_Texture_Kind */
  uint8_t sample_type; /* Wgsl_Sample_Type */
  bool normalized;     /* coordinates in [0, 1] (else texels) */
  bool linear;         /* the sampler filters (depth compare blends four taps) */
  uint8_t compare;     /* depth compare: 0 none, else the TSC function + 1 */
  uint8_t swizzle[4];  /* TEX_SOURCE_* per output component */
} Wgsl_Texture;

/* Everything the WGSL depends on beyond the program itself; equal
 * descriptors give equal text (the caller caches by it). */
typedef struct Wgsl_Program_Desc {
  /* Texture instruction at word w samples binding binding_of[w]
   * (WGSL_NO_BINDING: reads 0, 0, 0, 1). */
  uint8_t binding_of[SM_MAX_WORDS];
  uint32_t texture_count;
  Wgsl_Texture textures[WGSL_MAX_TEXTURES];
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
  bool lower_left;        /* position y counts from the bottom */
} Wgsl_Program_Desc;

typedef struct Wgsl_Result {
  char *text;          /* NUL-terminated; in the caller's buffer */
  size_t length;
  bool ok;             /* false: the program uses something not translated */
  char reason[96];     /* why not, when !ok */
  uint32_t cbuf_slots; /* bit per constant buffer the WGSL binds */
} Wgsl_Result;

/* Translates `program` (a pixel program) for `desc` into `buffer`. */
Wgsl_Result wgsl_translate(const Sm_Program *program, const Wgsl_Program_Desc *desc, char *buffer, size_t capacity);

/* FNV-1a over the descriptor's meaningful bytes (the WGSL cache key, with
 * the program's hash). */
uint64_t wgsl_desc_hash(const Wgsl_Program_Desc *desc, const Sm_Program *program);

#endif /* SWITCH_GPU_WGSL_H */
