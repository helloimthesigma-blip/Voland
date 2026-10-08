/**
 * The A32 interpreter's side of the AArch32 differential test
 * (tools/a32-oracle.py runs the same cases on Unicorn, a QEMU-based CPU
 * emulator used only as a black-box reference, and compares).
 *
 *   a32_runner CASES RESULTS
 *
 * CASES: "A32C", u32 count, then per case: u32 n, n instruction words,
 * r0-r14, CPSR (NZCVQ, GE), FPSCR, D0-D31 (u64), DATA_BYTES of data.
 * RESULTS: "A32R", then per case: u32 exit (0 ran n instructions,
 * 1 undefined, 2 fault, 3 other), u32 pc offset from CODE_GVA, r0-r14,
 * CPSR, FPSCR, D0-D31, the data.
 * Code is at CODE_GVA (read-execute), data at DATA_GVA (two pages,
 * read-write).
 */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/a32/a32.h"
#include "cpu/cpu.h"

#define CODE_GVA 0x100000ull
#define DATA_GVA 0x200000ull
#define CODE_PA 0x0ull
#define DATA_PA 0x10000ull
#define DATA_BYTES 0x2000u
#define MAX_INSNS 256u
#define D_REGS 32u

static VMM_Context *g_vmm;
static bool g_undefined;

static void on_undefined(CPU_State *s, uint32_t insn, void *user) {
  (void)s;
  (void)insn;
  (void)user;
  g_undefined = true;
}

static bool read_u32s(FILE *f, uint32_t *out, size_t n) { return fread(out, 4, n, f) == n; }

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: a32_runner CASES RESULTS\n");
    return 2;
  }
  FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
  CHECK(in && out);
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX | VMM_PERM_W)); /* the runner writes each case's code */
  for (uint64_t page = 0; page < DATA_BYTES / VMM_PAGE_SIZE; page++)
    CHECK_OK(vmm_map(g_vmm, DATA_GVA + page * VMM_PAGE_SIZE, DATA_PA + page * VMM_PAGE_SIZE, VMM_PAGE_SIZE, VMM_PERM_RW));
  const CPU_Backend *cpu = &CPU_BACKEND_A32;
  CPU_State *s = cpu->create(g_vmm, NULL);
  CHECK(s != NULL);
  cpu->set_undefined_handler(s, on_undefined);
  char magic[4];
  uint32_t count = 0;
  CHECK(fread(magic, 1, 4, in) == 4 && !memcmp(magic, "A32C", 4) && read_u32s(in, &count, 1));
  fwrite("A32R", 1, 4, out);
  static uint32_t code[MAX_INSNS];
  static uint8_t data[DATA_BYTES];
  for (uint32_t c = 0; c < count; c++) {
    uint32_t n = 0, r[15], cpsr, fpscr;
    uint64_t d[D_REGS];
    CHECK(read_u32s(in, &n, 1) && n <= MAX_INSNS && read_u32s(in, code, n));
    CHECK(read_u32s(in, r, 15) && read_u32s(in, &cpsr, 1) && read_u32s(in, &fpscr, 1));
    CHECK(fread(d, 8, D_REGS, in) == D_REGS && fread(data, 1, DATA_BYTES, in) == DATA_BYTES);
    CHECK_OK(vmm_write_block(g_vmm, CODE_GVA, code, (uint64_t)n * 4u)); /* the vmm's own path, as a loader writes code */
    CHECK_OK(vmm_write_block(g_vmm, DATA_GVA, data, DATA_BYTES));
    cpu->clear_cache(s);
    for (uint8_t i = 0; i < 15u; i++) cpu->set_reg(s, i, r[i]);
    cpu->set_sp(s, r[13]);
    cpu->set_pc(s, CODE_GVA);
    cpu->set_pstate(s, cpsr & CPU_PSTATE_NZCV_MASK);
    a32_set_state_for_test(s, cpsr, fpscr);
    for (uint8_t i = 0; i < D_REGS / 2u; i++) {
      CPU_Vector_Register v = {d[2u * i], d[2u * i + 1u]};
      cpu->set_vector_reg(s, i, v);
    }
    g_undefined = false;
    uint32_t exit_code = 0;
    for (uint32_t i = 0; i < n; i++) {
      const CPU_ExitReason why = cpu->step(s);
      if (why == CPU_EXIT_CYCLES_ELAPSED) continue;
      if (why == CPU_EXIT_SVC) continue;
      exit_code = g_undefined ? 1u : why == CPU_EXIT_FAULT ? 2u : 3u;
      break;
    }
    const uint32_t pc_offset = (uint32_t)(cpu->get_pc(s) - CODE_GVA);
    fwrite(&exit_code, 4, 1, out);
    fwrite(&pc_offset, 4, 1, out);
    for (uint8_t i = 0; i < 15u; i++) {
      const uint32_t v = (uint32_t)cpu->get_reg(s, i);
      fwrite(&v, 4, 1, out);
    }
    const uint32_t out_cpsr = a32_cpsr(s), out_fpscr = a32_fpscr_of(s);
    fwrite(&out_cpsr, 4, 1, out);
    fwrite(&out_fpscr, 4, 1, out);
    for (uint8_t i = 0; i < D_REGS / 2u; i++) {
      const CPU_Vector_Register v = cpu->get_vector_reg(s, i);
      fwrite(&v.lo, 8, 1, out);
      fwrite(&v.hi, 8, 1, out);
    }
    CHECK_OK(vmm_read_block(g_vmm, DATA_GVA, data, DATA_BYTES));
    fwrite(data, 1, DATA_BYTES, out);
  }
  fclose(out);
  fclose(in);
  return 0;
}
