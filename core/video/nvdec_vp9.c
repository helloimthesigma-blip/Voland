#include "video/nvdec_vp9.h"

#include <stddef.h>
#include <string.h>

#define E_INTER_MODE 0x400u
#define E_IS_INTER 0x41Cu
#define E_UV_MODE 0x420u
#define E_TX8 0x470u
#define E_TX16 0x472u
#define E_TX32 0x476u
#define E_Y_MODE_LAST 0x47Cu
#define E_Y_MODE 0x480u
#define E_PARTITION 0x4E0u
#define E_UV_MODE_LAST 0x520u
#define E_INTERP_FILTER 0x52Au
#define E_COMP_MODE 0x532u
#define E_SKIP 0x537u
#define E_MV_JOINTS 0x53Bu
#define E_MV_SIGN 0x53Eu
#define E_MV_CLASS0 0x540u
#define E_MV_FR 0x542u
#define E_MV_CLASS0_HP 0x548u
#define E_MV_HP 0x54Au
#define E_MV_CLASSES 0x54Cu
#define E_MV_CLASS0_FR 0x560u
#define E_MV_BITS 0x56Cu
#define E_SINGLE_REF 0x580u
#define E_COMP_REF 0x58Au
#define E_COEF 0x5A0u
#define E_PADDED 4u /* inter_mode, partition and coefficient contexts are 4 bytes apart */

void nvdec_vp9_probs(const uint8_t e[NVDEC_VP9_ENTROPY_BYTES], Vp9_Probs *p) {
  memset(p, 0, sizeof(*p));
  for (uint32_t i = 0; i < 7; i++) memcpy(p->inter_mode[i], e + E_INTER_MODE + E_PADDED * i, 3);
  memcpy(p->is_inter, e + E_IS_INTER, sizeof(p->is_inter));
  for (uint32_t i = 0; i < 10; i++) {
    memcpy(p->uv_mode[i], e + E_UV_MODE + 8u * i, 8);
    p->uv_mode[i][8] = e[E_UV_MODE_LAST + i];
  }
  memcpy(p->tx8, e + E_TX8, sizeof(p->tx8));
  memcpy(p->tx16, e + E_TX16, sizeof(p->tx16));
  memcpy(p->tx32, e + E_TX32, sizeof(p->tx32));
  for (uint32_t i = 0; i < 4; i++) {
    memcpy(p->y_mode[i], e + E_Y_MODE + 8u * i, 8);
    p->y_mode[i][8] = e[E_Y_MODE_LAST + i];
  }
  for (uint32_t i = 0; i < 16; i++) memcpy(p->partition[i], e + E_PARTITION + E_PADDED * i, 3);
  memcpy(p->interp_filter, e + E_INTERP_FILTER, sizeof(p->interp_filter));
  memcpy(p->comp_mode, e + E_COMP_MODE, sizeof(p->comp_mode));
  memcpy(p->skip, e + E_SKIP, sizeof(p->skip));
  memcpy(p->mv_joints, e + E_MV_JOINTS, sizeof(p->mv_joints));
  memcpy(p->mv_sign, e + E_MV_SIGN, sizeof(p->mv_sign));
  for (uint32_t i = 0; i < 2; i++) p->mv_class0[i][0] = e[E_MV_CLASS0 + i];
  memcpy(p->mv_fr, e + E_MV_FR, sizeof(p->mv_fr));
  memcpy(p->mv_class0_hp, e + E_MV_CLASS0_HP, sizeof(p->mv_class0_hp));
  memcpy(p->mv_hp, e + E_MV_HP, sizeof(p->mv_hp));
  memcpy(p->mv_classes, e + E_MV_CLASSES, sizeof(p->mv_classes));
  memcpy(p->mv_class0_fr, e + E_MV_CLASS0_FR, sizeof(p->mv_class0_fr));
  memcpy(p->mv_bits, e + E_MV_BITS, sizeof(p->mv_bits));
  memcpy(p->single_ref, e + E_SINGLE_REF, sizeof(p->single_ref));
  memcpy(p->comp_ref, e + E_COMP_REF, sizeof(p->comp_ref));
  const uint8_t *c = e + E_COEF;
  for (uint32_t tx = 0; tx < 4; tx++)
    for (uint32_t i = 0; i < 2; i++)
      for (uint32_t j = 0; j < 2; j++)
        for (uint32_t k = 0; k < 6; k++)
          for (uint32_t l = 0; l < 6; l++) {
            if (k > 0 || l < 3) memcpy(p->coef[tx][i][j][k][l], c, 3);
            c += E_PADDED;
          }
}

/* ---- picture setup (offsets in bytes; nvdec_vp9.h) ----------------- */

#define S_BITSTREAM_BYTES 0x30u
#define S_FRAME_SIZE 0x60u
#define S_FLAGS 0x68u
#define S_FLAG_KEY 0x01u
#define S_FLAG_PREVIOUS_SHOWN 0x10u
#define S_SIGN_BIAS 0x6Cu
#define S_LF_LEVEL 0x70u
#define S_LF_SHARPNESS 0x71u
#define S_BASE_Q 0x72u
#define S_DELTA_Y_DC 0x73u
#define S_DELTA_UV_AC 0x74u
#define S_DELTA_UV_DC 0x75u
#define S_TX_MODE 0x77u
#define S_ALLOW_HP 0x78u
#define S_INTERP_FILTER 0x79u
#define S_REFERENCE_MODE 0x7Au
#define S_TILE_COLS_LOG2 0x7Eu
#define S_TILE_ROWS_LOG2 0x7Fu
#define S_SEGMENTATION 0x80u
#define S_SEGMENTATION_BYTES 0x64u
#define S_LF_DELTA_ENABLED 0xE4u
#define S_LF_REF_DELTAS 0xE5u
#define S_LF_MODE_DELTAS 0xE9u
#define VP9_CS_BT709 2u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

uint32_t nvdec_vp9_bitstream_bytes(const uint8_t s[NVDEC_VP9_SETUP_BYTES]) { return rd32(s + S_BITSTREAM_BYTES); }
bool nvdec_vp9_key_frame(const uint8_t s[NVDEC_VP9_SETUP_BYTES]) { return (s[S_FLAGS] & S_FLAG_KEY) != 0; }
bool nvdec_vp9_previous_shown(const uint8_t s[NVDEC_VP9_SETUP_BYTES]) { return (s[S_FLAGS] & S_FLAG_PREVIOUS_SHOWN) != 0; }

int32_t nvdec_vp9_slot_of(const Nvdec_Vp9_State *st, uint32_t surface) {
  for (uint32_t i = 0; i < VP9_REF_SLOTS; i++)
    if (st->slot_surface[i] == surface && surface) return (int32_t)i;
  return -1;
}

bool nvdec_vp9_header(const uint8_t s[NVDEC_VP9_SETUP_BYTES], const uint32_t surfaces[NVDEC_VP9_SURFACES], bool show_frame,
                      Nvdec_Vp9_State *st, Vp9_Frame_Header *h) {
  memset(h, 0, sizeof(*h));
  for (uint32_t i = 0; i < S_SEGMENTATION_BYTES; i++)
    if (s[S_SEGMENTATION + i]) return false;
  h->width = rd16(s + S_FRAME_SIZE);
  h->height = rd16(s + S_FRAME_SIZE + 2u);
  if (!h->width || !h->height) return false;
  h->frame_type = nvdec_vp9_key_frame(s) ? VP9_KEY_FRAME : VP9_NON_KEY_FRAME;
  h->show_frame = show_frame;
  h->bit_depth = 8;
  h->color_space = VP9_CS_BT709;
  h->subsampling_x = h->subsampling_y = true;
  /* Every frame refreshes the context it started from (the caller picks
   * it: vp9_closest_context), with no backward adaptation. */
  h->refresh_frame_context = true;
  h->frame_parallel = true;
  h->frame_context_idx = 0;
  h->lf_level = s[S_LF_LEVEL];
  h->lf_sharpness = s[S_LF_SHARPNESS];
  h->lf_delta_enabled = s[S_LF_DELTA_ENABLED] != 0;
  h->lf_delta_update = h->lf_delta_enabled; /* restated every frame */
  for (uint32_t i = 0; i < 4; i++) {
    h->lf_ref_update[i] = h->lf_delta_enabled;
    h->lf_ref_deltas[i] = (int8_t)s[S_LF_REF_DELTAS + i];
  }
  for (uint32_t i = 0; i < 2; i++) {
    h->lf_mode_update[i] = h->lf_delta_enabled;
    h->lf_mode_deltas[i] = (int8_t)s[S_LF_MODE_DELTAS + i];
  }
  h->base_q_idx = s[S_BASE_Q];
  h->delta_q_y_dc = (int8_t)s[S_DELTA_Y_DC];
  h->delta_q_uv_ac = (int8_t)s[S_DELTA_UV_AC];
  h->delta_q_uv_dc = (int8_t)s[S_DELTA_UV_DC];
  h->tx_mode = s[S_TX_MODE];
  h->tile_cols_log2 = s[S_TILE_COLS_LOG2];
  h->tile_rows_log2 = s[S_TILE_ROWS_LOG2];
  st->stamp++;
  if (h->frame_type == VP9_KEY_FRAME) {
    h->refresh_frame_flags = 0xFFu;
    for (uint32_t i = 0; i < VP9_REF_SLOTS; i++) {
      st->slot_surface[i] = surfaces[NVDEC_VP9_OUTPUT];
      st->slot_stamp[i] = st->stamp;
    }
    return true;
  }
  h->allow_high_precision_mv = s[S_ALLOW_HP] != 0;
  h->interp_filter = s[S_INTERP_FILTER];
  h->reference_mode = s[S_REFERENCE_MODE];
  uint32_t used = 0;
  for (uint32_t i = 0; i < VP9_REFS; i++) {
    const int32_t slot = nvdec_vp9_slot_of(st, surfaces[i]);
    if (slot < 0) return false;
    h->ref_frame_idx[i] = (uint32_t)slot;
    h->ref_sign_bias[i] = s[S_SIGN_BIAS + 1u + i] != 0;
    st->slot_stamp[slot] = st->stamp;
    used |= 1u << slot;
  }
  /* The picture replaces whatever slots held its surface before (the
   * guest reuses surfaces: SSBU decodes every hidden altref frame into the
   * same one), else the least recently used slot no reference uses. */
  uint32_t refresh = 0;
  for (uint32_t i = 0; i < VP9_REF_SLOTS; i++)
    if (st->slot_surface[i] == surfaces[NVDEC_VP9_OUTPUT] && !(used & (1u << i))) refresh |= 1u << i;
  if (!refresh) {
    int32_t victim = -1;
    for (uint32_t i = 0; i < VP9_REF_SLOTS; i++) {
      if (used & (1u << i)) continue;
      if (victim < 0 || st->slot_stamp[i] < st->slot_stamp[victim]) victim = (int32_t)i;
    }
    refresh = 1u << victim;
  }
  h->refresh_frame_flags = refresh;
  for (uint32_t i = 0; i < VP9_REF_SLOTS; i++) {
    if (!(refresh & (1u << i))) continue;
    st->slot_surface[i] = surfaces[NVDEC_VP9_OUTPUT];
    st->slot_stamp[i] = st->stamp;
  }
  return true;
}

/* ---- the stream ------------------------------------------------------- */

void nvdec_vp9_stream_init(Nvdec_Vp9_Stream *s) {
  memset(s, 0, offsetof(Nvdec_Vp9_Stream, tiles));
  s->held_bytes = 0;
  s->held_count = 0;
  vp9_default_probs(&s->defaults);
}

static void put_le32(uint8_t *p, uint32_t v) {
  for (uint32_t k = 0; k < 4u; k++) p[k] = (uint8_t)(v >> (8u * k));
}

/* Rebuilds the pending frame with `shown`; see nvdec_vp9_push. */
static int complete_pending(Nvdec_Vp9_Stream *s, bool shown, Nvdec_Vp9_Chunk *chunk) {
  if (!s->pending) return 0;
  s->pending = false;
  memset(chunk, 0, sizeof(*chunk));
  const bool key = nvdec_vp9_key_frame(s->setup);
  const uint32_t width = rd16(s->setup + S_FRAME_SIZE), height = rd16(s->setup + S_FRAME_SIZE + 2u);
  if (key) {
    chunk->new_stream = !s->started || width != s->width || height != s->height;
    s->started = true;
    s->width = width;
    s->height = height;
    s->held_bytes = 0; /* a key frame starts over */
    s->held_count = 0;
    if (chunk->new_stream) {
      memset(&s->contexts, 0, sizeof(s->contexts));
      memset(&s->state, 0, sizeof(s->state));
    }
  }
  if (!s->started) return 0; /* no key frame yet */
  Vp9_Frame_Header h;
  if (!nvdec_vp9_header(s->setup, s->surfaces, shown, &s->state, &h)) return -1;
  Vp9_Probs probs, base;
  nvdec_vp9_probs(s->entropy, &probs);
  if (h.frame_type != VP9_KEY_FRAME) h.frame_context_idx = vp9_closest_context(&s->contexts, &probs);
  vp9_contexts_begin(&s->contexts, &h, &s->defaults, &base);
  uint8_t *frame = s->chunk + s->held_bytes;
  const size_t room = sizeof(s->chunk) - s->held_bytes - NVDEC_VP9_SUPERFRAME_INDEX;
  const size_t head = vp9_write_frame_headers(&h, &probs, &base, frame, room - s->tile_bytes);
  vp9_contexts_end(&s->contexts, &h, &probs, &base);
  if (!head) return -1;
  memcpy(frame + head, s->tiles, s->tile_bytes);
  const uint32_t bytes = (uint32_t)head + s->tile_bytes;
  if (!shown) {
    if (s->held_bytes + bytes > sizeof(s->held) || s->held_count >= NVDEC_VP9_SUPERFRAME_FRAMES - 1u) return -1;
    memcpy(s->held + s->held_bytes, frame, bytes);
    s->held_bytes += bytes;
    s->held_size[s->held_count++] = bytes;
    return 0;
  }
  uint32_t total = bytes;
  if (s->held_count) {
    memcpy(s->chunk, s->held, s->held_bytes); /* this frame already sits right after them */
    const uint32_t frames = s->held_count + 1u;
    uint8_t *ix = s->chunk + s->held_bytes + bytes;
    const uint8_t marker = (uint8_t)(0xC0u | (3u << 3) | (frames - 1u)); /* superframe, 4-byte sizes */
    ix[0] = marker;
    for (uint32_t i = 0; i < s->held_count; i++) put_le32(ix + 1u + 4u * i, s->held_size[i]);
    put_le32(ix + 1u + 4u * s->held_count, bytes);
    ix[1u + 4u * frames] = marker;
    total = s->held_bytes + bytes + 2u + 4u * frames;
    s->held_bytes = 0;
    s->held_count = 0;
  }
  chunk->data = s->chunk;
  chunk->bytes = total;
  chunk->key = key;
  chunk->width = width;
  chunk->height = height;
  chunk->surface = s->surfaces[NVDEC_VP9_OUTPUT];
  return 1;
}

int nvdec_vp9_push(Nvdec_Vp9_Stream *s, const uint8_t setup[NVDEC_VP9_SETUP_BYTES],
                   const uint8_t entropy[NVDEC_VP9_ENTROPY_BYTES], const uint32_t surfaces[NVDEC_VP9_SURFACES],
                   const uint8_t *tiles, uint32_t tile_bytes, Nvdec_Vp9_Chunk *chunk) {
  const int done = complete_pending(s, nvdec_vp9_previous_shown(setup), chunk);
  if (tile_bytes > sizeof(s->tiles)) return done != 0 ? done : -1;
  memcpy(s->setup, setup, sizeof(s->setup));
  memcpy(s->entropy, entropy, sizeof(s->entropy));
  memcpy(s->surfaces, surfaces, sizeof(s->surfaces));
  memcpy(s->tiles, tiles, tile_bytes);
  s->tile_bytes = tile_bytes;
  s->pending = true;
  return done;
}

int nvdec_vp9_flush(Nvdec_Vp9_Stream *s, Nvdec_Vp9_Chunk *chunk) { return complete_pending(s, true, chunk); }

bool nvdec_vp9_show_existing(const Nvdec_Vp9_Stream *s, uint32_t surface, uint8_t *out) {
  const int32_t slot = nvdec_vp9_slot_of(&s->state, surface);
  if (slot < 0) return false;
  out[0] = (uint8_t)(0x88u | (uint32_t)slot); /* frame_marker 2, profile 0, show_existing_frame, slot */
  return true;
}
