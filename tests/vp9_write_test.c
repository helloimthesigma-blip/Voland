/**
 * Unit tests for core/video/vp9_write.c, against a VP9 boolean decoder
 * written from the specification (9.2) here: random symbols, literals and
 * probability updates round-trip, every (old, new) probability pair has
 * a delta index, and the bit writer packs MSB first.
 */
#define CHECK_NAME "vp9_write_test"
#include "check.h"

#include "video/vp9_write.h"

#include <string.h>

typedef struct Bool_Reader {
  const uint8_t *data;
  size_t size, pos;
  uint32_t value, range;
  int32_t max_bits;
} Bool_Reader;

static uint32_t read_bit_raw(Bool_Reader *r) {
  const size_t byte = r->pos >> 3;
  const uint32_t bit = byte < r->size ? (r->data[byte] >> (7u - (r->pos & 7u))) & 1u : 0u;
  r->pos++;
  return bit;
}

static bool read_bool(Bool_Reader *r, uint8_t p) {
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
      next = read_bit_raw(r);
      r->max_bits--;
    }
    r->range <<= 1;
    r->value = (r->value << 1) | next;
  }
  return bit;
}

static void reader_init(Bool_Reader *r, const uint8_t *data, size_t size) {
  memset(r, 0, sizeof(*r));
  r->data = data;
  r->size = size;
  for (int i = 0; i < 8; i++) r->value = (r->value << 1) | read_bit_raw(r);
  r->range = 255;
  r->max_bits = (int32_t)(8 * size) - 8;
  CHECK(!read_bool(r, 128)); /* the marker bit */
}

static uint32_t read_literal(Bool_Reader *r, uint32_t n) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; i++) v = (v << 1) | (read_bool(r, 128) ? 1u : 0u);
  return v;
}

static uint32_t decode_term_subexp(Bool_Reader *r) {
  if (!read_literal(r, 1)) return read_literal(r, 4);
  if (!read_literal(r, 1)) return read_literal(r, 4) + 16u;
  if (!read_literal(r, 1)) return read_literal(r, 5) + 32u;
  const uint32_t v = read_literal(r, 7);
  if (v < 65u) return v + 64u;
  return (v << 1) - 1u + read_literal(r, 1);
}

static uint8_t read_prob_update(Bool_Reader *r, uint8_t old) {
  if (!read_bool(r, 252)) return old;
  return vp9_inv_remap_prob(decode_term_subexp(r), old);
}

static uint32_t g_seed = 12345;
static uint32_t rnd(void) {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 8;
}

static void test_bool_round_trip(void) {
  enum { N = 20000 };
  static bool bits[N];
  static uint8_t probs[N];
  static uint8_t buffer[N];
  Vp9_Bool_Writer w;
  vp9_bool_init(&w, buffer, sizeof(buffer));
  for (int i = 0; i < N; i++) {
    probs[i] = (uint8_t)(1u + rnd() % 255u);
    /* Mostly follow the probability so the coder compresses. */
    bits[i] = (rnd() % 256u) >= probs[i];
    vp9_bool_write(&w, bits[i], probs[i]);
  }
  vp9_bool_literal(&w, 0x5A5A5u, 20);
  const size_t size = vp9_bool_finish(&w);
  CHECK(size > 0 && size < sizeof(buffer));
  Bool_Reader r;
  reader_init(&r, buffer, size);
  for (int i = 0; i < N; i++) CHECK(read_bool(&r, probs[i]) == bits[i]);
  CHECK(read_literal(&r, 20) == 0x5A5A5u);
}

static void test_every_delta(void) {
  for (uint32_t old = 1; old <= 255; old++) {
    for (uint32_t updated = 1; updated <= 255; updated++) {
      if (updated == old) continue; /* an update always changes the probability */
      const uint32_t i = vp9_prob_delta_index((uint8_t)old, (uint8_t)updated);
      if (vp9_inv_remap_prob(i, (uint8_t)old) != updated) {
        CHECK(!"delta index missing");
        return;
      }
    }
  }
}

static void test_prob_updates(void) {
  static uint8_t buffer[65536];
  enum { N = 3000 };
  static uint8_t olds[N], news[N];
  Vp9_Bool_Writer w;
  vp9_bool_init(&w, buffer, sizeof(buffer));
  for (int i = 0; i < N; i++) {
    olds[i] = (uint8_t)(1u + rnd() % 255u);
    news[i] = (rnd() & 1u) ? olds[i] : (uint8_t)(1u + rnd() % 255u);
    vp9_write_prob_update(&w, olds[i], news[i]);
  }
  const size_t size = vp9_bool_finish(&w);
  Bool_Reader r;
  reader_init(&r, buffer, size);
  for (int i = 0; i < N; i++) CHECK(read_prob_update(&r, olds[i]) == news[i]);
}

static void test_bits(void) {
  uint8_t out[4];
  Vp9_Bits b;
  vp9_bits_init(&b, out, sizeof(out));
  vp9_bits_put(&b, 2, 2);        /* frame_marker */
  vp9_bits_put(&b, 0x49, 8);
  vp9_bits_put_signed(&b, -3, 4);
  CHECK(!b.overflow && vp9_bits_bytes(&b) == 2);
  CHECK(out[0] == 0x92 && out[1] == 0x4E);
  vp9_bits_put(&b, 0xFFFFFFFFu, 32);
  CHECK(b.overflow);
}

int main(void) {
  test_bool_round_trip();
  test_every_delta();
  test_prob_updates();
  test_bits();
  printf("[vp9_write_test] all tests passed\n");
  return 0;
}
