/**
 * Maxwell pixel programs -> WGSL: see wgsl.h. Every instruction's
 * semantics mirror maxwell_shader.c's execute() for one lane; the comments
 * there are the reference for the field positions used here.
 *
 * Structure of the generated fragment stage:
 *
 *   var r<n>: u32 (registers used), p0..p6, condition codes, pc;
 *   loop {
 *     if pc is DONE / KILL / FAULT: leave (KILL and FAULT discard);
 *     switch pc { case <basic block leader>: { ...instructions...; pc = next; } ... }
 *   }
 *   alpha test; outputs from the registers the SPH output map names.
 *
 * One invocation is one lane, so SSY/PBK/PCNT and CAL are plain arrays and
 * no divergence bookkeeping exists. Quad operations read neighbours through
 * dpdxFine/dpdyFine (derivative uniformity is waived: the dispatch loop is
 * never uniform as far as WGSL can tell).
 */
#include "gpu/wgsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gpu/texture.h"

#define BITS(w, at, n) ((uint32_t)(((w) >> (at)) & ((1ull << (n)) - 1ull)))
#define BIT(w, at) ((uint32_t)(((w) >> (at)) & 1ull))
#define REG_D(w) BITS(w, 0, 8)
#define REG_A(w) BITS(w, 8, 8)
#define REG_B(w) BITS(w, 20, 8)
#define REG_C(w) BITS(w, 39, 8)

#define PC_DONE 0xffffffffu
#define PC_KILL 0xfffffffeu
#define PC_FAULT 0xfffffffdu
#define FLOW_DEPTH (SM_STACK_DEPTH * 3u)
#define MAX_BLOCK_STEPS 1048576u
#define STACK_SSY 1u
#define STACK_PBK 2u
#define STACK_PCNT 3u
#define F32_ONE 0x3f800000u
#define EXPR_BYTES 160u
#define LOCAL_WORDS (SM_LOCAL_BYTES / 4u)

/* Half-precision field values (maxwell_shader.c). */
#define HALF_SWZ_F32 1u
#define HALF_SWZ_H1_H0 0u
#define HALF_MERGE_H1_H0 0u

typedef struct Ex {
  char s[EXPR_BYTES];
} Ex;

typedef struct Out {
  char *buf;
  size_t cap;
  size_t len;
  bool overflow;
} Out;

typedef struct Tr {
  const Sm_Program *p;
  const Wgsl_Program_Desc *d;
  Out body;
  bool ok;
  char reason[96];
  uint8_t reg_used[SM_REGISTERS / 8u];
  uint32_t cbuf_slots;
  uint32_t textures_used;
  bool uses_local;
  bool straight;    /* no branches or flow stacks: blocks run in order, no dispatch loop */
  bool straight_live; /* straight: the blocks so far can fall through to the next */
  /* Implicit texture LODs need derivatives in uniform control flow: a
   * straight program before any conditional end, outside predicated
   * instructions. Elsewhere implicit LODs are level 0 (the reference). */
  bool uniform_flow;
  bool guarded;       /* emitting a predicated instruction */
  bool vertex;        /* a vertex program (desc->stage == SM_STAGE_VERTEX) */
  bool compute;       /* a compute program (desc->stage == SM_STAGE_COMPUTE) */
  bool uses_shared;   /* compute: LDS/STS */
  uint32_t cbt;       /* the draw-constant word of this stage's constant-buffer table */
  Wgsl_Globals globals; /* a vertex program's storage buffers (LDG) */
  bool uses_globals;
  bool leader[SM_MAX_WORDS];
  bool reached[SM_MAX_WORDS];
} Tr;

static bool is_texture_op(uint16_t op);
static void emit_ldg(Tr *t, const Sm_Insn *in, uint32_t pc);
static void emit_gmem(Tr *t, const Sm_Insn *in);

static void out_add(Out *o, const char *fmt, ...) {
  if (o->overflow) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= o->cap - o->len) {
    o->overflow = true;
    return;
  }
  o->len += (size_t)n;
}

static Ex ex(const char *fmt, ...) {
  Ex e;
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(e.s, sizeof(e.s), fmt, ap);
  va_end(ap);
  return e;
}

static void fail(Tr *t, const char *fmt, ...) {
  if (!t->ok) return;
  t->ok = false;
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(t->reason, sizeof(t->reason), fmt, ap);
  va_end(ap);
}

#define EMIT(...) out_add(&t->body, __VA_ARGS__)

/* ---- operands ----------------------------------------------------- */

static Ex reg(Tr *t, uint32_t r) {
  r &= 0xffu;
  if (r == SM_RZ) return ex("0u");
  t->reg_used[r / 8u] |= (uint8_t)(1u << (r % 8u));
  return ex("r%u", r);
}

static Ex reg_dst(Tr *t, uint32_t r) {
  r &= 0xffu;
  if (r == SM_RZ) return ex("_d");
  t->reg_used[r / 8u] |= (uint8_t)(1u << (r % 8u));
  return ex("r%u", r);
}

/* A constant-buffer word at a constant offset: its slot's base and size
 * are read once per invocation (cbB<slot>, cbN<slot>). */
static Ex cbuf(Tr *t, uint32_t slot, uint32_t byte_offset) {
  if (slot >= SM_CBUF_SLOTS) return ex("0u");
  t->cbuf_slots |= 1u << slot;
  const uint32_t w = byte_offset / 4u;
  return ex("select(0u, D[cbB%u + %uu], %uu < cbN%u)", slot, w, w, slot);
}

static Ex op_b(Tr *t, const Sm_Insn *in) {
  switch (in->form) {
  case SM_FORM_REG: return reg(t, REG_B(in->raw));
  case SM_FORM_CBUF: return cbuf(t, in->cbuf, in->imm);
  case SM_FORM_REG_CBUF: return reg(t, REG_C(in->raw));
  default: return ex("0x%xu", in->imm);
  }
}

static Ex op_c(Tr *t, const Sm_Insn *in) {
  if (in->form == SM_FORM_REG_CBUF) return cbuf(t, in->cbuf, in->imm);
  return reg(t, REG_C(in->raw));
}

static Ex fl(Ex u) { return ex("F(%s)", u.s); }

static Ex fmod_an(Ex f, uint32_t abs_bit, uint32_t neg_bit) {
  Ex r = f;
  if (abs_bit) r = ex("abs(%s)", r.s);
  if (neg_bit) r = ex("(-%s)", r.s);
  return r;
}

static Ex imm_f(float v) {
  uint32_t u;
  memcpy(&u, &v, sizeof(u));
  return ex("F(0x%xu)", u);
}

/* A predicate read: "true" for PT. */
static Ex pred(uint32_t p, uint32_t negate) {
  if (p >= SM_PT) return ex(negate ? "false" : "true");
  return ex(negate ? "(!p%u)" : "p%u", p);
}

static void set_pred(Tr *t, uint32_t p, const char *value) {
  if (p < SM_PT) EMIT("p%u = %s; ", p, value);
}

/* Condition-code test for control instructions (maxwell_shader.c cc_test). */
static Ex cc_test(uint32_t cond) {
  switch (cond) {
  case 0: return ex("false");
  case 1: return ex("(ccs != cco)");
  case 2: return ex("ccz");
  case 3: return ex("(ccz || ccs != cco)");
  case 4: return ex("(!ccz && ccs == cco)");
  case 5: return ex("(!ccz)");
  case 6: return ex("(ccs == cco)");
  case 7: return ex("(!cco)");
  case 8: return ex("cco");
  case 9: return ex("(ccs != cco || cco)");
  case 10: return ex("(ccz || cco)");
  case 11: return ex("(ccz || ccs != cco || cco)");
  case 12: return ex("((!ccz && ccs == cco) || cco)");
  case 13: return ex("(!ccz)");
  case 14: return ex("(ccs == cco || cco)");
  case 15: return ex("true");
  case 16: return ex("(!cco)");
  case 17: return ex("(!ccc)");
  case 18: return ex("(!ccs)");
  case 19: return ex("(!ccc || ccz)");
  case 20: return ex("(ccc && !ccz)");
  case 21: return ex("ccs");
  case 22: return ex("ccc");
  case 23: return ex("cco");
  default: return ex("false");
  }
}

/* ---- half precision fields (maxwell_shader.c half_fields) -------- */

typedef struct Half_Fields {
  uint32_t swz_a, swz_b, swz_c;
  uint32_t abs_a, neg_a, abs_b, neg_b, neg_c;
  uint32_t sat, merge, precision;
  uint32_t cond, flag;
} Half_Fields;

static Half_Fields half_fields(const Sm_Insn *in) {
  const uint64_t w = in->raw;
  Half_Fields f;
  memset(&f, 0, sizeof(f));
  f.swz_a = BITS(w, 47, 2);
  f.merge = BITS(w, 49, 2);
  f.swz_b = in->form == SM_FORM_CBUF ? HALF_SWZ_F32 : HALF_SWZ_H1_H0;
  f.swz_c = HALF_SWZ_H1_H0;
  const bool is_reg = in->form == SM_FORM_REG, imm32 = in->form == SM_FORM_IMM32;
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
    f.neg_a = (in->op == SM_OP_HMUL2 && is_reg) ? 0u : BIT(w, 43);
    if (in->op == SM_OP_HMUL2) f.precision = BITS(w, 39, 2);
    if (is_reg) {
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
    default:
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
    if (is_reg) {
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

/* ---- prelude ------------------------------------------------------ */

/* Shared helpers; printf formats (each takes the constant-buffer table
 * word, possibly unused). Split to stay within C11 string limits. */
static const char *const k_prelude[] = {
    "diagnostic(off, derivative_uniformity);\n"
    "@group(0) @binding(0) var<storage, read> D: array<u32>;\n"
    "fn F(x: u32) -> f32 { return bitcast<f32>(x); }\n"
    "fn U(x: f32) -> u32 { return bitcast<u32>(x); }\n"
    "fn sat(v: f32) -> f32 { if (!(v > 0.0)) { return 0.0; } return min(v, 1.0); }\n"
    "fn isnan_(v: f32) -> bool { return (U(v) & 0x7fffffffu) > 0x7f800000u; }\n"
    "fn fmin_(a: f32, b: f32) -> f32 { if (isnan_(a)) { return b; } if (isnan_(b)) { return a; } return min(a, b); }\n"
    "fn fmax_(a: f32, b: f32) -> f32 { if (isnan_(a)) { return b; } if (isnan_(b)) { return a; } return max(a, b); }\n"
    "fn fcmp(c: u32, a: f32, b: f32) -> bool {\n"
    "  let u = isnan_(a) || isnan_(b);\n"
    "  switch (c & 15u) {\n"
    "    case 0u: { return false; } case 1u: { return !u && a < b; } case 2u: { return !u && a == b; }\n"
    "    case 3u: { return !u && a <= b; } case 4u: { return !u && a > b; } case 5u: { return !u && a != b; }\n"
    "    case 6u: { return !u && a >= b; } case 7u: { return !u; } case 8u: { return u; }\n"
    "    case 9u: { return u || a < b; } case 10u: { return u || a == b; } case 11u: { return u || a <= b; }\n"
    "    case 12u: { return u || a > b; } case 13u: { return u || a != b; } case 14u: { return u || a >= b; }\n"
    "    default: { return true; }\n"
    "  }\n"
    "}\n"
    "fn icmp(c: u32, a: u32, b: u32, s: bool) -> bool {\n"
    "  var lt = a < b; if (s) { lt = i32(a) < i32(b); }\n"
    "  let eq = a == b;\n"
    "  switch (c & 7u) {\n"
    "    case 0u: { return false; } case 1u: { return lt; } case 2u: { return eq; } case 3u: { return lt || eq; }\n"
    "    case 4u: { return !lt && !eq; } case 5u: { return !eq; } case 6u: { return !lt; } default: { return true; }\n"
    "  }\n"
    "}\n"
    "fn bop(o: u32, a: bool, b: bool) -> bool {\n"
    "  switch (o & 3u) { case 0u: { return a && b; } case 1u: { return a || b; } case 2u: { return a != b; }\n"
    "    default: { return a; } }\n"
    "}\n"
    "fn rnd(v: f32, m: u32) -> f32 {\n"
    "  switch (m & 3u) { case 0u: { return round(v); } case 1u: { return floor(v); } case 2u: { return ceil(v); }\n"
    "    default: { return trunc(v); } }\n"
    "}\n"
    /* maxwell_shader.c float_to_half, bit for bit. */
    "fn f2h(f: f32) -> u32 {\n"
    "  let v = U(f); let sign = (v >> 16u) & 0x8000u; let e8 = (v >> 23u) & 0xffu;\n"
    "  let e = i32(e8) - 112; var m = v & 0x7fffffu;\n"
    "  if (e8 == 0xffu) { return sign | 0x7c00u | select(0u, 0x200u, m != 0u); }\n"
    "  if (e >= 31) { return sign | 0x7c00u; }\n"
    "  if (e <= 0) {\n"
    "    if (e < -10) { return sign; }\n"
    "    m = m | 0x800000u; let sh = u32(14 - e); var h = m >> sh;\n"
    "    if (((m >> (sh - 1u)) & 1u) != 0u) { h = h + 1u; }\n"
    "    return sign | h;\n"
    "  }\n"
    "  var h = sign | (u32(e) << 10u) | (m >> 13u);\n"
    "  if ((m & 0x1000u) != 0u) { h = h + 1u; }\n"
    "  return h & 0xffffu;\n"
    "}\n"
    "fn h2f(h: u32) -> f32 { return unpack2x16float(h & 0xffffu).x; }\n",
    "fn hx(v: u32, swz: u32, ab: bool, ng: bool) -> vec2<f32> {\n"
    "  let h0 = h2f(v); let h1 = h2f(v >> 16u); var p = vec2<f32>(h0, h1);\n"
    "  if (swz == 1u) { p = vec2<f32>(F(v), F(v)); } else if (swz == 2u) { p = vec2<f32>(h0, h0); }\n"
    "  else if (swz == 3u) { p = vec2<f32>(h1, h1); }\n"
    "  if (ab) { p = abs(p); } if (ng) { p = -p; }\n"
    "  return p;\n"
    "}\n"
    "fn hmul(a: f32, b: f32, prec: u32) -> f32 { if (prec == 2u && (a == 0.0 || b == 0.0)) { return 0.0; } return a * b; }\n"
    "fn hmerge(old: u32, r: vec2<f32>, m: u32) -> u32 {\n"
    "  switch (m) { case 1u: { return U(r.x); } case 2u: { return (old & 0xffff0000u) | f2h(r.x); }\n"
    "    case 3u: { return (old & 0xffffu) | (f2h(r.y) << 16u); } default: { return f2h(r.x) | (f2h(r.y) << 16u); } }\n"
    "}\n"
    "fn f2i(v: f32, s: bool, size: u32) -> u32 {\n"
    "  if (isnan_(v)) { return 0u; }\n"
    "  let bits = 8u << min(size, 2u);\n"
    "  if (s) {\n"
    "    if (bits >= 32u) { if (v >= 2147483648.0) { return 0x7fffffffu; } if (v <= -2147483648.0) { return 0x80000000u; }\n"
    "      return u32(i32(v)); }\n"
    "    let hi = f32((1u << (bits - 1u)) - 1u); let lo = -f32(1u << (bits - 1u));\n"
    "    return u32(i32(clamp(v, lo, hi)));\n"
    "  }\n"
    "  if (bits >= 32u) { if (v >= 4294967296.0) { return 0xffffffffu; } if (v <= 0.0) { return 0u; } return u32(v); }\n"
    "  return u32(clamp(v, 0.0, f32((1u << bits) - 1u)));\n"
    "}\n"
    "fn isrc(v: u32, size: u32, sel: u32, s: bool) -> u32 {\n"
    "  if (size == 0u) { let b = (v >> (sel * 8u)) & 0xffu; if (s) { return u32(i32(b << 24u) >> 24u); } return b; }\n"
    "  if (size == 1u) { let h = (v >> ((sel >> 1u) * 16u)) & 0xffffu; if (s) { return u32(i32(h << 16u) >> 16u); }\n"
    "    return h; }\n"
    "  return v;\n"
    "}\n"
    "fn i2f(v: u32, size: u32, sel: u32, s: bool, ab: bool, ng: bool) -> f32 {\n"
    "  let x = isrc(v, size, sel, s); var f = f32(x);\n"
    "  if (s) { f = f32(i32(x)); if (ab) { f = abs(f); } }\n"
    "  if (ng) { f = -f; }\n"
    "  return f;\n"
    "}\n"
    /* I2I in sign and magnitude (the interpreter works in 64 bits, so
     * |INT_MIN| and a negated large unsigned value do not wrap). */
    "fn i2i(v: u32, ssize: u32, sel: u32, ss: bool, ab: bool, ng: bool, dsize: u32, ds: bool, st: bool) -> u32 {\n"
    "  let x = isrc(v, ssize, sel, ss);\n"
    "  var neg = ss && i32(x) < 0; var mag = select(x, 0u - x, neg);\n"
    "  if (ab) { neg = false; }\n"
    "  if (ng && mag != 0u) { neg = !neg; }\n"
    "  let bits = 8u << min(dsize, 2u);\n"
    "  if (st) {\n"
    "    let half = 1u << (bits - 1u);\n"
    "    if (ds) { if (neg) { mag = min(mag, half); } else { mag = min(mag, half - 1u); } }\n"
    "    else { if (neg) { mag = 0u; neg = false; } else if (bits < 32u) { mag = min(mag, (1u << bits) - 1u); } }\n"
    "  }\n"
    "  let r = select(mag, 0u - mag, neg);\n"
    "  if (bits >= 32u) { return r; }\n"
    "  let mask = (1u << bits) - 1u; var o = r & mask;\n"
    "  if (ds && (o & (1u << (bits - 1u))) != 0u) { o = o | ~mask; }\n"
    "  return o;\n"
    "}\n"
    "fn iaddsat(a: u32, b: u32) -> u32 {\n"
    "  let s = i32(a) + i32(b);\n"
    "  if ((i32(a) >= 0) == (i32(b) >= 0) && (s >= 0) != (i32(a) >= 0)) { return select(0x80000000u, 0x7fffffffu, i32(a) >= 0); }\n"
    "  return u32(s);\n"
    "}\n",
    /* 32 x 32 -> 64 multiply: (low, high). */
    "fn mul64(a: u32, b: u32, s: bool) -> vec2<u32> {\n"
    "  let al = a & 0xffffu; let ah = a >> 16u; let bl = b & 0xffffu; let bh = b >> 16u;\n"
    "  let ll = al * bl; let lh = al * bh; let hl = ah * bl; let hh = ah * bh;\n"
    "  let mid = (ll >> 16u) + (lh & 0xffffu) + (hl & 0xffffu);\n"
    "  var hi = hh + (lh >> 16u) + (hl >> 16u) + (mid >> 16u);\n"
    "  let lo = a * b;\n"
    "  if (s) { if (i32(a) < 0) { hi = hi - b; } if (i32(b) < 0) { hi = hi - a; } }\n"
    "  return vec2<u32>(lo, hi);\n"
    "}\n"
    "fn neg64(p: vec2<u32>) -> vec2<u32> { return vec2<u32>(0u - p.x, ~p.y + select(0u, 1u, p.x == 0u)); }\n"
    "fn lop(o: u32, a: u32, b: u32) -> u32 {\n"
    "  switch (o & 3u) { case 0u: { return a & b; } case 1u: { return a | b; } case 2u: { return a ^ b; }\n"
    "    default: { return b; } }\n"
    "}\n"
    "fn lop3(lut: u32, a: u32, b: u32, c: u32) -> u32 {\n"
    "  var r = 0u;\n"
    "  for (var i = 0u; i < 8u; i = i + 1u) {\n"
    "    if ((lut & (1u << i)) == 0u) { continue; }\n"
    "    r = r | (select(~a, a, (i & 4u) != 0u) & select(~b, b, (i & 2u) != 0u) & select(~c, c, (i & 1u) != 0u));\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "fn shl_(a: u32, s: u32) -> u32 { if (s >= 32u) { return 0u; } return a << s; }\n"
    "fn shr_(a: u32, s: u32, sg: bool) -> u32 {\n"
    "  if (sg) { return u32(i32(a) >> min(s, 31u)); }\n"
    "  if (s >= 32u) { return 0u; } return a >> s;\n"
    "}\n"
    "fn bfe_(a: u32, b: u32, sg: bool, brev: bool) -> u32 {\n"
    "  var av = a; if (brev) { av = reverseBits(a); }\n"
    "  let pos = b & 0xffu; let len = (b >> 8u) & 0xffu;\n"
    "  if (len == 0u) { return 0u; }\n"
    "  if (pos >= 32u) { if (sg) { return u32(i32(av) >> 31u); } return 0u; }\n"
    "  let n = min(len, 32u - pos);\n"
    "  var mask = 0xffffffffu; if (n < 32u) { mask = (1u << n) - 1u; }\n"
    "  var r = (av >> pos) & mask;\n"
    "  if (sg && n < 32u && ((r >> (n - 1u)) & 1u) != 0u) { r = r | ~mask; }\n"
    "  return r;\n"
    "}\n"
    "fn bfi_(a: u32, b: u32, c: u32) -> u32 {\n"
    "  let pos = b & 0xffu; let len = (b >> 8u) & 0xffu;\n"
    "  if (pos >= 32u || len == 0u) { return c; }\n"
    "  let n = min(len, 32u - pos);\n"
    "  var m = 0xffffffffu; if (n < 32u) { m = (1u << n) - 1u; }\n"
    "  m = m << pos;\n"
    "  return (c & ~m) | ((a << pos) & m);\n"
    "}\n"
    "fn flo_(b: u32, sg: bool, sh: bool) -> u32 {\n"
    "  var v = b; if (sg && (v >> 31u) != 0u) { v = ~v; }\n"
    "  var r = firstLeadingBit(v); if (v == 0u) { r = 0xffffffffu; }\n"
    "  if (sh && r != 0xffffffffu) { r = 31u - r; }\n"
    "  return r;\n"
    "}\n"
    "fn prmt_(a: u32, b: u32, c: u32) -> u32 {\n"
    "  var r = 0u;\n"
    "  for (var i = 0u; i < 4u; i = i + 1u) {\n"
    "    let s = (b >> (i * 4u)) & 15u; let k = s & 7u;\n"
    "    var byte = select((c >> ((k - 4u) * 8u)) & 0xffu, (a >> (k * 8u)) & 0xffu, k < 4u);\n"
    "    if ((s & 8u) != 0u) { byte = select(0u, 0xffu, (byte & 0x80u) != 0u); }\n"
    "    r = r | (byte << (i * 8u));\n"
    "  }\n"
    "  return r;\n"
    "}\n",
    "fn mufu(f: u32, x: f32) -> f32 {\n"
    "  switch (f) { case 0u: { return cos(x); } case 1u: { return sin(x); } case 2u: { return exp2(x); }\n"
    "    case 3u: { return log2(x); } case 4u: { return 1.0 / x; } case 5u: { return inverseSqrt(x); }\n"
    "    case 8u: { return sqrt(x); } default: { return x; } }\n"
    "}\n"
    "fn swz_ka(s: u32) -> f32 { return select(select(1.0, 0.0, s == 3u), -1.0, s == 1u); }\n"
    "fn swz_kb(s: u32) -> f32 { return select(1.0, -1.0, s == 2u); }\n"
    "fn cb(s: u32, i: u32) -> u32 {\n"
    "  let base = D[" "%u" "u + 2u * s]; let size = D[" "%u" "u + 2u * s + 1u];\n"
    "  if (i < size) { return D[base + i]; } return 0u;\n"
    "}\n"
    /* Bytes of a constant buffer (LDC): `n` bytes at byte address `a`. */
    "fn cbyte(s: u32, a: u32, n: u32) -> u32 {\n"
    "  let size = D[" "%u" "u + 2u * s + 1u] * 4u;\n"
    "  if (a + n > size || a + n < a) { return 0u; }\n"
    "  let w = cb(s, a >> 2u); let sh = (a & 3u) * 8u;\n"
    "  if (n >= 4u) { if (sh == 0u) { return w; } return (w >> sh) | (cb(s, (a >> 2u) + 1u) << (32u - sh)); }\n"
    "  return (w >> sh) & ((1u << (n * 8u)) - 1u);\n"
    "}\n"
    "fn wrapi(i: i32, size: i32, mode: u32) -> i32 {\n"
    "  switch (mode) {\n"
    "    case 0u: { let m = i %% size; return select(m, m + size, m < 0); }\n"
    "    case 1u: { let p = size * 2; var m = i %% p; if (m < 0) { m = m + p; } return select(p - 1 - m, m, m < size); }\n"
    "    case 3u: { return select(i, -1, i < 0 || i >= size); }\n"
    "    case 6u: { let m = select(i, -1 - i, i < 0); return select(m, -1, m >= size); }\n"
    "    case 5u, 7u: { let m = select(i, -1 - i, i < 0); return select(m, size - 1, m >= size); }\n"
    "    default: { return clamp(i, 0, size - 1); }\n"
    "  }\n"
    "}\n"
    "fn tcmp(f: u32, r: f32, v: f32) -> bool {\n"
    "  switch (f & 7u) { case 0u: { return false; } case 1u: { return r < v; } case 2u: { return r == v; }\n"
    "    case 3u: { return r <= v; } case 4u: { return r > v; } case 5u: { return r != v; } case 6u: { return r >= v; }\n"
    "    default: { return true; } }\n"
    "}\n"
    "fn swz1(t: vec4<u32>, s: u32) -> u32 {\n"
    "  switch (s) { case 2u: { return t.x; } case 3u: { return t.y; } case 4u: { return t.z; } case 5u: { return t.w; }\n"
    "    case 6u: { return 1u; } case 7u: { return 0x3f800000u; } default: { return 0u; } }\n"
    "}\n"
    "fn swz(t: vec4<u32>, s: u32) -> vec4<u32> {\n"
    "  if (s == 0x5432u) { return t; }\n"
    "  return vec4<u32>(swz1(t, s & 15u), swz1(t, (s >> 4u) & 15u), swz1(t, (s >> 8u) & 15u), swz1(t, (s >> 12u) & 15u));\n"
    "}\n"
    /* maxwell texture.c cube_face: (s, t in [0,1], face). */
    "fn cubeface(c: vec3<f32>) -> vec3<f32> {\n"
    "  let a = abs(c); var sc = 0.0; var tc = 0.0; var ma = 0.0; var face = 0.0;\n"
    "  if (a.x >= a.y && a.x >= a.z) { face = select(1.0, 0.0, c.x >= 0.0); ma = a.x; sc = select(c.z, -c.z, c.x >= 0.0);\n"
    "    tc = -c.y; }\n"
    "  else if (a.y >= a.z) { face = select(3.0, 2.0, c.y >= 0.0); ma = a.y; sc = c.x; tc = select(-c.z, c.z, c.y >= 0.0); }\n"
    "  else { face = select(5.0, 4.0, c.z >= 0.0); ma = a.z; sc = select(-c.x, c.x, c.z >= 0.0); tc = -c.y; }\n"
    "  if (ma == 0.0) { ma = 1.0; }\n"
    "  return vec3<f32>(0.5 * (sc / ma + 1.0), 0.5 * (tc / ma + 1.0), face);\n"
    "}\n",
};

/* Per-texture helpers (texture.c's texel_at / tex_sample / tex_gather /
 * tex_fetch); %1$u = index, %2$s = the WGSL sample type, %3$s = the texel
 * -> u32 conversion, %4$s = whether bilinear filtering applies. */
static void emit_texture_helpers(Out *o, uint32_t i, uint8_t sample_type, bool hw) {
  const char *type = sample_type == WGSL_SAMPLE_UINT ? "u32" : (sample_type == WGSL_SAMPLE_SINT ? "i32" : "f32");
  const char *conv = sample_type == WGSL_SAMPLE_UINT ? "v" : "bitcast<vec4<u32>>(v)";
  const bool filterable = sample_type == WGSL_SAMPLE_FLOAT;
  const uint32_t pw = WGSL_DRAW_TEXTURE_PARAMS + WGSL_TEX_PARAM_WORDS * i;
  out_add(o, "@group(0) @binding(%u) var T%u: texture_2d_array<%s>;\n", WGSL_TEXTURE_BINDING_BASE + i, i, type);
  if (hw) out_add(o, "@group(0) @binding(%u) var S%u: sampler;\n", WGSL_SAMPLER_BINDING_BASE + i, i);
  out_add(o,
          "fn t%u_texel(x: i32, y: i32, l: u32, lv: u32) -> vec4<u32> {\n"
          "  let dm = vec2<i32>(textureDimensions(T%u, lv)); let wr = D[%uu];\n"
          "  let wx = wrapi(x, dm.x, wr & 15u); let wy = wrapi(y, dm.y, (wr >> 4u) & 15u);\n"
          "  if (wx < 0 || wy < 0) { return vec4<u32>(D[%uu], D[%uu], D[%uu], D[%uu]); }\n"
          "  let v = textureLoad(T%u, vec2<i32>(wx, wy), min(l, textureNumLayers(T%u) - 1u), lv);\n"
          "  return %s;\n"
          "}\n",
          i, i, pw + WGSL_TEXP_WRAP, pw + WGSL_TEXP_BORDER, pw + WGSL_TEXP_BORDER + 1u, pw + WGSL_TEXP_BORDER + 2u,
          pw + WGSL_TEXP_BORDER + 3u, i, i, conv);
  /* Coordinates -> texel space (resolve_coords): (u, v, layer). */
  out_add(o,
          "fn t%u_coords(c: vec3<f32>, layer: f32) -> vec3<f32> {\n"
          "  let fl = D[%uu]; let dm = vec2<f32>(textureDimensions(T%u));\n"
          "  var s = c.x; var t = select(0.0, c.y, dm.y > 1.0);\n"
          "  var l = select(floor(layer + 0.5), 0.0, layer < 0.0);\n"
          "  if ((fl & %uu) != 0u) { let cf = cubeface(c); s = cf.x; t = cf.y; l = l * 6.0 + cf.z; }\n"
          "  if ((fl & %uu) != 0u) { s = s * dm.x; t = t * dm.y; }\n"
          "  return vec3<f32>(s, t, l);\n"
          "}\n",
          i, pw + WGSL_TEXP_FLAGS, i, WGSL_TEXP_CUBE, WGSL_TEXP_SCALE | WGSL_TEXP_CUBE);
  char hw_tap[256] = "";
  if (hw)
    snprintf(hw_tap, sizeof(hw_tap),
             "  if (!shadow) { let uv = (q0.xy + vec2<f32>(off)) / vec2<f32>(textureDimensions(T%u));\n"
             "    return swz(bitcast<vec4<u32>>(textureSampleLevel(T%u, S%u, uv, l, lf)), sw); }\n",
             i, i, i);
  /* Level selection: `lod` is the instruction's (explicit, or implicit
   * from derivatives plus biases); the sampler clamps it, the texture's
   * levels bound it. One level (the reference): level 0, the magnification
   * filter, as before. Shader-side filtering picks the nearest level. */
  out_add(o,
          "fn t%u_lod(c: vec3<f32>, layer: f32) -> f32 {\n"
          "  let q = t%u_coords(c, layer).xy; let dx = dpdxFine(q); let dy = dpdyFine(q);\n"
          "  let r = max(dot(dx, dx), dot(dy, dy)); if (!(r > 0.0)) { return -32.0; }\n"
          "  return 0.5 * log2(r) + F(D[%uu]);\n"
          "}\n",
          i, i, pw + WGSL_TEXP_LOD_BIAS);
  out_add(o,
          "fn t%u_sample2(c: vec3<f32>, layer: f32, dref: f32, shadow: bool, off: vec2<i32>, lod: f32) -> vec4<u32> {\n"
          "  let fl = D[%uu]; let sw = D[%uu]; let cf = D[%uu];\n"
          "  let q0 = t%u_coords(c, layer); let l = u32(q0.z);\n"
          "  let nl = textureNumLevels(T%u); var lf = 0.0; var lv = 0u; var lin = (fl & %uu) != 0u;\n"
          "  if (nl > 1u) { let lc = clamp(lod, F(D[%uu]), F(D[%uu]));\n"
          "    lf = clamp(lc, 0.0, f32(nl - 1u)); lv = u32(floor(lf + 0.5)); if (lc > 0.0) { lin = (fl & %uu) != 0u; } }\n"
          "  var q = q0; if (lv > 0u) { q = vec3<f32>(q0.xy * vec2<f32>(textureDimensions(T%u, lv)) /\n"
          "    vec2<f32>(textureDimensions(T%u)), q0.z); }\n"
          "  let cmp = shadow && (fl & %uu) != 0u;\n"
          "%s"
          "  if (!%s || !lin) {\n"
          "    var tx = t%u_texel(i32(floor(q.x)) + off.x, i32(floor(q.y)) + off.y, l, lv);\n"
          "    if (cmp) { let r = select(0u, 0x3f800000u, tcmp(cf, dref, F(tx.x))); tx = vec4<u32>(r, r, r, tx.w); }\n"
          "    return swz(tx, sw);\n"
          "  }\n"
          "  let x = q.x - 0.5; let y = q.y - 0.5; let fx = floor(x); let fy = floor(y);\n"
          "  let ax = x - fx; let ay = y - fy; let x0 = i32(fx) + off.x; let y0 = i32(fy) + off.y;\n"
          "  let t00 = t%u_texel(x0, y0, l, lv); let t10 = t%u_texel(x0 + 1, y0, l, lv);\n"
          "  var t01 = t%u_texel(x0, y0 + 1, l, lv); var t11 = t%u_texel(x0 + 1, y0 + 1, l, lv);\n"
          "  if (textureDimensions(T%u, lv).y <= 1u) { t01 = t00; t11 = t10; }\n"
          "  var a = bitcast<vec4<f32>>(t00); var b = bitcast<vec4<f32>>(t10);\n"
          "  var cc = bitcast<vec4<f32>>(t01); var d = bitcast<vec4<f32>>(t11);\n"
          "  if (cmp) {\n"
          "    a = vec4<f32>(select(0.0, 1.0, tcmp(cf, dref, a.x))); b = vec4<f32>(select(0.0, 1.0, tcmp(cf, dref, b.x)));\n"
          "    cc = vec4<f32>(select(0.0, 1.0, tcmp(cf, dref, cc.x))); d = vec4<f32>(select(0.0, 1.0, tcmp(cf, dref, d.x)));\n"
          "  }\n"
          "  let top = a + (b - a) * ax; let bottom = cc + (d - cc) * ax;\n"
          "  return swz(bitcast<vec4<u32>>(top + (bottom - top) * ay), sw);\n"
          "}\n",
          i, pw + WGSL_TEXP_FLAGS, pw + WGSL_TEXP_SWIZZLE, pw + WGSL_TEXP_COMPARE, i, i, WGSL_TEXP_LINEAR,
          pw + WGSL_TEXP_MIN_LOD, pw + WGSL_TEXP_MAX_LOD, WGSL_TEXP_MIN_LINEAR, i, i, WGSL_TEXP_DEPTH_COMPARE, hw_tap,
          filterable ? "true" : "false", i, i, i, i, i, i);
  /* 3D textures arrive as layers (their slices): blend the two around r
   * (texture.c tex_sample's arithmetic), or take the nearest. */
  out_add(o,
          "fn t%u_sample(c: vec3<f32>, layer: f32, dref: f32, shadow: bool, off: vec2<i32>, lod: f32) -> vec4<u32> {\n"
          "  let fl = D[%uu];\n"
          "  if ((fl & %uu) == 0u) { return t%u_sample2(c, layer, dref, shadow, off, lod); }\n"
          "  let n = i32(textureNumLayers(T%u)); let wm = (D[%uu] >> 8u) & 15u;\n"
          "  let w = select(c.z, c.z * f32(n), (fl & %uu) != 0u);\n"
          "  if ((fl & %uu) == 0u) {\n"
          "    let z = max(wrapi(i32(clamp(floor(w), -1073741824.0, 1073741824.0)), n, wm), 0);\n"
          "    return t%u_sample2(vec3<f32>(c.xy, 0.0), f32(z), dref, shadow, off, lod);\n"
          "  }\n"
          "  let z = w - 0.5; let z0f = floor(z); let az = z - z0f;\n"
          "  let zi = i32(clamp(z0f, -1073741824.0, 1073741824.0));\n"
          "  var z0 = wrapi(zi, n, wm); var z1 = wrapi(zi + 1, n, wm);\n"
          "  if (z0 < 0) { z0 = 0; } if (z1 < 0) { z1 = n - 1; }\n"
          "  let a = bitcast<vec4<f32>>(t%u_sample2(vec3<f32>(c.xy, 0.0), f32(z0), dref, shadow, off, lod));\n"
          "  let b = bitcast<vec4<f32>>(t%u_sample2(vec3<f32>(c.xy, 0.0), f32(z1), dref, shadow, off, lod));\n"
          "  return bitcast<vec4<u32>>(a + (b - a) * az);\n"
          "}\n",
          i, pw + WGSL_TEXP_FLAGS, WGSL_TEXP_3D, i, i, pw + WGSL_TEXP_WRAP, WGSL_TEXP_SCALE, WGSL_TEXP_LINEAR, i, i, i);
  out_add(o,
          "fn t%u_gather(c: vec3<f32>, layer: f32, comp: u32, dref: f32, shadow: bool, off: vec2<i32>) -> vec4<u32> {\n"
          "  let fl = D[%uu]; let sw = D[%uu]; let cf = D[%uu];\n"
          "  let q = t%u_coords(c, layer); let l = u32(q.z);\n"
          "  let x0 = i32(floor(q.x - 0.5)) + off.x; let y0 = i32(floor(q.y - 0.5)) + off.y;\n"
          "  let xs = vec4<i32>(x0, x0 + 1, x0 + 1, x0); let ys = vec4<i32>(y0 + 1, y0 + 1, y0, y0);\n"
          "  var o = vec4<u32>(0u);\n"
          "  for (var k = 0u; k < 4u; k = k + 1u) {\n"
          "    let tx = t%u_texel(xs[k], ys[k], l, 0u);\n"
          "    o[k] = swz(tx, sw)[comp & 3u];\n"
          "    if (shadow && (fl & %uu) != 0u) { o[k] = select(0u, 0x3f800000u, tcmp(cf, dref, F(tx.x))); }\n"
          "  }\n"
          "  return o;\n"
          "}\n",
          i, pw + WGSL_TEXP_FLAGS, pw + WGSL_TEXP_SWIZZLE, pw + WGSL_TEXP_COMPARE, i, i, WGSL_TEXP_DEPTH_COMPARE);
  out_add(o,
          "fn t%u_fetch(x: i32, y: i32, l: i32) -> vec4<u32> {\n"
          "  let dm = vec2<i32>(textureDimensions(T%u));\n"
          "  if (x < 0 || y < 0 || x >= dm.x || y >= dm.y) { return vec4<u32>(0u); }\n"
          "  let v = textureLoad(T%u, vec2<i32>(x, y), min(u32(max(l, 0)), textureNumLayers(T%u) - 1u), 0);\n"
          "  return swz(%s, D[%uu]);\n"
          "}\n",
          i, i, i, i, conv, pw + WGSL_TEXP_SWIZZLE);
  out_add(o,
          "fn t%u_dims() -> vec4<u32> {\n"
          "  let dm = textureDimensions(T%u); return vec4<u32>(dm.x, dm.y, textureNumLayers(T%u), D[%uu]);\n"
          "}\n",
          i, i, i, pw + WGSL_TEXP_LEVELS);
}

/* ---- textures ----------------------------------------------------- */

/* The texture instruction at `pc`'s binding, or WGSL_NO_BINDING. */
static uint32_t texture_binding(Tr *t, uint32_t pc) {
  const uint32_t b = t->d->binding_of[pc];
  if (b == WGSL_NO_BINDING || b >= t->d->texture_count || b >= WGSL_MAX_TEXTURES) return WGSL_NO_BINDING;
  t->textures_used |= 1u << b;
  return b;
}

/* tex_args: the packed TEXS/TLDS arguments, as expressions. */
static void tex_args(Tr *t, uint64_t w, uint32_t n, Ex args[8]) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  uint32_t in_a, in_b;
  if (n <= 1u) {
    in_a = n;
    in_b = 0;
  } else if (n == 2u) {
    in_a = 1;
    in_b = 1;
  } else {
    in_a = 2;
    in_b = n - 2u;
  }
  for (uint32_t i = 0; i < 8u; i++) args[i] = ex("0u");
  for (uint32_t i = 0; i < in_a; i++) args[i] = a == SM_RZ ? ex("0u") : reg(t, a + i);
  for (uint32_t i = 0; i < in_b; i++) args[in_a + i] = b == SM_RZ ? ex("0u") : reg(t, b + i);
}

static void tex_args_vec(Tr *t, uint64_t w, uint32_t n, uint32_t in_a, Ex args[8], uint32_t b_skip) {
  const uint32_t a = REG_A(w), b = REG_B(w);
  for (uint32_t i = 0; i < 8u; i++) args[i] = ex("0u");
  for (uint32_t i = 0; i < n && i < 8u; i++) {
    const uint32_t base = i < in_a ? a : b;
    const uint32_t k = i < in_a ? i : i - in_a + b_skip;
    args[i] = base == SM_RZ ? ex("0u") : reg(t, base + k);
  }
}

static uint32_t scalar_components(uint32_t code, bool dual, uint32_t out[4]) {
  static const uint8_t single[8][2] = {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {2, 0}, {2, 0}, {2, 1}, {2, 2}};
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

/* write_scalar_results from the vec4<u32> `tx` (`half`: packed halves,
 * write_scalar_results_half). */
static void write_scalar(Tr *t, uint64_t w, const uint32_t *comps, uint32_t count, bool half) {
  const uint32_t d0 = REG_D(w), d1 = BITS(w, 28, 8);
  if (half) {
    for (uint32_t i = 0; i < count; i += 2u) {
      const uint32_t base = i < 2u ? d0 : d1;
      if (base == SM_RZ) continue;
      if (i + 1u < count)
        EMIT("%s = f2h(F(tx[%u])) | (f2h(F(tx[%u])) << 16u); ", reg_dst(t, base).s, comps[i], comps[i + 1u]);
      else
        EMIT("%s = f2h(F(tx[%u])); ", reg_dst(t, base).s, comps[i]);
    }
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t base = i < 2u ? d0 : d1;
    if (base == SM_RZ) continue;
    const uint32_t r = (base + (i & 1u)) & 0xffu;
    if (r == SM_RZ) continue;
    EMIT("%s = tx[%u]; ", reg_dst(t, r).s, comps[i]);
  }
}

static const char k_unbound[] = "vec4<u32>(0u, 0u, 0u, 0x3f800000u)";

/* A sample's level of detail: LZ (level 0), LL (explicit), automatic or
 * LB (automatic + bias) - automatic only where derivatives are defined
 * (Tr.uniform_flow), else level 0 as the reference. */
typedef enum Lod_Mode { LOD_ZERO, LOD_EXPLICIT, LOD_AUTO, LOD_BIAS } Lod_Mode;

static Ex lod_expr(Tr *t, uint32_t b, Lod_Mode mode, Ex value, Ex c, Ex layer) {
  if (mode == LOD_EXPLICIT) return ex("F(%s)", value.s);
  if (t->vertex) return mode == LOD_BIAS ? ex("F(%s)", value.s) : ex("0.0"); /* no derivatives: level 0 */
  if (mode == LOD_ZERO || !t->uniform_flow || t->guarded) return ex("0.0");
  if (mode == LOD_BIAS) return ex("(t%u_lod(%s, %s) + F(%s))", b, c.s, layer.s, value.s);
  return ex("t%u_lod(%s, %s)", b, c.s, layer.s);
}

static void emit_texs(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4), b = texture_binding(t, pc);
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  Ex a[8];
  Ex x = ex("0.0"), y = ex("0.0"), z = ex("0.0"), layer = ex("0.0"), dref = ex("0.0"), lod = ex("0u");
  bool shadow = false;
  /* TEXS targets: 0 1D LZ, 1 2D, 2 2D LZ, 3 2D LL, 4 2D DC, 5 2D LL DC,
   * 6 2D LZ DC, 7 2D array, 8 2D array LZ, 9 2D array LZ DC, 10 3D,
   * 11 3D LZ, 12 cube, 13 cube LL. */
  Lod_Mode mode = LOD_ZERO;
  switch (target) {
  case 0: tex_args(t, w, 1, a); x = fl(a[0]); break;
  case 1: case 2: tex_args(t, w, 2, a); x = fl(a[0]); y = fl(a[1]); mode = target == 1u ? LOD_AUTO : LOD_ZERO; break;
  case 3: tex_args(t, w, 3, a); x = fl(a[0]); y = fl(a[1]); lod = a[2]; mode = LOD_EXPLICIT; break;
  case 4: tex_args(t, w, 3, a); x = fl(a[0]); y = fl(a[1]); dref = fl(a[2]); shadow = true; mode = LOD_AUTO; break;
  case 5: tex_args(t, w, 4, a); x = fl(a[0]); y = fl(a[1]); lod = a[2]; dref = fl(a[3]); shadow = true; mode = LOD_EXPLICIT;
    break;
  case 6: tex_args(t, w, 3, a); x = fl(a[0]); y = fl(a[1]); dref = fl(a[2]); shadow = true; break;
  case 7: case 8: tex_args(t, w, 3, a); layer = ex("f32(%s)", a[0].s); x = fl(a[1]); y = fl(a[2]);
    mode = target == 7u ? LOD_AUTO : LOD_ZERO; break;
  case 9: tex_args(t, w, 4, a); layer = ex("f32(%s)", a[0].s); x = fl(a[1]); y = fl(a[2]); dref = fl(a[3]); shadow = true;
    break;
  case 10: case 11: case 12: tex_args(t, w, 3, a); x = fl(a[0]); y = fl(a[1]); z = fl(a[2]); break;
  case 13: tex_args(t, w, 4, a); x = fl(a[0]); y = fl(a[1]); z = fl(a[2]); lod = a[3]; mode = LOD_EXPLICIT; break;
  default: break;
  }
  if (b == WGSL_NO_BINDING) {
    EMIT("let tx = %s; ", k_unbound);
  } else {
    EMIT("let tc = vec3<f32>(%s, %s, %s); let tl = %s; ", x.s, y.s, z.s, layer.s);
    const Ex l = lod_expr(t, b, mode, lod, ex("tc"), ex("tl"));
    EMIT("let tx = t%u_sample(tc, tl, %s, %s, vec2<i32>(0), %s); ", b, dref.s, shadow ? "true" : "false", l.s);
  }
  write_scalar(t, w, comps, n, !BIT(w, SM_TEXS_F32_BIT));
}

static void emit_tlds(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  const uint32_t target = BITS(w, 53, 4), b = texture_binding(t, pc);
  uint32_t comps[4];
  const uint32_t n = scalar_components(BITS(w, 50, 3), BITS(w, 28, 8) != SM_RZ, comps);
  Ex a[8];
  Ex x = ex("0"), y = ex("0"), layer = ex("0");
  switch (target) {
  case 0: tex_args(t, w, 1, a); x = ex("i32(%s)", a[0].s); break;
  case 1: tex_args(t, w, 2, a); x = ex("i32(%s)", a[0].s); break;
  case 2: tex_args(t, w, 2, a); x = ex("i32(%s)", a[0].s); y = ex("i32(%s)", a[1].s); break;
  case 4: case 5: case 6: case 12:
    tex_args(t, w, target == 12u ? 4u : 3u, a);
    x = ex("i32(%s)", a[0].s);
    y = ex("i32(%s)", a[1].s);
    break;
  case 7: tex_args(t, w, 3, a); x = ex("i32(%s)", a[0].s); y = ex("i32(%s)", a[1].s); break;
  case 8: tex_args(t, w, 3, a); layer = ex("i32(f32(%s))", a[0].s); x = ex("i32(%s)", a[1].s); y = ex("i32(%s)", a[2].s);
    break;
  default: break;
  }
  if (b == WGSL_NO_BINDING) EMIT("let tx = %s; ", k_unbound);
  else EMIT("let tx = t%u_fetch(%s, %s, %s); ", b, x.s, y.s, layer.s);
  write_scalar(t, w, comps, n, !BIT(w, SM_TEXS_F32_BIT));
}

static void emit_tld4s(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  const uint32_t b = texture_binding(t, pc);
  const bool has_offset = BIT(w, 51) != 0, shadow = BIT(w, 50) != 0;
  const uint32_t n = 2u + (has_offset ? 1u : 0u) + (shadow ? 1u : 0u);
  Ex a[8];
  tex_args(t, w, n, a);
  uint32_t k = 2;
  Ex off = ex("vec2<i32>(0)"), dref = ex("0.0");
  if (has_offset) {
    off = ex("vec2<i32>(i32(%s << 26u) >> 26u, i32(%s << 18u) >> 26u)", a[k].s, a[k].s);
    k++;
  }
  if (shadow) dref = fl(a[k]);
  if (b == WGSL_NO_BINDING) EMIT("let tx = %s; ", k_unbound);
  else
    EMIT("let tx = t%u_gather(vec3<f32>(F(%s), F(%s), 0.0), 0.0, %uu, %s, %s, %s); ", b, a[0].s, a[1].s, BITS(w, 52, 2),
         dref.s, shadow ? "true" : "false", off.s);
  static const uint32_t all[4] = {0, 1, 2, 3};
  write_scalar(t, w, all, 4, false);
}

static void emit_tex_vector(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  const uint32_t mask = BITS(w, 31, 4);
  const uint32_t dim = BITS(w, 29, 2);
  const uint32_t dims = dim == 3u ? 3u : dim + 1u;
  const bool array = BIT(w, 28) != 0;
  enum { K_SAMPLE, K_FETCH, K_GATHER, K_DIMS, K_ZERO } kind;
  bool lod = false, bias = false, offset = false, dc = false, ms = false, lod_zero = false;
  uint32_t gather_comp = 0;
  switch (in->op) {
  case SM_OP_TEX: {
    const uint32_t lodm = BITS(w, 55, 2);
    kind = K_SAMPLE;
    lod = lodm == 3u;
    bias = lodm == 2u;
    lod_zero = lodm == 1u;
    offset = BIT(w, 54) != 0;
    dc = BIT(w, 50) != 0;
    break;
  }
  case SM_OP_TEX_B: {
    const uint32_t lodm = BITS(w, 37, 2);
    kind = K_SAMPLE;
    lod = lodm == 3u;
    bias = lodm == 2u;
    lod_zero = lodm == 1u;
    offset = BIT(w, 36) != 0;
    dc = BIT(w, 50) != 0;
    break;
  }
  case SM_OP_TLD:
    kind = K_FETCH;
    lod = BIT(w, 55) != 0;
    ms = BIT(w, 50) != 0;
    offset = BIT(w, 35) != 0;
    break;
  case SM_OP_TLD4:
    kind = K_GATHER;
    gather_comp = BITS(w, 56, 2);
    offset = BITS(w, 54, 2) != 0;
    dc = BIT(w, 50) != 0;
    break;
  case SM_OP_TXD: kind = K_SAMPLE; break;
  case SM_OP_TXQ: kind = BITS(w, 22, 6) == 1u ? K_DIMS : K_ZERO; break;
  default: kind = K_ZERO; break;
  }
  const uint32_t b = kind == K_ZERO ? WGSL_NO_BINDING : texture_binding(t, pc);
  if (kind == K_ZERO) {
    EMIT("let tx = vec4<u32>(0u); ");
  } else if (b == WGSL_NO_BINDING) {
    EMIT("let tx = %s; ", k_unbound);
  } else if (kind == K_DIMS) {
    EMIT("let tx = t%u_dims(); ", b);
  } else {
    const uint32_t in_a = (array ? 1u : 0u) + dims;
    uint32_t n = in_a + ((lod || bias) ? 1u : 0u) + (offset ? 1u : 0u) + (ms ? 1u : 0u) + (dc ? 1u : 0u);
    Ex a[8];
    tex_args_vec(t, w, n, in_a, a, in->op == SM_OP_TEX_B ? 1u : 0u);
    uint32_t k = 0;
    Ex layer = ex("0u");
    if (array) layer = ex("(%s & 0xffffu)", a[k++].s);
    Ex c[3] = {ex("0u"), ex("0u"), ex("0u")};
    for (uint32_t i = 0; i < dims; i++) c[i] = a[k++];
    Ex lod_value = ex("0u");
    if (lod || bias) lod_value = a[k++];
    Ex off = ex("vec2<i32>(0)");
    if (offset) {
      off = ex("vec2<i32>(i32(%s << 28u) >> 28u, i32(%s << 24u) >> 28u)", a[k].s, a[k].s);
      k++;
    }
    if (ms) k++;
    Ex dref = ex("0.0");
    if (dc) dref = fl(a[k]);
    if (kind == K_FETCH) {
      EMIT("let tx = t%u_fetch(i32(%s), i32(%s), i32(f32(%s))); ", b, c[0].s, dims > 1u ? c[1].s : "0u", layer.s);
    } else if (kind == K_GATHER) {
      EMIT("let tx = t%u_gather(vec3<f32>(F(%s), F(%s), F(%s)), f32(%s), %uu, %s, %s, %s); ", b, c[0].s, c[1].s, c[2].s,
           layer.s, gather_comp, dref.s, dc ? "true" : "false", off.s);
    } else {
      EMIT("let tc = vec3<f32>(F(%s), F(%s), F(%s)); let tl = f32(%s); ", c[0].s, c[1].s, c[2].s, layer.s);
      const Lod_Mode mode = in->op == SM_OP_TXD ? LOD_ZERO
                            : lod               ? LOD_EXPLICIT
                            : bias              ? LOD_BIAS
                            : lod_zero          ? LOD_ZERO
                                                : LOD_AUTO;
      const Ex l = lod_expr(t, b, mode, lod_value, ex("tc"), ex("tl"));
      EMIT("let tx = t%u_sample(tc, tl, %s, %s, %s, %s); ", b, dref.s, dc ? "true" : "false", off.s, l.s);
    }
  }
  uint32_t d = REG_D(w);
  for (uint32_t c = 0; c < 4; c++) {
    if (!(mask & (1u << c))) continue;
    if (d != SM_RZ) {
      EMIT("%s = tx[%u]; ", reg_dst(t, d).s, c);
      d = (d + 1u) & 0xffu;
    }
  }
}

/* ---- attributes (IPA) --------------------------------------------- */

/* The u32 value of input attribute `addr` (a constant address). */
static Ex attribute(Tr *t, uint32_t addr) {
  switch (addr) {
  case SM_ATTR_POSITION: return ex("U(fin.pos.x)");
  case SM_ATTR_POSITION + 4u: return ex("U(fy)");
  case SM_ATTR_POSITION + 8u: return ex("U(fin.pos.z)");
  case SM_ATTR_POSITION + 12u: return ex("U(fin.inv_w)");
  case SM_ATTR_FRONT_FACING: return ex("select(0u, 0xffffffffu, ff)");
  default: break;
  }
  if (addr >= SM_ATTR_GENERIC && addr < SM_ATTR_GENERIC + 16u * SM_ATTR_GENERIC_COUNT) {
    const uint32_t v = (addr - SM_ATTR_GENERIC) / 16u, c = (addr / 4u) & 3u;
    const uint32_t loc = t->d->varying_location[v];
    if (loc == 0xffu || loc >= t->d->varying_count) return ex("0u");
    if ((t->d->flat_mask >> loc) & 1u) return ex("fin.v%u[%u]", loc, c);
    return ex("U(fin.v%u[%u])", loc, c);
  }
  return ex("0u");
}

static void emit_ipa(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t addr0 = BITS(w, 28, 10);
  const bool indexed = BIT(w, 38) != 0, multiply = BITS(w, 54, 2) == 1u, sat = BIT(w, 51) != 0;
  const Ex d = reg_dst(t, REG_D(w));
  if (!indexed && addr0 == SM_ATTR_FRONT_FACING) {
    EMIT("%s = %s; ", d.s, attribute(t, addr0).s);
    return;
  }
  EMIT("{ ");
  if (indexed) {
    /* Every attribute the program could address. */
    const Ex idx = reg(t, REG_A(w));
    EMIT("let ia = %uu + %s; var av = 0u; switch (ia) { ", addr0, idx.s);
    for (uint32_t a = SM_ATTR_POSITION; a < SM_ATTR_POSITION + 16u; a += 4u) EMIT("case %uu: { av = %s; } ", a, attribute(t, a).s);
    for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
      if (t->d->varying_location[v] == 0xffu) continue;
      for (uint32_t c = 0; c < 4u; c++) {
        const uint32_t a = SM_ATTR_GENERIC + 16u * v + 4u * c;
        EMIT("case %uu: { av = %s; } ", a, attribute(t, a).s);
      }
    }
    EMIT("case %uu: { av = %s; } default: { } } ", SM_ATTR_FRONT_FACING, attribute(t, SM_ATTR_FRONT_FACING).s);
    EMIT("var f = F(av); ");
  } else {
    EMIT("var f = F(%s); ", attribute(t, addr0).s);
  }
  if (multiply) EMIT("f = f * F(%s); ", reg(t, REG_B(w)).s);
  if (sat) EMIT("f = sat(f); ");
  if (indexed) EMIT("%s = select(U(f), av, ia == %uu); } ", d.s, SM_ATTR_FRONT_FACING);
  else EMIT("%s = U(f); } ", d.s);
}

/* ---- vertex attributes (ALD / AST) --------------------------------- */

/* As the interpreter: inputs and outputs are 256-word attribute arrays
 * (ain / aout), addressed by (base + index register) / 4; ALD of the vertex
 * or instance id address reads those ids. */
static void emit_ald(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t count = BITS(w, 47, 2) + 1u, d = REG_D(w);
  const bool out = BIT(w, 32) != 0;
  if (d == SM_RZ) return;
  EMIT("{ let ab = %uu + %s; ", BITS(w, 20, 10), reg(t, REG_A(w)).s);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t r = (d + i) & 0xffu;
    if (r == SM_RZ) continue;
    EMIT("{ let a = ab + %uu; var v = %s[(a >> 2u) & %uu]; ", 4u * i, out ? "aout" : "ain", SM_ATTRIBUTE_WORDS - 1u);
    if (!out) EMIT("if (a == %uu) { v = vid; } if (a == %uu) { v = iid; } ", SM_ATTR_VERTEX_ID, SM_ATTR_INSTANCE_ID);
    EMIT("%s = v; } ", reg_dst(t, r).s);
  }
  EMIT("} ");
}

static void emit_ast(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t count = BITS(w, 47, 2) + 1u, d = REG_D(w);
  EMIT("{ let ab = (%uu + %s) >> 2u; ", BITS(w, 20, 10), reg(t, REG_A(w)).s);
  for (uint32_t i = 0; i < count; i++)
    EMIT("aout[(ab + %uu) & %uu] = %s; ", i, SM_ATTRIBUTE_WORDS - 1u, d == SM_RZ ? "0u" : reg(t, (d + i) & 0xffu).s);
  EMIT("} ");
}

/* ---- memory ------------------------------------------------------- */

static uint32_t access_bytes(uint32_t size) {
  switch (size) {
  case 0: case 1: return 1;
  case 2: case 3: return 2;
  case 5: return 8;
  case 6: return 16;
  default: return 4;
  }
}

/* Writes the loaded value(s) `word(i)` to Rd.. (load_to_regs). `fmt` is a
 * printf format taking the word index i. */
static void sign_extend_small(Tr *t, uint32_t size, const Ex *d) {
  if (size == 1) EMIT("%s = u32(i32(%s << 24u) >> 24u); ", d->s, d->s);
  if (size == 3) EMIT("%s = u32(i32(%s << 16u) >> 16u); ", d->s, d->s);
}

static void emit_ldc(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t slot = BITS(w, 36, 5), size = BITS(w, 48, 3), n = access_bytes(size);
  const int32_t off = (int32_t)(BITS(w, 20, 16) << 16) >> 16;
  const uint32_t dr = REG_D(w);
  if (slot >= SM_CBUF_SLOTS) {
    for (uint32_t i = 0; i < (n < 4u ? 1u : n / 4u); i++)
      if (((dr + i) & 0xffu) != SM_RZ && dr != SM_RZ) EMIT("%s = 0u; ", reg_dst(t, dr + i).s);
    return;
  }
  t->cbuf_slots |= 1u << slot;
  EMIT("let la = %s + 0x%xu; ", reg(t, REG_A(w)).s, (uint32_t)off);
  if (n < 4u) {
    const Ex d = reg_dst(t, dr);
    EMIT("%s = cbyte(%uu, la, %uu); ", d.s, slot, n);
    sign_extend_small(t, size, &d);
    return;
  }
  /* All-or-nothing: the whole access must fit (cbyte checks each word;
   * the interpreter checks the total). */
  EMIT("let lok = la + %uu <= D[%uu] * 4u; ", n, t->cbt + 2u * slot + 1u);
  for (uint32_t i = 0; i < n / 4u; i++) {
    if (dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ) continue;
    EMIT("%s = select(0u, cbyte(%uu, la + %uu, 4u), lok); ", reg_dst(t, dr + i).s, slot, 4u * i);
  }
}

static void emit_shared(Tr *t, const Sm_Insn *in);

static void emit_local(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t top = (uint32_t)(w >> 48) & SM_LDST_SPACE_MASK;
  if (top == SM_LDS_OPCODE || top == SM_STS_OPCODE) {
    if (!t->compute) {
      fail(t, "shared memory outside a compute program");
      return;
    }
    emit_shared(t, in);
    return;
  }
  const uint32_t size = BITS(w, 48, 3), n = access_bytes(size);
  const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
  const uint32_t dr = REG_D(w);
  t->uses_local = true;
  EMIT("let la = %s + 0x%xu; let lok = la + %uu <= %uu && la + %uu >= la; ", reg(t, REG_A(w)).s, (uint32_t)off, n,
       SM_LOCAL_BYTES, n);
  if (in->op == SM_OP_LDL) {
    if (n < 4u) {
      const Ex d = reg_dst(t, dr);
      EMIT("%s = select(0u, (lm[(la >> 2u) & %uu] >> ((la & 3u) * 8u)) & 0x%xu, lok); ", d.s, LOCAL_WORDS - 1u,
           (1u << (n * 8u)) - 1u);
      sign_extend_small(t, size, &d);
      return;
    }
    for (uint32_t i = 0; i < n / 4u; i++) {
      if (dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ) continue;
      EMIT("%s = select(0u, lm[((la >> 2u) + %uu) & %uu], lok); ", reg_dst(t, dr + i).s, i, LOCAL_WORDS - 1u);
    }
    return;
  }
  /* STL */
  EMIT("if (lok) { ");
  if (n < 4u) {
    const uint32_t mask = (1u << (n * 8u)) - 1u;
    EMIT("let lw = (la >> 2u) & %uu; let ls = (la & 3u) * 8u; lm[lw] = (lm[lw] & ~(0x%xu << ls)) | ((%s & 0x%xu) << ls); ",
         LOCAL_WORDS - 1u, mask, reg(t, dr).s, mask);
  } else {
    for (uint32_t i = 0; i < n / 4u; i++)
      EMIT("lm[((la >> 2u) + %uu) & %uu] = %s; ", i, LOCAL_WORDS - 1u, dr == SM_RZ ? "0u" : reg(t, dr + i).s);
  }
  EMIT("} ");
}

/* LDS/STS: the block's shared memory, `sh` (desc->shared_bytes): out of
 * range reads 0 and writes nothing, as the interpreter. Words only - a
 * sub-word store would race with its neighbours' lanes. */
static void emit_shared(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t size = BITS(w, 48, 3), n = access_bytes(size);
  const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
  const uint32_t dr = REG_D(w), bytes = t->d->shared_bytes & ~3u;
  if (!bytes) {
    fail(t, "shared memory with none allocated");
    return;
  }
  t->uses_shared = true;
  EMIT("let sa = %s + 0x%xu; let sok = sa + %uu <= %uu && sa + %uu >= sa && (sa & 3u) == 0u; ", reg(t, REG_A(w)).s,
       (uint32_t)off, n < 4u ? 4u : n, bytes, n < 4u ? 4u : n);
  if (in->op == SM_OP_LDL) {
    if (n < 4u) {
      fail(t, "sub-word LDS");
      return;
    }
    for (uint32_t i = 0; i < n / 4u; i++) {
      if (dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ) continue;
      EMIT("%s = select(0u, sh[min((sa >> 2u) + %uu, %uu)], sok); ", reg_dst(t, dr + i).s, i, bytes / 4u - 1u);
    }
    return;
  }
  if (n < 4u) {
    fail(t, "sub-word STS");
    return;
  }
  EMIT("if (sok) { ");
  for (uint32_t i = 0; i < n / 4u; i++)
    EMIT("sh[min((sa >> 2u) + %uu, %uu)] = %s; ", i, bytes / 4u - 1u, dr == SM_RZ ? "0u" : reg(t, dr + i).s);
  EMIT("} ");
}

/* LDG/STG in a compute program: the 64-bit address (32-bit without .E)
 * plus the offset, looked up in the bound windows (gfind); a miss reads 0
 * and writes nothing. Words only: Maxwell needs natural alignment, and
 * sub-word stores would race with neighbouring lanes. */
static void emit_gmem(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t size = BITS(w, 48, 3), n = access_bytes(size);
  const bool wide = BIT(w, 45) != 0;
  const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
  const uint32_t ra = REG_A(w), dr = REG_D(w);
  const bool load = in->op == SM_OP_LDG;
  if (n < 4u && !load) {
    fail(t, "sub-word STG");
    return;
  }
  t->uses_globals = true;
  EMIT("{ let a0 = %s; let ga = a0 + 0x%xu; ", ra == SM_RZ ? "0u" : reg(t, ra).s, (uint32_t)off);
  if (wide && ra != SM_RZ)
    EMIT("let gh = %s %s; ", reg(t, (ra + 1u) & 0xffu).s, off >= 0 ? "+ select(0u, 1u, ga < a0)" : "- select(0u, 1u, ga > a0)");
  else
    EMIT("let gh = %s; ", off >= 0 ? "select(0u, 1u, ga < a0)" : "0u - select(0u, 1u, ga > a0)");
  EMIT("let gw = gfind(ga, gh, %uu); ", n < 4u ? 4u : n);
  if (load) {
    if (n < 4u) {
      if (dr != SM_RZ) {
        const Ex d = reg_dst(t, dr);
        EMIT("%s = (gld(vec2<u32>(gw.x, gw.y & ~3u)) >> ((gw.y & 3u) * 8u)) & 0x%xu; ", d.s, n == 1u ? 0xffu : 0xffffu);
        sign_extend_small(t, size, &d);
      }
    } else {
      for (uint32_t i = 0; i < n / 4u; i++) {
        if (dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ) continue;
        EMIT("%s = gld(vec2<u32>(gw.x, gw.y + %uu)); ", reg_dst(t, dr + i).s, 4u * i);
      }
    }
  } else {
    for (uint32_t i = 0; i < n / 4u; i++)
      EMIT("gst(vec2<u32>(gw.x, gw.y + %uu), %s); ", 4u * i, dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ ? "0u" : reg(t, dr + i).s);
  }
  EMIT("} ");
}

/* ---- quad operations ---------------------------------------------- */

/* SHFL within a quad (maxwell_shader.c's SHFL with lane = the quad lane
 * ql: a fragment's warp position beyond its quad is unknown here). Lane j
 * outside the quad keeps the predicate's validity but reads the lane's own
 * value. Immediate operands resolve per quad lane at translation;
 * register operands at run time. */
static void emit_shfl(Tr *t, const Sm_Insn *in) {
  const uint64_t w = in->raw;
  const uint32_t mode = BITS(w, 30, 2);
  const uint32_t pd = BITS(w, 48, 3);
  if (BIT(w, 28) && BIT(w, 29)) {
    const uint32_t bv = BITS(w, 20, 5), cv = BITS(w, 34, 13);
    const uint32_t segmask = (cv >> 8) & 0x1fu, clamp = cv & 0x1fu;
    uint32_t sources = 0, valid = 0;
    for (uint32_t l = 0; l < 4u; l++) {
      const uint32_t max_lane = (l & segmask) | (clamp & ~segmask), min_lane = l & segmask;
      int32_t j;
      bool ok;
      switch (mode) {
      case 0: j = (int32_t)(min_lane | (bv & ~segmask)); ok = (uint32_t)j <= max_lane; break;
      case 1: j = (int32_t)l - (int32_t)bv; ok = j >= (int32_t)max_lane; break;
      case 2: j = (int32_t)(l + bv); ok = (uint32_t)j <= max_lane; break;
      default: j = (int32_t)(l ^ bv); ok = (uint32_t)j <= max_lane; break;
      }
      if (!ok || j < 0 || j >= (int32_t)SM_LANES) {
        j = (int32_t)l;
      } else {
        valid |= 1u << l;
        if (j > 3) j = (int32_t)l;
      }
      sources |= (uint32_t)j << (2u * l);
    }
    EMIT("{ let sv = quad_read_(%s, (0x%xu >> (ql * 2u)) & 3u, ql); ", reg(t, REG_A(w)).s, sources);
    if (pd < SM_PT) EMIT("p%u = ((0x%xu >> ql) & 1u) != 0u; ", pd, valid);
    EMIT("%s = sv; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  const Ex b = BIT(w, 28) ? ex("%uu", BITS(w, 20, 5)) : ex("(%s & 31u)", reg(t, REG_B(w)).s);
  const Ex c = BIT(w, 29) ? ex("%uu", BITS(w, 34, 13)) : reg(t, REG_C(w));
  EMIT("{ let bv = %s; let cv = %s; let sg = (cv >> 8u) & 31u; let mx = i32((ql & sg) | (cv & 31u & ~sg)); ", b.s, c.s);
  switch (mode) {
  case 0: EMIT("let j = i32((ql & sg) | (bv & ~sg)); let ok = j <= mx; "); break;
  case 1: EMIT("let j = i32(ql) - i32(bv); let ok = j >= mx; "); break;
  case 2: EMIT("let j = i32(ql + bv); let ok = j <= mx; "); break;
  default: EMIT("let j = i32(ql ^ bv); let ok = j <= mx; "); break;
  }
  EMIT("let vl = ok && j >= 0 && j < %d; let sv = quad_read_(%s, select(ql, u32(j), vl && j <= 3), ql); ",
       (int)SM_LANES, reg(t, REG_A(w)).s);
  if (pd < SM_PT) EMIT("p%u = vl; ", pd);
  EMIT("%s = sv; } ", reg_dst(t, REG_D(w)).s);
}

/* ---- instructions ------------------------------------------------- */

static void emit_insn(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  switch ((Sm_Op)in->op) {
  case SM_OP_FADD: {
    const Ex a = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
    const Ex b = fmod_an(fl(op_b(t, in)), BIT(w, 49), BIT(w, 45));
    const Ex r = ex("%s + %s", a.s, b.s);
    EMIT("%s = U(%s%s%s); ", reg_dst(t, REG_D(w)).s, BIT(w, 50) ? "sat(" : "(", r.s, ")");
    return;
  }
  case SM_OP_FADD32I: {
    const Ex a = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 54), BIT(w, 56));
    float bf;
    memcpy(&bf, &in->imm, sizeof(bf));
    const Ex b = fmod_an(imm_f(bf), BIT(w, 57), BIT(w, 53));
    EMIT("%s = U(%s + %s); ", reg_dst(t, REG_D(w)).s, a.s, b.s);
    return;
  }
  case SM_OP_FMUL: {
    static const float scale[8] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f, 1.0f};
    const float k = scale[BITS(w, 41, 3)] * (BIT(w, 48) ? -1.0f : 1.0f);
    const Ex a = fl(reg(t, REG_A(w))), b = fl(op_b(t, in));
    EMIT("%s = U(%s(%s * %s * %s)); ", reg_dst(t, REG_D(w)).s, BIT(w, 50) ? "sat" : "", a.s, b.s, imm_f(k).s);
    return;
  }
  case SM_OP_FMUL32I: {
    const Ex a = fl(reg(t, REG_A(w)));
    EMIT("%s = U(%s(%s * F(0x%xu))); ", reg_dst(t, REG_D(w)).s, BIT(w, 55) ? "sat" : "", a.s, in->imm);
    return;
  }
  case SM_OP_FFMA: {
    const Ex a = fl(reg(t, REG_A(w))), b = fl(op_b(t, in)), c = fl(op_c(t, in));
    EMIT("%s = U(%s(%s * %s * %s + %s * %s)); ", reg_dst(t, REG_D(w)).s, BIT(w, 50) ? "sat" : "", a.s, b.s,
         BIT(w, 48) ? "-1.0" : "1.0", c.s, BIT(w, 49) ? "-1.0" : "1.0");
    return;
  }
  case SM_OP_FFMA32I: {
    const Ex a = fl(reg(t, REG_A(w))), c = fl(reg(t, REG_D(w)));
    float bf;
    memcpy(&bf, &in->imm, sizeof(bf));
    bf *= BIT(w, 56) ? -1.0f : 1.0f;
    EMIT("%s = U(%s(%s * %s + %s * %s)); ", reg_dst(t, REG_D(w)).s, BIT(w, 55) ? "sat" : "", a.s, imm_f(bf).s, c.s,
         BIT(w, 57) ? "-1.0" : "1.0");
    return;
  }
  case SM_OP_FMNMX: {
    const Ex x = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
    const Ex y = fmod_an(fl(op_b(t, in)), BIT(w, 49), BIT(w, 45));
    const Ex mins = pred(BITS(w, 39, 3), BIT(w, 42));
    EMIT("{ let x = %s; let y = %s; %s = U(select(fmax_(x, y), fmin_(x, y), %s)); } ", x.s, y.s,
         reg_dst(t, REG_D(w)).s, mins.s);
    return;
  }
  case SM_OP_FSET: {
    const Ex a = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 54), BIT(w, 43));
    const Ex b = fmod_an(fl(op_b(t, in)), BIT(w, 44), BIT(w, 53));
    EMIT("%s = select(0u, 0x%xu, bop(%uu, fcmp(%uu, %s, %s), %s)); ", reg_dst(t, REG_D(w)).s,
         BIT(w, 52) ? F32_ONE : 0xffffffffu, BITS(w, 45, 2), BITS(w, 48, 4), a.s, b.s,
         pred(BITS(w, 39, 3), BIT(w, 42)).s);
    return;
  }
  case SM_OP_FSETP: {
    const Ex a = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 7), BIT(w, 43));
    const Ex b = fmod_an(fl(op_b(t, in)), BIT(w, 44), BIT(w, 6));
    EMIT("{ let cm = fcmp(%uu, %s, %s); let pc_ = %s; ", BITS(w, 48, 4), a.s, b.s, pred(BITS(w, 39, 3), BIT(w, 42)).s);
    set_pred(t, BITS(w, 3, 3), ex("bop(%uu, cm, pc_)", BITS(w, 45, 2)).s);
    set_pred(t, BITS(w, 0, 3), ex("bop(%uu, !cm, pc_)", BITS(w, 45, 2)).s);
    EMIT("} ");
    return;
  }
  case SM_OP_HADD2:
  case SM_OP_HMUL2:
  case SM_OP_HFMA2: {
    const Half_Fields f = half_fields(in);
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in);
    const Ex d = reg_dst(t, REG_D(w));
    EMIT("{ let x = hx(%s, %uu, %s, %s); let y = hx(%s, %uu, %s, %s); ", a.s, f.swz_a, f.abs_a ? "true" : "false",
         f.neg_a ? "true" : "false", b.s, f.swz_b, f.abs_b ? "true" : "false", f.neg_b ? "true" : "false");
    if (in->op == SM_OP_HADD2) {
      EMIT("var r = x + y; ");
    } else {
      EMIT("var r = vec2<f32>(hmul(x.x, y.x, %uu), hmul(x.y, y.y, %uu)); ", f.precision, f.precision);
      if (in->op == SM_OP_HFMA2) {
        const Ex c = in->form == SM_FORM_IMM32 ? reg(t, REG_D(w)) : op_c(t, in);
        EMIT("r = r + hx(%s, %uu, false, %s); ", c.s, f.swz_c, f.neg_c ? "true" : "false");
      }
    }
    if (f.sat) EMIT("r = vec2<f32>(sat(r.x), sat(r.y)); ");
    EMIT("%s = hmerge(%s, r, %uu); } ", d.s, REG_D(w) == SM_RZ ? "0u" : d.s, f.merge);
    return;
  }
  case SM_OP_HSET2:
  case SM_OP_HSETP2: {
    const Half_Fields f = half_fields(in);
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in);
    const uint32_t bopv = BITS(w, 45, 2);
    EMIT("{ let x = hx(%s, %uu, %s, %s); let y = hx(%s, %uu, %s, %s); let pc_ = %s; ", a.s, f.swz_a,
         f.abs_a ? "true" : "false", f.neg_a ? "true" : "false", b.s, f.swz_b, f.abs_b ? "true" : "false",
         f.neg_b ? "true" : "false", pred(BITS(w, 39, 3), BIT(w, 42)).s);
    EMIT("let lo = bop(%uu, fcmp(%uu, x.x, y.x), pc_); let hi = bop(%uu, fcmp(%uu, x.y, y.y), pc_); ", bopv, f.cond, bopv,
         f.cond);
    if (in->op == SM_OP_HSET2) {
      const uint32_t yes = f.flag ? 0x3c00u : 0xffffu;
      EMIT("%s = select(0u, 0x%xu, lo) | select(0u, 0x%xu, hi); } ", reg_dst(t, REG_D(w)).s, yes, yes << 16);
      return;
    }
    if (f.flag) {
      set_pred(t, BITS(w, 3, 3), "(lo && hi)");
      set_pred(t, BITS(w, 0, 3), "!(lo && hi)");
    } else {
      set_pred(t, BITS(w, 3, 3), "lo");
      set_pred(t, BITS(w, 0, 3), "hi");
    }
    EMIT("} ");
    return;
  }
  case SM_OP_FCMP: {
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in), c = op_c(t, in);
    EMIT("%s = select(%s, %s, fcmp(%uu, F(%s), 0.0)); ", reg_dst(t, REG_D(w)).s, b.s, a.s, BITS(w, 48, 4), c.s);
    return;
  }
  case SM_OP_MUFU: {
    const Ex x = fmod_an(fl(reg(t, REG_A(w))), BIT(w, 46), BIT(w, 48));
    EMIT("%s = U(%s(mufu(%uu, %s))); ", reg_dst(t, REG_D(w)).s, BIT(w, 50) ? "sat" : "", BITS(w, 20, 4), x.s);
    return;
  }
  case SM_OP_RRO: {
    EMIT("%s = U(%s); ", reg_dst(t, REG_D(w)).s, fmod_an(fl(op_b(t, in)), BIT(w, 49), BIT(w, 45)).s);
    return;
  }
  case SM_OP_FSWZADD: {
    EMIT("{ let s = (0x%xu >> (2u * ql)) & 3u; %s = U(F(%s) * swz_ka(s) + F(%s) * swz_kb(s)); } ", BITS(w, 28, 8),
         reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, reg(t, REG_B(w)).s);
    return;
  }
  case SM_OP_F2F: {
    const Ex b = op_b(t, in);
    const uint32_t src_size = BITS(w, 10, 2), dst_size = BITS(w, 8, 2);
    Ex v = src_size == 1u ? ex("h2f(%s >> %uu)", b.s, BIT(w, 41) ? 16u : 0u) : fl(b);
    v = fmod_an(v, BIT(w, 49), BIT(w, 45));
    if (BIT(w, 42)) v = ex("rnd(%s, %uu)", v.s, BITS(w, 39, 2));
    if (BIT(w, 50)) v = ex("sat(%s)", v.s);
    EMIT("%s = %s; ", reg_dst(t, REG_D(w)).s, dst_size == 1u ? ex("f2h(%s)", v.s).s : ex("U(%s)", v.s).s);
    return;
  }
  case SM_OP_F2I: {
    const Ex b = op_b(t, in);
    Ex v = BITS(w, 10, 2) == 1u ? ex("h2f(%s)", b.s) : fl(b);
    v = ex("rnd(%s, %uu)", fmod_an(v, BIT(w, 49), BIT(w, 45)).s, BITS(w, 39, 2));
    EMIT("%s = f2i(%s, %s, %uu); ", reg_dst(t, REG_D(w)).s, v.s, BIT(w, 12) ? "true" : "false", BITS(w, 8, 2));
    return;
  }
  case SM_OP_I2F: {
    const Ex f = ex("i2f(%s, %uu, %uu, %s, %s, %s)", op_b(t, in).s, BITS(w, 10, 2), BITS(w, 41, 2),
                    BIT(w, 13) ? "true" : "false", BIT(w, 49) ? "true" : "false", BIT(w, 45) ? "true" : "false");
    EMIT("%s = %s; ", reg_dst(t, REG_D(w)).s, BITS(w, 8, 2) == 1u ? ex("f2h(%s)", f.s).s : ex("U(%s)", f.s).s);
    return;
  }
  case SM_OP_I2I: {
    EMIT("%s = i2i(%s, %uu, %uu, %s, %s, %s, %uu, %s, %s); ", reg_dst(t, REG_D(w)).s, op_b(t, in).s, BITS(w, 10, 2),
         BITS(w, 41, 2), BIT(w, 13) ? "true" : "false", BIT(w, 49) ? "true" : "false", BIT(w, 45) ? "true" : "false",
         BITS(w, 8, 2), BIT(w, 12) ? "true" : "false", BIT(w, 50) ? "true" : "false");
    return;
  }
  case SM_OP_IADD:
  case SM_OP_IADD32I: {
    const bool is32i = in->op == SM_OP_IADD32I;
    const uint32_t na = is32i ? BIT(w, 56) : BIT(w, 49), nb = is32i ? 0u : BIT(w, 48);
    const bool x = is32i ? BIT(w, 53) != 0 : BIT(w, 43) != 0;
    const bool sat = is32i ? BIT(w, 54) != 0 : BIT(w, 50) != 0;
    const bool cc = is32i ? BIT(w, 52) != 0 : BIT(w, 47) != 0;
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in);
    EMIT("{ let av = %s%s; let bv = %s%s; let s1 = av + bv; let s = s1 + %s; ", na ? "0u - " : "", a.s, nb ? "0u - " : "",
         b.s, x ? "select(0u, 1u, ccc)" : "0u");
    EMIT("let r = %s; ", sat ? "iaddsat(av, bv)" : "s");
    if (cc)
      EMIT("ccc = s1 < av || s < s1; cco = ((~(av ^ bv) & (av ^ r)) >> 31u) != 0u; ccz = r == 0u; ccs = (r >> 31u) != 0u; ");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_IADD3: { /* maxwell_shader.c's IADD3 */
    const bool reg_form = in->form == SM_FORM_REG;
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in), c = reg(t, REG_C(w));
    const Ex srcs[3] = {a, b, c};
    const uint32_t height_bits[3] = {IADD3_HEIGHT_A_BIT, IADD3_HEIGHT_B_BIT, IADD3_HEIGHT_C_BIT};
    const uint32_t neg_bits[3] = {IADD3_NEG_A_BIT, IADD3_NEG_B_BIT, IADD3_NEG_C_BIT};
    EMIT("{ ");
    for (uint32_t k = 0; k < 3u; k++) {
      const uint32_t h = reg_form ? BITS(w, height_bits[k], 2) : 0u;
      const char *sel = h == IADD3_HEIGHT_LOWER ? " & 0xffffu" : h == IADD3_HEIGHT_UPPER ? " >> 16u" : "";
      EMIT("let v%u = %s(%s%s); ", k, BIT(w, neg_bits[k]) ? "0u - " : "", srcs[k].s, sel);
    }
    const uint32_t mode = reg_form ? BITS(w, IADD3_MODE_BIT, 2) : 0u;
    EMIT("let ab = v0 + v1; let abc = ab < v0; let s = %s; let s1 = s + v2; let r = s1 + %s; ",
         mode == IADD3_MODE_RIGHT_SHIFT ? "ab >> 16u" : mode == IADD3_MODE_LEFT_SHIFT ? "ab << 16u" : "ab",
         BIT(w, IADD3_X_BIT) ? "select(0u, 1u, ccc)" : "0u");
    if (BIT(w, IADD3_CC_BIT))
      EMIT("ccc = %s s1 < s || r < s1; cco = false; ccz = r == 0u; ccs = (r >> 31u) != 0u; ",
           mode == 0u ? "abc ||" : "");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_ISCADD: {
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in);
    EMIT("{ let r = ((%s%s) << %uu) + (%s%s); ", BIT(w, 49) ? "0u - " : "", a.s, BITS(w, 39, 5), BIT(w, 48) ? "0u - " : "",
         b.s);
    if (BIT(w, 47)) EMIT("ccz = r == 0u; ccs = (r >> 31u) != 0u; ");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_IMNMX: {
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in);
    EMIT("{ let a = %s; let b = %s; let al = %s; %s = select(select(a, b, al), select(b, a, al), %s); } ", a.s, b.s,
         BIT(w, 48) ? "i32(a) < i32(b)" : "a < b", reg_dst(t, REG_D(w)).s, pred(BITS(w, 39, 3), BIT(w, 42)).s);
    return;
  }
  case SM_OP_ISET: {
    EMIT("%s = select(0u, 0x%xu, bop(%uu, icmp(%uu, %s, %s, %s), %s)); ", reg_dst(t, REG_D(w)).s,
         BIT(w, 44) ? F32_ONE : 0xffffffffu, BITS(w, 45, 2), BITS(w, 49, 3), reg(t, REG_A(w)).s, op_b(t, in).s,
         BIT(w, 48) ? "true" : "false", pred(BITS(w, 39, 3), BIT(w, 42)).s);
    return;
  }
  case SM_OP_ISETP: {
    EMIT("{ let cm = icmp(%uu, %s, %s, %s); let pc_ = %s; ", BITS(w, 49, 3), reg(t, REG_A(w)).s, op_b(t, in).s,
         BIT(w, 48) ? "true" : "false", pred(BITS(w, 39, 3), BIT(w, 42)).s);
    set_pred(t, BITS(w, 3, 3), ex("bop(%uu, cm, pc_)", BITS(w, 45, 2)).s);
    set_pred(t, BITS(w, 0, 3), ex("bop(%uu, !cm, pc_)", BITS(w, 45, 2)).s);
    EMIT("} ");
    return;
  }
  case SM_OP_ICMP: {
    EMIT("%s = select(%s, %s, icmp(%uu, %s, 0u, %s)); ", reg_dst(t, REG_D(w)).s, op_b(t, in).s, reg(t, REG_A(w)).s,
         BITS(w, 49, 3), op_c(t, in).s, BIT(w, 48) ? "true" : "false");
    return;
  }
  case SM_OP_IMUL:
  case SM_OP_IMUL32I: {
    const bool is32i = in->op == SM_OP_IMUL32I;
    const bool sa = is32i ? BIT(w, 55) != 0 : BIT(w, 41) != 0;
    const bool high = is32i ? BIT(w, 53) != 0 : BIT(w, 39) != 0;
    EMIT("%s = mul64(%s, %s, %s)[%u]; ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s, sa ? "true" : "false",
         high ? 1u : 0u);
    return;
  }
  case SM_OP_IMAD: {
    const Ex a = reg(t, REG_A(w)), b = op_b(t, in), c = op_c(t, in);
    EMIT("{ var p = mul64(%s, %s, %s); %s %s = p[%u] + %s%s; } ", a.s, b.s, BIT(w, 53) ? "true" : "false",
         BIT(w, 51) ? "p = neg64(p);" : "", reg_dst(t, REG_D(w)).s, BIT(w, 54) ? 1u : 0u, BIT(w, 52) ? "0u - " : "", c.s);
    return;
  }
  case SM_OP_XMAD: {
    Ex b, c;
    bool psl, mrg, b_hi;
    uint32_t cmode;
    switch (in->form) {
    case SM_FORM_REG:
      b = reg(t, REG_B(w)); c = reg(t, REG_C(w));
      psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = BIT(w, 35) != 0; cmode = BITS(w, 50, 3);
      break;
    case SM_FORM_CBUF:
      b = cbuf(t, in->cbuf, in->imm); c = reg(t, REG_C(w));
      psl = BIT(w, 55) != 0; mrg = BIT(w, 56) != 0; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
      break;
    case SM_FORM_IMM:
      b = ex("0x%xu", in->imm); c = reg(t, REG_C(w));
      psl = BIT(w, 36) != 0; mrg = BIT(w, 37) != 0; b_hi = false; cmode = BITS(w, 50, 3);
      break;
    default:
      b = reg(t, REG_C(w)); c = cbuf(t, in->cbuf, in->imm);
      psl = false; mrg = false; b_hi = BIT(w, 52) != 0; cmode = BITS(w, 50, 2);
      break;
    }
    const bool a_hi = BIT(w, 53) != 0;
    EMIT("{ let a = %s; let b = %s; let c = %s; ", reg(t, REG_A(w)).s, b.s, c.s);
    EMIT("var pr = %s * %s; ", a_hi ? "(a >> 16u)" : "(a & 0xffffu)", b_hi ? "(b >> 16u)" : "(b & 0xffffu)");
    if (psl) EMIT("pr = pr << 16u; ");
    switch (cmode) {
    case 1: EMIT("let cv = c & 0xffffu; "); break;
    case 2: EMIT("let cv = c >> 16u; "); break;
    case 4: EMIT("let cv = c + (b << 16u); "); break;
    default: EMIT("let cv = c; "); break;
    }
    EMIT("var r = pr + cv; ");
    if (mrg) EMIT("r = (r & 0xffffu) | (b << 16u); ");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_LOP: {
    EMIT("{ let r = lop(%uu, %s ^ 0x%xu, %s ^ 0x%xu); ", BITS(w, 41, 2), reg(t, REG_A(w)).s,
         BIT(w, 39) ? 0xffffffffu : 0u, op_b(t, in).s, BIT(w, 40) ? 0xffffffffu : 0u);
    set_pred(t, BITS(w, 48, 3), "r != 0u");
    if (BIT(w, 47)) EMIT("ccz = r == 0u; ccs = (r >> 31u) != 0u; ");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_LOP32I: {
    EMIT("{ let r = lop(%uu, %s ^ 0x%xu, 0x%xu); ", BITS(w, 53, 2), reg(t, REG_A(w)).s, BIT(w, 55) ? 0xffffffffu : 0u,
         BIT(w, 56) ? ~in->imm : in->imm);
    if (BIT(w, 52)) EMIT("ccz = r == 0u; ccs = (r >> 31u) != 0u; ");
    EMIT("%s = r; } ", reg_dst(t, REG_D(w)).s);
    return;
  }
  case SM_OP_LOP3: {
    const uint32_t lut = in->form == SM_FORM_REG ? BITS(w, 28, 8) : BITS(w, 48, 8);
    const Ex b = in->form == SM_FORM_IMM ? ex("0x%xu", in->imm) : op_b(t, in);
    EMIT("%s = lop3(%uu, %s, %s, %s); ", reg_dst(t, REG_D(w)).s, lut, reg(t, REG_A(w)).s, b.s, reg(t, REG_C(w)).s);
    return;
  }
  case SM_OP_SHL: {
    EMIT("%s = shl_(%s, %s%s); ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s, BIT(w, 39) ? " & 31u" : "");
    return;
  }
  case SM_OP_SHR: {
    EMIT("%s = shr_(%s, %s%s, %s); ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s,
         BIT(w, 39) ? " & 31u" : "", BIT(w, 48) ? "true" : "false");
    return;
  }
  case SM_OP_BFE: {
    EMIT("%s = bfe_(%s, %s, %s, %s); ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s,
         BIT(w, 48) ? "true" : "false", BIT(w, 40) ? "true" : "false");
    return;
  }
  case SM_OP_BFI: {
    EMIT("%s = bfi_(%s, %s, %s); ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s, op_c(t, in).s);
    return;
  }
  case SM_OP_POPC: {
    EMIT("%s = countOneBits(%s ^ 0x%xu); ", reg_dst(t, REG_D(w)).s, op_b(t, in).s, BIT(w, 40) ? 0xffffffffu : 0u);
    return;
  }
  case SM_OP_FLO: {
    EMIT("%s = flo_(%s ^ 0x%xu, %s, %s); ", reg_dst(t, REG_D(w)).s, op_b(t, in).s, BIT(w, 40) ? 0xffffffffu : 0u,
         BIT(w, 48) ? "true" : "false", BIT(w, 41) ? "true" : "false");
    return;
  }
  case SM_OP_PRMT: {
    EMIT("%s = prmt_(%s, %s, %s); ", reg_dst(t, REG_D(w)).s, reg(t, REG_A(w)).s, op_b(t, in).s, op_c(t, in).s);
    return;
  }
  case SM_OP_SEL: {
    EMIT("%s = select(%s, %s, %s); ", reg_dst(t, REG_D(w)).s, op_b(t, in).s, reg(t, REG_A(w)).s,
         pred(BITS(w, 39, 3), BIT(w, 42)).s);
    return;
  }
  case SM_OP_MOV:
  case SM_OP_MOV32I: {
    const Ex b = in->op == SM_OP_MOV32I ? ex("0x%xu", in->imm) : op_b(t, in);
    EMIT("%s = %s; ", reg_dst(t, REG_D(w)).s, b.s);
    return;
  }
  case SM_OP_PSETP: {
    EMIT("{ let ab = bop(%uu, %s, %s); let pc_ = %s; ", BITS(w, 24, 2), pred(BITS(w, 12, 3), BIT(w, 15)).s,
         pred(BITS(w, 29, 3), BIT(w, 32)).s, pred(BITS(w, 39, 3), BIT(w, 42)).s);
    set_pred(t, BITS(w, 3, 3), ex("bop(%uu, ab, pc_)", BITS(w, 45, 2)).s);
    set_pred(t, BITS(w, 0, 3), ex("bop(%uu, !ab, pc_)", BITS(w, 45, 2)).s);
    EMIT("} ");
    return;
  }
  case SM_OP_P2R: {
    const uint32_t shift = BITS(w, 41, 2) * 8u;
    EMIT("{ var bits = 0u; ");
    for (uint32_t i = 0; i < 7u; i++) EMIT("if (p%u) { bits = bits | %uu; } ", i, 1u << i);
    EMIT("let b = %s; %s = (%s & ~(b << %uu)) | ((bits & b) << %uu); } ", op_b(t, in).s, reg_dst(t, REG_D(w)).s,
         reg(t, REG_A(w)).s, shift, shift);
    return;
  }
  case SM_OP_R2P: {
    const uint32_t shift = BITS(w, 41, 2) * 8u;
    EMIT("{ let v = %s >> %uu; let b = %s; ", reg(t, REG_A(w)).s, shift, op_b(t, in).s);
    for (uint32_t i = 0; i < 7u; i++) EMIT("if ((b & %uu) != 0u) { p%u = ((v >> %uu) & 1u) != 0u; } ", 1u << i, i, i);
    EMIT("} ");
    return;
  }
  case SM_OP_CSETP: {
    const char *cond;
    switch (BITS(w, 8, 5)) {
    case 0x00: cond = "false"; break;
    case 0x01: cond = "ccs"; break;
    case 0x02: cond = "ccz"; break;
    case 0x05: cond = "(!ccz)"; break;
    default: cond = "true"; break;
    }
    EMIT("{ let cm = %s; let pc_ = %s; ", cond, pred(BITS(w, 39, 3), BIT(w, 42)).s);
    set_pred(t, BITS(w, 3, 3), ex("bop(%uu, cm, pc_)", BITS(w, 45, 2)).s);
    set_pred(t, BITS(w, 0, 3), ex("bop(%uu, !cm, pc_)", BITS(w, 45, 2)).s);
    EMIT("} ");
    return;
  }
  case SM_OP_S2R:
  case SM_OP_CS2R: {
    const char *v;
    if (t->compute) {
      switch (BITS(w, 20, 8)) {
      case 0x00: v = "(lidx & 31u)"; break;
      case 0x20: v = "(lid.x | (lid.y << 16u) | (lid.z << 26u))"; break;
      case 0x21: v = "lid.x"; break;
      case 0x22: v = "lid.y"; break;
      case 0x23: v = "lid.z"; break;
      case 0x25: v = "wid.x"; break;
      case 0x26: v = "wid.y"; break;
      case 0x27: v = "wid.z"; break;
      case 0x38: v = "(1u << (lidx & 31u))"; break;
      case 0x39: v = "((1u << (lidx & 31u)) - 1u)"; break;
      case 0x3a: v = "((2u << (lidx & 31u)) - 1u)"; break;
      case 0x3b: v = "(~((2u << (lidx & 31u)) - 1u))"; break;
      case 0x3c: v = "(~((1u << (lidx & 31u)) - 1u))"; break;
      default: v = "0u"; break;
      }
      EMIT("%s = %s; ", reg_dst(t, REG_D(w)).s, v);
      return;
    }
    switch (BITS(w, 20, 8)) {
    case 0x00: v = "ql"; break;
    case 0x12: v = "0x3f800000u"; break;
    case 0x38: v = "(1u << ql)"; break;
    case 0x39: v = "((1u << ql) - 1u)"; break;
    case 0x3a: v = "((2u << ql) - 1u)"; break;
    case 0x3b: v = "(~((2u << ql) - 1u))"; break;
    case 0x3c: v = "(~((1u << ql) - 1u))"; break;
    default: v = "0u"; break;
    }
    EMIT("%s = %s; ", reg_dst(t, REG_D(w)).s, v);
    return;
  }
  case SM_OP_VOTE: {
    const Ex v = pred(BITS(w, 39, 3), BIT(w, 42));
    EMIT("{ let v = %s; %s = select(0u, 1u << ql, v); ", v.s, reg_dst(t, REG_D(w)).s);
    set_pred(t, BITS(w, 45, 3), BITS(w, 48, 2) <= 1u ? "v" : "true");
    EMIT("} ");
    return;
  }
  case SM_OP_SHFL: emit_shfl(t, in); return;
  case SM_OP_IPA: emit_ipa(t, in); return;
  case SM_OP_LDC: EMIT("{ "); emit_ldc(t, in); EMIT("} "); return;
  case SM_OP_LDL:
  case SM_OP_STL: EMIT("{ "); emit_local(t, in); EMIT("} "); return;
  case SM_OP_OUT: EMIT("%s = 0u; ", reg_dst(t, REG_D(w)).s); return;
  case SM_OP_TEXS: EMIT("{ "); emit_texs(t, in, pc); EMIT("} "); return;
  case SM_OP_TLDS: EMIT("{ "); emit_tlds(t, in, pc); EMIT("} "); return;
  case SM_OP_TLD4S: EMIT("{ "); emit_tld4s(t, in, pc); EMIT("} "); return;
  case SM_OP_TEX:
  case SM_OP_TEX_B:
  case SM_OP_TLD:
  case SM_OP_TLD4:
  case SM_OP_TXD:
  case SM_OP_TXQ:
  case SM_OP_TMML: EMIT("{ "); emit_tex_vector(t, in, pc); EMIT("} "); return;
  case SM_OP_ALD:
    if (t->vertex) emit_ald(t, in);
    else fail(t, "ALD in a pixel program");
    return;
  case SM_OP_AST:
    if (t->vertex) emit_ast(t, in);
    else fail(t, "AST in a pixel program");
    return;
  case SM_OP_LDG: if (t->compute) emit_gmem(t, in); else emit_ldg(t, in, pc); return;
  case SM_OP_LD:
  case SM_OP_ST:
  case SM_OP_STG:
    if (t->compute) emit_gmem(t, in);
    else fail(t, "global memory (%s)", sm_op_name((Sm_Op)in->op));
    return;
  default:
    /* NOP, SCHED, BARRIER and unknown words: nothing (as the interpreter). */
    return;
  }
}

/* ---- control flow ------------------------------------------------- */

static bool is_control(uint16_t op) {
  switch ((Sm_Op)op) {
  case SM_OP_BRA: case SM_OP_SYNC: case SM_OP_BRK: case SM_OP_CONT: case SM_OP_CAL: case SM_OP_RET:
  case SM_OP_EXIT: case SM_OP_KIL:
    return true;
  default:
    return false;
  }
}

static uint32_t norm_pc(uint32_t pc) { return pc % 4u == 0 ? pc + 1u : pc; }

/* BAR.SYNC (not DEPBAR, which shares SM_OP_BARRIER and does nothing here). */
static bool is_bar_sync(const Sm_Insn *in) {
  return in->op == SM_OP_BARRIER && ((uint32_t)(in->raw >> 48) & SM_LDST_SPACE_MASK) == SM_BAR_OPCODE;
}

/* Marks reachable words and basic-block leaders. Flow-stack targets
 * (SSY/PBK/PCNT) and return addresses are reached through SYNC/BRK/CONT/
 * RET, so they are leaders too. */
static void find_blocks(Tr *t) {
  const Sm_Program *p = t->p;
  uint32_t work[SM_MAX_WORDS];
  uint32_t top = 0;
  const uint32_t start = norm_pc(1u);
  if (start >= p->word_count) return;
  t->leader[start] = true;
  work[top++] = start;
  while (top > 0) {
    uint32_t pc = work[--top];
    while (pc < p->word_count && !t->reached[pc]) {
      t->reached[pc] = true;
      const Sm_Insn *in = &p->insns[pc];
      const uint32_t next = norm_pc(in->next);
      uint32_t targets[2];
      uint32_t nt = 0;
      switch ((Sm_Op)in->op) {
      case SM_OP_BRA: case SM_OP_SSY: case SM_OP_PBK: case SM_OP_PCNT: case SM_OP_CAL:
        if (in->target >= 0) targets[nt++] = norm_pc((uint32_t)in->target);
        break;
      default: break;
      }
      if ((is_control(in->op) || (t->compute && is_bar_sync(in))) && next < p->word_count) {
        t->leader[next] = true;
        if (!t->reached[next] && top < SM_MAX_WORDS) work[top++] = next;
      }
      for (uint32_t i = 0; i < nt; i++) {
        if (targets[i] >= p->word_count) continue;
        t->leader[targets[i]] = true;
        if (!t->reached[targets[i]] && top < SM_MAX_WORDS) work[top++] = targets[i];
      }
      if (in->op == SM_OP_EXIT && in->pred == SM_PT) break; /* unconditional: nothing falls through */
      if (in->op == SM_OP_BRA && in->pred == SM_PT && (in->raw & 0x1fu) == 15u) break;
      pc = next;
      if (pc < p->word_count && t->leader[pc]) break;
    }
  }
}

/* The guard of a control instruction (predicate and, where tested, CC). */
static Ex control_guard(const Sm_Insn *in) {
  const Ex p = pred(in->pred & 7u, in->pred & 8u);
  switch ((Sm_Op)in->op) {
  case SM_OP_BRA: case SM_OP_EXIT: case SM_OP_KIL: case SM_OP_BRK: case SM_OP_CONT: case SM_OP_RET: {
    const uint32_t cond = (uint32_t)(in->raw & 0x1fu);
    if (cond == 15u) return p;
    return ex("(%s && %s)", p.s, cc_test(cond).s);
  }
  default: return p;
  }
}

static void emit_pop(Tr *t, uint32_t kind, bool keep) {
  EMIT("pc = %uu; loop { if (fsd == 0u) { break; } if (fsk[fsd - 1u] == %uu) { pc = fst[fsd - 1u]; %s break; } "
       "fsd = fsd - 1u; } ",
       PC_FAULT, kind, keep ? "" : "fsd = fsd - 1u;");
}

static void emit_control(Tr *t, const Sm_Insn *in) {
  const Ex g = control_guard(in);
  const uint32_t next = norm_pc(in->next);
  switch ((Sm_Op)in->op) {
  case SM_OP_BRA:
    if (in->target < 0) {
      fail(t, "BRA through a constant buffer");
      return;
    }
    EMIT("if (%s) { pc = %uu; } else { pc = %uu; } ", g.s, norm_pc((uint32_t)in->target), next);
    return;
  case SM_OP_SYNC:
  case SM_OP_BRK:
  case SM_OP_CONT: {
    const uint32_t kind = in->op == SM_OP_SYNC ? STACK_SSY : (in->op == SM_OP_BRK ? STACK_PBK : STACK_PCNT);
    EMIT("if (%s) { ", g.s);
    emit_pop(t, kind, in->op == SM_OP_CONT);
    EMIT("} else { pc = %uu; } ", next);
    return;
  }
  case SM_OP_CAL:
    if (in->target < 0) {
      fail(t, "CAL through a constant buffer");
      return;
    }
    EMIT("if (csd >= %uu) { pc = %uu; } else { cst[csd] = %uu; csd = csd + 1u; pc = %uu; } ", SM_STACK_DEPTH, PC_FAULT,
         next, norm_pc((uint32_t)in->target));
    return;
  case SM_OP_RET:
    EMIT("if (%s) { if (csd == 0u) { pc = %uu; } else { csd = csd - 1u; pc = cst[csd]; } } else { pc = %uu; } ", g.s,
         PC_DONE, next);
    return;
  case SM_OP_EXIT:
    EMIT("if (%s) { pc = %uu; } else { pc = %uu; } ", g.s, PC_DONE, next);
    return;
  case SM_OP_KIL:
    EMIT("if (%s) { pc = %uu; } else { pc = %uu; } ", g.s, PC_KILL, next);
    return;
  default: return;
  }
}

static void emit_flow_push(Tr *t, const Sm_Insn *in) {
  const uint32_t kind = in->op == SM_OP_SSY ? STACK_SSY : (in->op == SM_OP_PBK ? STACK_PBK : STACK_PCNT);
  if (in->target < 0) {
    fail(t, "flow target through a constant buffer");
    return;
  }
  EMIT("if (fsd < %uu) { fsk[fsd] = %uu; fst[fsd] = %uu; fsd = fsd + 1u; } else { pc = %uu; break; } ", FLOW_DEPTH, kind,
       norm_pc((uint32_t)in->target), PC_FAULT);
}

static bool is_flow(uint16_t op) {
  switch ((Sm_Op)op) {
  case SM_OP_BRA: case SM_OP_SSY: case SM_OP_SYNC: case SM_OP_PBK: case SM_OP_BRK: case SM_OP_PCNT: case SM_OP_CONT:
  case SM_OP_CAL: case SM_OP_RET:
    return true;
  default:
    return false;
  }
}

/* Straight-line programs: each block's ending EXIT / KIL leaves the
 * enclosing `loop { ... break; }`. Returns false once nothing after it can
 * run (an unconditional end). */
static bool emit_straight_end(Tr *t, const Sm_Insn *in) {
  const Ex g = control_guard(in);
  const bool always = !strcmp(g.s, "true");
  const char *leave = in->op == SM_OP_KIL && !t->vertex ? "discard; break;" : "break;";
  if (!always) t->uniform_flow = false;
  if (always) EMIT("%s ", leave);
  else EMIT("if (%s) { %s } ", g.s, leave);
  return !always;
}

static void emit_block(Tr *t, uint32_t start) {
  const Sm_Program *p = t->p;
  if (t->straight) EMIT("    ");
  else EMIT("    case %uu: {\n      ", start);
  uint32_t pc = start;
  for (;;) {
    if (pc >= p->word_count) {
      EMIT("pc = %uu; ", PC_FAULT);
      break;
    }
    const Sm_Insn *in = &p->insns[pc];
    if (is_control(in->op)) {
      if (t->straight) t->straight_live = emit_straight_end(t, in);
      else emit_control(t, in);
      break;
    }
    if (t->compute && is_bar_sync(in)) {
      /* The phase ends here; the workgroup meets at the barrier outside the
       * dispatch loop (uniform control flow), then resumes at `next`. */
      const uint32_t next = norm_pc(in->next);
      if ((in->pred & 7u) != SM_PT || (in->pred & 8u)) fail(t, "predicated BAR");
      EMIT("pc = %uu; bar = true; ", next >= p->word_count ? PC_FAULT : next);
      break;
    }
    if (in->op == SM_OP_SSY || in->op == SM_OP_PBK || in->op == SM_OP_PCNT) {
      emit_flow_push(t, in);
    } else if (in->op != SM_OP_SCHED && in->op != SM_OP_NOP && in->op != SM_OP_BARRIER && in->op != SM_OP_INVALID) {
      const uint32_t pr = in->pred & 7u, neg = in->pred & 8u;
      if (!(pr == SM_PT && neg)) {
        if (pr == SM_PT) {
          emit_insn(t, in, pc);
        } else {
          EMIT("if (%s) { ", pred(pr, neg).s);
          t->guarded = true;
          emit_insn(t, in, pc);
          t->guarded = false;
          EMIT("} ");
        }
        EMIT("\n      ");
      }
    }
    const uint32_t next = norm_pc(in->next);
    if (next >= p->word_count || t->leader[next]) {
      if (t->straight) {
        if (next >= p->word_count) {
          EMIT(t->vertex ? "break; " : "discard; break; ");
          t->straight_live = false;
        }
      } else {
        EMIT("pc = %uu; ", next >= p->word_count ? PC_FAULT : next);
      }
      break;
    }
    pc = next;
  }
  EMIT(t->straight ? "\n" : "\n    }\n");
}

/* ---- the whole program -------------------------------------------- */

static const char *target_type(const Wgsl_Program_Desc *d, uint32_t i) {
  if ((d->target_sint_mask >> i) & 1u) return "vec4<i32>";
  if ((d->target_int_mask >> i) & 1u) return "vec4<u32>";
  return "vec4<f32>";
}

/* The vertex -> pixel interface, shared by both stages' modules. Every
 * varying - and 1/w - interpolates linearly in screen space: the values
 * are already divided by w where the program wants perspective (as
 * to_screen does), which with the pass-through vertex stage's w of 1 and
 * with a GPU vertex stage's real w gives the same results. */
static void emit_vout(Out *o, const Wgsl_Program_Desc *d) {
  out_add(o, "struct VOut {\n  @builtin(position) pos: vec4<f32>,\n  @location(0) @interpolate(linear) inv_w: f32,\n");
  for (uint32_t i = 0; i < d->varying_count; i++) {
    if ((d->flat_mask >> i) & 1u) out_add(o, "  @location(%u) @interpolate(flat) v%u: vec4<u32>,\n", i + 1u, i);
    else out_add(o, "  @location(%u) @interpolate(linear) v%u: vec4<f32>,\n", i + 1u, i);
  }
  out_add(o, "}\n");
}

static void emit_io(Out *o, const Wgsl_Program_Desc *d, bool depth) {
  out_add(o, "struct VIn {\n  @location(0) p: vec4<f32>,\n");
  for (uint32_t i = 0; i < d->varying_count; i++) out_add(o, "  @location(%u) v%u: vec4<u32>,\n", i + 1u, i);
  out_add(o, "}\n");
  emit_vout(o, d);
  out_add(o, "@vertex fn vs(vin: VIn) -> VOut {\n  var o: VOut;\n  o.pos = vec4<f32>(vin.p.xyz, 1.0);\n"
             "  o.inv_w = vin.p.w;\n");
  for (uint32_t i = 0; i < d->varying_count; i++) {
    if ((d->flat_mask >> i) & 1u) out_add(o, "  o.v%u = vin.v%u;\n", i, i);
    else out_add(o, "  o.v%u = bitcast<vec4<f32>>(vin.v%u);\n", i, i);
  }
  out_add(o, "  return o;\n}\n");
  if (d->target_count || depth) {
    out_add(o, "struct FOut {\n");
    for (uint32_t i = 0; i < d->target_count; i++) out_add(o, "  @location(%u) c%u: %s,\n", i, i, target_type(d, i));
    if (depth) out_add(o, "  @builtin(frag_depth) depth: f32,\n");
    out_add(o, "}\n");
  }
}

/* A GPU vertex stage: the inputs, then (after the translated body) the
 * epilogue that turns the position output into clip space - x, y, z
 * become to_screen's window coordinates and gpu_put_vertex's NDC once
 * divided by w - and the outputs into the pixel program's varyings. */
/* Vertex pulling: the attribute formats of raster3d's fetch_attribute,
 * decoded from the copied stream bytes (WGSL_DRAW_VS_INPUTS). */
_Static_assert(WGSL_VSI_RESIDENT == 0x20000000u, "k_vertex_pull tests the resident flag as 0x20000000u");
static const char k_vertex_pull[] =
    "fn vword(b: u32, r: bool) -> u32 {\n"
    "  let s = (b & 3u) * 8u; let i = b >> 2u;\n"
    "  var lo = 0u; var hi = 0u;\n"
    "  if (r) { if (i < arrayLength(&R)) { lo = R[i]; } if (i + 1u < arrayLength(&R)) { hi = R[i + 1u]; } }\n"
    "  else { lo = D[i]; hi = D[i + 1u]; }\n"
    "  if (s == 0u) { return lo; }\n"
    "  return (lo >> s) | (hi << (32u - s));\n"
    "}\n"
    "fn vconv(v: u32, n: u32, ty: u32) -> u32 {\n"
    "  let mx = select((1u << n) - 1u, 0xffffffffu, n >= 32u);\n"
    "  let sv = select(i32(v << (32u - n)) >> (32u - n), i32(v), n >= 32u);\n"
    "  switch (ty) {\n"
    "    case 1u: { return bitcast<u32>(max(f32(sv) / f32(mx >> 1u), -1.0)); }\n"  /* SNORM */
    "    case 2u: { return bitcast<u32>(f32(v) / f32(mx)); }\n"                    /* UNORM */
    "    case 3u: { return u32(sv); }\n"                                           /* SINT */
    "    case 4u: { return v; }\n"                                                 /* UINT */
    "    case 5u: { return bitcast<u32>(f32(v)); }\n"                              /* USCALED */
    "    case 6u: { return bitcast<u32>(f32(sv)); }\n"                             /* SSCALED */
    "    default: {\n"                                                             /* FLOAT */
    "      if (n == 32u) { return v; }\n"
    "      if (n == 16u) { return bitcast<u32>(unpack2x16float(v).x); }\n"
    "      if (n == 11u) { return bitcast<u32>(unpack2x16float(v << 4u).x); }\n"
    "      if (n == 10u) { return bitcast<u32>(unpack2x16float(v << 5u).x); }\n"
    "      return 0u;\n"
    "    }\n"
    "  }\n"
    "}\n"
    "fn vfetch(at: u32, vid: u32) -> vec4<u32> {\n"
    "  let st = D[at + 1u]; let a = D[at + 2u];\n"
    "  let ty = (a >> 27u) & 7u;\n"
    "  var o = vec4<u32>(0u, 0u, 0u, select(0x3f800000u, 1u, ty == 3u || ty == 4u));\n"
    "  if ((st & 0x40000000u) == 0u) { return o; }\n"
    "  let size = (a >> 21u) & 0x3fu;\n"
    "  var cnt = 0u; var bits = vec4<u32>(0u);\n"
    "  switch (size) {\n"
    "    case 0x01u: { cnt = 4u; bits = vec4<u32>(32u); }\n"
    "    case 0x02u: { cnt = 3u; bits = vec4<u32>(32u); }\n"
    "    case 0x03u: { cnt = 4u; bits = vec4<u32>(16u); }\n"
    "    case 0x04u: { cnt = 2u; bits = vec4<u32>(32u); }\n"
    "    case 0x05u: { cnt = 3u; bits = vec4<u32>(16u); }\n"
    "    case 0x0Au, 0x2Fu: { cnt = 4u; bits = vec4<u32>(8u); }\n"
    "    case 0x0Fu: { cnt = 2u; bits = vec4<u32>(16u); }\n"
    "    case 0x12u: { cnt = 1u; bits = vec4<u32>(32u); }\n"
    "    case 0x13u, 0x33u: { cnt = 3u; bits = vec4<u32>(8u); }\n"
    "    case 0x18u, 0x32u: { cnt = 2u; bits = vec4<u32>(8u); }\n"
    "    case 0x1Bu: { cnt = 1u; bits = vec4<u32>(16u); }\n"
    "    case 0x1Du, 0x34u: { cnt = 1u; bits = vec4<u32>(8u); }\n"
    "    case 0x30u: { cnt = 4u; bits = vec4<u32>(10u, 10u, 10u, 2u); }\n"
    "    case 0x31u: { cnt = 3u; bits = vec4<u32>(11u, 11u, 10u, 0u); }\n"
    "    default: { return o; }\n"
    "  }\n"
    "  let elem = select(vid - D[at + 3u], 0u, (st & 0x80000000u) != 0u);\n"
    "  let b = D[at] + elem * (st & 0xfffu) + ((a >> 7u) & 0x3fffu);\n"
    "  let res = (st & 0x20000000u) != 0u;\n" /* WGSL_VSI_RESIDENT */
    "  var bit = 0u;\n"
    "  for (var c = 0u; c < cnt; c = c + 1u) {\n"
    "    let n = bits[c];\n"
    "    let w = vword(b + (bit >> 3u), res) >> (bit & 7u);\n"
    "    o[c] = vconv(select(w & ((1u << n) - 1u), w, n >= 32u), n, ty);\n"
    "    bit = bit + n;\n"
    "  }\n"
    "  if (size == 0x34u) { o.w = o.x; o.x = 0u; }\n"
    "  if ((a >> 31u) != 0u) { let t = o.x; o.x = o.z; o.z = t; }\n"
    "  return o;\n"
    "}\n";

/* ---- global memory (vertex programs) ------------------------------- */

#define IADD_CBUF_X_BIT 43u  /* IADD (constant buffer): carry in */
#define IADD_CBUF_CC_BIT 47u /* IADD (constant buffer): carry out */
#define LDG_WIDE_BIT 45u     /* a 64-bit address (Ra, Ra + 1) */
#define GLOBAL_DEF_SEARCH 64u

/* The latest instruction before `pc` writing register r, or -1 when it is
 * not certain: a control transfer or a branch target lies in between (another
 * path may join), the write is predicated, or a load that may write several
 * registers covers r. Flow-stack pushes (SSY, PBK, PCNT) change nothing. */
static int32_t last_def(const Sm_Program *p, const bool *targeted, uint32_t pc, uint32_t r) {
  for (uint32_t back = 1; back <= GLOBAL_DEF_SEARCH && back <= pc; back++) {
    const uint32_t at = pc - back;
    const Sm_Insn *in = &p->insns[at];
    if (targeted[at + 1u]) return -1;
    if (in->op == SM_OP_SCHED || in->op == SM_OP_NOP || in->op == SM_OP_SSY || in->op == SM_OP_PBK ||
        in->op == SM_OP_PCNT)
      continue;
    if (is_flow(in->op)) return -1;
    const uint32_t d = REG_D(in->raw);
    uint32_t span = 1;
    if (in->op == SM_OP_LDG || in->op == SM_OP_LDC || in->op == SM_OP_LDL || in->op == SM_OP_LD)
      span = (access_bytes(BITS(in->raw, 48, 3)) + 3u) / 4u;
    else if (in->op == SM_OP_ALD)
      span = BITS(in->raw, 47, 2) + 1u;
    else if (is_texture_op(in->op))
      span = 4;
    if (span > 1u && r > d && r < d + span) return -1; /* written by a multi-register load */
    if (d != r) continue;
    return (in->pred & 7u) == SM_PT && !(in->pred & 8u) ? (int32_t)at : -1;
  }
  return -1;
}

bool wgsl_reads_globals(const Sm_Program *program) {
  for (uint32_t pc = 0; pc < program->word_count; pc++)
    if (program->insns[pc].op == SM_OP_LDG || program->insns[pc].op == SM_OP_STG ||
        program->insns[pc].op == SM_OP_LD || program->insns[pc].op == SM_OP_ST)
      return true;
  return false;
}

bool wgsl_find_globals(const Sm_Program *program, Wgsl_Globals *out) {
  memset(out, 0, sizeof(*out));
  memset(out->buffer_of, 0xff, sizeof(out->buffer_of));
  static _Thread_local bool targeted[SM_MAX_WORDS + 1u];
  memset(targeted, 0, sizeof(targeted));
  for (uint32_t pc = 0; pc < program->word_count; pc++) {
    const int32_t target = program->insns[pc].target;
    if (target >= 0 && (uint32_t)target < SM_MAX_WORDS) targeted[target] = true;
  }
  for (uint32_t pc = 0; pc < program->word_count; pc++) {
    const Sm_Insn *in = &program->insns[pc];
    if (in->op == SM_OP_STG || in->op == SM_OP_LD || in->op == SM_OP_ST) return false;
    if (in->op != SM_OP_LDG) continue;
    const uint64_t w = in->raw;
    const uint32_t ra = REG_A(w);
    if (!BIT(w, LDG_WIDE_BIT) || ra == SM_RZ) return false;
    const int32_t lo_at = last_def(program, targeted, pc, ra), hi_at = last_def(program, targeted, pc, (ra + 1u) & 0xffu);
    if (lo_at < 0 || hi_at < 0) return false;
    const Sm_Insn *lo = &program->insns[lo_at], *hi = &program->insns[hi_at];
    if (lo->op != SM_OP_IADD || hi->op != SM_OP_IADD || lo->form != SM_FORM_CBUF || hi->form != SM_FORM_CBUF) return false;
    if (!BIT(lo->raw, IADD_CBUF_CC_BIT) || BIT(lo->raw, IADD_CBUF_X_BIT) || !BIT(hi->raw, IADD_CBUF_X_BIT) ||
        REG_A(hi->raw) != SM_RZ || BIT(lo->raw, 48) || BIT(lo->raw, 49) || BIT(hi->raw, 48) || BIT(hi->raw, 49))
      return false; /* no negations */
    if (hi->cbuf != lo->cbuf || hi->imm != lo->imm + 4u || lo->imm > 0xfff0u) return false;
    uint32_t k = 0;
    while (k < out->count && !(out->slot[k] == lo->cbuf && out->offset[k] == lo->imm)) k++;
    if (k == out->count) {
      if (k == WGSL_MAX_GLOBALS) return false;
      out->slot[k] = (uint8_t)lo->cbuf;
      out->offset[k] = (uint16_t)lo->imm;
      out->count++;
    }
    out->buffer_of[pc] = (uint8_t)k;
  }
  return true;
}

/* LDG from buffer k: the 64-bit address relative to its base, in range,
 * read from its copy in D; zeros outside (wgsl_find_globals). */
static void emit_ldg(Tr *t, const Sm_Insn *in, uint32_t pc) {
  const uint64_t w = in->raw;
  const uint32_t k = t->globals.buffer_of[pc];
  if (!t->vertex || k == 0xffu) {
    fail(t, "global memory (LDG)");
    return;
  }
  t->uses_globals = true;
  const uint32_t size = BITS(w, 48, 3), n = access_bytes(size), gb = WGSL_DRAW_GLOBALS + WGSL_GLOBAL_WORDS * k;
  const int32_t off = (int32_t)(BITS(w, 20, 24) << 8) >> 8;
  const uint32_t ra = REG_A(w), dr = REG_D(w);
  EMIT("{ let a0 = %s; let ga = a0 + 0x%xu; let gh = %s %s; ", reg(t, ra).s, (uint32_t)off, reg(t, (ra + 1u) & 0xffu).s,
       off >= 0 ? "+ select(0u, 1u, ga < a0)" : "- select(0u, 1u, ga > a0)");
  EMIT("let rel = ga - D[%uu]; let rh = gh - D[%uu] - select(0u, 1u, ga < D[%uu]); ", gb, gb + 1u, gb);
  EMIT("let gok = rh == 0u && rel <= D[%uu] && D[%uu] - rel >= %uu; let gat = D[%uu] + rel; ", gb + 2u, gb + 2u, n,
       gb + 3u);
  if (n < 4u) {
    if (dr != SM_RZ) {
      const Ex d = reg_dst(t, dr);
      EMIT("%s = select(0u, gword(gat) & 0x%xu, gok); ", d.s, n == 1u ? 0xffu : 0xffffu);
      sign_extend_small(t, size, &d);
    }
  } else {
    for (uint32_t i = 0; i < n / 4u; i++) {
      if (dr == SM_RZ || ((dr + i) & 0xffu) == SM_RZ) continue;
      EMIT("%s = select(0u, gword(gat + %uu), gok); ", reg_dst(t, dr + i).s, 4u * i);
    }
  }
  EMIT("} ");
}

static const char k_gword[] =
    "fn gword(b: u32) -> u32 {\n"
    "  let s = (b & 3u) * 8u; let lo = D[b >> 2u];\n"
    "  if (s == 0u) { return lo; }\n"
    "  return (lo >> s) | (D[(b >> 2u) + 1u] << (32u - s));\n"
    "}\n";

const char *wgsl_vertex_pull_source(void) { return k_vertex_pull; }

static void emit_vertex_io(Out *o, const Wgsl_Program_Desc *d) {
  if (d->vertex_pull) {
    out_add(o, "@group(0) @binding(%u) var<storage, read> R: array<u32>;\n", WGSL_VS_RESIDENT_BINDING);
    out_add(o, "%s", k_vertex_pull);
    out_add(o, "struct VIn {\n  @builtin(vertex_index) vi: u32,\n}\n");
  } else {
    out_add(o, "struct VIn {\n  @location(%u) ids: vec4<u32>,\n", WGSL_VERTEX_ID_LOCATION);
    for (uint32_t i = 0; i < d->input_count; i++) out_add(o, "  @location(%u) a%u: vec4<u32>,\n", i + 1u, i);
    out_add(o, "}\n");
  }
  emit_vout(o, d);
}

static void emit_vertex_epilogue(Out *o, const Wgsl_Program_Desc *d) {
  const uint32_t vp = WGSL_DRAW_VIEWPORT, pw = SM_ATTR_POSITION / 4u;
  out_add(o, "  var o: VOut;\n");
  out_add(o, "  let x = F(aout[%uu]); let y = F(aout[%uu]); let z = F(aout[%uu]); let w = F(aout[%uu]);\n", pw,
          pw + 1u, pw + 2u, pw + 3u);
  /* Window coordinates times w (no division until the GPU's). */
  out_add(o, "  var xw = x; var yw = y; var zw = z;\n");
  out_add(o, "  if ((D[%uu] & %uu) != 0u) { xw = x * F(D[%uu]) + F(D[%uu]) * w; yw = y * F(D[%uu]) + F(D[%uu]) * w; "
             "zw = z * F(D[%uu]) + F(D[%uu]) * w; }\n",
          vp + WGSL_VP_FLAGS, WGSL_VP_TRANSFORM, vp + WGSL_VP_SCALE, vp + WGSL_VP_OFFSET, vp + WGSL_VP_SCALE + 1u,
          vp + WGSL_VP_OFFSET + 1u, vp + WGSL_VP_SCALE + 2u, vp + WGSL_VP_OFFSET + 2u);
  out_add(o, "  if ((D[%uu] & %uu) != 0u) { yw = F(D[%uu]) * w - yw; }\n", WGSL_DRAW_FLAGS, WGSL_DRAW_LOWER_LEFT,
          WGSL_DRAW_SURFACE_HEIGHT);
  out_add(o, "  o.pos = vec4<f32>(xw / F(D[%uu]) * 2.0 - w, w - yw / F(D[%uu]) * 2.0, zw, w);\n",
          vp + WGSL_VP_TARGET, vp + WGSL_VP_TARGET + 1u);
  out_add(o, "  let iw = 1.0 / w;\n  o.inv_w = iw;\n");
  for (uint32_t l = 0; l < d->varying_count; l++) {
    const bool flat = (d->flat_mask >> l) & 1u;
    char comp[4][48];
    for (uint32_t c = 0; c < 4u; c++) {
      const uint16_t word = d->output_word[l][c];
      const bool persp = (d->perspective_mask >> (l * 4u + c)) & 1u;
      if (word >= SM_ATTRIBUTE_WORDS) snprintf(comp[c], sizeof(comp[c]), flat ? "0u" : "0.0");
      else if (flat) snprintf(comp[c], sizeof(comp[c]), "aout[%uu]", word);
      else snprintf(comp[c], sizeof(comp[c]), persp ? "F(aout[%uu]) * iw" : "F(aout[%uu])", word);
    }
    out_add(o, "  o.v%u = vec4<%s>(%s, %s, %s, %s);\n", l, flat ? "u32" : "f32", comp[0], comp[1], comp[2], comp[3]);
  }
  out_add(o, "  return o;\n");
}

/* "fn quad_read": quad lane k's value of `v` (SHFL in a quad). */
static const char k_quad[] =
    /* Bit-exact neighbour reads: each 16-bit half is an exact small float,
     * so its derivative is the exact difference to the neighbour. */
    "fn quad_x_(v: u32, ql: u32) -> u32 {\n"
    "  let lo = f32(v & 0xffffu); let hi = f32(v >> 16u); let s = select(1.0, -1.0, (ql & 1u) != 0u);\n"
    "  return u32(lo + s * dpdxFine(lo)) | (u32(hi + s * dpdxFine(hi)) << 16u);\n"
    "}\n"
    "fn quad_y_(v: u32, ql: u32) -> u32 {\n"
    "  let lo = f32(v & 0xffffu); let hi = f32(v >> 16u); let s = select(1.0, -1.0, (ql & 2u) != 0u);\n"
    "  return u32(lo + s * dpdyFine(lo)) | (u32(hi + s * dpdyFine(hi)) << 16u);\n"
    "}\n"
    /* Quad lane k's value of v, read from lane ql. */
    "fn quad_read_(v: u32, k: u32, ql: u32) -> u32 {\n"
    "  let x = quad_x_(v, ql); let y = quad_y_(v, ql); let xy = quad_y_(x, ql); let d = k ^ ql;\n"
    "  return select(select(v, x, d == 1u), select(y, xy, d == 3u), d >= 2u);\n"
    "}\n";

static void emit_outputs(Tr *t, Out *o) {
  const Sm_Header *h = &t->p->header;
  uint8_t color_reg[WGSL_MAX_TARGETS][4];
  uint32_t r = 0;
  memset(color_reg, 0xff, sizeof(color_reg));
  for (uint32_t tg = 0; tg < WGSL_MAX_TARGETS; tg++)
    for (uint32_t c = 0; c < 4; c++)
      if ((h->omap_target >> (4u * tg + c)) & 1u) color_reg[tg][c] = (uint8_t)r++;
  const uint32_t depth_reg = r;
  /* Alpha test against target 0's alpha (output_pixel). */
  const uint8_t ar = color_reg[0][3];
  out_add(o, "  if (D[%uu] != 0u && !tcmp(D[%uu] - 1u, %s, F(D[%uu]))) { discard; }\n", WGSL_DRAW_ALPHA_FUNC,
          WGSL_DRAW_ALPHA_FUNC, ar == 0xffu ? "1.0" : ex("F(%s)", reg(t, ar).s).s, WGSL_DRAW_ALPHA_REF);
  if (!t->d->target_count && !h->omap_depth) return;
  out_add(o, "  var fo: FOut;\n");
  for (uint32_t tg = 0; tg < t->d->target_count && tg < WGSL_MAX_TARGETS; tg++) {
    const uint32_t src = t->d->mrt ? tg : 0u;
    Ex comp[4];
    for (uint32_t c = 0; c < 4; c++) {
      const uint8_t cr = color_reg[src][c];
      comp[c] = cr == 0xffu ? ex(c == 3u ? "0x3f800000u" : "0u") : reg(t, cr);
    }
    out_add(o, "  fo.c%u = bitcast<%s>(vec4<u32>(%s, %s, %s, %s));\n", tg, target_type(t->d, tg), comp[0].s, comp[1].s,
            comp[2].s, comp[3].s);
  }
  if (h->omap_depth) out_add(o, "  fo.depth = F(%s);\n", reg(t, depth_reg).s);
  out_add(o, "  return fo;\n");
}

/* A compute program's declarations and entry point: the global-memory
 * windows and their lookup, shared memory, the barrier bookkeeping. */
static void emit_compute_head(const Tr *t, Out *o) {
  const Wgsl_Program_Desc *d = t->d;
  for (uint32_t k = 0; k < d->window_count; k++)
    out_add(o, "@group(0) @binding(%u) var<storage, read_write> G%u: array<u32>;\n", WGSL_CS_WINDOW_BINDING_BASE + k, k);
  if (t->uses_globals) {
    /* (window, byte offset) of an n-byte access at hi:lo, or window ~0. */
    out_add(o, "fn gfind(lo: u32, hi: u32, n: u32) -> vec2<u32> {\n");
    for (uint32_t k = 0; k < d->window_count; k++) {
      const uint32_t b = WGSL_CS_WINDOWS + WGSL_CS_WINDOW_WORDS * k;
      out_add(o, "  { let rel = lo - D[%uu]; let rh = hi - D[%uu] - select(0u, 1u, lo < D[%uu]);\n"
                 "    if (rh == 0u && rel <= D[%uu] && D[%uu] - rel >= n) { return vec2<u32>(%uu, rel); } }\n",
              b, b + 1u, b, b + 2u, b + 2u, k);
    }
    out_add(o, "  return vec2<u32>(0xffffffffu, 0u);\n}\n");
    out_add(o, "fn gld(a: vec2<u32>) -> u32 {\n  let i = a.y >> 2u;\n  switch (a.x) {\n");
    for (uint32_t k = 0; k < d->window_count; k++)
      out_add(o, "    case %uu: { if (i < arrayLength(&G%u)) { return G%u[i]; } }\n", k, k, k);
    out_add(o, "    default: {}\n  }\n  return 0u;\n}\n");
    out_add(o, "fn gst(a: vec2<u32>, v: u32) {\n  let i = a.y >> 2u;\n  switch (a.x) {\n");
    for (uint32_t k = 0; k < d->window_count; k++)
      out_add(o, "    case %uu: { if (i < arrayLength(&G%u)) { G%u[i] = v; } }\n", k, k, k);
    out_add(o, "    default: {}\n  }\n}\n");
  }
  if (t->uses_shared) out_add(o, "var<workgroup> sh: array<u32, %u>;\n", d->shared_bytes / 4u);
  out_add(o, "var<workgroup> cs_wait: atomic<u32>;\nvar<workgroup> cs_count: u32;\n");
  out_add(o, "@compute @workgroup_size(%u, %u, %u)\nfn cs(@builtin(local_invocation_id) lid: vec3<u32>, "
             "@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_index) lidx: u32) {\n",
          d->block[0], d->block[1], d->block[2]);
}

Wgsl_Result wgsl_translate(const Sm_Program *program, const Wgsl_Program_Desc *desc, char *buffer, size_t capacity) {
  Wgsl_Result res;
  memset(&res, 0, sizeof(res));
  res.text = buffer;
  if (capacity) buffer[0] = '\0';
  static _Thread_local Tr tr;
  Tr *t = &tr;
  memset(t, 0, sizeof(*t));
  t->p = program;
  t->d = desc;
  t->ok = true;
  t->vertex = desc->stage == SM_STAGE_VERTEX;
  t->compute = desc->stage == SM_STAGE_COMPUTE;
  t->cbt = t->vertex ? WGSL_DRAW_VS_CBUF_TABLE : WGSL_DRAW_CBUF_TABLE;
  memset(t->globals.buffer_of, 0xff, sizeof(t->globals.buffer_of));
  if (t->vertex && wgsl_reads_globals(program) && !wgsl_find_globals(program, &t->globals))
    memset(t->globals.buffer_of, 0xff, sizeof(t->globals.buffer_of)); /* its LDGs stay untranslated */
  const uint8_t want_stage = t->compute ? SM_STAGE_COMPUTE : t->vertex ? SM_STAGE_VERTEX : SM_STAGE_PIXEL;
  if (program->header.stage != want_stage)
    fail(t, t->compute ? "not a compute program" : t->vertex ? "not a vertex program" : "not a pixel program");
  /* A compute program: lanes are workgroup invocations - no quads, no
   * attributes, no textures yet; warp votes and shuffles have no WGSL
   * equivalent without subgroups. */
  if (t->compute) {
    if (!desc->block[0] || !desc->block[1] || !desc->block[2] ||
        (uint32_t)desc->block[0] * desc->block[1] * desc->block[2] > WGSL_CS_MAX_INVOCATIONS)
      fail(t, "workgroup %ux%ux%u", desc->block[0], desc->block[1], desc->block[2]);
    if (desc->window_count > WGSL_CS_MAX_WINDOWS) fail(t, "%u windows", desc->window_count);
    if (desc->shared_bytes > WGSL_CS_MAX_SHARED_BYTES) fail(t, "%u bytes of shared memory", desc->shared_bytes);
    for (uint32_t pc = 0; pc < program->word_count && t->ok; pc++) {
      const uint16_t op = program->insns[pc].op;
      if (op == SM_OP_KIL || op == SM_OP_IPA || op == SM_OP_SHFL || op == SM_OP_FSWZADD || op == SM_OP_VOTE ||
          op == SM_OP_ALD || op == SM_OP_AST || op == SM_OP_OUT || op == SM_OP_LD || op == SM_OP_ST ||
          is_texture_op(op))
        fail(t, "%s in a compute program", sm_op_name((Sm_Op)op));
    }
  }
  if (t->vertex && desc->input_count + 1u > (desc->vertex_pull ? WGSL_VSI_MAX : 16u))
    fail(t, "%u vertex inputs", desc->input_count);
  /* A vertex stage has no quads (derivatives, discard) or pixel inputs;
   * its texture reads take level 0 (TEX.B and TMML stay on the CPU path:
   * a bindless handle needs a shaded probe, and TMML derivatives). */
  for (uint32_t pc = 0; t->vertex && pc < program->word_count && t->ok; pc++) {
    const uint16_t op = program->insns[pc].op;
    if (op == SM_OP_KIL || op == SM_OP_IPA || op == SM_OP_SHFL || op == SM_OP_FSWZADD || op == SM_OP_TEX_B ||
        op == SM_OP_TMML)
      fail(t, "%s in a vertex program", sm_op_name((Sm_Op)op));
  }
  if (desc->varying_count > WGSL_MAX_VARYINGS) fail(t, "%u varyings", desc->varying_count);
  if (desc->target_count > WGSL_MAX_TARGETS) fail(t, "%u targets", desc->target_count);
  if (capacity < 4096u) fail(t, "buffer too small");
  if (!t->ok) {
    res.ok = false;
    memcpy(res.reason, t->reason, sizeof(res.reason));
    return res;
  }
  /* The body is built in the back half of the buffer, then the header
   * (declarations, which depend on what the body used) in the front, and
   * the body is moved up behind it. */
  const size_t half = capacity / 2u;
  t->body.buf = buffer + half;
  t->body.cap = capacity - half;
  find_blocks(t);
  t->straight = !t->compute; /* compute: the barrier phases need the dispatch loop */
  for (uint32_t pc = 0; pc < program->word_count; pc++)
    if (t->reached[pc] && is_flow(program->insns[pc].op)) t->straight = false;
  t->straight_live = true;
  t->uniform_flow = t->straight;
  for (uint32_t pc = 0; pc < program->word_count && t->ok && (!t->straight || t->straight_live); pc++)
    if (t->leader[pc] && t->reached[pc]) emit_block(t, pc);
  Out head = {buffer, half, 0, false};
  /* Outputs reference registers too: emit them into a scratch tail first
   * by running them against the header buffer later; they only add to
   * reg_used, so collect that before writing declarations. */
  if (!t->vertex && !t->compute) {
    char scratch[2048];
    Out probe = {scratch, sizeof(scratch), 0, false};
    emit_outputs(t, &probe);
  }
  if (!t->ok) {
    res.ok = false;
    memcpy(res.reason, t->reason, sizeof(res.reason));
    buffer[0] = '\0';
    return res;
  }
  for (size_t i = 0; i < sizeof(k_prelude) / sizeof(k_prelude[0]); i++)
    out_add(&head, k_prelude[i], t->cbt, t->cbt, t->cbt);
  if (!t->vertex && !t->compute) out_add(&head, "%s", k_quad);
  if (t->uses_globals && !t->compute) out_add(&head, "%s", k_gword);
  for (uint32_t i = 0; i < desc->texture_count && i < WGSL_MAX_TEXTURES; i++)
    if ((t->textures_used >> i) & 1u)
      emit_texture_helpers(&head, i, desc->sample_type[i],
                           ((desc->hw_sample_mask >> i) & 1u) && desc->sample_type[i] == WGSL_SAMPLE_FLOAT);
  if (t->compute) {
    emit_compute_head(t, &head);
  } else if (t->vertex) {
    emit_vertex_io(&head, desc);
    if (desc->vertex_pull)
      out_add(&head, "@vertex fn vs(vin: VIn) -> VOut {\n  let vid = vin.vi; let iid = D[%uu];\n", WGSL_DRAW_VS_INSTANCE);
    else
      out_add(&head, "@vertex fn vs(vin: VIn) -> VOut {\n  let vid = vin.ids.x; let iid = vin.ids.y;\n");
    out_add(&head, "  var ain: array<u32, %u>; var aout: array<u32, %u>;\n  aout[%uu] = 0x3f800000u;\n",
            SM_ATTRIBUTE_WORDS, SM_ATTRIBUTE_WORDS, SM_ATTR_POSITION / 4u + 3u);
    for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
      const uint8_t loc = desc->input_location[v];
      if (loc == 0xffu || loc >= desc->input_count) continue;
      if (desc->vertex_pull) {
        out_add(&head, "  { let a = vfetch(%uu, vid);", WGSL_DRAW_VS_INPUTS + WGSL_VSI_WORDS * loc);
        for (uint32_t c = 0; c < 4u; c++) out_add(&head, " ain[%uu] = a[%u];", SM_ATTR_GENERIC / 4u + 4u * v + c, c);
        out_add(&head, " }\n");
      } else {
        for (uint32_t c = 0; c < 4u; c++)
          out_add(&head, "  ain[%uu] = vin.a%u[%u];\n", SM_ATTR_GENERIC / 4u + 4u * v + c, loc, c);
      }
    }
  } else {
    emit_io(&head, desc, program->header.omap_depth);
    const bool has_out = desc->target_count || program->header.omap_depth;
    out_add(&head, "@fragment fn fs(fin: VOut, @builtin(front_facing) ff: bool)%s {\n", has_out ? " -> FOut" : "");
    out_add(&head, "  let ql = (u32(fin.pos.x) & 1u) | ((u32(fin.pos.y) & 1u) << 1u);\n");
    out_add(&head, "  let fy = select(fin.pos.y, F(D[%uu]) - fin.pos.y, (D[%uu] & %uu) != 0u);\n",
            WGSL_DRAW_SURFACE_HEIGHT, WGSL_DRAW_FLAGS, WGSL_DRAW_LOWER_LEFT);
  }
  for (uint32_t r = 0; r < SM_RZ; r++)
    if ((t->reg_used[r / 8u] >> (r % 8u)) & 1u) out_add(&head, "  var r%u: u32;\n", r);
  out_add(&head, "  var _d: u32;\n  var p0: bool; var p1: bool; var p2: bool; var p3: bool; var p4: bool; var p5: bool; "
                 "var p6: bool;\n  var ccz: bool; var ccs: bool; var ccc: bool; var cco: bool;\n");
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++)
    if ((t->cbuf_slots >> s) & 1u)
      out_add(&head, "  let cbB%u = D[%uu]; let cbN%u = D[%uu];\n", s, t->cbt + 2u * s, s, t->cbt + 2u * s + 1u);
  if (t->uses_local) out_add(&head, "  var lm: array<u32, %u>;\n", LOCAL_WORDS);
  if (t->straight) {
    out_add(&head, "  loop {\n");
  } else {
    out_add(&head, "  var fsk: array<u32, %u>; var fst: array<u32, %u>; var fsd = 0u;\n", FLOW_DEPTH, FLOW_DEPTH);
    out_add(&head, "  var cst: array<u32, %u>; var csd = 0u;\n", SM_STACK_DEPTH);
    out_add(&head, "  var pc = %uu; var steps = 0u;\n", norm_pc(1u));
    if (t->compute)
      out_add(&head, "  var bar = false;\n  loop {\n  bar = false; steps = 0u;\n  loop {\n    if (pc >= %uu || bar) { break; }\n", PC_FAULT);
    else if (t->vertex) out_add(&head, "  loop {\n    if (pc >= %uu) { break; }\n", PC_FAULT);
    else out_add(&head, "  loop {\n    if (pc >= %uu) { if (pc != %uu) { discard; } break; }\n", PC_FAULT, PC_DONE);
    out_add(&head, "    steps = steps + 1u; if (steps > %uu) { pc = %uu; continue; }\n", MAX_BLOCK_STEPS, PC_FAULT);
    out_add(&head, "    switch (pc) {\n");
  }
  if (head.overflow || t->body.overflow || head.len + t->body.len + 4096u > capacity) {
    res.ok = false;
    snprintf(res.reason, sizeof(res.reason), "WGSL exceeds %zu bytes", capacity);
    buffer[0] = '\0';
    return res;
  }
  memmove(buffer + head.len, t->body.buf, t->body.len);
  head.len += t->body.len;
  head.cap = capacity;
  if (t->straight) out_add(&head, "%s  }\n", t->straight_live ? "    break;\n" : "");
  else out_add(&head, "    default: { pc = %uu; }\n    }\n  }\n", PC_FAULT);
  if (t->compute) {
    /* Every invocation is at a barrier or done: the workgroup meets (in
     * uniform control flow), and another phase runs while any is waiting. */
    out_add(&head, "  if (bar) { atomicAdd(&cs_wait, 1u); }\n  workgroupBarrier();\n"
                   "  if (lidx == 0u) { cs_count = atomicExchange(&cs_wait, 0u); }\n"
                   "  if (workgroupUniformLoad(&cs_count) == 0u) { break; }\n  }\n");
  } else if (t->vertex) {
    emit_vertex_epilogue(&head, desc);
  } else {
    emit_outputs(t, &head);
  }
  out_add(&head, "}\n");
  if (head.overflow) {
    res.ok = false;
    snprintf(res.reason, sizeof(res.reason), "WGSL exceeds %zu bytes", capacity);
    buffer[0] = '\0';
    return res;
  }
  res.ok = true;
  res.length = head.len;
  res.cbuf_slots = t->cbuf_slots;
  return res;
}

static bool is_texture_op(uint16_t op) {
  switch ((Sm_Op)op) {
  case SM_OP_TEX: case SM_OP_TEX_B: case SM_OP_TEXS: case SM_OP_TLD: case SM_OP_TLDS: case SM_OP_TLD4:
  case SM_OP_TLD4S: case SM_OP_TXQ: case SM_OP_TMML: case SM_OP_TXD:
    return true;
  default:
    return false;
  }
}

void wgsl_default_desc(const Sm_Program *program, Wgsl_Program_Desc *desc) {
  memset(desc, 0, sizeof(*desc));
  memset(desc->binding_of, WGSL_NO_BINDING, sizeof(desc->binding_of));
  memset(desc->varying_location, 0xff, sizeof(desc->varying_location));
  const Sm_Header *h = &program->header;
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT && desc->varying_count < WGSL_MAX_VARYINGS; v++) {
    bool used = false, flat = true;
    for (uint32_t c = 0; c < 4u; c++) {
      const uint8_t mode = h->input_interp[v * 4u + c];
      if (mode == SM_INTERP_UNUSED) continue;
      used = true;
      if (mode != SM_INTERP_CONSTANT) flat = false;
    }
    if (!used) continue;
    if (flat) desc->flat_mask |= 1u << desc->varying_count;
    desc->varying_location[v] = (uint8_t)desc->varying_count++;
  }
  for (uint32_t pc = 0; pc < program->word_count && pc < SM_MAX_WORDS; pc++) {
    if (!is_texture_op(program->insns[pc].op) || desc->texture_count >= WGSL_MAX_TEXTURES) continue;
    desc->binding_of[pc] = (uint8_t)desc->texture_count;
    desc->sample_type[desc->texture_count++] = WGSL_SAMPLE_FLOAT;
  }
  for (uint32_t t = 0; t < WGSL_MAX_TARGETS; t++)
    if ((h->omap_target >> (4u * t)) & 0xfu) desc->target_count = t + 1u;
  desc->mrt = h->mrt_enable;
  desc->stage = h->stage == SM_STAGE_VERTEX ? SM_STAGE_VERTEX : SM_STAGE_PIXEL;
  memset(desc->input_location, 0xff, sizeof(desc->input_location));
  memset(desc->output_word, 0xff, sizeof(desc->output_word));
  if (desc->stage != SM_STAGE_VERTEX) return;
  /* A vertex program alone: every input vector it reads is supplied, and
   * each output vector becomes the next varying, interpolated with
   * perspective (tests and diagnostics; draws use the pixel program's). */
  desc->varying_count = 0;
  desc->flat_mask = 0;
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++) {
    if ((h->input_generic[v / 8u] >> ((v % 8u) * 4u)) & 0xfu) desc->input_location[v] = (uint8_t)desc->input_count++;
    if (!((h->output_generic[v / 8u] >> ((v % 8u) * 4u)) & 0xfu) || desc->varying_count >= WGSL_MAX_VARYINGS) continue;
    const uint32_t l = desc->varying_count++;
    for (uint32_t c = 0; c < 4u; c++) {
      desc->output_word[l][c] = (uint16_t)(SM_ATTR_GENERIC / 4u + 4u * v + c);
      desc->perspective_mask |= 1ull << (l * 4u + c);
    }
  }
}

uint64_t wgsl_desc_hash(const Wgsl_Program_Desc *desc, const Sm_Program *program) {
  uint64_t h = 0xcbf29ce484222325ull;
#define MIX(ptr, n)                                         \
  do {                                                      \
    const uint8_t *b_ = (const uint8_t *)(ptr);             \
    for (size_t i_ = 0; i_ < (size_t)(n); i_++) {           \
      h ^= b_[i_];                                          \
      h *= 0x100000001b3ull;                                \
    }                                                       \
  } while (0)
  MIX(&program->hash, sizeof(program->hash));
  MIX(&program->byte_size, sizeof(program->byte_size));
  MIX(desc->binding_of, program->word_count < SM_MAX_WORDS ? program->word_count : SM_MAX_WORDS);
  MIX(&desc->texture_count, sizeof(desc->texture_count));
  MIX(desc->sample_type, sizeof(desc->sample_type));
  MIX(desc->varying_location, sizeof(desc->varying_location));
  MIX(&desc->varying_count, sizeof(desc->varying_count));
  MIX(&desc->flat_mask, sizeof(desc->flat_mask));
  MIX(&desc->target_count, sizeof(desc->target_count));
  MIX(&desc->target_int_mask, sizeof(desc->target_int_mask));
  MIX(&desc->target_sint_mask, sizeof(desc->target_sint_mask));
  MIX(&desc->mrt, sizeof(desc->mrt));
  MIX(&desc->hw_sample_mask, sizeof(desc->hw_sample_mask));
  MIX(&desc->stage, sizeof(desc->stage));
  if (desc->stage == SM_STAGE_VERTEX) {
    MIX(&desc->vertex_pull, sizeof(desc->vertex_pull));
    MIX(desc->input_location, sizeof(desc->input_location));
    MIX(&desc->input_count, sizeof(desc->input_count));
    MIX(desc->output_word, sizeof(desc->output_word));
    MIX(&desc->perspective_mask, sizeof(desc->perspective_mask));
  }
#undef MIX
  return h;
}
