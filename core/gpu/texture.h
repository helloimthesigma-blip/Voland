/**
 * Maxwell texture headers (TIC), samplers (TSC) and texel decoding
 * (§13), for the reference rasterizer. Layouts per NVIDIA's open-gpu-doc
 * clb197tex.h (TEXHEAD_BL / TEXHEAD_PITCH / TEXSAMP0-7); the code is
 * Voland's own.
 *
 * An image is decoded from guest memory once (block-linear -> linear,
 * BCn -> RGBA8) into a Tex_Image, then sampled any number of times.
 * Mipmaps: level 0 only (the reference path has no derivatives - see
 * maxwell_shader.h); explicit LODs clamp to level 0.
 */
#ifndef SWITCH_GPU_TEXTURE_H
#define SWITCH_GPU_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#define TEX_HEADER_BYTES 32u
#define TEX_SAMPLER_BYTES 32u

typedef enum Tex_Type {
  TEX_TYPE_1D = 0,
  TEX_TYPE_2D = 1,
  TEX_TYPE_3D = 2,
  TEX_TYPE_CUBE = 3,
  TEX_TYPE_1D_ARRAY = 4,
  TEX_TYPE_2D_ARRAY = 5,
  TEX_TYPE_1D_BUFFER = 6,
  TEX_TYPE_2D_NO_MIPMAP = 7,
  TEX_TYPE_CUBE_ARRAY = 8,
} Tex_Type;

typedef enum Tex_Layout {
  TEX_LAYOUT_BUFFER = 0,
  TEX_LAYOUT_PITCH = 2,
  TEX_LAYOUT_BLOCK_LINEAR = 3,
} Tex_Layout;

/* TEXHEAD component data types. */
#define TEX_DATA_SNORM 1u
#define TEX_DATA_UNORM 2u
#define TEX_DATA_SINT 3u
#define TEX_DATA_UINT 4u
#define TEX_DATA_SNORM_FORCE_FP16 5u
#define TEX_DATA_UNORM_FORCE_FP16 6u
#define TEX_DATA_FLOAT 7u

/* Swizzle sources. */
#define TEX_SOURCE_ZERO 0u
#define TEX_SOURCE_R 2u
#define TEX_SOURCE_G 3u
#define TEX_SOURCE_B 4u
#define TEX_SOURCE_A 5u
#define TEX_SOURCE_ONE_INT 6u
#define TEX_SOURCE_ONE_FLOAT 7u

typedef struct Tex_Header {
  uint32_t words[8];
  uint32_t format;          /* COMPONENTS (TEXHEAD0_COMPONENT_SIZES values) */
  uint8_t data_type[4];     /* R, G, B, A */
  uint8_t swizzle[4];       /* X, Y, Z, W sources */
  Tex_Layout layout;
  uint64_t address;
  uint32_t pitch;           /* PITCH layout */
  uint32_t block_height_log2;
  uint32_t block_depth_log2;
  uint32_t width, height, depth; /* depth: layers for arrays, slices for 3D */
  uint32_t levels;
  Tex_Type type;
  bool srgb;
  bool normalized;
} Tex_Header;

typedef struct Tex_Sampler {
  uint8_t wrap[3];
  bool depth_compare;
  uint8_t compare_func;     /* 0 NEVER .. 7 ALWAYS */
  uint8_t mag_filter;       /* 1 point, 2 linear */
  uint8_t min_filter;
  bool srgb_border;
  float border[4];
} Tex_Sampler;

/* A decoded level-0 image: linear rows of `bytes_per_texel` elements
 * (BCn images are expanded to A8B8G8R8). */
typedef struct Tex_Image {
  Tex_Header header;
  uint32_t format;          /* the format the texels are stored in now */
  uint32_t bytes_per_texel;
  uint32_t width, height, layers;
  uint32_t row_bytes;
  uint64_t layer_bytes;
  const uint8_t *texels;
  bool rgba8;               /* texels are 4-byte R,G,B,A UNORM (fast path) */
  bool valid;
} Tex_Image;

void tex_header_parse(const uint32_t words[8], Tex_Header *out);
void tex_sampler_parse(const uint32_t words[8], Tex_Sampler *out);

/* Builds the sampling lookup tables. Idempotent; call once before
 * sampling from several threads (raster3d_init does). */
void tex_init_tables(void);

/* Bytes per element and block size (1x1, or 4x4 for BCn). Returns false
 * for unsupported formats. */
bool tex_format_info(uint32_t format, uint32_t *bytes_per_element, uint32_t *block_width, uint32_t *block_height);

/* Size in guest memory of level 0 of one layer, and the layer stride
 * (the whole mip chain, block aligned). */
uint64_t tex_level0_bytes(const Tex_Header *header);
uint64_t tex_layer_stride(const Tex_Header *header);

/* Guest-memory size the decoder must read (all layers' level 0). */
uint64_t tex_read_bytes(const Tex_Header *header);

/* Decodes `raw` (tex_read_bytes of guest memory at header->address) into
 * `dst` (tex_decoded_bytes long). */
uint64_t tex_decoded_bytes(const Tex_Header *header);
bool tex_decode(const Tex_Header *header, const uint8_t *raw, uint8_t *dst, Tex_Image *out);

/* Sampling. Results follow the header swizzle; integer formats return
 * raw integers, everything else floats (as bits). */
/* Many plain samples of one texture (layer 0, no offset, no compare):
 * out[i] = tex_sample(image, sampler, {u[i], v[i], 0}, 0, ...), with the
 * per-image decisions made once - the shader's texture instructions,
 * a lane at a time. */
void tex_sample_batch(const Tex_Image *image, const Tex_Sampler *sampler, const float *u, const float *v, uint32_t count,
                      uint32_t (*out)[4]);
void tex_sample(const Tex_Image *image, const Tex_Sampler *sampler, const float coords[3], float layer,
                float dref, bool shadow, const int32_t offset[3], uint32_t out[4]);
void tex_gather(const Tex_Image *image, const Tex_Sampler *sampler, const float coords[3], float layer,
                uint32_t component, float dref, bool shadow, const int32_t offset[3], uint32_t out[4]);
void tex_fetch(const Tex_Image *image, const int32_t coords[3], int32_t layer, uint32_t out[4]);

/* The texel at (x, y, layer) of a valid image as sampling sees it before
 * the swizzle: floats (as bits) or raw integers (the WebGPU renderer's
 * upload of formats it has no direct equivalent for). */
void tex_texel(const Tex_Image *image, uint32_t x, uint32_t y, uint32_t layer, uint32_t out[4]);

/* One texel, unswizzled, as floats (or integer bits): R, G, B, A. */
void tex_decode_texel(uint32_t format, const uint8_t data_type[4], bool srgb, const uint8_t *texel, uint32_t out[4]);

#endif /* SWITCH_GPU_TEXTURE_H */
