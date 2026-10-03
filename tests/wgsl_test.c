/**
 * Maxwell -> WGSL translator (gpu/wgsl): structural checks here, and
 * differential vectors for the browser.
 *
 * Each vector is a hand-assembled pixel program run once through the
 * interpreter (maxwell_shader.c, the reference) with fixed inputs - flat
 * varyings, 1/w, constant buffers, a small RGBA8 texture sampled through
 * texture.c - and its translation. `wgsl_test DIR` writes DIR/<name>.wgsl
 * and DIR/<name>.json (inputs + the interpreter's output registers, or
 * "killed"); platform/web/tools/wgsl-vectors.mjs renders each on WebGPU
 * and compares. Without DIR only the structural checks run (ctest).
 */
#define CHECK_NAME "wgsl_test"
#include "check.h"

#include <stdio.h>
#include <string.h>

#include "gpu/maxwell_shader.h"
#include "gpu/texture.h"
#include "gpu/wgsl.h"

/* ---- assembler (field layouts as tests/maxwell_shader_test.c) ----- */

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
static uint64_t KIL_P(uint32_t p) { return op_top(0xe330) | ((uint64_t)p << 16) | 0xfull; }
static uint64_t ALD(uint32_t d, uint32_t addr, uint32_t n) { return op_top(0xefd8) | GUARD | ((uint64_t)(n - 1u) << 47) | rc(RZ) | ((uint64_t)addr << 20) | ra(RZ) | rd(d); }
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

typedef struct Builder {
  uint8_t bytes[SM_SPH_BYTES + 8u * 256u];
  uint32_t words;
} Builder;

/* A pixel program writing target 0's RGBA from R0-R3; generic vectors
 * 0 and 1 read flat (constant) in all components. */
static void begin(Builder *b) {
  memset(b, 0, sizeof(*b));
  const uint32_t sph0 = ((uint32_t)SM_STAGE_PIXEL << 10) | (3u << 5) | 1u;
  memcpy(b->bytes, &sph0, 4);
  b->bytes[0x48] = 0x0f;
  b->bytes[0x18] = 0x55; /* vector 0: constant x4 */
  b->bytes[0x19] = 0x55; /* vector 1 */
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

static uint32_t next_index(const Builder *b) { return b->words % 4u == 0 ? b->words + 1u : b->words; }
static int32_t offset_to(uint32_t from_index, uint32_t to_index) { return ((int32_t)to_index - (int32_t)from_index - 1) * 8; }

/* ---- the reference run -------------------------------------------- */

#define CB_SLOT 3u
#define CB_WORDS 16u
#define TEX_W 2u
#define TEX_H 2u

static Sm_Program g_prog;
static Sm_Thread g_thread;
static uint32_t g_cb[CB_WORDS];
static uint8_t g_texels[TEX_W * TEX_H * 4u] = {
    255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 255, 255, 255, 0,
};
static Tex_Image g_image;
static Tex_Sampler g_sampler;
static uint32_t g_varying[2][4];
static float g_inv_w = 0.5f;

static void fake_texture(void *user, const Sm_Tex_Request *req, uint32_t out[4]) {
  (void)user;
  tex_sample(&g_image, &g_sampler, req->coords, req->layer, req->dref, req->shadow, NULL, out);
}

static void setup_image(void) {
  memset(&g_image, 0, sizeof(g_image));
  g_image.header.type = TEX_TYPE_2D;
  g_image.header.normalized = true;
  g_image.header.swizzle[0] = TEX_SOURCE_R;
  g_image.header.swizzle[1] = TEX_SOURCE_G;
  g_image.header.swizzle[2] = TEX_SOURCE_B;
  g_image.header.swizzle[3] = TEX_SOURCE_A;
  for (uint32_t c = 0; c < 4; c++) g_image.header.data_type[c] = TEX_DATA_UNORM;
  g_image.bytes_per_texel = 4;
  g_image.width = TEX_W;
  g_image.height = TEX_H;
  g_image.layers = 1;
  g_image.row_bytes = TEX_W * 4u;
  g_image.layer_bytes = TEX_W * TEX_H * 4u;
  g_image.texels = g_texels;
  g_image.rgba8 = true;
  g_image.valid = true;
  memset(&g_sampler, 0, sizeof(g_sampler));
  g_sampler.wrap[0] = g_sampler.wrap[1] = g_sampler.wrap[2] = 2; /* clamp to edge */
  g_sampler.mag_filter = 2;
  g_sampler.min_filter = 2;
}

/* Runs the program for the pixel at (0.5, 0.5); false when it was killed. */
static bool reference(uint32_t out[4]) {
  Sm_Env env;
  memset(&env, 0, sizeof(env));
  env.cbuf[CB_SLOT] = (const uint8_t *)g_cb;
  env.cbuf_size[CB_SLOT] = sizeof(g_cb);
  env.texture = fake_texture;
  sm_thread_reset(&g_thread, 1);
  g_thread.attr_in[SM_ATTR_POSITION / 4u][0] = (uint32_t)f_bits(0.5f);
  g_thread.attr_in[SM_ATTR_POSITION / 4u + 1u][0] = (uint32_t)f_bits(0.5f);
  g_thread.attr_in[SM_ATTR_POSITION / 4u + 2u][0] = (uint32_t)f_bits(0.25f);
  g_thread.attr_in[SM_ATTR_POSITION / 4u + 3u][0] = (uint32_t)f_bits(g_inv_w);
  for (uint32_t v = 0; v < 2; v++)
    for (uint32_t c = 0; c < 4; c++) g_thread.attr_in[SM_ATTR_GENERIC / 4u + 4u * v + c][0] = g_varying[v][c];
  g_thread.front_facing = SM_ALL_LANES;
  const bool ok = sm_run(&g_prog, &env, &g_thread);
  if (!ok) fprintf(stderr, "[wgsl_test] the interpreter faulted\n");
  CHECK(ok);
  for (uint32_t c = 0; c < 4; c++) out[c] = g_thread.r[c][0];
  return !(g_thread.killed & 1u);
}

/* ---- vectors ------------------------------------------------------ */

static const char *g_dir;
static char g_text[1u << 20];
static uint32_t g_vectors;

static uint32_t g_hw_sample_mask; /* the next vector's hardware-sampled textures */
/* The next vector samples a 2-level texture (level 1 = the 2x2 box of
 * level 0) with the sampler's level clamps at [0, 1]: the expected output
 * is level 1's texel, not the interpreter's (level 0 only - the WebGPU
 * renderer's mip chains are a deliberate improvement, DESIGN.md §13). */
static bool g_mip_vector;

static void finish(const Builder *b, const char *name, bool float_compare) {
  sm_program_decode(b->bytes, SM_SPH_BYTES + 8u * b->words, 0, &g_prog);
  Wgsl_Program_Desc desc;
  wgsl_default_desc(&g_prog, &desc);
  desc.target_int_mask = 1u; /* raw register bits out (rgba32uint) */
  desc.hw_sample_mask = g_hw_sample_mask;
  g_hw_sample_mask = 0;
  const Wgsl_Result r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  if (!r.ok) fprintf(stderr, "[wgsl_test] %s: %s\n", name, r.reason);
  CHECK(r.ok);
  CHECK(r.length == strlen(g_text));
  CHECK(strstr(g_text, "@fragment fn fs") != NULL && strstr(g_text, "@vertex fn vs") != NULL);
  uint32_t out[4];
  fprintf(stderr, "[wgsl_test] %s\n", name);
  const bool alive = reference(out);
  g_vectors++;
  if (!g_dir) return;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.wgsl", g_dir, name);
  FILE *f = fopen(path, "w");
  CHECK(f != NULL);
  fwrite(r.text, 1, r.length, f);
  fclose(f);
  snprintf(path, sizeof(path), "%s/%s.json", g_dir, name);
  f = fopen(path, "w");
  CHECK(f != NULL);
  fprintf(f, "{\"name\":\"%s\",\"float\":%s,\"inv_w\":%u,\"cbuf_slot\":%u,\"cbuf\":[", name,
          float_compare ? "true" : "false", (uint32_t)f_bits(g_inv_w), CB_SLOT);
  for (uint32_t i = 0; i < CB_WORDS; i++) fprintf(f, "%s%u", i ? "," : "", g_cb[i]);
  fprintf(f, "],\"varyings\":[");
  for (uint32_t v = 0; v < desc.varying_count; v++)
    fprintf(f, "%s[%u,%u,%u,%u]", v ? "," : "", g_varying[v][0], g_varying[v][1], g_varying[v][2], g_varying[v][3]);
  fprintf(f, "],\"texture\":{\"width\":%u,\"height\":%u,\"rgba8\":[", TEX_W, TEX_H);
  for (uint32_t i = 0; i < sizeof(g_texels); i++) fprintf(f, "%s%u", i ? "," : "", g_texels[i]);
  fprintf(f, "],\"wrap\":%u,\"linear\":%s,\"textures\":%u", 0x222u, "true", desc.texture_count);
  if (g_mip_vector) {
    uint32_t mip1[4];
    for (uint32_t c = 0; c < 4; c++) {
      uint32_t sum = 0;
      for (uint32_t i = 0; i < TEX_W * TEX_H; i++) sum += g_texels[4u * i + c];
      mip1[c] = (sum + 2u) / 4u;
      out[c] = (uint32_t)f_bits((float)mip1[c] / 255.0f);
    }
    fprintf(f, ",\"levels\":2,\"mip1\":[%u,%u,%u,%u],\"lod\":[0,0,1]", mip1[0], mip1[1], mip1[2], mip1[3]);
    g_mip_vector = false;
  }
  fprintf(f, "},");
  if (alive) fprintf(f, "\"expected\":[%u,%u,%u,%u]}\n", out[0], out[1], out[2], out[3]);
  else fprintf(f, "\"expected\":\"killed\"}\n");
  fclose(f);
}

static void put_varying(uint32_t v, float x, float y, float z, float w) {
  g_varying[v][0] = (uint32_t)f_bits(x);
  g_varying[v][1] = (uint32_t)f_bits(y);
  g_varying[v][2] = (uint32_t)f_bits(z);
  g_varying[v][3] = (uint32_t)f_bits(w);
}

static void vector_float(void) {
  Builder b;
  begin(&b);
  g_cb[0] = (uint32_t)f_bits(3.0f);
  put_varying(0, 0.25f, -1.5f, 7.0f, 0.125f);
  emit(&b, IPA(4, 0x7c, 0, (uint32_t)RZ)); /* 1/w */
  emit(&b, MUFU(5, 4, 4));                 /* w */
  emit(&b, IPA(0, SM_ATTR_GENERIC, 0, (uint32_t)RZ));
  emit(&b, IPA(1, SM_ATTR_GENERIC + 4u, 0, (uint32_t)RZ));
  emit(&b, IPA(2, SM_ATTR_GENERIC + 8u, 0, (uint32_t)RZ));
  emit(&b, FFMA_RR(0, 0, 5, 1));           /* 0.25 * 2 + -1.5 */
  emit(&b, FMUL_C(1, 1, CB_SLOT, 0));      /* -1.5 * 3 */
  emit(&b, FADD_R(2, 2, 5));               /* 7 + 2 */
  emit(&b, MUFU(3, 2, 5));                 /* rsqrt(9) */
  emit(&b, EXIT());
  finish(&b, "float", true);
}

static void vector_integer(void) {
  Builder b;
  begin(&b);
  g_cb[1] = 5;
  emit(&b, MOV32I(4, 0x12345678u));
  emit(&b, MOV32I(5, 0x0f0f0f0fu));
  emit(&b, LOP_R(0, 4, 5, 0));             /* and */
  emit(&b, SHL_I(1, 5, 4));
  emit(&b, IADD_R(1, 1, 4));
  emit(&b, XMAD_R(2, 4, 5, 0));
  emit(&b, MOV32I(6, (uint32_t)f_bits(-37.75f)));
  emit(&b, F2I_R(7, 6));                   /* trunc -> -37 */
  emit(&b, I2F_R(3, 7));
  emit(&b, IADD32I(3, 3, 0x10u));
  emit(&b, EXIT());
  finish(&b, "integer", false);
}

static void vector_branches(void) {
  /* r0 = sum of 1..4 through a BRA loop; r1 via SSY/SYNC around a skipped
   * block; r2 from a PBK/BRK loop. */
  Builder b;
  begin(&b);
  g_cb[2] = 4;
  emit(&b, MOV32I(0, 0));
  emit(&b, MOV32I(4, 1));
  const uint32_t loop = next_index(&b);
  emit(&b, IADD_R(0, 0, 4));
  emit(&b, IADD32I(4, 4, 1));
  emit(&b, ISETP_C(0, 4, CB_SLOT, 8, 3)); /* p0 = r4 <= 4 */
  const uint32_t bra = next_index(&b);
  emit(&b, BRA(offset_to(bra, loop), 0, false));
  /* SSY: skip the MOV when p0 is false (it is now). */
  const uint32_t ssy = next_index(&b);
  emit(&b, SSY(0)); /* patched below */
  emit(&b, MOV32I(1, 11));
  const uint32_t bra2 = next_index(&b);
  emit(&b, BRA(0, 0, true)); /* patched: @!P0 BRA to sync */
  emit(&b, MOV32I(1, 22));
  const uint32_t sync = next_index(&b);
  emit(&b, SYNC());
  const uint32_t after = next_index(&b);
  /* PBK loop: r2 counts to 3, then BRK. */
  const uint32_t pbk = next_index(&b);
  emit(&b, PBK(0)); /* patched */
  emit(&b, MOV32I(2, 0));
  const uint32_t loop2 = next_index(&b);
  emit(&b, IADD32I(2, 2, 1));
  emit(&b, ISETP_C(1, 2, CB_SLOT, 8, 1)); /* p1 = r2 < 4 */
  emit(&b, BRK(1, true));                 /* @!P1 BRK */
  const uint32_t bra3 = next_index(&b);
  emit(&b, BRA(offset_to(bra3, loop2), (uint32_t)PT, false));
  const uint32_t end = next_index(&b);
  emit(&b, MOV32I(3, 0x99));
  emit(&b, EXIT());
  /* Patch the forward targets. */
  uint64_t w;
  w = SSY(offset_to(ssy, after));
  memcpy(b.bytes + SM_SPH_BYTES + 8u * ssy, &w, 8);
  w = BRA(offset_to(bra2, sync), 0, true);
  memcpy(b.bytes + SM_SPH_BYTES + 8u * bra2, &w, 8);
  w = PBK(offset_to(pbk, end));
  memcpy(b.bytes + SM_SPH_BYTES + 8u * pbk, &w, 8);
  finish(&b, "branches", false);
}

static void vector_select_texture(void) {
  Builder b;
  begin(&b);
  put_varying(0, 0.25f, 0.75f, 0.6f, 0.0f);
  put_varying(1, 1.0f, 2.0f, 0.0f, 0.0f);
  emit(&b, IPA(4, SM_ATTR_GENERIC, 0, (uint32_t)RZ));
  emit(&b, IPA(5, SM_ATTR_GENERIC + 4u, 0, (uint32_t)RZ));
  emit(&b, TEXS(0, 2, 4, 5, 1, 4, 0)); /* RGBA -> r0, r1, r2, r3 */
  emit(&b, IPA(6, SM_ATTR_GENERIC + 16u, 0, (uint32_t)RZ));
  emit(&b, IPA(7, SM_ATTR_GENERIC + 20u, 0, (uint32_t)RZ));
  emit(&b, FSETP_R(2, 6, 7, 1));      /* p2 = 1 < 2 */
  emit(&b, SEL_R(3, 6, 7, 2));        /* r3 = p2 ? r6 : r7 */
  emit(&b, EXIT());
  finish(&b, "texture", true);
}

/* The same sample through a hardware sampler (bilinear weights differ in
 * the last bits: compared as floats). */
static void vector_texture_hw(void) {
  Builder b;
  begin(&b);
  put_varying(0, 0.3f, 0.6f, 0.0f, 0.0f);
  put_varying(1, 1.0f, 2.0f, 0.0f, 0.0f);
  emit(&b, IPA(4, SM_ATTR_GENERIC, 0, (uint32_t)RZ));
  emit(&b, IPA(5, SM_ATTR_GENERIC + 4u, 0, (uint32_t)RZ));
  emit(&b, TEXS(0, 2, 4, 5, 1, 4, 0));
  emit(&b, EXIT());
  g_hw_sample_mask = 1u;
  finish(&b, "texture_hw", true);
}

/* TEXS 2D LL: the explicit level picks level 1 (see g_mip_vector). */
static void vector_texture_lod(void) {
  Builder b;
  begin(&b);
  put_varying(0, 0.3f, 0.6f, 0.0f, 0.0f);
  put_varying(1, 1.0f, 2.0f, 0.0f, 0.0f);
  emit(&b, IPA(4, SM_ATTR_GENERIC, 0, (uint32_t)RZ));
  emit(&b, IPA(5, SM_ATTR_GENERIC + 4u, 0, (uint32_t)RZ));
  emit(&b, IPA(6, SM_ATTR_GENERIC + 16u, 0, (uint32_t)RZ)); /* lod 1.0 */
  emit(&b, TEXS(0, 2, 4, 6, 3, 4, 0));
  emit(&b, EXIT());
  g_mip_vector = true;
  finish(&b, "texture_lod", true);
}

static void vector_kill(void) {
  Builder b;
  begin(&b);
  put_varying(0, 1.0f, 0.0f, 0.0f, 0.0f);
  emit(&b, IPA(4, SM_ATTR_GENERIC, 0, (uint32_t)RZ));
  emit(&b, IPA(5, SM_ATTR_GENERIC + 4u, 0, (uint32_t)RZ));
  emit(&b, FSETP_R(0, 4, 5, 4));      /* p0 = 1 > 0 */
  emit(&b, KIL_P(0));
  emit(&b, MOV32I(0, 1));
  emit(&b, EXIT());
  finish(&b, "kill", false);
}

static void vector_ldc(void) {
  Builder b;
  begin(&b);
  for (uint32_t i = 0; i < CB_WORDS; i++) g_cb[i] = 0x1000u + i;
  emit(&b, MOV32I(4, 8));
  emit(&b, LDC(0, 4, CB_SLOT, 4));    /* c[3][12] */
  emit(&b, LDC(1, (uint32_t)RZ, CB_SLOT, 60)); /* last word */
  emit(&b, LDC(2, (uint32_t)RZ, CB_SLOT, 64)); /* out of range: 0 */
  emit(&b, MOV32I(3, 7));
  emit(&b, EXIT());
  finish(&b, "ldc", false);
}

/* ---- structural ---------------------------------------------------- */

static void structural(void) {
  Builder b;
  /* Not a pixel program. */
  begin(&b);
  uint32_t sph0 = ((uint32_t)SM_STAGE_VERTEX << 10) | (3u << 5) | 1u;
  memcpy(b.bytes, &sph0, 4);
  emit(&b, EXIT());
  sm_program_decode(b.bytes, SM_SPH_BYTES + 8u * b.words, 0, &g_prog);
  Wgsl_Program_Desc desc;
  wgsl_default_desc(&g_prog, &desc);
  Wgsl_Result r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(!r.ok && strstr(r.reason, "pixel") != NULL);
  /* Unsupported in a pixel program: reported, nothing emitted. */
  begin(&b);
  emit(&b, ALD(0, 0x80, 1));
  emit(&b, EXIT());
  sm_program_decode(b.bytes, SM_SPH_BYTES + 8u * b.words, 0, &g_prog);
  wgsl_default_desc(&g_prog, &desc);
  r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(!r.ok && strstr(r.reason, "ALD") != NULL && g_text[0] == '\0');
  /* A buffer too small fails cleanly. */
  begin(&b);
  emit(&b, MOV32I(0, 1));
  emit(&b, EXIT());
  sm_program_decode(b.bytes, SM_SPH_BYTES + 8u * b.words, 0, &g_prog);
  wgsl_default_desc(&g_prog, &desc);
  static char small[6000];
  r = wgsl_translate(&g_prog, &desc, small, sizeof(small));
  CHECK(!r.ok && small[0] == '\0');
  r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(r.ok);
  /* The default descriptor: flat vectors 0 and 1, one target. */
  CHECK(desc.varying_count == 2 && desc.flat_mask == 3u && desc.varying_location[0] == 0 &&
        desc.varying_location[1] == 1 && desc.varying_location[2] == 0xff && desc.target_count == 1);
  /* The cache key follows the descriptor. */
  const uint64_t h = wgsl_desc_hash(&desc, &g_prog);
  CHECK(h == wgsl_desc_hash(&desc, &g_prog));
  desc.target_int_mask = 1u;
  CHECK(h != wgsl_desc_hash(&desc, &g_prog));
}

/* Level selection (the WebGPU renderer's mip chains): TEXS 2D picks its
 * level from derivatives in uniform control flow, LZ takes level 0, LL
 * its register; a predicated sample has no derivatives (level 0). */
static void lod_selection(void) {
  Builder b;
  Wgsl_Program_Desc desc;
  begin(&b);
  emit(&b, TEXS(0, 2, 4, 5, 1, 4, 0));            /* 2D: automatic */
  emit(&b, TEXS(6, 8, 4, 5, 2, 4, 0));            /* 2D LZ */
  emit(&b, TEXS(10, 12, 4, 5, 3, 4, 0));          /* 2D LL: the level in the second operand pair */
  emit(&b, EXIT());
  sm_program_decode(b.bytes, SM_SPH_BYTES + 8u * b.words, 0, &g_prog);
  wgsl_default_desc(&g_prog, &desc);
  Wgsl_Result r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(r.ok);
  CHECK(strstr(g_text, "fn t0_lod(") != NULL);
  CHECK(strstr(g_text, "vec2<i32>(0), t0_lod(tc, tl)); ") != NULL);
  CHECK(strstr(g_text, "vec2<i32>(0), 0.0); ") != NULL);
  CHECK(strstr(g_text, "vec2<i32>(0), F(r") != NULL);
  /* Predicated: no derivatives. */
  begin(&b);
  emit(&b, TEXS(0, 2, 4, 5, 1, 4, 0) & ~(0xfull << 16)); /* @P0 */
  emit(&b, EXIT());
  sm_program_decode(b.bytes, SM_SPH_BYTES + 8u * b.words, 0, &g_prog);
  wgsl_default_desc(&g_prog, &desc);
  r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(r.ok);
  CHECK(strstr(g_text, "t0_lod(tc") == NULL && strstr(g_text, "vec2<i32>(0), 0.0); ") != NULL);
}

int main(int argc, char **argv) {
  g_dir = argc > 1 ? argv[1] : NULL;
  tex_init_tables();
  setup_image();
  structural();
  lod_selection();
  vector_float();
  vector_integer();
  vector_branches();
  vector_select_texture();
  vector_texture_hw();
  vector_texture_lod();
  vector_kill();
  vector_ldc();
  printf("[wgsl_test] passed (%u vectors%s%s)\n", g_vectors, g_dir ? " written to " : "", g_dir ? g_dir : "");
  return 0;
}
