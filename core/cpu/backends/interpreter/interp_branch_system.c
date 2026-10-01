/**
 * A64 branches, exception generation and system instructions
 * (DDI 0487 C4.1.87), EL0 view. ARMv8.0 only - the Switch's Cortex-A57 -
 * so pointer-authentication branches and other later extensions are
 * undefined here, exactly as on the console.
 */
#include "cpu/backends/interpreter/interp_internal.h"

#include <string.h>

#define DC_ZVA_BLOCK_BYTES 64u /* DCZID_EL0.BS = 4 (interpreter.c) */

/* SYS encodings EL0 may execute (op1=3, CRn=7). */
#define SYS_CRM_DC_ZVA 4u
#define SYS_CRM_IC_IVAU 5u
#define SYS_CRM_DC_CVAC 10u
#define SYS_CRM_DC_CVAU 11u
#define SYS_CRM_DC_CVAP 12u
#define SYS_CRM_DC_CIVAC 14u

static Interp_Status branch_to(Interp_State *s, uint64_t target) {
  s->regs.pc = target;
  return INTERP_CONTINUE;
}

static Interp_Status unconditional_immediate(Interp_State *s, uint32_t insn) {
  const uint64_t offset = (uint64_t)sign_extend((uint64_t)bits(insn, 25, 0) << 2, 28);
  if (bit(insn, 31)) s->regs.x[30] = s->regs.pc + 4u; /* BL */
  return branch_to(s, s->regs.pc + offset);
}

static Interp_Status compare_and_branch(Interp_State *s, uint32_t insn) {
  const bool sf = bit(insn, 31), nonzero = bit(insn, 24);
  const uint64_t value = xreg(s, bits(insn, 4, 0)) & width_mask(sf);
  if ((value != 0) == nonzero) {
    return branch_to(s, s->regs.pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21));
  }
  return advance(s);
}

static Interp_Status test_and_branch(Interp_State *s, uint32_t insn) {
  const uint32_t bit_position = (bit(insn, 31) << 5) | bits(insn, 23, 19);
  const bool want_set = bit(insn, 24);
  const bool is_set = (xreg(s, bits(insn, 4, 0)) >> bit_position) & 1u;
  if (is_set == want_set) {
    return branch_to(s, s->regs.pc + (uint64_t)sign_extend((uint64_t)bits(insn, 18, 5) << 2, 16));
  }
  return advance(s);
}

static Interp_Status conditional_branch(Interp_State *s, uint32_t insn) {
  if (bit(insn, 4)) return INTERP_UNDEFINED; /* BC.cond (FEAT_HBC) */
  if (interp_condition_holds(s, bits(insn, 3, 0))) {
    return branch_to(s, s->regs.pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21));
  }
  return advance(s);
}

static Interp_Status exception_generation(Interp_State *s, uint32_t insn) {
  const uint32_t opc = bits(insn, 23, 21), op2 = bits(insn, 4, 2), ll = bits(insn, 1, 0);
  if (op2 != 0) return INTERP_UNDEFINED;
  if (opc == 0 && ll == 1) { /* SVC: the immediate is the Horizon syscall number (§12) */
    s->svc_immediate = bits(insn, 20, 5);
    s->regs.pc += 4u;        /* preferred return address: the next instruction */
    return INTERP_SVC;
  }
  if (opc == 1 && ll == 0) return INTERP_BREAKPOINT; /* BRK */
  return INTERP_UNDEFINED; /* HVC/SMC/HLT/DCPS: not at EL0 */
}

static Interp_Status dc_zva(Interp_State *s, uint32_t rt) {
  static const uint8_t zeros[DC_ZVA_BLOCK_BYTES];
  const uint64_t address = xreg(s, rt) & ~(uint64_t)(DC_ZVA_BLOCK_BYTES - 1u);
  if (!interp_write(s, address, zeros, DC_ZVA_BLOCK_BYTES)) return INTERP_FAULT;
  return advance(s);
}

static Interp_Status system(Interp_State *s, uint32_t insn) {
  const bool read = bit(insn, 21);
  const uint32_t op0 = bits(insn, 20, 19), op1 = bits(insn, 18, 16);
  const uint32_t crn = bits(insn, 15, 12), crm = bits(insn, 11, 8), op2 = bits(insn, 7, 5);
  const uint32_t rt = bits(insn, 4, 0);

  if (op0 == 0) {
    if (read || op1 != 3) return INTERP_UNDEFINED;
    if (crn == 2 && rt == 31) return advance(s); /* HINT space: NOP, YIELD, WFE, SEV, ... */
    if (crn == 3 && rt == 31) {                   /* barriers */
      if (op2 == 2) s->exclusive_valid = false;   /* CLREX */
      else if (op2 < 4 || op2 == 7) return INTERP_UNDEFINED;
      return advance(s);                          /* DSB, DMB, ISB: single-threaded, no-ops */
    }
    return INTERP_UNDEFINED; /* MSR (immediate) to PSTATE fields: EL1+ on ARMv8.0 */
  }

  if (op0 == 1) { /* SYS / SYSL */
    if (read || op1 != 3 || crn != 7) return INTERP_UNDEFINED;
    switch (crm) {
    case SYS_CRM_DC_ZVA:
      return op2 == 1 ? dc_zva(s, rt) : INTERP_UNDEFINED;
    case SYS_CRM_IC_IVAU: case SYS_CRM_DC_CVAC: case SYS_CRM_DC_CVAU:
    case SYS_CRM_DC_CVAP: case SYS_CRM_DC_CIVAC:
      /* Cache maintenance: nothing to maintain - fetch reads vmm every time. */
      return op2 == 1 ? advance(s) : INTERP_UNDEFINED;
    default:
      return INTERP_UNDEFINED;
    }
  }

  /* MRS / MSR (register): bits 20:5 are the CPU_SYSREG_ENCODE layout. */
  const uint32_t reg = insn & (0xFFFFu << 5);
  if (read) {
    bool known = false;
    const uint64_t value = interp_read_sys_reg(s, reg, &known);
    if (!known) return INTERP_UNDEFINED;
    set_xreg(s, rt, value);
    return advance(s);
  }
  if (!interp_write_sys_reg(s, reg, xreg(s, rt))) return INTERP_UNDEFINED;
  return advance(s);
}

static Interp_Status branch_register(Interp_State *s, uint32_t insn) {
  const uint32_t opc = bits(insn, 24, 21), op2 = bits(insn, 20, 16);
  const uint32_t op3 = bits(insn, 15, 10), op4 = bits(insn, 4, 0);
  if (op2 != 0x1F || op3 != 0 || op4 != 0) return INTERP_UNDEFINED; /* incl. PAC forms */
  const uint64_t target = xreg(s, bits(insn, 9, 5));
  switch (opc) {
  case 0: return branch_to(s, target);  /* BR */
  case 1:                               /* BLR: read target before writing LR */
    s->regs.x[30] = s->regs.pc + 4u;
    return branch_to(s, target);
  case 2: return branch_to(s, target);  /* RET */
  default: return INTERP_UNDEFINED;     /* ERET, DRPS: not at EL0 */
  }
}

Interp_Status interp_branch_system(Interp_State *s, uint32_t insn) {
  if (bits(insn, 30, 26) == 0x05) return unconditional_immediate(s, insn);
  if (bits(insn, 30, 25) == 0x1A) return compare_and_branch(s, insn);
  if (bits(insn, 30, 25) == 0x1B) return test_and_branch(s, insn);
  if (bits(insn, 31, 24) == 0x54) return conditional_branch(s, insn);
  if (bits(insn, 31, 24) == 0xD4) return exception_generation(s, insn);
  if (bits(insn, 31, 22) == 0x354) return system(s, insn);
  if (bits(insn, 31, 25) == 0x6B) return branch_register(s, insn);
  return INTERP_UNDEFINED;
}
