/**
 * H.264 parameter-set synthesis for NVDEC (DESIGN §13, "bitstream
 * reconstruction"). NVDEC is handed slice data plus a picture-setup
 * struct holding the already-parsed SPS/PPS fields; a standard decoder
 * (WebCodecs, VideoToolbox) needs Annex-B SPS and PPS NAL units instead.
 * This rebuilds them (ITU-T H.264 7.3.2.1.1 and 7.3.2.2) from the fields
 * that shape slice parsing, so the guest's slices decode unchanged.
 *
 * The picture-setup layout (H264_SETUP_*) is NVIDIA's nvdec_h264_pic_s
 * as observed in the guest's submissions (docs/handoff/BOT_2_STATUS.md).
 */
#ifndef SWITCH_VIDEO_H264_H
#define SWITCH_VIDEO_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H264_SCALING_4X4_LISTS 6u
#define H264_SCALING_8X8_LISTS 2u

typedef struct H264_Params {
  uint32_t width_mbs, height_mbs; /* the frame, in 16x16 macroblocks */
  uint32_t log2_max_frame_num_minus4;
  uint32_t pic_order_cnt_type;
  uint32_t log2_max_poc_lsb_minus4;
  bool delta_pic_order_always_zero;
  bool frame_mbs_only;
  bool direct_8x8_inference;
  uint32_t chroma_format_idc;
  uint32_t max_num_ref_frames;
  /* PPS */
  bool entropy_coding_mode;      /* CABAC */
  bool bottom_field_pic_order_in_frame_present;
  uint32_t num_ref_idx_l0_default_minus1, num_ref_idx_l1_default_minus1;
  bool weighted_pred;
  uint32_t weighted_bipred_idc;
  int32_t pic_init_qp_minus26;
  int32_t chroma_qp_index_offset, second_chroma_qp_index_offset;
  bool deblocking_filter_control_present;
  bool constrained_intra_pred;
  bool redundant_pic_cnt_present;
  bool transform_8x8_mode;
  bool scaling_matrix_present;
  uint8_t scaling_4x4[H264_SCALING_4X4_LISTS][16]; /* zig-zag order, as coded */
  uint8_t scaling_8x8[H264_SCALING_8X8_LISTS][64];
} H264_Params;

/* The profile_idc the synthesized SPS declares (High: covers Baseline
 * and Main streams too) and the level (5.1: any Switch-sized stream). */
#define H264_SYNTH_PROFILE 100u
#define H264_SYNTH_LEVEL 51u

/* Appends Annex-B SPS (id 0) and PPS (id 0) NAL units - start codes
 * included - to out. Returns the bytes written, or 0 if `capacity` is
 * too small. */
size_t h264_write_parameter_sets(const H264_Params *params, uint8_t *out, size_t capacity);

/* "avc1.PPCCLL" for the synthesized SPS (WebCodecs codec string). */
void h264_codec_string(char out[16]);

/* True if params differ in anything the parameter sets carry. */
bool h264_params_equal(const H264_Params *a, const H264_Params *b);

#endif /* SWITCH_VIDEO_H264_H */
