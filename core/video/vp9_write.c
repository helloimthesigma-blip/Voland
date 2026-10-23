#include "video/vp9_write.h"

#include <string.h>

#define VP9_UPDATE_PROB 252u
#define VP9_HALF_PROB 128u
#define VP9_MAX_PROB 255u
#define VP9_PADDING_BITS 32u

void vp9_bits_init(Vp9_Bits *b, uint8_t *data, size_t capacity) {
  b->data = data;
  b->capacity = capacity;
  b->bit = 0;
  b->overflow = false;
  memset(data, 0, capacity);
}

void vp9_bits_put(Vp9_Bits *b, uint32_t value, uint32_t count) {
  for (uint32_t i = count; i-- > 0;) {
    const size_t byte = b->bit >> 3;
    if (byte >= b->capacity) {
      b->overflow = true;
      return;
    }
    if ((value >> i) & 1u) b->data[byte] |= (uint8_t)(0x80u >> (b->bit & 7u));
    b->bit++;
  }
}

void vp9_bits_put_signed(Vp9_Bits *b, int32_t value, uint32_t count) {
  const uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
  vp9_bits_put(b, magnitude, count);
  vp9_bits_put(b, value < 0 ? 1u : 0u, 1u);
}

size_t vp9_bits_bytes(const Vp9_Bits *b) { return (b->bit + 7u) >> 3; }

/* ---- boolean encoder (the decoder of spec 9.2, run backwards) ------- */

void vp9_bool_init(Vp9_Bool_Writer *w, uint8_t *data, size_t capacity) {
  w->data = data;
  w->capacity = capacity;
  w->pos = 0;
  w->low = 0;
  w->range = 255;
  w->count = 24;
  w->overflow = false;
  /* The marker bit the decoder reads (and requires to be 0) first. */
  vp9_bool_write(w, false, VP9_HALF_PROB);
}

static void carry(Vp9_Bool_Writer *w) {
  size_t at = w->pos;
  while (at > 0 && w->data[at - 1] == 0xFFu) w->data[--at] = 0;
  if (at > 0) w->data[at - 1]++;
}

static void emit(Vp9_Bool_Writer *w, uint8_t byte) {
  if (w->pos >= w->capacity) {
    w->overflow = true;
    return;
  }
  w->data[w->pos++] = byte;
}

void vp9_bool_write(Vp9_Bool_Writer *w, bool bit, uint8_t probability) {
  const uint32_t split = 1u + (((w->range - 1u) * probability) >> 8);
  if (bit) {
    w->low += split;
    w->range -= split;
  } else {
    w->range = split;
  }
  while (w->range < 128u) {
    w->range <<= 1;
    if (w->low & 0x80000000u) carry(w);
    w->low <<= 1;
    if (--w->count == 0) {
      emit(w, (uint8_t)(w->low >> 24));
      w->low &= 0xFFFFFFu;
      w->count = 8;
    }
  }
}

void vp9_bool_literal(Vp9_Bool_Writer *w, uint32_t value, uint32_t count) {
  for (uint32_t i = count; i-- > 0;) vp9_bool_write(w, (value >> i) & 1u, VP9_HALF_PROB);
}

size_t vp9_bool_finish(Vp9_Bool_Writer *w) {
  /* Zero padding the decoder may read past the last symbol. */
  for (uint32_t i = 0; i < VP9_PADDING_BITS; i++) vp9_bool_write(w, false, VP9_HALF_PROB);
  int32_t c = w->count;
  uint32_t v = w->low;
  if (c < 32 && (v & (1u << (32 - c)))) carry(w);
  v <<= c & 7;
  c >>= 3;
  while (--c >= 0) v <<= 8;
  for (c = 0; c < 4; c++) {
    emit(w, (uint8_t)(v >> 24));
    v <<= 8;
  }
  return w->overflow ? 0 : w->pos;
}

/* ---- probability deltas (spec 6.3.5 / 9.2.2) ------------------------ */

#define VP9_MAP_STEP 13u
#define VP9_MAP_FIRST 7u
#define VP9_MAP_LEADING 20u

/* inv_map_table: 7, 20, ... 254 (every 13th from 7), then every other
 * value from 1 to 253 in order. */
static uint8_t inv_map(uint32_t index) {
  if (index < VP9_MAP_LEADING) return (uint8_t)(VP9_MAP_FIRST + VP9_MAP_STEP * index);
  uint32_t seen = VP9_MAP_LEADING;
  for (uint32_t v = 1; v < VP9_MAX_PROB; v++) {
    if (v >= VP9_MAP_FIRST && (v - VP9_MAP_FIRST) % VP9_MAP_STEP == 0) continue;
    if (seen == index) return (uint8_t)v;
    seen++;
  }
  return 0;
}

static uint32_t inv_recenter_nonneg(uint32_t v, uint32_t m) {
  if (v > 2u * m) return v;
  return (v & 1u) ? m - ((v + 1u) >> 1) : m + (v >> 1);
}

uint8_t vp9_inv_remap_prob(uint32_t index, uint8_t old) {
  const uint32_t v = inv_map(index);
  const uint32_t m = (uint32_t)old - 1u;
  if ((m << 1) <= VP9_MAX_PROB) return (uint8_t)(1u + inv_recenter_nonneg(v, m));
  return (uint8_t)(VP9_MAX_PROB - inv_recenter_nonneg(v, VP9_MAX_PROB - 1u - m));
}

uint32_t vp9_prob_delta_index(uint8_t old, uint8_t updated) {
  for (uint32_t i = 0; i < VP9_DELTA_INDICES; i++)
    if (vp9_inv_remap_prob(i, old) == updated) return i;
  return 0;
}

/* decode_term_subexp, run backwards. */
static void write_term_subexp(Vp9_Bool_Writer *w, uint32_t v) {
  if (v < 16u) {
    vp9_bool_literal(w, 0, 1);
    vp9_bool_literal(w, v, 4);
  } else if (v < 32u) {
    vp9_bool_literal(w, 2, 2);
    vp9_bool_literal(w, v - 16u, 4);
  } else if (v < 64u) {
    vp9_bool_literal(w, 6, 3);
    vp9_bool_literal(w, v - 32u, 5);
  } else {
    vp9_bool_literal(w, 7, 3);
    const uint32_t rest = v - 64u;
    if (rest < 65u) {
      vp9_bool_literal(w, rest, 7);
    } else { /* (x << 1) - 1 + bit == rest + 64 */
      const uint32_t x = (rest + 1u + 64u) >> 1, bit = (rest + 1u + 64u) & 1u;
      vp9_bool_literal(w, x, 7);
      vp9_bool_literal(w, bit, 1);
    }
  }
}

void vp9_write_prob_update(Vp9_Bool_Writer *w, uint8_t old, uint8_t updated) {
  const bool update = old != updated;
  vp9_bool_write(w, update, VP9_UPDATE_PROB);
  if (update) write_term_subexp(w, vp9_prob_delta_index(old, updated));
}

void vp9_write_mv_prob_update(Vp9_Bool_Writer *w, uint8_t old, uint8_t updated) {
  const bool update = old != updated;
  vp9_bool_write(w, update, VP9_UPDATE_PROB);
  if (update) vp9_bool_literal(w, updated >> 1, 7);
}
