#include "gpu/bc7.h"

#include <stdbool.h>
#include <string.h>

#include "gpu/bc7_tables.inc"

#define BC7_MODES 8u
#define BC7_MAX_SUBSETS 3u
#define BC7_CHANNELS 4u
#define BC7_WEIGHT_SCALE 64u
#define BC7_WEIGHT_ROUND 32u
#define BC7_WEIGHT_SHIFT 6u

/* Per mode: subsets, partition bits, rotation bits, index-selection bit,
 * colour bits, alpha bits, per-endpoint p-bits, shared p-bits, index bits,
 * secondary index bits. */
typedef struct Bc7_Mode {
  uint8_t subsets, partition_bits, rotation_bits, selection_bit;
  uint8_t color_bits, alpha_bits, endpoint_pbits, shared_pbits;
  uint8_t index_bits, index2_bits;
} Bc7_Mode;

static const Bc7_Mode k_modes[BC7_MODES] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

static const uint8_t k_weights2[4] = {0, 21, 43, 64};
static const uint8_t k_weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t k_weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct Bit_Reader {
  const uint8_t *bytes;
  uint32_t at;
} Bit_Reader;

/* `count` bits, least significant first (the block is little-endian). */
static uint32_t read_bits(Bit_Reader *r, uint32_t count) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < count; i++, r->at++) value |= (uint32_t)((r->bytes[r->at >> 3] >> (r->at & 7u)) & 1u) << i;
  return value;
}

/* An n-bit value widened to 8 bits by replicating its top bits. */
static uint8_t widen(uint32_t value, uint32_t bits) {
  if (bits >= 8u) return (uint8_t)value;
  value <<= 8u - bits;
  return (uint8_t)(value | (value >> bits));
}

static const uint8_t *weights_for(uint32_t bits) {
  return bits == 2u ? k_weights2 : bits == 3u ? k_weights3 : k_weights4;
}

static uint8_t interpolate(uint8_t e0, uint8_t e1, uint32_t weight) {
  return (uint8_t)(((BC7_WEIGHT_SCALE - weight) * e0 + weight * e1 + BC7_WEIGHT_ROUND) >> BC7_WEIGHT_SHIFT);
}

static uint32_t subset_of(const Bc7_Mode *m, uint32_t partition, uint32_t texel) {
  if (m->subsets == 2u) return k_partition2[partition][texel];
  if (m->subsets == 3u) return k_partition3[partition][texel];
  return 0;
}

/* Anchor texels store one index bit fewer (their top bit is 0). */
static bool is_anchor(const Bc7_Mode *m, uint32_t partition, uint32_t texel) {
  if (texel == 0) return true;
  if (m->subsets == 2u) return texel == k_anchor2[partition];
  if (m->subsets == 3u) return texel == k_anchor3_second[partition] || texel == k_anchor3_third[partition];
  return false;
}

void bc7_decode_block(const uint8_t block[BC7_BLOCK_BYTES], uint8_t out[BC7_TEXELS][4]) {
  uint32_t mode = 0;
  while (mode < BC7_MODES && !((block[0] >> mode) & 1u)) mode++;
  if (mode == BC7_MODES) {
    memset(out, 0, BC7_TEXELS * 4u);
    return;
  }
  const Bc7_Mode *m = &k_modes[mode];
  Bit_Reader r = {block, mode + 1u};
  const uint32_t partition = read_bits(&r, m->partition_bits);
  const uint32_t rotation = read_bits(&r, m->rotation_bits);
  const uint32_t selection = read_bits(&r, m->selection_bit);

  /* Endpoints: per channel, per subset, two values. */
  const uint32_t endpoints = 2u * m->subsets;
  uint32_t raw[BC7_MAX_SUBSETS * 2u][BC7_CHANNELS];
  memset(raw, 0, sizeof(raw));
  for (uint32_t ch = 0; ch < 3u; ch++)
    for (uint32_t e = 0; e < endpoints; e++) raw[e][ch] = read_bits(&r, m->color_bits);
  if (m->alpha_bits)
    for (uint32_t e = 0; e < endpoints; e++) raw[e][3] = read_bits(&r, m->alpha_bits);

  /* P-bits extend every channel of an endpoint by one low bit. */
  uint32_t color_bits = m->color_bits, alpha_bits = m->alpha_bits;
  if (m->endpoint_pbits || m->shared_pbits) {
    uint32_t pbits[BC7_MAX_SUBSETS * 2u];
    if (m->endpoint_pbits) {
      for (uint32_t e = 0; e < endpoints; e++) pbits[e] = read_bits(&r, 1);
    } else {
      for (uint32_t s = 0; s < m->subsets; s++) pbits[2u * s] = pbits[2u * s + 1u] = read_bits(&r, 1);
    }
    for (uint32_t e = 0; e < endpoints; e++)
      for (uint32_t ch = 0; ch < BC7_CHANNELS; ch++) raw[e][ch] = (raw[e][ch] << 1) | pbits[e];
    color_bits++;
    if (alpha_bits) alpha_bits++;
  }
  uint8_t ends[BC7_MAX_SUBSETS * 2u][BC7_CHANNELS];
  for (uint32_t e = 0; e < endpoints; e++) {
    for (uint32_t ch = 0; ch < 3u; ch++) ends[e][ch] = widen(raw[e][ch], color_bits);
    ends[e][3] = alpha_bits ? widen(raw[e][3], alpha_bits) : 255u;
  }

  /* Indices: the primary set, then (modes 4 and 5) the secondary set. */
  uint8_t index1[BC7_TEXELS], index2[BC7_TEXELS];
  for (uint32_t t = 0; t < BC7_TEXELS; t++)
    index1[t] = (uint8_t)read_bits(&r, m->index_bits - (is_anchor(m, partition, t) ? 1u : 0u));
  for (uint32_t t = 0; t < BC7_TEXELS && m->index2_bits; t++)
    index2[t] = (uint8_t)read_bits(&r, m->index2_bits - (t == 0 ? 1u : 0u));

  for (uint32_t t = 0; t < BC7_TEXELS; t++) {
    const uint32_t s = subset_of(m, partition, t);
    const uint8_t *e0 = ends[2u * s], *e1 = ends[2u * s + 1u];
    uint32_t color_weight, alpha_weight;
    if (m->index2_bits) {
      /* Mode 4 swaps which set (2- or 3-bit) drives colour and alpha. */
      const bool swap = selection != 0;
      const uint32_t color_bits_used = swap ? m->index2_bits : m->index_bits;
      const uint32_t alpha_bits_used = swap ? m->index_bits : m->index2_bits;
      color_weight = weights_for(color_bits_used)[swap ? index2[t] : index1[t]];
      alpha_weight = weights_for(alpha_bits_used)[swap ? index1[t] : index2[t]];
    } else {
      color_weight = alpha_weight = weights_for(m->index_bits)[index1[t]];
    }
    for (uint32_t ch = 0; ch < 3u; ch++) out[t][ch] = interpolate(e0[ch], e1[ch], color_weight);
    out[t][3] = interpolate(e0[3], e1[3], alpha_weight);
    if (rotation) { /* 1: alpha<->red, 2: alpha<->green, 3: alpha<->blue */
      const uint8_t tmp = out[t][3];
      out[t][3] = out[t][rotation - 1u];
      out[t][rotation - 1u] = tmp;
    }
  }
}
