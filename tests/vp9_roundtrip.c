/**
 * VP9 header round trip (a developer tool, run by tests/vp9_roundtrip.sh
 * with ffmpeg): reads an IVF stream, parses every frame's uncompressed and
 * compressed headers into Vp9_Frame_Header + Vp9_Probs exactly as NVDEC's
 * client library hands them over (the frame's final probabilities), and
 * writes a new IVF whose headers are rebuilt by core/video/vp9.c in front
 * of the original tile data. If the rebuilt stream decodes to the same
 * pixels, the writer is right.
 *
 *   vp9_roundtrip IN.ivf OUT.ivf
 *
 * The parser's default probabilities are placeholders: the writer only
 * needs to agree with the decoder about the base each delta applies to,
 * so any defaults round-trip.
 */
#include "video/vp9.h"
#include "video/vp9_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit and bool readers (spec 9.2) -------------------------------- */

typedef struct Bits {
  const uint8_t *d;
  size_t size, pos;
} Bits;

static uint32_t f(Bits *b, uint32_t n) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    const size_t byte = b->pos >> 3;
    v = (v << 1) | (byte < b->size ? (b->d[byte] >> (7u - (b->pos & 7u))) & 1u : 0u);
    b->pos++;
  }
  return v;
}

static int32_t su(Bits *b, uint32_t n) {
  const int32_t v = (int32_t)f(b, n);
  return f(b, 1) ? -v : v;
}

typedef struct Bools {
  Bits bits;
  uint32_t value, range;
  int32_t max_bits;
} Bools;

static FILE *g_sym;
static bool B_raw(Bools *r, uint8_t p);
static bool B(Bools *r, uint8_t p) {
  const bool v = B_raw(r, p);
  if (g_sym) fprintf(g_sym, "%u:%u\n", p, v);
  return v;
}
static bool B_raw(Bools *r, uint8_t p) {
  const uint32_t split = 1u + (((r->range - 1u) * p) >> 8);
  bool bit;
  if (r->value < split) {
    r->range = split;
    bit = false;
  } else {
    r->range -= split;
    r->value -= split;
    bit = true;
  }
  while (r->range < 128u) {
    uint32_t next = 0;
    if (r->max_bits > 0) {
      next = f(&r->bits, 1);
      r->max_bits--;
    }
    r->range <<= 1;
    r->value = (r->value << 1) | next;
  }
  return bit;
}

static uint32_t L(Bools *r, uint32_t n) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; i++) v = (v << 1) | B(r, 128);
  return v;
}

static uint32_t term_subexp(Bools *r) {
  if (!L(r, 1)) return L(r, 4);
  if (!L(r, 1)) return L(r, 4) + 16u;
  if (!L(r, 1)) return L(r, 5) + 32u;
  const uint32_t v = L(r, 7);
  if (v < 65u) return v + 64u;
  const uint32_t t = (v << 1) - 1u + L(r, 1);
  if (t >= 254u && getenv("VP9_TRACE")) fprintf(stderr, "delta index %u\n", t);
  return t;
}

static uint32_t g_rec[8192], g_rec_n;
static void diff(Bools *r, uint8_t *p) {
  if (B(r, 252)) {
    const uint32_t d = term_subexp(r);
    if (g_rec_n < 8192) g_rec[g_rec_n++] = d << 16 | *p << 8 | vp9_inv_remap_prob(d, *p);
    *p = vp9_inv_remap_prob(d, *p);
  }
}

static void mv_diff(Bools *r, uint8_t *p) {
  if (B(r, 252)) *p = (uint8_t)((L(r, 7) << 1) | 1u);
}

#define DIFF_ALL(r, arr) \
  do { uint8_t *p_ = (uint8_t *)(arr); for (size_t i_ = 0; i_ < sizeof(arr); i_++) diff((r), &p_[i_]); } while (0)
#define MV_DIFF_ALL(r, arr) \
  do { uint8_t *p_ = (uint8_t *)(arr); for (size_t i_ = 0; i_ < sizeof(arr); i_++) mv_diff((r), &p_[i_]); } while (0)

/* ---- the parser ------------------------------------------------------ */

typedef struct Ref_Size { uint32_t w, h; } Ref_Size;
static Ref_Size g_ref_size[VP9_REF_SLOTS];

static void color_config(Bits *b, Vp9_Frame_Header *h) {
  h->bit_depth = 8;
  if (h->profile >= 2) h->bit_depth = f(b, 1) ? 12 : 10;
  h->color_space = f(b, 3);
  if (h->color_space != 7u) {
    h->color_range = f(b, 1);
    if (h->profile == 1 || h->profile == 3) {
      h->subsampling_x = f(b, 1);
      h->subsampling_y = f(b, 1);
      (void)f(b, 1);
    } else {
      h->subsampling_x = h->subsampling_y = true;
    }
  } else {
    h->color_range = true;
    if (h->profile == 1 || h->profile == 3) (void)f(b, 1);
  }
}

static void render_size(Bits *b, Vp9_Frame_Header *h) {
  if (f(b, 1)) {
    h->render_width = f(b, 16) + 1u;
    h->render_height = f(b, 16) + 1u;
  } else {
    h->render_width = h->render_height = 0;
  }
}

static const uint32_t k_literal_to_filter[4] = {VP9_EIGHTTAP_SMOOTH, VP9_EIGHTTAP, VP9_EIGHTTAP_SHARP, VP9_BILINEAR};
static const uint32_t k_feature_bits[4] = {8, 6, 2, 0};
static const bool k_feature_signed[4] = {true, true, false, false};

/* Loop filter / segmentation state persists across frames in the
 * decoder; the header only codes changes. */
static Vp9_Frame_Header g_persist, g_persist_save;

static bool parse_uncompressed(Bits *b, Vp9_Frame_Header *h, uint32_t *header_size) {
  const uint32_t marker = f(b, 2);
  if (marker != 2u) return false;
  h->profile = f(b, 1);
  h->profile |= f(b, 1) << 1;
  if (h->profile == 3) (void)f(b, 1);
  h->show_existing_frame = f(b, 1);
  if (h->show_existing_frame) {
    h->frame_to_show = f(b, 3);
    *header_size = 0;
    return true;
  }
  h->frame_type = f(b, 1);
  h->show_frame = f(b, 1);
  h->error_resilient = f(b, 1);
  h->intra_only = false;
  h->reset_frame_context = 0;
  memset(h->size_from_ref, 0, sizeof(h->size_from_ref));
  if (h->frame_type == VP9_KEY_FRAME) {
    if (f(b, 8) != 0x49 || f(b, 8) != 0x83 || f(b, 8) != 0x42) return false;
    color_config(b, h);
    h->width = f(b, 16) + 1u;
    h->height = f(b, 16) + 1u;
    render_size(b, h);
    h->refresh_frame_flags = 0xFF;
  } else {
    if (!h->show_frame) h->intra_only = f(b, 1);
    if (!h->error_resilient) h->reset_frame_context = f(b, 2);
    if (h->intra_only) {
      if (f(b, 8) != 0x49 || f(b, 8) != 0x83 || f(b, 8) != 0x42) return false;
      if (h->profile > 0) color_config(b, h);
      else { h->bit_depth = 8; h->color_space = 2; h->subsampling_x = h->subsampling_y = true; h->color_range = false; }
      h->refresh_frame_flags = f(b, 8);
      h->width = f(b, 16) + 1u;
      h->height = f(b, 16) + 1u;
      render_size(b, h);
    } else {
      h->refresh_frame_flags = f(b, 8);
      for (uint32_t i = 0; i < VP9_REFS; i++) {
        h->ref_frame_idx[i] = f(b, 3);
        h->ref_sign_bias[i] = f(b, 1);
      }
      bool found = false;
      for (uint32_t i = 0; i < VP9_REFS && !found; i++) {
        found = f(b, 1);
        if (found) {
          h->size_from_ref[i] = true;
          h->width = g_ref_size[h->ref_frame_idx[i]].w;
          h->height = g_ref_size[h->ref_frame_idx[i]].h;
        }
      }
      if (!found) {
        h->width = f(b, 16) + 1u;
        h->height = f(b, 16) + 1u;
      }
      render_size(b, h);
      h->allow_high_precision_mv = f(b, 1);
      h->interp_filter = f(b, 1) ? VP9_SWITCHABLE : k_literal_to_filter[f(b, 2)];
    }
  }
  if (!h->error_resilient) {
    h->refresh_frame_context = f(b, 1);
    h->frame_parallel = f(b, 1);
  } else {
    h->refresh_frame_context = false;
    h->frame_parallel = true;
  }
  h->frame_context_idx = f(b, 2);
  if (vp9_frame_is_intra(h) || h->error_resilient) {
    /* setup_past_independence: loop filter deltas and segmentation reset. */
    g_persist.lf_delta_enabled = true;
    const int8_t ref[4] = {1, 0, -1, -1};
    memcpy(g_persist.lf_ref_deltas, ref, sizeof(ref));
    memset(g_persist.lf_mode_deltas, 0, sizeof(g_persist.lf_mode_deltas));
    memset(g_persist.seg_feature_enabled, 0, sizeof(g_persist.seg_feature_enabled));
    memset(g_persist.seg_feature_value, 0, sizeof(g_persist.seg_feature_value));
    g_persist.seg_abs_delta = false;
  }
  h->lf_level = f(b, 6);
  h->lf_sharpness = f(b, 3);
  h->lf_delta_enabled = f(b, 1);
  h->lf_delta_update = false;
  memset(h->lf_ref_update, 0, sizeof(h->lf_ref_update));
  memset(h->lf_mode_update, 0, sizeof(h->lf_mode_update));
  memcpy(h->lf_ref_deltas, g_persist.lf_ref_deltas, sizeof(h->lf_ref_deltas));
  memcpy(h->lf_mode_deltas, g_persist.lf_mode_deltas, sizeof(h->lf_mode_deltas));
  if (h->lf_delta_enabled) {
    h->lf_delta_update = f(b, 1);
    if (h->lf_delta_update) {
      for (uint32_t i = 0; i < 4; i++) if ((h->lf_ref_update[i] = f(b, 1))) h->lf_ref_deltas[i] = (int8_t)su(b, 6);
      for (uint32_t i = 0; i < 2; i++) if ((h->lf_mode_update[i] = f(b, 1))) h->lf_mode_deltas[i] = (int8_t)su(b, 6);
    }
  }
  memcpy(g_persist.lf_ref_deltas, h->lf_ref_deltas, sizeof(h->lf_ref_deltas));
  memcpy(g_persist.lf_mode_deltas, h->lf_mode_deltas, sizeof(h->lf_mode_deltas));
  h->base_q_idx = f(b, 8);
  h->delta_q_y_dc = f(b, 1) ? su(b, 4) : 0;
  h->delta_q_uv_dc = f(b, 1) ? su(b, 4) : 0;
  h->delta_q_uv_ac = f(b, 1) ? su(b, 4) : 0;
  h->seg_enabled = f(b, 1);
  h->seg_update_map = h->seg_temporal_update = h->seg_update_data = false;
  if (h->seg_enabled) {
    h->seg_update_map = f(b, 1);
    if (h->seg_update_map) {
      for (uint32_t i = 0; i < 7; i++) h->seg_tree_probs[i] = f(b, 1) ? (uint8_t)f(b, 8) : 255u;
      h->seg_temporal_update = f(b, 1);
      for (uint32_t i = 0; i < 3; i++) h->seg_pred_probs[i] = h->seg_temporal_update && f(b, 1) ? (uint8_t)f(b, 8) : 255u;
    }
    h->seg_update_data = f(b, 1);
    if (h->seg_update_data) {
      h->seg_abs_delta = f(b, 1);
      for (uint32_t i = 0; i < VP9_SEGMENTS; i++)
        for (uint32_t j = 0; j < VP9_SEG_FEATURES; j++) {
          h->seg_feature_enabled[i][j] = f(b, 1);
          h->seg_feature_value[i][j] = 0;
          if (h->seg_feature_enabled[i][j]) {
            int32_t v = (int32_t)f(b, k_feature_bits[j]);
            if (k_feature_signed[j] && f(b, 1)) v = -v;
            h->seg_feature_value[i][j] = v;
          }
        }
    }
  }
  /* tile_info */
  const uint32_t mi_cols = (h->width + 7u) >> 3, sb64 = (mi_cols + 7u) >> 3;
  uint32_t min_log2 = 0, max_log2 = 1;
  while ((64u << min_log2) < sb64) min_log2++;
  while ((sb64 >> max_log2) >= 4u) max_log2++;
  max_log2--;
  h->tile_cols_log2 = min_log2;
  while (h->tile_cols_log2 < max_log2 && f(b, 1)) h->tile_cols_log2++;
  h->tile_rows_log2 = f(b, 1);
  if (h->tile_rows_log2) h->tile_rows_log2 += f(b, 1);
  *header_size = f(b, 16);
  return true;
}

static void parse_compressed(Bools *r, Vp9_Frame_Header *h, Vp9_Probs *p) {
  if (vp9_lossless(h)) {
    h->tx_mode = VP9_ONLY_4X4;
  } else {
    h->tx_mode = L(r, 2);
    if (h->tx_mode == VP9_ALLOW_32X32) h->tx_mode += L(r, 1);
  }
  if (h->tx_mode == VP9_TX_MODE_SELECT) {
    DIFF_ALL(r, p->tx8);
    DIFF_ALL(r, p->tx16);
    DIFF_ALL(r, p->tx32);
  }
  static const uint32_t biggest[5] = {0, 1, 2, 3, 3};
  for (uint32_t tx = 0; tx <= biggest[h->tx_mode]; tx++) {
    if (!L(r, 1)) continue;
    for (uint32_t i = 0; i < 2; i++)
      for (uint32_t j = 0; j < 2; j++)
        for (uint32_t k = 0; k < 6; k++)
          for (uint32_t l = 0; l < (k == 0 ? 3u : 6u); l++)
            for (uint32_t m = 0; m < 3; m++) diff(r, &p->coef[tx][i][j][k][l][m]);
  }
  DIFF_ALL(r, p->skip);
  h->reference_mode = VP9_SINGLE_REFERENCE;
  if (vp9_frame_is_intra(h)) return;
  DIFF_ALL(r, p->inter_mode);
  if (h->interp_filter == VP9_SWITCHABLE) DIFF_ALL(r, p->interp_filter);
  DIFF_ALL(r, p->is_inter);
  bool compound = false;
  for (uint32_t i = 1; i < VP9_REFS; i++) compound |= h->ref_sign_bias[i] != h->ref_sign_bias[0];
  if (compound && L(r, 1)) h->reference_mode = L(r, 1) ? VP9_REFERENCE_MODE_SELECT : VP9_COMPOUND_REFERENCE;
  if (h->reference_mode == VP9_REFERENCE_MODE_SELECT) DIFF_ALL(r, p->comp_mode);
  if (h->reference_mode != VP9_COMPOUND_REFERENCE)
    for (uint32_t i = 0; i < 5; i++) { diff(r, &p->single_ref[i][0]); diff(r, &p->single_ref[i][1]); }
  if (h->reference_mode != VP9_SINGLE_REFERENCE) DIFF_ALL(r, p->comp_ref);
  DIFF_ALL(r, p->y_mode);
  DIFF_ALL(r, p->partition);
  MV_DIFF_ALL(r, p->mv_joints);
  for (uint32_t i = 0; i < 2; i++) {
    mv_diff(r, &p->mv_sign[i]);
    MV_DIFF_ALL(r, p->mv_classes[i]);
    mv_diff(r, &p->mv_class0[i][0]);
    MV_DIFF_ALL(r, p->mv_bits[i]);
  }
  for (uint32_t i = 0; i < 2; i++) {
    MV_DIFF_ALL(r, p->mv_class0_fr[i]);
    MV_DIFF_ALL(r, p->mv_fr[i]);
  }
  if (h->allow_high_precision_mv)
    for (uint32_t i = 0; i < 2; i++) { mv_diff(r, &p->mv_class0_hp[i]); mv_diff(r, &p->mv_hp[i]); }
}

/* ---- frames, superframes, IVF ---------------------------------------- */

static Vp9_Contexts g_in_ctx, g_out_ctx;
static Vp9_Probs g_defaults;

/* One frame in, rebuilt into `out`; returns its size (0 on error). */
static size_t rebuild(const uint8_t *in, size_t size, uint8_t *out, size_t capacity) {
  Bits b = {in, size, 0};
  Vp9_Frame_Header h;
  memset(&h, 0, sizeof(h));
  uint32_t header_size = 0;
  if (!parse_uncompressed(&b, &h, &header_size)) {
    fprintf(stderr, "bad uncompressed header\n");
    return 0;
  }
  if (h.show_existing_frame) {
    const size_t n = vp9_write_frame_headers(&h, NULL, NULL, out, capacity);
    return n;
  }
  const size_t head = (b.pos + 7u) >> 3;
  /* The decoder's side: probabilities as NVDEC's client hands them over. */
  Vp9_Probs probs, in_base;
  vp9_contexts_begin(&g_in_ctx, &h, &g_defaults, &probs);
  in_base = probs;
  Bools r;
  memset(&r, 0, sizeof(r));
  r.bits = (Bits){in + head, header_size, 0};
  r.value = f(&r.bits, 8);
  r.range = 255;
  r.max_bits = (int32_t)(8 * header_size) - 8;
  if (B(&r, 128)) fprintf(stderr, "marker bit set\n");
  g_rec_n = 0;
  static int sym_frame = -1;
  sym_frame++;
  if (getenv("VP9_SYMS") && atoi(getenv("VP9_SYMS")) == sym_frame) g_sym = fopen("/private/tmp/claude-502/vp9rt/sym_a.txt", "w");
  parse_compressed(&r, &h, &probs);
  if (g_sym) { fclose(g_sym); g_sym = NULL; }
  static uint32_t orig_rec[8192], orig_n;
  memcpy(orig_rec, g_rec, sizeof(g_rec));
  orig_n = g_rec_n;
  if (getenv("VP9_TRACE"))
    fprintf(stderr, "type %u show %u intra %u er %u reset %u ref %u/%u/%u bias %u%u%u hp %u filt %u rctx %u fp %u idx %u tx %u refmode %u q %u lf %u seg %u hdr %u tiles %u/%u\n",
            h.frame_type, h.show_frame, h.intra_only, h.error_resilient, h.reset_frame_context, h.ref_frame_idx[0],
            h.ref_frame_idx[1], h.ref_frame_idx[2], h.ref_sign_bias[0], h.ref_sign_bias[1], h.ref_sign_bias[2],
            h.allow_high_precision_mv, h.interp_filter, h.refresh_frame_context, h.frame_parallel, h.frame_context_idx,
            h.tx_mode, h.reference_mode, h.base_q_idx, h.lf_level, h.seg_enabled, header_size, h.tile_cols_log2, h.tile_rows_log2);
  vp9_contexts_end(&g_in_ctx, &h, &probs, &in_base);
  for (uint32_t i = 0; i < VP9_REF_SLOTS; i++)
    if (h.refresh_frame_flags & (1u << i)) g_ref_size[i] = (Ref_Size){h.width, h.height};
  /* Rebuild. */
  Vp9_Probs base;
  vp9_contexts_begin(&g_out_ctx, &h, &g_defaults, &base);
  const size_t n = vp9_write_frame_headers(&h, &probs, &base, out, capacity);
  {
    /* Self-check: the rebuilt compressed header parses back to `probs`. */
    Bits b2 = {out, n, 0};
    Vp9_Frame_Header h2;
    memset(&h2, 0, sizeof(h2));
    g_persist_save = g_persist;
    Ref_Size saved_sizes[VP9_REF_SLOTS];
    memcpy(saved_sizes, g_ref_size, sizeof(saved_sizes));
    uint32_t hs2 = 0;
    (void)parse_uncompressed(&b2, &h2, &hs2);
    g_persist = g_persist_save;
    memcpy(g_ref_size, saved_sizes, sizeof(saved_sizes));
    const size_t head2 = (b2.pos + 7u) >> 3;
    Vp9_Probs p2 = base;
    Bools r2;
    memset(&r2, 0, sizeof(r2));
    r2.bits = (Bits){out + head2, hs2, 0};
    r2.value = f(&r2.bits, 8);
    r2.range = 255;
    r2.max_bits = (int32_t)(8 * hs2) - 8;
    (void)B(&r2, 128);
    g_rec_n = 0;
    if (getenv("VP9_SYMS") && atoi(getenv("VP9_SYMS")) == sym_frame) g_sym = fopen("/private/tmp/claude-502/vp9rt/sym_b.txt", "w");
    parse_compressed(&r2, &h2, &p2);
    if (g_sym) { fclose(g_sym); g_sym = NULL; }
    if (getenv("VP9_DELTAS")) {
      fprintf(stderr, "updates: original %u, rebuilt %u\n", orig_n, g_rec_n);
      for (uint32_t k = 0; k < orig_n && k < g_rec_n; k++)
        if (orig_rec[k] != g_rec[k]) fprintf(stderr, "  #%u: original d=%u base=%u -> %u, rebuilt d=%u base=%u -> %u\n", k,
                                             orig_rec[k] >> 16, (orig_rec[k] >> 8) & 255, orig_rec[k] & 255, g_rec[k] >> 16,
                                             (g_rec[k] >> 8) & 255, g_rec[k] & 255);
    }
    if (memcmp(&p2, &probs, sizeof(p2)) != 0 || head2 + hs2 != n) {
      const uint8_t *x = (const uint8_t *)&p2, *y = (const uint8_t *)&probs;
      size_t k = 0;
      while (k < sizeof(p2) && x[k] == y[k]) k++;
      fprintf(stderr, "self-check: byte %zu of the tables differs (%u vs %u), sizes %zu+%u vs %zu\n", k, k < sizeof(p2) ? x[k] : 0,
              k < sizeof(p2) ? y[k] : 0, head2, hs2, n);
    }
  }
  vp9_contexts_end(&g_out_ctx, &h, &probs, &base);
  const size_t tiles = size - head - header_size;
  if (!n || n + tiles > capacity) return 0;
  {
    /* VP9_ONLY=N: rebuild only frame N; the others are copied (bisection). */
    static int frame_no = -1;
    frame_no++;
    const char *only = getenv("VP9_ONLY");
    if (only && atoi(only) != frame_no) {
      memcpy(out, in, size);
      return size;
    }
  }
  memcpy(out + n, in + head + header_size, tiles);
  return n + tiles;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: vp9_roundtrip IN.ivf OUT.ivf\n");
    return 2;
  }
  vp9_default_probs(&g_defaults);
  FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
  if (!in || !out) return 2;
  uint8_t header[32];
  if (fread(header, 1, 32, in) != 32 || memcmp(header, "DKIF", 4) != 0) return 2;
  fwrite(header, 1, 32, out);
  static uint8_t packet[4 << 20], rebuilt[4 << 20], frame_out[4 << 20];
  uint8_t ph[12];
  uint32_t frames = 0;
  while (fread(ph, 1, 12, in) == 12) {
    const uint32_t size = rd32(ph);
    if (size > sizeof(packet) || fread(packet, 1, size, in) != size) return 3;
    /* Superframe index: the last byte 0b110xxyyy repeated at the start of the index. */
    const uint8_t marker = packet[size - 1];
    size_t offsets[8], sizes[8];
    uint32_t count = 1;
    offsets[0] = 0;
    sizes[0] = size;
    if ((marker & 0xE0u) == 0xC0u) {
      const uint32_t n = (marker & 7u) + 1u, mag = ((marker >> 3) & 3u) + 1u, index = 2u + mag * n;
      if (size >= index && packet[size - index] == marker) {
        size_t at = 0;
        count = n;
        for (uint32_t i = 0; i < n; i++) {
          uint32_t s = 0;
          for (uint32_t k = 0; k < mag; k++) s |= (uint32_t)packet[size - index + 1 + i * mag + k] << (8 * k);
          offsets[i] = at;
          sizes[i] = s;
          at += s;
        }
      }
    }
    size_t total = 0, out_sizes[8];
    for (uint32_t i = 0; i < count; i++) {
      const size_t n = rebuild(packet + offsets[i], sizes[i], frame_out, sizeof(frame_out));
      if (!n) {
        fprintf(stderr, "frame %u: rebuild failed\n", frames);
        return 4;
      }
      memcpy(rebuilt + total, frame_out, n);
      out_sizes[i] = n;
      total += n;
      frames++;
    }
    if (count > 1) { /* a new index, 4-byte sizes */
      const uint8_t m = (uint8_t)(0xC0u | (3u << 3) | (count - 1u));
      rebuilt[total++] = m;
      for (uint32_t i = 0; i < count; i++) { wr32(rebuilt + total, (uint32_t)out_sizes[i]); total += 4; }
      rebuilt[total++] = m;
    }
    wr32(ph, (uint32_t)total);
    fwrite(ph, 1, 12, out);
    fwrite(rebuilt, 1, total, out);
  }
  fclose(in);
  fclose(out);
  fprintf(stderr, "vp9_roundtrip: %u frames\n", frames);
  return 0;
}
