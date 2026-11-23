/**
 * The JIT's cross-session hot list (jit.h jit_hot_*, docs/JIT.md "Hot code
 * across sessions"): offsets imported, kept once, exported back; offsets
 * outside the code region skipped; a move of the same program keeps them,
 * another program forgets them.
 *
 * Natively (the native-jit preset) nothing compiles, so only the list is
 * checked; tests/jit_wasm builds the same file under Emscripten, where an
 * imported offset must compile the first time its PC runs although the
 * hot threshold is far away, and only then stop waiting.
 */
#define CHECK_NAME "jit_hot_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/a32/a32.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <stdio.h>

#define TEST_BACKEND (&CPU_BACKEND_A32_JIT)

#define CODE_GVA 0x100000ull
#define CODE_PA 0x0ull
#define CODE_BYTES 0x4000ull
#define MOVED_GVA 0x900000ull
#define FAR_THRESHOLD 1000000u
#define RUN_CYCLES 100u
#define EXPORT_CAPACITY 16u

/*   0x00 add r0, r0, #1
 *   0x04 svc #0 */
static const uint32_t k_program[] = {0xE2800001u, 0xEF000000u};

static bool g_done;

static void on_svc(CPU_State *state, uint32_t swi, void *userdata) {
  (void)state;
  (void)swi;
  (void)userdata;
  g_done = true;
}
static void on_undefined(CPU_State *state, uint32_t insn, void *userdata) {
  (void)state;
  (void)insn;
  (void)userdata;
}

static bool exported(uint32_t offset) {
  uint32_t out[EXPORT_CAPACITY];
  const uint32_t n = jit_hot_export(out, EXPORT_CAPACITY);
  for (uint32_t i = 0; i < n; i++) {
    if (out[i] == offset) return true;
  }
  return false;
}

static void check_list(void) {
  jit_hot_set_code(CODE_GVA, CODE_BYTES);
  CHECK(jit_hot_count() == 0);
  const uint32_t offsets[] = {0x0u, 0x40u, 0x40u, (uint32_t)CODE_BYTES, 0x3FFCu};
  CHECK(jit_hot_import(offsets, sizeof(offsets) / sizeof(offsets[0])) == 3u); /* a repeat and one outside */
  CHECK(jit_hot_count() == 3u);
  CHECK(jit_hot_pending() == 3u);
  CHECK(exported(0x0u) && exported(0x40u) && exported(0x3FFCu));
  CHECK(!exported((uint32_t)CODE_BYTES));
  uint32_t one;
  CHECK(jit_hot_export(&one, 1u) == 1u); /* bounded by the capacity */
  CHECK(jit_hot_import(offsets, 2u) == 0u); /* nothing new */

  jit_hot_set_code(MOVED_GVA, CODE_BYTES); /* the same program elsewhere (a save state) */
  CHECK(jit_hot_count() == 3u);
  CHECK(exported(0x40u));
  jit_hot_set_code(CODE_GVA, CODE_BYTES + VMM_PAGE_SIZE); /* another program */
  CHECK(jit_hot_count() == 0);
  CHECK(jit_hot_pending() == 0);
  jit_hot_set_code(0, 0);
  CHECK(jit_hot_import(offsets, 1u) == 0u); /* off */
}

static void check_first_sight(VMM_Context *vmm) {
  jit_hot_set_code(CODE_GVA, CODE_BYTES);
  const uint32_t entry = 0;
  CHECK(jit_hot_import(&entry, 1u) == 1u);
  const CPU_Backend *cpu = TEST_BACKEND;
  CPU_State *s = cpu->create(vmm, NULL);
  CHECK(s != NULL);
  cpu->set_svc_handler(s, on_svc);
  cpu->set_undefined_handler(s, on_undefined);
  cpu->set_reg(s, 0, 0);
  cpu->set_pc(s, CODE_GVA);
  const uint64_t compiled_before = jit_stats()->blocks_compiled;
  g_done = false;
  for (uint32_t i = 0; i < RUN_CYCLES && !g_done; i++) (void)cpu->run(s, RUN_CYCLES);
  CHECK(g_done);
  CHECK(cpu->get_reg(s, 0) == 1u);
#ifdef __EMSCRIPTEN__
  CHECK(jit_stats()->blocks_compiled > compiled_before); /* far below the threshold */
  CHECK(jit_hot_pending() == 0);
#else
  CHECK(jit_stats()->blocks_compiled == compiled_before);
  CHECK(jit_hot_pending() == 1u);
#endif
  CHECK(exported(0x0u));
  cpu->destroy(s);
  jit_hot_set_code(0, 0);
}

int main(void) {
  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  CHECK_OK(vmm_map(vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_write_physical(vmm, CODE_PA, k_program, sizeof(k_program)));
  jit_set_hot_threshold(FAR_THRESHOLD);
  check_list();
  check_first_sight(vmm);
  vmm_destroy(vmm);
  layout_destroy();
  printf("[jit_hot_test] passed\n");
  return 0;
}
