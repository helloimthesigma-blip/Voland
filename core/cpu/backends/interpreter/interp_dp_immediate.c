/**
 * A64 data processing - immediate (DDI 0487 C4.1.86): PC-relative
 * addressing, add/subtract, logical, move wide, bitfield, extract.
 */
#include "cpu/backends/interpreter/interp_internal.h"

static Interp_Status pc_relative(Interp_State *s, uint32_t insn) {
  const uint32_t rd = bits(insn, 4, 0);
  const uint64_t imm = (uint64_t)sign_extend((bits(insn, 23, 5) << 2) | bits(insn, 30, 29), 21);
  if (bit(insn, 31)) { /* ADRP */
    set_xreg(s, rd, (s->regs.pc & ~(uint64_t)0xFFF) + (imm << 12));
  } else {             /* ADR */
    set_xreg(s, rd, s->regs.pc + imm);
  }
  return advance(s);
}

static Interp_Status add_sub_immediate(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  const uint64_t imm = (uint64_t)bits(insn, 21, 10) << (bit(insn, 22) ? 12 : 0);
  const uint64_t operand1 = xreg_sp(s, rn) & width_mask(sf);
  uint32_t nzcv = 0;
  const uint64_t result = sub ? interp_add_with_carry(operand1, ~imm & width_mask(sf), 1, sf, &nzcv)
                              : interp_add_with_carry(operand1, imm, 0, sf, &nzcv);
  if (set_flags) {
    set_nzcv(s, nzcv);
    set_reg_width(s, rd, sf, result);
  } else {
    set_xreg_sp(s, rd, result & width_mask(sf));
  }
  return advance(s);
}

static Interp_Status logical_immediate(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), n = bit(insn, 22);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (!sf && n) return INTERP_UNDEFINED;
  uint64_t imm = 0, unused = 0;
  if (!interp_decode_bit_masks(n, bits(insn, 15, 10), bits(insn, 21, 16), true, sf, &imm, &unused)) {
    return INTERP_UNDEFINED;
  }
  const uint64_t operand1 = xreg(s, rn);
  uint64_t result;
  switch (opc) {
  case 0: result = operand1 & imm; break;
  case 1: result = operand1 | imm; break;
  case 2: result = operand1 ^ imm; break;
  default: result = operand1 & imm; break;
  }
  result &= width_mask(sf);
  if (opc == 3) { /* ANDS */
    const unsigned msb = sf ? 63u : 31u;
    set_nzcv(s, (uint32_t)(((result >> msb) & 1u) << 3) | ((result == 0) << 2));
    set_xreg(s, rd, result);
  } else {
    set_xreg_sp(s, rd, result);
  }
  return advance(s);
}

static Interp_Status move_wide(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), hw = bits(insn, 22, 21), rd = bits(insn, 4, 0);
  if (opc == 1 || (!sf && hw >= 2)) return INTERP_UNDEFINED;
  const unsigned shift = hw * 16u;
  const uint64_t imm = (uint64_t)bits(insn, 20, 5) << shift;
  uint64_t result;
  switch (opc) {
  case 0: result = ~imm; break;                                            /* MOVN */
  case 2: result = imm; break;                                             /* MOVZ */
  default: result = (xreg(s, rd) & ~((uint64_t)0xFFFF << shift)) | imm; break; /* MOVK */
  }
  set_reg_width(s, rd, sf, result);
  return advance(s);
}

static Interp_Status bitfield(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), n = bit(insn, 22);
  const uint32_t immr = bits(insn, 21, 16), imms = bits(insn, 15, 10);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (opc == 3 || n != (uint32_t)sf) return INTERP_UNDEFINED;
  if (!sf && ((immr | imms) & 0x20u)) return INTERP_UNDEFINED;
  uint64_t wmask = 0, tmask = 0;
  if (!interp_decode_bit_masks(n, imms, immr, false, sf, &wmask, &tmask)) return INTERP_UNDEFINED;
  const unsigned width = sf ? 64u : 32u;
  const bool inzero = opc != 1, extend = opc == 0;
  const uint64_t dst = inzero ? 0 : xreg(s, rd);
  const uint64_t src = xreg(s, rn) & width_mask(sf);
  const uint64_t bot = (dst & ~wmask) | (ror64(src, immr, width) & wmask);
  const uint64_t top = extend ? (((src >> imms) & 1u) ? width_mask(sf) : 0) : dst;
  set_reg_width(s, rd, sf, (top & ~tmask) | (bot & tmask));
  return advance(s);
}

static Interp_Status extract(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t op21 = bits(insn, 30, 29), n = bit(insn, 22), o0 = bit(insn, 21);
  const uint32_t lsb = bits(insn, 15, 10);
  if (op21 != 0 || o0 != 0 || n != (uint32_t)sf || (!sf && lsb >= 32u)) return INTERP_UNDEFINED;
  const uint64_t hi = xreg(s, bits(insn, 9, 5)) & width_mask(sf);
  const uint64_t lo = xreg(s, bits(insn, 20, 16)) & width_mask(sf);
  const unsigned width = sf ? 64u : 32u;
  const uint64_t result = lsb == 0 ? lo : ((lo >> lsb) | (hi << (width - lsb)));
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

Interp_Status interp_dp_immediate(Interp_State *s, uint32_t insn) {
  switch (bits(insn, 25, 23)) {
  case 0: case 1: return pc_relative(s, insn);
  case 2: return add_sub_immediate(s, insn);
  case 4: return logical_immediate(s, insn);
  case 5: return move_wide(s, insn);
  case 6: return bitfield(s, insn);
  case 7: return extract(s, insn);
  default: return INTERP_UNDEFINED; /* 3: add/sub with tags (MTE) */
  }
}
