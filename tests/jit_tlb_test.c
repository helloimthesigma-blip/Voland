/**
 * The JIT's translation cache across regions (jit_compile.c emit_walk):
 * a region reads a word, leaves through BX (storing its cache in the
 * state), an SVC remaps that page to other physical memory with other
 * contents, and the next region reads the word again - it must see the
 * new mapping (the vmm generation moved, so the carried cache is not
 * used). Run serially and in multicore mode.
 *
 * Natively the backend is the A32 interpreter; tests/jit_wasm builds the
 * same file against the A32 JIT with a hot threshold of 1.
 */
#define CHECK_NAME "jit_tlb_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/a32/a32.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <stdio.h>

#ifdef SWITCH_CPU_BACKEND_JIT
#define TEST_BACKEND (&CPU_BACKEND_A32_JIT)
#else
#define TEST_BACKEND (&CPU_BACKEND_A32)
#endif

#define CODE_GVA 0x100000ull
#define CODE_PA 0x0ull
#define DATA_GVA 0x200000ull
#define DATA_PA_OLD 0x1000ull
#define DATA_PA_NEW 0x2000ull
#define OLD_WORD 0x11111111u
#define NEW_WORD 0x22222222u
#define SVC_REMAP 1u
#define SVC_DONE 0u
#define RUNS 64u

/* r0 = DATA_GVA, r3 = CODE_GVA + 8:
 *   0x00 ldr r1, [r0]   ; through the cache (old page)
 *   0x04 bx r3          ; leaves the region: its cache goes to the state
 *   0x08 svc #1         ; remap DATA_GVA to DATA_PA_NEW
 *   0x0c ldr r2, [r0]   ; must read the new page
 *   0x10 svc #0 */
static const uint32_t k_program[] = {0xE5901000u, 0xE12FFF13u, 0xEF000001u, 0xE5902000u, 0xEF000000u};

static VMM_Context *g_vmm;
static bool g_done;

static void on_svc(CPU_State *state, uint32_t swi, void *userdata) {
  (void)state;
  (void)userdata;
  if (swi == SVC_REMAP) {
    CHECK_OK(vmm_unmap(g_vmm, DATA_GVA, VMM_PAGE_SIZE));
    CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_NEW, VMM_PAGE_SIZE, VMM_PERM_RW));
  } else {
    g_done = true;
  }
}
static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
}

static void run_once(const CPU_Backend *cpu, CPU_State *s) {
  CHECK_OK(vmm_unmap(g_vmm, DATA_GVA, VMM_PAGE_SIZE));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_OLD, VMM_PAGE_SIZE, VMM_PERM_RW));
  cpu->set_reg(s, 0, DATA_GVA);
  cpu->set_reg(s, 3, CODE_GVA + 8u);
  cpu->set_reg(s, 1, 0);
  cpu->set_reg(s, 2, 0);
  cpu->set_pc(s, CODE_GVA);
  g_done = false;
  for (uint32_t i = 0; i < 100u && !g_done; i++) {
    const CPU_ExitReason reason = cpu->run(s, 1000u);
    CHECK(reason == CPU_EXIT_CYCLES_ELAPSED || reason == CPU_EXIT_SVC);
  }
  CHECK(g_done);
  CHECK(cpu->get_reg(s, 1) == OLD_WORD);
  CHECK(cpu->get_reg(s, 2) == NEW_WORD);
}

int main(void) {
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(g_vmm, DATA_GVA, DATA_PA_OLD, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA, k_program, sizeof(k_program)));
  const uint32_t old_word = OLD_WORD, new_word = NEW_WORD;
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA_OLD, &old_word, sizeof(old_word)));
  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA_NEW, &new_word, sizeof(new_word)));
#ifdef SWITCH_CPU_BACKEND_JIT
  jit_set_hot_threshold(1);
#endif
  const CPU_Backend *cpu = TEST_BACKEND;
  for (uint32_t multicore = 0; multicore < 2u; multicore++) {
    if (multicore && !cpu->supports_multicore) break;
    cpu_set_multicore(multicore != 0);
    CPU_State *s = cpu->create(g_vmm, NULL);
    CHECK(s != NULL);
    cpu->set_svc_handler(s, on_svc);
    cpu->set_undefined_handler(s, on_undefined);
    for (uint32_t r = 0; r < RUNS; r++) run_once(cpu, s); /* compiled from the first run on */
    cpu->destroy(s);
  }
  cpu_set_multicore(false);
  vmm_destroy(g_vmm);
  layout_destroy();
  printf("[jit_tlb_test] passed\n");
  return 0;
}
