/**
 * ARM-semantics soft-float. See softfloat.h for why and what. Structure
 * mirrors the Arm ARM pseudocode: FPUnpack -> exact intermediate result
 * (128-bit integer significand, plus a sticky bit for anything below it)
 * -> FPRound. Reimplemented from the architecture's published pseudocode,
 * no third-party float library.
 */
#include "cpu/backends/interpreter/softfloat.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Formats.                                                            */
/* ------------------------------------------------------------------ */

typedef struct Fmt {
  unsigned exp_bits;
  unsigned frac_bits;
  int bias;
} Fmt;

static const Fmt k_formats[3] = {
    {5, 10, 15},    /* FP_HALF */
    {8, 23, 127},   /* FP_SINGLE */
    {11, 52, 1023}, /* FP_DOUBLE */
};

/* Count of leading zero bits; 64 for zero. Portable (MSVC builds this too). */
static int clz64(uint64_t x) {
  if (x == 0) return 64;
  int n = 0;
  if (!(x >> 32)) { n += 32; x <<= 32; }
  if (!(x >> 48)) { n += 16; x <<= 16; }
  if (!(x >> 56)) { n += 8; x <<= 8; }
  if (!(x >> 60)) { n += 4; x <<= 4; }
  if (!(x >> 62)) { n += 2; x <<= 2; }
  if (!(x >> 63)) { n += 1; }
  return n;
}

static uint64_t mask_bits(unsigned n) { return n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1u); }
static uint64_t exp_all_ones(const Fmt *f) { return mask_bits(f->exp_bits); }
static uint64_t pack(const Fmt *f, bool sign, uint64_t biased_exp, uint64_t frac) {
  return ((uint64_t)sign << (f->exp_bits + f->frac_bits)) | (biased_exp << f->frac_bits) | frac;
}
static uint64_t fp_zero(const Fmt *f, bool sign) { return pack(f, sign, 0, 0); }
static uint64_t fp_inf(const Fmt *f, bool sign) { return pack(f, sign, exp_all_ones(f), 0); }
static uint64_t fp_max_normal(const Fmt *f, bool sign) {
  return pack(f, sign, exp_all_ones(f) - 1u, mask_bits(f->frac_bits));
}
static uint64_t fp_default_nan(const Fmt *f) {
  return pack(f, false, exp_all_ones(f), (uint64_t)1 << (f->frac_bits - 1u));
}
static uint64_t fp_two(const Fmt *f, bool sign) { return pack(f, sign, (uint64_t)f->bias + 1u, 0); }
static uint64_t fp_one_point_five(const Fmt *f, bool sign) {
  return pack(f, sign, (uint64_t)f->bias, (uint64_t)1 << (f->frac_bits - 1u));
}

static void raise(FP_Env *env, uint32_t flags) { *env->fpsr |= flags; }

/* ------------------------------------------------------------------ */
/* 128-bit unsigned helpers.                                           */
/* ------------------------------------------------------------------ */

typedef struct U128 {
  uint64_t hi;
  uint64_t lo;
} U128;

static U128 u128(uint64_t lo) { return (U128){0, lo}; }
static bool u128_zero(U128 x) { return (x.hi | x.lo) == 0; }
static int u128_cmp(U128 a, U128 b) {
  if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
  if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
  return 0;
}
static U128 u128_add(U128 a, U128 b) {
  U128 r = {a.hi + b.hi, a.lo + b.lo};
  if (r.lo < a.lo) r.hi++;
  return r;
}
static U128 u128_sub(U128 a, U128 b) {
  U128 r = {a.hi - b.hi, a.lo - b.lo};
  if (a.lo < b.lo) r.hi--;
  return r;
}
static U128 u128_shl(U128 x, unsigned n) {
  if (n == 0) return x;
  if (n >= 128) return (U128){0, 0};
  if (n >= 64) return (U128){x.lo << (n - 64), 0};
  return (U128){(x.hi << n) | (x.lo >> (64 - n)), x.lo << n};
}
static U128 u128_shr(U128 x, unsigned n) {
  if (n == 0) return x;
  if (n >= 128) return (U128){0, 0};
  if (n >= 64) return (U128){0, x.hi >> (n - 64)};
  return (U128){x.hi >> n, (x.lo >> n) | (x.hi << (64 - n))};
}
/* Bits shifted out are ORed into *sticky. */
static U128 u128_shr_sticky(U128 x, unsigned n, bool *sticky) {
  if (n == 0) return x;
  if (n >= 128) {
    *sticky |= !u128_zero(x);
    return (U128){0, 0};
  }
  const U128 kept = u128_shr(x, n);
  if (u128_cmp(u128_shl(kept, n), x) != 0) *sticky = true;
  return kept;
}
static int u128_msb(U128 x) {
  if (x.hi) return 127 - clz64(x.hi);
  if (x.lo) return 63 - clz64(x.lo);
  return -1;
}
static U128 mul64(uint64_t a, uint64_t b) {
  const uint64_t a_lo = (uint32_t)a, a_hi = a >> 32, b_lo = (uint32_t)b, b_hi = b >> 32;
  const uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
  const uint64_t middle = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
  return (U128){p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32), (middle << 32) | (uint32_t)p0};
}
static bool u128_bit(U128 x, unsigned n) {
  return n >= 64 ? (x.hi >> (n - 64)) & 1u : (x.lo >> n) & 1u;
}

/* ------------------------------------------------------------------ */
/* Unpack.                                                             */
/* ------------------------------------------------------------------ */

typedef enum FP_Type { T_ZERO, T_NONZERO, T_INF, T_QNAN, T_SNAN } FP_Type;

typedef struct Unpacked {
  FP_Type type;
  bool sign;
  int exp;      /* value = sig * 2^exp for T_NONZERO */
  uint64_t sig; /* includes the implicit bit for normals */
} Unpacked;

static Unpacked unpack(const Fmt *f, FP_Format format, uint64_t bits, FP_Env *env) {
  Unpacked u;
  u.sign = (bits >> (f->exp_bits + f->frac_bits)) & 1u;
  const uint64_t exp_field = (bits >> f->frac_bits) & exp_all_ones(f);
  const uint64_t frac = bits & mask_bits(f->frac_bits);
  u.exp = 0;
  u.sig = 0;
  if (exp_field == 0) {
    if (frac == 0) {
      u.type = T_ZERO;
    } else if ((env->fpcr & FPCR_FZ) && format != FP_HALF) {
      u.type = T_ZERO;
      raise(env, FPSR_IDC);
    } else {
      u.type = T_NONZERO;
      u.sig = frac;
      u.exp = 1 - f->bias - (int)f->frac_bits;
    }
  } else if (exp_field == exp_all_ones(f)) {
    if (frac == 0) u.type = T_INF;
    else u.type = ((frac >> (f->frac_bits - 1u)) & 1u) ? T_QNAN : T_SNAN;
  } else {
    u.type = T_NONZERO;
    u.sig = frac | ((uint64_t)1 << f->frac_bits);
    u.exp = (int)exp_field - f->bias - (int)f->frac_bits;
  }
  return u;
}

static bool is_nan(FP_Type t) { return t == T_QNAN || t == T_SNAN; }

/* FPProcessNaN. */
static uint64_t process_nan(const Fmt *f, FP_Type type, uint64_t op, FP_Env *env) {
  uint64_t result = op;
  if (type == T_SNAN) {
    result |= (uint64_t)1 << (f->frac_bits - 1u);
    raise(env, FPSR_IOC);
  }
  if (env->fpcr & FPCR_DN) result = fp_default_nan(f);
  return result;
}

/* FPProcessNaNs: true if a NaN decided the result. */
static bool process_nans(const Fmt *f, const Unpacked *a, const Unpacked *b, uint64_t op1, uint64_t op2,
                         FP_Env *env, uint64_t *result) {
  if (a->type == T_SNAN) { *result = process_nan(f, a->type, op1, env); return true; }
  if (b->type == T_SNAN) { *result = process_nan(f, b->type, op2, env); return true; }
  if (a->type == T_QNAN) { *result = process_nan(f, a->type, op1, env); return true; }
  if (b->type == T_QNAN) { *result = process_nan(f, b->type, op2, env); return true; }
  return false;
}

static bool process_nans3(const Fmt *f, const Unpacked *a, const Unpacked *b, const Unpacked *c, uint64_t op1,
                          uint64_t op2, uint64_t op3, FP_Env *env, uint64_t *result) {
  if (a->type == T_SNAN) { *result = process_nan(f, a->type, op1, env); return true; }
  if (b->type == T_SNAN) { *result = process_nan(f, b->type, op2, env); return true; }
  if (c->type == T_SNAN) { *result = process_nan(f, c->type, op3, env); return true; }
  if (a->type == T_QNAN) { *result = process_nan(f, a->type, op1, env); return true; }
  if (b->type == T_QNAN) { *result = process_nan(f, b->type, op2, env); return true; }
  if (c->type == T_QNAN) { *result = process_nan(f, c->type, op3, env); return true; }
  return false;
}

/* ------------------------------------------------------------------ */
/* FPRound: sign * (sig + sticky_epsilon) * 2^exp -> format.            */
/* ------------------------------------------------------------------ */

static uint64_t round_value(const Fmt *f, FP_Format format, bool sign, int exp, U128 sig, bool sticky,
                            FP_Rounding rounding, FP_Env *env) {
  const int msb = u128_msb(sig);
  /* Callers never pass an exact zero (they return FPZero themselves). */
  const int exponent = exp + msb; /* value in [2^exponent, 2^(exponent+1)) */
  const int minimum_exp = 1 - f->bias;
  const int F = (int)f->frac_bits;

  if ((env->fpcr & FPCR_FZ) && format != FP_HALF && exponent < minimum_exp) {
    raise(env, FPSR_UFC);
    return fp_zero(f, sign);
  }

  const int effective_exp = exponent < minimum_exp ? minimum_exp : exponent;
  uint64_t biased_exp = exponent < minimum_exp ? 0 : (uint64_t)(exponent - minimum_exp + 1);
  const int shift = (effective_exp - F) - exp; /* int_mant = value / 2^(effective_exp - F) */

  U128 mant;
  bool round_bit = false; /* the bit just below int_mant: error >= 0.5 */
  bool rest = sticky;     /* anything below that */
  if (shift > 0) {
    round_bit = shift <= 128 && u128_bit(sig, (unsigned)shift - 1u);
    if (shift > 1) (void)u128_shr_sticky(sig, (unsigned)(shift - 1), &rest);
    mant = u128_shr(sig, (unsigned)shift);
  } else {
    mant = u128_shl(sig, (unsigned)(-shift));
  }
  uint64_t int_mant = mant.lo;
  const bool error_nonzero = round_bit || rest;

  if (biased_exp == 0 && error_nonzero) raise(env, FPSR_UFC);

  bool round_up, overflow_to_inf;
  switch (rounding) {
  case FP_ROUND_NEAREST_EVEN:
    round_up = round_bit && (rest || (int_mant & 1u));
    overflow_to_inf = true;
    break;
  case FP_ROUND_PLUS_INF:
    round_up = error_nonzero && !sign;
    overflow_to_inf = !sign;
    break;
  case FP_ROUND_MINUS_INF:
    round_up = error_nonzero && sign;
    overflow_to_inf = sign;
    break;
  case FP_ROUND_ZERO:
    round_up = false;
    overflow_to_inf = false;
    break;
  case FP_ROUND_ODD:
    round_up = false;
    overflow_to_inf = false;
    break;
  default: /* FP_ROUND_TIE_AWAY */
    round_up = round_bit;
    overflow_to_inf = true;
    break;
  }
  if (rounding == FP_ROUND_ODD && error_nonzero) int_mant |= 1u;
  if (round_up) {
    int_mant++;
    if (int_mant == (uint64_t)1 << F) biased_exp = 1;
    if (int_mant == (uint64_t)1 << (F + 1)) {
      biased_exp++;
      int_mant >>= 1;
    }
  }

  uint64_t result;
  bool inexact = error_nonzero;
  if (biased_exp >= exp_all_ones(f)) {
    result = overflow_to_inf ? fp_inf(f, sign) : fp_max_normal(f, sign);
    raise(env, FPSR_OFC);
    inexact = true;
  } else {
    result = pack(f, sign, biased_exp, int_mant & mask_bits(f->frac_bits));
  }
  if (inexact) raise(env, FPSR_IXC);
  return result;
}

static uint64_t round_fpcr(const Fmt *f, FP_Format format, bool sign, int exp, U128 sig, bool sticky,
                           FP_Env *env) {
  return round_value(f, format, sign, exp, sig, sticky, fp_env_rounding(env), env);
}

/* ------------------------------------------------------------------ */
/* Exact addition of two signed values with sticky.                     */
/* ------------------------------------------------------------------ */

#define NORMALIZE_MSB 125

typedef struct Exact {
  bool zero; /* exact zero (caller picks the sign) */
  bool sign;
  int exp;
  U128 sig;
  bool sticky;
} Exact;

static void normalize_to(U128 *sig, int *exp) {
  const int msb = u128_msb(*sig);
  if (msb < NORMALIZE_MSB) {
    *sig = u128_shl(*sig, (unsigned)(NORMALIZE_MSB - msb));
    *exp -= NORMALIZE_MSB - msb;
  }
}

static Exact exact_add(bool sign_x, int exp_x, U128 sig_x, bool sign_y, int exp_y, U128 sig_y) {
  Exact r = {false, false, 0, {0, 0}, false};
  normalize_to(&sig_x, &exp_x);
  normalize_to(&sig_y, &exp_y);
  /* Make x the operand with the larger exponent. */
  if (exp_y > exp_x || (exp_y == exp_x && u128_cmp(sig_y, sig_x) > 0)) {
    const bool ts = sign_x; sign_x = sign_y; sign_y = ts;
    const int te = exp_x; exp_x = exp_y; exp_y = te;
    const U128 tg = sig_x; sig_x = sig_y; sig_y = tg;
  }
  bool sticky = false;
  const unsigned distance = (unsigned)(exp_x - exp_y);
  sig_y = u128_shr_sticky(sig_y, distance > 200u ? 200u : distance, &sticky);
  r.exp = exp_x;
  r.sign = sign_x;
  if (sign_x == sign_y) {
    r.sig = u128_add(sig_x, sig_y);
    r.sticky = sticky;
  } else {
    r.sig = u128_sub(sig_x, sig_y);
    if (sticky) { /* x - (y + epsilon) = (x - y - 1) + (1 - epsilon) */
      r.sig = u128_sub(r.sig, u128(1));
      r.sticky = true;
    }
    if (u128_zero(r.sig) && !r.sticky) r.zero = true;
  }
  return r;
}

/* ------------------------------------------------------------------ */
/* Arithmetic.                                                         */
/* ------------------------------------------------------------------ */

static const Fmt *fmt_of(FP_Format format) { return &k_formats[format]; }

static uint64_t add_impl(FP_Format format, uint64_t op1, uint64_t op2, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  uint64_t result;
  if (process_nans(f, &a, &b, op1, op2, env, &result)) return result;
  const bool inf1 = a.type == T_INF, inf2 = b.type == T_INF;
  const bool zero1 = a.type == T_ZERO, zero2 = b.type == T_ZERO;
  if (inf1 && inf2 && a.sign != b.sign) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if ((inf1 && !a.sign) || (inf2 && !b.sign)) return fp_inf(f, false);
  if ((inf1 && a.sign) || (inf2 && b.sign)) return fp_inf(f, true);
  if (zero1 && zero2 && a.sign == b.sign) return fp_zero(f, a.sign);
  const bool round_down = fp_env_rounding(env) == FP_ROUND_MINUS_INF;
  if (zero1 && zero2) return fp_zero(f, round_down);
  if (zero1) return round_fpcr(f, format, b.sign, b.exp, u128(b.sig), false, env);
  if (zero2) return round_fpcr(f, format, a.sign, a.exp, u128(a.sig), false, env);
  const Exact sum = exact_add(a.sign, a.exp, u128(a.sig), b.sign, b.exp, u128(b.sig));
  if (sum.zero) return fp_zero(f, round_down);
  return round_fpcr(f, format, sum.sign, sum.exp, sum.sig, sum.sticky, env);
}

uint64_t fp_add(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return add_impl(format, a, b, env); }

uint64_t fp_sub(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) {
  /* FPSub negates op2 only after NaN processing, so the NaN payload/sign
   * of op2 survives unnegated. */
  const Fmt *f = fmt_of(format);
  const Unpacked ua = unpack(f, format, a, env), ub = unpack(f, format, b, env);
  uint64_t result;
  if (process_nans(f, &ua, &ub, a, b, env, &result)) return result;
  /* Flags already raised by unpack (IDC) must not be raised twice - they
   * are cumulative bits, so a second unpack inside add_impl is harmless. */
  return add_impl(format, a, fp_neg(format, b), env);
}

static uint64_t mul_impl(FP_Format format, uint64_t op1, uint64_t op2, bool mulx, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  uint64_t result;
  if (process_nans(f, &a, &b, op1, op2, env, &result)) return result;
  const bool inf1 = a.type == T_INF, inf2 = b.type == T_INF;
  const bool zero1 = a.type == T_ZERO, zero2 = b.type == T_ZERO;
  const bool sign = a.sign != b.sign;
  if ((inf1 && zero2) || (zero1 && inf2)) {
    if (mulx) return fp_two(f, sign);
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if (inf1 || inf2) return fp_inf(f, sign);
  if (zero1 || zero2) return fp_zero(f, sign);
  return round_fpcr(f, format, sign, a.exp + b.exp, mul64(a.sig, b.sig), false, env);
}

uint64_t fp_mul(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return mul_impl(format, a, b, false, env); }
uint64_t fp_mulx(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return mul_impl(format, a, b, true, env); }

/* (num << 64) / den for den != 0; quotient and whether a remainder exists. */
static U128 divide_128_by_64(uint64_t num, uint64_t den, bool *remainder) {
  U128 quotient = {0, 0};
  uint64_t rem = 0;
  U128 dividend = {num, 0};
  for (int i = 127; i >= 0; i--) {
    const uint64_t carry = rem >> 63;
    rem = (rem << 1) | (u128_bit(dividend, (unsigned)i) ? 1u : 0u);
    quotient = u128_shl(quotient, 1);
    if (carry || rem >= den) {
      rem -= den;
      quotient.lo |= 1u;
    }
  }
  *remainder = rem != 0;
  return quotient;
}

uint64_t fp_div(FP_Format format, uint64_t op1, uint64_t op2, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  uint64_t result;
  if (process_nans(f, &a, &b, op1, op2, env, &result)) return result;
  const bool inf1 = a.type == T_INF, inf2 = b.type == T_INF;
  const bool zero1 = a.type == T_ZERO, zero2 = b.type == T_ZERO;
  const bool sign = a.sign != b.sign;
  if ((inf1 && inf2) || (zero1 && zero2)) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if (inf1 || zero2) {
    if (!inf1) raise(env, FPSR_DZC);
    return fp_inf(f, sign);
  }
  if (zero1 || inf2) return fp_zero(f, sign);
  /* Normalize both significands to bit 63, then divide (num << 64). */
  const int na = clz64(a.sig), nb = clz64(b.sig);
  bool remainder = false;
  const U128 q = divide_128_by_64(a.sig << na, b.sig << nb, &remainder);
  const int exp = (a.exp - na) - (b.exp - nb) - 64;
  return round_fpcr(f, format, sign, exp, q, remainder, env);
}

/* floor(sqrt(x)) for a 128-bit x; *inexact if x is not a perfect square. */
static uint64_t isqrt128(U128 x, bool *inexact) {
  U128 rem = {0, 0};
  U128 root = {0, 0};
  for (int i = 63; i >= 0; i--) {
    const uint64_t pair = (u128_bit(x, (unsigned)(2 * i + 1)) ? 2u : 0u) | (u128_bit(x, (unsigned)(2 * i)) ? 1u : 0u);
    rem = u128_add(u128_shl(rem, 2), u128(pair));
    const U128 trial = u128_add(u128_shl(root, 2), u128(1));
    root = u128_shl(root, 1);
    if (u128_cmp(rem, trial) >= 0) {
      rem = u128_sub(rem, trial);
      root.lo |= 1u;
    }
  }
  *inexact = !u128_zero(rem);
  return root.lo;
}

uint64_t fp_sqrt(FP_Format format, uint64_t op, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  if (is_nan(a.type)) return process_nan(f, a.type, op, env);
  if (a.type == T_ZERO) return fp_zero(f, a.sign);
  if (a.sign) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if (a.type == T_INF) return fp_inf(f, false);
  U128 sig = u128(a.sig);
  int exp = a.exp;
  /* Put the MSB at bit 125 or 126 with an even exponent. */
  const int msb = u128_msb(sig);
  int shift = 125 - msb;
  if (((exp - shift) & 1) != 0) shift++;
  sig = u128_shl(sig, (unsigned)shift);
  exp -= shift;
  bool inexact = false;
  const uint64_t root = isqrt128(sig, &inexact);
  return round_fpcr(f, format, false, exp / 2, u128(root), inexact, env);
}

uint64_t fp_mul_add(FP_Format format, uint64_t addend, uint64_t op1, uint64_t op2, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked c = unpack(f, format, addend, env);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  const bool inf1 = a.type == T_INF, inf2 = b.type == T_INF;
  const bool zero1 = a.type == T_ZERO, zero2 = b.type == T_ZERO;
  uint64_t result;
  const bool done = process_nans3(f, &c, &a, &b, addend, op1, op2, env, &result);
  if (c.type == T_QNAN && ((inf1 && zero2) || (zero1 && inf2))) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if (done) return result;
  const bool infa = c.type == T_INF, zeroa = c.type == T_ZERO;
  const bool signp = a.sign != b.sign;
  const bool infp = inf1 || inf2, zerop = zero1 || zero2;
  if ((inf1 && zero2) || (zero1 && inf2) || (infa && infp && c.sign != signp)) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if ((infa && !c.sign) || (infp && !signp)) return fp_inf(f, false);
  if ((infa && c.sign) || (infp && signp)) return fp_inf(f, true);
  if (zeroa && zerop && c.sign == signp) return fp_zero(f, c.sign);
  const bool round_down = fp_env_rounding(env) == FP_ROUND_MINUS_INF;
  if (zerop && zeroa) return fp_zero(f, round_down);
  if (zerop) return round_fpcr(f, format, c.sign, c.exp, u128(c.sig), false, env);
  const U128 product = mul64(a.sig, b.sig);
  if (zeroa) return round_fpcr(f, format, signp, a.exp + b.exp, product, false, env);
  const Exact sum = exact_add(c.sign, c.exp, u128(c.sig), signp, a.exp + b.exp, product);
  if (sum.zero) return fp_zero(f, round_down);
  return round_fpcr(f, format, sum.sign, sum.exp, sum.sig, sum.sticky, env);
}

uint64_t fp_abs(FP_Format format, uint64_t a) {
  const Fmt *f = fmt_of(format);
  return a & ~((uint64_t)1 << (f->exp_bits + f->frac_bits));
}

uint64_t fp_neg(FP_Format format, uint64_t a) {
  const Fmt *f = fmt_of(format);
  return a ^ ((uint64_t)1 << (f->exp_bits + f->frac_bits));
}

/* -1, 0, 1 comparing two non-NaN unpacked values (zeros equal). */
static int compare_values(const Unpacked *a, const Unpacked *b) {
  const bool a_zero = a->type == T_ZERO, b_zero = b->type == T_ZERO;
  if (a_zero && b_zero) return 0;
  /* Magnitude ordering, then apply signs. */
  int magnitude;
  if (a->type == T_INF || b->type == T_INF) {
    magnitude = (a->type == T_INF) - (b->type == T_INF);
  } else if (a_zero || b_zero) {
    magnitude = a_zero ? -1 : 1;
  } else {
    const int ma = a->exp + (63 - clz64(a->sig)), mb = b->exp + (63 - clz64(b->sig));
    if (ma != mb) {
      magnitude = ma < mb ? -1 : 1;
    } else {
      /* Same top exponent: compare significands aligned to the same exp. */
      U128 sa = u128(a->sig), sb = u128(b->sig);
      int ea = a->exp, eb = b->exp;
      normalize_to(&sa, &ea);
      normalize_to(&sb, &eb);
      magnitude = u128_cmp(sa, sb);
    }
  }
  const bool a_neg = a->sign && !a_zero, b_neg = b->sign && !b_zero;
  if (a_neg != b_neg) return a_neg ? -1 : 1;
  return a_neg ? -magnitude : magnitude;
}

static uint64_t repack(const Fmt *f, FP_Format format, const Unpacked *u, FP_Env *env) {
  if (u->type == T_INF) return fp_inf(f, u->sign);
  if (u->type == T_ZERO) return fp_zero(f, u->sign);
  return round_fpcr(f, format, u->sign, u->exp, u128(u->sig), false, env);
}

static uint64_t minmax(FP_Format format, uint64_t op1, uint64_t op2, bool want_max, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  uint64_t result;
  if (process_nans(f, &a, &b, op1, op2, env, &result)) return result;
  const int cmp = compare_values(&a, &b);
  Unpacked chosen = (want_max ? cmp > 0 : cmp < 0) ? a : b;
  if (chosen.type == T_ZERO) chosen.sign = want_max ? (a.sign && b.sign) : (a.sign || b.sign);
  return repack(f, format, &chosen, env);
}

uint64_t fp_max(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return minmax(format, a, b, true, env); }
uint64_t fp_min(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return minmax(format, a, b, false, env); }

static uint64_t minmax_num(FP_Format format, uint64_t op1, uint64_t op2, bool want_max, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  /* Types only (FPUnpack's flags are raised again inside minmax). */
  uint32_t scratch = 0;
  FP_Env quiet = {env->fpcr, &scratch};
  const FP_Type t1 = unpack(f, format, op1, &quiet).type, t2 = unpack(f, format, op2, &quiet).type;
  if (t1 == T_QNAN && t2 != T_QNAN) op1 = fp_inf(f, want_max);
  else if (t1 != T_QNAN && t2 == T_QNAN) op2 = fp_inf(f, want_max);
  return minmax(format, op1, op2, want_max, env);
}

uint64_t fp_max_num(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return minmax_num(format, a, b, true, env); }
uint64_t fp_min_num(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return minmax_num(format, a, b, false, env); }

/* ------------------------------------------------------------------ */
/* Rounding to integral, compare, convert.                             */
/* ------------------------------------------------------------------ */

/* Splits |value| = sig * 2^exp into integer part and rounding info. Returns
 * false if the integer part does not fit in 64 bits. */
static bool split_integer(uint64_t sig, int exp, uint64_t *integer, bool *half, bool *above_half, bool *below_half) {
  *half = *above_half = *below_half = false;
  if (exp >= 0) {
    /* (exp == 0 must not reach `sig >> 64`: shifting by the width is UB.) */
    if (exp >= 64 || (exp > 0 && (sig >> (64 - exp)) != 0)) return false;
    *integer = sig << exp;
    return true;
  }
  const unsigned shift = (unsigned)(-exp);
  if (shift >= 128) {
    *integer = 0;
    *below_half = sig != 0;
    return true;
  }
  U128 s = u128(sig);
  *integer = u128_shr(s, shift).lo;
  const U128 frac = u128_sub(s, u128_shl(u128_shr(s, shift), shift)); /* low `shift` bits */
  const U128 halfway = u128_shl(u128(1), shift - 1u);
  const int c = u128_cmp(frac, halfway);
  if (u128_zero(frac)) return true;
  if (c == 0) *half = true;
  else if (c > 0) *above_half = true;
  else *below_half = true;
  return true;
}

/* Rounds a magnitude with rounding info; `negative` is the sign of the value. */
static bool magnitude_round_up(FP_Rounding rounding, bool negative, uint64_t integer, bool half, bool above, bool below) {
  const bool inexact = half || above || below;
  switch (rounding) {
  case FP_ROUND_NEAREST_EVEN: return above || (half && (integer & 1u));
  case FP_ROUND_PLUS_INF: return inexact && !negative;
  case FP_ROUND_MINUS_INF: return inexact && negative;
  case FP_ROUND_ZERO: return false;
  default: return above || half; /* ties away */
  }
}

uint64_t fp_round_int(FP_Format format, uint64_t op, FP_Rounding rounding, bool exact, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  if (is_nan(a.type)) return process_nan(f, a.type, op, env);
  if (a.type == T_INF) return fp_inf(f, a.sign);
  if (a.type == T_ZERO) return fp_zero(f, a.sign);
  if (a.exp >= 0) return op; /* already integral */
  uint64_t integer = 0;
  bool half, above, below;
  (void)split_integer(a.sig, a.exp, &integer, &half, &above, &below);
  const bool inexact = half || above || below;
  if (magnitude_round_up(rounding, a.sign, integer, half, above, below)) integer++;
  if (inexact && exact) raise(env, FPSR_IXC);
  if (integer == 0) return fp_zero(f, a.sign);
  uint32_t ignored = 0;
  FP_Env exact_env = {env->fpcr, &ignored};
  return round_value(f, format, a.sign, 0, u128(integer), false, FP_ROUND_ZERO, &exact_env);
}

uint32_t fp_compare(FP_Format format, uint64_t op1, uint64_t op2, bool signal_all_nans, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  if (is_nan(a.type) || is_nan(b.type)) {
    if (a.type == T_SNAN || b.type == T_SNAN || signal_all_nans) raise(env, FPSR_IOC);
    return 0x3u;
  }
  const int cmp = compare_values(&a, &b);
  if (cmp == 0) return 0x6u;
  return cmp < 0 ? 0x8u : 0x2u;
}

static int compare_or_nan(FP_Format format, uint64_t op1, uint64_t op2, bool signal_any_nan, FP_Env *env,
                          bool *unordered) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  *unordered = is_nan(a.type) || is_nan(b.type);
  if (*unordered) {
    if (signal_any_nan || a.type == T_SNAN || b.type == T_SNAN) raise(env, FPSR_IOC);
    return 0;
  }
  return compare_values(&a, &b);
}

bool fp_compare_eq(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) {
  bool unordered;
  const int c = compare_or_nan(format, a, b, false, env, &unordered);
  return !unordered && c == 0;
}
bool fp_compare_ge(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) {
  bool unordered;
  const int c = compare_or_nan(format, a, b, true, env, &unordered);
  return !unordered && c >= 0;
}
bool fp_compare_gt(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) {
  bool unordered;
  const int c = compare_or_nan(format, a, b, true, env, &unordered);
  return !unordered && c > 0;
}

static uint64_t convert_impl(FP_Format to, FP_Format from, uint64_t op, bool round_odd, FP_Env *env);

uint64_t fp_convert(FP_Format to, FP_Format from, uint64_t op, FP_Env *env) {
  return convert_impl(to, from, op, false, env);
}

uint64_t fp_convert_round_odd(uint64_t op, FP_Env *env) {
  return convert_impl(FP_SINGLE, FP_DOUBLE, op, true, env);
}

static uint64_t convert_impl(FP_Format to, FP_Format from, uint64_t op, bool round_odd, FP_Env *env) {
  const Fmt *ff = fmt_of(from), *tf = fmt_of(to);
  const Unpacked a = unpack(ff, from, op, env);
  if (is_nan(a.type)) {
    uint64_t result;
    if (env->fpcr & FPCR_DN) {
      result = fp_default_nan(tf);
    } else {
      const uint64_t frac = op & mask_bits(ff->frac_bits);
      const uint64_t moved = tf->frac_bits >= ff->frac_bits ? frac << (tf->frac_bits - ff->frac_bits)
                                                            : frac >> (ff->frac_bits - tf->frac_bits);
      result = pack(tf, a.sign, exp_all_ones(tf), moved | ((uint64_t)1 << (tf->frac_bits - 1u)));
    }
    if (a.type == T_SNAN) raise(env, FPSR_IOC);
    return result;
  }
  if (a.type == T_INF) return fp_inf(tf, a.sign);
  if (a.type == T_ZERO) return fp_zero(tf, a.sign);
  if (round_odd) return round_value(tf, to, a.sign, a.exp, u128(a.sig), false, FP_ROUND_ODD, env);
  return round_fpcr(tf, to, a.sign, a.exp, u128(a.sig), false, env);
}

uint64_t fp_to_int(FP_Format format, uint64_t op, unsigned fbits, bool is_unsigned, unsigned int_bits,
                   FP_Rounding rounding, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  const uint64_t int_mask = mask_bits(int_bits);
  const uint64_t max_unsigned = int_mask;
  const uint64_t max_signed = int_mask >> 1;
  const uint64_t min_signed = ((uint64_t)1 << (int_bits - 1u)); /* magnitude of the minimum */
  if (is_nan(a.type)) {
    raise(env, FPSR_IOC);
    return 0;
  }
  if (a.type == T_ZERO) return 0;
  bool overflow = false;
  uint64_t magnitude = 0;
  bool half = false, above = false, below = false;
  if (a.type == T_INF) {
    overflow = true;
  } else if (!split_integer(a.sig, a.exp + (int)fbits, &magnitude, &half, &above, &below)) {
    overflow = true;
  } else if (magnitude_round_up(rounding, a.sign, magnitude, half, above, below)) {
    if (magnitude == ~(uint64_t)0) overflow = true;
    else magnitude++;
  }
  uint64_t result = 0;
  if (!overflow) {
    if (is_unsigned) {
      if (a.sign && magnitude != 0) overflow = true;
      else if (magnitude > max_unsigned) overflow = true;
      else result = magnitude;
    } else if (a.sign) {
      if (magnitude > min_signed) overflow = true;
      else result = (0u - magnitude) & int_mask;
    } else {
      if (magnitude > max_signed) overflow = true;
      else result = magnitude;
    }
  }
  if (overflow) {
    raise(env, FPSR_IOC);
    if (is_unsigned) return a.sign ? 0 : max_unsigned;
    return a.sign ? min_signed : max_signed;
  }
  if (half || above || below) raise(env, FPSR_IXC);
  return result;
}

uint64_t fp_from_int(FP_Format format, uint64_t value, unsigned fbits, bool is_signed, unsigned int_bits,
                     FP_Env *env) {
  const Fmt *f = fmt_of(format);
  value &= mask_bits(int_bits);
  bool negative = false;
  if (is_signed && ((value >> (int_bits - 1u)) & 1u)) {
    negative = true;
    value = (0u - value) & mask_bits(int_bits); /* the minimum negates to itself: 2^(n-1) */
  }
  if (value == 0) return fp_zero(f, false);
  return round_fpcr(f, format, negative, -(int)fbits, u128(value), false, env);
}

/* ------------------------------------------------------------------ */
/* Estimates (Arm ARM RecipEstimate / RecipSqrtEstimate, ARMv8.0).     */
/* ------------------------------------------------------------------ */

static uint32_t recip_estimate(uint32_t a) {
  a = a * 2u + 1u;
  const uint32_t b = (1u << 19) / a;
  return (b + 1u) / 2u;
}

static uint32_t recip_sqrt_estimate(uint32_t a) {
  if (a < 256u) {
    a = a * 2u + 1u;
  } else {
    a = (a >> 1) << 1;
    a = (a + 1u) * 2u;
  }
  uint64_t b = 512;
  while ((uint64_t)a * (b + 1u) * (b + 1u) < ((uint64_t)1 << 28)) b++;
  return (uint32_t)((b + 1u) / 2u);
}

static int floor_div2(int x) { return x >= 0 ? x / 2 : -((-x + 1) / 2); }

uint64_t fp_recip_estimate(FP_Format format, uint64_t op, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  if (is_nan(a.type)) return process_nan(f, a.type, op, env);
  if (a.type == T_INF) return fp_zero(f, a.sign);
  if (a.type == T_ZERO) {
    raise(env, FPSR_DZC);
    return fp_inf(f, a.sign);
  }
  const int top = a.exp + (63 - clz64(a.sig)); /* |value| in [2^top, 2^(top+1)) */
  const int tiny = format == FP_SINGLE ? -128 : -1024;
  if (top < tiny) {
    const FP_Rounding r = fp_env_rounding(env);
    const bool to_inf = r == FP_ROUND_NEAREST_EVEN || (r == FP_ROUND_PLUS_INF && !a.sign) ||
                        (r == FP_ROUND_MINUS_INF && a.sign);
    raise(env, FPSR_OFC | FPSR_IXC);
    return to_inf ? fp_inf(f, a.sign) : fp_max_normal(f, a.sign);
  }
  const int huge = format == FP_SINGLE ? 126 : 1022;
  if ((env->fpcr & FPCR_FZ) && top >= huge) {
    raise(env, FPSR_UFC);
    return fp_zero(f, a.sign);
  }
  /* 52-bit fraction view, as the pseudocode builds for both widths. */
  uint64_t fraction;
  int exp;
  if (format == FP_SINGLE) {
    fraction = (op & 0x7FFFFFu) << 29;
    exp = (int)((op >> 23) & 0xFFu);
  } else {
    fraction = op & 0xFFFFFFFFFFFFFull;
    exp = (int)((op >> 52) & 0x7FFu);
  }
  if (exp == 0) {
    if (((fraction >> 51) & 1u) == 0) {
      exp = -1;
      fraction = (fraction << 2) & 0xFFFFFFFFFFFFFull;
    } else {
      fraction = (fraction << 1) & 0xFFFFFFFFFFFFFull;
    }
  }
  const uint32_t scaled = (uint32_t)(0x100u | ((fraction >> 44) & 0xFFu));
  int result_exp = (format == FP_SINGLE ? 253 : 2045) - exp;
  const uint32_t estimate = recip_estimate(scaled);
  fraction = (uint64_t)(estimate & 0xFFu) << 44;
  if (result_exp == 0) {
    fraction = ((uint64_t)1 << 51) | (fraction >> 1);
  } else if (result_exp == -1) {
    fraction = ((uint64_t)1 << 50) | (fraction >> 2);
    result_exp = 0;
  }
  if (format == FP_SINGLE) return pack(f, a.sign, (uint64_t)result_exp & 0xFFu, fraction >> 29);
  return pack(f, a.sign, (uint64_t)result_exp & 0x7FFu, fraction);
}

uint64_t fp_rsqrt_estimate(FP_Format format, uint64_t op, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  if (is_nan(a.type)) return process_nan(f, a.type, op, env);
  if (a.type == T_ZERO) {
    raise(env, FPSR_DZC);
    return fp_inf(f, a.sign);
  }
  if (a.sign) {
    raise(env, FPSR_IOC);
    return fp_default_nan(f);
  }
  if (a.type == T_INF) return fp_zero(f, false);
  uint64_t fraction;
  int exp;
  if (format == FP_SINGLE) {
    fraction = (op & 0x7FFFFFu) << 29;
    exp = (int)((op >> 23) & 0xFFu);
  } else {
    fraction = op & 0xFFFFFFFFFFFFFull;
    exp = (int)((op >> 52) & 0x7FFu);
  }
  if (exp == 0) {
    while (((fraction >> 51) & 1u) == 0) {
      fraction = (fraction << 1) & 0xFFFFFFFFFFFFFull;
      exp--;
    }
    fraction = (fraction << 1) & 0xFFFFFFFFFFFFFull;
  }
  const uint32_t scaled = (exp & 1) == 0 ? (uint32_t)(0x100u | ((fraction >> 44) & 0xFFu))
                                         : (uint32_t)(0x80u | ((fraction >> 45) & 0x7Fu));
  const int result_exp = floor_div2((format == FP_SINGLE ? 380 : 3068) - exp);
  const uint32_t estimate = recip_sqrt_estimate(scaled);
  if (format == FP_SINGLE) {
    return pack(f, false, (uint64_t)result_exp & 0xFFu, (uint64_t)(estimate & 0xFFu) << 15);
  }
  return pack(f, false, (uint64_t)result_exp & 0x7FFu, (uint64_t)(estimate & 0xFFu) << 44);
}

static uint64_t step_impl(FP_Format format, uint64_t op1, uint64_t op2, bool rsqrt, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  op1 = fp_neg(format, op1);
  const Unpacked a = unpack(f, format, op1, env), b = unpack(f, format, op2, env);
  uint64_t result;
  if (process_nans(f, &a, &b, op1, op2, env, &result)) return result;
  const bool inf1 = a.type == T_INF, inf2 = b.type == T_INF;
  const bool zero1 = a.type == T_ZERO, zero2 = b.type == T_ZERO;
  if ((inf1 && zero2) || (zero1 && inf2)) return rsqrt ? fp_one_point_five(f, false) : fp_two(f, false);
  if (inf1 || inf2) return fp_inf(f, a.sign != b.sign);
  /* constant + value1 * value2, then (rsqrt) halved, with one rounding. */
  const U128 constant = u128(3u); /* 2 = 1 * 2^1, 3 = 3 * 2^0 */
  const bool signp = a.sign != b.sign;
  Exact sum;
  if (zero1 || zero2) {
    sum.zero = false;
    sum.sign = false;
    sum.exp = rsqrt ? 0 : 1;
    sum.sig = rsqrt ? constant : u128(1u);
    sum.sticky = false;
  } else {
    sum = exact_add(false, rsqrt ? 0 : 1, rsqrt ? constant : u128(1u), signp, a.exp + b.exp, mul64(a.sig, b.sig));
  }
  const bool round_down = fp_env_rounding(env) == FP_ROUND_MINUS_INF;
  if (sum.zero) return fp_zero(f, round_down);
  return round_fpcr(f, format, sum.sign, sum.exp - (rsqrt ? 1 : 0), sum.sig, sum.sticky, env);
}

uint64_t fp_recip_step(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return step_impl(format, a, b, false, env); }
uint64_t fp_rsqrt_step(FP_Format format, uint64_t a, uint64_t b, FP_Env *env) { return step_impl(format, a, b, true, env); }

uint64_t fp_recip_exponent(FP_Format format, uint64_t op, FP_Env *env) {
  const Fmt *f = fmt_of(format);
  const Unpacked a = unpack(f, format, op, env);
  if (is_nan(a.type)) return process_nan(f, a.type, op, env);
  const uint64_t exp_field = (op >> f->frac_bits) & exp_all_ones(f);
  if (exp_field == 0) return pack(f, a.sign, exp_all_ones(f) - 1u, 0);
  return pack(f, a.sign, ~exp_field & exp_all_ones(f), 0);
}

uint32_t fp_unsigned_recip_estimate(uint32_t a) {
  if (!(a >> 31)) return 0xFFFFFFFFu;
  return recip_estimate((a >> 23) & 0x1FFu) << 23;
}

uint32_t fp_unsigned_rsqrt_estimate(uint32_t a) {
  if (!(a >> 30)) return 0xFFFFFFFFu;
  return recip_sqrt_estimate((a >> 23) & 0x1FFu) << 23;
}

uint64_t fp_expand_imm8(FP_Format format, uint32_t imm8) {
  const Fmt *f = fmt_of(format);
  const uint64_t b6 = (imm8 >> 6) & 1u;
  const unsigned e = f->exp_bits;
  uint64_t exp = ((b6 ^ 1u) << (e - 1u)) | ((b6 ? mask_bits(e - 3u) : 0) << 2) | ((imm8 >> 4) & 3u);
  const uint64_t frac = (uint64_t)(imm8 & 0xFu) << (f->frac_bits - 4u);
  return pack(f, (imm8 >> 7) & 1u, exp, frac);
}
