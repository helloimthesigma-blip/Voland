/**
 * ASTC LDR decoder. See astc.h. Section numbers refer to the Khronos Data
 * Format Specification 1.3, chapter 23.
 */
#include "gpu/astc.h"

#include <string.h>

#define MAX_WEIGHTS 64u
#define MAX_COLOR_VALUES 18u
#define MIN_WEIGHT_BITS 24u
#define MAX_WEIGHT_BITS 96u
#define VOID_EXTENT_MODE 0x1FCu
#define LEVEL_COUNT 21u
#define WEIGHT_LEVELS 12u

/* Integer sequence encoding ranges (§23.12), smallest first: 2, 3, 4, 5,
 * 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256. */
typedef struct Level {
  uint8_t trits;
  uint8_t quints;
  uint8_t bits;
} Level;

static const Level k_levels[LEVEL_COUNT] = {
    {0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1}, {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3},
    {0, 0, 5}, {0, 1, 3}, {1, 0, 4}, {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8},
};

static uint32_t ise_bit_count(uint32_t count, uint32_t level) {
  const Level *l = &k_levels[level];
  return count * l->bits + (8u * count * l->trits + 4u) / 5u + (7u * count * l->quints + 2u) / 3u;
}

/* Bits [start, start+n) of a little-endian bit string, zero beyond
 * `limit` (an ISE stream's end). */
static uint32_t bits_at(const uint8_t *data, uint32_t start, uint32_t n, uint32_t limit) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t at = start + i;
    if (at >= limit || at >= 128u) break;
    v |= (uint32_t)((data[at / 8u] >> (at % 8u)) & 1u) << i;
  }
  return v;
}

typedef struct Ise_Value {
  uint8_t m;  /* low bits */
  uint8_t tq; /* trit / quint (0 when the range has neither) */
} Ise_Value;

static void decode_trits(uint32_t t, uint8_t out[5]) {
  uint32_t c, t3, t4;
  if (((t >> 2) & 7u) == 7u) {
    c = (((t >> 5) & 7u) << 2) | (t & 3u);
    t4 = 2;
    t3 = 2;
  } else {
    c = t & 0x1fu;
    if (((t >> 5) & 3u) == 3u) {
      t4 = 2;
      t3 = (t >> 7) & 1u;
    } else {
      t4 = (t >> 7) & 1u;
      t3 = (t >> 5) & 3u;
    }
  }
  uint32_t t0, t1, t2;
  if ((c & 3u) == 3u) {
    t2 = 2;
    t1 = (c >> 4) & 1u;
    t0 = (((c >> 3) & 1u) << 1) | (((c >> 2) & 1u) & ~((c >> 3) & 1u) & 1u);
  } else if (((c >> 2) & 3u) == 3u) {
    t2 = 2;
    t1 = 2;
    t0 = c & 3u;
  } else {
    t2 = (c >> 4) & 1u;
    t1 = (c >> 2) & 3u;
    t0 = (((c >> 1) & 1u) << 1) | ((c & 1u) & ~((c >> 1) & 1u) & 1u);
  }
  out[0] = (uint8_t)t0;
  out[1] = (uint8_t)t1;
  out[2] = (uint8_t)t2;
  out[3] = (uint8_t)t3;
  out[4] = (uint8_t)t4;
}

static void decode_quints(uint32_t q, uint8_t out[3]) {
  uint32_t q0, q1, q2;
  if (((q >> 1) & 3u) == 3u && ((q >> 5) & 3u) == 0) {
    const uint32_t b0 = q & 1u;
    q2 = (b0 << 2) | ((((q >> 4) & 1u) & ~b0 & 1u) << 1) | (((q >> 3) & 1u) & ~b0 & 1u);
    q1 = 4;
    q0 = 4;
  } else {
    uint32_t c;
    if (((q >> 1) & 3u) == 3u) {
      q2 = 4;
      c = (((q >> 3) & 3u) << 3) | ((~(q >> 5) & 3u) << 1) | (q & 1u);
    } else {
      q2 = (q >> 5) & 3u;
      c = q & 0x1fu;
    }
    if ((c & 7u) == 5u) {
      q1 = 4;
      q0 = (c >> 3) & 3u;
    } else {
      q1 = (c >> 3) & 3u;
      q0 = c & 7u;
    }
  }
  out[0] = (uint8_t)q0;
  out[1] = (uint8_t)q1;
  out[2] = (uint8_t)q2;
}

/* Decodes `count` values of `level` from bit `start` of `data`. */
static void ise_decode(const uint8_t *data, uint32_t start, uint32_t count, uint32_t level, Ise_Value *out) {
  const Level *l = &k_levels[level];
  const uint32_t n = l->bits;
  const uint32_t limit = start + ise_bit_count(count, level);
  if (l->trits) {
    for (uint32_t block = 0; block * 5u < count; block++) {
      const uint32_t b = start + block * (8u + 5u * n);
      uint32_t m[5];
      uint32_t t = 0;
      uint32_t at = b;
      m[0] = bits_at(data, at, n, limit); at += n;
      t |= bits_at(data, at, 2, limit); at += 2;
      m[1] = bits_at(data, at, n, limit); at += n;
      t |= bits_at(data, at, 2, limit) << 2; at += 2;
      m[2] = bits_at(data, at, n, limit); at += n;
      t |= bits_at(data, at, 1, limit) << 4; at += 1;
      m[3] = bits_at(data, at, n, limit); at += n;
      t |= bits_at(data, at, 2, limit) << 5; at += 2;
      m[4] = bits_at(data, at, n, limit); at += n;
      t |= bits_at(data, at, 1, limit) << 7;
      uint8_t trits[5];
      decode_trits(t, trits);
      for (uint32_t i = 0; i < 5u && block * 5u + i < count; i++) {
        out[block * 5u + i].m = (uint8_t)m[i];
        out[block * 5u + i].tq = trits[i];
      }
    }
    return;
  }
  if (l->quints) {
    for (uint32_t block = 0; block * 3u < count; block++) {
      const uint32_t b = start + block * (7u + 3u * n);
      uint32_t m[3];
      uint32_t q = 0;
      uint32_t at = b;
      m[0] = bits_at(data, at, n, limit); at += n;
      q |= bits_at(data, at, 3, limit); at += 3;
      m[1] = bits_at(data, at, n, limit); at += n;
      q |= bits_at(data, at, 2, limit) << 3; at += 2;
      m[2] = bits_at(data, at, n, limit); at += n;
      q |= bits_at(data, at, 2, limit) << 5;
      uint8_t quints[3];
      decode_quints(q, quints);
      for (uint32_t i = 0; i < 3u && block * 3u + i < count; i++) {
        out[block * 3u + i].m = (uint8_t)m[i];
        out[block * 3u + i].tq = quints[i];
      }
    }
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    out[i].m = (uint8_t)bits_at(data, start + i * n, n, limit);
    out[i].tq = 0;
  }
}

/* Replicates an n-bit value to `width` bits. */
static uint32_t replicate(uint32_t v, uint32_t n, uint32_t width) {
  if (n == 0) return 0;
  uint32_t out = 0;
  int32_t shift = (int32_t)width - (int32_t)n;
  while (shift > -(int32_t)n) {
    out |= shift >= 0 ? v << shift : v >> -shift;
    shift -= (int32_t)n;
  }
  return out & ((1u << width) - 1u);
}

/* Colour endpoint unquantization to 0..255 (§23.13, Table 158). */
static uint32_t unquantize_color(Ise_Value v, uint32_t level) {
  const Level *l = &k_levels[level];
  if (!l->trits && !l->quints) return replicate(v.m, l->bits, 8);
  const uint32_t a = v.m & 1u, b = (v.m >> 1) & 1u, c = (v.m >> 2) & 1u, d = (v.m >> 3) & 1u, e = (v.m >> 4) & 1u,
                 f = (v.m >> 5) & 1u;
  uint32_t bb = 0, cc = 0;
  if (l->trits) {
    switch (l->bits) {
    case 1: bb = 0; cc = 204; break;
    case 2: bb = b * 0x116u; cc = 93; break;
    case 3: bb = c * 0x10Au + b * 0x085u; cc = 44; break;
    case 4: bb = d * 0x104u + c * 0x082u + b * 0x041u; cc = 22; break;
    case 5: bb = e * 0x102u + d * 0x081u + c * 0x040u + b * 0x020u; cc = 11; break;
    default: bb = f * 0x101u + e * 0x080u + d * 0x040u + c * 0x020u + b * 0x010u; cc = 5; break;
    }
  } else {
    switch (l->bits) {
    case 1: bb = 0; cc = 113; break;
    case 2: bb = b * 0x10Cu; cc = 54; break;
    case 3: bb = c * 0x105u + b * 0x082u; cc = 26; break;
    case 4: bb = d * 0x102u + c * 0x081u + b * 0x040u; cc = 13; break;
    default: bb = e * 0x101u + d * 0x080u + c * 0x040u + b * 0x020u; cc = 6; break;
    }
  }
  const uint32_t aa = a ? 0x1FFu : 0u;
  uint32_t unq = v.tq * cc + bb;
  unq ^= aa;
  return ((aa & 0x80u) | (unq >> 2)) & 0xFFu;
}

/* Weight unquantization to 0..64 (§23.17, Tables 167/168). */
static uint32_t unquantize_weight(Ise_Value v, uint32_t level) {
  const Level *l = &k_levels[level];
  uint32_t unq;
  if (!l->trits && !l->quints) {
    unq = replicate(v.m, l->bits, 6);
  } else if (l->bits == 0) {
    static const uint8_t trit_values[3] = {0, 32, 63};
    static const uint8_t quint_values[5] = {0, 16, 32, 47, 63};
    unq = l->trits ? trit_values[v.tq % 3u] : quint_values[v.tq % 5u];
  } else {
    const uint32_t a = v.m & 1u, b = (v.m >> 1) & 1u, c = (v.m >> 2) & 1u;
    uint32_t bb = 0, cc = 0;
    if (l->trits) {
      switch (l->bits) {
      case 1: bb = 0; cc = 50; break;
      case 2: bb = b * 0x45u; cc = 23; break;
      default: bb = c * 0x42u + b * 0x21u; cc = 11; break;
      }
    } else {
      switch (l->bits) {
      case 1: bb = 0; cc = 28; break;
      default: bb = b * 0x42u; cc = 13; break;
      }
    }
    const uint32_t aa = a ? 0x7Fu : 0u;
    unq = v.tq * cc + bb;
    unq ^= aa;
    unq = ((aa & 0x20u) | (unq >> 2)) & 0x3Fu;
  }
  return unq > 32u ? unq + 1u : unq;
}

/* §23.10, Table 147. Returns false for reserved modes. */
typedef struct Block_Mode {
  uint32_t grid_w, grid_h;
  bool dual_plane;
  uint32_t weight_level; /* index into k_levels (ranges 2..32) */
} Block_Mode;

static bool decode_block_mode(uint32_t mode, Block_Mode *out) {
  uint32_t rho, w, h;
  bool dp = ((mode >> 10) & 1u) != 0, p = ((mode >> 9) & 1u) != 0;
  const uint32_t a = (mode >> 5) & 3u;
  if (mode & 3u) {
    rho = ((mode & 3u) << 1) | ((mode >> 4) & 1u);
    const uint32_t b = (mode >> 7) & 3u;
    switch ((mode >> 2) & 3u) {
    case 0: w = b + 4u; h = a + 2u; break;
    case 1: w = b + 8u; h = a + 2u; break;
    case 2: w = a + 2u; h = b + 8u; break;
    default:
      if ((mode >> 8) & 1u) {
        w = ((mode >> 7) & 1u) + 2u;
        h = a + 2u;
      } else {
        w = a + 2u;
        h = ((mode >> 7) & 1u) + 6u;
      }
      break;
    }
  } else {
    rho = (((mode >> 2) & 3u) << 1) | ((mode >> 4) & 1u);
    if (((mode >> 2) & 3u) == 0) return false;
    switch ((mode >> 7) & 3u) {
    case 0: w = 12; h = a + 2u; break;
    case 1: w = a + 2u; h = 12; break;
    case 2:
      w = a + 6u;
      h = ((mode >> 9) & 3u) + 6u;
      dp = false;
      p = false;
      break;
    default:
      if (a == 0) { w = 6; h = 10; }
      else if (a == 1) { w = 10; h = 6; }
      else return false;
      break;
    }
  }
  if (rho < 2u) return false;
  out->grid_w = w;
  out->grid_h = h;
  out->dual_plane = dp;
  out->weight_level = (rho - 2u) + (p ? 6u : 0u);
  return true;
}

static uint32_t hash52(uint32_t p) {
  p ^= p >> 15;
  p -= p << 17;
  p += p << 7;
  p += p << 4;
  p ^= p >> 5;
  p += p << 16;
  p ^= p >> 7;
  p ^= p >> 3;
  p ^= p << 6;
  p ^= p >> 17;
  return p;
}

/* §23.21 (z = 0). */
static uint32_t select_partition(uint32_t seed, uint32_t x, uint32_t y, uint32_t count, bool small_block) {
  if (small_block) {
    x <<= 1;
    y <<= 1;
  }
  seed += (count - 1u) * 1024u;
  const uint32_t rnum = hash52(seed);
  uint32_t s[13];
  s[1] = rnum & 0xFu;
  s[2] = (rnum >> 4) & 0xFu;
  s[3] = (rnum >> 8) & 0xFu;
  s[4] = (rnum >> 12) & 0xFu;
  s[5] = (rnum >> 16) & 0xFu;
  s[6] = (rnum >> 20) & 0xFu;
  s[7] = (rnum >> 24) & 0xFu;
  s[8] = (rnum >> 28) & 0xFu;
  s[9] = (rnum >> 18) & 0xFu;
  s[10] = (rnum >> 22) & 0xFu;
  s[11] = (rnum >> 26) & 0xFu;
  s[12] = ((rnum >> 30) | (rnum << 2)) & 0xFu;
  for (uint32_t i = 1; i <= 12u; i++) s[i] *= s[i];
  uint32_t sh1, sh2;
  if (seed & 1u) {
    sh1 = (seed & 2u) ? 4u : 5u;
    sh2 = count == 3u ? 6u : 5u;
  } else {
    sh1 = count == 3u ? 6u : 5u;
    sh2 = (seed & 2u) ? 4u : 5u;
  }
  const uint32_t sh3 = (seed & 0x10u) ? sh1 : sh2;
  s[1] >>= sh1; s[2] >>= sh2; s[3] >>= sh1; s[4] >>= sh2;
  s[5] >>= sh1; s[6] >>= sh2; s[7] >>= sh1; s[8] >>= sh2;
  s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3; s[12] >>= sh3;
  uint32_t a = s[1] * x + s[2] * y + (rnum >> 14);
  uint32_t b = s[3] * x + s[4] * y + (rnum >> 10);
  uint32_t c = s[5] * x + s[6] * y + (rnum >> 6);
  uint32_t d = s[7] * x + s[8] * y + (rnum >> 2);
  a &= 0x3Fu; b &= 0x3Fu; c &= 0x3Fu; d &= 0x3Fu;
  if (count < 4u) d = 0;
  if (count < 3u) c = 0;
  if (a >= b && a >= c && a >= d) return 0;
  if (b >= c && b >= d) return 1;
  if (c >= d) return 2;
  return 3;
}

static void bit_transfer_signed(int32_t *a, int32_t *b) {
  *b >>= 1;
  *b |= *a & 0x80;
  *a >>= 1;
  *a &= 0x3F;
  if (*a & 0x20) *a -= 0x40;
}

static int32_t clamp8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void set4(int32_t e[4], int32_t r, int32_t g, int32_t b, int32_t a) {
  e[0] = r;
  e[1] = g;
  e[2] = b;
  e[3] = a;
}

static void blue_contract(int32_t e[4], int32_t r, int32_t g, int32_t b, int32_t a) {
  set4(e, (r + b) >> 1, (g + b) >> 1, b, a);
}

/* §23.14. Returns false for HDR modes (error colour in LDR). */
static bool decode_endpoints(uint32_t cem, const int32_t *v, int32_t e0[4], int32_t e1[4]) {
  int32_t t[8];
  for (uint32_t i = 0; i < 8u; i++) t[i] = v[i];
  switch (cem) {
  case 0:
    set4(e0, t[0], t[0], t[0], 0xFF);
    set4(e1, t[1], t[1], t[1], 0xFF);
    return true;
  case 1: {
    const int32_t l0 = (t[0] >> 2) | (t[1] & 0xC0);
    int32_t l1 = l0 + (t[1] & 0x3F);
    if (l1 > 0xFF) l1 = 0xFF;
    set4(e0, l0, l0, l0, 0xFF);
    set4(e1, l1, l1, l1, 0xFF);
    return true;
  }
  case 4:
    set4(e0, t[0], t[0], t[0], t[2]);
    set4(e1, t[1], t[1], t[1], t[3]);
    return true;
  case 5:
    bit_transfer_signed(&t[1], &t[0]);
    bit_transfer_signed(&t[3], &t[2]);
    set4(e0, t[0], t[0], t[0], t[2]);
    set4(e1, t[0] + t[1], t[0] + t[1], t[0] + t[1], t[2] + t[3]);
    break;
  case 6:
    set4(e0, (t[0] * t[3]) >> 8, (t[1] * t[3]) >> 8, (t[2] * t[3]) >> 8, 0xFF);
    set4(e1, t[0], t[1], t[2], 0xFF);
    return true;
  case 8:
    if (t[1] + t[3] + t[5] >= t[0] + t[2] + t[4]) {
      set4(e0, t[0], t[2], t[4], 0xFF);
      set4(e1, t[1], t[3], t[5], 0xFF);
    } else {
      blue_contract(e0, t[1], t[3], t[5], 0xFF);
      blue_contract(e1, t[0], t[2], t[4], 0xFF);
    }
    return true;
  case 9:
    bit_transfer_signed(&t[1], &t[0]);
    bit_transfer_signed(&t[3], &t[2]);
    bit_transfer_signed(&t[5], &t[4]);
    if (t[1] + t[3] + t[5] >= 0) {
      set4(e0, t[0], t[2], t[4], 0xFF);
      set4(e1, t[0] + t[1], t[2] + t[3], t[4] + t[5], 0xFF);
    } else {
      blue_contract(e0, t[0] + t[1], t[2] + t[3], t[4] + t[5], 0xFF);
      blue_contract(e1, t[0], t[2], t[4], 0xFF);
    }
    break;
  case 10:
    set4(e0, (t[0] * t[3]) >> 8, (t[1] * t[3]) >> 8, (t[2] * t[3]) >> 8, t[4]);
    set4(e1, t[0], t[1], t[2], t[5]);
    return true;
  case 12:
    if (t[1] + t[3] + t[5] >= t[0] + t[2] + t[4]) {
      set4(e0, t[0], t[2], t[4], t[6]);
      set4(e1, t[1], t[3], t[5], t[7]);
    } else {
      blue_contract(e0, t[1], t[3], t[5], t[7]);
      blue_contract(e1, t[0], t[2], t[4], t[6]);
    }
    return true;
  case 13:
    bit_transfer_signed(&t[1], &t[0]);
    bit_transfer_signed(&t[3], &t[2]);
    bit_transfer_signed(&t[5], &t[4]);
    bit_transfer_signed(&t[7], &t[6]);
    if (t[1] + t[3] + t[5] >= 0) {
      set4(e0, t[0], t[2], t[4], t[6]);
      set4(e1, t[0] + t[1], t[2] + t[3], t[4] + t[5], t[6] + t[7]);
    } else {
      blue_contract(e0, t[0] + t[1], t[2] + t[3], t[4] + t[5], t[6] + t[7]);
      blue_contract(e1, t[0], t[2], t[4], t[6]);
    }
    break;
  default:
    return false; /* HDR */
  }
  for (uint32_t c = 0; c < 4u; c++) {
    e0[c] = clamp8(e0[c]);
    e1[c] = clamp8(e1[c]);
  }
  return true;
}

static void fill_error(uint8_t out[][4], uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    out[i][0] = 0xFF;
    out[i][1] = 0x00;
    out[i][2] = 0xFF;
    out[i][3] = 0xFF;
  }
}

bool astc_decode_block(const uint8_t block[ASTC_BLOCK_BYTES], uint32_t bw, uint32_t bh, bool srgb, uint8_t out[][4]) {
  const uint32_t texels = bw * bh;
  if (bw < 2u || bh < 2u || bw > ASTC_MAX_FOOTPRINT || bh > ASTC_MAX_FOOTPRINT) return false;
  const uint32_t mode = bits_at(block, 0, 11, 128);
  if ((mode & 0x1FFu) == VOID_EXTENT_MODE) {
    if ((mode >> 9) & 1u) { /* HDR void extent */
      fill_error(out, texels);
      return false;
    }
    uint8_t rgba[4];
    for (uint32_t c = 0; c < 4u; c++) rgba[c] = (uint8_t)(bits_at(block, 64u + 16u * c, 16, 128) >> 8);
    for (uint32_t i = 0; i < texels; i++) memcpy(out[i], rgba, 4);
    return true;
  }
  Block_Mode bm;
  if (!decode_block_mode(mode, &bm) || bm.grid_w > bw || bm.grid_h > bh) {
    fill_error(out, texels);
    return false;
  }
  const uint32_t planes = bm.dual_plane ? 2u : 1u;
  const uint32_t weight_count = bm.grid_w * bm.grid_h * planes;
  const uint32_t weight_bits = ise_bit_count(weight_count, bm.weight_level);
  if (weight_count > MAX_WEIGHTS || weight_bits < MIN_WEIGHT_BITS || weight_bits > MAX_WEIGHT_BITS) {
    fill_error(out, texels);
    return false;
  }
  const uint32_t partitions = bits_at(block, 11, 2, 128) + 1u;
  if (bm.dual_plane && partitions == 4u) {
    fill_error(out, texels);
    return false;
  }
  /* Colour endpoint modes (§23.11). */
  uint32_t cem[4] = {0, 0, 0, 0};
  uint32_t seed = 0;
  uint32_t color_start, config_bits, extra_cem_bits = 0;
  if (partitions == 1u) {
    cem[0] = bits_at(block, 13, 4, 128);
    color_start = 17;
    config_bits = 17;
  } else {
    seed = bits_at(block, 13, 10, 128);
    const uint32_t selector = bits_at(block, 23, 2, 128);
    color_start = 29;
    if (selector == 0) {
      const uint32_t all = bits_at(block, 25, 4, 128);
      for (uint32_t i = 0; i < partitions; i++) cem[i] = all;
      config_bits = 29;
    } else {
      extra_cem_bits = 3u * partitions - 4u;
      config_bits = 25u + 3u * partitions;
      const uint32_t extra = bits_at(block, 128u - weight_bits - extra_cem_bits, extra_cem_bits, 128);
      const uint32_t field = bits_at(block, 23, 6, 128) | (extra << 6);
      for (uint32_t i = 0; i < partitions; i++) {
        const uint32_t c = (field >> (2u + i)) & 1u;
        const uint32_t m = (field >> (2u + partitions + 2u * i)) & 3u;
        cem[i] = ((selector - 1u + c) << 2) | m;
      }
    }
  }
  uint32_t ccs = 0;
  if (bm.dual_plane) {
    config_bits += 2u;
    ccs = bits_at(block, 128u - weight_bits - extra_cem_bits - 2u, 2, 128);
  }
  /* Colour values and their range (§23.22). */
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < partitions; i++) color_count += ((cem[i] >> 2) + 1u) * 2u;
  if (color_count > MAX_COLOR_VALUES || config_bits + weight_bits > 128u) {
    fill_error(out, texels);
    return false;
  }
  const uint32_t remaining = 128u - config_bits - weight_bits;
  if (remaining < (13u * color_count + 4u) / 5u) {
    fill_error(out, texels);
    return false;
  }
  uint32_t color_level = LEVEL_COUNT - 1u;
  while (color_level > 0 && ise_bit_count(color_count, color_level) > remaining) color_level--;
  Ise_Value raw[MAX_COLOR_VALUES];
  ise_decode(block, color_start, color_count, color_level, raw);
  int32_t values[MAX_COLOR_VALUES];
  for (uint32_t i = 0; i < color_count; i++) values[i] = (int32_t)unquantize_color(raw[i], color_level);
  int32_t e0[4][4], e1[4][4];
  bool hdr[4] = {false, false, false, false};
  uint32_t at = 0;
  for (uint32_t i = 0; i < partitions; i++) {
    int32_t v[8] = {0};
    const uint32_t n = ((cem[i] >> 2) + 1u) * 2u;
    for (uint32_t k = 0; k < n; k++) v[k] = values[at + k];
    at += n;
    hdr[i] = !decode_endpoints(cem[i], v, e0[i], e1[i]);
  }
  /* Weights: stored bit-reversed from the top of the block (§23.16). */
  uint8_t reversed[ASTC_BLOCK_BYTES];
  for (uint32_t i = 0; i < ASTC_BLOCK_BYTES; i++) {
    uint8_t b = block[ASTC_BLOCK_BYTES - 1u - i];
    b = (uint8_t)(((b * 0x0802u & 0x22110u) | (b * 0x8020u & 0x88440u)) * 0x10101u >> 16);
    reversed[i] = b;
  }
  Ise_Value wraw[MAX_WEIGHTS];
  ise_decode(reversed, 0, weight_count, bm.weight_level, wraw);
  uint8_t weights[MAX_WEIGHTS];
  for (uint32_t i = 0; i < weight_count; i++) weights[i] = (uint8_t)unquantize_weight(wraw[i], bm.weight_level);
  /* Per texel: infill (§23.18), partition, interpolation (§23.19). */
  const uint32_t ds = (1024u + bw / 2u) / (bw - 1u);
  const uint32_t dt = (1024u + bh / 2u) / (bh - 1u);
  const bool small_block = texels < 31u;
  for (uint32_t y = 0; y < bh; y++) {
    for (uint32_t x = 0; x < bw; x++) {
      const uint32_t gs = (ds * x * (bm.grid_w - 1u) + 32u) >> 6;
      const uint32_t gt = (dt * y * (bm.grid_h - 1u) + 32u) >> 6;
      const uint32_t js = gs >> 4, fs = gs & 0xFu, jt = gt >> 4, ft = gt & 0xFu;
      const uint32_t w11 = (fs * ft + 8u) >> 4;
      const uint32_t w10 = ft - w11, w01 = fs - w11, w00 = 16u - fs - ft + w11;
      uint32_t plane_weight[2];
      for (uint32_t p = 0; p < planes; p++) {
        const uint32_t x1 = js + 1u < bm.grid_w ? js + 1u : js;
        const uint32_t y1 = jt + 1u < bm.grid_h ? jt + 1u : jt;
        const uint32_t p00 = weights[(js + jt * bm.grid_w) * planes + p];
        const uint32_t p01 = weights[(x1 + jt * bm.grid_w) * planes + p];
        const uint32_t p10 = weights[(js + y1 * bm.grid_w) * planes + p];
        const uint32_t p11 = weights[(x1 + y1 * bm.grid_w) * planes + p];
        plane_weight[p] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8u) >> 4;
      }
      const uint32_t part = partitions > 1u ? select_partition(seed, x, y, partitions, small_block) : 0u;
      uint8_t *texel = out[y * bw + x];
      if (hdr[part]) {
        texel[0] = 0xFF; texel[1] = 0x00; texel[2] = 0xFF; texel[3] = 0xFF;
        continue;
      }
      for (uint32_t c = 0; c < 4u; c++) {
        const uint32_t i = (bm.dual_plane && c == ccs) ? plane_weight[1] : plane_weight[0];
        uint32_t c0 = (uint32_t)e0[part][c], c1 = (uint32_t)e1[part][c];
        if (srgb && c < 3u) {
          c0 = (c0 << 8) | 0x80u;
          c1 = (c1 << 8) | 0x80u;
        } else {
          c0 = (c0 << 8) | c0;
          c1 = (c1 << 8) | c1;
        }
        const uint32_t v = (c0 * (64u - i) + c1 * i + 32u) / 64u;
        texel[c] = (uint8_t)(v >> 8);
      }
    }
  }
  return true;
}
