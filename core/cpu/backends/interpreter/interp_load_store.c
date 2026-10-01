/**
 * A64 loads and stores (DDI 0487 C4.1.88), ARMv8.0: exclusive and
 * acquire/release, literal, pair, the four imm9 forms, register offset,
 * unsigned offset - for general and SIMD&FP registers. LSE atomics,
 * RCpc, MTE and memcpy/memset instructions postdate the Switch's
 * Cortex-A57 and decode as undefined. Multiple/single-structure SIMD
 * forms (LD1-LD4 & co.) are in interp_simd_fp.c.
 *
 * Every access goes through interp_read/interp_write (vmm, §5), and all
 * register writeback happens after the access succeeded, so a faulting
 * instruction leaves architectural state untouched.
 */
#include "cpu/backends/interpreter/interp_internal.h"

#include <string.h>

#define MAX_ACCESS_BYTES 16u

/* ------------------------------------------------------------------ */
/* Register transfer helpers.                                          */
/* ------------------------------------------------------------------ */

static uint64_t load_le(const uint8_t *bytes, uint32_t size) {
  uint64_t value = 0;
  for (uint32_t i = 0; i < size && i < 8u; i++) value |= (uint64_t)bytes[i] << (8u * i);
  return value;
}

static void store_le(uint8_t *bytes, uint64_t value, uint32_t size) {
  for (uint32_t i = 0; i < size && i < 8u; i++) bytes[i] = (uint8_t)(value >> (8u * i));
}

/* Writing a scalar SIMD&FP register zeroes the rest of the vector. */
static void set_vreg_bytes(Interp_State *s, uint32_t t, const uint8_t *bytes, uint32_t size) {
  uint8_t full[16] = {0};
  memcpy(full, bytes, size);
  s->v[t].lo = load_le(full, 8);
  s->v[t].hi = load_le(full + 8, 8);
}

static void get_vreg_bytes(const Interp_State *s, uint32_t t, uint8_t *bytes, uint32_t size) {
  uint8_t full[16];
  store_le(full, s->v[t].lo, 8);
  store_le(full + 8, s->v[t].hi, 8);
  memcpy(bytes, full, size);
}

/* What one transfer register does with the bytes. */
typedef enum Transfer_Kind {
  TRANSFER_STORE,
  TRANSFER_LOAD_ZERO_EXTEND,
  TRANSFER_LOAD_SIGN_EXTEND_64,
  TRANSFER_LOAD_SIGN_EXTEND_32,
  TRANSFER_STORE_VECTOR,
  TRANSFER_LOAD_VECTOR,
  TRANSFER_PREFETCH,
} Transfer_Kind;

/* Applies a completed load to register t. */
static void complete_load(Interp_State *s, Transfer_Kind kind, uint32_t t, const uint8_t *bytes,
                          uint32_t size) {
  switch (kind) {
  case TRANSFER_LOAD_ZERO_EXTEND:
    set_xreg(s, t, load_le(bytes, size));
    break;
  case TRANSFER_LOAD_SIGN_EXTEND_64:
    set_xreg(s, t, (uint64_t)sign_extend(load_le(bytes, size), size * 8u));
    break;
  case TRANSFER_LOAD_SIGN_EXTEND_32:
    set_xreg(s, t, (uint64_t)sign_extend(load_le(bytes, size), size * 8u) & 0xFFFFFFFFull);
    break;
  case TRANSFER_LOAD_VECTOR:
    set_vreg_bytes(s, t, bytes, size);
    break;
  default:
    break;
  }
}

static void prepare_store(const Interp_State *s, Transfer_Kind kind, uint32_t t, uint8_t *bytes,
                          uint32_t size) {
  if (kind == TRANSFER_STORE_VECTOR) get_vreg_bytes(s, t, bytes, size);
  else store_le(bytes, xreg(s, t), size);
}

static bool is_store(Transfer_Kind kind) {
  return kind == TRANSFER_STORE || kind == TRANSFER_STORE_VECTOR;
}

/* ------------------------------------------------------------------ */
/* Single-register forms (imm9, unsigned offset, register offset).     */
/* ------------------------------------------------------------------ */

/* Decodes size/V/opc into (kind, bytes, scale). Returns false if
 * unallocated. `allow_prefetch`: PRFM exists in the unsigned-offset and
 * register forms, PRFUM in the unscaled form; not in pre/post-index. */
static bool decode_single(uint32_t size, bool vector, uint32_t opc, bool allow_prefetch,
                          Transfer_Kind *kind, uint32_t *bytes) {
  if (vector) {
    const uint32_t scale = ((opc >> 1) << 2) | size;
    if (scale > 4u) return false;
    *bytes = 1u << scale;
    *kind = (opc & 1u) ? TRANSFER_LOAD_VECTOR : TRANSFER_STORE_VECTOR;
    return true;
  }
  *bytes = 1u << size;
  switch (opc) {
  case 0: *kind = TRANSFER_STORE; return true;
  case 1: *kind = TRANSFER_LOAD_ZERO_EXTEND; return true;
  case 2:
    if (size == 3) {
      *kind = TRANSFER_PREFETCH;
      return allow_prefetch;
    }
    *kind = TRANSFER_LOAD_SIGN_EXTEND_64;
    return true;
  default:
    if (size >= 2) return false;
    *kind = TRANSFER_LOAD_SIGN_EXTEND_32;
    return true;
  }
}

/* Executes a single transfer at `address`, then (if wback) base update. */
static Interp_Status transfer_single(Interp_State *s, Transfer_Kind kind, uint32_t size_bytes,
                                     uint32_t t, uint32_t n, uint64_t address, bool wback,
                                     uint64_t new_base) {
  uint8_t data[MAX_ACCESS_BYTES];
  if (kind == TRANSFER_PREFETCH) return advance(s);
  if (is_store(kind)) {
    prepare_store(s, kind, t, data, size_bytes);
    if (!interp_write(s, address, data, size_bytes)) return INTERP_FAULT;
  } else {
    if (!interp_read(s, address, data, size_bytes)) return INTERP_FAULT;
  }
  /* Writeback first, then the loaded value: for a load with t == n the
   * result is CONSTRAINED UNPREDICTABLE; letting the load win matches
   * what Cortex-A57 is documented to do for the non-writeback cases. */
  if (wback) set_xreg_sp(s, n, new_base);
  if (!is_store(kind)) complete_load(s, kind, t, data, size_bytes);
  return advance(s);
}

static Interp_Status single_imm9(Interp_State *s, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30), opc = bits(insn, 23, 22), form = bits(insn, 11, 10);
  const bool vector = bit(insn, 26);
  Transfer_Kind kind;
  uint32_t bytes = 0;
  /* form: 00 unscaled (LDUR/PRFUM), 01 post-index, 10 unprivileged, 11 pre-index */
  if (!decode_single(size, vector, opc, form == 0, &kind, &bytes)) return INTERP_UNDEFINED;
  if (form == 2 && vector) return INTERP_UNDEFINED;
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  const uint64_t offset = (uint64_t)sign_extend(bits(insn, 20, 12), 9);
  const uint64_t base = xreg_sp(s, n);
  const bool wback = form == 1 || form == 3;
  const uint64_t address = form == 1 ? base : base + offset;
  return transfer_single(s, kind, bytes, t, n, address, wback, base + offset);
}

static Interp_Status single_unsigned_offset(Interp_State *s, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30), opc = bits(insn, 23, 22);
  const bool vector = bit(insn, 26);
  Transfer_Kind kind;
  uint32_t bytes = 0;
  if (!decode_single(size, vector, opc, true, &kind, &bytes)) return INTERP_UNDEFINED;
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  const uint64_t address = xreg_sp(s, n) + ((uint64_t)bits(insn, 21, 10) * bytes);
  return transfer_single(s, kind, bytes, t, n, address, false, 0);
}

static Interp_Status single_register_offset(Interp_State *s, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30), opc = bits(insn, 23, 22);
  const uint32_t option = bits(insn, 15, 13);
  const bool vector = bit(insn, 26);
  if (!(option & 2u)) return INTERP_UNDEFINED; /* UXTB/UXTH/SXTB/SXTH not allowed */
  Transfer_Kind kind;
  uint32_t bytes = 0;
  if (!decode_single(size, vector, opc, true, &kind, &bytes)) return INTERP_UNDEFINED;
  uint32_t scale = 0;
  while ((1u << scale) < bytes) scale++;
  const uint32_t shift = bit(insn, 12) ? scale : 0;
  uint64_t offset = xreg(s, bits(insn, 20, 16));
  switch (option) {
  case 2: offset = offset & 0xFFFFFFFFull; break;               /* UXTW */
  case 6: offset = (uint64_t)sign_extend(offset, 32); break;    /* SXTW */
  default: break;                                               /* LSL/UXTX, SXTX */
  }
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  return transfer_single(s, kind, bytes, t, n, xreg_sp(s, n) + (offset << shift), false, 0);
}

/* ------------------------------------------------------------------ */
/* Literal and pair.                                                   */
/* ------------------------------------------------------------------ */

static Interp_Status load_literal(Interp_State *s, uint32_t insn) {
  const uint32_t opc = bits(insn, 31, 30), t = bits(insn, 4, 0);
  const bool vector = bit(insn, 26);
  const uint64_t address = s->regs.pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21);
  Transfer_Kind kind;
  uint32_t bytes;
  if (vector) {
    if (opc == 3) return INTERP_UNDEFINED;
    kind = TRANSFER_LOAD_VECTOR;
    bytes = 4u << opc;
  } else {
    switch (opc) {
    case 0: kind = TRANSFER_LOAD_ZERO_EXTEND; bytes = 4; break;
    case 1: kind = TRANSFER_LOAD_ZERO_EXTEND; bytes = 8; break;
    case 2: kind = TRANSFER_LOAD_SIGN_EXTEND_64; bytes = 4; break;
    default: return advance(s); /* PRFM (literal) */
    }
  }
  uint8_t data[MAX_ACCESS_BYTES];
  if (!interp_read(s, address, data, bytes)) return INTERP_FAULT;
  complete_load(s, kind, t, data, bytes);
  return advance(s);
}

static Interp_Status pair(Interp_State *s, uint32_t insn) {
  const uint32_t opc = bits(insn, 31, 30), form = bits(insn, 24, 23);
  const bool vector = bit(insn, 26), load = bit(insn, 22);
  Transfer_Kind kind;
  uint32_t bytes;
  if (vector) {
    if (opc == 3) return INTERP_UNDEFINED;
    bytes = 4u << opc;
    kind = load ? TRANSFER_LOAD_VECTOR : TRANSFER_STORE_VECTOR;
  } else {
    switch (opc) {
    case 0: bytes = 4; kind = load ? TRANSFER_LOAD_ZERO_EXTEND : TRANSFER_STORE; break;
    case 1:
      if (!load || form == 0) return INTERP_UNDEFINED; /* STGP (MTE); no LDNPSW */
      bytes = 4; kind = TRANSFER_LOAD_SIGN_EXTEND_64;
      break;
    case 2: bytes = 8; kind = load ? TRANSFER_LOAD_ZERO_EXTEND : TRANSFER_STORE; break;
    default: return INTERP_UNDEFINED;
    }
  }
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0), t2 = bits(insn, 14, 10);
  const uint64_t offset = (uint64_t)sign_extend(bits(insn, 21, 15), 7) * bytes;
  const uint64_t base = xreg_sp(s, n);
  /* form: 00 non-temporal (offset), 01 post-index, 10 offset, 11 pre-index */
  const bool wback = form == 1 || form == 3;
  const uint64_t address = form == 1 ? base : base + offset;

  uint8_t data[2u * MAX_ACCESS_BYTES];
  if (is_store(kind)) {
    prepare_store(s, kind, t, data, bytes);
    prepare_store(s, kind, t2, data + bytes, bytes);
    if (!interp_write(s, address, data, 2u * bytes)) return INTERP_FAULT;
  } else {
    if (!interp_read(s, address, data, 2u * bytes)) return INTERP_FAULT;
  }
  if (wback) set_xreg_sp(s, n, base + offset);
  if (!is_store(kind)) {
    complete_load(s, kind, t, data, bytes);
    complete_load(s, kind, t2, data + bytes, bytes);
  }
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Exclusive and acquire/release (ARMv8.0 subset).                     */
/* ------------------------------------------------------------------ */

static uint64_t granule_of(uint64_t address) {
  return address & ~(uint64_t)(INTERP_EXCLUSIVE_GRANULE - 1u);
}

static Interp_Status exclusive(Interp_State *s, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30);
  const bool o2 = bit(insn, 23), load = bit(insn, 22), o1 = bit(insn, 21), o0 = bit(insn, 15);
  const uint32_t rs = bits(insn, 20, 16), t2 = bits(insn, 14, 10), n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  const uint32_t element = 1u << size;
  const uint64_t address = xreg_sp(s, n);
  uint8_t data[MAX_ACCESS_BYTES];

  if (o2) { /* LDAR / STLR; o0 = 0 forms are LORegion (8.1), o1 = 1 is CAS (LSE) */
    if (o1 || !o0) return INTERP_UNDEFINED;
    if (address & (element - 1u)) {
      s->fault_address = address; /* alignment fault */
      return INTERP_FAULT;
    }
    if (load) {
      if (!interp_read(s, address, data, element)) return INTERP_FAULT;
      set_xreg(s, t, load_le(data, element));
    } else {
      store_le(data, xreg(s, t), element);
      if (!interp_write(s, address, data, element)) return INTERP_FAULT;
    }
    return advance(s);
  }

  /* LDXR/LDAXR/STXR/STLXR (o1 = 0) and the pair forms (o1 = 1, 32/64-bit). */
  if (o1 && size < 2) return INTERP_UNDEFINED; /* CASP (LSE) */
  const uint32_t total = o1 ? 2u * element : element;
  if (address & (total - 1u)) {
    s->fault_address = address;
    return INTERP_FAULT;
  }
  if (load) {
    if (!interp_read(s, address, data, total)) return INTERP_FAULT;
    s->exclusive_valid = true;
    s->exclusive_address = address;
    set_xreg(s, t, load_le(data, element));
    if (o1) set_xreg(s, t2, load_le(data + element, element));
    return advance(s);
  }

  /* Store-exclusive: succeeds iff this thread's monitor is set on the
   * same granule. The status register is written in either case, and the
   * monitor is cleared either way. A failing store writes no memory. */
  const bool pass = s->exclusive_valid && granule_of(s->exclusive_address) == granule_of(address);
  if (pass) {
    store_le(data, xreg(s, t), element);
    if (o1) store_le(data + element, xreg(s, t2), element);
    if (!interp_write(s, address, data, total)) return INTERP_FAULT;
  } else {
    /* Hardware still checks translation on a failing store-exclusive. */
    uint8_t probe;
    if (!interp_read(s, address, &probe, 1)) return INTERP_FAULT;
  }
  s->exclusive_valid = false;
  set_xreg(s, rs, pass ? 0u : 1u);
  (void)o0; /* acquire/release ordering is implicit: one thread at a time (§7) */
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Group decode.                                                       */
/* ------------------------------------------------------------------ */

Interp_Status interp_load_store(Interp_State *s, uint32_t insn) {
  const bool vector = bit(insn, 26);

  /* op0 = bits 31:28, op2 = bits 24:23, op3 = bits 21:16, op4 = bits 11:10 */
  if (bits(insn, 29, 28) == 0) {
    if (vector) {
      if (bit(insn, 31) == 0) return interp_load_store_simd(s, insn); /* LD1-4/ST1-4 & co. */
      return INTERP_UNDEFINED;
    }
    if (bits(insn, 24, 24) == 0) return exclusive(s, insn);
    return INTERP_UNDEFINED; /* LDAPR/STLUR (RCpc, 8.4) */
  }
  if (bits(insn, 29, 28) == 1) {
    if (bit(insn, 24) == 0) return load_literal(s, insn);
    return INTERP_UNDEFINED; /* memcpy/memset (8.8), RCpc unscaled */
  }
  if (bits(insn, 29, 28) == 2) return pair(s, insn);

  /* bits 29:28 == 3: single register */
  if (bit(insn, 24)) return single_unsigned_offset(s, insn);
  if (bit(insn, 21) == 0) return single_imm9(s, insn);
  if (bits(insn, 11, 10) == 2) return single_register_offset(s, insn);
  return INTERP_UNDEFINED; /* LSE atomics (op4 00), PAC loads (op4 x1) */
}
