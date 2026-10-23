/**
 * VP9 frame headers (DESIGN §13 "Video decode"). NVDEC is given a frame's
 * header fields already parsed and its probability tables already
 * updated; the platform decoder (WebCodecs) wants a plain VP9 frame. This
 * module holds the header model and writes it back: the uncompressed
 * header bit for bit, then a compressed header whose probability updates
 * turn the decoder's current context (`base`) into the frame's tables.
 *
 * Contexts: a VP9 decoder keeps four saved probability contexts. Which one
 * a frame starts from (or the defaults, after a reset) follows from its
 * header; Vp9_Contexts mirrors that bookkeeping so `base` is always what
 * the platform decoder will hold when it reads the frame. The frame's
 * tables come from NVDEC; their difference from `base` is exactly the
 * update the original stream coded, so the rebuilt frame decodes to the
 * same probabilities whatever backward adaptation either side does - with
 * one gap: motion-vector probabilities are coded as absolute values, so
 * with backward adaptation on (frame_parallel off) an update to the value
 * the mirror already holds cannot be told from no update and is dropped
 * (tests/vp9_roundtrip.sh shows it on a libvpx stream).
 *
 * Syntax follows the VP9 bitstream specification (6.2 uncompressed
 * header, 6.3 compressed header).
 */
#ifndef SWITCH_VIDEO_VP9_H
#define SWITCH_VIDEO_VP9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VP9_REFS 3u
#define VP9_REF_SLOTS 8u
#define VP9_CONTEXTS 4u
#define VP9_SEGMENTS 8u
#define VP9_SEG_FEATURES 4u

typedef struct Vp9_Probs {
  uint8_t tx8[2][1], tx16[2][2], tx32[2][3];
  uint8_t coef[4][2][2][6][6][3];
  uint8_t skip[3];
  uint8_t inter_mode[7][3];
  uint8_t interp_filter[4][2];
  uint8_t is_inter[4];
  uint8_t comp_mode[5];
  uint8_t single_ref[5][2];
  uint8_t comp_ref[5];
  uint8_t y_mode[4][9];
  uint8_t uv_mode[10][9];
  uint8_t partition[16][3];
  uint8_t mv_joints[3];
  uint8_t mv_sign[2];
  uint8_t mv_classes[2][10];
  uint8_t mv_class0[2][1];
  uint8_t mv_bits[2][10];
  uint8_t mv_class0_fr[2][2][3];
  uint8_t mv_fr[2][3];
  uint8_t mv_class0_hp[2];
  uint8_t mv_hp[2];
} Vp9_Probs;

enum { VP9_KEY_FRAME = 0, VP9_NON_KEY_FRAME = 1 };
enum { VP9_ONLY_4X4 = 0, VP9_ALLOW_8X8, VP9_ALLOW_16X16, VP9_ALLOW_32X32, VP9_TX_MODE_SELECT };
enum { VP9_SINGLE_REFERENCE = 0, VP9_COMPOUND_REFERENCE, VP9_REFERENCE_MODE_SELECT };
/* Interpolation filters by type (not the header's literal order). */
enum { VP9_EIGHTTAP = 0, VP9_EIGHTTAP_SMOOTH, VP9_EIGHTTAP_SHARP, VP9_BILINEAR, VP9_SWITCHABLE };

typedef struct Vp9_Frame_Header {
  uint32_t profile;
  bool show_existing_frame;
  uint32_t frame_to_show;
  uint32_t frame_type; /* VP9_KEY_FRAME / VP9_NON_KEY_FRAME */
  bool show_frame;
  bool error_resilient;
  bool intra_only;
  uint32_t reset_frame_context;
  /* color_config (key frames, intra-only frames of profile > 0) */
  uint32_t bit_depth;   /* 8, 10, 12 */
  uint32_t color_space;
  bool color_range;
  bool subsampling_x, subsampling_y;
  uint32_t width, height;
  uint32_t render_width, render_height; /* 0: the frame size */
  uint32_t refresh_frame_flags;
  uint32_t ref_frame_idx[VP9_REFS];
  bool ref_sign_bias[VP9_REFS]; /* LAST, GOLDEN, ALTREF */
  /* Inter frames: per reference, whether its size equals this frame's
   * (found_ref). The first match is written. */
  bool size_from_ref[VP9_REFS];
  bool allow_high_precision_mv;
  uint32_t interp_filter; /* VP9_EIGHTTAP... or VP9_SWITCHABLE */
  bool refresh_frame_context;
  bool frame_parallel;
  uint32_t frame_context_idx;
  /* loop filter */
  uint32_t lf_level, lf_sharpness;
  bool lf_delta_enabled, lf_delta_update;
  bool lf_ref_update[4];
  int8_t lf_ref_deltas[4];
  bool lf_mode_update[2];
  int8_t lf_mode_deltas[2];
  /* quantization */
  uint32_t base_q_idx;
  int32_t delta_q_y_dc, delta_q_uv_dc, delta_q_uv_ac;
  /* segmentation */
  bool seg_enabled, seg_update_map, seg_temporal_update, seg_update_data, seg_abs_delta;
  uint8_t seg_tree_probs[7], seg_pred_probs[3];
  bool seg_feature_enabled[VP9_SEGMENTS][VP9_SEG_FEATURES];
  int32_t seg_feature_value[VP9_SEGMENTS][VP9_SEG_FEATURES];
  /* tiles */
  uint32_t tile_cols_log2, tile_rows_log2;
  /* compressed header */
  uint32_t tx_mode;
  uint32_t reference_mode;
} Vp9_Frame_Header;

/* The specification's default probabilities (vp9_defaults.c). */
void vp9_default_probs(Vp9_Probs *p);

bool vp9_frame_is_intra(const Vp9_Frame_Header *h);
bool vp9_lossless(const Vp9_Frame_Header *h);

/* Writes the uncompressed header and the compressed header (probability
 * updates from `base` to `probs`) into `out`. Returns the bytes written
 * (the tile data follows), or 0 if `capacity` is too small. A frame with
 * show_existing_frame is only its 1-2 byte header. */
size_t vp9_write_frame_headers(const Vp9_Frame_Header *h, const Vp9_Probs *probs, const Vp9_Probs *base,
                               uint8_t *out, size_t capacity);

/* The decoder's saved contexts, mirrored. */
typedef struct Vp9_Contexts {
  Vp9_Probs saved[VP9_CONTEXTS];
  bool valid;
} Vp9_Contexts;

/* The probabilities the decoder starts frame `h` from (after its header's
 * resets), into `base`; `defaults` is the specification's default set. */
void vp9_contexts_begin(Vp9_Contexts *c, const Vp9_Frame_Header *h, const Vp9_Probs *defaults, Vp9_Probs *base);
/* After frame `h` with tables `probs`: what the decoder saves. */
void vp9_contexts_end(Vp9_Contexts *c, const Vp9_Frame_Header *h, const Vp9_Probs *probs);

#endif /* SWITCH_VIDEO_VP9_H */
