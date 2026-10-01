/**
 * A64 Advanced SIMD data processing, ARMv8.0 (DDI 0487 C4.1.90-ish
 * tables): vector and scalar forms of three-same, two-register misc,
 * across lanes, three different, shift by immediate, modified immediate,
 * copy, permute, extract, table lookup and by-element. FP lanes use
 * softfloat.c. Crypto (AES/SHA/PMULL.1Q) and every post-8.0 extension
 * (FP16, dot product, RDM, ...) decode as undefined.
 */
#include "cpu/backends/interpreter/interp_simd.h"
#include "cpu/backends/interpreter/simd_lanes.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Small integer helpers.                                              */
/* ------------------------------------------------------------------ */

/* Shifts that are defined for any count (C leaves >= width undefined). */
static uint64_t shl64(uint64_t v, unsigned n) { return n >= 64 ? 0 : v << n; }
static uint64_t lsr64(uint64_t v, unsigned n) { return n >= 64 ? 0 : v >> n; }
static int64_t asr64(int64_t v, unsigned n) { return n >= 64 ? (v < 0 ? -1 : 0) : v >> n; }

/* Rounding right shifts by n >= 1: (v + 2^(n-1)) >> n without overflow. */
static uint64_t urshr(uint64_t v, unsigned n) { return lsr64(v, n) + (lsr64(v, n - 1u) & 1u); }
static int64_t srshr(int64_t v, unsigned n) { return asr64(v, n) + (int64_t)((uint64_t)asr64(v, n - 1u) & 1u); }

static unsigned popcount64(uint64_t v) {
  unsigned n = 0;
  while (v) { v &= v - 1u; n++; }
  return n;
}

static uint64_t ones_or_zero(bool condition, unsigned esize) { return condition ? lane_mask(esize) : 0; }

static FP_Format fp_format_of(unsigned esize) { return esize == 64u ? FP_DOUBLE : (esize == 32u ? FP_SINGLE : FP_HALF); }

/* ------------------------------------------------------------------ */
/* Integer element operations (shared by vector and scalar forms).      */
/* ------------------------------------------------------------------ */

/* SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL: shift by the signed low
 * byte of `b`. */
static uint64_t shift_by_register(Interp_State *s, uint64_t a, uint64_t b, unsigned esize, bool is_unsigned,
                                  bool rounding, bool saturating) {
  const int shift = (int8_t)(uint8_t)b;
  const uint64_t m = lane_mask(esize);
  if (shift >= 0) {
    const unsigned n = (unsigned)shift;
    if (!saturating) return shl64(a, n) & m;
    if (is_unsigned) {
      if (a == 0) return 0;
      if (n >= esize || (shl64(a, n) & m) >> n != a) { set_qc(s); return m; }
      return shl64(a, n) & m;
    }
    const int64_t sa = sign_extend(a, esize);
    if (sa == 0) return 0;
    if (n >= esize || (sign_extend(shl64(a, n) & m, esize) >> n) != sa) {
      set_qc(s);
      return sa < 0 ? (m >> 1) + 1u : m >> 1;
    }
    return shl64(a, n) & m;
  }
  const unsigned n = (unsigned)(-shift);
  if (is_unsigned) return (rounding ? urshr(a, n) : lsr64(a, n)) & m;
  const int64_t sa = sign_extend(a, esize);
  return (uint64_t)(rounding ? srshr(sa, n) : asr64(sa, n)) & m;
}

/* Signed saturating doubling multiply high (esize 16/32). */
static uint64_t sqdmulh(Interp_State *s, uint64_t a, uint64_t b, unsigned esize, bool rounding) {
  const int64_t x = sign_extend(a, esize), y = sign_extend(b, esize);
  const int64_t min = -(int64_t)(lane_mask(esize) >> 1) - 1;
  if (x == min && y == min) {
    set_qc(s);
    return lane_mask(esize) >> 1;
  }
  int64_t product = x * y * 2;
  if (rounding) product += (int64_t)1 << (esize - 1u);
  return (uint64_t)(product >> esize) & lane_mask(esize);
}

/* Signed saturating doubling multiply long: 2*a*b in 2*esize, saturated. */
static int64_t sqdmull_value(Interp_State *s, int64_t x, int64_t y, unsigned esize) {
  const int64_t min = -(int64_t)(lane_mask(esize) >> 1) - 1;
  if (x == min && y == min) {
    set_qc(s);
    return esize == 32u ? INT64_MAX : (int64_t)(lane_mask(2u * esize) >> 1);
  }
  return x * y * 2;
}

/* Saturating add of two 2*esize signed values (2*esize may be 64). */
static uint64_t sqadd_wide(Interp_State *s, int64_t a, int64_t b, unsigned wide) {
  if (wide == 64u) return sqadd64(s, (uint64_t)a, (uint64_t)b);
  return sat_s(s, a + b, wide);
}
static uint64_t sqsub_wide(Interp_State *s, int64_t a, int64_t b, unsigned wide) {
  if (wide == 64u) return sqsub64(s, (uint64_t)a, (uint64_t)b);
  return sat_s(s, a - b, wide);
}

static uint64_t polynomial_multiply(uint64_t a, uint64_t b, unsigned esize) {
  uint64_t result = 0;
  for (unsigned i = 0; i < esize; i++) {
    if ((b >> i) & 1u) result ^= a << i;
  }
  return result;
}

/* Non-pairwise integer three-same. false = undefined combination. */
static bool int_three_same(Interp_State *s, bool u, unsigned opcode, unsigned esize, uint64_t a, uint64_t b,
                           uint64_t d, uint64_t *out) {
  const uint64_t m = lane_mask(esize);
  const int64_t sa = sign_extend(a, esize), sb = sign_extend(b, esize);
  const bool wide = esize == 64u;
  switch (opcode) {
  case 0x00: /* SHADD / UHADD */
    if (wide) return false;
    *out = (uint64_t)(u ? (int64_t)((a + b) >> 1) : (sa + sb) >> 1) & m;
    return true;
  case 0x01: /* SQADD / UQADD */
    if (wide) *out = u ? uqadd64(s, a, b) : sqadd64(s, a, b);
    else *out = u ? sat_u(s, a + b, esize) : sat_s(s, sa + sb, esize);
    return true;
  case 0x02: /* SRHADD / URHADD */
    if (wide) return false;
    *out = (uint64_t)(u ? (int64_t)((a + b + 1u) >> 1) : (sa + sb + 1) >> 1) & m;
    return true;
  case 0x04: /* SHSUB / UHSUB */
    if (wide) return false;
    *out = (uint64_t)(u ? ((int64_t)a - (int64_t)b) >> 1 : (sa - sb) >> 1) & m;
    return true;
  case 0x05: /* SQSUB / UQSUB */
    if (wide) *out = u ? uqsub64(s, a, b) : sqsub64(s, a, b);
    else *out = u ? (a < b ? (set_qc(s), 0u) : a - b) : sat_s(s, sa - sb, esize);
    return true;
  case 0x06: *out = ones_or_zero(u ? a > b : sa > sb, esize); return true;   /* CMGT / CMHI */
  case 0x07: *out = ones_or_zero(u ? a >= b : sa >= sb, esize); return true; /* CMGE / CMHS */
  case 0x08: *out = shift_by_register(s, a, b, esize, u, false, false); return true;
  case 0x09: *out = shift_by_register(s, a, b, esize, u, false, true); return true;
  case 0x0A: *out = shift_by_register(s, a, b, esize, u, true, false); return true;
  case 0x0B: *out = shift_by_register(s, a, b, esize, u, true, true); return true;
  case 0x0C: /* SMAX / UMAX */
    if (wide) return false;
    *out = u ? (a > b ? a : b) : (sa > sb ? a : b);
    return true;
  case 0x0D: /* SMIN / UMIN */
    if (wide) return false;
    *out = u ? (a < b ? a : b) : (sa < sb ? a : b);
    return true;
  case 0x0E: case 0x0F: { /* SABD/UABD, SABA/UABA */
    if (wide) return false;
    const uint64_t diff = u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa);
    *out = (opcode == 0x0F ? d + diff : diff) & m;
    return true;
  }
  case 0x10: *out = (u ? a - b : a + b) & m; return true;                     /* ADD / SUB */
  case 0x11: *out = ones_or_zero(u ? a == b : (a & b) != 0, esize); return true; /* CMEQ / CMTST */
  case 0x12: /* MLA / MLS */
    if (wide) return false;
    *out = (u ? d - a * b : d + a * b) & m;
    return true;
  case 0x13: /* MUL / PMUL */
    if (wide || (u && esize != 8u)) return false;
    *out = (u ? polynomial_multiply(a, b, esize) : a * b) & m;
    return true;
  case 0x16: /* SQDMULH / SQRDMULH */
    if (esize != 16u && esize != 32u) return false;
    *out = sqdmulh(s, a, b, esize, u);
    return true;
  default:
    return false;
  }
}

/* ------------------------------------------------------------------ */
/* FP element operations.                                              */
/* ------------------------------------------------------------------ */

/* FP three-same lane op by (U, size<1>, opcode). false = undefined. */
static bool fp_three_same(Interp_State *s, bool u, bool a1, unsigned opcode, FP_Format f, uint64_t a, uint64_t b,
                          uint64_t d, uint64_t *out) {
  FP_Env env = simd_env(s);
  const unsigned key = ((unsigned)u << 4) | ((unsigned)a1 << 3) | (opcode & 7u);
  const unsigned esize = f == FP_DOUBLE ? 64u : 32u;
  switch (key) {
  case 0x00: *out = fp_max_num(f, a, b, &env); return true;                 /* FMAXNM */
  case 0x01: *out = fp_mul_add(f, d, a, b, &env); return true;              /* FMLA */
  case 0x02: *out = fp_add(f, a, b, &env); return true;                     /* FADD */
  case 0x03: *out = fp_mulx(f, a, b, &env); return true;                    /* FMULX */
  case 0x04: *out = ones_or_zero(fp_compare_eq(f, a, b, &env), esize); return true; /* FCMEQ */
  case 0x06: *out = fp_max(f, a, b, &env); return true;                     /* FMAX */
  case 0x07: *out = fp_recip_step(f, a, b, &env); return true;              /* FRECPS */
  case 0x08: *out = fp_min_num(f, a, b, &env); return true;                 /* FMINNM */
  case 0x09: *out = fp_mul_add(f, d, fp_neg(f, a), b, &env); return true;   /* FMLS */
  case 0x0A: *out = fp_sub(f, a, b, &env); return true;                     /* FSUB */
  case 0x0E: *out = fp_min(f, a, b, &env); return true;                     /* FMIN */
  case 0x0F: *out = fp_rsqrt_step(f, a, b, &env); return true;              /* FRSQRTS */
  case 0x13: *out = fp_mul(f, a, b, &env); return true;                     /* FMUL */
  case 0x14: *out = ones_or_zero(fp_compare_ge(f, a, b, &env), esize); return true; /* FCMGE */
  case 0x15: *out = ones_or_zero(fp_compare_ge(f, fp_abs(f, a), fp_abs(f, b), &env), esize); return true; /* FACGE */
  case 0x17: *out = fp_div(f, a, b, &env); return true;                     /* FDIV */
  case 0x1A: *out = fp_abs(f, fp_sub(f, a, b, &env)); return true;          /* FABD */
  case 0x1C: *out = ones_or_zero(fp_compare_gt(f, a, b, &env), esize); return true; /* FCMGT */
  case 0x1D: *out = ones_or_zero(fp_compare_gt(f, fp_abs(f, a), fp_abs(f, b), &env), esize); return true; /* FACGT */
  default: return false;
  }
}

/* FP pairwise ops (FMAXNMP, FADDP, FMAXP, FMINNMP, FMINP). */
static bool fp_pairwise_op(Interp_State *s, unsigned which, FP_Format f, uint64_t a, uint64_t b, uint64_t *out) {
  FP_Env env = simd_env(s);
  switch (which) {
  case 0: *out = fp_max_num(f, a, b, &env); return true;
  case 1: *out = fp_add(f, a, b, &env); return true;
  case 2: *out = fp_max(f, a, b, &env); return true;
  case 3: *out = fp_min_num(f, a, b, &env); return true;
  case 4: *out = fp_min(f, a, b, &env); return true;
  default: return false;
  }
}

/* FP two-register misc lane op shared by vector and scalar forms, keyed
 * by (U, size<1>, opcode). Returns false if not an FP misc op. */
static bool fp_misc(Interp_State *s, bool u, bool a1, unsigned opcode, FP_Format f, uint64_t a, uint64_t *out) {
  FP_Env env = simd_env(s);
  const unsigned esize = f == FP_DOUBLE ? 64u : 32u;
  const unsigned key = ((unsigned)u << 6) | ((unsigned)a1 << 5) | opcode;
  switch (key) {
  case 0x18: *out = fp_round_int(f, a, FP_ROUND_NEAREST_EVEN, false, &env); return true; /* FRINTN */
  case 0x38: *out = fp_round_int(f, a, FP_ROUND_PLUS_INF, false, &env); return true;     /* FRINTP */
  case 0x19: *out = fp_round_int(f, a, FP_ROUND_MINUS_INF, false, &env); return true;    /* FRINTM */
  case 0x39: *out = fp_round_int(f, a, FP_ROUND_ZERO, false, &env); return true;         /* FRINTZ */
  case 0x58: *out = fp_round_int(f, a, FP_ROUND_TIE_AWAY, false, &env); return true;     /* FRINTA */
  case 0x59: *out = fp_round_int(f, a, fp_env_rounding(&env), true, &env); return true;  /* FRINTX */
  case 0x79: *out = fp_round_int(f, a, fp_env_rounding(&env), false, &env); return true; /* FRINTI */
  case 0x1A: *out = fp_to_int(f, a, 0, false, esize, FP_ROUND_NEAREST_EVEN, &env); return true; /* FCVTNS */
  case 0x3A: *out = fp_to_int(f, a, 0, false, esize, FP_ROUND_PLUS_INF, &env); return true;     /* FCVTPS */
  case 0x1B: *out = fp_to_int(f, a, 0, false, esize, FP_ROUND_MINUS_INF, &env); return true;    /* FCVTMS */
  case 0x3B: *out = fp_to_int(f, a, 0, false, esize, FP_ROUND_ZERO, &env); return true;         /* FCVTZS */
  case 0x1C: *out = fp_to_int(f, a, 0, false, esize, FP_ROUND_TIE_AWAY, &env); return true;     /* FCVTAS */
  case 0x5A: *out = fp_to_int(f, a, 0, true, esize, FP_ROUND_NEAREST_EVEN, &env); return true;  /* FCVTNU */
  case 0x7A: *out = fp_to_int(f, a, 0, true, esize, FP_ROUND_PLUS_INF, &env); return true;      /* FCVTPU */
  case 0x5B: *out = fp_to_int(f, a, 0, true, esize, FP_ROUND_MINUS_INF, &env); return true;     /* FCVTMU */
  case 0x7B: *out = fp_to_int(f, a, 0, true, esize, FP_ROUND_ZERO, &env); return true;          /* FCVTZU */
  case 0x5C: *out = fp_to_int(f, a, 0, true, esize, FP_ROUND_TIE_AWAY, &env); return true;      /* FCVTAU */
  case 0x1D: *out = fp_from_int(f, a, 0, true, esize, &env); return true;   /* SCVTF */
  case 0x5D: *out = fp_from_int(f, a, 0, false, esize, &env); return true;  /* UCVTF */
  case 0x3D: *out = fp_recip_estimate(f, a, &env); return true;            /* FRECPE */
  case 0x7D: *out = fp_rsqrt_estimate(f, a, &env); return true;            /* FRSQRTE */
  case 0x2C: *out = ones_or_zero(fp_compare_gt(f, a, 0, &env), esize); return true;  /* FCMGT #0 */
  case 0x2D: *out = ones_or_zero(fp_compare_eq(f, a, 0, &env), esize); return true;  /* FCMEQ #0 */
  case 0x2E: *out = ones_or_zero(fp_compare_gt(f, 0, a, &env), esize); return true;  /* FCMLT #0 */
  case 0x6C: *out = ones_or_zero(fp_compare_ge(f, a, 0, &env), esize); return true;  /* FCMGE #0 */
  case 0x6D: *out = ones_or_zero(fp_compare_ge(f, 0, a, &env), esize); return true;  /* FCMLE #0 */
  case 0x2F: *out = fp_abs(f, a); return true;                             /* FABS */
  case 0x6F: *out = fp_neg(f, a); return true;                             /* FNEG */
  case 0x7F: *out = fp_sqrt(f, a, &env); return true;                      /* FSQRT */
  default: return false;
  }
}

/* ------------------------------------------------------------------ */
/* Vector: three same.                                                 */
/* ------------------------------------------------------------------ */

static Interp_Status vector_three_same(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 15, 11);
  const uint32_t rm = bits(insn, 20, 16), rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  const Vec n = vread(s, rn), m = vread(s, rm), dv = vread(s, rd);
  Vec r = {{0, 0}};

  if (opcode == 0x03) { /* logical */
    for (int i = 0; i < 2; i++) {
      const uint64_t a = n.d[i], b = m.d[i], d = dv.d[i];
      uint64_t x;
      switch ((u << 2) | size) {
      case 0: x = a & b; break;
      case 1: x = a & ~b; break;
      case 2: x = a | b; break;
      case 3: x = a | ~b; break;
      case 4: x = a ^ b; break;
      case 5: x = b ^ ((b ^ a) & d); break; /* BSL */
      case 6: x = d ^ ((d ^ a) & b); break; /* BIT */
      default: x = d ^ ((d ^ a) & ~b); break; /* BIF */
      }
      r.d[i] = x;
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }

  if (opcode >= 0x18) { /* floating point */
    const bool sz = size & 1u, a1 = (size >> 1) & 1u;
    if (sz && !q) return INTERP_UNDEFINED;
    const unsigned esize = sz ? 64u : 32u, lanes = (q ? 128u : 64u) / esize;
    const FP_Format f = sz ? FP_DOUBLE : FP_SINGLE;
    if (u && (opcode & 7u) != 3 && (opcode & 7u) != 4 && (opcode & 7u) != 5 && (opcode & 7u) != 7 &&
        !((opcode & 7u) == 2 && a1)) {
      /* pairwise: FMAXNMP/FMINNMP (000), FADDP (010, a=0), FMAXP/FMINP (110) */
      unsigned which;
      switch (((unsigned)a1 << 3) | (opcode & 7u)) {
      case 0x0: which = 0; break;
      case 0x2: which = 1; break;
      case 0x6: which = 2; break;
      case 0x8: which = 3; break;
      case 0xE: which = 4; break;
      default: return INTERP_UNDEFINED;
      }
      for (unsigned i = 0; i < lanes; i++) {
        const Vec *src = i < lanes / 2u ? &n : &m;
        const unsigned j = (i % (lanes / 2u)) * 2u;
        uint64_t x;
        (void)fp_pairwise_op(s, which, f, ue(src, j, esize), ue(src, j + 1u, esize), &x);
        put(&r, i, esize, x);
      }
      vwrite(s, rd, r, q);
      return advance(s);
    }
    for (unsigned i = 0; i < lanes; i++) {
      uint64_t x;
      if (!fp_three_same(s, u, a1, opcode, f, ue(&n, i, esize), ue(&m, i, esize), ue(&dv, i, esize), &x)) {
        return INTERP_UNDEFINED;
      }
      put(&r, i, esize, x);
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }

  const unsigned esize = 8u << size;
  if (size == 3 && !q) return INTERP_UNDEFINED;
  const unsigned lanes = (q ? 128u : 64u) / esize;

  if (opcode == 0x14 || opcode == 0x15 || opcode == 0x17) { /* SMAXP/UMAXP, SMINP/UMINP, ADDP */
    if (size == 3 && opcode != 0x17) return INTERP_UNDEFINED;
    if (opcode == 0x17 && u) return INTERP_UNDEFINED;
    for (unsigned i = 0; i < lanes; i++) {
      const Vec *src = i < lanes / 2u ? &n : &m;
      const unsigned j = (i % (lanes / 2u)) * 2u;
      const uint64_t a = ue(src, j, esize), b = ue(src, j + 1u, esize);
      const int64_t sa = sign_extend(a, esize), sb = sign_extend(b, esize);
      uint64_t x;
      if (opcode == 0x17) x = (a + b) & lane_mask(esize);
      else if (opcode == 0x14) x = u ? (a > b ? a : b) : (sa > sb ? a : b);
      else x = u ? (a < b ? a : b) : (sa < sb ? a : b);
      put(&r, i, esize, x);
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }

  for (unsigned i = 0; i < lanes; i++) {
    uint64_t x;
    if (!int_three_same(s, u, opcode, esize, ue(&n, i, esize), ue(&m, i, esize), ue(&dv, i, esize), &x)) {
      return INTERP_UNDEFINED;
    }
    put(&r, i, esize, x);
  }
  vwrite(s, rd, r, q);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Vector: two-register miscellaneous.                                 */
/* ------------------------------------------------------------------ */

static unsigned count_leading_zeros(uint64_t v, unsigned esize) {
  unsigned n = 0;
  while (n < esize && !((v >> (esize - 1u - n)) & 1u)) n++;
  return n;
}

static unsigned count_leading_sign_bits(uint64_t v, unsigned esize) {
  const uint64_t sign = (v >> (esize - 1u)) & 1u;
  unsigned n = 0;
  while (n < esize - 1u && ((v >> (esize - 2u - n)) & 1u) == sign) n++;
  return n;
}

static uint64_t reverse_bits_in_bytes(uint64_t v) {
  uint64_t r = 0;
  for (unsigned byte = 0; byte < 8; byte++) {
    const uint64_t b = (v >> (8u * byte)) & 0xFFu;
    uint64_t rb = 0;
    for (unsigned i = 0; i < 8; i++) rb |= ((b >> i) & 1u) << (7u - i);
    r |= rb << (8u * byte);
  }
  return r;
}

/* Integer misc ops that work on one lane; scalar forms reuse it.
 * Returns false if (u, opcode) is not one of these. */
static bool int_misc_lane(Interp_State *s, bool u, unsigned opcode, unsigned esize, uint64_t a, uint64_t d,
                          uint64_t *out) {
  const int64_t sa = sign_extend(a, esize);
  const uint64_t m = lane_mask(esize);
  const bool wide = esize == 64u;
  const uint64_t min = (m >> 1) + 1u;
  switch (((unsigned)u << 5) | opcode) {
  case 0x03: /* SUQADD: signed accumulator plus unsigned addend */
    if (wide) {
      const uint64_t r = d + a;
      const bool d_neg = (d >> 63) != 0;
      if (!d_neg && r < d) { set_qc(s); *out = 0x7FFFFFFFFFFFFFFFull; }
      else if (!d_neg && (r >> 63)) { set_qc(s); *out = 0x7FFFFFFFFFFFFFFFull; }
      else *out = r;
    } else {
      *out = sat_s(s, sign_extend(d, esize) + (int64_t)a, esize);
    }
    return true;
  case 0x23: /* USQADD: unsigned accumulator plus signed addend */
    if (wide) {
      if ((int64_t)a < 0) {
        const uint64_t mag = 0u - a;
        *out = d < mag ? (set_qc(s), 0u) : d - mag;
      } else {
        *out = uqadd64(s, d, a);
      }
    } else {
      *out = sat_u_from_s(s, (int64_t)d + sa, esize);
    }
    return true;
  case 0x07: /* SQABS */
    if (a == min) { set_qc(s); *out = m >> 1; }
    else *out = (uint64_t)(sa < 0 ? -sa : sa) & m;
    return true;
  case 0x27: /* SQNEG */
    if (a == min) { set_qc(s); *out = m >> 1; }
    else *out = (uint64_t)(-sa) & m;
    return true;
  case 0x08: *out = ones_or_zero(sa > 0, esize); return true;   /* CMGT #0 */
  case 0x09: *out = ones_or_zero(a == 0, esize); return true;   /* CMEQ #0 */
  case 0x0A: *out = ones_or_zero(sa < 0, esize); return true;   /* CMLT #0 */
  case 0x28: *out = ones_or_zero(sa >= 0, esize); return true;  /* CMGE #0 */
  case 0x29: *out = ones_or_zero(sa <= 0, esize); return true;  /* CMLE #0 */
  case 0x0B: *out = (uint64_t)(sa < 0 ? -sa : sa) & m; return true; /* ABS */
  case 0x2B: *out = (0u - a) & m; return true;                      /* NEG */
  default: return false;
  }
}

/* Narrowing saturations for XTN family: wide value -> esize. */
static uint64_t narrow(Interp_State *s, unsigned kind, uint64_t wide_value, unsigned esize) {
  const unsigned wide = esize * 2u;
  switch (kind) {
  case 0: return wide_value & lane_mask(esize);                                   /* XTN */
  case 1: return sat_s(s, sign_extend(wide_value, wide), esize);                  /* SQXTN */
  case 2: return sat_u(s, wide_value, esize);                                     /* UQXTN */
  default: return sat_u_from_s(s, sign_extend(wide_value, wide), esize);          /* SQXTUN */
  }
}

static Interp_Status vector_two_misc(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 16, 12);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  const Vec n = vread(s, rn), dv = vread(s, rd);
  Vec r = {{0, 0}};
  const unsigned esize = 8u << size;
  const unsigned lanes = (q ? 128u : 64u) / esize;

  /* FP and FP-conversion ops: opcode 1100x-11111 and 01100-01111 with size<1>. */
  if (opcode >= 0x18 || (opcode >= 0x0C && opcode <= 0x0F)) {
    const bool sz = size & 1u, a1 = (size >> 1) & 1u;
    if (opcode == 0x1C && a1) { /* URECPE / URSQRTE */
      if (sz) return INTERP_UNDEFINED;
      for (unsigned i = 0; i < (q ? 4u : 2u); i++) {
        const uint32_t a = (uint32_t)ue(&n, i, 32);
        put(&r, i, 32, u ? fp_unsigned_rsqrt_estimate(a) : fp_unsigned_recip_estimate(a));
      }
      vwrite(s, rd, r, q);
      return advance(s);
    }
    if (sz && !q) return INTERP_UNDEFINED;
    const unsigned fesize = sz ? 64u : 32u, flanes = (q ? 128u : 64u) / fesize;
    const FP_Format f = sz ? FP_DOUBLE : FP_SINGLE;
    for (unsigned i = 0; i < flanes; i++) {
      uint64_t x;
      if (!fp_misc(s, u, a1, opcode, f, ue(&n, i, fesize), &x)) return INTERP_UNDEFINED;
      put(&r, i, fesize, x);
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }

  switch (((unsigned)u << 5) | opcode) {
  case 0x00: case 0x20: case 0x01: { /* REV64, REV32, REV16 */
    const unsigned container = opcode == 0x01 ? 16u : (u ? 32u : 64u);
    if (esize >= container) return INTERP_UNDEFINED;
    const unsigned per = container / esize;
    for (unsigned i = 0; i < lanes; i++) {
      const unsigned base = (i / per) * per;
      put(&r, i, esize, ue(&n, base + (per - 1u - (i - base)), esize));
    }
    break;
  }
  case 0x02: case 0x22: case 0x06: case 0x26: { /* SADDLP/UADDLP, SADALP/UADALP */
    if (size == 3) return INTERP_UNDEFINED;
    const unsigned wide = esize * 2u, out_lanes = lanes / 2u;
    for (unsigned i = 0; i < out_lanes; i++) {
      const uint64_t a = ue(&n, 2u * i, esize), b = ue(&n, 2u * i + 1u, esize);
      uint64_t sum = u ? a + b : (uint64_t)(sign_extend(a, esize) + sign_extend(b, esize));
      if (opcode == 0x06) sum += ue(&dv, i, wide);
      put(&r, i, wide, sum & lane_mask(wide));
    }
    break;
  }
  case 0x04: case 0x24: /* CLS / CLZ */
    if (size == 3) return INTERP_UNDEFINED;
    for (unsigned i = 0; i < lanes; i++) {
      const uint64_t a = ue(&n, i, esize);
      put(&r, i, esize, u ? count_leading_zeros(a, esize) : count_leading_sign_bits(a, esize));
    }
    break;
  case 0x05: /* CNT */
    if (size != 0) return INTERP_UNDEFINED;
    for (unsigned i = 0; i < lanes; i++) put(&r, i, 8, popcount64(ue(&n, i, 8)));
    break;
  case 0x25: /* NOT (size 00) / RBIT (size 01) */
    if (size == 0) { r.d[0] = ~n.d[0]; r.d[1] = ~n.d[1]; }
    else if (size == 1) { r.d[0] = reverse_bits_in_bytes(n.d[0]); r.d[1] = reverse_bits_in_bytes(n.d[1]); }
    else return INTERP_UNDEFINED;
    break;
  case 0x12: case 0x14: case 0x32: case 0x34: { /* XTN, SQXTN, SQXTUN, UQXTN */
    if (size == 3) return INTERP_UNDEFINED;
    const unsigned kind = !u ? (opcode == 0x12 ? 0u : 1u) : (opcode == 0x12 ? 3u : 2u);
    const Vec src = n;
    uint64_t half = 0;
    for (unsigned i = 0; i < 64u / esize; i++) {
      const uint64_t v = narrow(s, kind, ue(&src, i, esize * 2u), esize);
      half |= v << (i * esize);
    }
    vwrite_part(s, rd, half, q);
    return advance(s);
  }
  case 0x33: { /* SHLL / SHLL2 */
    if (size == 3) return INTERP_UNDEFINED;
    const unsigned count = 64u / esize, base = q ? count : 0u;
    for (unsigned i = 0; i < count; i++) put(&r, i, esize * 2u, ue(&n, base + i, esize) << esize);
    vwrite(s, rd, r, true);
    return advance(s);
  }
  case 0x16: case 0x36: { /* FCVTN / FCVTXN */
    const bool sz = size & 1u;
    if (size > 1 || (u && !sz)) return INTERP_UNDEFINED;
    FP_Env env = simd_env(s);
    uint64_t half = 0;
    const unsigned out_esize = sz ? 32u : 16u;
    for (unsigned i = 0; i < 64u / out_esize; i++) {
      const uint64_t a = ue(&n, i, out_esize * 2u);
      uint64_t x;
      if (u) {
        x = fp_convert_round_odd(a, &env);
      } else {
        x = fp_convert(sz ? FP_SINGLE : FP_HALF, sz ? FP_DOUBLE : FP_SINGLE, a, &env);
      }
      half |= x << (i * out_esize);
    }
    vwrite_part(s, rd, half, q);
    return advance(s);
  }
  case 0x17: { /* FCVTL / FCVTL2 */
    const bool sz = size & 1u;
    if (size > 1) return INTERP_UNDEFINED;
    FP_Env env = simd_env(s);
    const unsigned in_esize = sz ? 32u : 16u, count = 64u / in_esize, base = q ? count : 0u;
    for (unsigned i = 0; i < count; i++) {
      put(&r, i, in_esize * 2u,
          fp_convert(sz ? FP_DOUBLE : FP_SINGLE, sz ? FP_SINGLE : FP_HALF, ue(&n, base + i, in_esize), &env));
    }
    vwrite(s, rd, r, true);
    return advance(s);
  }
  default:
    if (size == 3 && !q) return INTERP_UNDEFINED;
    for (unsigned i = 0; i < lanes; i++) {
      uint64_t x;
      if (!int_misc_lane(s, u, opcode, esize, ue(&n, i, esize), ue(&dv, i, esize), &x)) return INTERP_UNDEFINED;
      put(&r, i, esize, x);
    }
    break;
  }
  vwrite(s, rd, r, q);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Vector: across lanes.                                               */
/* ------------------------------------------------------------------ */

static uint64_t fp_reduce(Interp_State *s, unsigned which, FP_Format f, const Vec *v, unsigned lo, unsigned count,
                          unsigned esize) {
  if (count == 1u) return ue(v, lo, esize);
  const uint64_t a = fp_reduce(s, which, f, v, lo, count / 2u, esize);
  const uint64_t b = fp_reduce(s, which, f, v, lo + count / 2u, count / 2u, esize);
  uint64_t x = 0;
  (void)fp_pairwise_op(s, which, f, a, b, &x);
  return x;
}

static Interp_Status vector_across(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 16, 12);
  const Vec n = vread(s, bits(insn, 9, 5));
  const uint32_t rd = bits(insn, 4, 0);

  if (opcode == 0x0C || opcode == 0x0F) { /* FMAXNMV/FMINNMV, FMAXV/FMINV: 4S only */
    if (!u || (size & 1u) || !q) return INTERP_UNDEFINED;
    const bool min = (size >> 1) & 1u;
    const unsigned which = opcode == 0x0C ? (min ? 3u : 0u) : (min ? 4u : 2u);
    const Vec r = {{fp_reduce(s, which, FP_SINGLE, &n, 0, 4, 32), 0}};
    vwrite(s, rd, r, false);
    return advance(s);
  }

  const unsigned esize = 8u << size;
  if (size == 3 || (size == 2 && !q)) return INTERP_UNDEFINED;
  const unsigned lanes = (q ? 128u : 64u) / esize;
  uint64_t acc;
  unsigned out_esize = esize;
  switch (((unsigned)u << 5) | opcode) {
  case 0x03: case 0x23: { /* SADDLV / UADDLV */
    int64_t sum = 0;
    for (unsigned i = 0; i < lanes; i++) sum += u ? (int64_t)ue(&n, i, esize) : se(&n, i, esize);
    out_esize = esize * 2u;
    acc = (uint64_t)sum & lane_mask(out_esize);
    break;
  }
  case 0x1B: { /* ADDV */
    uint64_t sum = 0;
    for (unsigned i = 0; i < lanes; i++) sum += ue(&n, i, esize);
    acc = sum & lane_mask(esize);
    break;
  }
  case 0x0A: case 0x2A: case 0x1A: case 0x3A: { /* SMAXV/UMAXV, SMINV/UMINV */
    const bool want_max = opcode == 0x0A;
    acc = ue(&n, 0, esize);
    for (unsigned i = 1; i < lanes; i++) {
      const uint64_t x = ue(&n, i, esize);
      const bool greater = u ? x > acc : sign_extend(x, esize) > sign_extend(acc, esize);
      const bool less = u ? x < acc : sign_extend(x, esize) < sign_extend(acc, esize);
      if (want_max ? greater : less) acc = x;
    }
    break;
  }
  default:
    return INTERP_UNDEFINED;
  }
  const Vec r = {{acc & lane_mask(out_esize), 0}};
  vwrite(s, rd, r, false);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Vector and scalar: three different (widening / narrowing).           */
/* ------------------------------------------------------------------ */

static Interp_Status vector_three_different(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 15, 12);
  const uint32_t rm = bits(insn, 20, 16), rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (size == 3) return INTERP_UNDEFINED; /* PMULL .1Q is the crypto extension */
  const unsigned esize = 8u << size, wide = esize * 2u, count = 64u / esize, part = q ? count : 0u;
  const Vec n = vread(s, rn), m = vread(s, rm), dv = vread(s, rd);
  Vec r = {{0, 0}};

  if (opcode == 0x4 || opcode == 0x6) { /* ADDHN/RADDHN, SUBHN/RSUBHN: narrow into a part */
    uint64_t half = 0;
    for (unsigned i = 0; i < count; i++) {
      const uint64_t a = ue(&n, i, wide), b = ue(&m, i, wide);
      uint64_t x = opcode == 0x4 ? a + b : a - b;
      x &= lane_mask(wide);
      if (u) x = (x + ((uint64_t)1 << (esize - 1u))) & lane_mask(wide);
      half |= ((x >> esize) & lane_mask(esize)) << (i * esize);
    }
    vwrite_part(s, rd, half, q);
    return advance(s);
  }

  for (unsigned i = 0; i < count; i++) {
    const uint64_t an = ue(&n, part + i, esize), am = ue(&m, part + i, esize);
    const int64_t xn = u ? (int64_t)an : sign_extend(an, esize);
    const int64_t xm = u ? (int64_t)am : sign_extend(am, esize);
    const uint64_t d = ue(&dv, i, wide);
    uint64_t x;
    switch (opcode) {
    case 0x0: x = (uint64_t)(xn + xm); break;                           /* ADDL */
    case 0x1: x = ue(&n, i, wide) + (uint64_t)xm; break;                 /* ADDW */
    case 0x2: x = (uint64_t)(xn - xm); break;                           /* SUBL */
    case 0x3: x = ue(&n, i, wide) - (uint64_t)xm; break;                 /* SUBW */
    case 0x5: x = d + (uint64_t)(xn > xm ? xn - xm : xm - xn); break;   /* ABAL */
    case 0x7: x = (uint64_t)(xn > xm ? xn - xm : xm - xn); break;       /* ABDL */
    case 0x8: x = d + (uint64_t)xn * (uint64_t)xm; break;               /* MLAL */
    case 0xA: x = d - (uint64_t)xn * (uint64_t)xm; break;               /* MLSL */
    case 0xC: x = (uint64_t)xn * (uint64_t)xm; break;                   /* MULL */
    case 0x9: case 0xB: case 0xD: { /* SQDMLAL, SQDMLSL, SQDMULL */
      if (u || size == 0) return INTERP_UNDEFINED;
      const int64_t product = sqdmull_value(s, xn, xm, esize);
      if (opcode == 0xD) x = (uint64_t)product;
      else if (opcode == 0x9) x = sqadd_wide(s, sign_extend(d, wide), product, wide);
      else x = sqsub_wide(s, sign_extend(d, wide), product, wide);
      break;
    }
    case 0xE: /* PMULL (8 -> 16) */
      if (u || size != 0) return INTERP_UNDEFINED;
      x = polynomial_multiply(an, am, 8);
      break;
    default:
      return INTERP_UNDEFINED;
    }
    put(&r, i, wide, x & lane_mask(wide));
  }
  vwrite(s, rd, r, true);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Shift by immediate (vector and scalar share the lane logic).        */
/* ------------------------------------------------------------------ */

typedef struct Shift_Imm {
  unsigned esize;
  unsigned right; /* 2*esize - immh:immb */
  unsigned left;  /* immh:immb - esize */
} Shift_Imm;

static bool decode_shift_imm(uint32_t insn, Shift_Imm *out) {
  const uint32_t immh = bits(insn, 22, 19), immhb = bits(insn, 22, 16);
  if (immh == 0) return false;
  unsigned esize = 8;
  if (immh & 8u) esize = 64;
  else if (immh & 4u) esize = 32;
  else if (immh & 2u) esize = 16;
  out->esize = esize;
  out->right = 2u * esize - immhb;
  out->left = immhb - esize;
  return true;
}

/* Lane op for the non-narrowing, non-widening shifts. false = undefined. */
static bool shift_imm_lane(Interp_State *s, bool u, unsigned opcode, const Shift_Imm *sh, uint64_t a, uint64_t d,
                           uint64_t *out) {
  const unsigned esize = sh->esize, n = sh->right;
  const uint64_t m = lane_mask(esize);
  const int64_t sa = sign_extend(a, esize);
  switch (opcode) {
  case 0x00: *out = (u ? lsr64(a, n) : (uint64_t)asr64(sa, n)) & m; return true;          /* SSHR/USHR */
  case 0x02: *out = (d + (u ? lsr64(a, n) : (uint64_t)asr64(sa, n))) & m; return true;    /* SSRA/USRA */
  case 0x04: *out = (u ? urshr(a, n) : (uint64_t)srshr(sa, n)) & m; return true;          /* SRSHR/URSHR */
  case 0x06: *out = (d + (u ? urshr(a, n) : (uint64_t)srshr(sa, n))) & m; return true;    /* SRSRA/URSRA */
  case 0x08: { /* SRI */
    if (!u) return false;
    const uint64_t mask = lsr64(m, n);
    *out = (d & ~mask & m) | (lsr64(a, n) & mask);
    return true;
  }
  case 0x0A: { /* SHL / SLI */
    const uint64_t shifted = shl64(a, sh->left) & m;
    if (!u) *out = shifted;
    else *out = (d & ~(shl64(m, sh->left) & m) & m) | shifted;
    return true;
  }
  case 0x0C: /* SQSHLU */
    if (!u) return false;
    if (sa < 0) { if (sa != 0) set_qc(s); *out = 0; return true; }
    *out = shift_by_register(s, a, sh->left, esize, true, false, true);
    return true;
  case 0x0E: /* SQSHL / UQSHL (immediate) */
    *out = shift_by_register(s, a, sh->left, esize, u, false, true);
    return true;
  default:
    return false;
  }
}

/* Narrowing shift lane: 2*esize source -> esize. kind per opcode/u. */
static uint64_t shift_narrow_lane(Interp_State *s, bool u, unsigned opcode, unsigned esize, unsigned n, uint64_t a) {
  const unsigned wide = esize * 2u;
  const bool rounding = opcode & 1u;
  const int64_t sa = sign_extend(a, wide);
  switch (opcode) {
  case 0x10: case 0x11:
    if (!u) return (rounding ? urshr(a, n) : lsr64(a, n)) & lane_mask(esize);                /* SHRN/RSHRN */
    return sat_u_from_s(s, rounding ? srshr(sa, n) : asr64(sa, n), esize);                   /* SQSHRUN/SQRSHRUN */
  default: /* 0x12/0x13: SQSHRN/SQRSHRN, UQSHRN/UQRSHRN */
    if (u) return sat_u(s, rounding ? urshr(a, n) : lsr64(a, n), esize);
    return sat_s(s, rounding ? srshr(sa, n) : asr64(sa, n), esize);
  }
}

static Interp_Status vector_shift_immediate(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t opcode = bits(insn, 15, 11), rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  Shift_Imm sh;
  if (!decode_shift_imm(insn, &sh)) return INTERP_UNDEFINED;
  const Vec n = vread(s, rn), dv = vread(s, rd);
  Vec r = {{0, 0}};

  if (opcode >= 0x10 && opcode <= 0x13) { /* narrowing */
    if (sh.esize == 64u) return INTERP_UNDEFINED;
    uint64_t half = 0;
    for (unsigned i = 0; i < 64u / sh.esize; i++) {
      half |= shift_narrow_lane(s, u, opcode, sh.esize, sh.right, ue(&n, i, sh.esize * 2u)) << (i * sh.esize);
    }
    vwrite_part(s, rd, half, q);
    return advance(s);
  }
  if (opcode == 0x14) { /* SSHLL / USHLL */
    if (sh.esize == 64u) return INTERP_UNDEFINED;
    const unsigned count = 64u / sh.esize, base = q ? count : 0u, wide = sh.esize * 2u;
    for (unsigned i = 0; i < count; i++) {
      const uint64_t a = ue(&n, base + i, sh.esize);
      const uint64_t x = u ? a : (uint64_t)sign_extend(a, sh.esize);
      put(&r, i, wide, (x << sh.left) & lane_mask(wide));
    }
    vwrite(s, rd, r, true);
    return advance(s);
  }
  if (sh.esize == 64u && !q) return INTERP_UNDEFINED;
  const unsigned lanes = (q ? 128u : 64u) / sh.esize;
  if (opcode == 0x1C || opcode == 0x1F) { /* SCVTF/UCVTF, FCVTZS/FCVTZU (fixed) */
    if (sh.esize < 32u) return INTERP_UNDEFINED;
    FP_Env env = simd_env(s);
    const FP_Format f = fp_format_of(sh.esize);
    for (unsigned i = 0; i < lanes; i++) {
      const uint64_t a = ue(&n, i, sh.esize);
      put(&r, i, sh.esize,
          opcode == 0x1C ? fp_from_int(f, a, sh.right, !u, sh.esize, &env)
                         : fp_to_int(f, a, sh.right, u, sh.esize, FP_ROUND_ZERO, &env));
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }
  for (unsigned i = 0; i < lanes; i++) {
    uint64_t x;
    if (!shift_imm_lane(s, u, opcode, &sh, ue(&n, i, sh.esize), ue(&dv, i, sh.esize), &x)) return INTERP_UNDEFINED;
    put(&r, i, sh.esize, x);
  }
  vwrite(s, rd, r, q);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Modified immediate, copy, permute, extract, table.                  */
/* ------------------------------------------------------------------ */

static uint64_t replicate32(uint64_t v) { return (v & 0xFFFFFFFFull) | (v << 32); }
static uint64_t replicate16(uint64_t v) { v &= 0xFFFFu; return v | (v << 16) | (v << 32) | (v << 48); }

static Interp_Status vector_modified_immediate(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), op = bit(insn, 29);
  const uint32_t cmode = bits(insn, 15, 12), rd = bits(insn, 4, 0);
  if (bit(insn, 11)) return INTERP_UNDEFINED; /* o2: FP16 FMOV */
  const uint64_t imm8 = (bits(insn, 18, 16) << 5) | bits(insn, 9, 5);
  uint64_t imm;
  switch (cmode >> 1) {
  case 0: imm = replicate32(imm8); break;
  case 1: imm = replicate32(imm8 << 8); break;
  case 2: imm = replicate32(imm8 << 16); break;
  case 3: imm = replicate32(imm8 << 24); break;
  case 4: imm = replicate16(imm8); break;
  case 5: imm = replicate16(imm8 << 8); break;
  case 6: imm = (cmode & 1u) ? replicate32((imm8 << 16) | 0xFFFFu) : replicate32((imm8 << 8) | 0xFFu); break;
  default:
    if (!(cmode & 1u) && !op) {
      imm = imm8 * 0x0101010101010101ull;
    } else if (!(cmode & 1u) && op) {
      imm = 0;
      for (unsigned i = 0; i < 8; i++) if ((imm8 >> i) & 1u) imm |= 0xFFull << (8u * i);
    } else if (!op) {
      imm = replicate32(fp_expand_imm8(FP_SINGLE, (uint32_t)imm8));
    } else {
      if (!q) return INTERP_UNDEFINED;
      imm = fp_expand_imm8(FP_DOUBLE, (uint32_t)imm8);
    }
    break;
  }
  Vec r = vread(s, rd);
  const bool is_orr_bic = (cmode < 12u) && (cmode & 1u);
  if (is_orr_bic) {
    for (int i = 0; i < 2; i++) r.d[i] = op ? (r.d[i] & ~imm) : (r.d[i] | imm);
  } else {
    const bool invert = op && cmode != 14u && cmode != 15u;
    r.d[0] = r.d[1] = invert ? ~imm : imm;
  }
  vwrite(s, rd, r, q);
  return advance(s);
}

static Interp_Status vector_copy(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), op = bit(insn, 29);
  const uint32_t imm5 = bits(insn, 20, 16), imm4 = bits(insn, 14, 11);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  unsigned size = 0;
  while (size < 4 && !((imm5 >> size) & 1u)) size++;
  if (size > 3) return INTERP_UNDEFINED;
  const unsigned esize = 8u << size, index = imm5 >> (size + 1u);
  const Vec n = vread(s, rn);
  Vec r = vread(s, rd);

  if (op) { /* INS (element) */
    if (!q) return INTERP_UNDEFINED;
    put(&r, index, esize, ue(&n, imm4 >> size, esize));
    vwrite(s, rd, r, true);
    return advance(s);
  }
  switch (imm4) {
  case 0x0: { /* DUP (element) */
    if (size == 3 && !q) return INTERP_UNDEFINED;
    const uint64_t x = ue(&n, index, esize);
    Vec d = {{0, 0}};
    for (unsigned i = 0; i < (q ? 128u : 64u) / esize; i++) put(&d, i, esize, x);
    vwrite(s, rd, d, q);
    return advance(s);
  }
  case 0x1: { /* DUP (general) */
    if (size == 3 && !q) return INTERP_UNDEFINED;
    const uint64_t x = xreg(s, rn) & lane_mask(esize);
    Vec d = {{0, 0}};
    for (unsigned i = 0; i < (q ? 128u : 64u) / esize; i++) put(&d, i, esize, x);
    vwrite(s, rd, d, q);
    return advance(s);
  }
  case 0x3: /* INS (general) */
    if (!q) return INTERP_UNDEFINED;
    put(&r, index, esize, xreg(s, rn));
    vwrite(s, rd, r, true);
    return advance(s);
  case 0x5: /* SMOV */
    if (size == 3 || (size == 2 && !q)) return INTERP_UNDEFINED;
    set_reg_width(s, rd, q, (uint64_t)se(&n, index, esize));
    return advance(s);
  case 0x7: /* UMOV */
    if ((q && size != 3) || (!q && size == 3)) return INTERP_UNDEFINED;
    set_xreg(s, rd, ue(&n, index, esize));
    return advance(s);
  default:
    return INTERP_UNDEFINED;
  }
}

static Interp_Status vector_permute(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 14, 12);
  if (size == 3 && !q) return INTERP_UNDEFINED;
  const unsigned esize = 8u << size, lanes = (q ? 128u : 64u) / esize, pairs = lanes / 2u;
  const Vec n = vread(s, bits(insn, 9, 5)), m = vread(s, bits(insn, 20, 16));
  const unsigned part = (opcode >> 2) & 1u;
  Vec r = {{0, 0}};
  switch (opcode & 3u) {
  case 1: /* UZP1/UZP2 */
    for (unsigned i = 0; i < lanes; i++) {
      const unsigned src = 2u * i + part;
      put(&r, i, esize, src < lanes ? ue(&n, src, esize) : ue(&m, src - lanes, esize));
    }
    break;
  case 2: /* TRN1/TRN2 */
    for (unsigned p = 0; p < pairs; p++) {
      put(&r, 2u * p, esize, ue(&n, 2u * p + part, esize));
      put(&r, 2u * p + 1u, esize, ue(&m, 2u * p + part, esize));
    }
    break;
  case 3: /* ZIP1/ZIP2 */
    for (unsigned p = 0; p < pairs; p++) {
      put(&r, 2u * p, esize, ue(&n, part * pairs + p, esize));
      put(&r, 2u * p + 1u, esize, ue(&m, part * pairs + p, esize));
    }
    break;
  default:
    return INTERP_UNDEFINED;
  }
  vwrite(s, bits(insn, 4, 0), r, q);
  return advance(s);
}

static Interp_Status vector_extract(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30);
  const uint32_t imm4 = bits(insn, 14, 11);
  if (bits(insn, 23, 22) != 0 || (!q && (imm4 & 8u))) return INTERP_UNDEFINED;
  const Vec n = vread(s, bits(insn, 9, 5)), m = vread(s, bits(insn, 20, 16));
  const unsigned bytes = q ? 16u : 8u;
  Vec r = {{0, 0}};
  for (unsigned i = 0; i < bytes; i++) {
    const unsigned src = imm4 + i;
    put(&r, i, 8, src < bytes ? ue(&n, src, 8) : ue(&m, src - bytes, 8));
  }
  vwrite(s, bits(insn, 4, 0), r, q);
  return advance(s);
}

static Interp_Status vector_table(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), extension = bit(insn, 12);
  if (bits(insn, 23, 22) != 0) return INTERP_UNDEFINED;
  const unsigned regs = bits(insn, 14, 13) + 1u;
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  Vec table[4];
  for (unsigned i = 0; i < regs; i++) table[i] = vread(s, (rn + i) % 32u);
  const Vec indices = vread(s, bits(insn, 20, 16));
  Vec r = vread(s, rd);
  if (!extension) r = (Vec){{0, 0}};
  for (unsigned i = 0; i < (q ? 16u : 8u); i++) {
    const unsigned index = (unsigned)ue(&indices, i, 8);
    if (index < 16u * regs) put(&r, i, 8, ue(&table[index / 16u], index % 16u, 8));
    else if (!extension) put(&r, i, 8, 0);
  }
  vwrite(s, rd, r, q);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* By element (vector and scalar).                                     */
/* ------------------------------------------------------------------ */

typedef struct Element_Operand {
  unsigned esize;
  unsigned index;
  uint32_t rm;
  bool is_fp;
} Element_Operand;

static bool decode_by_element(uint32_t insn, bool u, Element_Operand *out) {
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 15, 12);
  const uint32_t h = bit(insn, 11), l = bit(insn, 21), m = bit(insn, 20), rm = bits(insn, 19, 16);
  const bool fp = (opcode == 0x1 || opcode == 0x5 || opcode == 0x9) && !(u && opcode != 0x9);
  out->is_fp = fp;
  if (fp) {
    if (!(size & 2u)) return false; /* FP16 */
    if (size & 1u) {
      if (l) return false;
      out->esize = 64;
      out->index = h;
    } else {
      out->esize = 32;
      out->index = (h << 1) | l;
    }
    out->rm = (m << 4) | rm;
    return true;
  }
  switch (size) {
  case 1: out->esize = 16; out->index = (h << 2) | (l << 1) | m; out->rm = rm; return true;
  case 2: out->esize = 32; out->index = (h << 1) | l; out->rm = (m << 4) | rm; return true;
  default: return false;
  }
}

static Interp_Status by_element(Interp_State *s, uint32_t insn, bool scalar) {
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t opcode = bits(insn, 15, 12), rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  Element_Operand e;
  if (!decode_by_element(insn, u, &e)) return INTERP_UNDEFINED;
  const Vec mv = vread(s, e.rm), n = vread(s, rn), dv = vread(s, rd);
  const uint64_t element = ue(&mv, e.index, e.esize);
  const unsigned esize = e.esize;
  Vec r = {{0, 0}};

  if (e.is_fp) {
    if (!scalar && esize == 64u && !q) return INTERP_UNDEFINED;
    const FP_Format f = fp_format_of(esize);
    const unsigned lanes = scalar ? 1u : (q ? 128u : 64u) / esize;
    FP_Env env = simd_env(s);
    for (unsigned i = 0; i < lanes; i++) {
      const uint64_t a = ue(&n, i, esize), d = ue(&dv, i, esize);
      uint64_t x;
      switch (opcode) {
      case 0x1: x = fp_mul_add(f, d, a, element, &env); break;              /* FMLA */
      case 0x5: x = fp_mul_add(f, d, fp_neg(f, a), element, &env); break;   /* FMLS */
      default: x = u ? fp_mulx(f, a, element, &env) : fp_mul(f, a, element, &env); break; /* FMUL(X) */
      }
      put(&r, i, esize, x);
    }
    vwrite(s, rd, r, scalar ? false : q);
    return advance(s);
  }

  const unsigned wide = esize * 2u;
  switch (opcode) {
  case 0x0: case 0x4: case 0x8: { /* MLA, MLS (U=1), MUL (U=0) */
    if (scalar || (opcode == 0x8) == u) return INTERP_UNDEFINED;
    const unsigned lanes = (q ? 128u : 64u) / esize;
    for (unsigned i = 0; i < lanes; i++) {
      const uint64_t prod = ue(&n, i, esize) * element, d = ue(&dv, i, esize);
      put(&r, i, esize, (opcode == 0x8 ? prod : opcode == 0x0 ? d + prod : d - prod) & lane_mask(esize));
    }
    vwrite(s, rd, r, q);
    return advance(s);
  }
  case 0xC: case 0xD: { /* SQDMULH, SQRDMULH */
    if (u) return INTERP_UNDEFINED;
    const unsigned lanes = scalar ? 1u : (q ? 128u : 64u) / esize;
    for (unsigned i = 0; i < lanes; i++) put(&r, i, esize, sqdmulh(s, ue(&n, i, esize), element, esize, opcode == 0xD));
    vwrite(s, rd, r, scalar ? false : q);
    return advance(s);
  }
  case 0x2: case 0x6: case 0xA: case 0x3: case 0x7: case 0xB: { /* long forms */
    const bool saturating = opcode == 0x3 || opcode == 0x7 || opcode == 0xB;
    if (saturating && u) return INTERP_UNDEFINED;
    if (!saturating && scalar) return INTERP_UNDEFINED;
    const unsigned count = scalar ? 1u : 64u / esize, part = (!scalar && q) ? 64u / esize : 0u;
    const int64_t y = u ? (int64_t)element : sign_extend(element, esize);
    for (unsigned i = 0; i < count; i++) {
      const uint64_t a = ue(&n, part + i, esize);
      const int64_t x = u ? (int64_t)a : sign_extend(a, esize);
      const uint64_t d = ue(&dv, i, wide);
      uint64_t out;
      if (saturating) {
        const int64_t product = sqdmull_value(s, x, y, esize);
        if (opcode == 0xB) out = (uint64_t)product;
        else if (opcode == 0x3) out = sqadd_wide(s, sign_extend(d, wide), product, wide);
        else out = sqsub_wide(s, sign_extend(d, wide), product, wide);
      } else {
        const uint64_t product = (uint64_t)x * (uint64_t)y;
        out = opcode == 0xA ? product : opcode == 0x2 ? d + product : d - product;
      }
      put(&r, i, wide, out & lane_mask(wide));
    }
    vwrite(s, rd, r, !scalar);
    return advance(s);
  }
  default:
    return INTERP_UNDEFINED;
  }
}

/* ------------------------------------------------------------------ */
/* Scalar forms.                                                       */
/* ------------------------------------------------------------------ */

static void write_scalar_lane(Interp_State *s, uint32_t rd, unsigned esize, uint64_t value) {
  const Vec r = {{value & lane_mask(esize), 0}};
  vwrite(s, rd, r, false);
}

static Interp_Status scalar_three_same(Interp_State *s, uint32_t insn) {
  const bool u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 15, 11);
  const uint32_t rd = bits(insn, 4, 0);
  const Vec n = vread(s, bits(insn, 9, 5)), m = vread(s, bits(insn, 20, 16));
  if (opcode >= 0x18) {
    const bool sz = size & 1u, a1 = (size >> 1) & 1u;
    const unsigned key = ((unsigned)u << 4) | ((unsigned)a1 << 3) | (opcode & 7u);
    /* Scalar FP three-same: FMULX, FCMEQ, FRECPS, FRSQRTS, FCMGE, FACGE, FABD, FCMGT, FACGT */
    if (key != 0x03 && key != 0x04 && key != 0x07 && key != 0x0F && key != 0x14 && key != 0x15 && key != 0x1A &&
        key != 0x1C && key != 0x1D) {
      return INTERP_UNDEFINED;
    }
    const unsigned esize = sz ? 64u : 32u;
    uint64_t x;
    (void)fp_three_same(s, u, a1, opcode, fp_format_of(esize), ue(&n, 0, esize), ue(&m, 0, esize), 0, &x);
    write_scalar_lane(s, rd, esize, x);
    return advance(s);
  }
  const unsigned esize = 8u << size;
  switch (opcode) {
  case 0x01: case 0x05: case 0x09: case 0x0B: break;            /* QADD, QSUB, QSHL, QRSHL: any size */
  case 0x06: case 0x07: case 0x08: case 0x0A: case 0x10: case 0x11:
    if (size != 3) return INTERP_UNDEFINED;                     /* CMGT.., SSHL, SRSHL, ADD/SUB, CMTST/CMEQ */
    break;
  case 0x16:
    if (size != 1 && size != 2) return INTERP_UNDEFINED;        /* SQDMULH / SQRDMULH */
    break;
  default:
    return INTERP_UNDEFINED;
  }
  uint64_t x;
  if (!int_three_same(s, u, opcode, esize, ue(&n, 0, esize), ue(&m, 0, esize), 0, &x)) return INTERP_UNDEFINED;
  write_scalar_lane(s, rd, esize, x);
  return advance(s);
}

static Interp_Status scalar_two_misc(Interp_State *s, uint32_t insn) {
  const bool u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 16, 12);
  const uint32_t rd = bits(insn, 4, 0);
  const Vec n = vread(s, bits(insn, 9, 5)), dv = vread(s, rd);
  const unsigned key = ((unsigned)u << 5) | opcode;

  /* FP: FCVTxx/xCVTF/FRECPE/FRSQRTE/FRECPX/FCMxx #0 on one S or D lane. */
  if (opcode >= 0x1A || (opcode >= 0x0C && opcode <= 0x0E)) {
    const bool sz = size & 1u, a1 = (size >> 1) & 1u;
    const unsigned esize = sz ? 64u : 32u;
    const FP_Format f = fp_format_of(esize);
    uint64_t x;
    if (opcode == 0x1F && !u && a1) { /* FRECPX */
      FP_Env env = simd_env(s);
      x = fp_recip_exponent(f, ue(&n, 0, esize), &env);
    } else if ((opcode == 0x1C && a1) || opcode == 0x1F) {
      return INTERP_UNDEFINED; /* URECPE/URSQRTE and FSQRT have no scalar form */
    } else if (!fp_misc(s, u, a1, opcode, f, ue(&n, 0, esize), &x)) {
      return INTERP_UNDEFINED;
    }
    write_scalar_lane(s, rd, esize, x);
    return advance(s);
  }
  if (key == 0x36) { /* FCVTXN (scalar) */
    if (size != 1) return INTERP_UNDEFINED;
    FP_Env env = simd_env(s);
    write_scalar_lane(s, rd, 32, fp_convert_round_odd(ue(&n, 0, 64), &env));
    return advance(s);
  }
  if (key == 0x14 || key == 0x34 || key == 0x32) { /* SQXTN, UQXTN, SQXTUN */
    if (size == 3) return INTERP_UNDEFINED;
    const unsigned esize = 8u << size;
    const unsigned kind = key == 0x14 ? 1u : (key == 0x34 ? 2u : 3u);
    write_scalar_lane(s, rd, esize, narrow(s, kind, ue(&n, 0, esize * 2u), esize));
    return advance(s);
  }
  const unsigned esize = 8u << size;
  switch (key) {
  case 0x03: case 0x23: case 0x07: case 0x27: break; /* SUQADD, USQADD, SQABS, SQNEG: any size */
  case 0x08: case 0x09: case 0x0A: case 0x28: case 0x29: case 0x0B: case 0x2B:
    if (size != 3) return INTERP_UNDEFINED;
    break;
  default:
    return INTERP_UNDEFINED;
  }
  uint64_t x;
  if (!int_misc_lane(s, u, opcode, esize, ue(&n, 0, esize), ue(&dv, 0, esize), &x)) return INTERP_UNDEFINED;
  write_scalar_lane(s, rd, esize, x);
  return advance(s);
}

static Interp_Status scalar_pairwise(Interp_State *s, uint32_t insn) {
  const bool u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 16, 12), rd = bits(insn, 4, 0);
  const Vec n = vread(s, bits(insn, 9, 5));
  if (!u && opcode == 0x1B) { /* ADDP (scalar): D from 2D */
    if (size != 3) return INTERP_UNDEFINED;
    write_scalar_lane(s, rd, 64, n.d[0] + n.d[1]);
    return advance(s);
  }
  if (!u) return INTERP_UNDEFINED;
  const bool sz = size & 1u, a1 = (size >> 1) & 1u;
  unsigned which;
  switch (((unsigned)a1 << 5) | opcode) {
  case 0x0C: which = 0; break; /* FMAXNMP */
  case 0x0D: which = 1; break; /* FADDP */
  case 0x0F: which = 2; break; /* FMAXP */
  case 0x2C: which = 3; break; /* FMINNMP */
  case 0x2F: which = 4; break; /* FMINP */
  default: return INTERP_UNDEFINED;
  }
  const unsigned esize = sz ? 64u : 32u;
  uint64_t x;
  (void)fp_pairwise_op(s, which, fp_format_of(esize), ue(&n, 0, esize), ue(&n, 1, esize), &x);
  write_scalar_lane(s, rd, esize, x);
  return advance(s);
}

static Interp_Status scalar_shift_immediate(Interp_State *s, uint32_t insn) {
  const bool u = bit(insn, 29);
  const uint32_t opcode = bits(insn, 15, 11), rd = bits(insn, 4, 0);
  Shift_Imm sh;
  if (!decode_shift_imm(insn, &sh)) return INTERP_UNDEFINED;
  const Vec n = vread(s, bits(insn, 9, 5)), dv = vread(s, rd);
  if (opcode >= 0x10 && opcode <= 0x13) { /* narrowing: saturating forms only */
    if (sh.esize == 64u || (!u && opcode <= 0x11)) return INTERP_UNDEFINED;
    write_scalar_lane(s, rd, sh.esize, shift_narrow_lane(s, u, opcode, sh.esize, sh.right, ue(&n, 0, sh.esize * 2u)));
    return advance(s);
  }
  if (opcode == 0x1C || opcode == 0x1F) {
    if (sh.esize < 32u) return INTERP_UNDEFINED;
    FP_Env env = simd_env(s);
    const FP_Format f = fp_format_of(sh.esize);
    const uint64_t a = ue(&n, 0, sh.esize);
    write_scalar_lane(s, rd, sh.esize,
                      opcode == 0x1C ? fp_from_int(f, a, sh.right, !u, sh.esize, &env)
                                     : fp_to_int(f, a, sh.right, u, sh.esize, FP_ROUND_ZERO, &env));
    return advance(s);
  }
  /* SSHR..SLI exist only for D; the saturating left shifts for any size. */
  if (opcode != 0x0C && opcode != 0x0E && sh.esize != 64u) return INTERP_UNDEFINED;
  uint64_t x;
  if (!shift_imm_lane(s, u, opcode, &sh, ue(&n, 0, sh.esize), ue(&dv, 0, sh.esize), &x)) return INTERP_UNDEFINED;
  write_scalar_lane(s, rd, sh.esize, x);
  return advance(s);
}

static Interp_Status scalar_three_different(Interp_State *s, uint32_t insn) {
  const uint32_t size = bits(insn, 23, 22), opcode = bits(insn, 15, 12), rd = bits(insn, 4, 0);
  if (bit(insn, 29) || (size != 1 && size != 2) || (opcode != 0x9 && opcode != 0xB && opcode != 0xD)) {
    return INTERP_UNDEFINED;
  }
  const unsigned esize = 8u << size, wide = esize * 2u;
  const Vec n = vread(s, bits(insn, 9, 5)), m = vread(s, bits(insn, 20, 16)), dv = vread(s, rd);
  const int64_t product = sqdmull_value(s, se(&n, 0, esize), se(&m, 0, esize), esize);
  uint64_t x;
  if (opcode == 0xD) x = (uint64_t)product;
  else if (opcode == 0x9) x = sqadd_wide(s, se(&dv, 0, wide), product, wide);
  else x = sqsub_wide(s, se(&dv, 0, wide), product, wide);
  write_scalar_lane(s, rd, wide, x);
  return advance(s);
}

static Interp_Status scalar_copy(Interp_State *s, uint32_t insn) {
  if (bit(insn, 29) || bits(insn, 14, 11) != 0) return INTERP_UNDEFINED;
  const uint32_t imm5 = bits(insn, 20, 16);
  unsigned size = 0;
  while (size < 4 && !((imm5 >> size) & 1u)) size++;
  if (size > 3) return INTERP_UNDEFINED;
  const unsigned esize = 8u << size;
  const Vec n = vread(s, bits(insn, 9, 5));
  write_scalar_lane(s, bits(insn, 4, 0), esize, ue(&n, imm5 >> (size + 1u), esize));
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Group decode.                                                       */
/* ------------------------------------------------------------------ */

Interp_Status interp_simd_vector(Interp_State *s, uint32_t insn) {
  if (bit(insn, 31)) return INTERP_UNDEFINED;
  if (bits(insn, 28, 24) == 0x0E) {
    if (bit(insn, 21)) {
      if (bit(insn, 10)) return vector_three_same(s, insn);
      if (bits(insn, 11, 10) == 2) {
        if (bits(insn, 20, 17) == 0x0) return vector_two_misc(s, insn);
        if (bits(insn, 20, 17) == 0x8) return vector_across(s, insn);
        return INTERP_UNDEFINED; /* AES and friends */
      }
      if (bits(insn, 11, 10) == 0) return vector_three_different(s, insn);
      return INTERP_UNDEFINED;
    }
    if (bit(insn, 10)) {
      if (bits(insn, 23, 21) == 0 && !bit(insn, 15)) return vector_copy(s, insn);
      return INTERP_UNDEFINED; /* three-register extension (8.1+) */
    }
    if (bit(insn, 15)) return INTERP_UNDEFINED;
    if (bit(insn, 29)) return vector_extract(s, insn); /* bit 11 is imm4<0>; bit 10 == 0 checked above */
    if (bits(insn, 11, 10) == 0) return vector_table(s, insn);
    if (bits(insn, 11, 10) == 2) return vector_permute(s, insn);
    return INTERP_UNDEFINED;
  }
  /* bits 28:24 == 01111 */
  if (bit(insn, 10)) {
    if (bits(insn, 23, 19) == 0) return vector_modified_immediate(s, insn);
    if (bit(insn, 23)) return INTERP_UNDEFINED;
    return vector_shift_immediate(s, insn);
  }
  return by_element(s, insn, false);
}

Interp_Status interp_simd_scalar(Interp_State *s, uint32_t insn) {
  if (bit(insn, 31)) return INTERP_UNDEFINED;
  if (bits(insn, 28, 24) == 0x1E) {
    if (bit(insn, 21)) {
      if (bit(insn, 10)) return scalar_three_same(s, insn);
      if (bits(insn, 11, 10) == 2) {
        if (bits(insn, 20, 17) == 0x0) return scalar_two_misc(s, insn);
        if (bits(insn, 20, 17) == 0x8) return scalar_pairwise(s, insn);
        return INTERP_UNDEFINED;
      }
      if (bits(insn, 11, 10) == 0) return scalar_three_different(s, insn);
      return INTERP_UNDEFINED;
    }
    if (bit(insn, 10) && bits(insn, 23, 21) == 0 && !bit(insn, 15)) return scalar_copy(s, insn);
    return INTERP_UNDEFINED; /* SHA, three-register extension */
  }
  /* bits 28:24 == 11111 */
  if (bit(insn, 10)) {
    if (bits(insn, 23, 19) == 0 || bit(insn, 23)) return INTERP_UNDEFINED;
    return scalar_shift_immediate(s, insn);
  }
  return by_element(s, insn, true);
}
