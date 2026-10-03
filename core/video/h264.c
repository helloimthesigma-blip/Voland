/**
 * H.264 SPS/PPS synthesis. See h264.h; syntax per ITU-T H.264 7.3.2.
 */
#include "video/h264.h"

#include <stdio.h>
#include <string.h>

#define NAL_SPS 0x67u /* nal_ref_idc 3, type 7 */
#define NAL_PPS 0x68u /* nal_ref_idc 3, type 8 */
#define RBSP_MAX 512u
#define MAX_MV_LENGTH_LOG2 16u
#define MAX_BYTES_PER_PIC_DENOM 2u
#define MAX_BITS_PER_MB_DENOM 1u

typedef struct Bits {
  uint8_t data[RBSP_MAX];
  uint32_t bit;
  bool overflow;
} Bits;

static void put_bit(Bits *b, uint32_t v) {
  if (b->bit >= RBSP_MAX * 8u) {
    b->overflow = true;
    return;
  }
  if (v) b->data[b->bit >> 3] |= (uint8_t)(0x80u >> (b->bit & 7u));
  b->bit++;
}

static void put_u(Bits *b, uint32_t v, uint32_t n) {
  for (uint32_t i = n; i-- > 0;) put_bit(b, (v >> i) & 1u);
}

static void put_ue(Bits *b, uint32_t v) {
  const uint64_t code = (uint64_t)v + 1u;
  uint32_t len = 0;
  while ((code >> len) > 1u) len++;
  put_u(b, 0, len);
  for (uint32_t i = len + 1u; i-- > 0;) put_bit(b, (uint32_t)(code >> i) & 1u);
}

static void put_se(Bits *b, int32_t v) { put_ue(b, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }

static void put_trailing(Bits *b) {
  put_bit(b, 1);
  while (b->bit & 7u) put_bit(b, 0);
}

/* Start code, header byte, then the RBSP with emulation prevention. */
static size_t emit_nal(uint8_t header, const Bits *b, uint8_t *out, size_t capacity) {
  const uint32_t bytes = b->bit / 8u;
  size_t n = 0;
  if (b->overflow || capacity < 5u) return 0;
  out[n++] = 0;
  out[n++] = 0;
  out[n++] = 0;
  out[n++] = 1;
  out[n++] = header;
  uint32_t zeros = 0;
  for (uint32_t i = 0; i < bytes; i++) {
    if (n + 2u > capacity) return 0;
    if (zeros >= 2u && b->data[i] <= 3u) {
      out[n++] = 3;
      zeros = 0;
    }
    out[n++] = b->data[i];
    zeros = b->data[i] ? 0 : zeros + 1u;
  }
  return n;
}

static void put_scaling_list(Bits *b, const uint8_t *list, uint32_t size) {
  int32_t last = 8;
  for (uint32_t i = 0; i < size; i++) {
    int32_t delta = (int32_t)list[i] - last;
    if (delta > 127) delta -= 256;
    if (delta < -128) delta += 256;
    put_se(b, delta);
    last = list[i];
  }
}

static size_t write_sps(const H264_Params *p, uint8_t *out, size_t capacity) {
  Bits b;
  memset(&b, 0, sizeof(b));
  put_u(&b, H264_SYNTH_PROFILE, 8);
  put_u(&b, 0, 8); /* constraint flags */
  put_u(&b, H264_SYNTH_LEVEL, 8);
  put_ue(&b, 0); /* seq_parameter_set_id */
  put_ue(&b, p->chroma_format_idc);
  if (p->chroma_format_idc == 3u) put_bit(&b, 0); /* separate_colour_plane_flag */
  put_ue(&b, 0);                                  /* bit_depth_luma_minus8 */
  put_ue(&b, 0);                                  /* bit_depth_chroma_minus8 */
  put_bit(&b, 0);                                 /* qpprime_y_zero_transform_bypass_flag */
  put_bit(&b, 0);                                 /* seq_scaling_matrix_present_flag: the PPS carries them */
  put_ue(&b, p->log2_max_frame_num_minus4);
  put_ue(&b, p->pic_order_cnt_type);
  if (p->pic_order_cnt_type == 0u) {
    put_ue(&b, p->log2_max_poc_lsb_minus4);
  } else if (p->pic_order_cnt_type == 1u) {
    put_bit(&b, p->delta_pic_order_always_zero);
    put_se(&b, 0); /* offset_for_non_ref_pic */
    put_se(&b, 0); /* offset_for_top_to_bottom_field */
    put_ue(&b, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
  }
  put_ue(&b, p->max_num_ref_frames);
  put_bit(&b, 0); /* gaps_in_frame_num_value_allowed_flag */
  put_ue(&b, p->width_mbs - 1u);
  put_ue(&b, (p->frame_mbs_only ? p->height_mbs : p->height_mbs / 2u) - 1u);
  put_bit(&b, p->frame_mbs_only);
  if (!p->frame_mbs_only) put_bit(&b, 1); /* mb_adaptive_frame_field_flag */
  put_bit(&b, p->direct_8x8_inference);
  put_bit(&b, 0); /* frame_cropping_flag */
  /* VUI: only bitstream_restriction, so decoders size the reorder buffer
   * from the stream instead of the level's maximum (output latency). */
  put_bit(&b, 1); /* vui_parameters_present_flag */
  put_bit(&b, 0); /* aspect_ratio_info_present_flag */
  put_bit(&b, 0); /* overscan_info_present_flag */
  put_bit(&b, 0); /* video_signal_type_present_flag */
  put_bit(&b, 0); /* chroma_loc_info_present_flag */
  put_bit(&b, 0); /* timing_info_present_flag */
  put_bit(&b, 0); /* nal_hrd_parameters_present_flag */
  put_bit(&b, 0); /* vcl_hrd_parameters_present_flag */
  put_bit(&b, 0); /* pic_struct_present_flag */
  put_bit(&b, 1); /* bitstream_restriction_flag */
  put_bit(&b, 1); /* motion_vectors_over_pic_boundaries_flag */
  put_ue(&b, MAX_BYTES_PER_PIC_DENOM);
  put_ue(&b, MAX_BITS_PER_MB_DENOM);
  put_ue(&b, MAX_MV_LENGTH_LOG2);
  put_ue(&b, MAX_MV_LENGTH_LOG2);
  put_ue(&b, p->max_num_ref_frames); /* max_num_reorder_frames: no more than the DPB holds */
  put_ue(&b, p->max_num_ref_frames); /* max_dec_frame_buffering */
  put_trailing(&b);
  return emit_nal(NAL_SPS, &b, out, capacity);
}

static size_t write_pps(const H264_Params *p, uint8_t *out, size_t capacity) {
  Bits b;
  memset(&b, 0, sizeof(b));
  put_ue(&b, 0); /* pic_parameter_set_id */
  put_ue(&b, 0); /* seq_parameter_set_id */
  put_bit(&b, p->entropy_coding_mode);
  put_bit(&b, p->bottom_field_pic_order_in_frame_present);
  put_ue(&b, 0); /* num_slice_groups_minus1 */
  put_ue(&b, p->num_ref_idx_l0_default_minus1);
  put_ue(&b, p->num_ref_idx_l1_default_minus1);
  put_bit(&b, p->weighted_pred);
  put_u(&b, p->weighted_bipred_idc, 2);
  put_se(&b, p->pic_init_qp_minus26);
  put_se(&b, 0); /* pic_init_qs_minus26 */
  put_se(&b, p->chroma_qp_index_offset);
  put_bit(&b, p->deblocking_filter_control_present);
  put_bit(&b, p->constrained_intra_pred);
  put_bit(&b, p->redundant_pic_cnt_present);
  put_bit(&b, p->transform_8x8_mode);
  put_bit(&b, p->scaling_matrix_present);
  if (p->scaling_matrix_present) {
    for (uint32_t i = 0; i < H264_SCALING_4X4_LISTS; i++) {
      put_bit(&b, 1);
      put_scaling_list(&b, p->scaling_4x4[i], 16);
    }
    if (p->transform_8x8_mode) {
      for (uint32_t i = 0; i < H264_SCALING_8X8_LISTS; i++) {
        put_bit(&b, 1);
        put_scaling_list(&b, p->scaling_8x8[i], 64);
      }
    }
  }
  put_se(&b, p->second_chroma_qp_index_offset);
  put_trailing(&b);
  return emit_nal(NAL_PPS, &b, out, capacity);
}

size_t h264_write_parameter_sets(const H264_Params *params, uint8_t *out, size_t capacity) {
  if (!params->width_mbs || !params->height_mbs) return 0;
  const size_t sps = write_sps(params, out, capacity);
  if (!sps) return 0;
  const size_t pps = write_pps(params, out + sps, capacity - sps);
  return pps ? sps + pps : 0;
}

void h264_codec_string(char out[16]) { snprintf(out, 16, "avc1.%02X00%02X", H264_SYNTH_PROFILE, H264_SYNTH_LEVEL); }

bool h264_params_equal(const H264_Params *a, const H264_Params *b) { return memcmp(a, b, sizeof(*a)) == 0; }
