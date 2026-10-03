/**
 * JIT vs the interpreter (docs/JIT.md): random instruction streams run
 * once through the interpreter's run() - itself checked against the
 * one-instruction reference decoder by predecode_test - and once through
 * CPU_BACKEND_JIT's run() with a hot threshold of 1, so every block is
 * compiled to wasm on first sight, both with the same sequence of budgets
 * (so the exclusive-monitor grace past a budget is compared too). Registers, SP, flags, PC, exit reason,
 * fault address, cycle count and every data byte must match. Budgets are
 * varied so compiled blocks are both entered and refused (budget too
 * short), and streams loop back on themselves so blocks run repeatedly.
 *
 * Natively nothing is compiled (no wasm engine), so this checks the mixed-
 * mode loop alone; the Emscripten build (tests/jit_wasm, run under Node)
 * checks the compiled code.
 *
 *   jit_diff_test [iterations] [seed]
 */
#define CHECK_NAME "jit_diff_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/backends/jit/jit_internal.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_GVA 0x100000ull
#define DATA_GVA 0x200000ull
#define CODE_PA 0x0ull
#define DATA_PA 0x10000ull
#define DATA_BYTES 0x4000u          /* 4 pages: accesses that cross pages */
#define STREAM_MAX 48u
#define STEP_LIMIT 400u
#define SVC_ZERO 0xD4000001u
#define DEFAULT_ITERATIONS 20000u

static VMM_Context *g_vmm;
static const CPU_Backend *g_ref_cpu, *g_jit_cpu;
static CPU_State *g_ref, *g_jit;
static uint64_t g_rng;
static uint8_t g_data_init[DATA_BYTES], g_data_ref[DATA_BYTES];

static uint64_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}
static uint32_t pick(uint32_t n) { return (uint32_t)(rnd() % n); }

static void on_svc(CPU_State *state, uint32_t swi, void *userdata) {
  (void)state;
  (void)swi;
  (void)userdata;
}
static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
}

/* ------------------------------------------------------------------ */
/* Instruction templates (field layouts: DDI 0487 C4).                 */
/* ------------------------------------------------------------------ */

static uint32_t reg(void) { return pick(32); }
/* Bases for memory forms: x20-x23 (pointers into data) or SP. */
static uint32_t base_reg(void) { const uint32_t r = pick(5); return r == 4 ? 31u : 20u + r; }
/* Destinations that keep the data pointers (x20-x23) intact, mostly. */
static uint32_t dst(void) { const uint32_t r = pick(32); return (r >= 20 && r < 24 && pick(4)) ? r - 10u : r; }

static uint32_t gen_add_sub_imm(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0x22u << 23) | (pick(2) << 22) |
         ((uint32_t)pick(4096) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_logical_imm(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x24u << 23) | (pick(2) << 22) | (pick(64) << 16) | (pick(64) << 10) |
         (reg() << 5) | dst();
}
static uint32_t gen_move_wide(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x25u << 23) | (pick(4) << 21) | (pick(65536) << 5) | dst();
}
static uint32_t gen_bitfield(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x26u << 23) | (pick(2) << 22) | (pick(64) << 16) | (pick(64) << 10) |
         (reg() << 5) | dst();
}
static uint32_t gen_extract(void) {
  const uint32_t sf = pick(2);
  return (sf << 31) | (0x27u << 23) | (sf << 22) | (reg() << 16) | (pick(sf ? 64 : 32) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_adr(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x10u << 24) | (pick(1u << 19) << 5) | dst();
}
static uint32_t gen_add_sub_shifted(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0x0Bu << 24) | (pick(4) << 22) | (reg() << 16) |
         (pick(64) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_add_sub_extended(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0x59u << 21) | (pick(4) == 0 ? pick(4) << 22 : 0) |
         (reg() << 16) | (pick(8) << 13) | (pick(6) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_logical_shifted(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x0Au << 24) | (pick(4) << 22) | (pick(2) << 21) | (reg() << 16) |
         (pick(64) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_add_sub_carry(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0xD0u << 21) | (reg() << 16) |
         (pick(8) == 0 ? pick(64) << 10 : 0) | (reg() << 5) | dst();
}
static uint32_t gen_cond_compare(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(8) ? 1u << 29 : 0) | (0xD2u << 21) | (reg() << 16) |
         (pick(16) << 12) | (pick(2) << 11) | (pick(8) == 0 ? 1u << 10 : 0) | (reg() << 5) |
         (pick(8) == 0 ? 1u << 4 : 0) | pick(16);
}
static uint32_t gen_cond_select(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(4) == 0 ? 1u << 29 : 0) | (0xD4u << 21) | (reg() << 16) |
         (pick(16) << 12) | (pick(4) == 0 ? 1u << 11 : 0) | (pick(2) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_dp_two_source(void) {
  static const uint32_t opcodes[8] = {0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x16};
  return (pick(2) << 31) | (0xD6u << 21) | (reg() << 16) | (opcodes[pick(8)] << 10) | (reg() << 5) | dst();
}
static uint32_t gen_dp_one_source(void) {
  return (pick(2) << 31) | (1u << 30) | (0xD6u << 21) | (pick(6) << 10) | (reg() << 5) | dst();
}
static uint32_t gen_madd(void) {
  static const uint32_t op31[5] = {0, 1, 5, 2, 6};
  const uint32_t o = op31[pick(5)];
  return ((o ? 1u : pick(2)) << 31) | (0x1Bu << 24) | (o << 21) | (reg() << 16) | ((o == 2 || o == 6) ? 0 : pick(2) << 15) |
         (reg() << 10) | (reg() << 5) | dst();
}
static int32_t small_branch(void) { return (int32_t)pick(17) - 8; } /* within the stream, mostly */
static uint32_t gen_branch(void) {
  switch (pick(7)) {
  case 0: return (pick(2) << 31) | (0x05u << 26) | ((uint32_t)small_branch() & 0x3FFFFFFu);
  case 1: return (0x54u << 24) | (((uint32_t)small_branch() & 0x7FFFFu) << 5) | pick(16);
  case 2: return (pick(2) << 31) | (0x1Au << 25) | (pick(2) << 24) | (((uint32_t)small_branch() & 0x7FFFFu) << 5) | reg();
  case 3: return (pick(2) << 31) | (0x1Bu << 25) | (pick(2) << 24) | (pick(32) << 19) |
                 (((uint32_t)small_branch() & 0x3FFFu) << 5) | reg();
  case 4: return 0xD65F0000u | (reg() << 5); /* RET xN */
  case 5: return (pick(2) ? 0xD61F0000u : 0xD63F0000u) | (28u << 5); /* BR/BLR x28: into the stream */
  default: return (pick(2) ? 0xD61F0000u : 0xD63F0000u) | ((20u + pick(4)) << 5); /* BR/BLR to data: faults */
  }
}
static uint32_t gen_system(void) {
  switch (pick(8)) {
  case 0: return 0xD503201Fu;                                   /* NOP */
  case 1: return 0xD5033BBFu;                                   /* DMB ISH */
  case 2: return 0xD503305Fu;                                   /* CLREX */
  case 3: return 0xD53B4200u | dst();                           /* MRS xN, NZCV */
  case 4: return 0xD51B4200u | reg();                           /* MSR NZCV, xN */
  case 5: return 0xD53BD040u | dst();                           /* MRS xN, TPIDR_EL0 */
  case 6: return 0xD53BE040u | dst();                           /* MRS xN, CNTVCT_EL0 */
  default: return 0xD51BD040u | reg();                          /* MSR TPIDR_EL0, xN */
  }
}
static uint32_t gen_load_store_unsigned(void) {
  return (pick(4) << 30) | (0x39u << 24) | (pick(4) << 22) | (pick(64) << 10) | (base_reg() << 5) | reg();
}
static uint32_t gen_load_store_single(void) {
  if (pick(2)) { /* imm9: unscaled / post / pre (and the unprivileged form) */
    return (pick(4) << 30) | (0x38u << 24) | (pick(4) << 22) | ((uint32_t)(pick(512)) << 12) | (pick(4) << 10) |
           (base_reg() << 5) | reg();
  }
  const uint32_t options[4] = {2, 3, 6, 7};
  return (pick(4) << 30) | (0x38u << 24) | (pick(4) << 22) | (1u << 21) | ((24u + pick(4)) << 16) |
         (options[pick(4)] << 13) | (pick(2) << 12) | (2u << 10) | (base_reg() << 5) | reg();
}
static uint32_t gen_pair(void) {
  return (pick(4) << 30) | (0x5u << 27) | (pick(4) << 23) | (pick(2) << 22) | ((uint32_t)pick(128) << 15) |
         (reg() << 10) | (base_reg() << 5) | reg();
}
static uint32_t gen_literal(void) {
  /* Mostly into the code page (readable), sometimes far away (faults). */
  const uint32_t imm19 = pick(4) ? pick(256) : pick(1u << 19);
  return (pick(4) << 30) | (0x18u << 24) | (imm19 << 5) | dst();
}
static uint32_t gen_simd_ldst(void) {
  const uint32_t v = pick(32);
  switch (pick(5)) {
  case 0: /* LDR/STR (SIMD&FP, unsigned offset) */
    return (pick(4) << 30) | (0x3Du << 24) | (pick(2) << 23) | (pick(2) << 22) | (pick(64) << 10) | (base_reg() << 5) | v;
  case 1: /* imm9: unscaled / post / pre (and the unallocated unprivileged form) */
    return (pick(4) << 30) | (0x3Cu << 24) | (pick(2) << 23) | (pick(2) << 22) | (pick(512) << 12) | (pick(4) << 10) |
           (base_reg() << 5) | v;
  case 2: { /* register offset */
    const uint32_t options[4] = {2, 3, 6, 7};
    return (pick(4) << 30) | (0x3Cu << 24) | (pick(2) << 23) | (pick(2) << 22) | (1u << 21) | ((24u + pick(4)) << 16) |
           (options[pick(4)] << 13) | (pick(2) << 12) | (2u << 10) | (base_reg() << 5) | v;
  }
  case 3: /* LDP/STP (SIMD&FP) */
    return (pick(4) << 30) | (0x2Cu << 24) | (pick(4) << 23) | (pick(2) << 22) | (pick(128) << 15) | (pick(32) << 10) |
           (base_reg() << 5) | v;
  default: /* LDR (literal, SIMD&FP) */
    return (pick(4) << 30) | (0x1Cu << 24) | ((pick(4) ? pick(256) : pick(1u << 19)) << 5) | v;
  }
}
/* LDXR/STXR/LDAXR/STLXR/LDAR/STLR (and the odd pair/undefined form). */
static uint32_t gen_exclusive(void) {
  const uint32_t o2 = pick(3) == 0, o1 = pick(8) == 0, o0 = o2 ? (pick(8) != 0) : pick(2);
  return (pick(4) << 30) | (0x08u << 24) | (o2 << 23) | (pick(2) << 22) | (o1 << 21) | (dst() << 16) | (o0 << 15) |
         (31u << 10) | (base_reg() << 5) | reg();
}

/* SIMD&FP data processing and structure loads/stores (direct calls). */
static uint32_t vreg(void) { return pick(32); }
static uint32_t ftype(void) { return pick(8) == 0 ? 3u : pick(2); } /* single/double, sometimes half/undefined */
static uint32_t gen_simd_fp(void) {
  switch (pick(14)) {
  case 0: return 0x1E200800u | (ftype() << 22) | (vreg() << 16) | (pick(9) << 12) | (vreg() << 5) | vreg(); /* 2-source */
  case 1: return 0x1E204000u | (ftype() << 22) | (pick(16) << 15) | (vreg() << 5) | vreg();               /* 1-source */
  case 2: return 0x1F000000u | (ftype() << 22) | (pick(2) << 21) | (vreg() << 16) | (pick(2) << 15) | (vreg() << 10) |
                 (vreg() << 5) | vreg();                                                                    /* FMADD & co. */
  case 3: return 0x1E202000u | (ftype() << 22) | (vreg() << 16) | (vreg() << 5) | (pick(4) << 3);         /* FCMP(E) */
  case 4: return 0x1E200400u | (ftype() << 22) | (vreg() << 16) | (pick(16) << 12) | (vreg() << 5) | (pick(2) << 4) |
                 pick(16);                                                                                  /* FCCMP(E) */
  case 5: return 0x1E200C00u | (ftype() << 22) | (vreg() << 16) | (pick(16) << 12) | (vreg() << 5) | vreg(); /* FCSEL */
  case 6: { /* FMOV/SCVTF/UCVTF/FCVT*S/U (general) */
    static const uint32_t opcodes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    return (pick(2) << 31) | 0x1E200000u | (ftype() << 22) | (pick(4) << 19) | (opcodes[pick(8)] << 16) | (reg() << 5) |
           dst();
  }
  case 7: return 0x1E201000u | (ftype() << 22) | (pick(256) << 13) | vreg(); /* FMOV (immediate) */
  case 8: return 0x0E200400u | (pick(2) << 30) | (pick(2) << 29) | (pick(4) << 22) | (vreg() << 16) | (pick(32) << 11) |
                 (vreg() << 5) | vreg();                                                    /* three same */
  case 9: return 0x0E201800u | (pick(2) << 30) | (pick(2) << 29) | (pick(4) << 22) | (pick(32) << 12) | (vreg() << 5) |
                 vreg();                                                                    /* two-reg misc */
  case 10: { /* copy: DUP/INS/SMOV/UMOV, general and element */
    const uint32_t general = pick(2);
    return 0x0E000400u | (pick(2) << 30) | ((general ? 0u : pick(2)) << 29) | ((1u + pick(31)) << 16) |
           ((general ? (pick(4) * 2u + 1u) : pick(16)) << 11) | ((general ? reg() : vreg()) << 5) |
           (general ? dst() : vreg());
  }
  case 11: return 0x1E220000u | (pick(2) << 31) | (pick(2) << 22) | (pick(2) << 16) | (64u - 1u - pick(32)) << 10 |
                  (reg() << 5) | vreg();                                                    /* SCVTF (fixed) */
  case 12: /* LD1/ST1 (multiple structures), offset or post-index */
    return 0x0C000000u | (pick(2) << 30) | (pick(2) << 23) | (pick(2) << 22) | ((pick(2) ? 31u : 24u + pick(4)) << 16) |
           (pick(16) << 12) | (pick(4) << 10) | (base_reg() << 5) | vreg();
  default: /* scalar/vector by element, shifts, pairwise: anything in the space */
    return 0x0E000000u | ((uint32_t)rnd() & 0xF0FFFFFFu & ~(1u << 31)) | (pick(2) << 28);
  }
}

/* Flag setters and readers, for the setter-then-reader idiom (lazy flags). */
static uint32_t gen_flag_setter(void) {
  if (pick(4) == 0) { /* SUBS/ADDS rd, rn, rn: equal operands */
    const uint32_t r = reg();
    return (pick(2) << 31) | (pick(2) << 30) | (1u << 29) | (0x0Bu << 24) | (r << 16) | (r << 5) | dst();
  }
  switch (pick(4)) {
  case 0: return (gen_add_sub_imm() | (1u << 29));                      /* ADDS/SUBS imm */
  case 1: return (gen_add_sub_shifted() | (1u << 29)) & ~(3u << 22);    /* ADDS/SUBS reg, LSL */
  case 2: return gen_logical_shifted() | (3u << 29);                     /* ANDS/BICS */
  default: return (gen_add_sub_extended() | (1u << 29)) & ~(3u << 22);  /* ADDS/SUBS ext */
  }
}
static uint32_t gen_flag_reader(void) {
  switch (pick(6)) {
  case 0: case 1: return (0x54u << 24) | (((uint32_t)small_branch() & 0x7FFFFu) << 5) | pick(16); /* B.cond */
  case 2: return gen_cond_select() & ~(1u << 29);
  case 3: return gen_cond_compare() | (1u << 29);
  case 4: return gen_add_sub_carry();
  default: return 0xD53B4200u | dst(); /* MRS xN, NZCV */
  }
}

static uint32_t gen(void) {
  switch (pick(27)) {
  case 0: return gen_add_sub_imm();
  case 1: return gen_logical_imm();
  case 2: return gen_move_wide();
  case 3: return gen_bitfield();
  case 4: return gen_adr();
  case 5: return gen_add_sub_shifted();
  case 6: return gen_logical_shifted();
  case 7: return gen_cond_select();
  case 8: return gen_madd();
  case 9: case 10: return gen_branch();
  case 11: case 12: return gen_load_store_unsigned();
  case 13: return gen_load_store_single();
  case 14: return gen_pair();
  case 15: return gen_extract();
  case 16: return gen_add_sub_extended();
  case 17: return gen_add_sub_carry();
  case 18: return gen_cond_compare();
  case 19: return gen_dp_two_source();
  case 20: return gen_dp_one_source();
  case 21: return pick(2) ? gen_system() : gen_literal();
  case 22: return pick(2) ? gen_simd_ldst() : gen_exclusive();
  case 23: case 24: case 25: return gen_simd_fp();
  default: return (uint32_t)rnd(); /* anything: the interpreter fallback, often undefined */
  }
}

/* ------------------------------------------------------------------ */
/* One case.                                                           */
/* ------------------------------------------------------------------ */

typedef struct Snapshot {
  uint64_t x[31];
  uint64_t sp, pc;
  uint32_t pstate;
  CPU_Vector_Register v[CPU_VECTOR_REGISTER_COUNT];
  uint64_t tpidr;
  uint32_t fpsr;
  CPU_ExitReason exit;
  uint64_t fault;
  uint64_t cycles;
} Snapshot;

static uint32_t g_fpcr, g_fpsr;

static void set_state(const CPU_Backend *cpu, CPU_State *s, const uint64_t *x, uint64_t sp, uint32_t nzcv) {
  for (uint8_t i = 0; i < 31; i++) cpu->set_reg(s, i, x[i]);
  for (uint8_t i = 0; i < CPU_VECTOR_REGISTER_COUNT; i++) {
    const CPU_Vector_Register v = {x[i % 31] * 3u, x[(i + 7) % 31]};
    cpu->set_vector_reg(s, i, v);
  }
  cpu->set_sp(s, sp);
  cpu->set_pstate(s, nzcv);
  cpu->set_sys_reg(s, CPU_SYSREG_TPIDR_EL0, 0x1234);
  cpu->set_sys_reg(s, CPU_SYSREG_FPCR, g_fpcr);
  cpu->set_sys_reg(s, CPU_SYSREG_FPSR, g_fpsr);
  cpu->set_sys_reg(s, CPU_SYSREG_CNTVCT_EL0, 0);
  cpu->set_pc(s, CODE_GVA);
}

static Snapshot snapshot(const CPU_Backend *cpu, CPU_State *s, CPU_ExitReason exit, uint64_t cycles) {
  Snapshot snap;
  memset(&snap, 0, sizeof(snap));
  for (uint8_t i = 0; i < 31; i++) snap.x[i] = cpu->get_reg(s, i);
  for (uint8_t i = 0; i < CPU_VECTOR_REGISTER_COUNT; i++) snap.v[i] = cpu->get_vector_reg(s, i);
  snap.sp = cpu->get_sp(s);
  snap.pc = cpu->get_pc(s);
  snap.pstate = cpu->get_pstate(s);
  snap.tpidr = cpu->get_sys_reg(s, CPU_SYSREG_TPIDR_EL0);
  snap.fpsr = (uint32_t)cpu->get_sys_reg(s, CPU_SYSREG_FPSR);
  snap.exit = exit;
  snap.fault = exit == CPU_EXIT_FAULT ? cpu->get_fault_address(s) : 0;
  snap.cycles = cycles;
  return snap;
}

static void run_case(uint32_t iteration) {
  /* Code: the stream, then SVCs to the end of the page (R-X: cacheable). */
  static uint32_t code[VMM_PAGE_SIZE / 4];
  for (uint32_t i = 0; i < VMM_PAGE_SIZE / 4; i++) code[i] = SVC_ZERO;
  const uint32_t length = 1u + pick(STREAM_MAX);
  for (uint32_t i = 0; i < length; i++) {
    if (i + 1u < length && pick(16) == 0) { /* LDAXR then STLXR on the same base: the monitor passes */
      const uint32_t size = 2u + pick(2), base = base_reg();
      code[i++] = (size << 30) | (0x08u << 24) | (1u << 22) | (31u << 16) | (pick(2) << 15) | (31u << 10) | (base << 5) | reg();
      code[i] = (size << 30) | (0x08u << 24) | (dst() << 16) | (pick(2) << 15) | (31u << 10) | (base << 5) | reg();
    } else if (i + 1u < length && pick(4) == 0) {
      code[i++] = gen_flag_setter();
      code[i] = gen_flag_reader();
    } else {
      code[i] = gen();
    }
  }
  /* Some streams loop: a backward branch at the end makes blocks hot. */
  if (pick(2)) code[length] = (0x05u << 26) | ((uint32_t)(-(int32_t)pick(length + 1u)) & 0x3FFFFFFu);
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA, code, sizeof(code)));
  g_jit_cpu->clear_cache(g_jit);

  uint64_t x[31];
  for (int i = 0; i < 31; i++) x[i] = pick(4) == 0 ? rnd() : pick(2) ? rnd() & 0xFFFF : pick(4); /* small: equalities */
  for (int i = 20; i < 24; i++) { /* bases: often aligned (exclusives), sometimes near a page end */
    x[i] = DATA_GVA + 0x200u + pick(DATA_BYTES - 0x400u);
    if (pick(2)) x[i] &= ~(uint64_t)15;
    if (pick(8) == 0) x[i] = (x[i] | 0xFFFu) - pick(24);
  }
  for (int i = 24; i < 28; i++) x[i] = pick(64) - 16u;                                 /* indexes */
  x[28] = CODE_GVA + 4u * pick(length + 1u);                                         /* BR target */
  const uint64_t sp = (DATA_GVA + 0x1000u + pick(0x1000u)) & ~(uint64_t)15;
  const uint32_t nzcv = (uint32_t)pick(16) << 28;
  /* FP environment: mostly the default, sometimes FZ/DN/rounding modes; FPSR flags clear or sticky-set. */
  g_fpcr = pick(4) ? 0u : (pick(2) << 24) | (pick(2) << 25) | (pick(4) << 22);
  g_fpsr = pick(2) ? 0u : 0x10u;
  for (uint32_t i = 0; i < DATA_BYTES; i++) g_data_init[i] = (uint8_t)rnd();

  /* The same budgets for both: random sizes up to the limit. */
  uint64_t budgets[STEP_LIMIT];
  for (uint32_t i = 0; i < STEP_LIMIT; i++) budgets[i] = 1u + pick(pick(2) ? 8u : 200u);

  /* Reference: the interpreter's run() (its exclusive-monitor grace past a
   * budget is the run loop's, and the JIT must match it). */
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_ref_cpu, g_ref, x, sp, nzcv);
  CPU_ExitReason ref_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t ref_cycles = 0;
  for (uint32_t i = 0; ref_cycles < STEP_LIMIT; i++) {
    const uint64_t budget = budgets[i] < STEP_LIMIT - ref_cycles ? budgets[i] : STEP_LIMIT - ref_cycles;
    ref_exit = g_ref_cpu->run(g_ref, budget);
    ref_cycles += g_ref_cpu->get_cycles_consumed(g_ref);
    if (ref_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot ref = snapshot(g_ref_cpu, g_ref, ref_exit, ref_cycles);
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, g_data_ref, DATA_BYTES));

  /* JIT: run() with the same budgets. */
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_jit_cpu, g_jit, x, sp, nzcv);
  CPU_ExitReason jit_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t jit_cycles = 0;
  for (uint32_t i = 0; jit_cycles < STEP_LIMIT; i++) {
    const uint64_t budget = budgets[i] < STEP_LIMIT - jit_cycles ? budgets[i] : STEP_LIMIT - jit_cycles;
    jit_exit = g_jit_cpu->run(g_jit, budget);
    jit_cycles += g_jit_cpu->get_cycles_consumed(g_jit);
    if (jit_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot jit = snapshot(g_jit_cpu, g_jit, jit_exit, jit_cycles);
  static uint8_t data_jit[DATA_BYTES];
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, data_jit, DATA_BYTES));

  const bool same = memcmp(ref.x, jit.x, sizeof(ref.x)) == 0 && ref.sp == jit.sp && ref.pc == jit.pc &&
                    ref.pstate == jit.pstate && ref.exit == jit.exit && ref.fault == jit.fault &&
                    ref.cycles == jit.cycles && ref.tpidr == jit.tpidr && ref.fpsr == jit.fpsr &&
                    memcmp(ref.v, jit.v, sizeof(ref.v)) == 0 && memcmp(g_data_ref, data_jit, DATA_BYTES) == 0;
  if (!same) {
    fprintf(stderr, "[jit_diff_test] MISMATCH at iteration %u (%u instructions):\n", iteration, length);
    for (uint32_t i = 0; i <= length; i++) fprintf(stderr, "  %3u: %08x\n", i, code[i]);
    fprintf(stderr, "  exit %d/%d pc %llx/%llx cycles %llu/%llu fault %llx/%llx pstate %x/%x sp %llx/%llx\n",
            ref.exit, jit.exit, (unsigned long long)ref.pc, (unsigned long long)jit.pc,
            (unsigned long long)ref.cycles, (unsigned long long)jit.cycles, (unsigned long long)ref.fault,
            (unsigned long long)jit.fault, ref.pstate, jit.pstate, (unsigned long long)ref.sp,
            (unsigned long long)jit.sp);
    for (int i = 0; i < 31; i++) {
      if (ref.x[i] != jit.x[i]) {
        fprintf(stderr, "  x%d %016llx / %016llx (initial %016llx)\n", i, (unsigned long long)ref.x[i],
                (unsigned long long)jit.x[i], (unsigned long long)x[i]);
      }
    }
    if (memcmp(ref.v, jit.v, sizeof(ref.v)) != 0) fprintf(stderr, "  vector registers differ\n");
    if (ref.tpidr != jit.tpidr) fprintf(stderr, "  tpidr differs\n");
    if (ref.fpsr != jit.fpsr) fprintf(stderr, "  fpsr %x / %x\n", ref.fpsr, jit.fpsr);
    if (memcmp(g_data_ref, data_jit, DATA_BYTES) != 0) fprintf(stderr, "  data differs\n");
    exit(1);
  }
}

int main(int argc, char **argv) {
  const uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : DEFAULT_ITERATIONS;
  g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ull;
  if (!g_rng) g_rng = 1;
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA, DATA_BYTES, VMM_PERM_RW));
  g_ref_cpu = &CPU_BACKEND_INTERPRETER;
  g_jit_cpu = &CPU_BACKEND_JIT;
  jit_set_hot_threshold(1);
  g_ref = g_ref_cpu->create(g_vmm, NULL);
  g_jit = g_jit_cpu->create(g_vmm, NULL);
  CHECK(g_ref && g_jit);
  g_ref_cpu->set_svc_handler(g_ref, on_svc);
  g_jit_cpu->set_svc_handler(g_jit, on_svc);
  g_ref_cpu->set_undefined_handler(g_ref, on_undefined);
  g_jit_cpu->set_undefined_handler(g_jit, on_undefined);
  for (uint32_t i = 0; i < iterations; i++) run_case(i);
  const Jit_Stats *stats = jit_stats();
  printf("[jit_diff_test] %u streams, seed %llx: passed (%llu blocks compiled, %llu compiled-block entries, "
         "%llu interpreted blocks, %llu module bytes)\n",
         iterations, (unsigned long long)g_rng, (unsigned long long)stats->blocks_compiled,
         (unsigned long long)stats->block_entries, (unsigned long long)stats->interpreted_blocks,
         (unsigned long long)stats->module_bytes);
  g_ref_cpu->destroy(g_ref);
  g_jit_cpu->destroy(g_jit);
  vmm_destroy(g_vmm);
  layout_destroy();
  return 0;
}
