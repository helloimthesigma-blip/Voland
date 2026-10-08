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
/* Vertex programs on the GPU (WGSL_STAGE vertex): their own constant
 * buffer table, then the viewport the CPU path applies in to_screen. */
#define WGSL_DRAW_VS_CBUF_TABLE (WGSL_DRAW_CBUF_TABLE + 2u * SM_CBUF_SLOTS)
#define WGSL_DRAW_VIEWPORT (WGSL_DRAW_VS_CBUF_TABLE + 2u * SM_CBUF_SLOTS)
#define WGSL_VP_SCALE 0u      /* f32 x, y, z */
#define WGSL_VP_OFFSET 3u     /* f32 x, y, z */
#define WGSL_VP_TARGET 6u     /* f32 target width, height (the NDC mapping) */
#define WGSL_VP_FLAGS 8u      /* WGSL_VP_TRANSFORM */
#define WGSL_VP_WORDS 10u
#define WGSL_VP_TRANSFORM 1u  /* the viewport transform is enabled */
/* Vertex pulling (Wgsl_Program_Desc.vertex_pull): per vertex input location
 * l, words WGSL_DRAW_VS_INPUTS + 4 l: the byte offset in this buffer of its
 * stream's first copied element, the stride (bits 0-11) | WGSL_VSI_ACTIVE |
 * WGSL_VSI_INSTANCED, the guest's VERTEX_ATTRIB word (offset, size, type,
 * BGRA swap), and the vertex id of the first copied element. Then the
 * instance id. */
#define WGSL_DRAW_VS_INPUTS (WGSL_DRAW_VIEWPORT + WGSL_VP_WORDS)
#define WGSL_VSI_WORDS 4u
#define WGSL_VSI_MAX 16u
#define WGSL_VSI_STRIDE_MASK 0xfffu
#define WGSL_VSI_ACTIVE 0x40000000u    /* else the input reads (0, 0, 0, 1) */
#define WGSL_VSI_INSTANCED 0x80000000u /* one element (the draw's instance) is copied */
#define WGSL_VSI_RESIDENT 0x20000000u  /* the stream is read from R (a GPU mirror), at word 0's byte offset */
#define WGSL_DRAW_VS_INSTANCE (WGSL_DRAW_VS_INPUTS + WGSL_VSI_WORDS * WGSL_VSI_MAX)
/* Global memory a vertex program reads (wgsl_find_globals): per buffer k,
 * words WGSL_DRAW_GLOBALS + 4 k hold its base address (low, high), its size
 * and the byte offset in this buffer where its bytes were copied. */
#define WGSL_MAX_GLOBALS 4u
#define WGSL_GLOBAL_WORDS 4u
#define WGSL_DRAW_GLOBALS (WGSL_DRAW_VS_INSTANCE + 2u)
#define WGSL_DRAW_CONSTANT_WORDS (WGSL_DRAW_GLOBALS + WGSL_GLOBAL_WORDS * WGSL_MAX_GLOBALS)
/* Compute programs (desc->stage SM_STAGE_COMPUTE): global memory is a few
 * windows of GPU virtual memory, window k bound at
 * WGSL_CS_WINDOW_BINDING_BASE + k as a read-write array<u32>. Words
 * WGSL_CS_WINDOWS + 4 k hold its base address (low, high) and size in
 * bytes; an LDG/STG address inside no window reads 0 / writes nothing (the
 * interpreter's unmapped memory). The constant-buffer table is
 * WGSL_DRAW_CBUF_TABLE's. */
#define WGSL_CS_MAX_WINDOWS 6u /* with the data buffer, WebGPU's 8 storage buffers per stage minus one */
#define WGSL_CS_WINDOW_WORDS 4u
#define WGSL_CS_WINDOWS WGSL_DRAW_VS_INPUTS
#define WGSL_CS_WINDOW_BINDING_BASE (WGSL_SAMPLER_BINDING_BASE + WGSL_MAX_TEXTURES)
/* A pulled vertex program's second source: a GPU mirror holding vertex
 * streams a compute dispatch wrote (WGSL_VSI_RESIDENT inputs read it). */
#define WGSL_VS_RESIDENT_BINDING (WGSL_CS_WINDOW_BINDING_BASE + WGSL_CS_MAX_WINDOWS)
#define WGSL_CS_MAX_INVOCATIONS 256u   /* WebGPU's default maxComputeInvocationsPerWorkgroup */
#define WGSL_CS_MAX_SHARED_BYTES 16384u /* its default maxComputeWorkgroupStorageSize */
#define WGSL_VERTEX_ID_LOCATION 0u /* vertex input: vec4<u32>(vertex id, instance id, 0, 0) */
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
#define WGSL_TEXP_3D 32u           /* a 3D texture: its slices are layers, r picks and blends them */

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
  /* The stage: SM_STAGE_PIXEL, or SM_STAGE_VERTEX for a vertex program run
   * on the GPU. A vertex program's outputs must match its pixel program's
   * inputs: varying_count and flat_mask are the pixel program's, and
   * output_word[l][c] is the attribute word (SM_ATTRIBUTE_WORDS index)
   * feeding location l component c (0xffff: 0), divided by w where
   * perspective_mask bit l * 4 + c is set (as to_screen does). Generic
   * input vector v arrives at vertex input input_location[v] (0xff: not
   * supplied; reads 0); input 0 is the vertex and instance id. */
  uint8_t stage;
  /* Vertex programs: the inputs are decoded in WGSL from raw vertex
   * buffer bytes in the data buffer (WGSL_DRAW_VS_INPUTS), with the vertex
   * id from @builtin(vertex_index); else they arrive as vertex attributes. */
  bool vertex_pull;
  uint8_t input_location[SM_ATTR_GENERIC_COUNT];
  uint32_t input_count;
  uint16_t output_word[WGSL_MAX_VARYINGS][4];
  uint64_t perspective_mask;
  /* Compute programs: the block (workgroup) size, the block's shared
   * memory, and how many global-memory windows are bound. */
  uint16_t block[3];
  uint32_t shared_bytes;
  uint32_t window_count;
} Wgsl_Program_Desc;

/* The storage buffers a vertex program's LDGs read: each LDG's 64-bit
 * address is a buffer base from a constant buffer plus an offset -
 * IADD Rlo = Rx + c[slot][offset] (carry out), IADD.X Rhi = RZ +
 * c[slot][offset + 4] - as NVN's storage buffers compile; the buffer's
 * size is the word after (c[slot][offset + 8]). buffer_of[pc] is each
 * LDG's buffer (0xff: not an LDG). False when an LDG does not follow the
 * pattern, or the program writes global memory. */
typedef struct Wgsl_Globals {
  uint32_t count;
  uint8_t slot[WGSL_MAX_GLOBALS];
  uint16_t offset[WGSL_MAX_GLOBALS];
  uint8_t buffer_of[SM_MAX_WORDS];
} Wgsl_Globals;
bool wgsl_find_globals(const Sm_Program *program, Wgsl_Globals *out);
bool wgsl_reads_globals(const Sm_Program *program);

/* The vertex-pulling decoder's WGSL (vword, vconv, vfetch over a global
 * `D: array<u32>`), as pulled vertex programs include it - for tests. */
const char *wgsl_vertex_pull_source(void);

typedef struct Wgsl_Result {
  char *text;          /* NUL-terminated; in the caller's buffer */
  size_t length;
  bool ok;             /* false: the program uses something not translated */
  char reason[96];     /* why not, when !ok */
  uint32_t cbuf_slots; /* bit per constant buffer the WGSL reads */
} Wgsl_Result;

/* Translates `program` (a pixel program, or a vertex program when
 * desc->stage is SM_STAGE_VERTEX) for `desc` into `buffer`. A vertex
 * module's entry point is `vs`: its VOut is the pixel module's input, its
 * clip position reproduces to_screen's window coordinates once divided
 * by w, and its varyings are what gpu_put_vertex would have sent. */
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
