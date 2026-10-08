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

static void a32_leave_for_val(Ctx *c) {
  lget(c, L_VAL);
  i64c(c, ~(uint64_t)A32_WORD_ALIGN_MASK & MASK32);
  op(c, WASM_OP_I64_AND);
  exit_to_stack(c);
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
      a32_leave_for_val(c);
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
 * the interpreter's. */
static void a32_branch_register(Ctx *c, uint32_t rm, bool link) {
  a32_get(c, rm);
  lset(c, L_VAL);
  a32_check_arm_target(c);
  if (link) {
    i64c(c, (c->pc + INSN_BYTES) & MASK32);
    set_x(c, A32_REG_LR);
  }
  a32_leave_for_val(c);
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
        if (bits(insn, 24, 24)) return OUTCOME_HELPER; /* synchronization */
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
  case 7: if (bits(insn, 27, 24) == 0xFu) return OUTCOME_END_HELPER; /* SVC */
    return a32_direct(c, insn); /* VFP data processing and transfers */
  default: return a32_direct(c, insn); /* VFP loads/stores */
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

static Outcome a32_compile_instruction_checked(Ctx *c, uint32_t insn) {
  const A32_Flag_Snapshot saved = {c->flag_kind, c->flag_sf, c->flag_carry, c->flag_event};
  const uint32_t cond = bits(insn, 31, 28);
  if (cond == A32_COND_NV) { /* Advanced SIMD directly; BLX (immediate) is Thumb */
    if (bits(insn, 27, 25) == 5u) return OUTCOME_END_HELPER;
    return a32_direct(c, insn);
  }
  if (bits(insn, 27, 25) == 5u) { /* B, BL */
    const uint64_t target = a32_branch_target(c, insn);
    const bool link = bit(insn, 24);
    if (cond == A32_COND_AL) {
      if (link) {
        i64c(c, (c->pc + INSN_BYTES) & MASK32);
        set_x(c, A32_REG_LR);
        i64c(c, target);
        exit_to_stack(c);
      } else {
        branch_to(c, target);
      }
      return OUTCOME_END;
    }
    emit_condition(c, cond);
    open_if(c, WASM_BLOCK_VOID);
    if (link) {
      i64c(c, (c->pc + INSN_BYTES) & MASK32);
      set_x(c, A32_REG_LR);
      i64c(c, target);
      exit_to_stack(c);
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
