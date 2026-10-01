/**
 * A64 data processing - register (DDI 0487 C4.1.89), ARMv8.0 plus CRC32
 * (present on Cortex-A57): logical/add/sub shifted and extended, add with
 * carry, conditional compare/select, 1-, 2- and 3-source.
 */
#include "cpu/backends/interpreter/interp_internal.h"

#define CRC32_POLY_REFLECTED 0xEDB88320u  /* CRC-32 (0x04C11DB7), bit-reflected */
#define CRC32C_POLY_REFLECTED 0x82F63B78u /* CRC-32C (0x1EDC6F41), bit-reflected */

typedef enum Shift_Type { SHIFT_LSL = 0, SHIFT_LSR = 1, SHIFT_ASR = 2, SHIFT_ROR = 3 } Shift_Type;

static uint64_t shift_reg(uint64_t value, uint32_t type, uint32_t amount, bool sf) {
  const unsigned width = sf ? 64u : 32u;
  value &= width_mask(sf);
  if (amount == 0) return value;
  switch (type) {
  case SHIFT_LSL: return (value << amount) & width_mask(sf);
  case SHIFT_LSR: return value >> amount;
  case SHIFT_ASR: return ((uint64_t)(sign_extend(value, width) >> amount)) & width_mask(sf);
  default: return ror64(value, amount, width);
  }
}

static uint64_t extend_reg(uint64_t value, uint32_t option, uint32_t shift, bool sf) {
  uint64_t extended;
  switch (option) {
  case 0: extended = value & 0xFFu; break;
  case 1: extended = value & 0xFFFFu; break;
  case 2: extended = value & 0xFFFFFFFFull; break;
  case 3: extended = value; break;
  case 4: extended = (uint64_t)sign_extend(value, 8); break;
  case 5: extended = (uint64_t)sign_extend(value, 16); break;
  case 6: extended = (uint64_t)sign_extend(value, 32); break;
  default: extended = value; break;
  }
  return (extended << shift) & width_mask(sf);
}

static uint32_t logic_flags(uint64_t result, bool sf) {
  const unsigned msb = sf ? 63u : 31u;
  return (uint32_t)(((result >> msb) & 1u) << 3) | ((result == 0) << 2);
}

static Interp_Status logical_shifted(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31), invert = bit(insn, 21);
  const uint32_t opc = bits(insn, 30, 29), amount = bits(insn, 15, 10);
  if (!sf && amount >= 32u) return INTERP_UNDEFINED;
  uint64_t operand2 = shift_reg(xreg(s, bits(insn, 20, 16)), bits(insn, 23, 22), amount, sf);
  if (invert) operand2 = ~operand2 & width_mask(sf);
  const uint64_t operand1 = xreg(s, bits(insn, 9, 5)) & width_mask(sf);
  uint64_t result;
  switch (opc) {
  case 0: case 3: result = operand1 & operand2; break;
  case 1: result = operand1 | operand2; break;
  default: result = operand1 ^ operand2; break;
  }
  if (opc == 3) set_nzcv(s, logic_flags(result, sf));
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

static Interp_Status add_sub_shifted(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t type = bits(insn, 23, 22), amount = bits(insn, 15, 10);
  if (type == SHIFT_ROR || (!sf && amount >= 32u)) return INTERP_UNDEFINED;
  uint64_t operand2 = shift_reg(xreg(s, bits(insn, 20, 16)), type, amount, sf);
  const uint64_t operand1 = xreg(s, bits(insn, 9, 5)) & width_mask(sf);
  if (sub) operand2 = ~operand2 & width_mask(sf);
  uint32_t nzcv = 0;
  const uint64_t result = interp_add_with_carry(operand1, operand2, sub ? 1u : 0u, sf, &nzcv);
  if (set_flags) set_nzcv(s, nzcv);
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

static Interp_Status add_sub_extended(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t shift = bits(insn, 12, 10);
  if (bits(insn, 23, 22) != 0 || shift > 4u) return INTERP_UNDEFINED;
  uint64_t operand2 = extend_reg(xreg(s, bits(insn, 20, 16)), bits(insn, 15, 13), shift, sf);
  const uint64_t operand1 = xreg_sp(s, bits(insn, 9, 5)) & width_mask(sf);
  if (sub) operand2 = ~operand2 & width_mask(sf);
  uint32_t nzcv = 0;
  const uint64_t result = interp_add_with_carry(operand1, operand2, sub ? 1u : 0u, sf, &nzcv);
  const uint32_t rd = bits(insn, 4, 0);
  if (set_flags) {
    set_nzcv(s, nzcv);
    set_reg_width(s, rd, sf, result);
  } else {
    set_xreg_sp(s, rd, result & width_mask(sf));
  }
  return advance(s);
}

static Interp_Status add_sub_carry(Interp_State *s, uint32_t insn) {
  if (bits(insn, 15, 10) != 0) return INTERP_UNDEFINED;
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  uint64_t operand2 = xreg(s, bits(insn, 20, 16)) & width_mask(sf);
  if (sub) operand2 = ~operand2 & width_mask(sf);
  const uint32_t carry = (get_nzcv(s) >> 1) & 1u;
  uint32_t nzcv = 0;
  const uint64_t result =
      interp_add_with_carry(xreg(s, bits(insn, 9, 5)) & width_mask(sf), operand2, carry, sf, &nzcv);
  if (set_flags) set_nzcv(s, nzcv);
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

static Interp_Status conditional_compare(Interp_State *s, uint32_t insn) {
  if (!bit(insn, 29) || bit(insn, 10) || bit(insn, 4)) return INTERP_UNDEFINED;
  const bool sf = bit(insn, 31), sub = bit(insn, 30), immediate = bit(insn, 11);
  if (!interp_condition_holds(s, bits(insn, 15, 12))) {
    set_nzcv(s, bits(insn, 3, 0));
    return advance(s);
  }
  uint64_t operand2 = immediate ? bits(insn, 20, 16) : (xreg(s, bits(insn, 20, 16)) & width_mask(sf));
  if (sub) operand2 = ~operand2 & width_mask(sf);
  uint32_t nzcv = 0;
  (void)interp_add_with_carry(xreg(s, bits(insn, 9, 5)) & width_mask(sf), operand2, sub ? 1u : 0u, sf,
                              &nzcv);
  set_nzcv(s, nzcv);
  return advance(s);
}

static Interp_Status conditional_select(Interp_State *s, uint32_t insn) {
  if (bit(insn, 29) || bit(insn, 11)) return INTERP_UNDEFINED;
  const bool sf = bit(insn, 31), else_inv = bit(insn, 30), else_inc = bit(insn, 10);
  uint64_t result;
  if (interp_condition_holds(s, bits(insn, 15, 12))) {
    result = xreg(s, bits(insn, 9, 5));
  } else {
    result = xreg(s, bits(insn, 20, 16));
    if (else_inv) result = ~result;
    if (else_inc) result += 1u;
  }
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

static uint32_t crc32_update(uint32_t crc, uint64_t data, uint32_t bytes, uint32_t poly) {
  for (uint32_t i = 0; i < bytes; i++) {
    crc ^= (uint32_t)((data >> (8u * i)) & 0xFFu);
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1u)));
  }
  return crc;
}

static Interp_Status dp_two_source(Interp_State *s, uint32_t insn) {
  if (bit(insn, 29)) return INTERP_UNDEFINED;
  const bool sf = bit(insn, 31);
  const uint32_t opcode = bits(insn, 15, 10), rd = bits(insn, 4, 0);
  const uint64_t n = xreg(s, bits(insn, 9, 5)) & width_mask(sf);
  const uint64_t m = xreg(s, bits(insn, 20, 16)) & width_mask(sf);
  const unsigned width = sf ? 64u : 32u;
  uint64_t result;
  switch (opcode) {
  case 0x02: /* UDIV */
    result = m == 0 ? 0 : n / m;
    break;
  case 0x03: { /* SDIV: rounds toward zero; INT_MIN / -1 = INT_MIN; x / 0 = 0 */
    const int64_t a = sign_extend(n, width), b = sign_extend(m, width);
    if (b == 0) result = 0;
    else if (b == -1) result = (uint64_t)0 - (uint64_t)a;
    else result = (uint64_t)(a / b);
    break;
  }
  case 0x08: case 0x09: case 0x0A: case 0x0B: /* LSLV, LSRV, ASRV, RORV */
    result = shift_reg(n, opcode - 0x08u, (uint32_t)(m % width), sf);
    break;
  case 0x10: case 0x11: case 0x12: case 0x13:   /* CRC32B/H/W/X */
  case 0x14: case 0x15: case 0x16: case 0x17: { /* CRC32CB/H/W/X */
    const uint32_t size = opcode & 3u;
    if ((size == 3u) != sf) return INTERP_UNDEFINED;
    const uint32_t poly = (opcode & 4u) ? CRC32C_POLY_REFLECTED : CRC32_POLY_REFLECTED;
    result = crc32_update((uint32_t)n, xreg(s, bits(insn, 20, 16)), 1u << size, poly);
    set_reg_width(s, rd, false, result);
    return advance(s);
  }
  default:
    return INTERP_UNDEFINED; /* incl. PACGA, IRG/GMI/SUBP (MTE) */
  }
  set_reg_width(s, rd, sf, result);
  return advance(s);
}

static uint64_t reverse_bytes_in(uint64_t value, unsigned container_bytes, unsigned total_bytes) {
  uint64_t out = 0;
  for (unsigned c = 0; c < total_bytes; c += container_bytes) {
    for (unsigned i = 0; i < container_bytes; i++) {
      const uint64_t byte = (value >> (8u * (c + i))) & 0xFFu;
      out |= byte << (8u * (c + container_bytes - 1u - i));
    }
  }
  return out;
}

static Interp_Status dp_one_source(Interp_State *s, uint32_t insn) {
  if (bit(insn, 29) || bits(insn, 20, 16) != 0) return INTERP_UNDEFINED; /* incl. PAC */
  const bool sf = bit(insn, 31);
  const unsigned width = sf ? 64u : 32u;
  const uint64_t n = xreg(s, bits(insn, 9, 5)) & width_mask(sf);
  uint64_t result = 0;
  switch (bits(insn, 15, 10)) {
  case 0: /* RBIT */
    for (unsigned i = 0; i < width; i++) result |= ((n >> i) & 1u) << (width - 1u - i);
    break;
  case 1: result = reverse_bytes_in(n, 2, width / 8u); break;           /* REV16 */
  case 2: result = reverse_bytes_in(n, 4, width / 8u); break; /* REV32 (X) / REV (W) */
  case 3:
    if (!sf) return INTERP_UNDEFINED;
    result = reverse_bytes_in(n, 8, 8); /* REV (X) */
    break;
  case 4: { /* CLZ */
    unsigned count = 0;
    while (count < width && !((n >> (width - 1u - count)) & 1u)) count++;
    result = count;
    break;
  }
  case 5: { /* CLS: leading bits equal to the sign bit, excluding it */
    const uint64_t sign = (n >> (width - 1u)) & 1u;
    unsigned count = 0;
    while (count < width - 1u && ((n >> (width - 2u - count)) & 1u) == sign) count++;
    result = count;
    break;
  }
  default:
    return INTERP_UNDEFINED;
  }
  set_reg_width(s, bits(insn, 4, 0), sf, result);
  return advance(s);
}

/* 64x64 -> 128 without __int128 (MSVC builds this file too). */
static void multiply_u128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  const uint64_t a_lo = (uint32_t)a, a_hi = a >> 32, b_lo = (uint32_t)b, b_hi = b >> 32;
  const uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
  const uint64_t middle = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
  *lo = (middle << 32) | (uint32_t)p0;
  *hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
}

static uint64_t multiply_high_signed(int64_t a, int64_t b) {
  uint64_t hi = 0, lo = 0;
  multiply_u128((uint64_t)a, (uint64_t)b, &hi, &lo);
  if (a < 0) hi -= (uint64_t)b;
  if (b < 0) hi -= (uint64_t)a;
  return hi;
}

static Interp_Status dp_three_source(Interp_State *s, uint32_t insn) {
  if (bits(insn, 30, 29) != 0) return INTERP_UNDEFINED;
  const bool sf = bit(insn, 31), subtract = bit(insn, 15);
  const uint32_t op31 = bits(insn, 23, 21);
  const uint64_t n = xreg(s, bits(insn, 9, 5)), m = xreg(s, bits(insn, 20, 16));
  const uint64_t a = xreg(s, bits(insn, 14, 10));
  const uint32_t rd = bits(insn, 4, 0);
  if (op31 == 0) { /* MADD / MSUB */
    const uint64_t product = (n & width_mask(sf)) * (m & width_mask(sf));
    set_reg_width(s, rd, sf, subtract ? a - product : a + product);
    return advance(s);
  }
  if (!sf) return INTERP_UNDEFINED;
  switch (op31) {
  case 1: { /* SMADDL / SMSUBL */
    const uint64_t product = (uint64_t)(sign_extend(n, 32) * sign_extend(m, 32));
    set_xreg(s, rd, subtract ? a - product : a + product);
    return advance(s);
  }
  case 5: { /* UMADDL / UMSUBL */
    const uint64_t product = (n & 0xFFFFFFFFull) * (m & 0xFFFFFFFFull);
    set_xreg(s, rd, subtract ? a - product : a + product);
    return advance(s);
  }
  case 2: /* SMULH */
    if (subtract) return INTERP_UNDEFINED;
    set_xreg(s, rd, multiply_high_signed((int64_t)n, (int64_t)m));
    return advance(s);
  case 6: { /* UMULH */
    if (subtract) return INTERP_UNDEFINED;
    uint64_t hi = 0, lo = 0;
    multiply_u128(n, m, &hi, &lo);
    set_xreg(s, rd, hi);
    return advance(s);
  }
  default:
    return INTERP_UNDEFINED;
  }
}

Interp_Status interp_dp_register(Interp_State *s, uint32_t insn) {
  if (!bit(insn, 28)) {
    if (!bit(insn, 24)) return logical_shifted(s, insn);
    if (!bit(insn, 21)) return add_sub_shifted(s, insn);
    return add_sub_extended(s, insn);
  }
  if (bit(insn, 24)) return dp_three_source(s, insn);
  switch (bits(insn, 23, 21)) {
  case 0: return add_sub_carry(s, insn); /* rotate/set-flags forms are 8.4: op3 != 0 */
  case 2: return conditional_compare(s, insn);
  case 4: return conditional_select(s, insn);
  case 6: return bit(insn, 30) ? dp_one_source(s, insn) : dp_two_source(s, insn);
  default: return INTERP_UNDEFINED;
  }
}
