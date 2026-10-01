/**
 * Interpreter vs. hardware, instruction by instruction (ARM64 hosts only).
 *
 * For every template below, random instances (random fields, edge-case
 * register values, random NZCV, random memory) execute twice:
 *   - natively, in a JIT page via tests/a64_oracle_stub.S - the oracle;
 *   - in CPU_BACKEND_INTERPRETER, through vmm.
 * Guest memory is mapped at the SAME virtual addresses as the host's
 * buffers (code page and scratch), so pointers and PC-relative results
 * compare directly with no relocation. Every X register (but X18, which
 * Darwin reserves), NZCV, V0-V31, FPCR, FPSR and all scratch memory must
 * match; an instruction the CPU traps as undefined (SIGILL) or faulting
 * (SIGBUS/SIGSEGV) must be reported the same way by the interpreter, with
 * its state unchanged.
 *
 * The host is ARMv8.4+ (Apple M1 here); the Switch is ARMv8.0. Templates
 * stay inside ARMv8.0 encodings so "defined on the host" equals "defined
 * on the Switch" for everything generated.
 *
 *   a64_diff_test [iterations-per-template] [seed]
 */
#define CHECK_NAME "a64_diff_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/cpu.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ */
/* Oracle state and stub.                                              */
/* ------------------------------------------------------------------ */

typedef struct Oracle_State {
  uint64_t x[31];
  uint64_t nzcv;
  uint64_t fpcr;
  uint64_t fpsr;
  uint64_t pad[2];
  uint64_t v[32][2];
} Oracle_State;

_Static_assert(sizeof(Oracle_State) == 800, "layout must match a64_oracle_stub.S");

extern const uint8_t oracle_stub_start[];
extern const uint8_t oracle_stub_slot[];
extern const uint8_t oracle_stub_end[];

#define CODE_BYTES 0x8000u      /* JIT region: two 16KB host pages */
#define STUB_OFFSET 0x4000u     /* stub in the middle: literal loads reach both ways */
#define SCRATCH_BYTES 0x10000u  /* 64KB of data memory */
#define SCRATCH_BASE_LOW 0x4000u
#define SCRATCH_BASE_SPAN 0x8000u
#define CODE_GUEST_PA 0x0u
#define SCRATCH_GUEST_PA 0x100000u
#define NZCV_SHIFT 28u

static uint8_t *g_code;
static uint8_t *g_scratch;
static uint8_t g_scratch_initial[SCRATCH_BYTES];
static uint8_t g_scratch_guest_after[SCRATCH_BYTES];
static uint64_t g_slot_address;

static sigjmp_buf g_jump;
static volatile sig_atomic_t g_signal;

static void on_signal(int sig) {
  g_signal = sig;
  siglongjmp(g_jump, 1);
}

static void set_code_writable(bool writable) {
#if defined(__APPLE__)
  pthread_jit_write_protect_np(writable ? 0 : 1);
#else
  (void)writable;
#endif
}

static void flush_icache(void *start, size_t size) {
#if defined(__APPLE__)
  sys_icache_invalidate(start, size);
#else
  __builtin___clear_cache((char *)start, (char *)start + size);
#endif
}

static void setup_host(void) {
#if defined(__APPLE__)
  g_code = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
  g_code = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  CHECK(g_code != MAP_FAILED);
  g_scratch = mmap(NULL, SCRATCH_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(g_scratch != MAP_FAILED);
  CHECK((uint64_t)(uintptr_t)g_code + CODE_BYTES <= VMM_ADDRESS_SPACE_SIZE);
  CHECK((uint64_t)(uintptr_t)g_scratch + SCRATCH_BYTES <= VMM_ADDRESS_SPACE_SIZE);

  const size_t stub_bytes = (size_t)(oracle_stub_end - oracle_stub_start);
  set_code_writable(true);
  memset(g_code, 0, CODE_BYTES);
  memcpy(g_code + STUB_OFFSET, oracle_stub_start, stub_bytes);
  set_code_writable(false);
  flush_icache(g_code, CODE_BYTES);
  g_slot_address = (uint64_t)(uintptr_t)(g_code + STUB_OFFSET + (oracle_stub_slot - oracle_stub_start));

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGILL, &action, NULL) == 0);
  CHECK(sigaction(SIGBUS, &action, NULL) == 0);
  CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
}

typedef enum Outcome { OUTCOME_OK, OUTCOME_UNDEFINED, OUTCOME_FAULT } Outcome;

static Outcome run_host(uint32_t insn, Oracle_State *state) {
  set_code_writable(true);
  memcpy((void *)(uintptr_t)g_slot_address, &insn, sizeof(insn));
  set_code_writable(false);
  flush_icache((void *)(uintptr_t)g_slot_address, sizeof(insn));
  memcpy(g_scratch, g_scratch_initial, SCRATCH_BYTES);

  void (*stub)(Oracle_State *) = (void (*)(Oracle_State *))(void *)(g_code + STUB_OFFSET);
  g_signal = 0;
  if (sigsetjmp(g_jump, 1) == 0) {
    stub(state);
    return OUTCOME_OK;
  }
  __asm__ volatile("msr fpcr, xzr");
  return g_signal == SIGILL ? OUTCOME_UNDEFINED : OUTCOME_FAULT;
}

/* ------------------------------------------------------------------ */
/* Interpreter side.                                                   */
/* ------------------------------------------------------------------ */

static VMM_Context *g_vmm;
static const CPU_Backend *g_cpu;
static CPU_State *g_state;
static bool g_undefined_called;

static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
  g_undefined_called = true;
}

static void setup_guest(void) {
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, (uint64_t)(uintptr_t)g_code, CODE_GUEST_PA, CODE_BYTES, VMM_PERM_ALL));
  CHECK_OK(vmm_map(g_vmm, (uint64_t)(uintptr_t)g_scratch, SCRATCH_GUEST_PA, SCRATCH_BYTES, VMM_PERM_RW));
  CHECK_OK(vmm_write_block(g_vmm, (uint64_t)(uintptr_t)g_code, g_code, CODE_BYTES));
  g_cpu = &CPU_BACKEND_INTERPRETER;
  g_state = g_cpu->create(g_vmm, NULL);
  CHECK(g_state != NULL);
  g_cpu->set_undefined_handler(g_state, on_undefined);
}

static Outcome run_guest(uint32_t insn, const Oracle_State *pre, Oracle_State *post) {
  CHECK_OK(vmm_write32(g_vmm, g_slot_address, insn));
  CHECK_OK(vmm_write_block(g_vmm, (uint64_t)(uintptr_t)g_scratch, g_scratch_initial, SCRATCH_BYTES));
  for (uint8_t i = 0; i < 31; i++) g_cpu->set_reg(g_state, i, pre->x[i]);
  g_cpu->set_pstate(g_state, (uint32_t)pre->nzcv);
  g_cpu->set_sys_reg(g_state, CPU_SYSREG_FPCR, pre->fpcr);
  g_cpu->set_sys_reg(g_state, CPU_SYSREG_FPSR, pre->fpsr);
  for (uint8_t i = 0; i < 32; i++) {
    g_cpu->set_vector_reg(g_state, i, (CPU_Vector_Register){pre->v[i][0], pre->v[i][1]});
  }
  g_cpu->set_pc(g_state, g_slot_address);
  g_undefined_called = false;

  const CPU_ExitReason reason = g_cpu->step(g_state);

  for (uint8_t i = 0; i < 31; i++) post->x[i] = g_cpu->get_reg(g_state, i);
  post->nzcv = g_cpu->get_pstate(g_state);
  post->fpcr = g_cpu->get_sys_reg(g_state, CPU_SYSREG_FPCR);
  post->fpsr = g_cpu->get_sys_reg(g_state, CPU_SYSREG_FPSR);
  for (uint8_t i = 0; i < 32; i++) {
    const CPU_Vector_Register v = g_cpu->get_vector_reg(g_state, i);
    post->v[i][0] = v.lo;
    post->v[i][1] = v.hi;
  }
  CHECK_OK(vmm_read_block(g_vmm, (uint64_t)(uintptr_t)g_scratch, g_scratch_guest_after, SCRATCH_BYTES));

  if (reason == CPU_EXIT_FAULT) return g_undefined_called ? OUTCOME_UNDEFINED : OUTCOME_FAULT;
  CHECK(reason == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_pc(g_state) == g_slot_address + 4u);
  return OUTCOME_OK;
}

/* ------------------------------------------------------------------ */
/* Random generation.                                                  */
/* ------------------------------------------------------------------ */

static uint64_t g_rng;

static uint64_t rnd(void) {
  g_rng ^= g_rng >> 12;
  g_rng ^= g_rng << 25;
  g_rng ^= g_rng >> 27;
  return g_rng * 0x2545F4914F6CDD1Dull;
}
static uint32_t rnd_below(uint32_t n) { return (uint32_t)(rnd() % n); }
static uint32_t rbits(unsigned n) { return (uint32_t)(rnd() & ((1ull << n) - 1u)); }

/* X18 is the platform register on Darwin: never generated, never compared. */
static uint32_t reg(void) {
  uint32_t r;
  do r = rnd_below(31); while (r == 18);
  return r;
}
static uint32_t reg_or_zr(void) {
  uint32_t r;
  do r = rnd_below(32); while (r == 18);
  return r;
}

static uint64_t interesting_value(void) {
  static const uint64_t specials[] = {
      0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
      0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
      0xFFFFFFFF80000000ull, 0x00000000FFFFFFFEull,
  };
  switch (rnd_below(4)) {
  case 0: return specials[rnd_below(sizeof(specials) / sizeof(specials[0]))];
  case 1: return rnd() & 0xFFFF;
  case 2: return (uint64_t)(int64_t)(int32_t)(uint32_t)rnd();
  default: return rnd();
  }
}

/* What the state setup must do for one generated instruction. */
typedef struct Case {
  uint32_t insn;
  uint32_t base_regs;  /* bitmask: set to a pointer into scratch */
  uint32_t index_regs; /* bitmask: set to a small offset (0..255) */
  bool misalign;       /* allow base pointers that are not 16-aligned */
  /* Alignment-checked accesses (exclusive, acquire/release): the host has
   * FEAT_LSE2 (ARMv8.4), which lets a misaligned access that stays inside
   * one 16-byte block succeed; the Switch's ARMv8.0 Cortex-A57 always
   * faults, and so does the interpreter. Misalignments generated for
   * these cross a 16-byte boundary, where both architectures fault. */
  uint32_t checked_access_bytes;
  bool fp; /* V registers get FP edge values; FPCR/FPSR randomized */
  bool table_indices; /* TBL/TBX: index bytes mostly in range */
} Case;

typedef void (*Make_Fn)(Case *c);

#define REG_BIT(r) ((r) == 31u ? 0u : (1u << (r)))

/* --- Data processing - immediate ------------------------------------ */

static void make_adr(Case *c) {
  c->insn = 0x10000000u | (rbits(1) << 31) | (rbits(2) << 29) | (rbits(19) << 5) | reg_or_zr();
}
static void make_add_sub_imm(Case *c) {
  const uint32_t s = rbits(1);
  c->insn = 0x11000000u | (rbits(2) << 30) | (s << 29) | (rbits(1) << 22) | (rbits(12) << 10) |
            (reg() << 5) | (s ? reg_or_zr() : reg());
}
static void make_logical_imm(Case *c) {
  const uint32_t opc = rbits(2), sf = rbits(1);
  const uint32_t n = sf ? rbits(1) : (rnd_below(16) == 0);
  c->insn = 0x12000000u | (sf << 31) | (opc << 29) | (n << 22) | (rbits(6) << 16) | (rbits(6) << 10) |
            (reg_or_zr() << 5) | (opc == 3 ? reg_or_zr() : reg());
}
static void make_move_wide(Case *c) {
  c->insn = 0x12800000u | (rbits(3) << 29) | (rbits(2) << 21) | (rbits(16) << 5) | reg_or_zr();
}
static void make_bitfield(Case *c) {
  const uint32_t sf = rbits(1);
  const uint32_t n = rnd_below(10) == 0 ? rbits(1) : sf;
  const uint32_t width_bits = sf ? 6u : (rnd_below(10) == 0 ? 6u : 5u);
  c->insn = 0x13000000u | (sf << 31) | (rbits(2) << 29) | (n << 22) | (rbits(width_bits) << 16) |
            (rbits(width_bits) << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_extract(Case *c) {
  const uint32_t sf = rbits(1);
  c->insn = 0x13800000u | (sf << 31) | (sf << 22) | (reg_or_zr() << 16) | (rbits(sf ? 6u : 5u) << 10) |
            (reg_or_zr() << 5) | reg_or_zr();
}

/* --- Data processing - register -------------------------------------- */

static void make_logical_shifted(Case *c) {
  const uint32_t sf = rbits(1);
  c->insn = 0x0A000000u | (sf << 31) | (rbits(2) << 29) | (rbits(2) << 22) | (rbits(1) << 21) |
            (reg_or_zr() << 16) | (rbits(sf ? 6u : 5u) << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_add_sub_shifted(Case *c) {
  const uint32_t sf = rbits(1);
  c->insn = 0x0B000000u | (sf << 31) | (rbits(2) << 29) | (rnd_below(3) << 22) | (reg_or_zr() << 16) |
            (rbits(sf ? 6u : 5u) << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_add_sub_extended(Case *c) {
  const uint32_t s = rbits(1);
  c->insn = 0x0B200000u | (rbits(2) << 30) | (s << 29) | (reg_or_zr() << 16) | (rbits(3) << 13) |
            ((rnd_below(8) == 0 ? 5u + rnd_below(3) : rnd_below(5)) << 10) | (reg() << 5) |
            (s ? reg_or_zr() : reg());
}
static void make_add_sub_carry(Case *c) {
  c->insn = 0x1A000000u | (rbits(3) << 29) | (reg_or_zr() << 16) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_conditional_compare(Case *c) {
  c->insn = 0x3A400000u | (rbits(2) << 30) | (rbits(5) << 16) | (rbits(4) << 12) | (rbits(1) << 11) |
            (reg_or_zr() << 5) | rbits(4);
}
static void make_conditional_select(Case *c) {
  c->insn = 0x1A800000u | (rbits(2) << 30) | (reg_or_zr() << 16) | (rbits(4) << 12) | (rbits(1) << 10) |
            (reg_or_zr() << 5) | reg_or_zr();
}
static void make_dp_two_source(Case *c) {
  static const uint32_t opcodes[] = {2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 20, 21, 22, 23};
  c->insn = 0x1AC00000u | (rbits(1) << 31) | (reg_or_zr() << 16) |
            (opcodes[rnd_below(sizeof(opcodes) / sizeof(opcodes[0]))] << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_dp_one_source(Case *c) {
  c->insn = 0x5AC00000u | (rbits(1) << 31) | (rnd_below(6) << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_dp_three_source(Case *c) {
  static const uint32_t ops[] = {0, 0, 1, 2, 5, 6};
  const uint32_t op31 = ops[rnd_below(6)];
  const bool high = op31 == 2 || op31 == 6;
  c->insn = 0x1B000000u | ((op31 ? 1u : rbits(1)) << 31) | (op31 << 21) | (reg_or_zr() << 16) |
            ((high ? 0u : rbits(1)) << 15) | ((high ? 31u : reg_or_zr()) << 10) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_nzcv_transfer(Case *c) {
  c->insn = (rbits(1) ? 0xD53B4200u : 0xD51B4200u) | reg_or_zr(); /* MRS / MSR NZCV */
}

/* --- Loads and stores ------------------------------------------------ */

static void make_load_store_unsigned(Case *c) {
  const uint32_t n = reg();
  uint32_t t;
  do t = reg_or_zr(); while (t == n);
  c->insn = 0x39000000u | (rbits(2) << 30) | (rbits(1) << 26) | (rbits(2) << 22) | (rnd_below(0x200) << 10) |
            (n << 5) | t;
  c->base_regs = REG_BIT(n);
  c->misalign = true;
}
static void make_load_store_imm9(Case *c) {
  const uint32_t n = reg();
  uint32_t t;
  do t = reg_or_zr(); while (t == n);
  c->insn = 0x38000000u | (rbits(2) << 30) | (rbits(1) << 26) | (rbits(2) << 22) | (rbits(9) << 12) |
            (rbits(2) << 10) | (n << 5) | t;
  c->base_regs = REG_BIT(n);
  c->misalign = true;
}
static void make_load_store_register(Case *c) {
  static const uint32_t options[] = {2, 3, 6, 7, 2, 3, 6, 7, 0};
  const uint32_t n = reg();
  uint32_t m, t;
  do m = reg(); while (m == n);
  do t = reg_or_zr(); while (t == n || t == m);
  c->insn = 0x38200800u | (rbits(2) << 30) | (rbits(1) << 26) | (rbits(2) << 22) | (m << 16) |
            (options[rnd_below(9)] << 13) | (rbits(1) << 12) | (n << 5) | t;
  c->base_regs = REG_BIT(n);
  c->index_regs = REG_BIT(m);
  c->misalign = true;
}
static void make_load_store_pair(Case *c) {
  const uint32_t n = reg();
  uint32_t t, t2;
  do t = reg_or_zr(); while (t == n);
  do t2 = reg_or_zr(); while (t2 == n || t2 == t);
  const uint32_t imm7 = (uint32_t)((int32_t)rnd_below(33) - 16) & 0x7Fu;
  c->insn = 0x28000000u | (rbits(2) << 30) | (rbits(1) << 26) | (rbits(2) << 23) | (rbits(1) << 22) |
            (imm7 << 15) | (t2 << 10) | (n << 5) | t;
  c->base_regs = REG_BIT(n);
  c->misalign = true;
}
static void make_load_literal(Case *c) {
  const uint32_t imm19 = (uint32_t)((int32_t)rnd_below(513) - 256) & 0x7FFFFu;
  c->insn = 0x18000000u | (rbits(2) << 30) | (rbits(1) << 26) | (imm19 << 5) | reg_or_zr();
}
static void make_acquire_release(Case *c) {
  const uint32_t n = reg();
  const uint32_t size = rbits(2);
  c->insn = 0x089FFC00u | (size << 30) | (rbits(1) << 22) | (n << 5) | reg_or_zr(); /* LDAR/STLR */
  c->base_regs = REG_BIT(n);
  c->misalign = rnd_below(8) == 0;
  c->checked_access_bytes = 1u << size;
}
static void make_load_exclusive(Case *c) {
  const uint32_t n = reg();
  uint32_t t, t2;
  do t = reg_or_zr(); while (t == n);
  do t2 = reg_or_zr(); while (t2 == t);
  if (rbits(1)) { /* LDXR / LDAXR */
    const uint32_t size = rbits(2);
    c->insn = 0x085F7C00u | (size << 30) | (rbits(1) << 15) | (n << 5) | t;
    c->checked_access_bytes = 1u << size;
  } else {        /* LDXP / LDAXP */
    const uint32_t sz = rbits(1);
    c->insn = 0x887F0000u | (sz << 30) | (rbits(1) << 15) | (t2 << 10) | (n << 5) | t;
    c->checked_access_bytes = 8u << sz;
  }
  c->base_regs = REG_BIT(n);
  c->misalign = rnd_below(8) == 0;
}
static void make_dc_zva(Case *c) {
  const uint32_t t = reg();
  c->insn = 0xD50B7420u | t;
  c->base_regs = REG_BIT(t);
  c->misalign = true;
}

/* --- Scalar floating point ------------------------------------------- */

/* ftype 11 (half) is FEAT_FP16 arithmetic: the host has it, the Switch's
 * ARMv8.0 core does not. Generated only where ARMv8.0 defines it (FCVT). */
static uint32_t ftype_any(void) {
  static const uint32_t ftypes[] = {0, 1, 0, 1, 0, 1, 0, 2};
  return ftypes[rnd_below(8)];
}
static uint32_t ftype_with_half(void) {
  static const uint32_t ftypes[] = {0, 1, 3, 0, 1, 3, 2};
  return ftypes[rnd_below(7)];
}

static void make_fp_one_source(Case *c) {
  static const uint32_t opcodes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 3, 3, 8, 12, 14};
  const uint32_t opcode = opcodes[rnd_below(20)];
  const bool fcvt = (opcode & 0x3Cu) == 0x04u;
  c->insn = 0x1E204000u | ((fcvt ? ftype_with_half() : ftype_any()) << 22) | (opcode << 15) | (rbits(5) << 5) | rbits(5);
  c->fp = true;
}
static void make_fp_two_source(Case *c) {
  c->insn = 0x1E200800u | (ftype_any() << 22) | (rbits(5) << 16) | (rnd_below(10) << 12) | (rbits(5) << 5) | rbits(5);
  c->fp = true;
}
static void make_fp_three_source(Case *c) {
  c->insn = 0x1F000000u | (ftype_any() << 22) | (rbits(1) << 21) | (rbits(5) << 16) | (rbits(1) << 15) |
            (rbits(5) << 10) | (rbits(5) << 5) | rbits(5);
  c->fp = true;
}
static void make_fp_compare(Case *c) {
  static const uint32_t opcode2[] = {0x00, 0x08, 0x10, 0x18};
  c->insn = 0x1E202000u | (ftype_any() << 22) | (rbits(5) << 16) | (rbits(5) << 5) | opcode2[rnd_below(4)];
  c->fp = true;
}
static void make_fp_conditional(Case *c) {
  if (rbits(1)) { /* FCCMP / FCCMPE */
    c->insn = 0x1E200400u | (ftype_any() << 22) | (rbits(5) << 16) | (rbits(4) << 12) | (rbits(5) << 5) |
              (rbits(1) << 4) | rbits(4);
  } else {        /* FCSEL */
    c->insn = 0x1E200C00u | (ftype_any() << 22) | (rbits(5) << 16) | (rbits(4) << 12) | (rbits(5) << 5) | rbits(5);
  }
  c->fp = true;
}
static void make_fp_move_immediate(Case *c) {
  c->insn = 0x1E201000u | (ftype_any() << 22) | (rbits(8) << 13) | ((rnd_below(16) == 0 ? rbits(5) : 0u) << 5) | rbits(5);
  c->fp = true;
}
static void make_fp_int_conversion(Case *c) {
  static const uint32_t opcodes[] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3};
  const uint32_t opcode = opcodes[rnd_below(12)];
  uint32_t rmode = (opcode <= 1) ? rbits(2) : (rnd_below(8) == 0 ? rbits(2) : 0u);
  uint32_t ftype = ftype_any();
  uint32_t sf = rbits(1);
  if (opcode >= 6) rmode = 0; /* rmode 11 + opcode 110 is FJCVTZS (ARMv8.3): the host has it, the Switch does not */
  if (opcode >= 6 && rnd_below(4) == 0) { ftype = 2; sf = 1; rmode = 1; } /* FMOV to/from D[1] */
  c->insn = (sf << 31) | 0x1E200000u | (ftype << 22) | (rmode << 19) | (opcode << 16) | (reg_or_zr() << 5) | reg_or_zr();
  c->fp = true;
}
static void make_fp_fixed_conversion(Case *c) {
  static const uint32_t forms[][2] = {{3, 0}, {3, 1}, {0, 2}, {0, 3}};
  const uint32_t form = rnd_below(4);
  const uint32_t sf = rbits(1);
  const uint32_t scale = sf ? rbits(6) : (rnd_below(10) == 0 ? rbits(6) : 32u + rbits(5));
  c->insn = (sf << 31) | 0x1E000000u | (ftype_any() << 22) | (forms[form][0] << 19) | (forms[form][1] << 16) |
            (scale << 10) | (reg_or_zr() << 5) | reg_or_zr();
  c->fp = true;
}

/* --- Advanced SIMD ---------------------------------------------------- */

static uint32_t vreg(void) { return rbits(5); }
static uint32_t q_u(void) { return (rbits(1) << 30) | (rbits(1) << 29); }

static void make_simd_three_same_int(Case *c) {
  c->insn = 0x0E200400u | q_u() | (rbits(2) << 22) | (vreg() << 16) | (rnd_below(24) << 11) | (vreg() << 5) | vreg();
}
static void make_simd_three_same_fp(Case *c) {
  /* Not U=0 11101 / U=1 11001: FMLAL/FMLSL (FEAT_FHM, ARMv8.2) - host only. */
  uint32_t u, opcode;
  do {
    u = rbits(1);
    opcode = 24u + rnd_below(8);
  } while ((!u && opcode == 0x1D) || (u && opcode == 0x19));
  c->insn = 0x0E200400u | (rbits(1) << 30) | (u << 29) | (rbits(2) << 22) | (vreg() << 16) | (opcode << 11) |
            (vreg() << 5) | vreg();
  c->fp = true;
}
static void make_simd_two_misc(Case *c) {
  uint32_t opcode, size;
  do { /* FRINT32/64 (ARMv8.5): opcode 1111x with size<1> = 0 */
    opcode = rbits(5);
    size = rbits(2);
  } while (opcode >= 0x1E && !(size & 2u));
  c->insn = 0x0E200800u | q_u() | (size << 22) | (opcode << 12) | (vreg() << 5) | vreg();
  c->fp = opcode >= 12;
}
static void make_simd_across(Case *c) {
  static const uint32_t opcodes[] = {3, 10, 26, 27, 12, 15};
  const uint32_t opcode = opcodes[rnd_below(6)];
  /* FP across lanes with U = 0 is FP16 (host only). */
  const uint32_t u = opcode >= 12 ? 1u : rbits(1);
  c->insn = 0x0E300800u | (rbits(1) << 30) | (u << 29) | (rbits(2) << 22) | (opcode << 12) | (vreg() << 5) | vreg();
  c->fp = opcode >= 12;
}
static void make_simd_three_different(Case *c) {
  /* size 11 is PMULL.1Q (crypto) or unallocated: the host has crypto. */
  c->insn = 0x0E200000u | q_u() | (rnd_below(3) << 22) | (vreg() << 16) | (rbits(4) << 12) | (vreg() << 5) | vreg();
}
static void make_simd_shift_immediate(Case *c) {
  uint32_t immhb = 8u + rnd_below(120); /* immh != 0 */
  const uint32_t opcode = rbits(5);
  if (opcode >= 0x1C && immhb < 32u) immhb += 32u; /* FP16 fixed-point conversions: host only */
  c->insn = 0x0F000400u | q_u() | (immhb << 16) | (opcode << 11) | (vreg() << 5) | vreg();
  c->fp = opcode >= 0x1C;
}
static void make_simd_modified_immediate(Case *c) {
  c->insn = 0x0F000400u | q_u() | (rbits(3) << 16) | (rbits(4) << 12) | (0u << 11) /* o2: FP16 FMOV */ |
            (rbits(5) << 5) | vreg();
}
static void make_simd_copy(Case *c) {
  static const uint32_t imm4s[] = {0, 1, 3, 5, 7, 0, 1, 3, 5, 7, 2, 4};
  const uint32_t op = rnd_below(5) == 0;
  c->insn = 0x0E000400u | (rbits(1) << 30) | (op << 29) | (rbits(5) << 16) |
            ((op ? rbits(4) : imm4s[rnd_below(12)]) << 11) | (reg_or_zr() << 5) | reg_or_zr();
}
static void make_simd_permute(Case *c) {
  c->insn = 0x0E000800u | (rbits(1) << 30) | (rbits(2) << 22) | (vreg() << 16) | (rbits(3) << 12) | (vreg() << 5) | vreg();
}
static void make_simd_extract(Case *c) {
  c->insn = 0x2E000000u | (rbits(1) << 30) | (vreg() << 16) | (rbits(4) << 11) | (vreg() << 5) | vreg();
}
static void make_simd_table(Case *c) {
  c->insn = 0x0E000000u | (rbits(1) << 30) | (vreg() << 16) | (rbits(2) << 13) | (rbits(1) << 12) | (vreg() << 5) | vreg();
  c->table_indices = true;
}
/* ARMv8.0 by-element (U, opcode) pairs: the host also implements FMLAL
 * (FHM), SQRDMLAH (RDM), FCMLA, SDOT and FP16 forms, which the Switch
 * does not, so only these are generated. FP ones get size 1x. */
static const uint32_t k_by_element_v80[][2] = {
    {1, 0x0}, {1, 0x4}, {0, 0x2}, {1, 0x2}, {0, 0x3}, {0, 0x6}, {1, 0x6}, {0, 0x7}, {0, 0x8},
    {0, 0xA}, {1, 0xA}, {0, 0xB}, {0, 0xC}, {0, 0xD}, {0, 0x1}, {0, 0x5}, {0, 0x9}, {1, 0x9},
};
static uint32_t by_element_insn(uint32_t base, bool scalar, bool *fp) {
  const uint32_t *pick = k_by_element_v80[rnd_below(18)];
  const uint32_t u = pick[0], opcode = pick[1];
  *fp = opcode == 1 || opcode == 5 || opcode == 9;
  const uint32_t size = *fp ? (2u | rbits(1)) : rbits(2);
  return base | (scalar ? 0u : rbits(1) << 30) | (u << 29) | (size << 22) | (rbits(1) << 21) | (rbits(1) << 20) |
         (rbits(4) << 16) | (opcode << 12) | (rbits(1) << 11) | (vreg() << 5) | vreg();
}
static void make_simd_by_element(Case *c) {
  bool fp = false;
  c->insn = by_element_insn(0x0F000000u, false, &fp);
  c->fp = fp;
}
static void make_simd_scalar_three_same(Case *c) {
  const uint32_t opcode = rbits(5);
  c->insn = 0x5E200400u | (rbits(1) << 29) | (rbits(2) << 22) | (vreg() << 16) | (opcode << 11) | (vreg() << 5) | vreg();
  c->fp = opcode >= 24;
}
static void make_simd_scalar_two_misc(Case *c) {
  const uint32_t opcode = rbits(5);
  c->insn = 0x5E200800u | (rbits(1) << 29) | (rbits(2) << 22) | (opcode << 12) | (vreg() << 5) | vreg();
  c->fp = opcode >= 12;
}
static void make_simd_scalar_pairwise(Case *c) {
  static const uint32_t opcodes[] = {27, 12, 13, 15};
  const uint32_t opcode = opcodes[rnd_below(4)];
  /* FP pairwise with U = 0 is FP16 (host only). */
  c->insn = 0x5E300800u | ((opcode == 27 ? rbits(1) : 1u) << 29) | (rbits(2) << 22) | (opcode << 12) | (vreg() << 5) | vreg();
  c->fp = true;
}
static void make_simd_scalar_shift_immediate(Case *c) {
  const uint32_t opcode = rbits(5);
  uint32_t immhb = 8u + rnd_below(120);
  if (opcode >= 0x1C && immhb < 32u) immhb += 32u;
  c->insn = 0x5F000400u | (rbits(1) << 29) | (immhb << 16) | (opcode << 11) | (vreg() << 5) | vreg();
  c->fp = opcode >= 0x1C;
}
static void make_simd_scalar_three_different(Case *c) {
  static const uint32_t opcodes[] = {9, 11, 13, 9, 11, 13, 0, 5};
  c->insn = 0x5E200000u | (rbits(1) << 29) | (rbits(2) << 22) | (vreg() << 16) | (opcodes[rnd_below(8)] << 12) |
            (vreg() << 5) | vreg();
}
static void make_simd_scalar_by_element(Case *c) {
  bool fp = false;
  c->insn = by_element_insn(0x5F000000u, true, &fp);
  c->fp = fp;
}
static void make_simd_scalar_copy(Case *c) {
  c->insn = 0x5E000400u | ((rnd_below(10) == 0) << 29) | (rbits(5) << 16) | ((rnd_below(10) == 0 ? rbits(4) : 0u) << 11) |
            (vreg() << 5) | vreg();
}
static void make_simd_structure_multiple(Case *c) {
  static const uint32_t opcodes[] = {0, 2, 4, 6, 7, 8, 10, 7, 7, 3};
  const uint32_t n = reg();
  const bool post = rbits(1);
  uint32_t m = 31;
  if (post && rbits(1)) {
    do m = reg(); while (m == n);
    c->index_regs = REG_BIT(m);
  }
  c->insn = 0x0C000000u | (rbits(1) << 30) | ((uint32_t)post << 23) | (rbits(1) << 22) | ((post ? m : 0u) << 16) |
            (opcodes[rnd_below(10)] << 12) | (rbits(2) << 10) | (n << 5) | vreg();
  c->base_regs = REG_BIT(n);
  c->misalign = true;
}
static void make_simd_structure_single(Case *c) {
  const uint32_t n = reg();
  const bool post = rbits(1);
  uint32_t m = 31;
  if (post && rbits(1)) {
    do m = reg(); while (m == n);
    c->index_regs = REG_BIT(m);
  }
  c->insn = 0x0D000000u | (rbits(1) << 30) | ((uint32_t)post << 23) | (rbits(1) << 22) | (rbits(1) << 21) |
            ((post ? m : 0u) << 16) | (rbits(3) << 13) | (rbits(1) << 12) | (rbits(2) << 10) | (n << 5) | vreg();
  c->base_regs = REG_BIT(n);
  c->misalign = true;
}

typedef struct Template {
  const char *name;
  Make_Fn make;
} Template;

static const Template k_templates[] = {
    {"adr/adrp", make_adr},
    {"add/sub (immediate)", make_add_sub_imm},
    {"logical (immediate)", make_logical_imm},
    {"move wide", make_move_wide},
    {"bitfield", make_bitfield},
    {"extract", make_extract},
    {"logical (shifted register)", make_logical_shifted},
    {"add/sub (shifted register)", make_add_sub_shifted},
    {"add/sub (extended register)", make_add_sub_extended},
    {"add/sub with carry", make_add_sub_carry},
    {"conditional compare", make_conditional_compare},
    {"conditional select", make_conditional_select},
    {"data processing (2 source)", make_dp_two_source},
    {"data processing (1 source)", make_dp_one_source},
    {"data processing (3 source)", make_dp_three_source},
    {"mrs/msr nzcv", make_nzcv_transfer},
    {"load/store (unsigned offset)", make_load_store_unsigned},
    {"load/store (imm9 forms)", make_load_store_imm9},
    {"load/store (register offset)", make_load_store_register},
    {"load/store pair", make_load_store_pair},
    {"load literal", make_load_literal},
    {"ldar/stlr", make_acquire_release},
    {"load exclusive", make_load_exclusive},
    {"dc zva", make_dc_zva},
    {"fp (1 source)", make_fp_one_source},
    {"fp (2 source)", make_fp_two_source},
    {"fp (3 source, fused)", make_fp_three_source},
    {"fp compare", make_fp_compare},
    {"fp conditional compare/select", make_fp_conditional},
    {"fp move immediate", make_fp_move_immediate},
    {"fp <-> integer", make_fp_int_conversion},
    {"fp <-> fixed point", make_fp_fixed_conversion},
    {"simd three same (int)", make_simd_three_same_int},
    {"simd three same (fp)", make_simd_three_same_fp},
    {"simd two-reg misc", make_simd_two_misc},
    {"simd across lanes", make_simd_across},
    {"simd three different", make_simd_three_different},
    {"simd shift by immediate", make_simd_shift_immediate},
    {"simd modified immediate", make_simd_modified_immediate},
    {"simd copy", make_simd_copy},
    {"simd permute", make_simd_permute},
    {"simd extract", make_simd_extract},
    {"simd table lookup", make_simd_table},
    {"simd by element", make_simd_by_element},
    {"simd scalar three same", make_simd_scalar_three_same},
    {"simd scalar two-reg misc", make_simd_scalar_two_misc},
    {"simd scalar pairwise", make_simd_scalar_pairwise},
    {"simd scalar shift by imm", make_simd_scalar_shift_immediate},
    {"simd scalar three different", make_simd_scalar_three_different},
    {"simd scalar by element", make_simd_scalar_by_element},
    {"simd scalar copy", make_simd_scalar_copy},
    {"simd ld/st multiple", make_simd_structure_multiple},
    {"simd ld/st single", make_simd_structure_single},
};

/* ------------------------------------------------------------------ */
/* Comparison.                                                         */
/* ------------------------------------------------------------------ */

static uint64_t fp32_value(void) {
  static const uint32_t specials[] = {
      0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7FA00000, 0xFF800001,
      0x00000001, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000,
      0xBF800000, 0x3FC00000, 0x40000000, 0x3F000000, 0x40200000, 0x40600000, 0xBF000000, 0x4F000000,
      0xCF000000, 0x4F800000, 0x5F000000, 0xDF000000, 0x5F800000, 0x3EFFFFFF, 0x4B000000, 0x4AFFFFFF,
      0x00800001, 0x7E800000, 0x01000000, 0x34000000, 0x0C000000, 0x72000000,
  };
  switch (rnd_below(4)) {
  case 0: return specials[rnd_below(sizeof(specials) / sizeof(specials[0]))];
  case 1: return (uint32_t)rnd();
  case 2: /* moderate exponent, random mantissa */
    return ((uint32_t)rbits(1) << 31) | ((100u + rnd_below(56)) << 23) | rbits(23);
  default: /* near rounding boundaries: small integers plus fractions */
    return ((uint32_t)rbits(1) << 31) | ((120u + rnd_below(40)) << 23) | (rbits(3) << 20);
  }
}

static uint64_t fp64_value(void) {
  static const uint64_t specials[] = {
      0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull,
      0xFFF8000000000001ull, 0x7FF4000000000000ull, 0x7FF0000000000001ull, 1, 0x800FFFFFFFFFFFFFull,
      0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
      0x3FF8000000000000ull, 0x4000000000000000ull, 0x3FE0000000000000ull, 0x4004000000000000ull,
      0x41E0000000000000ull, 0xC1E0000000000000ull, 0x41F0000000000000ull, 0x43E0000000000000ull,
      0xC3E0000000000000ull, 0x43F0000000000000ull, 0x4330000000000000ull, 0x432FFFFFFFFFFFFFull,
      0x0010000000000001ull, 0x47EFFFFFE0000000ull, 0x36A0000000000000ull, 0x3810000000000000ull,
      0x380FFFFFFFFFFFFFull, 0x47F0000000000000ull,
  };
  switch (rnd_below(4)) {
  case 0: return specials[rnd_below(sizeof(specials) / sizeof(specials[0]))];
  case 1: return rnd();
  case 2: return ((uint64_t)rbits(1) << 63) | ((uint64_t)(900u + rnd_below(250)) << 52) | (rnd() & 0xFFFFFFFFFFFFFull);
  default: return ((uint64_t)rbits(1) << 63) | ((uint64_t)(1015u + rnd_below(60)) << 52) | ((uint64_t)rbits(3) << 49);
  }
}

static uint64_t random_fpcr(void) {
  static const uint64_t fz = 1u << 24, dn = 1u << 25;
  if (rnd_below(2)) return 0;
  return (rbits(1) ? fz : 0) | (rbits(1) ? dn : 0) | ((uint64_t)rbits(2) << 22);
}

static void random_state(const Case *c, Oracle_State *state) {
  memset(state, 0, sizeof(*state));
  for (uint32_t i = 0; i < 31; i++) state->x[i] = interesting_value();
  state->x[18] = 0;
  for (uint32_t i = 0; i < 31; i++) {
    if (c->base_regs & (1u << i)) {
      uint64_t offset = SCRATCH_BASE_LOW + (rnd() % SCRATCH_BASE_SPAN);
      if (!c->misalign || rnd_below(2)) offset &= ~(uint64_t)0xF;
      if (c->checked_access_bytes) {
        offset &= ~(uint64_t)0xF;
        if (c->misalign && c->checked_access_bytes > 1u) {
          offset += 16u - (1u + rnd_below(c->checked_access_bytes - 1u)); /* straddles 16 */
        }
      }
      state->x[i] = (uint64_t)(uintptr_t)g_scratch + offset;
    } else if (c->index_regs & (1u << i)) {
      state->x[i] = rnd() & 0xFFu;
    }
  }
  state->nzcv = (uint64_t)rbits(4) << NZCV_SHIFT;
  for (uint32_t i = 0; i < 32; i++) {
    state->v[i][0] = interesting_value();
    state->v[i][1] = interesting_value();
    if (c->fp) {
      state->v[i][0] = rbits(1) ? fp64_value() : ((rnd() << 32) | fp32_value());
      if (rnd_below(8) == 0) state->v[i][0] = (state->v[i][0] & ~0xFFFFull) | (rnd() & 0xFFFFu); /* halves */
    }
  }
  if (c->table_indices) {
    for (uint32_t i = 0; i < 32; i++) {
      state->v[i][0] &= 0x3F3F3F3F3F3F3F3Full;
      state->v[i][1] &= rbits(1) ? 0x3F3F3F3F3F3F3F3Full : ~0ull;
    }
  }
  if (!c->fp) state->fpsr = rnd_below(4) == 0 ? (1u << 27) : 0; /* QC */
  if (c->fp) {
    state->fpcr = random_fpcr();
    state->fpsr = rnd_below(4) == 0 ? (rnd() & 0x9Fu) : 0;
  }
}

static bool compare(const char *name, uint32_t insn, Outcome host_outcome, Outcome guest_outcome,
                    const Oracle_State *pre, const Oracle_State *host, const Oracle_State *guest) {
  bool ok = true;
  if (host_outcome != guest_outcome) {
    fprintf(stderr, "MISMATCH %s %08x: hardware %s, interpreter %s\n", name, insn,
            host_outcome == OUTCOME_OK ? "executed" : host_outcome == OUTCOME_UNDEFINED ? "undefined" : "faulted",
            guest_outcome == OUTCOME_OK ? "executed" : guest_outcome == OUTCOME_UNDEFINED ? "undefined" : "faulted");
    return false;
  }
  if (host_outcome != OUTCOME_OK) host = pre; /* a trapping instruction changes nothing */
  for (uint32_t i = 0; i < 31; i++) {
    if (i == 18) continue;
    if (host->x[i] != guest->x[i]) {
      fprintf(stderr, "MISMATCH %s %08x: x%u hardware %016llx interpreter %016llx (pre %016llx)\n", name, insn, i,
              (unsigned long long)host->x[i], (unsigned long long)guest->x[i], (unsigned long long)pre->x[i]);
      ok = false;
    }
  }
  if ((host->nzcv & 0xF0000000u) != (guest->nzcv & 0xF0000000u)) {
    fprintf(stderr, "MISMATCH %s %08x: nzcv hardware %llx interpreter %llx\n", name, insn,
            (unsigned long long)(host->nzcv >> 28), (unsigned long long)(guest->nzcv >> 28));
    ok = false;
  }
  if (host->fpsr != guest->fpsr) {
    fprintf(stderr, "MISMATCH %s %08x: fpsr hardware %llx interpreter %llx\n", name, insn,
            (unsigned long long)host->fpsr, (unsigned long long)guest->fpsr);
    ok = false;
  }
  for (uint32_t i = 0; i < 32; i++) {
    if (host->v[i][0] != guest->v[i][0] || host->v[i][1] != guest->v[i][1]) {
      fprintf(stderr, "MISMATCH %s %08x: v%u hardware %016llx%016llx interpreter %016llx%016llx\n", name, insn, i,
              (unsigned long long)host->v[i][1], (unsigned long long)host->v[i][0],
              (unsigned long long)guest->v[i][1], (unsigned long long)guest->v[i][0]);
      ok = false;
    }
  }
  const uint8_t *host_memory = host_outcome == OUTCOME_OK ? g_scratch : g_scratch_initial;
  if (memcmp(host_memory, g_scratch_guest_after, SCRATCH_BYTES) != 0) {
    for (uint32_t i = 0; i < SCRATCH_BYTES; i++) {
      if (host_memory[i] != g_scratch_guest_after[i]) {
        fprintf(stderr, "MISMATCH %s %08x: memory +0x%x hardware %02x interpreter %02x\n", name, insn, i,
                host_memory[i], g_scratch_guest_after[i]);
        break;
      }
    }
    ok = false;
  }
  return ok;
}

int main(int argc, char **argv) {
  const uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 3000u;
  g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x5EED5EED12345678ull;
  if (g_rng == 0) g_rng = 1;
  const uint64_t seed = g_rng;

  setup_host();
  setup_guest();

  uint32_t failures = 0;
  for (size_t t = 0; t < sizeof(k_templates) / sizeof(k_templates[0]); t++) {
    uint32_t executed = 0, undefined = 0, faulted = 0, template_failures = 0;
    for (uint32_t i = 0; i < iterations; i++) {
      for (uint32_t b = 0; b < SCRATCH_BYTES; b++) g_scratch_initial[b] = (uint8_t)rnd();
      Case c;
      memset(&c, 0, sizeof(c));
      k_templates[t].make(&c);
      Oracle_State pre, host, guest;
      random_state(&c, &pre);
      host = pre;
      const Outcome host_outcome = run_host(c.insn, &host);
      const Outcome guest_outcome = run_guest(c.insn, &pre, &guest);
      if (!compare(k_templates[t].name, c.insn, host_outcome, guest_outcome, &pre, &host, &guest)) {
        template_failures++;
        if (template_failures >= 5) break;
      }
      executed += host_outcome == OUTCOME_OK;
      undefined += host_outcome == OUTCOME_UNDEFINED;
      faulted += host_outcome == OUTCOME_FAULT;
    }
    printf("[a64_diff_test] %-30s %5u executed %4u undefined %4u faulted  %s\n", k_templates[t].name, executed,
           undefined, faulted, template_failures ? "FAIL" : "ok");
    failures += template_failures;
  }
  printf("[a64_diff_test] seed 0x%llx, %u iterations per template: %s\n", (unsigned long long)seed, iterations,
         failures ? "FAILED" : "passed");
  return failures ? 1 : 0;
}
