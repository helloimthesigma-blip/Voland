/**
 * Advanced SIMD structure loads/stores (DDI 0487: LD1-LD4, ST1-ST4, the
 * single-lane forms and LD1R-LD4R), with and without post-index.
 *
 * Every form touches one contiguous span of memory, so the span is read
 * or written with a single vmm access and registers are updated only
 * after it succeeded: a faulting instruction changes nothing (the
 * architecture leaves the registers UNKNOWN; "unchanged" is a valid
 * UNKNOWN and keeps the interpreter's fault contract uniform).
 */
#include "cpu/backends/interpreter/simd_lanes.h"

#define MAX_STRUCTURE_BYTES 64u /* LD4 of four Q registers */

static Interp_Status writeback(Interp_State *s, uint32_t insn, uint64_t address, uint64_t total) {
  if (bit(insn, 23)) { /* post-index */
    const uint32_t rm = bits(insn, 20, 16);
    set_xreg_sp(s, bits(insn, 9, 5), address + (rm == 31u ? total : xreg(s, rm)));
  }
  return advance(s);
}

static Interp_Status multiple_structures(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), load = bit(insn, 22);
  const uint32_t opcode = bits(insn, 15, 12), size = bits(insn, 11, 10);
  const uint32_t rn = bits(insn, 9, 5), rt = bits(insn, 4, 0);
  if (!bit(insn, 23) && bits(insn, 20, 16) != 0) return INTERP_UNDEFINED;
  unsigned rpt, selem;
  switch (opcode) {
  case 0x0: rpt = 1; selem = 4; break;
  case 0x2: rpt = 4; selem = 1; break;
  case 0x4: rpt = 1; selem = 3; break;
  case 0x6: rpt = 3; selem = 1; break;
  case 0x7: rpt = 1; selem = 1; break;
  case 0x8: rpt = 1; selem = 2; break;
  case 0xA: rpt = 2; selem = 1; break;
  default: return INTERP_UNDEFINED;
  }
  if (size == 3 && !q && selem != 1) return INTERP_UNDEFINED;
  const unsigned esize = 8u << size, ebytes = esize / 8u, elements = (q ? 128u : 64u) / esize;
  const uint64_t total = (uint64_t)rpt * elements * selem * ebytes;
  const uint64_t address = xreg_sp(s, rn);

  Vec regs[4];
  uint8_t data[MAX_STRUCTURE_BYTES];
  const unsigned count = rpt * selem;
  for (unsigned i = 0; i < count; i++) regs[i] = load ? (Vec){{0, 0}} : vread(s, (rt + i) % 32u);
  if (load && !interp_read(s, address, data, (uint32_t)total)) return INTERP_FAULT;

  uint64_t offset = 0;
  for (unsigned r = 0; r < rpt; r++) {
    for (unsigned e = 0; e < elements; e++) {
      for (unsigned k = 0; k < selem; k++) {
        Vec *reg = &regs[r + k];
        if (load) {
          uint64_t value = 0;
          for (unsigned b = 0; b < ebytes; b++) value |= (uint64_t)data[offset + b] << (8u * b);
          put(reg, e, esize, value);
        } else {
          const uint64_t value = ue(reg, e, esize);
          for (unsigned b = 0; b < ebytes; b++) data[offset + b] = (uint8_t)(value >> (8u * b));
        }
        offset += ebytes;
      }
    }
  }
  if (load) {
    for (unsigned i = 0; i < count; i++) vwrite(s, (rt + i) % 32u, regs[i], q);
  } else if (!interp_write(s, address, data, (uint32_t)total)) {
    return INTERP_FAULT;
  }
  return writeback(s, insn, address, total);
}

static Interp_Status single_structure(Interp_State *s, uint32_t insn) {
  const bool q = bit(insn, 30), load = bit(insn, 22), r_bit = bit(insn, 21), s_bit = bit(insn, 12);
  const uint32_t opcode = bits(insn, 15, 13), size = bits(insn, 11, 10);
  const uint32_t rn = bits(insn, 9, 5), rt = bits(insn, 4, 0);
  if (!bit(insn, 23) && bits(insn, 20, 16) != 0) return INTERP_UNDEFINED;
  const unsigned selem = (((opcode & 1u) << 1) | r_bit) + 1u;
  unsigned scale = opcode >> 1, index = 0;
  bool replicate = false;
  switch (scale) {
  case 0: index = ((unsigned)q << 3) | ((unsigned)s_bit << 2) | size; break;
  case 1:
    if (size & 1u) return INTERP_UNDEFINED;
    index = ((unsigned)q << 2) | ((unsigned)s_bit << 1) | (size >> 1);
    break;
  case 2:
    if (size & 2u) return INTERP_UNDEFINED;
    if (size == 0) {
      index = ((unsigned)q << 1) | s_bit;
    } else {
      if (s_bit) return INTERP_UNDEFINED;
      index = q;
      scale = 3;
    }
    break;
  default: /* LDnR */
    if (!load || s_bit) return INTERP_UNDEFINED;
    scale = size;
    replicate = true;
    break;
  }
  const unsigned esize = 8u << scale, ebytes = esize / 8u;
  const uint64_t total = (uint64_t)selem * ebytes;
  const uint64_t address = xreg_sp(s, rn);
  uint8_t data[MAX_STRUCTURE_BYTES];

  if (load) {
    if (!interp_read(s, address, data, (uint32_t)total)) return INTERP_FAULT;
    for (unsigned k = 0; k < selem; k++) {
      uint64_t value = 0;
      for (unsigned b = 0; b < ebytes; b++) value |= (uint64_t)data[k * ebytes + b] << (8u * b);
      const uint32_t reg = (rt + k) % 32u;
      if (replicate) {
        Vec r = {{0, 0}};
        for (unsigned e = 0; e < (q ? 128u : 64u) / esize; e++) put(&r, e, esize, value);
        vwrite(s, reg, r, q);
      } else {
        Vec r = vread(s, reg);
        put(&r, index, esize, value);
        vwrite(s, reg, r, true);
      }
    }
  } else {
    for (unsigned k = 0; k < selem; k++) {
      const Vec r = vread(s, (rt + k) % 32u);
      const uint64_t value = ue(&r, index, esize);
      for (unsigned b = 0; b < ebytes; b++) data[k * ebytes + b] = (uint8_t)(value >> (8u * b));
    }
    if (!interp_write(s, address, data, (uint32_t)total)) return INTERP_FAULT;
  }
  return writeback(s, insn, address, total);
}

Interp_Status interp_load_store_simd(Interp_State *s, uint32_t insn) {
  /* 0 Q 0011 0 0 {post} L ... multiple; 0 Q 0011 0 1 {post} L R ... single */
  if (bits(insn, 29, 25) != 0x06) return INTERP_UNDEFINED;
  if (bit(insn, 24)) return single_structure(s, insn);
  if (bit(insn, 21)) return INTERP_UNDEFINED;
  return multiple_structures(s, insn);
}
