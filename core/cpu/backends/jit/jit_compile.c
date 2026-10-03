/**
 * A64 block -> WebAssembly function (jit_internal.h, docs/JIT.md).
 *
 * Function shape (depths are wasm label depths inside the body):
 *
 *   prologue: l1 and every guest register the block touches -> locals
 *   block $raw                       ;; leave with $status, state complete
 *     block $exit                    ;; leave normally: $npc, $ncycles
 *       ...instructions...
 *     end
 *     epilogue: written locals -> state, pc = $npc, cycles += $ncycles
 *     return JIT_BLOCK_CONTINUE
 *   end
 *   return $status
 *
 * Semantics come from the interpreter (interp_*.c), which is the oracle
 * tests/jit_diff_test.c checks every compiled form against. Anything not
 * inlined here calls jit_helper_interpret for that one instruction with
 * the state spilled; loads and stores inline the softmmu walk of
 * vmm_translate_inline() and take the same helper when the walk cannot
 * finish (unmapped, no permission, page-crossing), so faults are exactly
 * the interpreter's.
 *
 * Cycle accounting is exact: `pending` counts instructions retired inline
 * since the state's counters were last brought up to date; every exit adds
 * it, and a helper call flushes it first (the helper retires its own
 * instruction).
 *
 * The body is emitted twice: the first pass only collects which registers
 * the block reads and writes, the second emits the prologue/spill/reload
 * code from those sets.
 */
#include "cpu/backends/jit/jit_internal.h"

#include <stddef.h>

#include "common/assert.h"
#include "cpu/backends/interpreter/softfloat.h"
#include "cpu/backends/jit/jit_wasm.h"

/* ------------------------------------------------------------------ */
/* Function layout.                                                    */
/* ------------------------------------------------------------------ */

enum {
  L_STATE = 0, /* the parameter */
  L_X0 = 1,    /* x0..x30 -> 1..31 */
  L_SP = 32,
  L_L1 = 33,
  L_NPC = 34,
  L_CYC = 35,  /* cycles retired in this call, not yet in the state (incl. all of the current block) */
  L_T0 = 36,
  L_T1 = 37,
  L_T2 = 38,
  L_T3 = 39,
  L_ADDR = 40,
  L_HOST = 41,
  L_BASE = 42,
  L_VAL = 43,
  L_VAL2 = 44,
  L_ROOM = 45, /* cycle_budget - the state's cycles_consumed */
  L_HPC = 46,  /* the instruction the shared handlers give the interpreter: PC */
  L_HREST = 47, /* ...the instructions pre-counted in L_CYC but not run, from it on */
  L_HNEXT = 48, /* $helper: the PC to resume at */
  L_HNEXTLEN = 49, /* ...and that region block's length */
  L_FA = 50,   /* lazy flags: AddWithCarry's operands and result, width-masked */
  L_FB = 51,
  L_FR = 52,
  L_LAST_I64 = 52,
  L_NZCV = 53,
  L_STATUS = 54,
  L_IDX = 55,    /* the region target to dispatch to */
  L_HINSN = 56,  /* the handlers' instruction word */
  L_RESUME = 57, /* $helper: the region block to resume in */
  L_PASS = 58,   /* store-exclusive: did the monitor pass */
  L_LAST_I32 = 58,
  L_FS = 59,     /* FP fast paths: f32 */
  L_FS2 = 60,
  L_FS3 = 61,
  L_LAST_F32 = 61,
  L_FD = 62,     /* ...f64 */
  L_FD2 = 63,
  L_FD3 = 64,
  L_LAST_F64 = 64,
  L_VA = 65,     /* vector FP fast paths: v128 */
  L_VB = 66,
  L_VD = 67,
  L_VR = 68,
  L_VR2 = 69,
  L_VT = 70,
  L_LAST_V128 = 70,
};
#define I64_LOCALS (L_LAST_I64 - L_STATE)
#define I32_LOCALS (L_LAST_I32 - L_LAST_I64)
#define F32_LOCALS (L_LAST_F32 - L_LAST_I32)
#define F64_LOCALS (L_LAST_F64 - L_LAST_F32)
#define V128_LOCALS (L_LAST_V128 - L_LAST_F64)

/* Imported function indices, then the block function. */
#define FUNC_INTERPRET 0u
#define FUNC_READ 1u
#define FUNC_STORE 2u
#define FUNC_WRITE 3u
#define FUNC_SIMD 4u
#define FUNC_BLOCK 5u
#define TYPE_BLOCK 0u
#define TYPE_INTERPRET 1u
#define TYPE_READ 2u
#define TYPE_STORE 3u
#define TYPE_COUNT 4u
#define IMPORT_COUNT 7u /* table, interpret, read, store, write, simd, memory */

/* Label levels (the wasm depth right after the label's block opened). */
#define LEVEL_LEAVE 1u    /* the instruction in L_HPC/L_HINSN through the interpreter, then return */
#define LEVEL_EXIT 2u     /* leave normally for L_NPC */
#define LEVEL_DISPATCH 3u /* the region loop: br_table on L_IDX */

/* Register-set bits: x0..x30, then SP and NZCV. */
#define MASK_SP ((uint64_t)1 << 31)
#define MASK_NZCV ((uint64_t)1 << 32)
#define REG_ZR 31u

#define STATE_OFFSET(field) ((uint64_t)offsetof(Jit_State, field))
#define OFF_X(r) (STATE_OFFSET(interp.regs.x) + (uint64_t)(r) * sizeof(uint64_t))
#define OFF_SP STATE_OFFSET(interp.regs.sp)
#define OFF_PC STATE_OFFSET(interp.regs.pc)
#define OFF_PSTATE STATE_OFFSET(interp.regs.pstate)
#define OFF_FPCR STATE_OFFSET(interp.fpcr)
#define OFF_FPSR STATE_OFFSET(interp.fpsr)
#define OFF_TPIDR STATE_OFFSET(interp.tpidr_el0)
#define OFF_TPIDRRO STATE_OFFSET(interp.tpidrro_el0)
#define OFF_EXCLUSIVE_VALID STATE_OFFSET(interp.exclusive_valid)
#define OFF_CYCLES STATE_OFFSET(interp.cycles_consumed)
#define OFF_TOTAL_CYCLES STATE_OFFSET(interp.total_cycles)
#define OFF_L1 STATE_OFFSET(interp.l1)
#define OFF_BUDGET STATE_OFFSET(cycle_budget)
#define OFF_SCRATCH STATE_OFFSET(scratch)
#define OFF_EXCLUSIVE_ADDRESS STATE_OFFSET(interp.exclusive_address)
#define OFF_EXCLUSIVE_SIZE STATE_OFFSET(interp.exclusive_size)
#define OFF_EXCLUSIVE_VALUE STATE_OFFSET(interp.exclusive_value)
#define OFF_V(t) (STATE_OFFSET(interp.v) + (uint64_t)(t) * sizeof(CPU_Vector_Register))
#define V_HIGH_HALF 8u /* CPU_Vector_Register.hi */

#define ENTRY_OFFSET(field) ((uint64_t)offsetof(Jit_Entry, field))
#define TABLE_INDEX 0u

#define ALIGN_8 3u /* memarg alignment hints, log2 */
#define ALIGN_4 2u
#define ALIGN_1 0u

#define MASK32 0xFFFFFFFFull
#define INSN_BYTES 4u

/* The softmmu walk (vmm.h): L1 index = gva >> (page + L2 index bits);
 * L2 byte offset = ((gva >> page bits) & L2 mask) * 8. */
#define WALK_L1_SHIFT (VMM_PAGE_BITS + VMM_L2_INDEX_BITS)
#define WALK_ENTRY_LOG2 3u
#define WALK_L2_SHIFT (VMM_PAGE_BITS - WALK_ENTRY_LOG2)
#define WALK_L2_MASK (VMM_L2_INDEX_MASK << WALK_ENTRY_LOG2)

/* Regions: the entry block plus blocks in the same page it reaches by
 * direct branches, in one function (docs/JIT.md). */
#define JIT_MAX_REGION_BLOCKS 32u

typedef struct Region_Block {
  uint64_t pc;
  uint32_t length;  /* instructions */
  bool needs_flags; /* may observe NZCV before writing it: entering edges materialize */
  bool return_site; /* the instruction after a BL in the region: a RET may come back here */
} Region_Block;

typedef struct Region_Page {
  uint64_t base;
  const uint32_t *code;
} Region_Page;

typedef enum Carry { CARRY_ZERO, CARRY_ONE, CARRY_FLAG } Carry;

/* Lazy flags: after ADDS/SUBS/ANDS & co. only the operands are kept
 * (L_FA/L_FB/L_FR); readers right after them test the operands directly
 * (CMP + B.cond is two compares, not a 40-op NZCV build), and NZCV is
 * built only where it can be observed - exits, the interpreter, and
 * edges into region blocks that may read it before writing it. */
typedef enum Flag_Kind {
  FLAGS_LIVE,  /* L_NZCV holds the flags */
  FLAGS_ADD,   /* AddWithCarry(L_FA, L_FB, flag_carry) = L_FR */
  FLAGS_LOGIC, /* N and Z of L_FR; C = V = 0 */
} Flag_Kind;

typedef enum Flag_Event { FLAG_EVENT_NONE, FLAG_EVENT_READ, FLAG_EVENT_WRITE } Flag_Event;

typedef struct Ctx {
  Wasm_Buf *b;
  const Jit_Link *link;
  uint32_t depth;
  uint64_t used, written;         /* this pass */
  uint64_t all_used, all_written; /* the analysis pass's result */
  bool discover;                  /* analysis pass: collect region blocks */
  Region_Block blocks[JIT_MAX_REGION_BLOCKS];
  uint32_t block_count, max_blocks, max_block_insns;
  const Jit_Code_Source *source;
  Region_Page pages[JIT_MAX_REGION_PAGES];
  uint32_t page_count;
  /* Within the current block: registers holding known constants (ADRP,
   * ADD #imm) and registers predicted from guest memory at compile time
   * (an LDR from a known address: a PLT stub's GOT slot). */
  uint32_t known_mask, predicted_mask;
  uint64_t known_value[31], predicted_value[31];
  uint32_t block_len;             /* the current block's length (final pass) */
  bool uses_helper;               /* some block calls the shared $helper */
  Flag_Kind flag_kind;            /* lazy flags, at this point of the block */
  bool flag_sf;
  Carry flag_carry;
  Flag_Event flag_event;          /* the block's first flags access */
  uint32_t index;                 /* the current instruction's index in its block */
  uint64_t pc;
  uint32_t insn;
  uint32_t max_local;     /* highest local used (this pass) */
  uint32_t declared_local; /* how many to declare (the analysis pass's max_local) */
} Ctx;

/* ------------------------------------------------------------------ */
/* Emission primitives.                                                */
/* ------------------------------------------------------------------ */

static void op(Ctx *c, uint8_t opcode) { wasm_u8(c->b, opcode); }
static void i64c(Ctx *c, uint64_t value) {
  op(c, WASM_OP_I64_CONST);
  wasm_sleb(c->b, (int64_t)value);
}
static void i32c(Ctx *c, uint32_t value) {
  op(c, WASM_OP_I32_CONST);
  wasm_sleb(c->b, (int32_t)value);
}
static void note_local(Ctx *c, uint32_t local) {
  if (local > c->max_local) c->max_local = local;
}
static void lget(Ctx *c, uint32_t local) {
  note_local(c, local);
  op(c, WASM_OP_LOCAL_GET);
  wasm_uleb(c->b, local);
}
static void lset(Ctx *c, uint32_t local) {
  note_local(c, local);
  op(c, WASM_OP_LOCAL_SET);
  wasm_uleb(c->b, local);
}
static void ltee(Ctx *c, uint32_t local) {
  note_local(c, local);
  op(c, WASM_OP_LOCAL_TEE);
  wasm_uleb(c->b, local);
}
static void mem(Ctx *c, uint8_t opcode, uint32_t align_log2, uint64_t offset) {
  op(c, opcode);
  wasm_uleb(c->b, align_log2);
  wasm_uleb(c->b, offset);
}
static void open_block(Ctx *c) {
  op(c, WASM_OP_BLOCK);
  op(c, WASM_BLOCK_VOID);
  c->depth++;
}
static void open_if(Ctx *c, uint8_t result_type) {
  op(c, WASM_OP_IF);
  op(c, result_type);
  c->depth++;
}
static void else_(Ctx *c) { op(c, WASM_OP_ELSE); }
static void end_(Ctx *c) {
  op(c, WASM_OP_END);
  c->depth--;
}
static void br(Ctx *c, uint32_t level) {
  op(c, WASM_OP_BR);
  wasm_uleb(c->b, c->depth - level);
}
static void br_if(Ctx *c, uint32_t level) {
  op(c, WASM_OP_BR_IF);
  wasm_uleb(c->b, c->depth - level);
}

/* State fields (the parameter is the state's address). */
static void state_load64(Ctx *c, uint64_t offset) {
  lget(c, L_STATE);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, offset);
}
/* Stores the i64 in `local` to the state field. */
static void state_store64_local(Ctx *c, uint64_t offset, uint32_t local) {
  lget(c, L_STATE);
  lget(c, local);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, offset);
}

/* 32-bit results are zero-extended. */
static void mask32(Ctx *c) {
  op(c, WASM_OP_I32_WRAP_I64);
  op(c, WASM_OP_I64_EXTEND_I32_U);
}
static void width(Ctx *c, bool sf) {
  if (!sf) mask32(c);
}
static uint64_t width_mask_of(bool sf) { return sf ? ~(uint64_t)0 : MASK32; }

/* ------------------------------------------------------------------ */
/* Guest registers. Field value 31 is XZR or SP by instruction context. */
/* ------------------------------------------------------------------ */

static void get_x(Ctx *c, uint32_t r) {
  if (r == REG_ZR) {
    i64c(c, 0);
    return;
  }
  c->used |= (uint64_t)1 << r;
  lget(c, L_X0 + r);
}
static void get_xsp(Ctx *c, uint32_t r) {
  if (r != REG_ZR) {
    get_x(c, r);
    return;
  }
  c->used |= MASK_SP;
  lget(c, L_SP);
}
static void get_xw(Ctx *c, uint32_t r, bool sf) {
  get_x(c, r);
  width(c, sf);
}
/* Pops the value into the register (dropped for XZR). */
static void set_x(Ctx *c, uint32_t r) {
  if (r == REG_ZR) {
    op(c, WASM_OP_DROP);
    return;
  }
  c->known_mask &= ~(1u << r);
  c->predicted_mask &= ~(1u << r);
  c->used |= (uint64_t)1 << r;
  c->written |= (uint64_t)1 << r;
  lset(c, L_X0 + r);
}
static void set_xsp(Ctx *c, uint32_t r) {
  if (r != REG_ZR) {
    set_x(c, r);
    return;
  }
  c->used |= MASK_SP;
  c->written |= MASK_SP;
  lset(c, L_SP);
}
static void flags_observed(Ctx *c) {
  if (c->flag_event == FLAG_EVENT_NONE) c->flag_event = FLAG_EVENT_READ;
}
static void flags_written(Ctx *c) {
  if (c->flag_event == FLAG_EVENT_NONE) c->flag_event = FLAG_EVENT_WRITE;
  c->used |= MASK_NZCV;
  c->written |= MASK_NZCV;
}
static void emit_materialize(Ctx *c);
static void materialize(Ctx *c) {
  emit_materialize(c);
  c->flag_kind = FLAGS_LIVE;
}
/* NZCV as an i32 (materialized first if lazy). */
static void load_flags(Ctx *c) {
  flags_observed(c);
  if (c->flag_kind != FLAGS_LIVE) materialize(c);
  c->used |= MASK_NZCV;
  lget(c, L_NZCV);
}
static void store_flags(Ctx *c) {
  flags_written(c);
  c->flag_kind = FLAGS_LIVE;
  lset(c, L_NZCV);
}

/* ------------------------------------------------------------------ */
/* Spill / reload / exits.                                             */
/* ------------------------------------------------------------------ */

static void spill(Ctx *c) {
  for (uint32_t r = 0; r < REG_ZR; r++) {
    if (c->all_written & ((uint64_t)1 << r)) state_store64_local(c, OFF_X(r), L_X0 + r);
  }
  if (c->all_written & MASK_SP) state_store64_local(c, OFF_SP, L_SP);
  if (c->all_written & MASK_NZCV) {
    lget(c, L_STATE);
    lget(c, L_NZCV);
    mem(c, WASM_OP_I32_STORE, ALIGN_4, OFF_PSTATE);
  }
}

static void reload(Ctx *c) {
  for (uint32_t r = 0; r < REG_ZR; r++) {
    if (c->all_used & ((uint64_t)1 << r)) {
      state_load64(c, OFF_X(r));
      lset(c, L_X0 + r);
    }
  }
  if (c->all_used & MASK_SP) {
    state_load64(c, OFF_SP);
    lset(c, L_SP);
  }
  if (c->all_used & MASK_NZCV) {
    lget(c, L_STATE);
    mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_PSTATE);
    lset(c, L_NZCV);
  }
}

/* The state's cycle counters += L_CYC - rest (the value on the stack):
 * pre-counted instructions that have not run are taken back out. */
static void flush_cycles(Ctx *c) {
  lset(c, L_T0);
  lget(c, L_CYC);
  lget(c, L_T0);
  op(c, WASM_OP_I64_SUB);
  lset(c, L_T0);
  static const uint64_t offsets[2] = {OFF_CYCLES, OFF_TOTAL_CYCLES};
  for (uint32_t i = 0; i < 2u; i++) {
    lget(c, L_STATE);
    state_load64(c, offsets[i]);
    lget(c, L_T0);
    op(c, WASM_OP_I64_ADD);
    mem(c, WASM_OP_I64_STORE, ALIGN_8, offsets[i]);
  }
}

/* L_ROOM from the state. */
static void compute_room(Ctx *c) {
  state_load64(c, OFF_BUDGET);
  state_load64(c, OFF_CYCLES);
  op(c, WASM_OP_I64_SUB);
  lset(c, L_ROOM);
}

/* regs.pc = L_HPC, then the interpreter runs L_HINSN; i32 status on the
 * stack. The state must be spilled and its cycles flushed. */
static void call_interpreter(Ctx *c) {
  state_store64_local(c, OFF_PC, L_HPC);
  lget(c, L_STATE);
  lget(c, L_HINSN);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_INTERPRET);
}

/* This instruction through the interpreter, then return to the
 * dispatcher (the shared $leave handler): terminating instructions, and
 * accesses the inline softmmu and the memory helpers cannot complete -
 * there the interpreter raises the fault exactly as it would have. */
static void leave_via_interpreter(Ctx *c) {
  flags_observed(c);
  emit_materialize(c);
  i64c(c, c->pc);
  lset(c, L_HPC);
  i32c(c, c->insn);
  lset(c, L_HINSN);
  i64c(c, c->block_len - c->index);
  lset(c, L_HREST);
  br(c, LEVEL_LEAVE);
}

/* Leave the function for the PC on the stack (computed targets, calls). */
static void exit_to_stack(Ctx *c) {
  lset(c, L_NPC);
  flags_observed(c);
  emit_materialize(c);
  br(c, LEVEL_EXIT);
}

/* The page table index of `page`, adding it if there is room and its code
 * may be compiled; -1 otherwise. */
static int32_t region_page(Ctx *c, uint64_t page) {
  for (uint32_t i = 0; i < c->page_count; i++) {
    if (c->pages[i].base == page) return (int32_t)i;
  }
  if (!c->discover || c->page_count >= JIT_MAX_REGION_PAGES) return -1;
  const uint32_t *code = c->source->page_code(c->source->context, page);
  if (!code) return -1;
  c->pages[c->page_count].base = page;
  c->pages[c->page_count].code = code;
  return (int32_t)c->page_count++;
}

static int32_t find_block(const Ctx *c, uint64_t pc) {
  for (uint32_t i = 0; i < c->block_count; i++) {
    if (c->blocks[i].pc == pc) return (int32_t)i;
  }
  return -1;
}

/* Adds `pc` as a region block if it can be one; true if it is one. */
static bool add_block(Ctx *c, uint64_t pc) {
  if (pc & 3u) return false;
  if (find_block(c, pc) >= 0) return true;
  if (c->block_count >= c->max_blocks) return false;
  if (region_page(c, pc & ~(uint64_t)VMM_PAGE_OFFSET_MASK) < 0) return false;
  c->blocks[c->block_count].pc = pc;
  c->blocks[c->block_count++].length = 0;
  return true;
}

/* An instruction the compiler does not inline, mid-block: it ends this
 * region block and the shared $helper runs it, then the region goes on
 * with the block after it (if that can be a region block; otherwise
 * through $leave). */
static void helper_and_continue(Ctx *c) {
  const uint64_t next = c->pc + INSN_BYTES;
  if (c->discover) {
    if (add_block(c, next)) c->uses_helper = true;
  }
  const int32_t m = find_block(c, next);
  if (m < 0) {
    leave_via_interpreter(c);
    return;
  }
  flags_observed(c);
  emit_materialize(c);
  i64c(c, c->pc);
  lset(c, L_HPC);
  i32c(c, c->insn);
  lset(c, L_HINSN);
  i32c(c, (uint32_t)m);
  lset(c, L_RESUME);
  i64c(c, c->blocks[m].length);
  lset(c, L_HNEXTLEN);
  i64c(c, next);
  lset(c, L_HNEXT);
  i32c(c, c->block_count); /* $helper's dispatch index */
  lset(c, L_IDX);
  br(c, LEVEL_DISPATCH);
}

/* Control goes to `target`, a direct branch: within the region if it is
 * a region block and its instructions fit the budget, else out. */
static void branch_to(Ctx *c, uint64_t target) {
  if (c->discover) {
    (void)add_block(c, target);
    flags_observed(c); /* conservatively: the target may read them */
  }
  const int32_t m = c->discover ? -1 : find_block(c, target);
  if (m < 0) {
    i64c(c, target);
    exit_to_stack(c);
    return;
  }
  const uint32_t length = c->blocks[m].length;
  lget(c, L_CYC);
  i64c(c, length);
  op(c, WASM_OP_I64_ADD);
  ltee(c, L_T0);
  lget(c, L_ROOM);
  op(c, WASM_OP_I64_GT_U);
  open_if(c, WASM_BLOCK_VOID); /* does not fit: out, the dispatcher interprets it */
  i64c(c, target);
  exit_to_stack(c);
  end_(c);
  lget(c, L_T0);
  lset(c, L_CYC);
  if (c->block_count > 1u) {
    i32c(c, (uint32_t)m);
    lset(c, L_IDX);
  }
  if (c->blocks[m].needs_flags) emit_materialize(c);
  br(c, LEVEL_DISPATCH);
}

/* ------------------------------------------------------------------ */
/* Flags and conditions.                                               */
/* ------------------------------------------------------------------ */

#define NZCV_SHIFT_N 31u
#define NZCV_SHIFT_Z 30u
#define NZCV_SHIFT_C 29u
#define NZCV_SHIFT_V 28u

static uint64_t msb_of(bool sf) { return sf ? 63u : 31u; }

/* (L_FR >> msb) as an i32: N. */
static void emit_flag_n(Ctx *c) {
  lget(c, L_FR);
  i64c(c, msb_of(c->flag_sf));
  op(c, WASM_OP_I64_SHR_U);
  op(c, WASM_OP_I32_WRAP_I64);
}
/* ((a ^ r) & (y ^ r)) >> msb as an i32: V of AddWithCarry. */
static void emit_flag_v(Ctx *c) {
  lget(c, L_FA);
  lget(c, L_FR);
  op(c, WASM_OP_I64_XOR);
  lget(c, L_FB);
  lget(c, L_FR);
  op(c, WASM_OP_I64_XOR);
  op(c, WASM_OP_I64_AND);
  i64c(c, msb_of(c->flag_sf));
  op(c, WASM_OP_I64_SHR_U);
  op(c, WASM_OP_I32_WRAP_I64);
}
/* C of AddWithCarry(L_FA, L_FB, flag_carry) as an i32. */
static void emit_flag_c(Ctx *c) {
  if (!c->flag_sf) { /* the 33-bit sum's bit 32 */
    lget(c, L_FA);
    lget(c, L_FB);
    op(c, WASM_OP_I64_ADD);
    if (c->flag_carry == CARRY_ONE) {
      i64c(c, 1);
      op(c, WASM_OP_I64_ADD);
    }
    i64c(c, 32);
    op(c, WASM_OP_I64_SHR_U);
    op(c, WASM_OP_I32_WRAP_I64);
    return;
  }
  lget(c, L_FR);
  lget(c, L_FA);
  op(c, c->flag_carry == CARRY_ONE ? WASM_OP_I64_LE_U : WASM_OP_I64_LT_U);
}

/* L_NZCV from the lazy flags (no change to the compile-time state, so it
 * can sit on one arm of a branch). */
static void emit_materialize(Ctx *c) {
  if (c->flag_kind == FLAGS_LIVE) return;
  emit_flag_n(c);
  i32c(c, NZCV_SHIFT_N);
  op(c, WASM_OP_I32_SHL);
  lget(c, L_FR);
  op(c, WASM_OP_I64_EQZ);
  i32c(c, NZCV_SHIFT_Z);
  op(c, WASM_OP_I32_SHL);
  op(c, WASM_OP_I32_OR);
  if (c->flag_kind == FLAGS_ADD) {
    emit_flag_c(c);
    i32c(c, NZCV_SHIFT_C);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
    emit_flag_v(c);
    i32c(c, NZCV_SHIFT_V);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
  }
  lset(c, L_NZCV);
}

typedef enum Flags_Set {
  FLAGS_UNCHANGED,
  FLAGS_SET_LAZY,  /* record the operands (the usual case) */
  FLAGS_SET_EAGER, /* build NZCV now (on one arm of a branch; carry-in from NZCV) */
} Flags_Set;

/* AddWithCarry(T0, T1, carry): T0 and T1 hold width-masked operands; the
 * width-masked result goes to T2. */
static void emit_add_with_carry(Ctx *c, bool sf, Carry carry, Flags_Set flags) {
  lget(c, L_T0);
  lget(c, L_T1);
  op(c, WASM_OP_I64_ADD);
  if (carry == CARRY_ONE) {
    i64c(c, 1);
    op(c, WASM_OP_I64_ADD);
  } else if (carry == CARRY_FLAG) {
    load_flags(c);
    i32c(c, NZCV_SHIFT_C);
    op(c, WASM_OP_I32_SHR_U);
    i32c(c, 1);
    op(c, WASM_OP_I32_AND);
    op(c, WASM_OP_I64_EXTEND_I32_U);
    op(c, WASM_OP_I64_ADD);
  }
  if (sf) {
    lset(c, L_T2);
  } else {
    ltee(c, L_T3); /* the 33-bit sum: bit 32 is the carry */
    mask32(c);
    lset(c, L_T2);
  }
  if (flags == FLAGS_UNCHANGED) return;
  if (carry == CARRY_FLAG) {
    /* ADCS/SBCS: C = carry out of a + y + C_in, which the lazy form
     * cannot recompute after NZCV changes - build NZCV here. */
    const uint64_t msb = msb_of(sf);
    if (!sf) {
      lget(c, L_T3);
      i64c(c, 32);
      op(c, WASM_OP_I64_SHR_U);
      op(c, WASM_OP_I32_WRAP_I64);
    } else {
      lget(c, L_T2);
      lget(c, L_T0);
      op(c, WASM_OP_I64_LT_U);
      load_flags(c);
      i32c(c, NZCV_SHIFT_C);
      op(c, WASM_OP_I32_SHR_U);
      lget(c, L_T2);
      lget(c, L_T0);
      op(c, WASM_OP_I64_EQ);
      op(c, WASM_OP_I32_AND);
      op(c, WASM_OP_I32_OR);
      i32c(c, 1);
      op(c, WASM_OP_I32_AND);
    }
    i32c(c, NZCV_SHIFT_C);
    op(c, WASM_OP_I32_SHL);
    lget(c, L_T0);
    lget(c, L_T2);
    op(c, WASM_OP_I64_XOR);
    lget(c, L_T1);
    lget(c, L_T2);
    op(c, WASM_OP_I64_XOR);
    op(c, WASM_OP_I64_AND);
    i64c(c, msb);
    op(c, WASM_OP_I64_SHR_U);
    op(c, WASM_OP_I32_WRAP_I64);
    i32c(c, NZCV_SHIFT_V);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
    lget(c, L_T2);
    i64c(c, msb);
    op(c, WASM_OP_I64_SHR_U);
    op(c, WASM_OP_I32_WRAP_I64);
    i32c(c, NZCV_SHIFT_N);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
    lget(c, L_T2);
    op(c, WASM_OP_I64_EQZ);
    i32c(c, NZCV_SHIFT_Z);
    op(c, WASM_OP_I32_SHL);
    op(c, WASM_OP_I32_OR);
    store_flags(c);
    return;
  }
  lget(c, L_T0);
  lset(c, L_FA);
  lget(c, L_T1);
  lset(c, L_FB);
  lget(c, L_T2);
  lset(c, L_FR);
  flags_written(c);
  c->flag_kind = FLAGS_ADD;
  c->flag_sf = sf;
  c->flag_carry = carry;
  if (flags == FLAGS_SET_EAGER) materialize(c);
}

/* Logical-op flags from the width-masked result in T2: N, Z; C = V = 0. */
static void set_logic_flags(Ctx *c, bool sf) {
  lget(c, L_T2);
  lset(c, L_FR);
  flags_written(c);
  c->flag_kind = FLAGS_LOGIC;
  c->flag_sf = sf;
}

/* NZCV-based ConditionHolds(cond) as a truthy i32, before inversion. */
static void emit_condition_nzcv(Ctx *c, uint32_t cond) {
  switch (cond >> 1) {
  case 0: /* EQ: Z */
    load_flags(c);
    i32c(c, CPU_PSTATE_Z);
    op(c, WASM_OP_I32_AND);
    break;
  case 1: /* CS: C */
    load_flags(c);
    i32c(c, CPU_PSTATE_C);
    op(c, WASM_OP_I32_AND);
    break;
  case 2: /* MI: N */
    load_flags(c);
    i32c(c, CPU_PSTATE_N);
    op(c, WASM_OP_I32_AND);
    break;
  case 3: /* VS: V */
    load_flags(c);
    i32c(c, CPU_PSTATE_V);
    op(c, WASM_OP_I32_AND);
    break;
  case 4: /* HI: C && !Z */
    load_flags(c);
    i32c(c, CPU_PSTATE_C | CPU_PSTATE_Z);
    op(c, WASM_OP_I32_AND);
    i32c(c, CPU_PSTATE_C);
    op(c, WASM_OP_I32_EQ);
    break;
  default: /* GE: N == V, i.e. ((nzcv << 3) ^ nzcv) has bit 31 clear; GT: also !Z */
    load_flags(c);
    i32c(c, NZCV_SHIFT_N - NZCV_SHIFT_V);
    op(c, WASM_OP_I32_SHL);
    load_flags(c);
    op(c, WASM_OP_I32_XOR);
    i32c(c, CPU_PSTATE_N);
    op(c, WASM_OP_I32_AND);
    if ((cond >> 1) == 6) {
      load_flags(c);
      i32c(c, CPU_PSTATE_Z);
      op(c, WASM_OP_I32_AND);
      op(c, WASM_OP_I32_OR);
    }
    op(c, WASM_OP_I32_EQZ);
    break;
  }
}

/* L_FA, then b = ~L_FB (the subtrahend), signed in the width if `sign`. */
static void emit_sub_operands(Ctx *c, bool sign) {
  lget(c, L_FA);
  if (sign && !c->flag_sf) op(c, WASM_OP_I64_EXTEND32_S);
  lget(c, L_FB);
  i64c(c, width_mask_of(c->flag_sf));
  op(c, WASM_OP_I64_XOR);
  if (sign && !c->flag_sf) op(c, WASM_OP_I64_EXTEND32_S);
}

/* L_FR as a signed value in the width. */
static void emit_result_signed(Ctx *c) {
  lget(c, L_FR);
  if (!c->flag_sf) op(c, WASM_OP_I64_EXTEND32_S);
}

/* ConditionHolds(cond) straight from lazy SUBS/CMP operands; false if
 * this kind cannot. Before inversion. */
static bool emit_condition_fused(Ctx *c, uint32_t cond) {
  const uint32_t base = cond >> 1;
  if (c->flag_kind == FLAGS_ADD && c->flag_carry == CARRY_ONE) { /* SUBS: a - b */
    switch (base) {
    case 0: lget(c, L_FR); op(c, WASM_OP_I64_EQZ); return true;                      /* EQ */
    case 1: emit_sub_operands(c, false); op(c, WASM_OP_I64_GE_U); return true;       /* CS: a >= b */
    case 2: emit_flag_n(c); return true;                                              /* MI */
    case 3: emit_flag_v(c); return true;                                              /* VS */
    case 4: emit_sub_operands(c, false); op(c, WASM_OP_I64_GT_U); return true;       /* HI: a > b */
    case 5: emit_sub_operands(c, true); op(c, WASM_OP_I64_GE_S); return true;        /* GE */
    default: emit_sub_operands(c, true); op(c, WASM_OP_I64_GT_S); return true;       /* GT */
    }
  }
  if (c->flag_kind == FLAGS_LOGIC) { /* C = V = 0 */
    switch (base) {
    case 0: lget(c, L_FR); op(c, WASM_OP_I64_EQZ); return true; /* EQ */
    case 1: case 3: case 4: i32c(c, 0); return true;            /* CS, VS, HI */
    case 2: emit_flag_n(c); return true;                         /* MI */
    case 5: emit_flag_n(c); op(c, WASM_OP_I32_EQZ); return true; /* GE: !N */
    default: emit_result_signed(c); i64c(c, 0); op(c, WASM_OP_I64_GT_S); return true; /* GT: r > 0 */
    }
  }
  if (c->flag_kind == FLAGS_ADD) { /* ADDS/CMN: only the result's own flags */
    switch (base) {
    case 0: lget(c, L_FR); op(c, WASM_OP_I64_EQZ); return true;
    case 2: emit_flag_n(c); return true;
    default: return false;
    }
  }
  return false;
}

/* ConditionHolds(cond) as a truthy i32 (non-zero = holds). */
static void emit_condition(Ctx *c, uint32_t cond) {
  if ((cond >> 1) == 7) { /* AL, NV */
    i32c(c, 1);
    return;
  }
  if (!emit_condition_fused(c, cond)) emit_condition_nzcv(c, cond);
  if (cond & 1u) op(c, WASM_OP_I32_EQZ);
}

/* ------------------------------------------------------------------ */
/* Shifts and extends.                                                 */
/* ------------------------------------------------------------------ */

/* ROR of the width-masked value in `local` by a constant. */
static void emit_ror_local(Ctx *c, uint32_t local, uint32_t amount, bool sf) {
  lget(c, local);
  if (amount == 0) return;
  if (sf) {
    i64c(c, amount);
    op(c, WASM_OP_I64_ROTR);
  } else {
    op(c, WASM_OP_I32_WRAP_I64);
    i32c(c, amount);
    op(c, WASM_OP_I32_ROTR);
    op(c, WASM_OP_I64_EXTEND_I32_U);
  }
}

/* ShiftReg(r, type, amount) for a constant amount, width-masked. */
static void emit_shift_reg(Ctx *c, uint32_t r, uint32_t type, uint32_t amount, bool sf) {
  get_xw(c, r, sf);
  if (amount == 0) return;
  switch (type) {
  case 0: /* LSL */
    i64c(c, amount);
    op(c, WASM_OP_I64_SHL);
    width(c, sf);
    break;
  case 1: /* LSR */
    i64c(c, amount);
    op(c, WASM_OP_I64_SHR_U);
    break;
  case 2: /* ASR */
    if (!sf) op(c, WASM_OP_I64_EXTEND32_S);
    i64c(c, amount);
    op(c, WASM_OP_I64_SHR_S);
    width(c, sf);
    break;
  default: /* ROR */
    lset(c, L_T3);
    emit_ror_local(c, L_T3, amount, sf);
    break;
  }
}

/* ExtendReg's extension step (before the shift). */
static void emit_extend(Ctx *c, uint32_t option) {
  switch (option) {
  case 0: i64c(c, 0xFFu); op(c, WASM_OP_I64_AND); break;
  case 1: i64c(c, 0xFFFFu); op(c, WASM_OP_I64_AND); break;
  case 2: mask32(c); break;
  case 4: op(c, WASM_OP_I64_EXTEND8_S); break;
  case 5: op(c, WASM_OP_I64_EXTEND16_S); break;
  case 6: op(c, WASM_OP_I64_EXTEND32_S); break;
  default: break; /* UXTX, SXTX */
  }
}

/* ------------------------------------------------------------------ */
/* Data processing - immediate.                                        */
/* ------------------------------------------------------------------ */

static void know(Ctx *c, uint32_t r, uint64_t value) {
  if (r == REG_ZR) return;
  c->known_mask |= 1u << r;
  c->known_value[r] = value;
}

static bool c_pc_relative(Ctx *c, uint32_t insn) {
  const uint64_t imm = (uint64_t)sign_extend((bits(insn, 23, 5) << 2) | bits(insn, 30, 29), 21);
  const uint64_t value = bit(insn, 31) ? (c->pc & ~(uint64_t)VMM_PAGE_OFFSET_MASK) + (imm << 12) : c->pc + imm;
  i64c(c, value);
  set_x(c, bits(insn, 4, 0));
  know(c, bits(insn, 4, 0), value);
  return true;
}

static bool c_add_sub_immediate(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  const uint64_t imm = (uint64_t)bits(insn, 21, 10) << (bit(insn, 22) ? 12 : 0);
  if (!set_flags) {
    get_xsp(c, rn);
    i64c(c, imm);
    op(c, sub ? WASM_OP_I64_SUB : WASM_OP_I64_ADD);
    width(c, sf);
    const bool known = sf && rn != REG_ZR && (c->known_mask & (1u << rn));
    const uint64_t value = known ? (sub ? c->known_value[rn] - imm : c->known_value[rn] + imm) : 0;
    set_xsp(c, rd);
    if (known) know(c, rd, value);
    return true;
  }
  get_xsp(c, rn);
  width(c, sf);
  lset(c, L_T0);
  i64c(c, sub ? (~imm & width_mask_of(sf)) : imm);
  lset(c, L_T1);
  emit_add_with_carry(c, sf, sub ? CARRY_ONE : CARRY_ZERO, FLAGS_SET_LAZY);
  lget(c, L_T2);
  set_x(c, rd);
  return true;
}

static bool c_logical_immediate(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), n = bit(insn, 22);
  if (!sf && n) return false;
  uint64_t imm = 0, unused = 0;
  if (!interp_decode_bit_masks(n, bits(insn, 15, 10), bits(insn, 21, 16), true, sf, &imm, &unused)) return false;
  static const uint8_t ops[4] = {WASM_OP_I64_AND, WASM_OP_I64_OR, WASM_OP_I64_XOR, WASM_OP_I64_AND};
  get_x(c, bits(insn, 9, 5));
  i64c(c, imm);
  op(c, ops[opc]);
  width(c, sf);
  lset(c, L_T2);
  if (opc == 3) set_logic_flags(c, sf);
  lget(c, L_T2);
  if (opc == 3) set_x(c, bits(insn, 4, 0));
  else set_xsp(c, bits(insn, 4, 0));
  return true;
}

static bool c_move_wide(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), hw = bits(insn, 22, 21), rd = bits(insn, 4, 0);
  if (opc == 1 || (!sf && hw >= 2)) return false;
  const unsigned shift = hw * 16u;
  const uint64_t imm = (uint64_t)bits(insn, 20, 5) << shift;
  if (opc == 3) { /* MOVK */
    get_x(c, rd);
    i64c(c, ~((uint64_t)0xFFFF << shift));
    op(c, WASM_OP_I64_AND);
    i64c(c, imm);
    op(c, WASM_OP_I64_OR);
    width(c, sf);
  } else {
    i64c(c, (opc == 0 ? ~imm : imm) & width_mask_of(sf));
  }
  set_x(c, rd);
  return true;
}

static bool c_bitfield(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t opc = bits(insn, 30, 29), n = bit(insn, 22);
  const uint32_t immr = bits(insn, 21, 16), imms = bits(insn, 15, 10);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (opc == 3 || n != (uint32_t)sf) return false;
  if (!sf && ((immr | imms) & 0x20u)) return false;
  uint64_t wmask = 0, tmask = 0;
  if (!interp_decode_bit_masks(n, imms, immr, false, sf, &wmask, &tmask)) return false;
  const bool inzero = opc != 1, extend = opc == 0;
  get_xw(c, rn, sf);
  lset(c, L_T0); /* src */
  /* bot = (dst & ~wmask) | (ROR(src, immr) & wmask) */
  emit_ror_local(c, L_T0, immr, sf);
  i64c(c, wmask);
  op(c, WASM_OP_I64_AND);
  if (!inzero) {
    get_x(c, rd);
    i64c(c, ~wmask);
    op(c, WASM_OP_I64_AND);
    op(c, WASM_OP_I64_OR);
  }
  lset(c, L_T1);
  /* top */
  if (extend) {
    i64c(c, 0);
    lget(c, L_T0);
    i64c(c, imms);
    op(c, WASM_OP_I64_SHR_U);
    i64c(c, 1);
    op(c, WASM_OP_I64_AND);
    op(c, WASM_OP_I64_SUB);
    width(c, sf);
  } else if (inzero) {
    i64c(c, 0);
  } else {
    get_x(c, rd);
  }
  i64c(c, ~tmask);
  op(c, WASM_OP_I64_AND);
  lget(c, L_T1);
  i64c(c, tmask);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_OR);
  width(c, sf);
  set_x(c, rd);
  return true;
}

static bool c_extract(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31);
  const uint32_t op21 = bits(insn, 30, 29), n = bit(insn, 22), o0 = bit(insn, 21);
  const uint32_t lsb = bits(insn, 15, 10);
  if (op21 != 0 || o0 != 0 || n != (uint32_t)sf || (!sf && lsb >= 32u)) return false;
  get_xw(c, bits(insn, 20, 16), sf);
  if (lsb != 0) {
    i64c(c, lsb);
    op(c, WASM_OP_I64_SHR_U);
    get_xw(c, bits(insn, 9, 5), sf);
    i64c(c, (sf ? 64u : 32u) - lsb);
    op(c, WASM_OP_I64_SHL);
    op(c, WASM_OP_I64_OR);
    width(c, sf);
  }
  set_x(c, bits(insn, 4, 0));
  return true;
}

static bool c_dp_immediate(Ctx *c, uint32_t insn) {
  switch (bits(insn, 25, 23)) {
  case 0: case 1: return c_pc_relative(c, insn);
  case 2: return c_add_sub_immediate(c, insn);
  case 4: return c_logical_immediate(c, insn);
  case 5: return c_move_wide(c, insn);
  case 6: return c_bitfield(c, insn);
  case 7: return c_extract(c, insn);
  default: return false;
  }
}

/* ------------------------------------------------------------------ */
/* Data processing - register.                                         */
/* ------------------------------------------------------------------ */

static bool c_logical_shifted(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31), invert = bit(insn, 21);
  const uint32_t opc = bits(insn, 30, 29), amount = bits(insn, 15, 10);
  if (!sf && amount >= 32u) return false;
  static const uint8_t ops[4] = {WASM_OP_I64_AND, WASM_OP_I64_OR, WASM_OP_I64_XOR, WASM_OP_I64_AND};
  get_xw(c, bits(insn, 9, 5), sf);
  emit_shift_reg(c, bits(insn, 20, 16), bits(insn, 23, 22), amount, sf);
  if (invert) {
    i64c(c, width_mask_of(sf));
    op(c, WASM_OP_I64_XOR);
  }
  op(c, ops[opc]);
  lset(c, L_T2);
  if (opc == 3) set_logic_flags(c, sf);
  lget(c, L_T2);
  set_x(c, bits(insn, 4, 0));
  return true;
}

static bool c_add_sub_shifted(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t type = bits(insn, 23, 22), amount = bits(insn, 15, 10);
  if (type == 3u || (!sf && amount >= 32u)) return false;
  get_xw(c, bits(insn, 9, 5), sf);
  lset(c, L_T0);
  emit_shift_reg(c, bits(insn, 20, 16), type, amount, sf);
  if (sub) {
    i64c(c, width_mask_of(sf));
    op(c, WASM_OP_I64_XOR);
  }
  lset(c, L_T1);
  emit_add_with_carry(c, sf, sub ? CARRY_ONE : CARRY_ZERO, set_flags ? FLAGS_SET_LAZY : FLAGS_UNCHANGED);
  lget(c, L_T2);
  set_x(c, bits(insn, 4, 0));
  return true;
}

static bool c_add_sub_extended(Ctx *c, uint32_t insn) {
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  const uint32_t shift = bits(insn, 12, 10);
  if (bits(insn, 23, 22) != 0 || shift > 4u) return false;
  get_xsp(c, bits(insn, 9, 5));
  width(c, sf);
  lset(c, L_T0);
  get_x(c, bits(insn, 20, 16));
  emit_extend(c, bits(insn, 15, 13));
  if (shift) {
    i64c(c, shift);
    op(c, WASM_OP_I64_SHL);
  }
  width(c, sf);
  if (sub) {
    i64c(c, width_mask_of(sf));
    op(c, WASM_OP_I64_XOR);
  }
  lset(c, L_T1);
  emit_add_with_carry(c, sf, sub ? CARRY_ONE : CARRY_ZERO, set_flags ? FLAGS_SET_LAZY : FLAGS_UNCHANGED);
  lget(c, L_T2);
  if (set_flags) set_x(c, bits(insn, 4, 0));
  else set_xsp(c, bits(insn, 4, 0));
  return true;
}

static bool c_add_sub_carry(Ctx *c, uint32_t insn) {
  if (bits(insn, 15, 10) != 0) return false;
  const bool sf = bit(insn, 31), sub = bit(insn, 30), set_flags = bit(insn, 29);
  get_xw(c, bits(insn, 9, 5), sf);
  lset(c, L_T0);
  get_xw(c, bits(insn, 20, 16), sf);
  if (sub) {
    i64c(c, width_mask_of(sf));
    op(c, WASM_OP_I64_XOR);
  }
  lset(c, L_T1);
  emit_add_with_carry(c, sf, CARRY_FLAG, set_flags ? FLAGS_SET_EAGER : FLAGS_UNCHANGED);
  lget(c, L_T2);
  set_x(c, bits(insn, 4, 0));
  return true;
}

static bool c_conditional_compare(Ctx *c, uint32_t insn) {
  if (!bit(insn, 29) || bit(insn, 10) || bit(insn, 4)) return false;
  const bool sf = bit(insn, 31), sub = bit(insn, 30), immediate = bit(insn, 11);
  emit_condition(c, bits(insn, 15, 12));
  open_if(c, WASM_BLOCK_VOID);
  get_xw(c, bits(insn, 9, 5), sf);
  lset(c, L_T0);
  if (immediate) i64c(c, bits(insn, 20, 16));
  else get_xw(c, bits(insn, 20, 16), sf);
  if (sub) {
    i64c(c, width_mask_of(sf));
    op(c, WASM_OP_I64_XOR);
  }
  lset(c, L_T1);
  emit_add_with_carry(c, sf, sub ? CARRY_ONE : CARRY_ZERO, FLAGS_SET_EAGER);
  else_(c);
  i32c(c, bits(insn, 3, 0) << NZCV_SHIFT_V);
  store_flags(c);
  end_(c);
  return true;
}

static bool c_conditional_select(Ctx *c, uint32_t insn) {
  if (bit(insn, 29) || bit(insn, 11)) return false;
  const bool sf = bit(insn, 31), else_inv = bit(insn, 30), else_inc = bit(insn, 10);
  get_x(c, bits(insn, 9, 5));
  get_x(c, bits(insn, 20, 16));
  if (else_inv) {
    i64c(c, ~(uint64_t)0);
    op(c, WASM_OP_I64_XOR);
  }
  if (else_inc) {
    i64c(c, 1);
    op(c, WASM_OP_I64_ADD);
  }
  emit_condition(c, bits(insn, 15, 12));
  op(c, WASM_OP_SELECT);
  width(c, sf);
  set_x(c, bits(insn, 4, 0));
  return true;
}

static void shr32(Ctx *c) {
  i64c(c, 32);
  op(c, WASM_OP_I64_SHR_U);
}

/* High 64 bits of T0 * T1 (unsigned, or signed when `is_signed`). */
static void emit_multiply_high(Ctx *c, bool is_signed) {
  lget(c, L_T0);
  mask32(c);
  lget(c, L_T1);
  mask32(c);
  op(c, WASM_OP_I64_MUL);
  shr32(c); /* p0 >> 32 */
  lget(c, L_T0);
  mask32(c);
  lget(c, L_T1);
  shr32(c);
  op(c, WASM_OP_I64_MUL);
  ltee(c, L_T2); /* p1 */
  mask32(c);
  op(c, WASM_OP_I64_ADD);
  lget(c, L_T0);
  shr32(c);
  lget(c, L_T1);
  mask32(c);
  op(c, WASM_OP_I64_MUL);
  ltee(c, L_T3); /* p2 */
  mask32(c);
  op(c, WASM_OP_I64_ADD); /* middle */
  shr32(c);
  lget(c, L_T0);
  shr32(c);
  lget(c, L_T1);
  shr32(c);
  op(c, WASM_OP_I64_MUL); /* p3 */
  op(c, WASM_OP_I64_ADD);
  lget(c, L_T2);
  shr32(c);
  op(c, WASM_OP_I64_ADD);
  lget(c, L_T3);
  shr32(c);
  op(c, WASM_OP_I64_ADD);
  if (!is_signed) return;
  /* hi -= (a < 0 ? b : 0) + (b < 0 ? a : 0) */
  lget(c, L_T1);
  lget(c, L_T0);
  i64c(c, 63);
  op(c, WASM_OP_I64_SHR_S);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_SUB);
  lget(c, L_T0);
  lget(c, L_T1);
  i64c(c, 63);
  op(c, WASM_OP_I64_SHR_S);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_SUB);
}

static bool c_dp_three_source(Ctx *c, uint32_t insn) {
  if (bits(insn, 30, 29) != 0) return false;
  const bool sf = bit(insn, 31), subtract = bit(insn, 15);
  const uint32_t op31 = bits(insn, 23, 21);
  const uint32_t rn = bits(insn, 9, 5), rm = bits(insn, 20, 16), ra = bits(insn, 14, 10), rd = bits(insn, 4, 0);
  if (op31 == 0) { /* MADD / MSUB */
    get_x(c, ra);
    get_xw(c, rn, sf);
    get_xw(c, rm, sf);
    op(c, WASM_OP_I64_MUL);
    op(c, subtract ? WASM_OP_I64_SUB : WASM_OP_I64_ADD);
    width(c, sf);
    set_x(c, rd);
    return true;
  }
  if (!sf) return false;
  switch (op31) {
  case 1: case 5: /* SMADDL/SMSUBL, UMADDL/UMSUBL */
    get_x(c, ra);
    get_x(c, rn);
    if (op31 == 1) op(c, WASM_OP_I64_EXTEND32_S);
    else mask32(c);
    get_x(c, rm);
    if (op31 == 1) op(c, WASM_OP_I64_EXTEND32_S);
    else mask32(c);
    op(c, WASM_OP_I64_MUL);
    op(c, subtract ? WASM_OP_I64_SUB : WASM_OP_I64_ADD);
    set_x(c, rd);
    return true;
  case 2: case 6: /* SMULH, UMULH */
    if (subtract) return false;
    get_x(c, rn);
    lset(c, L_T0);
    get_x(c, rm);
    lset(c, L_T1);
    emit_multiply_high(c, op31 == 2);
    set_x(c, rd);
    return true;
  default:
    return false;
  }
}

static bool c_dp_two_source(Ctx *c, uint32_t insn) {
  if (bit(insn, 29)) return false;
  const bool sf = bit(insn, 31);
  const uint32_t opcode = bits(insn, 15, 10);
  get_xw(c, bits(insn, 9, 5), sf);
  lset(c, L_T0);
  get_xw(c, bits(insn, 20, 16), sf);
  lset(c, L_T1);
  switch (opcode) {
  case 0x02: /* UDIV: x / 0 = 0 */
    lget(c, L_T1);
    op(c, WASM_OP_I64_EQZ);
    open_if(c, WASM_TYPE_I64);
    i64c(c, 0);
    else_(c);
    lget(c, L_T0);
    lget(c, L_T1);
    op(c, WASM_OP_I64_DIV_U);
    end_(c);
    break;
  case 0x03: /* SDIV: x / 0 = 0, INT_MIN / -1 = INT_MIN */
    if (!sf) { /* the 32-bit quotient can't overflow in 64 bits */
      lget(c, L_T0);
      op(c, WASM_OP_I64_EXTEND32_S);
      lset(c, L_T0);
      lget(c, L_T1);
      op(c, WASM_OP_I64_EXTEND32_S);
      lset(c, L_T1);
    }
    lget(c, L_T1);
    op(c, WASM_OP_I64_EQZ);
    open_if(c, WASM_TYPE_I64);
    i64c(c, 0);
    else_(c);
    lget(c, L_T1);
    i64c(c, ~(uint64_t)0);
    op(c, WASM_OP_I64_EQ);
    open_if(c, WASM_TYPE_I64);
    i64c(c, 0);
    lget(c, L_T0);
    op(c, WASM_OP_I64_SUB);
    else_(c);
    lget(c, L_T0);
    lget(c, L_T1);
    op(c, WASM_OP_I64_DIV_S);
    end_(c);
    end_(c);
    width(c, sf);
    break;
  case 0x08: case 0x09: case 0x0A: case 0x0B: { /* LSLV, LSRV, ASRV, RORV: amount mod width */
    static const uint8_t ops64[4] = {WASM_OP_I64_SHL, WASM_OP_I64_SHR_U, WASM_OP_I64_SHR_S, WASM_OP_I64_ROTR};
    static const uint8_t ops32[4] = {WASM_OP_I32_SHL, WASM_OP_I32_SHR_U, WASM_OP_I32_SHR_S, WASM_OP_I32_ROTR};
    lget(c, L_T0);
    if (sf) {
      lget(c, L_T1);
      op(c, ops64[opcode - 0x08u]);
    } else {
      op(c, WASM_OP_I32_WRAP_I64);
      lget(c, L_T1);
      op(c, WASM_OP_I32_WRAP_I64);
      op(c, ops32[opcode - 0x08u]);
      op(c, WASM_OP_I64_EXTEND_I32_U);
    }
    break;
  }
  default:
    return false; /* CRC32 & co.: the interpreter */
  }
  set_x(c, bits(insn, 4, 0));
  return true;
}

/* T0 = ((T0 >> k) & mask) | ((T0 & mask) << k): swaps adjacent k-bit fields. */
static void emit_swap_fields(Ctx *c, uint32_t k, uint64_t mask) {
  lget(c, L_T0);
  i64c(c, k);
  op(c, WASM_OP_I64_SHR_U);
  i64c(c, mask);
  op(c, WASM_OP_I64_AND);
  lget(c, L_T0);
  i64c(c, mask);
  op(c, WASM_OP_I64_AND);
  i64c(c, k);
  op(c, WASM_OP_I64_SHL);
  op(c, WASM_OP_I64_OR);
  lset(c, L_T0);
}

/* RBIT/REV16/REV32/REV: field swaps from `first_k` up to the width. */
static void emit_reverse(Ctx *c, uint32_t rn, uint32_t rd, bool sf, uint32_t first_k, uint32_t last_k) {
  static const uint64_t masks[6] = {0x5555555555555555ull, 0x3333333333333333ull, 0x0F0F0F0F0F0F0F0Full,
                                    0x00FF00FF00FF00FFull, 0x0000FFFF0000FFFFull, 0x00000000FFFFFFFFull};
  get_xw(c, rn, sf);
  lset(c, L_T0);
  for (uint32_t i = 0, k = 1; i < 6u; i++, k *= 2u) {
    if (k >= first_k && k <= last_k) emit_swap_fields(c, k, masks[i] & width_mask_of(sf));
  }
  lget(c, L_T0);
  set_x(c, rd);
}

static bool c_dp_one_source(Ctx *c, uint32_t insn) {
  if (bit(insn, 29) || bits(insn, 20, 16) != 0) return false;
  const bool sf = bit(insn, 31);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0), width_bits = sf ? 64u : 32u;
  switch (bits(insn, 15, 10)) {
  case 0: emit_reverse(c, rn, rd, sf, 1u, width_bits / 2u); return true;   /* RBIT */
  case 1: emit_reverse(c, rn, rd, sf, 8u, 8u); return true;                 /* REV16 */
  case 2: emit_reverse(c, rn, rd, sf, 8u, 16u); return true;                /* REV32 / REV (W) */
  case 3:                                                                    /* REV (X) */
    if (!sf) return false;
    emit_reverse(c, rn, rd, sf, 8u, 32u);
    return true;
  case 5: /* CLS: CLZ(x ^ (x >> width-1, arithmetic)) - 1 */
    get_x(c, rn);
    if (sf) {
      ltee(c, L_T0);
      lget(c, L_T0);
      i64c(c, 63);
      op(c, WASM_OP_I64_SHR_S);
      op(c, WASM_OP_I64_XOR);
      op(c, WASM_OP_I64_CLZ);
    } else {
      op(c, WASM_OP_I32_WRAP_I64);
      op(c, WASM_OP_I64_EXTEND_I32_U);
      op(c, WASM_OP_I64_EXTEND32_S);
      ltee(c, L_T0);
      lget(c, L_T0);
      i64c(c, 63);
      op(c, WASM_OP_I64_SHR_S);
      op(c, WASM_OP_I64_XOR);
      mask32(c);
      op(c, WASM_OP_I32_WRAP_I64);
      op(c, WASM_OP_I32_CLZ);
      op(c, WASM_OP_I64_EXTEND_I32_U);
    }
    i64c(c, 1);
    op(c, WASM_OP_I64_SUB);
    set_x(c, rd);
    return true;
  case 4: break; /* CLZ, below */
  default: return false;
  }
  get_x(c, rn);
  if (sf) {
    op(c, WASM_OP_I64_CLZ);
  } else {
    op(c, WASM_OP_I32_WRAP_I64);
    op(c, WASM_OP_I32_CLZ);
    op(c, WASM_OP_I64_EXTEND_I32_U);
  }
  set_x(c, bits(insn, 4, 0));
  return true;
}

static bool c_dp_register(Ctx *c, uint32_t insn) {
  if (!bit(insn, 28)) {
    if (!bit(insn, 24)) return c_logical_shifted(c, insn);
    if (!bit(insn, 21)) return c_add_sub_shifted(c, insn);
    return c_add_sub_extended(c, insn);
  }
  if (bit(insn, 24)) return c_dp_three_source(c, insn);
  switch (bits(insn, 23, 21)) {
  case 0: return c_add_sub_carry(c, insn);
  case 2: return c_conditional_compare(c, insn);
  case 4: return c_conditional_select(c, insn);
  case 6: return bit(insn, 30) ? c_dp_one_source(c, insn) : c_dp_two_source(c, insn);
  default: return false;
  }
}

/* ------------------------------------------------------------------ */
/* Loads and stores (general registers).                               */
/* ------------------------------------------------------------------ */

typedef enum Access {
  ACCESS_STORE,
  ACCESS_LOAD_ZERO,
  ACCESS_LOAD_SIGN64,
  ACCESS_LOAD_SIGN32,
  ACCESS_PREFETCH,
} Access;

/* The inline softmmu walk (vmm_translate_inline) for `size` bytes at
 * L_ADDR with `perm`: branches to `slow` if the access is out of range,
 * crosses a page, or is unmapped / not permitted; otherwise L_HOST is the
 * host address. */
static void emit_walk(Ctx *c, uint32_t size, uint32_t perm, uint32_t slow) {
  lget(c, L_ADDR);
  i64c(c, VMM_ADDRESS_SPACE_SIZE);
  op(c, WASM_OP_I64_GE_U);
  br_if(c, slow);
  if (size > 1u) {
    lget(c, L_ADDR);
    i64c(c, VMM_PAGE_OFFSET_MASK);
    op(c, WASM_OP_I64_AND);
    i64c(c, VMM_PAGE_SIZE - size);
    op(c, WASM_OP_I64_GT_U);
    br_if(c, slow);
  }
  lget(c, L_L1);
  lget(c, L_ADDR);
  i64c(c, WALK_L1_SHIFT);
  op(c, WASM_OP_I64_SHR_U);
  i64c(c, WALK_ENTRY_LOG2);
  op(c, WASM_OP_I64_SHL);
  op(c, WASM_OP_I64_ADD);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, 0);
  ltee(c, L_HOST);
  op(c, WASM_OP_I64_EQZ);
  br_if(c, slow);
  lget(c, L_HOST);
  lget(c, L_ADDR);
  i64c(c, WALK_L2_SHIFT);
  op(c, WASM_OP_I64_SHR_U);
  i64c(c, WALK_L2_MASK);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_ADD);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, 0);
  ltee(c, L_HOST);
  i64c(c, perm);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_EQZ);
  br_if(c, slow);
  lget(c, L_HOST);
  i64c(c, VMM_PTE_HOST_MASK);
  op(c, WASM_OP_I64_AND);
  lget(c, L_ADDR);
  i64c(c, VMM_PAGE_OFFSET_MASK);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_OR);
  lset(c, L_HOST);
}

/* L_HOST = where the `size` bytes at L_ADDR can be loaded from: guest
 * memory, or (page-crossing) the state's scratch after jit_helper_read
 * copied them there. A fault leaves through the interpreter. */
static void emit_load_address(Ctx *c, uint32_t size) {
  open_block(c); /* $go */
  const uint32_t go = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, size, VMM_PERM_R, c->depth);
  br(c, go);
  end_(c);
  lget(c, L_STATE);
  lget(c, L_ADDR);
  i32c(c, size);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_READ);
  op(c, WASM_OP_I32_EQZ);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
  lget(c, L_STATE);
  i64c(c, OFF_SCRATCH);
  op(c, WASM_OP_I64_ADD);
  lset(c, L_HOST);
  end_(c); /* $go */
}

/* Stores x[t] (and x[t2] after it, for a pair) of `size` bytes each at
 * L_ADDR: inline, or through jit_helper_store, which writes nothing
 * unless every page is writable. A fault leaves through the interpreter. */
static void emit_store(Ctx *c, uint32_t size, uint32_t t, bool pair, uint32_t t2);

static uint8_t load_opcode(Access access, uint32_t size) {
  const bool sign = access == ACCESS_LOAD_SIGN64 || access == ACCESS_LOAD_SIGN32;
  switch (size) {
  case 1: return sign ? WASM_OP_I64_LOAD8_S : WASM_OP_I64_LOAD8_U;
  case 2: return sign ? WASM_OP_I64_LOAD16_S : WASM_OP_I64_LOAD16_U;
  case 4: return sign ? WASM_OP_I64_LOAD32_S : WASM_OP_I64_LOAD32_U;
  default: return WASM_OP_I64_LOAD;
  }
}

static uint8_t store_opcode(uint32_t size) {
  switch (size) {
  case 1: return WASM_OP_I64_STORE8;
  case 2: return WASM_OP_I64_STORE16;
  case 4: return WASM_OP_I64_STORE32;
  default: return WASM_OP_I64_STORE;
  }
}

/* Host value -> L_VAL for a load. */
static void emit_host_load(Ctx *c, Access access, uint32_t size, uint64_t offset, uint32_t into) {
  lget(c, L_HOST);
  mem(c, load_opcode(access, size), ALIGN_1, offset);
  if (access == ACCESS_LOAD_SIGN32) mask32(c);
  lset(c, into);
}

#define STORE_SHAPE_PAIR 0x100u /* jit_helper_store: two elements */

static void emit_store(Ctx *c, uint32_t size, uint32_t t, bool pair, uint32_t t2) {
  open_block(c); /* $done */
  const uint32_t done = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, pair ? 2u * size : size, VMM_PERM_W, c->depth);
  lget(c, L_HOST);
  get_x(c, t);
  mem(c, store_opcode(size), ALIGN_1, 0);
  if (pair) {
    lget(c, L_HOST);
    get_x(c, t2);
    mem(c, store_opcode(size), ALIGN_1, size);
  }
  br(c, done);
  end_(c);
  lget(c, L_STATE);
  lget(c, L_ADDR);
  i32c(c, size | (pair ? STORE_SHAPE_PAIR : 0u));
  get_x(c, t);
  if (pair) get_x(c, t2);
  else i64c(c, 0);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_STORE);
  op(c, WASM_OP_I32_EQZ);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
  end_(c); /* $done */
}

/* One general-register transfer at L_ADDR; L_BASE + `offset` is the
 * written-back base when `wback`. */
static void emit_transfer(Ctx *c, Access access, uint32_t size, uint32_t t, uint32_t n, bool wback, uint64_t offset) {
  if (access == ACCESS_PREFETCH) return;
  const bool store = access == ACCESS_STORE;
  if (store) {
    emit_store(c, size, t, false, 0);
  } else {
    emit_load_address(c, size);
    emit_host_load(c, access, size, 0, L_VAL);
  }
  if (wback) { /* the base update first, then the loaded value wins */
    lget(c, L_BASE);
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
    set_xsp(c, n);
  }
  if (!store) {
    lget(c, L_VAL);
    set_x(c, t);
  }
}

/* size/opc -> access kind (decode_single, general registers). */
static bool decode_access(uint32_t size, uint32_t opc, bool allow_prefetch, Access *access) {
  switch (opc) {
  case 0: *access = ACCESS_STORE; return true;
  case 1: *access = ACCESS_LOAD_ZERO; return true;
  case 2:
    if (size == 3) {
      *access = ACCESS_PREFETCH;
      return allow_prefetch;
    }
    *access = ACCESS_LOAD_SIGN64;
    return true;
  default:
    if (size >= 2) return false;
    *access = ACCESS_LOAD_SIGN32;
    return true;
  }
}

static void base_to_locals(Ctx *c, uint32_t n) {
  get_xsp(c, n);
  lset(c, L_BASE);
}

static bool c_single_unsigned_offset(Ctx *c, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30);
  Access access;
  if (!decode_access(size, bits(insn, 23, 22), true, &access)) return false;
  const uint32_t bytes = 1u << size, n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  if (access == ACCESS_PREFETCH) return true;
  get_xsp(c, n);
  i64c(c, (uint64_t)bits(insn, 21, 10) * bytes);
  op(c, WASM_OP_I64_ADD);
  lset(c, L_ADDR);
  emit_transfer(c, access, bytes, t, n, false, 0);
  /* LDR Xt, [Xn, #imm] from a known address (a PLT stub's GOT slot):
   * remember what is there now, to predict a BR Xt. */
  uint64_t value;
  if (access == ACCESS_LOAD_ZERO && bytes == sizeof(uint64_t) && n != REG_ZR && t != REG_ZR &&
      (c->known_mask & (1u << n)) &&
      c->source->peek64(c->source->context, c->known_value[n] + (uint64_t)bits(insn, 21, 10) * bytes, &value)) {
    c->predicted_mask |= 1u << t;
    c->predicted_value[t] = value;
  }
  return true;
}

static bool c_single_imm9(Ctx *c, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30), form = bits(insn, 11, 10);
  Access access;
  if (!decode_access(size, bits(insn, 23, 22), form == 0, &access)) return false;
  if (access == ACCESS_PREFETCH) return true;
  const uint32_t n = bits(insn, 9, 5);
  const uint64_t offset = (uint64_t)sign_extend(bits(insn, 20, 12), 9);
  const bool wback = form == 1 || form == 3;
  base_to_locals(c, n);
  lget(c, L_BASE);
  if (form != 1) {
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
  }
  lset(c, L_ADDR);
  emit_transfer(c, access, 1u << size, bits(insn, 4, 0), n, wback, offset);
  return true;
}

static bool c_single_register_offset(Ctx *c, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30), option = bits(insn, 15, 13);
  if (!(option & 2u)) return false;
  Access access;
  if (!decode_access(size, bits(insn, 23, 22), true, &access)) return false;
  if (access == ACCESS_PREFETCH) return true;
  const uint32_t shift = bit(insn, 12) ? size : 0;
  const uint32_t n = bits(insn, 9, 5);
  get_xsp(c, n);
  get_x(c, bits(insn, 20, 16));
  if (option == 2) mask32(c);
  else if (option == 6) op(c, WASM_OP_I64_EXTEND32_S);
  if (shift) {
    i64c(c, shift);
    op(c, WASM_OP_I64_SHL);
  }
  op(c, WASM_OP_I64_ADD);
  lset(c, L_ADDR);
  emit_transfer(c, access, 1u << size, bits(insn, 4, 0), n, false, 0);
  return true;
}

static bool c_load_literal(Ctx *c, uint32_t insn) {
  Access access;
  uint32_t bytes;
  switch (bits(insn, 31, 30)) {
  case 0: access = ACCESS_LOAD_ZERO; bytes = 4; break;
  case 1: access = ACCESS_LOAD_ZERO; bytes = 8; break;
  case 2: access = ACCESS_LOAD_SIGN64; bytes = 4; break;
  default: return true; /* PRFM (literal) */
  }
  i64c(c, c->pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21));
  lset(c, L_ADDR);
  emit_transfer(c, access, bytes, bits(insn, 4, 0), 0, false, 0);
  return true;
}

static bool c_pair(Ctx *c, uint32_t insn) {
  const uint32_t opc = bits(insn, 31, 30), form = bits(insn, 24, 23);
  const bool load = bit(insn, 22);
  Access access;
  uint32_t bytes;
  switch (opc) {
  case 0: bytes = 4; access = load ? ACCESS_LOAD_ZERO : ACCESS_STORE; break;
  case 1:
    if (!load || form == 0) return false;
    bytes = 4; access = ACCESS_LOAD_SIGN64;
    break;
  case 2: bytes = 8; access = load ? ACCESS_LOAD_ZERO : ACCESS_STORE; break;
  default: return false;
  }
  const uint32_t n = bits(insn, 9, 5), t = bits(insn, 4, 0), t2 = bits(insn, 14, 10);
  const uint64_t offset = (uint64_t)sign_extend(bits(insn, 21, 15), 7) * bytes;
  const bool wback = form == 1 || form == 3;
  base_to_locals(c, n);
  lget(c, L_BASE);
  if (form != 1) {
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
  }
  lset(c, L_ADDR);
  if (!load) {
    emit_store(c, bytes, t, true, t2);
  } else {
    emit_load_address(c, 2u * bytes);
    emit_host_load(c, access, bytes, 0, L_VAL);
    emit_host_load(c, access, bytes, bytes, L_VAL2);
  }
  if (wback) {
    lget(c, L_BASE);
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
    set_xsp(c, n);
  }
  if (load) {
    lget(c, L_VAL);
    set_x(c, t);
    lget(c, L_VAL2);
    set_x(c, t2);
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* Direct interpreter calls (SIMD&FP).                                 */
/* ------------------------------------------------------------------ */

/* What an instruction executed by a direct call reads or writes besides
 * the vector registers, FPCR and FPSR (which live in the state): those
 * general registers (bit 31 = SP) and NZCV are synchronised around it. */
typedef struct Sync {
  uint64_t regs;
  bool nzcv;
} Sync;

static void sync_x(Sync *sync, uint32_t r) {
  if (r != REG_ZR) sync->regs |= (uint64_t)1 << r;
}
static void sync_xsp(Sync *sync, uint32_t r) { sync->regs |= (uint64_t)1 << r; }

/* The interpreter runs this instruction against the state (jit_helper_
 * simd): the registers in `sync` go to the state first and come back
 * after, every other register stays in its local. The instruction
 * counts as retired inline; one the interpreter refuses (undefined, a
 * fault) leaves through $leave with nothing changed. */
static void emit_direct_call(Ctx *c, const Sync *sync) {
  for (uint32_t r = 0; r < REG_ZR; r++) {
    if (sync->regs & ((uint64_t)1 << r)) {
      c->used |= (uint64_t)1 << r;
      state_store64_local(c, OFF_X(r), L_X0 + r);
    }
  }
  if (sync->regs & MASK_SP) {
    c->used |= MASK_SP;
    state_store64_local(c, OFF_SP, L_SP);
  }
  if (sync->nzcv) {
    lget(c, L_STATE);
    load_flags(c);
    mem(c, WASM_OP_I32_STORE, ALIGN_4, OFF_PSTATE);
  }
  lget(c, L_STATE);
  i32c(c, c->insn);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_SIMD);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
  for (uint32_t r = 0; r < REG_ZR; r++) {
    if (sync->regs & ((uint64_t)1 << r)) {
      state_load64(c, OFF_X(r));
      set_x(c, r);
    }
  }
  if (sync->regs & MASK_SP) {
    state_load64(c, OFF_SP);
    set_xsp(c, REG_ZR);
  }
  if (sync->nzcv) {
    lget(c, L_STATE);
    mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_PSTATE);
    store_flags(c);
  }
}

/* SIMD&FP data processing (op0 x111): the forms that touch general
 * registers or NZCV, mirroring interp_simd_fp's decode. */
static void simd_fp_sync(uint32_t insn, Sync *sync) {
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (bit(insn, 28) && !bit(insn, 30)) { /* scalar floating point (and crypto) */
    if (bit(insn, 29) || bits(insn, 28, 24) < 0x1E) return;
    if (!bit(insn, 24) && (!bit(insn, 21) || bits(insn, 15, 10) == 0)) { /* conversions, FMOV (general) */
      sync_x(sync, rn);
      sync_x(sync, rd);
      return;
    }
    if (bit(insn, 31) || bit(insn, 24) || bits(insn, 14, 10) == 0x10) return;
    if (bits(insn, 13, 10) == 0x8) { /* FCMP, FCMPE */
      sync->nzcv = true;
      return;
    }
    if (bits(insn, 12, 10) == 0x4) return;
    if (bits(insn, 11, 10) == 1 || bits(insn, 11, 10) == 3) sync->nzcv = true; /* FCCMP, FCSEL */
    return;
  }
  /* Advanced SIMD: only the copy group's general forms (DUP, INS, SMOV, UMOV). */
  if (!bit(insn, 28) && bits(insn, 28, 24) == 0x0E && !bit(insn, 21) && bit(insn, 10) && bits(insn, 23, 21) == 0 &&
      !bit(insn, 15) && !bit(insn, 29) && (bits(insn, 14, 11) & 1u)) {
    sync_x(sync, rn);
    sync_x(sync, rd);
  }
}

/* ------------------------------------------------------------------ */
/* Scalar floating point: native fast paths.                           */
/* ------------------------------------------------------------------ */
/*
 * The interpreter's FP is softfloat with ARM semantics and sticky FPSR
 * flags. A wasm f32/f64 operation gives the identical bits and flags
 * when: FPCR is 0 (round to nearest, no FZ, no DN); no NaN is involved;
 * the result is finite and strictly above the smallest normal (no
 * overflow, no underflow - ARM detects tininess before rounding, hence
 * "strictly"); and FPSR.IXC is already set, so an inexact result changes
 * nothing. Anything else - including the first inexact operation of a
 * run - takes the exact path (emit_direct_call into the interpreter),
 * which has not changed any state yet.
 */

#define FP_MIN_NORMAL_D 0x0010000000000000ull
#define FP_INFINITY_D 0x7FF0000000000000ull
#define FP_MIN_NORMAL_S 0x00800000u
#define FP_INFINITY_S 0x7F800000u
#define FP_SIGN_D 0x8000000000000000ull
#define FP_SIGN_S 0x80000000u
#define FPSR_IXC_BIT 4u /* softfloat.h FPSR_IXC */
#define F32_EXTRA_BITS 29u /* f64 fraction bits below an f32's */
#define F32_MIDPOINT (1ull << (F32_EXTRA_BITS - 1u))

static void f64c(Ctx *c, uint64_t bits_value) {
  op(c, WASM_OP_F64_CONST);
  uint8_t raw[sizeof(uint64_t)];
  for (uint32_t i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)(bits_value >> (8u * i));
  wasm_bytes(c->b, raw, sizeof(raw));
}
static void f32c(Ctx *c, uint32_t bits_value) {
  op(c, WASM_OP_F32_CONST);
  uint8_t raw[sizeof(uint32_t)];
  for (uint32_t i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)(bits_value >> (8u * i));
  wasm_bytes(c->b, raw, sizeof(raw));
}

/* i32: FPCR == 0. */
static void emit_fpcr_default(Ctx *c) {
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_FPCR);
  op(c, WASM_OP_I32_EQZ);
}
/* i32: FPCR == 0 and FPSR.IXC set. */
static void emit_fp_env_ok(Ctx *c) {
  emit_fpcr_default(c);
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_FPSR);
  i32c(c, FPSR_IXC_BIT);
  op(c, WASM_OP_I32_SHR_U);
  op(c, WASM_OP_I32_AND);
}

/* The scalar in V[n] as f32/f64 on the stack. */
static void load_fp(Ctx *c, uint32_t n, bool dbl) {
  lget(c, L_STATE);
  mem(c, dbl ? WASM_OP_F64_LOAD : WASM_OP_F32_LOAD, dbl ? ALIGN_8 : ALIGN_4, OFF_V(n));
}

/* V[d] = the f32/f64 in `local`, the rest of the register zeroed. */
static void store_fp_local(Ctx *c, uint32_t d, bool dbl, uint32_t local) {
  lget(c, L_STATE);
  i64c(c, 0);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(d) + V_HIGH_HALF);
  if (!dbl) {
    lget(c, L_STATE);
    i64c(c, 0);
    mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(d));
  }
  lget(c, L_STATE);
  lget(c, local);
  mem(c, dbl ? WASM_OP_F64_STORE : WASM_OP_F32_STORE, dbl ? ALIGN_8 : ALIGN_4, OFF_V(d));
}

/* V[d] = the i64 bit pattern on the stack (a scalar of the format), the
 * rest zeroed. */
static void store_fp_bits(Ctx *c, uint32_t d) {
  lset(c, L_T0);
  state_store64_local(c, OFF_V(d), L_T0);
  lget(c, L_STATE);
  i64c(c, 0);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(d) + V_HIGH_HALF);
}

/* i32: smallest normal < |local| < infinity (false for NaN). */
static void emit_result_normal(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  op(c, dbl ? WASM_OP_F64_ABS : WASM_OP_F32_ABS);
  if (dbl) f64c(c, FP_MIN_NORMAL_D);
  else f32c(c, FP_MIN_NORMAL_S);
  op(c, dbl ? WASM_OP_F64_GT : WASM_OP_F32_GT);
  lget(c, local);
  op(c, dbl ? WASM_OP_F64_ABS : WASM_OP_F32_ABS);
  if (dbl) f64c(c, FP_INFINITY_D);
  else f32c(c, FP_INFINITY_S);
  op(c, dbl ? WASM_OP_F64_LT : WASM_OP_F32_LT);
  op(c, WASM_OP_I32_AND);
}

/* i32: |local| < infinity (finite, not NaN). Sums and differences are
 * exact whenever they are tiny (zero or subnormal), so finite suffices. */
static void emit_result_finite(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  op(c, dbl ? WASM_OP_F64_ABS : WASM_OP_F32_ABS);
  if (dbl) f64c(c, FP_INFINITY_D);
  else f32c(c, FP_INFINITY_S);
  op(c, dbl ? WASM_OP_F64_LT : WASM_OP_F32_LT);
}

/* i32: local == 0.0 (either sign). */
static void emit_is_zero(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  if (dbl) f64c(c, 0);
  else f32c(c, 0);
  op(c, dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ);
}

/* i32: an f64 that is non-zero but no larger than the smallest f32
 * normal - a fused single result that may underflow. */
#define FP_MIN_NORMAL_S_AS_D 0x3810000000000000ull
static void emit_tiny_single_in_double(Ctx *c, uint32_t local) {
  lget(c, local);
  f64c(c, 0);
  op(c, WASM_OP_F64_NE);
  lget(c, local);
  op(c, WASM_OP_F64_ABS);
  f64c(c, FP_MIN_NORMAL_S_AS_D);
  op(c, WASM_OP_F64_LE);
  op(c, WASM_OP_I32_AND);
}

/* if (guard on the stack) { fast } else { exact direct call }: opens the
 * fast arm; finish with fp_else_exact(). */
static void fp_fast_arm(Ctx *c) { open_if(c, WASM_BLOCK_VOID); }
static void fp_else_exact(Ctx *c, const Sync *sync) {
  else_(c);
  emit_direct_call(c, sync);
  end_(c);
}

static uint32_t fp_result_local(bool dbl) { return dbl ? L_FD : L_FS; }

/* FMUL, FDIV, FADD, FSUB, FNMUL, and FSQRT (two = false). */
static void emit_fp_arith(Ctx *c, bool dbl, uint32_t opcode, bool two, uint32_t n, uint32_t m, uint32_t d) {
  static const uint8_t ops64[4] = {WASM_OP_F64_MUL, WASM_OP_F64_DIV, WASM_OP_F64_ADD, WASM_OP_F64_SUB};
  static const uint8_t ops32[4] = {WASM_OP_F32_MUL, WASM_OP_F32_DIV, WASM_OP_F32_ADD, WASM_OP_F32_SUB};
  const uint32_t r = fp_result_local(dbl), a = dbl ? L_FD2 : L_FS2, b = dbl ? L_FD3 : L_FS3;
  load_fp(c, n, dbl);
  ltee(c, a);
  if (two) {
    load_fp(c, m, dbl);
    ltee(c, b);
    const uint32_t k = opcode == 8u ? 0u : opcode;
    op(c, dbl ? ops64[k] : ops32[k]);
    if (opcode == 8u) op(c, dbl ? WASM_OP_F64_NEG : WASM_OP_F32_NEG); /* FNMUL: -(a*b), after rounding */
  } else {
    op(c, dbl ? WASM_OP_F64_SQRT : WASM_OP_F32_SQRT);
  }
  lset(c, r);
  if (two && (opcode == 2u || opcode == 3u)) { /* FADD, FSUB: any finite result is exact or merely inexact */
    emit_result_finite(c, r, dbl);
  } else { /* normal, or an exact zero: x*0, 0/x, sqrt(+-0) */
    emit_result_normal(c, r, dbl);
    emit_is_zero(c, r, dbl);
    if (!two) {
      /* sqrt: a zero result means a zero input */
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
  emit_fp_env_ok(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  store_fp_local(c, d, dbl, r);
  Sync none = {0};
  fp_else_exact(c, &none);
}

/* Single-precision FMADD/FMSUB/FNMADD/FNMSUB: addend + n*m in f64 (the
 * product is exact there, the sum rounds once), then to f32 - the same
 * as one rounding unless the f64 sum sits exactly on an f32 midpoint. */
static void emit_fp_fused_single(Ctx *c, bool o1, bool o0, uint32_t n, uint32_t m, uint32_t a, uint32_t d) {
  load_fp(c, a, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  if (o1) op(c, WASM_OP_F64_NEG);
  load_fp(c, n, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  if (o0 != o1) op(c, WASM_OP_F64_NEG);
  load_fp(c, m, false);
  op(c, WASM_OP_F64_PROMOTE_F32);
  op(c, WASM_OP_F64_MUL);
  op(c, WASM_OP_F64_ADD);
  ltee(c, L_FD);
  op(c, WASM_OP_F32_DEMOTE_F64);
  lset(c, L_FS);
  emit_result_finite(c, L_FS, false); /* finite, and not tiny unless exactly zero */
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
  store_fp_local(c, d, false, L_FS);
  Sync none = {0};
  fp_else_exact(c, &none);
}

/* FCMP/FCMPE (and with #0.0): NZCV from an ordered compare. */
static void emit_fp_compare(Ctx *c, bool dbl, uint32_t n, uint32_t m, bool with_zero) {
  load_fp(c, n, dbl);
  if (dbl) lset(c, L_FD);
  else lset(c, L_FS);
  if (with_zero) {
    if (dbl) f64c(c, 0);
    else f32c(c, 0);
  } else {
    load_fp(c, m, dbl);
  }
  if (dbl) lset(c, L_FD2);
  else lset(c, L_FS2);
  const uint32_t a = dbl ? L_FD : L_FS, b = dbl ? L_FD2 : L_FS2;
  const uint8_t eq = dbl ? WASM_OP_F64_EQ : WASM_OP_F32_EQ, lt = dbl ? WASM_OP_F64_LT : WASM_OP_F32_LT;
  lget(c, a); /* no NaN: each equals itself */
  lget(c, a);
  op(c, eq);
  lget(c, b);
  lget(c, b);
  op(c, eq);
  op(c, WASM_OP_I32_AND);
  emit_fpcr_default(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  i32c(c, CPU_PSTATE_N);                  /* a < b: 1000 */
  i32c(c, CPU_PSTATE_Z | CPU_PSTATE_C);   /* a == b: 0110 */
  i32c(c, CPU_PSTATE_C);                  /* a > b: 0010 */
  lget(c, a);
  lget(c, b);
  op(c, eq);
  op(c, WASM_OP_SELECT);
  lget(c, a);
  lget(c, b);
  op(c, lt);
  op(c, WASM_OP_SELECT);
  store_flags(c);
  Sync flags = {0};
  flags.nzcv = true;
  fp_else_exact(c, &flags);
  c->flag_kind = FLAGS_LIVE;
}

/* Scalar FP with a native fast path; false: not one of these forms. */
static bool c_scalar_fp_fast(Ctx *c, uint32_t insn) {
  if (bits(insn, 31, 29) != 0 || bits(insn, 28, 24) < 0x1E) {
    /* FMOV/SCVTF/UCVTF/FCVTZS/FCVTZU (general) have sf in bit 31 */
    if (!(bits(insn, 30, 24) == 0x1E && bit(insn, 21) && bits(insn, 15, 10) == 0)) return false;
  }
  const uint32_t ftype = bits(insn, 23, 22);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0), rm = bits(insn, 20, 16);
  if (bit(insn, 24)) { /* FMADD & co.: single only (doubles need a true fused multiply-add) */
    if (bit(insn, 31) || ftype != 0) return false;
    emit_fp_fused_single(c, bit(insn, 21), bit(insn, 15), rn, rm, bits(insn, 14, 10), rd);
    return true;
  }
  if (!bit(insn, 21)) return false; /* fixed-point conversions */
  const bool dbl = ftype == 1u;
  if (bits(insn, 15, 10) == 0) { /* conversions between FP and general registers */
    const bool sf = bit(insn, 31);
    const uint32_t rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16);
    if (opcode == 6 || opcode == 7) { /* FMOV (general) */
      if (sf && ftype == 2u && rmode == 1u) { /* Vd.D[1] <-> Xn */
        if (opcode == 7) {
          lget(c, L_STATE);
          get_x(c, rn);
          mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(rd) + V_HIGH_HALF);
        } else {
          state_load64(c, OFF_V(rn) + V_HIGH_HALF);
          set_x(c, rd);
        }
        return true;
      }
      if (rmode != 0 || !((!sf && ftype == 0u) || (sf && ftype == 1u))) return false;
      if (opcode == 7) {
        get_xw(c, rn, sf);
        store_fp_bits(c, rd);
      } else {
        state_load64(c, OFF_V(rn));
        width(c, sf);
        set_x(c, rd);
      }
      return true;
    }
    if (ftype > 1u) return false;
    if ((opcode == 2 || opcode == 3) && rmode == 0) { /* SCVTF / UCVTF */
      static const uint8_t to64[2][2] = {{WASM_OP_F64_CONVERT_I32_S, WASM_OP_F64_CONVERT_I32_U},
                                         {WASM_OP_F64_CONVERT_I64_S, WASM_OP_F64_CONVERT_I64_U}};
      static const uint8_t to32[2][2] = {{WASM_OP_F32_CONVERT_I32_S, WASM_OP_F32_CONVERT_I32_U},
                                         {WASM_OP_F32_CONVERT_I64_S, WASM_OP_F32_CONVERT_I64_U}};
      const uint32_t is_unsigned = opcode == 3;
      const uint32_t r = fp_result_local(dbl);
      get_x(c, rn);
      if (!sf) op(c, WASM_OP_I32_WRAP_I64);
      op(c, dbl ? to64[sf][is_unsigned] : to32[sf][is_unsigned]);
      lset(c, r);
      if (dbl && !sf) { /* a 32-bit integer always fits a double exactly */
        emit_fpcr_default(c);
      } else {
        emit_fp_env_ok(c);
      }
      fp_fast_arm(c);
      store_fp_local(c, rd, dbl, r);
      Sync sync = {0};
      sync_x(&sync, rn);
      sync_x(&sync, rd);
      fp_else_exact(c, &sync);
      return true;
    }
    if ((opcode == 0 || opcode == 1) && rmode == 3) { /* FCVTZS / FCVTZU: truncation, in range only */
      const bool is_unsigned = opcode == 1;
      const uint32_t a = fp_result_local(dbl);
      load_fp(c, rn, dbl);
      lset(c, a);
      /* low < a < high, as doubles (exact for every f32) */
      const double high = sf ? 18446744073709551616.0 : 4294967296.0;
      const double high_signed = sf ? 9223372036854775808.0 : 2147483648.0;
      const double low = is_unsigned ? -1.0 : -(high_signed) - 1.0;
      const double top = is_unsigned ? high : high_signed;
      uint64_t low_bits, top_bits;
      memcpy(&low_bits, &low, sizeof(low_bits));
      memcpy(&top_bits, &top, sizeof(top_bits));
      lget(c, a);
      if (!dbl) op(c, WASM_OP_F64_PROMOTE_F32);
      f64c(c, low_bits);
      op(c, WASM_OP_F64_GT);
      lget(c, a);
      if (!dbl) op(c, WASM_OP_F64_PROMOTE_F32);
      f64c(c, top_bits);
      op(c, WASM_OP_F64_LT);
      op(c, WASM_OP_I32_AND);
      emit_fp_env_ok(c);
      op(c, WASM_OP_I32_AND);
      fp_fast_arm(c);
      lget(c, a);
      if (sf) {
        static const uint8_t trunc64[2][2] = {{WASM_OP_I64_TRUNC_F32_S, WASM_OP_I64_TRUNC_F32_U},
                                              {WASM_OP_I64_TRUNC_F64_S, WASM_OP_I64_TRUNC_F64_U}};
        op(c, trunc64[dbl][is_unsigned]);
      } else {
        static const uint8_t trunc32[2][2] = {{WASM_OP_I32_TRUNC_F32_S, WASM_OP_I32_TRUNC_F32_U},
                                              {WASM_OP_I32_TRUNC_F64_S, WASM_OP_I32_TRUNC_F64_U}};
        op(c, trunc32[dbl][is_unsigned]);
        op(c, WASM_OP_I64_EXTEND_I32_U);
      }
      set_x(c, rd);
      Sync sync = {0};
      sync_x(&sync, rn);
      sync_x(&sync, rd);
      fp_else_exact(c, &sync);
      return true;
    }
    return false;
  }
  if (ftype > 1u) return false;
  const uint64_t sign = dbl ? FP_SIGN_D : FP_SIGN_S;
  if (bits(insn, 14, 10) == 0x10) { /* one source */
    const uint32_t opcode = bits(insn, 20, 15);
    switch (opcode) {
    case 0: case 1: case 2: /* FMOV, FABS, FNEG: bit operations */
      state_load64(c, OFF_V(rn));
      width(c, dbl);
      if (opcode == 1) {
        i64c(c, ~sign);
        op(c, WASM_OP_I64_AND);
      } else if (opcode == 2) {
        i64c(c, sign);
        op(c, WASM_OP_I64_XOR);
      }
      store_fp_bits(c, rd);
      return true;
    case 3: /* FSQRT */
      emit_fp_arith(c, dbl, 0, false, rn, 0, rd);
      return true;
    case 4: case 5: { /* FCVT between single and double */
      const uint32_t to = opcode & 3u;
      if (to == ftype) return false;
      if (!dbl) { /* single -> double: exact unless NaN */
        load_fp(c, rn, false);
        op(c, WASM_OP_F64_PROMOTE_F32);
        ltee(c, L_FD);
        lget(c, L_FD);
        op(c, WASM_OP_F64_EQ);
        emit_fpcr_default(c);
        op(c, WASM_OP_I32_AND);
        fp_fast_arm(c);
        store_fp_local(c, rd, true, L_FD);
      } else { /* double -> single: rounds */
        load_fp(c, rn, true);
        ltee(c, L_FD);
        op(c, WASM_OP_F32_DEMOTE_F64);
        lset(c, L_FS);
        emit_result_normal(c, L_FS, false);
        emit_is_zero(c, L_FD, true); /* a zero converts exactly */
        op(c, WASM_OP_I32_OR);
        emit_fp_env_ok(c);
        op(c, WASM_OP_I32_AND);
        fp_fast_arm(c);
        store_fp_local(c, rd, false, L_FS);
      }
      Sync none = {0};
      fp_else_exact(c, &none);
      return true;
    }
    default:
      return false;
    }
  }
  if (bits(insn, 13, 10) == 0x8) { /* FCMP, FCMPE */
    const uint32_t opcode2 = bits(insn, 4, 0);
    if (bits(insn, 15, 14) != 0 || (opcode2 & 7u) != 0) return false;
    emit_fp_compare(c, dbl, rn, rm, (opcode2 >> 3) & 1u);
    return true;
  }
  if (bits(insn, 12, 10) == 0x4) { /* FMOV (immediate) */
    if (rn != 0) return false;
    i64c(c, fp_expand_imm8(dbl ? FP_DOUBLE : FP_SINGLE, bits(insn, 20, 13)));
    store_fp_bits(c, rd);
    return true;
  }
  switch (bits(insn, 11, 10)) {
  case 2: { /* two source */
    const uint32_t opcode = bits(insn, 15, 12);
    if (opcode > 3u && opcode != 8u) return false; /* FMAX/FMIN & co.: NaN and zero rules */
    emit_fp_arith(c, dbl, opcode, true, rn, rm, rd);
    return true;
  }
  case 3: /* FCSEL */
    state_load64(c, OFF_V(rn));
    width(c, dbl);
    state_load64(c, OFF_V(rm));
    width(c, dbl);
    emit_condition(c, bits(insn, 15, 12));
    op(c, WASM_OP_SELECT);
    store_fp_bits(c, rd);
    return true;
  default:
    return false;
  }
}

/* ------------------------------------------------------------------ */
/* Vector floating point: SIMD128 fast paths.                          */
/* ------------------------------------------------------------------ */
/*
 * The scalar rules (see "Scalar floating point" above) lane by lane:
 * every active lane's result normal and finite, FPCR 0, FPSR.IXC set;
 * otherwise the exact direct call, which has not changed anything yet.
 */

#define SIMD_LANE_BYTES 16u

static void simd(Ctx *c, uint32_t opcode) {
  op(c, WASM_OP_SIMD_PREFIX);
  wasm_uleb(c->b, opcode);
}
static void v128_const(Ctx *c, uint64_t low, uint64_t high) {
  simd(c, WASM_SIMD_V128_CONST);
  uint8_t raw[SIMD_LANE_BYTES];
  for (uint32_t i = 0; i < sizeof(uint64_t); i++) {
    raw[i] = (uint8_t)(low >> (8u * i));
    raw[sizeof(uint64_t) + i] = (uint8_t)(high >> (8u * i));
  }
  wasm_bytes(c->b, raw, sizeof(raw));
}
static void v128_shuffle(Ctx *c, const uint8_t lanes[SIMD_LANE_BYTES]) {
  simd(c, WASM_SIMD_I8X16_SHUFFLE);
  wasm_bytes(c->b, lanes, SIMD_LANE_BYTES);
}
static void load_v(Ctx *c, uint32_t r) {
  lget(c, L_STATE);
  simd(c, WASM_SIMD_V128_LOAD);
  wasm_uleb(c->b, ALIGN_8);
  wasm_uleb(c->b, OFF_V(r));
}
/* The upper two f32 lanes moved down (for promote_low). */
static void v128_high_to_low(Ctx *c) {
  static const uint8_t lanes[SIMD_LANE_BYTES] = {8, 9, 10, 11, 12, 13, 14, 15, 8, 9, 10, 11, 12, 13, 14, 15};
  lget(c, L_VT); /* both operands the same vector */
  lget(c, L_VT);
  v128_shuffle(c, lanes);
}

/* i32: every active lane set in the mask on the stack. */
static void emit_all_lanes(Ctx *c, bool q) {
  if (!q) { /* the upper half is not part of a 64-bit vector */
    v128_const(c, 0, ~(uint64_t)0);
    simd(c, WASM_SIMD_V128_OR);
  }
  simd(c, WASM_SIMD_I32X4_ALL_TRUE);
}

/* Lane mask: |local| < infinity. */
static void emit_lane_finite(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  simd(c, dbl ? WASM_SIMD_F64X2_ABS : WASM_SIMD_F32X4_ABS);
  if (dbl) f64c(c, FP_INFINITY_D);
  else f32c(c, FP_INFINITY_S);
  simd(c, dbl ? WASM_SIMD_F64X2_SPLAT : WASM_SIMD_F32X4_SPLAT);
  simd(c, dbl ? WASM_SIMD_F64X2_LT : WASM_SIMD_F32X4_LT);
}

/* Lane mask: local == 0.0. */
static void emit_lane_zero(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  if (dbl) f64c(c, 0);
  else f32c(c, 0);
  simd(c, dbl ? WASM_SIMD_F64X2_SPLAT : WASM_SIMD_F32X4_SPLAT);
  simd(c, dbl ? WASM_SIMD_F64X2_EQ : WASM_SIMD_F32X4_EQ);
}

/* Lane mask: smallest normal < |local| < infinity. */
static void emit_lane_normal(Ctx *c, uint32_t local, bool dbl) {
  lget(c, local);
  simd(c, dbl ? WASM_SIMD_F64X2_ABS : WASM_SIMD_F32X4_ABS);
  if (dbl) f64c(c, FP_MIN_NORMAL_D);
  else f32c(c, FP_MIN_NORMAL_S);
  simd(c, dbl ? WASM_SIMD_F64X2_SPLAT : WASM_SIMD_F32X4_SPLAT);
  simd(c, dbl ? WASM_SIMD_F64X2_GT : WASM_SIMD_F32X4_GT);
  emit_lane_finite(c, local, dbl);
  simd(c, WASM_SIMD_V128_AND);
}

/* i32: no f64 lane of `local` non-zero and at most the smallest f32 normal. */
static void emit_no_tiny_singles(Ctx *c, uint32_t local) {
  lget(c, local);
  f64c(c, 0);
  simd(c, WASM_SIMD_F64X2_SPLAT);
  simd(c, WASM_SIMD_F64X2_NE);
  lget(c, local);
  simd(c, WASM_SIMD_F64X2_ABS);
  f64c(c, FP_MIN_NORMAL_S_AS_D);
  simd(c, WASM_SIMD_F64X2_SPLAT);
  simd(c, WASM_SIMD_F64X2_LE);
  simd(c, WASM_SIMD_V128_AND);
  simd(c, WASM_SIMD_V128_ANY_TRUE);
  op(c, WASM_OP_I32_EQZ);
}

/* V[d] = the v128 in `local` (only the low 64 bits for a 64-bit vector). */
static void store_v(Ctx *c, uint32_t d, uint32_t local, bool q) {
  if (q) {
    lget(c, L_STATE);
    lget(c, local);
    simd(c, WASM_SIMD_V128_STORE);
    wasm_uleb(c->b, ALIGN_8);
    wasm_uleb(c->b, OFF_V(d));
    return;
  }
  lget(c, L_STATE);
  lget(c, local);
  simd(c, WASM_SIMD_I64X2_EXTRACT_LANE);
  wasm_u8(c->b, 0);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(d));
  lget(c, L_STATE);
  i64c(c, 0);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(d) + V_HIGH_HALF);
}

/* i32: no lane of the f64x2 in `local` sits exactly on an f32 midpoint. */
static void emit_off_midpoints(Ctx *c, uint32_t local) {
  lget(c, local);
  v128_const(c, (1ull << F32_EXTRA_BITS) - 1u, (1ull << F32_EXTRA_BITS) - 1u);
  simd(c, WASM_SIMD_V128_AND);
  v128_const(c, F32_MIDPOINT, F32_MIDPOINT);
  simd(c, WASM_SIMD_I64X2_NE);
  simd(c, WASM_SIMD_I64X2_ALL_TRUE);
}

/* f64x2 of D + A*B for the two f32 lanes promote_low picks from L_VA,
 * L_VB, L_VD (or from their upper halves when `high`). */
static void emit_fused_pair(Ctx *c, bool high) {
  static const uint32_t sources[3] = {L_VD, L_VA, L_VB};
  for (uint32_t i = 0; i < 3u; i++) {
    lget(c, sources[i]);
    if (high) {
      lset(c, L_VT);
      v128_high_to_low(c);
    }
    simd(c, WASM_SIMD_F64X2_PROMOTE_LOW_F32X4);
  }
  simd(c, WASM_SIMD_F64X2_MUL);
  simd(c, WASM_SIMD_F64X2_ADD);
}

typedef enum Vector_Fp_Op { VFP_ADD, VFP_SUB, VFP_MUL, VFP_DIV, VFP_MLA, VFP_MLS } Vector_Fp_Op;

/* rd = op(rn, rm or the broadcast element) over 2 or 4 lanes. L_VB must
 * hold the second operand already. */
static void emit_vector_fp(Ctx *c, Vector_Fp_Op vop, bool dbl, bool q, uint32_t n, uint32_t d) {
  static const uint32_t ops32[4] = {WASM_SIMD_F32X4_ADD, WASM_SIMD_F32X4_SUB, WASM_SIMD_F32X4_MUL, WASM_SIMD_F32X4_DIV};
  static const uint32_t ops64[4] = {WASM_SIMD_F64X2_ADD, WASM_SIMD_F64X2_SUB, WASM_SIMD_F64X2_MUL, WASM_SIMD_F64X2_DIV};
  load_v(c, n);
  lset(c, L_VA);
  if (vop == VFP_MLA || vop == VFP_MLS) { /* single precision only */
    if (vop == VFP_MLS) {
      lget(c, L_VA);
      simd(c, WASM_SIMD_F32X4_NEG);
      lset(c, L_VA);
    }
    load_v(c, d);
    lset(c, L_VD);
    emit_fused_pair(c, false);
    lset(c, L_VR);
    emit_fused_pair(c, true);
    lset(c, L_VR2);
    emit_off_midpoints(c, L_VR);
    emit_no_tiny_singles(c, L_VR);
    op(c, WASM_OP_I32_AND);
    if (q) {
      emit_off_midpoints(c, L_VR2);
      op(c, WASM_OP_I32_AND);
      emit_no_tiny_singles(c, L_VR2);
      op(c, WASM_OP_I32_AND);
    }
    lset(c, L_PASS);
    static const uint8_t join[SIMD_LANE_BYTES] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
    lget(c, L_VR);
    simd(c, WASM_SIMD_F32X4_DEMOTE_F64X2_ZERO);
    lget(c, L_VR2);
    simd(c, WASM_SIMD_F32X4_DEMOTE_F64X2_ZERO);
    v128_shuffle(c, join);
    lset(c, L_VR);
    emit_lane_finite(c, L_VR, false);
    emit_all_lanes(c, q);
    lget(c, L_PASS);
    op(c, WASM_OP_I32_AND);
  } else {
    lget(c, L_VA);
    lget(c, L_VB);
    simd(c, dbl ? ops64[vop] : ops32[vop]);
    lset(c, L_VR);
    if (vop == VFP_ADD || vop == VFP_SUB) {
      emit_lane_finite(c, L_VR, dbl);
    } else { /* normal, or an exact zero: x*0, 0/x */
      emit_lane_normal(c, L_VR, dbl);
      emit_lane_zero(c, L_VR, dbl);
      emit_lane_zero(c, L_VA, dbl);
      if (vop == VFP_MUL) {
        emit_lane_zero(c, L_VB, dbl);
        simd(c, WASM_SIMD_V128_OR);
      }
      simd(c, WASM_SIMD_V128_AND);
      simd(c, WASM_SIMD_V128_OR);
    }
    emit_all_lanes(c, q);
  }
  emit_fp_env_ok(c);
  op(c, WASM_OP_I32_AND);
  fp_fast_arm(c);
  store_v(c, d, L_VR, q);
  Sync none = {0};
  fp_else_exact(c, &none);
}

/* Advanced SIMD FP with a fast path; false: not one of these forms. */
static bool c_vector_fp_fast(Ctx *c, uint32_t insn) {
  if (bit(insn, 31)) return false;
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t rn = bits(insn, 9, 5), rd = bits(insn, 4, 0);
  if (bits(insn, 28, 24) == 0x0E && bit(insn, 21) && bit(insn, 10) && (bits(insn, 15, 11) >> 3) == 3u) {
    /* three same, floating point */
    const bool dbl = bit(insn, 22), a1 = bit(insn, 23);
    if (dbl && !q) return false;
    Vector_Fp_Op vop;
    switch (((uint32_t)u << 4) | ((uint32_t)a1 << 3) | bits(insn, 13, 11)) {
    case 0x02: vop = VFP_ADD; break;
    case 0x0A: vop = VFP_SUB; break;
    case 0x13: vop = VFP_MUL; break;
    case 0x17: vop = VFP_DIV; break;
    case 0x01: vop = VFP_MLA; break;
    case 0x09: vop = VFP_MLS; break;
    default: return false;
    }
    if (dbl && (vop == VFP_MLA || vop == VFP_MLS)) return false; /* needs a true fused multiply-add */
    load_v(c, bits(insn, 20, 16));
    lset(c, L_VB);
    emit_vector_fp(c, vop, dbl, q, rn, rd);
    return true;
  }
  if (bits(insn, 28, 24) == 0x0F && !bit(insn, 10) && bit(insn, 23)) { /* by element, floating point */
    const uint32_t opcode = bits(insn, 15, 12);
    const bool dbl = bit(insn, 22);
    Vector_Fp_Op vop;
    if (opcode == 0x9 && !u) vop = VFP_MUL;
    else if (opcode == 0x1 && !u) vop = VFP_MLA;
    else if (opcode == 0x5 && !u) vop = VFP_MLS;
    else return false;
    if (dbl && (!q || bit(insn, 21) || vop != VFP_MUL)) return false;
    const uint32_t index = dbl ? bit(insn, 11) : ((bit(insn, 11) << 1) | bit(insn, 21));
    const uint32_t rm = bits(insn, 20, 16);
    /* A scalar load and a splat, not v128.load32/64_splat: V8's Liftoff
     * (Node 24 / V8 13) returns zeros for those at memory64 addresses
     * past 4 GiB, where the state lives (docs/JIT.md, "V8 quirks"). */
    lget(c, L_STATE);
    mem(c, dbl ? WASM_OP_F64_LOAD : WASM_OP_F32_LOAD, dbl ? ALIGN_8 : ALIGN_4,
        OFF_V(rm) + index * (dbl ? sizeof(uint64_t) : sizeof(uint32_t)));
    simd(c, dbl ? WASM_SIMD_F64X2_SPLAT : WASM_SIMD_F32X4_SPLAT);
    lset(c, L_VB);
    emit_vector_fp(c, vop, dbl, q, rn, rd);
    return true;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* Integer Advanced SIMD: bitwise ops and permutes, inline.            */
/* ------------------------------------------------------------------ */

/* V[d] = shuffle(V[n], V[m]) by byte lanes (0-15 from n, 16-31 from m). */
static void emit_shuffle_store(Ctx *c, uint32_t n, uint32_t m, const uint8_t lanes[SIMD_LANE_BYTES], bool q,
                               uint32_t d) {
  load_v(c, n);
  load_v(c, m);
  v128_shuffle(c, lanes);
  lset(c, L_VR);
  store_v(c, d, L_VR, q);
}

/* Byte lanes for "result element i = (from_m ? m : n) element src[i]". */
static void element_lanes(uint8_t lanes[SIMD_LANE_BYTES], uint32_t esize_bytes, uint32_t count, const uint8_t *src,
                          const bool *from_m) {
  memset(lanes, 0, SIMD_LANE_BYTES);
  for (uint32_t i = 0; i < count; i++) {
    for (uint32_t b = 0; b < esize_bytes; b++) {
      lanes[i * esize_bytes + b] = (uint8_t)((from_m[i] ? SIMD_LANE_BYTES : 0u) + src[i] * esize_bytes + b);
    }
  }
}

/* An instruction whose result is (Vd & and_mask) | or_mask whatever the
 * other registers hold (MOVI, MVNI, ORR/BIC (immediate), FMOV (vector,
 * immediate)): the interpreter evaluates it at compile time on two
 * scratch states, Vd all zeros and all ones. */
static bool c_vector_affine_immediate(Ctx *c, uint32_t insn) {
  static Interp_State scratch;
  CPU_Vector_Register results[2];
  for (uint32_t k = 0; k < 2u; k++) {
    memset(&scratch, 0, sizeof(scratch));
    const uint64_t fill = k ? ~(uint64_t)0 : 0;
    scratch.v[bits(insn, 4, 0)].lo = fill;
    scratch.v[bits(insn, 4, 0)].hi = fill;
    if (interp_execute(&scratch, insn) != INTERP_CONTINUE) return false;
    results[k] = scratch.v[bits(insn, 4, 0)];
  }
  const uint32_t d = bits(insn, 4, 0);
  load_v(c, d);
  v128_const(c, results[1].lo, results[1].hi);
  simd(c, WASM_SIMD_V128_AND);
  v128_const(c, results[0].lo, results[0].hi);
  simd(c, WASM_SIMD_V128_OR);
  lset(c, L_VR);
  store_v(c, d, L_VR, true);
  return true;
}

#define VECTOR_MAX_LANES 16u

static bool c_vector_int_fast(Ctx *c, uint32_t insn) {
  if (bit(insn, 31)) return false;
  const bool q = bit(insn, 30), u = bit(insn, 29);
  const uint32_t size = bits(insn, 23, 22), rn = bits(insn, 9, 5), rm = bits(insn, 20, 16), rd = bits(insn, 4, 0);
  uint8_t lanes[SIMD_LANE_BYTES];
  uint8_t src[VECTOR_MAX_LANES];
  bool from_m[VECTOR_MAX_LANES];
  if (bits(insn, 28, 24) == 0x0F && bit(insn, 10) && bits(insn, 23, 19) == 0) { /* modified immediate */
    return c_vector_affine_immediate(c, insn);
  }
  if (bits(insn, 28, 24) != 0x0E) return false;
  if (bit(insn, 21) && bit(insn, 10) && bits(insn, 15, 11) == 0x03) { /* AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF */
    load_v(c, rn);
    lset(c, L_VA);
    load_v(c, rm);
    lset(c, L_VB);
    const uint32_t which = ((uint32_t)u << 2) | size;
    if (which >= 5u) {
      load_v(c, rd);
      lset(c, L_VD);
    }
    switch (which) {
    case 0: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_AND); break;
    case 1: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_ANDNOT); break;
    case 2: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_OR); break;
    case 3: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_NOT); simd(c, WASM_SIMD_V128_OR); break;
    case 4: lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_XOR); break;
    case 5: lget(c, L_VA); lget(c, L_VB); lget(c, L_VD); simd(c, WASM_SIMD_V128_BITSELECT); break; /* BSL */
    case 6: lget(c, L_VA); lget(c, L_VD); lget(c, L_VB); simd(c, WASM_SIMD_V128_BITSELECT); break; /* BIT */
    default: lget(c, L_VD); lget(c, L_VA); lget(c, L_VB); simd(c, WASM_SIMD_V128_BITSELECT); break; /* BIF */
    }
    lset(c, L_VR);
    store_v(c, rd, L_VR, q);
    return true;
  }
  if (!bit(insn, 21) && !bit(insn, 15) && !bit(insn, 10) && u) { /* EXT */
    const uint32_t imm4 = bits(insn, 14, 11), bytes = q ? 16u : 8u;
    if (size != 0 || (!q && (imm4 & 8u))) return false;
    for (uint32_t i = 0; i < bytes; i++) {
      src[i] = (uint8_t)((imm4 + i) % bytes);
      from_m[i] = imm4 + i >= bytes;
    }
    element_lanes(lanes, 1, bytes, src, from_m);
    emit_shuffle_store(c, rn, rm, lanes, q, rd);
    return true;
  }
  const uint32_t esize = 1u << size, count = (q ? 16u : 8u) / esize;
  if (!bit(insn, 21) && !bit(insn, 15) && bits(insn, 11, 10) == 2 && !u) { /* UZP/TRN/ZIP */
    const uint32_t opcode = bits(insn, 14, 12), part = (opcode >> 2) & 1u, pairs = count / 2u;
    if (size == 3 && !q) return false;
    switch (opcode & 3u) {
    case 1:
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t s2 = 2u * i + part;
        from_m[i] = s2 >= count;
        src[i] = (uint8_t)(s2 % count);
      }
      break;
    case 2:
      for (uint32_t p = 0; p < pairs; p++) {
        src[2u * p] = src[2u * p + 1u] = (uint8_t)(2u * p + part);
        from_m[2u * p] = false;
        from_m[2u * p + 1u] = true;
      }
      break;
    case 3:
      for (uint32_t p = 0; p < pairs; p++) {
        src[2u * p] = src[2u * p + 1u] = (uint8_t)(part * pairs + p);
        from_m[2u * p] = false;
        from_m[2u * p + 1u] = true;
      }
      break;
    default:
      return false;
    }
    element_lanes(lanes, esize, count, src, from_m);
    emit_shuffle_store(c, rn, rm, lanes, q, rd);
    return true;
  }
  if (bit(insn, 21) && bits(insn, 11, 10) == 2 && bits(insn, 20, 17) == 0) { /* two-reg misc: REV16/32/64 */
    const uint32_t opcode = bits(insn, 16, 12);
    const uint32_t key = ((uint32_t)u << 5) | opcode;
    if (key != 0x00 && key != 0x20 && key != 0x01) return false;
    const uint32_t container = opcode == 0x01 ? 2u : (u ? 4u : 8u); /* bytes */
    if (esize >= container) return false;
    const uint32_t per = container / esize;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t base = (i / per) * per;
      src[i] = (uint8_t)(base + (per - 1u - (i - base)));
      from_m[i] = false;
    }
    element_lanes(lanes, esize, count, src, from_m);
    emit_shuffle_store(c, rn, rn, lanes, q, rd);
    return true;
  }
  if (!bit(insn, 21) && bit(insn, 10) && bits(insn, 23, 21) == 0 && !bit(insn, 15) && !u &&
      bits(insn, 14, 11) == 0) { /* DUP (element) */
    const uint32_t imm5 = bits(insn, 20, 16);
    uint32_t lsize = 0;
    while (lsize < 4u && !((imm5 >> lsize) & 1u)) lsize++;
    if (lsize > 3u || (lsize == 3u && !q)) return false;
    const uint32_t lbytes = 1u << lsize, lcount = (q ? 16u : 8u) / lbytes;
    for (uint32_t i = 0; i < lcount; i++) {
      src[i] = (uint8_t)(imm5 >> (lsize + 1u));
      from_m[i] = false;
    }
    element_lanes(lanes, lbytes, lcount, src, from_m);
    emit_shuffle_store(c, rn, rn, lanes, q, rd);
    return true;
  }
  return false;
}

static bool c_simd_fp(Ctx *c, uint32_t insn) {
  if (c_scalar_fp_fast(c, insn) || c_vector_fp_fast(c, insn) || c_vector_int_fast(c, insn)) return true;
  Sync sync = {0};
  simd_fp_sync(insn, &sync);
  emit_direct_call(c, &sync);
  return true;
}

/* ------------------------------------------------------------------ */
/* SIMD&FP loads and stores (the vector registers live in the state).  */
/* ------------------------------------------------------------------ */

/* v[t] = the `bytes` (1..16) at L_HOST + offset, zero-extended to 128
 * bits (a scalar SIMD&FP write clears the rest of the register). */
static void emit_vector_load(Ctx *c, uint32_t bytes, uint64_t offset, uint32_t t) {
  lget(c, L_STATE);
  lget(c, L_HOST);
  mem(c, load_opcode(ACCESS_LOAD_ZERO, bytes < sizeof(uint64_t) ? bytes : sizeof(uint64_t)), ALIGN_1, offset);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(t));
  lget(c, L_STATE);
  if (bytes > sizeof(uint64_t)) {
    lget(c, L_HOST);
    mem(c, WASM_OP_I64_LOAD, ALIGN_1, offset + V_HIGH_HALF);
  } else {
    i64c(c, 0);
  }
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_V(t) + V_HIGH_HALF);
}

/* The low `bytes` of v[t] to `local` + offset. */
static void emit_vector_store_to(Ctx *c, uint32_t local, uint32_t bytes, uint64_t offset, uint32_t t) {
  const uint32_t low = bytes < sizeof(uint64_t) ? bytes : sizeof(uint64_t);
  lget(c, local);
  state_load64(c, OFF_V(t));
  mem(c, store_opcode(low), ALIGN_1, offset);
  if (bytes > sizeof(uint64_t)) {
    lget(c, local);
    state_load64(c, OFF_V(t) + V_HIGH_HALF);
    mem(c, WASM_OP_I64_STORE, ALIGN_1, offset + V_HIGH_HALF);
  }
}

/* Stores v[t] (and v[t2] after it) of `bytes` each at L_ADDR: inline, or
 * staged in the scratch and written by jit_helper_write (all or nothing).
 * A fault leaves through the interpreter. */
static void emit_vector_store(Ctx *c, uint32_t bytes, uint32_t t, bool pair, uint32_t t2) {
  const uint32_t total = pair ? 2u * bytes : bytes;
  open_block(c); /* $done */
  const uint32_t done = c->depth;
  open_block(c); /* $slow */
  emit_walk(c, total, VMM_PERM_W, c->depth);
  emit_vector_store_to(c, L_HOST, bytes, 0, t);
  if (pair) emit_vector_store_to(c, L_HOST, bytes, bytes, t2);
  br(c, done);
  end_(c);
  lget(c, L_STATE);
  i64c(c, OFF_SCRATCH);
  op(c, WASM_OP_I64_ADD);
  lset(c, L_HOST);
  emit_vector_store_to(c, L_HOST, bytes, 0, t);
  if (pair) emit_vector_store_to(c, L_HOST, bytes, bytes, t2);
  lget(c, L_STATE);
  lget(c, L_ADDR);
  i32c(c, total);
  op(c, WASM_OP_CALL);
  wasm_uleb(c->b, FUNC_WRITE);
  op(c, WASM_OP_I32_EQZ);
  open_if(c, WASM_BLOCK_VOID);
  leave_via_interpreter(c);
  end_(c);
  end_(c); /* $done */
}

/* One SIMD&FP transfer (or pair) at L_ADDR, then the base writeback. */
static void emit_vector_transfer(Ctx *c, bool load, uint32_t bytes, uint32_t t, bool pair, uint32_t t2, uint32_t n,
                                 bool wback, uint64_t offset) {
  if (load) {
    emit_load_address(c, pair ? 2u * bytes : bytes);
    emit_vector_load(c, bytes, 0, t);
    if (pair) emit_vector_load(c, bytes, bytes, t2);
  } else {
    emit_vector_store(c, bytes, t, pair, t2);
  }
  if (wback) {
    lget(c, L_BASE);
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
    set_xsp(c, n);
  }
}

/* L_ADDR = L_BASE (+ offset unless post-index). */
static void address_from_base(Ctx *c, uint32_t n, bool post, uint64_t offset) {
  base_to_locals(c, n);
  lget(c, L_BASE);
  if (!post) {
    i64c(c, offset);
    op(c, WASM_OP_I64_ADD);
  }
  lset(c, L_ADDR);
}

static bool c_vector_load_store(Ctx *c, uint32_t insn) {
  const uint32_t t = bits(insn, 4, 0), n = bits(insn, 9, 5);
  switch (bits(insn, 29, 28)) {
  case 1: { /* LDR (literal, SIMD&FP) */
    const uint32_t opc = bits(insn, 31, 30);
    if (bit(insn, 24) || opc == 3) return false;
    i64c(c, c->pc + (uint64_t)sign_extend((uint64_t)bits(insn, 23, 5) << 2, 21));
    lset(c, L_ADDR);
    emit_vector_transfer(c, true, 4u << opc, t, false, 0, 0, false, 0);
    return true;
  }
  case 2: { /* LDP/STP/LDNP/STNP (SIMD&FP) */
    const uint32_t opc = bits(insn, 31, 30), form = bits(insn, 24, 23);
    if (opc == 3) return false;
    const uint32_t bytes = 4u << opc;
    const uint64_t offset = (uint64_t)sign_extend(bits(insn, 21, 15), 7) * bytes;
    address_from_base(c, n, form == 1, offset);
    emit_vector_transfer(c, bit(insn, 22), bytes, t, true, bits(insn, 14, 10), n, form == 1 || form == 3, offset);
    return true;
  }
  case 3: { /* LDR/STR (SIMD&FP): unsigned offset, imm9, register offset */
    const uint32_t scale = (bits(insn, 23, 23) << 2) | bits(insn, 31, 30);
    if (scale > 4u) return false;
    const uint32_t bytes = 1u << scale;
    const bool load = bit(insn, 22);
    if (bit(insn, 24)) {
      get_xsp(c, n);
      i64c(c, (uint64_t)bits(insn, 21, 10) * bytes);
      op(c, WASM_OP_I64_ADD);
      lset(c, L_ADDR);
      emit_vector_transfer(c, load, bytes, t, false, 0, n, false, 0);
      return true;
    }
    if (bit(insn, 21) == 0) {
      const uint32_t form = bits(insn, 11, 10);
      if (form == 2) return false; /* no unprivileged SIMD&FP form */
      const uint64_t offset = (uint64_t)sign_extend(bits(insn, 20, 12), 9);
      address_from_base(c, n, form == 1, offset);
      emit_vector_transfer(c, load, bytes, t, false, 0, n, form == 1 || form == 3, offset);
      return true;
    }
    if (bits(insn, 11, 10) != 2) return false;
    const uint32_t option = bits(insn, 15, 13);
    if (!(option & 2u)) return false;
    get_xsp(c, n);
    get_x(c, bits(insn, 20, 16));
    if (option == 2) mask32(c);
    else if (option == 6) op(c, WASM_OP_I64_EXTEND32_S);
    if (bit(insn, 12) && scale) {
      i64c(c, scale);
      op(c, WASM_OP_I64_SHL);
    }
    op(c, WASM_OP_I64_ADD);
    lset(c, L_ADDR);
    emit_vector_transfer(c, load, bytes, t, false, 0, n, false, 0);
    return true;
  }
  default: { /* LD1-LD4 & co.: the interpreter's, called directly */
    if (bit(insn, 31)) return false;
    Sync sync = {0};
    sync_xsp(&sync, n);
    sync_x(&sync, bits(insn, 20, 16));
    emit_direct_call(c, &sync);
    return true;
  }
  }
}

/* ------------------------------------------------------------------ */
/* Exclusive and acquire/release (interp_load_store.c, exclusive()).   */
/* ------------------------------------------------------------------ */

#define EXCLUSIVE_GRANULE_MASK (~(uint64_t)(INTERP_EXCLUSIVE_GRANULE - 1u))

/* atomic.fence (threads proposal: 0xFE prefix, opcode 0x03, flags 0). */
#define WASM_ATOMIC_PREFIX 0xFEu
#define WASM_ATOMIC_FENCE 0x03u

/* A full fence, emitted only into code compiled while guest threads run
 * on several host threads (cpu_multicore(), docs/PARALLEL.md): the
 * interpreter's ordering rules, inline. Each core compiles its own code,
 * and the mode never changes while cores exist. */
static void emit_multicore_fence(Ctx *c) {
  if (!cpu_multicore()) return;
  op(c, WASM_ATOMIC_PREFIX);
  op(c, WASM_ATOMIC_FENCE);
  op(c, 0);
}

/* i64.atomic.rmw{8,16,32,}.cmpxchg_u (threads proposal, 0xFE prefix). */
#define WASM_ATOMIC_I64_CMPXCHG 0x49u
#define WASM_ATOMIC_I64_CMPXCHG8_U 0x4Cu
#define WASM_ATOMIC_I64_CMPXCHG16_U 0x4Du
#define WASM_ATOMIC_I64_CMPXCHG32_U 0x4Eu

static uint32_t cmpxchg_opcode(uint32_t size) {
  switch (size) {
  case 1: return WASM_ATOMIC_I64_CMPXCHG8_U;
  case 2: return WASM_ATOMIC_I64_CMPXCHG16_U;
  case 4: return WASM_ATOMIC_I64_CMPXCHG32_U;
  default: return WASM_ATOMIC_I64_CMPXCHG;
  }
}

static uint32_t log2_size(uint32_t size) { return size == 8u ? 3u : size == 4u ? 2u : size == 2u ? 1u : 0u; }

/* Multicore STXR / STLXR of one register (the interpreter's
 * store_exclusive_shared, inline): it passes iff this thread's monitor is
 * set on this address and size and memory still holds what the LDXR read
 * (state->exclusive_value) - one host compare-and-swap, seq_cst, so a
 * store by another core in between makes it fail rather than be lost.
 * Unmapped or read-only pages leave through the interpreter, which faults
 * precisely; a failing store still probes translation, as serially. */
static void emit_store_exclusive_shared(Ctx *c, uint32_t element, uint32_t t, uint32_t rs) {
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD8_U, ALIGN_1, OFF_EXCLUSIVE_VALID);
  state_load64(c, OFF_EXCLUSIVE_ADDRESS);
  lget(c, L_ADDR);
  op(c, WASM_OP_I64_EQ);
  op(c, WASM_OP_I32_AND);
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_EXCLUSIVE_SIZE);
  i32c(c, element);
  op(c, WASM_OP_I32_EQ);
  op(c, WASM_OP_I32_AND);
  open_if(c, WASM_BLOCK_VOID);
  {
    open_block(c); /* $done */
    const uint32_t done = c->depth;
    open_block(c); /* $slow */
    emit_walk(c, element, VMM_PERM_W, c->depth); /* aligned: never crosses a page */
    lget(c, L_HOST);
    lget(c, L_STATE);
    mem(c, load_opcode(ACCESS_LOAD_ZERO, element), ALIGN_1, OFF_EXCLUSIVE_VALUE);
    get_x(c, t);
    op(c, WASM_ATOMIC_PREFIX);
    wasm_uleb(c->b, cmpxchg_opcode(element));
    wasm_uleb(c->b, log2_size(element)); /* atomics need natural alignment */
    wasm_uleb(c->b, 0);
    lget(c, L_STATE);
    mem(c, load_opcode(ACCESS_LOAD_ZERO, element), ALIGN_1, OFF_EXCLUSIVE_VALUE);
    op(c, WASM_OP_I64_EQ);
    lset(c, L_PASS);
    br(c, done);
    end_(c); /* $slow */
    leave_via_interpreter(c);
    end_(c); /* $done */
  }
  else_(c);
  i32c(c, 0);
  lset(c, L_PASS);
  emit_load_address(c, 1u);
  end_(c);
  lget(c, L_STATE);
  i32c(c, 0);
  mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
  lget(c, L_PASS);
  op(c, WASM_OP_I32_EQZ);
  op(c, WASM_OP_I64_EXTEND_I32_U);
  set_x(c, rs);
}

static bool c_exclusive(Ctx *c, uint32_t insn) {
  const uint32_t size = bits(insn, 31, 30);
  const bool o2 = bit(insn, 23), load = bit(insn, 22), o1 = bit(insn, 21), o0 = bit(insn, 15);
  const uint32_t rs = bits(insn, 20, 16), n = bits(insn, 9, 5), t = bits(insn, 4, 0);
  const uint32_t element = 1u << size;
  if (o1) return false;        /* pairs (and LSE CAS): the interpreter */
  if (o2 && !o0) return false; /* LORegion: undefined */

  get_xsp(c, n);
  lset(c, L_ADDR);
  if (element > 1u) { /* alignment fault: the interpreter raises it */
    lget(c, L_ADDR);
    i64c(c, element - 1u);
    op(c, WASM_OP_I64_AND);
    op(c, WASM_OP_I64_EQZ);
    op(c, WASM_OP_I32_EQZ);
    open_if(c, WASM_BLOCK_VOID);
    leave_via_interpreter(c);
    end_(c);
  }
  if (o2) { /* LDAR / STLR (multicore: fenced on both sides, RCsc) */
    emit_multicore_fence(c);
    if (load) {
      emit_load_address(c, element);
      emit_host_load(c, ACCESS_LOAD_ZERO, element, 0, L_VAL);
      lget(c, L_VAL);
      set_x(c, t);
    } else {
      emit_store(c, element, t, false, 0);
    }
    emit_multicore_fence(c);
    return true;
  }
  if (load) { /* LDXR / LDAXR: arm the monitor */
    emit_load_address(c, element);
    emit_host_load(c, ACCESS_LOAD_ZERO, element, 0, L_VAL);
    lget(c, L_STATE);
    i32c(c, 1);
    mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
    state_store64_local(c, OFF_EXCLUSIVE_ADDRESS, L_ADDR);
    lget(c, L_STATE); /* what was read, for the multicore store-exclusive's compare-and-swap */
    i32c(c, element);
    mem(c, WASM_OP_I32_STORE, ALIGN_4, OFF_EXCLUSIVE_SIZE);
    lget(c, L_STATE);
    lget(c, L_VAL);
    mem(c, store_opcode(element), ALIGN_1, OFF_EXCLUSIVE_VALUE);
    lget(c, L_VAL);
    set_x(c, t);
    if (o0) emit_multicore_fence(c); /* LDAXR: acquire */
    return true;
  }
  if (cpu_multicore()) { /* STXR / STLXR: a compare-and-swap */
    emit_store_exclusive_shared(c, element, t, rs);
    return true;
  }
  /* STXR / STLXR: stores iff the monitor holds this granule; the status
   * register and the monitor's clearing happen either way. A failing
   * store still checks translation (a 1-byte read probe). */
  lget(c, L_STATE);
  mem(c, WASM_OP_I32_LOAD8_U, ALIGN_1, OFF_EXCLUSIVE_VALID);
  state_load64(c, OFF_EXCLUSIVE_ADDRESS);
  lget(c, L_ADDR);
  op(c, WASM_OP_I64_XOR);
  i64c(c, EXCLUSIVE_GRANULE_MASK);
  op(c, WASM_OP_I64_AND);
  op(c, WASM_OP_I64_EQZ);
  op(c, WASM_OP_I32_AND);
  ltee(c, L_PASS);
  open_if(c, WASM_BLOCK_VOID);
  emit_store(c, element, t, false, 0);
  else_(c);
  emit_load_address(c, 1u);
  end_(c);
  lget(c, L_STATE);
  i32c(c, 0);
  mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
  lget(c, L_PASS);
  op(c, WASM_OP_I32_EQZ);
  op(c, WASM_OP_I64_EXTEND_I32_U);
  set_x(c, rs);
  return true;
}

static bool c_load_store(Ctx *c, uint32_t insn) {
  if (bit(insn, 26)) return c_vector_load_store(c, insn);
  switch (bits(insn, 29, 28)) {
  case 0: return bit(insn, 24) ? false : c_exclusive(c, insn);
  case 1: return bit(insn, 24) ? false : c_load_literal(c, insn);
  case 2: return c_pair(c, insn);
  default:
    if (bit(insn, 24)) return c_single_unsigned_offset(c, insn);
    if (bit(insn, 21) == 0) return c_single_imm9(c, insn);
    if (bits(insn, 11, 10) == 2) return c_single_register_offset(c, insn);
    return false;
  }
}

/* ------------------------------------------------------------------ */
/* Branches and system.                                                */
/* ------------------------------------------------------------------ */

typedef enum Outcome {
  OUTCOME_NEXT,     /* inlined; the block goes on */
  OUTCOME_HELPER,   /* not inlined; the interpreter runs it, block goes on */
  OUTCOME_END,      /* the block ended (an exit was emitted) */
  OUTCOME_END_HELPER, /* not inlined, and the block ends after it */
} Outcome;

static uint64_t branch_target(Ctx *c, uint32_t insn, unsigned hi, unsigned lo) {
  return c->pc + (uint64_t)sign_extend((uint64_t)bits(insn, hi, lo) << 2, hi - lo + 3u);
}

/* if (condition on stack) target else the next instruction. */
static void exit_conditional(Ctx *c, uint64_t target) {
  open_if(c, WASM_BLOCK_VOID);
  branch_to(c, target);
  else_(c);
  branch_to(c, c->pc + INSN_BYTES);
  end_(c);
}

#define SYSREG_FIELD_MASK (0xFFFFu << 5)
#define DC_ZVA_ENCODING 0xD50B7420u /* SYS #3, C7, C4, #1 with Rt = 0 */
#define DC_ZVA_BYTES 64u

static Outcome c_system(Ctx *c, uint32_t insn) {
  const bool read = bit(insn, 21);
  const uint32_t op0 = bits(insn, 20, 19), op1 = bits(insn, 18, 16);
  const uint32_t crn = bits(insn, 15, 12), op2 = bits(insn, 7, 5), rt = bits(insn, 4, 0);
  if (op0 == 0) {
    if (read || op1 != 3 || rt != REG_ZR) return OUTCOME_END_HELPER;
    if (crn == 2) return OUTCOME_NEXT; /* hints */
    if (crn == 3) {
      if (op2 == 2) { /* CLREX */
        lget(c, L_STATE);
        i32c(c, 0);
        mem(c, WASM_OP_I32_STORE8, ALIGN_1, OFF_EXCLUSIVE_VALID);
        return OUTCOME_NEXT;
      }
      if (op2 >= 4 && op2 != 7) { /* DSB, DMB, ISB */
        if (op2 != 6) emit_multicore_fence(c); /* not ISB */
        return OUTCOME_NEXT;
      }
    }
    return OUTCOME_END_HELPER;
  }
  if (op0 == 1) {
    if ((insn & ~(uint32_t)REG_ZR) == DC_ZVA_ENCODING) { /* zero the 64-byte block (DCZID_EL0.BS = 4) */
      get_x(c, rt);
      i64c(c, ~(uint64_t)(DC_ZVA_BYTES - 1u));
      op(c, WASM_OP_I64_AND);
      lset(c, L_ADDR);
      open_block(c); /* $done */
      const uint32_t done = c->depth;
      open_block(c); /* $slow: aligned, so only a fault - the interpreter raises it */
      emit_walk(c, DC_ZVA_BYTES, VMM_PERM_W, c->depth);
      for (uint32_t i = 0; i < DC_ZVA_BYTES; i += (uint32_t)sizeof(uint64_t)) {
        lget(c, L_HOST);
        i64c(c, 0);
        mem(c, WASM_OP_I64_STORE, ALIGN_1, i);
      }
      br(c, done);
      end_(c);
      leave_via_interpreter(c);
      end_(c);
      return OUTCOME_NEXT;
    }
    return OUTCOME_END_HELPER; /* SYS: IC IVAU (flushes code), other cache maintenance */
  }
  const uint32_t reg = insn & SYSREG_FIELD_MASK;
  if (read) {
    switch (reg) {
    case CPU_SYSREG_TPIDRRO_EL0: state_load64(c, OFF_TPIDRRO); break;
    case CPU_SYSREG_TPIDR_EL0: state_load64(c, OFF_TPIDR); break;
    case CPU_SYSREG_NZCV: load_flags(c); op(c, WASM_OP_I64_EXTEND_I32_U); break;
    case CPU_SYSREG_FPCR:
      lget(c, L_STATE);
      mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_FPCR);
      op(c, WASM_OP_I64_EXTEND_I32_U);
      break;
    case CPU_SYSREG_FPSR:
      lget(c, L_STATE);
      mem(c, WASM_OP_I32_LOAD, ALIGN_4, OFF_FPSR);
      op(c, WASM_OP_I64_EXTEND_I32_U);
      break;
    default: return OUTCOME_HELPER; /* counters (exact cycles), CTR, DCZID, unknown */
    }
    set_x(c, rt);
    return OUTCOME_NEXT;
  }
  switch (reg) {
  case CPU_SYSREG_NZCV:
    get_x(c, rt);
    op(c, WASM_OP_I32_WRAP_I64);
    i32c(c, CPU_PSTATE_NZCV_MASK);
    op(c, WASM_OP_I32_AND);
    store_flags(c);
    return OUTCOME_NEXT;
  case CPU_SYSREG_TPIDR_EL0:
    lget(c, L_STATE);
    get_x(c, rt);
    mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_TPIDR);
    return OUTCOME_NEXT;
  default:
    return OUTCOME_HELPER;
  }
}

static Outcome c_branch_system(Ctx *c, uint32_t insn) {
  if (bits(insn, 30, 26) == 0x05) { /* B, BL */
    if (bit(insn, 31)) { /* BL: into the callee in the region if it can be there, back at the return site */
      if (g_interp_trace_count) return OUTCOME_END_HELPER;
      const uint64_t target = branch_target(c, insn, 25, 0), back = c->pc + INSN_BYTES;
      i64c(c, back);
      set_x(c, CPU_REG_X30);
      if (c->discover && add_block(c, target) && add_block(c, back)) c->blocks[find_block(c, back)].return_site = true;
      branch_to(c, target);
      return OUTCOME_END;
    }
    branch_to(c, branch_target(c, insn, 25, 0));
    return OUTCOME_END;
  }
  if (bits(insn, 30, 25) == 0x1A) { /* CBZ, CBNZ */
    get_xw(c, bits(insn, 4, 0), bit(insn, 31));
    op(c, WASM_OP_I64_EQZ);
    if (bit(insn, 24)) op(c, WASM_OP_I32_EQZ);
    exit_conditional(c, branch_target(c, insn, 23, 5));
    return OUTCOME_END;
  }
  if (bits(insn, 30, 25) == 0x1B) { /* TBZ, TBNZ */
    get_x(c, bits(insn, 4, 0));
    i64c(c, (uint64_t)1 << ((bit(insn, 31) << 5) | bits(insn, 23, 19)));
    op(c, WASM_OP_I64_AND);
    op(c, WASM_OP_I64_EQZ);
    if (bit(insn, 24)) op(c, WASM_OP_I32_EQZ);
    exit_conditional(c, branch_target(c, insn, 18, 5));
    return OUTCOME_END;
  }
  if (bits(insn, 31, 24) == 0x54) { /* B.cond */
    if (bit(insn, 4)) return OUTCOME_END_HELPER;
    emit_condition(c, bits(insn, 3, 0));
    exit_conditional(c, branch_target(c, insn, 23, 5));
    return OUTCOME_END;
  }
  if (bits(insn, 31, 22) == 0x354) return c_system(c, insn);
  if (bits(insn, 31, 25) == 0x6B) { /* BR, BLR, RET */
    const uint32_t opc = bits(insn, 24, 21);
    if (bits(insn, 20, 16) != 0x1F || bits(insn, 15, 10) != 0 || bits(insn, 4, 0) != 0 || opc > 2u) {
      return OUTCOME_END_HELPER;
    }
    if (g_interp_trace_count && opc != 0) return OUTCOME_END_HELPER;
    const uint32_t rn = bits(insn, 9, 5);
    const bool predicted = opc == 0 && rn != REG_ZR && (c->predicted_mask & (1u << rn));
    const uint64_t prediction = predicted ? c->predicted_value[rn] : 0;
    get_x(c, rn);
    lset(c, L_NPC);
    if (opc == 1) {
      i64c(c, c->pc + INSN_BYTES);
      set_x(c, CPU_REG_X30);
    }
    if (predicted && c->discover) (void)add_block(c, prediction);
    /* Guarded internal targets: a RET to a return site in the region, a
     * BR to the predicted PLT target; anything else leaves. */
    for (uint32_t i = 0; i < c->block_count && opc != 1; i++) {
      const bool candidate = opc == 2 ? c->blocks[i].return_site : (predicted && c->blocks[i].pc == prediction);
      if (!candidate) continue;
      lget(c, L_NPC);
      i64c(c, c->blocks[i].pc);
      op(c, WASM_OP_I64_EQ);
      open_if(c, WASM_BLOCK_VOID);
      branch_to(c, c->blocks[i].pc);
      end_(c);
    }
    lget(c, L_NPC);
    exit_to_stack(c);
    return OUTCOME_END;
  }
  return OUTCOME_END_HELPER; /* SVC, BRK, undefined */
}

/* ------------------------------------------------------------------ */
/* Blocks and modules.                                                 */
/* ------------------------------------------------------------------ */

static Outcome compile_instruction(Ctx *c, uint32_t insn) {
  switch (bits(insn, 28, 25)) {
  case 0x8: case 0x9: return c_dp_immediate(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  case 0xA: case 0xB: return c_branch_system(c, insn);
  case 0x4: case 0x6: case 0xC: case 0xE: return c_load_store(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  case 0x5: case 0xD: return c_dp_register(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  case 0x7: case 0xF: return c_simd_fp(c, insn) ? OUTCOME_NEXT : OUTCOME_HELPER;
  default: return OUTCOME_END_HELPER; /* SVE, unallocated: undefined */
  }
}

/* Region block `b`'s instructions; returns how many. */
static uint32_t compile_block_code(Ctx *c, uint32_t b) {
  const uint64_t start = c->blocks[b].pc;
  const uint64_t page = start & ~(uint64_t)VMM_PAGE_OFFSET_MASK;
  int32_t p = -1;
  for (uint32_t i = 0; i < c->page_count; i++) {
    if (c->pages[i].base == page) p = (int32_t)i;
  }
  SWITCH_ASSERT_ALWAYS(p >= 0, "jit: region block outside the region's pages");
  const uint32_t *code = c->pages[p].code + ((start - page) / INSN_BYTES);
  const uint32_t available = (uint32_t)((page + VMM_PAGE_SIZE - start) / INSN_BYTES);
  c->known_mask = 0;
  c->predicted_mask = 0;
  const uint32_t count = available < c->max_block_insns ? available : c->max_block_insns;
  c->block_len = c->blocks[b].length;
  c->pc = start;
  c->flag_kind = FLAGS_LIVE; /* entering edges materialized them, or the block writes them first */
  c->flag_event = FLAG_EVENT_NONE;
  for (uint32_t i = 0; i < count; i++, c->pc += INSN_BYTES) {
    c->insn = code[i];
    c->index = i;
    const uint32_t mark = c->b->length;
    const uint64_t used = c->used, written = c->written;
    const uint32_t depth = c->depth, blocks = c->block_count;
    Outcome outcome = compile_instruction(c, c->insn);
    if (outcome == OUTCOME_HELPER || outcome == OUTCOME_END_HELPER) {
      /* Throw away anything a decoder emitted before giving up. */
      c->b->length = mark;
      c->used = used;
      c->written = written;
      c->depth = depth;
      c->block_count = blocks;
    }
    switch (outcome) {
    case OUTCOME_NEXT: break;
    case OUTCOME_HELPER: helper_and_continue(c); return i + 1u;
    case OUTCOME_END: return i + 1u;
    case OUTCOME_END_HELPER: leave_via_interpreter(c); return i + 1u;
    }
  }
  /* Ran off the end (block limit or page end): on to the next pc. */
  branch_to(c, c->pc);
  return count;
}

/* ...and if the next block is compiled, validated in the current code
 * generation and fits the remaining budget, tail-call it (the dispatcher
 * in jit.c makes the same checks). L_T0 holds cycles_consumed. */
static void emit_chain(Ctx *c) {
  lget(c, L_NPC);
  i64c(c, JIT_INSN_SHIFT);
  op(c, WASM_OP_I64_SHR_U);
  i64c(c, JIT_HASH_MULTIPLIER);
  op(c, WASM_OP_I64_MUL);
  i64c(c, JIT_HASH_BITS - JIT_CACHE_BITS);
  op(c, WASM_OP_I64_SHR_U);
  i64c(c, sizeof(Jit_Entry));
  op(c, WASM_OP_I64_MUL);
  i64c(c, c->link->cache_address);
  op(c, WASM_OP_I64_ADD);
  lset(c, L_HOST); /* the entry */
  open_block(c);
  const uint32_t miss = c->depth;
  lget(c, L_HOST);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, ENTRY_OFFSET(pc));
  lget(c, L_NPC);
  op(c, WASM_OP_I64_NE);
  br_if(c, miss);
  lget(c, L_HOST);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, ENTRY_OFFSET(generation));
  i64c(c, c->link->generation_address);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, 0);
  op(c, WASM_OP_I64_NE);
  br_if(c, miss);
  lget(c, L_T0);
  lget(c, L_HOST);
  mem(c, WASM_OP_I64_LOAD32_U, ALIGN_4, ENTRY_OFFSET(length));
  op(c, WASM_OP_I64_ADD);
  state_load64(c, OFF_BUDGET);
  op(c, WASM_OP_I64_GT_U);
  br_if(c, miss);
  lget(c, L_HOST);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, ENTRY_OFFSET(function));
  ltee(c, L_ADDR);
  op(c, WASM_OP_I64_EQZ);
  br_if(c, miss);
  lget(c, L_STATE);
  lget(c, L_ADDR);
  op(c, WASM_OP_RETURN_CALL_INDIRECT);
  wasm_uleb(c->b, TYPE_BLOCK);
  wasm_uleb(c->b, TABLE_INDEX);
  end_(c);
}

static void emit_function(Ctx *c) {
  /* Locals: runs of i64, i32, f32, f64, v128 - only as far as the
   * analysis pass saw locals used (engines zero every declared local on
   * each call, and tiny regions are called millions of times). */
  static const uint32_t group_last[5] = {L_LAST_I64, L_LAST_I32, L_LAST_F32, L_LAST_F64, L_LAST_V128};
  static const uint8_t group_type[5] = {WASM_TYPE_I64, WASM_TYPE_I32, WASM_TYPE_F32, WASM_TYPE_F64, WASM_TYPE_V128};
  const uint32_t limit = c->discover ? L_LAST_V128 : c->declared_local;
  uint32_t groups = 0, first = L_STATE + 1u;
  for (uint32_t g = 0; g < 5u; g++) {
    if (first <= limit) groups++;
    first = group_last[g] + 1u;
  }
  wasm_uleb(c->b, groups);
  first = L_STATE + 1u;
  for (uint32_t g = 0; g < 5u; g++) {
    if (first <= limit) {
      const uint32_t last = group_last[g] < limit ? group_last[g] : limit;
      wasm_uleb(c->b, last - first + 1u);
      wasm_u8(c->b, group_type[g]);
    }
    first = group_last[g] + 1u;
  }

  /* Prologue. The caller checked that block 0 fits the budget. */
  if (c->link->count_entries) {
    const uint64_t entry = c->link->cache_address + jit_cache_index(c->blocks[0].pc) * sizeof(Jit_Entry);
    i64c(c, entry);
    i64c(c, entry);
    mem(c, WASM_OP_I64_LOAD, ALIGN_8, ENTRY_OFFSET(entries));
    i64c(c, 1);
    op(c, WASM_OP_I64_ADD);
    mem(c, WASM_OP_I64_STORE, ALIGN_8, ENTRY_OFFSET(entries));
  }
  state_load64(c, OFF_L1);
  lset(c, L_L1);
  reload(c);
  i64c(c, c->blocks[0].length);
  lset(c, L_CYC);
  compute_room(c);

  c->depth = 0;
  open_block(c); /* $leave */
  open_block(c); /* $exit */
  op(c, WASM_OP_LOOP);
  op(c, WASM_BLOCK_VOID);
  c->depth++; /* $dispatch */
  /* Dispatch targets: the region blocks, then $helper. Discovery appends
   * blocks while compiling; the final pass has them all. */
  const bool helper = !c->discover && c->uses_helper;
  const uint32_t targets = c->discover ? 1u : c->block_count + (helper ? 1u : 0u);
  if (targets > 1u) {
    for (uint32_t i = 0; i < targets; i++) open_block(c);
    lget(c, L_IDX);
    op(c, WASM_OP_BR_TABLE);
    wasm_uleb(c->b, targets);
    for (uint32_t i = 0; i < targets; i++) wasm_uleb(c->b, i);
    wasm_uleb(c->b, 0);
  }
  for (uint32_t b = 0; b < c->block_count; b++) {
    if (targets > 1u) end_(c);
    const uint32_t length = compile_block_code(c, b);
    if (c->discover) {
      c->blocks[b].length = length;
      c->blocks[b].needs_flags = c->flag_event != FLAG_EVENT_WRITE;
    }
  }
  if (helper) {
    /* $helper: the instruction in L_HPC/L_HINSN (its block's last,
     * pre-counted) through the interpreter, then on to L_RESUME. Every
     * call site materialized the flags, and the reload brings them back. */
    end_(c);
    c->flag_kind = FLAGS_LIVE;
    spill(c);
    i64c(c, 1);
    flush_cycles(c);
    call_interpreter(c);
    ltee(c, L_STATUS);
    open_if(c, WASM_BLOCK_VOID);
    lget(c, L_STATUS);
    op(c, WASM_OP_RETURN);
    end_(c);
    reload(c);
    compute_room(c);
    lget(c, L_HNEXTLEN);
    lget(c, L_ROOM);
    op(c, WASM_OP_I64_GT_U);
    open_if(c, WASM_BLOCK_VOID);
    i64c(c, 0);
    lset(c, L_CYC);
    lget(c, L_HNEXT);
    exit_to_stack(c);
    end_(c);
    lget(c, L_HNEXTLEN);
    lset(c, L_CYC);
    lget(c, L_RESUME);
    lset(c, L_IDX);
    br(c, LEVEL_DISPATCH);
  }
  op(c, WASM_OP_UNREACHABLE); /* every block ends in a branch */
  end_(c); /* $dispatch */
  end_(c); /* $exit */

  /* Epilogue: the state is complete... */
  spill(c);
  state_store64_local(c, OFF_PC, L_NPC);
  lget(c, L_STATE);
  lget(c, L_STATE);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, OFF_TOTAL_CYCLES);
  lget(c, L_CYC);
  op(c, WASM_OP_I64_ADD);
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_TOTAL_CYCLES);
  lget(c, L_STATE);
  lget(c, L_STATE);
  mem(c, WASM_OP_I64_LOAD, ALIGN_8, OFF_CYCLES);
  lget(c, L_CYC);
  op(c, WASM_OP_I64_ADD);
  ltee(c, L_T0); /* cycles_consumed */
  mem(c, WASM_OP_I64_STORE, ALIGN_8, OFF_CYCLES);
  emit_chain(c);
  i32c(c, JIT_BLOCK_CONTINUE);
  op(c, WASM_OP_RETURN);
  end_(c); /* $leave: the interpreter takes the instruction, and its status is ours */
  spill(c);
  lget(c, L_HREST);
  flush_cycles(c);
  call_interpreter(c);
  op(c, WASM_OP_END);
}

static void name(Wasm_Buf *b, const char *text) {
  const uint32_t length = (uint32_t)strlen(text);
  wasm_uleb(b, length);
  wasm_bytes(b, text, length);
}

static void emit_module_header(Wasm_Buf *b, uint64_t memory_pages) {
  wasm_bytes(b, WASM_MAGIC, WASM_MAGIC_BYTES);
  const uint8_t version[WASM_VERSION_BYTES] = {WASM_VERSION, 0, 0, 0};
  wasm_bytes(b, version, WASM_VERSION_BYTES);

  wasm_u8(b, WASM_SECTION_TYPE);
  uint32_t size = wasm_reserve_size(b);
  wasm_uleb(b, TYPE_COUNT);
  wasm_u8(b, WASM_TYPE_FUNC); /* TYPE_BLOCK: (i64) -> i32 */
  wasm_uleb(b, 1);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_uleb(b, 1);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_u8(b, WASM_TYPE_FUNC); /* TYPE_INTERPRET: (i64, i32) -> i32 */
  wasm_uleb(b, 2);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_uleb(b, 1);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_u8(b, WASM_TYPE_FUNC); /* TYPE_READ: (i64, i64, i32) -> i32 */
  wasm_uleb(b, 3);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_uleb(b, 1);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_u8(b, WASM_TYPE_FUNC); /* TYPE_STORE: (i64, i64, i32, i64, i64) -> i32 */
  wasm_uleb(b, 5);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_u8(b, WASM_TYPE_I64);
  wasm_uleb(b, 1);
  wasm_u8(b, WASM_TYPE_I32);
  wasm_patch_size(b, size);

  wasm_u8(b, WASM_SECTION_IMPORT);
  size = wasm_reserve_size(b);
  wasm_uleb(b, IMPORT_COUNT);
  name(b, "env");
  name(b, "table");
  wasm_u8(b, WASM_EXTERNAL_TABLE);
  wasm_u8(b, WASM_REFTYPE_FUNCREF);
  wasm_u8(b, WASM_LIMITS_MEMORY64); /* 64-bit indices, no maximum */
  wasm_uleb(b, 0);
  name(b, "env");
  name(b, "interpret");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, TYPE_INTERPRET);
  name(b, "env");
  name(b, "read");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, TYPE_READ);
  name(b, "env");
  name(b, "store");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, TYPE_STORE);
  name(b, "env");
  name(b, "write");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, TYPE_READ);
  name(b, "env");
  name(b, "simd");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, TYPE_INTERPRET);
  name(b, "env");
  name(b, "memory");
  wasm_u8(b, WASM_EXTERNAL_MEMORY);
  wasm_u8(b, WASM_LIMITS_HAS_MAX | WASM_LIMITS_SHARED | WASM_LIMITS_MEMORY64);
  wasm_uleb(b, memory_pages);
  wasm_uleb(b, memory_pages);
  wasm_patch_size(b, size);

  wasm_u8(b, WASM_SECTION_FUNCTION);
  size = wasm_reserve_size(b);
  wasm_uleb(b, 1);
  wasm_uleb(b, TYPE_BLOCK);
  wasm_patch_size(b, size);

  wasm_u8(b, WASM_SECTION_EXPORT);
  size = wasm_reserve_size(b);
  wasm_uleb(b, 1);
  name(b, "b");
  wasm_u8(b, WASM_EXTERNAL_FUNCTION);
  wasm_uleb(b, FUNC_BLOCK);
  wasm_patch_size(b, size);
}

/* Analysis output is discarded; it only needs room. */
#define JIT_ANALYSIS_BYTES (256u * 1024u)
static uint8_t g_analysis[JIT_ANALYSIS_BYTES];

bool jit_compile_block(uint64_t pc, const Jit_Code_Source *source, uint64_t memory_pages, const Jit_Link *link,
                       uint8_t *out, uint32_t capacity, Jit_Compiled *result) {
  static Ctx analysis, c;
  uint32_t max_blocks = JIT_MAX_REGION_BLOCKS, max_block_insns = JIT_MAX_BLOCK_INSNS;
  for (;;) {
    /* Pass 1: the region's blocks and pages, and which registers they touch. */
    Wasm_Buf scratch = wasm_buf(g_analysis, JIT_ANALYSIS_BYTES);
    memset(&analysis, 0, sizeof(analysis));
    analysis.b = &scratch;
    analysis.link = link;
    analysis.discover = true;
    analysis.source = source;
    analysis.max_blocks = max_blocks;
    analysis.max_block_insns = max_block_insns;
    if (region_page(&analysis, pc & ~(uint64_t)VMM_PAGE_OFFSET_MASK) < 0) return false;
    analysis.blocks[0].pc = pc;
    analysis.block_count = 1;
    emit_function(&analysis);

    /* Pass 2: the module. */
    Wasm_Buf b = wasm_buf(out, capacity);
    emit_module_header(&b, memory_pages);
    wasm_u8(&b, WASM_SECTION_CODE);
    const uint32_t section = wasm_reserve_size(&b);
    wasm_uleb(&b, 1);
    const uint32_t body = wasm_reserve_size(&b);
    memset(&c, 0, sizeof(c));
    c.b = &b;
    c.link = link;
    c.source = source;
    memcpy(c.pages, analysis.pages, sizeof(c.pages));
    c.page_count = analysis.page_count;
    c.max_blocks = max_blocks;
    c.max_block_insns = max_block_insns;
    memcpy(c.blocks, analysis.blocks, sizeof(c.blocks));
    c.block_count = analysis.block_count;
    c.uses_helper = analysis.uses_helper;
    c.declared_local = analysis.max_local;
    c.all_used = analysis.used | analysis.written;
    c.all_written = analysis.written;
    emit_function(&c);
    wasm_patch_size(&b, body);
    wasm_patch_size(&b, section);
    if (!b.overflow && !scratch.overflow) {
      /* One code range per page: the span of the blocks in it. */
      result->range_count = c.page_count;
      for (uint32_t p = 0; p < c.page_count; p++) {
        uint64_t low = UINT64_MAX, high = 0;
        for (uint32_t i = 0; i < c.block_count; i++) {
          const uint64_t start = c.blocks[i].pc, end = start + (uint64_t)c.blocks[i].length * INSN_BYTES;
          if ((start & ~(uint64_t)VMM_PAGE_OFFSET_MASK) != c.pages[p].base) continue;
          if (start < low) low = start;
          if (end > high) high = end;
        }
        if (low == UINT64_MAX) low = high = c.pages[p].base; /* a page with no surviving block */
        result->ranges[p].start = low;
        result->ranges[p].words = (uint32_t)((high - low) / INSN_BYTES);
      }
      result->instructions = c.blocks[0].length;
      result->blocks = c.block_count;
      result->module_bytes = b.length;
      return true;
    }
    /* Too big: a smaller region, then shorter blocks. */
    if (max_blocks > 1u) max_blocks /= 2u;
    else if (max_block_insns > 1u) max_block_insns /= 2u;
    else return false;
  }
}
