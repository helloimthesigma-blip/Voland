/**
 * 3D reference renderer (gpu/raster3d) end to end over a synthetic GPU
 * address space: hand-assembled vertex and pixel programs, a vertex
 * buffer, a texture header/sampler pool, and pitch / block-linear render
 * targets. Checks clears, coverage (top-left fill, scissor), alpha
 * blending, texture sampling and the write-back to guest memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/block_linear.h"
#include "gpu/gpu_channel.h"
#include "gpu/raster3d.h"

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

/* ---- synthetic GPU memory: one flat window at GPU_BASE ------------- */

#define GPU_BASE 0x100000ull
#define GPU_BYTES 0x80000u
#define PROGRAM_REGION GPU_BASE
#define VS_OFFSET 0x0u
#define PS_OFFSET 0x800u
#define PS_TEX_OFFSET 0x1000u
#define PS_DERIV_OFFSET 0x1800u
#define VERTICES (GPU_BASE + 0x4000u)
#define CBUF (GPU_BASE + 0x6000u)
#define TIC_POOL (GPU_BASE + 0x7000u)
#define TSC_POOL (GPU_BASE + 0x7400u)
#define TEXELS (GPU_BASE + 0x7800u)
#define RT (GPU_BASE + 0x10000u)
#define RT_BL (GPU_BASE + 0x30000u)
#define RT_SIZE 64u

static uint8_t g_gpu[GPU_BYTES];

static bool mem_read(void *user, uint64_t va, void *out, uint64_t size) {
  (void)user;
  if (va < GPU_BASE || va + size > GPU_BASE + GPU_BYTES) return false;
  memcpy(out, g_gpu + (va - GPU_BASE), size);
  return true;
}

static bool mem_write(void *user, uint64_t va, const void *src, uint64_t size) {
  (void)user;
  if (va < GPU_BASE || va + size > GPU_BASE + GPU_BYTES) return false;
  memcpy(g_gpu + (va - GPU_BASE), src, size);
  return true;
}

static void put32(uint64_t va, uint32_t v) { memcpy(g_gpu + (va - GPU_BASE), &v, 4); }
static void putf(uint64_t va, float f) { memcpy(g_gpu + (va - GPU_BASE), &f, 4); }

/* ---- programs ----------------------------------------------------- */

#define GUARD (7ull << 16)
#define RZ 0xffull

static uint64_t ALD(uint32_t d, uint32_t addr, uint32_t n) {
  return (0xefd8ull << 48) | GUARD | ((uint64_t)(n - 1u) << 47) | (RZ << 39) | ((uint64_t)addr << 20) | (RZ << 8) | d;
}
static uint64_t AST(uint32_t s, uint32_t addr, uint32_t n) {
  return (0xeff0ull << 48) | GUARD | ((uint64_t)(n - 1u) << 47) | (RZ << 39) | ((uint64_t)addr << 20) | (RZ << 8) | s;
}
static uint64_t IPA(uint32_t d, uint32_t addr, uint32_t mode) {
  return (0xe000ull << 48) | GUARD | ((uint64_t)mode << 54) | (7ull << 47) | ((uint64_t)addr << 28) | (RZ << 39) |
         (RZ << 20) | (RZ << 8) | d;
}
static uint64_t TEXS_RGBA(uint32_t d0, uint32_t d1, uint32_t a, uint32_t b) {
  return (0xd800ull << 48) | GUARD | (1ull << 53) | (4ull << 50) | ((uint64_t)d1 << 28) | ((uint64_t)b << 20) |
         ((uint64_t)a << 8) | d0;
}
static uint64_t EXIT(void) { return (0xe300ull << 48) | GUARD | 0xfull; }
static uint64_t MOV32I(uint32_t d, uint32_t imm) { return (0x0100ull << 48) | GUARD | ((uint64_t)imm << 20) | (0xfull << 12) | d; }
/* SHFL.BFLY within a quad (segment mask 0x1c, clamp 3), and FSWZADD. */
static uint64_t SHFL_BFLY(uint32_t d, uint32_t a, uint32_t lane) {
  return (0xef10ull << 48) | (7ull << 48) | GUARD | (3ull << 30) | (3ull << 28) | (0x1c03ull << 34) |
         ((uint64_t)lane << 20) | ((uint64_t)a << 8) | d;
}
static uint64_t FSWZADD(uint32_t d, uint32_t a, uint32_t b, uint32_t mask) {
  return (0x50f8ull << 48) | GUARD | ((uint64_t)mask << 28) | ((uint64_t)b << 20) | ((uint64_t)a << 8) | d;
}
#define QUAD_DFDX 0x99u /* nouveau's QUADOP(SUB, SUBR, SUB, SUBR) */
#define QUAD_DFDY 0xA5u /* QUADOP(SUB, SUB, SUBR, SUBR) */

static void write_program(uint32_t offset, const uint8_t sph[SM_SPH_BYTES], const uint64_t *code, uint32_t count) {
  uint8_t *p = g_gpu + (PROGRAM_REGION - GPU_BASE) + offset;
  memcpy(p, sph, SM_SPH_BYTES);
  uint32_t w = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (w % 4u == 0) {
      const uint64_t sched = 0x001f8000fc0007e0ull;
      memcpy(p + SM_SPH_BYTES + 8u * w++, &sched, 8);
    }
    memcpy(p + SM_SPH_BYTES + 8u * w++, &code[i], 8);
  }
  /* end marker: BRA to self */
  if (w % 4u == 0) w++;
  const uint64_t bra = (0xe240ull << 48) | GUARD | (0xfffff8ull << 20) | 0xfull;
  memcpy(p + SM_SPH_BYTES + 8u * w, &bra, 8);
}

static void build_programs(void) {
  /* VS: position = attr0, generic0 = attr1. */
  uint8_t vs_sph[SM_SPH_BYTES];
  memset(vs_sph, 0, sizeof(vs_sph));
  const uint32_t vs0 = (1u << 10) | (3u << 5) | 1u;
  memcpy(vs_sph, &vs0, 4);
  vs_sph[0x18] = 0xff; /* inputs: generic 0 and 1, xyzw */
  vs_sph[0x35] = 0xf0; /* outputs: position xyzw */
  vs_sph[0x36] = 0x0f; /* outputs: generic 0 xyzw */
  const uint64_t vs[] = {ALD(0, 0x80, 4), AST(0, 0x70, 4), ALD(4, 0x90, 4), AST(4, 0x80, 4), EXIT()};
  write_program(VS_OFFSET, vs_sph, vs, 5);
  /* PS: colour = generic0 (screen-linear). */
  uint8_t ps_sph[SM_SPH_BYTES];
  memset(ps_sph, 0, sizeof(ps_sph));
  const uint32_t ps0 = (5u << 10) | (3u << 5) | 2u;
  memcpy(ps_sph, &ps0, 4);
  ps_sph[0x18] = 0xff; /* generic 0: xyzw screen-linear (3) */
  ps_sph[0x48] = 0x0f; /* RT0 rgba */
  const uint64_t ps[] = {IPA(0, 0x80, 3), IPA(1, 0x84, 3), IPA(2, 0x88, 3), IPA(3, 0x8c, 3), EXIT()};
  write_program(PS_OFFSET, ps_sph, ps, 5);
  /* PS: colour = texture(generic0.xy). */
  ps_sph[0x18] = 0x0f; /* generic 0: xy screen-linear */
  const uint64_t pst[] = {IPA(0, 0x80, 3), IPA(1, 0x84, 3), TEXS_RGBA(0, 2, 0, 1), EXIT()};
  write_program(PS_TEX_OFFSET, ps_sph, pst, 4);
  /* PS: colour = (dFdx(u), dFdy(u), dFdy(v), 1) of generic0.xy. */
  const uint32_t one = 0x3f800000u;
  const uint64_t psd[] = {IPA(4, 0x80, 3), IPA(5, 0x84, 3), SHFL_BFLY(6, 4, 1), FSWZADD(0, 4, 6, QUAD_DFDX),
                          SHFL_BFLY(7, 4, 2), FSWZADD(1, 4, 7, QUAD_DFDY), SHFL_BFLY(8, 5, 2),
                          FSWZADD(2, 5, 8, QUAD_DFDY), MOV32I(3, one), EXIT()};
  write_program(PS_DERIV_OFFSET, ps_sph, psd, 10);
}

/* ---- register state ----------------------------------------------- */

static uint32_t g_regs[GPU_3D_REGISTER_WORDS];
static Raster3d_Bindings g_bindings;

static void base_state(uint64_t rt, bool block_linear, uint32_t ps_offset) {
  memset(g_regs, 0, sizeof(g_regs));
  memset(&g_bindings, 0, sizeof(g_bindings));
  /* RT0 64x64 A8B8G8R8 */
  g_regs[0x200] = (uint32_t)(rt >> 32);
  g_regs[0x201] = (uint32_t)rt;
  g_regs[0x202] = block_linear ? RT_SIZE : RT_SIZE * 4u;
  g_regs[0x203] = RT_SIZE;
  g_regs[0x204] = 0xD5;
  g_regs[0x205] = block_linear ? (1u << 4) : (1u << 12);
  g_regs[0x487] = 1;           /* CT_SELECT: one target, RT0 */
  g_regs[0x680] = 0x1111;      /* CT_WRITE(0) */
  /* viewport 0: [-1,1] -> [0,64] */
  const float scale = RT_SIZE / 2.0f;
  memcpy(&g_regs[0x280], &scale, 4);
  memcpy(&g_regs[0x281], &scale, 4);
  const float half = 0.5f;
  memcpy(&g_regs[0x282], &half, 4);
  memcpy(&g_regs[0x283], &scale, 4);
  memcpy(&g_regs[0x284], &scale, 4);
  memcpy(&g_regs[0x285], &half, 4);
  g_regs[0x64b] = 1;           /* viewport transform on */
  g_regs[0x582] = (uint32_t)(PROGRAM_REGION >> 32);
  g_regs[0x583] = (uint32_t)PROGRAM_REGION;
  g_regs[0x800 + 16u * 1u] = 1u | (1u << 4); /* vertex B */
  g_regs[0x801 + 16u * 1u] = VS_OFFSET;
  g_regs[0x804 + 16u * 1u] = 0;
  g_regs[0x800 + 16u * 5u] = 1u | (5u << 4); /* pixel */
  g_regs[0x801 + 16u * 5u] = ps_offset;
  g_regs[0x804 + 16u * 5u] = 4;
  /* attr0: pos (4 x f32 @0), attr1: colour/uv (4 x f32 @16), stream 0 */
  g_regs[0x458] = (0x01u << 21) | (7u << 27);
  g_regs[0x459] = (16u << 7) | (0x01u << 21) | (7u << 27);
  g_regs[0x700] = 32u | (1u << 12);
  g_regs[0x701] = (uint32_t)(VERTICES >> 32);
  g_regs[0x702] = (uint32_t)VERTICES;
}

static void vertex(uint32_t i, float x, float y, float r, float g, float b, float a) {
  const uint64_t at = VERTICES + 32u * i;
  putf(at, x);
  putf(at + 4, y);
  putf(at + 8, 0.0f);
  putf(at + 12, 1.0f);
  putf(at + 16, r);
  putf(at + 20, g);
  putf(at + 24, b);
  putf(at + 28, a);
}

static const uint8_t *pixel(uint64_t rt, bool block_linear, uint32_t x, uint32_t y) {
  const uint64_t off = block_linear ? block_linear_offset(x * 4u, y, RT_SIZE * 4u, 1) : (uint64_t)y * RT_SIZE * 4u + x * 4u;
  return g_gpu + (rt - GPU_BASE) + off;
}

static bool rgba_is(const uint8_t *p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const int tolerance = 1;
  return abs(p[0] - r) <= tolerance && abs(p[1] - g) <= tolerance && abs(p[2] - b) <= tolerance &&
         abs(p[3] - a) <= tolerance;
}

static const Gpu_Memory k_mem = {NULL, mem_read, mem_write, NULL, NULL, NULL};

static void clear_to(Raster3d *r, float red, float green, float blue, float alpha) {
  memcpy(&g_regs[0x360], &red, 4);
  memcpy(&g_regs[0x361], &green, 4);
  memcpy(&g_regs[0x362], &blue, 4);
  memcpy(&g_regs[0x363], &alpha, 4);
  raster3d_clear(r, g_regs, &k_mem, 0x3cu);
}

static void draw_arrays(Raster3d *r, uint32_t topology, uint32_t count) {
  Raster3d_Draw d;
  memset(&d, 0, sizeof(d));
  d.kind = RASTER_DRAW_ARRAYS;
  d.topology = topology;
  d.count = count;
  raster3d_draw(r, g_regs, &g_bindings, &k_mem, &d);
}

static void test_triangle_and_blend(Raster3d *r, bool block_linear) {
  const uint64_t rt = block_linear ? RT_BL : RT;
  base_state(rt, block_linear, PS_OFFSET);
  raster3d_begin_submission(r);
  clear_to(r, 0.0f, 0.0f, 1.0f, 1.0f);
  /* Lower-left half (y down): x + y < 64. */
  vertex(0, -1.0f, -1.0f, 1, 0, 0, 1);
  vertex(1, 1.0f, -1.0f, 1, 0, 0, 1);
  vertex(2, -1.0f, 1.0f, 1, 0, 0, 1);
  draw_arrays(r, 4, 3);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(rt, block_linear, 10, 10), 255, 0, 0, 255), "inside is red (%s)", block_linear ? "BL" : "pitch");
  CHECK(rgba_is(pixel(rt, block_linear, 60, 60), 0, 0, 255, 255), "outside is the clear colour");
  /* The diagonal x + y = 63 has centres at x+y+1 = 64: exactly on the
   * edge, owned by exactly one side - not covered here. */
  uint32_t covered = 0;
  for (uint32_t y = 0; y < RT_SIZE; y++)
    for (uint32_t x = 0; x < RT_SIZE; x++)
      if (pixel(rt, block_linear, x, y)[0] == 255) covered++;
  CHECK(covered == 64u * 63u / 2u, "half-space coverage %u", covered);
  /* Scissor: only x < 8. */
  g_regs[0x380] = 1;
  g_regs[0x381] = 8u << 16;
  g_regs[0x382] = 64u << 16;
  /* 50% green over everything. */
  g_regs[0x4d8] = 1;
  g_regs[0x4d0] = 0x8006; g_regs[0x4d1] = 0x4302; g_regs[0x4d2] = 0x4303;
  g_regs[0x4d3] = 0x8006; g_regs[0x4d4] = 0x4302; g_regs[0x4d6] = 0x4303;
  vertex(0, -1.0f, -1.0f, 0, 1, 0, 0.5f);
  vertex(1, 3.0f, -1.0f, 0, 1, 0, 0.5f);
  vertex(2, -1.0f, 3.0f, 0, 1, 0, 0.5f);
  draw_arrays(r, 4, 3);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(rt, block_linear, 4, 10), 128, 128, 0, 191), "blended red+green (%u %u %u %u)",
        pixel(rt, block_linear, 4, 10)[0], pixel(rt, block_linear, 4, 10)[1], pixel(rt, block_linear, 4, 10)[2],
        pixel(rt, block_linear, 4, 10)[3]);
  CHECK(rgba_is(pixel(rt, block_linear, 20, 10), 255, 0, 0, 255), "scissored pixel untouched");
  /* Frame skip: neither draws nor clears touch the target, nor count. */
  r->skip_draws = true;
  const uint64_t draws = r->stats.draws, clears = r->stats.clears;
  clear_to(r, 1, 1, 1, 1);
  draw_arrays(r, 4, 3);
  raster3d_flush(r, &k_mem);
  CHECK(r->stats.draws == draws && r->stats.clears == clears, "skipped draws are not counted");
  CHECK(rgba_is(pixel(rt, block_linear, 20, 10), 255, 0, 0, 255), "a skipped frame leaves the target alone");
  r->skip_draws = false;
}

static void test_texture(Raster3d *r) {
  base_state(RT, false, PS_TEX_OFFSET);
  raster3d_begin_submission(r);
  /* 2x2 A8B8G8R8 pitch texture (32-byte rows): red, green / blue, white. */
  uint32_t tic[8];
  memset(tic, 0, sizeof(tic));
  tic[0] = 0x08u | (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16) | (2u << 19) | (3u << 22) | (4u << 25) | (5u << 28);
  tic[1] = (uint32_t)TEXELS & ~0x1fu;
  tic[2] = (uint32_t)(TEXELS >> 32) | (2u << 21); /* header version: pitch */
  tic[3] = 32u >> 5;                               /* pitch, in 32-byte units */
  tic[4] = (2u - 1u) | (1u << 23);                 /* width-1, 2D */
  tic[5] = (2u - 1u) | (1u << 31);                 /* height-1, normalized */
  memcpy(g_gpu + (TIC_POOL - GPU_BASE), tic, sizeof(tic));
  const uint32_t rows[2][8] = {{0xff0000ffu, 0xff00ff00u}, {0xffff0000u, 0xffffffffu}};
  memcpy(g_gpu + (TEXELS - GPU_BASE), rows[0], 8);
  memcpy(g_gpu + (TEXELS - GPU_BASE) + 32, rows[1], 8);
  uint32_t tsc[8];
  memset(tsc, 0, sizeof(tsc));
  tsc[0] = 2u | (2u << 3); /* clamp to edge */
  tsc[1] = 1u;             /* nearest */
  memcpy(g_gpu + (TSC_POOL - GPU_BASE), tsc, sizeof(tsc));
  g_regs[0x55d] = (uint32_t)(TIC_POOL >> 32);
  g_regs[0x55e] = (uint32_t)TIC_POOL;
  g_regs[0x557] = (uint32_t)(TSC_POOL >> 32);
  g_regs[0x558] = (uint32_t)TSC_POOL;
  g_regs[0x982] = 0;       /* bound textures: c[0] */
  put32(CBUF, 0x00000000u); /* handle 0: TIC 0, TSC 0 */
  g_bindings.address[4][0] = CBUF;
  g_bindings.size[4][0] = 0x100;
  clear_to(r, 0, 0, 0, 1);
  /* Full-screen quad as a strip, uv 0..1 in generic0.xy. */
  vertex(0, -1.0f, -1.0f, 0, 0, 0, 0);
  vertex(1, 1.0f, -1.0f, 1, 0, 0, 0);
  vertex(2, -1.0f, 1.0f, 0, 1, 0, 0);
  vertex(3, 1.0f, 1.0f, 1, 1, 0, 0);
  draw_arrays(r, 5, 4);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(RT, false, 10, 10), 255, 0, 0, 255), "texel (0,0) red: %08x", *(const uint32_t *)(const void *)pixel(RT, false, 10, 10));
  CHECK(rgba_is(pixel(RT, false, 50, 10), 0, 255, 0, 255), "texel (1,0) green");
  CHECK(rgba_is(pixel(RT, false, 10, 50), 0, 0, 255, 255), "texel (0,1) blue");
  CHECK(rgba_is(pixel(RT, false, 50, 50), 255, 255, 255, 255), "texel (1,1) white");
  CHECK(r->stats.texture_misses == 0, "no texture misses (%llu)", (unsigned long long)r->stats.texture_misses);
}

/* Render-to-texture inside one submission: a texture already sampled
 * (and so validated for this submission) is re-read after the GPU draws
 * into its memory again - SDL_FontCache draws glyphs this way. Reuses
 * test_texture's pools and bindings. */
static void set_target(uint64_t rt) {
  g_regs[0x200] = (uint32_t)(rt >> 32);
  g_regs[0x201] = (uint32_t)rt;
}

static void test_render_to_texture(Raster3d *r) {
  const uint64_t target = RT_BL; /* a 64x64 pitch surface, sampled as a texture */
  uint32_t tic[8];
  memset(tic, 0, sizeof(tic));
  tic[0] = 0x08u | (2u << 7) | (2u << 10) | (2u << 13) | (2u << 16) | (2u << 19) | (3u << 22) | (4u << 25) | (5u << 28);
  tic[1] = (uint32_t)target;
  tic[2] = (uint32_t)(target >> 32) | (2u << 21);
  tic[3] = (RT_SIZE * 4u) >> 5;
  tic[4] = (RT_SIZE - 1u) | (1u << 23);
  tic[5] = (RT_SIZE - 1u) | (1u << 31);
  memcpy(g_gpu + (TIC_POOL - GPU_BASE), tic, sizeof(tic));
  raster3d_begin_submission(r);
  set_target(target);
  clear_to(r, 1, 0, 0, 1);
  set_target(RT);
  draw_arrays(r, 5, 4);
  set_target(target);
  clear_to(r, 0, 1, 0, 1);
  set_target(RT);
  draw_arrays(r, 5, 4);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(RT, false, 30, 30), 0, 255, 0, 255), "second draw samples the re-rendered texture: %08x",
        *(const uint32_t *)(const void *)pixel(RT, false, 30, 30));
}

/* Stencil, the way NanoVG fills a path: a colour-masked pass counts
 * coverage with two-sided INCR/DECR_WRAP, then a cover pass draws where
 * the count is non-zero and zeroes it. T1 (lower-left half) is drawn
 * twice one way and once the other (net +-1); T2 (upper-right half)
 * once each way (net 0). */
#define ZT (GPU_BASE + 0x40000u)
#define ZT_Z24S8 0x14u
#define OP_KEEP 0x1e00u
#define OP_ZERO 0x0u
#define OP_INCR_WRAP 0x8507u
#define OP_DECR_WRAP 0x8508u
#define FUNC_ALWAYS 0x207u
#define FUNC_NOTEQUAL 0x205u

static void stencil_ops(uint32_t base, uint32_t fail, uint32_t zfail, uint32_t zpass, uint32_t func) {
  g_regs[base] = fail;
  g_regs[base + 1u] = zfail;
  g_regs[base + 2u] = zpass;
  g_regs[base + 3u] = func;
}

static void triangle(float ax, float ay, float bx, float by, float cx, float cy) {
  vertex(0, ax, ay, 1, 0, 0, 1);
  vertex(1, bx, by, 1, 0, 0, 1);
  vertex(2, cx, cy, 1, 0, 0, 1);
}

static void test_stencil(Raster3d *r) {
  base_state(RT, false, PS_OFFSET);
  g_regs[0x3f8] = (uint32_t)(ZT >> 32);
  g_regs[0x3f9] = (uint32_t)ZT;
  g_regs[0x3fa] = ZT_Z24S8;
  g_regs[0x3fb] = 0;              /* block height 1 GOB */
  g_regs[0x48a] = RT_SIZE;
  g_regs[0x48b] = RT_SIZE;
  g_regs[0x54e] = 1;              /* zeta enabled */
  memset(g_gpu + (ZT - GPU_BASE), 0xA5, RT_SIZE * RT_SIZE * 4u);
  raster3d_begin_submission(r);
  g_regs[0x368] = 0;              /* stencil clear value */
  g_regs[0x4e7] = 0xff;           /* stencil write mask (clears use it too) */
  memcpy(&g_regs[0x360], &(float){0.0f}, 4);
  memcpy(&g_regs[0x361], &(float){0.0f}, 4);
  memcpy(&g_regs[0x362], &(float){1.0f}, 4);
  memcpy(&g_regs[0x363], &(float){1.0f}, 4);
  raster3d_clear(r, g_regs, &k_mem, 0x3cu | 2u);
  /* Pass 1: count. */
  g_regs[0x680] = 0;              /* colour writes off */
  g_regs[0x4e0] = 1;
  stencil_ops(0x4e1, OP_KEEP, OP_KEEP, OP_INCR_WRAP, FUNC_ALWAYS);
  g_regs[0x4e5] = 0;
  g_regs[0x4e6] = 0xff;
  g_regs[0x565] = 1;              /* two-sided */
  stencil_ops(0x566, OP_KEEP, OP_KEEP, OP_DECR_WRAP, FUNC_ALWAYS);
  g_regs[0x3d5] = 0;
  g_regs[0x3d6] = 0xff;
  g_regs[0x3d7] = 0xff;
  triangle(-1, -1, 1, -1, -1, 1);
  draw_arrays(r, 4, 3);
  draw_arrays(r, 4, 3);
  triangle(-1, -1, -1, 1, 1, -1);
  draw_arrays(r, 4, 3);
  triangle(1, 1, 1, -1, -1, 1);
  draw_arrays(r, 4, 3);
  triangle(1, 1, -1, 1, 1, -1);
  draw_arrays(r, 4, 3);
  CHECK(r->stats.skipped_draws == 0, "stencil-only draws are not skipped (%llu)", (unsigned long long)r->stats.skipped_draws);
  /* Pass 2: cover where non-zero, zeroing. */
  g_regs[0x680] = 0x1111;
  g_regs[0x565] = 0;
  stencil_ops(0x4e1, OP_KEEP, OP_KEEP, OP_ZERO, FUNC_NOTEQUAL);
  vertex(0, -1.0f, -1.0f, 1, 0, 0, 1);
  vertex(1, 1.0f, -1.0f, 1, 0, 0, 1);
  vertex(2, -1.0f, 1.0f, 1, 0, 0, 1);
  vertex(3, 1.0f, 1.0f, 1, 0, 0, 1);
  draw_arrays(r, 5, 4);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(RT, false, 10, 10), 255, 0, 0, 255), "non-zero count is covered");
  CHECK(rgba_is(pixel(RT, false, 60, 60), 0, 0, 255, 255), "zero count is not");
  const uint8_t *zt = g_gpu + (ZT - GPU_BASE);
  const uint32_t at10 = (uint32_t)block_linear_offset(10u * 4u, 10, RT_SIZE * 4u, 0);
  const uint32_t at60 = (uint32_t)block_linear_offset(60u * 4u, 60, RT_SIZE * 4u, 0);
  CHECK(zt[at10] == 0 && zt[at60] == 0, "stencil zeroed by the cover pass (%u %u)", zt[at10], zt[at60]);
  /* Depth bits were cleared? No - only stencil was cleared: they keep the
   * guest's bytes. */
  CHECK(zt[at10 + 3u] == 0xA5, "depth bytes untouched by a stencil-only clear");
  g_regs[0x4e0] = 0;
  g_regs[0x54e] = 0;
}

/* Derivatives through the rasterizer's quads: u = x and v = y in pixels,
 * so dFdx(u) = dFdy(v) = 1 and dFdy(u) = 0 at every covered pixel -
 * including along the diagonal edge, whose quads need helper lanes. A
 * flat triangle's derivatives are exactly zero. */
static void test_derivatives(Raster3d *r) {
  base_state(RT, false, PS_DERIV_OFFSET);
  raster3d_begin_submission(r);
  clear_to(r, 0, 1, 0, 1);
  vertex(0, -1.0f, -1.0f, 0.0f, 0.0f, 0, 1);
  vertex(1, 1.0f, -1.0f, 64.0f, 0.0f, 0, 1);
  vertex(2, -1.0f, 1.0f, 0.0f, 64.0f, 0, 1);
  draw_arrays(r, 4, 3);
  raster3d_flush(r, &k_mem);
  uint32_t covered = 0, wrong = 0;
  for (uint32_t y = 0; y < RT_SIZE; y++)
    for (uint32_t x = 0; x < RT_SIZE; x++) {
      const uint8_t *p = pixel(RT, false, x, y);
      if (rgba_is(p, 0, 255, 0, 255)) continue; /* the clear colour: not covered */
      covered++;
      if (!rgba_is(p, 255, 0, 255, 255)) wrong++;
    }
  CHECK(covered == 64u * 63u / 2u && wrong == 0, "derivatives: %u covered, %u wrong", covered, wrong);
  /* Flat: every input constant -> derivatives 0 -> (0, 0, 0, 1). */
  raster3d_begin_submission(r);
  vertex(0, -1.0f, -1.0f, -5.0f, -7.0f, 0, 1); /* negative: a stale neighbour shows as a positive slope */
  vertex(1, 1.0f, -1.0f, -5.0f, -7.0f, 0, 1);
  vertex(2, -1.0f, 1.0f, -5.0f, -7.0f, 0, 1);
  draw_arrays(r, 4, 3);
  raster3d_flush(r, &k_mem);
  CHECK(rgba_is(pixel(RT, false, 10, 10), 0, 0, 0, 255), "flat triangle: zero derivatives %08x", *(const uint32_t *)(const void *)pixel(RT, false, 10, 10));
}

/* Pixel work is split over row bands (raster3d_set_workers): the output
 * must not depend on the worker count. One draw of overlapping, blended
 * triangles (order-sensitive) queues more than the parallel threshold,
 * then the derivative program (2x2 quads across band edges) draws over
 * half of it. */
static uint32_t render_scene(Raster3d *r, uint32_t workers, uint8_t out[RT_SIZE * RT_SIZE * 4u]) {
  raster3d_set_workers(r, workers);
  const uint64_t pixels_before = r->stats.pixels;
  base_state(RT, false, PS_OFFSET);
  raster3d_begin_submission(r);
  clear_to(r, 0.1f, 0.2f, 0.3f, 1.0f);
  g_regs[0x4d8] = 1; /* src alpha / one-minus-src alpha */
  g_regs[0x4d0] = 0x8006; g_regs[0x4d1] = 0x4302; g_regs[0x4d2] = 0x4303;
  g_regs[0x4d3] = 0x8006; g_regs[0x4d4] = 0x4302; g_regs[0x4d6] = 0x4303;
  for (uint32_t t = 0; t < 8u; t++) {
    const float k = (float)t / 8.0f;
    vertex(3u * t + 0u, -1.0f + k * 0.2f, -1.0f, k, 1.0f - k, 0.5f, 0.6f);
    vertex(3u * t + 1u, 1.0f, -1.0f + k * 0.3f, 1.0f - k, k, 0.25f, 0.4f);
    vertex(3u * t + 2u, -1.0f + k, 1.0f, 0.5f, k, 1.0f - k, 0.7f);
  }
  draw_arrays(r, 4, 24);
  base_state(RT, false, PS_DERIV_OFFSET);
  vertex(0, -1.0f, -1.0f, 0.0f, 0.0f, 0, 1);
  vertex(1, 1.0f, -1.0f, 64.0f, 0.0f, 0, 1);
  vertex(2, -1.0f, 1.0f, 0.0f, 64.0f, 0, 1);
  draw_arrays(r, 4, 3); /* the upper-left half; the blended scene shows in the rest */
  raster3d_flush(r, &k_mem);
  memcpy(out, g_gpu + (RT - GPU_BASE), RT_SIZE * RT_SIZE * 4u);
  return (uint32_t)(r->stats.pixels - pixels_before);
}

static void test_worker_count_invariance(Raster3d *r) {
  static uint8_t serial[RT_SIZE * RT_SIZE * 4u], parallel[RT_SIZE * RT_SIZE * 4u];
  const uint32_t serial_pixels = render_scene(r, 1u, serial);
  CHECK(r->workers.count == 1u, "serial renderer");
  CHECK(serial_pixels > 4096u, "the scene is big enough to go parallel (%u pixels)", serial_pixels);
  static const uint32_t counts[] = {4u, 3u};
  for (uint32_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    const uint32_t pixels = render_scene(r, counts[i], parallel);
    CHECK(pixels == serial_pixels, "%u workers: %u pixels, serial %u", r->workers.count, pixels, serial_pixels);
    CHECK(memcmp(serial, parallel, sizeof(serial)) == 0, "%u workers: identical image", r->workers.count);
  }
  raster3d_set_workers(r, workers_default_count());
}

int main(void) {
  const size_t bytes = raster3d_storage_bytes();
  uint8_t *storage = (uint8_t *)malloc(bytes + 64u);
  if (!storage) {
    fprintf(stderr, "raster3d_test: no memory\n");
    return 1;
  }
  static Raster3d r;
  raster3d_init(&r, (uint8_t *)(((uintptr_t)storage + 63u) & ~(uintptr_t)63u), bytes);
  CHECK(r.ready, "renderer initialises");
  build_programs();
  test_triangle_and_blend(&r, false);
  test_triangle_and_blend(&r, true);
  test_texture(&r);
  test_render_to_texture(&r);
  test_stencil(&r);
  test_derivatives(&r);
  test_worker_count_invariance(&r);
  CHECK(r.stats.shader_faults == 0, "no shader faults");
  raster3d_shutdown(&r);
  free(storage);
  if (g_failures) {
    fprintf(stderr, "raster3d_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("raster3d_test: ok\n");
  return 0;
}
