/**
 * Maxwell shader decode + interpreter (gpu/maxwell_shader): data-driven
 * vectors. Each vector is a hand-assembled program (instruction words
 * built from the documented fields), an initial register state and the
 * expected registers / predicates after the run.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gpu/maxwell_shader.h"

static int g_failures;

#define CHECK(cond, ...)                \
  do {                                  \
    if (!(cond)) {                      \
      fprintf(stderr, "FAIL: ");        \
      fprintf(stderr, __VA_ARGS__);     \
      fprintf(stderr, "\n");            \
      g_failures++;                     \
    }                                   \
  } while (0)

/* ---- assembler ---------------------------------------------------- */

#define PT 7ull
#define RZ 0xffull
#define GUARD (PT << 16)

static uint64_t f_bits(float f) {
  uint32_t v;
  memcpy(&v, &f, sizeof(v));
  return v;
}

static uint64_t op_top(uint64_t top16) { return top16 << 48; }
static uint64_t rd(uint64_t r) { return r; }
static uint64_t ra(uint64_t r) { return r << 8; }
static uint64_t rb(uint64_t r) { return r << 20; }
static uint64_t rc(uint64_t r) { return r << 39; }
static uint64_t cbuf(uint64_t slot, uint64_t offset) { return (slot << 34) | ((offset / 4u) << 20); }
static uint64_t branch(int32_t byte_offset) { return ((uint64_t)(uint32_t)byte_offset & 0xffffffull) << 20; }

static uint64_t MOV32I(uint32_t d, uint32_t imm) { return op_top(0x0100) | GUARD | ((uint64_t)imm << 20) | (0xfull << 12) | rd(d); }
static uint64_t FADD_R(uint32_t d, uint32_t a, uint32_t b) { return op_top(0x5c58) | GUARD | rb(b) | ra(a) | rd(d); }
static uint64_t FMUL_C(uint32_t d, uint32_t a, uint32_t slot, uint32_t off) { return op_top(0x4c68) | GUARD | cbuf(slot, off) | ra(a) | rd(d); }
static uint64_t FFMA_RR(uint32_t d, uint32_t a, uint32_t b, uint32_t c) { return op_top(0x5980) | GUARD | rc(c) | rb(b) | ra(a) | rd(d); }
static uint64_t IADD_R(uint32_t d, uint32_t a, uint32_t b) { return op_top(0x5c10) | GUARD | rb(b) | ra(a) | rd(d); }
static uint64_t IADD32I(uint32_t d, uint32_t a, uint32_t imm) { return op_top(0x1c00) | GUARD | ((uint64_t)imm << 20) | ra(a) | rd(d); }
static uint64_t SHL_I(uint32_t d, uint32_t a, uint32_t s) { return op_top(0x3848) | GUARD | ((uint64_t)s << 20) | ra(a) | rd(d); }
static uint64_t LOP_R(uint32_t d, uint32_t a, uint32_t b, uint32_t op) { return op_top(0x5c40) | GUARD | (PT << 48) | ((uint64_t)op << 41) | rb(b) | ra(a) | rd(d); }
static uint64_t XMAD_R(uint32_t d, uint32_t a, uint32_t b, uint32_t c) { return op_top(0x5b00) | GUARD | rc(c) | rb(b) | ra(a) | rd(d); }
static uint64_t I2F_R(uint32_t d, uint32_t b) { return op_top(0x5cb8) | GUARD | (1ull << 13) | (2ull << 10) | (2ull << 8) | rb(b) | rd(d); }
static uint64_t F2I_R(uint32_t d, uint32_t b) { return op_top(0x5cb0) | GUARD | (3ull << 39) | (1ull << 12) | (2ull << 10) | (2ull << 8) | rb(b) | rd(d); }
static uint64_t MUFU(uint32_t d, uint32_t a, uint32_t fn) { return op_top(0x5080) | GUARD | ((uint64_t)fn << 20) | ra(a) | rd(d); }
static uint64_t ISETP_C(uint32_t pd, uint32_t a, uint32_t slot, uint32_t off, uint32_t cond) {
  return op_top(0x4b60) | GUARD | ((uint64_t)cond << 49) | (PT << 39) | cbuf(slot, off) | ra(a) | ((uint64_t)pd << 3) | PT;
}
static uint64_t FSETP_R(uint32_t pd, uint32_t a, uint32_t b, uint32_t cond) {
  return op_top(0x5bb0) | GUARD | ((uint64_t)cond << 48) | (PT << 39) | rb(b) | ra(a) | ((uint64_t)pd << 3) | PT;
}
static uint64_t SEL_R(uint32_t d, uint32_t a, uint32_t b, uint32_t p) { return op_top(0x5ca0) | GUARD | ((uint64_t)p << 39) | rb(b) | ra(a) | rd(d); }
static uint64_t BRA(int32_t off, uint32_t p, bool negate) {
  return op_top(0xe240) | ((uint64_t)p << 16) | ((uint64_t)negate << 19) | branch(off) | 0xfull;
}
static uint64_t SSY(int32_t off) { return op_top(0xe290) | branch(off); }
static uint64_t SYNC(void) { return op_top(0xf0f8) | GUARD | 0xfull; }
static uint64_t PBK(int32_t off) { return op_top(0xe2a0) | branch(off); }
static uint64_t BRK(uint32_t p, bool negate) { return op_top(0xe340) | ((uint64_t)p << 16) | ((uint64_t)negate << 19) | 0xfull; }
static uint64_t EXIT(void) { return op_top(0xe300) | GUARD | 0xfull; }
static uint64_t KIL(void) { return op_top(0xe330) | GUARD | 0xfull; }
static uint64_t ALD(uint32_t d, uint32_t addr, uint32_t n) { return op_top(0xefd8) | GUARD | ((uint64_t)(n - 1u) << 47) | rc(RZ) | ((uint64_t)addr << 20) | ra(RZ) | rd(d); }
static uint64_t AST(uint32_t s, uint32_t addr, uint32_t n) { return op_top(0xeff0) | GUARD | ((uint64_t)(n - 1u) << 47) | rc(RZ) | ((uint64_t)addr << 20) | ra(RZ) | rd(s); }
static uint64_t IPA(uint32_t d, uint32_t addr, uint32_t mode, uint32_t mul) {
  return op_top(0xe000) | GUARD | ((uint64_t)mode << 54) | (PT << 47) | ((uint64_t)addr << 28) | rc(RZ) | rb(mul) | ra(RZ) | rd(d);
}
static uint64_t TEXS(uint32_t d0, uint32_t d1, uint32_t a, uint32_t b, uint32_t target, uint32_t mask, uint32_t tex) {
  return op_top(0xd800) | GUARD | ((uint64_t)target << 53) | ((uint64_t)mask << 50) | ((uint64_t)tex << 36) |
         ((uint64_t)d1 << 28) | rb(b) | ra(a) | rd(d0);
}
static uint64_t LDC(uint32_t d, uint32_t a, uint32_t slot, uint32_t off) {
  return op_top(0xef90) | GUARD | (4ull << 48) | ((uint64_t)slot << 36) | ((uint64_t)off << 20) | ra(a) | rd(d);
}

/* Program builder: inserts a scheduling word before every three
 * instructions. Branch offsets are relative to the next instruction, in
 * bytes, and must count scheduling words. */
typedef struct Builder {
  uint8_t bytes[SM_SPH_BYTES + 8u * 256u];
  uint32_t words;
} Builder;

static void begin(Builder *b, uint32_t stage) {
  memset(b, 0, sizeof(*b));
  const uint32_t sph0 = (stage << 10) | (3u << 5) | 1u;
  memcpy(b->bytes, &sph0, 4);
}

static void emit(Builder *b, uint64_t insn) {
  if (b->words % 4u == 0) {
    const uint64_t sched = 0x001f8000fc0007e0ull;
    memcpy(b->bytes + SM_SPH_BYTES + 8u * b->words, &sched, 8);
    b->words++;
  }
  memcpy(b->bytes + SM_SPH_BYTES + 8u * b->words, &insn, 8);
  b->words++;
}

/* Word index the next emitted instruction will get. */
static uint32_t next_index(const Builder *b) { return b->words % 4u == 0 ? b->words + 1u : b->words; }

static int32_t offset_to(uint32_t from_index, uint32_t to_index) { return ((int32_t)to_index - (int32_t)from_index - 1) * 8; }

static Sm_Program g_prog;
static Sm_Thread g_thread;

static void load(const Builder *b) { sm_program_decode(b->bytes, SM_SPH_BYTES + 8u * b->words, 0, &g_prog); }

static float as_f(uint32_t v) {
  float f;
  memcpy(&f, &v, sizeof(f));
  return f;
}

/* ---- environment -------------------------------------------------- */

static uint8_t g_cbuf[2][256];
static uint32_t g_last_handle;
static float g_last_coords[2];

static void fake_texture(void *user, const Sm_Tex_Request *req, uint32_t out[4]) {
  (void)user;
  g_last_handle = req->handle;
  g_last_coords[0] = req->coords[0];
  g_last_coords[1] = req->coords[1];
  /* texel = (u, v, u+v, 0.5) */
  out[0] = (uint32_t)f_bits(req->coords[0]);
  out[1] = (uint32_t)f_bits(req->coords[1]);
  out[2] = (uint32_t)f_bits(req->coords[0] + req->coords[1]);
  out[3] = (uint32_t)f_bits(0.5f);
}

static Sm_Env make_env(void) {
  Sm_Env env;
  memset(&env, 0, sizeof(env));
  env.cbuf[0] = g_cbuf[0];
  env.cbuf_size[0] = sizeof(g_cbuf[0]);
  env.cbuf[2] = g_cbuf[1];
  env.cbuf_size[2] = sizeof(g_cbuf[1]);
  env.texture_cbuf_slot = 0;
  env.texture = fake_texture;
  return env;
}

static void put_cb(uint32_t which, uint32_t offset, uint32_t value) { memcpy(g_cbuf[which] + offset, &value, 4); }

static bool run(void) {
  const Sm_Env env = make_env();
  return sm_run(&g_prog, &env, &g_thread);
}

/* ---- vectors ------------------------------------------------------ */

static void test_float_arith(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, MOV32I(0, (uint32_t)f_bits(1.5f)));
  emit(&b, MOV32I(1, (uint32_t)f_bits(2.25f)));
  emit(&b, FADD_R(2, 0, 1));                 /* 3.75 */
  emit(&b, FMUL_C(3, 2, 2, 8));              /* * c[2][8] = 2 -> 7.5 */
  emit(&b, FFMA_RR(4, 0, 1, 3));             /* 1.5*2.25 + 7.5 = 10.875 */
  emit(&b, MUFU(5, 1, 4));                   /* rcp(2.25) */
  emit(&b, EXIT());
  load(&b);
  put_cb(1, 8, (uint32_t)f_bits(2.0f));
  sm_thread_reset(&g_thread, 1);
  CHECK(run(), "float program runs");
  CHECK(as_f(g_thread.r[2][0]) == 3.75f, "FADD %f", as_f(g_thread.r[2][0]));
  CHECK(as_f(g_thread.r[3][0]) == 7.5f, "FMUL c[2][8] %f", as_f(g_thread.r[3][0]));
  CHECK(as_f(g_thread.r[4][0]) == 10.875f, "FFMA %f", as_f(g_thread.r[4][0]));
  CHECK(fabsf(as_f(g_thread.r[5][0]) - 1.0f / 2.25f) < 1e-6f, "MUFU.RCP %f", as_f(g_thread.r[5][0]));
  CHECK(g_prog.cbuf_used == (1u << 2) && g_prog.cbuf_extent[2] == 12u, "cbuf usage recorded (%x, %u)", g_prog.cbuf_used,
        g_prog.cbuf_extent[2]);
}

static void test_integer_arith(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, MOV32I(0, 0x12345678u));
  emit(&b, MOV32I(1, 0x00010001u));
  emit(&b, IADD_R(2, 0, 1));                 /* 0x12355679 */
  emit(&b, IADD32I(3, 0, 0xffffffffu));      /* -1 */
  emit(&b, SHL_I(4, 1, 4));                  /* 0x00100010 */
  emit(&b, LOP_R(5, 0, 1, 2));               /* xor */
  emit(&b, MOV32I(6, 7u));
  emit(&b, MOV32I(7, 0x00030000u));
  emit(&b, XMAD_R(8, 6, 7, 6));              /* 7 * 0 (lo half of b) + 7 = 7 */
  emit(&b, I2F_R(9, 6));                     /* 7.0 */
  emit(&b, MOV32I(10, (uint32_t)f_bits(-2.75f)));
  emit(&b, F2I_R(11, 10));                   /* trunc -> -2 */
  emit(&b, EXIT());
  load(&b);
  sm_thread_reset(&g_thread, 1);
  CHECK(run(), "integer program runs");
  CHECK(g_thread.r[2][0] == 0x12355679u, "IADD %08x", g_thread.r[2][0]);
  CHECK(g_thread.r[3][0] == 0x12345677u, "IADD32I %08x", g_thread.r[3][0]);
  CHECK(g_thread.r[4][0] == 0x00100010u, "SHL %08x", g_thread.r[4][0]);
  CHECK(g_thread.r[5][0] == (0x12345678u ^ 0x00010001u), "LOP.XOR %08x", g_thread.r[5][0]);
  CHECK(g_thread.r[8][0] == 7u, "XMAD %08x", g_thread.r[8][0]);
  CHECK(as_f(g_thread.r[9][0]) == 7.0f, "I2F %f", as_f(g_thread.r[9][0]));
  CHECK((int32_t)g_thread.r[11][0] == -2, "F2I.TRUNC %d", (int32_t)g_thread.r[11][0]);
}

/* if (c[2][0] != 0) r0 = 1 else r0 = 2, through SSY/BRA/SYNC. */
static void test_branches(void) {
  for (uint32_t pass = 0; pass < 2; pass++) {
    Builder b;
    begin(&b, SM_STAGE_PIXEL);
    const uint32_t ssy_at = next_index(&b);
    emit(&b, 0); /* SSY placeholder, patched below */
    emit(&b, ISETP_C(0, (uint32_t)RZ, 2, 0, 5));     /* P0 = c[2][0] != 0 */
    const uint32_t bra_at = next_index(&b);
    emit(&b, 0); /* @!P0 BRA else */
    emit(&b, MOV32I(0, 1));
    emit(&b, SYNC());
    const uint32_t else_at = next_index(&b);
    emit(&b, MOV32I(0, 2));
    emit(&b, SYNC());
    const uint32_t join_at = next_index(&b);
    emit(&b, EXIT());
    uint64_t w = SSY(offset_to(ssy_at, join_at));
    memcpy(b.bytes + SM_SPH_BYTES + 8u * ssy_at, &w, 8);
    w = BRA(offset_to(bra_at, else_at), 0, true);
    memcpy(b.bytes + SM_SPH_BYTES + 8u * bra_at, &w, 8);
    load(&b);
    put_cb(1, 0, pass);
    sm_thread_reset(&g_thread, 1);
    CHECK(run(), "branch program runs (pass %u)", pass);
    CHECK(g_thread.r[0][0] == (pass ? 1u : 2u), "branch pass %u -> r0=%u", pass, g_thread.r[0][0]);
  }
}

/* for (r0 = 0; ; r0++) if (r0 == 5) break; via PBK/BRK. */
static void test_loop(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, MOV32I(0, 0));
  emit(&b, MOV32I(1, (uint32_t)f_bits(5.0f)));
  const uint32_t pbk_at = next_index(&b);
  emit(&b, 0);
  const uint32_t loop_at = next_index(&b);
  emit(&b, I2F_R(2, 0));
  emit(&b, FSETP_R(0, 2, 1, 2));             /* P0 = float(r0) == 5 */
  emit(&b, BRK(0, false));
  emit(&b, IADD32I(0, 0, 1));
  const uint32_t back_at = next_index(&b);
  emit(&b, BRA(0, (uint32_t)PT, false));
  const uint32_t out_at = next_index(&b);
  emit(&b, EXIT());
  uint64_t w = PBK(offset_to(pbk_at, out_at));
  memcpy(b.bytes + SM_SPH_BYTES + 8u * pbk_at, &w, 8);
  w = BRA(offset_to(back_at, loop_at), (uint32_t)PT, false);
  memcpy(b.bytes + SM_SPH_BYTES + 8u * back_at, &w, 8);
  load(&b);
  sm_thread_reset(&g_thread, 1);
  CHECK(run(), "loop runs");
  CHECK(g_thread.r[0][0] == 5u, "loop counter %u", g_thread.r[0][0]);
}

static void test_attributes_and_select(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, ALD(0, SM_ATTR_GENERIC, 4));
  emit(&b, AST(0, SM_ATTR_POSITION, 4));
  emit(&b, FSETP_R(1, 0, 1, 1));             /* P1 = x < y */
  emit(&b, SEL_R(4, 0, 1, 1));               /* P1 ? x : y  (min) */
  emit(&b, AST(4, SM_ATTR_GENERIC + 16u, 1));
  emit(&b, EXIT());
  load(&b);
  sm_thread_reset(&g_thread, 1);
  const float in[4] = {3.0f, 4.0f, 5.0f, 1.0f};
  for (uint32_t c = 0; c < 4u; c++) memcpy(&g_thread.attr_in[SM_ATTR_GENERIC / 4u + c][0], &in[c], 4);
  for (uint32_t i = 0; i < SM_ATTRIBUTE_WORDS; i++) g_thread.attr_out[i][0] = 0;
  CHECK(run(), "attribute program runs");
  for (uint32_t c = 0; c < 4u; c++) CHECK(as_f(g_thread.attr_out[SM_ATTR_POSITION / 4u + c][0]) == in[c], "ALD/AST copy %u", c);
  CHECK(as_f(g_thread.attr_out[SM_ATTR_GENERIC / 4u + 4u][0]) == 3.0f, "SEL picked min %f",
        as_f(g_thread.attr_out[SM_ATTR_GENERIC / 4u + 4u][0]));
}

static void test_pixel_ops(void) {
  Builder b;
  begin(&b, SM_STAGE_PIXEL);
  emit(&b, IPA(4, 0x7c, 0, (uint32_t)RZ));   /* 1/w */
  emit(&b, MUFU(4, 4, 4));                   /* w */
  emit(&b, IPA(0, SM_ATTR_GENERIC, 1, 4));   /* u/w * w */
  emit(&b, IPA(1, SM_ATTR_GENERIC + 4u, 1, 4));
  emit(&b, TEXS(2, 6, 0, 1, 1, 4, 3));       /* RG -> r2,r3; BA -> r6,r7 (handle at c[0][12]) */
  emit(&b, LDC(8, (uint32_t)RZ, 0, 4));
  emit(&b, EXIT());
  load(&b);
  put_cb(0, 12, 0x00500007u);
  put_cb(0, 4, 0xabcdu);
  sm_thread_reset(&g_thread, 1);
  g_thread.attr_in[0x7c / 4u][0] = (uint32_t)f_bits(0.5f);           /* w = 2 */
  g_thread.attr_in[SM_ATTR_GENERIC / 4u][0] = (uint32_t)f_bits(0.125f); /* u = 0.25 */
  g_thread.attr_in[SM_ATTR_GENERIC / 4u + 1u][0] = (uint32_t)f_bits(0.25f);
  CHECK(run(), "pixel program runs");
  CHECK(as_f(g_thread.r[0][0]) == 0.25f && as_f(g_thread.r[1][0]) == 0.5f, "IPA.MUL perspective (%f, %f)",
        as_f(g_thread.r[0][0]), as_f(g_thread.r[1][0]));
  CHECK(g_last_handle == 0x00500007u, "TEXS handle from the bound-texture buffer: %08x", g_last_handle);
  CHECK(as_f(g_thread.r[2][0]) == 0.25f && as_f(g_thread.r[3][0]) == 0.5f, "TEXS RG");
  CHECK(as_f(g_thread.r[6][0]) == 0.75f && as_f(g_thread.r[7][0]) == 0.5f, "TEXS BA -> second destination");
  CHECK(g_thread.r[8][0] == 0xabcdu, "LDC %x", g_thread.r[8][0]);
}

static void test_kill_and_header(void) {
  Builder b;
  begin(&b, SM_STAGE_PIXEL);
  uint8_t omap = 0x0f;
  memcpy(b.bytes + 0x48, &omap, 1);
  uint8_t interp = 0x0a; /* generic 0: x,y perspective */
  memcpy(b.bytes + 0x18, &interp, 1);
  emit(&b, KIL());
  emit(&b, MOV32I(0, 1));
  emit(&b, EXIT());
  load(&b);
  CHECK(g_prog.header.stage == SM_STAGE_PIXEL, "SPH stage %d", g_prog.header.stage);
  CHECK(g_prog.header.omap_target == 0x0fu, "SPH omap %x", g_prog.header.omap_target);
  CHECK(g_prog.header.input_interp[0] == SM_INTERP_PERSPECTIVE && g_prog.header.input_interp[2] == SM_INTERP_UNUSED,
        "SPH interpolation map");
  sm_thread_reset(&g_thread, 1);
  g_thread.r[0][0] = 9;
  CHECK(run(), "kill program runs");
  CHECK((g_thread.killed & 1u) && g_thread.r[0][0] == 9u, "KIL stops the invocation");
}

static void test_extent(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, EXIT());
  const uint32_t at = next_index(&b);
  emit(&b, BRA(-8, (uint32_t)PT, false)); /* self-branch end marker */
  (void)at;
  emit(&b, 0x50b0000000070f00ull);        /* NOP padding */
  emit(&b, 0x50b0000000070f00ull);
  const uint32_t size = SM_SPH_BYTES + 8u * b.words;
  CHECK(sm_program_extent(b.bytes, size) == SM_SPH_BYTES + 8u * 3u, "extent stops after the end marker (%u)",
        sm_program_extent(b.bytes, size));
}


/* SIMT: 16 lanes run one program whose branches and loop trip counts
 * depend on per-lane inputs; every lane must end with its scalar result:
 * r0 = n (loop count = lane's input), r1 = 1 if n is odd else 2. */
static void test_divergence(void) {
  Builder b;
  begin(&b, SM_STAGE_VERTEX);
  emit(&b, ALD(2, SM_ATTR_GENERIC, 1));      /* r2 = n (integer) */
  emit(&b, MOV32I(0, 0));
  /* loop: if (r0 == r2) break; r0++ */
  const uint32_t pbk_at = next_index(&b);
  emit(&b, 0);
  const uint32_t loop_at = next_index(&b);
  emit(&b, (0x5b60ull << 48) | GUARD | (2ull << 49) | (PT << 39) | rb(2) | ra(0) | (0ull << 3) | PT); /* ISETP.EQ P0 = r0 == r2 */
  emit(&b, BRK(0, false));
  emit(&b, IADD32I(0, 0, 1));
  const uint32_t back_at = next_index(&b);
  emit(&b, 0);
  const uint32_t after_loop = next_index(&b);
  /* if (n & 1) r1 = 1 else r1 = 2, through SSY/SYNC */
  const uint32_t ssy_at = next_index(&b);
  emit(&b, 0);
  emit(&b, (0x3840ull << 48) | GUARD | (PT << 48) | (0ull << 41) | (1ull << 20) | ra(2) | rd(3)); /* LOP.AND r3 = r2 & 1 */
  emit(&b, (0x3660ull << 48) | GUARD | (5ull << 49) | (PT << 39) | (0ull << 20) | ra(3) | (1ull << 3) | PT); /* ISETP.NE P1 = r3 != 0 */
  const uint32_t bra_at = next_index(&b);
  emit(&b, 0);
  emit(&b, MOV32I(1, 1));
  emit(&b, SYNC());
  const uint32_t even_at = next_index(&b);
  emit(&b, MOV32I(1, 2));
  emit(&b, SYNC());
  const uint32_t join_at = next_index(&b);
  emit(&b, EXIT());
  uint64_t w = PBK(offset_to(pbk_at, after_loop));
  memcpy(b.bytes + SM_SPH_BYTES + 8u * pbk_at, &w, 8);
  w = BRA(offset_to(back_at, loop_at), (uint32_t)PT, false);
  memcpy(b.bytes + SM_SPH_BYTES + 8u * back_at, &w, 8);
  w = SSY(offset_to(ssy_at, join_at));
  memcpy(b.bytes + SM_SPH_BYTES + 8u * ssy_at, &w, 8);
  w = BRA(offset_to(bra_at, even_at), 1, true); /* @!P1 BRA even */
  memcpy(b.bytes + SM_SPH_BYTES + 8u * bra_at, &w, 8);
  load(&b);
  sm_thread_reset(&g_thread, SM_LANES);
  for (uint32_t l = 0; l < SM_LANES; l++) g_thread.attr_in[SM_ATTR_GENERIC / 4u][l] = (l * 7u) % 11u;
  CHECK(run(), "divergent program runs");
  for (uint32_t l = 0; l < SM_LANES; l++) {
    const uint32_t n = (l * 7u) % 11u;
    CHECK(g_thread.r[0][l] == n && g_thread.r[1][l] == ((n & 1u) ? 1u : 2u), "lane %u: r0=%u r1=%u (n=%u)", l,
          g_thread.r[0][l], g_thread.r[1][l], n);
  }
}

int main(void) {
  test_float_arith();
  test_integer_arith();
  test_branches();
  test_loop();
  test_attributes_and_select();
  test_pixel_ops();
  test_kill_and_header();
  test_extent();
  test_divergence();
  if (g_failures) {
    fprintf(stderr, "maxwell_shader_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("maxwell_shader_test: ok\n");
  return 0;
}
