/**
 * AArch32 VFP (coprocessors 10 and 11) and Advanced SIMD for the A32
 * interpreter. Arithmetic is the A64 interpreter's softfloat with the
 * FPSCR's control bits as the FPCR (ARM semantics: flush-to-zero, default
 * NaN, rounding mode). Short vectors (FPSCR.Len/Stride) do not exist on
 * ARMv8 and are not implemented.
 */
#include "cpu/backends/a32/a32_internal.h"

#include <string.h>

static inline uint32_t f(uint32_t insn, unsigned hi, unsigned lo) { return bits(insn, hi, lo); }
static inline uint32_t b1(uint32_t insn, unsigned n) { return bit(insn, n); }

/* Register numbers: single precision Vx:X, double precision X:Vx. */
static uint32_t vd(uint32_t insn, bool dbl) { return dbl ? (b1(insn, 22) << 4) | f(insn, 15, 12) : (f(insn, 15, 12) << 1) | b1(insn, 22); }
static uint32_t vn(uint32_t insn, bool dbl) { return dbl ? (b1(insn, 7) << 4) | f(insn, 19, 16) : (f(insn, 19, 16) << 1) | b1(insn, 7); }
static uint32_t vm(uint32_t insn, bool dbl) { return dbl ? (b1(insn, 5) << 4) | f(insn, 3, 0) : (f(insn, 3, 0) << 1) | b1(insn, 5); }

static uint64_t get_fp(const A32_State *a, uint32_t n, bool dbl) { return dbl ? a32_d(a, n) : a32_s(a, n); }
static void set_fp(A32_State *a, uint32_t n, bool dbl, uint64_t v) {
  if (dbl) a32_set_d(a, n, v);
  else a32_set_s(a, n, (uint32_t)v);
}

static FP_Env env_of(A32_State *a) {
  FP_Env e = {a->s.fpcr, &a->s.fpsr};
  return e;
}

/* ---- loads and stores ---------------------------------------------- */

static Interp_Status load_store(A32_State *a, uint32_t insn) {
  const uint32_t p = b1(insn, 24), u = b1(insn, 23), w = b1(insn, 21), load = b1(insn, 20), rn = f(insn, 19, 16);
  const bool dbl = b1(insn, 8);
  const uint32_t imm8 = f(insn, 7, 0);
  const uint32_t base = rn == A32_PC ? (uint32_t)(a->s.regs.pc + A32_PC_READ_OFFSET) & ~3u : a32_reg(a, rn);
  if (p && !w) { /* VLDR / VSTR */
    const uint32_t address = u ? base + imm8 * 4u : base - imm8 * 4u;
    const uint32_t d = vd(insn, dbl);
    if (load) {
      uint64_t v = 0;
      if (!a32_read(a, address, &v, dbl ? 8u : 4u)) return INTERP_FAULT;
      set_fp(a, d, dbl, v);
    } else {
      const uint64_t v = get_fp(a, d, dbl);
      if (!a32_write(a, address, &v, dbl ? 8u : 4u)) return INTERP_FAULT;
    }
    return INTERP_CONTINUE;
  }
  /* VLDM / VSTM (IA with optional writeback, DB with writeback): VPUSH / VPOP. */
  if (p == u || (p && !w)) return INTERP_UNDEFINED;
  const uint32_t words = imm8;                  /* doubles: 2 per register (FLDMX: odd, one spare) */
  const uint32_t regs = dbl ? words / 2u : words;
  const uint32_t first = vd(insn, dbl);
  if (!regs || first + regs > (dbl ? 32u : 32u)) return INTERP_UNDEFINED;
  const uint32_t start = u ? base : base - words * 4u;
  uint32_t buffer[64];
  if (load) {
    if (!a32_read(a, start, buffer, regs * (dbl ? 8u : 4u))) return INTERP_FAULT;
    for (uint32_t i = 0; i < regs; i++) {
      uint64_t v = buffer[dbl ? 2u * i : i];
      if (dbl) v |= (uint64_t)buffer[2u * i + 1u] << 32;
      set_fp(a, first + i, dbl, v);
    }
  } else {
    for (uint32_t i = 0; i < regs; i++) {
      const uint64_t v = get_fp(a, first + i, dbl);
      if (dbl) {
        buffer[2u * i] = (uint32_t)v;
        buffer[2u * i + 1u] = (uint32_t)(v >> 32);
      } else {
        buffer[i] = (uint32_t)v;
      }
    }
    if (!a32_write(a, start, buffer, regs * (dbl ? 8u : 4u))) return INTERP_FAULT;
  }
  if (w) a32_set_reg(a, rn, u ? base + words * 4u : base - words * 4u);
  return INTERP_CONTINUE;
}

/* VMOV between two core registers and two singles / one double. */
static Interp_Status two_register_move(A32_State *a, uint32_t insn) {
  const uint32_t load = b1(insn, 20), rt2 = f(insn, 19, 16), rt = f(insn, 15, 12);
  const bool dbl = b1(insn, 8);
  if (rt == A32_PC || rt2 == A32_PC) return INTERP_UNDEFINED;
  if (dbl) {
    const uint32_t m = vm(insn, true);
    if (load) {
      const uint64_t v = a32_d(a, m);
      a32_set_reg(a, rt, (uint32_t)v);
      a32_set_reg(a, rt2, (uint32_t)(v >> 32));
    } else {
      a32_set_d(a, m, ((uint64_t)a32_reg(a, rt2) << 32) | a32_reg(a, rt));
    }
    return INTERP_CONTINUE;
  }
  const uint32_t m = vm(insn, false);
  if (m == 31u) return INTERP_UNDEFINED;
  if (load) {
    a32_set_reg(a, rt, a32_s(a, m));
    a32_set_reg(a, rt2, a32_s(a, m + 1u));
  } else {
    a32_set_s(a, m, a32_reg(a, rt));
    a32_set_s(a, m + 1u, a32_reg(a, rt2));
  }
  return INTERP_CONTINUE;
}

/* ---- register transfers (bit 4 set) --------------------------------- */

static Interp_Status transfer(A32_State *a, uint32_t insn) {
  const uint32_t opc1 = f(insn, 23, 21), load = b1(insn, 20), rt = f(insn, 15, 12);
  if (!b1(insn, 8)) {
    if (opc1 == 0u) { /* VMOV Sn <-> Rt */
      const uint32_t n = vn(insn, false);
      if (load) a32_set_reg(a, rt, a32_s(a, n));
      else a32_set_s(a, n, a32_reg(a, rt));
      return INTERP_CONTINUE;
    }
    if (opc1 == 7u) { /* VMRS / VMSR */
      const uint32_t reg = f(insn, 19, 16);
      if (load) {
        uint32_t v;
        if (reg == 1u) v = a32_fpscr(a);
        else if (reg == 0u) v = 0x41023010u; /* FPSID: an Arm VFPv4-class unit */
        else if (reg == 7u) v = 0x00000001u; /* MVFR0-ish: present */
        else if (reg == 6u) v = 0x00000000u;
        else if (reg == 5u) v = 0x00000000u;
        else return INTERP_UNDEFINED;
        if (rt == A32_PC) { /* VMRS APSR_nzcv, FPSCR */
          a->s.regs.pstate = a32_fpscr(a) & CPU_PSTATE_NZCV_MASK;
          return INTERP_CONTINUE;
        }
        a32_set_reg(a, rt, v);
        return INTERP_CONTINUE;
      }
      if (reg != 1u) return INTERP_UNDEFINED;
      a32_set_fpscr(a, a32_reg(a, rt));
      return INTERP_CONTINUE;
    }
    return INTERP_UNDEFINED;
  }
  /* Bit 8 set: the scalar forms. */
  const uint32_t d = (b1(insn, 7) << 4) | f(insn, 19, 16);
  if (!load && b1(insn, 23)) { /* VDUP (core register) to D or Q */
    const uint32_t be = (b1(insn, 22) << 1) | b1(insn, 5), q = b1(insn, 21);
    uint32_t v = a32_reg(a, rt);
    uint64_t lanes;
    if (be == 0u) lanes = (uint64_t)v | ((uint64_t)v << 32);
    else if (be == 1u) { v &= 0xFFFFu; v |= v << 16; lanes = (uint64_t)v | ((uint64_t)v << 32); }
    else if (be == 2u) { v &= 0xFFu; v |= v << 8; v |= v << 16; lanes = (uint64_t)v | ((uint64_t)v << 32); }
    else return INTERP_UNDEFINED;
    if (q && (d & 1u)) return INTERP_UNDEFINED;
    a32_set_d(a, d, lanes);
    if (q) a32_set_d(a, d + 1u, lanes);
    return INTERP_CONTINUE;
  }
  /* VMOV Dd[x] <-> Rt: opc1 23:21 + opc2 6:5 give size and index. */
  const uint32_t o = (f(insn, 23, 21) << 2) | f(insn, 6, 5);
  uint32_t size, index;
  bool sign = false;
  if (!load) {
    if ((o & 0x8u) == 0x8u) { size = 8u; index = o & 7u; }     /* 1xxx */
    else if ((o & 0x9u) == 0x1u) { size = 16u; index = (o >> 1) & 3u; } /* 0xx1 */
    else if ((o & 0xBu) == 0x0u) { size = 32u; index = (o >> 2) & 1u; } /* 0x00 */
    else return INTERP_UNDEFINED;
  } else {
    sign = !b1(insn, 23);
    const uint32_t oo = o & 0xFu;
    if ((oo & 0x8u) == 0x8u) { size = 8u; index = oo & 7u; }
    else if ((oo & 0x9u) == 0x1u) { size = 16u; index = (oo >> 1) & 3u; }
    else if ((oo & 0xBu) == 0x0u) { size = 32u; index = (oo >> 2) & 1u; sign = false; }
    else return INTERP_UNDEFINED;
  }
  const uint32_t shift = index * size;
  const uint64_t mask = (size == 32u ? 0xFFFFFFFFull : ((1ull << size) - 1u)) << shift;
  if (load) {
    uint32_t v = (uint32_t)((a32_d(a, d) & mask) >> shift);
    if (sign && size == 8u) v = (uint32_t)(int32_t)(int8_t)v;
    if (sign && size == 16u) v = (uint32_t)(int32_t)(int16_t)v;
    a32_set_reg(a, rt, v);
  } else {
    a32_set_d(a, d, (a32_d(a, d) & ~mask) | (((uint64_t)a32_reg(a, rt) << shift) & mask));
  }
  return INTERP_CONTINUE;
}

/* ---- data processing (bit 4 clear) ---------------------------------- */

/* VFPExpandImm: imm8 = a:b:c:d:efgh as A64's FMOV immediate. */
static uint64_t vfp_imm(uint32_t insn, bool dbl) {
  const uint32_t imm8 = (f(insn, 19, 16) << 4) | f(insn, 3, 0);
  return fp_expand_imm8(dbl ? FP_DOUBLE : FP_SINGLE, imm8);
}

static Interp_Status data_processing(A32_State *a, uint32_t insn) {
  const bool dbl = b1(insn, 8);
  const FP_Format fmt = dbl ? FP_DOUBLE : FP_SINGLE;
  const uint32_t opc1 = (b1(insn, 23) << 2) | f(insn, 21, 20), op6 = b1(insn, 6);
  const uint32_t d = vd(insn, dbl);
  FP_Env env = env_of(a);
  if (opc1 != 7u) {
    const uint64_t n = get_fp(a, vn(insn, dbl), dbl), m = get_fp(a, vm(insn, dbl), dbl);
    uint64_t r;
    switch (opc1) {
    case 0: { /* VMLA / VMLS: d + (n*m), rounded twice */
      uint64_t p = fp_mul(fmt, n, m, &env);
      if (op6) p = fp_neg(fmt, p);
      r = fp_add(fmt, get_fp(a, d, dbl), p, &env);
      break;
    }
    case 1: { /* VNMLS (op 0): -d + n*m; VNMLA (op 1): -d - n*m */
      uint64_t p = fp_mul(fmt, n, m, &env);
      if (op6) p = fp_neg(fmt, p);
      r = fp_add(fmt, fp_neg(fmt, get_fp(a, d, dbl)), p, &env);
      break;
    }
    case 2: /* VMUL / VNMUL */
      r = fp_mul(fmt, n, m, &env);
      if (op6) r = fp_neg(fmt, r);
      break;
    case 3: /* VADD / VSUB */
      r = op6 ? fp_sub(fmt, n, m, &env) : fp_add(fmt, n, m, &env);
      break;
    case 4: /* VDIV */
      if (op6) return INTERP_UNDEFINED;
      r = fp_div(fmt, n, m, &env);
      break;
    case 5: /* VFNMS (op 0): -d + n*m; VFNMA (op 1): -d - n*m, fused */
      r = fp_mul_add(fmt, fp_neg(fmt, get_fp(a, d, dbl)), op6 ? fp_neg(fmt, n) : n, m, &env);
      break;
    default: /* 6: VFMA (op 0) / VFMS (op 1), fused */
      r = fp_mul_add(fmt, get_fp(a, d, dbl), op6 ? fp_neg(fmt, n) : n, m, &env);
      break;
    }
    set_fp(a, d, dbl, r);
    return INTERP_CONTINUE;
  }
  if (!op6) { /* VMOV (immediate) */
    set_fp(a, d, dbl, vfp_imm(insn, dbl));
    return INTERP_CONTINUE;
  }
  const uint32_t opc2 = f(insn, 19, 16), op7 = b1(insn, 7);
  switch (opc2) {
  case 0: { /* VMOV (register) / VABS */
    const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
    set_fp(a, d, dbl, op7 ? fp_abs(fmt, m) : m);
    return INTERP_CONTINUE;
  }
  case 1: { /* VNEG / VSQRT */
    const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
    set_fp(a, d, dbl, op7 ? fp_sqrt(fmt, m, &env) : fp_neg(fmt, m));
    return INTERP_CONTINUE;
  }
  case 2: case 3: { /* VCVTB / VCVTT: half <-> single (or double, ARMv8) */
    const bool to_half = opc2 == 3u, top = op7;
    if (to_half) {
      const uint64_t h = fp_convert(FP_HALF, fmt, get_fp(a, vm(insn, dbl), dbl), &env);
      const uint32_t dst = vd(insn, false), old = a32_s(a, dst);
      a32_set_s(a, dst, top ? (old & 0xFFFFu) | ((uint32_t)h << 16) : (old & 0xFFFF0000u) | ((uint32_t)h & 0xFFFFu));
    } else {
      const uint32_t src = a32_s(a, vm(insn, false));
      const uint64_t h = top ? src >> 16 : src & 0xFFFFu;
      set_fp(a, d, dbl, fp_convert(fmt, FP_HALF, h, &env));
    }
    return INTERP_CONTINUE;
  }
  case 4: case 5: { /* VCMP / VCMPE (with zero: opc2 5) */
    const uint64_t n = get_fp(a, d, dbl), m = opc2 == 5u ? 0u : get_fp(a, vm(insn, dbl), dbl);
    a->fpscr_nzcv = (fp_compare(fmt, n, m, op7, &env) & 0xFu) << 28; /* NZCV in 4 bits -> FPSCR 31:28 */
    return INTERP_CONTINUE;
  }
  case 6: { /* VRINTR (op 0: FPSCR rounding) / VRINTZ (op 1) */
    const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
    set_fp(a, d, dbl, fp_round_int(fmt, m, op7 ? FP_ROUND_ZERO : fp_env_rounding(&env), false, &env));
    return INTERP_CONTINUE;
  }
  case 7: {
    if (!op7) { /* VRINTX */
      const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
      set_fp(a, d, dbl, fp_round_int(fmt, m, fp_env_rounding(&env), true, &env));
      return INTERP_CONTINUE;
    }
    /* VCVT between single and double (sz is the source's). */
    const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
    const uint64_t r = fp_convert(dbl ? FP_SINGLE : FP_DOUBLE, fmt, m, &env);
    set_fp(a, vd(insn, !dbl), !dbl, r);
    return INTERP_CONTINUE;
  }
  case 8: { /* VCVT integer -> floating point (op 1: signed); the integer is a single register */
    const uint32_t src = a32_s(a, vm(insn, false));
    set_fp(a, d, dbl, fp_from_int(fmt, src, 0, op7, 32u, &env));
    return INTERP_CONTINUE;
  }
  case 10: case 11: case 14: case 15: { /* VCVT fixed point <-> floating point, in place */
    const bool to_fixed = opc2 >= 14u, is_unsigned = b1(insn, 16) != 0u, size32 = op7 != 0u;
    const uint32_t imm4 = f(insn, 3, 0), i = b1(insn, 5);
    const uint32_t frac = (size32 ? 32u : 16u) - ((imm4 << 1) | i);
    const uint64_t v = get_fp(a, d, dbl);
    if (to_fixed) {
      const uint64_t r = fp_to_int(fmt, v, frac, is_unsigned, size32 ? 32u : 16u, FP_ROUND_ZERO, &env);
      const uint64_t widened = size32 ? (uint32_t)r : (is_unsigned ? (uint64_t)(uint16_t)r : (uint64_t)(uint32_t)(int32_t)(int16_t)r);
      set_fp(a, d, dbl, dbl ? (uint64_t)(uint32_t)widened : widened);
    } else {
      uint64_t src = (uint32_t)v;
      if (!size32) src = is_unsigned ? (uint16_t)src : (uint64_t)(uint32_t)(int32_t)(int16_t)src;
      set_fp(a, d, dbl, fp_from_int(fmt, src, frac, !is_unsigned, 32u, &env));
    }
    return INTERP_CONTINUE;
  }
  case 12: case 13: { /* VCVTR (op 0: FPSCR rounding) / VCVT (op 1: toward zero) fp -> integer */
    const bool is_signed = opc2 == 13u;
    const uint64_t m = get_fp(a, vm(insn, dbl), dbl);
    const uint64_t r = fp_to_int(fmt, m, 0, !is_signed, 32u, op7 ? FP_ROUND_ZERO : fp_env_rounding(&env), &env);
    a32_set_s(a, vd(insn, false), (uint32_t)r);
    return INTERP_CONTINUE;
  }
  default:
    return INTERP_UNDEFINED;
  }
}

Interp_Status a32_vfp(A32_State *a, uint32_t insn) {
  const uint32_t top = f(insn, 27, 24);
  if (top == 0xCu || top == 0xDu) {
    if (f(insn, 24, 21) == 2u) return two_register_move(a, insn); /* 1100 010x */
    return load_store(a, insn);
  }
  if (top == 0xEu) return b1(insn, 4) ? transfer(a, insn) : data_processing(a, insn);
  return INTERP_UNDEFINED;
}

/* ---- the ARMv8 unconditional VFP forms (1111 1110, coprocessor 10/11) --- */

/* VSEL, VMAXNM/VMINNM, VRINTA/N/P/M, VCVTA/N/P/M (to 32-bit integers). */
Interp_Status a32_vfp_v8(A32_State *a, uint32_t insn) {
  const bool dbl = b1(insn, 8);
  const FP_Format fmt = dbl ? FP_DOUBLE : FP_SINGLE;
  FP_Env env = env_of(a);
  static const FP_Rounding RM[4] = {FP_ROUND_TIE_AWAY, FP_ROUND_NEAREST_EVEN, FP_ROUND_PLUS_INF, FP_ROUND_MINUS_INF};
  if (!b1(insn, 23)) { /* VSEL: EQ, VS, GE, GT on CPSR */
    const uint32_t nzcv = (uint32_t)(a->s.regs.pstate >> 28);
    const bool n = nzcv & 8u, z = nzcv & 4u, v = nzcv & 1u;
    bool take;
    switch (f(insn, 21, 20)) {
      case 0: take = z; break;
      case 1: take = v; break;
      case 2: take = n == v; break;
      default: take = !z && n == v; break;
    }
    if (b1(insn, 6)) return INTERP_UNDEFINED;
    set_fp(a, vd(insn, dbl), dbl, get_fp(a, take ? vn(insn, dbl) : vm(insn, dbl), dbl));
    return INTERP_CONTINUE;
  }
  if (f(insn, 21, 20) == 0u) { /* VMAXNM / VMINNM */
    const uint64_t x = get_fp(a, vn(insn, dbl), dbl), y = get_fp(a, vm(insn, dbl), dbl);
    set_fp(a, vd(insn, dbl), dbl, b1(insn, 6) ? fp_min_num(fmt, x, y, &env) : fp_max_num(fmt, x, y, &env));
    return INTERP_CONTINUE;
  }
  if (f(insn, 21, 18) == 0xEu && b1(insn, 6)) { /* VRINTA/N/P/M */
    const uint64_t x = get_fp(a, vm(insn, dbl), dbl);
    set_fp(a, vd(insn, dbl), dbl, fp_round_int(fmt, x, RM[f(insn, 17, 16)], false, &env));
    return INTERP_CONTINUE;
  }
  if (f(insn, 21, 18) == 0xFu && b1(insn, 6)) { /* VCVTA/N/P/M: to S32/U32 in Sd */
    const uint64_t x = get_fp(a, vm(insn, dbl), dbl);
    const bool is_signed = b1(insn, 7);
    a32_set_s(a, vd(insn, false), (uint32_t)fp_to_int(fmt, x, 0, !is_signed, 32u, RM[f(insn, 17, 16)], &env));
    return INTERP_CONTINUE;
  }
  return INTERP_UNDEFINED;
}
