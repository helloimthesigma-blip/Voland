/**
 * AArch32 on several host threads (docs/PARALLEL.md): three host threads,
 * each running its own A32 CPU_State in multicore mode
 * (cpu_set_multicore), increment one shared word with an LDREX/STREX
 * retry loop - and a second word with LDAEX/STLEX - many times, under
 * small random budgets so that run() exits often in the middle of the
 * loop. A lost update fails it.
 *
 * Natively the backend is the A32 interpreter; tests/jit_wasm builds the
 * same file against the A32 JIT (compiled code on every core).
 *
 *   a32_parallel_test [increments per thread]
 */
#define CHECK_NAME "a32_parallel_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/a32/a32.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef SWITCH_CPU_BACKEND_JIT
#define TEST_BACKEND (&CPU_BACKEND_A32_JIT)
#else
#define TEST_BACKEND (&CPU_BACKEND_A32)
#endif

#define CODE_GVA 0x100000ull
#define CODE_PA 0x0ull
#define DATA_GVA 0x200000ull
#define DATA_PA 0x1000ull
#define THREADS 3u
#define DEFAULT_INCREMENTS 20000u
#define COUNTER_OFFSET 0x40u  /* LDREX/STREX */
#define COUNTER2_OFFSET 0x80u /* LDAEX/STLEX */

/* r0 = &counter, r4 = &counter2, r3 = increments:
 *   loop:  ldrex r1, [r0]; add r1, r1, #1; strex r2, r1, [r0]; cmp r2, #0; bne loop
 *   loop2: ldaex r5, [r4]; add r5, r5, #2; stlex r2, r5, [r4]; cmp r2, #0; bne loop2
 *          dmb ish; subs r3, r3, #1; bne loop; svc #0 */
static const uint32_t k_program[] = {
    0xE1901F9Fu, /* 0x00 ldrex r1, [r0] */
    0xE2811001u, /* 0x04 add r1, r1, #1 */
    0xE1802F91u, /* 0x08 strex r2, r1, [r0] */
    0xE3520000u, /* 0x0c cmp r2, #0 */
    0x1AFFFFFAu, /* 0x10 bne 0x00 */
    0xE1945E9Fu, /* 0x14 ldaex r5, [r4] */
    0xE2855002u, /* 0x18 add r5, r5, #2 */
    0xE1842E95u, /* 0x1c stlex r2, r5, [r4] */
    0xE3520000u, /* 0x20 cmp r2, #0 */
    0x1AFFFFFAu, /* 0x24 bne 0x14 */
    0xF57FF05Bu, /* 0x28 dmb ish */
    0xE2533001u, /* 0x2c subs r3, r3, #1 */
    0x1AFFFFF2u, /* 0x30 bne 0x00 */
    0xEF000000u, /* 0x34 svc #0 */
};

typedef struct Worker {
  pthread_t thread;
  VMM_Context *vmm;
  uint32_t increments;
  uint64_t seed;
  bool done;
  bool ok;
} Worker;

static void on_svc(CPU_State *state, uint32_t swi, void *userdata) {
  (void)state;
  (void)swi;
  ((Worker *)userdata)->done = true;
}
static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
}

static void *worker_main(void *arg) {
  Worker *w = (Worker *)arg;
  const CPU_Backend *cpu = TEST_BACKEND;
  CPU_State *s = cpu->create(w->vmm, w);
  if (!s) return NULL;
  cpu->set_svc_handler(s, on_svc);
  cpu->set_undefined_handler(s, on_undefined);
  cpu->set_reg(s, 0, DATA_GVA + COUNTER_OFFSET);
  cpu->set_reg(s, 4, DATA_GVA + COUNTER2_OFFSET);
  cpu->set_reg(s, 3, w->increments);
  cpu->set_pc(s, CODE_GVA);
  uint64_t rng = w->seed;
  for (uint64_t runs = 0; !w->done && runs < 100000000ull; runs++) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    const CPU_ExitReason exit = cpu->run(s, 1u + rng % 300u);
    if (exit != CPU_EXIT_CYCLES_ELAPSED && !w->done) break;
  }
  w->ok = w->done;
  cpu->destroy(s);
  return NULL;
}

int main(int argc, char **argv) {
  const uint32_t increments = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : DEFAULT_INCREMENTS;
  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  CHECK_OK(vmm_map(vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(vmm, DATA_GVA, DATA_PA, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write_physical(vmm, CODE_PA, k_program, sizeof(k_program)));
  const uint32_t zero[2] = {0, 0};
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + COUNTER_OFFSET, zero, sizeof(uint32_t)));
  CHECK_OK(vmm_write_physical(vmm, DATA_PA + COUNTER2_OFFSET, zero, sizeof(uint32_t)));
  CHECK(TEST_BACKEND->supports_multicore);
  cpu_set_multicore(true);
#ifdef SWITCH_CPU_BACKEND_JIT
  jit_set_hot_threshold(8); /* the loop compiles at once, on every core */
#endif

  static Worker workers[THREADS];
  for (uint32_t i = 0; i < THREADS; i++) {
    workers[i] = (Worker){.vmm = vmm, .increments = increments, .seed = 0x9E3779B97F4A7C15ull * (i + 1u)};
    CHECK(pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]) == 0);
  }
  for (uint32_t i = 0; i < THREADS; i++) {
    CHECK(pthread_join(workers[i].thread, NULL) == 0);
    CHECK(workers[i].ok);
  }
  cpu_set_multicore(false);

  uint32_t counter = 0, counter2 = 0;
  CHECK_OK(vmm_read_physical(vmm, DATA_PA + COUNTER_OFFSET, &counter, sizeof(counter)));
  CHECK_OK(vmm_read_physical(vmm, DATA_PA + COUNTER2_OFFSET, &counter2, sizeof(counter2)));
  printf("[a32_parallel_test] %u threads x %u: counter %u (want %u), counter2 %u (want %u)\n", THREADS, increments,
         counter, THREADS * increments, counter2, 2u * THREADS * increments);
  CHECK(counter == THREADS * increments);
  CHECK(counter2 == 2u * THREADS * increments);
  vmm_destroy(vmm);
  layout_destroy();
  printf("[a32_parallel_test] passed\n");
  return 0;
}
