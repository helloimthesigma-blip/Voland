/**
 * GPU command processing (gpu/gpu_channel.h) over a flat fake GPU memory:
 * GP entries and every method-header form, the bind/semaphore/syncpoint
 * host methods, and the DMA engine's pitch <-> block-linear multi-line
 * copies, 1D copies, component remap and semaphore releases, and the 2D
 * engine's surface copies. Method
 * encodings are restated from NVIDIA's published class headers.
 */
#define CHECK_NAME "gpu_channel_test"
#include "check.h"

#include "gpu/block_linear.h"
#include "gpu/gpu_channel.h"

#include <string.h>

#define MEM_BASE 0x100000ull
#define MEM_BYTES 0x80000u
static uint8_t g_mem[MEM_BYTES];
static uint32_t g_syncpoint_increments[8];
static Gpu_Channel g_channel;

static bool fake_read(void *user, uint64_t va, void *out, uint64_t size) {
  (void)user;
  if (va < MEM_BASE || va - MEM_BASE + size > MEM_BYTES) return false;
  memcpy(out, g_mem + (va - MEM_BASE), size);
  return true;
}
static bool fake_write(void *user, uint64_t va, const void *src, uint64_t size) {
  (void)user;
  if (va < MEM_BASE || va - MEM_BASE + size > MEM_BYTES) return false;
  memcpy(g_mem + (va - MEM_BASE), src, size);
  return true;
}
static void fake_syncpoint(void *user, uint32_t id) {
  (void)user;
  if (id < 8) g_syncpoint_increments[id]++;
}
static const Gpu_Memory k_mem = {NULL, fake_read, fake_write, fake_syncpoint, NULL};

/* Pushbuffer builder. */
static uint32_t g_pb[0x400];
static uint32_t g_pb_len;
static uint32_t header(uint32_t op, uint32_t sub, uint32_t method, uint32_t count) {
  return (op << 29) | (count << 16) | (sub << 13) | method;
}
static void inc(uint32_t sub, uint32_t method, const uint32_t *data, uint32_t n) {
  g_pb[g_pb_len++] = header(1, sub, method, n);
  for (uint32_t i = 0; i < n; i++) g_pb[g_pb_len++] = data[i];
}
static void one(uint32_t sub, uint32_t method, uint32_t data) { inc(sub, method, &data, 1); }
static void imm(uint32_t sub, uint32_t method, uint32_t data) { g_pb[g_pb_len++] = header(4, sub, method, data); }
/* Non-incrementing: every word to the same method (upload ports). */
static void port(uint32_t sub, uint32_t method, const uint32_t *data, uint32_t n) {
  g_pb[g_pb_len++] = header(3, sub, method, n);
  for (uint32_t i = 0; i < n; i++) g_pb[g_pb_len++] = data[i];
}

#define PB_VA (MEM_BASE + 0x70000u)
static void submit(void) {
  memcpy(g_mem + (PB_VA - MEM_BASE), g_pb, g_pb_len * 4u);
  const uint64_t entry = PB_VA | ((uint64_t)g_pb_len << 42);
  const uint64_t entries[2] = {0 /* control entry: no fetch */, entry};
  gpu_channel_submit(&g_channel, &k_mem, entries, 2);
  g_pb_len = 0;
}

/* DMA register word addresses. */
#define R(byte) ((byte) / 4u)
#define LAUNCH R(0x300)

static void set_offsets(uint64_t in, uint64_t out) {
  const uint32_t regs[4] = {(uint32_t)(in >> 32), (uint32_t)in, (uint32_t)(out >> 32), (uint32_t)out};
  inc(1, R(0x400), regs, 4);
}

static void test_headers_and_host(void) {
  gpu_channel_init(&g_channel);
  one(1, 0, 0xB0B5);                 /* bind DMA on subchannel 1 */
  one(2, 0, 0xB197);                 /* 3D on 2 */
  imm(1, R(0x41C), 0x1FFF);          /* immediate form: 13-bit data */
  /* Non-incrementing and increment-once forms land where they should. */
  g_pb[g_pb_len++] = header(3, 1, R(0x410), 2); /* PITCH_IN twice */
  g_pb[g_pb_len++] = 7;
  g_pb[g_pb_len++] = 9;
  g_pb[g_pb_len++] = header(5, 1, R(0x418), 3); /* LINE_LENGTH_IN, then LINE_COUNT x2 */
  g_pb[g_pb_len++] = 11;
  g_pb[g_pb_len++] = 12;
  g_pb[g_pb_len++] = 13;
  g_pb[g_pb_len++] = 0;              /* a NOP word */
  one(2, 0x100, 0x1234);             /* a 3D method: ignored */
  /* Host semaphore release (4-byte) and a syncpoint increment. */
  const uint32_t sem[3] = {0, (uint32_t)(MEM_BASE + 0x100), 0xCAFE};
  inc(0, 4, sem, 3);
  one(0, 7, 2u | (1u << 24));
  one(0, 0x1D, 1u | (5u << 8));
  submit();
  CHECK(g_channel.subchannel_class[1] == GPU_CLASS_DMA && g_channel.subchannel_class[2] == GPU_CLASS_3D);
  CHECK(g_channel.methods >= 3);
  CHECK(g_channel.dma[R(0x410)] == 9 && g_channel.dma[R(0x418)] == 11 && g_channel.dma[R(0x41C)] == 13);
  CHECK(g_channel.ignored_methods == 1 && g_channel.engine3d[0x100] == 0x1234);
  uint32_t v;
  memcpy(&v, g_mem + 0x100, 4);
  CHECK(v == 0xCAFE && g_syncpoint_increments[5] == 1);
}

#define SRC (MEM_BASE + 0x1000)
#define DST (MEM_BASE + 0x20000)
#define W 128u /* bytes per line */
#define H 24u

static void test_pitch_to_block_linear(void) {
  for (uint32_t i = 0; i < W * H; i++) g_mem[(SRC - MEM_BASE) + i] = (uint8_t)(i * 3u + 1u);
  memset(g_mem + (DST - MEM_BASE), 0, 0x10000);
  set_offsets(SRC, DST);
  const uint32_t pitches[4] = {W, 0, W, H}; /* PITCH_IN, PITCH_OUT, LINE_LENGTH_IN, LINE_COUNT */
  inc(1, R(0x410), pitches, 4);
  const uint32_t dst_surface[5] = {1u << 4 /* 2-GOB blocks */, W, H, 1, 0};
  inc(1, R(0x70C), dst_surface, 5);
  one(1, R(0x720), 0);
  /* Semaphore: four-word release of 0x55 at +0x200. */
  const uint32_t sem[3] = {0, (uint32_t)(MEM_BASE + 0x200), 0x55};
  inc(1, R(0x240), sem, 3);
  one(1, LAUNCH, 2u | (2u << 3) | (1u << 7) /* src pitch */ | (1u << 9) /* multi-line */);
  submit();
  for (uint32_t y = 0; y < H; y++) {
    for (uint32_t x = 0; x < W; x++) {
      CHECK(g_mem[(DST - MEM_BASE) + block_linear_offset(x, y, W, 1)] == (uint8_t)((y * W + x) * 3u + 1u));
    }
  }
  uint32_t v[4];
  memcpy(v, g_mem + 0x200, sizeof(v));
  CHECK(v[0] == 0x55 && v[1] == 0 && v[2] == 0 && v[3] == 0);

  /* And back: block-linear (origin 16,8) -> pitch, 32x8 window. */
  memset(g_mem + (SRC - MEM_BASE), 0, W * H);
  set_offsets(DST, SRC);
  const uint32_t window[4] = {0, 32, 32, 8};
  inc(1, R(0x410), window, 4);
  const uint32_t src_surface[5] = {1u << 4, W, H, 1, 0};
  inc(1, R(0x728), src_surface, 5);
  one(1, R(0x73C), 16u | (8u << 16));
  one(1, LAUNCH, 2u | (1u << 8) /* dst pitch */ | (1u << 9));
  submit();
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      CHECK(g_mem[(SRC - MEM_BASE) + y * 32 + x] == (uint8_t)(((y + 8) * W + x + 16) * 3u + 1u));
    }
  }
  CHECK(g_channel.dma_copies == 2 && g_channel.faults == 0);
}

static void test_linear_and_remap(void) {
  /* 1D: 100 KB, larger than one staging line. */
  for (uint32_t i = 0; i < 0x19000; i++) g_mem[0x1000 + i] = (uint8_t)(i ^ (i >> 8));
  set_offsets(MEM_BASE + 0x1000, MEM_BASE + 0x40000);
  one(1, R(0x418), 0x19000);
  one(1, LAUNCH, 2u | (1u << 7) | (1u << 8));
  submit();
  CHECK(memcmp(g_mem + 0x1000, g_mem + 0x40000, 0x19000) == 0);

  /* Remap: 4-byte components, 2 source -> 3 destination: (B, A, CONST_A). */
  const uint32_t src_words[4] = {0x11, 0x22, 0x33, 0x44}; /* two elements: (11,22), (33,44) */
  memcpy(g_mem + 0x1000, src_words, sizeof(src_words));
  memset(g_mem + 0x2000, 0xEE, 0x40);
  set_offsets(MEM_BASE + 0x1000, MEM_BASE + 0x2000);
  one(1, R(0x700), 0x77);
  one(1, R(0x708), 1u | (0u << 4) | (4u << 8) | (3u << 16) | (1u << 20) | (2u << 24));
  one(1, R(0x418), 2);
  one(1, LAUNCH, 2u | (1u << 7) | (1u << 8) | (1u << 10));
  submit();
  uint32_t out[6];
  memcpy(out, g_mem + 0x2000, sizeof(out));
  CHECK(out[0] == 0x22 && out[1] == 0x11 && out[2] == 0x77 && out[3] == 0x44 && out[4] == 0x33 && out[5] == 0x77);

  /* An unmapped source is a fault, not a crash. */
  set_offsets(0x10, MEM_BASE);
  one(1, R(0x418), 16);
  one(1, LAUNCH, 2u | (1u << 7) | (1u << 8));
  submit();
  CHECK(g_channel.faults == 1);
}

static void test_3d_sync(void) {
  /* deko3d's fence signal: SYNCPT_ACTION increment, then a one-word
   * REPORT_SEMAPHORE release; plus a four-word counter report. */
  one(2, 0xB2, 3u | (1u << 16));
  const uint32_t report[4] = {0, (uint32_t)(MEM_BASE + 0x300), 0x77, 0u | (1u << 28)};
  inc(2, 0x6C0, report, 4);
  memset(g_mem + 0x310, 0xAA, 16);
  const uint32_t counter[4] = {0, (uint32_t)(MEM_BASE + 0x310), 0x99, 2u};
  inc(2, 0x6C0, counter, 4);
  const uint64_t ignored = g_channel.ignored_methods;
  one(2, 0x35E, 1); /* a draw-state register: kept, otherwise ignored */
  submit();
  uint32_t v[4];
  memcpy(v, g_mem + 0x300, 4);
  CHECK(v[0] == 0x77 && g_syncpoint_increments[3] == 1);
  memcpy(v, g_mem + 0x310, sizeof(v));
  CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0);
  CHECK(g_channel.engine3d[0x35E] == 1 && g_channel.ignored_methods > ignored);
}

/* MME instruction encoders (Fermi macro ISA field layout). */
static uint32_t mme_alu(uint32_t assign, uint32_t dst, uint32_t a, uint32_t b, uint32_t alu, bool end) {
  return 0u | (assign << 4) | ((uint32_t)end << 7) | (dst << 8) | (a << 11) | (b << 14) | (alu << 17);
}
static uint32_t mme_addi(uint32_t assign, uint32_t dst, uint32_t a, int32_t imm, bool end) {
  return 1u | (assign << 4) | ((uint32_t)end << 7) | (dst << 8) | (a << 11) | (((uint32_t)imm & 0x3FFFFu) << 14);
}
static uint32_t mme_branch(uint32_t a, bool not_zero, bool no_delay, int32_t offset, bool end) {
  return 7u | ((uint32_t)not_zero << 4) | ((uint32_t)no_delay << 5) | ((uint32_t)end << 7) | (a << 11) |
         (((uint32_t)offset & 0x3FFFFu) << 14);
}
static uint32_t mme_state(uint32_t assign, uint32_t dst, uint32_t a, int32_t imm) {
  return 5u | (assign << 4) | (dst << 8) | (a << 11) | (((uint32_t)imm & 0x3FFFFu) << 14);
}

/* A macro that sums its parameters (count in r1) into SET_MME_SHADOW_SCRATCH(0)
 * via the method port, and one that loops emitting an incrementing method. */
static void test_mme(void) {
  /* macro 0 at ip 0:
   *   0: r2 = 0                     (move)
   *   1: r3 = fetch                 loop: next param
   *   2: r2 = r2 + r3               (alu add, move)
   *   3: r1 = r1 + -1               (addi, move)
   *   4: branch r1 != 0 -> -3 (to 1), delayed
   *   5: (delay slot) r4 = r4 + 1   counts iterations
   *   6: set method = 0xD00 (scratch 0): r5 = 0 + 0xD00, move+set method
   *   7: send r2 (move+send), exit
   *   8: send r4 (exit delay slot: emits to scratch 1)                    */
  const uint32_t macro0[] = {
      mme_addi(1, 2, 0, 0, false),
      mme_alu(0, 3, 0, 0, 0, false),
      mme_alu(1, 2, 2, 3, 0, false),
      mme_addi(1, 1, 1, -1, false),
      mme_branch(1, true, false, -3, false),
      mme_addi(1, 4, 4, 1, false),
      mme_addi(2, 5, 0, 0xD00 | (1 << 12), false),
      mme_alu(4, 0, 2, 0, 0, true),
      mme_alu(4, 0, 4, 0, 0, false),
  };
  /* macro 1 at ip 16: reads register 0xD00 via STATE and writes it + 1 to 0xD02. */
  const uint32_t macro1[] = {
      mme_addi(2, 6, 0, 0xD02, false),
      mme_state(1, 7, 0, 0xD00),
      mme_addi(4, 0, 7, 1, true),
      mme_alu(1, 0, 0, 0, 0, false),
  };
  one(2, 0x45, 0);                                       /* instruction RAM pointer */
  port(2, 0x46, macro0, sizeof(macro0) / 4);
  one(2, 0x45, 16);
  port(2, 0x46, macro1, sizeof(macro1) / 4);
  one(2, 0x47, 0);                                       /* start address RAM */
  const uint32_t starts[2] = {0, 16};
  port(2, 0x48, starts, 2);
  /* CALL_MME_MACRO(0) with count 4, then CALL_MME_DATA(0) x4 (increment-once header). */
  g_pb[g_pb_len++] = (5u << 29) | (5u << 16) | (2u << 13) | 0xE00;
  const uint32_t params[5] = {4, 10, 20, 30, 40};
  for (int i = 0; i < 5; i++) g_pb[g_pb_len++] = params[i];
  one(2, 0xE02, 0);                                      /* CALL_MME_MACRO(1): ends macro 0's data */
  submit();                                              /* end of submission runs macro 1 */
  CHECK(g_channel.engine3d[0xD00] == 100 && g_channel.engine3d[0xD01] == 4);
  CHECK(g_channel.engine3d[0xD02] == 101 && g_channel.mme_runs == 2 && g_channel.mme_faults == 0);

  /* A counted fill loop whose exit-flagged send sits in a taken branch's
   * delay slot (deko3d's FillRegisters): every iteration must send.
   *   0: maddr = r1; r2 = fetch      (count)
   *   1: r3 = fetch                  (value)
   *   2: r2 = r2 - 1                 loop:
   *   3: branch r2 != 0 -> 2, delayed
   *   4: send r3, exit               (delay slot)
   *   5: nop                         (exit delay slot)          */
  const uint32_t fill[] = {
      mme_addi(5, 2, 1, 0, false),
      mme_alu(0, 3, 0, 0, 0, false),
      mme_addi(1, 2, 2, -1, false),
      mme_branch(2, true, false, -1, false),
      mme_alu(4, 0, 3, 0, 0, true),
      mme_alu(1, 0, 0, 0, 0, false),
  };
  one(2, 0x45, 48);
  port(2, 0x46, fill, sizeof(fill) / 4);
  one(2, 0x47, 3);
  one(2, 0x48, 48);
  g_pb[g_pb_len++] = (5u << 29) | (3u << 16) | (2u << 13) | 0xE06; /* CALL_MME_MACRO(3) + 2 data words */
  const uint32_t fill_params[3] = {0xD10u | (1u << 12), 4, 7};
  for (int i = 0; i < 3; i++) g_pb[g_pb_len++] = fill_params[i];
  submit();
  CHECK(g_channel.engine3d[0xD10] == 7 && g_channel.engine3d[0xD13] == 7 && g_channel.engine3d[0xD14] == 0);

  /* A runaway macro (branch to itself, no exit) is cut off and counted. */
  const uint32_t spin[] = {mme_branch(0, false, true, 0, false)};
  one(2, 0x45, 32);
  port(2, 0x46, spin, 1);
  one(2, 0x47, 2);
  one(2, 0x48, 32);
  one(2, 0xE04, 0);
  submit();
  CHECK(g_channel.mme_faults == 1);

  /* Constant buffer upload: selector (size, address), offset, data. */
  const uint32_t cb[4] = {0x100, 0, (uint32_t)(MEM_BASE + 0x800), 8};
  inc(2, 0x8E0, cb, 4);
  const uint32_t words[3] = {0x11111111, 0x22222222, 0x33333333};
  g_pb[g_pb_len++] = header(3, 2, 0x8E4, 3); /* non-incrementing: offset advances itself */
  for (int i = 0; i < 3; i++) g_pb[g_pb_len++] = words[i];
  submit();
  uint32_t got[3];
  memcpy(got, g_mem + 0x808, sizeof(got));
  CHECK(got[0] == 0x11111111 && got[1] == 0x22222222 && got[2] == 0x33333333 && g_channel.engine3d[0x8E3] == 20);
}


/* Inline-to-memory (3D class methods 0x60-0x6D, and the I2M class):
 * LAUNCH_DMA then LOAD_INLINE_DATA words land in pitch and block-linear
 * destinations; the pipeline bind groups power up per stage. */
static void test_inline_to_memory(void) {
  gpu_channel_init(&g_channel);
  CHECK(g_channel.engine3d[0x804 + 16u * 5u] == 4u && g_channel.engine3d[0x804 + 16u * 1u] == 0u);
  /* SET_PIPELINE_SHADER power-on: each slot's type, only the vertex stage
   * (slot 1) enabled - NVN never writes slot 1's control word. */
  CHECK(g_channel.engine3d[0x800 + 16u * 1u] == 0x11u && g_channel.engine3d[0x800 + 16u * 5u] == 0x50u);
  CHECK(g_channel.engine3d[0x800] == 0x00u);
  one(2, 0, 0xB197);
  /* Pitch: 2 lines of 6 bytes, pitch 0x40, at MEM_BASE + 0x2000. */
  const uint64_t dst = MEM_BASE + 0x2000u;
  memset(g_mem + 0x2000, 0xEE, 0x100);
  const uint32_t setup[5] = {6, 2, (uint32_t)(dst >> 32), (uint32_t)dst, 0x40};
  inc(2, 0x60, setup, 5);
  one(2, 0x6C, 1); /* LAUNCH_DMA: pitch destination */
  const uint32_t data[3] = {0x04030201u, 0x08070605u, 0x0C0B0A09u};
  port(2, 0x6D, data, 3);
  submit();
  CHECK(memcmp(g_mem + 0x2000, "\x01\x02\x03\x04\x05\x06", 6) == 0 && g_mem[0x2006] == 0xEE);
  CHECK(memcmp(g_mem + 0x2040, "\x07\x08\x09\x0A\x0B\x0C", 6) == 0);
  CHECK(g_channel.i2m_uploads == 1);
  /* Block linear through the I2M class: 64-byte rows, 2 rows at y=0. */
  one(3, 0, 0xA140);
  memset(g_mem + 0x3000, 0, 0x800);
  const uint64_t bl = MEM_BASE + 0x3000u;
  const uint32_t bl_setup[7] = {64, 2, (uint32_t)(bl >> 32), (uint32_t)bl, 0, 0 /* block: 1 GOB */, 64 /* width bytes */};
  inc(3, 0x60, bl_setup, 7);
  one(3, 0x6C, 0); /* block-linear destination */
  uint32_t words[32];
  for (uint32_t i = 0; i < 32u; i++) words[i] = 0x01010101u * (i + 1u);
  port(3, 0x6D, words, 32);
  submit();
  /* Row 1, byte 0 lives at the GOB's (x=0, y=1) slot. */
  CHECK(g_mem[0x3000 + block_linear_offset(0, 0, 64, 0)] == 1);
  CHECK(g_mem[0x3000 + block_linear_offset(0, 1, 64, 0)] == 17);
  CHECK(g_mem[0x3000 + block_linear_offset(63, 1, 64, 0)] == 32);
}

/* 2D engine (902D) PIXELS_FROM_MEMORY: a pitch -> block-linear upload at
 * 1:1 into a sub-rectangle (NVN's texture uploads), then a 2x downscale
 * back to pitch with an R/B swap (A8B8G8R8 -> A8R8G8B8). Register offsets
 * per cl902d.h. */
#define B2_SRC (MEM_BASE + 0x20000u)
#define B2_DST (MEM_BASE + 0x30000u)
#define B2_OUT (MEM_BASE + 0x40000u)
#define B2_W 24u
#define B2_H 6u
#define B2_DST_W 32u
#define B2_DST_H 8u
#define B2_X0 4u
#define B2_Y0 1u
#define FMT_A8B8G8R8 0xD5u
#define FMT_A8R8G8B8 0xCFu
#define HALF_TEXEL 0x80000000u

static void texel(uint32_t x, uint32_t y, uint8_t out[4]) {
  out[0] = (uint8_t)x;
  out[1] = (uint8_t)y;
  out[2] = (uint8_t)(x ^ y);
  out[3] = (uint8_t)(0x80u + x);
}

static void blit_surface(uint32_t method, uint32_t format, bool pitch, uint32_t pitch_bytes, uint32_t w, uint32_t h,
                         uint64_t va) {
  const uint32_t regs[10] = {format, pitch ? 1u : 0u, 1u << 4 /* 2-GOB blocks */, 1, 0, pitch_bytes, w, h,
                             (uint32_t)(va >> 32), (uint32_t)va};
  inc(3, method, regs, 10);
}

static void blit(uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, uint32_t scale) {
  const uint32_t params[12] = {dx, dy, w, h, 0, scale, 0, scale, HALF_TEXEL, 0, HALF_TEXEL, 0};
  one(3, R(0x2AC), 3);        /* SRCCOPY */
  one(3, R(0x88C), 1);        /* corner origin, point filter */
  inc(3, R(0x8B0), params, 12); /* ... SRC_Y0_INT launches */
}

static void test_2d_blit(void) {
  one(3, 0, 0x902D);
  for (uint32_t y = 0; y < B2_H; y++)
    for (uint32_t x = 0; x < B2_W; x++) texel(x, y, g_mem + (B2_SRC - MEM_BASE) + (y * B2_W + x) * 4u);
  memset(g_mem + (B2_DST - MEM_BASE), 0xEE, 0x8000);
  blit_surface(R(0x230), FMT_A8B8G8R8, true, B2_W * 4u, B2_W, B2_H, B2_SRC);
  blit_surface(R(0x200), FMT_A8B8G8R8, false, 0, B2_DST_W, B2_DST_H, B2_DST);
  blit(B2_X0, B2_Y0, B2_W, B2_H, 1);
  submit();
  CHECK(g_channel.blits == 1 && g_channel.faults == 0);
  for (uint32_t y = 0; y < B2_DST_H; y++) {
    for (uint32_t x = 0; x < B2_DST_W; x++) {
      const uint8_t *p = g_mem + (B2_DST - MEM_BASE) + block_linear_offset(x * 4u, y, B2_DST_W * 4u, 1);
      const bool inside = x >= B2_X0 && x < B2_X0 + B2_W && y >= B2_Y0 && y < B2_Y0 + B2_H;
      uint8_t want[4] = {0xEE, 0xEE, 0xEE, 0xEE};
      if (inside) texel(x - B2_X0, y - B2_Y0, want);
      CHECK(memcmp(p, want, 4) == 0);
    }
  }
  /* Half size: every other texel of the uploaded 32x8 image, R/B swapped. */
  blit_surface(R(0x230), FMT_A8B8G8R8, false, 0, B2_DST_W, B2_DST_H, B2_DST);
  blit_surface(R(0x200), FMT_A8R8G8B8, true, (B2_DST_W / 2u) * 4u, B2_DST_W / 2u, B2_DST_H / 2u, B2_OUT);
  blit(0, 0, B2_DST_W / 2u, B2_DST_H / 2u, 2);
  submit();
  CHECK(g_channel.blits == 2 && g_channel.faults == 0);
  for (uint32_t y = 0; y < B2_DST_H / 2u; y++) {
    for (uint32_t x = 0; x < B2_DST_W / 2u; x++) {
      const uint8_t *src = g_mem + (B2_DST - MEM_BASE) + block_linear_offset(2u * x * 4u, 2u * y, B2_DST_W * 4u, 1);
      const uint8_t *out = g_mem + (B2_OUT - MEM_BASE) + (y * (B2_DST_W / 2u) + x) * 4u;
      CHECK(out[0] == src[2] && out[1] == src[1] && out[2] == src[0] && out[3] == src[3]);
    }
  }
}

int main(void) {
  test_headers_and_host();
  test_pitch_to_block_linear();
  test_linear_and_remap();
  test_3d_sync();
  test_mme();
  test_inline_to_memory();
  test_2d_blit();
  printf("[gpu_channel_test] passed\n");
  return 0;
}
