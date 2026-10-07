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

/* ---- BC6H (BPTC float) ------------------------------------------- */
/* Khronos Data Format Specification 1.3 §20.2: 14 modes (Table 121),
 * their block layouts (Table 122, transcribed below in block order),
 * two-subset partitions shared with BC7, 3- or 4-bit indices, endpoint
 * unquantisation and the final 31/64 (unsigned) or 31/32 (signed)
 * scaling to a half float. */

#define BC6H_MODES 14u
#define BC6H_MAX_FIELDS 23u
#define BC6H_ENDPOINTS 4u /* E0..E3: subset 0's two, then subset 1's */
#define BC6H_CHANNELS 3u
#define BC6H_PARTITION_VAR 12u
#define BC6H_ONE_SUBSET_INDEX_BITS 4u
#define BC6H_TWO_SUBSET_INDEX_BITS 3u
#define BC6H_HALF_MAX_UNSIGNED 0xFFFFu
#define BC6H_HALF_MAX_SIGNED 0x7FFFu

/* One field: variable (endpoint * 3 + channel, or BC6H_PARTITION_VAR) and
 * its bits a..b - the first block bit goes to bit b, then towards a. */
typedef struct Bc6h_Field {
  uint8_t var, a, b;
} Bc6h_Field;

typedef struct Bc6h_Mode {
  uint8_t number;           /* the mode value in the low 2 or 5 bits */
  uint8_t mode_bits;        /* 2 or 5 */
  bool transformed;
  uint8_t partition_bits;   /* 5: two subsets; 0: one */
  uint8_t endpoint_bits;    /* EPB */
  uint8_t delta_bits[3];    /* R, G, B */
  uint8_t field_count;
} Bc6h_Mode;

static const Bc6h_Mode k_bc6h_modes[BC6H_MODES] = {
    {0, 2, true, 5, 10, {5, 5, 5}, 19},  {1, 2, true, 5, 7, {6, 6, 6}, 23},   {2, 5, true, 5, 11, {5, 4, 4}, 18},
    {6, 5, true, 5, 11, {4, 5, 4}, 20},  {10, 5, true, 5, 11, {4, 4, 5}, 20}, {14, 5, true, 5, 9, {5, 5, 5}, 19},
    {18, 5, true, 5, 8, {6, 5, 5}, 19},  {22, 5, true, 5, 8, {5, 6, 5}, 21},  {26, 5, true, 5, 8, {5, 5, 6}, 21},
    {30, 5, false, 5, 6, {6, 6, 6}, 23}, {3, 5, false, 0, 10, {10, 10, 10}, 6}, {7, 5, true, 0, 11, {9, 9, 9}, 9},
    {11, 5, true, 0, 12, {8, 8, 8}, 9},  {15, 5, true, 0, 16, {4, 4, 4}, 9},
};

/* Table 122, the fields after the mode bits (partition bits as var 12). */
static const Bc6h_Field k_bc6h_fields[BC6H_MODES][BC6H_MAX_FIELDS] = {
    {{7, 4, 4}, {8, 4, 4}, {11, 4, 4}, {0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}}, /* mode 0 */
    {{7, 5, 5}, {10, 4, 4}, {10, 5, 5}, {0, 6, 0}, {11, 0, 0}, {11, 1, 1}, {8, 4, 4}, {1, 6, 0}, {8, 5, 5}, {11, 2, 2}, {7, 4, 4}, {2, 6, 0}, {11, 3, 3}, {11, 5, 5}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}}, /* mode 1 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 4, 0}, {0, 10, 10}, {7, 3, 0}, {4, 3, 0}, {1, 10, 10}, {11, 0, 0}, {10, 3, 0}, {5, 3, 0}, {2, 10, 10}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}}, /* mode 2 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 10}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {1, 10, 10}, {10, 3, 0}, {5, 3, 0}, {2, 10, 10}, {11, 1, 1}, {8, 3, 0}, {6, 3, 0}, {11, 0, 0}, {11, 2, 2}, {9, 3, 0}, {7, 4, 4}, {11, 3, 3}}, /* mode 6 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 10}, {8, 4, 4}, {7, 3, 0}, {4, 3, 0}, {1, 10, 10}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {2, 10, 10}, {8, 3, 0}, {6, 3, 0}, {11, 1, 1}, {11, 2, 2}, {9, 3, 0}, {11, 4, 4}, {11, 3, 3}}, /* mode 10 */
    {{0, 8, 0}, {8, 4, 4}, {1, 8, 0}, {7, 4, 4}, {2, 8, 0}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}}, /* mode 14 */
    {{0, 7, 0}, {10, 4, 4}, {8, 4, 4}, {1, 7, 0}, {11, 2, 2}, {7, 4, 4}, {2, 7, 0}, {11, 3, 3}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}}, /* mode 18 */
    {{0, 7, 0}, {11, 0, 0}, {8, 4, 4}, {1, 7, 0}, {7, 5, 5}, {7, 4, 4}, {2, 7, 0}, {10, 5, 5}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}}, /* mode 22 */
    {{0, 7, 0}, {11, 1, 1}, {8, 4, 4}, {1, 7, 0}, {8, 5, 5}, {7, 4, 4}, {2, 7, 0}, {11, 5, 5}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}}, /* mode 26 */
    {{0, 5, 0}, {10, 4, 4}, {11, 0, 0}, {11, 1, 1}, {8, 4, 4}, {1, 5, 0}, {7, 5, 5}, {8, 5, 5}, {11, 2, 2}, {7, 4, 4}, {2, 5, 0}, {10, 5, 5}, {11, 3, 3}, {11, 5, 5}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}}, /* mode 30 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 9, 0}, {4, 9, 0}, {5, 9, 0}}, /* mode 3 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 8, 0}, {0, 10, 10}, {4, 8, 0}, {1, 10, 10}, {5, 8, 0}, {2, 10, 10}}, /* mode 7 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 7, 0}, {0, 10, 11}, {4, 7, 0}, {1, 10, 11}, {5, 7, 0}, {2, 10, 11}}, /* mode 11 */
    {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 15}, {4, 3, 0}, {1, 10, 15}, {5, 3, 0}, {2, 10, 15}}, /* mode 15 */
};

static int32_t sign_extend(uint32_t value, uint32_t bits) {
  return (int32_t)(value << (32u - bits)) >> (32u - bits);
}

static int32_t bc6h_unquantize(int32_t x, uint32_t epb, bool is_signed) {
  if (!is_signed) {
    if (epb >= 15u) return x;
    if (x == 0) return 0;
    if (x == (int32_t)((1u << epb) - 1u)) return (int32_t)BC6H_HALF_MAX_UNSIGNED;
    return (int32_t)((((uint32_t)x << 15) + 0x4000u) >> (epb - 1u));
  }
  if (epb >= 16u) return x;
  const bool negative = x < 0;
  int32_t magnitude = negative ? -x : x;
  int32_t unq;
  if (magnitude == 0) unq = 0;
  else if (magnitude >= (int32_t)((1u << (epb - 1u)) - 1u)) unq = (int32_t)BC6H_HALF_MAX_SIGNED;
  else unq = (int32_t)((((uint32_t)magnitude << 15) + 0x4000u) >> (epb - 1u));
  return negative ? -unq : unq;
}

static uint16_t bc6h_finish(int32_t i, bool is_signed) {
  if (!is_signed) return (uint16_t)((i * 31) >> 6);
  return i < 0 ? (uint16_t)((((-i) * 31) >> 5) | 0x8000) : (uint16_t)((i * 31) >> 5);
}

void bc6h_decode_block(const uint8_t block[BC7_BLOCK_BYTES], bool is_signed, uint16_t out[BC7_TEXELS][4]) {
  static const uint16_t one = 0x3C00u; /* alpha: half 1.0 */
  Bit_Reader r = {block, 0};
  uint32_t number = read_bits(&r, 2);
  if (number > 1u) number |= read_bits(&r, 3) << 2;
  const Bc6h_Mode *mode = NULL;
  uint32_t m = 0;
  for (; m < BC6H_MODES; m++) {
    if (k_bc6h_modes[m].number == number) {
      mode = &k_bc6h_modes[m];
      break;
    }
  }
  if (!mode) { /* reserved: (0, 0, 0, 1) */
    for (uint32_t t = 0; t < BC7_TEXELS; t++) out[t][0] = out[t][1] = out[t][2] = 0, out[t][3] = one;
    return;
  }
  uint32_t e[BC6H_ENDPOINTS][BC6H_CHANNELS];
  uint32_t partition = 0;
  memset(e, 0, sizeof(e));
  for (uint32_t f = 0; f < mode->field_count; f++) {
    const Bc6h_Field *fd = &k_bc6h_fields[m][f];
    const int32_t step = fd->a >= fd->b ? 1 : -1;
    const uint32_t count = (uint32_t)(fd->a >= fd->b ? fd->a - fd->b : fd->b - fd->a) + 1u;
    int32_t bit = fd->b;
    for (uint32_t k = 0; k < count; k++, bit += step) {
      const uint32_t v = read_bits(&r, 1);
      if (fd->var == BC6H_PARTITION_VAR) partition |= v << bit;
      else e[fd->var / BC6H_CHANNELS][fd->var % BC6H_CHANNELS] |= v << bit;
    }
  }
  const bool two = mode->partition_bits != 0;
  if (two) partition = read_bits(&r, mode->partition_bits);
  const uint32_t endpoints = two ? 4u : 2u;
  int32_t s[BC6H_ENDPOINTS][BC6H_CHANNELS];
  for (uint32_t c = 0; c < BC6H_CHANNELS; c++) {
    const uint32_t epb = mode->endpoint_bits, mask = (1u << epb) - 1u;
    s[0][c] = is_signed ? sign_extend(e[0][c], epb) : (int32_t)e[0][c];
    for (uint32_t i = 1; i < endpoints; i++) {
      if (mode->transformed) {
        const int32_t delta = sign_extend(e[i][c], mode->delta_bits[c]);
        const uint32_t sum = ((uint32_t)e[0][c] + (uint32_t)delta) & mask;
        s[i][c] = is_signed ? sign_extend(sum, epb) : (int32_t)sum;
      } else {
        s[i][c] = is_signed ? sign_extend(e[i][c], epb) : (int32_t)e[i][c];
      }
    }
    for (uint32_t i = 0; i < endpoints; i++) s[i][c] = bc6h_unquantize(s[i][c], epb, is_signed);
  }
  const uint32_t index_bits = two ? BC6H_TWO_SUBSET_INDEX_BITS : BC6H_ONE_SUBSET_INDEX_BITS;
  const uint8_t *weights = weights_for(index_bits);
  for (uint32_t t = 0; t < BC7_TEXELS; t++) {
    const uint32_t subset = two ? k_partition2[partition][t] : 0u;
    const bool anchor = t == 0 || (two && t == k_anchor2[partition]);
    const uint32_t w = weights[read_bits(&r, anchor ? index_bits - 1u : index_bits)];
    for (uint32_t c = 0; c < BC6H_CHANNELS; c++) {
      const int32_t a = s[2u * subset][c], b = s[2u * subset + 1u][c];
      const int32_t i = (a * (int32_t)(BC7_WEIGHT_SCALE - w) + b * (int32_t)w + (int32_t)BC7_WEIGHT_ROUND) >> BC7_WEIGHT_SHIFT;
      out[t][c] = bc6h_finish(i, is_signed);
    }
    out[t][3] = one;
  }
}
