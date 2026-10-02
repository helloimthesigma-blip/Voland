/**
 * Maxwell shader decode + interpreter. See maxwell_shader.h.
 *
 * Field notation below: "@n" is the bit position in the 64-bit word.
 * Common fields: Rd @0, Ra @8, predicate guard @16 (3 bits + negate @19),
 * Rb @20, Rc @39, c[@34][@20 * 4] constant operands, 19-bit immediates
 * @20 with the sign (bit 19) @56.
 */
#include "gpu/maxwell_shader.h"

#include <math.h>
#include <string.h>

#define BITS(w, at, n) ((uint32_t)(((w) >> (at)) & ((1ull << (n)) - 1ull)))
#define BIT(w, at) ((uint32_t)(((w) >> (at)) & 1ull))

#define REG_D(w) BITS(w, 0, 8)
#define REG_A(w) BITS(w, 8, 8)
#define REG_B(w) BITS(w, 20, 8)
#define REG_C(w) BITS(w, 39, 8)

#define STACK_SSY 1u
#define STACK_PBK 2u
#define STACK_PCNT 3u

/* ---- decode table ------------------------------------------------- */

typedef struct Op_Pattern {
  uint16_t mask;   /* over bits 48-63 */
  uint16_t match;
  uint8_t op;
  uint8_t form;
  uint8_t float_imm; /* 19-bit immediate is the top of an f32 */
} Op_Pattern;

#define R SM_FORM_REG
#define C SM_FORM_CBUF
#define I SM_FORM_IMM
#define L SM_FORM_IMM32
#define X SM_FORM_REG_CBUF

/* Order matters: narrower patterns first where ranges overlap. */
static const Op_Pattern k_patterns[] = {
    /* float */
    {0xfff8, 0x5c58, SM_OP_FADD, R, 1}, {0xfff8, 0x4c58, SM_OP_FADD, C, 1}, {0xfef8, 0x3858, SM_OP_FADD, I, 1},
    {0xfc00, 0x0800, SM_OP_FADD32I, L, 1},
    {0xfff8, 0x5c68, SM_OP_FMUL, R, 1}, {0xfff8, 0x4c68, SM_OP_FMUL, C, 1}, {0xfef8, 0x3868, SM_OP_FMUL, I, 1},
    {0xff00, 0x1e00, SM_OP_FMUL32I, L, 1},
    {0xff80, 0x5980, SM_OP_FFMA, R, 1}, {0xff80, 0x4980, SM_OP_FFMA, C, 1}, {0xfe80, 0x3280, SM_OP_FFMA, I, 1},
    {0xff80, 0x5180, SM_OP_FFMA, X, 1}, {0xfc00, 0x0c00, SM_OP_FFMA32I, L, 1},
    {0xfff8, 0x5c60, SM_OP_FMNMX, R, 1}, {0xfff8, 0x4c60, SM_OP_FMNMX, C, 1}, {0xfef8, 0x3860, SM_OP_FMNMX, I, 1},
    {0xfff0, 0x5bb0, SM_OP_FSETP, R, 1}, {0xfff0, 0x4bb0, SM_OP_FSETP, C, 1}, {0xfef0, 0x36b0, SM_OP_FSETP, I, 1},
    {0xfff0, 0x5ba0, SM_OP_FCMP, R, 1}, {0xfff0, 0x4ba0, SM_OP_FCMP, C, 1}, {0xfef0, 0x36a0, SM_OP_FCMP, I, 1},
    {0xfff0, 0x53a0, SM_OP_FCMP, X, 1},
    {0xff00, 0x5800, SM_OP_FSET, R, 1}, {0xff00, 0x4800, SM_OP_FSET, C, 1}, {0xfe00, 0x3000, SM_OP_FSET, I, 1},
    {0xfff8, 0x5080, SM_OP_MUFU, R, 0},
    {0xfff8, 0x5c90, SM_OP_RRO, R, 1}, {0xfff8, 0x4c90, SM_OP_RRO, C, 1}, {0xfef8, 0x3890, SM_OP_RRO, I, 1},
    {0xfff8, 0x50f8, SM_OP_FSWZADD, R, 0},
    {0xfff8, 0x5ca8, SM_OP_F2F, R, 1}, {0xfff8, 0x4ca8, SM_OP_F2F, C, 1}, {0xfef8, 0x38a8, SM_OP_F2F, I, 1},
    {0xfff8, 0x5cb0, SM_OP_F2I, R, 1}, {0xfff8, 0x4cb0, SM_OP_F2I, C, 1}, {0xfef8, 0x38b0, SM_OP_F2I, I, 1},
    {0xfff8, 0x5cb8, SM_OP_I2F, R, 0}, {0xfff8, 0x4cb8, SM_OP_I2F, C, 0}, {0xfef8, 0x38b8, SM_OP_I2F, I, 0},
    {0xfff8, 0x5ce0, SM_OP_I2I, R, 0}, {0xfff8, 0x4ce0, SM_OP_I2I, C, 0}, {0xfef8, 0x38e0, SM_OP_I2I, I, 0},
    /* integer */
    {0xfff8, 0x5c10, SM_OP_IADD, R, 0}, {0xfff8, 0x4c10, SM_OP_IADD, C, 0}, {0xfef8, 0x3810, SM_OP_IADD, I, 0},
    {0xfe00, 0x1c00, SM_OP_IADD32I, L, 0},
    {0xfff8, 0x5c18, SM_OP_ISCADD, R, 0}, {0xfff8, 0x4c18, SM_OP_ISCADD, C, 0}, {0xfef8, 0x3818, SM_OP_ISCADD, I, 0},
    {0xfff8, 0x5c20, SM_OP_IMNMX, R, 0}, {0xfff8, 0x4c20, SM_OP_IMNMX, C, 0}, {0xfef8, 0x3820, SM_OP_IMNMX, I, 0},
    {0xfff0, 0x5b50, SM_OP_ISET, R, 0}, {0xfff0, 0x4b50, SM_OP_ISET, C, 0}, {0xfef0, 0x3650, SM_OP_ISET, I, 0},
    {0xfff0, 0x5b60, SM_OP_ISETP, R, 0}, {0xfff0, 0x4b60, SM_OP_ISETP, C, 0}, {0xfef0, 0x3660, SM_OP_ISETP, I, 0},
    {0xfff0, 0x5b40, SM_OP_ICMP, R, 0}, {0xfff0, 0x4b40, SM_OP_ICMP, C, 0}, {0xfef0, 0x3640, SM_OP_ICMP, I, 0},
    {0xfff0, 0x5340, SM_OP_ICMP, X, 0},
    {0xfff8, 0x5c38, SM_OP_IMUL, R, 0}, {0xfff8, 0x4c38, SM_OP_IMUL, C, 0}, {0xfef8, 0x3838, SM_OP_IMUL, I, 0},
    {0xff00, 0x1f00, SM_OP_IMUL32I, L, 0},
    {0xff80, 0x5a00, SM_OP_IMAD, R, 0}, {0xff80, 0x4a00, SM_OP_IMAD, C, 0}, {0xfe80, 0x3400, SM_OP_IMAD, I, 0},
    {0xff80, 0x5200, SM_OP_IMAD, X, 0},
    {0xffc0, 0x5b00, SM_OP_XMAD, R, 0}, {0xfe00, 0x4e00, SM_OP_XMAD, C, 0}, {0xffc0, 0x3600, SM_OP_XMAD, I, 0},
    {0xff80, 0x5100, SM_OP_XMAD, X, 0},
    {0xfff8, 0x5c40, SM_OP_LOP, R, 0}, {0xfff8, 0x4c40, SM_OP_LOP, C, 0}, {0xfef8, 0x3840, SM_OP_LOP, I, 0},
    {0xfc00, 0x0400, SM_OP_LOP32I, L, 0},
    {0xfff8, 0x5be0, SM_OP_LOP3, R, 0}, {0xfe00, 0x0200, SM_OP_LOP3, C, 0}, {0xfe00, 0x3c00, SM_OP_LOP3, I, 0},
    {0xfff8, 0x5c48, SM_OP_SHL, R, 0}, {0xfff8, 0x4c48, SM_OP_SHL, C, 0}, {0xfef8, 0x3848, SM_OP_SHL, I, 0},
    {0xfff8, 0x5c28, SM_OP_SHR, R, 0}, {0xfff8, 0x4c28, SM_OP_SHR, C, 0}, {0xfef8, 0x3828, SM_OP_SHR, I, 0},
    {0xfff8, 0x5c00, SM_OP_BFE, R, 0}, {0xfff8, 0x4c00, SM_OP_BFE, C, 0}, {0xfef8, 0x3800, SM_OP_BFE, I, 0},
    {0xfff8, 0x5bf0, SM_OP_BFI, R, 0}, {0xfff8, 0x4bf0, SM_OP_BFI, C, 0}, {0xfef8, 0x36f0, SM_OP_BFI, I, 0},
    {0xfff8, 0x53f0, SM_OP_BFI, X, 0},
    {0xfff8, 0x5c08, SM_OP_POPC, R, 0}, {0xfff8, 0x4c08, SM_OP_POPC, C, 0}, {0xfef8, 0x3808, SM_OP_POPC, I, 0},
    {0xfff8, 0x5c30, SM_OP_FLO, R, 0}, {0xfff8, 0x4c30, SM_OP_FLO, C, 0}, {0xfef8, 0x3830, SM_OP_FLO, I, 0},
    {0xfff8, 0x5bc0, SM_OP_PRMT, R, 0}, {0xfff8, 0x4bc0, SM_OP_PRMT, C, 0}, {0xfef8, 0x36c0, SM_OP_PRMT, I, 0},
    {0xfff8, 0x53c0, SM_OP_PRMT, X, 0},
    {0xfff8, 0x5ca0, SM_OP_SEL, R, 0}, {0xfff8, 0x4ca0, SM_OP_SEL, C, 0}, {0xfef8, 0x38a0, SM_OP_SEL, I, 0},
    {0xfff8, 0x5c98, SM_OP_MOV, R, 0}, {0xfff8, 0x4c98, SM_OP_MOV, C, 0}, {0xfef8, 0x3898, SM_OP_MOV, I, 0},
    {0xfff0, 0x0100, SM_OP_MOV32I, L, 0},
    {0xfff8, 0x5090, SM_OP_PSETP, R, 0},
    {0xfff8, 0x5ce8, SM_OP_P2R, R, 0}, {0xfff8, 0x4ce8, SM_OP_P2R, C, 0}, {0xfef8, 0x38e8, SM_OP_P2R, I, 0},
    {0xfff8, 0x5cf0, SM_OP_R2P, R, 0}, {0xfff8, 0x4cf0, SM_OP_R2P, C, 0}, {0xfef8, 0x38f0, SM_OP_R2P, I, 0},
    {0xfff8, 0x50a0, SM_OP_CSETP, R, 0},
    /* system */
    {0xfff8, 0xf0c8, SM_OP_S2R, R, 0}, {0xfff8, 0x50c8, SM_OP_CS2R, R, 0},
    {0xfff8, 0x50d8, SM_OP_VOTE, R, 0}, {0xfff8, 0xef10, SM_OP_SHFL, R, 0},
    {0xfff8, 0xf0f0, SM_OP_BARRIER, R, 0}, {0xfff8, 0xf0a8, SM_OP_BARRIER, R, 0}, {0xfff8, 0xef98, SM_OP_BARRIER, R, 0},
    {0xfff8, 0x50b0, SM_OP_NOP, R, 0},
    /* memory */
    {0xfff8, 0xefd8, SM_OP_ALD, R, 0}, {0xfff8, 0xeff0, SM_OP_AST, R, 0}, {0xff00, 0xe000, SM_OP_IPA, R, 0},
    {0xfff8, 0xef90, SM_OP_LDC, R, 0},
    {0xfff8, 0xeed0, SM_OP_LDG, R, 0}, {0xfff8, 0xeed8, SM_OP_STG, R, 0},
    {0xfff8, 0xef40, SM_OP_LDL, R, 0}, {0xfff8, 0xef50, SM_OP_STL, R, 0},
    {0xfff8, 0xef48, SM_OP_LDL, R, 0}, {0xfff8, 0xef58, SM_OP_STL, R, 0}, /* LDS/STS: per-invocation */
    {0xe000, 0x8000, SM_OP_LD, R, 0}, {0xe000, 0xa000, SM_OP_ST, R, 0},
    {0xfff8, 0xfbe0, SM_OP_OUT, R, 0}, {0xfef8, 0xf6e0, SM_OP_OUT, I, 0}, {0xfff8, 0xebe0, SM_OP_OUT, C, 0},
    /* texture */
    {0xfff8, 0xdf48, SM_OP_TXQ, R, 0}, {0xfff8, 0xdf58, SM_OP_TMML, R, 0},
    {0xffc0, 0xdf00, SM_OP_TLD4S, R, 0},
    {0xfe00, 0xd800, SM_OP_TEXS, R, 0}, {0xfe00, 0xda00, SM_OP_TLDS, R, 0},
    {0xff78, 0xdc38, SM_OP_TLD, R, 0}, {0xfff8, 0xde38, SM_OP_TXD, R, 0},
    {0xfc38, 0xc838, SM_OP_TLD4, R, 0}, {0xfe38, 0xc038, SM_OP_TEX, R, 0},
    /* control */
    {0xfff0, 0xe240, SM_OP_BRA, R, 0}, {0xfff0, 0xe290, SM_OP_SSY, R, 0}, {0xfff0, 0xe2a0, SM_OP_PBK, R, 0},
    {0xfff0, 0xe2b0, SM_OP_PCNT, R, 0}, {0xfff0, 0xe260, SM_OP_CAL, R, 0},
    {0xfff0, 0xe300, SM_OP_EXIT, R, 0}, {0xfff0, 0xe330, SM_OP_KIL, R, 0}, {0xfff0, 0xe320, SM_OP_RET, R, 0},
    {0xfff0, 0xe340, SM_OP_BRK, R, 0}, {0xfff0, 0xe350, SM_OP_CONT, R, 0}, {0xfff8, 0xf0f8, SM_OP_SYNC, R, 0},
};

#undef R
#undef C
#undef I
#undef L
#undef X

static const char *const k_names[SM_OP_COUNT] = {
    [SM_OP_INVALID] = "???", [SM_OP_SCHED] = "SCHED", [SM_OP_NOP] = "NOP", [SM_OP_FADD] = "FADD",
    [SM_OP_FADD32I] = "FADD32I", [SM_OP_FMUL] = "FMUL", [SM_OP_FMUL32I] = "FMUL32I", [SM_OP_FFMA] = "FFMA",
    [SM_OP_FFMA32I] = "FFMA32I", [SM_OP_FMNMX] = "FMNMX", [SM_OP_FSET] = "FSET", [SM_OP_FSETP] = "FSETP",
    [SM_OP_FCMP] = "FCMP", [SM_OP_MUFU] = "MUFU", [SM_OP_RRO] = "RRO", [SM_OP_FSWZADD] = "FSWZADD",
    [SM_OP_F2F] = "F2F", [SM_OP_F2I] = "F2I", [SM_OP_I2F] = "I2F", [SM_OP_I2I] = "I2I", [SM_OP_IADD] = "IADD",
    [SM_OP_IADD32I] = "IADD32I", [SM_OP_IADD3] = "IADD3", [SM_OP_ISCADD] = "ISCADD", [SM_OP_ISCADD32I] = "ISCADD32I",
    [SM_OP_IMNMX] = "IMNMX", [SM_OP_ISET] = "ISET", [SM_OP_ISETP] = "ISETP", [SM_OP_ICMP] = "ICMP",
    [SM_OP_IMUL] = "IMUL", [SM_OP_IMUL32I] = "IMUL32I", [SM_OP_IMAD] = "IMAD", [SM_OP_XMAD] = "XMAD",
    [SM_OP_LOP] = "LOP", [SM_OP_LOP32I] = "LOP32I", [SM_OP_LOP3] = "LOP3", [SM_OP_SHL] = "SHL", [SM_OP_SHR] = "SHR",
    [SM_OP_BFE] = "BFE", [SM_OP_BFI] = "BFI", [SM_OP_POPC] = "POPC", [SM_OP_FLO] = "FLO", [SM_OP_PRMT] = "PRMT",
    [SM_OP_SEL] = "SEL", [SM_OP_MOV] = "MOV", [SM_OP_MOV32I] = "MOV32I", [SM_OP_PSETP] = "PSETP",
    [SM_OP_P2R] = "P2R", [SM_OP_R2P] = "R2P", [SM_OP_CSETP] = "CSETP", [SM_OP_S2R] = "S2R", [SM_OP_CS2R] = "CS2R",
    [SM_OP_VOTE] = "VOTE", [SM_OP_SHFL] = "SHFL", [SM_OP_BARRIER] = "BAR", [SM_OP_ALD] = "ALD", [SM_OP_AST] = "AST",
    [SM_OP_IPA] = "IPA", [SM_OP_LDC] = "LDC", [SM_OP_LD] = "LD", [SM_OP_ST] = "ST", [SM_OP_LDG] = "LDG",
    [SM_OP_STG] = "STG", [SM_OP_LDL] = "LDL", [SM_OP_STL] = "STL", [SM_OP_OUT] = "OUT", [SM_OP_TEX] = "TEX",
    [SM_OP_TEXS] = "TEXS", [SM_OP_TLD] = "TLD", [SM_OP_TLDS] = "TLDS", [SM_OP_TLD4] = "TLD4", [SM_OP_TLD4S] = "TLD4S",
    [SM_OP_TXQ] = "TXQ", [SM_OP_TMML] = "TMML", [SM_OP_TXD] = "TXD", [SM_OP_BRA] = "BRA", [SM_OP_SSY] = "SSY",
    [SM_OP_SYNC] = "SYNC", [SM_OP_PBK] = "PBK", [SM_OP_BRK] = "BRK", [SM_OP_PCNT] = "PCNT", [SM_OP_CONT] = "CONT",
    [SM_OP_CAL] = "CAL", [SM_OP_RET] = "RET", [SM_OP_EXIT] = "EXIT", [SM_OP_KIL] = "KIL",
};

const char *sm_op_name(Sm_Op op) {
  return (unsigned)op < SM_OP_COUNT && k_names[op] ? k_names[op] : "???";
}

static uint32_t imm20(uint64_t w) { return BITS(w, 20, 19) | (BIT(w, 56) << 19); }

static int32_t branch_offset(uint64_t w) {
  uint32_t v = BITS(w, 20, 24);
  if (v & 0x800000u) v |= 0xff000000u;
  return (int32_t)v;
}

static bool is_branchy(Sm_Op op) {
  return op == SM_OP_BRA || op == SM_OP_SSY || op == SM_OP_PBK || op == SM_OP_PCNT || op == SM_OP_CAL;
}

static void note_cbuf(Sm_Program *p, uint32_t slot, uint32_t end) {
  if (slot >= SM_CBUF_SLOTS) return;
  p->cbuf_used |= 1u << slot;
  if (end > p->cbuf_extent[slot]) p->cbuf_extent[slot] = end;
}

static Sm_Insn decode_word(uint64_t w, uint32_t index) {
  Sm_Insn insn;
  memset(&insn, 0, sizeof(insn));
  insn.raw = w;
  insn.target = -1;
  insn.pred = (uint8_t)BITS(w, 16, 4);
  const uint16_t top = (uint16_t)(w >> 48);
  for (uint32_t i = 0; i < sizeof(k_patterns) / sizeof(k_patterns[0]); i++) {
    const Op_Pattern *pat = &k_patterns[i];
    if ((top & pat->mask) != pat->match) continue;
    insn.op = pat->op;
    insn.form = pat->form;
    switch (pat->form) {
    case SM_FORM_CBUF:
    case SM_FORM_REG_CBUF:
      insn.cbuf = (uint8_t)BITS(w, 34, 5);
      insn.imm = BITS(w, 20, 14) * 4u;
      break;
    case SM_FORM_IMM: {
      const uint32_t v = imm20(w);
      insn.imm = pat->float_imm ? v << 12 : ((v & 0x80000u) ? v | 0xfff00000u : v);
      break;
    }
    case SM_FORM_IMM32:
      insn.imm = BITS(w, 20, 32);
      break;
    default:
      break;
    }
    break;
  }
  /* Per-op immediates that are not the generic B operand. */
  switch (insn.op) {
  case SM_OP_XMAD:
    if (insn.form == SM_FORM_IMM) insn.imm = BITS(w, 20, 16);
    break;
  case SM_OP_LOP3:
    if (insn.form == SM_FORM_IMM) insn.imm = BITS(w, 20, 32);
    break;
  default:
    break;
  }
  /* SSY/PBK/PCNT/CAL carry no predicate guard. */
  if (insn.op == SM_OP_SSY || insn.op == SM_OP_PBK || insn.op == SM_OP_PCNT || insn.op == SM_OP_CAL) insn.pred = SM_PT;
  if (is_branchy((Sm_Op)insn.op)) {
    if (BIT(w, 5)) {
      insn.target = -1; /* constant-buffer target: unsupported */
    } else {
      insn.target = (int32_t)index + 1 + branch_offset(w) / 8;
    }
  }
  return insn;
}

uint32_t sm_hash(const uint8_t *bytes, uint32_t size) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < size; i++) {
    h ^= bytes[i];
    h *= 16777619u;
  }
  return h;
}

static uint64_t rd64(const uint8_t *p) {
  uint64_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

uint32_t sm_program_extent(const uint8_t *bytes, uint32_t size) {
  if (size <= SM_SPH_BYTES) return size;
  const uint32_t words = (size - SM_SPH_BYTES) / 8u;
  for (uint32_t i = 0; i < words && i < SM_MAX_WORDS; i++) {
    if (i % 4u == 0) continue;
    const uint64_t w = rd64(bytes + SM_SPH_BYTES + i * 8u);
    /* An all-zero instruction word is not an instruction: padding after
     * the program (compilers that emit no end marker). */
    if (w == 0) return SM_SPH_BYTES + i * 8u;
    /* BRA to itself (possibly via the scheduling word before it): the
     * end-of-program marker. */
    if ((w >> 52) == 0xe24u) {
      const int32_t target = (int32_t)i + 1 + branch_offset(w) / 8;
      if (target == (int32_t)i || (target == (int32_t)i - 1 && (i - 1u) % 4u == 0))
        return SM_SPH_BYTES + (i + 1u) * 8u;
    }
  }
  const uint32_t limit = SM_SPH_BYTES + SM_MAX_WORDS * 8u;
  return size < limit ? size : limit;
}

void sm_header_parse(const uint32_t words[SM_SPH_WORDS], Sm_Header *out) {
  memset(out, 0, sizeof(*out));
  memcpy(out->words, words, sizeof(out->words));
  out->stage = (Sm_Stage)BITS(words[0], 10, 4);
  out->mrt_enable = BIT(words[0], 14) != 0;
  out->kills_pixels = BIT(words[0], 15) != 0;
  out->local_memory_bytes = BITS(words[1], 0, 24);
  const uint8_t *b = (const uint8_t *)words;
  if (out->stage == SM_STAGE_PIXEL) {
    /* ImapSystemValuesB @0x17: position x,y,z,w (2 bits each? no -
     * one bit each in the upper nibble). */
    for (uint32_t c = 0; c < 4; c++)
      out->position_interp[c] = (b[0x17] >> (4u + c)) & 1u ? SM_INTERP_SCREEN_LINEAR : SM_INTERP_UNUSED;
    /* ImapGenericVector[32] @0x18: 2 bits per component. */
    for (uint32_t i = 0; i < SM_ATTR_GENERIC_COUNT * 4u; i++)
      out->input_interp[i] = (uint8_t)((b[0x18 + i / 4u] >> ((i % 4u) * 2u)) & 3u);
    out->omap_target = words[0x48 / 4];
    out->omap_sample_mask = BIT(words[0x4c / 4], 0) != 0;
    out->omap_depth = BIT(words[0x4c / 4], 1) != 0;
  } else {
    /* ImapGenericVector[32] @0x18 and OmapGenericVector[32] @0x36: 4
     * bits per vector. */
    for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
      const uint32_t in = (b[0x18 + v / 2u] >> ((v % 2u) * 4u)) & 0xfu;
      const uint32_t outn = (b[0x36 + v / 2u] >> ((v % 2u) * 4u)) & 0xfu;
      for (uint32_t c = 0; c < 4; c++) {
        const uint32_t bit = v * 4u + c;
        if (in & (1u << c)) out->input_generic[bit / 32u] |= 1u << (bit % 32u);
        if (outn & (1u << c)) out->output_generic[bit / 32u] |= 1u << (bit % 32u);
      }
    }
  }
}

void sm_program_decode(const uint8_t *bytes, uint32_t size, uint64_t address, Sm_Program *out) {
  out->address = address;
  out->byte_size = size;
  out->hash = sm_hash(bytes, size);
  out->cbuf_used = 0;
  out->unknown_ops = 0;
  out->reads_fragcoord_xy = false;
  out->reads_fragcoord_z = false;
  memset(out->cbuf_extent, 0, sizeof(out->cbuf_extent));
  uint32_t sph[SM_SPH_WORDS];
  memset(sph, 0, sizeof(sph));
  memcpy(sph, bytes, size < SM_SPH_BYTES ? size : SM_SPH_BYTES);
  sm_header_parse(sph, &out->header);
  const uint32_t words = size > SM_SPH_BYTES ? (size - SM_SPH_BYTES) / 8u : 0u;
  out->word_count = words < SM_MAX_WORDS ? words : SM_MAX_WORDS;
  for (uint32_t i = 0; i < out->word_count; i++) {
    const uint64_t w = rd64(bytes + SM_SPH_BYTES + i * 8u);
    if (i % 4u == 0) {
      memset(&out->insns[i], 0, sizeof(out->insns[i]));
      out->insns[i].op = SM_OP_SCHED;
      out->insns[i].raw = w;
      out->insns[i].target = -1;
      continue;
    }
    out->insns[i] = decode_word(w, i);
    const Sm_Insn *in = &out->insns[i];
    if (in->op == SM_OP_INVALID) out->unknown_ops++;
    if (in->form == SM_FORM_CBUF || in->form == SM_FORM_REG_CBUF) note_cbuf(out, in->cbuf, in->imm + 4u);
    if (in->op == SM_OP_LDC) note_cbuf(out, BITS(w, 36, 5), 0x10000u);
    if (in->op == SM_OP_IPA) {
      const uint32_t addr = BITS(w, 28, 10);
      if (BIT(w, 38) || addr == SM_ATTR_POSITION || addr == SM_ATTR_POSITION + 4u) out->reads_fragcoord_xy = true;
      if (addr == SM_ATTR_POSITION + 8u) out->reads_fragcoord_z = true;
    }
  }
}

/* ---- execution helpers -------------------------------------------- */

static float f32(uint32_t v) {
  float f;
  memcpy(&f, &v, sizeof(f));
  return f;
}

static uint32_t u32f(float f) {
  uint32_t v;
  memcpy(&v, &f, sizeof(v));
  return v;
}

static uint32_t reg(const Sm_Thread *t, uint32_t r) { return r == SM_RZ ? 0u : t->r[r]; }

static void set_reg(Sm_Thread *t, uint32_t r, uint32_t v) {
  if (r != SM_RZ) t->r[r] = v;
}

static bool pred(const Sm_Thread *t, uint32_t p) { return p >= SM_PT ? true : t->p[p]; }

static bool pred_neg(const Sm_Thread *t, uint32_t p, uint32_t negate) { return pred(t, p) != (negate != 0); }

static void set_pred(Sm_Thread *t, uint32_t p, bool v) {
  if (p < SM_PT) t->p[p] = v;
}

static uint32_t cbuf_read(const Sm_Env *env, uint32_t slot, uint32_t offset) {
  if (slot >= SM_CBUF_SLOTS || !env->cbuf[slot]) return 0;
  if ((uint64_t)offset + 4u > env->cbuf_size[slot]) return 0;
  uint32_t v;
  memcpy(&v, env->cbuf[slot] + offset, sizeof(v));
  return v;
}

/* The second (B) operand by form. */
static uint32_t op_b(const Sm_Insn *in, const Sm_Env *env, const Sm_Thread *t) {
  switch (in->form) {
  case SM_FORM_REG: return reg(t, REG_B(in->raw));
  case SM_FORM_CBUF: return cbuf_read(env, in->cbuf, in->imm);
  case SM_FORM_REG_CBUF: return reg(t, REG_C(in->raw));
  default: return in->imm;
  }
}

/* The third (C) operand of three-source ops. */
static uint32_t op_c(const Sm_Insn *in, const Sm_Env *env, const Sm_Thread *t) {
  if (in->form == SM_FORM_REG_CBUF) return cbuf_read(env, in->cbuf, in->imm);
  return reg(t, REG_C(in->raw));
}

static float fmod_abs_neg(float v, uint32_t abs_bit, uint32_t neg_bit) {
  if (abs_bit) v = fabsf(v);
  if (neg_bit) v = -v;
  return v;
}

static float saturate(float v) {
  if (!(v > 0.0f)) return 0.0f; /* NaN -> 0 */
  return v > 1.0f ? 1.0f : v;
}

static bool fcompare(uint32_t cond, float a, float b) {
  const bool unordered = isnan(a) || isnan(b);
  switch (cond & 0xfu) {
  case 0x0: return false;
  case 0x1: return !unordered && a < b;
  case 0x2: return !unordered && a == b;
  case 0x3: return !unordered && a <= b;
  case 0x4: return !unordered && a > b;
  case 0x5: return !unordered && a != b;
  case 0x6: return !unordered && a >= b;
  case 0x7: return !unordered;
  case 0x8: return unordered;
  case 0x9: return unordered || a < b;
  case 0xa: return unordered || a == b;
  case 0xb: return unordered || a <= b;
  case 0xc: return unordered || a > b;
  case 0xd: return unordered || a != b;
  case 0xe: return unordered || a >= b;
  default: return true;
  }
}

static bool icompare(uint32_t cond, uint32_t a, uint32_t b, bool is_signed) {
  const int64_t sa = is_signed ? (int64_t)(int32_t)a : (int64_t)a;
  const int64_t sb = is_signed ? (int64_t)(int32_t)b : (int64_t)b;
  switch (cond & 7u) {
  case 0: return false;
  case 1: return sa < sb;
  case 2: return sa == sb;
  case 3: return sa <= sb;
  case 4: return sa > sb;
  case 5: return sa != sb;
  case 6: return sa >= sb;
  default: return true;
  }
}

static bool bool_op(uint32_t op, bool a, bool b) {
  switch (op & 3u) {
  case 0: return a && b;
  case 1: return a || b;
  case 2: return a != b;
  default: return a;
  }
}

static float half_to_float(uint16_t h) {
  const uint32_t sign = (uint32_t)(h >> 15) << 31;
  const uint32_t exp = (h >> 10) & 0x1fu;
  uint32_t mant = h & 0x3ffu;
  if (exp == 0) {
    if (mant == 0) return f32(sign);
    float v = (float)mant / 1024.0f / 16384.0f; /* 2^-14 * mant/1024 */
    return sign ? -v : v;
  }
  if (exp == 31) return f32(sign | 0x7f800000u | (mant << 13));
  return f32(sign | ((exp + 112u) << 23) | (mant << 13));
}

static uint16_t float_to_half(float f) {
  const uint32_t v = u32f(f);
  const uint32_t sign = (v >> 16) & 0x8000u;
  const int32_t exp = (int32_t)((v >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = v & 0x7fffffu;
  if (((v >> 23) & 0xffu) == 0xffu) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0u));
  if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
  if (exp <= 0) {
    if (exp < -10) return (uint16_t)sign;
    mant |= 0x800000u;
    const uint32_t shift = (uint32_t)(14 - exp);
    uint32_t h = mant >> shift;
    if ((mant >> (shift - 1u)) & 1u) h++;
    return (uint16_t)(sign | h);
  }
  uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
  if (mant & 0x1000u) h++; /* round half up */
  return (uint16_t)h;
}

static float round_mode(float v, uint32_t mode) {
  switch (mode & 3u) {
  case 0: return rintf(v);
  case 1: return floorf(v);
  case 2: return ceilf(v);
  default: return truncf(v);
  }
}

static uint32_t float_to_int(float v, bool is_signed, uint32_t size_log2) {
  if (isnan(v)) return 0;
  const uint32_t bits = 8u << (size_log2 > 2u ? 2u : size_log2);
  if (is_signed) {
    const double lo = -(double)(1ull << (bits - 1u));
    const double hi = (double)(1ull << (bits - 1u)) - 1.0;
    double d = v;
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    return (uint32_t)(int32_t)(int64_t)d;
  }
  const double hi = (double)((bits >= 32u) ? 0xffffffffull : ((1ull << bits) - 1ull));
  double d = v;
  if (d < 0.0) d = 0.0;
  if (d > hi) d = hi;
  return (uint32_t)(uint64_t)d;
}

/* An integer source of `size_log2` bytes (selector picks the byte /
 * half), sign- or zero-extended. */
static int64_t int_source(uint32_t v, uint32_t size_log2, uint32_t selector, bool is_signed) {
  switch (size_log2) {
  case 0: {
    const uint32_t b = (v >> (selector * 8u)) & 0xffu;
    return is_signed ? (int64_t)(int8_t)b : (int64_t)b;
  }
  case 1: {
    const uint32_t h = (v >> ((selector >> 1) * 16u)) & 0xffffu;
    return is_signed ? (int64_t)(int16_t)h : (int64_t)h;
  }
  default:
    return is_signed ? (int64_t)(int32_t)v : (int64_t)v;
  }
}

static uint32_t clamp_int(int64_t v, uint32_t size_log2, bool is_signed, bool sat) {
  const uint32_t bits = 8u << (size_log2 > 2u ? 2u : size_log2);
  if (sat) {
    const int64_t lo = is_signed ? -(int64_t)(1ull << (bits - 1u)) : 0;
    const int64_t hi = is_signed ? (int64_t)(1ull << (bits - 1u)) - 1 : (int64_t)((bits >= 32u) ? 0xffffffffll : (int64_t)((1ull << bits) - 1ull));
    if (v < lo) v = lo;
    if (v > hi) v = hi;
  }
  if (bits >= 32u) return (uint32_t)v;
  const uint32_t mask = (1u << bits) - 1u;
  uint32_t r = (uint32_t)v & mask;
  if (is_signed && (r & (1u << (bits - 1u)))) r |= ~mask;
  return r;
}

static uint32_t bit_reverse(uint32_t v) {
  v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
  v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
  v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
  v = ((v >> 8) & 0x00ff00ffu) | ((v & 0x00ff00ffu) << 8);
  return (v >> 16) | (v << 16);
}

static uint32_t popcount(uint32_t v) {
  uint32_t n = 0;
  while (v) {
    v &= v - 1u;
    n++;
  }
  return n;
}

static uint32_t lop(uint32_t op, uint32_t a, uint32_t b) {
  switch (op & 3u) {
  case 0: return a & b;
  case 1: return a | b;
  case 2: return a ^ b;
  default: return b;
  }
}

static uint32_t lop3(uint32_t lut, uint32_t a, uint32_t b, uint32_t c) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 8u; i++) {
    if (!(lut & (1u << i))) continue;
    r |= ((i & 4u) ? a : ~a) & ((i & 2u) ? b : ~b) & ((i & 1u) ? c : ~c);
  }
  return r;
}

/* Memory access sizes (LDG/LDL/LDC "size" fields): 0 u8, 1 s8, 2 u16,
 * 3 s16, 4 32, 5 64, 6 128. Returns bytes. */
static uint32_t access_bytes(uint32_t size) {
  switch (size) {
  case 0: case 1: return 1;
  case 2: case 3: return 2;
  case 5: return 8;
  case 6: return 16;
  default: return 4;
  }
}

static void load_to_regs(Sm_Thread *t, uint32_t d, uint32_t size, const uint8_t *src) {
  const uint32_t n = access_bytes(size);
  if (n < 4u) {
    uint32_t v = 0;
    memcpy(&v, src, n);
    if (size == 1) v = (uint32_t)(int32_t)(int8_t)v;
    if (size == 3) v = (uint32_t)(int32_t)(int16_t)v;
    set_reg(t, d, v);
    return;
  }
  for (uint32_t i = 0; i < n / 4u; i++) {
    uint32_t v;
    memcpy(&v, src + i * 4u, sizeof(v));
    if (d != SM_RZ) set_reg(t, (d + i) & 0xffu, v);
  }
}

static void regs_to_bytes(const Sm_Thread *t, uint32_t d, uint32_t size, uint8_t *dst) {
  const uint32_t n = access_bytes(size);
  if (n < 4u) {
    const uint32_t v = reg(t, d);
    memcpy(dst, &v, n);
    return;
  }
  for (uint32_t i = 0; i < n / 4u; i++) {
    const uint32_t v = d == SM_RZ ? 0u : reg(t, (d + i) & 0xffu);
    memcpy(dst + i * 4u, &v, sizeof(v));
  }
}

/* ---- textures ----------------------------------------------------- */

static uint32_t texture_handle(const Sm_Env *env, uint32_t index) {
  return cbuf_read(env, env->texture_cbuf_slot, index * 4u);
}

/* Reads `n` packed texture arguments: the first `split` from Ra.., the
 * rest from Rb.. (TEXS/TLDS packing - see the table users). */
static void tex_args(const Sm_Thread *t, uint64_t w, uint32_t n, uint32_t args[8]) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  uint32_t in_a, in_b;
  if (n <= 1u) { in_a = n; in_b = 0; }
  else if (n == 2u) { in_a = 1; in_b = 1; }
  else { in_a = 2; in_b = n - 2u; }
  for (uint32_t i = 0; i < in_a; i++) args[i] = a == SM_RZ ? 0u : reg(t, (a + i) & 0xffu);
  for (uint32_t i = 0; i < in_b; i++) args[in_a + i] = b == SM_RZ ? 0u : reg(t, (b + i) & 0xffu);
}

/* TEX/TLD/TLD4 vector packing: up to four arguments in Ra.., the rest
 * in Rb... */
static void tex_args_vec(const Sm_Thread *t, uint64_t w, uint32_t n, uint32_t args[8]) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  for (uint32_t i = 0; i < n && i < 8u; i++) {
    const uint32_t base = i < 4u ? a : b;
    const uint32_t k = i < 4u ? i : i - 4u;
    args[i] = base == SM_RZ ? 0u : reg(t, (base + k) & 0xffu);
  }
}

/* TEXS/TLDS/TLD4S destinations: components in order to Rd, Rd+1, Rd2,
 * Rd2+1. */
static void write_scalar_results(Sm_Thread *t, uint64_t w, const uint32_t *values, uint32_t count) {
  const uint32_t d0 = REG_D(w), d1 = BITS(w, 28, 8);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t base = i < 2u ? d0 : d1;
    if (base == SM_RZ) continue;
    set_reg(t, (base + (i & 1u)) & 0xffu, values[i]);
  }
}

/* Component lists for the 3-bit TEXS/TLDS mask, single / dual dest. */
static uint32_t scalar_components(uint32_t code, bool dual, uint32_t out[4]) {
  static const uint8_t single[8][3] = {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {2, 0}, {2, 0}, {2, 1}, {2, 2}};
  static const uint8_t single_second[8] = {0, 0, 0, 0, 1, 3, 3, 3};
  static const uint8_t dual_list[5][4] = {{0, 1, 2, 0}, {0, 1, 3, 0}, {0, 2, 3, 0}, {1, 2, 3, 0}, {0, 1, 2, 3}};
  static const uint8_t dual_count[5] = {3, 3, 3, 3, 4};
  if (!dual) {
    out[0] = single[code & 7u][1];
    if (single[code & 7u][0] == 2u) {
      out[1] = single_second[code & 7u];
      return 2;
    }
    return 1;
  }
  if (code > 4u) code = 4u;
  for (uint32_t i = 0; i < dual_count[code]; i++) out[i] = dual_list[code][i];
  return dual_count[code];
}

static void tex_call(const Sm_Env *env, Sm_Tex_Request *req, uint32_t out[4]) {
  out[0] = out[1] = out[2] = 0;
  out[3] = u32f(1.0f);
  if (env->texture) env->texture(env->user, req, out);
}

static void exec_texs(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4);
  Sm_Tex_Request req;
  memset(&req, 0, sizeof(req));
  req.kind = SM_TEX_SAMPLE;
  req.handle = texture_handle(env, BITS(w, 36, 13));
  uint32_t args[8] = {0};
  float x = 0, y = 0, z = 0;
  switch (target) {
  case 0: tex_args(t, w, 1, args); req.dims = 1; req.has_lod = true; x = f32(args[0]); break;
  case 1: tex_args(t, w, 2, args); req.dims = 2; x = f32(args[0]); y = f32(args[1]); break;
  case 2: tex_args(t, w, 2, args); req.dims = 2; req.has_lod = true; x = f32(args[0]); y = f32(args[1]); break;
  case 3: tex_args(t, w, 3, args); req.dims = 2; req.has_lod = true; x = f32(args[0]); y = f32(args[1]); req.lod = f32(args[2]); break;
  case 4: tex_args(t, w, 3, args); req.dims = 2; req.shadow = true; x = f32(args[0]); y = f32(args[1]); req.dref = f32(args[2]); break;
  case 5: tex_args(t, w, 4, args); req.dims = 2; req.shadow = true; req.has_lod = true; x = f32(args[0]); y = f32(args[1]); req.lod = f32(args[2]); req.dref = f32(args[3]); break;
  case 6: tex_args(t, w, 3, args); req.dims = 2; req.shadow = true; req.has_lod = true; x = f32(args[0]); y = f32(args[1]); req.dref = f32(args[2]); break;
  case 7: tex_args(t, w, 3, args); req.dims = 2; req.array = true; req.layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); break;
  case 8: tex_args(t, w, 3, args); req.dims = 2; req.array = true; req.has_lod = true; req.layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); break;
  case 9: tex_args(t, w, 4, args); req.dims = 2; req.array = true; req.shadow = true; req.has_lod = true; req.layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); req.dref = f32(args[3]); break;
  case 10: case 11: tex_args(t, w, 3, args); req.dims = 3; req.has_lod = target == 11u; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); break;
  case 12: tex_args(t, w, 3, args); req.dims = 3; req.cube = true; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); break;
  case 13: tex_args(t, w, 4, args); req.dims = 3; req.cube = true; req.has_lod = true; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); req.lod = f32(args[3]); break;
  default: req.dims = 2; break;
  }
  req.coords[0] = x;
  req.coords[1] = y;
  req.coords[2] = z;
  uint32_t texel[4];
  tex_call(env, &req, texel);
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  uint32_t values[4];
  for (uint32_t i = 0; i < n; i++) values[i] = texel[comps[i]];
  write_scalar_results(t, w, values, n);
}

static void exec_tlds(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4);
  Sm_Tex_Request req;
  memset(&req, 0, sizeof(req));
  req.kind = SM_TEX_FETCH;
  req.handle = texture_handle(env, BITS(w, 36, 13));
  uint32_t args[8] = {0};
  switch (target) {
  case 0: tex_args(t, w, 1, args); req.dims = 1; req.icoords[0] = (int32_t)args[0]; break;
  case 1: tex_args(t, w, 2, args); req.dims = 1; req.icoords[0] = (int32_t)args[0]; req.ilod = (int32_t)args[1]; break;
  case 2: tex_args(t, w, 2, args); req.dims = 2; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1]; break;
  case 4: tex_args(t, w, 3, args); req.dims = 2; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1];
    req.offset[0] = (int32_t)(args[2] << 28) >> 28; req.offset[1] = (int32_t)(args[2] << 24) >> 28; break;
  case 5: tex_args(t, w, 3, args); req.dims = 2; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1]; req.ilod = (int32_t)args[2]; break;
  case 6: tex_args(t, w, 3, args); req.dims = 2; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1]; break;
  case 7: tex_args(t, w, 3, args); req.dims = 3; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1]; req.icoords[2] = (int32_t)args[2]; break;
  case 8: tex_args(t, w, 3, args); req.dims = 2; req.array = true; req.layer = (float)args[0]; req.icoords[0] = (int32_t)args[1]; req.icoords[1] = (int32_t)args[2]; break;
  case 12: tex_args(t, w, 4, args); req.dims = 2; req.icoords[0] = (int32_t)args[0]; req.icoords[1] = (int32_t)args[1]; req.ilod = (int32_t)args[2];
    req.offset[0] = (int32_t)(args[3] << 28) >> 28; req.offset[1] = (int32_t)(args[3] << 24) >> 28; break;
  default: req.dims = 2; break;
  }
  uint32_t texel[4];
  tex_call(env, &req, texel);
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  uint32_t values[4];
  for (uint32_t i = 0; i < n; i++) values[i] = texel[comps[i]];
  write_scalar_results(t, w, values, n);
}

static void exec_tld4s(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t) {
  const uint64_t w = in->raw;
  Sm_Tex_Request req;
  memset(&req, 0, sizeof(req));
  req.kind = SM_TEX_GATHER;
  req.handle = texture_handle(env, BITS(w, 36, 13));
  req.dims = 2;
  req.gather_component = (uint8_t)BITS(w, 52, 2);
  req.has_offset = BIT(w, 51) != 0;
  req.shadow = BIT(w, 50) != 0;
  const uint32_t n = 2u + (req.has_offset ? 1u : 0u) + (req.shadow ? 1u : 0u);
  uint32_t args[8] = {0};
  tex_args(t, w, n, args);
  req.coords[0] = f32(args[0]);
  req.coords[1] = f32(args[1]);
  uint32_t k = 2;
  if (req.has_offset) {
    req.offset[0] = (int32_t)(args[k] << 26) >> 26;
    req.offset[1] = (int32_t)(args[k] << 18) >> 26;
    k++;
  }
  if (req.shadow) req.dref = f32(args[k]);
  uint32_t texel[4];
  tex_call(env, &req, texel);
  write_scalar_results(t, w, texel, 4);
}

/* TEX / TLD / TLD4 / TXD / TXQ / TMML (vector forms). */
static void exec_tex_vector(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t) {
  const uint64_t w = in->raw;
  Sm_Tex_Request req;
  memset(&req, 0, sizeof(req));
  req.handle = texture_handle(env, BITS(w, 36, 13));
  const uint32_t dim = BITS(w, 29, 2);
  req.dims = (uint8_t)(dim == 3u ? 3u : dim + 1u);
  req.cube = dim == 3u;
  req.array = BIT(w, 28) != 0;
  const uint32_t mask = BITS(w, 31, 4);
  uint32_t coord_count = req.dims;
  uint32_t args[8] = {0};
  uint32_t n = (req.array ? 1u : 0u) + coord_count;
  bool lod = false, bias = false, offset = false, dc = false, ms = false;
  switch (in->op) {
  case SM_OP_TEX: {
    const uint32_t lodm = BITS(w, 55, 2);
    req.kind = SM_TEX_SAMPLE;
    lod = lodm == 3u;
    bias = lodm == 2u;
    req.has_lod = lodm == 1u || lodm == 3u;
    offset = BIT(w, 54) != 0;
    dc = BIT(w, 50) != 0;
    break;
  }
  case SM_OP_TLD:
    req.kind = SM_TEX_FETCH;
    lod = BIT(w, 55) != 0;
    ms = BIT(w, 50) != 0;
    offset = BIT(w, 35) != 0;
    break;
  case SM_OP_TLD4:
    req.kind = SM_TEX_GATHER;
    req.gather_component = (uint8_t)BITS(w, 56, 2);
    offset = BITS(w, 54, 2) != 0;
    dc = BIT(w, 50) != 0;
    break;
  case SM_OP_TXD:
    req.kind = SM_TEX_SAMPLE;
    req.has_lod = true; /* derivatives ignored: base level */
    break;
  case SM_OP_TXQ:
    req.kind = SM_TEX_QUERY_DIMS;
    break;
  default: /* TMML */
    req.kind = SM_TEX_QUERY_LOD;
    break;
  }
  if (req.kind == SM_TEX_QUERY_DIMS) {
    const uint32_t query = BITS(w, 22, 6);
    req.ilod = (int32_t)reg(t, REG_A(w));
    uint32_t out[4];
    tex_call(env, &req, out);
    if (query != 1u) out[0] = out[1] = out[2] = out[3] = 0;
    uint32_t d = REG_D(w);
    for (uint32_t c = 0; c < 4; c++) {
      if (!(mask & (1u << c))) continue;
      set_reg(t, d, out[c]);
      if (d != SM_RZ) d = (d + 1u) & 0xffu;
    }
    return;
  }
  if (req.kind == SM_TEX_QUERY_LOD) {
    uint32_t d = REG_D(w);
    for (uint32_t c = 0; c < 4; c++) {
      if (!(mask & (1u << c))) continue;
      set_reg(t, d, 0);
      if (d != SM_RZ) d = (d + 1u) & 0xffu;
    }
    return;
  }
  n += (lod || bias) ? 1u : 0u;
  n += offset ? 1u : 0u;
  n += ms ? 1u : 0u;
  n += dc ? 1u : 0u;
  tex_args_vec(t, w, n, args);
  uint32_t k = 0;
  if (req.array) req.layer = (float)(args[k++] & 0xffffu);
  for (uint32_t c = 0; c < coord_count; c++) {
    if (req.kind == SM_TEX_FETCH) req.icoords[c] = (int32_t)args[k];
    else req.coords[c] = f32(args[k]);
    k++;
  }
  if (lod || bias) {
    if (req.kind == SM_TEX_FETCH) req.ilod = (int32_t)args[k];
    else req.lod = f32(args[k]);
    req.has_lod = lod;
    req.has_bias = bias;
    k++;
  }
  if (offset) {
    req.has_offset = true;
    for (uint32_t c = 0; c < coord_count && c < 3u; c++) req.offset[c] = (int32_t)(args[k] << (28u - 4u * c)) >> 28;
    k++;
  }
  if (ms) k++;
  if (dc) {
    req.shadow = true;
    req.dref = f32(args[k]);
  }
  uint32_t texel[4];
  tex_call(env, &req, texel);
  uint32_t d = REG_D(w);
  for (uint32_t c = 0; c < 4; c++) {
    if (!(mask & (1u << c))) continue;
    set_reg(t, d, texel[c]);
    if (d != SM_RZ) d = (d + 1u) & 0xffu;
  }
}

/* ---- the interpreter ---------------------------------------------- */

typedef struct Stack_Entry {
  uint32_t kind;
  uint32_t target;
} Stack_Entry;

typedef struct Flow {
  Stack_Entry entries[SM_STACK_DEPTH * 3u];
  uint32_t depth;
} Flow;

static bool flow_push(Flow *f, uint32_t kind, int32_t target) {
  if (target < 0 || f->depth >= SM_STACK_DEPTH * 3u) return false;
  f->entries[f->depth].kind = kind;
  f->entries[f->depth].target = (uint32_t)target;
  f->depth++;
  return true;
}

/* Pops to the most recent entry of `kind`; keeps it when `keep`.
 * Returns its target or -1. */
static int32_t flow_pop_to(Flow *f, uint32_t kind, bool keep) {
  while (f->depth > 0) {
    const Stack_Entry e = f->entries[f->depth - 1u];
    if (e.kind == kind) {
      if (!keep) f->depth--;
      return (int32_t)e.target;
    }
    f->depth--;
  }
  return -1;
}

void sm_thread_reset(Sm_Thread *t) {
  memset(t->r, 0, sizeof(t->r));
  for (uint32_t i = 0; i < SM_PREDICATES; i++) t->p[i] = false;
  t->p[SM_PT] = true;
  t->cc_carry = t->cc_zero = t->cc_sign = t->cc_overflow = false;
  t->killed = false;
  t->faulted = false;
  t->ssy_depth = t->pbk_depth = t->pcnt_depth = t->call_depth = 0;
}

void sm_thread_reset_light(Sm_Thread *t) {
  for (uint32_t i = 0; i < SM_PREDICATES; i++) t->p[i] = false;
  t->p[SM_PT] = true;
  t->cc_carry = t->cc_zero = t->cc_sign = t->cc_overflow = false;
  t->killed = false;
  t->faulted = false;
}

static void set_cc_from(Sm_Thread *t, uint32_t v) {
  t->cc_zero = v == 0;
  t->cc_sign = (v >> 31) != 0;
}

static uint32_t sysreg(const Sm_Thread *t, uint32_t id) {
  static uint32_t clock;
  switch (id) {
  case 0x12: return u32f(1.0f);  /* Y direction */
  case 0x38: return 1;           /* lanemask eq */
  case 0x3a: return 1;           /* lanemask le */
  case 0x3c: return 0xffffffffu; /* lanemask ge */
  case 0x50: return clock++;
  case 0x51: return 0;
  default: (void)t; return 0;
  }
}

bool sm_run(const Sm_Program *program, const Sm_Env *env, Sm_Thread *t) {
  Flow flow;
  flow.depth = 0;
  uint32_t calls[SM_STACK_DEPTH];
  uint32_t call_depth = 0;
  uint32_t pc = 1;
  for (uint32_t step = 0; step < SM_MAX_STEPS; step++) {
    if (pc >= program->word_count) {
      t->faulted = true;
      return false;
    }
    if (pc % 4u == 0) {
      pc++;
      continue;
    }
    const Sm_Insn *in = &program->insns[pc];
    const uint64_t w = in->raw;
    uint32_t next = pc + 1u;
    if ((in->pred & 7u) != SM_PT || (in->pred & 8u)) {
      if (!pred_neg(t, in->pred & 7u, in->pred & 8u)) {
        pc = next;
        continue;
      }
    }
    switch ((Sm_Op)in->op) {
    case SM_OP_NOP:
    case SM_OP_SCHED:
    case SM_OP_BARRIER:
      break;

    /* ---- float ---- */
    case SM_OP_FADD: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
      const float b = fmod_abs_neg(f32(op_b(in, env, t)), BIT(w, 49), BIT(w, 45));
      float r = a + b;
      if (BIT(w, 50)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_FADD32I: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 54), BIT(w, 56));
      const float b = fmod_abs_neg(f32(in->imm), BIT(w, 57), BIT(w, 53));
      set_reg(t, REG_D(w), u32f(a + b));
      break;
    }
    case SM_OP_FMUL: {
      float r = f32(reg(t, REG_A(w))) * f32(op_b(in, env, t));
      /* @41: 1-3 divide by 2^n, 4-6 multiply by 8, 4, 2. */
      static const float scale[8] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f, 1.0f};
      r *= scale[BITS(w, 41, 3)];
      if (BIT(w, 48)) r = -r;
      if (BIT(w, 50)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_FMUL32I: {
      float r = f32(reg(t, REG_A(w))) * f32(in->imm);
      if (BIT(w, 55)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_FFMA: {
      float ab = f32(reg(t, REG_A(w))) * f32(op_b(in, env, t));
      float c = f32(op_c(in, env, t));
      if (BIT(w, 48)) ab = -ab;
      if (BIT(w, 49)) c = -c;
      float r = ab + c;
      if (BIT(w, 50)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_FFMA32I: {
      float ab = f32(reg(t, REG_A(w))) * f32(in->imm);
      float c = f32(reg(t, REG_D(w)));
      if (BIT(w, 56)) ab = -ab;
      if (BIT(w, 57)) c = -c;
      float r = ab + c;
      if (BIT(w, 55)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_FMNMX: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
      const float b = fmod_abs_neg(f32(op_b(in, env, t)), BIT(w, 49), BIT(w, 45));
      const bool min = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      set_reg(t, REG_D(w), u32f(min ? fminf(a, b) : fmaxf(a, b)));
      break;
    }
    case SM_OP_FSET: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 54), BIT(w, 43));
      const float b = fmod_abs_neg(f32(op_b(in, env, t)), BIT(w, 44), BIT(w, 53));
      const bool r = bool_op(BITS(w, 45, 2), fcompare(BITS(w, 48, 4), a, b), pred_neg(t, BITS(w, 39, 3), BIT(w, 42)));
      set_reg(t, REG_D(w), r ? (BIT(w, 52) ? u32f(1.0f) : 0xffffffffu) : 0u);
      break;
    }
    case SM_OP_FSETP: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 7), BIT(w, 43));
      const float b = fmod_abs_neg(f32(op_b(in, env, t)), BIT(w, 44), BIT(w, 6));
      const bool cmp = fcompare(BITS(w, 48, 4), a, b);
      const bool pc_v = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      const uint32_t bop = BITS(w, 45, 2);
      set_pred(t, BITS(w, 3, 3), bool_op(bop, cmp, pc_v));
      set_pred(t, BITS(w, 0, 3), bool_op(bop, !cmp, pc_v));
      break;
    }
    case SM_OP_FCMP: {
      const float c = f32(op_c(in, env, t));
      const bool r = fcompare(BITS(w, 48, 4), c, 0.0f);
      set_reg(t, REG_D(w), r ? reg(t, REG_A(w)) : op_b(in, env, t));
      break;
    }
    case SM_OP_MUFU: {
      const float a = fmod_abs_neg(f32(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
      float r;
      switch (BITS(w, 20, 4)) {
      case 0: r = cosf(a); break;
      case 1: r = sinf(a); break;
      case 2: r = exp2f(a); break;
      case 3: r = log2f(a); break;
      case 4: r = 1.0f / a; break;
      case 5: r = 1.0f / sqrtf(a); break;
      case 8: r = sqrtf(a); break;
      default: r = a; break;
      }
      if (BIT(w, 50)) r = saturate(r);
      set_reg(t, REG_D(w), u32f(r));
      break;
    }
    case SM_OP_RRO:
      set_reg(t, REG_D(w), u32f(fmod_abs_neg(f32(op_b(in, env, t)), BIT(w, 49), BIT(w, 45))));
      break;
    case SM_OP_FSWZADD:
      set_reg(t, REG_D(w), 0); /* derivatives of a single invocation */
      break;
    case SM_OP_F2F: {
      const uint32_t src_size = BITS(w, 10, 2), dst_size = BITS(w, 8, 2);
      const uint32_t raw = op_b(in, env, t);
      float v = src_size == 1u ? half_to_float((uint16_t)(raw >> (BIT(w, 41) ? 16u : 0u))) : f32(raw);
      v = fmod_abs_neg(v, BIT(w, 49), BIT(w, 45));
      if (BIT(w, 42)) v = round_mode(v, BITS(w, 39, 2));
      if (BIT(w, 50)) v = saturate(v);
      set_reg(t, REG_D(w), dst_size == 1u ? (uint32_t)float_to_half(v) : u32f(v));
      break;
    }
    case SM_OP_F2I: {
      const uint32_t src_size = BITS(w, 10, 2);
      const uint32_t raw = op_b(in, env, t);
      float v = src_size == 1u ? half_to_float((uint16_t)raw) : f32(raw);
      v = fmod_abs_neg(v, BIT(w, 49), BIT(w, 45));
      v = round_mode(v, BITS(w, 39, 2));
      set_reg(t, REG_D(w), float_to_int(v, BIT(w, 12) != 0, BITS(w, 8, 2)));
      break;
    }
    case SM_OP_I2F: {
      const bool is_signed = BIT(w, 13) != 0;
      int64_t v = int_source(op_b(in, env, t), BITS(w, 10, 2), BITS(w, 41, 2), is_signed);
      if (BIT(w, 49) && v < 0) v = -v;
      if (BIT(w, 45)) v = -v;
      const float f = is_signed || v < 0 ? (float)v : (float)(uint64_t)v;
      set_reg(t, REG_D(w), BITS(w, 8, 2) == 1u ? (uint32_t)float_to_half(f) : u32f(f));
      break;
    }
    case SM_OP_I2I: {
      const bool src_signed = BIT(w, 13) != 0, dst_signed = BIT(w, 12) != 0;
      int64_t v = int_source(op_b(in, env, t), BITS(w, 10, 2), BITS(w, 41, 2), src_signed);
      if (BIT(w, 49) && v < 0) v = -v;
      if (BIT(w, 45)) v = -v;
      set_reg(t, REG_D(w), clamp_int(v, BITS(w, 8, 2), dst_signed, BIT(w, 50) != 0));
      break;
    }

    /* ---- integer ---- */
    case SM_OP_IADD:
    case SM_OP_IADD32I: {
      const bool is32i = in->op == SM_OP_IADD32I;
      uint32_t a = reg(t, REG_A(w));
      uint32_t b = op_b(in, env, t);
      if (is32i ? BIT(w, 56) : BIT(w, 49)) a = (uint32_t)(-(int64_t)a);
      if (!is32i && BIT(w, 48)) b = (uint32_t)(-(int64_t)b);
      const bool x = is32i ? BIT(w, 53) != 0 : BIT(w, 43) != 0;
      const uint64_t sum = (uint64_t)a + (uint64_t)b + (x && t->cc_carry ? 1u : 0u);
      uint32_t r = (uint32_t)sum;
      const bool sat = is32i ? BIT(w, 54) != 0 : BIT(w, 50) != 0;
      if (sat) {
        const int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)b;
        r = s > INT32_MAX ? (uint32_t)INT32_MAX : s < INT32_MIN ? (uint32_t)INT32_MIN : (uint32_t)s;
      }
      if (is32i ? BIT(w, 52) : BIT(w, 47)) {
        t->cc_carry = (sum >> 32) != 0;
        t->cc_overflow = ((~(a ^ b) & (a ^ r)) >> 31) != 0;
        set_cc_from(t, r);
      }
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_ISCADD: {
      uint32_t a = reg(t, REG_A(w));
      uint32_t b = op_b(in, env, t);
      if (BIT(w, 49)) a = (uint32_t)(-(int64_t)a);
      if (BIT(w, 48)) b = (uint32_t)(-(int64_t)b);
      const uint32_t r = (a << BITS(w, 39, 5)) + b;
      if (BIT(w, 47)) set_cc_from(t, r);
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_IMNMX: {
      const uint32_t a = reg(t, REG_A(w)), b = op_b(in, env, t);
      const bool is_signed = BIT(w, 48) != 0;
      const bool min = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      const bool a_less = is_signed ? (int32_t)a < (int32_t)b : a < b;
      set_reg(t, REG_D(w), min ? (a_less ? a : b) : (a_less ? b : a));
      break;
    }
    case SM_OP_ISET: {
      const bool cmp = icompare(BITS(w, 49, 3), reg(t, REG_A(w)), op_b(in, env, t), BIT(w, 48) != 0);
      const bool r = bool_op(BITS(w, 45, 2), cmp, pred_neg(t, BITS(w, 39, 3), BIT(w, 42)));
      set_reg(t, REG_D(w), r ? (BIT(w, 44) ? u32f(1.0f) : 0xffffffffu) : 0u);
      break;
    }
    case SM_OP_ISETP: {
      const bool cmp = icompare(BITS(w, 49, 3), reg(t, REG_A(w)), op_b(in, env, t), BIT(w, 48) != 0);
      const bool pc_v = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      const uint32_t bop = BITS(w, 45, 2);
      set_pred(t, BITS(w, 3, 3), bool_op(bop, cmp, pc_v));
      set_pred(t, BITS(w, 0, 3), bool_op(bop, !cmp, pc_v));
      break;
    }
    case SM_OP_ICMP: {
      const bool r = icompare(BITS(w, 49, 3), op_c(in, env, t), 0, BIT(w, 48) != 0);
      set_reg(t, REG_D(w), r ? reg(t, REG_A(w)) : op_b(in, env, t));
      break;
    }
    case SM_OP_IMUL:
    case SM_OP_IMUL32I: {
      const bool is32i = in->op == SM_OP_IMUL32I;
      const bool sa = is32i ? BIT(w, 55) != 0 : BIT(w, 41) != 0;
      const bool high = is32i ? BIT(w, 53) != 0 : BIT(w, 39) != 0;
      const uint32_t a = reg(t, REG_A(w)), b = op_b(in, env, t);
      const int64_t p = sa ? (int64_t)(int32_t)a * (int64_t)(int32_t)b : (int64_t)((uint64_t)a * (uint64_t)b);
      set_reg(t, REG_D(w), high ? (uint32_t)((uint64_t)p >> 32) : (uint32_t)p);
      break;
    }
    case SM_OP_IMAD: {
      const bool is_signed = BIT(w, 53) != 0;
      const uint32_t a = reg(t, REG_A(w)), b = op_b(in, env, t);
      uint32_t c = op_c(in, env, t);
      int64_t p = is_signed ? (int64_t)(int32_t)a * (int64_t)(int32_t)b : (int64_t)((uint64_t)a * (uint64_t)b);
      if (BIT(w, 51)) p = -p;
      if (BIT(w, 52)) c = (uint32_t)(-(int64_t)c);
      const uint32_t base = BIT(w, 54) ? (uint32_t)((uint64_t)p >> 32) : (uint32_t)p;
      set_reg(t, REG_D(w), base + c);
      break;
    }
    case SM_OP_XMAD: {
      const uint32_t a = reg(t, REG_A(w));
      uint32_t b, c;
      bool psl, mrg, b_hi;
      uint32_t cmode;
      switch (in->form) {
      case SM_FORM_REG:
        b = reg(t, REG_B(w)); c = reg(t, REG_C(w));
        psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = BIT(w, 35) != 0; cmode = BITS(w, 50, 3);
        break;
      case SM_FORM_CBUF:
        b = cbuf_read(env, in->cbuf, in->imm); c = reg(t, REG_C(w));
        psl = BIT(w, 55) != 0; mrg = BIT(w, 56) != 0; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
        break;
      case SM_FORM_IMM:
        b = in->imm; c = reg(t, REG_C(w));
        psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = false; cmode = BITS(w, 50, 3);
        break;
      default: /* REG_CBUF: b = Rc slot, c = cbuf */
        b = reg(t, REG_C(w)); c = cbuf_read(env, in->cbuf, in->imm);
        psl = false; mrg = false; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
        break;
      }
      const uint32_t a16 = BIT(w, 53) ? a >> 16 : a & 0xffffu;
      const uint32_t b16 = b_hi ? b >> 16 : b & 0xffffu;
      uint32_t prod = a16 * b16;
      if (psl) prod <<= 16;
      uint32_t cv;
      switch (cmode) {
      case 1: cv = c & 0xffffu; break;
      case 2: cv = c >> 16; break;
      case 4: cv = c + (b << 16); break;
      default: cv = c; break;
      }
      uint32_t r = prod + cv;
      if (mrg) r = (r & 0xffffu) | (b << 16);
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_LOP: {
      uint32_t a = reg(t, REG_A(w)), b = op_b(in, env, t);
      if (BIT(w, 39)) a = ~a;
      if (BIT(w, 40)) b = ~b;
      const uint32_t r = lop(BITS(w, 41, 2), a, b);
      set_pred(t, BITS(w, 48, 3), r != 0);
      if (BIT(w, 47)) set_cc_from(t, r);
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_LOP32I: {
      uint32_t a = reg(t, REG_A(w)), b = in->imm;
      if (BIT(w, 55)) a = ~a;
      if (BIT(w, 56)) b = ~b;
      const uint32_t r = lop(BITS(w, 53, 2), a, b);
      if (BIT(w, 52)) set_cc_from(t, r);
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_LOP3: {
      const uint32_t lut = in->form == SM_FORM_REG ? BITS(w, 28, 8) : BITS(w, 48, 8);
      const uint32_t b = in->form == SM_FORM_IMM ? in->imm : op_b(in, env, t);
      set_reg(t, REG_D(w), lop3(lut, reg(t, REG_A(w)), b, reg(t, REG_C(w))));
      break;
    }
    case SM_OP_SHL: {
      const uint32_t a = reg(t, REG_A(w));
      uint32_t s = op_b(in, env, t);
      if (BIT(w, 39)) s &= 31u;
      set_reg(t, REG_D(w), s >= 32u ? 0u : a << s);
      break;
    }
    case SM_OP_SHR: {
      const uint32_t a = reg(t, REG_A(w));
      uint32_t s = op_b(in, env, t);
      if (BIT(w, 39)) s &= 31u;
      uint32_t r;
      if (BIT(w, 48)) r = s >= 32u ? (uint32_t)((int32_t)a >> 31) : (uint32_t)((int32_t)a >> s);
      else r = s >= 32u ? 0u : a >> s;
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_BFE: {
      uint32_t a = reg(t, REG_A(w));
      if (BIT(w, 40)) a = bit_reverse(a);
      const uint32_t b = op_b(in, env, t);
      const uint32_t pos = b & 0xffu, len = (b >> 8) & 0xffu;
      uint32_t r;
      if (len == 0) r = 0;
      else if (pos >= 32u) r = BIT(w, 48) ? (uint32_t)((int32_t)a >> 31) : 0u;
      else {
        const uint32_t l = len > 32u - pos ? 32u - pos : len;
        r = (a >> pos) & (l >= 32u ? 0xffffffffu : ((1u << l) - 1u));
        if (BIT(w, 48) && l < 32u && (r >> (l - 1u)) & 1u) r |= ~((1u << l) - 1u);
      }
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_BFI: {
      const uint32_t a = reg(t, REG_A(w));
      const uint32_t b = op_b(in, env, t);
      const uint32_t c = op_c(in, env, t);
      const uint32_t pos = b & 0xffu, len = (b >> 8) & 0xffu;
      if (pos >= 32u || len == 0) {
        set_reg(t, REG_D(w), c);
        break;
      }
      const uint32_t l = len > 32u - pos ? 32u - pos : len;
      const uint32_t mask = (l >= 32u ? 0xffffffffu : ((1u << l) - 1u)) << pos;
      set_reg(t, REG_D(w), (c & ~mask) | ((a << pos) & mask));
      break;
    }
    case SM_OP_POPC: {
      uint32_t b = op_b(in, env, t);
      if (BIT(w, 40)) b = ~b;
      set_reg(t, REG_D(w), popcount(b));
      break;
    }
    case SM_OP_FLO: {
      uint32_t b = op_b(in, env, t);
      if (BIT(w, 40)) b = ~b;
      if (BIT(w, 48) && (b >> 31)) b = ~b;
      uint32_t r = 0xffffffffu;
      for (int32_t i = 31; i >= 0; i--) {
        if ((b >> i) & 1u) {
          r = (uint32_t)i;
          break;
        }
      }
      if (BIT(w, 41) && r != 0xffffffffu) r = 31u - r;
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_PRMT: {
      const uint64_t src = (uint64_t)reg(t, REG_A(w)) | ((uint64_t)op_c(in, env, t) << 32);
      const uint32_t sel = op_b(in, env, t);
      uint32_t r = 0;
      for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t s = (sel >> (i * 4u)) & 0xfu;
        uint32_t byte = (uint32_t)(src >> ((s & 7u) * 8u)) & 0xffu;
        if (s & 8u) byte = (byte & 0x80u) ? 0xffu : 0u;
        r |= byte << (i * 8u);
      }
      set_reg(t, REG_D(w), r);
      break;
    }
    case SM_OP_SEL:
      set_reg(t, REG_D(w), pred_neg(t, BITS(w, 39, 3), BIT(w, 42)) ? reg(t, REG_A(w)) : op_b(in, env, t));
      break;
    case SM_OP_MOV:
      set_reg(t, REG_D(w), op_b(in, env, t));
      break;
    case SM_OP_MOV32I:
      set_reg(t, REG_D(w), in->imm);
      break;
    case SM_OP_PSETP: {
      const bool a = pred_neg(t, BITS(w, 12, 3), BIT(w, 15));
      const bool b = pred_neg(t, BITS(w, 29, 3), BIT(w, 32));
      const bool c = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      const bool ab = bool_op(BITS(w, 24, 2), a, b);
      const uint32_t bop2 = BITS(w, 45, 2);
      set_pred(t, BITS(w, 3, 3), bool_op(bop2, ab, c));
      set_pred(t, BITS(w, 0, 3), bool_op(bop2, !ab, c));
      break;
    }
    case SM_OP_P2R: {
      uint32_t bits = 0;
      for (uint32_t i = 0; i < 7u; i++) bits |= (t->p[i] ? 1u : 0u) << i;
      const uint32_t mask = op_b(in, env, t);
      const uint32_t shift = BITS(w, 41, 2) * 8u;
      const uint32_t a = reg(t, REG_A(w));
      set_reg(t, REG_D(w), (a & ~(mask << shift)) | ((bits & mask) << shift));
      break;
    }
    case SM_OP_R2P: {
      const uint32_t mask = op_b(in, env, t);
      const uint32_t v = reg(t, REG_A(w)) >> (BITS(w, 41, 2) * 8u);
      for (uint32_t i = 0; i < 7u; i++)
        if (mask & (1u << i)) t->p[i] = ((v >> i) & 1u) != 0;
      break;
    }
    case SM_OP_CSETP: {
      const bool pc_v = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      bool cond = true;
      switch (BITS(w, 8, 5)) {
      case 0x02: cond = t->cc_zero; break;          /* EQ */
      case 0x05: cond = !t->cc_zero; break;         /* NE */
      case 0x01: cond = t->cc_sign; break;          /* LT */
      case 0x00: cond = false; break;
      default: break;
      }
      const uint32_t bop = BITS(w, 45, 2);
      set_pred(t, BITS(w, 3, 3), bool_op(bop, cond, pc_v));
      set_pred(t, BITS(w, 0, 3), bool_op(bop, !cond, pc_v));
      break;
    }

    /* ---- system ---- */
    case SM_OP_S2R:
    case SM_OP_CS2R:
      set_reg(t, REG_D(w), sysreg(t, BITS(w, 20, 8)));
      break;
    case SM_OP_VOTE: {
      const bool v = pred_neg(t, BITS(w, 39, 3), BIT(w, 42));
      set_reg(t, REG_D(w), v ? 1u : 0u);
      set_pred(t, BITS(w, 45, 3), BITS(w, 48, 2) == 2u ? true : v);
      break;
    }
    case SM_OP_SHFL:
      set_reg(t, REG_D(w), reg(t, REG_A(w)));
      set_pred(t, BITS(w, 48, 3), true);
      break;

    /* ---- memory ---- */
    case SM_OP_ALD: {
      const uint32_t count = BITS(w, 47, 2) + 1u;
      const uint32_t base = BITS(w, 20, 10) + reg(t, REG_A(w));
      const uint32_t *src = BIT(w, 32) ? t->attr_out : t->attr_in;
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t word = (base / 4u + i) % SM_ATTRIBUTE_WORDS;
        uint32_t v = src[word];
        if (!BIT(w, 32)) {
          if (base + i * 4u == SM_ATTR_VERTEX_ID) v = t->vertex_id;
          if (base + i * 4u == SM_ATTR_INSTANCE_ID) v = t->instance_id;
        }
        if (REG_D(w) != SM_RZ) set_reg(t, (REG_D(w) + i) & 0xffu, v);
      }
      break;
    }
    case SM_OP_AST: {
      const uint32_t count = BITS(w, 47, 2) + 1u;
      const uint32_t base = BITS(w, 20, 10) + reg(t, REG_A(w));
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t word = (base / 4u + i) % SM_ATTRIBUTE_WORDS;
        t->attr_out[word] = REG_D(w) == SM_RZ ? 0u : reg(t, (REG_D(w) + i) & 0xffu);
      }
      break;
    }
    case SM_OP_IPA: {
      uint32_t addr = BITS(w, 28, 10);
      if (BIT(w, 38)) addr += reg(t, REG_A(w));
      const uint32_t word = (addr / 4u) % SM_ATTRIBUTE_WORDS;
      uint32_t v = t->attr_in[word];
      if (addr == SM_ATTR_FRONT_FACING) {
        v = t->front_facing ? 0xffffffffu : 0u;
      } else {
        float f = f32(v);
        if (BITS(w, 54, 2) == 1u) f *= f32(reg(t, REG_B(w)));
        if (BIT(w, 51)) f = saturate(f);
        v = u32f(f);
      }
      set_reg(t, REG_D(w), v);
      break;
    }
    case SM_OP_LDC: {
      const uint32_t slot = BITS(w, 36, 5);
      const uint32_t size = BITS(w, 48, 3);
      const int32_t off = (int32_t)(BITS(w, 20, 16) << 16) >> 16;
      const uint32_t addr = reg(t, REG_A(w)) + (uint32_t)off;
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      const uint32_t n = access_bytes(size);
      if (slot < SM_CBUF_SLOTS && env->cbuf[slot] && (uint64_t)addr + n <= env->cbuf_size[slot])
        memcpy(buf, env->cbuf[slot] + addr, n);
      load_to_regs(t, REG_D(w), size, buf);
      break;
    }
    case SM_OP_LD:
    case SM_OP_ST:
    case SM_OP_LDG:
    case SM_OP_STG: {
      const bool generic = in->op == SM_OP_LD || in->op == SM_OP_ST;
      const uint32_t size = generic ? BITS(w, 53, 3) : BITS(w, 48, 3);
      const bool wide = generic ? BIT(w, 52) != 0 : BIT(w, 45) != 0;
      int64_t off = generic ? (int64_t)(int32_t)BITS(w, 20, 32) : (int64_t)((int32_t)(BITS(w, 20, 24) << 8) >> 8);
      uint64_t addr = reg(t, REG_A(w));
      if (wide && REG_A(w) != SM_RZ) addr |= (uint64_t)reg(t, (REG_A(w) + 1u) & 0xffu) << 32;
      addr += (uint64_t)off;
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      const uint32_t n = access_bytes(size);
      if (in->op == SM_OP_LD || in->op == SM_OP_LDG) {
        if (env->global_read) (void)env->global_read(env->user, addr, buf, n);
        load_to_regs(t, REG_D(w), size, buf);
      } else {
        regs_to_bytes(t, REG_D(w), size, buf);
        if (env->global_write) (void)env->global_write(env->user, addr, buf, n);
      }
      break;
    }
    case SM_OP_LDL:
    case SM_OP_STL: {
      const uint32_t size = BITS(w, 48, 3);
      const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
      const uint32_t addr = reg(t, REG_A(w)) + (uint32_t)off;
      const uint32_t n = access_bytes(size);
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      if (in->op == SM_OP_LDL) {
        if ((uint64_t)addr + n <= SM_LOCAL_BYTES) memcpy(buf, t->local + addr, n);
        load_to_regs(t, REG_D(w), size, buf);
      } else {
        regs_to_bytes(t, REG_D(w), size, buf);
        if ((uint64_t)addr + n <= SM_LOCAL_BYTES) memcpy(t->local + addr, buf, n);
      }
      break;
    }
    case SM_OP_OUT:
      set_reg(t, REG_D(w), 0);
      break;

    /* ---- texture ---- */
    case SM_OP_TEXS: exec_texs(in, env, t); break;
    case SM_OP_TLDS: exec_tlds(in, env, t); break;
    case SM_OP_TLD4S: exec_tld4s(in, env, t); break;
    case SM_OP_TEX:
    case SM_OP_TLD:
    case SM_OP_TLD4:
    case SM_OP_TXD:
    case SM_OP_TXQ:
    case SM_OP_TMML:
      exec_tex_vector(in, env, t);
      break;

    /* ---- control ---- */
    case SM_OP_BRA:
      if (in->target < 0) {
        t->faulted = true;
        return false;
      }
      next = (uint32_t)in->target;
      break;
    case SM_OP_SSY:
      if (!flow_push(&flow, STACK_SSY, in->target)) { t->faulted = true; return false; }
      break;
    case SM_OP_PBK:
      if (!flow_push(&flow, STACK_PBK, in->target)) { t->faulted = true; return false; }
      break;
    case SM_OP_PCNT:
      if (!flow_push(&flow, STACK_PCNT, in->target)) { t->faulted = true; return false; }
      break;
    case SM_OP_SYNC: {
      const int32_t target = flow_pop_to(&flow, STACK_SSY, false);
      if (target < 0) { t->faulted = true; return false; }
      next = (uint32_t)target;
      break;
    }
    case SM_OP_BRK: {
      const int32_t target = flow_pop_to(&flow, STACK_PBK, false);
      if (target < 0) { t->faulted = true; return false; }
      next = (uint32_t)target;
      break;
    }
    case SM_OP_CONT: {
      const int32_t target = flow_pop_to(&flow, STACK_PCNT, true);
      if (target < 0) { t->faulted = true; return false; }
      next = (uint32_t)target;
      break;
    }
    case SM_OP_CAL:
      if (in->target < 0 || call_depth >= SM_STACK_DEPTH) { t->faulted = true; return false; }
      calls[call_depth++] = next;
      next = (uint32_t)in->target;
      break;
    case SM_OP_RET:
      if (call_depth == 0) return true;
      next = calls[--call_depth];
      break;
    case SM_OP_EXIT:
      return true;
    case SM_OP_KIL:
      t->killed = true;
      return true;

    default:
      /* Unknown or unsupported: skip it (counted at decode). */
      break;
    }
    pc = next;
  }
  t->faulted = true;
  return false;
}
