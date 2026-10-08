/**
 * The AArch32 (A32) interpreter's state and shared helpers (a32.c, the
 * integer instruction set; a32_vfp.c, VFP and Advanced SIMD).
 *
 * The state embeds the A64 interpreter's (Interp_State), as a 32-bit thread
 * on the Switch is an AArch32 view of the same registers:
 *   - r0-r14 are regs.x[0..14] (zero-extended), r13 the stack pointer;
 *   - regs.pc is r15, the address of the instruction executing (reads of
 *     r15 as an operand see it plus 8);
 *   - regs.pstate holds CPSR.NZCV in the NZCV layout; Q and GE live here;
 *   - D0-D31 are the low and high halves of V0-V15 (D2n = V[n].lo),
 *     Q0-Q15 are V0-V15, S0-S31 the halves of D0-D15;
 *   - FPSCR is split as AArch64 splits it: control bits in fpcr, cumulative
 *     flags and QC in fpsr, and its NZCV (VCMP's result) here;
 *   - TPIDRURO (CP15 c13, c0, 3) is TPIDRRO_EL0, TPIDRURW TPIDR_EL0.
 * Memory goes through the vmm (interp_read/interp_write), the exclusive
 * monitor and cycle accounting are the A64 interpreter's.
 */
#ifndef SWITCH_CPU_BACKENDS_A32_A32_INTERNAL_H
#define SWITCH_CPU_BACKENDS_A32_A32_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/backends/interpreter/interp_internal.h"
#include "cpu/backends/interpreter/softfloat.h"

#define A32_PC 15u
#define A32_LR 14u
#define A32_SP 13u
#define A32_INSN_BYTES 4u
#define A32_PC_READ_OFFSET 8u /* an A32 instruction reads r15 as its address + 8 */

/* FPSCR = flags | control | cumulative, at AArch64's FPCR/FPSR positions. */
#define A32_FPSCR_NZCV_MASK 0xF0000000u
#define A32_FPSCR_CONTROL_MASK 0x07FF9F00u /* AHP DN FZ RMode Stride Len, trap enables */
#define A32_FPSCR_STATUS_MASK 0x0800009Fu  /* QC, IDC IXC UFC OFC DZC IOC */

typedef struct A32_State {
  Interp_State s;
  uint32_t q;          /* CPSR.Q */
  uint32_t ge;         /* CPSR.GE[3:0] */
  uint32_t fpscr_nzcv; /* FPSCR.NZCV (VCMP/VCMPE) */
  bool pc_written;     /* the instruction wrote r15: no fall-through advance */
} A32_State;

static inline uint32_t a32_reg(const A32_State *a, uint32_t n) {
  return n == A32_PC ? (uint32_t)a->s.regs.pc + A32_PC_READ_OFFSET : (uint32_t)a->s.regs.x[n];
}

/* Writes r0-r14, or branches (r15: an ARM-state target, word aligned). */
static inline void a32_set_reg(A32_State *a, uint32_t n, uint32_t value) {
  if (n == A32_PC) {
    a->s.regs.pc = value & ~3u;
    a->pc_written = true;
    return;
  }
  a->s.regs.x[n] = value;
}

static inline uint32_t a32_fpscr(const A32_State *a) {
  return a->fpscr_nzcv | (a->s.fpcr & A32_FPSCR_CONTROL_MASK) | (a->s.fpsr & A32_FPSCR_STATUS_MASK);
}

static inline void a32_set_fpscr(A32_State *a, uint32_t v) {
  a->fpscr_nzcv = v & A32_FPSCR_NZCV_MASK;
  a->s.fpcr = v & A32_FPSCR_CONTROL_MASK;
  a->s.fpsr = v & A32_FPSCR_STATUS_MASK;
}

/* The double-precision register Dn (0-31) and single Sn (0-31). */
static inline uint64_t a32_d(const A32_State *a, uint32_t n) {
  return (n & 1u) ? a->s.v[n >> 1].hi : a->s.v[n >> 1].lo;
}
static inline void a32_set_d(A32_State *a, uint32_t n, uint64_t v) {
  if (n & 1u) a->s.v[n >> 1].hi = v;
  else a->s.v[n >> 1].lo = v;
}
static inline uint32_t a32_s(const A32_State *a, uint32_t n) {
  return (uint32_t)(a32_d(a, n >> 1) >> ((n & 1u) * 32u));
}
static inline void a32_set_s(A32_State *a, uint32_t n, uint32_t v) {
  const uint64_t d = a32_d(a, n >> 1);
  const uint32_t shift = (n & 1u) * 32u;
  a32_set_d(a, n >> 1, (d & ~((uint64_t)0xFFFFFFFFu << shift)) | ((uint64_t)v << shift));
}

/* Memory (all-or-nothing, faults leave the state unchanged). */
static inline bool a32_read(A32_State *a, uint32_t address, void *out, uint32_t size) {
  return interp_read(&a->s, address, out, size);
}
static inline bool a32_write(A32_State *a, uint32_t address, const void *data, uint32_t size) {
  return interp_write(&a->s, address, data, size);
}

/* One instruction at regs.pc (a32.c): PC advanced, branched, or left on
 * the instruction for UNDEFINED/FAULT. */
Interp_Status a32_execute(A32_State *a, uint32_t insn);

/* VFP and Advanced SIMD (a32_vfp.c): coprocessor 10/11 instructions in the
 * conditional space, and the unconditional Advanced SIMD space. */
Interp_Status a32_vfp(A32_State *a, uint32_t insn);
Interp_Status a32_neon(A32_State *a, uint32_t insn);
/* The ARMv8 VFP additions in the unconditional space (VSEL, VMAXNM, VRINTx, VCVTx). */
Interp_Status a32_vfp_v8(A32_State *a, uint32_t insn);

#endif /* SWITCH_CPU_BACKENDS_A32_A32_INTERNAL_H */
