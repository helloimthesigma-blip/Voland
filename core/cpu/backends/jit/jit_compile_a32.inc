/**
 * The A32 front end (included by jit_compile.c, whose emitter it shares;
 * docs/JIT.md "AArch32"). An A32 thread's registers are the same register
 * file - r0-r14 in x0-x14, zero-extended; NZCV in pstate - so locals,
 * spills, exits, chaining, the softmmu walk and the lazy flags are the
 * A64 path's, at 32-bit width (flag_sf false): A32 ADDS/SUBS/CMP set NZCV
 * exactly as A64's W forms do. Logical ops with S keep V and take C from
 * the shifter (FLAGS_NZ).
 *
 * The interpreter (cpu/backends/a32) is the reference and the fallback:
 * whatever is not inlined here runs there, one instruction at a time
 * (jit_helper_interpret dispatches by Jit_State.isa). Instructions that
 * may write the PC and are not inlined end the block (END_HELPER).
 */

#include "cpu/backends/a32/a32_internal.h"

#define A32_COND_AL 0xEu
#define A32_COND_NV 0xFu
#define A32_REG_SP 13u
#define A32_REG_LR 14u
#define A32_REG_PC 15u
#define A32_PC_AHEAD 8u     /* an A32 instruction reads r15 as its address + 8 */
#define A32_WORD_ALIGN_MASK 3u

/* A register's value (i64, zero-extended 32 bits); r15 is this
 * instruction's address + 8. */
static void a32_get(Ctx *c, uint32_t r) {
  if (r == A32_REG_PC) {
    i64c(c, (c->pc + A32_PC_AHEAD) & MASK32);
    return;
  }
  get_x(c, r);
}

/* ---- the shifter ------------------------------------------------------ */

/* Shift(value, type, imm5) for the immediate-shift forms (imm5 0: LSR/ASR
 * by 32, ROR is RRX), 32-bit result on the stack. */
static void a32_shift_imm(Ctx *c, uint32_t rm, uint32_t type, uint32_t imm5) {
  a32_get(c, rm);
  switch (type) {
  case 0: /* LSL */
    if (imm5) {
      i64c(c, imm5);
      op(c, WASM_OP_I64_SHL);
      mask32(c);
    }
    break;
  case 1: /* LSR */
    i64c(c, imm5 ? imm5 : 32u);
    op(c, WASM_OP_I64_SHR_U);
    break;
  case 2: /* ASR */
    op(c, WASM_OP_I64_EXTEND32_S);
    i64c(c, imm5 ? imm5 : 31u); /* by 32: the sign everywhere, as by 31 */
    op(c, WASM_OP_I64_SHR_S);
    mask32(c);
    break;
  default:
    if (imm5) { /* ROR */
      op(c, WASM_OP_I32_WRAP_I64);
      i32c(c, imm5);
      op(c, WASM_OP_I32_ROTR);
      op(c, WASM_OP_I64_EXTEND_I32_U);
    } else { /* RRX: C into bit 31 */
      i64c(c, 1);
      op(c, WASM_OP_I64_SHR_U);
      load_flags(c);
      i32c(c, NZCV_SHIFT_C);
      op(c, WASM_OP_I32_SHR_U);
      i32c(c, 1);
      op(c, WASM_OP_I32_AND);
      op(c, WASM_OP_I64_EXTEND_I32_U);
      i64c(c, 31);
      op(c, WASM_OP_I64_SHL);
      op(c, WASM_OP_I64_OR);
    }
    break;
  }
}

/* The shifter's carry-out for an immediate shift, as an i32 0/1 (the
 * caller handles LSL #0, which keeps C). */
static void a32_shift_imm_carry(Ctx *c, uint32_t rm, uint32_t type, uint32_t imm5) {
  a32_get(c, rm);
  uint32_t bitpos;
  switch (type) {
  case 0: bitpos = 32u - imm5; break;
  case 1: case 2: bitpos = imm5 ? imm5 - 1u : 31u; break;
  default: bitpos = imm5 ? imm5 - 1u : 0u; break; /* ROR, RRX */
  }
  i64c(c, bitpos);
  op(c, WASM_OP_I64_SHR_U);
  op(c, WASM_OP_I32_WRAP_I64);
  i32c(c, 1);
  op(c, WASM_OP_I32_AND);
}

/* Shift by Rs's low byte (register-shifted forms), 32-bit result. */
static void a32_shift_reg(Ctx *c, uint32_t rm, uint32_t type, uint32_t rs) {
  a32_get(c, rs);
  i64c(c, 0xFFu);
  op(c, WASM_OP_I64_AND);
  lset(c, L_T3); /* the amount, 0-255 */
  switch (type) {
  case 0:   /* LSL: 0 from 32 on */
  case 1: { /* LSR */
    a32_get(c, rm);
    lget(c, L_T3);
    op(c, type == 0 ? WASM_OP_I64_SHL : WASM_OP_I64_SHR_U);
    mask32(c);
    i64c(c, 0);
    lget(c, L_T3);
    i64c(c, 32);
    op(c, WASM_OP_I64_LT_U);
    op(c, WASM_OP_SELECT);
    break;
  }
  case 2: /* ASR: as by 31 from 32 on */
    a32_get(c, rm);
    op(c, WASM_OP_I64_EXTEND32_S);
    lget(c, L_T3);
    i64c(c, 31);
    lget(c, L_T3);
    i64c(c, 31);
    op(c, WASM_OP_I64_LT_U);
    op(c, WASM_OP_SELECT);
    op(c, WASM_OP_I64_SHR_S);
    mask32(c);
    break;
  default: /* ROR by the low five bits */
    a32_get(c, rm);
    op(c, WASM_OP_I32_WRAP_I64);
    lget(c, L_T3);
    op(c, WASM_OP_I32_WRAP_I64);
    op(c, WASM_OP_I32_ROTR);
    op(c, WASM_OP_I64_EXTEND_I32_U);
    break;
  }
}

/* A loaded PC (in L_VAL): an ARM-state target leaves for it; a Thumb one
 * (bit 0) goes to the interpreter before anything is committed. */
static void a32_check_arm_target(Ctx *c) {
  lget(c, L_VAL);
  i64c(c, 1);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I32_WRAP_I64);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
}

/* Leaves for the ARM-state target in L_VAL - unless it is a return site
 * in this region (a BL the region followed into its callee): then the
 * region goes on there, as the A64 RET does. */
static bool a32_small_callee(const Ctx *c, uint64_t target);
static void a32_leave_for_val_predicted(Ctx *c, bool predicted, uint64_t prediction);
static void a32_leave_for_val(Ctx *c) { a32_leave_for_val_predicted(c, false, 0); }

/* ...and, with a prediction (a PLT stub's GOT entry as it is now), goes on
 * at the predicted target when the region holds it. */
static void a32_leave_for_val_predicted(Ctx *c, bool predicted, uint64_t prediction) {
  lget(c, L_VAL);
  i64c(c, ~(uint64_t)A32_WORD_ALIGN_MASK & MASK32);
  op(c, WASM_OP_I64_AND);
  lset(c, L_NPC);
  if (predicted && c->discover && a32_small_callee(c, prediction)) (void)add_block(c, prediction);
  for (uint32_t i = 0; i < c->block_count; i++) {
    if (!c->blocks[i].return_site && !(predicted && c->blocks[i].pc == prediction)) continue;
    lget(c, L_NPC);
    i64c(c, c->blocks[i].pc);
    op(c, WASM_OP_I64_EQ);
    open_if(c, WASM_BLOCK_VOID);
    branch_to(c, c->blocks[i].pc);
    end_(c);
  }
  lget(c, L_NPC);
  exit_to_stack(c);
}

/* A callee the region can follow a BL into: an ARM function that returns
 * (BX LR, POP {..., PC}, LDR PC, [SP], #4 or MOV PC, LR) within
 * JIT_SMALL_CALLEE_INSNS instructions without calling anything. */
#define A32_BX_LR 0xE12FFF1Eu
#define A32_MOV_PC_LR 0xE1A0F00Eu
#define A32_LDR_PC_POP 0xE49DF004u
#define A32_POP_PC_MASK 0xFFFF8000u
#define A32_POP_PC 0xE8BD8000u     /* LDMIA SP!, {..., PC} */
#define A32_BLX_REG_MASK 0x0FFFFFF0u
#define A32_BLX_REG 0x012FFF30u
/* A PLT stub - ADD ip, pc, #a; ADD ip, ip, #b; LDR pc, [ip, #c]! - ending
 * at `ldr_pc` (the LDR's address): the GOT slot it jumps through, and
 * what the slot holds now (the import it calls). */
#define A32_PLT_ADD_PC 0xE28FC000u  /* ADD ip, pc, #imm (mask 0xFFFFF000) */
#define A32_PLT_ADD_IP 0xE28CC000u  /* ADD ip, ip, #imm */
#define A32_PLT_LDR_PC 0xE5BCF000u  /* LDR pc, [ip, #imm]! */
#define A32_IMM12_MASK 0xFFFu
static uint32_t a32_modified_imm(uint32_t imm12) {
  const uint32_t value = imm12 & 0xFFu, rotate = 2u * ((imm12 >> 8) & 0xFu);
  return rotate ? (value >> rotate) | (value << (32u - rotate)) : value;
}
static bool a32_plt_target(const Ctx *c, uint64_t ldr_pc, uint64_t *target) {
  if (ldr_pc < 2u * INSN_BYTES || (ldr_pc & A32_WORD_ALIGN_MASK)) return false;
  const uint64_t first = ldr_pc - 2u * INSN_BYTES, page = first & ~(uint64_t)VMM_PAGE_OFFSET_MASK;
  if ((ldr_pc & ~(uint64_t)VMM_PAGE_OFFSET_MASK) != page) return false;
  const uint32_t *code = c->source->page_code(c->source->context, page);
  if (!code) return false;
  const uint32_t *w = code + (first - page) / INSN_BYTES;
  if ((w[0] & ~A32_IMM12_MASK) != A32_PLT_ADD_PC || (w[1] & ~A32_IMM12_MASK) != A32_PLT_ADD_IP ||
      (w[2] & ~A32_IMM12_MASK) != A32_PLT_LDR_PC)
    return false;
  const uint64_t slot = (first + A32_PC_AHEAD + a32_modified_imm(w[0] & A32_IMM12_MASK) +
                         a32_modified_imm(w[1] & A32_IMM12_MASK) + (w[2] & A32_IMM12_MASK)) & MASK32;
  uint64_t value;
  if (!c->source->peek64(c->source->context, slot, &value)) return false;
  *target = value & MASK32;
  return (*target & A32_WORD_ALIGN_MASK) == 0;
}

static bool a32_small_callee(const Ctx *c, uint64_t target) {
  const uint64_t page = target & ~(uint64_t)VMM_PAGE_OFFSET_MASK;
  const uint32_t *code = c->source->page_code(c->source->context, page);
  if (!code || (target & A32_WORD_ALIGN_MASK)) return false;
  uint64_t import;
  if (a32_plt_target(c, target + 2u * INSN_BYTES, &import)) return true; /* a PLT stub: followed to the import */
  const uint32_t first = (uint32_t)((target - page) / INSN_BYTES);
  for (uint32_t i = first; i < first + JIT_SMALL_CALLEE_INSNS && i < VMM_PAGE_SIZE / INSN_BYTES; i++) {
    const uint32_t insn = code[i];
    if (insn == A32_BX_LR || insn == A32_MOV_PC_LR || insn == A32_LDR_PC_POP || (insn & A32_POP_PC_MASK) == A32_POP_PC)
      return true;
    if (bits(insn, 27, 24) == 0xBu || (insn & A32_BLX_REG_MASK) == A32_BLX_REG) return false; /* BL, BLX */
  }
  return false;
}

/* BL: into the callee in the region when it can be there (back at the
 * return site through a32_leave_for_val), otherwise leave for it. */
static void a32_branch_link(Ctx *c, uint64_t target) {
  const uint64_t back = (c->pc + INSN_BYTES) & MASK32;
  i64c(c, back);
  set_x(c, A32_REG_LR);
  if (!c->link->span_calls || (c->discover && find_block(c, target) < 0 && !a32_small_callee(c, target))) {
    i64c(c, target);
    exit_to_stack(c);
    return;
  }
  if (c->discover && add_block(c, target) && add_block(c, back)) c->blocks[find_block(c, back)].return_site = true;
  branch_to(c, target);
}

/* ---- data processing ----------------------------------------------------- */

enum {
  A32_AND, A32_EOR, A32_SUB, A32_RSB, A32_ADD, A32_ADC, A32_SBC, A32_RSC,
  A32_TST, A32_TEQ, A32_CMP, A32_CMN, A32_ORR, A32_MOV, A32_BIC, A32_MVN,
};

static bool a32_is_logical(uint32_t opcode) {
  return opcode <= A32_EOR || opcode == A32_TST || opcode == A32_TEQ || opcode >= A32_ORR;
}

/* Sets FLAGS_NZ from the 32-bit result in L_T2; C becomes the i32 in
 * L_PASS when `carry_known` (else stays). V stays. */
static void a32_set_nz(Ctx *c, bool carry_known) {
  load_flags(c); /* L_NZCV live underneath (the value itself is not needed here) */
  op(c, WASM_OP_DROP);
  if (carry_known) {
    lget(c, L_NZCV);
    i32c(c, ~(uint32_t)CPU_PSTATE_C);
    op(c, WASM_OP_I32_AND);
    lget(c, L_PASS);
    i32c(c, NZCV_SHIFT_C);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
    store_flags(c);
  }
  lget(c, L_T2);
  lset(c, L_FR);
  flags_written(c);
  c->flag_kind = FLAGS_NZ;
  c->flag_sf = false;
}

/* AND ... MVN in all three operand forms. Rd = 15 is not inlined (an
 * interworking branch). */
static bool a32_data_processing(Ctx *c, uint32_t insn) {
  const uint32_t opcode = bits(insn, 24, 21), s = bit(insn, 20), rn = bits(insn, 19, 16), rd = bits(insn, 15, 12);
  const bool immediate = bit(insn, 25), test = opcode >= A32_TST && opcode <= A32_CMN;
  if (test && !s) return false; /* the miscellaneous space (decoded elsewhere) */
  if (rd == A32_REG_PC && !test && s) return false; /* SUBS pc, lr & co: exception returns */
  const bool logical = a32_is_logical(opcode);
  bool carry_known = false; /* the shifter produced a carry (logical S forms only) */
  /* Operand 2 -> L_T1 (and its carry -> L_PASS). */
  if (immediate) {
    const uint32_t rot = bits(insn, 11, 8) * 2u, imm8 = bits(insn, 7, 0);
    const uint32_t value = rot ? (imm8 >> rot) | (imm8 << (32u - rot)) : imm8;
    i64c(c, value);
    lset(c, L_T1);
    if (logical && s && rot) {
      i32c(c, value >> 31);
      lset(c, L_PASS);
      carry_known = true;
    }
  } else if (!bit(insn, 4)) {
    const uint32_t imm5 = bits(insn, 11, 7), type = bits(insn, 6, 5), rm = bits(insn, 3, 0);
    a32_shift_imm(c, rm, type, imm5);
    lset(c, L_T1);
    if (logical && s && !(type == 0 && imm5 == 0)) {
      a32_shift_imm_carry(c, rm, type, imm5);
      lset(c, L_PASS);
      carry_known = true;
    }
  } else {
    if (logical && s) return false; /* register-shifted carry: the interpreter */
    const uint32_t rs = bits(insn, 11, 8), type = bits(insn, 6, 5), rm = bits(insn, 3, 0);
    if (rn == A32_REG_PC || rm == A32_REG_PC || rs == A32_REG_PC || rd == A32_REG_PC) return false;
    a32_shift_reg(c, rm, type, rs);
    lset(c, L_T1);
  }
  const bool wants_n = opcode != A32_MOV && opcode != A32_MVN;
  if (wants_n) {
    a32_get(c, rn);
    lset(c, L_T0);
  }
  if (logical) {
    switch (opcode) {
    case A32_AND: case A32_TST: lget(c, L_T0); lget(c, L_T1); op(c, WASM_OP_I64_AND); break;
    case A32_EOR: case A32_TEQ: lget(c, L_T0); lget(c, L_T1); op(c, WASM_OP_I64_XOR); break;
    case A32_ORR: lget(c, L_T0); lget(c, L_T1); op(c, WASM_OP_I64_OR); break;
    case A32_MOV: lget(c, L_T1); break;
    case A32_BIC: lget(c, L_T0); lget(c, L_T1); i64c(c, MASK32); op(c, WASM_OP_I64_XOR); op(c, WASM_OP_I64_AND); break;
    default: lget(c, L_T1); i64c(c, MASK32); op(c, WASM_OP_I64_XOR); break; /* MVN */
    }
    lset(c, L_T2);
    if (s) a32_set_nz(c, carry_known);
  } else {
    /* AddWithCarry(T0, T1, carry) in emit_add_with_carry's terms. */
    Carry carry = CARRY_ZERO;
    switch (opcode) {
    case A32_SUB: case A32_CMP: case A32_SBC:
      lget(c, L_T1);
      i64c(c, MASK32);
      op(c, WASM_OP_I64_XOR);
      lset(c, L_T1);
      carry = opcode == A32_SBC ? CARRY_FLAG : CARRY_ONE;
      break;
    case A32_RSB: case A32_RSC: /* m + ~n */
      lget(c, L_T0);
      i64c(c, MASK32);
      op(c, WASM_OP_I64_XOR);
      lget(c, L_T1);
      lset(c, L_T0);
      lset(c, L_T1);
      carry = opcode == A32_RSC ? CARRY_FLAG : CARRY_ONE;
      break;
    case A32_ADC: carry = CARRY_FLAG; break;
    default: break; /* ADD, CMN */
    }
    emit_add_with_carry(c, false, carry, s ? FLAGS_SET_LAZY : FLAGS_UNCHANGED);
  }
  if (rd == A32_REG_PC && !test) { /* ALUWritePC: an interworking branch (jump tables, MOV pc, lr) */
    lget(c, L_T2);
    lset(c, L_VAL);
    a32_check_arm_target(c);
    a32_leave_for_val(c);
    return true;
  }
  if (!test) {
    lget(c, L_T2);
    set_x(c, rd);
  }
  return true;
}

/* MOVW / MOVT. */
static bool a32_move_wide(Ctx *c, uint32_t insn) {
  const uint32_t rd = bits(insn, 15, 12), imm16 = (bits(insn, 19, 16) << 12) | bits(insn, 11, 0);
  if (rd == A32_REG_PC) return false;
  if (!bit(insn, 22)) {
    i64c(c, imm16);
  } else {
    get_x(c, rd);
    i64c(c, 0xFFFFu);
    op(c, WASM_OP_I64_AND);
    i64c(c, (uint64_t)imm16 << 16);
    op(c, WASM_OP_I64_OR);
  }
  set_x(c, rd);
  return true;
}

/* MUL, MLA, MLS (32-bit results); UMULL, SMULL, UMLAL, SMLAL without S. */
static bool a32_multiply(Ctx *c, uint32_t insn) {
  const uint32_t op1 = bits(insn, 23, 21), s = bit(insn, 20);
  const uint32_t rd = bits(insn, 19, 16), ra = bits(insn, 15, 12), rm = bits(insn, 11, 8), rn = bits(insn, 3, 0);
  if (rd == A32_REG_PC || ra == A32_REG_PC || rm == A32_REG_PC || rn == A32_REG_PC) return false;
  if (bit(insn, 22) == 0 && bit(insn, 23) == 0) { /* MUL, MLA, (MLS has bit 22) */
    if (op1 > 1u) return false;
    get_x(c, rn);
    get_x(c, rm);
    op(c, WASM_OP_I64_MUL);
    if (op1 == 1u) {
      get_x(c, ra);
      op(c, WASM_OP_I64_ADD);
    }
    mask32(c);
    lset(c, L_T2);
    if (s) a32_set_nz(c, false);
    lget(c, L_T2);
    set_x(c, rd);
    return true;
  }
  if (op1 == 3u && !s) { /* MLS: Ra - Rn*Rm */
    get_x(c, ra);
    get_x(c, rn);
    get_x(c, rm);
    op(c, WASM_OP_I64_MUL);
    op(c, WASM_OP_I64_SUB);
    mask32(c);
    set_x(c, rd);
    return true;
  }
  if (!bit(insn, 23) || s) return false;
  /* Long: RdHi = 19:16, RdLo = 15:12; op1 = 1 U A (U: bit 22 signed). */
  const uint32_t hi = rd, lo = ra;
  if (hi == lo) return false;
  const bool is_signed = bit(insn, 22), accumulate = bit(insn, 21);
  get_x(c, rn);
  if (is_signed) op(c, WASM_OP_I64_EXTEND32_S);
  get_x(c, rm);
  if (is_signed) op(c, WASM_OP_I64_EXTEND32_S);
  op(c, WASM_OP_I64_MUL);
  if (accumulate) {
    get_x(c, hi);
    i64c(c, 32);
    op(c, WASM_OP_I64_SHL);
    get_x(c, lo);
    op(c, WASM_OP_I64_OR);
    op(c, WASM_OP_I64_ADD);
  }
  lset(c, L_T2);
  lget(c, L_T2);
  mask32(c);
  set_x(c, lo);
  lget(c, L_T2);
  i64c(c, 32);
  op(c, WASM_OP_I64_SHR_U);
  set_x(c, hi);
  return true;
}

/* ---- loads and stores ---------------------------------------------------- */

/* L_ADDR = the access address (32-bit); L_BASE = the written-back base
 * when `wback`. `offset_local` holds the unsigned offset. */
static void a32_address(Ctx *c, uint32_t rn, bool pre, bool up, uint32_t offset_local) {
  if (rn == A32_REG_PC) {
    i64c(c, ((c->pc + A32_PC_AHEAD) & MASK32) & ~(uint64_t)A32_WORD_ALIGN_MASK);
  } else {
    get_x(c, rn);
  }
  lget(c, offset_local);
  op(c, up ? WASM_OP_I64_ADD : WASM_OP_I64_SUB);
  mask32(c);
  lset(c, L_BASE);
  if (pre) {
    lget(c, L_BASE);
  } else if (rn == A32_REG_PC) {
    i64c(c, ((c->pc + A32_PC_AHEAD) & MASK32) & ~(uint64_t)A32_WORD_ALIGN_MASK);
  } else {
    get_x(c, rn);
  }
  lset(c, L_ADDR);
}

/* One transfer at L_ADDR of `size` bytes, rt (r15 loads: a branch). */
static void a32_transfer(Ctx *c, bool load, Access access, uint32_t size, uint32_t rt, uint32_t rn, bool wback) {
  if (!load) {
    emit_store(c, size, rt, false, 0);
  } else {
    emit_load_address(c, size);
    emit_host_load(c, access, size, 0, L_VAL);
    if (rt == A32_REG_PC) a32_check_arm_target(c);
  }
  if (wback) {
    lget(c, L_BASE);
    set_x(c, rn);
  }
  if (load) {
    if (rt == A32_REG_PC) {
      uint64_t import = 0;
      const bool predicted = c->link->span_calls && a32_plt_target(c, c->pc, &import);
      a32_leave_for_val_predicted(c, predicted, import);
      return;
    }
    lget(c, L_VAL);
    set_x(c, rt);
  }
}

/* LDR/STR/LDRB/STRB, immediate and register offsets (not LDRT/STRT). */
static Outcome a32_load_store_word(Ctx *c, uint32_t insn) {
  const bool reg = bit(insn, 25), pre = bit(insn, 24), up = bit(insn, 23), byte = bit(insn, 22), w = bit(insn, 21);
  const bool load = bit(insn, 20);
  const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12);
  if (!pre && w) return OUTCOME_END_HELPER; /* LDRT/STRT */
  const bool wback = !pre || w;
  if (wback && (rn == A32_REG_PC || rn == rt)) return OUTCOME_END_HELPER;
  if (rt == A32_REG_PC && (!load || byte)) return OUTCOME_END_HELPER;
  if (reg) {
    if (bit(insn, 4)) return OUTCOME_HELPER; /* media space */
    const uint32_t rm = bits(insn, 3, 0);
    if (rm == A32_REG_PC) return OUTCOME_END_HELPER;
    a32_shift_imm(c, rm, bits(insn, 6, 5), bits(insn, 11, 7));
  } else {
    i64c(c, bits(insn, 11, 0));
  }
  lset(c, L_T1);
  a32_address(c, rn, pre, up, L_T1);
  a32_transfer(c, load, ACCESS_LOAD_ZERO, byte ? 1u : 4u, rt, rn, wback);
  return rt == A32_REG_PC ? OUTCOME_END : OUTCOME_NEXT;
}

/* LDRH/STRH/LDRSB/LDRSH/LDRD/STRD. */
static bool a32_load_store_extra(Ctx *c, uint32_t insn) {
  const bool pre = bit(insn, 24), up = bit(insn, 23), imm = bit(insn, 22), w = bit(insn, 21), load = bit(insn, 20);
  const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12), op2 = bits(insn, 6, 5);
  if (!pre && w) return false; /* the unprivileged forms */
  const bool wback = !pre || w;
  const bool dual = !load && op2 >= 2u; /* LDRD (op2 2) / STRD (op2 3) */
  if (rt == A32_REG_PC || (dual && ((rt & 1u) || rt == A32_REG_LR))) return false;
  if (wback && (rn == A32_REG_PC || rn == rt || (dual && rn == rt + 1u))) return false;
  if (imm) {
    i64c(c, (bits(insn, 11, 8) << 4) | bits(insn, 3, 0));
  } else {
    const uint32_t rm = bits(insn, 3, 0);
    if (rm == A32_REG_PC || (dual && op2 == 2u && (rm == rt || rm == rt + 1u))) return false;
    get_x(c, rm);
  }
  lset(c, L_T1);
  a32_address(c, rn, pre, up, L_T1);
  if (dual) {
    if (op2 == 2u) { /* LDRD: both words, then the registers */
      emit_load_address(c, 8u);
      emit_host_load(c, ACCESS_LOAD_ZERO, 4u, 0, L_VAL);
      emit_host_load(c, ACCESS_LOAD_ZERO, 4u, 4u, L_VAL2);
      if (wback) {
        lget(c, L_BASE);
        set_x(c, rn);
      }
      lget(c, L_VAL);
      set_x(c, rt);
      lget(c, L_VAL2);
      set_x(c, rt + 1u);
    } else {
      emit_store(c, 4u, rt, true, rt + 1u);
      if (wback) {
        lget(c, L_BASE);
        set_x(c, rn);
      }
    }
    return true;
  }
  uint32_t size = 2u;
  Access access = ACCESS_LOAD_ZERO;
  if (op2 == 2u) { size = 1u; access = ACCESS_LOAD_SIGN32; } /* LDRSB */
  if (op2 == 3u) access = ACCESS_LOAD_SIGN32;                  /* LDRSH */
  if (!load && op2 != 1u) return false;
  a32_transfer(c, load, access, size, rt, rn, wback);
  return true;
}

#define A32_MAX_LIST 16u

/* LDM/STM (all four modes, writeback; not the user-bank forms). POP with
 * PC leaves for the loaded address. */
static Outcome a32_block_transfer(Ctx *c, uint32_t insn) {
  const bool pre = bit(insn, 24), up = bit(insn, 23), w = bit(insn, 21), load = bit(insn, 20);
  const uint32_t rn = bits(insn, 19, 16), list = bits(insn, 15, 0);
  if (bit(insn, 22) || rn == A32_REG_PC || list == 0) return OUTCOME_END_HELPER;
  const uint32_t count = (uint32_t)__builtin_popcount(list);
  const bool has_pc = list & (1u << A32_REG_PC);
  if (w && (list & (1u << rn))) return OUTCOME_END_HELPER; /* base in the list with writeback */
  if (!load && has_pc) return OUTCOME_HELPER;             /* STM of PC (+8): the interpreter */
  const uint32_t bytes = 4u * count;
  /* The lowest address. */
  get_x(c, rn);
  if (up) {
    if (pre) {
      i64c(c, 4);
      op(c, WASM_OP_I64_ADD);
    }
  } else {
    i64c(c, pre ? bytes : bytes - 4u);
    op(c, WASM_OP_I64_SUB);
  }
  mask32(c);
  lset(c, L_ADDR);
  /* One walk for the whole run; anything it refuses (crossing a page,
   * a fault) is the interpreter's, which transfers all or nothing. */
  open_block(c); /* $go */
  const uint32_t go = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, bytes, load ? VMM_PERM_R : VMM_PERM_W, c->depth);
  br(c, go);
  end_(c);
  leave_via_interpreter(c);
  end_(c); /* $go: L_HOST */
  if (load && has_pc) {
    lget(c, L_HOST);
    mem(c, WASM_OP_I64_LOAD32_U, ALIGN_1, bytes - 4u);
    lset(c, L_VAL);
    a32_check_arm_target(c);
  }
  uint32_t at = 0;
  for (uint32_t r = 0; r < A32_REG_PC; r++) {
    if (!(list & (1u << r))) continue;
    if (load) {
      lget(c, L_HOST);
      mem(c, WASM_OP_I64_LOAD32_U, ALIGN_1, at);
      set_x(c, r);
    } else {
      lget(c, L_HOST);
      get_x(c, r);
      mem(c, WASM_OP_I64_STORE32, ALIGN_1, at);
    }
    at += 4u;
  }
  if (w) {
    get_x(c, rn);
    i64c(c, bytes);
    op(c, up ? WASM_OP_I64_ADD : WASM_OP_I64_SUB);
    mask32(c);
    set_x(c, rn);
  }
  if (has_pc) {
    a32_leave_for_val(c);
    return OUTCOME_END;
  }
  return OUTCOME_NEXT;
}

/* ---- branches ------------------------------------------------------------- */

static uint64_t a32_branch_target(Ctx *c, uint32_t insn) {
  return (c->pc + A32_PC_AHEAD + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 0) << 2, 26u)) & MASK32;
}

/* BX / BLX (register): an ARM-state target leaves for it, a Thumb one is
 * the interpreter's. A predicted target (a virtual call through a vtable
 * as the region's entry state finds it) the region may hold: BLX then
 * follows a small callee and comes back at the return site. */
static void a32_branch_register(Ctx *c, uint32_t rm, bool link) {
  const bool predicted = c->link->span_calls && rm < A32_REG_PC && (c->predicted_mask & (1u << rm)) &&
                         !(c->predicted_value[rm] & A32_WORD_ALIGN_MASK);
  const uint64_t prediction = predicted ? c->predicted_value[rm] : 0;
  a32_get(c, rm);
  lset(c, L_VAL);
  a32_check_arm_target(c);
  if (link) {
    const uint64_t back = (c->pc + INSN_BYTES) & MASK32;
    i64c(c, back);
    set_x(c, A32_REG_LR);
    if (predicted && c->discover && a32_small_callee(c, prediction) && add_block(c, prediction) && add_block(c, back))
      c->blocks[find_block(c, back)].return_site = true;
  }
  a32_leave_for_val_predicted(c, predicted, prediction);
}

/* Predictions from the entry state: the registers as they are now, then
 * what immediate-offset LDRs and MOVs make of them in the entry block. */
static void a32_seed_predictions(Ctx *c) {
  if (!c->source->entry_reg) return;
  for (uint32_t r = 0; r < A32_REG_PC; r++) {
    uint64_t value;
    if (!c->source->entry_reg(c->source->context, r, &value)) continue;
    c->predicted_mask |= 1u << r;
    c->predicted_value[r] = value & MASK32;
  }
}

#define A32_LDR_IMM_MASK 0x0E500000u /* LDR (immediate, word): 010P U0W1 */
#define A32_LDR_IMM 0x04100000u
#define A32_MOV_REG_MASK 0x0FEF0FF0u /* MOV{S} Rd, Rm (no shift) */
#define A32_MOV_REG 0x01A00000u
/* The prediction an instruction makes (before it is compiled - its base
 * may be written back): *rd gets *value. */
static bool a32_predict(const Ctx *c, uint32_t insn, uint32_t *rd, uint64_t *value) {
  if (!c->link->span_calls || bits(insn, 31, 28) != A32_COND_AL) return false;
  if ((insn & A32_LDR_IMM_MASK) == A32_LDR_IMM) {
    const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12);
    if (rt >= A32_REG_PC || rn >= A32_REG_PC || !(c->predicted_mask & (1u << rn))) return false;
    const uint32_t imm = bits(insn, 11, 0);
    uint64_t address = c->predicted_value[rn];
    if (bit(insn, 24)) address = (bit(insn, 23) ? address + imm : address - imm) & MASK32;
    uint64_t pair;
    if ((address & A32_WORD_ALIGN_MASK) || !c->source->peek64(c->source->context, address & ~(uint64_t)7u, &pair))
      return false;
    *rd = rt;
    *value = (pair >> (8u * (address & 4u))) & MASK32;
    return true;
  }
  if ((insn & A32_MOV_REG_MASK) == A32_MOV_REG) {
    const uint32_t rm = bits(insn, 3, 0), d = bits(insn, 15, 12);
    if (rm >= A32_REG_PC || d >= A32_REG_PC || !(c->predicted_mask & (1u << rm))) return false;
    *rd = d;
    *value = c->predicted_value[rm];
    return true;
  }
  return false;
}

/* ---- media: extends, bit fields, reversal ------------------------------------ */

/* SXTB/SXTH/UXTB/UXTH and their accumulating forms (Rn != 15). */
static bool a32_extend(Ctx *c, uint32_t insn) {
  const uint32_t kind = bits(insn, 22, 20), rn = bits(insn, 19, 16), rd = bits(insn, 15, 12), rm = bits(insn, 3, 0);
  const uint32_t rotate = bits(insn, 11, 10) * 8u;
  if (kind != 2u && kind != 3u && kind != 6u && kind != 7u) return false; /* the 16-bit-pair forms */
  if (rd == A32_REG_PC || rm == A32_REG_PC || bits(insn, 9, 8)) return false;
  get_x(c, rm);
  op(c, WASM_OP_I32_WRAP_I64);
  if (rotate) {
    i32c(c, rotate);
    op(c, WASM_OP_I32_ROTR);
  }
  op(c, WASM_OP_I64_EXTEND_I32_U);
  switch (kind) {
  case 2: op(c, WASM_OP_I64_EXTEND8_S); break;   /* SXTB */
  case 3: op(c, WASM_OP_I64_EXTEND16_S); break;  /* SXTH */
  case 6: i64c(c, 0xFFu); op(c, WASM_OP_I64_AND); break;    /* UXTB */
  default: i64c(c, 0xFFFFu); op(c, WASM_OP_I64_AND); break; /* UXTH */
  }
  if (rn != A32_REG_PC) {
    get_x(c, rn);
    op(c, WASM_OP_I64_ADD);
  }
  mask32(c);
  set_x(c, rd);
  return true;
}

/* BFI/BFC, UBFX/SBFX. */
static bool a32_bitfield(Ctx *c, uint32_t insn) {
  const uint32_t rd = bits(insn, 15, 12), lsb = bits(insn, 11, 7), rn = bits(insn, 3, 0), hi = bits(insn, 20, 16);
  if (rd == A32_REG_PC) return false;
  if (bits(insn, 22, 21) == 2u) { /* BFI / BFC: msb = hi */
    if (hi < lsb) return false;
    const uint64_t field = ((((uint64_t)1 << (hi - lsb + 1u)) - 1u) << lsb);
    get_x(c, rd);
    i64c(c, ~field & MASK32);
    op(c, WASM_OP_I64_AND);
    if (rn != A32_REG_PC) {
      get_x(c, rn);
      i64c(c, lsb);
      op(c, WASM_OP_I64_SHL);
      i64c(c, field);
      op(c, WASM_OP_I64_AND);
      op(c, WASM_OP_I64_OR);
    }
    set_x(c, rd);
    return true;
  }
  if (rn == A32_REG_PC || lsb + hi > 31u) return false; /* hi = width - 1 */
  const uint32_t width = hi + 1u;
  get_x(c, rn);
  if (bit(insn, 22)) { /* UBFX */
    i64c(c, lsb);
    op(c, WASM_OP_I64_SHR_U);
    i64c(c, ((uint64_t)1 << width) - 1u);
    op(c, WASM_OP_I64_AND);
  } else { /* SBFX: to the top, then arithmetic back down */
    i64c(c, 64u - lsb - width);
    op(c, WASM_OP_I64_SHL);
    i64c(c, 64u - width);
    op(c, WASM_OP_I64_SHR_S);
    mask32(c);
  }
  set_x(c, rd);
  return true;
}

/* REV, REV16, CLZ. */
static bool a32_reverse(Ctx *c, uint32_t insn, bool clz) {
  const uint32_t rd = bits(insn, 15, 12), rm = bits(insn, 3, 0);
  if (rd == A32_REG_PC || rm == A32_REG_PC) return false;
  get_x(c, rm);
  op(c, WASM_OP_I32_WRAP_I64);
  if (clz) {
    op(c, WASM_OP_I32_CLZ);
  } else {
    lset(c, L_PASS);
    const bool half = bit(insn, 7); /* REV16 */
    /* bytes b3 b2 b1 b0 -> REV: b0 b1 b2 b3; REV16: b2 b3 b0 b1 */
    lget(c, L_PASS);
    i32c(c, 8);
    op(c, WASM_OP_I32_SHL);
    i32c(c, half ? 0xFF00FF00u : 0x00FF0000u);
    op(c, WASM_OP_I32_AND);
    lget(c, L_PASS);
    i32c(c, 8);
    op(c, WASM_OP_I32_SHR_U);
    i32c(c, half ? 0x00FF00FFu : 0x0000FF00u);
    op(c, WASM_OP_I32_AND);
    op(c, WASM_OP_I32_OR);
    if (!half) {
      lget(c, L_PASS);
      i32c(c, 24);
      op(c, WASM_OP_I32_SHL);
      op(c, WASM_OP_I32_OR);
      lget(c, L_PASS);
      i32c(c, 24);
      op(c, WASM_OP_I32_SHR_U);
      op(c, WASM_OP_I32_OR);
    }
  }
  op(c, WASM_OP_I64_EXTEND_I32_U);
  set_x(c, rd);
  return true;
}

static Outcome a32_media(Ctx *c, uint32_t insn) {
  const uint32_t op1 = bits(insn, 24, 20), op2 = bits(insn, 7, 5);
  if ((op1 & 0x18u) == 0x08u && op2 == 3u) return a32_extend(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  if (op1 == 0x0Bu && (op2 == 1u || op2 == 5u) && bits(insn, 19, 16) == 0xFu && bits(insn, 11, 8) == 0xFu)
    return a32_reverse(c, insn, false) ? OUTCOME_NEXT : OUTCOME_HELPER;
  if (((op1 & 0x1Eu) == 0x1Cu && (op2 & 3u) == 0u) || ((op1 & 0x1Au) == 0x1Au && (op2 & 3u) == 2u))
    return a32_bitfield(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  return OUTCOME_HELPER;
}

/* ---- VFP loads, stores and moves, inline --------------------------------------- */

/* Where the A32 view of the vector registers lives in the state: Dd is
 * half d&1 of V[d>>1], Ss half s&1 of D(s>>1). */
static uint64_t a32_off_d(uint32_t d) { return OFF_V(d >> 1) + (uint64_t)(d & 1u) * V_HIGH_HALF; }
static uint64_t a32_off_s(uint32_t s) { return a32_off_d(s >> 1) + (uint64_t)(s & 1u) * 4u; }
#define A32_OFF_FPSCR_NZCV ((uint64_t)offsetof(A32_State, fpscr_nzcv))

/* VLDR / VSTR (single and double), any base but the PC-relative store. */
static bool a32_vfp_load_store(Ctx *c, uint32_t insn) {
  const bool pre = bit(insn, 24), up = bit(insn, 23), w = bit(insn, 21), load = bit(insn, 20), dbl = bit(insn, 8);
  if (!pre || w) return false; /* VLDM / VSTM, VPUSH / VPOP: the interpreter */
  const uint32_t rn = bits(insn, 19, 16), imm = bits(insn, 7, 0) * 4u, size = dbl ? 8u : 4u;
  const uint64_t off = dbl ? a32_off_d((bit(insn, 22) << 4) | bits(insn, 15, 12))
                           : a32_off_s((bits(insn, 15, 12) << 1) | bit(insn, 22));
  if (rn == A32_REG_PC) {
    const uint64_t base = ((c->pc + A32_PC_AHEAD) & MASK32) & ~(uint64_t)A32_WORD_ALIGN_MASK;
    i64c(c, (up ? base + imm : base - imm) & MASK32);
  } else {
    get_x(c, rn);
    i64c(c, imm);
    op(c, up ? WASM_OP_I64_ADD : WASM_OP_I64_SUB);
    mask32(c);
  }
  lset(c, L_ADDR);
  if (load) {
    emit_load_address(c, size);
    lget(c, L_STATE);
    lget(c, L_HOST);
    mem(c, dbl ? WASM_OP_I64_LOAD : WASM_OP_I32_LOAD, ALIGN_1, 0);
    mem(c, dbl ? WASM_OP_I64_STORE : WASM_OP_I32_STORE, ALIGN_1, off);
    return true;
  }
  open_block(c); /* $done */
  const uint32_t done = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, size, VMM_PERM_W, c->depth);
  lget(c, L_HOST);
  lget(c, L_STATE);
  mem(c, dbl ? WASM_OP_I64_LOAD : WASM_OP_I64_LOAD32_U, ALIGN_1, off);
  mem(c, dbl ? WASM_OP_I64_STORE : WASM_OP_I64_STORE32, ALIGN_1, 0);
  br(c, done);
  end_(c);
  lget(c, L_STATE);
  lget(c, L_ADDR);
  i32c(c, size);
  lget(c, L_STATE);
  mem(c, dbl ? WASM_OP_I64_LOAD : WASM_OP_I64_LOAD32_U, ALIGN_1, off);
  i64c(c, 0);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_STORE);
  op(c, WASM_OP_I32_EQZ);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
  end_(c); /* $done */
  return true;
}

/* A run of `count` registers of `size` bytes at L_ADDR (one walk; a
 * crossing or a fault is the interpreter's, which transfers all or
 * nothing): the state offsets come from `offset_of(first + i)`. */
static void a32_register_run(Ctx *c, bool load, uint32_t size, uint32_t count, uint32_t first,
                             uint64_t (*offset_of)(uint32_t)) {
  open_block(c); /* $go */
  const uint32_t go = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, size * count, load ? VMM_PERM_R : VMM_PERM_W, c->depth);
  br(c, go);
  end_(c);
  leave_via_interpreter(c);
  end_(c); /* $go: L_HOST */
  const uint8_t load_op = size == 8u ? WASM_OP_I64_LOAD : WASM_OP_I32_LOAD;
  const uint8_t store_op = size == 8u ? WASM_OP_I64_STORE : WASM_OP_I32_STORE;
  for (uint32_t i = 0; i < count; i++) {
    if (load) {
      lget(c, L_STATE);
      lget(c, L_HOST);
      mem(c, load_op, ALIGN_1, size * i);
      mem(c, store_op, ALIGN_4, offset_of(first + i));
    } else {
      lget(c, L_HOST);
      lget(c, L_STATE);
      mem(c, load_op, ALIGN_4, offset_of(first + i));
      mem(c, store_op, ALIGN_1, size * i);
    }
  }
}

/* VLD1/VST1 (multiple structures, one to four D registers): memory and
 * register bytes in the same order whatever the element size; the
 * alignment hint is not checked (as the interpreter). Rm: 15 no
 * writeback, 13 the size, else Rm. */
static bool a32_neon_load_store_lane(Ctx *c, uint32_t insn);
static bool a32_neon_load_store(Ctx *c, uint32_t insn) {
  if (bit(insn, 23)) return a32_neon_load_store_lane(c, insn);
  uint32_t regs;
  switch (bits(insn, 11, 8)) {
  case 7: regs = 1; break;
  case 0xA: regs = 2; break;
  case 6: regs = 3; break;
  case 2: regs = 4; break;
  default: return false; /* VLD2-4 */
  }
  const uint32_t rn = bits(insn, 19, 16), rm = bits(insn, 3, 0), d0 = (bit(insn, 22) << 4) | bits(insn, 15, 12);
  if (rn == A32_REG_PC || d0 + regs > 32u) return false;
  get_x(c, rn);
  lset(c, L_ADDR);
  a32_register_run(c, bit(insn, 21), 8u, regs, d0, a32_off_d);
  if (rm != A32_REG_PC) {
    lget(c, L_ADDR);
    if (rm == A32_REG_SP) i64c(c, 8u * regs);
    else get_x(c, rm);
    op(c, WASM_OP_I64_ADD);
    mask32(c);
    set_x(c, rn);
  }
  return true;
}

/* VLDM/VSTM: IA (writeback optional) and DB with writeback - VPUSH and
 * VPOP. FLDMX/FSTMX (an odd word count of doubles) moves imm8/2
 * registers and writes back imm8 words. */
static bool a32_vfp_load_store_multiple(Ctx *c, uint32_t insn) {
  const bool pre = bit(insn, 24), up = bit(insn, 23), w = bit(insn, 21), dbl = bit(insn, 8);
  const uint32_t rn = bits(insn, 19, 16), words = bits(insn, 7, 0);
  if (pre == up || (pre && !w) || rn == A32_REG_PC) return false;
  const uint32_t regs = dbl ? words / 2u : words;
  const uint32_t first = dbl ? (bit(insn, 22) << 4) | bits(insn, 15, 12) : (bits(insn, 15, 12) << 1) | bit(insn, 22);
  if (!regs || first + regs > 32u) return false;
  get_x(c, rn);
  if (!up) {
    i64c(c, 4u * words);
    op(c, WASM_OP_I64_SUB);
    mask32(c);
  }
  lset(c, L_ADDR);
  a32_register_run(c, bit(insn, 20), dbl ? 8u : 4u, regs, first, dbl ? a32_off_d : a32_off_s);
  if (w) {
    get_x(c, rn);
    i64c(c, 4u * words);
    op(c, up ? WASM_OP_I64_ADD : WASM_OP_I64_SUB);
    mask32(c);
    set_x(c, rn);
  }
  return true;
}

/* VMOV between a core register and a single, the two-register forms, and
 * VMRS APSR_nzcv, FPSCR. */
static bool a32_vmov_scalar(Ctx *c, uint32_t insn);
static bool a32_vdup_core(Ctx *c, uint32_t insn);
static bool a32_vfp_transfer(Ctx *c, uint32_t insn) {
  if (bits(insn, 27, 24) == 0xEu && bits(insn, 11, 8) == 0xBu && bits(insn, 3, 0) == 0u) { /* cp11: lanes */
    if (bit(insn, 23) && !bit(insn, 20)) return !bit(insn, 6) && a32_vdup_core(c, insn);
    return a32_vmov_scalar(c, insn);
  }
  if ((insn & 0x0FE00F7Fu) == 0x0E000A10u) { /* VMOV Sn <-> Rt */
    const uint32_t rt = bits(insn, 15, 12), sn = (bits(insn, 19, 16) << 1) | bit(insn, 7);
    if (rt == A32_REG_PC) return false;
    if (bit(insn, 20)) {
      lget(c, L_STATE);
      mem(c, WASM_OP_I64_LOAD32_U, ALIGN_4, a32_off_s(sn));
      set_x(c, rt);
    } else {
      lget(c, L_STATE);
      get_x(c, rt);
      mem(c, WASM_OP_I64_STORE32, ALIGN_4, a32_off_s(sn));
    }
    return true;
  }
  if ((insn & 0x0FE00ED0u) == 0x0C400A10u) { /* VMOV Dm <-> Rt, Rt2 (bit 8) / two singles */
    const uint32_t rt = bits(insn, 15, 12), rt2 = bits(insn, 19, 16);
    if (rt == A32_REG_PC || rt2 == A32_REG_PC || (bit(insn, 20) && rt == rt2)) return false;
    uint64_t lo, hi;
    if (bit(insn, 8)) {
      const uint32_t dm = (bit(insn, 5) << 4) | bits(insn, 3, 0);
      lo = a32_off_d(dm);
      hi = lo + 4u;
    } else {
      const uint32_t sm = (bits(insn, 3, 0) << 1) | bit(insn, 5);
      if (sm == 31u) return false;
      lo = a32_off_s(sm);
      hi = a32_off_s(sm + 1u);
    }
    if (bit(insn, 20)) {
      lget(c, L_STATE);
      mem(c, WASM_OP_I64_LOAD32_U, ALIGN_4, lo);
      set_x(c, rt);
      lget(c, L_STATE);
      mem(c, WASM_OP_I64_LOAD32_U, ALIGN_4, hi);
      set_x(c, rt2);
    } else {
      lget(c, L_STATE);
      get_x(c, rt);
      mem(c, WASM_OP_I64_STORE32, ALIGN_4, lo);
      lget(c, L_STATE);
      get_x(c, rt2);
      mem(c, WASM_OP_I64_STORE32, ALIGN_4, hi);
    }
    return true;
  }
  if (insn << 4 == 0xEF1FA100u) { /* VMRS APSR_nzcv, FPSCR (cond in 31:28) */
    lget(c, L_STATE);
    mem(c, WASM_OP_I32_LOAD, ALIGN_4, A32_OFF_FPSCR_NZCV);
    store_flags(c);
    return true;
  }
  return false;
}

/* ---- VFP arithmetic, native with the A64 fast paths' exactness guards ------ */

static void a32_fp_load(Ctx *c, uint32_t r, bool dbl) {
  lget(c, L_STATE);
  mem(c, dbl ? WASM_OP_F64_LOAD : WASM_OP_F32_LOAD, ALIGN_4, dbl ? a32_off_d(r) : a32_off_s(r));
}
static void a32_fp_store(Ctx *c, uint32_t r, bool dbl, uint32_t local) {
  lget(c, L_STATE);
  lget(c, local);
  mem(c, dbl ? WASM_OP_F64_STORE : WASM_OP_F32_STORE, ALIGN_4, dbl ? a32_off_d(r) : a32_off_s(r));
}

/* Register numbers: single Vx:X, double X:Vx. */
static uint32_t a32_vreg(uint32_t insn, bool dbl, unsigned hi, unsigned lo, unsigned x) {
  return dbl ? (bit(insn, x) << 4) | bits(insn, hi, lo) : (bits(insn, hi, lo) << 1) | bit(insn, x);
}

/* The exact (slow) arm: the interpreter, the instruction as AL (inside a
 * compiled condition the flags in the state may be stale). */
static void a32_fp_else_exact(Ctx *c) {
  const uint32_t original = c->insn;
  if (bits(original, 31, 28) != A32_COND_NV) c->insn = (c->insn & 0x0FFFFFFFu) | (A32_COND_AL << 28);
  Sync none = {0, false};
  fp_else_exact(c, &none);
  c->insn = original;
}

/* VFPExpandImm: the VMOV (immediate) constant's bits. */
static uint64_t a32_vfp_expand_imm(uint32_t imm8, bool dbl) {
  const uint64_t a = imm8 >> 7, b = (imm8 >> 6) & 1u, cdefgh = imm8 & 0x3Fu;
  if (dbl) return (a << 63) | ((b ^ 1u) << 62) | ((b ? 0xFFull : 0u) << 54) | (cdefgh << 48);
  return (a << 31) | ((b ^ 1u) << 30) | ((b ? 0x1Full : 0u) << 25) | (cdefgh << 19);
}

/* VCMP/VCMPE (register or #0.0): FPSCR.NZCV from an ordered compare;
 * NaNs (the only operands that raise IOC) take the exact arm. */
static void a32_vfp_compare(Ctx *c, bool dbl, uint32_t d, uint32_t m, bool with_zero) {
  const uint32_t a = dbl ? L_FD : L_FS, b = dbl ? L_FD2 : L_FS2;
  const uint8_t eq = dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ, lt = dbl ? WASM_OP_F64_LT : WASM_OP_F32_LT;
  a32_fp_load(c, d, dbl);
  lset(c, a);
  if (with_zero) {
    if (dbl) f64c(c, 0);
    else f32c(c, 0);
  } else {
    a32_fp_load(c, m, dbl);
  }
  lset(c, b);
  lget(c, a);
  lget(c, a);
  op(c, eq);
  lget(c, b);
  lget(c, b);
  op(c, eq);
  op(c, WASM_OP_I32_AND);
  emit_fpcr_default(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  lget(c, L_STATE);
  i32c(c, CPU_PSTATE_N);                /* d < m: 1000 */
  i32c(c, CPU_PSTATE_Z | CPU_PSTATE_C); /* equal: 0110 */
  i32c(c, CPU_PSTATE_C);                /* d > m: 0010 */
  lget(c, a);
  lget(c, b);
  op(c, eq);
  op(c, WASM_OP_SELECT);
  lget(c, a);
  lget(c, b);
  op(c, lt);
  op(c, WASM_OP_SELECT);
  mem(c, WASM_OP_I32_STORE, ALIGN_4, A32_OFF_FPSCR_NZCV);
  a32_fp_else_exact(c);
}

/* VCVT between F32 and F64 (op 0111): widening is exact (NaNs aside);
 * narrowing rounds once and must stay normal (or be an exact zero). */
static void a32_vfp_convert_precision(Ctx *c, bool from_dbl, uint32_t d, uint32_t m) {
  a32_fp_load(c, m, from_dbl);
  if (!from_dbl) {
    ltee(c, L_FS2);
    op(c, WASM_OP_F64_PROMOTE_F32);
    lset(c, L_FD);
    lget(c, L_FS2);
    lget(c, L_FS2);
    op(c, WASM_OP_F32_EQ); /* not NaN */
    emit_fpcr_default(c);
    op(c, WASM_OP_I32_AND);
    fp_fast_arm(c);
    a32_fp_store(c, d, true, L_FD);
    a32_fp_else_exact(c);
    return;
  }
  ltee(c, L_FD2);
  op(c, WASM_OP_F32_DEMOTE_F64);
  ltee(c, L_FS);
  op(c, WASM_OP_F64_PROMOTE_F32);
  lget(c, L_FD2);
  op(c, WASM_OP_F64_EQ);
  set_exactness(c);
  emit_result_normal(c, L_FS, false);
  emit_is_zero(c, L_FD2, true);
  op(c, WASM_OP_I32_OR);
  emit_fp_env_ok_exact(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_fp_store(c, d, false, L_FS);
  fp_note_inexact(c);
  a32_fp_else_exact(c);
}

/* VCVT from a 32-bit integer in Sm to F32/F64: a double holds every one
 * exactly; a single when its significant bits fit 24. */
static void a32_vfp_int_to_fp(Ctx *c, bool dbl, bool is_signed, uint32_t d, uint32_t m) {
  const uint32_t r = fp_result_local(dbl);
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, a32_off_s(m));
  ltee(c, L_PASS);
  if (dbl) op(c, is_signed ? WASM_OP_F64_CONVERT_I32_S : WASM_OP_F64_CONVERT_I32_U);
  else op(c, is_signed ? WASM_OP_F32_CONVERT_I32_S : WASM_OP_F32_CONVERT_I32_U);
  lset(c, r);
  if (dbl) {
    emit_fpcr_default(c);
  } else {
    lget(c, L_PASS);
    op(c, is_signed ? WASM_OP_I64_EXTEND_I32_S : WASM_OP_I64_EXTEND_I32_U);
    lset(c, L_T0);
    if (is_signed) { /* the magnitude */
      i64c(c, 0);
      lget(c, L_T0);
      op(c, WASM_OP_I64_SUB);
      lget(c, L_T0);
      lget(c, L_T0);
      i64c(c, 0);
      op(c, WASM_OP_I64_LT_S);
      op(c, WASM_OP_SELECT);
      lset(c, L_T0);
    }
    lget(c, L_T0);
    op(c, WASM_OP_I64_CLZ);
    lget(c, L_T0);
    op(c, WASM_OP_I64_CTZ);
    op(c, WASM_OP_I64_ADD);
    i64c(c, 64u - (F32_FRACTION_BITS + 1u));
    op(c, WASM_OP_I64_GE_S);
    set_exactness(c);
    emit_fp_env_ok_exact(c);
  }
  fp_fast_arm(c);
  a32_fp_store(c, d, dbl, r);
  if (!dbl) fp_note_inexact(c);
  a32_fp_else_exact(c);
}

typedef enum A32_Rounding { A32_ROUND_ZERO, A32_ROUND_NEAREST, A32_ROUND_PLUS, A32_ROUND_MINUS, A32_ROUND_AWAY } A32_Rounding;

/* The value in `a`, rounded to an integral value (the same format) on
 * the stack. Ties away: the truncation, one further out when the dropped
 * fraction is at least a half (exact in either format). */
static void a32_round_integral(Ctx *c, uint32_t a, bool dbl, A32_Rounding mode) {
  static const uint8_t ops32[4] = {WASM_OP_F32_TRUNC, WASM_OP_F32_NEAREST, WASM_OP_F32_CEIL, WASM_OP_F32_FLOOR};
  static const uint8_t ops64[4] = {WASM_OP_F64_TRUNC, WASM_OP_F64_NEAREST, WASM_OP_F64_CEIL, WASM_OP_F64_FLOOR};
  lget(c, a);
  if (mode != A32_ROUND_AWAY) {
    op(c, dbl ? ops64[mode] : ops32[mode]);
    return;
  }
  const uint32_t t = dbl ? L_FD3 : L_FS3;
  op(c, dbl ? WASM_OP_F64_TRUNC : WASM_OP_F32_TRUNC);
  ltee(c, t);
  if (dbl) f64c(c, 0x3FF0000000000000ull);
  else f32c(c, 0x3F800000u);
  lget(c, a);
  op(c, dbl ? WASM_OP_F64_COPYSIGN : WASM_OP_F32_COPYSIGN);
  op(c, dbl ? WASM_OP_F64_ADD : WASM_OP_F32_ADD);
  lget(c, t);
  lget(c, a);
  lget(c, t);
  op(c, dbl ? WASM_OP_F64_SUB : WASM_OP_F32_SUB);
  op(c, dbl ? WASM_OP_F64_ABS : WASM_OP_F32_ABS);
  if (dbl) f64c(c, 0x3FE0000000000000ull);
  else f32c(c, 0x3F000000u);
  op(c, dbl ? WASM_OP_F64_GE : WASM_OP_F32_GE);
  op(c, WASM_OP_SELECT);
}

/* VCVT/VCVTA/VCVTN/VCVTP/VCVTM from F32/F64 in Vm to a 32-bit integer in
 * Sd. Out of range and NaN take the exact arm (IOC, saturation). */
static void a32_vfp_fp_to_int(Ctx *c, bool dbl, bool is_signed, A32_Rounding mode, uint32_t d, uint32_t m) {
  const uint32_t a = dbl ? L_FD2 : L_FS2, r = fp_result_local(dbl);
  a32_fp_load(c, m, dbl);
  lset(c, a);
  a32_round_integral(c, a, dbl, mode);
  ltee(c, r);
  lget(c, a);
  op(c, dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ);
  set_exactness(c);
  emit_truncation_in_range(c, r, dbl, false, !is_signed);
  emit_fp_env_ok_exact(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  fp_note_inexact(c);
  lget(c, L_STATE);
  lget(c, r);
  if (dbl) op(c, is_signed ? WASM_OP_I32_TRUNC_F64_S : WASM_OP_I32_TRUNC_F64_U);
  else op(c, is_signed ? WASM_OP_I32_TRUNC_F32_S : WASM_OP_I32_TRUNC_F32_U);
  mem(c, WASM_OP_I32_STORE, ALIGN_4, a32_off_s(d));
  a32_fp_else_exact(c);
}

/* VMLA, VMLS, VNMLA, VNMLS: d = (+/-)d + (+/-)(n*m), two roundings. Native
 * when IXC is already set (no exactness to track), the product normal
 * (or an exact zero) and the sum finite (a tiny sum is exact). */
static void a32_vfp_multiply_accumulate(Ctx *c, bool dbl, bool negate_d, bool negate_product, uint32_t d,
                                        uint32_t n, uint32_t m) {
  const uint32_t p = fp_result_local(dbl), a = dbl ? L_FD2 : L_FS2, b = dbl ? L_FD3 : L_FS3;
  a32_fp_load(c, n, dbl);
  ltee(c, a);
  a32_fp_load(c, m, dbl);
  ltee(c, b);
  op(c, dbl ? WASM_OP_F64_MUL : WASM_OP_F32_MUL);
  lset(c, p);
  emit_result_normal(c, p, dbl);
  emit_is_zero(c, p, dbl);
  emit_is_zero(c, a, dbl);
  emit_is_zero(c, b, dbl);
  op(c, WASM_OP_I32_OR);
  op(c, WASM_OP_I32_AND);
  op(c, WASM_OP_I32_OR);
  a32_fp_load(c, d, dbl); /* the sum, into the product's operand local */
  if (negate_d) op(c, dbl ? WASM_OP_F64_NEG : WASM_OP_F32_NEG);
  lget(c, p);
  if (negate_product) op(c, dbl ? WASM_OP_F64_NEG : WASM_OP_F32_NEG);
  op(c, dbl ? WASM_OP_F64_ADD : WASM_OP_F32_ADD);
  lset(c, a);
  emit_result_finite(c, a, dbl);
  op(c, WASM_OP_I32_AND);
  emit_fp_env_ok(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_fp_store(c, d, dbl, a);
  a32_fp_else_exact(c);
}

/* VFMA, VFMS, VFNMA, VFNMS (F32): emit_fp_fused_single's f64 sum, with
 * A32 registers; o1 negates the addend, o0 != o1 the product. */
static void a32_vfp_fused_single(Ctx *c, bool o1, bool o0, uint32_t d, uint32_t n, uint32_t m) {
  a32_fp_load(c, d, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  if (o1) op(c, WASM_OP_F64_NEG);
  a32_fp_load(c, n, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  if (o0 != o1) op(c, WASM_OP_F64_NEG);
  a32_fp_load(c, m, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  op(c, WASM_OP_F64_MUL);
  op(c, WASM_OP_F64_ADD);
  ltee(c, L_FD);
  op(c, WASM_OP_F32_DEMOTE_F64);
  lset(c, L_FS);
  emit_result_finite(c, L_FS, false);
  emit_tiny_single_in_double(c, L_FD);
  op(c, WASM_OP_I32_EQZ);
  op(c, WASM_OP_I32_AND);
  lget(c, L_FD); /* not on a midpoint */
  op(c, WASM_OP_I64_REINTERPRET_F64);
  i64c(c, (1ull << F32_EXTRA_BITS) - 1u);
  op(c, WASM_OP_I64_AND);
  i64c(c, F32_MIDPOINT);
  op(c, WASM_OP_I64_NE);
  op(c, WASM_OP_I32_AND);
  emit_fp_env_ok(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_fp_store(c, d, false, L_FS);
  a32_fp_else_exact(c);
}

/* The ARMv8 VFP forms in the unconditional space: VSEL, VMAXNM/VMINNM,
 * VRINTA/N/P/M, VCVTA/N/P/M. */
static bool a32_jit_vfp_v8(Ctx *c, uint32_t insn) {
  if (bits(insn, 27, 24) != 0xEu || bits(insn, 11, 9) != 5u || bit(insn, 4)) return false;
  const bool dbl = bit(insn, 8);
  const uint32_t d = a32_vreg(insn, dbl, 15, 12, 22), n = a32_vreg(insn, dbl, 19, 16, 7), m = a32_vreg(insn, dbl, 3, 0, 5);
  if (!bit(insn, 23)) { /* VSEL: cc 00 EQ, 01 VS, 10 GE, 11 GT */
    if (bit(insn, 6)) return false;
    static const uint32_t conds[4] = {0x0u, 0x6u, 0xAu, 0xCu};
    lget(c, L_STATE);
    lget(c, L_STATE);
    mem(c, dbl ? WASM_OP_I64_LOAD : WASM_OP_I32_LOAD, ALIGN_4, dbl ? a32_off_d(n) : a32_off_s(n));
    lget(c, L_STATE);
    mem(c, dbl ? WASM_OP_I64_LOAD : WASM_OP_I32_LOAD, ALIGN_4, dbl ? a32_off_d(m) : a32_off_s(m));
    emit_condition(c, conds[bits(insn, 21, 20)]);
    op(c, WASM_OP_SELECT);
    mem(c, dbl ? WASM_OP_I64_STORE : WASM_OP_I32_STORE, ALIGN_4, dbl ? a32_off_d(d) : a32_off_s(d));
    return true;
  }
  if (bits(insn, 21, 20) == 0u) { /* VMAXNM / VMINNM without NaNs: one operand, unrounded, no flags */
    const uint32_t a = dbl ? L_FD2 : L_FS2, b = dbl ? L_FD3 : L_FS3, r = fp_result_local(dbl);
    const bool min = bit(insn, 6);
    a32_fp_load(c, n, dbl);
    ltee(c, a);
    a32_fp_load(c, m, dbl);
    ltee(c, b);
    if (dbl) op(c, min ? WASM_OP_F64_MIN : WASM_OP_F64_MAX);
    else op(c, min ? WASM_OP_F32_MIN : WASM_OP_F32_MAX);
    lset(c, r);
    lget(c, a);
    lget(c, a);
    op(c, dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ);
    lget(c, b);
    lget(c, b);
    op(c, dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ);
    op(c, WASM_OP_I32_AND);
    emit_fpcr_default(c);
    op(c, WASM_OP_I32_AND);
    fp_fast_arm(c);
    a32_fp_store(c, d, dbl, r);
    a32_fp_else_exact(c);
    return true;
  }
  if (bits(insn, 21, 20) != 3u || !bit(insn, 6) || bit(insn, 19) != 1u) return false;
  static const A32_Rounding modes[4] = {A32_ROUND_AWAY, A32_ROUND_NEAREST, A32_ROUND_PLUS, A32_ROUND_MINUS};
  const A32_Rounding mode = modes[bits(insn, 17, 16)];
  if (bit(insn, 18)) { /* VCVTA/N/P/M: Sd from Vm */
    a32_vfp_fp_to_int(c, dbl, bit(insn, 7), mode, a32_vreg(insn, false, 15, 12, 22), m);
    return true;
  }
  if (bit(insn, 7)) return false;
  /* VRINTA/N/P/M: integral, no Inexact; NaNs take the exact arm */
  const uint32_t a = dbl ? L_FD2 : L_FS2, r = fp_result_local(dbl);
  a32_fp_load(c, m, dbl);
  lset(c, a);
  a32_round_integral(c, a, dbl, mode);
  lset(c, r);
  lget(c, a);
  lget(c, a);
  op(c, dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ);
  emit_fpcr_default(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_fp_store(c, d, dbl, r);
  a32_fp_else_exact(c);
  return true;
}

/* The VFP data-processing forms beyond a32_vfp_arith's: VMOV (immediate),
 * the multiply-accumulates, VCMP/VCMPE, VCVT. */
static bool a32_vfp_more(Ctx *c, uint32_t insn) {
  const bool dbl = bit(insn, 8), op6 = bit(insn, 6), op7 = bit(insn, 7);
  const uint32_t opc1 = bits(insn, 23, 20) & 0xBu, d = a32_vreg(insn, dbl, 15, 12, 22), m = a32_vreg(insn, dbl, 3, 0, 5);
  const uint32_t n = a32_vreg(insn, dbl, 19, 16, 7);
  if (opc1 == 0x0u || opc1 == 0x1u) { /* VMLA / VMLS, VNMLS / VNMLA */
    a32_vfp_multiply_accumulate(c, dbl, opc1 == 0x1u, op6, d, n, m);
    return true;
  }
  if (opc1 == 0xAu || opc1 == 0x9u) { /* VFMA / VFMS, VFNMS / VFNMA */
    if (dbl) return false;            /* needs a true fused multiply-add */
    const bool o1 = opc1 == 0x9u;
    a32_vfp_fused_single(c, o1, op6 != o1, d, n, m);
    return true;
  }
  if (opc1 != 0xBu) return false;
  if (!op6) { /* VMOV (immediate) */
    if (op7 || bit(insn, 5)) return false;
    const uint64_t value = a32_vfp_expand_imm((bits(insn, 19, 16) << 4) | bits(insn, 3, 0), dbl);
    lget(c, L_STATE);
    if (dbl) {
      i64c(c, value);
      mem(c, WASM_OP_I64_STORE, ALIGN_4, a32_off_d(d));
    } else {
      i32c(c, (uint32_t)value);
      mem(c, WASM_OP_I32_STORE, ALIGN_4, a32_off_s(d));
    }
    return true;
  }
  const uint32_t opc2 = bits(insn, 19, 16);
  const uint32_t sd = a32_vreg(insn, false, 15, 12, 22), sm = a32_vreg(insn, false, 3, 0, 5);
  switch (opc2) {
  case 0x4: case 0x5: /* VCMP / VCMPE (E: op7 only changes which NaNs raise IOC) */
    if (opc2 == 0x5u && bits(insn, 5, 0)) return false;
    a32_vfp_compare(c, dbl, d, m, opc2 == 0x5u);
    return true;
  case 0x7: /* VCVT F64.F32 / F32.F64 */
    if (!op7) return false;
    a32_vfp_convert_precision(c, dbl, dbl ? sd : a32_vreg(insn, true, 15, 12, 22), dbl ? m : sm);
    return true;
  case 0x8: /* VCVT F32/F64 from S32 (op7) / U32 in Sm */
    a32_vfp_int_to_fp(c, dbl, op7, d, sm);
    return true;
  case 0xC: case 0xD: /* VCVT U32 / S32 (opc2 bit 0) from F32/F64, op7: towards zero */
    a32_vfp_fp_to_int(c, dbl, opc2 == 0xDu, op7 ? A32_ROUND_ZERO : A32_ROUND_NEAREST, sd, m);
    return true;
  default: return false;
  }
}

/* VMUL, VNMUL, VADD, VSUB, VDIV, VSQRT (F32/F64): the native result when
 * the FPSCR is the default and the result is exact or IXC is already
 * set (FPSCR's flags are FPSR's), else the interpreter. VMOV (register),
 * VABS, VNEG are bit operations. */
static bool a32_vfp_arith(Ctx *c, uint32_t insn) {
  if (bit(insn, 4) || bits(insn, 11, 9) != 5u) return false;
  const bool dbl = bit(insn, 8), op6 = bit(insn, 6);
  const uint32_t opc1 = bits(insn, 23, 20) & 0xBu, d = a32_vreg(insn, dbl, 15, 12, 22), m = a32_vreg(insn, dbl, 3, 0, 5);
  const uint32_t n = a32_vreg(insn, dbl, 19, 16, 7);
  uint32_t opcode;
  bool two = true;
  if (opc1 == 0x2u) opcode = op6 ? 8u : 0u;      /* VNMUL / VMUL */
  else if (opc1 == 0x3u) opcode = op6 ? 3u : 2u; /* VSUB / VADD */
  else if (opc1 == 0x8u && !op6) opcode = 1u;    /* VDIV */
  else if (opc1 == 0xBu && op6) {               /* the "other" group, opc2 in 19:16 */
    const uint32_t opc2 = bits(insn, 19, 16), op7 = bit(insn, 7);
    if (opc2 == 0u || (opc2 == 1u && !op7)) { /* VMOV / VABS (0), VNEG (1, op7 0) */
      if (dbl) {
        lget(c, L_STATE);
        lget(c, L_STATE);
        mem(c, WASM_OP_I64_LOAD, ALIGN_4, a32_off_d(m));
        if (opc2 == 0u && op7) { i64c(c, 0x7FFFFFFFFFFFFFFFull); op(c, WASM_OP_I64_AND); }
        if (opc2 == 1u) { i64c(c, 0x8000000000000000ull); op(c, WASM_OP_I64_XOR); }
        mem(c, WASM_OP_I64_STORE, ALIGN_4, a32_off_d(d));
      } else {
        lget(c, L_STATE);
        lget(c, L_STATE);
        mem(c, WASM_OP_I32_LOAD, ALIGN_4, a32_off_s(m));
        if (opc2 == 0u && op7) { i32c(c, 0x7FFFFFFFu); op(c, WASM_OP_I32_AND); }
        if (opc2 == 1u) { i32c(c, 0x80000000u); op(c, WASM_OP_I32_XOR); }
        mem(c, WASM_OP_I32_STORE, ALIGN_4, a32_off_s(d));
      }
      return true;
    }
    if (opc2 != 1u || !op7) return a32_vfp_more(c, insn); /* VSQRT here */
    two = false;
    opcode = 0u;
  } else {
    return a32_vfp_more(c, insn);
  }
  static const uint8_t ops64[4] = {WASM_OP_F64_MUL, WASM_OP_F64_DIV, WASM_OP_F64_ADD, WASM_OP_F64_SUB};
  static const uint8_t ops32[4] = {WASM_OP_F32_MUL, WASM_OP_F32_DIV, WASM_OP_F32_ADD, WASM_OP_F32_SUB};
  const uint32_t r = fp_result_local(dbl), a = dbl ? L_FD2 : L_FS2, b = dbl ? L_FD3 : L_FS3;
  a32_fp_load(c, two ? n : m, dbl);
  ltee(c, a);
  if (two) {
    a32_fp_load(c, m, dbl);
    ltee(c, b);
    const uint32_t k = opcode == 8u ? 0u : opcode;
    op(c, dbl ? ops64[k] : ops32[k]);
    if (opcode == 8u) op(c, dbl ? WASM_OP_F64_NEG : WASM_OP_F32_NEG);
  } else {
    op(c, dbl ? WASM_OP_F64_SQRT : WASM_OP_F32_SQRT);
  }
  lset(c, r);
  if (two && (opcode == 2u || opcode == 3u)) {
    emit_result_finite(c, r, dbl);
  } else {
    emit_result_normal(c, r, dbl);
    emit_is_zero(c, r, dbl);
    if (!two) {
    } else if (opcode == 1u) {
      emit_is_zero(c, a, dbl);
      op(c, WASM_OP_I32_AND);
    } else {
      emit_is_zero(c, a, dbl);
      emit_is_zero(c, b, dbl);
      op(c, WASM_OP_I32_OR);
      op(c, WASM_OP_I32_AND);
    }
    op(c, WASM_OP_I32_OR);
  }
  emit_arith_exact(c, dbl, opcode, two, a, b, r);
  emit_fp_env_ok_exact(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_fp_store(c, d, dbl, r);
  fp_note_inexact(c);
  a32_fp_else_exact(c);
  return true;
}

/* ---- Advanced SIMD data processing, inline ---------------------------------- */

/* Qn is V[n]; a D register is one half (a32_off_d). A 64-bit operand
 * is loaded into both halves (emit_all_lanes ignores the upper one) and
 * stored from the lower. */
static void a32_load_vec(Ctx *c, uint32_t r, bool q) {
  lget(c, L_STATE);
  if (q) {
    simd(c, WASM_SIMD_V128_LOAD);
    wasm_uleb(c->b, ALIGN_8);
    wasm_uleb(c->b, OFF_V(r >> 1));
    return;
  }
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, a32_off_d(r));
  simd(c, WASM_SIMD_I64X2_SPLAT);
}
static void a32_store_vec(Ctx *c, uint32_t r, uint32_t local, bool q) {
  lget(c, L_STATE);
  lget(c, local);
  if (q) {
    simd(c, WASM_SIMD_V128_STORE);
    wasm_uleb(c->b, ALIGN_8);
    wasm_uleb(c->b, OFF_V(r >> 1));
    return;
  }
  simd(c, WASM_SIMD_I64X2_EXTRACT_LANE);
  wasm_u8(c->b, 0);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, a32_off_d(r));
}

/* Lane mask: a single that flush-to-zero leaves alone (normal or zero). */
static void a32_lane_normal_or_zero(Ctx *c, uint32_t local) {
  emit_lane_normal(c, local, false);
  emit_lane_zero(c, local, false);
  simd(c, WASM_SIMD_V128_OR);
}

typedef enum Neon_Fp_Op { NEON_FP_ADD, NEON_FP_SUB, NEON_FP_MUL, NEON_FP_MLA, NEON_FP_MLS } Neon_Fp_Op;

/* F32 lanes of L_VA op L_VB (VMLA/VMLS: Vd +/- the product, two
 * roundings) under the standard FPSCR value Advanced SIMD always uses -
 * flush to zero, default NaN, round to nearest: native when IXC is
 * already set, every input normal or zero and every result normal or an
 * exact zero (nothing to flush, no flag but IXC). Else the interpreter. */
static void a32_neon_fp(Ctx *c, Neon_Fp_Op nop, bool q, uint32_t d) {
  a32_lane_normal_or_zero(c, L_VA);
  a32_lane_normal_or_zero(c, L_VB);
  simd(c, WASM_SIMD_V128_AND);
  lget(c, L_VA);
  lget(c, L_VB);
  if (nop == NEON_FP_ADD || nop == NEON_FP_SUB) {
    simd(c, nop == NEON_FP_ADD ? WASM_SIMD_F32X4_ADD : WASM_SIMD_F32X4_SUB);
    lset(c, L_VR);
    a32_lane_normal_or_zero(c, L_VR);
    simd(c, WASM_SIMD_V128_AND);
  } else {
    simd(c, WASM_SIMD_F32X4_MUL);
    lset(c, L_VT);
    emit_lane_normal(c, L_VT, false); /* normal, or zero from a zero operand */
    emit_lane_zero(c, L_VT, false);
    emit_lane_zero(c, L_VA, false);
    emit_lane_zero(c, L_VB, false);
    simd(c, WASM_SIMD_V128_OR);
    simd(c, WASM_SIMD_V128_AND);
    simd(c, WASM_SIMD_V128_OR);
    simd(c, WASM_SIMD_V128_AND);
    if (nop == NEON_FP_MUL) {
      lget(c, L_VT);
      lset(c, L_VR);
    } else {
      a32_load_vec(c, d, q);
      lset(c, L_VD);
      a32_lane_normal_or_zero(c, L_VD);
      simd(c, WASM_SIMD_V128_AND);
      lget(c, L_VD);
      lget(c, L_VT);
      simd(c, nop == NEON_FP_MLA ? WASM_SIMD_F32X4_ADD : WASM_SIMD_F32X4_SUB);
      lset(c, L_VR);
      a32_lane_normal_or_zero(c, L_VR);
      simd(c, WASM_SIMD_V128_AND);
    }
  }
  emit_all_lanes(c, q);
  emit_fp_env_ok(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  a32_store_vec(c, d, L_VR, q);
  a32_fp_else_exact(c);
}

/* An F32 operation with no rounding (compare, max, min) of L_VA and L_VB:
 * native when every input lane is normal or zero - no NaN to raise IOC
 * or propagate, no denormal for the standard FPSCR to flush. Else the
 * interpreter. */
static void a32_neon_fp_exact(Ctx *c, uint32_t wasm_op, bool q, uint32_t d) {
  a32_lane_normal_or_zero(c, L_VA);
  a32_lane_normal_or_zero(c, L_VB);
  simd(c, WASM_SIMD_V128_AND);
  emit_all_lanes(c, q);
  fp_fast_arm(c);
  lget(c, L_VA);
  lget(c, L_VB);
  simd(c, wasm_op);
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  a32_fp_else_exact(c);
}

/* VPADD.F32 Dd, Dn, Dm: [n0 + n1, m0 + m1], as VADD of [n0, m0] and [n1, m1]. */
static bool a32_neon_vpadd_f32(Ctx *c, uint32_t n, uint32_t m, uint32_t d) {
  static const uint8_t evens[SIMD_LANE_BYTES] = {0, 1, 2, 3, 16, 17, 18, 19, 0, 1, 2, 3, 16, 17, 18, 19};
  static const uint8_t odds[SIMD_LANE_BYTES] = {4, 5, 6, 7, 20, 21, 22, 23, 4, 5, 6, 7, 20, 21, 22, 23};
  a32_load_vec(c, n, false);
  lset(c, L_VT);
  a32_load_vec(c, m, false);
  lset(c, L_VR);
  lget(c, L_VT);
  lget(c, L_VR);
  v128_shuffle(c, evens);
  lset(c, L_VA);
  lget(c, L_VT);
  lget(c, L_VR);
  v128_shuffle(c, odds);
  lset(c, L_VB);
  a32_neon_fp(c, NEON_FP_ADD, false, d);
  return true;
}

/* Register numbers: D:Vd, N:Vn, M:Vm. */
static uint32_t a32_neon_d(uint32_t insn) { return (bit(insn, 22) << 4) | bits(insn, 15, 12); }
static uint32_t a32_neon_n(uint32_t insn) { return (bit(insn, 7) << 4) | bits(insn, 19, 16); }
static uint32_t a32_neon_m(uint32_t insn) { return (bit(insn, 5) << 4) | bits(insn, 3, 0); }

/* Three registers of the same length: bitwise ops, integer VADD/VSUB/
 * VMUL, F32 VADD/VSUB/VMUL/VMLA/VMLS. */
static bool a32_neon_three_same(Ctx *c, uint32_t insn) {
  const uint32_t opc = bits(insn, 11, 8), b = bit(insn, 4), u = bit(insn, 24), size = bits(insn, 21, 20);
  const bool q = bit(insn, 6);
  const uint32_t d = a32_neon_d(insn), n = a32_neon_n(insn), m = a32_neon_m(insn);
  if (q && ((d | n | m) & 1u)) return false;
  if (opc == 0xDu) {
    if (bit(insn, 20)) return false; /* half precision */
    Neon_Fp_Op nop;
    if (!b && !u) nop = size & 2u ? NEON_FP_SUB : NEON_FP_ADD;
    else if (b && !u) nop = size & 2u ? NEON_FP_MLS : NEON_FP_MLA;
    else if (b && u && !(size & 2u)) nop = NEON_FP_MUL;
    else if (!b && u && !(size & 2u)) return !q && a32_neon_vpadd_f32(c, n, m, d);
    else return false; /* VABD */
    a32_load_vec(c, n, q);
    lset(c, L_VA);
    a32_load_vec(c, m, q);
    lset(c, L_VB);
    a32_neon_fp(c, nop, q, d);
    return true;
  }
  if (opc == 0xEu && !b && !bit(insn, 20)) { /* VCEQ / VCGE / VCGT (F32) */
    if (!u && (size & 2u)) return false;
    a32_load_vec(c, n, q);
    lset(c, L_VA);
    a32_load_vec(c, m, q);
    lset(c, L_VB);
    a32_neon_fp_exact(c, !u ? WASM_SIMD_F32X4_EQ : (size & 2u) ? WASM_SIMD_F32X4_GT : WASM_SIMD_F32X4_GE, q, d);
    return true;
  }
  if (opc == 0xFu && !b && !u && !bit(insn, 20)) { /* VMAX / VMIN (F32) */
    a32_load_vec(c, n, q);
    lset(c, L_VA);
    a32_load_vec(c, m, q);
    lset(c, L_VB);
    a32_neon_fp_exact(c, (size & 2u) ? WASM_SIMD_F32X4_MIN : WASM_SIMD_F32X4_MAX, q, d);
    return true;
  }
  if (opc == 1u && b) { /* VAND VBIC VORR VORN VEOR VBSL VBIT VBIF */
    a32_load_vec(c, n, q);
    lset(c, L_VA);
    a32_load_vec(c, m, q);
    lset(c, L_VB);
    const uint32_t form = (u << 2) | size;
    if (form >= 5u) {
      a32_load_vec(c, d, q);
      lset(c, L_VD);
    }
    switch (form) {
    case 0: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_AND); break;
    case 1: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_ANDNOT); break;
    case 2: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_OR); break;
    case 3: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_NOT); simd(c, WASM_SIMD_V128_OR); break;
    case 4: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_XOR); break;
    case 5: lget(c, L_VA); lget(c, L_VB); lget(c, L_VD); simd(c, WASM_SIMD_V128_BITSELECT); break;
    case 6: lget(c, L_VA); lget(c, L_VD); lget(c, L_VB); simd(c, WASM_SIMD_V128_BITSELECT); break;
    default: lget(c, L_VD); lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_BITSELECT); break;
    }
    lset(c, L_VR);
    a32_store_vec(c, d, L_VR, q);
    return true;
  }
  static const uint32_t adds[4] = {WASM_SIMD_I8X16_ADD, WASM_SIMD_I16X8_ADD, WASM_SIMD_I32X4_ADD, WASM_SIMD_I64X2_ADD};
  static const uint32_t subs[4] = {WASM_SIMD_I8X16_SUB, WASM_SIMD_I16X8_SUB, WASM_SIMD_I32X4_SUB, WASM_SIMD_I64X2_SUB};
  uint32_t wasm_op;
  if (opc == 8u && !b) wasm_op = u ? subs[size] : adds[size]; /* VADD / VSUB (integer) */
  else if (opc == 9u && b && !u && (size == 1u || size == 2u)) wasm_op = size == 1u ? WASM_SIMD_I16X8_MUL : WASM_SIMD_I32X4_MUL;
  else return false;
  a32_load_vec(c, n, q);
  a32_load_vec(c, m, q);
  simd(c, wasm_op);
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  return true;
}

/* VMUL/VMLA/VMLS (F32) by scalar: Vm[index] in every lane (Dm is D0-D15). */
static bool a32_neon_by_scalar(Ctx *c, uint32_t insn) {
  const uint32_t opc = bits(insn, 11, 8), size = bits(insn, 21, 20);
  const bool q = bit(insn, 24);
  const uint32_t d = a32_neon_d(insn), n = a32_neon_n(insn), m = bits(insn, 3, 0), index = bit(insn, 5);
  if (size != 2u || (q && ((d | n) & 1u))) return false;
  Neon_Fp_Op nop;
  switch (opc) {
  case 0x1: nop = NEON_FP_MLA; break;
  case 0x5: nop = NEON_FP_MLS; break;
  case 0x9: nop = NEON_FP_MUL; break;
  default: return false; /* integer forms */
  }
  a32_load_vec(c, n, q);
  lset(c, L_VA);
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, a32_off_d(m) + 4u * index);
  simd(c, WASM_SIMD_I32X4_SPLAT);
  lset(c, L_VB);
  a32_neon_fp(c, nop, q, d);
  return true;
}

/* VEXT: bytes imm4.. of Vm:Vn (Vn the low end). */
static bool a32_neon_vext(Ctx *c, uint32_t insn) {
  const bool q = bit(insn, 6);
  const uint32_t imm = bits(insn, 11, 8), d = a32_neon_d(insn), n = a32_neon_n(insn), m = a32_neon_m(insn);
  if (q ? (d | n | m) & 1u : imm >= 8u) return false;
  const uint32_t length = q ? SIMD_LANE_BYTES : SIMD_LANE_BYTES / 2u;
  uint8_t lanes[SIMD_LANE_BYTES] = {0};
  for (uint32_t i = 0; i < length; i++) {
    const uint32_t at = i + imm;
    lanes[i] = (uint8_t)(at < length ? at : SIMD_LANE_BYTES + at - length);
  }
  a32_load_vec(c, n, q);
  a32_load_vec(c, m, q);
  v128_shuffle(c, lanes);
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  return true;
}

/* VDUP (scalar): Dm[index] in every lane. */
static bool a32_neon_vdup_scalar(Ctx *c, uint32_t insn) {
  const bool q = bit(insn, 6);
  const uint32_t imm4 = bits(insn, 19, 16), d = a32_neon_d(insn), m = a32_neon_m(insn);
  if (q && (d & 1u)) return false;
  uint32_t ebytes, index;
  if (imm4 & 1u) { ebytes = 1u; index = imm4 >> 1; }
  else if (imm4 & 2u) { ebytes = 2u; index = imm4 >> 2; }
  else if (imm4 & 4u) { ebytes = 4u; index = imm4 >> 3; }
  else return false;
  uint8_t lanes[SIMD_LANE_BYTES];
  for (uint32_t i = 0; i < SIMD_LANE_BYTES; i++) lanes[i] = (uint8_t)(index * ebytes + i % ebytes);
  a32_load_vec(c, m, false);
  a32_load_vec(c, m, false);
  v128_shuffle(c, lanes);
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  return true;
}

/* AdvSIMDExpandImm's 64-bit pattern; false: UNDEFINED (op 1, cmode 1111). */
static bool a32_neon_expand_imm(uint32_t op1, uint32_t cmode, uint32_t imm8, uint64_t *out) {
  const uint64_t i = imm8;
  uint64_t v;
  switch (cmode >> 1) {
  case 0: case 1: case 2: case 3: v = i << (8u * (cmode >> 1)); v |= v << 32; break;
  case 4: case 5: v = i << (8u * ((cmode >> 1) & 1u)); v |= v << 16; v |= v << 32; break;
  case 6: v = cmode & 1u ? (i << 16) | 0xFFFFu : (i << 8) | 0xFFu; v |= v << 32; break;
  default:
    if (!(cmode & 1u)) {
      if (!op1) {
        v = i * 0x0101010101010101ull;
      } else {
        v = 0;
        for (uint32_t k = 0; k < 8u; k++)
          if ((imm8 >> k) & 1u) v |= 0xFFull << (8u * k);
      }
    } else {
      if (op1) return false;
      v = a32_vfp_expand_imm(imm8, false);
      v |= v << 32;
    }
    break;
  }
  *out = v;
  return true;
}

/* VMOV, VMVN, VORR, VBIC (immediate). */
static bool a32_neon_modified_immediate(Ctx *c, uint32_t insn) {
  const bool q = bit(insn, 6);
  const uint32_t d = a32_neon_d(insn), cmode = bits(insn, 11, 8), op1 = bit(insn, 5);
  const uint32_t imm8 = (bit(insn, 24) << 7) | (bits(insn, 18, 16) << 4) | bits(insn, 3, 0);
  if (q && (d & 1u)) return false;
  uint64_t imm;
  if (!a32_neon_expand_imm(op1, cmode, imm8, &imm)) return false;
  const bool logical = (cmode & 1u) && cmode < 12u; /* VORR / VBIC */
  if (!logical) {
    if (op1 && cmode != 14u) imm = ~imm; /* VMVN */
    v128_const(c, imm, imm);
  } else {
    a32_load_vec(c, d, q);
    v128_const(c, imm, imm);
    simd(c, op1 ? WASM_SIMD_V128_ANDNOT : WASM_SIMD_V128_OR);
  }
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  return true;
}

/* VTRN, VUZP, VZIP: Vd and Vm become the two halves of the transposed,
 * de-interleaved or interleaved pair (byte shuffles fixed at compile
 * time). */
#define A32_NEON_VTRN 1u
#define A32_NEON_VUZP 2u
#define A32_NEON_VZIP 3u
static bool a32_neon_permute(Ctx *c, uint32_t insn, uint32_t kind) {
  const bool q = bit(insn, 6);
  const uint32_t size = bits(insn, 19, 18), d = a32_neon_d(insn), m = a32_neon_m(insn);
  if (size == 3u || d == m || (q && ((d | m) & 1u))) return false;
  if (!q && size == 2u && kind != A32_NEON_VTRN) return false; /* UNDEFINED */
  const uint32_t ebytes = 1u << size, lanes = (q ? SIMD_LANE_BYTES : SIMD_LANE_BYTES / 2u) / ebytes, half = lanes / 2u;
  uint32_t out_d[SIMD_LANE_BYTES], out_m[SIMD_LANE_BYTES]; /* element of (Vd, Vm): Vd's i, or Vm's i as lanes + i */
  for (uint32_t i = 0; i < half; i++) {
    switch (kind) {
    case A32_NEON_VZIP:
      out_d[2u * i] = i; out_d[2u * i + 1u] = lanes + i;
      out_m[2u * i] = half + i; out_m[2u * i + 1u] = lanes + half + i;
      break;
    case A32_NEON_VUZP:
      out_d[i] = 2u * i; out_d[half + i] = lanes + 2u * i;
      out_m[i] = 2u * i + 1u; out_m[half + i] = lanes + 2u * i + 1u;
      break;
    default: /* VTRN */
      out_d[2u * i] = 2u * i; out_d[2u * i + 1u] = lanes + 2u * i;
      out_m[2u * i] = 2u * i + 1u; out_m[2u * i + 1u] = lanes + 2u * i + 1u;
      break;
    }
  }
  uint8_t bytes_d[SIMD_LANE_BYTES] = {0}, bytes_m[SIMD_LANE_BYTES] = {0};
  for (uint32_t e = 0; e < lanes; e++) {
    for (uint32_t k = 0; k < ebytes; k++) {
      const uint32_t at = e * ebytes + k;
      bytes_d[at] = (uint8_t)(out_d[e] < lanes ? out_d[e] * ebytes + k : SIMD_LANE_BYTES + (out_d[e] - lanes) * ebytes + k);
      bytes_m[at] = (uint8_t)(out_m[e] < lanes ? out_m[e] * ebytes + k : SIMD_LANE_BYTES + (out_m[e] - lanes) * ebytes + k);
    }
  }
  a32_load_vec(c, d, q);
  lset(c, L_VA);
  a32_load_vec(c, m, q);
  lset(c, L_VB);
  lget(c, L_VA);
  lget(c, L_VB);
  v128_shuffle(c, bytes_d);
  lset(c, L_VR);
  lget(c, L_VA);
  lget(c, L_VB);
  v128_shuffle(c, bytes_m);
  lset(c, L_VT);
  a32_store_vec(c, d, L_VR, q);
  a32_store_vec(c, m, L_VT, q);
  return true;
}

/* Two registers, miscellaneous: VTRN/VUZP/VZIP, the F32 compares against
 * zero, VCVT.F32.S32/U32. */
static bool a32_neon_two_misc(Ctx *c, uint32_t insn) {
  const uint32_t a = bits(insn, 17, 16), b = bits(insn, 10, 6), size = bits(insn, 19, 18);
  const bool q = bit(insn, 6);
  const uint32_t d = a32_neon_d(insn), m = a32_neon_m(insn);
  if (a == 2u && (b >> 1) >= A32_NEON_VTRN && (b >> 1) <= A32_NEON_VZIP) return a32_neon_permute(c, insn, b >> 1);
  if (q && ((d | m) & 1u)) return false;
  if (a == 1u && (b & 0x10u) && size == 2u && ((b >> 1) & 7u) <= 4u) { /* VCGT VCGE VCEQ VCLE VCLT #0 (F32) */
    static const uint32_t ops[5] = {WASM_SIMD_F32X4_GT, WASM_SIMD_F32X4_GE, WASM_SIMD_F32X4_EQ, WASM_SIMD_F32X4_GE,
                                    WASM_SIMD_F32X4_GT};
    const uint32_t k = (b >> 1) & 7u;
    const bool swap = k >= 3u; /* VCLE, VCLT: 0 >= x, 0 > x */
    a32_load_vec(c, m, q);
    lset(c, swap ? L_VB : L_VA);
    f32c(c, 0);
    simd(c, WASM_SIMD_F32X4_SPLAT);
    lset(c, swap ? L_VA : L_VB);
    a32_neon_fp_exact(c, ops[k], q, d);
    return true;
  }
  if (a == 3u && (b & 0x1Cu) == 0x18u && size == 2u && !bit(insn, 8)) { /* VCVT.F32.S32 / .U32 */
    a32_load_vec(c, m, q);
    lset(c, L_VA);
    emit_fp_env_ok(c); /* rounding: IXC already set */
    fp_fast_arm(c);
    lget(c, L_VA);
    simd(c, bit(insn, 7) ? WASM_SIMD_F32X4_CONVERT_I32X4_U : WASM_SIMD_F32X4_CONVERT_I32X4_S);
    lset(c, L_VR);
    a32_store_vec(c, d, L_VR, q);
    a32_fp_else_exact(c);
    return true;
  }
  return false;
}

/* VTBL: Dd's bytes looked up in a table of one to four D registers from
 * Dn (an index past the table reads 0; i8x16.swizzle's 16-byte reach
 * covers two registers per lookup). VTBX keeps Dd's byte: not here. */
static void a32_neon_table_half(Ctx *c, uint32_t first, uint32_t regs) {
  if (regs == 2u) {
    lget(c, L_STATE);
    simd(c, WASM_SIMD_V128_LOAD);
    wasm_uleb(c->b, ALIGN_1);
    wasm_uleb(c->b, a32_off_d(first));
    return;
  }
  a32_load_vec(c, first, false);
  v128_const(c, ~(uint64_t)0, 0);
  simd(c, WASM_SIMD_V128_AND);
}
static bool a32_neon_vtbl(Ctx *c, uint32_t insn) {
  if (bit(insn, 6)) return false; /* VTBX */
  const uint32_t len = bits(insn, 9, 8) + 1u, n = a32_neon_n(insn), d = a32_neon_d(insn), m = a32_neon_m(insn);
  if (n + len > 32u) return false;
  a32_load_vec(c, m, false);
  lset(c, L_VB);
  a32_neon_table_half(c, n, len >= 2u ? 2u : 1u);
  lget(c, L_VB);
  simd(c, WASM_SIMD_I8X16_SWIZZLE);
  if (len > 2u) {
    a32_neon_table_half(c, n + 2u, len - 2u);
    lget(c, L_VB);
    v128_const(c, 0x1010101010101010ull, 0x1010101010101010ull);
    simd(c, WASM_SIMD_I8X16_SUB); /* indices 16..31 to 0..15; below 16 wrap past 15 and read 0 */
    simd(c, WASM_SIMD_I8X16_SWIZZLE);
    simd(c, WASM_SIMD_V128_OR);
  }
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, false);
  return true;
}

/* The Advanced SIMD data-processing space (unconditional, 1111 001x). */
static bool a32_neon_data_processing(Ctx *c, uint32_t insn) {
  if (!bit(insn, 23)) return a32_neon_three_same(c, insn);
  if (bit(insn, 4)) {
    if (bits(insn, 21, 19) == 0u && !bit(insn, 7)) return a32_neon_modified_immediate(c, insn);
    return false; /* shifts by immediate */
  }
  if (bits(insn, 21, 20) != 3u) return bit(insn, 6) ? a32_neon_by_scalar(c, insn) : false;
  if (!bit(insn, 24)) return a32_neon_vext(c, insn);
  if (bits(insn, 11, 8) < 8u) return a32_neon_two_misc(c, insn);
  if (bits(insn, 11, 10) == 2u) return a32_neon_vtbl(c, insn);
  if (bits(insn, 11, 8) == 0xCu && !bit(insn, 7)) return a32_neon_vdup_scalar(c, insn);
  return false;
}

/* VLD1 (one lane, all lanes) / VST1 (one lane): one element of 1-4
 * bytes at Rn, writeback as VLD1 (multiple). */
static bool a32_neon_load_store_lane(Ctx *c, uint32_t insn) {
  const uint32_t size = bits(insn, 11, 10), load = bit(insn, 21), rn = bits(insn, 19, 16), rm = bits(insn, 3, 0);
  const uint32_t d0 = a32_neon_d(insn);
  if (bits(insn, 9, 8) != 0u || rn == A32_REG_PC) return false; /* VLD2-4 */
  uint32_t ebytes, regs = 1;
  uint64_t lane_offset = 0;
  if (size == 3u) { /* all lanes */
    if (!load || bits(insn, 7, 6) == 3u) return false;
    ebytes = 1u << bits(insn, 7, 6);
    regs = bit(insn, 5) ? 2u : 1u;
  } else {
    ebytes = 1u << size;
    lane_offset = (uint64_t)(bits(insn, 7, 4) >> (size + 1u)) * ebytes;
  }
  if (d0 + regs > 32u) return false;
  static const uint8_t loads[3] = {WASM_OP_I64_LOAD8_U, WASM_OP_I64_LOAD16_U, WASM_OP_I64_LOAD32_U};
  static const uint8_t stores[3] = {WASM_OP_I64_STORE8, WASM_OP_I64_STORE16, WASM_OP_I64_STORE32};
  const uint32_t k = ebytes == 1u ? 0u : ebytes == 2u ? 1u : 2u;
  get_x(c, rn);
  lset(c, L_ADDR);
  open_block(c); /* $go */
  const uint32_t go = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, ebytes, load ? VMM_PERM_R : VMM_PERM_W, c->depth);
  br(c, go);
  end_(c);
  leave_via_interpreter(c);
  end_(c); /* $go: L_HOST */
  if (size == 3u) {
    lget(c, L_HOST);
    mem(c, loads[k], ALIGN_1, 0);
    i64c(c, ebytes == 1u ? 0x0101010101010101ull : ebytes == 2u ? 0x0001000100010001ull : 0x0000000100000001ull);
    op(c, WASM_OP_I64_MUL);
    lset(c, L_VAL);
    for (uint32_t r = 0; r < regs; r++) {
      lget(c, L_STATE);
      lget(c, L_VAL);
      mem(c, WASM_OP_I64_STORE, ALIGN_8, a32_off_d(d0 + r));
    }
  } else if (load) {
    lget(c, L_STATE);
    lget(c, L_HOST);
    mem(c, loads[k], ALIGN_1, 0);
    mem(c, stores[k], ALIGN_1, a32_off_d(d0) + lane_offset);
  } else {
    lget(c, L_HOST);
    lget(c, L_STATE);
    mem(c, loads[k], ALIGN_1, a32_off_d(d0) + lane_offset);
    mem(c, stores[k], ALIGN_1, 0);
  }
  if (rm != A32_REG_PC) {
    lget(c, L_ADDR);
    if (rm == A32_REG_SP) i64c(c, ebytes);
    else get_x(c, rm);
    op(c, WASM_OP_I64_ADD);
    mask32(c);
    set_x(c, rn);
  }
  return true;
}

/* VMOV between a core register and a D register's element (8, 16, 32
 * bits; to the core register sign- or zero-extended). */
static bool a32_vmov_scalar(Ctx *c, uint32_t insn) {
  const uint32_t rt = bits(insn, 15, 12), opc1 = bits(insn, 22, 21), opc2 = bits(insn, 6, 5);
  const uint32_t reg = (bit(insn, 7) << 4) | bits(insn, 19, 16), to_core = bit(insn, 20);
  if (rt == A32_REG_PC) return false;
  uint32_t ebytes, index;
  if (opc1 & 2u) { ebytes = 1u; index = ((opc1 & 1u) << 2) | opc2; }
  else if (opc2 & 1u) { ebytes = 2u; index = ((opc1 & 1u) << 1) | (opc2 >> 1); }
  else if (opc2 == 0u) { ebytes = 4u; index = opc1 & 1u; }
  else return false;
  if (to_core && ebytes == 4u && bit(insn, 23)) return false; /* U with 32 bits: UNDEFINED */
  const uint64_t at = a32_off_d(reg) + (uint64_t)index * ebytes;
  if (to_core) {
    static const uint8_t zero_ext[3] = {WASM_OP_I64_LOAD8_U, WASM_OP_I64_LOAD16_U, WASM_OP_I64_LOAD32_U};
    static const uint8_t sign_ext[2] = {WASM_OP_I64_LOAD8_S, WASM_OP_I64_LOAD16_S};
    const uint32_t k = ebytes == 1u ? 0u : ebytes == 2u ? 1u : 2u;
    lget(c, L_STATE);
    mem(c, (ebytes == 4u || bit(insn, 23)) ? zero_ext[k] : sign_ext[k], ALIGN_1, at);
    if (ebytes != 4u && !bit(insn, 23)) mask32(c);
    set_x(c, rt);
  } else {
    static const uint8_t stores[3] = {WASM_OP_I64_STORE8, WASM_OP_I64_STORE16, WASM_OP_I64_STORE32};
    lget(c, L_STATE);
    get_x(c, rt);
    mem(c, stores[ebytes == 1u ? 0u : ebytes == 2u ? 1u : 2u], ALIGN_1, at);
  }
  return true;
}

/* VDUP (core register): Rt's low 8, 16 or 32 bits in every lane. */
static bool a32_vdup_core(Ctx *c, uint32_t insn) {
  const uint32_t rt = bits(insn, 15, 12), be = (bit(insn, 22) << 1) | bit(insn, 5), d = (bit(insn, 7) << 4) | bits(insn, 19, 16);
  const bool q = bit(insn, 21);
  if (rt == A32_REG_PC || be == 3u || (q && (d & 1u))) return false;
  get_x(c, rt);
  op(c, WASM_OP_I32_WRAP_I64);
  simd(c, be == 2u ? WASM_SIMD_I8X16_SPLAT : be == 1u ? WASM_SIMD_I16X8_SPLAT : WASM_SIMD_I32X4_SPLAT);
  lset(c, L_VR);
  a32_store_vec(c, d, L_VR, q);
  return true;
}

/* ---- VFP and Advanced SIMD: direct interpreter calls ------------------------ */

/* The VFP/NEON instruction runs in the A32 interpreter without retiring
 * (jit_helper_simd -> a32_execute): only the general registers it names
 * are synchronised around it; D registers and FPSCR live in the state.
 * Inside a compiled condition the instruction goes over as AL (the
 * interpreter would test stale flags otherwise). */
static Outcome a32_direct(Ctx *c, uint32_t insn) {
  Sync sync = {0, false};
  const uint32_t top = bits(insn, 31, 28);
  if (top == A32_COND_NV) {
    if (bits(insn, 27, 24) == 4u) { /* element and structure loads/stores: Rn, Rm */
      sync_x(&sync, bits(insn, 19, 16));
      if (bits(insn, 3, 0) < A32_REG_PC) sync_x(&sync, bits(insn, 3, 0));
    } else if (bits(insn, 27, 25) != 1u) {
      return OUTCOME_HELPER;
    }
  } else {
    if (bits(insn, 11, 9) != 5u) return OUTCOME_HELPER; /* not coprocessor 10/11 (CP15: the interpreter) */
    const uint32_t group = bits(insn, 27, 25), rn = bits(insn, 19, 16), rt = bits(insn, 15, 12);
    if (group == 6u) { /* loads/stores (Rn), and the two-register VMOVs (Rt, Rt2) */
      if (bits(insn, 24, 21) == 2u) {
        if (rt == A32_REG_PC || rn == A32_REG_PC) return OUTCOME_HELPER;
        sync_x(&sync, rt);
        sync_x(&sync, rn);
      } else if (rn != A32_REG_PC) {
        sync_x(&sync, rn);
      }
    } else if (bit(insn, 4)) { /* transfers: Rt (r15: VMRS APSR_nzcv) */
      if (rt == A32_REG_PC) sync.nzcv = true;
      else sync_x(&sync, rt);
    }
    if (sync.nzcv) flags_written(c);
  }
  /* The PC, for PC-relative VLDR. */
  lget(c, L_STATE);
  i64c(c, c->pc);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_PC);
  const uint32_t original = c->insn;
  if (top != A32_COND_NV) c->insn = (insn & 0x0FFFFFFFu) | (A32_COND_AL << 28);
  emit_direct_call(c, &sync);
  c->insn = original;
  return OUTCOME_NEXT;
}

/* ---- exclusives, acquire/release, the thread registers ---------------------- */

/* LDREX/STREX/LDAEX/STLEX (byte, halfword, word) and LDA/STL, as the
 * interpreter's synchronization(): one monitor per thread (this backend
 * never runs multicore), exact address and size. Doublewords: the
 * interpreter. */
static bool a32_synchronization(Ctx *c, uint32_t insn) {
  static const uint32_t k_size[4] = {4u, 8u, 1u, 2u};
  const uint32_t size = k_size[bits(insn, 22, 21)], ordered = bits(insn, 9, 8), load = bit(insn, 20);
  const uint32_t rn = bits(insn, 19, 16), rd = bits(insn, 15, 12), rt = bits(insn, 3, 0);
  if (!bit(insn, 23) || size == 8u || ordered == 1u || cpu_multicore()) return false;
  if (rn == A32_REG_PC || ((load || ordered) && rd == A32_REG_PC) || (!load && rt == A32_REG_PC)) return false;
  get_x(c, rn);
  lset(c, L_ADDR);
  if (load) {
    emit_load_address(c, size);
    emit_host_load(c, ACCESS_LOAD_ZERO, size, 0, L_VAL);
    if (ordered) { /* arm the monitor */
      lget(c, L_STATE);
      i32c(c, 1);
      mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
      state_store64_local(c, OFF_EXCLUSIVE_ADDRESS, L_ADDR);
      lget(c, L_STATE);
      i32c(c, size);
      mem(c, WASM_OP_I32_STORE, ALIGN_4, OFF_EXCLUSIVE_SIZE);
    }
    lget(c, L_VAL);
    set_x(c, rd);
    return true;
  }
  if (!ordered) { /* STL */
    emit_store(c, size, rt, false, 0);
    return true;
  }
  /* STREX: stores iff the monitor holds this address and size; Rd = 0 on
   * success, 1 otherwise; the monitor clears either way. */
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD8_U, ALIGN_1, OFF_EXCLUSIVE_VALID);
  state_load64(c, OFF_EXCLUSIVE_ADDRESS);
  lget(c, L_ADDR);
  op(c, WASM_OP_I64_EQ);
  op(c, WASM_OP_I32_AND);
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_EXCLUSIVE_SIZE);
  i32c(c, size);
  op(c, WASM_OP_I32_EQ);
  op(c, WASM_OP_I32_AND);
  ltee(c, L_PASS);
  open_if(c, WASM_BLOCK_VOID);
  emit_store(c, size, rt, false, 0);
  end_(c);
  lget(c, L_STATE);
  i32c(c, 0);
  mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
  lget(c, L_PASS);
  op(c, WASM_OP_I32_EQZ);
  op(c, WASM_OP_I64_EXTEND_I32_U);
  set_x(c, rd);
  return true;
}

/* MRC/MCR p15 for TPIDRURW (c13, c0, 2) and MRC for TPIDRURO (c13, c0, 3). */
#define A32_CP15_REG_MASK 0x0FFF0FFFu
#define A32_MRC_TPIDRURO 0x0E1D0F70u
#define A32_MRC_TPIDRURW 0x0E1D0F50u
#define A32_MCR_TPIDRURW 0x0E0D0F50u
static bool a32_cp15(Ctx *c, uint32_t insn) {
  const uint32_t rt = bits(insn, 15, 12), key = insn & A32_CP15_REG_MASK;
  if (rt == A32_REG_PC) return false;
  if (key == A32_MRC_TPIDRURO || key == A32_MRC_TPIDRURW) {
    state_load64(c, key == A32_MRC_TPIDRURO ? OFF_TPIDRRO : OFF_TPIDR);
    mask32(c);
    set_x(c, rt);
    return true;
  }
  if (key == A32_MCR_TPIDRURW) {
    lget(c, L_STATE);
    get_x(c, rt);
    mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_TPIDR);
    return true;
  }
  return false;
}

/* ---- the dispatcher ------------------------------------------------------- */

/* May the instruction read or write NZCV (beyond its condition)? Bodies
 * that do run inside the condition's `if` with the flags live on both
 * arms. Conservative: anything unknown says yes. */
static bool a32_touches_flags(uint32_t insn) {
  const uint32_t top = bits(insn, 27, 25);
  if (top == 0u || top == 1u) {
    if (top == 0u && bit(insn, 7) && bit(insn, 4)) return bit(insn, 20) && !bits(insn, 6, 5); /* MULS & co */
    const uint32_t opcode = bits(insn, 24, 21);
    if (bit(insn, 20)) return true;
    if (opcode == A32_ADC || opcode == A32_SBC || opcode == A32_RSC) return true;
    if (top == 0u && !bit(insn, 4) && bits(insn, 6, 5) == 3u && bits(insn, 11, 7) == 0) return true; /* RRX */
    return false;
  }
  return top != 2u && top != 3u && top != 4u;
}

/* Data processing: an inlined write to the PC ended the block; one not
 * inlined still may (an interworking branch, an exception return). */
static Outcome a32_dp_outcome(uint32_t insn, bool inlined) {
  const uint32_t opcode = bits(insn, 24, 21);
  const bool pc = bits(insn, 15, 12) == A32_REG_PC && !(opcode >= A32_TST && opcode <= A32_CMN);
  if (inlined) return pc ? OUTCOME_END : OUTCOME_NEXT;
  return pc ? OUTCOME_END_HELPER : OUTCOME_HELPER;
}

/* The unconditional body of a non-branch instruction; false: not inlined
 * (nothing was emitted that matters - the caller discards it). */
static Outcome a32_body(Ctx *c, uint32_t insn) {
  const uint32_t top = bits(insn, 27, 25);
  switch (top) {
  case 0: {
    if (bit(insn, 7) && bit(insn, 4)) {
      if (bits(insn, 6, 5) == 0) {
        if (bits(insn, 24, 24)) return a32_synchronization(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
        return a32_multiply(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
      }
      return a32_load_store_extra(c, insn) ? OUTCOME_NEXT : (bits(insn, 15, 12) == A32_REG_PC ? OUTCOME_END_HELPER : OUTCOME_HELPER);
    }
    const uint32_t op5 = bits(insn, 24, 20);
    if ((op5 & 0x19u) == 0x10u) { /* miscellaneous: BX, BLX, CLZ, MRS, ... */
      if ((insn & 0x0FFFFFD0u) == 0x012FFF10u) { /* BX / BLX (register) */
        a32_branch_register(c, bits(insn, 3, 0), bit(insn, 5));
        return OUTCOME_END;
      }
      if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) return a32_reverse(c, insn, true) ? OUTCOME_NEXT : OUTCOME_HELPER; /* CLZ */
      return bit(insn, 7) ? OUTCOME_HELPER : OUTCOME_END_HELPER;
    }
    return a32_dp_outcome(insn, a32_data_processing(c, insn));
  }
  case 1: {
    const uint32_t op5 = bits(insn, 24, 20);
    if (op5 == 0x10u || op5 == 0x14u) return a32_move_wide(c, insn) ? OUTCOME_NEXT : OUTCOME_END_HELPER;
    if ((op5 & 0x1Bu) == 0x12u) return OUTCOME_HELPER; /* MSR (immediate), hints */
    return a32_dp_outcome(insn, a32_data_processing(c, insn));
  }
  case 3:
    if (bit(insn, 4)) return a32_media(c, insn);
    return a32_load_store_word(c, insn);
  case 2: return a32_load_store_word(c, insn);
  case 4: return a32_block_transfer(c, insn);
  case 7:
    if (bits(insn, 27, 24) == 0xFu) return OUTCOME_END_HELPER; /* SVC */
    if (bit(insn, 4) && bits(insn, 11, 8) == 0xFu && a32_cp15(c, insn)) return OUTCOME_NEXT;
    if (bits(insn, 11, 9) == 5u && bit(insn, 4) && a32_vfp_transfer(c, insn)) return OUTCOME_NEXT;
    if (bits(insn, 27, 24) == 0xEu && !bit(insn, 4) && a32_vfp_arith(c, insn)) return OUTCOME_NEXT;
    return a32_direct(c, insn); /* VFP data processing and transfers */
  default:
    if (bits(insn, 11, 9) == 5u) {
      if (bits(insn, 27, 25) == 6u && bits(insn, 24, 21) == 2u && a32_vfp_transfer(c, insn)) return OUTCOME_NEXT;
      if (a32_vfp_load_store(c, insn)) return OUTCOME_NEXT;
      if (bits(insn, 27, 25) == 6u && a32_vfp_load_store_multiple(c, insn)) return OUTCOME_NEXT;
    }
    return a32_direct(c, insn); /* VFP loads/stores */
  }
}

/* Compile-time flag state, put back when an attempt is thrown away. */
typedef struct A32_Flag_Snapshot {
  Flag_Kind kind;
  bool sf;
  Carry carry;
  Flag_Event event;
} A32_Flag_Snapshot;

/* Debugging: A32_JIT_OFF=mask leaves instruction classes to the
 * interpreter (1 data processing, 2 loads/stores, 4 extra loads/stores,
 * 8 LDM/STM, 16 branches, 32 media, 64 direct VFP/NEON, 128 conditional,
 * 256 multiplies), to bisect a miscompile in a real title. */
static uint32_t a32_off(void) {
  static int mask = -1;
  if (mask < 0) mask = getenv("A32_JIT_OFF") ? (int)strtoul(getenv("A32_JIT_OFF"), NULL, 0) : 0;
  return (uint32_t)mask;
}

static uint32_t a32_class(uint32_t insn) {
  const uint32_t top = bits(insn, 27, 25);
  if (bits(insn, 31, 28) == A32_COND_NV) return 64u;
  switch (top) {
  case 0:
    if (bit(insn, 7) && bit(insn, 4)) return bits(insn, 6, 5) ? 4u : 256u;
    if ((bits(insn, 24, 20) & 0x19u) == 0x10u) return 16u;
    return 1u;
  case 1: return 1u;
  case 2: return 2u;
  case 3: return bit(insn, 4) ? 32u : 2u;
  case 4: return 8u;
  case 5: return 16u;
  default: return 64u;
  }
}

static Outcome a32_compile_instruction_checked(Ctx *c, uint32_t insn);
static Outcome a32_compile_instruction(Ctx *c, uint32_t insn) {
  const uint32_t off = a32_off();
  if (off) {
    const uint32_t k = a32_class(insn);
    const bool conditional = bits(insn, 31, 28) < A32_COND_AL;
    if ((off & k) || (conditional && (off & 128u))) {
      const uint32_t top = bits(insn, 27, 25);
      return (top == 4u || top == 5u || (top == 0u && (bits(insn, 24, 20) & 0x19u) == 0x10u) ||
              bits(insn, 15, 12) == A32_REG_PC || bits(insn, 27, 24) == 0xFu)
                 ? OUTCOME_END_HELPER
                 : OUTCOME_HELPER;
    }
  }
  return a32_compile_instruction_checked(c, insn);
}

static Outcome a32_compile_instruction_unpredicted(Ctx *c, uint32_t insn);
static Outcome a32_compile_instruction_checked(Ctx *c, uint32_t insn) {
  uint32_t rd = 0;
  uint64_t value = 0;
  const bool predicts = c->predicted_mask && a32_predict(c, insn, &rd, &value);
  const Outcome outcome = a32_compile_instruction_unpredicted(c, insn);
  if (predicts && outcome == OUTCOME_NEXT) {
    c->predicted_mask |= 1u << rd;
    c->predicted_value[rd] = value;
  }
  return outcome;
}

static Outcome a32_compile_instruction_unpredicted(Ctx *c, uint32_t insn) {
  const A32_Flag_Snapshot saved = {c->flag_kind, c->flag_sf, c->flag_carry, c->flag_event};
  const uint32_t cond = bits(insn, 31, 28);
  if (cond == A32_COND_NV) { /* Advanced SIMD directly; BLX (immediate) is Thumb */
    if (bits(insn, 27, 25) == 5u) return OUTCOME_END_HELPER;
    if (bits(insn, 27, 24) == 4u && a32_neon_load_store(c, insn)) return OUTCOME_NEXT;
    if (bits(insn, 27, 25) == 1u && a32_neon_data_processing(c, insn)) return OUTCOME_NEXT;
    if (a32_jit_vfp_v8(c, insn)) return OUTCOME_NEXT;
    return a32_direct(c, insn);
  }
  if (bits(insn, 27, 25) == 5u) { /* B, BL */
    const uint64_t target = a32_branch_target(c, insn);
    const bool link = bit(insn, 24);
    if (cond == A32_COND_AL) {
      if (link) a32_branch_link(c, target);
      else branch_to(c, target);
      return OUTCOME_END;
    }
    emit_condition(c, cond);
    open_if(c, WASM_BLOCK_VOID);
    if (link) {
      a32_branch_link(c, target);
    } else {
      branch_to(c, target);
    }
    else_(c);
    branch_to(c, c->pc + INSN_BYTES);
    end_(c);
    return OUTCOME_END;
  }
  if (cond == A32_COND_AL) {
    const Outcome outcome = a32_body(c, insn);
    if (outcome == OUTCOME_HELPER || outcome == OUTCOME_END_HELPER) {
      c->flag_kind = saved.kind;
      c->flag_sf = saved.sf;
      c->flag_carry = saved.carry;
      c->flag_event = saved.event;
    }
    return outcome;
  }
  /* Conditional: the body inside `if (cond)`. Flags the body touches are
   * live on both arms; a body that ends the block (a conditional return)
   * branches out of its arm, and the other arm goes on. */
  const bool touches = a32_touches_flags(insn);
  if (touches) materialize(c);
  emit_condition(c, cond);
  open_if(c, WASM_BLOCK_VOID);
  const Outcome outcome = a32_body(c, insn);
  if (outcome == OUTCOME_HELPER || outcome == OUTCOME_END_HELPER) {
    c->flag_kind = saved.kind;
    c->flag_sf = saved.sf;
    c->flag_carry = saved.carry;
    c->flag_event = saved.event;
    return outcome; /* the interpreter evaluates the condition itself */
  }
  if (touches && c->flag_kind != FLAGS_LIVE) materialize(c);
  if (outcome == OUTCOME_END) {
    else_(c);
    branch_to(c, c->pc + INSN_BYTES);
    end_(c);
    return OUTCOME_END;
  }
  end_(c);
  return OUTCOME_NEXT;
}
