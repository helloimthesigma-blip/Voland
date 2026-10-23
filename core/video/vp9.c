#include "video/vp9.h"

#include <string.h>

#include "video/vp9_write.h"

#define VP9_SYNC_0 0x49u
#define VP9_SYNC_1 0x83u
#define VP9_SYNC_2 0x42u
#define VP9_FRAME_MARKER 2u
#define VP9_CS_RGB 7u
#define VP9_MIN_TILE_WIDTH_B64 4u
#define VP9_MAX_TILE_WIDTH_B64 64u
#define VP9_HEADER_SIZE_OFFSET_BITS 16u

bool vp9_frame_is_intra(const Vp9_Frame_Header *h) { return h->frame_type == VP9_KEY_FRAME || h->intra_only; }

bool vp9_lossless(const Vp9_Frame_Header *h) {
  return h->base_q_idx == 0 && h->delta_q_y_dc == 0 && h->delta_q_uv_dc == 0 && h->delta_q_uv_ac == 0;
}

/* ---- uncompressed header (spec 6.2) --------------------------------- */

static void color_config(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  if (h->profile >= 2) vp9_bits_put(b, h->bit_depth == 12 ? 1u : 0u, 1);
  vp9_bits_put(b, h->color_space, 3);
  if (h->color_space != VP9_CS_RGB) {
    vp9_bits_put(b, h->color_range, 1);
    if (h->profile == 1 || h->profile == 3) {
      vp9_bits_put(b, h->subsampling_x, 1);
      vp9_bits_put(b, h->subsampling_y, 1);
      vp9_bits_put(b, 0, 1);
    }
  } else if (h->profile == 1 || h->profile == 3) {
    vp9_bits_put(b, 0, 1);
  }
}

static void frame_size(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  vp9_bits_put(b, h->width - 1u, 16);
  vp9_bits_put(b, h->height - 1u, 16);
}

static void render_size(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  const bool different = h->render_width && h->render_height &&
                         (h->render_width != h->width || h->render_height != h->height);
  vp9_bits_put(b, different, 1);
  if (different) {
    vp9_bits_put(b, h->render_width - 1u, 16);
    vp9_bits_put(b, h->render_height - 1u, 16);
  }
}

static void sync_code(Vp9_Bits *b) {
  vp9_bits_put(b, VP9_SYNC_0, 8);
  vp9_bits_put(b, VP9_SYNC_1, 8);
  vp9_bits_put(b, VP9_SYNC_2, 8);
}

/* The header's 2-bit literal for each filter type. */
static uint32_t filter_literal(uint32_t type) {
  switch (type) {
    case VP9_EIGHTTAP_SMOOTH: return 0;
    case VP9_EIGHTTAP: return 1;
    case VP9_EIGHTTAP_SHARP: return 2;
    default: return 3; /* bilinear */
  }
}

static void tile_info(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  const uint32_t mi_cols = (h->width + 7u) >> 3, sb64_cols = (mi_cols + 7u) >> 3;
  uint32_t min_log2 = 0, max_log2 = 1;
  while ((VP9_MAX_TILE_WIDTH_B64 << min_log2) < sb64_cols) min_log2++;
  while ((sb64_cols >> max_log2) >= VP9_MIN_TILE_WIDTH_B64) max_log2++;
  max_log2--;
  uint32_t cols = min_log2;
  while (cols < max_log2) {
    const bool more = cols < h->tile_cols_log2;
    vp9_bits_put(b, more, 1);
    if (!more) break;
    cols++;
  }
  vp9_bits_put(b, h->tile_rows_log2 > 0, 1);
  if (h->tile_rows_log2 > 0) vp9_bits_put(b, h->tile_rows_log2 > 1, 1);
}

static void delta_q(Vp9_Bits *b, int32_t value) {
  vp9_bits_put(b, value != 0, 1);
  if (value) vp9_bits_put_signed(b, value, 4);
}

static void seg_prob(Vp9_Bits *b, uint8_t p) {
  vp9_bits_put(b, p != 255u, 1);
  if (p != 255u) vp9_bits_put(b, p, 8);
}

static const uint32_t k_seg_feature_bits[VP9_SEG_FEATURES] = {8, 6, 2, 0};
static const bool k_seg_feature_signed[VP9_SEG_FEATURES] = {true, true, false, false};

static void segmentation(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  vp9_bits_put(b, h->seg_enabled, 1);
  if (!h->seg_enabled) return;
  vp9_bits_put(b, h->seg_update_map, 1);
  if (h->seg_update_map) {
    for (uint32_t i = 0; i < 7; i++) seg_prob(b, h->seg_tree_probs[i]);
    vp9_bits_put(b, h->seg_temporal_update, 1);
    for (uint32_t i = 0; i < 3; i++)
      if (h->seg_temporal_update) seg_prob(b, h->seg_pred_probs[i]);
  }
  vp9_bits_put(b, h->seg_update_data, 1);
  if (!h->seg_update_data) return;
  vp9_bits_put(b, h->seg_abs_delta, 1);
  for (uint32_t i = 0; i < VP9_SEGMENTS; i++) {
    for (uint32_t j = 0; j < VP9_SEG_FEATURES; j++) {
      vp9_bits_put(b, h->seg_feature_enabled[i][j], 1);
      if (!h->seg_feature_enabled[i][j]) continue;
      const int32_t v = h->seg_feature_value[i][j];
      vp9_bits_put(b, (uint32_t)(v < 0 ? -v : v), k_seg_feature_bits[j]);
      if (k_seg_feature_signed[j]) vp9_bits_put(b, v < 0, 1);
    }
  }
}

static void loop_filter(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  vp9_bits_put(b, h->lf_level, 6);
  vp9_bits_put(b, h->lf_sharpness, 3);
  vp9_bits_put(b, h->lf_delta_enabled, 1);
  if (!h->lf_delta_enabled) return;
  vp9_bits_put(b, h->lf_delta_update, 1);
  if (!h->lf_delta_update) return;
  for (uint32_t i = 0; i < 4; i++) {
    vp9_bits_put(b, h->lf_ref_update[i], 1);
    if (h->lf_ref_update[i]) vp9_bits_put_signed(b, h->lf_ref_deltas[i], 6);
  }
  for (uint32_t i = 0; i < 2; i++) {
    vp9_bits_put(b, h->lf_mode_update[i], 1);
    if (h->lf_mode_update[i]) vp9_bits_put_signed(b, h->lf_mode_deltas[i], 6);
  }
}

/* Writes everything up to header_size_in_bytes; returns the bit position
 * of that 16-bit field. */
static size_t uncompressed_header(Vp9_Bits *b, const Vp9_Frame_Header *h) {
  vp9_bits_put(b, VP9_FRAME_MARKER, 2);
  vp9_bits_put(b, h->profile & 1u, 1);
  vp9_bits_put(b, h->profile >> 1, 1);
  if (h->profile == 3) vp9_bits_put(b, 0, 1);
  vp9_bits_put(b, h->show_existing_frame, 1);
  if (h->show_existing_frame) {
    vp9_bits_put(b, h->frame_to_show, 3);
    return 0;
  }
  vp9_bits_put(b, h->frame_type, 1);
  vp9_bits_put(b, h->show_frame, 1);
  vp9_bits_put(b, h->error_resilient, 1);
  if (h->frame_type == VP9_KEY_FRAME) {
    sync_code(b);
    color_config(b, h);
    frame_size(b, h);
    render_size(b, h);
  } else {
    if (!h->show_frame) vp9_bits_put(b, h->intra_only, 1);
    if (!h->error_resilient) vp9_bits_put(b, h->reset_frame_context, 2);
    if (h->intra_only) {
      sync_code(b);
      if (h->profile > 0) color_config(b, h);
      vp9_bits_put(b, h->refresh_frame_flags, 8);
      frame_size(b, h);
      render_size(b, h);
    } else {
      vp9_bits_put(b, h->refresh_frame_flags, 8);
      for (uint32_t i = 0; i < VP9_REFS; i++) {
        vp9_bits_put(b, h->ref_frame_idx[i], 3);
        vp9_bits_put(b, h->ref_sign_bias[i], 1);
      }
      bool found = false;
      for (uint32_t i = 0; i < VP9_REFS && !found; i++) {
        found = h->size_from_ref[i];
        vp9_bits_put(b, found, 1);
      }
      if (!found) frame_size(b, h);
      render_size(b, h);
      vp9_bits_put(b, h->allow_high_precision_mv, 1);
      vp9_bits_put(b, h->interp_filter == VP9_SWITCHABLE, 1);
      if (h->interp_filter != VP9_SWITCHABLE) vp9_bits_put(b, filter_literal(h->interp_filter), 2);
    }
  }
  if (!h->error_resilient) {
    vp9_bits_put(b, h->refresh_frame_context, 1);
    vp9_bits_put(b, h->frame_parallel, 1);
  }
  vp9_bits_put(b, h->frame_context_idx, 2);
  loop_filter(b, h);
  vp9_bits_put(b, h->base_q_idx, 8);
  delta_q(b, h->delta_q_y_dc);
  delta_q(b, h->delta_q_uv_dc);
  delta_q(b, h->delta_q_uv_ac);
  segmentation(b, h);
  tile_info(b, h);
  const size_t at = b->bit;
  vp9_bits_put(b, 0, VP9_HEADER_SIZE_OFFSET_BITS); /* filled in afterwards */
  return at;
}

/* ---- compressed header (spec 6.3) ----------------------------------- */

static const uint32_t k_biggest_tx[5] = {0, 1, 2, 3, 3};

#define UPDATE_ALL(w, cur, base) \
  do { \
    const uint8_t *c_ = (const uint8_t *)(cur), *b_ = (const uint8_t *)(base); \
    for (size_t i_ = 0; i_ < sizeof(cur); i_++) vp9_write_prob_update((w), b_[i_], c_[i_]); \
  } while (0)

#define MV_UPDATE_ALL(w, cur, base) \
  do { \
    const uint8_t *c_ = (const uint8_t *)(cur), *b_ = (const uint8_t *)(base); \
    for (size_t i_ = 0; i_ < sizeof(cur); i_++) vp9_write_mv_prob_update((w), b_[i_], c_[i_]); \
  } while (0)

static bool compound_allowed(const Vp9_Frame_Header *h) {
  for (uint32_t i = 1; i < VP9_REFS; i++)
    if (h->ref_sign_bias[i] != h->ref_sign_bias[0]) return true;
  return false;
}

static void compressed_header(Vp9_Bool_Writer *w, const Vp9_Frame_Header *h, const Vp9_Probs *p, const Vp9_Probs *base) {
  const uint32_t tx_mode = vp9_lossless(h) ? VP9_ONLY_4X4 : h->tx_mode;
  if (!vp9_lossless(h)) {
    vp9_bool_literal(w, tx_mode > VP9_ALLOW_32X32 ? VP9_ALLOW_32X32 : tx_mode, 2);
    if (tx_mode >= VP9_ALLOW_32X32) vp9_bool_literal(w, tx_mode == VP9_TX_MODE_SELECT, 1);
  }
  if (tx_mode == VP9_TX_MODE_SELECT) {
    UPDATE_ALL(w, p->tx8, base->tx8);
    UPDATE_ALL(w, p->tx16, base->tx16);
    UPDATE_ALL(w, p->tx32, base->tx32);
  }
  for (uint32_t tx = 0; tx <= k_biggest_tx[tx_mode]; tx++) {
    const bool changed = memcmp(p->coef[tx], base->coef[tx], sizeof(p->coef[tx])) != 0;
    vp9_bool_literal(w, changed, 1);
    if (!changed) continue;
    for (uint32_t i = 0; i < 2; i++)
      for (uint32_t j = 0; j < 2; j++)
        for (uint32_t k = 0; k < 6; k++)
          for (uint32_t l = 0; l < (k == 0 ? 3u : 6u); l++)
            for (uint32_t m = 0; m < 3; m++)
              vp9_write_prob_update(w, base->coef[tx][i][j][k][l][m], p->coef[tx][i][j][k][l][m]);
  }
  UPDATE_ALL(w, p->skip, base->skip);
  if (vp9_frame_is_intra(h)) return;
  UPDATE_ALL(w, p->inter_mode, base->inter_mode);
  if (h->interp_filter == VP9_SWITCHABLE) UPDATE_ALL(w, p->interp_filter, base->interp_filter);
  UPDATE_ALL(w, p->is_inter, base->is_inter);
  uint32_t mode = VP9_SINGLE_REFERENCE;
  if (compound_allowed(h)) {
    mode = h->reference_mode;
    vp9_bool_literal(w, mode != VP9_SINGLE_REFERENCE, 1);
    if (mode != VP9_SINGLE_REFERENCE) vp9_bool_literal(w, mode == VP9_REFERENCE_MODE_SELECT, 1);
  }
  if (mode == VP9_REFERENCE_MODE_SELECT) UPDATE_ALL(w, p->comp_mode, base->comp_mode);
  if (mode != VP9_COMPOUND_REFERENCE) {
    for (uint32_t i = 0; i < 5; i++) {
      vp9_write_prob_update(w, base->single_ref[i][0], p->single_ref[i][0]);
      vp9_write_prob_update(w, base->single_ref[i][1], p->single_ref[i][1]);
    }
  }
  if (mode != VP9_SINGLE_REFERENCE) UPDATE_ALL(w, p->comp_ref, base->comp_ref);
  UPDATE_ALL(w, p->y_mode, base->y_mode);
  UPDATE_ALL(w, p->partition, base->partition);
  MV_UPDATE_ALL(w, p->mv_joints, base->mv_joints);
  for (uint32_t i = 0; i < 2; i++) {
    vp9_write_mv_prob_update(w, base->mv_sign[i], p->mv_sign[i]);
    MV_UPDATE_ALL(w, p->mv_classes[i], base->mv_classes[i]);
    vp9_write_mv_prob_update(w, base->mv_class0[i][0], p->mv_class0[i][0]);
    MV_UPDATE_ALL(w, p->mv_bits[i], base->mv_bits[i]);
  }
  for (uint32_t i = 0; i < 2; i++) {
    MV_UPDATE_ALL(w, p->mv_class0_fr[i], base->mv_class0_fr[i]);
    MV_UPDATE_ALL(w, p->mv_fr[i], base->mv_fr[i]);
  }
  if (h->allow_high_precision_mv) {
    for (uint32_t i = 0; i < 2; i++) {
      vp9_write_mv_prob_update(w, base->mv_class0_hp[i], p->mv_class0_hp[i]);
      vp9_write_mv_prob_update(w, base->mv_hp[i], p->mv_hp[i]);
    }
  }
}

size_t vp9_write_frame_headers(const Vp9_Frame_Header *h, const Vp9_Probs *probs, const Vp9_Probs *base,
                               uint8_t *out, size_t capacity) {
  Vp9_Bits b;
  vp9_bits_init(&b, out, capacity);
  const size_t size_at = uncompressed_header(&b, h);
  if (b.overflow) return 0;
  const size_t head = vp9_bits_bytes(&b); /* trailing_bits: zero to the byte */
  if (h->show_existing_frame) return head;
  Vp9_Bool_Writer w;
  vp9_bool_init(&w, out + head, capacity - head);
  compressed_header(&w, h, probs, base);
  const size_t compressed = vp9_bool_finish(&w);
  if (!compressed || compressed > 0xFFFFu) return 0;
  for (uint32_t i = 0; i < VP9_HEADER_SIZE_OFFSET_BITS; i++) {
    const size_t bit = size_at + i;
    if ((compressed >> (VP9_HEADER_SIZE_OFFSET_BITS - 1u - i)) & 1u) out[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7u));
  }
  return head + compressed;
}

/* ---- contexts (spec 7.2 setup_past_independence, refresh_probs) ----- */

uint32_t vp9_closest_context(const Vp9_Contexts *c, const Vp9_Probs *probs) {
  if (!c->valid) return 0;
  uint32_t best = 0, best_diff = UINT32_MAX;
  for (uint32_t i = 0; i < VP9_CONTEXTS; i++) {
    const uint8_t *a = (const uint8_t *)&c->saved[i], *b = (const uint8_t *)probs;
    uint32_t diff = 0;
    for (size_t k = 0; k < sizeof(Vp9_Probs); k++) diff += a[k] != b[k];
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
  }
  return best;
}

void vp9_contexts_begin(Vp9_Contexts *c, const Vp9_Frame_Header *h, const Vp9_Probs *defaults, Vp9_Probs *base) {
  if (!c->valid) {
    for (uint32_t i = 0; i < VP9_CONTEXTS; i++) c->saved[i] = *defaults;
    c->valid = true;
  }
  uint32_t idx = h->frame_context_idx;
  if (vp9_frame_is_intra(h) || h->error_resilient) {
    if (h->frame_type == VP9_KEY_FRAME || h->error_resilient || h->reset_frame_context == 3u) {
      for (uint32_t i = 0; i < VP9_CONTEXTS; i++) c->saved[i] = *defaults;
    } else if (h->reset_frame_context == 2u) {
      c->saved[idx] = *defaults;
    }
    idx = 0;
  }
  *base = c->saved[idx];
}

void vp9_conveyed_probs(const Vp9_Frame_Header *h, const Vp9_Probs *p, const Vp9_Probs *base, Vp9_Probs *out) {
  *out = *base;
  const uint32_t tx_mode = vp9_lossless(h) ? VP9_ONLY_4X4 : h->tx_mode;
  if (tx_mode == VP9_TX_MODE_SELECT) {
    memcpy(out->tx8, p->tx8, sizeof(out->tx8));
    memcpy(out->tx16, p->tx16, sizeof(out->tx16));
    memcpy(out->tx32, p->tx32, sizeof(out->tx32));
  }
  for (uint32_t tx = 0; tx <= k_biggest_tx[tx_mode]; tx++) memcpy(out->coef[tx], p->coef[tx], sizeof(out->coef[tx]));
  memcpy(out->skip, p->skip, sizeof(out->skip));
  if (vp9_frame_is_intra(h)) return;
  memcpy(out->inter_mode, p->inter_mode, sizeof(out->inter_mode));
  if (h->interp_filter == VP9_SWITCHABLE) memcpy(out->interp_filter, p->interp_filter, sizeof(out->interp_filter));
  memcpy(out->is_inter, p->is_inter, sizeof(out->is_inter));
  const uint32_t mode = compound_allowed(h) ? h->reference_mode : VP9_SINGLE_REFERENCE;
  if (mode == VP9_REFERENCE_MODE_SELECT) memcpy(out->comp_mode, p->comp_mode, sizeof(out->comp_mode));
  if (mode != VP9_COMPOUND_REFERENCE) memcpy(out->single_ref, p->single_ref, sizeof(out->single_ref));
  if (mode != VP9_SINGLE_REFERENCE) memcpy(out->comp_ref, p->comp_ref, sizeof(out->comp_ref));
  memcpy(out->y_mode, p->y_mode, sizeof(out->y_mode));
  memcpy(out->partition, p->partition, sizeof(out->partition));
  memcpy(out->mv_joints, p->mv_joints, sizeof(out->mv_joints));
  memcpy(out->mv_sign, p->mv_sign, sizeof(out->mv_sign));
  memcpy(out->mv_classes, p->mv_classes, sizeof(out->mv_classes));
  memcpy(out->mv_class0, p->mv_class0, sizeof(out->mv_class0));
  memcpy(out->mv_bits, p->mv_bits, sizeof(out->mv_bits));
  memcpy(out->mv_class0_fr, p->mv_class0_fr, sizeof(out->mv_class0_fr));
  memcpy(out->mv_fr, p->mv_fr, sizeof(out->mv_fr));
  if (h->allow_high_precision_mv) {
    memcpy(out->mv_class0_hp, p->mv_class0_hp, sizeof(out->mv_class0_hp));
    memcpy(out->mv_hp, p->mv_hp, sizeof(out->mv_hp));
  }
}

void vp9_contexts_end(Vp9_Contexts *c, const Vp9_Frame_Header *h, const Vp9_Probs *probs, const Vp9_Probs *base) {
  if (h->show_existing_frame || h->error_resilient || !h->refresh_frame_context) return;
  const uint32_t idx = (vp9_frame_is_intra(h) || h->error_resilient) ? 0u : h->frame_context_idx;
  vp9_conveyed_probs(h, probs, base, &c->saved[idx]);
}
