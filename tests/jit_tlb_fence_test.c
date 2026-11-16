/**
 * The A64 JIT's translation cache within a region, in multicore mode
 * (jit_compile.c emit_tlb_recheck): a guest thread reads a word (its
 * page's translation now cached), spins on a flag with LDAR, and reads the
 * word again. Meanwhile another host thread maps that page to other
 * physical memory and then sets the flag. After the acquire (or, spinning
 * on a plain load, the next time round the loop) the second read must see
 * the new mapping, although the region never exited - the
 * cached translation reached the old physical page (Tears of the Kingdom's
 * loader corrupted memory that way).
 *
 * Natively the backend is the interpreter; tests/jit_wasm builds the same
 * file against the JIT with a hot threshold of 1.
 */
#define _POSIX_C_SOURCE 200809L /* nanosleep under -std=c11 */
#define CHECK_NAME "jit_tlb_fence_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#ifdef SWITCH_CPU_BACKEND_JIT
#define TEST_BACKEND (&CPU_BACKEND_JIT)
#else
#define TEST_BACKEND (&CPU_BACKEND_INTERPRETER)
#endif

#define CODE_GVA 0x100000ull
#define CODE_PA 0x0ull
#define DATA_GVA 0x200000ull
#define FLAG_GVA 0x300000ull
#define DATA_PA_OLD 0x1000ull
#define DATA_PA_NEW 0x2000ull
#define FLAG_PA 0x3000ull
#define OLD_WORD 0x11111111u
#define NEW_WORD 0x22222222u
#define RUNS 16u
#define REMAP_DELAY_NS 20000000l /* the guest spins this long first */
#define RUN_BUDGET 4000000000u   /* the whole spin in one run */

/* x0 = DATA_GVA, x4 = FLAG_GVA:
 *   0x00 ldr w1, [x0]    ; through the cache (old page)
 *   0x04 ldar w3, [x4]   ; the flag
 *   0x08 cbz w3, 0x04    ; spin
 *   0x0c ldr w2, [x0]    ; must read the new page
 *   0x10 svc #0 */
static const uint32_t k_program[] = {0xB9400001u, 0x88DFFC83u, 0x34FFFFE3u, 0xB9400002u, 0xD4000001u};
/* A plain load of the flag (no fence): the loop's back edge rechecks. The
 * data is read after one more time round once the flag was seen (w6, the
 * last iteration's flag), so a back edge comes between - a load in the
 * same iteration as the flag is not ordered after it on hardware either,
 * and one during the remap would fault there too.
 *   0x00 ldr w1, [x0]
 *   0x04 mov w6, w3      ; the flag the last iteration saw (x3 = 0 first)
 *   0x08 ldr w3, [x4]
 *   0x0c cbz w6, 0x04
 *   0x10 ldr w2, [x0]    ; must read the new page
 *   0x14 svc #0 */
static const uint32_t k_program_plain[] = {0xB9400001u, 0x2A0303E6u, 0xB9400083u, 0x34FFFFC6u, 0xB9400002u, 0xD4000001u};
#define PLAIN_GVA (CODE_GVA + 0x100u)

static VMM_Context *g_vmm;
static volatile bool g_done;

static void on_svc(CPU_State *state, uint32_t imm, void *userdata) {
  (void)state;
  (void)imm;
  (void)userdata;
  g_done = true;
}

/* The other core: remaps the data page, then raises the flag (release). */
static void *remapper(void *arg) {
  (void)arg;
  const struct timespec delay = {0, REMAP_DELAY_NS};
  nanosleep(&delay, NULL);
  CHECK_OK(vmm_unmap(g_vmm, DATA_GVA, VMM_PAGE_SIZE));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_NEW, VMM_PAGE_SIZE, VMM_PERM_RW));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  const uint32_t one = 1;
  CHECK_OK(vmm_write_physical(g_vmm, FLAG_PA, &one, sizeof(one)));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return NULL;
}

static void run_once(const CPU_Backend *cpu, CPU_State *s, uint64_t entry) {
  CHECK_OK(vmm_unmap(g_vmm, DATA_GVA, VMM_PAGE_SIZE));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_OLD, VMM_PAGE_SIZE, VMM_PERM_RW));
  const uint32_t zero = 0;
  CHECK_OK(vmm_write_physical(g_vmm, FLAG_PA, &zero, sizeof(zero)));
  cpu->set_reg(s, 0, DATA_GVA);
  cpu->set_reg(s, 4, FLAG_GVA);
  cpu->set_reg(s, 1, 0);
  cpu->set_reg(s, 2, 0);
  cpu->set_reg(s, 3, 0);
  cpu->set_pc(s, entry);
  g_done = false;
  pthread_t other;
  CHECK(pthread_create(&other, NULL, remapper, NULL) == 0);
  while (!g_done) {
    const CPU_ExitReason reason = cpu->run(s, RUN_BUDGET);
    CHECK(reason == CPU_EXIT_CYCLES_ELAPSED || reason == CPU_EXIT_SVC);
  }
  CHECK(pthread_join(other, NULL) == 0);
  CHECK(cpu->get_reg(s, 1) == OLD_WORD);
  if (cpu->get_reg(s, 2) != NEW_WORD)
    fprintf(stderr, "[jit_tlb_fence_test] program at 0x%llx: read 0x%llx after the flag (0x%x is the old page)\n",
            (unsigned long long)entry, (unsigned long long)cpu->get_reg(s, 2), OLD_WORD);
  CHECK(cpu->get_reg(s, 2) == NEW_WORD);
}

int main(void) {
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_OLD, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_map(g_vmm, FLAG_GVA, FLAG_PA, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA, k_program, sizeof(k_program)));
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA + (PLAIN_GVA - CODE_GVA), k_program_plain, sizeof(k_program_plain)));
  const uint32_t old_word = OLD_WORD, new_word = NEW_WORD;
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA_OLD, &old_word, sizeof(old_word)));
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA_NEW, &new_word, sizeof(new_word)));
#ifdef SWITCH_CPU_BACKEND_JIT
  jit_set_hot_threshold(1);
#endif
  const CPU_Backend *cpu = TEST_BACKEND;
  CHECK(cpu->supports_multicore);
  cpu_set_multicore(true);
  CPU_State *s = cpu->create(g_vmm, NULL);
  CHECK(s != NULL);
  cpu->set_svc_handler(s, on_svc);
  for (uint32_t r = 0; r < RUNS; r++) run_once(cpu, s, CODE_GVA); /* compiled from the first runs on */
  for (uint32_t r = 0; r < RUNS; r++) run_once(cpu, s, PLAIN_GVA);
  cpu->destroy(s);
  cpu_set_multicore(false);
  vmm_destroy(g_vmm);
  layout_destroy();
  printf("[jit_tlb_fence_test] passed\n");
  return 0;
}
