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
#define SM_LDST_SPACE_MASK 0xfff8u
#define SM_LDS_OPCODE 0xef48u
#define SM_STS_OPCODE 0xef58u
#define SM_BAR_OPCODE 0xf0a8u

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
    /* half precision; the 0x6/0x7 cbuf/imm forms share prefixes, HFMA2's are narrower */
    {0xfff8, 0x5d10, SM_OP_HADD2, R, 0}, {0xfe80, 0x7a80, SM_OP_HADD2, C, 0}, {0xfe80, 0x7a00, SM_OP_HADD2, I, 0},
    {0xfe00, 0x2c00, SM_OP_HADD2, L, 0},
    {0xfff8, 0x5d08, SM_OP_HMUL2, R, 0}, {0xfe80, 0x7880, SM_OP_HMUL2, C, 0}, {0xfe80, 0x7800, SM_OP_HMUL2, I, 0},
    {0xfe00, 0x2a00, SM_OP_HMUL2, L, 0},
    {0xfff8, 0x5d00, SM_OP_HFMA2, R, 0}, {0xf880, 0x6080, SM_OP_HFMA2, X, 0}, {0xf880, 0x7080, SM_OP_HFMA2, C, 0},
    {0xf880, 0x7000, SM_OP_HFMA2, I, 0}, {0xfe00, 0x2800, SM_OP_HFMA2, L, 0},
    {0xfff8, 0x5d18, SM_OP_HSET2, R, 0}, {0xfe80, 0x7c80, SM_OP_HSET2, C, 0}, {0xfe80, 0x7c00, SM_OP_HSET2, I, 0},
    {0xfff8, 0x5d20, SM_OP_HSETP2, R, 0}, {0xfe80, 0x7e80, SM_OP_HSETP2, C, 0}, {0xfe80, 0x7e00, SM_OP_HSETP2, I, 0},
    /* integer */
    {0xfff8, 0x5c10, SM_OP_IADD, R, 0}, {0xfff8, 0x4c10, SM_OP_IADD, C, 0}, {0xfef8, 0x3810, SM_OP_IADD, I, 0},
    {0xfe00, 0x1c00, SM_OP_IADD32I, L, 0},
    {0xfff0, 0x5cc0, SM_OP_IADD3, R, 0}, {0xfff0, 0x4cc0, SM_OP_IADD3, C, 0}, {0xfef0, 0x38c0, SM_OP_IADD3, I, 0},
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
    {0xfff8, 0x50b0, SM_OP_NOP, R, 0}, {0xfff8, 0x50e0, SM_OP_NOP, R, 0}, /* VOTE.VTG: a VTG-culling hint, no effect here */
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
    {0xfe00, 0xd000, SM_OP_TEXS, R, 0}, {0xfe00, 0xd200, SM_OP_TLDS, R, 0}, /* bit 59 clear: half results */
    {0xff78, 0xdc38, SM_OP_TLD, R, 0}, {0xfff8, 0xde38, SM_OP_TXD, R, 0},
    {0xfc38, 0xc838, SM_OP_TLD4, R, 0}, {0xfe38, 0xc038, SM_OP_TEX, R, 0}, {0xfff8, 0xdeb8, SM_OP_TEX_B, R, 0},
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
    [SM_OP_F2F] = "F2F", [SM_OP_HADD2] = "HADD2", [SM_OP_HMUL2] = "HMUL2", [SM_OP_HFMA2] = "HFMA2",
    [SM_OP_HSET2] = "HSET2", [SM_OP_HSETP2] = "HSETP2", [SM_OP_F2I] = "F2I", [SM_OP_I2F] = "I2F", [SM_OP_I2I] = "I2I", [SM_OP_IADD] = "IADD",
    [SM_OP_IADD32I] = "IADD32I", [SM_OP_IADD3] = "IADD3", [SM_OP_ISCADD] = "ISCADD", [SM_OP_ISCADD32I] = "ISCADD32I",
    [SM_OP_IMNMX] = "IMNMX", [SM_OP_ISET] = "ISET", [SM_OP_ISETP] = "ISETP", [SM_OP_ICMP] = "ICMP",
    [SM_OP_IMUL] = "IMUL", [SM_OP_IMUL32I] = "IMUL32I", [SM_OP_IMAD] = "IMAD", [SM_OP_XMAD] = "XMAD",
    [SM_OP_LOP] = "LOP", [SM_OP_LOP32I] = "LOP32I", [SM_OP_LOP3] = "LOP3", [SM_OP_SHL] = "SHL", [SM_OP_SHR] = "SHR",
    [SM_OP_BFE] = "BFE", [SM_OP_BFI] = "BFI", [SM_OP_POPC] = "POPC", [SM_OP_FLO] = "FLO", [SM_OP_PRMT] = "PRMT",
    [SM_OP_SEL] = "SEL", [SM_OP_MOV] = "MOV", [SM_OP_MOV32I] = "MOV32I", [SM_OP_PSETP] = "PSETP",
    [SM_OP_P2R] = "P2R", [SM_OP_R2P] = "R2P", [SM_OP_CSETP] = "CSETP", [SM_OP_S2R] = "S2R", [SM_OP_CS2R] = "CS2R",
    [SM_OP_VOTE] = "VOTE", [SM_OP_SHFL] = "SHFL", [SM_OP_BARRIER] = "BAR", [SM_OP_ALD] = "ALD", [SM_OP_AST] = "AST",
    [SM_OP_IPA] = "IPA", [SM_OP_LDC] = "LDC", [SM_OP_LD] = "LD", [SM_OP_ST] = "ST", [SM_OP_LDG] = "LDG",
    [SM_OP_STG] = "STG", [SM_OP_LDL] = "LDL", [SM_OP_STL] = "STL", [SM_OP_OUT] = "OUT", [SM_OP_TEX] = "TEX", [SM_OP_TEX_B] = "TEX.B",
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
  case SM_OP_HADD2:
  case SM_OP_HMUL2:
  case SM_OP_HFMA2:
  case SM_OP_HSET2:
  case SM_OP_HSETP2:
    /* A pair of f16 with 9-bit exponent+top-mantissa fields: low at 20
     * (sign 29), high at 30 (sign 56); the low 6 mantissa bits are 0. */
    if (insn.form == SM_FORM_IMM) {
      insn.imm = (BITS(w, 20, 9) << 6) | (BIT(w, 29) << 15) | (BITS(w, 30, 9) << 22) | (BIT(w, 56) << 31);
    }
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
  out->uses_quads = false;
  out->uses_bindless_textures = false;
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
    if (in->op == SM_OP_FSWZADD || in->op == SM_OP_SHFL) out->uses_quads = true;
    if (in->op == SM_OP_TEX_B) out->uses_bindless_textures = true;
    if (in->op == SM_OP_IPA) {
      const uint32_t addr = BITS(w, 28, 10);
      if (BIT(w, 38) || addr == SM_ATTR_POSITION || addr == SM_ATTR_POSITION + 4u) out->reads_fragcoord_xy = true;
      if (addr == SM_ATTR_POSITION + 8u) out->reads_fragcoord_z = true;
    }
  }
  /* Fall-through successors skip what does nothing here: scheduling
   * words, NOP, DEPBAR and MEMBAR - not BAR, where compute blocks meet. */
  uint32_t following = out->word_count;
  for (uint32_t i = out->word_count; i-- > 0;) {
    out->insns[i].next = (uint16_t)following;
    const uint16_t op = out->insns[i].op;
    const bool bar_sync = op == SM_OP_BARRIER && ((uint32_t)(out->insns[i].raw >> 48) & SM_LDST_SPACE_MASK) == SM_BAR_OPCODE;
    if (op != SM_OP_SCHED && op != SM_OP_NOP && (op != SM_OP_BARRIER || bar_sync)) following = i;
  }
}

/* ---- execution helpers -------------------------------------------- */

/* Control flow's condition-code test (bits 4:0; 15 = always). The
 * comparisons read the flags; the overflow/carry/sign-only tests read one
 * flag; the special CSM / FCSM / RLE / RGT conditions belong to fixed-
 * function hardware (e.g. VTG culling - NVN's vertex prologues EXIT on
 * FCSM_TR when a primitive is culled) that is not modelled: never true,
 * so execution falls through to the real work. */
#define SM_CC_TEST_MASK 0x1fu
static Sm_Mask cc_test(const Sm_Thread *t, uint32_t cond) {
  const Sm_Mask n = t->cc_sign, z = t->cc_zero, c = t->cc_carry, v = t->cc_overflow, all = SM_ALL_LANES;
  const Sm_Mask lt = (Sm_Mask)(n ^ v);
  switch (cond) {
  case 0: return 0;                                  /* F */
  case 1: return lt;                                 /* LT */
  case 2: return z;                                  /* EQ */
  case 3: return (Sm_Mask)(z | lt);                  /* LE */
  case 4: return (Sm_Mask)(all & ~z & ~lt);          /* GT */
  case 5: return (Sm_Mask)(all & ~z);                /* NE */
  case 6: return (Sm_Mask)(all & ~lt);               /* GE */
  case 7: return (Sm_Mask)(all & ~v);                /* NUM */
  case 8: return v;                                  /* NAN */
  case 9: return (Sm_Mask)(lt | v);                  /* LTU */
  case 10: return (Sm_Mask)(z | v);                  /* EQU */
  case 11: return (Sm_Mask)(z | lt | v);             /* LEU */
  case 12: return (Sm_Mask)((all & ~z & ~lt) | v);   /* GTU */
  case 13: return (Sm_Mask)(all & ~z);               /* NEU */
  case 14: return (Sm_Mask)((all & ~lt) | v);        /* GEU */
  case 15: return all;                               /* T */
  case 16: return (Sm_Mask)(all & ~v);               /* OFF */
  case 17: return (Sm_Mask)(all & ~c);               /* LO */
  case 18: return (Sm_Mask)(all & ~n);               /* SFF */
  case 19: return (Sm_Mask)((all & ~c) | z);         /* LS */
  case 20: return (Sm_Mask)(c & ~z);                 /* HI */
  case 21: return n;                                 /* SFT */
  case 22: return c;                                 /* HS */
  case 23: return v;                                 /* OFT */
  default: return 0;                                 /* CSM_*, FCSM_*, RLE, RGT */
  }
}

static bool is_cc_tested_control(uint8_t op) {
  switch ((Sm_Op)op) {
  case SM_OP_BRA: case SM_OP_EXIT: case SM_OP_KIL: case SM_OP_BRK: case SM_OP_CONT: case SM_OP_RET:
    return true;
  default:
    return false;
  }
}

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

#define LANE(m, l) (((m) >> (l)) & 1u)
#define FOR_LANES(m) for (uint32_t l = 0; l < SM_LANES; l++) if (LANE(m, l))
/* Pure arithmetic computes every lane into a temporary (inactive lanes'
 * registers are valid memory; their results are discarded), then
 * store_masked keeps only the active ones: both loops are branch-free,
 * so they vectorize (NEON natively, WASM SIMD on the web). */
#define FOR_ALL_LANES for (uint32_t l = 0; l < SM_LANES; l++)

static inline void store_masked(uint32_t *restrict d, const uint32_t *restrict res, Sm_Mask m) {
  if (m == SM_ALL_LANES) {
    memcpy(d, res, sizeof(uint32_t) * SM_LANES);
    return;
  }
  for (uint32_t l = 0; l < SM_LANES; l++) {
    const uint32_t keep = 0u - (uint32_t)LANE(m, l);
    d[l] = (res[l] & keep) | (d[l] & ~keep);
  }
}

/* d[l] = expr for the active lanes, where expr is pure in `l`. */
#define LANES_ASSIGN(m, d, expr)                \
  do {                                          \
    uint32_t res_[SM_LANES];                    \
    FOR_ALL_LANES res_[l] = (expr);             \
    store_masked((d), res_, (m));               \
  } while (0)

/* Destination row: writes to RZ go to the discard row. */
static uint32_t *dst_row(Sm_Thread *t, uint32_t r) { return r == SM_RZ ? t->discard : t->r[r]; }

static Sm_Mask pred_mask(const Sm_Thread *t, uint32_t p, uint32_t negate) {
  const Sm_Mask m = p >= SM_PT ? SM_ALL_LANES : t->p[p];
  return negate ? (Sm_Mask)~m : m;
}

/* Sets predicate `p` to `value` in the lanes of `m`. */
static void set_pred(Sm_Thread *t, uint32_t p, Sm_Mask m, Sm_Mask value) {
  if (p < SM_PT) t->p[p] = (Sm_Mask)((t->p[p] & ~m) | (value & m));
}

static void set_mask(Sm_Mask *flag, Sm_Mask m, Sm_Mask value) { *flag = (Sm_Mask)((*flag & ~m) | (value & m)); }

static uint32_t cbuf_read(const Sm_Env *env, uint32_t slot, uint32_t offset) {
  if (slot >= SM_CBUF_SLOTS || !env->cbuf[slot]) return 0;
  if ((uint64_t)offset + 4u > env->cbuf_size[slot]) return 0;
  uint32_t v;
  memcpy(&v, env->cbuf[slot] + offset, sizeof(v));
  return v;
}

static const uint32_t *splat(uint32_t v, uint32_t tmp[SM_LANES]) {
  for (uint32_t l = 0; l < SM_LANES; l++) tmp[l] = v;
  return tmp;
}

/* The second (B) operand, per lane, by form. */
/* The word sm_run is executing (texture requests carry it). */
static _Thread_local uint32_t g_current_pc;

static const uint32_t *op_b(const Sm_Insn *in, const Sm_Env *env, const Sm_Thread *t, uint32_t tmp[SM_LANES]) {
  switch (in->form) {
  case SM_FORM_REG: return t->r[REG_B(in->raw)];
  case SM_FORM_CBUF: return splat(cbuf_read(env, in->cbuf, in->imm), tmp);
  case SM_FORM_REG_CBUF: return t->r[REG_C(in->raw)];
  default: return splat(in->imm, tmp);
  }
}

/* Whether B is one value for every lane (a constant or an immediate),
 * and that value: arithmetic can then skip splatting it. */
static bool op_b_scalar(const Sm_Insn *in, const Sm_Env *env, uint32_t *value) {
  if (in->form == SM_FORM_CBUF) {
    *value = cbuf_read(env, in->cbuf, in->imm);
    return true;
  }
  if (in->form == SM_FORM_IMM) {
    *value = in->imm;
    return true;
  }
  return false;
}

/* The third (C) operand of three-source ops. */
static const uint32_t *op_c(const Sm_Insn *in, const Sm_Env *env, const Sm_Thread *t, uint32_t tmp[SM_LANES]) {
  if (in->form == SM_FORM_REG_CBUF) return splat(cbuf_read(env, in->cbuf, in->imm), tmp);
  return t->r[REG_C(in->raw)];
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

#define HALF_EXP_MANT_MASK 0x7fffu
#define HALF_INF_OR_NAN 0x7c00u
#define HALF_TO_FLOAT_SHIFT 13u
#define FLOAT_EXP_MASK 0x7f800000u
#define HALF_TO_FLOAT_SCALE 0x1p112f /* 2^(127 - 15): rebias the exponent */

/* Exact: the half's exponent and mantissa moved into float position read
 * as 2^-112 times the value (subnormals as float subnormals), so one exact
 * power-of-two multiply rebias them; infinities and NaNs are rebuilt. */
static float half_to_float(uint16_t h) {
  const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  const uint32_t em = h & HALF_EXP_MANT_MASK;
  if (em >= HALF_INF_OR_NAN) return f32(sign | FLOAT_EXP_MASK | ((em & 0x3ffu) << HALF_TO_FLOAT_SHIFT));
  return f32(u32f(f32(em << HALF_TO_FLOAT_SHIFT) * HALF_TO_FLOAT_SCALE) | sign);
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

/* ---- half precision ---------------------------------------------- */

/* Operand swizzles: which f16 of a register feed the low and high lanes
 * of a paired op (F32 reads the register as one f32 for both). */
#define HALF_SWZ_H1_H0 0u
#define HALF_SWZ_F32 1u
#define HALF_SWZ_H0_H0 2u
#define HALF_SWZ_H1_H1 3u
/* Result merges: both halves, the low result as an f32, or one half
 * inserted into the destination's existing value. */
#define HALF_MERGE_H1_H0 0u
#define HALF_MERGE_F32 1u
#define HALF_MERGE_MRG_H0 2u
#define HALF_MERGE_MRG_H1 3u
#define HALF_PRECISION_FMZ 2u /* D3D9 rule: anything * 0 = 0 */
#define HALF_ONE 0x3c00u
#define HALF_TRUE 0xffffu
#define HALF_LOW_MASK 0xffffu
#define HALF_HIGH_SHIFT 16u

typedef struct Half_Pair {
  float lo, hi;
} Half_Pair;

static Half_Pair half_extract(uint32_t v, uint32_t swizzle, uint32_t abs_bit, uint32_t neg_bit) {
  const float h0 = half_to_float((uint16_t)(v & HALF_LOW_MASK)), h1 = half_to_float((uint16_t)(v >> HALF_HIGH_SHIFT));
  Half_Pair p;
  switch (swizzle) {
  case HALF_SWZ_F32: p.lo = p.hi = f32(v); break;
  case HALF_SWZ_H0_H0: p.lo = p.hi = h0; break;
  case HALF_SWZ_H1_H1: p.lo = p.hi = h1; break;
  default: p.lo = h0; p.hi = h1; break;
  }
  p.lo = fmod_abs_neg(p.lo, abs_bit, neg_bit);
  p.hi = fmod_abs_neg(p.hi, abs_bit, neg_bit);
  return p;
}

static uint32_t half_merge(uint32_t old, Half_Pair r, uint32_t merge) {
  switch (merge) {
  case HALF_MERGE_F32: return u32f(r.lo);
  case HALF_MERGE_MRG_H0: return (old & ~HALF_LOW_MASK) | float_to_half(r.lo);
  case HALF_MERGE_MRG_H1: return (old & HALF_LOW_MASK) | ((uint32_t)float_to_half(r.hi) << HALF_HIGH_SHIFT);
  default: return float_to_half(r.lo) | ((uint32_t)float_to_half(r.hi) << HALF_HIGH_SHIFT);
  }
}

/* The modifier fields of HADD2/HMUL2/HFMA2/HSET2/HSETP2, which move per
 * op and form (layouts per the public Maxwell ISA descriptions). */
typedef struct Half_Fields {
  uint32_t swz_a, swz_b, swz_c;
  uint32_t abs_a, neg_a, abs_b, neg_b, neg_c;
  uint32_t sat, merge, precision;
  uint32_t cond, flag; /* HSET2: BF (1.0 not all-ones); HSETP2: H_AND */
} Half_Fields;

static Half_Fields half_fields(const Sm_Insn *in) {
  const uint64_t w = in->raw;
  Half_Fields f;
  memset(&f, 0, sizeof(f));
  f.swz_a = BITS(w, 47, 2);
  f.merge = BITS(w, 49, 2);
  f.swz_b = in->form == SM_FORM_CBUF ? HALF_SWZ_F32 : HALF_SWZ_H1_H0;
  f.swz_c = HALF_SWZ_H1_H0;
  const bool reg = in->form == SM_FORM_REG, imm32 = in->form == SM_FORM_IMM32;
  switch ((Sm_Op)in->op) {
  case SM_OP_HADD2:
  case SM_OP_HMUL2:
    if (imm32) {
      f.swz_a = BITS(w, 53, 2);
      f.merge = HALF_MERGE_H1_H0;
      f.sat = BIT(w, 52);
      if (in->op == SM_OP_HADD2) f.neg_a = BIT(w, 56);
      else f.precision = BITS(w, 55, 2);
      break;
    }
    f.abs_a = BIT(w, 44);
    f.neg_a = (in->op == SM_OP_HMUL2 && reg) ? 0u : BIT(w, 43);
    if (in->op == SM_OP_HMUL2) f.precision = BITS(w, 39, 2);
    if (reg) {
      f.sat = BIT(w, 32);
      f.neg_b = BIT(w, 31);
      f.abs_b = BIT(w, 30);
      f.swz_b = BITS(w, 28, 2);
    } else {
      f.sat = BIT(w, 52);
      if (in->form == SM_FORM_CBUF) {
        f.abs_b = BIT(w, 54);
        if (in->op == SM_OP_HADD2) f.neg_b = BIT(w, 56);
      }
    }
    break;
  case SM_OP_HFMA2:
    switch (in->form) {
    case SM_FORM_REG:
      f.swz_b = BITS(w, 28, 2);
      f.sat = BIT(w, 32);
      f.neg_b = BIT(w, 31);
      f.neg_c = BIT(w, 30);
      f.swz_c = BITS(w, 35, 2);
      f.precision = BITS(w, 37, 2);
      break;
    case SM_FORM_IMM32:
      f.swz_a = BITS(w, 53, 2);
      f.merge = HALF_MERGE_H1_H0;
      f.neg_c = BIT(w, 52);
      f.precision = BITS(w, 55, 2);
      break;
    default: /* b = Rc, c = cbuf (REG_CBUF); b = cbuf or imm, c = Rc */
      f.neg_c = BIT(w, 51);
      f.sat = BIT(w, 52);
      f.precision = BITS(w, 57, 2);
      if (in->form == SM_FORM_REG_CBUF) {
        f.swz_b = BITS(w, 53, 2);
        f.swz_c = HALF_SWZ_F32;
      } else {
        f.swz_c = BITS(w, 53, 2);
      }
      if (in->form != SM_FORM_IMM) f.neg_b = BIT(w, 56);
      break;
    }
    break;
  default: /* HSET2, HSETP2 */
    f.neg_a = BIT(w, 43);
    f.abs_a = BIT(w, 44);
    if (reg) {
      f.abs_b = BIT(w, 30);
      f.neg_b = BIT(w, 31);
      f.swz_b = BITS(w, 28, 2);
      f.cond = BITS(w, 35, 4);
      f.flag = BIT(w, 49);
    } else {
      f.cond = BITS(w, 49, 4);
      f.flag = BIT(w, 53);
      if (in->form == SM_FORM_CBUF) {
        f.neg_b = BIT(w, 56);
        if (in->op == SM_OP_HSETP2) f.abs_b = BIT(w, 54);
      }
    }
    break;
  }
  return f;
}

/* One lane of a product with the FMZ rule applied. */
static float half_mul(float a, float b, uint32_t precision) {
  if (precision == HALF_PRECISION_FMZ && (a == 0.0f || b == 0.0f)) return 0.0f;
  return a * b;
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

static void load_to_regs(Sm_Thread *t, uint32_t d, uint32_t size, const uint8_t *src, uint32_t l) {
  const uint32_t n = access_bytes(size);
  if (n < 4u) {
    uint32_t v = 0;
    memcpy(&v, src, n);
    if (size == 1) v = (uint32_t)(int32_t)(int8_t)v;
    if (size == 3) v = (uint32_t)(int32_t)(int16_t)v;
    dst_row(t, d)[l] = v;
    return;
  }
  for (uint32_t i = 0; i < n / 4u; i++) {
    uint32_t v;
    memcpy(&v, src + i * 4u, sizeof(v));
    if (d != SM_RZ) t->r[(d + i) & 0xffu][l] = v;
  }
  t->r[SM_RZ][l] = 0;
}

static void regs_to_bytes(const Sm_Thread *t, uint32_t d, uint32_t size, uint8_t *dst, uint32_t l) {
  const uint32_t n = access_bytes(size);
  if (n < 4u) {
    const uint32_t v = t->r[d][l];
    memcpy(dst, &v, n);
    return;
  }
  for (uint32_t i = 0; i < n / 4u; i++) {
    const uint32_t v = d == SM_RZ ? 0u : t->r[(d + i) & 0xffu][l];
    memcpy(dst + i * 4u, &v, sizeof(v));
  }
}

/* ---- textures ----------------------------------------------------- */

static uint32_t texture_handle(const Sm_Env *env, uint32_t index) {
  return cbuf_read(env, env->texture_cbuf_slot, index * 4u);
}

/* Reads `n` packed texture arguments: TEXS/TLDS put the first one or two
 * in Ra.. and the rest in Rb... */
static void tex_args(const Sm_Thread *t, uint64_t w, uint32_t n, uint32_t args[8], uint32_t l) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  uint32_t in_a, in_b;
  if (n <= 1u) { in_a = n; in_b = 0; }
  else if (n == 2u) { in_a = 1; in_b = 1; }
  else { in_a = 2; in_b = n - 2u; }
  for (uint32_t i = 0; i < in_a; i++) args[i] = a == SM_RZ ? 0u : t->r[(a + i) & 0xffu][l];
  for (uint32_t i = 0; i < in_b; i++) args[in_a + i] = b == SM_RZ ? 0u : t->r[(b + i) & 0xffu][l];
}

/* TEX/TLD/TLD4 vector packing: the array index and coordinates (the
 * first `in_a` arguments) in Ra.., everything after them - LOD/bias,
 * offsets, multisample index, depth reference - in Rb.., after `b_skip`
 * registers (TEX.B's handle). */
static void tex_args_vec(const Sm_Thread *t, uint64_t w, uint32_t n, uint32_t in_a, uint32_t args[8], uint32_t l,
                         uint32_t b_skip) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  for (uint32_t i = 0; i < n && i < 8u; i++) {
    const uint32_t base = i < in_a ? a : b;
    const uint32_t k = i < in_a ? i : i - in_a + b_skip;
    args[i] = base == SM_RZ ? 0u : t->r[(base + k) & 0xffu][l];
  }
}

/* TEXS/TLDS/TLD4S destinations: components in order to Rd, Rd+1, Rd2,
 * Rd2+1. */
static void write_scalar_results(Sm_Thread *t, uint64_t w, const uint32_t *values, uint32_t count, uint32_t l) {
  const uint32_t d0 = REG_D(w), d1 = BITS(w, 28, 8);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t base = i < 2u ? d0 : d1;
    if (base == SM_RZ) continue;
    t->r[(base + (i & 1u)) & 0xffu][l] = values[i];
  }
  t->r[SM_RZ][l] = 0;
}

/* TEXS/TLDS with bit 59 (SM_TEXS_F32_BIT) clear: the components as
 * half floats, two to a register - (c0, c1) in Rd, (c2, c3) in Rd2; a
 * missing second half is 0. */
static void write_scalar_results_half(Sm_Thread *t, uint64_t w, const uint32_t *values, uint32_t count, uint32_t l) {
  const uint32_t d0 = REG_D(w), d1 = BITS(w, 28, 8);
  for (uint32_t i = 0; i < count; i += 2u) {
    const uint32_t base = i < 2u ? d0 : d1;
    if (base == SM_RZ) continue;
    const uint32_t lo = float_to_half(f32(values[i]));
    const uint32_t hi = i + 1u < count ? float_to_half(f32(values[i + 1u])) : 0u;
    t->r[base][l] = lo | (hi << HALF_HIGH_SHIFT);
  }
  t->r[SM_RZ][l] = 0;
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

/* Samples every lane in `m` (requests[l] -> out[l]): the batch hook when
 * the environment has one, else one call per lane. Unanswered lanes read
 * (0, 0, 0, 1). Lanes are independent, so callers build every request
 * first and write every result after. */
static void tex_lanes(const Sm_Env *env, const Sm_Tex_Request *requests, Sm_Mask m, uint32_t (*out)[4]) {
  FOR_LANES(m) {
    out[l][0] = out[l][1] = out[l][2] = 0;
    out[l][3] = u32f(1.0f);
  }
  if (env->texture_batch) {
    env->texture_batch(env->user, requests, m, out);
    return;
  }
  if (!env->texture) return;
  FOR_LANES(m) env->texture(env->user, &requests[l], out[l]);
}

static void exec_texs(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t, Sm_Mask m) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4);
  const uint32_t handle = texture_handle(env, BITS(w, 36, 13));
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  Sm_Tex_Request reqs[SM_LANES];
  uint32_t texels[SM_LANES][4];
  FOR_LANES(m) {
    Sm_Tex_Request *req = &reqs[l];
    memset(req, 0, sizeof(*req));
    req->pc = g_current_pc;
    req->kind = SM_TEX_SAMPLE;
    req->handle = handle;
    uint32_t args[8] = {0};
    float x = 0, y = 0, z = 0;
    switch (target) {
    case 0: tex_args(t, w, 1, args, l); req->dims = 1; req->has_lod = true; x = f32(args[0]); break;
    case 1: tex_args(t, w, 2, args, l); req->dims = 2; x = f32(args[0]); y = f32(args[1]); break;
    case 2: tex_args(t, w, 2, args, l); req->dims = 2; req->has_lod = true; x = f32(args[0]); y = f32(args[1]); break;
    case 3: tex_args(t, w, 3, args, l); req->dims = 2; req->has_lod = true; x = f32(args[0]); y = f32(args[1]); req->lod = f32(args[2]); break;
    case 4: tex_args(t, w, 3, args, l); req->dims = 2; req->shadow = true; x = f32(args[0]); y = f32(args[1]); req->dref = f32(args[2]); break;
    case 5: tex_args(t, w, 4, args, l); req->dims = 2; req->shadow = true; req->has_lod = true; x = f32(args[0]); y = f32(args[1]); req->lod = f32(args[2]); req->dref = f32(args[3]); break;
    case 6: tex_args(t, w, 3, args, l); req->dims = 2; req->shadow = true; req->has_lod = true; x = f32(args[0]); y = f32(args[1]); req->dref = f32(args[2]); break;
    case 7: tex_args(t, w, 3, args, l); req->dims = 2; req->array = true; req->layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); break;
    case 8: tex_args(t, w, 3, args, l); req->dims = 2; req->array = true; req->has_lod = true; req->layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); break;
    case 9: tex_args(t, w, 4, args, l); req->dims = 2; req->array = true; req->shadow = true; req->has_lod = true; req->layer = (float)args[0]; x = f32(args[1]); y = f32(args[2]); req->dref = f32(args[3]); break;
    case 10: case 11: tex_args(t, w, 3, args, l); req->dims = 3; req->has_lod = target == 11u; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); break;
    case 12: tex_args(t, w, 3, args, l); req->dims = 3; req->cube = true; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); break;
    case 13: tex_args(t, w, 4, args, l); req->dims = 3; req->cube = true; req->has_lod = true; x = f32(args[0]); y = f32(args[1]); z = f32(args[2]); req->lod = f32(args[3]); break;
    default: req->dims = 2; break;
    }
    req->coords[0] = x;
    req->coords[1] = y;
    req->coords[2] = z;
  }
  tex_lanes(env, reqs, m, texels);
  FOR_LANES(m) {
    uint32_t values[4];
    for (uint32_t i = 0; i < n; i++) values[i] = texels[l][comps[i]];
    if (BIT(w, SM_TEXS_F32_BIT)) write_scalar_results(t, w, values, n, l);
    else write_scalar_results_half(t, w, values, n, l);
  }
}

static void exec_tlds(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t, Sm_Mask m) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4);
  const uint32_t handle = texture_handle(env, BITS(w, 36, 13));
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  Sm_Tex_Request reqs[SM_LANES];
  uint32_t texels[SM_LANES][4];
  FOR_LANES(m) {
    Sm_Tex_Request *req = &reqs[l];
    memset(req, 0, sizeof(*req));
    req->pc = g_current_pc;
    req->kind = SM_TEX_FETCH;
    req->handle = handle;
    uint32_t args[8] = {0};
    switch (target) {
    case 0: tex_args(t, w, 1, args, l); req->dims = 1; req->icoords[0] = (int32_t)args[0]; break;
    case 1: tex_args(t, w, 2, args, l); req->dims = 1; req->icoords[0] = (int32_t)args[0]; req->ilod = (int32_t)args[1]; break;
    case 2: tex_args(t, w, 2, args, l); req->dims = 2; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1]; break;
    case 4: tex_args(t, w, 3, args, l); req->dims = 2; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1];
      req->offset[0] = (int32_t)(args[2] << 28) >> 28; req->offset[1] = (int32_t)(args[2] << 24) >> 28; break;
    case 5: tex_args(t, w, 3, args, l); req->dims = 2; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1]; req->ilod = (int32_t)args[2]; break;
    case 6: tex_args(t, w, 3, args, l); req->dims = 2; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1]; break;
    case 7: tex_args(t, w, 3, args, l); req->dims = 3; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1]; req->icoords[2] = (int32_t)args[2]; break;
    case 8: tex_args(t, w, 3, args, l); req->dims = 2; req->array = true; req->layer = (float)args[0]; req->icoords[0] = (int32_t)args[1]; req->icoords[1] = (int32_t)args[2]; break;
    case 12: tex_args(t, w, 4, args, l); req->dims = 2; req->icoords[0] = (int32_t)args[0]; req->icoords[1] = (int32_t)args[1]; req->ilod = (int32_t)args[2];
      req->offset[0] = (int32_t)(args[3] << 28) >> 28; req->offset[1] = (int32_t)(args[3] << 24) >> 28; break;
    default: req->dims = 2; break;
    }
  }
  tex_lanes(env, reqs, m, texels);
  FOR_LANES(m) {
    uint32_t values[4];
    for (uint32_t i = 0; i < n; i++) values[i] = texels[l][comps[i]];
    if (BIT(w, SM_TEXS_F32_BIT)) write_scalar_results(t, w, values, n, l);
    else write_scalar_results_half(t, w, values, n, l);
  }
}

static void exec_tld4s(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t, Sm_Mask m) {
  const uint64_t w = in->raw;
  const uint32_t handle = texture_handle(env, BITS(w, 36, 13));
  Sm_Tex_Request reqs[SM_LANES];
  uint32_t texels[SM_LANES][4];
  FOR_LANES(m) {
    Sm_Tex_Request *req = &reqs[l];
    memset(req, 0, sizeof(*req));
    req->pc = g_current_pc;
    req->kind = SM_TEX_GATHER;
    req->handle = handle;
    req->dims = 2;
    req->gather_component = (uint8_t)BITS(w, 52, 2);
    req->has_offset = BIT(w, 51) != 0;
    req->shadow = BIT(w, 50) != 0;
    const uint32_t n = 2u + (req->has_offset ? 1u : 0u) + (req->shadow ? 1u : 0u);
    uint32_t args[8] = {0};
    tex_args(t, w, n, args, l);
    req->coords[0] = f32(args[0]);
    req->coords[1] = f32(args[1]);
    uint32_t k = 2;
    if (req->has_offset) {
      req->offset[0] = (int32_t)(args[k] << 26) >> 26;
      req->offset[1] = (int32_t)(args[k] << 18) >> 26;
      k++;
    }
    if (req->shadow) req->dref = f32(args[k]);
  }
  tex_lanes(env, reqs, m, texels);
  FOR_LANES(m) write_scalar_results(t, w, texels[l], 4, l);
}

/* TEX / TLD / TLD4 / TXD / TXQ / TMML (vector forms). */
static void exec_tex_vector(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t, Sm_Mask m) {
  const uint64_t w = in->raw;
  const uint32_t handle = texture_handle(env, BITS(w, 36, 13));
  const uint32_t mask = BITS(w, 31, 4);
  Sm_Tex_Request reqs[SM_LANES];
  uint32_t texels[SM_LANES][4];
  Sm_Mask sampled = 0; /* lanes whose result comes from the texture unit */
  FOR_LANES(m) {
    Sm_Tex_Request *req = &reqs[l];
    memset(req, 0, sizeof(*req));
    req->pc = g_current_pc;
    /* TEX.B (bindless): the handle is Rb's first register, in the same
     * {TIC index, TSC index << 20} encoding the constant buffer holds;
     * Rb's other arguments follow it. */
    req->handle = in->op == SM_OP_TEX_B ? (REG_B(w) == SM_RZ ? 0u : t->r[REG_B(w)][l]) : handle;
    const uint32_t dim = BITS(w, 29, 2);
    req->dims = (uint8_t)(dim == 3u ? 3u : dim + 1u);
    req->cube = dim == 3u;
    req->array = BIT(w, 28) != 0;
    const uint32_t coord_count = req->dims;
    uint32_t args[8] = {0};
    uint32_t n = (req->array ? 1u : 0u) + coord_count;
    bool lod = false, bias = false, offset = false, dc = false, ms = false;
    switch (in->op) {
    case SM_OP_TEX: {
      const uint32_t lodm = BITS(w, 55, 2);
      req->kind = SM_TEX_SAMPLE;
      lod = lodm == 3u;
      bias = lodm == 2u;
      req->has_lod = lodm == 1u || lodm == 3u;
      offset = BIT(w, 54) != 0;
      dc = BIT(w, 50) != 0;
      break;
    }
    case SM_OP_TEX_B: { /* the TEX fields, lodm and offset moved down (36-38) */
      const uint32_t lodm = BITS(w, 37, 2);
      req->kind = SM_TEX_SAMPLE;
      lod = lodm == 3u;
      bias = lodm == 2u;
      req->has_lod = lodm == 1u || lodm == 3u;
      offset = BIT(w, 36) != 0;
      dc = BIT(w, 50) != 0;
      break;
    }
    case SM_OP_TLD:
      req->kind = SM_TEX_FETCH;
      lod = BIT(w, 55) != 0;
      ms = BIT(w, 50) != 0;
      offset = BIT(w, 35) != 0;
      break;
    case SM_OP_TLD4:
      req->kind = SM_TEX_GATHER;
      req->gather_component = (uint8_t)BITS(w, 56, 2);
      offset = BITS(w, 54, 2) != 0;
      dc = BIT(w, 50) != 0;
      break;
    case SM_OP_TXD:
      req->kind = SM_TEX_SAMPLE;
      req->has_lod = true; /* derivatives ignored: base level */
      break;
    case SM_OP_TXQ:
      req->kind = SM_TEX_QUERY_DIMS;
      break;
    default: /* TMML */
      req->kind = SM_TEX_QUERY_LOD;
      break;
    }
    if (req->kind == SM_TEX_QUERY_DIMS) {
      req->ilod = (int32_t)t->r[REG_A(w)][l];
      if (BITS(w, 22, 6) == 1u) sampled |= (Sm_Mask)(1u << l); /* other queries read 0 */
    } else if (req->kind == SM_TEX_QUERY_LOD) {
      /* reads 0 */
    } else {
      n += (lod || bias) ? 1u : 0u;
      n += offset ? 1u : 0u;
      n += ms ? 1u : 0u;
      n += dc ? 1u : 0u;
      tex_args_vec(t, w, n, (req->array ? 1u : 0u) + coord_count, args, l, in->op == SM_OP_TEX_B ? 1u : 0u);
      uint32_t k = 0;
      if (req->array) req->layer = (float)(args[k++] & 0xffffu);
      for (uint32_t c = 0; c < coord_count; c++) {
        if (req->kind == SM_TEX_FETCH) req->icoords[c] = (int32_t)args[k];
        else req->coords[c] = f32(args[k]);
        k++;
      }
      if (lod || bias) {
        if (req->kind == SM_TEX_FETCH) req->ilod = (int32_t)args[k];
        else req->lod = f32(args[k]);
        req->has_lod = lod;
        req->has_bias = bias;
        k++;
      }
      if (offset) {
        req->has_offset = true;
        for (uint32_t c = 0; c < coord_count && c < 3u; c++) req->offset[c] = (int32_t)(args[k] << (28u - 4u * c)) >> 28;
        k++;
      }
      if (ms) k++;
      if (dc) {
        req->shadow = true;
        req->dref = f32(args[k]);
      }
      sampled |= (Sm_Mask)(1u << l);
    }
  }
  tex_lanes(env, reqs, sampled, texels);
  FOR_LANES(m) {
    const uint32_t *out = texels[l];
    static const uint32_t zero[4] = {0, 0, 0, 0};
    if (!LANE(sampled, l)) out = zero;
    uint32_t d = REG_D(w);
    for (uint32_t c = 0; c < 4; c++) {
      if (!(mask & (1u << c))) continue;
      if (d != SM_RZ) {
        t->r[d][l] = out[c];
        d = (d + 1u) & 0xffu;
      }
    }
    t->r[SM_RZ][l] = 0;
  }
}

/* ---- the interpreter ---------------------------------------------- */

typedef struct Stack_Entry {
  uint32_t kind;
  uint32_t target;
} Stack_Entry;

#define FLOW_DEPTH (SM_STACK_DEPTH * 3u)
#define MAX_WARPS SM_LANES

/* A group of lanes at one program counter, with its own reconvergence
 * (SSY/PBK/PCNT) and call stacks. */
typedef struct Warp {
  Sm_Mask mask;
  uint32_t pc;
  uint32_t depth;
  Stack_Entry flow[FLOW_DEPTH];
  uint32_t call_depth;
  uint32_t calls[SM_STACK_DEPTH];
} Warp;

static bool flow_push(Warp *w, uint32_t kind, int32_t target) {
  if (target < 0 || w->depth >= FLOW_DEPTH) return false;
  w->flow[w->depth].kind = kind;
  w->flow[w->depth].target = (uint32_t)target;
  w->depth++;
  return true;
}

/* Pops to the most recent entry of `kind`; keeps it when `keep`.
 * Returns its target or -1. */
static int32_t flow_pop_to(Warp *w, uint32_t kind, bool keep) {
  while (w->depth > 0) {
    const Stack_Entry e = w->flow[w->depth - 1u];
    if (e.kind == kind) {
      if (!keep) w->depth--;
      return (int32_t)e.target;
    }
    w->depth--;
  }
  return -1;
}

void sm_thread_reset(Sm_Thread *t, uint32_t lanes) {
  memset(t->r, 0, sizeof(t->r));
  sm_thread_reset_light(t, lanes);
}

void sm_thread_reset_light(Sm_Thread *t, uint32_t lanes) {
  t->lanes = lanes == 0 ? 1u : (lanes > SM_LANES ? SM_LANES : lanes);
  for (uint32_t i = 0; i < SM_PREDICATES; i++) t->p[i] = 0;
  t->p[SM_PT] = SM_ALL_LANES;
  t->cc_carry = t->cc_zero = t->cc_sign = t->cc_overflow = 0;
  t->killed = 0;
  t->faulted = false;
  memset(t->r[SM_RZ], 0, sizeof(t->r[SM_RZ]));
}

#define SR_TID 0x20u /* x | y << 16 | z << 26 */
#define SR_TID_X 0x21u
#define SR_TID_Y 0x22u
#define SR_TID_Z 0x23u
#define SR_CTAID_X 0x25u
#define SR_CTAID_Y 0x26u
#define SR_CTAID_Z 0x27u
#define SR_TID_Y_SHIFT 16u
#define SR_TID_Z_SHIFT 26u

static uint32_t sysreg(const Sm_Thread *t, uint32_t id, uint32_t lane) {
  static _Thread_local uint32_t clock;
  switch (id) {
  case SR_TID: return t->tid[0][lane] | (t->tid[1][lane] << SR_TID_Y_SHIFT) | (t->tid[2][lane] << SR_TID_Z_SHIFT);
  case SR_TID_X: case SR_TID_Y: case SR_TID_Z: return t->tid[id - SR_TID_X][lane];
  case SR_CTAID_X: case SR_CTAID_Y: case SR_CTAID_Z: return t->ctaid[id - SR_CTAID_X];
  case 0x00: return lane;        /* lane id */
  case 0x12: return u32f(1.0f);  /* Y direction */
  case 0x38: return 1u << lane;  /* lanemask eq */
  case 0x39: return (1u << lane) - 1u;
  case 0x3a: return (2u << lane) - 1u;
  case 0x3b: return ~((2u << lane) - 1u);
  case 0x3c: return ~((1u << lane) - 1u);
  case 0x50: return clock++;
  default: return 0;
  }
}

/* Executes the non-control instruction `in` for the lanes in `m`. */
static void execute(const Sm_Insn *in, const Sm_Env *env, Sm_Thread *t, Sm_Mask m) {
  const uint64_t w = in->raw;
  uint32_t tb[SM_LANES], tc[SM_LANES];
  switch ((Sm_Op)in->op) {
  case SM_OP_NOP:
  case SM_OP_SCHED:
  case SM_OP_BARRIER:
    return;

  /* ---- float ---- */
  case SM_OP_FADD: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t aa = BIT(w, 46), na = BIT(w, 48), ab = BIT(w, 49), nb = BIT(w, 45), sat = BIT(w, 50);
    uint32_t res[SM_LANES];
    uint32_t bs;
    if (op_b_scalar(in, env, &bs)) {
      const float bv = fmod_abs_neg(f32(bs), ab, nb);
      FOR_ALL_LANES {
        float r = fmod_abs_neg(f32(a[l]), aa, na) + bv;
        if (sat) r = saturate(r);
        res[l] = u32f(r);
      }
      store_masked(d, res, m);
      return;
    }
    const uint32_t *b = op_b(in, env, t, tb);
    FOR_ALL_LANES {
      float r = fmod_abs_neg(f32(a[l]), aa, na) + fmod_abs_neg(f32(b[l]), ab, nb);
      if (sat) r = saturate(r);
      res[l] = u32f(r);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FADD32I: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const float b = fmod_abs_neg(f32(in->imm), BIT(w, 57), BIT(w, 53));
    const uint32_t aa = BIT(w, 54), na = BIT(w, 56);
    uint32_t res[SM_LANES];
    FOR_ALL_LANES res[l] = u32f(fmod_abs_neg(f32(a[l]), aa, na) + b);
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FMUL: {
    /* @41: 1-3 divide by 2^n, 4-6 multiply by 8, 4, 2. */
    static const float scale[8] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f, 1.0f};
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const float k = scale[BITS(w, 41, 3)] * (BIT(w, 48) ? -1.0f : 1.0f);
    const uint32_t sat = BIT(w, 50);
    uint32_t res[SM_LANES];
    uint32_t bs;
    if (op_b_scalar(in, env, &bs)) {
      const float bv = f32(bs);
      FOR_ALL_LANES {
        float r = f32(a[l]) * bv * k;
        if (sat) r = saturate(r);
        res[l] = u32f(r);
      }
      store_masked(d, res, m);
      return;
    }
    const uint32_t *b = op_b(in, env, t, tb);
    FOR_ALL_LANES {
      float r = f32(a[l]) * f32(b[l]) * k;
      if (sat) r = saturate(r);
      res[l] = u32f(r);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FMUL32I: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const float b = f32(in->imm);
    const uint32_t sat = BIT(w, 55);
    uint32_t res[SM_LANES];
    FOR_ALL_LANES {
      float r = f32(a[l]) * b;
      if (sat) r = saturate(r);
      res[l] = u32f(r);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FFMA: {
    const uint32_t *a = t->r[REG_A(w)], *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    const float nab = BIT(w, 48) ? -1.0f : 1.0f, nc = BIT(w, 49) ? -1.0f : 1.0f;
    const uint32_t sat = BIT(w, 50);
    uint32_t res[SM_LANES];
    uint32_t bs;
    if (op_b_scalar(in, env, &bs)) {
      const float bv = f32(bs);
      FOR_ALL_LANES {
        float r = f32(a[l]) * bv * nab + f32(c[l]) * nc;
        if (sat) r = saturate(r);
        res[l] = u32f(r);
      }
      store_masked(d, res, m);
      return;
    }
    const uint32_t *b = op_b(in, env, t, tb);
    FOR_ALL_LANES {
      float r = f32(a[l]) * f32(b[l]) * nab + f32(c[l]) * nc;
      if (sat) r = saturate(r);
      res[l] = u32f(r);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FFMA32I: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const float b = f32(in->imm) * (BIT(w, 56) ? -1.0f : 1.0f), nc = BIT(w, 57) ? -1.0f : 1.0f;
    const uint32_t sat = BIT(w, 55);
    uint32_t res[SM_LANES];
    FOR_ALL_LANES {
      float r = f32(a[l]) * b + f32(d[l]) * nc;
      if (sat) r = saturate(r);
      res[l] = u32f(r);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FMNMX: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const Sm_Mask mins = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t aa = BIT(w, 46), na = BIT(w, 48), ab = BIT(w, 49), nb = BIT(w, 45);
    uint32_t res[SM_LANES];
    FOR_ALL_LANES {
      const float x = fmod_abs_neg(f32(a[l]), aa, na), y = fmod_abs_neg(f32(b[l]), ab, nb);
      res[l] = u32f(LANE(mins, l) ? fminf(x, y) : fmaxf(x, y));
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_FSET: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t cond = BITS(w, 48, 4), bop = BITS(w, 45, 2), aa = BIT(w, 54), na = BIT(w, 43), ab = BIT(w, 44),
                   nb = BIT(w, 53);
    const uint32_t yes = BIT(w, 52) ? u32f(1.0f) : 0xffffffffu;
    FOR_LANES(m) {
      const bool r = bool_op(bop, fcompare(cond, fmod_abs_neg(f32(a[l]), aa, na), fmod_abs_neg(f32(b[l]), ab, nb)),
                             LANE(pc, l) != 0);
      d[l] = r ? yes : 0u;
    }
    return;
  }
  case SM_OP_FSETP: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t cond = BITS(w, 48, 4), bop = BITS(w, 45, 2), aa = BIT(w, 7), na = BIT(w, 43), ab = BIT(w, 44),
                   nb = BIT(w, 6);
    Sm_Mask p0 = 0, p1 = 0;
    FOR_LANES(m) {
      const bool cmp = fcompare(cond, fmod_abs_neg(f32(a[l]), aa, na), fmod_abs_neg(f32(b[l]), ab, nb));
      const bool c = LANE(pc, l) != 0;
      if (bool_op(bop, cmp, c)) p0 |= (Sm_Mask)(1u << l);
      if (bool_op(bop, !cmp, c)) p1 |= (Sm_Mask)(1u << l);
    }
    set_pred(t, BITS(w, 3, 3), m, p0);
    set_pred(t, BITS(w, 0, 3), m, p1);
    return;
  }
  case SM_OP_HADD2:
  case SM_OP_HMUL2:
  case SM_OP_HFMA2: {
    const Half_Fields f = half_fields(in);
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    /* HFMA2.32I accumulates into its destination. */
    const uint32_t *c = in->op != SM_OP_HFMA2 ? NULL
                        : in->form == SM_FORM_IMM32 ? t->r[REG_D(w)]
                                                    : op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    uint32_t res[SM_LANES];
    FOR_ALL_LANES {
      const Half_Pair x = half_extract(a[l], f.swz_a, f.abs_a, f.neg_a);
      const Half_Pair y = half_extract(b[l], f.swz_b, f.abs_b, f.neg_b);
      Half_Pair r;
      if (in->op == SM_OP_HADD2) {
        r.lo = x.lo + y.lo;
        r.hi = x.hi + y.hi;
      } else {
        r.lo = half_mul(x.lo, y.lo, f.precision);
        r.hi = half_mul(x.hi, y.hi, f.precision);
        if (c) {
          const Half_Pair z = half_extract(c[l], f.swz_c, 0, f.neg_c);
          r.lo += z.lo;
          r.hi += z.hi;
        }
      }
      if (f.sat) {
        r.lo = saturate(r.lo);
        r.hi = saturate(r.hi);
      }
      res[l] = half_merge(d[l], r, f.merge);
    }
    store_masked(d, res, m);
    return;
  }
  case SM_OP_HSET2:
  case SM_OP_HSETP2: {
    const Half_Fields f = half_fields(in);
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t bop = BITS(w, 45, 2);
    const uint32_t yes = f.flag ? HALF_ONE : HALF_TRUE;
    uint32_t *d = dst_row(t, REG_D(w));
    Sm_Mask p0 = 0, p1 = 0;
    FOR_LANES(m) {
      const Half_Pair x = half_extract(a[l], f.swz_a, f.abs_a, f.neg_a);
      const Half_Pair y = half_extract(b[l], f.swz_b, f.abs_b, f.neg_b);
      const bool c = LANE(pc, l) != 0;
      const bool lo = bool_op(bop, fcompare(f.cond, x.lo, y.lo), c);
      const bool hi = bool_op(bop, fcompare(f.cond, x.hi, y.hi), c);
      if (in->op == SM_OP_HSET2) {
        d[l] = (lo ? yes : 0u) | (hi ? yes << HALF_HIGH_SHIFT : 0u);
        continue;
      }
      const bool first = f.flag ? (lo && hi) : lo, second = f.flag ? !(lo && hi) : hi;
      if (first) p0 |= (Sm_Mask)(1u << l);
      if (second) p1 |= (Sm_Mask)(1u << l);
    }
    if (in->op == SM_OP_HSETP2) {
      set_pred(t, BITS(w, 3, 3), m, p0);
      set_pred(t, BITS(w, 0, 3), m, p1);
    }
    return;
  }
  case SM_OP_FCMP: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t cond = BITS(w, 48, 4);
    LANES_ASSIGN(m, d, fcompare(cond, f32(c[l]), 0.0f) ? a[l] : b[l]);
    return;
  }
  case SM_OP_MUFU: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t fn = BITS(w, 20, 4), aa = BIT(w, 46), na = BIT(w, 48), sat = BIT(w, 50);
    FOR_LANES(m) {
      const float x = fmod_abs_neg(f32(a[l]), aa, na);
      float r;
      switch (fn) {
      case 0: r = cosf(x); break;
      case 1: r = sinf(x); break;
      case 2: r = exp2f(x); break;
      case 3: r = log2f(x); break;
      case 4: r = 1.0f / x; break;
      case 5: r = 1.0f / sqrtf(x); break;
      case 8: r = sqrtf(x); break;
      default: r = x; break;
      }
      if (sat) r = saturate(r);
      d[l] = u32f(r);
    }
    return;
  }
  case SM_OP_RRO: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t ab = BIT(w, 49), nb = BIT(w, 45);
    LANES_ASSIGN(m, d, u32f(fmod_abs_neg(f32(b[l]), ab, nb)));
    return;
  }
  case SM_OP_FSWZADD: {
    /* Per quad lane q, mask bits 2q..2q+1 pick ADD (a + b), SUBR (b - a),
     * SUB (a - b) or MOV2 (b) - with a SHFL of the neighbour, the
     * screen-space derivative compilers build dFdx/dFdy from. */
    static const float k_a[4] = {1.0f, -1.0f, 1.0f, 0.0f}, k_b[4] = {1.0f, 1.0f, -1.0f, 1.0f};
    const uint32_t *a = t->r[REG_A(w)], *b = t->r[REG_B(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t mask = BITS(w, 28, 8);
    LANES_ASSIGN(m, d, u32f(f32(a[l]) * k_a[(mask >> (2u * (l & 3u))) & 3u] + f32(b[l]) * k_b[(mask >> (2u * (l & 3u))) & 3u]));
    return;
  }
  case SM_OP_F2F: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t src_size = BITS(w, 10, 2), dst_size = BITS(w, 8, 2), hi = BIT(w, 41), ab = BIT(w, 49),
                   nb = BIT(w, 45), ri = BIT(w, 42), rm = BITS(w, 39, 2), sat = BIT(w, 50);
    FOR_LANES(m) {
      float v = src_size == 1u ? half_to_float((uint16_t)(b[l] >> (hi ? 16u : 0u))) : f32(b[l]);
      v = fmod_abs_neg(v, ab, nb);
      if (ri) v = round_mode(v, rm);
      if (sat) v = saturate(v);
      d[l] = dst_size == 1u ? (uint32_t)float_to_half(v) : u32f(v);
    }
    return;
  }
  case SM_OP_F2I: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t src_size = BITS(w, 10, 2), ab = BIT(w, 49), nb = BIT(w, 45), rm = BITS(w, 39, 2),
                   dst_size = BITS(w, 8, 2);
    const bool is_signed = BIT(w, 12) != 0;
    FOR_LANES(m) {
      float v = src_size == 1u ? half_to_float((uint16_t)b[l]) : f32(b[l]);
      v = round_mode(fmod_abs_neg(v, ab, nb), rm);
      d[l] = float_to_int(v, is_signed, dst_size);
    }
    return;
  }
  case SM_OP_I2F: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool is_signed = BIT(w, 13) != 0;
    const uint32_t src_size = BITS(w, 10, 2), sel = BITS(w, 41, 2), ab = BIT(w, 49), nb = BIT(w, 45),
                   dst_size = BITS(w, 8, 2);
    FOR_LANES(m) {
      int64_t v = int_source(b[l], src_size, sel, is_signed);
      if (ab && v < 0) v = -v;
      if (nb) v = -v;
      const float f = is_signed || v < 0 ? (float)v : (float)(uint64_t)v;
      d[l] = dst_size == 1u ? (uint32_t)float_to_half(f) : u32f(f);
    }
    return;
  }
  case SM_OP_I2I: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool src_signed = BIT(w, 13) != 0, dst_signed = BIT(w, 12) != 0, sat = BIT(w, 50) != 0;
    const uint32_t src_size = BITS(w, 10, 2), sel = BITS(w, 41, 2), ab = BIT(w, 49), nb = BIT(w, 45),
                   dst_size = BITS(w, 8, 2);
    FOR_LANES(m) {
      int64_t v = int_source(b[l], src_size, sel, src_signed);
      if (ab && v < 0) v = -v;
      if (nb) v = -v;
      d[l] = clamp_int(v, dst_size, dst_signed, sat);
    }
    return;
  }

  /* ---- integer ---- */
  case SM_OP_IADD:
  case SM_OP_IADD32I: {
    const bool is32i = in->op == SM_OP_IADD32I;
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t na = is32i ? BIT(w, 56) : BIT(w, 49), nb = is32i ? 0u : BIT(w, 48);
    const bool x = is32i ? BIT(w, 53) != 0 : BIT(w, 43) != 0;
    const bool sat = is32i ? BIT(w, 54) != 0 : BIT(w, 50) != 0;
    const bool cc = is32i ? BIT(w, 52) != 0 : BIT(w, 47) != 0;
    Sm_Mask carry = 0, zero = 0, sign = 0, over = 0;
    FOR_LANES(m) {
      uint32_t av = a[l], bv = b[l];
      if (na) av = (uint32_t)(-(int64_t)av);
      if (nb) bv = (uint32_t)(-(int64_t)bv);
      const uint64_t sum = (uint64_t)av + (uint64_t)bv + (x && LANE(t->cc_carry, l) ? 1u : 0u);
      uint32_t r = (uint32_t)sum;
      if (sat) {
        const int64_t s = (int64_t)(int32_t)av + (int64_t)(int32_t)bv;
        r = s > INT32_MAX ? (uint32_t)INT32_MAX : s < INT32_MIN ? (uint32_t)INT32_MIN : (uint32_t)s;
      }
      if (sum >> 32) carry |= (Sm_Mask)(1u << l);
      if (((~(av ^ bv) & (av ^ r)) >> 31) != 0) over |= (Sm_Mask)(1u << l);
      if (r == 0) zero |= (Sm_Mask)(1u << l);
      if (r >> 31) sign |= (Sm_Mask)(1u << l);
      d[l] = r;
    }
    if (cc) {
      set_mask(&t->cc_carry, m, carry);
      set_mask(&t->cc_overflow, m, over);
      set_mask(&t->cc_zero, m, zero);
      set_mask(&t->cc_sign, m, sign);
    }
    return;
  }
  case SM_OP_IADD3: {
    /* Rd = A + B + C, each optionally negated; the register form can
     * take a 16-bit half of each and shift A + B by 16 before adding C. */
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = t->r[REG_C(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const bool reg_form = in->form == SM_FORM_REG;
    const uint32_t mode = reg_form ? BITS(w, IADD3_MODE_BIT, 2) : 0u;
    const uint32_t heights[3] = {reg_form ? BITS(w, IADD3_HEIGHT_A_BIT, 2) : 0u, reg_form ? BITS(w, IADD3_HEIGHT_B_BIT, 2) : 0u,
                                 reg_form ? BITS(w, IADD3_HEIGHT_C_BIT, 2) : 0u};
    const uint32_t negs[3] = {BIT(w, IADD3_NEG_A_BIT), BIT(w, IADD3_NEG_B_BIT), BIT(w, IADD3_NEG_C_BIT)};
    const bool x = BIT(w, IADD3_X_BIT) != 0, cc = BIT(w, IADD3_CC_BIT) != 0;
    Sm_Mask carry = 0, zero = 0, sign = 0;
    FOR_LANES(m) {
      uint32_t v[3] = {a[l], b[l], c[l]};
      for (uint32_t k = 0; k < 3u; k++) {
        if (heights[k] == IADD3_HEIGHT_LOWER) v[k] &= HALF_LOW_MASK;
        else if (heights[k] == IADD3_HEIGHT_UPPER) v[k] >>= HALF_HIGH_SHIFT;
        if (negs[k]) v[k] = (uint32_t)(-(int64_t)v[k]);
      }
      uint64_t ab = (uint64_t)v[0] + v[1];
      if (mode == IADD3_MODE_RIGHT_SHIFT) ab = (uint32_t)ab >> HALF_HIGH_SHIFT;
      else if (mode == IADD3_MODE_LEFT_SHIFT) ab = (uint64_t)((uint32_t)ab << HALF_HIGH_SHIFT);
      const uint64_t sum = ab + v[2] + (x && LANE(t->cc_carry, l) ? 1u : 0u);
      const uint32_t r = (uint32_t)sum;
      if (sum >> 32) carry |= (Sm_Mask)(1u << l);
      if (r == 0) zero |= (Sm_Mask)(1u << l);
      if (r >> 31) sign |= (Sm_Mask)(1u << l);
      d[l] = r;
    }
    if (cc) {
      set_mask(&t->cc_carry, m, carry);
      set_mask(&t->cc_overflow, m, 0);
      set_mask(&t->cc_zero, m, zero);
      set_mask(&t->cc_sign, m, sign);
    }
    return;
  }
  case SM_OP_ISCADD: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t na = BIT(w, 49), nb = BIT(w, 48), shift = BITS(w, 39, 5);
    Sm_Mask zero = 0, sign = 0;
    FOR_LANES(m) {
      uint32_t av = a[l], bv = b[l];
      if (na) av = (uint32_t)(-(int64_t)av);
      if (nb) bv = (uint32_t)(-(int64_t)bv);
      const uint32_t r = (av << shift) + bv;
      if (r == 0) zero |= (Sm_Mask)(1u << l);
      if (r >> 31) sign |= (Sm_Mask)(1u << l);
      d[l] = r;
    }
    if (BIT(w, 47)) {
      set_mask(&t->cc_zero, m, zero);
      set_mask(&t->cc_sign, m, sign);
    }
    return;
  }
  case SM_OP_IMNMX: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool is_signed = BIT(w, 48) != 0;
    const Sm_Mask mins = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    FOR_LANES(m) {
      const bool a_less = is_signed ? (int32_t)a[l] < (int32_t)b[l] : a[l] < b[l];
      d[l] = LANE(mins, l) ? (a_less ? a[l] : b[l]) : (a_less ? b[l] : a[l]);
    }
    return;
  }
  case SM_OP_ISET: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t cond = BITS(w, 49, 3), bop = BITS(w, 45, 2);
    const bool is_signed = BIT(w, 48) != 0;
    const uint32_t yes = BIT(w, 44) ? u32f(1.0f) : 0xffffffffu;
    LANES_ASSIGN(m, d, bool_op(bop, icompare(cond, a[l], b[l], is_signed), LANE(pc, l) != 0) ? yes : 0u);
    return;
  }
  case SM_OP_ISETP: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t cond = BITS(w, 49, 3), bop = BITS(w, 45, 2);
    const bool is_signed = BIT(w, 48) != 0;
    Sm_Mask p0 = 0, p1 = 0;
    FOR_LANES(m) {
      const bool cmp = icompare(cond, a[l], b[l], is_signed);
      const bool c = LANE(pc, l) != 0;
      if (bool_op(bop, cmp, c)) p0 |= (Sm_Mask)(1u << l);
      if (bool_op(bop, !cmp, c)) p1 |= (Sm_Mask)(1u << l);
    }
    set_pred(t, BITS(w, 3, 3), m, p0);
    set_pred(t, BITS(w, 0, 3), m, p1);
    return;
  }
  case SM_OP_ICMP: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t cond = BITS(w, 49, 3);
    const bool is_signed = BIT(w, 48) != 0;
    LANES_ASSIGN(m, d, icompare(cond, c[l], 0, is_signed) ? a[l] : b[l]);
    return;
  }
  case SM_OP_IMUL:
  case SM_OP_IMUL32I: {
    const bool is32i = in->op == SM_OP_IMUL32I;
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool sa = is32i ? BIT(w, 55) != 0 : BIT(w, 41) != 0;
    const bool high = is32i ? BIT(w, 53) != 0 : BIT(w, 39) != 0;
    FOR_LANES(m) {
      const int64_t p = sa ? (int64_t)(int32_t)a[l] * (int64_t)(int32_t)b[l] : (int64_t)((uint64_t)a[l] * (uint64_t)b[l]);
      d[l] = high ? (uint32_t)((uint64_t)p >> 32) : (uint32_t)p;
    }
    return;
  }
  case SM_OP_IMAD: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool is_signed = BIT(w, 53) != 0, nab = BIT(w, 51) != 0, nc = BIT(w, 52) != 0, high = BIT(w, 54) != 0;
    FOR_LANES(m) {
      int64_t p = is_signed ? (int64_t)(int32_t)a[l] * (int64_t)(int32_t)b[l] : (int64_t)((uint64_t)a[l] * (uint64_t)b[l]);
      if (nab) p = -p;
      const uint32_t cv = nc ? (uint32_t)(-(int64_t)c[l]) : c[l];
      d[l] = (high ? (uint32_t)((uint64_t)p >> 32) : (uint32_t)p) + cv;
    }
    return;
  }
  case SM_OP_XMAD: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t *b, *c;
    bool psl, mrg, b_hi;
    uint32_t cmode;
    switch (in->form) {
    case SM_FORM_REG:
      b = t->r[REG_B(w)]; c = t->r[REG_C(w)];
      psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = BIT(w, 35) != 0; cmode = BITS(w, 50, 3);
      break;
    case SM_FORM_CBUF:
      b = splat(cbuf_read(env, in->cbuf, in->imm), tb); c = t->r[REG_C(w)];
      psl = BIT(w, 55) != 0; mrg = BIT(w, 56) != 0; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
      break;
    case SM_FORM_IMM:
      b = splat(in->imm, tb); c = t->r[REG_C(w)];
      psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = false; cmode = BITS(w, 50, 3);
      break;
    default: /* REG_CBUF: b = Rc slot, c = cbuf */
      b = t->r[REG_C(w)]; c = splat(cbuf_read(env, in->cbuf, in->imm), tc);
      psl = false; mrg = false; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
      break;
    }
    const bool a_hi = BIT(w, 53) != 0;
    FOR_LANES(m) {
      const uint32_t a16 = a_hi ? a[l] >> 16 : a[l] & 0xffffu;
      const uint32_t b16 = b_hi ? b[l] >> 16 : b[l] & 0xffffu;
      uint32_t prod = a16 * b16;
      if (psl) prod <<= 16;
      uint32_t cv;
      switch (cmode) {
      case 1: cv = c[l] & 0xffffu; break;
      case 2: cv = c[l] >> 16; break;
      case 4: cv = c[l] + (b[l] << 16); break;
      default: cv = c[l]; break;
      }
      uint32_t r = prod + cv;
      if (mrg) r = (r & 0xffffu) | (b[l] << 16);
      d[l] = r;
    }
    return;
  }
  case SM_OP_LOP: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t ia = BIT(w, 39) ? 0xffffffffu : 0u, ib = BIT(w, 40) ? 0xffffffffu : 0u, op = BITS(w, 41, 2);
    Sm_Mask nonzero = 0, zero = 0, sign = 0;
    FOR_LANES(m) {
      const uint32_t r = lop(op, a[l] ^ ia, b[l] ^ ib);
      if (r) nonzero |= (Sm_Mask)(1u << l);
      else zero |= (Sm_Mask)(1u << l);
      if (r >> 31) sign |= (Sm_Mask)(1u << l);
      d[l] = r;
    }
    set_pred(t, BITS(w, 48, 3), m, nonzero);
    if (BIT(w, 47)) {
      set_mask(&t->cc_zero, m, zero);
      set_mask(&t->cc_sign, m, sign);
    }
    return;
  }
  case SM_OP_LOP32I: {
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t ia = BIT(w, 55) ? 0xffffffffu : 0u, b = BIT(w, 56) ? ~in->imm : in->imm, op = BITS(w, 53, 2);
    Sm_Mask zero = 0, sign = 0;
    FOR_LANES(m) {
      const uint32_t r = lop(op, a[l] ^ ia, b);
      if (r == 0) zero |= (Sm_Mask)(1u << l);
      if (r >> 31) sign |= (Sm_Mask)(1u << l);
      d[l] = r;
    }
    if (BIT(w, 52)) {
      set_mask(&t->cc_zero, m, zero);
      set_mask(&t->cc_sign, m, sign);
    }
    return;
  }
  case SM_OP_LOP3: {
    const uint32_t lut = in->form == SM_FORM_REG ? BITS(w, 28, 8) : BITS(w, 48, 8);
    const uint32_t *a = t->r[REG_A(w)], *c = t->r[REG_C(w)];
    const uint32_t *b = in->form == SM_FORM_IMM ? splat(in->imm, tb) : op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    LANES_ASSIGN(m, d, lop3(lut, a[l], b[l], c[l]));
    return;
  }
  case SM_OP_SHL: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool wrap = BIT(w, 39) != 0;
    FOR_LANES(m) {
      const uint32_t s = wrap ? b[l] & 31u : b[l];
      d[l] = s >= 32u ? 0u : a[l] << s;
    }
    return;
  }
  case SM_OP_SHR: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool wrap = BIT(w, 39) != 0, is_signed = BIT(w, 48) != 0;
    FOR_LANES(m) {
      const uint32_t s = wrap ? b[l] & 31u : b[l];
      if (is_signed) d[l] = s >= 32u ? (uint32_t)((int32_t)a[l] >> 31) : (uint32_t)((int32_t)a[l] >> s);
      else d[l] = s >= 32u ? 0u : a[l] >> s;
    }
    return;
  }
  case SM_OP_BFE: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const bool brev = BIT(w, 40) != 0, is_signed = BIT(w, 48) != 0;
    FOR_LANES(m) {
      const uint32_t av = brev ? bit_reverse(a[l]) : a[l];
      const uint32_t pos = b[l] & 0xffu, len = (b[l] >> 8) & 0xffu;
      uint32_t r;
      if (len == 0) r = 0;
      else if (pos >= 32u) r = is_signed ? (uint32_t)((int32_t)av >> 31) : 0u;
      else {
        const uint32_t n = len > 32u - pos ? 32u - pos : len;
        r = (av >> pos) & (n >= 32u ? 0xffffffffu : ((1u << n) - 1u));
        if (is_signed && n < 32u && (r >> (n - 1u)) & 1u) r |= ~((1u << n) - 1u);
      }
      d[l] = r;
    }
    return;
  }
  case SM_OP_BFI: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    FOR_LANES(m) {
      const uint32_t pos = b[l] & 0xffu, len = (b[l] >> 8) & 0xffu;
      if (pos >= 32u || len == 0) {
        d[l] = c[l];
        continue;
      }
      const uint32_t n = len > 32u - pos ? 32u - pos : len;
      const uint32_t mask = (n >= 32u ? 0xffffffffu : ((1u << n) - 1u)) << pos;
      d[l] = (c[l] & ~mask) | ((a[l] << pos) & mask);
    }
    return;
  }
  case SM_OP_POPC: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t inv = BIT(w, 40) ? 0xffffffffu : 0u;
    LANES_ASSIGN(m, d, popcount(b[l] ^ inv));
    return;
  }
  case SM_OP_FLO: {
    const uint32_t *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t inv = BIT(w, 40) ? 0xffffffffu : 0u;
    const bool is_signed = BIT(w, 48) != 0, shift = BIT(w, 41) != 0;
    FOR_LANES(m) {
      uint32_t v = b[l] ^ inv;
      if (is_signed && (v >> 31)) v = ~v;
      uint32_t r = 0xffffffffu;
      for (int32_t i = 31; i >= 0; i--) {
        if ((v >> i) & 1u) {
          r = (uint32_t)i;
          break;
        }
      }
      if (shift && r != 0xffffffffu) r = 31u - r;
      d[l] = r;
    }
    return;
  }
  case SM_OP_PRMT: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb), *c = op_c(in, env, t, tc);
    uint32_t *d = dst_row(t, REG_D(w));
    FOR_LANES(m) {
      const uint64_t src = (uint64_t)a[l] | ((uint64_t)c[l] << 32);
      uint32_t r = 0;
      for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t s = (b[l] >> (i * 4u)) & 0xfu;
        uint32_t byte = (uint32_t)(src >> ((s & 7u) * 8u)) & 0xffu;
        if (s & 8u) byte = (byte & 0x80u) ? 0xffu : 0u;
        r |= byte << (i * 8u);
      }
      d[l] = r;
    }
    return;
  }
  case SM_OP_SEL: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const Sm_Mask choose = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    LANES_ASSIGN(m, d, LANE(choose, l) ? a[l] : b[l]);
    return;
  }
  case SM_OP_MOV:
  case SM_OP_MOV32I: {
    const uint32_t *b = in->op == SM_OP_MOV32I ? splat(in->imm, tb) : op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    LANES_ASSIGN(m, d, b[l]);
    return;
  }
  case SM_OP_PSETP: {
    const Sm_Mask pa = pred_mask(t, BITS(w, 12, 3), BIT(w, 15));
    const Sm_Mask pb = pred_mask(t, BITS(w, 29, 3), BIT(w, 32));
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t bop1 = BITS(w, 24, 2), bop2 = BITS(w, 45, 2);
    Sm_Mask p0 = 0, p1 = 0;
    FOR_LANES(m) {
      const bool ab = bool_op(bop1, LANE(pa, l) != 0, LANE(pb, l) != 0);
      const bool c = LANE(pc, l) != 0;
      if (bool_op(bop2, ab, c)) p0 |= (Sm_Mask)(1u << l);
      if (bool_op(bop2, !ab, c)) p1 |= (Sm_Mask)(1u << l);
    }
    set_pred(t, BITS(w, 3, 3), m, p0);
    set_pred(t, BITS(w, 0, 3), m, p1);
    return;
  }
  case SM_OP_P2R: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t shift = BITS(w, 41, 2) * 8u;
    FOR_LANES(m) {
      uint32_t bits = 0;
      for (uint32_t i = 0; i < 7u; i++) bits |= LANE(t->p[i], l) << i;
      d[l] = (a[l] & ~(b[l] << shift)) | ((bits & b[l]) << shift);
    }
    return;
  }
  case SM_OP_R2P: {
    const uint32_t *a = t->r[REG_A(w)], *b = op_b(in, env, t, tb);
    const uint32_t shift = BITS(w, 41, 2) * 8u;
    FOR_LANES(m) {
      const uint32_t v = a[l] >> shift;
      for (uint32_t i = 0; i < 7u; i++) {
        if (!(b[l] & (1u << i))) continue;
        if ((v >> i) & 1u) t->p[i] |= (Sm_Mask)(1u << l);
        else t->p[i] &= (Sm_Mask)~(1u << l);
      }
    }
    return;
  }
  case SM_OP_CSETP: {
    const Sm_Mask pc = pred_mask(t, BITS(w, 39, 3), BIT(w, 42));
    const uint32_t bop = BITS(w, 45, 2);
    Sm_Mask cond;
    switch (BITS(w, 8, 5)) {
    case 0x00: cond = 0; break;
    case 0x01: cond = t->cc_sign; break;          /* LT */
    case 0x02: cond = t->cc_zero; break;          /* EQ */
    case 0x05: cond = (Sm_Mask)~t->cc_zero; break; /* NE */
    default: cond = SM_ALL_LANES; break;
    }
    Sm_Mask p0 = 0, p1 = 0;
    FOR_LANES(m) {
      const bool c = LANE(cond, l) != 0, p = LANE(pc, l) != 0;
      if (bool_op(bop, c, p)) p0 |= (Sm_Mask)(1u << l);
      if (bool_op(bop, !c, p)) p1 |= (Sm_Mask)(1u << l);
    }
    set_pred(t, BITS(w, 3, 3), m, p0);
    set_pred(t, BITS(w, 0, 3), m, p1);
    return;
  }

  /* ---- system ---- */
  case SM_OP_S2R:
  case SM_OP_CS2R: {
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t id = BITS(w, 20, 8);
    FOR_LANES(m) d[l] = sysreg(t, id, l);
    return;
  }
  case SM_OP_VOTE: {
    const Sm_Mask v = pred_mask(t, BITS(w, 39, 3), BIT(w, 42)) & m;
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t mode = BITS(w, 48, 2);
    const bool all = (v & m) == m, any = v != 0;
    const bool r = mode == 0u ? all : (mode == 1u ? any : (all || !any));
    FOR_LANES(m) d[l] = v;
    set_pred(t, BITS(w, 45, 3), m, r ? SM_ALL_LANES : 0);
    return;
  }
  case SM_OP_SHFL: {
    /* Lane j's Ra to lane i (PTX shfl): mode IDX / UP / DOWN / BFLY, b the
     * lane operand, c = {segment mask @8, clamp @0}; an out-of-range j
     * reads the lane's own value and clears the predicate. Lanes are this
     * warp's SM_LANES (a quad is lanes 4q..4q+3, as the rasterizer packs them). */
    const uint32_t *a = t->r[REG_A(w)];
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t mode = BITS(w, 30, 2);
    const uint32_t *b_reg = t->r[REG_B(w)], *c_reg = t->r[BITS(w, 39, 8)];
    const bool b_imm = BIT(w, 28) != 0, c_imm = BIT(w, 29) != 0;
    uint32_t res[SM_LANES];
    Sm_Mask valid = 0;
    FOR_ALL_LANES {
      const uint32_t bv = (b_imm ? BITS(w, 20, 5) : b_reg[l]) & 0x1fu;
      const uint32_t cv = c_imm ? BITS(w, 34, 13) : c_reg[l];
      const uint32_t segmask = (cv >> 8) & 0x1fu, clamp = cv & 0x1fu;
      const uint32_t max_lane = (l & segmask) | (clamp & ~segmask), min_lane = l & segmask;
      int32_t j;
      bool ok;
      switch (mode) {
      case 0: j = (int32_t)(min_lane | (bv & ~segmask)); ok = (uint32_t)j <= max_lane; break;
      case 1: j = (int32_t)l - (int32_t)bv; ok = j >= (int32_t)max_lane; break;
      case 2: j = (int32_t)(l + bv); ok = (uint32_t)j <= max_lane; break;
      default: j = (int32_t)(l ^ bv); ok = (uint32_t)j <= max_lane; break;
      }
      if (!ok || j < 0 || j >= (int32_t)SM_LANES) j = (int32_t)l;
      else valid |= (Sm_Mask)(1u << l);
      res[l] = a[j];
    }
    store_masked(d, res, m);
    set_pred(t, BITS(w, 48, 3), m, valid);
    return;
  }

  /* ---- memory ---- */
  case SM_OP_ALD: {
    const uint32_t count = BITS(w, 47, 2) + 1u, d = REG_D(w);
    const uint32_t *idx = t->r[REG_A(w)];
    const bool out = BIT(w, 32) != 0;
    FOR_LANES(m) {
      const uint32_t base = BITS(w, 20, 10) + idx[l];
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t addr = base + i * 4u, word = (addr / 4u) % SM_ATTRIBUTE_WORDS;
        uint32_t v = out ? t->attr_out[word][l] : t->attr_in[word][l];
        if (!out && addr == SM_ATTR_VERTEX_ID) v = t->vertex_id[l];
        if (!out && addr == SM_ATTR_INSTANCE_ID) v = t->instance_id[l];
        if (d != SM_RZ) t->r[(d + i) & 0xffu][l] = v;
      }
    }
    memset(t->r[SM_RZ], 0, sizeof(t->r[SM_RZ]));
    return;
  }
  case SM_OP_AST: {
    const uint32_t count = BITS(w, 47, 2) + 1u, d = REG_D(w);
    const uint32_t *idx = t->r[REG_A(w)];
    FOR_LANES(m) {
      const uint32_t base = BITS(w, 20, 10) + idx[l];
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t word = (base / 4u + i) % SM_ATTRIBUTE_WORDS;
        t->attr_out[word][l] = d == SM_RZ ? 0u : t->r[(d + i) & 0xffu][l];
      }
    }
    return;
  }
  case SM_OP_IPA: {
    uint32_t *d = dst_row(t, REG_D(w));
    const uint32_t *idx = t->r[REG_A(w)], *mul = t->r[REG_B(w)];
    const uint32_t addr0 = BITS(w, 28, 10);
    const bool indexed = BIT(w, 38) != 0, multiply = BITS(w, 54, 2) == 1u, sat = BIT(w, 51) != 0;
    if (!indexed && addr0 != SM_ATTR_FRONT_FACING) {
      /* The common form: one attribute row for every lane. */
      const uint32_t *row = t->attr_in[(addr0 / 4u) % SM_ATTRIBUTE_WORDS];
      uint32_t res[SM_LANES];
      FOR_ALL_LANES {
        float f = f32(row[l]);
        if (multiply) f *= f32(mul[l]);
        if (sat) f = saturate(f);
        res[l] = u32f(f);
      }
      store_masked(d, res, m);
      return;
    }
    FOR_LANES(m) {
      const uint32_t addr = indexed ? addr0 + idx[l] : addr0;
      if (addr == SM_ATTR_FRONT_FACING) {
        d[l] = LANE(t->front_facing, l) ? 0xffffffffu : 0u;
        continue;
      }
      float f = f32(t->attr_in[(addr / 4u) % SM_ATTRIBUTE_WORDS][l]);
      if (multiply) f *= f32(mul[l]);
      if (sat) f = saturate(f);
      d[l] = u32f(f);
    }
    return;
  }
  case SM_OP_LDC: {
    const uint32_t slot = BITS(w, 36, 5), size = BITS(w, 48, 3), n = access_bytes(size);
    const int32_t off = (int32_t)(BITS(w, 20, 16) << 16) >> 16;
    const uint32_t *idx = t->r[REG_A(w)];
    FOR_LANES(m) {
      const uint32_t addr = idx[l] + (uint32_t)off;
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      if (slot < SM_CBUF_SLOTS && env->cbuf[slot] && (uint64_t)addr + n <= env->cbuf_size[slot])
        memcpy(buf, env->cbuf[slot] + addr, n);
      load_to_regs(t, REG_D(w), size, buf, l);
    }
    return;
  }
  case SM_OP_LD:
  case SM_OP_ST:
  case SM_OP_LDG:
  case SM_OP_STG: {
    const bool generic = in->op == SM_OP_LD || in->op == SM_OP_ST;
    const bool load = in->op == SM_OP_LD || in->op == SM_OP_LDG;
    const uint32_t size = generic ? BITS(w, 53, 3) : BITS(w, 48, 3), n = access_bytes(size);
    const bool wide = generic ? BIT(w, 52) != 0 : BIT(w, 45) != 0;
    const int64_t off = generic ? (int64_t)(int32_t)BITS(w, 20, 32) : (int64_t)((int32_t)(BITS(w, 20, 24) << 8) >> 8);
    const uint32_t ra = REG_A(w);
    FOR_LANES(m) {
      uint64_t addr = t->r[ra][l];
      if (wide && ra != SM_RZ) addr |= (uint64_t)t->r[(ra + 1u) & 0xffu][l] << 32;
      addr += (uint64_t)off;
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      if (load) {
        if (env->global_read) (void)env->global_read(env->user, addr, buf, n);
        load_to_regs(t, REG_D(w), size, buf, l);
      } else {
        regs_to_bytes(t, REG_D(w), size, buf, l);
        if (env->global_write) (void)env->global_write(env->user, addr, buf, n);
      }
    }
    return;
  }
  case SM_OP_LDL:
  case SM_OP_STL: {
    const uint32_t size = BITS(w, 48, 3), n = access_bytes(size);
    const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
    const uint32_t *idx = t->r[REG_A(w)];
    const uint32_t top = (uint32_t)(w >> 48) & SM_LDST_SPACE_MASK;
    if (env->shared && (top == SM_LDS_OPCODE || top == SM_STS_OPCODE)) { /* the block's shared memory */
      FOR_LANES(m) {
        const uint32_t addr = idx[l] + (uint32_t)off;
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        if (in->op == SM_OP_LDL) {
          if ((uint64_t)addr + n <= env->shared_bytes) memcpy(buf, env->shared + addr, n);
          load_to_regs(t, REG_D(w), size, buf, l);
        } else {
          regs_to_bytes(t, REG_D(w), size, buf, l);
          if ((uint64_t)addr + n <= env->shared_bytes) memcpy(env->shared + addr, buf, n);
        }
      }
      return;
    }
    FOR_LANES(m) {
      const uint32_t addr = idx[l] + (uint32_t)off;
      uint8_t buf[16];
      memset(buf, 0, sizeof(buf));
      if (in->op == SM_OP_LDL) {
        if ((uint64_t)addr + n <= SM_LOCAL_BYTES) memcpy(buf, t->local[l] + addr, n);
        load_to_regs(t, REG_D(w), size, buf, l);
      } else {
        regs_to_bytes(t, REG_D(w), size, buf, l);
        if ((uint64_t)addr + n <= SM_LOCAL_BYTES) memcpy(t->local[l] + addr, buf, n);
      }
    }
    return;
  }
  case SM_OP_OUT: {
    uint32_t *d = dst_row(t, REG_D(w));
    FOR_LANES(m) d[l] = 0;
    return;
  }

  /* ---- texture ---- */
  case SM_OP_TEXS: exec_texs(in, env, t, m); return;
  case SM_OP_TLDS: exec_tlds(in, env, t, m); return;
  case SM_OP_TLD4S: exec_tld4s(in, env, t, m); return;
  case SM_OP_TEX:
  case SM_OP_TEX_B:
  case SM_OP_TLD:
  case SM_OP_TLD4:
  case SM_OP_TXD:
  case SM_OP_TXQ:
  case SM_OP_TMML:
    exec_tex_vector(in, env, t, m);
    return;

  default:
    /* Unknown or unsupported: skip it (counted at decode). */
    return;
  }
}

/* Splits `w`: lanes in `stay` continue at w->pc + 1 in a new warp (same
 * stacks); the rest keep `w`. Returns false when out of warp slots. */
static bool split(Warp *warps, uint32_t *count, Warp *w, Sm_Mask stay) {
  if (!stay) return true;
  if (*count >= MAX_WARPS) return false;
  Warp *copy = &warps[(*count)++];
  *copy = *w;
  copy->mask = stay;
  copy->pc = w->pc + 1u;
  w->mask &= (Sm_Mask)~stay;
  return true;
}

/* The interpreter's state for one group of lanes: the divergent warps
 * still running and those parked at a barrier. */
typedef struct Run_State {
  Warp warps[MAX_WARPS];
  uint32_t count;
  Warp park[MAX_WARPS];
  uint32_t parked;
  uint32_t steps;
} Run_State;

static Sm_Group_Status run_warps(const Sm_Program *program, const Sm_Env *env, Sm_Thread *t, Run_State *r,
                                 bool barriers) {
  Warp *warps = r->warps;
  uint32_t count = r->count;
  uint32_t steps = r->steps;
  while (count > 0) {
    Warp *w = &warps[count - 1u];
    bool done = false;
    while (!done) {
      if (++steps > SM_MAX_STEPS || w->pc >= program->word_count) {
        t->faulted = true;
        r->count = count;
        return SM_GROUP_FAULT;
      }
      if (w->pc % 4u == 0) {
        w->pc++;
        continue;
      }
      const Sm_Insn *in = &program->insns[w->pc];
      Sm_Mask guard = pred_mask(t, in->pred & 7u, in->pred & 8u) & w->mask;
      if (is_cc_tested_control(in->op)) guard &= cc_test(t, (uint32_t)(in->raw & SM_CC_TEST_MASK));
      switch ((Sm_Op)in->op) {
      case SM_OP_BRA:
        if (!guard) {
          w->pc = in->next;
          break;
        }
        if (in->target < 0) {
          t->faulted = true;
          r->count = count;
          return SM_GROUP_FAULT;
        }
        if (!split(warps, &count, w, (Sm_Mask)(w->mask & ~guard))) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        w->pc = (uint32_t)in->target;
        break;
      case SM_OP_SSY:
      case SM_OP_PBK:
      case SM_OP_PCNT: {
        const uint32_t kind = in->op == SM_OP_SSY ? STACK_SSY : (in->op == SM_OP_PBK ? STACK_PBK : STACK_PCNT);
        if (!flow_push(w, kind, in->target)) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        w->pc = in->next;
        break;
      }
      case SM_OP_SYNC:
      case SM_OP_BRK:
      case SM_OP_CONT: {
        if (!guard) {
          w->pc = in->next;
          break;
        }
        if (!split(warps, &count, w, (Sm_Mask)(w->mask & ~guard))) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        const uint32_t kind = in->op == SM_OP_SYNC ? STACK_SSY : (in->op == SM_OP_BRK ? STACK_PBK : STACK_PCNT);
        const int32_t target = flow_pop_to(w, kind, in->op == SM_OP_CONT);
        if (target < 0) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        w->pc = (uint32_t)target;
        break;
      }
      case SM_OP_CAL:
        if (in->target < 0 || w->call_depth >= SM_STACK_DEPTH) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        w->calls[w->call_depth++] = in->next;
        w->pc = (uint32_t)in->target;
        break;
      case SM_OP_RET:
        if (!guard) {
          w->pc = in->next;
          break;
        }
        if (!split(warps, &count, w, (Sm_Mask)(w->mask & ~guard))) { t->faulted = true; r->count = count; return SM_GROUP_FAULT; }
        if (w->call_depth == 0) {
          w->mask = 0; /* return from the entry point: exit */
        } else {
          w->pc = w->calls[--w->call_depth];
        }
        break;
      case SM_OP_BARRIER:
        /* BAR.SYNC in a compute block: park the group's lanes until every
         * group reached it (sm_group_run); elsewhere a no-op. */
        if (barriers && guard && ((uint32_t)(in->raw >> 48) & SM_LDST_SPACE_MASK) == SM_BAR_OPCODE &&
            r->parked < MAX_WARPS) {
          r->park[r->parked] = *w;
          r->park[r->parked].pc = in->next;
          r->parked++;
          w->mask = 0;
          break;
        }
        w->pc = in->next;
        break;
      case SM_OP_EXIT:
        w->mask &= (Sm_Mask)~guard;
        w->pc = in->next;
        break;
      case SM_OP_KIL:
        t->killed |= guard;
        w->mask &= (Sm_Mask)~guard;
        w->pc = in->next;
        break;
      default:
        g_current_pc = w->pc;
        if (guard) execute(in, env, t, guard);
        w->pc = in->next;
        break;
      }
      if (!w->mask) done = true;
      /* A split appended a warp after `w`: finish the newest first. */
      if (!done && &warps[count - 1u] != w) break;
    }
    if (done) {
      /* Remove `w` (it may not be the last entry). */
      const uint32_t index = (uint32_t)(w - warps);
      warps[index] = warps[count - 1u];
      count--;
    }
  }
  r->count = 0;
  r->steps = steps;
  return r->parked ? SM_GROUP_BARRIER : SM_GROUP_DONE;
}

static void run_begin(Run_State *r, const Sm_Thread *t) {
  r->count = 1;
  r->parked = 0;
  r->steps = 0;
  r->warps[0].mask = (Sm_Mask)(t->lanes >= SM_LANES ? SM_ALL_LANES : ((1u << t->lanes) - 1u));
  r->warps[0].pc = 1;
  r->warps[0].depth = 0;
  r->warps[0].call_depth = 0;
}

bool sm_run(const Sm_Program *program, const Sm_Env *env, Sm_Thread *t) {
  static _Thread_local Run_State state; /* per shading thread (raster3d workers); too big for small stacks */
  run_begin(&state, t);
  return run_warps(program, env, t, &state, false) == SM_GROUP_DONE;
}

_Static_assert(sizeof(Run_State) <= SM_GROUP_STATE_BYTES, "Sm_Group_State too small");

void sm_group_begin(Sm_Group_State *state, const Sm_Thread *t) { run_begin((Run_State *)(void *)state->bytes, t); }

Sm_Group_Status sm_group_run(const Sm_Program *program, const Sm_Env *env, Sm_Thread *t, Sm_Group_State *state) {
  Run_State *r = (Run_State *)(void *)state->bytes;
  /* Resume past the barrier: the parked warps run again. */
  for (uint32_t i = 0; i < r->parked && r->count < MAX_WARPS; i++) r->warps[r->count++] = r->park[i];
  r->parked = 0;
  return run_warps(program, env, t, r, true);
}
