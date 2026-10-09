/**
 * The A32 JIT vs the A32 interpreter (docs/JIT.md "AArch32"): random A32
 * streams - biased towards what the front end inlines (data processing in
 * every operand form, conditional execution, MOVW/MOVT, multiplies, loads
 * and stores of every size, LDM/STM with PUSH/POP-style returns, B/BL/BX)
 * - run once through CPU_BACKEND_A32's run() and once through
 * CPU_BACKEND_A32_JIT's with a hot threshold of 1, with the same budgets.
 * r0-r14, PC, NZCV, exit reason, fault address, cycles and every data byte
 * must match. The interpreter itself is checked against Unicorn by
 * tools/a32-oracle.py.
 *
 * Natively nothing is compiled; tests/jit_wasm runs this under Node.
 *
 *   a32_jit_diff_test [iterations] [seed]
 */
#define CHECK_NAME "a32_jit_diff_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "cpu/backends/a32/a32.h"
#include "cpu/backends/jit/jit.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_GVA 0x100000ull
#define CODE_PA 0x0ull
#define DATA_GVA 0x200000ull
#define DATA_PA 0x10000ull
#define DATA_BYTES 0x4000u
#define STREAM_MAX 40u
#define STEP_LIMIT 400u
#define A32_SVC 0xEF000000u
#define DEFAULT_ITERATIONS 20000u
#define A32_REGS 15u

static VMM_Context *g_vmm;
static const CPU_Backend *g_ref_cpu, *g_jit_cpu;
static CPU_State *g_ref, *g_jit;
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

/* ---- templates --------------------------------------------------------- */

static uint32_t cond(void) { return pick(3) ? 0xEu : pick(15); }
/* r0-r7 and r12 are free; r8-r11 and r13 point into the data. */
static uint32_t any_reg(void) { return pick(16); }
static uint32_t dst(void) { const uint32_t r = pick(9); return r == 8 ? 12u : r; }
static uint32_t base(void) { const uint32_t r = pick(5); return r == 4 ? 13u : 8u + r; }

static uint32_t gen_dp(void) {
  const uint32_t opcode = pick(16), s = (opcode >= 8 && opcode <= 11) ? 1u : pick(2);
  const uint32_t rd = pick(40) == 0 ? 15u : dst();
  uint32_t w = (cond() << 28) | (opcode << 21) | (s << 20) | (any_reg() << 16) | (rd << 12);
  switch (pick(3)) {
  case 0: return w | (1u << 25) | (pick(16) << 8) | pick(256);
  case 1: return w | (pick(32) << 7) | (pick(4) << 5) | any_reg();
  default: return w | (pick(8) << 8) | (pick(4) << 5) | (1u << 4) | pick(8); /* Rs, Rm in r0-r7 */
  }
}
static uint32_t gen_move_wide(void) {
  return (cond() << 28) | (pick(2) ? 0x03000000u : 0x03400000u) | (pick(16) << 16) | (dst() << 12) | pick(4096);
}
static uint32_t gen_multiply(void) {
  static const uint32_t ops[7] = {0, 1, 3, 4, 5, 6, 7}; /* MUL MLA MLS UMULL UMLAL SMULL SMLAL */
  const uint32_t o = ops[pick(7)], s = o <= 1 ? pick(2) : 0;
  uint32_t hi = dst(), lo = dst();
  if (o >= 4 && hi == lo) lo = (lo + 1u) % 8u;
  return (cond() << 28) | (o << 21) | (s << 20) | (hi << 16) | (lo << 12) | (pick(8) << 8) | 0x90u | pick(8);
}
static uint32_t gen_load_store(void) {
  const uint32_t p = pick(4) ? 1u : 0u, w = p ? pick(3) == 0 : 0u, rn = base();
  uint32_t rt = pick(12) == 0 ? 14u : pick(8);
  const uint32_t load = pick(2);
  if (load && pick(30) == 0) rt = 15u;
  uint32_t word = (cond() << 28) | (1u << 26) | (p << 24) | (pick(2) << 23) | (pick(4) == 0 ? 1u << 22 : 0) | (w << 21) |
                  (load << 20) | (rn << 16) | (rt << 12);
  if (pick(3)) return word | pick(0x100);
  return word | (1u << 25) | (pick(3) << 7) | (pick(2) << 5) | 12u; /* r12: a small index */
}
static uint32_t gen_load_store_extra(void) {
  const uint32_t p = pick(4) ? 1u : 0u, w = p ? pick(3) == 0 : 0u, rn = base(), load = pick(2);
  const uint32_t op2 = 1u + pick(3);
  uint32_t rt = pick(8);
  if (!load && op2 >= 2u) rt &= ~1u; /* LDRD/STRD: even */
  const uint32_t imm = pick(256);
  uint32_t word = (cond() << 28) | (p << 24) | (pick(2) << 23) | (w << 21) | (load << 20) | (rn << 16) | (rt << 12) |
                  0x90u | (op2 << 5);
  if (pick(3)) return word | (1u << 22) | ((imm >> 4) << 8) | (imm & 15u);
  return word | 12u;
}
static uint32_t gen_block(void) {
  const uint32_t load = pick(2), rn = base();
  uint32_t list = pick(256) | (pick(4) == 0 ? 1u << 14 : 0) | (pick(4) == 0 ? 1u << 12 : 0);
  if (load && pick(6) == 0) list |= 1u << 15;
  if (!list) list = 1;
  const uint32_t mode = pick(4), w = pick(2);
  return (cond() << 28) | (0x4u << 25) | ((mode >> 1) << 24) | ((mode & 1u) << 23) | (w << 21) | (load << 20) |
         (rn << 16) | list;
}
static uint32_t gen_branch(int32_t here, uint32_t length) {
  const int32_t to = (int32_t)pick(length + 2u) - 1; /* within the stream, mostly */
  const uint32_t imm24 = (uint32_t)(to - here - 2) & 0xFFFFFFu;
  switch (pick(5)) {
  case 0: return (cond() << 28) | 0x0B000000u | imm24;         /* BL */
  case 1: return (cond() << 28) | 0x012FFF1Eu;                 /* BX LR */
  case 2: return (cond() << 28) | 0x012FFF30u | (pick(2) ? 14u : 7u); /* BLX LR / r7 */
  default: return (cond() << 28) | 0x0A000000u | imm24;        /* B */
  }
}

static uint32_t gen_media(void) {
  switch (pick(5)) {
  case 0: { /* SXTB/SXTH/UXTB/UXTH (+ accumulate) */
    static const uint32_t ops[4] = {2, 3, 6, 7};
    return (cond() << 28) | 0x06800070u | (ops[pick(4)] << 20) | ((pick(2) ? 15u : pick(8)) << 16) | (dst() << 12) |
           (pick(4) << 10) | pick(8);
  }
  case 1: { /* BFI / BFC */
    const uint32_t lsb = pick(32), msb = lsb + pick(32 - lsb);
    return (cond() << 28) | 0x07C00010u | (msb << 16) | (dst() << 12) | (lsb << 7) | (pick(3) ? pick(8) : 15u);
  }
  case 2: { /* UBFX / SBFX */
    const uint32_t lsb = pick(32), widthm1 = pick(32 - lsb);
    return (cond() << 28) | (pick(2) ? 0x07E00050u : 0x07A00050u) | (widthm1 << 16) | (dst() << 12) | (lsb << 7) | pick(8);
  }
  case 3: return (cond() << 28) | 0x016F0F10u | (dst() << 12) | pick(8); /* CLZ */
  default: return (cond() << 28) | (pick(2) ? 0x06BF0F30u : 0x06BF0FB0u) | (dst() << 12) | pick(8); /* REV / REV16 */
  }
}
static uint32_t gen_vfp(void) {
  const uint32_t dbl = pick(2), d = pick(16), n = pick(16), m = pick(16);
  const uint32_t vd = dbl ? d : d >> 1, dbit = dbl ? 0 : d & 1u;
  switch (pick(9)) {
  case 0: /* VLDR / VSTR through r8-r11 */
    return (cond() << 28) | 0x0D000A00u | (pick(2) << 23) | (dbit << 22) | (pick(2) << 20) | ((8u + pick(4)) << 16) |
           (vd << 12) | (dbl << 8) | pick(32);
  case 1: case 2: { /* VMUL / VADD / VSUB / VMLA / VNMUL / VDIV, VSQRT / VABS / VNEG / VMOV */
    static const uint32_t forms[10] = {0x0E200A00u, 0x0E300A00u, 0x0E300A40u, 0x0E000A00u, 0x0E200A40u,
                                       0x0E800A00u, 0x0EB10AC0u, 0x0EB00AC0u, 0x0EB10A40u, 0x0EB00A40u};
    const uint32_t f = forms[pick(10)], unary = (f & 0x00B00000u) == 0x00B00000u;
    return (cond() << 28) | f | (vd << 12) | (dbit << 22) | (unary ? 0 : ((dbl ? n : n >> 1) << 16)) | (dbl << 8) |
           (dbl ? m : m >> 1) | (unary ? 0 : ((dbl ? 0 : n & 1u) << 7)) | ((dbl ? 0 : m & 1u) << 5);
  }
  case 3: /* VMOV core <-> single */
    return (cond() << 28) | 0x0E000A10u | (pick(2) << 20) | ((d >> 1) << 16) | (dst() << 12) | ((d & 1u) << 7);
  case 4: /* VCMP then VMRS APSR_nzcv */
    return pick(2) ? (cond() << 28) | 0x0EB40A40u | (vd << 12) | (dbl << 8) | (dbl ? m : m >> 1)
                   : (cond() << 28) | 0x0EF1FA10u;
  case 5: /* VCVT to and from S32/U32 (op 7: signed / towards zero), F32 <-> F64 */
    switch (pick(3)) {
    case 0:
      return (cond() << 28) | (pick(2) ? 0x0EBD0A40u : 0x0EBC0A40u) | (pick(2) << 7) | (dbl << 8) | ((d >> 1) << 12) |
             ((d & 1u) << 22) | (dbl ? m : m >> 1) | ((dbl ? 0 : m & 1u) << 5);
    case 1:
      return (cond() << 28) | 0x0EB80A40u | (pick(2) << 7) | (dbl << 8) | (vd << 12) | (dbit << 22) | (m >> 1) |
             ((m & 1u) << 5);
    default: /* sz 1: Sd from Dm; 0: Dd from Sm */
      return (cond() << 28) | 0x0EB70AC0u | (dbl << 8) | ((dbl ? d >> 1 : d) << 12) | ((dbl ? d & 1u : 0) << 22) |
             (dbl ? m : m >> 1) | ((dbl ? 0 : m & 1u) << 5);
    }
  case 7: { /* ARMv8, unconditional: VSEL, VMAXNM/VMINNM, VRINTA/N/P/M, VCVTA/N/P/M */
    const uint32_t regs = (vd << 12) | (dbit << 22) | ((dbl ? n : n >> 1) << 16) | ((dbl ? 0 : n & 1u) << 7) |
                          (dbl ? m : m >> 1) | ((dbl ? 0 : m & 1u) << 5) | (dbl << 8);
    switch (pick(4)) {
    case 0: return 0xFE000A00u | (pick(4) << 20) | regs;
    case 1: return 0xFE800A00u | (pick(2) << 6) | regs;
    case 2: return 0xFEB80A40u | (pick(4) << 16) | (regs & ~(0xFu << 16) & ~(1u << 7));
    default: /* Sd destination */
      return 0xFEBC0A40u | (pick(4) << 16) | (pick(2) << 7) | (dbl << 8) | ((d >> 1) << 12) | ((d & 1u) << 22) |
             (dbl ? m : m >> 1) | ((dbl ? 0 : m & 1u) << 5);
    }
  }
  case 6: /* VMOV (immediate); VCMP/VCMPE with #0.0; the multiply-accumulates and fused forms */
    switch (pick(3)) {
    case 0: return (cond() << 28) | 0x0EB00A00u | (dbl << 8) | (vd << 12) | (dbit << 22) | (pick(16) << 16) | pick(16);
    case 1: return (cond() << 28) | 0x0EB50A40u | (pick(2) << 7) | (dbl << 8) | (vd << 12) | (dbit << 22);
    default: {
      static const uint32_t forms[8] = {0x0E000A00u, 0x0E000A40u, 0x0E100A00u, 0x0E100A40u,
                                        0x0EA00A00u, 0x0EA00A40u, 0x0E900A00u, 0x0E900A40u};
      return (cond() << 28) | forms[pick(8)] | (vd << 12) | (dbit << 22) | ((dbl ? n : n >> 1) << 16) | (dbl << 8) |
             (dbl ? m : m >> 1) | ((dbl ? 0 : n & 1u) << 7) | ((dbl ? 0 : m & 1u) << 5);
    }
    }
  default: /* NEON: VADD.I32 q, VLD1.32 {d} through r8-r11 */
    if (pick(2)) return 0xF2200840u | ((d & 14u) << 12) | ((n & 14u) << 16) | (m & 14u);
    if (pick(2)) {
      static const uint32_t types[4] = {7u, 0xAu, 6u, 2u};
      const uint32_t rm = pick(3) ? (pick(2) ? 15u : 13u) : pick(8);
      return 0xF4000000u | (pick(2) << 21) | (pick(2) << 22) | ((8u + pick(4)) << 16) | (pick(13) << 12) |
             (types[pick(4)] << 8) | (pick(16) << 4) | rm;
    }
    { /* VLDM / VSTM: IA, IA!, DB! (VPUSH / VPOP through sp) */
      static const uint32_t modes[3] = {0x0C800000u, 0x0CA00000u, 0x0D200000u};
      const uint32_t rn = pick(3) ? 8u + pick(4) : 13u, count = 1u + pick(8);
      return (cond() << 28) | modes[pick(3)] | (pick(2) << 20) | (rn << 16) | (dbit << 22) | (vd << 12) | 0xA00u |
             (dbl << 8) | (dbl ? 2u * count + (pick(8) == 0) : count);
    }
  }
}

/* LDREX/STREX (and the acquire/release forms, LDA/STL) through r8-r11,
 * TPIDRURO/TPIDRURW. */
static uint32_t gen_sync(void) {
  static const uint32_t ordered[3] = {0xF00u, 0xE00u, 0xC00u}; /* exclusive, acquire/release exclusive, plain */
  const uint32_t size = pick(3) ? 0u : 2u + pick(2), rn = 8u + pick(4), rd = dst();
  const uint32_t o = ordered[pick(4) ? 0 : 1u + pick(2)];
  uint32_t rt = pick(8);
  if (rt == rd) rt = (rt + 1u) & 7u;
  switch (pick(4)) {
  case 0: case 1: return (cond() << 28) | 0x0190009Fu | o | (size << 21) | (rn << 16) | (rd << 12);
  case 2: return (cond() << 28) | 0x01800090u | o | (size << 21) | (rn << 16) | ((o == 0xC00u ? 15u : rd) << 12) | rt;
  default:
    switch (pick(3)) {
    case 0: return (cond() << 28) | 0x0E1D0F70u | (rd << 12);
    case 1: return (cond() << 28) | 0x0E1D0F50u | (rd << 12);
    default: return (cond() << 28) | 0x0E0D0F50u | (pick(8) << 12);
    }
  }
}

/* Advanced SIMD: the inlined data-processing forms, single-lane and
 * all-lane VLD1/VST1, VMOV to and from scalars, VDUP. */
static uint32_t neon_regs(uint32_t q) {
  const uint32_t d = q ? pick(16) * 2u : pick(32), n = q ? pick(16) * 2u : pick(32), m = q ? pick(16) * 2u : pick(32);
  return ((d >> 4) << 22) | ((d & 15u) << 12) | ((n >> 4) << 7) | ((n & 15u) << 16) | ((m >> 4) << 5) | (m & 15u) |
         (q << 6);
}
static uint32_t gen_neon(void) {
  const uint32_t q = pick(2);
  switch (pick(10)) {
  case 0: case 1: { /* F32: VADD VSUB VMLA VMLS VMUL */
    static const uint32_t forms[5] = {0xF2000D00u, 0xF2200D00u, 0xF2000D10u, 0xF2200D10u, 0xF3000D10u};
    return forms[pick(5)] | neon_regs(q);
  }
  case 2: /* bitwise */
    return 0xF2000110u | (pick(2) << 24) | (pick(4) << 20) | neon_regs(q);
  case 3: /* VADD/VSUB (integer), VMUL.I16/I32 */
    if (pick(3) == 0) return 0xF2000910u | ((1u + pick(2)) << 20) | neon_regs(q);
    return 0xF2000800u | (pick(2) << 24) | (pick(4) << 20) | neon_regs(q);
  case 4: { /* VMLA/VMLS/VMUL.F32 by scalar */
    static const uint32_t ops[3] = {0x1u, 0x5u, 0x9u};
    const uint32_t d = q ? pick(16) * 2u : pick(32), n = q ? pick(16) * 2u : pick(32);
    return 0xF2A00040u | (q << 24) | ((d >> 4) << 22) | ((d & 15u) << 12) | ((n >> 4) << 7) | ((n & 15u) << 16) |
           (ops[pick(3)] << 8) | (pick(2) << 5) | pick(16);
  }
  case 5: /* VEXT, VDUP (scalar) */
    if (pick(2)) return 0xF2B00000u | ((q ? pick(16) : pick(8)) << 8) | neon_regs(q);
    {
      static const uint32_t imm4[3] = {1u, 2u, 4u};
      const uint32_t k = pick(3), index = pick(8u >> k);
      return 0xF3B00C00u | ((imm4[k] | (index << (k + 1u))) << 16) | (neon_regs(q) & ~(0xFu << 16) & ~(1u << 7));
    }
  case 6: /* VMOV/VMVN/VORR/VBIC (immediate) */
    return 0xF2800010u | (pick(2) << 24) | (pick(8) << 16) | (pick(16) << 8) | (pick(2) << 5) | pick(16) |
           (neon_regs(q) & ((1u << 22) | (0xFu << 12) | (1u << 6)));
  case 7: { /* VLD1/VST1 one lane, VLD1 all lanes, through r8-r11 */
    const uint32_t rm = pick(3) ? (pick(2) ? 15u : 13u) : pick(8), d = pick(31);
    const uint32_t base = 0xF4800000u | ((8u + pick(4)) << 16) | ((d >> 4) << 22) | ((d & 15u) << 12) | rm;
    if (pick(3) == 0) return base | (1u << 21) | (3u << 10) | (pick(3) << 6) | (pick(2) << 5);
    return base | (pick(2) << 21) | (pick(3) << 10) | (pick(16) << 4);
  }
  default: { /* VMOV core <-> scalar, VDUP (core) */
    const uint32_t d = pick(32);
    const uint32_t regs = ((d >> 4) << 7) | ((d & 15u) << 16) | (dst() << 12);
    if (pick(4) == 0) return (cond() << 28) | 0x0E800B10u | (pick(2) << 22) | (pick(2) << 21) | (pick(2) << 5) | regs;
    return (cond() << 28) | 0x0E000B10u | (pick(2) << 20) | (pick(2) << 23) | (pick(4) << 21) | (pick(4) << 5) | regs;
  }
  }
}

static uint32_t gen(int32_t here, uint32_t length) {
  static int only = -2; /* debugging: A32_GEN=k keeps one generator */
  if (only == -2) only = getenv("A32_GEN") ? atoi(getenv("A32_GEN")) : -1;
  switch (only >= 0 ? (uint32_t)only : pick(21)) {
  case 20: return gen_neon();
  case 19: return gen_sync();
  case 16: return gen_media();
  case 17: case 18: return gen_vfp();
  case 0: case 1: case 2: case 3: case 4: return gen_dp();
  case 5: return gen_move_wide();
  case 6: return gen_multiply();
  case 7: case 8: case 9: return gen_load_store();
  case 10: case 11: return gen_load_store_extra();
  case 12: case 13: return gen_block();
  case 14: return gen_branch(here, length);
  default: return pick(4) ? 0xE92D4000u : 0xE8BD8000u; /* PUSH {lr} / POP {pc} */
  }
}

/* ---- running ------------------------------------------------------------ */

typedef struct Snapshot {
  uint64_t r[A32_REGS];
  uint64_t pc;
  uint32_t pstate;
  CPU_ExitReason exit;
  uint64_t fault;
  uint64_t cycles;
  CPU_Vector_Register v[16];
  uint64_t fpsr, fpcr;
} Snapshot;

/* Small functions after the stream, each returning one way the JIT
 * follows a BL into (BX LR; PUSH {r4, lr} .. POP {r4, pc}; STR lr, [sp,
 * #-4]! .. LDR pc, [sp], #4), a PLT stub to one of them, and BLs to them
 * in place of a few stream instructions: regions then run stub, callee
 * and return site inline. */
#define A32_BX_LR_INSN 0xE12FFF1Eu
#define VTABLE_REG 9u
static uint32_t g_vtable; /* code word index of a function-pointer table, 0: none */
static void add_callees(uint32_t *code, uint32_t length) {
  static const uint32_t prologue[3] = {0, 0xE92D4010u, 0xE52DE004u};
  static const uint32_t epilogue[3] = {A32_BX_LR_INSN, 0xE8BD8010u, 0xE49DF004u};
  uint32_t at = length + 1u, entry[4];
  for (uint32_t f = 0; f < 3u; f++) {
    entry[f] = at;
    if (prologue[f]) code[at++] = prologue[f];
    for (uint32_t k = pick(5); k; k--) code[at++] = gen_dp();
    code[at++] = epilogue[f];
  }
  /* A PLT stub (ADD ip, pc, #0; ADD ip, ip, #0; LDR pc, [ip, #off]!) to
   * one of them through a GOT slot after it. */
  entry[3] = at;
  const uint32_t got = at + 4u;
  code[at++] = 0xE28FC000u;
  code[at++] = 0xE28CC000u;
  code[at] = 0xE5BCF000u | (4u * got - (4u * entry[3] + 8u));
  at++;
  code[got] = (uint32_t)CODE_GVA + 4u * entry[pick(3)];
  /* A vtable of the three, called as LDR r12, [r9, #off]; BLX r12 (the
   * JIT predicts r12 from the entry state). */
  g_vtable = got + 1u;
  for (uint32_t f = 0; f < 3u; f++) code[g_vtable + f] = (uint32_t)CODE_GVA + 4u * entry[f];
  if (length >= 2u) {
    const uint32_t here = pick(length - 1u);
    code[here] = 0xE599C000u | (4u * pick(3));      /* LDR r12, [r9, #4k] */
    code[here + 1u] = (cond() << 28) | 0x012FFF3Cu; /* BLX r12 */
  }
  for (uint32_t n = 1u + pick(3); n; n--) {
    const uint32_t here = pick(length), f = entry[pick(4)];
    code[here] = (cond() << 28) | 0x0B000000u | ((uint32_t)((int32_t)f - (int32_t)here - 2) & 0xFFFFFFu);
  }
}

static uint32_t g_fpcr, g_fpsr;static uint32_t g_fpcr, g_fpsr;

static void set_state(const CPU_Backend *cpu, CPU_State *s, const uint32_t *r, uint32_t nzcv) {
  for (uint8_t i = 0; i < A32_REGS; i++) cpu->set_reg(s, i, r[i]);
  cpu->set_sp(s, r[13]);
  cpu->set_pstate(s, nzcv);
  cpu->set_pc(s, CODE_GVA);
  for (uint8_t i = 0; i < 16u; i++) { /* ordinary floats and doubles */
    float f[2] = {(float)(int32_t)(r[i % 8] & 0xFFF) / 3.0f, (float)(int32_t)((r[(i + 3) % 8] >> 4) & 0x3FF) - 300.0f};
    double dv = (double)(int32_t)(r[(i + 5) % 8] & 0xFFFF) / 7.0;
    switch ((r[(i + 1) % 8] >> 20) & 7u) { /* some exact values, zeros, tiny, NaN */
    case 0: f[0] = (float)(r[i % 8] & 0xFF); f[1] = 0.0f; dv = (double)(r[i % 8] & 0xFF); break;
    case 1: f[0] = -0.0f; dv = 1e-310; break;
    case 2: f[1] = 1e-39f; break;
    case 3: f[0] = __builtin_nanf(""); break;
    default: break;
    }
    CPU_Vector_Register v;
    memcpy(&v.lo, f, sizeof(f));
    memcpy(&v.hi, &dv, sizeof(dv));
    cpu->set_vector_reg(s, i, v);
  }
  cpu->set_sys_reg(s, CPU_SYSREG_TPIDRRO_EL0, r[3] ^ 0x5A5A0000u);
  cpu->set_sys_reg(s, CPU_SYSREG_TPIDR_EL0, r[4] ^ 0x0F0F0000u);
  cpu->set_sys_reg(s, CPU_SYSREG_FPCR, g_fpcr);
  cpu->set_sys_reg(s, CPU_SYSREG_FPSR, g_fpsr);
}

static Snapshot snapshot(const CPU_Backend *cpu, CPU_State *s, CPU_ExitReason exit, uint64_t cycles) {
  Snapshot snap;
  memset(&snap, 0, sizeof(snap));
  for (uint8_t i = 0; i < A32_REGS; i++) snap.r[i] = cpu->get_reg(s, i);
  snap.pc = cpu->get_pc(s);
  snap.pstate = cpu->get_pstate(s);
  snap.exit = exit;
  snap.fault = exit == CPU_EXIT_FAULT ? cpu->get_fault_address(s) : 0;
  snap.cycles = cycles;
  for (uint8_t i = 0; i < 16u; i++) snap.v[i] = cpu->get_vector_reg(s, i);
  snap.fpsr = cpu->get_sys_reg(s, CPU_SYSREG_FPSR);
  snap.fpcr = cpu->get_sys_reg(s, CPU_SYSREG_FPCR);
  return snap;
}

static void run_case(uint32_t iteration) {
  static uint32_t code[VMM_PAGE_SIZE / 4];
  for (uint32_t i = 0; i < VMM_PAGE_SIZE / 4; i++) code[i] = A32_SVC;
  const uint32_t length = 1u + pick(STREAM_MAX);
  for (uint32_t i = 0; i < length; i++) code[i] = gen((int32_t)i, length);
  if (pick(2)) code[length] = 0xEA000000u | ((uint32_t)(-(int32_t)pick(length + 1u) - 2) & 0xFFFFFFu); /* loop */
  g_vtable = 0;
  if (pick(3) == 0) add_callees(code, length);
  CHECK_OK(vmm_write_physical(g_vmm, CODE_PA, code, sizeof(code)));
  g_jit_cpu->clear_cache(g_jit);

  uint32_t r[A32_REGS];
  for (uint32_t i = 0; i < 8u; i++) r[i] = pick(4) == 0 ? (uint32_t)rnd() : pick(2) ? (uint32_t)rnd() & 0xFFFF : pick(4);
  for (uint32_t i = 8; i < 12u; i++) {
    r[i] = (uint32_t)(DATA_GVA + 0x400u + pick(DATA_BYTES - 0x800u));
    if (pick(2)) r[i] &= ~3u;
    if (pick(8) == 0) r[i] = (r[i] | 0xFFFu) - pick(24); /* near a page end: crossing accesses */
  }
  r[12] = pick(64);
  if (g_vtable && pick(2)) r[VTABLE_REG] = (uint32_t)CODE_GVA + 4u * g_vtable;
  r[13] = (uint32_t)(DATA_GVA + 0x1800u + pick(0x1000u)) & ~7u;
  r[14] = (uint32_t)(CODE_GVA + 4u * pick(length + 1u)) | (pick(40) == 0 ? 1u : 0u);
  if (pick(4) == 0) r[7] = (uint32_t)(CODE_GVA + 4u * pick(length + 1u));
  const uint32_t nzcv = (uint32_t)pick(16) << 28;
  g_fpcr = pick(4) ? 0u : (pick(2) << 24) | (pick(2) << 25) | (pick(4) << 22); /* FZ, DN, rounding */
  g_fpsr = pick(2) ? 0u : 0x10u;                                                /* IXC sticky or clear */
  for (uint32_t i = 0; i < DATA_BYTES; i++) g_data_init[i] = (uint8_t)rnd();
  /* POP {pc} often finds a return address into the stream on the stack. */
  for (uint32_t k = 0; k < 64u; k++) {
    const uint32_t at = (uint32_t)(r[13] - DATA_GVA) - 128u + 4u * k;
    const uint32_t target = (uint32_t)(CODE_GVA + 4u * pick(length + 1u));
    if (pick(2) && at + 4u <= DATA_BYTES) memcpy(g_data_init + at, &target, sizeof(target));
  }

  uint64_t budgets[STEP_LIMIT];
  for (uint32_t i = 0; i < STEP_LIMIT; i++) budgets[i] = 1u + pick(pick(2) ? 8u : 200u);

  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_ref_cpu, g_ref, r, nzcv);
  CPU_ExitReason ref_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t ref_cycles = 0;
  for (uint32_t i = 0; ref_cycles < STEP_LIMIT; i++) {
    const uint64_t budget = budgets[i] < STEP_LIMIT - ref_cycles ? budgets[i] : STEP_LIMIT - ref_cycles;
    ref_exit = g_ref_cpu->run(g_ref, budget);
    ref_cycles += g_ref_cpu->get_cycles_consumed(g_ref);
    if (ref_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot ref = snapshot(g_ref_cpu, g_ref, ref_exit, ref_cycles);
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, g_data_ref, DATA_BYTES));

  CHECK_OK(vmm_write_physical(g_vmm, DATA_PA, g_data_init, DATA_BYTES));
  set_state(g_jit_cpu, g_jit, r, nzcv);
  CPU_ExitReason jit_exit = CPU_EXIT_CYCLES_ELAPSED;
  uint64_t jit_cycles = 0;
  for (uint32_t i = 0; jit_cycles < STEP_LIMIT; i++) {
    const uint64_t budget = budgets[i] < STEP_LIMIT - jit_cycles ? budgets[i] : STEP_LIMIT - jit_cycles;
    jit_exit = g_jit_cpu->run(g_jit, budget);
    jit_cycles += g_jit_cpu->get_cycles_consumed(g_jit);
    if (jit_exit != CPU_EXIT_CYCLES_ELAPSED) break;
  }
  const Snapshot jit = snapshot(g_jit_cpu, g_jit, jit_exit, jit_cycles);
  static uint8_t data_jit[DATA_BYTES];
  CHECK_OK(vmm_read_physical(g_vmm, DATA_PA, data_jit, DATA_BYTES));

  const bool same = memcmp(ref.r, jit.r, sizeof(ref.r)) == 0 && ref.pc == jit.pc && ref.pstate == jit.pstate &&
                    ref.exit == jit.exit && ref.fault == jit.fault && ref.cycles == jit.cycles &&
                    memcmp(ref.v, jit.v, sizeof(ref.v)) == 0 && ref.fpsr == jit.fpsr && ref.fpcr == jit.fpcr &&
                    memcmp(g_data_ref, data_jit, DATA_BYTES) == 0;
  if (!same) {
    fprintf(stderr, "[a32_jit_diff_test] MISMATCH at iteration %u (%u instructions):\n", iteration, length);
    for (uint32_t i = 0; i <= length; i++) fprintf(stderr, "  %3u: %08x\n", i, code[i]);
    fprintf(stderr, "  exit %d/%d pc %llx/%llx cycles %llu/%llu fault %llx/%llx nzcv %x/%x (initial %x)\n", ref.exit,
            jit.exit, (unsigned long long)ref.pc, (unsigned long long)jit.pc, (unsigned long long)ref.cycles,
            (unsigned long long)jit.cycles, (unsigned long long)ref.fault, (unsigned long long)jit.fault, ref.pstate,
            jit.pstate, nzcv);
    for (uint32_t i = 0; i < A32_REGS; i++) {
      if (ref.r[i] != jit.r[i]) {
        fprintf(stderr, "  r%u %08llx / %08llx (initial %08x)\n", i, (unsigned long long)ref.r[i],
                (unsigned long long)jit.r[i], r[i]);
      }
    }
    for (int i = 0; i < 16; i++)
      if (memcmp(&ref.v[i], &jit.v[i], sizeof(ref.v[i])))
        fprintf(stderr, "  q%d %016llx:%016llx / %016llx:%016llx\n", i, (unsigned long long)ref.v[i].hi,
                (unsigned long long)ref.v[i].lo, (unsigned long long)jit.v[i].hi, (unsigned long long)jit.v[i].lo);
    if (ref.fpsr != jit.fpsr || ref.fpcr != jit.fpcr)
      fprintf(stderr, "  fpsr %08llx / %08llx, fpcr %08llx / %08llx\n", (unsigned long long)ref.fpsr,
              (unsigned long long)jit.fpsr, (unsigned long long)ref.fpcr, (unsigned long long)jit.fpcr);
    if (memcmp(g_data_ref, data_jit, DATA_BYTES) != 0) fprintf(stderr, "  data differs\n");
    exit(1);
  }
}

int main(int argc, char **argv) {
  const uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : DEFAULT_ITERATIONS;
  g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x2545F4914F6CDD1Dull;
  if (!g_rng) g_rng = 1;
  CHECK_OK(layout_create());
  g_vmm = vmm_create();
  CHECK(g_vmm != NULL);
  CHECK_OK(vmm_map(g_vmm, CODE_GVA, CODE_PA, VMM_PAGE_SIZE, VMM_PERM_RX));
  for (uint64_t page = 0; page < DATA_BYTES / VMM_PAGE_SIZE; page++) {
    const uint64_t physical = DATA_PA + (DATA_BYTES / VMM_PAGE_SIZE - 1u - page) * VMM_PAGE_SIZE;
    CHECK_OK(vmm_map(g_vmm, DATA_GVA + page * VMM_PAGE_SIZE, physical, VMM_PAGE_SIZE, VMM_PERM_RW));
  }
  g_ref_cpu = &CPU_BACKEND_A32;
  g_jit_cpu = &CPU_BACKEND_A32_JIT;
  jit_set_hot_threshold(1);
  if (getenv("JIT_DUMP")) jit_set_dump_directory(getenv("JIT_DUMP"));
  g_ref = g_ref_cpu->create(g_vmm, NULL);
  g_jit = g_jit_cpu->create(g_vmm, NULL);
  CHECK(g_ref && g_jit);
  g_ref_cpu->set_svc_handler(g_ref, on_svc);
  g_jit_cpu->set_svc_handler(g_jit, on_svc);
  g_ref_cpu->set_undefined_handler(g_ref, on_undefined);
  g_jit_cpu->set_undefined_handler(g_jit, on_undefined);
  for (uint32_t i = 0; i < iterations; i++) run_case(i);
  const Jit_Stats *stats = jit_stats();
  printf("[a32_jit_diff_test] %u streams: passed (%llu regions compiled, %llu compiled entries, %llu interpreted "
         "blocks)\n",
         iterations, (unsigned long long)stats->blocks_compiled, (unsigned long long)stats->block_entries,
         (unsigned long long)stats->interpreted_blocks);
  g_ref_cpu->destroy(g_ref);
  g_jit_cpu->destroy(g_jit);
  vmm_destroy(g_vmm);
  layout_destroy();
  return 0;
}
