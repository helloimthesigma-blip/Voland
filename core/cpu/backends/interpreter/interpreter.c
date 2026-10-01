/**
 * CPU_BACKEND_INTERPRETER: state, the bounded run loop (§7/§8), shared
 * helpers and the top-level A64 decode. Instruction groups live in the
 * interp_*.c files beside this one.
 */
#include "cpu/backends/interpreter/interpreter.h"

#include "common/assert.h"
#include "common/log.h"
#include "cpu/backends/interpreter/interp_internal.h"

#include <stdlib.h>
#include <string.h>

/* §7 starvation guard, layer 2: when the budget runs out while this
 * thread holds the exclusive monitor, keep going for at most this many
 * instructions so a LDAXR/STLXR pair is never split by preemption. */

/* Switch timer frequency (CNTFRQ_EL0) and the nominal CPU clock the
 * cycle counter is scaled from, until §7's virtual time owns CNTVCT. */
#define INTERP_CNTFRQ_HZ 19200000ull
#define INTERP_CPU_HZ 1020000000ull

/* CTR_EL0 as Cortex-A57 reports it (64-byte lines; DminLine/IminLine = 4)
 * and DCZID_EL0 with a 64-byte DC ZVA block (BS = 4). */
#define INTERP_CTR_EL0 0x8444C004ull
#define INTERP_DCZID_EL0 0x4ull

/* ------------------------------------------------------------------ */
/* Shared helpers.                                                     */
/* ------------------------------------------------------------------ */


uint64_t interp_add_with_carry(uint64_t x, uint64_t y, uint32_t carry_in, bool sf, uint32_t *nzcv) {
  if (sf) {
    const uint64_t r = x + y + carry_in;
    const bool c = (r < x) || (carry_in && r == x);
    const bool v = ((~(x ^ y) & (x ^ r)) >> 63) & 1u;
    *nzcv = ((r >> 63) << 3) | ((r == 0) << 2) | ((uint32_t)c << 1) | (uint32_t)v;
    return r;
  }
  const uint32_t a = (uint32_t)x, b = (uint32_t)y;
  const uint64_t wide = (uint64_t)a + (uint64_t)b + carry_in;
  const uint32_t r = (uint32_t)wide;
  const bool c = (wide >> 32) != 0;
  const bool v = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
  *nzcv = ((r >> 31) << 3) | ((r == 0) << 2) | ((uint32_t)c << 1) | (uint32_t)v;
  return r;
}

static uint64_t ones(unsigned n) { return n >= 64u ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1u); }

static uint64_t ror_bits(uint64_t value, unsigned amount, unsigned width) {
  amount %= width;
  if (amount == 0) return value;
  return ((value >> amount) | (value << (width - amount))) & ones(width);
}

static uint64_t replicate(uint64_t element, unsigned esize) {
  uint64_t out = 0;
  for (unsigned i = 0; i < 64u; i += esize) out |= element << i;
  return out;
}

bool interp_decode_bit_masks(uint32_t n, uint32_t imms, uint32_t immr, bool immediate, bool sf,
                             uint64_t *wmask, uint64_t *tmask) {
  const uint32_t combined = (n << 6) | (~imms & 0x3Fu);
  if (combined == 0) return false;
  int len = 6;
  while (len >= 0 && !((combined >> len) & 1u)) len--;
  if (len < 1) return false;
  if (!sf && len == 6) return false; /* 32-bit ops cannot use 64-bit elements */
  const uint32_t levels = (uint32_t)ones((unsigned)len);
  if (immediate && (imms & levels) == levels) return false;
  const uint32_t S = imms & levels;
  const uint32_t R = immr & levels;
  const unsigned esize = 1u << len;
  const uint32_t d = (S - R) & levels;
  const uint64_t welem = ones(S + 1u);
  const uint64_t telem = ones(d + 1u);
  const uint64_t w = replicate(ror_bits(welem, R, esize), esize);
  const uint64_t t = replicate(telem, esize);
  *wmask = sf ? w : (w & 0xFFFFFFFFull);
  *tmask = sf ? t : (t & 0xFFFFFFFFull);
  return true;
}

bool interp_read(Interp_State *s, uint64_t address, void *out, uint32_t size) {
  VMM_Fault fault;
  if (vmm_access_crosses_page(address, size)) {
    if (vmm_read_cross_page(s->l1, address, out, size, &fault)) return true;
  } else {
    const uint8_t *host = vmm_translate_inline(s->l1, address, VMM_PERM_R, &fault);
    if (host) {
      memcpy(out, host, size);
      return true;
    }
  }
  s->fault_address = fault.gva;
  return false;
}

bool interp_write(Interp_State *s, uint64_t address, const void *data, uint32_t size) {
  VMM_Fault fault;
  if (vmm_access_crosses_page(address, size)) {
    if (vmm_write_cross_page(s->l1, address, data, size, &fault)) return true;
  } else {
    uint8_t *host = vmm_translate_inline(s->l1, address, VMM_PERM_W, &fault);
    if (host) {
      memcpy(host, data, size);
      return true;
    }
  }
  s->fault_address = fault.gva;
  return false;
}

/* ------------------------------------------------------------------ */
/* Top-level decode (DDI 0487 C4.1, op0 = bits 28:25).                 */
/* ------------------------------------------------------------------ */

Interp_Status interp_execute(Interp_State *s, uint32_t insn) {
  const uint32_t op0 = bits(insn, 28, 25);
  switch (op0) {
  case 0x8: case 0x9:
    return interp_dp_immediate(s, insn);
  case 0xA: case 0xB:
    return interp_branch_system(s, insn);
  case 0x4: case 0x6: case 0xC: case 0xE:
    return interp_load_store(s, insn);
  case 0x5: case 0xD:
    return interp_dp_register(s, insn);
  case 0x7: case 0xF:
    return interp_simd_fp(s, insn);
  default:
    return INTERP_UNDEFINED; /* UDF, SVE, SME, unallocated */
  }
}

/* ------------------------------------------------------------------ */
/* Backend vtable.                                                     */
/* ------------------------------------------------------------------ */

static Interp_State *as_interp(CPU_State *state) { return (Interp_State *)state; }

static CPU_State *interp_create(VMM_Context *vmm, void *userdata) {
  Interp_State *s = (Interp_State *)calloc(1, sizeof(Interp_State));
  if (!s) return NULL;
  s->vmm = vmm;
  s->l1 = vmm ? vmm_page_table_l1(vmm) : NULL;
  s->userdata = userdata;
  return (CPU_State *)s;
}

static void interp_destroy(CPU_State *state) { free(state); }

/* Accounts one executed instruction and maps its status to an exit
 * reason. Returns true to keep going. Shared with the predecoded path. */
bool interp_retire(Interp_State *s, Interp_Status status, uint64_t pc, uint32_t insn, CPU_ExitReason *exit_reason) {
  s->cycles_consumed++;
  s->total_cycles++;
  switch (status) {
  case INTERP_CONTINUE:
    return true;
  case INTERP_SVC:
    if (s->svc_handler) s->svc_handler((CPU_State *)s, s->svc_immediate, s->userdata);
    *exit_reason = CPU_EXIT_SVC;
    return false;
  case INTERP_BREAKPOINT:
    if (s->breakpoint_handler) s->breakpoint_handler((CPU_State *)s, pc, s->userdata);
    *exit_reason = CPU_EXIT_BREAKPOINT;
    return false;
  case INTERP_UNDEFINED:
    s->cycles_consumed--;
    s->total_cycles--;
    s->fault_address = pc;
    if (s->undefined_handler) s->undefined_handler((CPU_State *)s, insn, s->userdata);
    *exit_reason = CPU_EXIT_FAULT;
    return false;
  case INTERP_FAULT:
    s->cycles_consumed--;
    s->total_cycles--;
    *exit_reason = CPU_EXIT_FAULT;
    return false;
  }
  return false;
}

/* One instruction through the full decoder (the reference path: step()
 * and anything the predecoded path does not cache). */
bool interp_run_one(Interp_State *s, CPU_ExitReason *exit_reason) {
  const uint64_t pc = s->regs.pc;
  uint32_t insn = 0;
  VMM_Fault fault;
  const uint8_t *host = (pc & 3u) ? NULL : vmm_translate_inline(s->l1, pc, VMM_PERM_X, &fault);
  if (!host) {
    s->fault_address = pc;
    *exit_reason = CPU_EXIT_FAULT;
    return false;
  }
  memcpy(&insn, host, sizeof(insn));
  return interp_retire(s, interp_execute(s, insn), pc, insn, exit_reason);
}

/* The reference loop: one instruction at a time through the decoder. */
static CPU_ExitReason run_reference(Interp_State *s, uint64_t cycle_budget) {
  uint32_t grace = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  for (;;) {
    if (s->cycles_consumed >= cycle_budget) {
      if (!s->exclusive_valid || grace >= INTERP_EXCLUSIVE_GRACE_INSTRUCTIONS) {
        return CPU_EXIT_CYCLES_ELAPSED;
      }
      grace++;
    }
    if (!interp_run_one(s, &exit_reason)) return exit_reason;
  }
}

/* Decoded blocks (interp_predecode.c) unless the predecoder is disabled,
 * in which case the reference loop runs - both give identical results. */
static CPU_ExitReason interp_run(CPU_State *state, uint64_t cycle_budget) {
  Interp_State *s = as_interp(state);
  SWITCH_ASSERT_ALWAYS(s->l1 != NULL, "interpreter run() without a vmm");
  s->cycles_consumed = 0;
  /* Entering run() is a potential context switch: the monitor of
   * whatever ran before is gone (§7). */
  s->exclusive_valid = false;
  return interp_predecode_enabled() ? interp_predecode_execute(s, cycle_budget) : run_reference(s, cycle_budget);
}

static CPU_ExitReason interp_step(CPU_State *state) {
  Interp_State *s = as_interp(state);
  s->cycles_consumed = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  (void)interp_run_one(s, &exit_reason);
  return exit_reason;
}

static uint64_t interp_get_fault_address(CPU_State *state) { return as_interp(state)->fault_address; }
static uint64_t interp_get_cycles_consumed(CPU_State *state) { return as_interp(state)->cycles_consumed; }

static uint64_t interp_get_reg(CPU_State *state, uint8_t index) {
  SWITCH_ASSERT(index <= 30, "interp_get_reg: index out of range");
  return as_interp(state)->regs.x[index];
}
static void interp_set_reg(CPU_State *state, uint8_t index, uint64_t value) {
  SWITCH_ASSERT(index <= 30, "interp_set_reg: index out of range");
  as_interp(state)->regs.x[index] = value;
}
static uint64_t interp_get_pc(CPU_State *state) { return as_interp(state)->regs.pc; }
static void interp_set_pc(CPU_State *state, uint64_t v) { as_interp(state)->regs.pc = v; }
static uint64_t interp_get_sp(CPU_State *state) { return as_interp(state)->regs.sp; }
static void interp_set_sp(CPU_State *state, uint64_t v) { as_interp(state)->regs.sp = v; }
static uint32_t interp_get_pstate(CPU_State *state) { return as_interp(state)->regs.pstate; }
static void interp_set_pstate(CPU_State *state, uint32_t v) {
  as_interp(state)->regs.pstate = v & CPU_PSTATE_NZCV_MASK;
}
static CPU_Register_File *interp_get_register_file(CPU_State *state) { return &as_interp(state)->regs; }

uint64_t interp_read_sys_reg(const Interp_State *s, uint32_t reg, bool *known) {
  *known = true;
  switch (reg) {
  case CPU_SYSREG_TPIDRRO_EL0: return s->tpidrro_el0;
  case CPU_SYSREG_TPIDR_EL0: return s->tpidr_el0;
  case CPU_SYSREG_NZCV: return s->regs.pstate & CPU_PSTATE_NZCV_MASK;
  case CPU_SYSREG_FPCR: return s->fpcr;
  case CPU_SYSREG_FPSR: return s->fpsr;
  case CPU_SYSREG_CNTFRQ_EL0: return INTERP_CNTFRQ_HZ;
  case CPU_SYSREG_CNTVCT_EL0:
  case CPU_SYSREG_CNTPCT_EL0:
    /* The scheduler's virtual time at run() entry plus this run's cycles. */
    return s->cntvct_base +
           (s->total_cycles - s->cntvct_origin) * (INTERP_CNTFRQ_HZ / 100000ull) / (INTERP_CPU_HZ / 100000ull);
  case CPU_SYSREG_CTR_EL0: return INTERP_CTR_EL0;
  case CPU_SYSREG_DCZID_EL0: return INTERP_DCZID_EL0;
  default:
    *known = false;
    return 0;
  }
}

bool interp_write_sys_reg(Interp_State *s, uint32_t reg, uint64_t value) {
  switch (reg) {
  case CPU_SYSREG_TPIDR_EL0: s->tpidr_el0 = value; return true;
  case CPU_SYSREG_NZCV: s->regs.pstate = (uint32_t)value & CPU_PSTATE_NZCV_MASK; return true;
  case CPU_SYSREG_FPCR: s->fpcr = (uint32_t)value; return true;
  case CPU_SYSREG_FPSR: s->fpsr = (uint32_t)value; return true;
  default: return false; /* TPIDRRO_EL0 is read-only at EL0 */
  }
}

static uint64_t interp_get_sys_reg(CPU_State *state, uint32_t reg) {
  bool known = false;
  return interp_read_sys_reg(as_interp(state), reg, &known);
}
static void interp_set_sys_reg(CPU_State *state, uint32_t reg, uint64_t value) {
  Interp_State *s = as_interp(state);
  /* The kernel (HLE) may set TPIDRRO_EL0; guest MSR may not. */
  if (reg == CPU_SYSREG_TPIDRRO_EL0) {
    s->tpidrro_el0 = value;
  } else if (reg == CPU_SYSREG_CNTVCT_EL0) { /* the scheduler publishes virtual time (§7) */
    s->cntvct_base = value;
    s->cntvct_origin = s->total_cycles;
  } else {
    (void)interp_write_sys_reg(s, reg, value);
  }
}

static CPU_Vector_Register interp_get_vector_reg(CPU_State *state, uint8_t index) {
  SWITCH_ASSERT(index < CPU_VECTOR_REGISTER_COUNT, "interp_get_vector_reg: index out of range");
  return as_interp(state)->v[index];
}
static void interp_set_vector_reg(CPU_State *state, uint8_t index, CPU_Vector_Register value) {
  SWITCH_ASSERT(index < CPU_VECTOR_REGISTER_COUNT, "interp_set_vector_reg: index out of range");
  as_interp(state)->v[index] = value;
}

/* No translation cache yet: fetch-decode-execute reads code through vmm
 * every time, so self-modifying code is always seen. The predecoded
 * interpreter (§25 Phase 5) gives these meaning. */
static void interp_invalidate_cache(CPU_State *state, uint64_t address, uint64_t size) {
  interp_predecode_flush();
  (void)state;
  (void)address;
  (void)size;
}
static void interp_clear_cache(CPU_State *state) {
  (void)state;
  interp_predecode_flush();
}

static void interp_set_svc_handler(CPU_State *state, CPU_SVC_Handler h) { as_interp(state)->svc_handler = h; }
static void interp_set_undefined_handler(CPU_State *state, CPU_Undefined_Handler h) {
  as_interp(state)->undefined_handler = h;
}
static void interp_set_breakpoint_handler(CPU_State *state, CPU_Breakpoint_Handler h) {
  as_interp(state)->breakpoint_handler = h;
}

const CPU_Backend CPU_BACKEND_INTERPRETER = {
    .create = interp_create,
    .destroy = interp_destroy,
    .run = interp_run,
    .step = interp_step,
    .get_fault_address = interp_get_fault_address,
    .get_cycles_consumed = interp_get_cycles_consumed,
    .get_reg = interp_get_reg,
    .set_reg = interp_set_reg,
    .get_pc = interp_get_pc,
    .set_pc = interp_set_pc,
    .get_sp = interp_get_sp,
    .set_sp = interp_set_sp,
    .get_pstate = interp_get_pstate,
    .set_pstate = interp_set_pstate,
    .get_register_file = interp_get_register_file,
    .get_sys_reg = interp_get_sys_reg,
    .set_sys_reg = interp_set_sys_reg,
    .get_vector_reg = interp_get_vector_reg,
    .set_vector_reg = interp_set_vector_reg,
    .invalidate_cache = interp_invalidate_cache,
    .clear_cache = interp_clear_cache,
    .set_svc_handler = interp_set_svc_handler,
    .set_undefined_handler = interp_set_undefined_handler,
    .set_breakpoint_handler = interp_set_breakpoint_handler,
    .name = "interpreter",
    .version = "0.1.0",
    .supports_jit = false,
};
