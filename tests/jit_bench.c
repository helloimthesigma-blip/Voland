/**
 * JIT micro-benchmark (docs/JIT.md): guest loops whose cost is dominated
 * by region-to-region transitions - a call through a PLT-style stub to a
 * tiny function and back - and by straight-line ALU work, timed through
 * CPU_BACKEND_JIT's run(). Reports nanoseconds per guest instruction.
 * Only meaningful under Emscripten (natively nothing is compiled).
 *
 *   jit_bench [iterations]
 */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#define CHECK_NAME "jit_bench"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CODE_GVA 0x100000ull
#define DATA_GVA 0x200000ull
#define CODE_PA 0x0ull
#define DATA_PA 0x10000ull
#define BUDGET 2000000ull

static double now_seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

#define BENCH_REPEATS 5

/* Runs from CODE_GVA + entry until the SVC; returns ns per instruction. */
static double bench_once(VMM_Context *vmm, const uint32_t *code, uint32_t words, uint64_t x0, uint64_t *count) {
  static uint32_t page[VMM_PAGE_SIZE / 4];
  for (uint32_t i = 0; i < VMM_PAGE_SIZE / 4; i++) page[i] = 0xD4000001u; /* SVC #0 */
  for (uint32_t i = 0; i < words; i++) page[i] = code[i];
  CHECK_OK(vmm_write_physical(vmm, CODE_PA, page, sizeof(page)));
  const CPU_Backend *cpu = &CPU_BACKEND_JIT;
  CPU_State *s = cpu->create(vmm, NULL);
  cpu->clear_cache(s);
  cpu->set_reg(s, 0, x0);
  cpu->set_reg(s, 16, DATA_GVA);
  cpu->set_sp(s, DATA_GVA + 0x800);
  cpu->set_pc(s, CODE_GVA);
  uint64_t instructions = 0;
  const double start = now_seconds();
  for (;;) {
    const CPU_ExitReason exit = cpu->run(s, BUDGET);
    instructions += cpu->get_cycles_consumed(s);
    if (exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const double elapsed = now_seconds() - start;
  cpu->destroy(s);
  *count = instructions;
  return elapsed * 1e9 / (double)instructions;
}

/* The best of BENCH_REPEATS runs (the machine is shared). */
static double bench(VMM_Context *vmm, const char *name, const uint32_t *code, uint32_t words, uint64_t x0) {
  double best = 0;
  uint64_t instructions = 0;
  for (uint32_t i = 0; i < BENCH_REPEATS; i++) {
    const double ns = bench_once(vmm, code, words, x0, &instructions);
    if (i == 0 || ns < best) best = ns;
  }
  printf("[jit_bench] %-28s %10llu instructions  %6.2f ns/instruction  %7.1f M instr/s\n", name,
         (unsigned long long)instructions, best, 1e3 / best);
  return best;
}

int main(int argc, char **argv) {
  const uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 5000000ull;
  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  CHECK_OK(vmm_map(vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(vmm, DATA_GVA, DATA_PA, VMM_PAGE_SIZE, VMM_PERM_RW));
  jit_set_hot_threshold(2);

  /* 1. ALU loop: x1 += x0 ... (8 ALU + subs/b.ne). */
  const uint32_t alu[] = {
      0x8B000021, 0xCA010042, 0x8B020063, 0xD3410084, 0x8B0400A5, 0xAA0500C6, 0x8B0600E7, 0x91000508,
      0xF1000400, /* subs x0, x0, #1 */
      0x54FFFEE1, /* b.ne -9 */
      0xD4000001};
  bench(vmm, "ALU loop", alu, sizeof(alu) / 4, n);

  /* 2. Load/store loop through [x16]. */
  const uint32_t mem[] = {
      0xF9400201, 0x91000421, 0xF9000201, 0xF9400602, 0x8B010042, 0xF9000602,
      0xF1000400, 0x54FFFF41, /* subs, b.ne -6 */
      0xD4000001};
  bench(vmm, "load/store loop", mem, sizeof(mem) / 4, n);

  /* 3. Call loop: bl f; subs; b.ne; svc ... f: add x1, x1, #1; ret
   *    (f on the same page, but a call is a region exit + entry). */
  uint32_t call[64];
  for (int i = 0; i < 64; i++) call[i] = 0xD4000001u;
  call[0] = 0x94000000u | 16u; /* bl +16 words */
  call[1] = 0xF1000400u;
  call[2] = 0x54FFFFC1u;       /* b.ne -2 */
  call[3] = 0xD4000001u;
  call[16] = 0x91000421u;      /* add x1, x1, #1 */
  call[17] = 0xD65F03C0u;      /* ret */
  bench(vmm, "call loop (bl/ret)", call, 64, n);

  /* 4. Call through a PLT-style stub: bl stub; stub: adrp-free form
   *    ldr x17, [x16]; br x17 ... with [x16] = &f. */
  uint32_t plt[64];
  for (int i = 0; i < 64; i++) plt[i] = 0xD4000001u;
  plt[0] = 0x94000000u | 16u;  /* bl stub */
  plt[1] = 0xF1000400u;
  plt[2] = 0x54FFFFC1u;
  plt[3] = 0xD4000001u;
  plt[16] = 0xF9400211u;       /* ldr x17, [x16] */
  plt[17] = 0xD61F0220u;       /* br x17 */
  plt[32] = 0x91000421u;       /* f: add x1, x1, #1 */
  plt[33] = 0xD65F03C0u;       /* ret */
  const uint64_t f = CODE_GVA + 32u * 4u;
  CHECK_OK(vmm_write_physical(vmm, DATA_PA, &f, sizeof(f)));
  bench(vmm, "call through stub", plt, 64, n);

  const Jit_Stats *stats = jit_stats();
  printf("[jit_bench] %llu regions compiled, %llu dispatcher entries, %llu interpreted blocks\n",
         (unsigned long long)stats->blocks_compiled, (unsigned long long)stats->block_entries,
         (unsigned long long)stats->interpreted_blocks);
  vmm_destroy(vmm);
  layout_destroy();
  return 0;
}
