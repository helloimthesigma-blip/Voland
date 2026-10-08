/**
 * Predecoded execution (§25 Phase 5 "predecoded interpreter"): straight-
 * line runs of guest code are decoded once into blocks of operations -
 * a handler plus the fields it needs, with immediates, bitmasks and PC-
 * relative targets precomputed - and executed from a direct-mapped cache.
 *
 * Correctness model: the reference decoder (interp_execute) stays the
 * definition. Every instruction without a specialized handler runs it
 * (op_reference), and a specialized handler that meets anything unusual
 * (an access crossing a page, a translation fault, writeback overlap)
 * defers to it for that execution - so the predecoded path computes
 * exactly what step() computes. tests/predecode_test.c checks that on
 * random instruction streams.
 *
 * Blocks end after any branch / exception / system instruction (group
 * 0xA/0xB), at a page boundary, or at PREDECODE_BLOCK_OPS. They are only
 * built from pages mapped executable and not writable; anything else runs
 * one instruction at a time through the reference path. The cache is
 * keyed by PC and validated by vmm_generation(): any mapping or
 * permission change drops every block, as do IC maintenance instructions
 * and the backend's invalidate_cache / clear_cache.
 *
 * The cache is shared by every guest thread's CPU_State (one guest
 * address space at a time, vmm.h) and private to each host thread.
 */
#include "cpu/backends/interpreter/interp_internal.h"

#include <stdlib.h>
#include <string.h>

#define PREDECODE_BLOCK_OPS 32u
#define PREDECODE_CACHE_BLOCKS 4096u
#define PREDECODE_INDEX(pc) (((pc) >> 2) & (PREDECODE_CACHE_BLOCKS - 1u))
#define REG_ZR 31u

typedef struct Op Op;
typedef Interp_Status (*Op_Handler)(Interp_State *s, const Op *op);

struct Op {
  Op_Handler fn;
  uint32_t insn;
  uint8_t rd, rn, rm, ra; /* register fields, as the handler uses them */
  uint8_t flags;          /* handler-specific (sf, set_flags, condition, ...) */
  uint8_t size;           /* access bytes, shift amount, bit position */
  uint8_t aux;
  uint8_t pad;
  uint64_t imm;           /* immediate, mask, or absolute target */
  uint64_t imm2;          /* second mask (bitfield tmask) */
};

typedef struct Block {
  uint64_t pc;
  uint64_t generation;    /* 0 = empty */
  uint32_t count;
  uint32_t pad;
  Op ops[PREDECODE_BLOCK_OPS];
} Block;

/* The block cache is per host thread: with parallel guest threads
 * (docs/PARALLEL.md) several host threads decode at once. The first
 * thread to run uses the static cache; any other allocates its own once
 * (released when that thread exits). A flush moves the shared code generation, so
 * it reaches every thread's cache. */
static Block g_blocks[PREDECODE_CACHE_BLOCKS];
/* A flush moves the shared code generation (vmm.h), so it reaches every
 * host thread's cache - and compiled JIT code checking the same word. */
void interp_predecode_flush(void) { vmm_bump_generation(); }

static uint64_t current_generation(void) { return vmm_generation(); }

#if (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)) || defined(_WIN32)
static Block *thread_blocks(void) { return g_blocks; }
#else
#include <pthread.h>

static _Thread_local Block *t_blocks;
static bool g_static_claimed;
static pthread_key_t g_blocks_key;
static pthread_once_t g_blocks_key_once = PTHREAD_ONCE_INIT;

static void make_blocks_key(void) { (void)pthread_key_create(&g_blocks_key, free); }

static Block *thread_blocks(void) {
  if (t_blocks) return t_blocks;
  if (!__atomic_exchange_n(&g_static_claimed, true, __ATOMIC_ACQ_REL)) {
    t_blocks = g_blocks;
    return t_blocks;
  }
  /* Once per extra host thread, never in the run loop's steady state. */
  Block *blocks = (Block *)calloc(PREDECODE_CACHE_BLOCKS, sizeof(Block));
  if (!blocks) return NULL;
  (void)pthread_once(&g_blocks_key_once, make_blocks_key);
  (void)pthread_setspecific(g_blocks_key, blocks);
  t_blocks = blocks;
  return t_blocks;
}
#endif

/* ------------------------------------------------------------------ */
/* Flag bits in Op.flags.                                              */
/* ------------------------------------------------------------------ */

#define F_SF 0x01u        /* 64-bit operation */
#define F_SET_FLAGS 0x02u
#define F_SUB 0x04u
#define F_SIGNED 0x08u    /* loads: sign-extend */
#define F_SIGNED32 0x10u  /* loads: sign-extend to 32 then zero-extend */
#define F_WBACK 0x20u
#define F_POST 0x40u
#define F_LINK 0x80u

/* ------------------------------------------------------------------ */
/* Reference fallback.                                                 */
/* ------------------------------------------------------------------ */

static Interp_Status op_reference(Interp_State *s, const Op *op) { return interp_execute(s, op->insn); }

/* ------------------------------------------------------------------ */
/* Data processing.                                                    */
/* ------------------------------------------------------------------ */

static Interp_Status op_add_imm(Interp_State *s, const Op *op) {
  const uint64_t mask = width_mask(op->flags & F_SF);
  set_xreg_sp(s, op->rd, (xreg_sp(s, op->rn) + op->imm) & mask);
  return advance(s);
}

static Interp_Status op_sub_imm(Interp_State *s, const Op *op) {
  const uint64_t mask = width_mask(op->flags & F_SF);
  set_xreg_sp(s, op->rd, (xreg_sp(s, op->rn) - op->imm) & mask);
  return advance(s);
}

/* ADDS/SUBS immediate (CMP/CMN): flags through AddWithCarry. */
static Interp_Status op_adds_imm(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  const uint64_t operand1 = xreg_sp(s, op->rn) & width_mask(sf);
  uint32_t nzcv = 0;
  const uint64_t result = (op->flags & F_SUB) ? interp_add_with_carry(operand1, ~op->imm & width_mask(sf), 1, sf, &nzcv)
                                              : interp_add_with_carry(operand1, op->imm, 0, sf, &nzcv);
  set_nzcv(s, nzcv);
  set_reg_width(s, op->rd, sf, result);
  return advance(s);
}

/* Logical immediate, AND/ORR/EOR (no flags): Rd may be SP. imm = mask. */
static Interp_Status op_and_imm(Interp_State *s, const Op *op) {
  set_xreg_sp(s, op->rd, (xreg(s, op->rn) & op->imm) & width_mask(op->flags & F_SF));
  return advance(s);
}
static Interp_Status op_orr_imm(Interp_State *s, const Op *op) {
  set_xreg_sp(s, op->rd, (xreg(s, op->rn) | op->imm) & width_mask(op->flags & F_SF));
  return advance(s);
}
static Interp_Status op_eor_imm(Interp_State *s, const Op *op) {
  set_xreg_sp(s, op->rd, (xreg(s, op->rn) ^ op->imm) & width_mask(op->flags & F_SF));
  return advance(s);
}

static uint32_t logic_nzcv(uint64_t result, bool sf) {
  const unsigned msb = sf ? 63u : 31u;
  return (uint32_t)(((result >> msb) & 1u) << 3) | ((result == 0) << 2);
}

static Interp_Status op_ands_imm(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  const uint64_t result = (xreg(s, op->rn) & op->imm) & width_mask(sf);
  set_nzcv(s, logic_nzcv(result, sf));
  set_xreg(s, op->rd, result);
  return advance(s);
}

/* MOVZ/MOVN: the whole value is known at decode time. */
static Interp_Status op_mov_const(Interp_State *s, const Op *op) {
  set_xreg(s, op->rd, op->imm);
  return advance(s);
}

/* MOVK: imm = the shifted 16 bits, size = the shift. */
static Interp_Status op_movk(Interp_State *s, const Op *op) {
  const uint64_t keep = ~((uint64_t)0xFFFF << op->size);
  set_reg_width(s, op->rd, op->flags & F_SF, (xreg(s, op->rd) & keep) | op->imm);
  return advance(s);
}

/* Shifted-register operand. size = amount, aux = shift type. */
static uint64_t shifted(const Interp_State *s, const Op *op, bool sf) {
  uint64_t value = xreg(s, op->rm) & width_mask(sf);
  const uint32_t amount = op->size;
  if (amount == 0) return value;
  switch (op->aux) {
  case 0: return (value << amount) & width_mask(sf);
  case 1: return value >> amount;
  case 2: return (uint64_t)(sign_extend(value, sf ? 64u : 32u) >> amount) & width_mask(sf);
  default: return ror64(value, amount, sf ? 64u : 32u);
  }
}

static Interp_Status op_add_sub_shifted(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  uint64_t operand2 = shifted(s, op, sf);
  const uint64_t operand1 = xreg(s, op->rn) & width_mask(sf);
  if (!(op->flags & F_SET_FLAGS)) {
    const uint64_t result = (op->flags & F_SUB) ? operand1 - operand2 : operand1 + operand2;
    set_reg_width(s, op->rd, sf, result);
    return advance(s);
  }
  if (op->flags & F_SUB) operand2 = ~operand2 & width_mask(sf);
  uint32_t nzcv = 0;
  const uint64_t result = interp_add_with_carry(operand1, operand2, (op->flags & F_SUB) ? 1u : 0u, sf, &nzcv);
  set_nzcv(s, nzcv);
  set_reg_width(s, op->rd, sf, result);
  return advance(s);
}

/* Logical shifted register. imm: 0 AND, 1 ORR, 2 EOR, 3 ANDS; ra != 0: invert operand 2. */
static Interp_Status op_logical_shifted(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  uint64_t operand2 = shifted(s, op, sf);
  if (op->ra) operand2 = ~operand2 & width_mask(sf);
  const uint64_t operand1 = xreg(s, op->rn) & width_mask(sf);
  uint64_t result;
  switch (op->imm) {
  case 1: result = operand1 | operand2; break;
  case 2: result = operand1 ^ operand2; break;
  default: result = operand1 & operand2; break;
  }
  if (op->imm == 3) set_nzcv(s, logic_nzcv(result, sf));
  set_reg_width(s, op->rd, sf, result);
  return advance(s);
}

/* CSEL/CSINC/CSINV/CSNEG: size = cond, aux bit0 = invert, bit1 = increment. */
static Interp_Status op_cond_select(Interp_State *s, const Op *op) {
  uint64_t result;
  if (interp_condition_holds(s, op->size)) {
    result = xreg(s, op->rn);
  } else {
    result = xreg(s, op->rm);
    if (op->aux & 1u) result = ~result;
    if (op->aux & 2u) result += 1u;
  }
  set_reg_width(s, op->rd, op->flags & F_SF, result);
  return advance(s);
}

/* SBFM/BFM/UBFM (ASR, LSL, LSR, SXTx, UXTx, BFI, BFXIL): imm = wmask, imm2 =
 * tmask, size = immr, aux = imms, rm = opc. */
static Interp_Status op_bitfield(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  const unsigned width = sf ? 64u : 32u;
  const bool inzero = op->rm != 1u, extend = op->rm == 0u;
  const uint64_t dst = inzero ? 0 : xreg(s, op->rd);
  const uint64_t src = xreg(s, op->rn) & width_mask(sf);
  const uint64_t bot = (dst & ~op->imm) | (ror64(src, op->size, width) & op->imm);
  const uint64_t top = extend ? (((src >> op->aux) & 1u) ? width_mask(sf) : 0) : dst;
  set_reg_width(s, op->rd, sf, (top & ~op->imm2) | (bot & op->imm2));
  return advance(s);
}

/* MADD/MSUB. */
static Interp_Status op_multiply_add(Interp_State *s, const Op *op) {
  const bool sf = op->flags & F_SF;
  const uint64_t product = xreg(s, op->rn) * xreg(s, op->rm);
  const uint64_t result = (op->flags & F_SUB) ? xreg(s, op->ra) - product : xreg(s, op->ra) + product;
  set_reg_width(s, op->rd, sf, result);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Branches.                                                           */
/* ------------------------------------------------------------------ */

/* ADR/ADRP: the result depends only on the instruction's own PC. */
static Interp_Status op_adr(Interp_State *s, const Op *op) {
  set_xreg(s, op->rd, op->imm);
  return advance(s);
}

static Interp_Status op_branch(Interp_State *s, const Op *op) {
  if (op->flags & F_LINK) {
    interp_maybe_trace_call(s, op->imm);
    s->regs.x[30] = s->regs.pc + 4u;
  }
  s->regs.pc = op->imm;
  return INTERP_CONTINUE;
}

static Interp_Status op_branch_cond(Interp_State *s, const Op *op) {
  if (interp_condition_holds(s, op->size)) {
    s->regs.pc = op->imm;
    return INTERP_CONTINUE;
  }
  return advance(s);
}

/* CBZ/CBNZ: aux = 1 for CBNZ. */
static Interp_Status op_compare_branch(Interp_State *s, const Op *op) {
  const uint64_t value = xreg(s, op->rd) & width_mask(op->flags & F_SF);
  if ((value != 0) == (op->aux != 0)) {
    s->regs.pc = op->imm;
    return INTERP_CONTINUE;
  }
  return advance(s);
}

/* TBZ/TBNZ: size = bit position, aux = 1 for TBNZ. */
static Interp_Status op_test_branch(Interp_State *s, const Op *op) {
  const bool is_set = (xreg(s, op->rd) >> op->size) & 1u;
  if (is_set == (op->aux != 0)) {
    s->regs.pc = op->imm;
    return INTERP_CONTINUE;
  }
  return advance(s);
}

/* BR/BLR/RET. */
static Interp_Status op_branch_register(Interp_State *s, const Op *op) {
  const uint64_t target = xreg(s, op->rn);
  if (op->flags & F_LINK) {
    interp_maybe_trace_call(s, target);
    s->regs.x[30] = s->regs.pc + 4u;
  } else {
    interp_maybe_trace_return(s, target); /* RET or BR: either may leave a traced call */
  }
  s->regs.pc = target;
  return INTERP_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Loads and stores (general registers, no vector).                    */
/* ------------------------------------------------------------------ */

static uint64_t load_value(const uint8_t *host, uint32_t size, uint8_t flags) {
  uint64_t raw = 0;
  switch (size) {
  case 1: raw = host[0]; break;
  case 2: { uint16_t v; memcpy(&v, host, 2); raw = v; break; }
  case 4: { uint32_t v; memcpy(&v, host, 4); raw = v; break; }
  default: memcpy(&raw, host, 8); break;
  }
  if (flags & F_SIGNED) return (uint64_t)sign_extend(raw, size * 8u);
  if (flags & F_SIGNED32) return (uint64_t)sign_extend(raw, size * 8u) & 0xFFFFFFFFull;
  return raw;
}

static void store_value(uint8_t *host, uint64_t value, uint32_t size) {
  switch (size) {
  case 1: host[0] = (uint8_t)value; break;
  case 2: { const uint16_t v = (uint16_t)value; memcpy(host, &v, 2); break; }
  case 4: { const uint32_t v = (uint32_t)value; memcpy(host, &v, 4); break; }
  default: memcpy(host, &value, 8); break;
  }
}

/* Host pointer for [address, +bytes) if it stays in one page with the
 * permission; NULL sends the instruction to the reference path. */
static inline uint8_t *fast_translate(const Interp_State *s, uint64_t address, uint32_t bytes, uint32_t perm) {
  if (vmm_access_crosses_page(address, bytes)) return NULL;
  VMM_Fault fault;
  uint8_t *host = vmm_translate_inline(s->l1, address, perm, &fault);
  return host && interp_watched(host) ? NULL : host; /* a watched read takes the reference path */
}

/* LDR/LDRB/LDRH/LDRS* unsigned offset: imm = byte offset. */
static Interp_Status op_load_unsigned(Interp_State *s, const Op *op) {
  const uint64_t address = xreg_sp(s, op->rn) + op->imm;
  const uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_R);
  if (!host) return interp_execute(s, op->insn);
  set_xreg(s, op->rd, load_value(host, op->size, op->flags));
  return advance(s);
}

static Interp_Status op_store_unsigned(Interp_State *s, const Op *op) {
  const uint64_t address = xreg_sp(s, op->rn) + op->imm;
  uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_W);
  if (!host) return interp_execute(s, op->insn);
  store_value(host, xreg(s, op->rd), op->size);
  return advance(s);
}

/* imm9 forms: LDUR/STUR (no writeback), pre-index (F_WBACK), post-index
 * (F_WBACK|F_POST). Order as the reference: access, writeback, then the
 * loaded value - so a load into the base register keeps the loaded value. */
static Interp_Status op_load_imm9(Interp_State *s, const Op *op) {
  const uint64_t base = xreg_sp(s, op->rn);
  const uint64_t address = (op->flags & F_POST) ? base : base + op->imm;
  const uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_R);
  if (!host) return interp_execute(s, op->insn);
  const uint64_t value = load_value(host, op->size, op->flags);
  if (op->flags & F_WBACK) set_xreg_sp(s, op->rn, base + op->imm);
  set_xreg(s, op->rd, value);
  return advance(s);
}

static Interp_Status op_store_imm9(Interp_State *s, const Op *op) {
  const uint64_t base = xreg_sp(s, op->rn);
  const uint64_t address = (op->flags & F_POST) ? base : base + op->imm;
  uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_W);
  if (!host) return interp_execute(s, op->insn);
  store_value(host, xreg(s, op->rd), op->size);
  if (op->flags & F_WBACK) set_xreg_sp(s, op->rn, base + op->imm);
  return advance(s);
}

/* Register offset: aux = option (2 UXTW, 6 SXTW, else unextended),
 * ra = shift amount. */
static uint64_t register_offset(const Interp_State *s, const Op *op) {
  uint64_t offset = xreg(s, op->rm);
  if (op->aux == 2u) offset &= 0xFFFFFFFFull;
  else if (op->aux == 6u) offset = (uint64_t)sign_extend(offset, 32);
  return offset << op->ra;
}

static Interp_Status op_load_register(Interp_State *s, const Op *op) {
  const uint64_t address = xreg_sp(s, op->rn) + register_offset(s, op);
  const uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_R);
  if (!host) return interp_execute(s, op->insn);
  set_xreg(s, op->rd, load_value(host, op->size, op->flags));
  return advance(s);
}

static Interp_Status op_store_register(Interp_State *s, const Op *op) {
  const uint64_t address = xreg_sp(s, op->rn) + register_offset(s, op);
  uint8_t *host = fast_translate(s, address, op->size, VMM_PERM_W);
  if (!host) return interp_execute(s, op->insn);
  store_value(host, xreg(s, op->rd), op->size);
  return advance(s);
}

/* LDP/STP (X or W, no LDPSW): imm = scaled signed offset; F_WBACK/F_POST. */
static Interp_Status op_load_pair(Interp_State *s, const Op *op) {
  const uint64_t base = xreg_sp(s, op->rn);
  const uint64_t address = (op->flags & F_POST) ? base : base + op->imm;
  const uint8_t *host = fast_translate(s, address, 2u * op->size, VMM_PERM_R);
  if (!host) return interp_execute(s, op->insn);
  const uint64_t first = load_value(host, op->size, 0), second = load_value(host + op->size, op->size, 0);
  if (op->flags & F_WBACK) set_xreg_sp(s, op->rn, base + op->imm);
  set_xreg(s, op->rd, first);
  set_xreg(s, op->ra, second);
  return advance(s);
}

static Interp_Status op_store_pair(Interp_State *s, const Op *op) {
  const uint64_t base = xreg_sp(s, op->rn);
  const uint64_t address = (op->flags & F_POST) ? base : base + op->imm;
  uint8_t *host = fast_translate(s, address, 2u * op->size, VMM_PERM_W);
  if (!host) return interp_execute(s, op->insn);
  store_value(host, xreg(s, op->rd), op->size);
  store_value(host + op->size, xreg(s, op->ra), op->size);
  if (op->flags & F_WBACK) set_xreg_sp(s, op->rn, base + op->imm);
  return advance(s);
}

/* ------------------------------------------------------------------ */
/* Decode.                                                             */
/* ------------------------------------------------------------------ */

static Op reference_op(uint32_t insn) {
  Op op;
  memset(&op, 0, sizeof(op));
  op.fn = op_reference;
  op.insn = insn;
  return op;
}

/* Load/store single, unsigned offset, general registers. */
static bool decode_load_store_unsigned(uint32_t insn, Op *op) {
  if (bits(insn, 29, 27) != 7u || bit(insn, 26) || bits(insn, 25, 24) != 1u) return false;
  const uint32_t size = bits(insn, 31, 30), opc = bits(insn, 23, 22);
  const uint32_t bytes = 1u << size;
  op->rd = (uint8_t)bits(insn, 4, 0);
  op->rn = (uint8_t)bits(insn, 9, 5);
  op->size = (uint8_t)bytes;
  op->imm = (uint64_t)bits(insn, 21, 10) << size;
  switch (opc) {
  case 0: op->fn = op_store_unsigned; return true;
  case 1: op->fn = op_load_unsigned; return true;
  case 2:
    if (size == 3) return false;                   /* PRFM */
    op->fn = op_load_unsigned;
    op->flags = F_SIGNED;
    return true;
  default:
    if (size >= 2) return false;
    op->fn = op_load_unsigned;
    op->flags = F_SIGNED32;
    return true;
  }
}

/* Shared size/opc decode for single general-register transfers: false
 * for prefetches, vector-only or unallocated forms. */
static bool single_kind(uint32_t size, uint32_t opc, Op *op, bool *load) {
  op->size = (uint8_t)(1u << size);
  switch (opc) {
  case 0: *load = false; return true;
  case 1: *load = true; return true;
  case 2:
    if (size == 3) return false; /* prefetch */
    *load = true;
    op->flags |= F_SIGNED;
    return true;
  default:
    if (size >= 2) return false;
    *load = true;
    op->flags |= F_SIGNED32;
    return true;
  }
}

/* LDUR/STUR, pre/post-index, register offset (general registers). */
static bool decode_load_store_single(uint32_t insn, Op *op) {
  if (bits(insn, 29, 27) != 7u || bit(insn, 26) || bits(insn, 25, 24) != 0u) return false;
  bool load = false;
  if (!single_kind(bits(insn, 31, 30), bits(insn, 23, 22), op, &load)) return false;
  op->rd = (uint8_t)bits(insn, 4, 0);
  op->rn = (uint8_t)bits(insn, 9, 5);
  const uint32_t form = bits(insn, 11, 10);
  if (!bit(insn, 21)) {
    if (form == 2u) return false; /* unprivileged LDTR/STTR */
    op->imm = (uint64_t)sign_extend(bits(insn, 20, 12), 9);
    op->flags |= (uint8_t)((form == 1u || form == 3u ? F_WBACK : 0u) | (form == 1u ? F_POST : 0u));
    op->fn = load ? op_load_imm9 : op_store_imm9;
    return true;
  }
  if (form != 2u) return false; /* atomics, PAC loads */
  const uint32_t option = bits(insn, 15, 13);
  if (!(option & 2u)) return false;
  uint32_t scale = 0;
  while ((1u << scale) < op->size) scale++;
  op->rm = (uint8_t)bits(insn, 20, 16);
  op->aux = (uint8_t)option;
  op->ra = (uint8_t)(bit(insn, 12) ? scale : 0u);
  op->fn = load ? op_load_register : op_store_register;
  return true;
}

/* LDP/STP general registers (opc 00 W, 10 X), forms 01/10/11. */
static bool decode_pair(uint32_t insn, Op *op) {
  if (bits(insn, 29, 27) != 5u || bit(insn, 26)) return false;
  const uint32_t opc = bits(insn, 31, 30), form = bits(insn, 24, 23);
  if ((opc != 0 && opc != 2) || form == 0) return false;
  const uint32_t bytes = opc == 2 ? 8u : 4u;
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0), t2 = bits(insn, 14, 10);
  const bool load = bit(insn, 22), wback = form == 1 || form == 3;
  /* Writeback with an overlapping register, or LDP of one register
   * twice: leave the CONSTRAINED UNPREDICTABLE choices to the reference. */
  if (wback && n != REG_ZR && (t == n || t2 == n)) return false;
  if (load && t == t2) return false;
  op->rd = (uint8_t)t;
  op->ra = (uint8_t)t2;
  op->rn = (uint8_t)n;
  op->size = (uint8_t)bytes;
  op->imm = (uint64_t)sign_extend(bits(insn, 21, 15), 7) * bytes;
  op->flags = (uint8_t)((wback ? F_WBACK : 0u) | (form == 1 ? F_POST : 0u));
  op->fn = load ? op_load_pair : op_store_pair;
  return true;
}

static bool decode_dp_immediate(uint32_t insn, uint64_t pc, Op *op) {
  const bool sf = bit(insn, 31);
  op->flags = sf ? F_SF : 0u;
  op->rd = (uint8_t)bits(insn, 4, 0);
  op->rn = (uint8_t)bits(insn, 9, 5);
  switch (bits(insn, 28, 23)) {
  case 0x20: case 0x21: { /* ADR / ADRP */
    const uint64_t imm = (uint64_t)sign_extend((bits(insn, 23, 5) << 2) | bits(insn, 30, 29), 21);
    op->imm = bit(insn, 31) ? (pc & ~(uint64_t)0xFFF) + (imm << 12) : pc + imm;
    op->fn = op_adr;
    return true;
  }
  case 0x22: { /* add/sub immediate */
    const bool sub = bit(insn, 30), set_flags = bit(insn, 29);
    op->imm = (uint64_t)bits(insn, 21, 10) << (bit(insn, 22) ? 12 : 0);
    if (set_flags) {
      op->flags |= sub ? F_SUB : 0u;
      op->fn = op_adds_imm;
    } else {
      op->fn = sub ? op_sub_imm : op_add_imm;
    }
    return true;
  }
  case 0x24: { /* logical immediate */
    const uint32_t n = bit(insn, 22), opc = bits(insn, 30, 29);
    if (!sf && n) return false;
    uint64_t imm = 0, unused = 0;
    if (!interp_decode_bit_masks(n, bits(insn, 15, 10), bits(insn, 21, 16), true, sf, &imm, &unused)) return false;
    op->imm = imm;
    static const Op_Handler k_logical[4] = {op_and_imm, op_orr_imm, op_eor_imm, op_ands_imm};
    op->fn = k_logical[opc];
    return true;
  }
  case 0x26: { /* bitfield */
    const uint32_t opc = bits(insn, 30, 29), n = bit(insn, 22);
    const uint32_t immr = bits(insn, 21, 16), imms = bits(insn, 15, 10);
    if (opc == 3 || n != (uint32_t)sf || (!sf && ((immr | imms) & 0x20u))) return false;
    uint64_t wmask = 0, tmask = 0;
    if (!interp_decode_bit_masks(n, imms, immr, false, sf, &wmask, &tmask)) return false;
    op->imm = wmask;
    op->imm2 = tmask;
    op->size = (uint8_t)immr;
    op->aux = (uint8_t)imms;
    op->rm = (uint8_t)opc;
    op->fn = op_bitfield;
    return true;
  }
  case 0x25: { /* move wide */
    const uint32_t opc = bits(insn, 30, 29), hw = bits(insn, 22, 21);
    if (opc == 1 || (!sf && hw >= 2)) return false;
    const unsigned shift = hw * 16u;
    const uint64_t imm = (uint64_t)bits(insn, 20, 5) << shift;
    if (opc == 3) {
      op->imm = imm;
      op->size = (uint8_t)shift;
      op->fn = op_movk;
    } else {
      op->imm = (opc == 0 ? ~imm : imm) & width_mask(sf);
      op->fn = op_mov_const;
    }
    return true;
  }
  default:
    return false;
  }
}

static bool decode_dp_register(uint32_t insn, Op *op) {
  const bool sf = bit(insn, 31);
  op->flags = sf ? F_SF : 0u;
  op->rd = (uint8_t)bits(insn, 4, 0);
  op->rn = (uint8_t)bits(insn, 9, 5);
  op->rm = (uint8_t)bits(insn, 20, 16);
  const uint32_t amount = bits(insn, 15, 10), type = bits(insn, 23, 22);
  if (bits(insn, 28, 24) == 0x0A) { /* logical shifted register */
    if (!sf && amount >= 32u) return false;
    op->size = (uint8_t)amount;
    op->aux = (uint8_t)type;
    op->ra = (uint8_t)bit(insn, 21); /* invert */
    op->imm = bits(insn, 30, 29);
    op->fn = op_logical_shifted;
    return true;
  }
  if (bits(insn, 28, 24) == 0x0B && !bit(insn, 21)) { /* add/sub shifted register */
    if (type == 3u || (!sf && amount >= 32u)) return false;
    op->size = (uint8_t)amount;
    op->aux = (uint8_t)type;
    op->flags |= (uint8_t)((bit(insn, 30) ? F_SUB : 0u) | (bit(insn, 29) ? F_SET_FLAGS : 0u));
    op->fn = op_add_sub_shifted;
    return true;
  }
  if (bits(insn, 28, 21) == 0xD4) { /* conditional select */
    if (bit(insn, 29) || bit(insn, 11)) return false;
    op->size = (uint8_t)bits(insn, 15, 12);
    op->aux = (uint8_t)(bit(insn, 30) | (bit(insn, 10) << 1));
    op->fn = op_cond_select;
    return true;
  }
  if (bits(insn, 28, 24) == 0x1B && bits(insn, 23, 21) == 0 && bits(insn, 30, 29) == 0) { /* MADD/MSUB */
    op->ra = (uint8_t)bits(insn, 14, 10);
    op->flags |= bit(insn, 15) ? F_SUB : 0u;
    op->fn = op_multiply_add;
    return true;
  }
  return false;
}

static bool decode_branch(uint32_t insn, uint64_t pc, Op *op) {
  op->rd = (uint8_t)bits(insn, 4, 0);
  if (bits(insn, 30, 26) == 0x05) { /* B / BL */
    op->imm = pc + (uint64_t)sign_extend((uint64_t)bits(insn, 25, 0) << 2, 28);
    op->flags = bit(insn, 31) ? F_LINK : 0u;
    op->fn = op_branch;
    return true;
  }
  if (bits(insn, 31, 24) == 0x54) { /* B.cond */
    if (bit(insn, 4)) return false;
    op->size = (uint8_t)bits(insn, 3, 0);
    op->imm = pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21);
    op->fn = op_branch_cond;
    return true;
  }
  if (bits(insn, 30, 25) == 0x1A) { /* CBZ / CBNZ */
    op->flags = bit(insn, 31) ? F_SF : 0u;
    op->aux = (uint8_t)bit(insn, 24);
    op->imm = pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21);
    op->fn = op_compare_branch;
    return true;
  }
  if (bits(insn, 30, 25) == 0x1B) { /* TBZ / TBNZ */
    op->size = (uint8_t)((bit(insn, 31) << 5) | bits(insn, 23, 19));
    op->aux = (uint8_t)bit(insn, 24);
    op->imm = pc + (uint64_t)sign_extend((uint64_t)bits(insn, 18, 5) << 2, 16);
    op->fn = op_test_branch;
    return true;
  }
  /* BR (D61F0000), BLR (D63F0000), RET (D65F0000), no PAC variants. */
  const uint32_t masked = insn & 0xFFFFFC1Fu;
  if (masked == 0xD61F0000u || masked == 0xD63F0000u || masked == 0xD65F0000u) {
    op->rn = (uint8_t)bits(insn, 9, 5);
    op->flags = masked == 0xD63F0000u ? F_LINK : 0u;
    op->fn = op_branch_register;
    return true;
  }
  return false;
}

/* Each attempt starts from a clean op: a decoder that bails out part-way
 * leaves nothing behind for the next one. */
#define TRY(decoder)        \
  do {                      \
    op = reference_op(insn); \
    if (decoder) return op; \
  } while (0)

static Op decode(uint32_t insn, uint64_t pc) {
  Op op;
  switch (bits(insn, 28, 25)) {
  case 0x8: case 0x9: TRY(decode_dp_immediate(insn, pc, &op)); break;
  case 0xA: case 0xB: TRY(decode_branch(insn, pc, &op)); break;
  case 0x4: case 0x6: case 0xC: case 0xE:
    TRY(decode_load_store_unsigned(insn, &op));
    TRY(decode_load_store_single(insn, &op));
    TRY(decode_pair(insn, &op));
    break;
  case 0x5: case 0xD: TRY(decode_dp_register(insn, &op)); break;
  default: break;
  }
  return reference_op(insn);
}
#undef TRY

/* Does `insn` end a block? Branch/exception/system group. */
static bool ends_block(uint32_t insn) {
  const uint32_t op0 = bits(insn, 28, 25);
  return op0 == 0xA || op0 == 0xB;
}

/* Is it IC IVAU (SYS #3, C7, C5, #1, Xt)? Executing one drops the cache
 * afterwards, so code written and then synchronized is seen. Only the
 * instruction-cache invalidate can mean code changed: DC ZVA and the DC
 * clean/invalidate ops touch data, and blocks only come from pages that
 * are not writable, so any code change also needs an IC IVAU (or a
 * mapping change). Flushing on DC ZVA too - memset's hot loop - retired
 * every block thousands of times a frame. */
#define IC_IVAU_MASK 0xFFFFFFE0u /* everything but Rt */
#define IC_IVAU_ENCODING 0xD50B7520u
static bool is_cache_maintenance(uint32_t insn) { return (insn & IC_IVAU_MASK) == IC_IVAU_ENCODING; }

/* Builds the block at `pc`; NULL if the page is not cacheable. */
static Block *build(Block *cache, const Interp_State *s, uint64_t pc, uint64_t generation) {
  const uint64_t pte = vmm_pte_inline(s->l1, pc);
  if ((pte & VMM_PERM_X) == 0 || (pte & VMM_PERM_W) != 0 || (pc & 3u)) return NULL;
  VMM_Fault fault;
  const uint8_t *host = vmm_translate_inline(s->l1, pc, VMM_PERM_X, &fault);
  if (!host) return NULL;
  Block *b = &cache[PREDECODE_INDEX(pc)];
  b->pc = pc;
  b->count = 0;
  const uint64_t page_end = (pc | VMM_PAGE_OFFSET_MASK) + 1u;
  for (uint64_t at = pc; at < page_end && b->count < PREDECODE_BLOCK_OPS; at += 4u) {
    uint32_t insn;
    memcpy(&insn, host + (at - pc), sizeof(insn));
    b->ops[b->count++] = decode(insn, at);
    if (ends_block(insn)) break;
  }
  b->generation = generation;
  return b;
}

bool interp_predecode_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
#ifdef __EMSCRIPTEN__
    enabled = 1;
#else
    enabled = getenv("VOLAND_NO_PREDECODE") == NULL;
#endif
  }
  return enabled != 0;
}

bool interp_is_cache_maintenance(uint32_t insn) { return is_cache_maintenance(insn); }

uint64_t interp_code_generation(void) { return current_generation(); }

bool interp_predecode_run_block(Interp_State *s, uint64_t cycle_budget, uint32_t *grace, CPU_ExitReason *exit_reason) {
  const uint64_t generation = current_generation();
  const uint64_t pc = s->regs.pc;
  Block *const cache = thread_blocks();
  Block *b = NULL;
  if (cache) { /* no memory for this thread's cache: one instruction at a time */
    b = &cache[PREDECODE_INDEX(pc)];
    if (b->generation != generation || b->pc != pc) b = build(cache, s, pc, generation);
  }
  const uint32_t count = b ? b->count : 1u;
  for (uint32_t i = 0; i < count; i++) {
    /* Exactly the reference loop's budget/grace rule (interpreter.c),
     * before every instruction. */
    if (s->cycles_consumed >= cycle_budget) {
      if (!s->exclusive_valid || *grace >= INTERP_EXCLUSIVE_GRACE_INSTRUCTIONS) {
        *exit_reason = CPU_EXIT_CYCLES_ELAPSED;
        return false;
      }
      (*grace)++;
    }
    if (!b) return interp_run_one(s, exit_reason); /* uncacheable: one instruction, reference path */
    const Op *op = &b->ops[i];
    const uint64_t at = s->regs.pc;
    const Interp_Status status = op->fn(s, op);
    if (status == INTERP_CONTINUE) { /* the common case, retired inline */
      s->cycles_consumed++;
      s->total_cycles++;
    } else if (!interp_retire(s, status, at, op->insn, exit_reason)) {
      return false;
    }
    if (op->fn == op_reference && is_cache_maintenance(op->insn)) {
      interp_predecode_flush();
      return true;
    }
    /* Mappings only change in SVCs, which end this run (retire ->
     * false); another core's SVC is seen at the next block. */
  }
  return true;
}

CPU_ExitReason interp_predecode_execute(Interp_State *s, uint64_t cycle_budget) {
  uint32_t grace = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  while (interp_predecode_run_block(s, cycle_budget, &grace, &exit_reason)) {
  }
  return exit_reason;
}
