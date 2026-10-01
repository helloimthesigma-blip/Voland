/**
 * GPU command processing (gpu/gpu_channel.h) over a flat fake GPU memory:
 * GP entries and every method-header form, the bind/semaphore/syncpoint
 * host methods, and the DMA engine's pitch <-> block-linear multi-line
 * copies, 1D copies, component remap and semaphore releases. Method
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
static const Gpu_Memory k_mem = {NULL, fake_read, fake_write, fake_syncpoint};

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

int main(void) {
  test_headers_and_host();
  test_pitch_to_block_linear();
  test_linear_and_remap();
  test_3d_sync();
  printf("[gpu_channel_test] passed\n");
  return 0;
}
