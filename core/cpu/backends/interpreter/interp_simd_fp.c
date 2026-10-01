/**
 * A64 SIMD&FP data processing (DDI 0487 C4.1.90). This file holds the
 * top-level split and the scalar floating-point instructions; arithmetic
 * is softfloat.c (ARM semantics on every host, §3). Advanced SIMD vector
 * and scalar forms live in interp_simd.c; structure loads/stores in
 * interp_simd_ldst.c.
 *
 * ARMv8.0: half precision exists only for FCVT (no FP16 arithmetic on the
 * Switch's Cortex-A57); ftype 11 elsewhere is undefined.
 */
#include "cpu/backends/interpreter/interp_internal.h"
#include "cpu/backends/interpreter/interp_simd.h"
#include "cpu/backends/interpreter/softfloat.h"

#define FTYPE_SINGLE 0u
#define FTYPE_DOUBLE 1u
#define FTYPE_HALF 3u

static bool ftype_to_format(uint32_t ftype, FP_Format *format) {
  switch (ftype) {
  case FTYPE_SINGLE: *format = FP_SINGLE; return true;
  case FTYPE_DOUBLE: *format = FP_DOUBLE; return true;
  default: return false; /* 10 unallocated, 11 needs FEAT_FP16 */
  }
}

static uint64_t format_mask(FP_Format format) {
  switch (format) {
  case FP_HALF: return 0xFFFFu;
  case FP_SINGLE: return 0xFFFFFFFFu;
  default: return ~(uint64_t)0;
  }
}

/* Scalar reads take the low bits; scalar writes zero the rest of V. */
static uint64_t read_scalar(const Interp_State *s, uint32_t reg, FP_Format format) {
  return s->v[reg].lo & format_mask(format);
}
static void write_scalar(Interp_State *s, uint32_t reg, FP_Format format, uint64_t value) {
  s->v[reg].lo = value & format_mask(format);
  s->v[reg].hi = 0;
}

static FP_Env env_of(Interp_State *s) { return (FP_Env){s->fpcr, &s->fpsr}; }

/* ------------------------------------------------------------------ */
/* Conversions between FP and integer / fixed-point.                   */
/* ------------------------------------------------------------------ */

static FP_Rounding rmode_rounding(uint32_t rmode) {
  switch (rmode) {
  case 0: return FP_ROUND_NEAREST_EVEN;
  case 1: return FP_ROUND_PLUS_INF;
  case 2: return FP_ROUND_MINUS_INF;
  default: return FP_ROUND_ZERO;
  }
}

static Interp_Status convert_fixed(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16), scale = bits(insn, 15, 10);
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format) || (!sf && scale < 32u)) return INTERP_UNDEFINED;
  const unsigned fbits = 64u - scale;
  const unsigned int_bits = sf ? 64u : 32u;
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  FP_Env env = env_of(s);
  if (rmode == 3 && (opcode == 0 || opcode == 1)) { /* FCVTZS / FCVTZU */
    const uint64_t r = fp_to_int(format, read_scalar(s, rn, format), fbits, opcode == 1, int_bits, FP_ROUND_ZERO, &env);
    set_reg_width(s, rd, sf, r);
    return advance(s);
  }
  if (rmode == 0 && (opcode == 2 || opcode == 3)) { /* SCVTF / UCVTF */
    write_scalar(s, rd, format, fp_from_int(format, xreg(s, rn), fbits, opcode == 2, int_bits, &env));
    return advance(s);
  }
  return INTERP_UNDEFINED;
}

static Interp_Status convert_integer(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t ftype = bits(insn, 23, 22), rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  const unsigned int_bits = sf ? 64u : 32u;

  if (opcode == 6 || opcode == 7) { /* FMOV (general) */
    const bool to_fp = opcode == 7;
    if (ftype == 2 && sf && rmode == 1) { /* FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn */
      if (to_fp) s->v[rd].hi = xreg(s, rn);
      else set_xreg(s, rd, s->v[rn].hi);
      return advance(s);
    }
    if (rmode != 0 || !((!sf && ftype == FTYPE_SINGLE) || (sf && ftype == FTYPE_DOUBLE))) return INTERP_UNDEFINED;
    const FP_Format format = sf ? FP_DOUBLE : FP_SINGLE;
    if (to_fp) write_scalar(s, rd, format, xreg(s, rn));
    else set_reg_width(s, rd, sf, read_scalar(s, rn, format));
    return advance(s);
  }

  FP_Format format;
  if (!ftype_to_format(ftype, &format)) return INTERP_UNDEFINED;
  FP_Env env = env_of(s);
  switch (opcode) {
  case 0: case 1: { /* FCVT{N,P,M,Z}{S,U} */
    const uint64_t r = fp_to_int(format, read_scalar(s, rn, format), 0, opcode == 1, int_bits, rmode_rounding(rmode), &env);
    set_reg_width(s, rd, sf, r);
    return advance(s);
  }
  case 2: case 3: /* SCVTF / UCVTF */
    if (rmode != 0) return INTERP_UNDEFINED;
    write_scalar(s, rd, format, fp_from_int(format, xreg(s, rn), 0, opcode == 2, int_bits, &env));
    return advance(s);
  case 4: case 5: { /* FCVTAS / FCVTAU */
    if (rmode != 0) return INTERP_UNDEFINED;
    const uint64_t r = fp_to_int(format, read_scalar(s, rn, format), 0, opcode == 5, int_bits, FP_ROUND_TIE_AWAY, &env);
    set_reg_width(s, rd, sf, r);
    return advance(s);
  }
  default:
    return INTERP_UNDEFINED;
  }
}

/* ------------------------------------------------------------------ */
/* Data processing.                                                    */
/* ------------------------------------------------------------------ */

static Interp_Status dp_one_source(Interp_State *s, uint32_t insn) {
  const uint32_t ftype = bits(insn, 23, 22), opcode = bits(insn, 20, 15);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  FP_Env env = env_of(s);

  if ((opcode & 0x3Cu) == 0x04u) { /* FCVT between precisions: opc = destination */
    const uint32_t opc = opcode & 3u;
    if (opc == ftype || opc == 2u || ftype == 2u) return INTERP_UNDEFINED;
    const FP_Format from = ftype == FTYPE_HALF ? FP_HALF : (ftype == FTYPE_DOUBLE ? FP_DOUBLE : FP_SINGLE);
    const FP_Format to = opc == FTYPE_HALF ? FP_HALF : (opc == FTYPE_DOUBLE ? FP_DOUBLE : FP_SINGLE);
    write_scalar(s, rd, to, fp_convert(to, from, read_scalar(s, rn, from), &env));
    return advance(s);
  }

  FP_Format format;
  if (!ftype_to_format(ftype, &format)) return INTERP_UNDEFINED;
  const uint64_t a = read_scalar(s, rn, format);
  uint64_t r;
  switch (opcode) {
  case 0x00: r = a; break;                                           /* FMOV */
  case 0x01: r = fp_abs(format, a); break;                           /* FABS */
  case 0x02: r = fp_neg(format, a); break;                           /* FNEG */
  case 0x03: r = fp_sqrt(format, a, &env); break;                    /* FSQRT */
  case 0x08: r = fp_round_int(format, a, FP_ROUND_NEAREST_EVEN, false, &env); break; /* FRINTN */
  case 0x09: r = fp_round_int(format, a, FP_ROUND_PLUS_INF, false, &env); break;     /* FRINTP */
  case 0x0A: r = fp_round_int(format, a, FP_ROUND_MINUS_INF, false, &env); break;    /* FRINTM */
  case 0x0B: r = fp_round_int(format, a, FP_ROUND_ZERO, false, &env); break;         /* FRINTZ */
  case 0x0C: r = fp_round_int(format, a, FP_ROUND_TIE_AWAY, false, &env); break;     /* FRINTA */
  case 0x0E: r = fp_round_int(format, a, fp_env_rounding(&env), true, &env); break;  /* FRINTX */
  case 0x0F: r = fp_round_int(format, a, fp_env_rounding(&env), false, &env); break; /* FRINTI */
  default: return INTERP_UNDEFINED;
  }
  write_scalar(s, rd, format, r);
  return advance(s);
}

static Interp_Status dp_two_source(Interp_State *s, uint32_t insn) {
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format)) return INTERP_UNDEFINED;
  const uint64_t a = read_scalar(s, bits(insn, 9, 5), format);
  const uint64_t b = read_scalar(s, bits(insn, 20, 16), format);
  FP_Env env = env_of(s);
  uint64_t r;
  switch (bits(insn, 15, 12)) {
  case 0x0: r = fp_mul(format, a, b, &env); break;
  case 0x1: r = fp_div(format, a, b, &env); break;
  case 0x2: r = fp_add(format, a, b, &env); break;
  case 0x3: r = fp_sub(format, a, b, &env); break;
  case 0x4: r = fp_max(format, a, b, &env); break;
  case 0x5: r = fp_min(format, a, b, &env); break;
  case 0x6: r = fp_max_num(format, a, b, &env); break;
  case 0x7: r = fp_min_num(format, a, b, &env); break;
  case 0x8: r = fp_neg(format, fp_mul(format, a, b, &env)); break; /* FNMUL negates even a NaN */
  default: return INTERP_UNDEFINED;
  }
  write_scalar(s, bits(insn, 4, 0), format, r);
  return advance(s);
}

static Interp_Status dp_three_source(Interp_State *s, uint32_t insn) {
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format)) return INTERP_UNDEFINED;
  const bool o1 = bit(insn, 21), o0 = bit(insn, 15);
  uint64_t addend = read_scalar(s, bits(insn, 14, 10), format);
  uint64_t n = read_scalar(s, bits(insn, 9, 5), format);
  const uint64_t m = read_scalar(s, bits(insn, 20, 16), format);
  /* FMADD 00, FMSUB 01 (negate n), FNMADD 10 (negate a and n), FNMSUB 11 (negate a). */
  if (o1) addend = fp_neg(format, addend);
  if (o0 != o1) n = fp_neg(format, n);
  FP_Env env = env_of(s);
  write_scalar(s, bits(insn, 4, 0), format, fp_mul_add(format, addend, n, m, &env));
  return advance(s);
}

static Interp_Status compare(Interp_State *s, uint32_t insn) {
  FP_Format format;
  const uint32_t opcode2 = bits(insn, 4, 0);
  if (!ftype_to_format(bits(insn, 23, 22), &format) || bits(insn, 15, 14) != 0 || (opcode2 & 7u) != 0) {
    return INTERP_UNDEFINED;
  }
  const bool with_zero = (opcode2 >> 3) & 1u, signal = (opcode2 >> 4) & 1u;
  const uint64_t a = read_scalar(s, bits(insn, 9, 5), format);
  const uint64_t b = with_zero ? 0 : read_scalar(s, bits(insn, 20, 16), format);
  FP_Env env = env_of(s);
  set_nzcv(s, fp_compare(format, a, b, signal, &env));
  return advance(s);
}

static Interp_Status conditional_compare(Interp_State *s, uint32_t insn) {
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format)) return INTERP_UNDEFINED;
  if (!interp_condition_holds(s, bits(insn, 15, 12))) {
    set_nzcv(s, bits(insn, 3, 0));
    return advance(s);
  }
  FP_Env env = env_of(s);
  set_nzcv(s, fp_compare(format, read_scalar(s, bits(insn, 9, 5), format), read_scalar(s, bits(insn, 20, 16), format),
                         bit(insn, 4), &env));
  return advance(s);
}

static Interp_Status conditional_select(Interp_State *s, uint32_t insn) {
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format)) return INTERP_UNDEFINED;
  const uint32_t source = interp_condition_holds(s, bits(insn, 15, 12)) ? bits(insn, 9, 5) : bits(insn, 20, 16);
  write_scalar(s, bits(insn, 4, 0), format, read_scalar(s, source, format));
  return advance(s);
}

static Interp_Status move_immediate(Interp_State *s, uint32_t insn) {
  FP_Format format;
  if (!ftype_to_format(bits(insn, 23, 22), &format) || bits(insn, 9, 5) != 0) return INTERP_UNDEFINED;
  write_scalar(s, bits(insn, 4, 0), format, fp_expand_imm8(format, bits(insn, 20, 13)));
  return advance(s);
}

static Interp_Status scalar_fp(Interp_State *s, uint32_t insn) {
  if (bit(insn, 29)) return INTERP_UNDEFINED; /* S must be 0 */
  /* Conversions use bit 31 as sf; everything else has M there, which must be 0. */
  if (!bit(insn, 24) && !bit(insn, 21)) return convert_fixed(s, insn);
  if (!bit(insn, 24) && bits(insn, 15, 10) == 0) return convert_integer(s, insn);
  if (bit(insn, 31)) return INTERP_UNDEFINED;
  if (bit(insn, 24)) return dp_three_source(s, insn);
  if (bits(insn, 14, 10) == 0x10) return dp_one_source(s, insn);
  if (bits(insn, 13, 10) == 0x8) return compare(s, insn);
  if (bits(insn, 12, 10) == 0x4) return move_immediate(s, insn);
  switch (bits(insn, 11, 10)) {
  case 1: return conditional_compare(s, insn);
  case 2: return dp_two_source(s, insn);
  case 3: return conditional_select(s, insn);
  default: return INTERP_UNDEFINED;
  }
}

Interp_Status interp_simd_fp(Interp_State *s, uint32_t insn) {
  if (bit(insn, 28)) {
    if (!bit(insn, 30)) {
      if (bits(insn, 28, 25) == 0xF && bits(insn, 28, 24) >= 0x1E) return scalar_fp(s, insn);
      return INTERP_UNDEFINED; /* crypto and others */
    }
    return interp_simd_scalar(s, insn);
  }
  return interp_simd_vector(s, insn);
}
