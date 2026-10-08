/**
 * Advanced SIMD (NEON) for the A32 interpreter: the unconditional
 * 1111 001x data-processing space and the 1111 0100 xxx0 element and
 * structure loads and stores, plus the ARMv8 AES/SHA instructions that
 * live in it.
 *
 * Registers are D0-D31 (a32_d), a Q register the even/odd pair. Lanes and
 * saturation come from the A64 interpreter's simd_lanes.h; floating point
 * is its softfloat under the "standard FPSCR value" Advanced SIMD always
 * uses (flush-to-zero, default NaN, round to nearest), with the cumulative
 * flags (and QC) landing in FPSCR.
 */
#include "cpu/backends/a32/a32_internal.h"
#include "cpu/backends/interpreter/simd_lanes.h"

#include <string.h>

static inline uint32_t f(uint32_t insn, unsigned hi, unsigned lo) { return bits(insn, hi, lo); }
static inline uint32_t b1(uint32_t insn, unsigned n) { return bit(insn, n); }

#define D_REGS 32u
#define D_BITS 64u
#define Q_BITS 128u
#define BYTE_BITS 8u
#define WORD_BITS 32u
#define AES_BYTES 16u
#define AES_POLY 0x1Bu   /* x^8 = x^4 + x^3 + x + 1 in GF(2^8) */
#define AES_AFFINE 0x63u /* SubBytes' affine constant */
#define SHIFT_BYTE_MASK 0xFFu

static uint32_t reg_d(uint32_t insn) { return (b1(insn, 22) << 4) | f(insn, 15, 12); }
static uint32_t reg_n(uint32_t insn) { return (b1(insn, 7) << 4) | f(insn, 19, 16); }
static uint32_t reg_m(uint32_t insn) { return (b1(insn, 5) << 4) | f(insn, 3, 0); }

static Vec get_v(const A32_State *a, uint32_t d, bool q) {
  Vec v = {{a32_d(a, d & (D_REGS - 1u)), q ? a32_d(a, (d + 1u) & (D_REGS - 1u)) : 0}};
  return v;
}

static void set_v(A32_State *a, uint32_t d, bool q, Vec v) {
  a32_set_d(a, d & (D_REGS - 1u), v.d[0]);
  if (q) a32_set_d(a, (d + 1u) & (D_REGS - 1u), v.d[1]);
}

/* Advanced SIMD arithmetic ignores FPSCR's controls but AHP. */
static FP_Env neon_env(A32_State *a) {
  FP_Env e = {FPCR_DN | FPCR_FZ | (a->s.fpcr & FPCR_AHP), &a->s.fpsr};
  return e;
}

static uint64_t ones(unsigned esize) { return lane_mask(esize); }

/* ---- shifts shared by the register and immediate forms ------------- */

/* value >> amount, arithmetic when is_signed (amount may reach 64+). */
static uint64_t shr(uint64_t value, bool is_signed, unsigned esize, unsigned amount) {
  if (is_signed) {
    const int64_t v = sign_extend(value, esize);
    return (uint64_t)(amount >= D_BITS ? (v < 0 ? -1 : 0) : v >> amount) & ones(esize);
  }
  return amount >= D_BITS ? 0 : (value & ones(esize)) >> amount;
}

/* Rounded right shift (amount >= 1): floor((v + 2^(amount-1)) / 2^amount)
 * computed without the overflowing add. */
static uint64_t rshr(uint64_t value, bool is_signed, unsigned esize, unsigned amount) {
  if (amount > esize) return 0;
  if (is_signed) {
    const int64_t v = sign_extend(value, esize) >> (amount - 1u);
    return (uint64_t)((v >> 1) + (v & 1)) & ones(esize);
  }
  const uint64_t v = (value & ones(esize)) >> (amount - 1u);
  return ((v >> 1) + (v & 1u)) & ones(esize);
}

/* Saturating left shift of an esize lane (value signed or unsigned), the
 * result saturated to signed or unsigned (VQSHLU: signed in, unsigned out). */
static uint64_t qshl(A32_State *a, uint64_t value, bool in_signed, bool out_signed, unsigned esize, unsigned amount) {
  const int64_t sv = in_signed ? sign_extend(value, esize) : (int64_t)(value & ones(esize));
  if (in_signed && sv < 0 && !out_signed) { set_qc(&a->s); return 0; }
  if (sv == 0) return 0;
  const uint64_t max = out_signed ? ones(esize) >> 1 : ones(esize);
  if (in_signed && sv < 0) {
    const uint64_t magnitude = (uint64_t)(-(sv + 1)); /* |sv| - 1, no overflow */
    if (amount >= esize || (magnitude >> (esize - 1u - amount)) != 0) {
      set_qc(&a->s);
      return (max + 1u) & ones(esize); /* the signed minimum */
    }
    return ((uint64_t)sv << amount) & ones(esize);
  }
  const uint64_t uv = (uint64_t)sv;
  if (amount >= esize || (amount && (uv >> (D_BITS - amount)) != 0) || (uv << amount) > max) {
    set_qc(&a->s);
    return max;
  }
  return uv << amount;
}

/* VSHL/VQSHL/VRSHL/VQRSHL (register): shift by the signed low byte. */
static uint64_t shift_by_reg(A32_State *a, uint64_t value, int32_t shift, bool is_signed, unsigned esize, bool round,
                             bool saturate) {
  if (shift >= 0) {
    if (saturate) return qshl(a, value, is_signed, is_signed, esize, (unsigned)shift);
    return (unsigned)shift >= esize ? 0 : (value << shift) & ones(esize);
  }
  const unsigned amount = (unsigned)-shift;
  return round ? rshr(value, is_signed, esize, amount) : shr(value, is_signed, esize, amount);
}

/* ---- three registers of the same length ------------------------------ */

static uint64_t poly_mul8(uint64_t x, uint64_t y) {
  uint64_t r = 0;
  for (unsigned i = 0; i < BYTE_BITS; i++)
    if ((y >> i) & 1u) r ^= x << i;
  return r;
}

static Interp_Status sha3(A32_State *a, uint32_t insn);

#define F32_EXP_MASK 0x7F800000u
#define F32_FRAC_MASK 0x007FFFFFu
#define F32_ABS_MASK 0x7FFFFFFFu
#define F32_ONE_POINT_FIVE 0x3FC00000u
#define F32_TWO 0x40000000u
#define F32_THREE 0x40400000u
#define F32_HALF 0x3F000000u

static bool f32_is_nan(uint64_t x) { return (x & F32_EXP_MASK) == F32_EXP_MASK && (x & F32_FRAC_MASK); }

/* VRECPS (2 - a*b) and VRSQRTS ((3 - a*b) / 2): unlike AArch64's
 * FRECPS/FRSQRTS the product is rounded on its own (FPRecipStep,
 * FPRSqrtStep); infinity times zero gives exactly 2 or 1.5. */
static uint64_t newton_step(uint64_t x, uint64_t y, bool rsqrt, FP_Env *env) {
  if (!f32_is_nan(x) && !f32_is_nan(y)) {
    const bool x_inf = (x & F32_ABS_MASK) == F32_EXP_MASK, y_inf = (y & F32_ABS_MASK) == F32_EXP_MASK;
    const bool x_zero = (x & F32_EXP_MASK) == 0, y_zero = (y & F32_EXP_MASK) == 0; /* FZ: denormals are zeros */
    if ((x_inf && y_zero) || (x_zero && y_inf)) {
      if ((x_zero && (x & F32_FRAC_MASK)) || (y_zero && (y & F32_FRAC_MASK))) *env->fpsr |= FPSR_IDC;
      return rsqrt ? F32_ONE_POINT_FIVE : F32_TWO;
    }
  }
  const uint64_t product = fp_mul(FP_SINGLE, x, y, env);
  if (!rsqrt) return fp_sub(FP_SINGLE, F32_TWO, product, env);
  return fp_mul(FP_SINGLE, fp_sub(FP_SINGLE, F32_THREE, product, env), F32_HALF, env);
}

static Interp_Status three_same_float(A32_State *a, uint32_t insn, uint32_t d, uint32_t n, uint32_t m, bool q) {
  const uint32_t op = f(insn, 11, 8), B = b1(insn, 4), U = b1(insn, 24), hi = b1(insn, 21);
  if (b1(insn, 20)) return INTERP_UNDEFINED; /* sz = 1: half precision is not in ARMv8.0 */
  FP_Env env = neon_env(a);
  const Vec vn = get_v(a, n, q), vm = get_v(a, m, q), vd = get_v(a, d, q);
  const unsigned elements = (q ? Q_BITS : D_BITS) / WORD_BITS;
  const bool pairwise = (op == 0xDu && !B && U && !hi) || (op == 0xFu && !B && U);
  if (pairwise && q) return INTERP_UNDEFINED;
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < elements; e++) {
    uint64_t x = ue(&vn, e, WORD_BITS), y = ue(&vm, e, WORD_BITS);
    if (pairwise) {
      const Vec *src = e < elements / 2u ? &vn : &vm;
      const unsigned pair = (e % (elements / 2u)) * 2u;
      x = ue(src, pair, WORD_BITS);
      y = ue(src, pair + 1u, WORD_BITS);
    }
    const uint64_t acc = ue(&vd, e, WORD_BITS);
    uint64_t v;
    switch (op) {
      case 0xC: /* VFMA / VFMS */
        if (!B || U) return INTERP_UNDEFINED;
        v = fp_mul_add(FP_SINGLE, acc, hi ? fp_neg(FP_SINGLE, x) : x, y, &env);
        break;
      case 0xD:
        if (!B) {
          if (!U) v = hi ? fp_sub(FP_SINGLE, x, y, &env) : fp_add(FP_SINGLE, x, y, &env);
          else v = hi ? fp_abs(FP_SINGLE, fp_sub(FP_SINGLE, x, y, &env)) : fp_add(FP_SINGLE, x, y, &env);
        } else if (!U) { /* VMLA / VMLS: two roundings */
          const uint64_t product = fp_mul(FP_SINGLE, x, y, &env);
          v = fp_add(FP_SINGLE, acc, hi ? fp_neg(FP_SINGLE, product) : product, &env);
        } else {
          if (hi) return INTERP_UNDEFINED;
          v = fp_mul(FP_SINGLE, x, y, &env);
        }
        break;
      case 0xE:
        if (!B) {
          if (!U && !hi) v = fp_compare_eq(FP_SINGLE, x, y, &env) ? ones(WORD_BITS) : 0;
          else if (U) v = (hi ? fp_compare_gt(FP_SINGLE, x, y, &env) : fp_compare_ge(FP_SINGLE, x, y, &env)) ? ones(WORD_BITS) : 0;
          else return INTERP_UNDEFINED;
        } else {
          if (!U) return INTERP_UNDEFINED;
          const uint64_t ax = fp_abs(FP_SINGLE, x), ay = fp_abs(FP_SINGLE, y);
          v = (hi ? fp_compare_gt(FP_SINGLE, ax, ay, &env) : fp_compare_ge(FP_SINGLE, ax, ay, &env)) ? ones(WORD_BITS) : 0;
        }
        break;
      default: /* 0xF */
        if (!B) v = hi ? fp_min(FP_SINGLE, x, y, &env) : fp_max(FP_SINGLE, x, y, &env);
        else if (!U) v = newton_step(x, y, hi, &env);
        else v = hi ? fp_min_num(FP_SINGLE, x, y, &env) : fp_max_num(FP_SINGLE, x, y, &env);
        break;
    }
    put(&r, e, WORD_BITS, v);
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

static Interp_Status three_same(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 11, 8), B = b1(insn, 4), U = b1(insn, 24), size = f(insn, 21, 20);
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn), n = reg_n(insn), m = reg_m(insn);
  if (q && ((d | n | m) & 1u)) return INTERP_UNDEFINED;
  if (op == 0xCu && !B) return sha3(a, insn);
  if (op >= 0xDu || op == 0xCu) return three_same_float(a, insn, d, n, m, q);
  const Vec vn = get_v(a, n, q), vm = get_v(a, m, q), vd = get_v(a, d, q);
  if (op == 1u && B) { /* bitwise */
    Vec r;
    for (unsigned i = 0; i < 2u; i++) {
      const uint64_t x = vn.d[i], y = vm.d[i], z = vd.d[i];
      switch ((U << 2) | size) {
        case 0: r.d[i] = x & y; break;              /* VAND */
        case 1: r.d[i] = x & ~y; break;             /* VBIC */
        case 2: r.d[i] = x | y; break;              /* VORR (VMOV) */
        case 3: r.d[i] = x | ~y; break;             /* VORN */
        case 4: r.d[i] = x ^ y; break;              /* VEOR */
        case 5: r.d[i] = (x & z) | (y & ~z); break; /* VBSL */
        case 6: r.d[i] = (x & y) | (z & ~y); break; /* VBIT */
        default: r.d[i] = (z & y) | (x & ~y); break; /* VBIF */
      }
    }
    set_v(a, d, q, r);
    return INTERP_CONTINUE;
  }
  const unsigned esize = BYTE_BITS << size, elements = (q ? Q_BITS : D_BITS) / esize;
  const bool is_signed = !U;
  const bool wide_ok = op == 0u ? B : op == 2u ? B : op == 4u || op == 5u || (op == 8u && !B);
  if (size == 3u && !wide_ok) return INTERP_UNDEFINED;
  const bool pairwise = op == 0xAu || (op == 0xBu && B);
  if (pairwise && q) return INTERP_UNDEFINED;
  if (op == 0xBu && ((!B && (size == 0u || size == 3u)) || (B && U))) return INTERP_UNDEFINED;
  if (op == 9u && B && U && size != 0u) return INTERP_UNDEFINED; /* VMUL.P8 only */
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < elements; e++) {
    uint64_t x = ue(&vn, e, esize), y = ue(&vm, e, esize);
    if (pairwise) {
      const Vec *src = e < elements / 2u ? &vn : &vm;
      const unsigned pair = (e % (elements / 2u)) * 2u;
      x = ue(src, pair, esize);
      y = ue(src, pair + 1u, esize);
    }
    const int64_t sx = sign_extend(x, esize), sy = sign_extend(y, esize);
    const uint64_t acc = ue(&vd, e, esize);
    uint64_t v = 0;
    switch (op) {
      case 0x0:
        if (!B) v = (uint64_t)(is_signed ? (sx + sy) >> 1 : (int64_t)((x + y) >> 1));
        else if (esize == D_BITS) v = is_signed ? sqadd64(&a->s, x, y) : uqadd64(&a->s, x, y);
        else v = is_signed ? sat_s(&a->s, sx + sy, esize) : sat_u(&a->s, x + y, esize);
        break;
      case 0x1: /* VRHADD */
        v = (uint64_t)(is_signed ? (sx + sy + 1) >> 1 : (int64_t)((x + y + 1u) >> 1));
        break;
      case 0x2:
        if (!B) v = (uint64_t)(is_signed ? (sx - sy) >> 1 : ((int64_t)x - (int64_t)y) >> 1);
        else if (esize == D_BITS) v = is_signed ? sqsub64(&a->s, x, y) : uqsub64(&a->s, x, y);
        else v = is_signed ? sat_s(&a->s, sx - sy, esize) : sat_u_from_s(&a->s, (int64_t)x - (int64_t)y, esize);
        break;
      case 0x3: { /* VCGT / VCGE */
        const bool gt = is_signed ? sx > sy : x > y, eq = x == y;
        v = (B ? gt || eq : gt) ? ones(esize) : 0;
        break;
      }
      case 0x4:
      case 0x5: /* shift Vm's lane by Vn's: x is n, y is m in the encoding's terms */
        v = shift_by_reg(a, y, (int8_t)(x & SHIFT_BYTE_MASK), is_signed, esize, op == 5u, B);
        break;
      case 0x6:
        if (is_signed) v = (uint64_t)((B ? sx < sy : sx > sy) ? sx : sy);
        else v = (B ? x < y : x > y) ? x : y;
        break;
      case 0x7: {
        const uint64_t diff = is_signed ? (uint64_t)(sx > sy ? sx - sy : sy - sx) : (x > y ? x - y : y - x);
        v = B ? acc + diff : diff;
        break;
      }
      case 0x8:
        if (!B) v = U ? x - y : x + y;
        else v = (U ? x == y : (x & y) != 0) ? ones(esize) : 0;
        break;
      case 0x9:
        if (!B) v = U ? acc - x * y : acc + x * y;
        else v = U ? poly_mul8(x, y) : x * y;
        break;
      case 0xA:
        if (is_signed) v = (uint64_t)((B ? sx < sy : sx > sy) ? sx : sy);
        else v = (B ? x < y : x > y) ? x : y;
        break;
      default: /* 0xB */
        if (B) {
          v = x + y; /* VPADD */
        } else {
          const int64_t min = -(int64_t)(ones(esize) >> 1) - 1;
          if (sx == min && sy == min) {
            set_qc(&a->s);
            v = ones(esize) >> 1;
          } else {
            int64_t product = 2 * sx * sy;
            if (U) product += (int64_t)1 << (esize - 1u); /* VQRDMULH */
            v = sat_s(&a->s, product >> esize, esize);
          }
        }
        break;
    }
    put(&r, e, esize, v & ones(esize));
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- one register and a modified immediate ----------------------------- */

static Interp_Status modified_immediate(A32_State *a, uint32_t insn) {
  const uint32_t cmode = f(insn, 11, 8), op = b1(insn, 5);
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn);
  if (q && (d & 1u)) return INTERP_UNDEFINED;
  const uint64_t imm8 = (b1(insn, 24) << 7) | (f(insn, 18, 16) << 4) | f(insn, 3, 0);
  uint64_t imm32 = 0, imm64;
  bool replicate32 = true;
  switch (cmode >> 1) {
    case 0: imm32 = imm8; break;
    case 1: imm32 = imm8 << 8; break;
    case 2: imm32 = imm8 << 16; break;
    case 3: imm32 = imm8 << 24; break;
    case 4: imm32 = imm8 | (imm8 << 16); break;
    case 5: imm32 = (imm8 << 8) | (imm8 << 24); break;
    case 6: imm32 = (cmode & 1u) ? (imm8 << 16) | 0xFFFFu : (imm8 << 8) | 0xFFu; break;
    default:
      replicate32 = false;
      if (!(cmode & 1u)) {
        imm64 = 0;
        for (unsigned i = 0; i < BYTE_BITS; i++) {
          const uint64_t byte = op ? (((imm8 >> i) & 1u) ? 0xFFu : 0) : imm8;
          imm64 |= byte << (i * BYTE_BITS);
        }
      } else {
        if (op) return INTERP_UNDEFINED;
        imm32 = fp_expand_imm8(FP_SINGLE, (uint32_t)imm8);
        replicate32 = true;
      }
      break;
  }
  if (replicate32) imm64 = imm32 | (imm32 << WORD_BITS);
  /* op/cmode pick VMOV, VMVN, VORR, VBIC. */
  const bool is_bitwise = cmode < 0xCu && (cmode & 1u);
  const bool invert = op && !(cmode == 0xEu) && cmode != 0xFu;
  Vec r = get_v(a, d, q);
  for (unsigned i = 0; i < (q ? 2u : 1u); i++) {
    if (!is_bitwise) r.d[i] = invert ? ~imm64 : imm64;
    else if (op) r.d[i] &= ~imm64; /* VBIC */
    else r.d[i] |= imm64;           /* VORR */
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- two registers and a shift amount ---------------------------------- */

static Interp_Status shift_immediate(A32_State *a, uint32_t insn) {
  const uint32_t U = b1(insn, 24), op = f(insn, 11, 8), L = b1(insn, 7), imm6 = f(insn, 21, 16);
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn), m = reg_m(insn);
  unsigned esize;
  if (L) esize = D_BITS;
  else if (imm6 & 0x20u) esize = 32u;
  else if (imm6 & 0x10u) esize = 16u;
  else esize = 8u;
  const unsigned right = (L ? D_BITS : 2u * esize) - imm6, left = L ? imm6 : imm6 - esize;
  const bool is_signed = !U;
  if (op >= 0xBu && op <= 0xDu) return INTERP_UNDEFINED;
  if (op >= 8u && op <= 0xAu) { /* narrowing and widening: one D side, one Q side */
    if (L) return INTERP_UNDEFINED;
    if (op == 0xAu) { /* VSHLL / VMOVL: Dm -> Qd */
      if (q || (d & 1u)) return INTERP_UNDEFINED;
      const Vec vm = get_v(a, m, false);
      Vec r = {{0, 0}};
      for (unsigned e = 0; e < D_BITS / esize; e++) {
        const uint64_t x = is_signed ? (uint64_t)se(&vm, e, esize) : ue(&vm, e, esize);
        put(&r, e, 2u * esize, x << left);
      }
      set_v(a, d, true, r);
      return INTERP_CONTINUE;
    }
    if (m & 1u) return INTERP_UNDEFINED;
    const Vec vm = get_v(a, m, true);
    const bool round = q; /* bit 6 picks the rounding form */
    Vec r = {{0, 0}};
    for (unsigned e = 0; e < D_BITS / esize; e++) {
      const uint64_t x = ue(&vm, e, 2u * esize);
      uint64_t v;
      if (op == 8u && !U) { /* VSHRN / VRSHRN */
        v = round ? rshr(x, false, 2u * esize, right) : shr(x, false, 2u * esize, right);
      } else {
        const bool src_signed = op == 8u ? true : is_signed; /* VQSHRUN: signed in */
        const uint64_t shifted = round ? rshr(x, src_signed, 2u * esize, right) : shr(x, src_signed, 2u * esize, right);
        if (src_signed) {
          const int64_t sv = sign_extend(shifted, 2u * esize);
          v = (op == 8u) ? sat_u_from_s(&a->s, sv, esize) : sat_s(&a->s, sv, esize);
        } else {
          v = sat_u(&a->s, shifted, esize);
        }
      }
      put(&r, e, esize, v);
    }
    set_v(a, d, false, r);
    return INTERP_CONTINUE;
  }
  if (q && ((d | m) & 1u)) return INTERP_UNDEFINED;
  const Vec vm = get_v(a, m, q), vd = get_v(a, d, q);
  if (op >= 0xEu) { /* VCVT between F32 and fixed point */
    if (L || !(imm6 & 0x20u)) return INTERP_UNDEFINED;
    const unsigned fbits = D_BITS - imm6;
    FP_Env env = neon_env(a);
    Vec r = {{0, 0}};
    for (unsigned e = 0; e < (q ? 4u : 2u); e++) {
      const uint64_t x = ue(&vm, e, WORD_BITS);
      const uint64_t v = (op & 1u) ? fp_to_int(FP_SINGLE, x, fbits, U, WORD_BITS, FP_ROUND_ZERO, &env)
                                   : fp_from_int(FP_SINGLE, x, fbits, !U, WORD_BITS, &env);
      put(&r, e, WORD_BITS, v);
    }
    set_v(a, d, q, r);
    return INTERP_CONTINUE;
  }
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < (q ? Q_BITS : D_BITS) / esize; e++) {
    const uint64_t x = ue(&vm, e, esize), acc = ue(&vd, e, esize);
    uint64_t v;
    switch (op) {
      case 0: v = shr(x, is_signed, esize, right); break;
      case 1: v = acc + shr(x, is_signed, esize, right); break;
      case 2: v = rshr(x, is_signed, esize, right); break;
      case 3: v = acc + rshr(x, is_signed, esize, right); break;
      case 4: {
        if (!U) return INTERP_UNDEFINED;
        const uint64_t mask = shr(ones(esize), false, esize, right); /* VSRI */
        v = (acc & ~mask) | shr(x, false, esize, right);
        break;
      }
      case 5:
        if (!U) {
          v = x << left;
        } else {
          const uint64_t mask = (ones(esize) << left) & ones(esize); /* VSLI */
          v = (acc & ~mask) | ((x << left) & mask);
        }
        break;
      case 6:
        if (!U) return INTERP_UNDEFINED;
        v = qshl(a, x, true, false, esize, left); /* VQSHLU */
        break;
      default: /* 7: VQSHL */
        v = qshl(a, x, is_signed, is_signed, esize, left);
        break;
    }
    put(&r, e, esize, v & ones(esize));
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- three registers of different lengths ------------------------------ */

static Interp_Status three_different(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 11, 8), U = b1(insn, 24), size = f(insn, 21, 20);
  const uint32_t d = reg_d(insn), n = reg_n(insn), m = reg_m(insn);
  const unsigned esize = BYTE_BITS << size, wide = 2u * esize, elements = D_BITS / esize;
  const bool is_signed = !U;
  const bool narrow = op == 4u || op == 6u;          /* VADDHN / VSUBHN: Qn, Qm -> Dd */
  const bool wide_n = op == 1u || op == 3u;          /* VADDW / VSUBW: Qn */
  if (narrow ? ((n | m) & 1u) : (d & 1u) || (wide_n && (n & 1u))) return INTERP_UNDEFINED;
  if ((op == 9u || op == 0xBu || op == 0xDu) && (U || size == 0u)) return INTERP_UNDEFINED;
  if (op == 0xEu && (U || size != 0u)) return INTERP_UNDEFINED; /* VMULL.P8 (P64 is crypto) */
  if (op == 0xFu) return INTERP_UNDEFINED;
  const Vec vn = get_v(a, n, narrow || wide_n), vm = get_v(a, m, narrow), vd = get_v(a, d, !narrow);
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < elements; e++) {
    if (narrow) {
      const uint64_t x = ue(&vn, e, wide), y = ue(&vm, e, wide);
      uint64_t v = op == 4u ? x + y : x - y;
      if (U) v += (uint64_t)1 << (esize - 1u); /* rounding */
      put(&r, e, esize, (v & ones(wide)) >> esize);
      continue;
    }
    const uint64_t xw = wide_n ? ue(&vn, e, wide) : (is_signed ? (uint64_t)se(&vn, e, esize) : ue(&vn, e, esize));
    const uint64_t y = is_signed ? (uint64_t)se(&vm, e, esize) : ue(&vm, e, esize);
    const int64_t sx = (int64_t)xw, sy = (int64_t)y;
    const uint64_t acc = ue(&vd, e, wide);
    uint64_t v;
    switch (op) {
      case 0: case 1: v = xw + y; break;
      case 2: case 3: v = xw - y; break;
      case 5: case 7: {
        const uint64_t diff = is_signed ? (uint64_t)(sx > sy ? sx - sy : sy - sx) : (xw > y ? xw - y : y - xw);
        v = op == 5u ? acc + diff : diff;
        break;
      }
      case 8: v = acc + xw * y; break;
      case 0xA: v = acc - xw * y; break;
      case 0xC: v = xw * y; break;
      case 0xE: v = poly_mul8(xw & 0xFFu, y & 0xFFu); break;
      default: { /* 9, 0xB, 0xD: VQDMLAL, VQDMLSL, VQDMULL */
        const int64_t min = -(int64_t)(ones(esize) >> 1) - 1;
        uint64_t product;
        if (sx == min && sy == min) {
          set_qc(&a->s);
          product = ones(wide) >> 1;
        } else {
          product = (uint64_t)(2 * sx * sy) & ones(wide);
        }
        if (op == 0xDu) {
          v = product;
        } else if (wide == D_BITS) {
          v = op == 9u ? sqadd64(&a->s, acc, product) : sqsub64(&a->s, acc, product);
        } else {
          const int64_t sa = sign_extend(acc, wide), sp = sign_extend(product, wide);
          v = sat_s(&a->s, op == 9u ? sa + sp : sa - sp, wide);
        }
        break;
      }
    }
    put(&r, e, wide, v & ones(wide));
  }
  set_v(a, d, !narrow, r);
  return INTERP_CONTINUE;
}

/* ---- two registers and a scalar ----------------------------------------- */

static Interp_Status two_scalar(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 11, 8), size = f(insn, 21, 20), top = b1(insn, 24);
  const uint32_t d = reg_d(insn), n = reg_n(insn);
  if (size == 0u) return INTERP_UNDEFINED;
  const unsigned esize = BYTE_BITS << size;
  uint32_t m, index;
  if (size == 1u) {
    m = f(insn, 2, 0);
    index = (b1(insn, 5) << 1) | b1(insn, 3);
  } else {
    m = f(insn, 3, 0);
    index = b1(insn, 5);
  }
  const Vec vm = get_v(a, m, false);
  const uint64_t scalar = ue(&vm, index, esize);
  const bool is_float = op == 1u || op == 5u || op == 9u;
  const bool is_long = op == 2u || op == 3u || op == 6u || op == 7u || op == 0xAu || op == 0xBu;
  if (is_float && size != 2u) return INTERP_UNDEFINED;
  if (op == 0xEu || op == 0xFu) return INTERP_UNDEFINED;
  if (is_long) {
    if (d & 1u) return INTERP_UNDEFINED;
    const bool is_signed = !top || op == 3u || op == 7u || op == 0xBu;
    if ((op == 3u || op == 7u || op == 0xBu) && top) return INTERP_UNDEFINED;
    const unsigned wide = 2u * esize;
    const Vec vn = get_v(a, n, false), vd = get_v(a, d, true);
    Vec r = {{0, 0}};
    for (unsigned e = 0; e < D_BITS / esize; e++) {
      const uint64_t x = is_signed ? (uint64_t)se(&vn, e, esize) : ue(&vn, e, esize);
      const uint64_t y = is_signed ? (uint64_t)sign_extend(scalar, esize) : scalar;
      const uint64_t acc = ue(&vd, e, wide);
      uint64_t v;
      if (op == 2u) v = acc + x * y;
      else if (op == 6u) v = acc - x * y;
      else if (op == 0xAu) v = x * y;
      else {
        const int64_t min = -(int64_t)(ones(esize) >> 1) - 1, sx = (int64_t)x, sy = (int64_t)y;
        uint64_t product;
        if (sx == min && sy == min) {
          set_qc(&a->s);
          product = ones(wide) >> 1;
        } else {
          product = (uint64_t)(2 * sx * sy) & ones(wide);
        }
        if (op == 0xBu) {
          v = product;
        } else if (wide == D_BITS) {
          v = op == 3u ? sqadd64(&a->s, acc, product) : sqsub64(&a->s, acc, product);
        } else {
          const int64_t sa = sign_extend(acc, wide), sp = sign_extend(product, wide);
          v = sat_s(&a->s, op == 3u ? sa + sp : sa - sp, wide);
        }
      }
      put(&r, e, wide, v & ones(wide));
    }
    set_v(a, d, true, r);
    return INTERP_CONTINUE;
  }
  const bool q = top;
  if (q && ((d | n) & 1u)) return INTERP_UNDEFINED;
  const Vec vn = get_v(a, n, q), vd = get_v(a, d, q);
  FP_Env env = neon_env(a);
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < (q ? Q_BITS : D_BITS) / esize; e++) {
    const uint64_t x = ue(&vn, e, esize), acc = ue(&vd, e, esize);
    uint64_t v;
    switch (op) {
      case 0: v = acc + x * scalar; break;
      case 4: v = acc - x * scalar; break;
      case 8: v = x * scalar; break;
      case 1:
      case 5: {
        const uint64_t product = fp_mul(FP_SINGLE, x, scalar, &env);
        v = fp_add(FP_SINGLE, acc, op == 5u ? fp_neg(FP_SINGLE, product) : product, &env);
        break;
      }
      case 9: v = fp_mul(FP_SINGLE, x, scalar, &env); break;
      default: { /* 0xC VQDMULH, 0xD VQRDMULH */
        const int64_t min = -(int64_t)(ones(esize) >> 1) - 1;
        const int64_t sx = sign_extend(x, esize), sy = sign_extend(scalar, esize);
        if (sx == min && sy == min) {
          set_qc(&a->s);
          v = ones(esize) >> 1;
        } else {
          int64_t product = 2 * sx * sy;
          if (op == 0xDu) product += (int64_t)1 << (esize - 1u);
          v = sat_s(&a->s, product >> esize, esize);
        }
        break;
      }
    }
    put(&r, e, esize, v & ones(esize));
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- AES and SHA --------------------------------------------------------- */

static uint8_t g_sbox[256], g_inv_sbox[256];
static bool g_sbox_ready;

static uint8_t gf_mul(uint8_t x, uint8_t y) {
  uint8_t r = 0;
  while (y) {
    if (y & 1u) r ^= x;
    x = (uint8_t)((x << 1) ^ ((x & 0x80u) ? AES_POLY : 0u));
    y >>= 1;
  }
  return r;
}

static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)((x << n) | (x >> (BYTE_BITS - n))); }

/* SubBytes from its definition: the GF(2^8) inverse, then the affine map. */
static void build_sbox(void) {
  for (unsigned x = 0; x < 256u; x++) {
    uint8_t inv = 0;
    if (x) {
      uint8_t p = (uint8_t)x; /* x^254 = x^-1 */
      inv = 1;
      for (unsigned e = 254u; e; e >>= 1) {
        if (e & 1u) inv = gf_mul(inv, p);
        p = gf_mul(p, p);
      }
    }
    const uint8_t s = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ AES_AFFINE);
    g_sbox[x] = s;
    g_inv_sbox[s] = (uint8_t)x;
  }
  g_sbox_ready = true;
}

static void vec_bytes(const Vec *v, uint8_t out[AES_BYTES]) { memcpy(out, v->d, AES_BYTES); }
static Vec bytes_vec(const uint8_t in[AES_BYTES]) {
  Vec v;
  memcpy(v.d, in, AES_BYTES);
  return v;
}

static void mix_column(uint8_t *c, bool inverse) {
  const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
  if (!inverse) {
    c[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
    c[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
    c[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
    c[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
  } else {
    c[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
    c[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
    c[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
    c[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
  }
}

/* AESE, AESD, AESMC, AESIMC (op = bits 7:6): the state is column-major,
 * byte 4c + r is row r of column c. */
static Interp_Status aes(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 7, 6), d = reg_d(insn), m = reg_m(insn);
  if (f(insn, 19, 18) != 0u || ((d | m) & 1u)) return INTERP_UNDEFINED;
  if (!g_sbox_ready) build_sbox();
  uint8_t s[AES_BYTES], t[AES_BYTES];
  const Vec vd = get_v(a, d, true), vm = get_v(a, m, true);
  if (op < 2u) {
    uint8_t x[AES_BYTES], y[AES_BYTES];
    vec_bytes(&vd, x);
    vec_bytes(&vm, y);
    for (unsigned i = 0; i < AES_BYTES; i++) s[i] = (uint8_t)(x[i] ^ y[i]);
    for (unsigned c = 0; c < 4u; c++)
      for (unsigned r = 0; r < 4u; r++) {
        if (op == 0u) t[4u * c + r] = g_sbox[s[4u * ((c + r) & 3u) + r]];         /* ShiftRows, SubBytes */
        else t[4u * ((c + r) & 3u) + r] = g_inv_sbox[s[4u * c + r]];              /* InvShiftRows, InvSubBytes */
      }
  } else {
    vec_bytes(&vm, t);
    for (unsigned c = 0; c < 4u; c++) mix_column(&t[4u * c], op == 3u);
  }
  set_v(a, d, true, bytes_vec(t));
  return INTERP_CONTINUE;
}

static uint32_t rol32(uint32_t x, unsigned n) { return n ? (x << n) | (x >> (WORD_BITS - n)) : x; }
static uint32_t ror32(uint32_t x, unsigned n) { return n ? (x >> n) | (x << (WORD_BITS - n)) : x; }
static uint32_t w32(const Vec *v, unsigned i) { return (uint32_t)ue(v, i, WORD_BITS); }

/* SHA1C/P/M/SU0 and SHA256H/H2/SU1: the 3-register SHA forms (Q only). */
static Interp_Status sha3(A32_State *a, uint32_t insn) {
  const uint32_t U = b1(insn, 24), size = f(insn, 21, 20);
  const uint32_t d = reg_d(insn), n = reg_n(insn), m = reg_m(insn);
  if (!b1(insn, 6) || ((d | n | m) & 1u) || (U && size == 3u)) return INTERP_UNDEFINED;
  const Vec vd = get_v(a, d, true), vn = get_v(a, n, true), vm = get_v(a, m, true);
  Vec r = {{0, 0}};
  if (!U && size == 3u) { /* SHA1SU0: Vn<63:0>:Vd<127:64> ^ Vd ^ Vm */
    r.d[0] = vd.d[1] ^ vd.d[0] ^ vm.d[0];
    r.d[1] = vn.d[0] ^ vd.d[1] ^ vm.d[1];
  } else if (!U) { /* SHA1C / SHA1P / SHA1M */
    uint32_t x[4] = {w32(&vd, 0), w32(&vd, 1), w32(&vd, 2), w32(&vd, 3)}, y = w32(&vn, 0);
    for (unsigned e = 0; e < 4u; e++) {
      uint32_t t;
      if (size == 0u) t = (x[1] & x[2]) | (~x[1] & x[3]);
      else if (size == 1u) t = x[1] ^ x[2] ^ x[3];
      else t = (x[1] & x[2]) | (x[1] & x[3]) | (x[2] & x[3]);
      y = y + rol32(x[0], 5) + t + w32(&vm, e);
      x[1] = rol32(x[1], 30);
      const uint32_t out = x[3]; /* (Y:X) rotated left by 32 */
      x[3] = x[2];
      x[2] = x[1];
      x[1] = x[0];
      x[0] = y;
      y = out;
    }
    for (unsigned e = 0; e < 4u; e++) put(&r, e, WORD_BITS, x[e]);
  } else if (size < 2u) { /* SHA256H (X = Vd, Y = Vn) / SHA256H2 (X = Vn, Y = Vd) */
    const bool part1 = size == 0u;
    const Vec *vx = part1 ? &vd : &vn, *vy = part1 ? &vn : &vd;
    uint32_t x[4] = {w32(vx, 0), w32(vx, 1), w32(vx, 2), w32(vx, 3)};
    uint32_t y[4] = {w32(vy, 0), w32(vy, 1), w32(vy, 2), w32(vy, 3)};
    for (unsigned e = 0; e < 4u; e++) {
      const uint32_t ch = (y[0] & y[1]) ^ (~y[0] & y[2]);
      const uint32_t maj = (x[0] & x[1]) ^ (x[0] & x[2]) ^ (x[1] & x[2]);
      const uint32_t sigma1 = ror32(y[0], 6) ^ ror32(y[0], 11) ^ ror32(y[0], 25);
      const uint32_t sigma0 = ror32(x[0], 2) ^ ror32(x[0], 13) ^ ror32(x[0], 22);
      const uint32_t t = y[3] + sigma1 + ch + w32(&vm, e);
      x[3] += t;
      y[3] = t + sigma0 + maj;
      /* (Y:X) rotated left by 32 bits */
      const uint32_t x3 = x[3], y3 = y[3];
      x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = y3;
      y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = x3;
    }
    const uint32_t *out = part1 ? x : y;
    for (unsigned e = 0; e < 4u; e++) put(&r, e, WORD_BITS, out[e]);
  } else { /* SHA256SU1 */
    const uint32_t t0[4] = {w32(&vn, 1), w32(&vn, 2), w32(&vn, 3), w32(&vm, 0)};
    uint32_t out[4];
    for (unsigned e = 0; e < 4u; e++) {
      const uint32_t src = e < 2u ? w32(&vm, e + 2u) : out[e - 2u];
      const uint32_t s1 = ror32(src, 17) ^ ror32(src, 19) ^ (src >> 10);
      out[e] = s1 + w32(&vd, e) + t0[e];
    }
    for (unsigned e = 0; e < 4u; e++) put(&r, e, WORD_BITS, out[e]);
  }
  set_v(a, d, true, r);
  return INTERP_CONTINUE;
}

/* ---- two registers, miscellaneous ----------------------------------------- */

static uint64_t count_leading_zeros(uint64_t x, unsigned esize) {
  unsigned n = 0;
  for (int b = (int)esize - 1; b >= 0 && !((x >> b) & 1u); b--) n++;
  return n;
}

static Interp_Status two_misc(A32_State *a, uint32_t insn) {
  const uint32_t opc1 = f(insn, 17, 16), opc2 = f(insn, 10, 7), size = f(insn, 19, 18);
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn), m = reg_m(insn);
  const unsigned esize = BYTE_BITS << size;
  if (opc1 == 0u && (opc2 == 6u || opc2 == 7u)) return aes(a, insn);
  FP_Env env = neon_env(a);
  /* Forms that change element width or swap registers first. */
  if (opc1 == 2u) {
    if (opc2 <= 3u) { /* VSWP, VTRN, VUZP, VZIP */
      if (q && ((d | m) & 1u)) return INTERP_UNDEFINED;
      if (opc2 == 0u && size != 0u) return INTERP_UNDEFINED;
      if (size == 3u || (opc2 >= 2u && !q && size == 2u)) return INTERP_UNDEFINED;
      const Vec x = get_v(a, d, q), y = get_v(a, m, q);
      Vec rd = x, rm = y;
      const unsigned elements = (q ? Q_BITS : D_BITS) / esize;
      if (opc2 == 0u) {
        rd = y;
        rm = x;
      } else if (opc2 == 1u) {
        for (unsigned e = 0; e + 1u < elements; e += 2u) {
          put(&rd, e + 1u, esize, ue(&y, e, esize));
          put(&rm, e, esize, ue(&x, e + 1u, esize));
        }
      } else {
        for (unsigned e = 0; e < elements; e++) {
          if (opc2 == 2u) { /* VUZP: evens to Dd, odds to Dm, of x then y */
            const unsigned even = 2u * e, odd = 2u * e + 1u;
            put(&rd, e, esize, even < elements ? ue(&x, even, esize) : ue(&y, even - elements, esize));
            put(&rm, e, esize, odd < elements ? ue(&x, odd, esize) : ue(&y, odd - elements, esize));
          } else { /* VZIP: x0 y0 x1 y1 ... */
            put(&rd, e, esize, (e & 1u) ? ue(&y, e / 2u, esize) : ue(&x, e / 2u, esize));
            const unsigned j = elements + e;
            put(&rm, e, esize, (j & 1u) ? ue(&y, j / 2u, esize) : ue(&x, j / 2u, esize));
          }
        }
      }
      set_v(a, d, q, rd);
      set_v(a, m, q, rm);
      return INTERP_CONTINUE;
    }
    if (opc2 == 4u || opc2 == 5u) { /* VMOVN, VQMOVUN, VQMOVN: Qm -> Dd */
      if (size == 3u || (m & 1u)) return INTERP_UNDEFINED;
      const uint32_t op = f(insn, 7, 6);
      const Vec vm = get_v(a, m, true);
      Vec r = {{0, 0}};
      for (unsigned e = 0; e < D_BITS / esize; e++) {
        const uint64_t x = ue(&vm, e, 2u * esize);
        uint64_t v;
        if (op == 0u) v = x & ones(esize);
        else if (op == 1u) v = sat_u_from_s(&a->s, sign_extend(x, 2u * esize), esize);
        else if (op == 2u) v = sat_s(&a->s, sign_extend(x, 2u * esize), esize);
        else v = sat_u(&a->s, x, esize);
        put(&r, e, esize, v);
      }
      set_v(a, d, false, r);
      return INTERP_CONTINUE;
    }
    if (opc2 == 6u && !q) { /* VSHLL (shift = esize): Dm -> Qd */
      if (size == 3u || (d & 1u)) return INTERP_UNDEFINED;
      const Vec vm = get_v(a, m, false);
      Vec r = {{0, 0}};
      for (unsigned e = 0; e < D_BITS / esize; e++) put(&r, e, 2u * esize, ue(&vm, e, esize) << esize);
      set_v(a, d, true, r);
      return INTERP_CONTINUE;
    }
    if (opc2 == 7u) { /* SHA1SU1 (bit 6 = 0), SHA256SU0 (bit 6 = 1) */
      if (size != 2u || ((d | m) & 1u)) return INTERP_UNDEFINED;
      const Vec vd = get_v(a, d, true), vm = get_v(a, m, true);
      Vec r = {{0, 0}};
      if (!q) {
        const uint32_t t[4] = {w32(&vd, 0) ^ w32(&vm, 1), w32(&vd, 1) ^ w32(&vm, 2), w32(&vd, 2) ^ w32(&vm, 3),
                               w32(&vd, 3)};
        put(&r, 0, WORD_BITS, rol32(t[0], 1));
        put(&r, 1, WORD_BITS, rol32(t[1], 1));
        put(&r, 2, WORD_BITS, rol32(t[2], 1));
        put(&r, 3, WORD_BITS, rol32(t[3], 1) ^ rol32(t[0], 2));
      } else {
        const uint32_t t[4] = {w32(&vd, 1), w32(&vd, 2), w32(&vd, 3), w32(&vm, 0)};
        for (unsigned e = 0; e < 4u; e++) {
          const uint32_t s0 = ror32(t[e], 7) ^ ror32(t[e], 18) ^ (t[e] >> 3);
          put(&r, e, WORD_BITS, s0 + w32(&vd, e));
        }
      }
      set_v(a, d, true, r);
      return INTERP_CONTINUE;
    }
    if (opc2 == 0xCu || opc2 == 0xEu) { /* VCVT.F16.F32 (Qm -> Dd), VCVT.F32.F16 (Dm -> Qd) */
      if (q || size != 1u) return INTERP_UNDEFINED;
      const bool to_half = opc2 == 0xCu;
      if (to_half ? (m & 1u) : (d & 1u)) return INTERP_UNDEFINED;
      const Vec vm = get_v(a, m, to_half);
      Vec r = {{0, 0}};
      for (unsigned e = 0; e < 4u; e++) {
        if (to_half) put(&r, e, 16u, fp_convert(FP_HALF, FP_SINGLE, ue(&vm, e, WORD_BITS), &env));
        else put(&r, e, WORD_BITS, fp_convert(FP_SINGLE, FP_HALF, ue(&vm, e, 16u), &env));
      }
      set_v(a, d, !to_half, r);
      return INTERP_CONTINUE;
    }
    return INTERP_UNDEFINED;
  }
  if (q && ((d | m) & 1u)) return INTERP_UNDEFINED;
  const Vec vm = get_v(a, m, q), vd = get_v(a, d, q);
  const unsigned elements = (q ? Q_BITS : D_BITS) / esize;
  Vec r = {{0, 0}};
  if (opc1 == 0u) {
    if (opc2 <= 2u) { /* VREV64 / VREV32 / VREV16 */
      const unsigned group = D_BITS >> opc2;
      if (esize >= group) return INTERP_UNDEFINED;
      const unsigned per = group / esize;
      for (unsigned e = 0; e < elements; e++) {
        const unsigned base = e - e % per;
        put(&r, base + (per - 1u - e % per), esize, ue(&vm, e, esize));
      }
      set_v(a, d, q, r);
      return INTERP_CONTINUE;
    }
    if (opc2 == 4u || opc2 == 5u || opc2 == 0xCu || opc2 == 0xDu) { /* VPADDL / VPADAL */
      if (size == 3u) return INTERP_UNDEFINED;
      const bool is_unsigned = opc2 & 1u, accumulate = opc2 >= 0xCu;
      for (unsigned e = 0; e < elements / 2u; e++) {
        const uint64_t x = is_unsigned ? ue(&vm, 2u * e, esize) : (uint64_t)se(&vm, 2u * e, esize);
        const uint64_t y = is_unsigned ? ue(&vm, 2u * e + 1u, esize) : (uint64_t)se(&vm, 2u * e + 1u, esize);
        uint64_t v = x + y;
        if (accumulate) v += ue(&vd, e, 2u * esize);
        put(&r, e, 2u * esize, v);
      }
      set_v(a, d, q, r);
      return INTERP_CONTINUE;
    }
    if (size == 3u) return INTERP_UNDEFINED;
    for (unsigned e = 0; e < elements; e++) {
      const uint64_t x = ue(&vm, e, esize);
      const int64_t sx = sign_extend(x, esize);
      uint64_t v;
      switch (opc2) {
        case 8: v = count_leading_zeros((uint64_t)(sx < 0 ? ~sx : sx) & ones(esize), esize) - 1u; break; /* VCLS */
        case 9: v = count_leading_zeros(x, esize); break;
        case 0xA: {
          if (size != 0u) return INTERP_UNDEFINED;
          uint64_t c = 0;
          for (uint64_t t = x; t; t &= t - 1u) c++;
          v = c;
          break;
        }
        case 0xB:
          if (size != 0u) return INTERP_UNDEFINED;
          v = ~x;
          break;
        case 0xE: v = sat_s(&a->s, sx < 0 ? -sx : sx, esize); break; /* VQABS */
        case 0xF: v = sat_s(&a->s, -sx, esize); break;               /* VQNEG */
        default: return INTERP_UNDEFINED;
      }
      put(&r, e, esize, v & ones(esize));
    }
    set_v(a, d, q, r);
    return INTERP_CONTINUE;
  }
  if (opc1 == 1u) {
    const bool is_float = opc2 & 8u;
    const uint32_t kind = opc2 & 7u;
    if (opc2 == 5u) { /* SHA1H */
      if (!q || size != 2u || ((d | m) & 1u)) return INTERP_UNDEFINED;
      put(&r, 0, WORD_BITS, rol32(w32(&vm, 0), 30));
      set_v(a, d, true, r);
      return INTERP_CONTINUE;
    }
    if (size == 3u || kind == 5u || (is_float && size != 2u)) return INTERP_UNDEFINED;
    for (unsigned e = 0; e < elements; e++) {
      const uint64_t x = ue(&vm, e, esize);
      const int64_t sx = sign_extend(x, esize);
      bool flag = false;
      uint64_t v = 0;
      if (is_float) {
        switch (kind) {
          case 0: flag = fp_compare_gt(FP_SINGLE, x, 0, &env); break;
          case 1: flag = fp_compare_ge(FP_SINGLE, x, 0, &env); break;
          case 2: flag = fp_compare_eq(FP_SINGLE, x, 0, &env); break;
          case 3: flag = fp_compare_ge(FP_SINGLE, 0, x, &env); break;
          case 4: flag = fp_compare_gt(FP_SINGLE, 0, x, &env); break;
          case 6: v = fp_abs(FP_SINGLE, x); break;
          default: v = fp_neg(FP_SINGLE, x); break;
        }
      } else {
        switch (kind) {
          case 0: flag = sx > 0; break;
          case 1: flag = sx >= 0; break;
          case 2: flag = sx == 0; break;
          case 3: flag = sx <= 0; break;
          case 4: flag = sx < 0; break;
          case 6: v = (uint64_t)(sx < 0 ? -sx : sx); break;
          default: v = (uint64_t)-sx; break;
        }
      }
      if (kind <= 4u) v = flag ? ones(esize) : 0;
      put(&r, e, esize, v & ones(esize));
    }
    set_v(a, d, q, r);
    return INTERP_CONTINUE;
  }
  /* opc1 == 3: VRECPE, VRSQRTE, VCVT (F32 <-> 32-bit integer) */
  if (size != 2u || opc2 < 8u) return INTERP_UNDEFINED;
  for (unsigned e = 0; e < elements; e++) {
    const uint64_t x = ue(&vm, e, WORD_BITS);
    uint64_t v;
    if (opc2 >= 0xCu) {
      const uint32_t op = opc2 & 3u; /* 00 S32->F32, 01 U32->F32, 10 F32->S32, 11 F32->U32 */
      v = op >= 2u ? fp_to_int(FP_SINGLE, x, 0, op == 3u, WORD_BITS, FP_ROUND_ZERO, &env)
                   : fp_from_int(FP_SINGLE, x, 0, op == 0u, WORD_BITS, &env);
    } else {
      const bool is_float = opc2 & 2u, rsqrt = opc2 & 1u;
      if (is_float) v = rsqrt ? fp_rsqrt_estimate(FP_SINGLE, x, &env) : fp_recip_estimate(FP_SINGLE, x, &env);
      else v = rsqrt ? fp_unsigned_rsqrt_estimate((uint32_t)x) : fp_unsigned_recip_estimate((uint32_t)x);
    }
    put(&r, e, WORD_BITS, v);
  }
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- VEXT, VTBL/VTBX, VDUP (scalar) ------------------------------------------ */

static Interp_Status vext(A32_State *a, uint32_t insn) {
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn), n = reg_n(insn), m = reg_m(insn), imm4 = f(insn, 11, 8);
  if ((q && ((d | n | m) & 1u)) || (!q && imm4 >= 8u)) return INTERP_UNDEFINED;
  const unsigned bytes = q ? 16u : 8u;
  uint8_t cat[32], out[16] = {0};
  const Vec vn = get_v(a, n, q), vm = get_v(a, m, q);
  memcpy(cat, vn.d, bytes);
  memcpy(cat + bytes, vm.d, bytes);
  for (unsigned i = 0; i < bytes; i++) out[i] = cat[imm4 + i];
  Vec r = {{0, 0}};
  memcpy(r.d, out, bytes);
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

static Interp_Status vtbl(A32_State *a, uint32_t insn) {
  const uint32_t d = reg_d(insn), n = reg_n(insn), m = reg_m(insn), len = f(insn, 9, 8) + 1u;
  const bool extend = b1(insn, 6);
  if (n + len > D_REGS) return INTERP_UNDEFINED;
  uint8_t table[32];
  for (uint32_t i = 0; i < len; i++) {
    const uint64_t v = a32_d(a, n + i);
    memcpy(table + 8u * i, &v, 8u);
  }
  const uint64_t indices = a32_d(a, m), old = a32_d(a, d);
  uint64_t result = 0;
  for (unsigned i = 0; i < 8u; i++) {
    const uint32_t index = (uint32_t)(indices >> (i * BYTE_BITS)) & 0xFFu;
    const uint64_t byte = index < 8u * len ? table[index] : extend ? (old >> (i * BYTE_BITS)) & 0xFFu : 0;
    result |= byte << (i * BYTE_BITS);
  }
  a32_set_d(a, d, result);
  return INTERP_CONTINUE;
}

static Interp_Status vdup_scalar(A32_State *a, uint32_t insn) {
  const bool q = b1(insn, 6);
  const uint32_t d = reg_d(insn), m = reg_m(insn), imm4 = f(insn, 19, 16);
  if (q && (d & 1u)) return INTERP_UNDEFINED;
  unsigned esize, index;
  if (imm4 & 1u) { esize = 8u; index = imm4 >> 1; }
  else if (imm4 & 2u) { esize = 16u; index = imm4 >> 2; }
  else if (imm4 & 4u) { esize = 32u; index = imm4 >> 3; }
  else return INTERP_UNDEFINED;
  const Vec vm = get_v(a, m, false);
  const uint64_t x = ue(&vm, index, esize);
  Vec r = {{0, 0}};
  for (unsigned e = 0; e < (q ? Q_BITS : D_BITS) / esize; e++) put(&r, e, esize, x);
  set_v(a, d, q, r);
  return INTERP_CONTINUE;
}

/* ---- element and structure loads and stores ------------------------------ */

static Interp_Status element_load_store(A32_State *a, uint32_t insn) {
  const uint32_t load = b1(insn, 21), rn = f(insn, 19, 16), rm = f(insn, 3, 0), d0 = reg_d(insn);
  if (rn == A32_PC) return INTERP_UNDEFINED;
  const uint32_t base = a32_reg(a, rn);
  uint32_t address = base, transferred = 0;
  if (!b1(insn, 23)) { /* multiple structures */
    const uint32_t type = f(insn, 11, 8), size = f(insn, 7, 6);
    uint32_t structure, regs, inc;
    switch (type) {
      case 7: structure = 1; regs = 1; inc = 1; break;
      case 0xA: structure = 1; regs = 2; inc = 1; break;
      case 6: structure = 1; regs = 3; inc = 1; break;
      case 2: structure = 1; regs = 4; inc = 1; break;
      case 8: structure = 2; regs = 1; inc = 1; break;
      case 9: structure = 2; regs = 1; inc = 2; break;
      case 3: structure = 2; regs = 2; inc = 2; break;
      case 4: structure = 3; regs = 1; inc = 1; break;
      case 5: structure = 3; regs = 1; inc = 2; break;
      case 0: structure = 4; regs = 1; inc = 1; break;
      case 1: structure = 4; regs = 1; inc = 2; break;
      default: return INTERP_UNDEFINED;
    }
    if (structure > 1u && size == 3u) return INTERP_UNDEFINED;
    const unsigned ebytes = 1u << size, elements = 8u / ebytes;
    /* register for component k, block r: VLD1 is regs consecutive; VLDn
     * puts component k in d + k*inc (+ r for VLD2's two-pair form). */
    uint8_t buffer[32];
    const uint32_t total = 8u * regs * structure;
    if (total > sizeof(buffer)) return INTERP_UNDEFINED;
    if (load && !a32_read(a, address, buffer, total)) return INTERP_FAULT;
    uint64_t dregs[8] = {0};
    uint32_t dnum[8];
    uint32_t count = 0;
    for (uint32_t r = 0; r < regs; r++)
      for (uint32_t k = 0; k < structure; k++) {
        const uint32_t reg = structure == 1u ? d0 + r : d0 + k * inc + r;
        if (reg >= D_REGS) return INTERP_UNDEFINED;
        dnum[r * structure + k] = reg;
        dregs[r * structure + k] = a32_d(a, reg);
        count++;
      }
    uint32_t offset = 0;
    for (uint32_t r = 0; r < regs; r++)
      for (unsigned e = 0; e < elements; e++)
        for (uint32_t k = 0; k < structure; k++) {
          const uint32_t slot = r * structure + k;
          const unsigned shift = e * ebytes * BYTE_BITS;
          const uint64_t mask = ones(ebytes * BYTE_BITS) << shift;
          if (load) {
            uint64_t v = 0;
            memcpy(&v, buffer + offset, ebytes);
            dregs[slot] = (dregs[slot] & ~mask) | (v << shift);
          } else {
            const uint64_t v = (dregs[slot] & mask) >> shift;
            memcpy(buffer + offset, &v, ebytes);
          }
          offset += ebytes;
        }
    if (!load && !a32_write(a, address, buffer, total)) return INTERP_FAULT;
    if (load)
      for (uint32_t i = 0; i < count; i++) a32_set_d(a, dnum[i], dregs[i]);
    transferred = total;
  } else {
    const uint32_t size = f(insn, 11, 10), n = f(insn, 9, 8) + 1u;
    if (size == 3u) { /* VLDn to all lanes */
      if (!load) return INTERP_UNDEFINED;
      const uint32_t esz = f(insn, 7, 6), t = b1(insn, 5);
      if (esz == 3u && n != 4u) return INTERP_UNDEFINED;
      const unsigned ebytes = esz == 3u ? 4u : 1u << esz;
      const uint32_t regs = n == 1u ? (t ? 2u : 1u) : n, inc = n == 1u ? 1u : (t ? 2u : 1u);
      uint8_t buffer[16];
      if (!a32_read(a, address, buffer, ebytes * n)) return INTERP_FAULT;
      for (uint32_t k = 0; k < regs; k++) {
        uint64_t v = 0;
        memcpy(&v, buffer + (n == 1u ? 0u : k * ebytes), ebytes);
        uint64_t rep = 0;
        for (unsigned e = 0; e < 8u / ebytes; e++) rep |= v << (e * ebytes * BYTE_BITS);
        const uint32_t reg = d0 + k * inc;
        if (reg >= D_REGS) return INTERP_UNDEFINED;
        a32_set_d(a, reg, rep);
      }
      transferred = ebytes * n;
    } else { /* VLDn / VSTn one lane */
      const uint32_t ia = f(insn, 7, 4);
      const unsigned ebytes = 1u << size;
      const uint32_t index = ia >> (size + 1u);
      uint32_t inc = 1;
      if (n > 1u) inc = size == 1u ? ((ia >> 1) & 1u) + 1u : size == 2u ? ((ia >> 2) & 1u) + 1u : 1u;
      uint8_t buffer[16];
      if (load && !a32_read(a, address, buffer, ebytes * n)) return INTERP_FAULT;
      const unsigned shift = index * ebytes * BYTE_BITS;
      const uint64_t mask = ones(ebytes * BYTE_BITS) << shift;
      for (uint32_t k = 0; k < n; k++) {
        const uint32_t reg = d0 + k * inc;
        if (reg >= D_REGS) return INTERP_UNDEFINED;
        uint64_t dv = a32_d(a, reg);
        if (load) {
          uint64_t v = 0;
          memcpy(&v, buffer + k * ebytes, ebytes);
          a32_set_d(a, reg, (dv & ~mask) | (v << shift));
        } else {
          const uint64_t v = (dv & mask) >> shift;
          memcpy(buffer + k * ebytes, &v, ebytes);
        }
      }
      if (!load && !a32_write(a, address, buffer, ebytes * n)) return INTERP_FAULT;
      transferred = ebytes * n;
    }
  }
  if (rm != A32_PC) a32_set_reg(a, rn, base + (rm == A32_SP ? transferred : a32_reg(a, rm)));
  return INTERP_CONTINUE;
}

/* ---- the data-processing decode -------------------------------------------- */

Interp_Status a32_neon(A32_State *a, uint32_t insn) {
  if (f(insn, 27, 24) == 4u) return element_load_store(a, insn);
  if (!b1(insn, 23)) return three_same(a, insn);
  if (b1(insn, 4)) {
    if (f(insn, 21, 19) == 0u && !b1(insn, 7)) return modified_immediate(a, insn);
    return shift_immediate(a, insn);
  }
  if (f(insn, 21, 20) != 3u) return b1(insn, 6) ? two_scalar(a, insn) : three_different(a, insn);
  if (!b1(insn, 24)) return vext(a, insn);
  if (!b1(insn, 11)) return two_misc(a, insn);
  if (f(insn, 11, 10) == 2u) return vtbl(a, insn);
  if (f(insn, 11, 8) == 0xCu && !b1(insn, 7)) return vdup_scalar(a, insn);
  return INTERP_UNDEFINED;
}
