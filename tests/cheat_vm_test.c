/**
 * Unit tests for core/hle/cheat_vm.c: parsing Atmosphere cheat text and
 * running the opcodes against a real vmm (an RW "heap" page and an RX
 * "main" page, which cheats patch through the debugger path).
 */
#define CHECK_NAME "cheat_vm_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "hle/cheat_vm.h"

#include <string.h>

#define MAIN_GVA ((uint64_t)0x08000000)
#define HEAP_GVA ((uint64_t)0x10000000)

static VMM_Context *g_vmm;
static Cheat_VM g_vm;

static uint32_t read32(uint64_t gva) {
  uint32_t v = 0;
  CHECK_OK(vmm_read32(g_vmm, gva, &v));
  return v;
}

static void run_text(const char *text, uint32_t keys) {
  cheat_vm_reset(&g_vm);
  CHECK(cheat_vm_parse(&g_vm, text, strlen(text)) >= 1);
  const Cheat_Env env = {g_vmm, MAIN_GVA, HEAP_GVA, 0, 0, keys};
  cheat_vm_run(&g_vm, &env);
}

static void test_parse(void) {
  const char text[] =
      "{Master}\n040A0000 00000010 12345678\n\n"
      "[Infinite HP]\r\n04100000 00000020 0000270F\n"
      "[Two words] 20000000\n";
  cheat_vm_reset(&g_vm);
  CHECK(cheat_vm_parse(&g_vm, text, strlen(text)) == 3);
  CHECK(g_vm.entries[0].master && !g_vm.entries[1].master);
  CHECK(strcmp(g_vm.entries[1].name, "Infinite HP") == 0);
  CHECK(g_vm.entries[0].count == 3 && g_vm.entries[0].ops[0] == 0x040A0000u);
  CHECK(g_vm.entries[2].count == 1);
}

static void test_store_and_regions(void) {
  /* 0: heap + 0x10 = 0x2A (4 bytes); main + 0x20 = 0xD503201F (RX page). */
  run_text("[a]\n04100000 00000010 0000002A\n04000000 00000020 D503201F\n", 0);
  CHECK(read32(HEAP_GVA + 0x10) == 0x2A);
  CHECK(read32(MAIN_GVA + 0x20) == 0xD503201Fu);
  /* 8-byte store. */
  run_text("[a]\n08100000 00000040 11111111 22222222\n", 0);
  CHECK(read32(HEAP_GVA + 0x40) == 0x22222222u && read32(HEAP_GVA + 0x44) == 0x11111111u);
}

static void test_conditionals_and_keys(void) {
  CHECK_OK(vmm_write32(g_vmm, HEAP_GVA + 0x100, 5));
  /* if [heap+0x100] == 5 then [heap+0x104]=1 else [heap+0x108]=1 */
  run_text("[c]\n14150000 00000100 00000005\n04100000 00000104 00000001\n21000000\n04100000 00000108 00000001\n20000000\n", 0);
  CHECK(read32(HEAP_GVA + 0x104) == 1 && read32(HEAP_GVA + 0x108) == 0);
  run_text("[c]\n14160000 00000100 00000005\n04100000 0000010C 00000001\n21000000\n04100000 00000110 00000001\n20000000\n", 0);
  CHECK(read32(HEAP_GVA + 0x10C) == 0 && read32(HEAP_GVA + 0x110) == 1);
  /* Key conditional: A (bit 0) held. */
  run_text("[k]\n80000001\n04100000 00000120 00000007\n20000000\n", 0);
  CHECK(read32(HEAP_GVA + 0x120) == 0);
  run_text("[k]\n80000001\n04100000 00000120 00000007\n20000000\n", 1);
  CHECK(read32(HEAP_GVA + 0x120) == 7);
}

static void test_registers_loop_arith(void) {
  /* R0 = heap base + 0x200 via 5 (load) of a pointer, then loop 4x
   * storing 0xAB at [R0] with increment. */
  CHECK_OK(vmm_write32(g_vmm, HEAP_GVA + 0x1F0, (uint32_t)(HEAP_GVA + 0x200)));
  CHECK_OK(vmm_write32(g_vmm, HEAP_GVA + 0x1F4, 0));
  run_text("[r]\n"
           "58100000 000001F0\n"          /* R0 = [heap+0x1F0] (8 bytes) */
           "30010000 00000004\n"          /* loop R1 = 4 */
           "61001000 00000000 000000AB\n" /* [R0] = 0xAB (1 byte), R0 += 1 */
           "31010000\n",
           0);
  CHECK(read32(HEAP_GVA + 0x200) == 0xABABABABu);
  /* 9: R2 = 40 + 2 (static), A: [heap + R3=0x300] = R2 (offset type 3). */
  run_text("[m]\n"
           "40020000 00000000 00000028\n"
           "94022100 00000002\n"   /* R2 = R2 + 2 (4-byte) */
           "40030000 00000000 00000300\n"
           "A4230310\n",           /* [heap(x=1) + R3] = R2 */
           0);
  CHECK(read32(HEAP_GVA + 0x300) == 42);
  /* C0 register conditional against a static value, C3 static registers. */
  run_text("[s]\n"
           "40040000 00000000 00000009\n"
           "C0454400 00000009\n"   /* if R4 == 9 (4-byte, static) */
           "C3000804\n"            /* static[0x80] = R4 */
           "20000000\n",
           0);
  CHECK(g_vm.static_registers[0x80] == 9);
}

static void test_runaway_and_unknown(void) {
  /* An infinite loop ends at CHEAT_MAX_STEPS; an unknown opcode stops. */
  run_text("[x]\n30010000 FFFFFFFF\n31010000\n", 0);
  run_text("[y]\nB0000000\n04100000 00000400 00000001\n", 0);
  CHECK(read32(HEAP_GVA + 0x400) == 0);
  /* Disabled cheats do not run. */
  cheat_vm_reset(&g_vm);
  const char t[] = "[z]\n04100000 00000404 00000001\n";
  CHECK(cheat_vm_parse(&g_vm, t, strlen(t)) == 1);
  g_vm.entries[0].enabled = false;
  const Cheat_Env env = {g_vmm, MAIN_GVA, HEAP_GVA, 0, 0, 0};
  cheat_vm_run(&g_vm, &env);
  CHECK(read32(HEAP_GVA + 0x404) == 0);
}

int main(void) {
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, MAIN_GVA, 0x1000, VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(g_vmm, HEAP_GVA, 0x2000, VMM_PAGE_SIZE, VMM_PERM_RW));
  test_parse();
  test_store_and_regions();
  test_conditionals_and_keys();
  test_registers_loop_arith();
  test_runaway_and_unknown();
  vmm_destroy(g_vmm);
  layout_destroy();
  printf("[cheat_vm_test] all tests passed\n");
  return 0;
}
