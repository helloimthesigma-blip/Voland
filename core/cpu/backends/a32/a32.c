/**
 * CPU_BACKEND_A32: an AArch32 (A32) interpreter for 32-bit titles (ARMv8-A
 * AArch32 at EL0: ARM state; Thumb is not implemented and faults). Field
 * names follow the Arm ARM (DDI 0487, part F); this is our own decoder.
 * VFP and Advanced SIMD are in a32_vfp.c. See a32_internal.h for how the
 * AArch32 registers sit in the shared state.
 */
#include "cpu/backends/a32/a32.h"
#include "cpu/backends/a32/a32_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/assert.h"

/* ---- fields ---------------------------------------------------------- */

static inline uint32_t f(uint32_t insn, unsigned hi, unsigned lo) { return bits(insn, hi, lo); }
static inline uint32_t b1(uint32_t insn, unsigned n) { return bit(insn, n); }
static inline uint32_t ror32(uint32_t v, uint32_t n) { n &= 31u; return n ? (v >> n) | (v << (32u - n)) : v; }

#define A32_COND_AL 0xEu
#define A32_COND_UNCONDITIONAL 0xFu

/* ---- flags ------------------------------------------------------------- */

static inline uint32_t flag_n(const A32_State *a) { return (a->s.regs.pstate >> 31) & 1u; }
static inline uint32_t flag_z(const A32_State *a) { return (a->s.regs.pstate >> 30) & 1u; }
static inline uint32_t flag_c(const A32_State *a) { return (a->s.regs.pstate >> 29) & 1u; }
static inline uint32_t flag_v(const A32_State *a) { return (a->s.regs.pstate >> 28) & 1u; }

static void a32_set_nzcv(A32_State *a, uint32_t n, uint32_t z, uint32_t c, uint32_t v) {
  a->s.regs.pstate = (n << 31) | (z << 30) | (c << 29) | (v << 28);
}

static void set_nz_keep_cv(A32_State *a, uint32_t result, uint32_t c) {
  a32_set_nzcv(a, result >> 31, result == 0, c, flag_v(a));
}

static bool condition_passed(const A32_State *a, uint32_t cond) {
  bool r;
  switch (cond >> 1) {
  case 0: r = flag_z(a); break;
  case 1: r = flag_c(a); break;
  case 2: r = flag_n(a); break;
  case 3: r = flag_v(a); break;
  case 4: r = flag_c(a) && !flag_z(a); break;
  case 5: r = flag_n(a) == flag_v(a); break;
  case 6: r = flag_n(a) == flag_v(a) && !flag_z(a); break;
  default: return true;
  }
  return (cond & 1u) ? !r : r;
}

static uint32_t add_with_carry(uint32_t x, uint32_t y, uint32_t carry_in, uint32_t *c, uint32_t *v) {
  const uint64_t unsigned_sum = (uint64_t)x + y + carry_in;
  const int64_t signed_sum = (int64_t)(int32_t)x + (int32_t)y + carry_in;
  const uint32_t result = (uint32_t)unsigned_sum;
  *c = unsigned_sum != result;
  *v = signed_sum != (int32_t)result;
  return result;
}

/* ---- the shifter --------------------------------------------------------- */

enum { SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR };

/* Shift by an amount from a register (0-255), with the carry out. */
static uint32_t shift_c(uint32_t value, uint32_t type, uint32_t amount, uint32_t carry_in, uint32_t *carry) {
  *carry = carry_in;
  if (amount == 0) return value;
  switch (type) {
  case SHIFT_LSL:
    if (amount < 32u) { *carry = (value >> (32u - amount)) & 1u; return value << amount; }
    *carry = amount == 32u ? value & 1u : 0u;
    return 0;
  case SHIFT_LSR:
    if (amount < 32u) { *carry = (value >> (amount - 1u)) & 1u; return value >> amount; }
    *carry = amount == 32u ? value >> 31 : 0u;
    return 0;
  case SHIFT_ASR:
    if (amount < 32u) { *carry = (value >> (amount - 1u)) & 1u; return (uint32_t)((int32_t)value >> amount); }
    *carry = value >> 31;
    return (value >> 31) ? 0xFFFFFFFFu : 0u;
  default: { /* ROR */
    const uint32_t r = ror32(value, amount);
    *carry = r >> 31;
    return r;
  }
  }
}

/* An immediate shift (imm5 encoding: LSR/ASR #0 mean #32, ROR #0 is RRX). */
static uint32_t imm_shift_c(uint32_t value, uint32_t type, uint32_t imm5, uint32_t carry_in, uint32_t *carry) {
  if (imm5 == 0) {
    switch (type) {
    case SHIFT_LSL: *carry = carry_in; return value;
    case SHIFT_LSR: return shift_c(value, SHIFT_LSR, 32u, carry_in, carry);
    case SHIFT_ASR: return shift_c(value, SHIFT_ASR, 32u, carry_in, carry);
    default: /* RRX */
      *carry = value & 1u;
      return (carry_in << 31) | (value >> 1);
    }
  }
  return shift_c(value, type, imm5, carry_in, carry);
}

/* ARMExpandImm_C: an 8-bit value rotated right by twice the 4-bit field. */
static uint32_t expand_imm_c(uint32_t imm12, uint32_t carry_in, uint32_t *carry) {
  const uint32_t rot = 2u * f(imm12, 11, 8);
  const uint32_t v = ror32(imm12 & 0xFFu, rot);
  *carry = rot ? v >> 31 : carry_in;
  return v;
}

/* ---- branches and interworking ----------------------------------------- */

/* BXWritePC: bit 0 set would enter Thumb state, which is not implemented. */
static Interp_Status bx_write_pc(A32_State *a, uint32_t target) {
  if (target & 1u) return INTERP_UNDEFINED; /* Thumb */
  a32_set_reg(a, A32_PC, target & ~3u);
  return INTERP_CONTINUE;
}

/* ---- data processing ----------------------------------------------------- */

enum {
  DP_AND, DP_EOR, DP_SUB, DP_RSB, DP_ADD, DP_ADC, DP_SBC, DP_RSC,
  DP_TST, DP_TEQ, DP_CMP, DP_CMN, DP_ORR, DP_MOV, DP_BIC, DP_MVN,
};

static Interp_Status data_processing(A32_State *a, uint32_t insn, uint32_t op2, uint32_t shifter_carry) {
  const uint32_t opcode = f(insn, 24, 21), s = b1(insn, 20), rn = f(insn, 19, 16), rd = f(insn, 15, 12);
  const uint32_t n = a32_reg(a, rn);
  uint32_t result = 0, c = shifter_carry, v = flag_v(a);
  bool logical = false, writes = true;
  switch (opcode) {
  case DP_AND: result = n & op2; logical = true; break;
  case DP_EOR: result = n ^ op2; logical = true; break;
  case DP_SUB: result = add_with_carry(n, ~op2, 1u, &c, &v); break;
  case DP_RSB: result = add_with_carry(~n, op2, 1u, &c, &v); break;
  case DP_ADD: result = add_with_carry(n, op2, 0u, &c, &v); break;
  case DP_ADC: result = add_with_carry(n, op2, flag_c(a), &c, &v); break;
  case DP_SBC: result = add_with_carry(n, ~op2, flag_c(a), &c, &v); break;
  case DP_RSC: result = add_with_carry(~n, op2, flag_c(a), &c, &v); break;
  case DP_TST: result = n & op2; logical = true; writes = false; break;
  case DP_TEQ: result = n ^ op2; logical = true; writes = false; break;
  case DP_CMP: result = add_with_carry(n, ~op2, 1u, &c, &v); writes = false; break;
  case DP_CMN: result = add_with_carry(n, op2, 0u, &c, &v); writes = false; break;
  case DP_ORR: result = n | op2; logical = true; break;
  case DP_MOV: result = op2; logical = true; break;
  case DP_BIC: result = n & ~op2; logical = true; break;
  default: result = ~op2; logical = true; break; /* MVN */
  }
  if (writes) {
    if (rd == A32_PC) {
      if (s) return INTERP_UNDEFINED; /* SUBS PC, LR & co: exception return, not at EL0 */
      return bx_write_pc(a, result);    /* ALUWritePC (ARMv7+: interworking) */
    }
    a32_set_reg(a, rd, result);
  }
  if (s || !writes) {
    if (logical) a32_set_nzcv(a, result >> 31, result == 0, c, flag_v(a));
    else a32_set_nzcv(a, result >> 31, result == 0, c, v);
  }
  return INTERP_CONTINUE;
}

/* ---- multiplies ------------------------------------------------------------ */

static Interp_Status multiply(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 23, 21), s = b1(insn, 20);
  const uint32_t rd_hi = f(insn, 19, 16), rd_lo = f(insn, 15, 12), rm = f(insn, 11, 8), rn = f(insn, 3, 0);
  const uint32_t x = a32_reg(a, rn), y = a32_reg(a, rm);
  switch (op) {
  case 0: case 1: { /* MUL, MLA (rd = 19:16, ra = 15:12) */
    const uint32_t r = x * y + (op == 1u ? a32_reg(a, rd_lo) : 0u);
    a32_set_reg(a, rd_hi, r);
    if (s) set_nz_keep_cv(a, r, flag_c(a));
    return INTERP_CONTINUE;
  }
  case 2: { /* UMAAL */
    if (s) return INTERP_UNDEFINED;
    const uint64_t r = (uint64_t)x * y + a32_reg(a, rd_hi) + a32_reg(a, rd_lo);
    a32_set_reg(a, rd_lo, (uint32_t)r);
    a32_set_reg(a, rd_hi, (uint32_t)(r >> 32));
    return INTERP_CONTINUE;
  }
  case 3: { /* MLS */
    if (s) return INTERP_UNDEFINED;
    a32_set_reg(a, rd_hi, a32_reg(a, rd_lo) - x * y);
    return INTERP_CONTINUE;
  }
  default: { /* UMULL, UMLAL, SMULL, SMLAL */
    const bool is_signed = op >= 6u, accumulate = op & 1u;
    uint64_t r = is_signed ? (uint64_t)((int64_t)(int32_t)x * (int32_t)y) : (uint64_t)x * y;
    if (accumulate) r += ((uint64_t)a32_reg(a, rd_hi) << 32) | a32_reg(a, rd_lo);
    a32_set_reg(a, rd_lo, (uint32_t)r);
    a32_set_reg(a, rd_hi, (uint32_t)(r >> 32));
    if (s) a32_set_nzcv(a, (uint32_t)(r >> 63), r == 0, flag_c(a), flag_v(a));
    return INTERP_CONTINUE;
  }
  }
}

static uint32_t sat_q(A32_State *a, int64_t v) {
  if (v > INT32_MAX) { a->q = 1; return (uint32_t)INT32_MAX; }
  if (v < INT32_MIN) { a->q = 1; return (uint32_t)INT32_MIN; }
  return (uint32_t)(int32_t)v;
}

/* SMLA<x><y>, SMLAW<y>, SMULW<y>, SMLAL<x><y>, SMUL<x><y> (bits 7 and
 * 4 = 1 and 0, 24:23 = 10, 20 = 0). */
static Interp_Status halfword_multiply(A32_State *a, uint32_t insn) {
  const uint32_t op1 = f(insn, 22, 21), rd = f(insn, 19, 16), ra = f(insn, 15, 12), rm = f(insn, 11, 8),
                 rn = f(insn, 3, 0);
  const uint32_t m_high = b1(insn, 6), n_high = b1(insn, 5);
  const int32_t n = (int32_t)a32_reg(a, rn), m = (int32_t)a32_reg(a, rm);
  const int32_t m16 = (int16_t)(m_high ? (uint32_t)m >> 16 : (uint32_t)m);
  const int32_t n16 = (int16_t)(n_high ? (uint32_t)n >> 16 : (uint32_t)n);
  switch (op1) {
  case 0: { /* SMLA<x><y> */
    const int64_t r = (int64_t)n16 * m16 + (int32_t)a32_reg(a, ra);
    if (r != (int32_t)r) a->q = 1;
    a32_set_reg(a, rd, (uint32_t)r);
    return INTERP_CONTINUE;
  }
  case 1: { /* SMLAW<y> (bit 5 = 0) / SMULW<y> (bit 5 = 1) */
    const int64_t product = ((int64_t)n * m16) >> 16;
    if (n_high) {
      a32_set_reg(a, rd, (uint32_t)product);
    } else {
      const int64_t r = product + (int32_t)a32_reg(a, ra);
      if (r != (int32_t)r) a->q = 1;
      a32_set_reg(a, rd, (uint32_t)r);
    }
    return INTERP_CONTINUE;
  }
  case 2: { /* SMLAL<x><y>: RdLo = 15:12, RdHi = 19:16 */
    int64_t acc = (int64_t)(((uint64_t)a32_reg(a, rd) << 32) | a32_reg(a, ra));
    acc += (int64_t)n16 * m16;
    a32_set_reg(a, ra, (uint32_t)acc);
    a32_set_reg(a, rd, (uint32_t)((uint64_t)acc >> 32));
    return INTERP_CONTINUE;
  }
  default: /* SMUL<x><y> */
    a32_set_reg(a, rd, (uint32_t)(n16 * m16));
    return INTERP_CONTINUE;
  }
}

/* QADD, QSUB, QDADD, QDSUB. */
static Interp_Status saturating_add(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 22, 21), rn = f(insn, 19, 16), rd = f(insn, 15, 12), rm = f(insn, 3, 0);
  const int64_t m = (int32_t)a32_reg(a, rm);
  int64_t n = (int32_t)a32_reg(a, rn);
  if (op & 2u) n = (int32_t)sat_q(a, 2 * n);
  a32_set_reg(a, rd, sat_q(a, (op & 1u) ? m - n : m + n));
  return INTERP_CONTINUE;
}

/* ---- loads and stores -------------------------------------------------- */

/* Base update and the address an access uses. */
typedef struct Addressing {
  uint32_t address;  /* accessed */
  uint32_t writeback; /* the base afterwards */
  bool write_base;
} Addressing;

static Addressing address_mode(A32_State *a, uint32_t insn, uint32_t offset) {
  const uint32_t p = b1(insn, 24), u = b1(insn, 23), w = b1(insn, 21), rn = f(insn, 19, 16);
  const uint32_t base = rn == A32_PC ? (uint32_t)(a->s.regs.pc + A32_PC_READ_OFFSET) & ~3u : a32_reg(a, rn);
  const uint32_t offset_address = u ? base + offset : base - offset;
  Addressing m;
  m.address = p ? offset_address : base;
  m.writeback = offset_address;
  m.write_base = !p || w;
  return m;
}

/* LDR/STR/LDRB/STRB (immediate and register offset). */
static Interp_Status load_store_word_byte(A32_State *a, uint32_t insn) {
  const uint32_t reg_form = b1(insn, 25), byte = b1(insn, 22), load = b1(insn, 20);
  const uint32_t rn = f(insn, 19, 16), rt = f(insn, 15, 12);
  if (!b1(insn, 24) && b1(insn, 21)) return INTERP_UNDEFINED; /* LDRT/STRT & co: unprivileged forms */
  uint32_t offset;
  if (reg_form) {
    if (b1(insn, 4)) return INTERP_UNDEFINED; /* media space, decoded elsewhere */
    uint32_t carry;
    offset = imm_shift_c(a32_reg(a, f(insn, 3, 0)), f(insn, 6, 5), f(insn, 11, 7), flag_c(a), &carry);
  } else {
    offset = f(insn, 11, 0);
  }
  const Addressing m = address_mode(a, insn, offset);
  if (load) {
    uint32_t v = 0;
    if (!a32_read(a, m.address, &v, byte ? 1u : 4u)) return INTERP_FAULT;
    if (byte) v &= 0xFFu;
    if (m.write_base && rn != rt) a32_set_reg(a, rn, m.writeback);
    if (rt == A32_PC) return bx_write_pc(a, v); /* LoadWritePC */
    a32_set_reg(a, rt, v);
  } else {
    const uint32_t v = a32_reg(a, rt);
    if (!a32_write(a, m.address, &v, byte ? 1u : 4u)) return INTERP_FAULT;
    if (m.write_base) a32_set_reg(a, rn, m.writeback);
  }
  return INTERP_CONTINUE;
}

/* LDRH/STRH, LDRSB/LDRSH, LDRD/STRD (bit 7 and bit 4 set, bits 6:5 != 0). */
static Interp_Status load_store_extra(A32_State *a, uint32_t insn) {
  const uint32_t imm_form = b1(insn, 22), load = b1(insn, 20), op2 = f(insn, 6, 5);
  const uint32_t rn = f(insn, 19, 16), rt = f(insn, 15, 12);
  const uint32_t offset = imm_form ? (f(insn, 11, 8) << 4) | f(insn, 3, 0) : a32_reg(a, f(insn, 3, 0));
  if (!b1(insn, 24) && b1(insn, 21)) return INTERP_UNDEFINED; /* LDRHT & co */
  const Addressing m = address_mode(a, insn, offset);
  if (!load && op2 != 1u) { /* LDRD (op2 2, L 0) / STRD (op2 3, L 0) */
    if (rt & 1u) return INTERP_UNDEFINED;
    if (op2 == 2u) {
      uint32_t v[2];
      if (!a32_read(a, m.address, v, 8u)) return INTERP_FAULT;
      if (m.write_base) a32_set_reg(a, rn, m.writeback);
      a32_set_reg(a, rt, v[0]);
      a32_set_reg(a, rt + 1u, v[1]);
    } else {
      const uint32_t v[2] = {a32_reg(a, rt), a32_reg(a, rt + 1u)};
      if (!a32_write(a, m.address, v, 8u)) return INTERP_FAULT;
      if (m.write_base) a32_set_reg(a, rn, m.writeback);
    }
    return INTERP_CONTINUE;
  }
  if (!load) { /* STRH */
    const uint16_t v = (uint16_t)a32_reg(a, rt);
    if (!a32_write(a, m.address, &v, 2u)) return INTERP_FAULT;
    if (m.write_base) a32_set_reg(a, rn, m.writeback);
    return INTERP_CONTINUE;
  }
  uint32_t v = 0;
  switch (op2) {
  case 1: { uint16_t h; if (!a32_read(a, m.address, &h, 2u)) return INTERP_FAULT; v = h; break; } /* LDRH */
  case 2: { int8_t b; if (!a32_read(a, m.address, &b, 1u)) return INTERP_FAULT; v = (uint32_t)(int32_t)b; break; }
  default: { int16_t h; if (!a32_read(a, m.address, &h, 2u)) return INTERP_FAULT; v = (uint32_t)(int32_t)h; break; }
  }
  if (m.write_base && rn != rt) a32_set_reg(a, rn, m.writeback);
  a32_set_reg(a, rt, v);
  return INTERP_CONTINUE;
}

/* LDREX/STREX (word, doubleword, byte, halfword) and the ARMv8 acquire /
 * release forms LDA/STL, LDAEX/STLEX. The monitor is the A64
 * interpreter's (one address per thread, cleared at run() entry). */
static Interp_Status synchronization(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 22, 21), load = b1(insn, 20), rn = f(insn, 19, 16), rd = f(insn, 15, 12),
                 rt = f(insn, 3, 0);
  const uint32_t ordered = f(insn, 9, 8); /* 3: LDREX/STREX; 2: LDAEX/STLEX; 0: LDA/STL */
  static const uint32_t k_size[4] = {4u, 8u, 1u, 2u};
  const uint32_t size = k_size[op];
  const uint32_t address = a32_reg(a, rn);
  if (ordered == 0u) { /* LDA/STL: not exclusive */
    if (op == 1u) return INTERP_UNDEFINED;
    if (load) {
      uint32_t v = 0;
      if (!a32_read(a, address, &v, size)) return INTERP_FAULT;
      a32_set_reg(a, rd, v);
    } else {
      const uint32_t v = a32_reg(a, rt);
      if (!a32_write(a, address, &v, size)) return INTERP_FAULT;
    }
    return INTERP_CONTINUE;
  }
  if (load) {
    uint32_t v[2] = {0, 0};
    if (!a32_read(a, address, v, size)) return INTERP_FAULT;
    a->s.exclusive_valid = true;
    a->s.exclusive_address = address;
    a->s.exclusive_size = size;
    a32_set_reg(a, rd, v[0]);
    if (size == 8u) a32_set_reg(a, rd + 1u, v[1]);
    return INTERP_CONTINUE;
  }
  /* STREX: Rd = 0 on success, 1 when the monitor was lost. */
  if (a->s.exclusive_valid && a->s.exclusive_address == address && a->s.exclusive_size == size) {
    const uint32_t v[2] = {a32_reg(a, rt), size == 8u ? a32_reg(a, rt + 1u) : 0u};
    if (!a32_write(a, address, v, size)) return INTERP_FAULT;
    a32_set_reg(a, rd, 0);
  } else {
    a32_set_reg(a, rd, 1);
  }
  a->s.exclusive_valid = false;
  return INTERP_CONTINUE;
}

/* LDM/STM (IA, IB, DA, DB), including PUSH/POP and loads of PC. */
static Interp_Status block_transfer(A32_State *a, uint32_t insn) {
  const uint32_t p = b1(insn, 24), u = b1(insn, 23), user = b1(insn, 22), w = b1(insn, 21), load = b1(insn, 20);
  const uint32_t rn = f(insn, 19, 16), list = f(insn, 15, 0);
  if (user || !list || rn == A32_PC) return INTERP_UNDEFINED;
  const uint32_t count = (uint32_t)__builtin_popcount(list);
  const uint32_t base = a32_reg(a, rn);
  uint32_t start = u ? base : base - 4u * count;
  if (p == u) start += 4u; /* IB, DB */
  const uint32_t new_base = u ? base + 4u * count : base - 4u * count;
  uint32_t values[16];
  if (load) {
    if (!a32_read(a, start, values, 4u * count)) return INTERP_FAULT;
    uint32_t k = 0;
    uint32_t pc_value = 0;
    bool pc = false;
    for (uint32_t r = 0; r < 16u; r++) {
      if (!((list >> r) & 1u)) continue;
      if (r == A32_PC) {
        pc = true;
        pc_value = values[k++];
      } else {
        a32_set_reg(a, r, values[k++]);
      }
    }
    if (w && !((list >> rn) & 1u)) a32_set_reg(a, rn, new_base);
    if (pc) return bx_write_pc(a, pc_value);
    return INTERP_CONTINUE;
  }
  uint32_t k = 0;
  for (uint32_t r = 0; r < 16u; r++)
    if ((list >> r) & 1u) values[k++] = r == A32_PC ? (uint32_t)a->s.regs.pc + A32_PC_READ_OFFSET : a32_reg(a, r);
  if (!a32_write(a, start, values, 4u * count)) return INTERP_FAULT;
  if (w) a32_set_reg(a, rn, new_base);
  return INTERP_CONTINUE;
}

/* ---- media instructions ---------------------------------------------- */

static uint32_t rev16(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }

/* Signed and unsigned saturation to `bits` (SSAT: 1-32 bits signed, USAT: 0-31 unsigned). */
static uint32_t saturate(A32_State *a, int64_t v, uint32_t bits_, bool is_unsigned) {
  if (is_unsigned) {
    const int64_t max = bits_ >= 32u ? (int64_t)0xFFFFFFFF : (((int64_t)1 << bits_) - 1);
    if (v < 0) { a->q = 1; return 0; }
    if (v > max) { a->q = 1; return (uint32_t)max; }
    return (uint32_t)v;
  }
  const int64_t max = ((int64_t)1 << (bits_ - 1u)) - 1, min = -((int64_t)1 << (bits_ - 1u));
  if (v > max) { a->q = 1; return (uint32_t)max; }
  if (v < min) { a->q = 1; return (uint32_t)min; }
  return (uint32_t)v;
}

/* Parallel add/subtract (UADD8, SSUB16, UQADD8, SHADD16, ... ):
 * op1 (22:20) picks signed/saturating/halving, op2 (7:5) the lanes. */
static Interp_Status parallel_add_sub(A32_State *a, uint32_t insn) {
  const uint32_t op1 = f(insn, 22, 20), op2 = f(insn, 7, 5), rn = f(insn, 19, 16), rd = f(insn, 15, 12),
                 rm = f(insn, 3, 0);
  const uint32_t n = a32_reg(a, rn), m = a32_reg(a, rm);
  const bool is_signed = op1 < 4u;
  const uint32_t kind = op1 & 3u; /* 1 plain (with GE), 2 saturating, 3 halving */
  if (kind == 0u || op2 == 5u || op2 == 6u) return INTERP_UNDEFINED;
  const bool bytes = op2 >= 4u;
  const uint32_t lanes = bytes ? 4u : 2u, width = bytes ? 8u : 16u;
  uint32_t result = 0, ge = 0;
  for (uint32_t i = 0; i < lanes; i++) {
    const uint32_t shift = i * width, mask = (1u << width) - 1u;
    int64_t x = (n >> shift) & mask, y = (m >> shift) & mask;
    if (is_signed) {
      x = (int64_t)((int32_t)(x << (32u - width)) >> (32u - width));
      y = (int64_t)((int32_t)(y << (32u - width)) >> (32u - width));
    }
    /* ASX/SAX (op2 1/2) pair lane 0 with lane 1 of the other operand. */
    int64_t r;
    bool add;
    if (op2 == 1u || op2 == 2u) {
      const uint32_t other = shift == 0 ? 16u : 0u;
      int64_t y2 = (m >> other) & 0xFFFFu;
      if (is_signed) y2 = (int16_t)y2;
      add = (op2 == 1u) == (i == 1u); /* ASX: lane 1 adds, lane 0 subtracts; SAX the reverse */
      y = y2;
    } else {
      add = op2 == 0u || op2 == 4u;
    }
    r = add ? x + y : x - y;
    uint32_t lane;
    if (kind == 2u) { /* saturating */
      const int64_t max = is_signed ? ((int64_t)1 << (width - 1u)) - 1 : ((int64_t)1 << width) - 1;
      const int64_t min = is_signed ? -((int64_t)1 << (width - 1u)) : 0;
      lane = (uint32_t)(r > max ? max : r < min ? min : r) & mask;
    } else if (kind == 3u) { /* halving */
      lane = (uint32_t)(r >> 1) & mask;
    } else {
      lane = (uint32_t)r & mask;
      const bool set = is_signed ? r >= 0 : (add ? r >= ((int64_t)1 << width) : r >= 0);
      if (set) ge |= (bytes ? 1u : 3u) << (bytes ? i : 2u * i);
    }
    result |= lane << shift;
  }
  if (kind == 1u) a->ge = ge;
  a32_set_reg(a, rd, result);
  return INTERP_CONTINUE;
}

/* The media space: bits 27:25 = 011 with bit 4 set. */
static Interp_Status media(A32_State *a, uint32_t insn) {
  const uint32_t op1 = f(insn, 24, 20), op2 = f(insn, 7, 5);
  const uint32_t rn = f(insn, 19, 16), rd = f(insn, 15, 12), rm = f(insn, 3, 0);
  if ((op1 & 0x18u) == 0x00u) return parallel_add_sub(a, insn);
  if ((op1 & 0x18u) == 0x08u) {
    /* Packing, unpacking, saturation and reversal. */
    if (op1 == 0x08u && (op2 & 1u) == 0u) { /* PKHBT / PKHTB */
      const uint32_t imm5 = f(insn, 11, 7), tb = b1(insn, 6);
      uint32_t carry;
      const uint32_t shifted = imm_shift_c(a32_reg(a, rm), tb ? SHIFT_ASR : SHIFT_LSL, imm5, 0u, &carry);
      const uint32_t n = a32_reg(a, rn);
      a32_set_reg(a, rd, tb ? (n & 0xFFFF0000u) | (shifted & 0xFFFFu) : (shifted & 0xFFFF0000u) | (n & 0xFFFFu));
      return INTERP_CONTINUE;
    }
    if ((op1 & 0x02u) && (op2 & 1u) == 0u) { /* SSAT / USAT */
      const bool is_unsigned = b1(insn, 22);
      const uint32_t sat = f(insn, 20, 16), imm5 = f(insn, 11, 7), sh = b1(insn, 6);
      uint32_t carry;
      const uint32_t operand = imm_shift_c(a32_reg(a, rm), sh ? SHIFT_ASR : SHIFT_LSL, imm5, 0u, &carry);
      a32_set_reg(a, rd, saturate(a, (int32_t)operand, is_unsigned ? sat : sat + 1u, is_unsigned));
      return INTERP_CONTINUE;
    }
    if ((op1 == 0x0Au || op1 == 0x0Eu) && op2 == 1u) { /* SSAT16 / USAT16 */
      const bool is_unsigned = op1 == 0x0Eu;
      const uint32_t sat = f(insn, 19, 16), v = a32_reg(a, rm);
      const uint32_t lo = saturate(a, (int16_t)v, is_unsigned ? sat : sat + 1u, is_unsigned) & 0xFFFFu;
      const uint32_t hi = saturate(a, (int16_t)(v >> 16), is_unsigned ? sat : sat + 1u, is_unsigned) & 0xFFFFu;
      a32_set_reg(a, rd, lo | (hi << 16));
      return INTERP_CONTINUE;
    }
    if (op2 == 3u) { /* SXTAB16, SXTAB, SXTAH, UXTAB16, UXTAB, UXTAH (and the Rn = 15 forms) */
      const uint32_t rot = 8u * f(insn, 11, 10);
      const uint32_t v = ror32(a32_reg(a, rm), rot);
      const uint32_t add = rn == A32_PC ? 0u : a32_reg(a, rn);
      uint32_t r;
      switch (op1) {
      case 0x08u: r = (((add & 0xFFFFu) + (uint32_t)(int32_t)(int8_t)v) & 0xFFFFu) |
                     ((((add >> 16) + (uint32_t)(int32_t)(int8_t)(v >> 16)) & 0xFFFFu) << 16); break;
      case 0x0Au: r = add + (uint32_t)(int32_t)(int8_t)v; break;
      case 0x0Bu: r = add + (uint32_t)(int32_t)(int16_t)v; break;
      case 0x0Cu: r = (((add & 0xFFFFu) + (v & 0xFFu)) & 0xFFFFu) | ((((add >> 16) + ((v >> 16) & 0xFFu)) & 0xFFFFu) << 16); break;
      case 0x0Eu: r = add + (v & 0xFFu); break;
      case 0x0Fu: r = add + (v & 0xFFFFu); break;
      default: return INTERP_UNDEFINED;
      }
      a32_set_reg(a, rd, r);
      return INTERP_CONTINUE;
    }
    if (op1 == 0x08u && op2 == 5u) { /* SEL */
      const uint32_t n = a32_reg(a, rn), m = a32_reg(a, rm);
      uint32_t r = 0;
      for (uint32_t i = 0; i < 4u; i++) r |= (((a->ge >> i) & 1u) ? n : m) & (0xFFu << (8u * i));
      a32_set_reg(a, rd, r);
      return INTERP_CONTINUE;
    }
    const uint32_t m = a32_reg(a, rm);
    if (op1 == 0x0Bu && op2 == 1u) { a32_set_reg(a, rd, __builtin_bswap32(m)); return INTERP_CONTINUE; } /* REV */
    if (op1 == 0x0Bu && op2 == 5u) { a32_set_reg(a, rd, rev16(m)); return INTERP_CONTINUE; }             /* REV16 */
    if (op1 == 0x0Fu && op2 == 1u) {                                                                      /* RBIT */
      uint32_t r = 0;
      for (uint32_t i = 0; i < 32u; i++) r |= ((m >> i) & 1u) << (31u - i);
      a32_set_reg(a, rd, r);
      return INTERP_CONTINUE;
    }
    if (op1 == 0x0Fu && op2 == 5u) { /* REVSH */
      a32_set_reg(a, rd, (uint32_t)(int32_t)(int16_t)(((m & 0xFFu) << 8) | ((m >> 8) & 0xFFu)));
      return INTERP_CONTINUE;
    }
    return INTERP_UNDEFINED;
  }
  if ((op1 & 0x18u) == 0x10u) {
    /* Signed multiplies (SMLAD, SMLSD, SMUAD, SMUSD, SMLALD, SMMLA, SMMUL, ...), SDIV/UDIV. */
    const uint32_t ra = f(insn, 15, 12), rd2 = f(insn, 19, 16), rm2 = f(insn, 11, 8);
    const uint32_t n = a32_reg(a, f(insn, 3, 0)), m0 = a32_reg(a, rm2);
    if (op1 == 0x11u || op1 == 0x13u) { /* SDIV / UDIV */
      if (op2 != 0u) return INTERP_UNDEFINED;
      uint32_t r;
      if (m0 == 0) r = 0; /* Arm: division by zero gives zero (no trap at EL0) */
      else if (op1 == 0x13u) r = n / m0;
      else if ((int32_t)n == INT32_MIN && (int32_t)m0 == -1) r = (uint32_t)INT32_MIN;
      else r = (uint32_t)((int32_t)n / (int32_t)m0);
      a32_set_reg(a, rd2, r);
      return INTERP_CONTINUE;
    }
    const uint32_t m = b1(insn, 5) ? ror32(m0, 16) : m0; /* the X (swap) bit */
    const int64_t p1 = (int64_t)(int16_t)n * (int16_t)m, p2 = (int64_t)(int16_t)(n >> 16) * (int16_t)(m >> 16);
    if (op1 == 0x10u) { /* SMLAD/SMUAD (bit 6 = 0), SMLSD/SMUSD (bit 6 = 1) */
      const int64_t sum = b1(insn, 6) ? p1 - p2 : p1 + p2;
      const int64_t r = ra == A32_PC ? sum : sum + (int32_t)a32_reg(a, ra);
      if (r != (int32_t)r) a->q = 1;
      a32_set_reg(a, rd2, (uint32_t)r);
      return INTERP_CONTINUE;
    }
    if (op1 == 0x14u) { /* SMLALD / SMLSLD */
      int64_t acc = (int64_t)(((uint64_t)a32_reg(a, rd2) << 32) | a32_reg(a, ra));
      acc += b1(insn, 6) ? p1 - p2 : p1 + p2;
      a32_set_reg(a, ra, (uint32_t)acc);
      a32_set_reg(a, rd2, (uint32_t)((uint64_t)acc >> 32));
      return INTERP_CONTINUE;
    }
    if (op1 == 0x15u) { /* SMMLA, SMMLS, SMMUL (R: bit 5 rounds) */
      const int64_t product = (int64_t)(int32_t)n * (int32_t)m0;
      int64_t r;
      if ((op2 >> 1) == 3u) r = (((int64_t)(int32_t)a32_reg(a, ra) << 32) - product); /* SMMLS */
      else r = (ra == A32_PC ? 0 : ((int64_t)(int32_t)a32_reg(a, ra) << 32)) + product;
      if (b1(insn, 5)) r += 0x80000000ll;
      a32_set_reg(a, rd2, (uint32_t)((uint64_t)r >> 32));
      return INTERP_CONTINUE;
    }
    return INTERP_UNDEFINED;
  }
  /* 0x18-0x1F: USAD8/USADA8, SBFX, BFC/BFI, UBFX. */
  if (op1 == 0x18u && op2 == 0u) { /* USAD8 / USADA8 */
    const uint32_t n = a32_reg(a, f(insn, 3, 0)), m = a32_reg(a, f(insn, 11, 8)), ra = f(insn, 15, 12);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 4u; i++) {
      const int32_t d = (int32_t)((n >> (8u * i)) & 0xFFu) - (int32_t)((m >> (8u * i)) & 0xFFu);
      sum += (uint32_t)(d < 0 ? -d : d);
    }
    a32_set_reg(a, f(insn, 19, 16), ra == A32_PC ? sum : sum + a32_reg(a, ra));
    return INTERP_CONTINUE;
  }
  const uint32_t lsb = f(insn, 11, 7), msb_or_width = f(insn, 20, 16);
  if ((op1 & 0x1Eu) == 0x1Au && (op2 & 3u) == 2u) { /* SBFX */
    const uint32_t width = msb_or_width + 1u;
    if (lsb + width > 32u) return INTERP_UNDEFINED;
    const uint32_t v = a32_reg(a, rm) << (32u - lsb - width);
    a32_set_reg(a, rd, (uint32_t)((int32_t)v >> (32u - width)));
    return INTERP_CONTINUE;
  }
  if ((op1 & 0x1Eu) == 0x1Eu && (op2 & 3u) == 2u) { /* UBFX */
    const uint32_t width = msb_or_width + 1u;
    if (lsb + width > 32u) return INTERP_UNDEFINED;
    const uint32_t v = a32_reg(a, rm) >> lsb;
    a32_set_reg(a, rd, width >= 32u ? v : v & ((1u << width) - 1u));
    return INTERP_CONTINUE;
  }
  if ((op1 & 0x1Eu) == 0x1Cu && (op2 & 3u) == 0u) { /* BFC (Rn = 15) / BFI */
    const uint32_t msb = msb_or_width;
    if (msb < lsb) return INTERP_UNDEFINED;
    const uint32_t width = msb - lsb + 1u;
    const uint32_t mask = (width >= 32u ? 0xFFFFFFFFu : ((1u << width) - 1u)) << lsb;
    const uint32_t src = rm == A32_PC ? 0u : a32_reg(a, rm) << lsb;
    a32_set_reg(a, rd, (a32_reg(a, rd) & ~mask) | (src & mask));
    return INTERP_CONTINUE;
  }
  if (op1 == 0x1Fu && op2 == 7u && f(insn, 31, 28) == A32_COND_AL) return INTERP_UNDEFINED; /* UDF */
  return INTERP_UNDEFINED;
}

/* ---- the miscellaneous space (bits 27:23 = 00010, bit 20 = 0) ---------- */

static uint32_t read_cpsr(const A32_State *a) {
  return (a->s.regs.pstate & CPU_PSTATE_NZCV_MASK) | (a->q << 27) | (a->ge << 16) | 0x10u; /* USR mode */
}

static Interp_Status miscellaneous(A32_State *a, uint32_t insn) {
  const uint32_t op = f(insn, 22, 21), op2 = f(insn, 6, 4);
  switch (op2) {
  case 0: /* MRS / MSR (register) */
    if (op & 1u) { /* MSR CPSR_<fields>, Rn: only the flags (and GE) at EL0 */
      const uint32_t mask = f(insn, 19, 16), v = a32_reg(a, f(insn, 3, 0));
      if (op & 2u) return INTERP_UNDEFINED; /* SPSR */
      if (mask & 8u) {
        a->s.regs.pstate = v & CPU_PSTATE_NZCV_MASK;
        a->q = (v >> 27) & 1u;
      }
      if (mask & 4u) a->ge = (v >> 16) & 0xFu;
      return INTERP_CONTINUE;
    }
    if (op & 2u) return INTERP_UNDEFINED; /* MRS SPSR */
    a32_set_reg(a, f(insn, 15, 12), read_cpsr(a));
    return INTERP_CONTINUE;
  case 1:
    if (op == 1u) return bx_write_pc(a, a32_reg(a, f(insn, 3, 0))); /* BX */
    if (op == 3u) {                                                 /* CLZ */
      const uint32_t v = a32_reg(a, f(insn, 3, 0));
      a32_set_reg(a, f(insn, 15, 12), v ? (uint32_t)__builtin_clz(v) : 32u);
      return INTERP_CONTINUE;
    }
    return INTERP_UNDEFINED;
  case 2:
    return op == 1u ? INTERP_UNDEFINED : INTERP_UNDEFINED; /* BXJ */
  case 3:
    if (op == 1u) { /* BLX (register) */
      const uint32_t target = a32_reg(a, f(insn, 3, 0));
      a32_set_reg(a, A32_LR, (uint32_t)a->s.regs.pc + A32_INSN_BYTES);
      return bx_write_pc(a, target);
    }
    return INTERP_UNDEFINED;
  case 5:
    return saturating_add(a, insn);
  case 7:
    if (op == 1u) return INTERP_BREAKPOINT; /* BKPT */
    return INTERP_UNDEFINED;
  default:
    return INTERP_UNDEFINED;
  }
}

/* ---- coprocessor 15 (the timer and thread registers) --------------------- */

/* MRC/MCR p15: TPIDRURW (c13, c0, 2), TPIDRURO (c13, c0, 3), barriers. */
static Interp_Status cp15(A32_State *a, uint32_t insn) {
  const uint32_t load = b1(insn, 20), opc1 = f(insn, 23, 21), crn = f(insn, 19, 16), rt = f(insn, 15, 12),
                 opc2 = f(insn, 7, 5), crm = f(insn, 3, 0);
  if (opc1 == 0 && crn == 13u && crm == 0) {
    if (opc2 == 2u) {
      if (load) a32_set_reg(a, rt, (uint32_t)a->s.tpidr_el0);
      else a->s.tpidr_el0 = a32_reg(a, rt);
      return INTERP_CONTINUE;
    }
    if (opc2 == 3u && load) {
      a32_set_reg(a, rt, (uint32_t)a->s.tpidrro_el0);
      return INTERP_CONTINUE;
    }
  }
  if (!load && opc1 == 0 && crn == 7u && (crm == 10u || crm == 5u) && (opc2 == 4u || opc2 == 5u || opc2 == 4u))
    return INTERP_CONTINUE; /* CP15DSB / CP15DMB / CP15ISB: ordered already */
  if (!load && opc1 == 0 && crn == 7u && crm == 5u && (opc2 == 1u || opc2 == 6u || opc2 == 7u)) {
    interp_predecode_flush(); /* ICIMVAU / BPI: decoded code may be stale */
    return INTERP_CONTINUE;
  }
  if (!load && opc1 == 0 && crn == 7u && (crm == 10u || crm == 11u || crm == 14u))
    return INTERP_CONTINUE; /* data cache maintenance by address: no cache here */
  return INTERP_UNDEFINED;
}

/* MRRC p15: CNTVCT (opc1 1, c14) and CNTPCT (opc1 0, c14). */
static Interp_Status cp15_double(A32_State *a, uint32_t insn) {
  const uint32_t load = b1(insn, 20), rt2 = f(insn, 19, 16), rt = f(insn, 15, 12), opc1 = f(insn, 7, 4),
                 crm = f(insn, 3, 0);
  if (!load || crm != 14u || opc1 > 1u) return INTERP_UNDEFINED;
  bool known = false;
  const uint64_t t = interp_read_sys_reg(&a->s, CPU_SYSREG_CNTVCT_EL0, &known);
  a32_set_reg(a, rt, (uint32_t)t);
  a32_set_reg(a, rt2, (uint32_t)(t >> 32));
  return INTERP_CONTINUE;
}

/* ---- the unconditional space (cond = 1111) ------------------------------ */

static Interp_Status unconditional(A32_State *a, uint32_t insn) {
  if (f(insn, 27, 25) == 5u) { /* BLX (immediate): to Thumb - not implemented */
    return INTERP_UNDEFINED;
  }
  if (f(insn, 27, 25) == 1u || (f(insn, 27, 24) == 4u && b1(insn, 20) == 0u)) return a32_neon(a, insn);
  if (f(insn, 27, 20) == 0x57u) { /* CLREX, DSB, DMB, ISB */
    const uint32_t op = f(insn, 7, 4);
    if (op == 1u) a->s.exclusive_valid = false;
    return op <= 6u ? INTERP_CONTINUE : INTERP_UNDEFINED;
  }
  const uint32_t top = f(insn, 27, 24);
  if ((top == 5u || top == 7u) && f(insn, 21, 20) == 1u) return INTERP_CONTINUE; /* PLD / PLDW: hints */
  if ((top == 4u || top == 6u) && f(insn, 22, 20) == 5u) return INTERP_CONTINUE; /* PLI */
  if (f(insn, 27, 20) == 0x10u && b1(insn, 16) == 1u) return INTERP_CONTINUE; /* SETEND (LE only: CPSR.E stays 0) */
  return INTERP_UNDEFINED;
}

/* ---- the top-level decoder -------------------------------------------- */

static Interp_Status execute(A32_State *a, uint32_t insn) {
  const uint32_t cond = f(insn, 31, 28);
  if (cond == A32_COND_UNCONDITIONAL) return unconditional(a, insn);
  if (!condition_passed(a, cond)) return INTERP_CONTINUE;
  const uint32_t op1 = f(insn, 27, 25);
  switch (op1) {
  case 0: case 1: {
    const uint32_t op = f(insn, 24, 20), b7 = b1(insn, 7), b4 = b1(insn, 4);
    if (op1 == 0 && b7 && b4) { /* multiplies, extra loads/stores, synchronization */
      const uint32_t op2 = f(insn, 6, 5);
      if (op2 == 0u) {
        if ((op & 0x10u) == 0u) return multiply(a, insn);
        if ((op & 0x1Bu) == 0x10u) return INTERP_UNDEFINED; /* SWP/SWPB: deprecated, not on ARMv8 at EL0 */
        if ((op & 0x18u) == 0x18u) return synchronization(a, insn);
        return INTERP_UNDEFINED;
      }
      return load_store_extra(a, insn);
    }
    if (op1 == 1u && (op == 0x10u || op == 0x14u)) { /* MOVW / MOVT */
      const uint32_t imm16 = (f(insn, 19, 16) << 12) | f(insn, 11, 0), rd = f(insn, 15, 12);
      if (rd == A32_PC) return INTERP_UNDEFINED;
      a32_set_reg(a, rd, op == 0x10u ? imm16 : (a32_reg(a, rd) & 0xFFFFu) | (imm16 << 16));
      return INTERP_CONTINUE;
    }
    if (op1 == 1u && (op & 0x1Bu) == 0x12u) { /* MSR (immediate) and the hints (NOP, YIELD, WFE, ...) */
      if (f(insn, 19, 16) == 0) return INTERP_CONTINUE; /* hints */
      if (op & 4u) return INTERP_UNDEFINED; /* SPSR */
      uint32_t carry;
      const uint32_t v = expand_imm_c(f(insn, 11, 0), flag_c(a), &carry);
      if (b1(insn, 19)) { a->s.regs.pstate = v & CPU_PSTATE_NZCV_MASK; a->q = (v >> 27) & 1u; }
      if (b1(insn, 18)) a->ge = (v >> 16) & 0xFu;
      return INTERP_CONTINUE;
    }
    if (op1 == 0 && (op & 0x19u) == 0x10u) { /* the miscellaneous space and halfword multiplies */
      if (!b7) return miscellaneous(a, insn);
      if (!b4) return halfword_multiply(a, insn);
      return INTERP_UNDEFINED;
    }
    /* Data processing. */
    uint32_t op2, carry;
    if (op1 == 1u) {
      op2 = expand_imm_c(f(insn, 11, 0), flag_c(a), &carry);
    } else if (!b4) { /* register, immediate shift */
      op2 = imm_shift_c(a32_reg(a, f(insn, 3, 0)), f(insn, 6, 5), f(insn, 11, 7), flag_c(a), &carry);
    } else { /* register-shifted register (bit 7 = 0) */
      if (f(insn, 3, 0) == A32_PC || f(insn, 11, 8) == A32_PC) return INTERP_UNDEFINED;
      op2 = shift_c(a32_reg(a, f(insn, 3, 0)), f(insn, 6, 5), a32_reg(a, f(insn, 11, 8)) & 0xFFu, flag_c(a), &carry);
    }
    return data_processing(a, insn, op2, carry);
  }
  case 2:
    return load_store_word_byte(a, insn);
  case 3:
    if (b1(insn, 4)) return media(a, insn);
    return load_store_word_byte(a, insn);
  case 4:
    return block_transfer(a, insn);
  case 5: { /* B, BL */
    const uint32_t imm = (uint32_t)((int32_t)(insn << 8) >> 6); /* sign-extended imm24 << 2 */
    if (b1(insn, 24)) a32_set_reg(a, A32_LR, (uint32_t)a->s.regs.pc + A32_INSN_BYTES);
    a32_set_reg(a, A32_PC, (uint32_t)a->s.regs.pc + A32_PC_READ_OFFSET + imm);
    return INTERP_CONTINUE;
  }
  case 6: /* coprocessor loads/stores, MCRR/MRRC */
    if (f(insn, 11, 9) == 5u) return a32_vfp(a, insn); /* cp10/cp11 */
    if (f(insn, 24, 21) == 2u && f(insn, 11, 8) == 15u) return cp15_double(a, insn);
    return INTERP_UNDEFINED;
  default: /* 7: SVC, coprocessor data processing and register transfers */
    if (b1(insn, 24)) {
      a->s.svc_immediate = f(insn, 23, 0);
      return INTERP_SVC;
    }
    if (f(insn, 11, 9) == 5u) return a32_vfp(a, insn);
    if (b1(insn, 4) && f(insn, 11, 8) == 15u) return cp15(a, insn);
    return INTERP_UNDEFINED;
  }
}

Interp_Status a32_execute(A32_State *a, uint32_t insn) {
  a->pc_written = false;
  const uint64_t pc = a->s.regs.pc;
  const Interp_Status st = execute(a, insn);
  if (st == INTERP_CONTINUE && !a->pc_written) a->s.regs.pc = (uint32_t)(pc + A32_INSN_BYTES);
  if (st == INTERP_SVC) a->s.regs.pc = (uint32_t)(pc + A32_INSN_BYTES); /* the SVC returns after itself */
  if (st == INTERP_UNDEFINED || st == INTERP_FAULT) a->s.regs.pc = pc;
  return st;
}

/* ---- the backend -------------------------------------------------------- */

static A32_State *as_a32(CPU_State *state) { return (A32_State *)state; }

static CPU_State *a32_create(VMM_Context *vmm, void *userdata) {
  A32_State *a = (A32_State *)calloc(1, sizeof(A32_State)); /* once per thread, at creation */
  if (!a) return NULL;
  a->s.vmm = vmm;
  a->s.l1 = vmm ? vmm_page_table_l1(vmm) : NULL;
  a->s.userdata = userdata;
  return (CPU_State *)a;
}

static void a32_destroy(CPU_State *state) { free(state); }

static bool run_one(A32_State *a, CPU_ExitReason *exit_reason) {
  const uint64_t pc = a->s.regs.pc;
  uint32_t insn = 0;
  VMM_Fault fault;
  const uint8_t *host = (pc & 3u) ? NULL : vmm_translate_inline(a->s.l1, pc, VMM_PERM_X, &fault);
  if (!host) {
    a->s.fault_address = pc;
    *exit_reason = CPU_EXIT_FAULT;
    return false;
  }
  memcpy(&insn, host, sizeof(insn));
  return interp_retire(&a->s, a32_execute(a, insn), pc, insn, exit_reason);
}

static CPU_ExitReason a32_run(CPU_State *state, uint64_t cycle_budget) {
  A32_State *a = as_a32(state);
  SWITCH_ASSERT_ALWAYS(a->s.l1 != NULL, "a32 run() without a vmm");
  a->s.cycles_consumed = 0;
  a->s.exclusive_valid = false; /* a potential context switch (§7) */
  uint32_t grace = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  for (;;) {
    if (a->s.cycles_consumed >= cycle_budget) {
      if (!a->s.exclusive_valid || grace >= INTERP_EXCLUSIVE_GRACE_INSTRUCTIONS) return CPU_EXIT_CYCLES_ELAPSED;
      grace++;
    }
    if (!run_one(a, &exit_reason)) return exit_reason;
  }
}

static CPU_ExitReason a32_step(CPU_State *state) {
  A32_State *a = as_a32(state);
  a->s.cycles_consumed = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  (void)run_one(a, &exit_reason);
  return exit_reason;
}

/* The rest is the A64 interpreter's state handling, on the embedded
 * Interp_State - except the stack pointer, which is r13. */
static uint64_t a32_get_fault_address(CPU_State *s) { return CPU_BACKEND_INTERPRETER.get_fault_address(s); }
static uint64_t a32_get_cycles(CPU_State *s) { return CPU_BACKEND_INTERPRETER.get_cycles_consumed(s); }
static uint64_t a32_get_reg(CPU_State *s, uint8_t i) { return CPU_BACKEND_INTERPRETER.get_reg(s, i); }
static void a32_set_reg_api(CPU_State *s, uint8_t i, uint64_t v) { CPU_BACKEND_INTERPRETER.set_reg(s, i, (uint32_t)v); }
static uint64_t a32_get_pc(CPU_State *s) { return CPU_BACKEND_INTERPRETER.get_pc(s); }
static void a32_set_pc(CPU_State *s, uint64_t v) { CPU_BACKEND_INTERPRETER.set_pc(s, (uint32_t)v); }
static uint64_t a32_get_sp(CPU_State *s) { return as_a32(s)->s.regs.x[A32_SP]; }
static void a32_set_sp(CPU_State *s, uint64_t v) {
  as_a32(s)->s.regs.x[A32_SP] = (uint32_t)v;
  as_a32(s)->s.regs.sp = (uint32_t)v;
}
static uint32_t a32_get_pstate(CPU_State *s) { return CPU_BACKEND_INTERPRETER.get_pstate(s); }
static void a32_set_pstate(CPU_State *s, uint32_t v) { CPU_BACKEND_INTERPRETER.set_pstate(s, v); }
static CPU_Register_File *a32_get_register_file(CPU_State *s) { return CPU_BACKEND_INTERPRETER.get_register_file(s); }
static uint64_t a32_get_sys_reg(CPU_State *s, uint32_t r) {
  if (r == CPU_SYSREG_FPSR) return a32_fpscr(as_a32(s)) & (A32_FPSCR_STATUS_MASK | A32_FPSCR_NZCV_MASK);
  return CPU_BACKEND_INTERPRETER.get_sys_reg(s, r);
}
static void a32_set_sys_reg(CPU_State *s, uint32_t r, uint64_t v) {
  if (r == CPU_SYSREG_FPSR) as_a32(s)->fpscr_nzcv = (uint32_t)v & A32_FPSCR_NZCV_MASK;
  CPU_BACKEND_INTERPRETER.set_sys_reg(s, r, v);
}
static CPU_Vector_Register a32_get_vector_reg(CPU_State *s, uint8_t i) { return CPU_BACKEND_INTERPRETER.get_vector_reg(s, i); }
static void a32_set_vector_reg(CPU_State *s, uint8_t i, CPU_Vector_Register v) { CPU_BACKEND_INTERPRETER.set_vector_reg(s, i, v); }
static void a32_invalidate_cache(CPU_State *s, uint64_t va, uint64_t n) { CPU_BACKEND_INTERPRETER.invalidate_cache(s, va, n); }
static void a32_clear_cache(CPU_State *s) { CPU_BACKEND_INTERPRETER.clear_cache(s); }
static void a32_set_svc_handler(CPU_State *s, CPU_SVC_Handler h) { CPU_BACKEND_INTERPRETER.set_svc_handler(s, h); }
static void a32_set_undefined_handler(CPU_State *s, CPU_Undefined_Handler h) {
  CPU_BACKEND_INTERPRETER.set_undefined_handler(s, h);
}
static void a32_set_breakpoint_handler(CPU_State *s, CPU_Breakpoint_Handler h) {
  CPU_BACKEND_INTERPRETER.set_breakpoint_handler(s, h);
}

const CPU_Backend CPU_BACKEND_A32 = {
    .create = a32_create,
    .destroy = a32_destroy,
    .run = a32_run,
    .step = a32_step,
    .get_fault_address = a32_get_fault_address,
    .get_cycles_consumed = a32_get_cycles,
    .get_reg = a32_get_reg,
    .set_reg = a32_set_reg_api,
    .get_pc = a32_get_pc,
    .set_pc = a32_set_pc,
    .get_sp = a32_get_sp,
    .set_sp = a32_set_sp,
    .get_pstate = a32_get_pstate,
    .set_pstate = a32_set_pstate,
    .get_register_file = a32_get_register_file,
    .get_sys_reg = a32_get_sys_reg,
    .set_sys_reg = a32_set_sys_reg,
    .get_vector_reg = a32_get_vector_reg,
    .set_vector_reg = a32_set_vector_reg,
    .invalidate_cache = a32_invalidate_cache,
    .clear_cache = a32_clear_cache,
    .set_svc_handler = a32_set_svc_handler,
    .set_undefined_handler = a32_set_undefined_handler,
    .set_breakpoint_handler = a32_set_breakpoint_handler,
    .name = "a32",
    .version = "0.1.0",
    .supports_jit = false,
    .supports_multicore = false, /* the exclusive monitor is not shared across host threads */
};

uint32_t a32_cpsr(const CPU_State *state) { return read_cpsr((const A32_State *)state); }
uint32_t a32_fpscr_of(const CPU_State *state) { return a32_fpscr((const A32_State *)state); }

void a32_set_state_for_test(CPU_State *state, uint32_t cpsr, uint32_t fpscr) {
  A32_State *a = (A32_State *)state;
  a->q = (cpsr >> 27) & 1u;
  a->ge = (cpsr >> 16) & 0xFu;
  a32_set_fpscr(a, fpscr);
}
