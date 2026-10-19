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
  cpu->set_reg(s, 17, DATA_GVA + VMM_PAGE_SIZE);
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
  CHECK_OK(vmm_map(vmm, DATA_GVA, DATA_PA, 2u * VMM_PAGE_SIZE, VMM_PERM_RW));
  jit_set_hot_threshold(2);
  if (getenv("JIT_BENCH_DUMP")) jit_set_dump_directory(getenv("JIT_BENCH_DUMP")); /* each module, for reading */

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

  /* 2b. Loads from two pages through two base registers (x16, x17 = the
   *     next page), then through one (x16 and x16 + one page): one-entry
   *     translation caches miss on every access of the second. */
  const uint32_t two_bases[] = {0xAA0003F8u, 0xF9400201u, 0xF9400222u, 0x8B020023u, 0xF9400601u,
                                0xF9400622u, 0x8B020063u, 0xF1000718u, 0x54FFFF21u, 0xD4000001u};
  const uint32_t one_base[] = {0xAA0003F8u, 0xF9400201u, 0xF9480202u, 0x8B020023u, 0xF9400601u,
                               0xF9480602u, 0x8B020063u, 0xF1000718u, 0x54FFFF21u, 0xD4000001u};
  bench(vmm, "two pages, two bases", two_bases, sizeof(two_bases) / 4, n);
  bench(vmm, "two pages, one base", one_base, sizeof(one_base) / 4, n);

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

  /* 5. A managed-code helper Celeste calls tens of millions of times
   * while it loads: the product of a multi-dimensional array's lengths.
   * Not a leaf (it calls a getter and a GC poll), so every call enters
   * and leaves its own region; inside, two small callees are spanned. */
  static const struct { uint32_t index, word; } array_length[] = {
      {0, 0xAA0003F8u},   {1, 0xAA1003F3u},   {2, 0xAA1303E0u},   {3, 0x9400003Du},   {4, 0xF1000718u},
      {5, 0x54FFFFA1u},   {6, 0xD4000001u},   {64, 0xA9BD57F6u},  {65, 0xA9014FF4u},  {66, 0xA9027BFDu},
      {67, 0x910083FDu},  {68, 0x90000816u},  {69, 0xF9419AD6u},  {70, 0xB94002C8u},  {71, 0xAA0003F3u},
      {72, 0x34000048u},  {73, 0x940000F7u},  {74, 0xAA1303E0u},  {75, 0x2A1F03E1u},  {76, 0x940000B4u},
      {77, 0x2A0003F4u},  {78, 0xAA1303E0u},  {79, 0x94000071u},  {80, 0x7100081Fu},  {81, 0x540001CBu},
      {82, 0x320003F5u},  {83, 0xB94002C8u},  {84, 0x34000048u},  {85, 0x940000EBu},  {86, 0xAA1303E0u},
      {87, 0x2A1503E1u},  {88, 0x940000A8u},  {89, 0x1B147C14u},  {90, 0xAA1303E0u},  {91, 0x110006B5u},
      {92, 0x94000064u},  {93, 0x6B0002BFu},  {94, 0x54FFFEABu},  {95, 0x2A1403E0u},  {96, 0xA9427BFDu},
      {97, 0xA9414FF4u},  {98, 0xA8C357F6u},  {99, 0xD65F03C0u},  {192, 0xB9401C00u}, {193, 0xD65F03C0u},
      {256, 0xA9BE4FF4u}, {257, 0xA9017BFDu}, {258, 0x910043FDu}, {259, 0xB9401C08u}, {260, 0x6B01011Fu},
      {261, 0x540000C9u}, {262, 0xF9401008u}, {263, 0xB861D900u}, {264, 0xA9417BFDu}, {265, 0xA8C24FF4u},
      {266, 0xD65F03C0u}, {267, 0xD4200000u}, {320, 0xD65F03C0u}};
  static uint32_t helper[VMM_PAGE_SIZE / 4];
  for (uint32_t i = 0; i < VMM_PAGE_SIZE / 4; i++) helper[i] = 0xD4000001u;
  for (uint32_t i = 0; i < sizeof(array_length) / sizeof(array_length[0]); i++)
    helper[array_length[i].index] = array_length[i].word;
  /* The array at DATA_GVA (rank 2 at +0x1c, lengths 16 x 16 at +0x100);
   * the GOT slot at +0x330 points at a zero GC-poll flag at +0x400. */
  const uint32_t rank = 2, lengths[2] = {16, 16}, flag = 0;
  const uint64_t lengths_gva = DATA_GVA + 0x100u, flag_gva = DATA_GVA + 0x400u;
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + 0x1Cu, &rank, sizeof(rank)));
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + 0x20u, &lengths_gva, sizeof(lengths_gva)));
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + 0x100u, lengths, sizeof(lengths)));
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + 0x330u, &flag_gva, sizeof(flag_gva)));
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + 0x400u, &flag, sizeof(flag)));
  bench(vmm, "array length helper", helper, VMM_PAGE_SIZE / 4, n / 8u);

  const Jit_Stats *stats = jit_stats();
  printf("[jit_bench] %llu regions compiled, %llu dispatcher entries, %llu interpreted blocks\n",
         (unsigned long long)stats->blocks_compiled, (unsigned long long)stats->block_entries,
         (unsigned long long)stats->interpreted_blocks);
  vmm_destroy(vmm);
  layout_destroy();
  return 0;
}
