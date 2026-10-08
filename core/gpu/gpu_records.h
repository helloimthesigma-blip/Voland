/**
 * GPU stream record types and payloads (gpu_stream.h carries them).
 * Mirrored by platform/web/workers/gpu-records.ts; docs/GPU_COMMAND_STREAM.md
 * describes them. All fields u32 little-endian unless noted; ids are the
 * producer's (0 = none). Bump GPU_STREAM_VERSION on any change.
 *
 * The producer (raster3d's GPU mode) does vertex work, assembly, clipping
 * and the viewport transform on the CPU and sends screen-space triangles:
 * each vertex is {x, y in WebGPU NDC, z in [0, 1], 1/w} as f32, then the
 * pixel program's varyings (4 x u32 each, already divided by w where the
 * program interpolates them perspective-correct - so every varying is
 * interpolated screen-linearly, flat ones not at all). Pixel programs
 * arrive as WGSL (gpu/wgsl.h) with a matching pass-through vertex stage.
 */
#ifndef SWITCH_GPU_GPU_RECORDS_H
#define SWITCH_GPU_GPU_RECORDS_H

#include <stdint.h>

enum {
  GPU_REC_TEXTURE_CREATE = 1, /* Gpu_Rec_Texture_Create */
  GPU_REC_TEXTURE_DESTROY,    /* u32 id */
  GPU_REC_TEXTURE_WRITE,      /* Gpu_Rec_Texture_Write + rows */
  GPU_REC_SHADER,             /* u32 id, u32 bytes, WGSL text */
  GPU_REC_CLEAR,              /* Gpu_Rec_Clear */
  GPU_REC_DRAW,               /* Gpu_Rec_Draw + bindings + vertices */
  GPU_REC_COPY,               /* Gpu_Rec_Copy */
  GPU_REC_PRESENT,            /* Gpu_Rec_Present */
};

/* Texture formats (the WebGPU format each maps to, in gpu-records.ts). */
enum {
  GPU_FMT_NONE = 0,
  GPU_FMT_RGBA8_UNORM,
  GPU_FMT_RGBA8_SRGB,
  GPU_FMT_BGRA8_UNORM,
  GPU_FMT_RGBA16_FLOAT,
  GPU_FMT_RGBA32_FLOAT,
  GPU_FMT_R8_UNORM,
  GPU_FMT_RG8_UNORM,
  GPU_FMT_R16_FLOAT,
  GPU_FMT_RG16_FLOAT,
  GPU_FMT_R32_FLOAT,
  GPU_FMT_RG32_FLOAT,
  GPU_FMT_RG11B10_UFLOAT,
  GPU_FMT_RGB10A2_UNORM,
  GPU_FMT_R32_UINT,
  GPU_FMT_RG32_UINT,
  GPU_FMT_RGBA32_UINT,
  GPU_FMT_RGBA16_UINT,
  GPU_FMT_RGBA8_UINT,
  GPU_FMT_DEPTH16,
  GPU_FMT_DEPTH24_STENCIL8,
  GPU_FMT_DEPTH32F,
  GPU_FMT_DEPTH32F_STENCIL8,
  GPU_FMT_BGRA8_SRGB,
  GPU_FMT_STENCIL8,
  GPU_FMT_RGBA16_SINT,
  GPU_FMT_RGBA32_SINT,
  GPU_FMT_R32_SINT,
  GPU_FMT_COUNT,
};

#define GPU_USAGE_SAMPLED 1u
#define GPU_USAGE_RENDER 2u /* render attachment (also copy source / destination) */

/* levels: mip levels (1 = none). The producer uploads level 0; the
 * consumer builds the rest from it (a box filter) after each upload
 * (version 2; a version-1 record ends after `usage`, one level). */
typedef struct Gpu_Rec_Texture_Create {
  uint32_t id, format, width, height, layers, usage, levels, reserved;
} Gpu_Rec_Texture_Create;

typedef struct Gpu_Rec_Texture_Write {
  uint32_t id, x, y, width, height, layer, bytes_per_row, data_bytes; /* then data_bytes of rows */
} Gpu_Rec_Texture_Write;

#define GPU_CLEAR_COLOR 1u
#define GPU_CLEAR_DEPTH 2u
#define GPU_CLEAR_STENCIL 4u

typedef struct Gpu_Rec_Clear {
  uint32_t color_id;     /* 0: no colour target */
  uint32_t color_mask;   /* RGBA write bits */
  uint32_t color[4];     /* f32 bits (or raw integers for integer formats) */
  uint32_t depth_id;     /* 0: no depth target */
  uint32_t flags;        /* GPU_CLEAR_* */
  uint32_t depth;        /* f32 bits */
  uint32_t stencil;
  uint32_t stencil_mask; /* bits of the stencil value written */
  int32_t rect[4];       /* x, y, width, height */
} Gpu_Rec_Clear;

/* Blend factors / ops / compare functions: WebGPU's, by index into the
 * tables in gpu-records.ts. */
enum {
  GPU_BF_ZERO, GPU_BF_ONE, GPU_BF_SRC, GPU_BF_ONE_MINUS_SRC, GPU_BF_SRC_ALPHA, GPU_BF_ONE_MINUS_SRC_ALPHA,
  GPU_BF_DST, GPU_BF_ONE_MINUS_DST, GPU_BF_DST_ALPHA, GPU_BF_ONE_MINUS_DST_ALPHA, GPU_BF_SRC_ALPHA_SATURATED,
  GPU_BF_CONSTANT, GPU_BF_ONE_MINUS_CONSTANT,
};
enum { GPU_BOP_ADD, GPU_BOP_SUBTRACT, GPU_BOP_REVERSE_SUBTRACT, GPU_BOP_MIN, GPU_BOP_MAX };
enum {
  GPU_CMP_NEVER, GPU_CMP_LESS, GPU_CMP_EQUAL, GPU_CMP_LESS_EQUAL, GPU_CMP_GREATER, GPU_CMP_NOT_EQUAL,
  GPU_CMP_GREATER_EQUAL, GPU_CMP_ALWAYS,
};
enum {
  GPU_SOP_KEEP, GPU_SOP_ZERO, GPU_SOP_REPLACE, GPU_SOP_INVERT, GPU_SOP_INCREMENT_CLAMP, GPU_SOP_DECREMENT_CLAMP,
  GPU_SOP_INCREMENT_WRAP, GPU_SOP_DECREMENT_WRAP,
};

#define GPU_MAX_TARGETS 8u

typedef struct Gpu_Rec_Target {
  uint32_t id;
  uint32_t write_mask;   /* RGBA bits */
  uint32_t blend;        /* 0 / 1 */
  uint32_t color_op, color_src, color_dst, alpha_op, alpha_src, alpha_dst;
} Gpu_Rec_Target;

typedef struct Gpu_Rec_Stencil_Face {
  uint32_t fail, depth_fail, pass, compare;
} Gpu_Rec_Stencil_Face;

#define GPU_VARYING_MAX 16u

typedef struct Gpu_Rec_Draw {
  uint32_t shader_id;
  uint32_t target_count;
  Gpu_Rec_Target targets[GPU_MAX_TARGETS];
  uint32_t depth_id;
  uint32_t depth_test, depth_write, depth_compare;
  uint32_t stencil;      /* 0 / 1 */
  Gpu_Rec_Stencil_Face stencil_front, stencil_back;
  uint32_t stencil_read_mask, stencil_write_mask, stencil_ref;
  uint32_t blend_constant[4]; /* f32 bits */
  int32_t scissor[4];         /* x, y, width, height */
  uint32_t varying_count;     /* vec4 varyings after the position and 1/w */
  uint32_t flat_mask;         /* bit per varying: not interpolated (u32) */
  uint32_t binding_count;     /* Gpu_Rec_Binding entries follow */
  uint32_t vertex_count;      /* vertices (a triangle list) follow the bindings */
  /* Version 3: a GPU vertex stage. vs_shader_id 0: vertices are already
   * transformed (GPU_VERTEX_HEADER_WORDS + varyings each; cullMode none).
   * Otherwise each vertex is (1 + vertex_input_count) x vec4<u32> - the
   * vertex and instance id, then the program's input vectors - run through
   * shader vs_shader_id's `vs`, with cull_mode (GPU_CULL_*) and front_face
   * (GPU_FRONT_*) for the pipeline. */
  uint32_t vs_shader_id;
  uint32_t vertex_input_count;
  uint32_t cull_mode;
  uint32_t front_face;
  /* Version 4: with a vertex stage, index_count > 0 means the vertex_count
   * vertices are each draw's distinct ones and index_count u32 indices into
   * them (a triangle list, each triangle's provoking vertex first) follow
   * the vertices. */
  uint32_t index_count;
  /* Version 5: GPU_DRAW_VERTEX_PULL - no vertices; the index_count indices
   * are guest vertex ids (the shader's vertex_index) and the vertex stage
   * decodes its inputs from vertex buffer bytes in the data binding
   * (gpu/wgsl.h WGSL_DRAW_VS_INPUTS). */
  uint32_t flags;
} Gpu_Rec_Draw;

#define GPU_DRAW_VERTEX_PULL 1u

#define GPU_CULL_NONE 0u
#define GPU_CULL_FRONT 1u
#define GPU_CULL_BACK 2u
#define GPU_FRONT_CCW 0u
#define GPU_FRONT_CW 1u

#define GPU_BIND_DATA 1u    /* read-only storage buffer: Gpu_Rec_Binding, then `bytes` of data (a multiple of 8) */
#define GPU_BIND_TEXTURE 2u /* texture_2d_array (gpu/wgsl.h); `bytes` = GPU_BIND_FILTERED when a sampler filters it */
#define GPU_BIND_SAMPLER 3u /* filtering sampler; `texture_id` = GPU_SAMPLER_* state */
#define GPU_BIND_FILTERED 1u
/* Sampler state: bit 0 magnification linear (else nearest), then 2 bits per
 * axis u, v, w: 0 repeat, 1 mirror-repeat, 2 clamp-to-edge; bit 7
 * minification linear, bit 8 linear between mip levels (version 2). */
#define GPU_SAMPLER_LINEAR 1u
#define GPU_SAMPLER_WRAP_SHIFT(axis) (1u + 2u * (axis))
#define GPU_SAMPLER_MIN_LINEAR (1u << 7)
#define GPU_SAMPLER_MIP_LINEAR (1u << 8)

typedef struct Gpu_Rec_Binding {
  uint32_t kind;
  uint32_t binding;
  uint32_t bytes;        /* DATA: data bytes that follow; TEXTURE: GPU_BIND_FILTERED or 0 */
  uint32_t texture_id;   /* TEXTURE: the texture; SAMPLER: its state */
} Gpu_Rec_Binding;

/* Vertex: x, y (WebGPU NDC), z, 1/w (f32), then varying_count x 4 u32.
 * Front-facing triangles wind counter-clockwise in NDC (y up), back-facing
 * ones clockwise: the producer has culled already (cullMode none), the
 * winding only feeds @builtin(front_facing). */
#define GPU_VERTEX_HEADER_WORDS 4u

typedef struct Gpu_Rec_Copy {
  uint32_t src_id, dst_id;
  int32_t src_rect[4], dst_rect[4]; /* x, y, width, height */
  uint32_t filter;                  /* 0 nearest, 1 linear (when scaling) */
} Gpu_Rec_Copy;

#define GPU_PRESENT_FLIP_X 1u
#define GPU_PRESENT_FLIP_Y 2u

typedef struct Gpu_Rec_Present {
  uint32_t id;
  int32_t rect[4];  /* the source rectangle shown */
  uint32_t flags;   /* GPU_PRESENT_FLIP_* */
} Gpu_Rec_Present;

#endif /* SWITCH_GPU_GPU_RECORDS_H */
