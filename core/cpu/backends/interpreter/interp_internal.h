/**
 * ARM64 interpreter internals (§8 CPU_BACKEND_INTERPRETER, §25 Phase 2).
 * Fetch-decode-execute over the softmmu's inline fast path (vmm.h); every
 * guest memory access goes through vmm_*_inline (§5). Instruction
 * semantics follow the Arm Architecture Reference Manual (DDI 0487)
 * pseudocode, reimplemented here - no emulator code is reused (CLAUDE.md
 * rule 2) - and are checked against vectors recorded on real ARM64
 * hardware (tests/tools/a64_oracle.c).
 *
 * Execution contract per instruction (interp_execute):
 *   - returns INTERP_CONTINUE after updating state and PC;
 *   - on a memory fault returns INTERP_FAULT with state UNCHANGED (all
 *     register writeback happens after the access succeeds) and
 *     fault_address set;
 *   - SVC/BRK/undefined return their own status; PC handling is the run
 *     loop's (interpreter.c).
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_INTERNAL_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "common/vmm.h"
#include "cpu/cpu.h"

typedef enum Interp_Status {
  INTERP_CONTINUE = 0,
  INTERP_SVC,       /* svc_immediate holds the SVC number; PC already advanced */
  INTERP_BREAKPOINT,
  INTERP_UNDEFINED, /* unallocated / unsupported encoding; PC unchanged */
  INTERP_FAULT,     /* memory fault; state unchanged; fault_address set */
} Interp_Status;

/* Bytes the exclusive monitor covers (an aligned 64-byte granule is the
 * architectural maximum; Cortex-A57's ERG is 64 bytes). */
#define INTERP_EXCLUSIVE_GRANULE 64u

/* Instructions the run loop keeps executing past the budget while an
 * exclusive monitor is held, so LDXR/STXR loops complete (§7). */
#define INTERP_EXCLUSIVE_GRACE_INSTRUCTIONS 64u

typedef struct Interp_State {
  CPU_Register_File regs;
  CPU_Vector_Register v[CPU_VECTOR_REGISTER_COUNT];
  uint32_t fpcr;
  uint32_t fpsr;
  uint64_t tpidr_el0;
  uint64_t tpidrro_el0;

  /* Local exclusive monitor (§7: per-thread, cleared at run() entry; the
   * run loop never yields while it is held, up to a grace cap). */
  bool exclusive_valid;
  uint64_t exclusive_address;

  uint64_t fault_address;
  uint64_t cycles_consumed;    /* by the last run()/step() */
  uint64_t total_cycles;       /* lifetime */
  uint64_t cntvct_base;        /* virtual time at the last set_sys_reg(CNTVCT) (§7) */
  uint64_t cntvct_origin;      /* total_cycles at that moment */
  uint32_t svc_immediate;

  VMM_Context *vmm;
  const uint64_t *l1;          /* vmm_page_table_l1(vmm), stable for the context */
  void *userdata;
  CPU_SVC_Handler svc_handler;
  CPU_Undefined_Handler undefined_handler;
  CPU_Breakpoint_Handler breakpoint_handler;
} Interp_State;

/* Executes the instruction `insn` at regs.pc. */
Interp_Status interp_execute(Interp_State *s, uint32_t insn);

/* Retires one instruction (cycle accounting, SVC/breakpoint/fault
 * handlers) and maps its status; true to keep running. */
bool interp_retire(Interp_State *s, Interp_Status status, uint64_t pc, uint32_t insn, CPU_ExitReason *exit_reason);
/* Fetch + decode + execute one instruction (the reference path). */
bool interp_run_one(Interp_State *s, CPU_ExitReason *exit_reason);

/* Predecoded execution (interp_predecode.c, §25 Phase 5): the run loop
 * over decoded blocks, chaining block to block until the budget is spent
 * (with the exclusive-monitor grace rule) or an instruction exits.
 * Results are identical to the reference loop (interp_run_one). */
CPU_ExitReason interp_predecode_execute(Interp_State *s, uint64_t cycle_budget);
/* Off when VOLAND_NO_PREDECODE is set in the environment (native only):
 * a switch for bisecting a suspected predecoder bug. */
bool interp_predecode_enabled(void);
/* Drops every decoded block (IC maintenance, invalidate_cache). */
void interp_predecode_flush(void);
/* One decoded block from regs.pc (the body of interp_predecode_execute's
 * loop, for the JIT's mixed-mode loop): false when the run must stop,
 * with *exit_reason set. `grace` is the run's exclusive-grace counter. */
bool interp_predecode_run_block(Interp_State *s, uint64_t cycle_budget, uint32_t *grace, CPU_ExitReason *exit_reason);
/* Changes whenever decoded code may be stale: any vmm mapping change or
 * a flush (IC maintenance, invalidate_cache, clear_cache). */
uint64_t interp_code_generation(void);
/* IC IVAU: flushes decoded code afterwards (interp_predecode.c). */
bool interp_is_cache_maintenance(uint32_t insn);

/* Instruction groups (A64 top-level encoding, DDI 0487 C4.1). */
Interp_Status interp_dp_immediate(Interp_State *s, uint32_t insn);
Interp_Status interp_branch_system(Interp_State *s, uint32_t insn);
Interp_Status interp_load_store(Interp_State *s, uint32_t insn);
Interp_Status interp_dp_register(Interp_State *s, uint32_t insn);
Interp_Status interp_simd_fp(Interp_State *s, uint32_t insn);
/* SIMD&FP loads/stores are dispatched from interp_load_store (V bit). */
Interp_Status interp_load_store_simd(Interp_State *s, uint32_t insn);

/* ------------------------------------------------------------------ */
/* Field extraction.                                                   */
/* ------------------------------------------------------------------ */

static inline uint32_t bits(uint32_t insn, unsigned hi, unsigned lo) {
  return (insn >> lo) & ((hi - lo == 31u) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1u)) - 1u));
}

static inline uint32_t bit(uint32_t insn, unsigned n) { return (insn >> n) & 1u; }

static inline int64_t sign_extend(uint64_t value, unsigned width) {
  const uint64_t m = (uint64_t)1 << (width - 1u);
  value &= (width == 64u) ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1u);
  return (int64_t)((value ^ m) - m);
}

/* ------------------------------------------------------------------ */
/* Register access. Field value 31 is XZR or SP by instruction context. */
/* ------------------------------------------------------------------ */

static inline uint64_t xreg(const Interp_State *s, uint32_t r) { return r == 31u ? 0 : s->regs.x[r]; }
static inline uint64_t xreg_sp(const Interp_State *s, uint32_t r) { return r == 31u ? s->regs.sp : s->regs.x[r]; }

static inline void set_xreg(Interp_State *s, uint32_t r, uint64_t value) {
  if (r != 31u) s->regs.x[r] = value;
}
static inline void set_xreg_sp(Interp_State *s, uint32_t r, uint64_t value) {
  if (r == 31u) s->regs.sp = value;
  else s->regs.x[r] = value;
}

/* 32-bit operations write zero-extended results. */
static inline uint64_t width_mask(bool sf) { return sf ? ~(uint64_t)0 : 0xFFFFFFFFull; }
static inline void set_reg_width(Interp_State *s, uint32_t r, bool sf, uint64_t value) {
  set_xreg(s, r, value & width_mask(sf));
}

/* ------------------------------------------------------------------ */
/* Flags and conditions.                                               */
/* ------------------------------------------------------------------ */

static inline void set_nzcv(Interp_State *s, uint32_t nzcv4) {
  s->regs.pstate = (s->regs.pstate & ~CPU_PSTATE_NZCV_MASK) | ((nzcv4 & 0xFu) << 28);
}
static inline uint32_t get_nzcv(const Interp_State *s) { return (s->regs.pstate >> 28) & 0xFu; }

/* ConditionHolds(cond) for the 4-bit cond field. */
static inline bool interp_condition_holds(const Interp_State *s, uint32_t cond) {
  const uint32_t nzcv = get_nzcv(s);
  const bool n = (nzcv & 8u) != 0, z = (nzcv & 4u) != 0, c = (nzcv & 2u) != 0, v = (nzcv & 1u) != 0;
  bool result;
  switch (cond >> 1) {
  case 0: result = z; break;
  case 1: result = c; break;
  case 2: result = n; break;
  case 3: result = v; break;
  case 4: result = c && !z; break;
  case 5: result = n == v; break;
  case 6: result = (n == v) && !z; break;
  default: result = true; break;
  }
  if ((cond & 1u) && cond != 0xFu) result = !result;
  return result;
}

/* AddWithCarry for 32/64-bit: returns the result, *nzcv = flags. */
uint64_t interp_add_with_carry(uint64_t x, uint64_t y, uint32_t carry_in, bool sf, uint32_t *nzcv);

/* DecodeBitMasks (logical immediates and bitfield moves). Returns false
 * for a reserved encoding. */
bool interp_decode_bit_masks(uint32_t n, uint32_t imms, uint32_t immr, bool immediate, bool sf,
                             uint64_t *wmask, uint64_t *tmask);

/* ------------------------------------------------------------------ */
/* Memory (all through vmm). On failure, fault_address is set.         */
/* ------------------------------------------------------------------ */

bool interp_read(Interp_State *s, uint64_t address, void *out, uint32_t size);
bool interp_write(Interp_State *s, uint64_t address, const void *data, uint32_t size);

/* System registers as MRS/MSR see them (CPU_SYSREG_* encodings). */
uint64_t interp_read_sys_reg(const Interp_State *s, uint32_t reg, bool *known);
bool interp_write_sys_reg(Interp_State *s, uint32_t reg, uint64_t value);

static inline uint64_t ror64(uint64_t value, unsigned amount, unsigned width) {
  const uint64_t mask = width == 64u ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1u);
  value &= mask;
  amount %= width;
  if (amount == 0) return value;
  return ((value >> amount) | (value << (width - amount))) & mask;
}

static inline Interp_Status advance(Interp_State *s) {
  s->regs.pc += 4u;
  return INTERP_CONTINUE;
}

/* Call tracing (interpreter.h): the targets, and the hook check. */
extern uint64_t g_interp_trace_targets[];
extern uint32_t g_interp_trace_count;
void interp_trace_call(Interp_State *s, uint64_t target);
void interp_trace_return(Interp_State *s, uint64_t target);
static inline void interp_maybe_trace_call(Interp_State *s, uint64_t target) {
  if (g_interp_trace_count) interp_trace_call(s, target);
}
static inline void interp_maybe_trace_return(Interp_State *s, uint64_t target) {
  if (g_interp_trace_count) interp_trace_return(s, target);
}

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_INTERNAL_H */
