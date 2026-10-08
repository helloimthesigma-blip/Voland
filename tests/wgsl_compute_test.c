/**
 * Maxwell compute programs -> WGSL (gpu/wgsl, desc->stage SM_STAGE_COMPUTE):
 * structural checks here, and differential vectors for the browser.
 *
 * Each vector is a compute program run through the interpreter
 * (maxwell_shader.c, the reference: lane groups of a block interleaved at
 * BAR.SYNC, as raster3d_compute runs them) over global-memory windows and
 * constant buffers, and its translation. `wgsl_compute_test DIR` writes
 * DIR/<name>.wgsl and DIR/<name>.json (the data buffer, the windows before,
 * the windows the interpreter left); platform/web/tools/compute-vectors.mjs
 * dispatches each on WebGPU and compares. Without DIR only the structural
 * checks run (ctest).
 *
 * `wgsl_compute_test DIR DUMP BLOCK GRID [SHARED]...` adds a program dumped
 * by voland-cli --dump-shaders (a title's compute program, run locally on
 * random data; not part of the repository).
 */
#define CHECK_NAME "wgsl_compute_test"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/maxwell_shader.h"
#include "gpu/wgsl.h"

/* ---- assembler (field layouts as tests/raster3d_test.c) ----------- */

#define GUARD (7ull << 16)
#define RZ 0xffull
#define LDST_32 4ull
#define LDST_64 5ull
#define LDST_128 6ull
#define SR_TID_X 0x21u
#define SR_CTAID_X 0x25u

static uint64_t S2R(uint32_t d, uint32_t sr) { return (0xf0c8ull << 48) | GUARD | ((uint64_t)sr << 20) | (RZ << 8) | d; }
static uint64_t SHL_I(uint32_t d, uint32_t a, uint32_t s) {
  return (0x3848ull << 48) | GUARD | ((uint64_t)s << 20) | ((uint64_t)a << 8) | d;
}
static uint64_t IADD_R(uint32_t d, uint32_t a, uint32_t b) {
  return (0x5c10ull << 48) | GUARD | ((uint64_t)b << 20) | ((uint64_t)a << 8) | d;
}
static uint64_t MOV32I(uint32_t d, uint32_t imm) {
  return (0x0100ull << 48) | GUARD | ((uint64_t)imm << 20) | (0xfull << 12) | d;
}
/* IADD Rd = Ra + c[slot][offset] with carry out / RZ + c[slot][offset] + carry. */
static uint64_t IADD_C_CC(uint32_t d, uint32_t a, uint32_t slot, uint32_t offset) {
  return (0x4c10ull << 48) | (1ull << 47) | GUARD | ((uint64_t)slot << 34) | ((uint64_t)(offset / 4u) << 20) |
         ((uint64_t)a << 8) | d;
}
static uint64_t IADD_C_X(uint32_t d, uint32_t slot, uint32_t offset) {
  return (0x4c10ull << 48) | (1ull << 43) | GUARD | ((uint64_t)slot << 34) | ((uint64_t)(offset / 4u) << 20) |
         (RZ << 8) | d;
}
/* LDG/STG .E (64-bit address in Ra, Ra + 1) of `size`, with a byte offset. */
static uint64_t LDG_E(uint64_t size, uint32_t d, uint32_t a, uint32_t offset) {
  return (0xeed0ull << 48) | (size << 48) | (1ull << 45) | GUARD | ((uint64_t)(offset & 0xffffffu) << 20) |
         ((uint64_t)a << 8) | d;
}
static uint64_t STG_E(uint64_t size, uint32_t d, uint32_t a, uint32_t offset) {
  return (0xeed8ull << 48) | (size << 48) | (1ull << 45) | GUARD | ((uint64_t)(offset & 0xffffffu) << 20) |
         ((uint64_t)a << 8) | d;
}
static uint64_t SHARED(uint64_t top, uint32_t d, uint32_t a) {
  return (top << 48) | GUARD | (LDST_32 << 48) | ((uint64_t)a << 8) | d;
}
static uint64_t FADD_R(uint32_t d, uint32_t a, uint32_t b) {
  return (0x5c58ull << 48) | GUARD | ((uint64_t)b << 20) | ((uint64_t)a << 8) | d;
}
static uint64_t FMUL_R(uint32_t d, uint32_t a, uint32_t b) {
  return (0x5c68ull << 48) | GUARD | ((uint64_t)b << 20) | ((uint64_t)a << 8) | d;
}
static uint64_t BAR_SYNC(void) { return (0xf0a8ull << 48) | GUARD; }
static uint64_t DEPBAR(void) { return 0xf0f0000034170000ull; }
static uint64_t EXIT(void) { return (0xe300ull << 48) | GUARD | 0xfull; }

/* ---- programs ------------------------------------------------------ */

static Sm_Program g_prog;
static uint8_t g_bytes[SM_SPH_BYTES + 8u * SM_MAX_WORDS];

/* A header-less compute program (gpu/compute.h): scheduling words every
 * fourth, as compute_program_get decodes. */
static void build(const uint64_t *code, uint32_t count) {
  memset(g_bytes, 0, sizeof(g_bytes));
  uint32_t w = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (w % 4u == 0) {
      const uint64_t sched = 0x001f8000fc0007e0ull;
      memcpy(g_bytes + SM_SPH_BYTES + 8u * w++, &sched, 8);
    }
    memcpy(g_bytes + SM_SPH_BYTES + 8u * w++, &code[i], 8);
  }
  if (w % 4u == 0) w++;
  const uint64_t bra = (0xe240ull << 48) | GUARD | (0xfffff8ull << 20) | 0xfull;
  memcpy(g_bytes + SM_SPH_BYTES + 8u * w++, &bra, 8);
  sm_program_decode(g_bytes, SM_SPH_BYTES + 8u * ((w + 3u) & ~3u), 0, &g_prog);
  g_prog.header.stage = SM_STAGE_COMPUTE;
}

/* ---- the reference run --------------------------------------------- */

#define WINDOWS 3u            /* the synthetic vectors' */
#define WINDOW_BYTES (64u * 1024u)
#define WINDOW_BASE 0x1a3400000ull /* GPU VAs above 4 GiB, as SSBU's */
#define WINDOW_STRIDE 0x100000ull
#define MAX_WINDOW_BYTES (4u << 20) /* a capture's */
#define CBUF_BYTES 0x400u
#define MAX_CBUF_BYTES 0x10000u
#define DATA_WORDS (WGSL_DRAW_CONSTANT_WORDS + SM_CBUF_SLOTS * (MAX_CBUF_BYTES / 4u))
#define MAX_GROUPS (WGSL_CS_MAX_INVOCATIONS / SM_LANES)
#define SHARED_MAX WGSL_CS_MAX_SHARED_BYTES

static uint8_t g_window[WGSL_CS_MAX_WINDOWS][MAX_WINDOW_BYTES];
static uint8_t g_window_before[WGSL_CS_MAX_WINDOWS][MAX_WINDOW_BYTES];
static uint64_t g_window_base[WGSL_CS_MAX_WINDOWS];
static uint32_t g_window_bytes[WGSL_CS_MAX_WINDOWS];
static uint32_t g_windows;
static uint8_t g_cbuf[SM_CBUF_SLOTS][MAX_CBUF_BYTES];
static uint32_t g_cbuf_bytes[SM_CBUF_SLOTS];
static uint32_t g_cbuf_used; /* bit per slot bound */
static uint8_t g_shared[SHARED_MAX];

static uint64_t window_base(uint32_t k) { return WINDOW_BASE + WINDOW_STRIDE * k; }

static bool window_at(uint64_t va, uint32_t size, uint32_t *k, uint32_t *rel) {
  for (uint32_t i = 0; i < g_windows; i++) {
    if (va >= g_window_base[i] && va + size <= g_window_base[i] + g_window_bytes[i]) {
      *k = i;
      *rel = (uint32_t)(va - g_window_base[i]);
      return true;
    }
  }
  return false;
}

static bool g_read(void *user, uint64_t va, void *out, uint32_t size) {
  (void)user;
  uint32_t k, rel;
  if (!window_at(va, size, &k, &rel)) return false;
  memcpy(out, g_window[k] + rel, size);
  return true;
}

static uint64_t g_writes, g_write_misses, g_last_miss;
static bool g_write(void *user, uint64_t va, const void *src, uint32_t size) {
  (void)user;
  uint32_t k, rel;
  g_writes++;
  if (!window_at(va, size, &k, &rel)) {
    g_write_misses++;
    g_last_miss = va;
    return false;
  }
  memcpy(g_window[k] + rel, src, size);
  return true;
}

static Sm_Thread g_groups[MAX_GROUPS];
static Sm_Group_State g_states[MAX_GROUPS];

/* Every block of the grid, lane groups interleaved at barriers. */
static void reference(const uint32_t block[3], const uint32_t grid[3], uint32_t shared_bytes) {
  Sm_Env env;
  memset(&env, 0, sizeof(env));
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++) {
    if (!((g_cbuf_used >> s) & 1u)) continue;
    env.cbuf[s] = g_cbuf[s];
    env.cbuf_size[s] = g_cbuf_bytes[s];
  }
  env.global_read = g_read;
  env.global_write = g_write;
  env.shared = g_shared;
  env.shared_bytes = shared_bytes;
  const uint32_t threads = block[0] * block[1] * block[2], groups = (threads + SM_LANES - 1u) / SM_LANES;
  for (uint32_t b = 0; b < grid[0] * grid[1] * grid[2]; b++) {
    memset(g_shared, 0, sizeof(g_shared));
    bool running[MAX_GROUPS];
    for (uint32_t g = 0; g < groups; g++) {
      Sm_Thread *t = &g_groups[g];
      const uint32_t first = g * SM_LANES, lanes = threads - first < SM_LANES ? threads - first : SM_LANES;
      sm_thread_reset(t, lanes);
      for (uint32_t l = 0; l < lanes; l++) {
        const uint32_t id = first + l;
        t->tid[0][l] = id % block[0];
        t->tid[1][l] = (id / block[0]) % block[1];
        t->tid[2][l] = id / (block[0] * block[1]);
      }
      t->ctaid[0] = b % grid[0];
      t->ctaid[1] = (b / grid[0]) % grid[1];
      t->ctaid[2] = b / (grid[0] * grid[1]);
      sm_group_begin(&g_states[g], t);
      running[g] = true;
    }
    for (bool any = true; any;) {
      any = false;
      for (uint32_t g = 0; g < groups; g++) {
        if (!running[g]) continue;
        const Sm_Group_Status st = sm_group_run(&g_prog, &env, &g_groups[g], &g_states[g]);
        CHECK(st != SM_GROUP_FAULT);
        running[g] = st == SM_GROUP_BARRIER;
        any = any || running[g];
      }
    }
  }
}

/* ---- vectors -------------------------------------------------------- */

static const char *g_dir;
static bool g_after_given; /* a capture: the windows already hold the result the title's run left */
static char g_text[1u << 21];
static uint32_t g_vectors;
static uint32_t g_data[DATA_WORDS];

/* The data buffer as raster3d lays it out: constants (the window table),
 * then each bound constant buffer, located by the table. */
static void build_data(void) {
  memset(g_data, 0, sizeof(g_data));
  uint32_t at = WGSL_DRAW_CONSTANT_WORDS;
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++) {
    if (!((g_cbuf_used >> s) & 1u)) continue;
    memcpy(g_data + at, g_cbuf[s], g_cbuf_bytes[s]);
    g_data[WGSL_DRAW_CBUF_TABLE + 2u * s] = at;
    g_data[WGSL_DRAW_CBUF_TABLE + 2u * s + 1u] = g_cbuf_bytes[s] / 4u;
    at += g_cbuf_bytes[s] / 4u;
  }
  for (uint32_t k = 0; k < g_windows; k++) {
    uint32_t *w = g_data + WGSL_CS_WINDOWS + WGSL_CS_WINDOW_WORDS * k;
    w[0] = (uint32_t)g_window_base[k];
    w[1] = (uint32_t)(g_window_base[k] >> 32);
    w[2] = g_window_bytes[k];
  }
}

static void write_words(FILE *f, const uint8_t *bytes, uint32_t n) {
  fprintf(f, "[");
  for (uint32_t i = 0; i < n / 4u; i++) {
    uint32_t v;
    memcpy(&v, bytes + 4u * i, 4);
    fprintf(f, "%s%u", i ? "," : "", v);
  }
  fprintf(f, "]");
}

static void finish(const char *name, const uint32_t block[3], const uint32_t grid[3], uint32_t shared_bytes,
                   uint32_t ulps) {
  Wgsl_Program_Desc desc;
  wgsl_default_desc(&g_prog, &desc);
  desc.stage = SM_STAGE_COMPUTE;
  for (uint32_t i = 0; i < 3u; i++) desc.block[i] = (uint16_t)block[i];
  desc.shared_bytes = shared_bytes;
  desc.window_count = g_windows;
  const Wgsl_Result r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  if (!r.ok) fprintf(stderr, "[wgsl_compute_test] %s: %s\n", name, r.reason);
  CHECK(r.ok);
  CHECK(strstr(g_text, "@compute @workgroup_size") != NULL);
  for (uint32_t k = 0; k < g_windows; k++) memcpy(g_window_before[k], g_window[k], g_window_bytes[k]);
  if (!g_after_given) reference(block, grid, shared_bytes);
  build_data();
  g_vectors++;
  fprintf(stderr, "[wgsl_compute_test] %s\n", name);
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
  fprintf(f, "{\"name\":\"%s\",\"block\":[%u,%u,%u],\"grid\":[%u,%u,%u],\"ulps\":%u,\"window_binding\":%u,\"data\":", name,
          block[0], block[1], block[2], grid[0], grid[1], grid[2], ulps, WGSL_CS_WINDOW_BINDING_BASE);
  write_words(f, (const uint8_t *)g_data, sizeof(g_data));
  fprintf(f, ",\"windows\":[");
  for (uint32_t k = 0; k < g_windows; k++) {
    if (k) fprintf(f, ",");
    write_words(f, g_window_before[k], g_window_bytes[k]);
  }
  fprintf(f, "],\"expected\":[");
  for (uint32_t k = 0; k < g_windows; k++) {
    if (k) fprintf(f, ",");
    write_words(f, g_window[k], g_window_bytes[k]);
  }
  fprintf(f, "]}\n");
  fclose(f);
}

/* c[0][0x310 + 16 k]: storage buffer k's base and size, as NVN lays out a
 * compute program's descriptors. */
#define SSBO_DESC 0x310u
#define SSBO_DESC_BYTES 16u

static void reset_inputs(uint32_t seed) {
  memset(g_cbuf, 0, sizeof(g_cbuf));
  for (uint32_t s2 = 0; s2 < SM_CBUF_SLOTS; s2++) g_cbuf_bytes[s2] = CBUF_BYTES;
  g_cbuf_used = 1u;
  g_windows = WINDOWS;
  for (uint32_t k = 0; k < WINDOWS; k++) {
    g_window_base[k] = window_base(k);
    g_window_bytes[k] = WINDOW_BYTES;
  }
  for (uint32_t k = 0; k < WINDOWS; k++) {
    const uint64_t base = window_base(k);
    const uint32_t desc[3] = {(uint32_t)base, (uint32_t)(base >> 32), WINDOW_BYTES};
    memcpy(g_cbuf[0] + SSBO_DESC + SSBO_DESC_BYTES * k, desc, sizeof(desc));
  }
  srand(seed);
  for (uint32_t k = 0; k < WINDOWS; k++) {
    for (uint32_t i = 0; i < WINDOW_BYTES / 4u; i++) {
      /* Ordinary floats (and small integers in the low bits' view). */
      const float v = (float)((rand() % 4001) - 2000) / 256.0f;
      memcpy(g_window[k] + 4u * i, &v, 4);
    }
  }
}

/* out[tid] = in[tid] + in[tid] * in[tid]: per-thread global memory through
 * the NVN storage-buffer address pattern, two blocks (CTAID). */
static void vector_global(void) {
  reset_inputs(1);
  const uint64_t code[] = {
      S2R(0, SR_TID_X), S2R(1, SR_CTAID_X), SHL_I(1, 1, 6), IADD_R(0, 0, 1), /* r0 = block * 64 + tid */
      SHL_I(2, 0, 2),                                                         /* r2 = r0 * 4 */
      IADD_C_CC(4, 2, 0, SSBO_DESC), IADD_C_X(5, 0, SSBO_DESC + 4u),         /* r4:r5 = in + r2 */
      IADD_C_CC(6, 2, 0, SSBO_DESC + SSBO_DESC_BYTES), IADD_C_X(7, 0, SSBO_DESC + SSBO_DESC_BYTES + 4u),
      LDG_E(LDST_32, 8, 4, 0), FMUL_R(9, 8, 8), FADD_R(9, 9, 8), DEPBAR(),
      STG_E(LDST_32, 9, 6, 0), EXIT()};
  build(code, sizeof(code) / sizeof(code[0]));
  const uint32_t block[3] = {64, 1, 1}, grid[3] = {2, 1, 1};
  finish("cs_global", block, grid, 0, 0);
}

/* 128-bit loads and stores at an offset, and a load outside every window
 * (reads zeros) stored back. */
static void vector_wide(void) {
  reset_inputs(2);
  const uint64_t code[] = {
      S2R(0, SR_TID_X), SHL_I(2, 0, 4), /* r2 = tid * 16 */
      IADD_C_CC(4, 2, 0, SSBO_DESC), IADD_C_X(5, 0, SSBO_DESC + 4u),
      IADD_C_CC(6, 2, 0, SSBO_DESC + 2u * SSBO_DESC_BYTES), IADD_C_X(7, 0, SSBO_DESC + 2u * SSBO_DESC_BYTES + 4u),
      LDG_E(LDST_128, 8, 4, 0x40), STG_E(LDST_128, 8, 6, 0x10),
      MOV32I(12, 0x10), MOV32I(13, 0), LDG_E(LDST_64, 14, 12, 0), /* unmapped: zeros */
      STG_E(LDST_64, 14, 6, 0), EXIT()};
  build(code, sizeof(code) / sizeof(code[0]));
  const uint32_t block[3] = {32, 1, 1}, grid[3] = {1, 1, 1};
  finish("cs_wide", block, grid, 0, 0);
}

/* shared[tid] = tid; BAR.SYNC; out[tid] = shared[63 - tid] - across lane
 * groups, so only a working barrier gives the expected values. */
static void vector_shared_barrier(void) {
  reset_inputs(3);
  const uint32_t threads = 64;
  const uint64_t code[] = {
      S2R(0, SR_TID_X), SHL_I(1, 0, 2),     /* r1 = tid * 4 */
      SHARED(0xef58, 0, 1),                 /* shared[tid] = tid */
      BAR_SYNC(),
      MOV32I(4, (threads - 1u) * 4u), (0x5c10ull << 48) | GUARD | (1ull << 49) | (4ull << 20) | (1ull << 8) | 3ull, /* r3 = r4 - r1 */
      SHARED(0xef48, 5, 3),                 /* r5 = shared[63 - tid] */
      IADD_C_CC(6, 1, 0, SSBO_DESC + SSBO_DESC_BYTES), IADD_C_X(7, 0, SSBO_DESC + SSBO_DESC_BYTES + 4u),
      STG_E(LDST_32, 5, 6, 0), EXIT()};
  build(code, sizeof(code) / sizeof(code[0]));
  const uint32_t block[3] = {threads, 1, 1}, grid[3] = {1, 1, 1};
  finish("cs_shared_barrier", block, grid, threads * 4u, 0);
}

static void structural(void) {
  reset_inputs(4);
  const uint64_t code[] = {S2R(0, SR_TID_X), EXIT()};
  build(code, 2);
  Wgsl_Program_Desc desc;
  wgsl_default_desc(&g_prog, &desc);
  desc.stage = SM_STAGE_COMPUTE;
  desc.block[0] = 512;
  desc.block[1] = desc.block[2] = 1;
  CHECK(!wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text)).ok); /* over WebGPU's 256 invocations */
  desc.block[0] = 32;
  desc.window_count = WGSL_CS_MAX_WINDOWS + 1u;
  CHECK(!wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text)).ok);
  desc.window_count = 1;
  const Wgsl_Result r = wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text));
  CHECK(r.ok && strstr(g_text, "workgroupBarrier") != NULL && strstr(g_text, "workgroupUniformLoad") != NULL);
  /* A texture instruction stays on the CPU. */
  const uint64_t tex[] = {(0xd800ull << 48) | GUARD | (1ull << 53) | (4ull << 50), EXIT()};
  build(tex, 2);
  CHECK(!wgsl_translate(&g_prog, &desc, g_text, sizeof(g_text)).ok);
}

/* A dumped program (voland-cli --dump-shaders): "N OP ... raw HEX" lines
 * after a "stage 6 words N" line. */
static bool load_dump(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  static uint64_t words[SM_MAX_WORDS];
  memset(words, 0, sizeof(words));
  char line[512];
  uint32_t max = 0;
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return false;
  }
  while (fgets(line, sizeof(line), f)) {
    unsigned idx;
    char op[32];
    const char *r = strstr(line, "raw ");
    if (!r || sscanf(line, "%u %31s", &idx, op) != 2 || idx >= SM_MAX_WORDS) continue;
    words[idx] = strtoull(r + 4, NULL, 16);
    if (idx + 1u > max) max = idx + 1u;
  }
  fclose(f);
  memset(g_bytes, 0, sizeof(g_bytes));
  memcpy(g_bytes + SM_SPH_BYTES, words, 8u * ((max + 3u) & ~3u));
  sm_program_decode(g_bytes, SM_SPH_BYTES + 8u * ((max + 3u) & ~3u), 0, &g_prog);
  g_prog.header.stage = SM_STAGE_COMPUTE;
  return true;
}

/* Random inputs for a title's program: every constant buffer small
 * integers (offsets and counts stay small), the storage-buffer
 * descriptors at c[0][0x310] pointing at the windows. */
static void dump_vector(const char *path, uint32_t threads, uint32_t blocks, uint32_t shared_bytes, uint32_t index) {
  CHECK(load_dump(path));
  reset_inputs(100u + index);
  g_cbuf_used = 0;
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++) {
    if (!((g_prog.cbuf_used >> s) & 1u)) continue;
    g_cbuf_used |= 1u << s;
    for (uint32_t i = 0; i < CBUF_BYTES / 4u; i++) {
      const uint32_t v = (uint32_t)(rand() % 16);
      memcpy(g_cbuf[s] + 4u * i, &v, 4);
    }
  }
  g_cbuf_used |= 1u;
  for (uint32_t k = 0; k < WINDOWS; k++) {
    const uint64_t base = window_base(k);
    const uint32_t desc[3] = {(uint32_t)base, (uint32_t)(base >> 32), WINDOW_BYTES};
    memcpy(g_cbuf[0] + SSBO_DESC + SSBO_DESC_BYTES * k, desc, sizeof(desc));
  }
  char name[64];
  snprintf(name, sizeof(name), "cs_dump%u", index);
  const uint32_t block[3] = {threads, 1, 1}, grid[3] = {blocks, 1, 1};
  finish(name, block, grid, shared_bytes, 4u); /* MUFU and friends: a few ULPs */
}

/* A dispatch captured by voland-cli (VOLAND_COMPUTE_CAPTURE): the
 * interpreter is replayed on the captured inputs and must reproduce the
 * captured result exactly; then the vector is written with it. */
static bool read_exact(FILE *f, void *out, size_t n) { return fread(out, 1, n, f) == n; }
#define CAPTURE_SENTINEL 0x7fc0deadu


static void capture_vector(const char *path, uint32_t index) {
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  char magic[4];
  uint32_t head[8];
  CHECK(read_exact(f, magic, 4) && !memcmp(magic, "VCC1", 4) && read_exact(f, head, sizeof(head)));
  static uint8_t code[0x8000];
  CHECK(head[7] <= sizeof(code) && read_exact(f, code, head[7]));
  memset(g_bytes, 0, sizeof(g_bytes));
  memcpy(g_bytes + SM_SPH_BYTES, code, head[7]);
  const uint32_t extent = sm_program_extent(g_bytes, SM_SPH_BYTES + head[7]);
  sm_program_decode(g_bytes, extent ? extent : SM_SPH_BYTES + head[7], 0, &g_prog);
  g_prog.header.stage = SM_STAGE_COMPUTE;
  memset(g_cbuf_bytes, 0, sizeof(g_cbuf_bytes));
  g_cbuf_used = 0;
  uint32_t slots = 0;
  CHECK(read_exact(f, &slots, 4));
  for (uint32_t i = 0; i < slots; i++) {
    uint32_t s2 = 0, bytes = 0;
    CHECK(read_exact(f, &s2, 4) && read_exact(f, &bytes, 4) && s2 < SM_CBUF_SLOTS && bytes <= MAX_CBUF_BYTES);
    CHECK(read_exact(f, g_cbuf[s2], bytes));
    g_cbuf_bytes[s2] = bytes;
    g_cbuf_used |= 1u << s2;
  }
  static uint8_t after[WGSL_CS_MAX_WINDOWS][MAX_WINDOW_BYTES];
  for (uint32_t pass = 0; pass < 2u; pass++) {
    uint32_t count = 0;
    CHECK(read_exact(f, &count, 4) && count <= WGSL_CS_MAX_WINDOWS);
    g_windows = count;
    for (uint32_t k = 0; k < count; k++) {
      CHECK(read_exact(f, &g_window_base[k], 8) && read_exact(f, &g_window_bytes[k], 4));
      CHECK(g_window_bytes[k] <= MAX_WINDOW_BYTES);
      CHECK(read_exact(f, pass ? after[k] : g_window[k], g_window_bytes[k]));
    }
  }
  fclose(f);
  const uint32_t block[3] = {head[0], head[1], head[2]}, grid[3] = {head[3], head[4], head[5]};
  /* The interpreter on the captured inputs gives the captured result. */
  for (uint32_t k = 0; k < g_windows; k++) memcpy(g_window_before[k], g_window[k], g_window_bytes[k]);
  reference(block, grid, head[6]);
  fprintf(stderr, "[wgsl_compute_test] capture: %llu writes, %llu outside the windows (last 0x%llx); windows:", (unsigned long long)g_writes,
          (unsigned long long)g_write_misses, (unsigned long long)g_last_miss);
  for (uint32_t k = 0; k < g_windows; k++) fprintf(stderr, " 0x%llx+0x%x", (unsigned long long)g_window_base[k], g_window_bytes[k]);
  fprintf(stderr, "\n");
  for (uint32_t k = 0; k < g_windows; k++) CHECK(!memcmp(g_window[k], after[k], g_window_bytes[k]));
  /* The title's run left this frame's result where last frame's already
   * was: the windows the program writes start filled with a sentinel
   * instead, so every word it writes shows. */
  bool written[WGSL_CS_MAX_WINDOWS] = {false};
  for (uint32_t k = 0; k < g_windows; k++) {
    for (uint32_t j = 0; j < g_windows; j++) memcpy(g_window[j], g_window_before[j], g_window_bytes[j]);
    for (uint32_t i = 0; i < g_window_bytes[k] / 4u; i++) memcpy(g_window[k] + 4u * i, &(uint32_t){CAPTURE_SENTINEL}, 4);
    reference(block, grid, head[6]);
    for (uint32_t i = 0; i < g_window_bytes[k] / 4u && !written[k]; i++) {
      uint32_t v;
      memcpy(&v, g_window[k] + 4u * i, 4);
      written[k] = v != CAPTURE_SENTINEL;
    }
  }
  for (uint32_t k = 0; k < g_windows; k++) {
    memcpy(g_window[k], g_window_before[k], g_window_bytes[k]);
    if (!written[k]) continue;
    for (uint32_t i = 0; i < g_window_bytes[k] / 4u; i++) memcpy(g_window[k] + 4u * i, &(uint32_t){CAPTURE_SENTINEL}, 4);
  }
  char name[64];
  snprintf(name, sizeof(name), "cs_capture%u", index);
  finish(name, block, grid, head[6], 4u);
}

int main(int argc, char **argv) {
  g_dir = argc > 1 ? argv[1] : NULL;
  structural();
  vector_global();
  vector_wide();
  vector_shared_barrier();
  for (int i = 2; i + 1 < argc && !strcmp(argv[i], "capture"); i += 2) capture_vector(argv[i + 1], (uint32_t)(i - 2) / 2u);
  for (int i = 2; i + 2 < argc && strcmp(argv[i], "capture"); i += 4) {
    const uint32_t shared_bytes = i + 3 < argc ? (uint32_t)atoi(argv[i + 3]) : 0u;
    dump_vector(argv[i], (uint32_t)atoi(argv[i + 1]), (uint32_t)atoi(argv[i + 2]), shared_bytes, (uint32_t)(i - 2) / 4u);
  }
  printf("[wgsl_compute_test] passed (%u vectors%s%s)\n", g_vectors, g_dir ? " written to " : "", g_dir ? g_dir : "");
  return 0;
}
