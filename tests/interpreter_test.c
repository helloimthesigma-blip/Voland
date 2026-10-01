/**
 * Interpreter behavior the hardware oracle (a64_diff_test.c, ARM64 hosts
 * only) cannot cover, as data-driven vectors plus run-loop checks:
 * branches, SVC/BRK, undefined and faulting instructions leaving state
 * untouched, exclusive load/store pairs, system registers, and the
 * bounded run() contract (§7/§8). Portable: runs on every host.
 */
#define CHECK_NAME "interpreter_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <string.h>

#define CODE_GVA 0x10000ull
#define DATA_GVA 0x20000ull
#define NOEXEC_GVA 0x30000ull
#define PAGE VMM_PAGE_SIZE

static VMM_Context *g_vmm;
static const CPU_Backend *g_cpu;
static CPU_State *g_state;

static uint32_t g_svc_count;
static uint32_t g_svc_last;
static uint32_t g_undefined_count;
static uint32_t g_breakpoint_count;

static void on_svc(CPU_State *state, uint32_t swi, void *userdata) {
  (void)state;
  (void)userdata;
  g_svc_count++;
  g_svc_last = swi;
}
static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
  g_undefined_count++;
}
static void on_breakpoint(CPU_State *state, uint64_t address, void *userdata) {
  (void)state;
  (void)address;
  (void)userdata;
  g_breakpoint_count++;
}

static void reset_state(void) {
  for (uint8_t i = 0; i < 31; i++) g_cpu->set_reg(g_state, i, 0);
  g_cpu->set_pstate(g_state, 0);
  g_cpu->set_pc(g_state, CODE_GVA);
}

static void load_code(const uint32_t *words, uint32_t count) {
  CHECK_OK(vmm_write_block(g_vmm, CODE_GVA, words, count * sizeof(uint32_t)));
}

/* ------------------------------------------------------------------ */
/* Branch vectors: one instruction at CODE_GVA + 0x100, state in, PC   */
/* (and optionally a register) out.                                    */
/* ------------------------------------------------------------------ */

#define VECTOR_PC (CODE_GVA + 0x100u)
#define NO_REG 0xFFu

typedef struct Branch_Vector {
  const char *name;
  uint32_t insn;
  uint32_t nzcv;          /* bits 31:28 */
  uint8_t in_reg;         /* register given `in_value`, or NO_REG */
  uint64_t in_value;
  uint64_t expected_pc;
  uint8_t out_reg;        /* register expected to hold `out_value`, or NO_REG */
  uint64_t out_value;
} Branch_Vector;

static const Branch_Vector k_branch_vectors[] = {
    /* B / BL: imm26 * 4, sign-extended. */
    {"b +8", 0x14000002u, 0, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b -4", 0x17FFFFFFu, 0, NO_REG, 0, VECTOR_PC - 4, NO_REG, 0},
    {"bl +0x40 sets x30", 0x94000010u, 0, NO_REG, 0, VECTOR_PC + 0x40, 30, VECTOR_PC + 4},
    /* B.cond: imm19 * 4. */
    {"b.eq taken", 0x54000040u, CPU_PSTATE_Z, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.eq not taken", 0x54000040u, 0, NO_REG, 0, VECTOR_PC + 4, NO_REG, 0},
    {"b.ne taken", 0x54000041u, 0, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.hi needs C and !Z", 0x54000048u, CPU_PSTATE_C | CPU_PSTATE_Z, NO_REG, 0, VECTOR_PC + 4, NO_REG, 0},
    {"b.ge with N == V", 0x5400004Au, CPU_PSTATE_N | CPU_PSTATE_V, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.lt with N != V", 0x5400004Bu, CPU_PSTATE_N, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.al", 0x5400004Eu, 0, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.nv behaves as always", 0x5400004Fu, 0, NO_REG, 0, VECTOR_PC + 8, NO_REG, 0},
    {"b.eq backwards", 0x54FFFFE0u, CPU_PSTATE_Z, NO_REG, 0, VECTOR_PC - 4, NO_REG, 0},
    /* CBZ / CBNZ: 32-bit form ignores the upper half. */
    {"cbz x1 taken", 0xB4000041u, 0, 1, 0, VECTOR_PC + 8, NO_REG, 0},
    {"cbz w1 ignores high bits", 0x34000041u, 0, 1, 0xFFFFFFFF00000000ull, VECTOR_PC + 8, NO_REG, 0},
    {"cbz x1 not taken", 0xB4000041u, 0, 1, 0xFFFFFFFF00000000ull, VECTOR_PC + 4, NO_REG, 0},
    {"cbnz x1 taken", 0xB5000041u, 0, 1, 5, VECTOR_PC + 8, NO_REG, 0},
    /* TBZ / TBNZ: bit number b5:b40. */
    {"tbz x1 #63 taken", 0xB6F80041u, 0, 1, 0x7FFFFFFFFFFFFFFFull, VECTOR_PC + 8, NO_REG, 0},
    {"tbnz x1 #63 taken", 0xB7F80041u, 0, 1, 0x8000000000000000ull, VECTOR_PC + 8, NO_REG, 0},
    {"tbnz w1 #3 not taken", 0x37180041u, 0, 1, 0xF7, VECTOR_PC + 4, NO_REG, 0},
    /* BR / BLR / RET. */
    {"br x2", 0xD61F0040u, 0, 2, 0x12340, 0x12340, NO_REG, 0},
    {"blr x2", 0xD63F0040u, 0, 2, 0x12340, 0x12340, 30, VECTOR_PC + 4},
    {"blr x30 reads before writing", 0xD63F03C0u, 0, 30, 0x55550, 0x55550, 30, VECTOR_PC + 4},
    {"ret", 0xD65F03C0u, 0, 30, 0x40000, 0x40000, NO_REG, 0},
};

static void test_branch_vectors(void) {
  for (size_t i = 0; i < sizeof(k_branch_vectors) / sizeof(k_branch_vectors[0]); i++) {
    const Branch_Vector *v = &k_branch_vectors[i];
    reset_state();
    CHECK_OK(vmm_write32(g_vmm, VECTOR_PC, v->insn));
    g_cpu->set_pc(g_state, VECTOR_PC);
    g_cpu->set_pstate(g_state, v->nzcv);
    if (v->in_reg != NO_REG) g_cpu->set_reg(g_state, v->in_reg, v->in_value);
    CHECK(g_cpu->step(g_state) == CPU_EXIT_CYCLES_ELAPSED);
    if (g_cpu->get_pc(g_state) != v->expected_pc) {
      fprintf(stderr, "vector '%s': pc %llx, expected %llx\n", v->name,
              (unsigned long long)g_cpu->get_pc(g_state), (unsigned long long)v->expected_pc);
      CHECK(false);
    }
    if (v->out_reg != NO_REG) CHECK(g_cpu->get_reg(g_state, v->out_reg) == v->out_value);
  }
}

/* ------------------------------------------------------------------ */
/* Exceptions, faults, system registers.                               */
/* ------------------------------------------------------------------ */

static void test_svc_and_brk(void) {
  const uint32_t code[] = {
      0xD2800540u, /* movz x0, #42 */
      0xD4000E21u, /* svc #0x71 */
      0xD2800021u, /* movz x1, #1 */
      0xD4200000u, /* brk #0 */
  };
  load_code(code, 4);
  reset_state();
  g_svc_count = 0;
  CHECK(g_cpu->run(g_state, 100) == CPU_EXIT_SVC);
  CHECK(g_svc_count == 1 && g_svc_last == 0x71);
  CHECK(g_cpu->get_pc(g_state) == CODE_GVA + 8); /* return address: after the SVC */
  CHECK(g_cpu->get_reg(g_state, 0) == 42);
  CHECK(g_cpu->get_cycles_consumed(g_state) == 2);
  /* Resuming continues after the SVC, then BRK stops at itself. */
  g_breakpoint_count = 0;
  CHECK(g_cpu->run(g_state, 100) == CPU_EXIT_BREAKPOINT);
  CHECK(g_breakpoint_count == 1);
  CHECK(g_cpu->get_reg(g_state, 1) == 1);
}

static void test_undefined_and_faults(void) {
  reset_state();
  g_cpu->set_reg(g_state, 3, 0x77);
  const uint32_t udf[] = {0x00000000u}; /* UDF #0 */
  load_code(udf, 1);
  g_undefined_count = 0;
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT);
  CHECK(g_undefined_count == 1);
  CHECK(g_cpu->get_pc(g_state) == CODE_GVA && g_cpu->get_fault_address(g_state) == CODE_GVA);

  /* A load from an unmapped address faults with every register intact,
   * including the base that a post-index form would have written back. */
  const uint32_t ldr_post[] = {0xF8408441u}; /* ldr x1, [x2], #8 */
  load_code(ldr_post, 1);
  reset_state();
  g_cpu->set_reg(g_state, 1, 0x1111);
  g_cpu->set_reg(g_state, 2, 0x7000000);
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT);
  CHECK(g_cpu->get_fault_address(g_state) == 0x7000000);
  CHECK(g_cpu->get_reg(g_state, 1) == 0x1111 && g_cpu->get_reg(g_state, 2) == 0x7000000);
  CHECK(g_cpu->get_pc(g_state) == CODE_GVA);

  /* A pair store straddling into an unmapped page writes nothing. */
  const uint32_t stp[] = {0xA9000441u}; /* stp x1, x1, [x2] */
  load_code(stp, 1);
  reset_state();
  g_cpu->set_reg(g_state, 1, 0xAAAAAAAAAAAAAAAAull);
  g_cpu->set_reg(g_state, 2, DATA_GVA + PAGE - 8);
  CHECK_OK(vmm_write64(g_vmm, DATA_GVA + PAGE - 8, 0));
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT);
  uint64_t untouched = 1;
  CHECK_OK(vmm_read64(g_vmm, DATA_GVA + PAGE - 8, &untouched));
  CHECK(untouched == 0);

  /* Fetch: a misaligned PC and a non-executable page both fault. */
  reset_state();
  g_cpu->set_pc(g_state, CODE_GVA + 2);
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT);
  reset_state();
  g_cpu->set_pc(g_state, NOEXEC_GVA);
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT && g_cpu->get_fault_address(g_state) == NOEXEC_GVA);

  /* ARMv8.1+ encodings are undefined on the Switch's ARMv8.0 core. */
  const uint32_t later[] = {
      0xF8200041u, /* ldadd x0, x1, [x2] (LSE) */
      0xC8A07C41u, /* cas x0, x1, [x2] (LSE) */
      0xD65F0BFFu, /* retaa (PAC) */
      0xF8BFC041u, /* ldapr x1, [x2] (RCpc) */
  };
  for (uint32_t i = 0; i < 4; i++) {
    load_code(&later[i], 1);
    reset_state();
    g_cpu->set_reg(g_state, 2, DATA_GVA);
    g_undefined_count = 0;
    CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT && g_undefined_count == 1);
  }
}

static void test_system_registers(void) {
  g_cpu->set_sys_reg(g_state, CPU_SYSREG_TPIDRRO_EL0, 0xABCD000);
  const uint32_t code[] = {
      0xD53BD060u, /* mrs x0, tpidrro_el0 */
      0xD51BD041u, /* msr tpidr_el0, x1 */
      0xD53BD042u, /* mrs x2, tpidr_el0 */
      0xD53BE003u, /* mrs x3, cntfrq_el0 */
      0xD53BE044u, /* mrs x4, cntvct_el0 */
      0xD51BD060u, /* msr tpidrro_el0, x0 : read-only at EL0 -> undefined */
  };
  load_code(code, 6);
  reset_state();
  g_cpu->set_reg(g_state, 1, 0x5150);
  CHECK(g_cpu->run(g_state, 5) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_reg(g_state, 0) == 0xABCD000);
  CHECK(g_cpu->get_reg(g_state, 2) == 0x5150);
  CHECK(g_cpu->get_sys_reg(g_state, CPU_SYSREG_TPIDR_EL0) == 0x5150);
  CHECK(g_cpu->get_reg(g_state, 3) == 19200000u);
  g_undefined_count = 0;
  CHECK(g_cpu->step(g_state) == CPU_EXIT_FAULT && g_undefined_count == 1);
  CHECK(g_cpu->get_sys_reg(g_state, CPU_SYSREG_TPIDRRO_EL0) == 0xABCD000);

  /* The physical counter reads the same virtual time (CNTVOFF = 0). */
  const uint32_t timers[] = {0xD53BE045u /* mrs x5, cntvct_el0 */, 0xD53BE026u /* mrs x6, cntpct_el0 */};
  load_code(timers, 2);
  reset_state();
  g_cpu->set_sys_reg(g_state, CPU_SYSREG_CNTVCT_EL0, 1000000u);
  CHECK(g_cpu->run(g_state, 2) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_reg(g_state, 5) >= 1000000u && g_cpu->get_reg(g_state, 6) - g_cpu->get_reg(g_state, 5) <= 1u);
}

/* ------------------------------------------------------------------ */
/* Exclusives and the run loop.                                        */
/* ------------------------------------------------------------------ */

static void test_exclusive_pair(void) {
  /* loop: ldaxr x1, [x0]; add x1, x1, #1; stlxr w2, x1, [x0]; cbnz w2, loop */
  const uint32_t code[] = {0xC85FFC01u, 0x91000421u, 0xC802FC01u, 0x35FFFFA2u, 0xD4000001u /* svc 0 */};
  load_code(code, 5);
  reset_state();
  CHECK_OK(vmm_write64(g_vmm, DATA_GVA, 41));
  g_cpu->set_reg(g_state, 0, DATA_GVA);
  CHECK(g_cpu->run(g_state, 100) == CPU_EXIT_SVC);
  uint64_t value = 0;
  CHECK_OK(vmm_read64(g_vmm, DATA_GVA, &value));
  CHECK(value == 42 && g_cpu->get_reg(g_state, 2) == 0);

  /* A store-exclusive with no monitor fails, writes status 1 and no memory. */
  const uint32_t lone_store[] = {0xC802FC01u};
  load_code(lone_store, 1);
  reset_state();
  g_cpu->set_reg(g_state, 0, DATA_GVA);
  g_cpu->set_reg(g_state, 1, 999);
  CHECK(g_cpu->run(g_state, 1) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_reg(g_state, 2) == 1);
  CHECK_OK(vmm_read64(g_vmm, DATA_GVA, &value));
  CHECK(value == 42);

  /* CLREX between the pair makes the store fail. */
  const uint32_t cleared[] = {0xC85FFC01u, 0xD503305Fu /* clrex */, 0xC802FC01u};
  load_code(cleared, 3);
  reset_state();
  g_cpu->set_reg(g_state, 0, DATA_GVA);
  CHECK(g_cpu->run(g_state, 3) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_reg(g_state, 2) == 1);

  /* §7 grace: a budget that expires right after the load-exclusive keeps
   * running until the store, so the pair is never split. */
  load_code(code, 5);
  reset_state();
  CHECK_OK(vmm_write64(g_vmm, DATA_GVA, 7));
  g_cpu->set_reg(g_state, 0, DATA_GVA);
  CHECK(g_cpu->run(g_state, 1) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_cycles_consumed(g_state) == 3); /* ldaxr, add, stlxr */
  CHECK(g_cpu->get_reg(g_state, 2) == 0);
  CHECK_OK(vmm_read64(g_vmm, DATA_GVA, &value));
  CHECK(value == 8);
}

static void test_bounded_run(void) {
  const uint32_t spin[] = {0x91000400u /* add x0, x0, #1 */, 0x17FFFFFFu /* b -4 */};
  load_code(spin, 2);
  reset_state();
  CHECK(g_cpu->run(g_state, 1000) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_cycles_consumed(g_state) == 1000);
  CHECK(g_cpu->get_reg(g_state, 0) == 500);
  CHECK(g_cpu->run(g_state, 10) == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(g_cpu->get_reg(g_state, 0) == 505);
}

int main(void) {
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, 0, PAGE, VMM_PERM_ALL));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, PAGE, PAGE, VMM_PERM_RW));
  CHECK_OK(vmm_map(g_vmm, NOEXEC_GVA, 2 * PAGE, PAGE, VMM_PERM_RW));
  g_cpu = &CPU_BACKEND_INTERPRETER;
  g_state = g_cpu->create(g_vmm, NULL);
  CHECK(g_state != NULL);
  g_cpu->set_svc_handler(g_state, on_svc);
  g_cpu->set_undefined_handler(g_state, on_undefined);
  g_cpu->set_breakpoint_handler(g_state, on_breakpoint);

  test_branch_vectors();
  test_svc_and_brk();
  test_undefined_and_faults();
  test_system_registers();
  test_exclusive_pair();
  test_bounded_run();

  g_cpu->destroy(g_state);
  vmm_destroy(g_vmm);
  layout_destroy();
  printf("[interpreter_test] passed\n");
  return 0;
}
