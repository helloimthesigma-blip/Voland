/**
 * Predecoded interpreter vs the reference decoder (§25 Phase 5): random
 * instruction streams built from every specialized form (and raw random
 * words, which exercise the in-block reference fallback) run once through
 * step() - the reference path, interp_execute per instruction - and once
 * through run() - decoded blocks - from identical register and memory
 * images. Registers, SP, flags, PC, exit reason, fault address, cycle
 * count and every data byte must match. Budgets are varied so blocks are
 * entered and left mid-way. Portable: no host ARM64 needed.
 *
 *   predecode_test [iterations] [seed]
 */
#define CHECK_NAME "predecode_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
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
#define STEP_LIMIT 160u
#define SVC_ZERO 0xD4000001u
#define DEFAULT_ITERATIONS 20000u

static VMM_Context *g_vmm;
static const CPU_Backend *g_cpu;
static CPU_State *g_ref, *g_pre;
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

static uint32_t gen_add_sub_imm(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0x22u << 23) | (pick(2) << 22) |
         ((uint32_t)pick(4096) << 10) | (reg() << 5) | reg();
}
static uint32_t gen_logical_imm(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x24u << 23) | (pick(2) << 22) | (pick(64) << 16) | (pick(64) << 10) |
         (reg() << 5) | reg();
}
static uint32_t gen_move_wide(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x25u << 23) | (pick(4) << 21) | (pick(65536) << 5) | reg();
}
static uint32_t gen_bitfield(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x26u << 23) | (pick(2) << 22) | (pick(64) << 16) | (pick(64) << 10) |
         (reg() << 5) | reg();
}
static uint32_t gen_adr(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x10u << 24) | (pick(1u << 19) << 5) | reg();
}
static uint32_t gen_add_sub_shifted(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(2) << 29) | (0x0Bu << 24) | (pick(4) << 22) | (reg() << 16) |
         (pick(64) << 10) | (reg() << 5) | reg();
}
static uint32_t gen_logical_shifted(void) {
  return (pick(2) << 31) | (pick(4) << 29) | (0x0Au << 24) | (pick(4) << 22) | (pick(2) << 21) | (reg() << 16) |
         (pick(64) << 10) | (reg() << 5) | reg();
}
static uint32_t gen_cond_select(void) {
  return (pick(2) << 31) | (pick(2) << 30) | (pick(4) == 0 ? 1u << 29 : 0) | (0xD4u << 21) | (reg() << 16) |
         (pick(16) << 12) | (pick(4) == 0 ? 1u << 11 : 0) | (pick(2) << 10) | (reg() << 5) | reg();
}
static uint32_t gen_madd(void) {
  return (pick(2) << 31) | (0x1Bu << 24) | (reg() << 16) | (pick(2) << 15) | (reg() << 10) | (reg() << 5) | reg();
}
static int32_t small_branch(void) { return (int32_t)pick(17) - 8; } /* within the stream, mostly */
static uint32_t gen_branch(void) {
  switch (pick(6)) {
  case 0: return (pick(2) << 31) | (0x05u << 26) | ((uint32_t)small_branch() & 0x3FFFFFFu);
  case 1: return (0x54u << 24) | (((uint32_t)small_branch() & 0x7FFFFu) << 5) | pick(16);
  case 2: return (pick(2) << 31) | (0x1Au << 25) | (pick(2) << 24) | (((uint32_t)small_branch() & 0x7FFFFu) << 5) | reg();
  case 3: return (pick(2) << 31) | (0x1Bu << 25) | (pick(2) << 24) | (pick(32) << 19) |
                 (((uint32_t)small_branch() & 0x3FFFu) << 5) | reg();
  case 4: return 0xD65F0000u | (reg() << 5); /* RET xN */
  default: return (pick(2) ? 0xD61F0000u : 0xD63F0000u) | ((20u + pick(4)) << 5); /* BR/BLR to data: faults */
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

static uint32_t gen(void) {
  switch (pick(16)) {
  case 0: return gen_add_sub_imm();
  case 1: return gen_logical_imm();
  case 2: return gen_move_wide();
  case 3: return gen_bitfield();
  case 4: return gen_adr();
  case 5: return gen_add_sub_shifted();
  case 6: return gen_logical_shifted();
  case 7: return gen_cond_select();
  case 8: return gen_madd();
  case 9: return gen_branch();
  case 10: case 11: return gen_load_store_unsigned();
  case 12: case 13: return gen_load_store_single();
  case 14: return gen_pair();
  default: return (uint32_t)rnd(); /* anything: the reference fallback, often undefined */
  }
}

/* ------------------------------------------------------------------ */
/* One case.                                                           */
/* ------------------------------------------------------------------ */

typedef struct Snapshot {
  uint64_t x[31];
  uint64_t sp, pc;
  uint32_t pstate;
  CPU_ExitReason exit;
  uint64_t fault;
  uint64_t cycles;
} Snapshot;

static void set_state(CPU_State *s, const uint64_t *x, uint64_t sp, uint32_t nzcv) {
  for (uint8_t i = 0; i < 31; i++) g_cpu->set_reg(s, i, x[i]);
  g_cpu->set_sp(s, sp);
  g_cpu->set_pstate(s, nzcv);
  g_cpu->set_pc(s, CODE_GVA);
}

static Snapshot snapshot(CPU_State *s, CPU_ExitReason exit, uint64_t cycles) {
  Snapshot snap;
  for (uint8_t i = 0; i < 31; i++) snap.x[i] = g_cpu->get_reg(s, i);
  snap.sp = g_cpu->get_sp(s);
  snap.pc = g_cpu->get_pc(s);
  snap.pstate = g_cpu->get_pstate(s);
  snap.exit = exit;
  snap.fault = exit == CPU_EXIT_FAULT ? g_cpu->get_fault_address(s) : 0;
  snap.cycles = cycles;
  return snap;
}

static void run_case(uint32_t iteration) {
  /* Code: the stream, then SVCs to the end of the page (R-X: cacheable). */
  static uint32_t code[VMM_PAGE_SIZE / 4];
  for (uint32_t i = 0; i < VMM_PAGE_SIZE / 4; i++) code[i] = SVC_ZERO;
  const uint32_t length = 1u + pick(STREAM_MAX);
  for (uint32_t i = 0; i < length; i++) code[i] = gen();
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA, code, sizeof(code)));
  g_cpu->clear_cache(g_pre);

  uint64_t x[31];
  for (int i = 0; i < 31; i++) x[i] = pick(4) == 0 ? rnd() : rnd() & 0xFFFF;
  for (int i = 20; i < 24; i++) x[i] = DATA_GVA + 0x200u + pick(DATA_BYTES - 0x400u); /* bases */
  for (int i = 24; i < 28; i++) x[i] = pick(64) - 16u;                                 /* indexes */
  const uint64_t sp = (DATA_GVA + 0x1000u + pick(0x1000u)) & ~(uint64_t)15;
  const uint32_t nzcv = (uint32_t)pick(16) << 28;
  for (uint32_t i = 0; i < DATA_BYTES; i++) g_data_init[i] = (uint8_t)rnd();

  /* Reference: step() until an exit or the limit. */
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_ref, x, sp, nzcv);
  CPU_ExitReason ref_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t ref_cycles = 0;
  for (uint32_t i = 0; i < STEP_LIMIT; i++) {
    ref_exit = g_cpu->step(g_ref);
    ref_cycles += g_cpu->get_cycles_consumed(g_ref);
    if (ref_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot ref = snapshot(g_ref, ref_exit, ref_cycles);
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, g_data_ref, DATA_BYTES));

  /* Predecoded: run() in random-sized budgets up to the same limit. */
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_pre, x, sp, nzcv);
  CPU_ExitReason pre_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t pre_cycles = 0;
  while (pre_cycles < STEP_LIMIT) {
    uint64_t budget = 1u + pick(pick(2) ? 4u : 64u);
    if (budget > STEP_LIMIT - pre_cycles) budget = STEP_LIMIT - pre_cycles;
    pre_exit = g_cpu->run(g_pre, budget);
    pre_cycles += g_cpu->get_cycles_consumed(g_pre);
    if (pre_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot pre = snapshot(g_pre, pre_exit, pre_cycles);
  static uint8_t data_pre[DATA_BYTES];
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, data_pre, DATA_BYTES));

  const bool same = memcmp(ref.x, pre.x, sizeof(ref.x)) == 0 && ref.sp == pre.sp && ref.pc == pre.pc &&
                    ref.pstate == pre.pstate && ref.exit == pre.exit && ref.fault == pre.fault &&
                    ref.cycles == pre.cycles && memcmp(g_data_ref, data_pre, DATA_BYTES) == 0;
  if (!same) {
    fprintf(stderr, "[predecode_test] MISMATCH at iteration %u (%u instructions):\n", iteration, length);
    for (uint32_t i = 0; i < length; i++) fprintf(stderr, "  %3u: %08x\n", i, code[i]);
    fprintf(stderr, "  exit %d/%d pc %llx/%llx cycles %llu/%llu fault %llx/%llx pstate %x/%x sp %llx/%llx\n",
            ref.exit, pre.exit, (unsigned long long)ref.pc, (unsigned long long)pre.pc,
            (unsigned long long)ref.cycles, (unsigned long long)pre.cycles, (unsigned long long)ref.fault,
            (unsigned long long)pre.fault, ref.pstate, pre.pstate, (unsigned long long)ref.sp,
            (unsigned long long)pre.sp);
    for (int i = 0; i < 31; i++) {
      if (ref.x[i] != pre.x[i]) {
        fprintf(stderr, "  x%d %016llx / %016llx\n", i, (unsigned long long)ref.x[i], (unsigned long long)pre.x[i]);
      }
    }
    if (memcmp(g_data_ref, data_pre, DATA_BYTES) != 0) fprintf(stderr, "  data differs\n");
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
  g_cpu = &CPU_BACKEND_INTERPRETER;
  g_ref = g_cpu->create(g_vmm, NULL);
  g_pre = g_cpu->create(g_vmm, NULL);
  CHECK(g_ref && g_pre);
  g_cpu->set_svc_handler(g_ref, on_svc);
  g_cpu->set_svc_handler(g_pre, on_svc);
  g_cpu->set_undefined_handler(g_ref, on_undefined);
  g_cpu->set_undefined_handler(g_pre, on_undefined);
  for (uint32_t i = 0; i < iterations; i++) run_case(i);
  g_cpu->destroy(g_ref);
  g_cpu->destroy(g_pre);
  vmm_destroy(g_vmm);
  layout_destroy();
  printf("[predecode_test] %u streams, seed %llx: passed\n", iterations, (unsigned long long)g_rng);
  return 0;
}
