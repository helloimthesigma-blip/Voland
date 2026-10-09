/**
 * The boot-path services over real IPC (§12): vi + the BufferQueue down to
 * composited pixels, fsp-srv on the SD card, time (calendar math and the
 * clock shared memory), set:sys, apm, and am's message queue and storages.
 * Request layouts are libnx's (its services and display sources), restated.
 */
#define CHECK_NAME "services_test"
#include "check.h"

#include "audio/audio_ring.h"
#include "gpu/block_linear.h"
#include "gpu/framebuffer.h"
#include "hle/kernel/handle_table.h"
#include "ipc_fixtures.h"

#include <string.h>

static Emulator g_emu;
static uint8_t g_reply[TEST_IPC_BUFFER_BYTES];
static uint64_t g_scratch; /* guest scratch: [g_scratch, +0x40000) */

#define SCRATCH(off) (g_scratch + (off))

static Test_Ipc_Reply call_ex(uint32_t handle, uint32_t command, const void *payload, uint32_t size,
                              const Test_Ipc_Message *extra, uint32_t expect_result) {
  Test_Ipc_Message m;
  if (extra) m = *extra;
  else memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = command;
  m.payload = payload;
  m.payload_size = size;
  CHECK(ipc_fixture_send(&g_emu, handle, &m, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_CMIF, false, 0, &reply);
  CHECK(reply.sfco_ok);
  if (reply.result != expect_result) {
    fprintf(stderr, "[services_test] command %u: result 0x%x, expected 0x%x\n", command, reply.result, expect_result);
    CHECK(reply.result == expect_result);
  }
  return reply;
}

static Test_Ipc_Reply call(uint32_t handle, uint32_t command, const void *payload, uint32_t size,
                           const Test_Ipc_Message *extra) {
  return call_ex(handle, command, payload, size, extra, 0);
}

static uint32_t object(uint32_t handle, uint32_t command, const void *payload, uint32_t size) {
  const Test_Ipc_Reply reply = call(handle, command, payload, size, NULL);
  CHECK(reply.move_count == 1);
  return reply.move_handles[0];
}

static uint32_t service(const char *name) {
  static uint32_t sm;
  if (!sm) {
    CHECK(ipc_fixture_connect(&g_emu, "sm:", &sm) == 0);
    Test_Ipc_Message m;
    memset(&m, 0, sizeof(m));
    m.send_pid = true;
    (void)call(sm, 0, NULL, 0, &m);
  }
  uint64_t wire = 0;
  memcpy(&wire, name, strlen(name));
  return object(sm, 1, &wire, sizeof(wire));
}

static Test_Ipc_Message with_x(uint64_t gva, uint64_t size) {
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.statics[0] = (Test_Ipc_Buffer){gva, size, 0};
  m.static_count = 1;
  return m;
}

static uint64_t rd64(uint64_t gva) {
  uint64_t v = 0;
  CHECK_OK(vmm_read64(g_emu.vmm, gva, &v));
  return v;
}
static uint32_t rd32(uint64_t gva) {
  uint32_t v = 0;
  CHECK_OK(vmm_read32(g_emu.vmm, gva, &v));
  return v;
}

/* ------------------------------------------------------------------ */
/* vi + IGraphicBufferProducer.                                        */
/* ------------------------------------------------------------------ */

#define SURFACE_W 64u
#define SURFACE_H 16u
#define SURFACE_BH 4u
#define PARCEL_IN SCRATCH(0x1000)
#define PARCEL_OUT SCRATCH(0x2000)
#define SURFACE SCRATCH(0x10000)

typedef struct Parcel {
  uint8_t data[0x400];
  uint32_t size;
} Parcel;

static void p_i32(Parcel *p, int32_t v) {
  memcpy(p->data + 0x10 + p->size, &v, 4);
  p->size += 4;
}
static void p_bytes(Parcel *p, const void *b, uint32_t n) {
  memcpy(p->data + 0x10 + p->size, b, n);
  p->size += (n + 3u) & ~3u;
}
static void p_token(Parcel *p) {
  static const char k_token[] = "android.gui.IGraphicBufferProducer";
  p_i32(p, 0x100);
  p_i32(p, (int32_t)strlen(k_token));
  uint16_t wide[sizeof(k_token)];
  for (size_t i = 0; i < sizeof(k_token); i++) wide[i] = (uint16_t)k_token[i];
  p_bytes(p, wide, sizeof(wide));
}

/* One TransactParcel; returns the reply payload (status is its last i32). */
static const uint8_t *transact(uint32_t relay, int32_t binder, uint32_t code, Parcel *p, uint32_t *payload_size) {
  const uint32_t header[4] = {p->size, 0x10, 0, 0x10 + p->size};
  memcpy(p->data, header, sizeof(header));
  CHECK_OK(vmm_write_block(g_emu.vmm, PARCEL_IN, p->data, 0x10 + p->size));
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.sends[0] = (Test_Ipc_Buffer){PARCEL_IN, 0x10 + p->size, 0};
  m.send_count = 1;
  m.receives[0] = (Test_Ipc_Buffer){PARCEL_OUT, 0x400, 0};
  m.receive_count = 1;
  const uint32_t in[3] = {(uint32_t)binder, code, 0};
  (void)call(relay, 3, in, sizeof(in), &m);
  static uint8_t out[0x400];
  CHECK_OK(vmm_read_block(g_emu.vmm, PARCEL_OUT, out, sizeof(out)));
  uint32_t size = 0, off = 0;
  memcpy(&size, out, 4);
  memcpy(&off, out + 4, 4);
  *payload_size = size;
  return out + off;
}

static int32_t status_of(const uint8_t *payload, uint32_t size) {
  int32_t v;
  memcpy(&v, payload + size - 4, 4);
  return v;
}

static void test_vi(void) {
  /* An nvmap buffer over guest scratch (nvdrv has its own test). */
  Nvmap_Handle *h = &g_emu.nvdrv.handles[0];
  memset(h, 0, sizeof(*h));
  h->references = 1;
  h->allocated = true;
  h->address = SURFACE;
  h->size = 0x10000;

  const uint32_t root = service("vi:u");
  const uint32_t policy = 0;
  const uint32_t display = object(root, 0, &policy, sizeof(policy));
  const uint32_t relay = object(display, 100, NULL, 0);
  Test_Ipc_Reply r = call(display, 1011, NULL, 0, NULL);
  CHECK(test_le64(r.data) == VI_DEFAULT_DISPLAY_ID);
  r = call(display, 1102, &policy, sizeof(policy), NULL);
  CHECK(test_le64(r.data) == 1280 && test_le64(r.data + 8) == 720);

  /* OpenLayer: the native window parcel names the binder. */
  uint8_t open_in[0x50];
  memset(open_in, 0, sizeof(open_in));
  memcpy(open_in, "Default", 7);
  open_in[0x40] = 1; /* layer id 1 */
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.send_pid = true;
  m.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x3000), 0x100, 0};
  m.receive_count = 1;
  r = call(display, 2020, open_in, sizeof(open_in), &m);
  CHECK(test_le64(r.data) >= 0x1C);
  const uint32_t payload_off = rd32(SCRATCH(0x3000) + 4);
  const int32_t binder = (int32_t)rd32(SCRATCH(0x3000) + payload_off + 8);
  CHECK(binder >= 1);
  /* The objects table lists the binder (offset 0): the Nintendo SDK's
   * Parcel::readStrongBinder needs it, and OpenParcel checks that data +
   * objects fit the returned size. */
  const uint32_t data_size = rd32(SCRATCH(0x3000)), objects_size = rd32(SCRATCH(0x3000) + 8);
  const uint32_t objects_off = rd32(SCRATCH(0x3000) + 12);
  CHECK(objects_size == 4 && rd32(SCRATCH(0x3000) + objects_off) == 0);
  CHECK(test_le64(r.data) >= 0x10u + data_size + objects_size);

  uint32_t n = 0;
  Parcel p;
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 0);
  p_i32(&p, 2); /* NATIVE_WINDOW_API_CPU */
  p_i32(&p, 0);
  const uint8_t *out = transact(relay, binder, 10 /* CONNECT */, &p, &n);
  CHECK(n == 0x14 && status_of(out, n) == 0);

  /* SET_PREALLOCATED_BUFFER slot 0: GBFR header + NvGraphicBuffer ints. */
  uint8_t gbfr[0x28 + 0x144];
  memset(gbfr, 0, sizeof(gbfr));
  const uint32_t head[10] = {0x47424652u, SURFACE_W, SURFACE_H, SURFACE_W, 1, 0xB00, 42, 0, 0, 0x144 / 4};
  memcpy(gbfr, head, sizeof(head));
  uint8_t *ints = gbfr + 0x28;
  const uint32_t nvmap_id = 1, format = 1;
  memcpy(ints + 0x04, &nvmap_id, 4);
  memcpy(ints + 0x1C, &format, 4);
  const uint32_t plane[10] = {SURFACE_W, SURFACE_H, 0, 0, 3 /* block-linear */, SURFACE_W * 4u, 0, 0, 0, SURFACE_BH};
  memcpy(ints + 0x34, plane, 8);                 /* width, height */
  memcpy(ints + 0x34 + 0x10, &plane[4], 8);      /* layout, pitch */
  memcpy(ints + 0x34 + 0x24, &plane[9], 4);      /* block_height_log2 */
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 0);
  p_i32(&p, 1);
  p_i32(&p, (int32_t)sizeof(gbfr));
  p_i32(&p, 0);
  p_bytes(&p, gbfr, sizeof(gbfr));
  out = transact(relay, binder, 14, &p, &n);
  CHECK(status_of(out, n) == 0);

  /* The release event: signalled while a buffer is free. */
  const uint32_t native_in[2] = {(uint32_t)binder, 0x0F};
  r = call(relay, 2, native_in, sizeof(native_in), NULL);
  CHECK(r.copy_count == 1);
  Kernel_Event *release = handle_table_get(&g_emu.process.handles, r.copy_handles[0], KERNEL_OBJECT_EVENT_READABLE);
  CHECK(release && release->signaled);

  /* DEQUEUE (async): slot 0 with an empty fence; then nothing is free. */
  memset(&p, 0, sizeof(p));
  p_token(&p);
  const int32_t dq[5] = {1, SURFACE_W, SURFACE_H, 1, 0xB00};
  for (int i = 0; i < 5; i++) p_i32(&p, dq[i]);
  Parcel again = p;
  out = transact(relay, binder, 3, &p, &n);
  int32_t v[4];
  memcpy(v, out, sizeof(v));
  CHECK(v[0] == 0 && v[1] == 1 && v[2] == 0x24 && v[3] == 0 && status_of(out, n) == 0);
  CHECK(!release->signaled);
  out = transact(relay, binder, 3, &again, &n);
  CHECK(status_of(out, n) == -11); /* WouldBlock */

  /* REQUEST_BUFFER echoes the graphic buffer. */
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 0);
  out = transact(relay, binder, 1, &p, &n);
  memcpy(v, out, 8);
  CHECK(v[0] == 1 && v[1] == (int32_t)sizeof(gbfr) && status_of(out, n) == 0);

  /* Draw a gradient in block-linear and queue it. */
  static uint8_t linear[SURFACE_W * 4 * SURFACE_H], tiled[0x8000];
  for (uint32_t y = 0; y < SURFACE_H; y++) {
    for (uint32_t x = 0; x < SURFACE_W; x++) {
      uint8_t *px = linear + (y * SURFACE_W + x) * 4u;
      px[0] = (uint8_t)(x * 4);
      px[1] = (uint8_t)(y * 16);
      px[2] = 0x80;
      px[3] = 0xFF;
    }
  }
  CHECK(block_linear_size(SURFACE_W * 4u, SURFACE_H, SURFACE_BH) == sizeof(tiled));
  pitch_to_block_linear(linear, SURFACE_W * 4u, tiled, SURFACE_W * 4u, SURFACE_H, SURFACE_BH);
  CHECK_OK(vmm_write_block(g_emu.vmm, SURFACE, tiled, sizeof(tiled)));
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 0);
  uint8_t input[0x54];
  memset(input, 0, sizeof(input));
  p_i32(&p, (int32_t)sizeof(input));
  p_i32(&p, 0);
  p_bytes(&p, input, sizeof(input));
  out = transact(relay, binder, 7 /* QUEUE */, &p, &n);
  memcpy(v, out, sizeof(v));
  CHECK(v[0] == 1280 && v[1] == 720 && v[3] == 1 && status_of(out, n) == 0);

  /* Vsync composites it into the §6 slot. */
  framebuffer_reset();
  const uint64_t presented = g_emu.vi.frames_presented;
  vi_update(&g_emu.vi, &g_emu.hle, g_emu.vi.next_vsync_ticks);
  CHECK(g_emu.vi.frames_presented == presented + 1 && framebuffer_published() == 1);
  /* Frame skip: clamped, carried to vi, and off clears the renderer's flag. */
  emulator_set_frame_skip(&g_emu, 9);
  CHECK(g_emu.frame_skip == EMULATOR_MAX_FRAME_SKIP && g_emu.vi.frame_skip == EMULATOR_MAX_FRAME_SKIP);
  g_emu.renderer.skip_draws = true;
  emulator_set_frame_skip(&g_emu, 0);
  CHECK(g_emu.vi.frame_skip == 0 && !g_emu.renderer.skip_draws);
  const uint8_t *region = (const uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base;
  const uint8_t *pixels = region + LAYOUT_FRAMEBUFFER_HEADER_BYTES;
  uint32_t meta[4];
  memcpy(meta, region + FRAMEBUFFER_OFFSET_METADATA, sizeof(meta));
  CHECK(meta[0] == SURFACE_W && meta[1] == SURFACE_H && meta[2] == SURFACE_W * 4u && meta[3] == FRAMEBUFFER_FORMAT_RGBA8);
  CHECK(memcmp(pixels, linear, sizeof(linear)) == 0);
  CHECK(!release->signaled); /* the only buffer is on screen */

  /* QueueBufferInput's crop picks the shown rectangle and FLIP_V flips
   * it: slot 1 over the same memory, cropped to x 2..10, y 1..5. */
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 1);
  p_i32(&p, 1);
  p_i32(&p, (int32_t)sizeof(gbfr));
  p_i32(&p, 0);
  p_bytes(&p, gbfr, sizeof(gbfr));
  out = transact(relay, binder, 14, &p, &n);
  CHECK(status_of(out, n) == 0);
  memset(&p, 0, sizeof(p));
  p_token(&p);
  for (int i = 0; i < 5; i++) p_i32(&p, dq[i]);
  out = transact(relay, binder, 3, &p, &n);
  memcpy(v, out, sizeof(v));
  CHECK(v[0] == 1 && status_of(out, n) == 0);
  memset(&p, 0, sizeof(p));
  p_token(&p);
  p_i32(&p, 1);
  const int32_t crop[4] = {2, 1, 10, 5};
  const uint32_t flip_v = 2;
  memset(input, 0, sizeof(input));
  memcpy(input + 12, crop, sizeof(crop));
  memcpy(input + 32, &flip_v, 4);
  p_i32(&p, (int32_t)sizeof(input));
  p_i32(&p, 0);
  p_bytes(&p, input, sizeof(input));
  out = transact(relay, binder, 7, &p, &n);
  CHECK(status_of(out, n) == 0);
  vi_update(&g_emu.vi, &g_emu.hle, g_emu.vi.next_vsync_ticks);
  CHECK(framebuffer_published() == 2); /* frame 2 lands in slot 1 */
  memcpy(meta, region + FRAMEBUFFER_OFFSET_METADATA + FRAMEBUFFER_METADATA_BYTES, sizeof(meta));
  CHECK(meta[0] == 8 && meta[1] == 4 && meta[2] == 8u * 4u);
  const uint8_t *cropped = pixels + LAYOUT_FRAMEBUFFER_SLOT_BYTES;
  for (uint32_t y = 0; y < 4; y++)
    for (uint32_t x = 0; x < 8; x++) {
      const uint8_t *got = cropped + (y * 8u + x) * 4u;
      const uint8_t *want = linear + ((4u - y) * SURFACE_W + (x + 2u)) * 4u; /* rows 4..1 */
      CHECK(memcmp(got, want, 4) == 0);
    }

  /* SET_BUFFER_COUNT: only slots below the count are dequeued (NVN
   * preallocates more buffers than it activates). With the count at 1 and
   * slot 0 on screen, the free slot 1 is not handed out. */
  {
    memset(&p, 0, sizeof(p));
    p_token(&p);
    p_i32(&p, 1);
    out = transact(relay, binder, 2 /* SET_BUFFER_COUNT */, &p, &n);
    CHECK(status_of(out, n) == 0 && g_emu.vi.layers[0].buffer_count == 1u);
    for (uint32_t i = 0; i < VI_MAX_SLOTS; i++)
      if (g_emu.vi.layers[0].slots[i].preallocated) g_emu.vi.layers[0].slots[i].state = VI_SLOT_FREE;
    g_emu.vi.layers[0].slots[0].state = VI_SLOT_PRESENTED; /* slot 0 busy, slot 1 free but beyond the count */
    memset(&p, 0, sizeof(p));
    p_token(&p);
    for (int i = 0; i < 5; i++) p_i32(&p, dq[i]);
    out = transact(relay, binder, 3, &p, &n);
    CHECK(status_of(out, n) == -11); /* WouldBlock: nothing below the count is free */
    g_emu.vi.layers[0].buffer_count = 0;
  }

  /* Vsync event; the scheduler is told when the next one is. */
  r = call(display, 5202, &policy, sizeof(policy), NULL);
  CHECK(r.copy_count == 1);
  CHECK(vi_next_wake(&g_emu.vi) == g_emu.vi.next_vsync_ticks);
}

/* ------------------------------------------------------------------ */
/* fsp-srv.                                                            */
/* ------------------------------------------------------------------ */

static void test_fs(void) {
  const uint32_t fsp = service("fsp-srv");
  const uint64_t pid = 0;
  (void)call(fsp, 1, &pid, sizeof(pid), NULL);
  const uint32_t sd = object(fsp, 18, NULL, 0);
  const uint64_t path = SCRATCH(0x4000);
  CHECK_OK(vmm_write_block(g_emu.vmm, path, "/save", 6));
  Test_Ipc_Message x = with_x(path, 0x301);
  (void)call(sd, 2, NULL, 0, &x);
  (void)call_ex(sd, 2, NULL, 0, &x, FS_RESULT_PATH_ALREADY_EXISTS);
  CHECK_OK(vmm_write_block(g_emu.vmm, path, "/save/data.bin", 15));
  const struct { uint32_t option, pad; uint64_t size; } create = {0, 0, 4};
  (void)call(sd, 0, &create, sizeof(create), &x);
  Test_Ipc_Reply r = call(sd, 7, NULL, 0, &x);
  CHECK(test_le32(r.data) == 1); /* file */

  const uint32_t mode = 3 | 4; /* read, write, append */
  r = call(sd, 8, &mode, sizeof(mode), &x);
  CHECK(r.move_count == 1);
  const uint32_t file = r.move_handles[0];
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x5000), "hello, switch", 13));
  const struct { uint32_t option, pad; uint64_t offset, size; } write = {0, 0, 2, 13};
  Test_Ipc_Message a;
  memset(&a, 0, sizeof(a));
  a.sends[0] = (Test_Ipc_Buffer){SCRATCH(0x5000), 13, 0};
  a.send_count = 1;
  (void)call(file, 1, &write, sizeof(write), &a);
  r = call(file, 4, NULL, 0, NULL);
  CHECK(test_le64(r.data) == 15);
  const struct { uint32_t option, pad; uint64_t offset, size; } read = {0, 0, 0, 64};
  Test_Ipc_Message b;
  memset(&b, 0, sizeof(b));
  b.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x6000), 64, 0};
  b.receive_count = 1;
  r = call(file, 0, &read, sizeof(read), &b);
  CHECK(test_le64(r.data) == 15);
  char back[16] = {0};
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0x6000), back, 15));
  CHECK(back[0] == 0 && back[1] == 0 && memcmp(back + 2, "hello, switch", 13) == 0);
  /* Deleting an open file is refused. */
  (void)call_ex(sd, 1, NULL, 0, &x, FS_RESULT_TARGET_LOCKED);

  /* The directory lists it with its size. */
  CHECK_OK(vmm_write_block(g_emu.vmm, path, "/save", 6));
  const uint32_t dir_mode = 3;
  Test_Ipc_Message xd = with_x(path, 0x301);
  r = call(sd, 9, &dir_mode, sizeof(dir_mode), &xd);
  const uint32_t dir = r.move_handles[0];
  r = call(dir, 1, NULL, 0, NULL);
  CHECK(test_le64(r.data) == 1);
  memset(&b, 0, sizeof(b));
  b.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x7000), 0x310 * 2, 0};
  b.receive_count = 1;
  r = call(dir, 0, NULL, 0, &b);
  CHECK(test_le64(r.data) == 1);
  char name[16] = {0};
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0x7000), name, 9));
  CHECK(strcmp(name, "data.bin") == 0 && rd64(SCRATCH(0x7000) + 0x308) == 15);

  /* Saves: same attribute, same filesystem; the NCA fixture has no RomFS. */
  uint8_t save_in[0x48];
  memset(save_in, 0, sizeof(save_in));
  save_in[0] = 1;
  const uint32_t save_a = object(fsp, 51, save_in, sizeof(save_in));
  (void)save_a;

  /* Title saves: program id 0 means the running program, so two titles
   * never share a save. A save made before that (keyed with program id 0)
   * is taken over by the first title that opens it, committed archive
   * included. */
  {
    const uint64_t saved_program = g_emu.fs.program_id;
    g_emu.fs.program_id = 0x0100abcd00010000ull;
    uint8_t legacy[FS_SAVE_ATTRIBUTE_BYTES];
    memset(legacy, 0, sizeof(legacy));
    legacy[0x10] = 0x77;   /* user id */
    legacy[0x20] = 1;      /* SaveDataType Account */
    uint32_t legacy_root = 0, root = 0;
    CHECK(fs_save_root(&g_emu.fs, 1, legacy, true, &legacy_root) == 0);
    CHECK(fs_commit_save(&g_emu.fs, legacy_root) == 0);
    const uint64_t commits = g_emu.fs.save_commits;
    uint8_t open_in[0x48];
    memset(open_in, 0, sizeof(open_in));
    open_in[0] = 1;
    memcpy(open_in + 8, legacy, sizeof(legacy));
    (void)object(fsp, 51, open_in, sizeof(open_in));
    uint8_t keyed[FS_SAVE_ATTRIBUTE_BYTES];
    memcpy(keyed, legacy, sizeof(keyed));
    for (uint32_t i = 0; i < 8; i++) keyed[i] = (uint8_t)(g_emu.fs.program_id >> (8u * i));
    CHECK(fs_save_root(&g_emu.fs, 1, keyed, false, &root) == 0 && root == legacy_root);
    CHECK(fs_save_root(&g_emu.fs, 1, legacy, false, &root) == FS_RESULT_TARGET_NOT_FOUND);
    CHECK(g_emu.fs.save_commits == commits + 1u); /* the archive was renamed */
    /* Another title gets a save of its own. */
    g_emu.fs.program_id = 0x0100abcd00020000ull;
    (void)object(fsp, 51, open_in, sizeof(open_in));
    for (uint32_t i = 0; i < 8; i++) keyed[i] = (uint8_t)(g_emu.fs.program_id >> (8u * i));
    CHECK(fs_save_root(&g_emu.fs, 1, keyed, false, &root) == 0 && root != legacy_root);
    g_emu.fs.program_id = saved_program;
  }
  (void)call_ex(fsp, 200, NULL, 0, NULL, FS_RESULT_TARGET_NOT_FOUND);
  /* IDeviceOperator: SD card in, no game card. */
  const uint32_t device = object(fsp, 400, NULL, 0);
  CHECK(call(device, 0, NULL, 0, NULL).data[0] == 1);
  CHECK(test_le64(call(device, 3, NULL, 0, NULL).data) == EMULATOR_RAMFS_BYTES);
  CHECK(call(device, 200, NULL, 0, NULL).data[0] == 0);
}

/* ------------------------------------------------------------------ */
/* time, set:sys, apm, am.                                             */
/* ------------------------------------------------------------------ */

static void test_time(void) {
  const Time_Calendar cal = time_to_calendar(TIME_DEFAULT_RTC);
  CHECK(cal.year == 2026 && cal.month == 1 && cal.day == 1 && cal.hour == 0 && cal.weekday == 4 && cal.yearday == 0);
  const Time_Calendar leap = time_to_calendar(951782400); /* 2000-02-29 */
  CHECK(leap.year == 2000 && leap.month == 2 && leap.day == 29 && leap.weekday == 2 && leap.yearday == 59);
  CHECK(time_from_calendar(1969, 12, 31, 23, 59, 59) == -1);
  CHECK(time_to_calendar(-1).year == 1969);

  const uint32_t time = service("time:u");
  const uint32_t user_clock = object(time, 0, NULL, 0);
  Test_Ipc_Reply r = call(user_clock, 0, NULL, 0, NULL);
  CHECK((int64_t)test_le64(r.data) == TIME_DEFAULT_RTC + (int64_t)(g_emu.scheduler.ticks / 19200000u));
  const uint32_t zone = object(time, 3, NULL, 0);
  const uint64_t stamp = 1767225600ull + 3600ull * 13 + 61;
  r = call(zone, 101, &stamp, sizeof(stamp), NULL);
  CHECK(test_le32(r.data) == (2026u | (1u << 16) | (1u << 24)) && r.data[4] == 13 && r.data[5] == 1 && r.data[6] == 1);

  /* The clock shared memory, mapped read-only: the context offset is the RTC. */
  r = call(time, 20, NULL, 0, NULL);
  CHECK(r.copy_count == 1);
  const Address_Space *as = &g_emu.process.address_space;
  const uint64_t view = as->aslr.base + as->aslr.size - 0x10000000ull;
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = r.copy_handles[0];
  regs->x[1] = view;
  regs->x[2] = 0x1000;
  regs->x[3] = 1;
  hle_on_svc(g_emu.cpu_state, 0x13, &g_emu.hle);
  CHECK(regs->x[0] == 0);
  const uint32_t counter = rd32(view + TIME_SHMEM_LOCAL_CONTEXT);
  const uint64_t copy = view + TIME_SHMEM_LOCAL_CONTEXT + 8 + (counter & 1u) * 0x20u;
  CHECK((int64_t)rd64(copy) == TIME_DEFAULT_RTC);
  /* The continuous adjustment (0x38 bytes per copy): no adjustment (five
   * zero words), then the steady clock's source id - the one every
   * context's time point carries (the steady context's and the local's). */
  const uint32_t adj_counter = rd32(view + TIME_SHMEM_CONTINUOUS_ADJUSTMENT);
  const uint64_t adj = view + TIME_SHMEM_CONTINUOUS_ADJUSTMENT + 8 + (adj_counter & 1u) * 0x38u;
  for (uint32_t w = 0; w < 5u; w++) CHECK(rd64(adj + 8u * w) == 0);
  const uint32_t steady_counter = rd32(view + TIME_SHMEM_STEADY);
  const uint64_t steady = view + TIME_SHMEM_STEADY + 8 + (steady_counter & 1u) * 0x18u;
  CHECK(rd64(adj + 0x28) == rd64(steady + 8) && rd64(adj + 0x30) == rd64(steady + 16));
  CHECK(rd64(adj + 0x28) == rd64(copy + 0x10) && rd64(adj + 0x30) == rd64(copy + 0x18));
}

static void test_set_apm_am(void) {
  const uint32_t setsys = service("set:sys");
  Test_Ipc_Message c;
  memset(&c, 0, sizeof(c));
  c.receive_list[0] = (Test_Ipc_Buffer){SCRATCH(0x8000), 0x100, 0};
  c.receive_list_count = 1;
  (void)call(setsys, 3, NULL, 0, &c);
  CHECK(rd32(SCRATCH(0x8000)) == SET_FIRMWARE_MAJOR);

  const uint32_t apm = service("apm");
  const uint32_t session = object(apm, 0, NULL, 0);
  const uint32_t set_cfg[2] = {0, 0x92220007u};
  (void)call(session, 0, set_cfg, sizeof(set_cfg), NULL);
  Test_Ipc_Reply r = call(session, 1, set_cfg, 4, NULL);
  CHECK(test_le32(r.data) == 0x92220007u);

  const uint32_t oe = service("appletOE");
  const uint64_t reserved = 0;
  const uint32_t proxy = object(oe, 0, &reserved, sizeof(reserved));
  const uint32_t getter = object(proxy, 0, NULL, 0);
  r = call(getter, 0, NULL, 0, NULL);
  Kernel_Event *messages = handle_table_get(&g_emu.process.handles, r.copy_handles[0], KERNEL_OBJECT_EVENT_READABLE);
  CHECK(messages && messages->signaled);
  r = call(getter, 1, NULL, 0, NULL);
  CHECK(test_le32(r.data) == AM_MESSAGE_FOCUS_STATE_CHANGED && !messages->signaled);
  (void)call_ex(getter, 1, NULL, 0, NULL, AM_RESULT_NO_MESSAGES);
  r = call(getter, 9, NULL, 0, NULL);
  CHECK(r.data[0] == AM_FOCUS_IN_FOCUS);

  /* GetSaveDataSize: a non-zero capacity and journal (titles check it). */
  const uint32_t app = object(proxy, 20, NULL, 0);
  uint8_t save_args[24];
  memset(save_args, 0, sizeof(save_args));
  save_args[0] = 1; /* account save */
  r = call(app, 26, save_args, sizeof(save_args), NULL);
  uint64_t sizes[2];
  memcpy(sizes, r.data, sizeof(sizes));
  CHECK(sizes[0] == AM_SAVE_DATA_SIZE && sizes[1] == AM_SAVE_JOURNAL_SIZE);

  /* Storage: create, write, read back; out of bounds refused. */
  const uint32_t creator = object(proxy, 11, NULL, 0);
  const uint64_t size = 16;
  const uint32_t storage = object(creator, 10, &size, sizeof(size));
  const uint32_t accessor = object(storage, 0, NULL, 0);
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x9000), "0123456789", 10));
  Test_Ipc_Message a;
  memset(&a, 0, sizeof(a));
  a.sends[0] = (Test_Ipc_Buffer){SCRATCH(0x9000), 10, 0};
  a.send_count = 1;
  const uint64_t offset = 4;
  (void)call(accessor, 10, &offset, sizeof(offset), &a);
  const uint64_t far = 10;
  (void)call_ex(accessor, 10, &far, sizeof(far), &a, AM_RESULT_OUT_OF_BOUNDS);
  Test_Ipc_Message b;
  memset(&b, 0, sizeof(b));
  b.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xA000), 16, 0};
  b.receive_count = 1;
  const uint64_t zero = 0;
  (void)call(accessor, 11, &zero, sizeof(zero), &b);
  char back[17] = {0};
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0xA000), back, 16));
  CHECK(memcmp(back + 4, "0123456789", 10) == 0 && back[0] == 0);
  /* Closing the IStorage frees its slot. */
  uint32_t live = 0;
  for (uint32_t i = 0; i < AM_STORAGE_CAPACITY; i++) live += g_emu.am.storages[i].in_use;
  CHECK(live == 1);
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = storage;
  hle_on_svc(g_emu.cpu_state, 0x16, &g_emu.hle);
  live = 0;
  for (uint32_t i = 0; i < AM_STORAGE_CAPACITY; i++) live += g_emu.am.storages[i].in_use;
  CHECK(regs->x[0] == 0 && live == 0);
}


/* audren:u with libnx's REV4 update layout: one mono PCM16 voice into the
 * final mix (left x1.0, right x0.5) and a stereo device sink. */
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void putf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }


/* Guest-write mirroring support (§15): the SD manifest lists files with a
 * version that changes on every write, and the generation counter moves. */
static void test_sd_manifest(void) {
  CHECK_OK(emulator_sd_card_write_file(&g_emu, "/mirror/a.txt", "hello", 5));
  CHECK_OK(emulator_sd_card_write_file(&g_emu, "/mirror/sub dir/b.bin", "xy", 2));
  const uint64_t g0 = emulator_sd_card_generation(&g_emu);
  static char manifest[0x4000];
  const uint64_t need = emulator_sd_card_manifest(&g_emu, NULL, 0);
  CHECK(need > 0 && need < sizeof(manifest));
  CHECK(emulator_sd_card_manifest(&g_emu, manifest, sizeof(manifest)) == need);
  manifest[need] = '\0';
  CHECK(strstr(manifest, " 5 /mirror/a.txt\n") != NULL);
  CHECK(strstr(manifest, " 2 /mirror/sub dir/b.bin\n") != NULL);
  char before[64] = {0};
  const char *line = strstr(manifest, "/mirror/a.txt");
  const char *start = line;
  while (start > manifest && start[-1] != '\n') start--;
  memcpy(before, start, (size_t)(line - start));
  /* Rewrite: the version (and generation) change. */
  CHECK_OK(emulator_sd_card_write_file(&g_emu, "/mirror/a.txt", "HELLO!", 6));
  CHECK(emulator_sd_card_generation(&g_emu) != g0);
  const uint64_t need2 = emulator_sd_card_manifest(&g_emu, manifest, sizeof(manifest));
  manifest[need2] = '\0';
  CHECK(strstr(manifest, " 6 /mirror/a.txt\n") != NULL && strstr(manifest, before) == NULL);
  char data[16] = {0};
  CHECK(emulator_sd_card_read_file(&g_emu, "/mirror/a.txt", data, sizeof(data)) == 6 && memcmp(data, "HELLO!", 6) == 0);
  CHECK(emulator_sd_card_read_file(&g_emu, "/mirror/missing", data, sizeof(data)) == -1);

  /* Saves share the host file API under "save:SS:<attribute hex>/...":
   * the manifest lists them, writes create the save, reads find it. */
  char save_path[0x200];
  char key_hex[2 * FS_SAVE_ATTRIBUTE_BYTES + 1];
  memset(key_hex, '0', sizeof(key_hex) - 1u);
  key_hex[sizeof(key_hex) - 1u] = '\0';
  key_hex[0] = 'a';
  key_hex[1] = '5'; /* attribute byte 0 = 0xA5 */
  snprintf(save_path, sizeof(save_path), "save:03:%s/progress/slot1.dat", key_hex);
  CHECK_OK(emulator_sd_card_write_file(&g_emu, save_path, "SAVE", 4));
  uint8_t key[FS_SAVE_ATTRIBUTE_BYTES];
  memset(key, 0, sizeof(key));
  key[0] = 0xA5;
  uint32_t root = 0, node = 0;
  CHECK(fs_save_root(&g_emu.fs, 3, key, false, &root) == 0);
  CHECK(ramfs_lookup(&g_emu.ramfs, root, "/progress/slot1.dat", &node) == 0 && g_emu.ramfs.nodes[node].size == 4);
  const uint64_t need3 = emulator_sd_card_manifest(&g_emu, manifest, sizeof(manifest));
  CHECK(need3 < sizeof(manifest));
  manifest[need3] = '\0';
  char expect[0x200];
  snprintf(expect, sizeof(expect), " 4 %s\n", save_path);
  CHECK(strstr(manifest, expect) != NULL);
  memset(data, 0, sizeof(data));
  CHECK(emulator_sd_card_read_file(&g_emu, save_path, data, sizeof(data)) == 4 && memcmp(data, "SAVE", 4) == 0);
  /* Reads never create a save; malformed save paths are rejected. */
  snprintf(save_path, sizeof(save_path), "save:04:%s/progress/slot1.dat", key_hex);
  CHECK(emulator_sd_card_read_file(&g_emu, save_path, data, sizeof(data)) == -1);
  CHECK(fs_save_root(&g_emu.fs, 4, key, false, &root) != 0);
  CHECK(!error_is_ok(emulator_sd_card_write_file(&g_emu, "save:03:zz/x", "x", 1)));
  snprintf(save_path, sizeof(save_path), "save:03:%sX", key_hex);
  CHECK(!error_is_ok(emulator_sd_card_write_file(&g_emu, save_path, "x", 1)));
}


/* ns:am2 (no installed titles), pdm:qry (no history), pm:shell (no other
 * application), fsp-srv save-data info readers (empty). */
/* svcSetThreadActivity / svcGetThreadContext3: a created thread can be
 * paused (never picked) and resumed, and its context read back - what
 * Unity's garbage collector does to every thread. */
static void test_thread_activity(void) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  const uint64_t entry = g_emu.process.address_space.code.base + 0x40u;
  const uint64_t stack_top = g_emu.process.main_thread_stack.base + g_emu.process.main_thread_stack.size;
  regs->x[1] = entry;
  regs->x[2] = 0x1234;
  regs->x[3] = stack_top - 0x1000u;
  regs->x[4] = 44;
  regs->x[5] = 0;
  hle_on_svc(g_emu.cpu_state, 0x08, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
  const uint32_t thread = (uint32_t)regs->x[1];
  Sched_Thread *t = (Sched_Thread *)handle_table_get(&g_emu.process.handles, thread, KERNEL_OBJECT_THREAD);
  CHECK(t != NULL);
  regs->x[0] = thread;
  regs->x[1] = 1; /* paused */
  hle_on_svc(g_emu.cpu_state, 0x32, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0 && t->paused);
  regs->x[0] = thread;
  regs->x[1] = 7; /* not an activity */
  hle_on_svc(g_emu.cpu_state, 0x32, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == HLE_RESULT_INVALID_ENUM_VALUE);
  /* ThreadContext: x0 = the argument, pc = the entry, sp = the stack top. */
  regs->x[0] = SCRATCH(0xB000);
  regs->x[1] = thread;
  hle_on_svc(g_emu.cpu_state, 0x33, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
  uint64_t ctx_x0 = 0, ctx_sp = 0, ctx_pc = 0;
  CHECK_OK(vmm_read64(g_emu.vmm, SCRATCH(0xB000), &ctx_x0));
  CHECK_OK(vmm_read64(g_emu.vmm, SCRATCH(0xB000) + 8u * 31u, &ctx_sp));
  CHECK_OK(vmm_read64(g_emu.vmm, SCRATCH(0xB000) + 8u * 32u, &ctx_pc));
  CHECK(ctx_x0 == 0x1234 && ctx_pc == entry && ctx_sp == stack_top - 0x1000u);
  regs->x[0] = thread;
  regs->x[1] = 0; /* runnable */
  hle_on_svc(g_emu.cpu_state, 0x32, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0 && !t->paused);
  regs->x[0] = 0xdead;
  regs->x[1] = 1;
  hle_on_svc(g_emu.cpu_state, 0x32, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == HLE_RESULT_INVALID_HANDLE);
}

/* svcSetThreadCoreMask: an affinity that excludes the ideal core moves the
 * ideal core into it (MK8DX's job workers pin to cores 1 and 2 and index
 * per-core contexts by GetCurrentProcessorNumber; both reporting core 0
 * made two workers share one). */
static void test_core_mask_moves_ideal_core(void) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[1] = g_emu.process.address_space.code.base + 0x40u;
  regs->x[2] = 0;
  regs->x[3] = g_emu.process.main_thread_stack.base + g_emu.process.main_thread_stack.size - 0x2000u;
  regs->x[4] = 44;
  regs->x[5] = 0; /* ideal core 0 */
  hle_on_svc(g_emu.cpu_state, 0x08, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
  const uint32_t thread = (uint32_t)regs->x[1];
  regs->x[0] = thread;
  regs->x[1] = 0xFFFFFFFDu; /* -3: keep the ideal core */
  regs->x[2] = 0x4;         /* core 2 only */
  hle_on_svc(g_emu.cpu_state, 0x0F, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
  regs->x[2] = thread;
  hle_on_svc(g_emu.cpu_state, 0x0E, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0 && regs->x[1] == 2u && regs->x[2] == 0x4u);
}

/* Threads whose handle is closed are reclaimed once they can never run:
 * creating and joining short-lived workers far past the thread table's
 * size keeps working (a Unity title does this every few frames), while a
 * live thread with a closed handle keeps its slot. */
static uint32_t create_thread(void) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[1] = g_emu.process.address_space.code.base + 0x40u;
  regs->x[2] = 0;
  regs->x[3] = g_emu.process.main_thread_stack.base + g_emu.process.main_thread_stack.size - 0x1000u;
  regs->x[4] = 44;
  regs->x[5] = 0;
  hle_on_svc(g_emu.cpu_state, 0x08, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
  return (uint32_t)regs->x[1];
}

static void close_handle(uint32_t handle) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  regs->x[0] = handle;
  hle_on_svc(g_emu.cpu_state, 0x16, &g_emu.hle);
  CHECK((uint32_t)regs->x[0] == 0);
}

static void test_thread_reclaim(void) {
  const uint32_t live = create_thread();
  Sched_Thread *keeper = (Sched_Thread *)handle_table_get(&g_emu.process.handles, live, KERNEL_OBJECT_THREAD);
  CHECK(keeper != NULL);
  keeper->state = THREAD_STATE_WAITING; /* blocked forever: never runs in this test */
  keeper->wait = WAIT_SLEEP;
  const uint64_t keeper_id = keeper->thread_id;
  close_handle(live);
  for (uint32_t i = 0; i < 3u * SCHEDULER_MAX_THREADS; i++) {
    const uint32_t h = create_thread();
    Sched_Thread *t = (Sched_Thread *)handle_table_get(&g_emu.process.handles, h, KERNEL_OBJECT_THREAD);
    CHECK(t != NULL);
    if (!t) return;
    if (i % 2u) scheduler_exit_thread(&g_emu.scheduler, t, g_emu.cpu_backend); /* else: never started */
    close_handle(h);
  }
  CHECK(keeper->state == THREAD_STATE_WAITING && keeper->thread_id == keeper_id && keeper->handle_closed);
  scheduler_exit_thread(&g_emu.scheduler, keeper, g_emu.cpu_backend);
}

/* Small services the Nintendo SDK opens at start-up: lm, ectx:aw, ldr:ro,
 * aoc:u and the vi:s / vi:m ports all answer. */
/* IAudioOut Stop releases every queued buffer and signals the buffer
 * event (a Unity title's audio thread wakes on it and exits). hid writes
 * the SDK's NpadCondition block. ILibraryAppletCreator 3 creates applets. */
static void test_sdk_behaviours(void) {
  /* audout: open, register the event, append, stop. */
  const uint32_t audout = service("audout:u");
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xC000), 0x100, 0};
  m.receive_count = 1;
  m.sends[0] = (Test_Ipc_Buffer){SCRATCH(0xC100), 0x100, 0};
  m.send_count = 1;
  const uint32_t params[2] = {48000, 2};
  Test_Ipc_Reply r = call(audout, 1, params, sizeof(params), &m);
  CHECK(r.move_count == 1);
  const uint32_t out = r.move_handles[0];
  r = call(out, 4, NULL, 0, NULL);
  CHECK(r.copy_count == 1);
  Kernel_Event *ev = (Kernel_Event *)handle_table_get(&g_emu.process.handles, r.copy_handles[0],
                                                      KERNEL_OBJECT_EVENT_READABLE);
  CHECK(ev != NULL);
  const uint64_t desc[5] = {0, SCRATCH(0xD000), 0x400, 0x400, 0};
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0xC200), desc, sizeof(desc)));
  Test_Ipc_Message a;
  memset(&a, 0, sizeof(a));
  a.sends[0] = (Test_Ipc_Buffer){SCRATCH(0xC200), sizeof(desc), 0};
  a.send_count = 1;
  const uint64_t tag = 0x77;
  CHECK(call(out, 3, &tag, sizeof(tag), &a).result == 0);
  ev->signaled = false;
  CHECK(call(out, 2, NULL, 0, NULL).result == 0); /* Stop */
  CHECK(ev->signaled && g_emu.audout.queue_count == 0 && g_emu.audout.released_count >= 1);

  /* hid NpadCondition: hold type + initialized at 0x3E200. */
  const uint32_t hid = service("hid");
  const uint64_t aruid = 0;
  const uint32_t resource = object(hid, 0, &aruid, sizeof(aruid));
  (void)call(resource, 0, NULL, 0, NULL); /* the shared memory exists */
  const struct { uint64_t aruid, type; } hold = {0, 1};
  CHECK(call(hid, 120, &hold, sizeof(hold), NULL).result == 0);
  uint32_t held = 0;
  uint8_t initialized = 0;
  const uint64_t base = g_emu.hid.shared_memory->guest_pa + HID_NPAD_CONDITION_OFFSET;
  CHECK_OK(vmm_read_physical(g_emu.vmm, base + HID_NPAD_CONDITION_HOLD_TYPE, &held, sizeof(held)));
  CHECK_OK(vmm_read_physical(g_emu.vmm, base + HID_NPAD_CONDITION_INITIALIZED, &initialized, sizeof(initialized)));
  CHECK(held == 1 && initialized == 1);
}

static void test_sdk_startup_services(void) {
  const uint32_t lm = service("lm");
  const uint32_t logger = object(lm, 0, NULL, 0);
  CHECK(call(logger, 0, NULL, 0, NULL).result == 0);
  const uint32_t ectx = service("ectx:aw");
  const uint32_t registrar = object(ectx, 0, NULL, 0);
  CHECK(call(registrar, 0, NULL, 0, NULL).result == 0);
  const uint32_t ro = service("ldr:ro");
  CHECK(call(ro, 4, NULL, 0, NULL).result == 0);
  /* LoadModule: a 3-page NRO (.text, .rodata, .data) plus a 1-page .bss
   * buffer is aliased at a fresh address with per-segment permissions; its
   * buffers become inaccessible until UnloadModule hands them back. */
  {
    const uint64_t nro = SCRATCH(0x20000), bss = SCRATCH(0x24000);
    uint8_t header[0x80];
    memset(header, 0, sizeof(header));
    const uint32_t fields[][2] = {{0x10, 0x304F524Eu}, {0x18, 0x3000}, {0x20, 0}, {0x24, 0x1000}, {0x28, 0x1000},
                                  {0x2C, 0x1000}, {0x30, 0x2000}, {0x34, 0x1000}, {0x38, 0x1000}};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) memcpy(header + fields[i][0], &fields[i][1], 4);
    CHECK_OK(vmm_write_block(g_emu.vmm, nro, header, sizeof(header)));
    CHECK_OK(vmm_write32(g_emu.vmm, nro + 0x2000, 0xDA7A));
    const uint64_t args[5] = {0, nro, 0x3000, bss, 0x1000};
    Test_Ipc_Reply loaded = call(ro, 0, args, sizeof(args), NULL);
    const uint64_t base = test_le64(loaded.data);
    CHECK(base != 0 && (base & 0x1FFFFFu) == 0);
    VMM_Region_Info info;
    CHECK_OK(vmm_query(g_emu.vmm, base, &info));
    CHECK(info.is_mapped && info.perms == VMM_PERM_RX);
    CHECK_OK(vmm_query(g_emu.vmm, base + 0x1000, &info));
    CHECK(info.perms == VMM_PERM_R);
    CHECK_OK(vmm_query(g_emu.vmm, base + 0x2000, &info));
    CHECK(info.perms == VMM_PERM_RW && info.base_gva + info.size >= base + 0x4000); /* .data then .bss */
    uint32_t word = 0;
    CHECK_OK(vmm_read32(g_emu.vmm, base + 0x2000, &word));
    CHECK(word == 0xDA7A); /* the same pages, not a copy */
    CHECK_OK(vmm_query(g_emu.vmm, nro, &info));
    CHECK(info.perms == VMM_PERM_NONE);
    /* A second module goes elsewhere; a non-NRO is refused. */
    const uint64_t junk_args[5] = {0, SCRATCH(0x28000), 0x1000, 0, 0};
    (void)call_ex(ro, 0, junk_args, sizeof(junk_args), NULL, HLE_RESULT_INVALID_MEMORY_STATE);
    const uint64_t unload_args[2] = {0, base};
    CHECK(call(ro, 1, unload_args, sizeof(unload_args), NULL).result == 0);
    CHECK_OK(vmm_query(g_emu.vmm, base, &info));
    CHECK(!info.is_mapped);
    CHECK_OK(vmm_query(g_emu.vmm, nro, &info));
    CHECK(info.perms == VMM_PERM_RW);
    (void)call_ex(ro, 1, unload_args, sizeof(unload_args), NULL, HLE_RESULT_INVALID_MEMORY_STATE);
  }
  /* Offline answers for what big titles open at start-up (social.h). */
  {
    const uint32_t prepo = service("prepo:u");
    CHECK(call(prepo, 10104, NULL, 0, NULL).result == 0);
    CHECK(test_le32(call(prepo, 10300, NULL, 0, NULL).data) == 0);
    const uint32_t friends = object(service("friend:u"), 0, NULL, 0);
    CHECK(test_le32(call(friends, 10100, NULL, 0, NULL).data) == 0);
    CHECK(call(friends, 0, NULL, 0, NULL).copy_count == 1);
    const uint32_t notifications = object(service("friend:u"), 1, NULL, 0);
    (void)call_ex(notifications, 2, NULL, 0, NULL, FRIENDS_RESULT_NO_NOTIFICATION);
    const uint32_t storage = object(service("bcat:u"), 1, NULL, 0);
    CHECK(test_le32(call(storage, 10, NULL, 0, NULL).data) == 0);
    (void)call_ex(storage, 0, NULL, 0, NULL, BCAT_RESULT_NOT_FOUND);
    CHECK(object(service("bcat:u"), 0, NULL, 0) != 0);
    CHECK(call(service("caps:su"), 32, NULL, 0, NULL).result == 0);
    const uint32_t nfp = object(service("nfp:user"), 0, NULL, 0);
    CHECK(call(nfp, 0, NULL, 0, NULL).result == 0);
    CHECK(test_le32(call(nfp, 2, NULL, 0, NULL).data) == 0);
    CHECK(test_le32(call(nfp, 19, NULL, 0, NULL).data) == NFP_STATE_INITIALIZED);
    /* mii: an empty database; BuildDefault gives a CharInfo with a create
     * id and a name. */
    const uint32_t mii = object(service("mii:e"), 0, NULL, 0);
    CHECK(test_le32(call(mii, 2, NULL, 0, NULL).data) == 0);
    const uint32_t mii_index = 0;
    Test_Ipc_Reply built = call(mii, 7, &mii_index, sizeof(mii_index), NULL);
    CHECK(built.data[0] != 0 && built.data[0x10] == 'M' && built.data[0x12] == 'i');
    /* hwopus: packets decode to the right count of silent samples. */
    {
      const uint8_t celt_20ms = (uint8_t)(31u << 3); /* CELT FB 20 ms, one frame */
      CHECK(hwopus_packet_samples(&celt_20ms, 1, 48000) == 960);
      const uint8_t silk_two_10ms[2] = {(uint8_t)((0u << 3) | 1u), 0}; /* SILK 10 ms x2 */
      CHECK(hwopus_packet_samples(silk_two_10ms, 2, 48000) == 960);
      const uint8_t celt_code3[2] = {(uint8_t)((16u << 3) | 3u), 4}; /* 2.5 ms x4 */
      CHECK(hwopus_packet_samples(celt_code3, 2, 24000) == 240);
      const uint32_t open_params[2] = {48000, 2};
      const uint32_t decoder = object(service("hwopus"), 0, open_params, sizeof(open_params));
      const uint8_t packet[8 + 3] = {0, 0, 0, 3, 0, 0, 0, 0, celt_20ms, 0xAA, 0xBB};
      CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x2C000), packet, sizeof(packet)));
      CHECK_OK(vmm_write32(g_emu.vmm, SCRATCH(0x2D000), 0x1234));
      Test_Ipc_Message d;
      memset(&d, 0, sizeof(d));
      d.sends[0] = (Test_Ipc_Buffer){SCRATCH(0x2C000), sizeof(packet), 0};
      d.send_count = 1;
      d.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x2D000), 960 * 2 * 2, 0};
      d.receive_count = 1;
      Test_Ipc_Reply decoded = call(decoder, 0, NULL, 0, &d);
      CHECK(test_le32(decoded.data) == sizeof(packet) && test_le32(decoded.data + 4) == 960);
      uint32_t pcm = 1;
      CHECK_OK(vmm_read32(g_emu.vmm, SCRATCH(0x2D000), &pcm));
      CHECK(pcm == 0);
    }
    /* fatal:u stops the process as crashed. */
    const uint32_t fatal_result = 0x1234u;
    (void)call(service("fatal:u"), 1, &fatal_result, sizeof(fatal_result), NULL);
    CHECK(g_emu.scheduler.process_crashed);
    g_emu.scheduler.process_crashed = false;
  }
  const uint32_t aoc = service("aoc:u");
  Test_Ipc_Reply r = call(aoc, 2, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 0);
  r = call(aoc, 8, NULL, 0, NULL);
  CHECK(r.copy_count == 1);
  const uint32_t policy = 0;
  CHECK(object(service("vi:s"), 1, &policy, sizeof(policy)) != 0);
  CHECK(object(service("vi:m"), 2, &policy, sizeof(policy)) != 0);
  /* mm:u (the video decoders' clock manager): Initialize -> id, SetAndWait, Get. */
  const uint32_t mm = service("mm:u");
  const uint32_t init[3] = {2, 0, 0}; /* module, priority, event clear mode */
  r = call(mm, 4, init, sizeof(init), NULL);
  const uint32_t id = test_le32(r.data);
  CHECK(r.result == 0 && id != 0);
  const uint32_t set[3] = {id, 600000000u, 700000000u};
  CHECK(call(mm, 6, set, sizeof(set), NULL).result == 0);
  r = call(mm, 7, &id, sizeof(id), NULL);
  CHECK(r.result == 0 && test_le32(r.data) == 600000000u);
  CHECK(call(mm, 5, &id, sizeof(id), NULL).result == 0);
}

/* pctl: the SDK opens pctl and pctl:s, creates the service on each and
 * closes the port sessions - every handle must close. */
static void test_pctl(void) {
  CPU_Register_File *regs = g_emu.cpu_backend->get_register_file(g_emu.cpu_state);
  const char *const ports[] = {"pctl", "pctl:s"};
  for (uint32_t i = 0; i < 2u; i++) {
    const uint32_t port = service(ports[i]);
    const uint32_t pc = object(port, 1, NULL, 0);
    Test_Ipc_Reply r = call(pc, 1031, NULL, 0, NULL);
    CHECK(r.data[0] == 0); /* not restricted */
    regs->x[0] = port;
    hle_on_svc(g_emu.cpu_state, 0x16, &g_emu.hle);
    CHECK((uint32_t)regs->x[0] == 0);
    regs->x[0] = pc;
    hle_on_svc(g_emu.cpu_state, 0x16, &g_emu.hle);
    CHECK((uint32_t)regs->x[0] == 0);
  }
}

static void test_system_queries(void) {
  const uint32_t ns = service("ns:am2");
  const uint32_t manager = object(ns, 7996, NULL, 0);
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x9000), 0x180, 0};
  m.receive_count = 1;
  const uint32_t offset = 0;
  Test_Ipc_Reply r = call(manager, 0, &offset, sizeof(offset), &m);
  CHECK(test_le32(r.data) == 0);
  r = call(manager, 1, NULL, 0, NULL);
  CHECK(test_le64(r.data) == 0);
  const uint8_t control_in[16] = {0};
  (void)call_ex(manager, 400, control_in, sizeof(control_in), &m, NS_RESULT_APPLICATION_NOT_FOUND);
  /* IContentManagementInterface (7998, libnx's numbering): storage sizes,
   * no content meta, nothing running. */
  const uint32_t content = object(ns, 7998, NULL, 0);
  const uint64_t sd_storage = 5;
  r = call(content, 47, &sd_storage, sizeof(sd_storage), NULL);
  const uint64_t total = test_le64(r.data);
  r = call(content, 48, &sd_storage, sizeof(sd_storage), NULL);
  CHECK(total > 0 && test_le64(r.data) <= total);
  const uint64_t app_id = 0x0100000000010000ull;
  r = call(content, 600, &app_id, sizeof(app_id), NULL);
  CHECK(test_le32(r.data) == 0);
  r = call(content, 607, NULL, 0, NULL);
  CHECK(r.data[0] == 0);
  r = call(content, 11, &app_id, sizeof(app_id), NULL);
  CHECK(r.result == 0);
  CHECK(call(content, 43, NULL, 0, NULL).result == 0);
  r = call(manager, 44, NULL, 0, NULL);
  CHECK(r.copy_count == 1);
  const uint32_t pdm = service("pdm:qry");
  r = call(pdm, 5, NULL, 0, NULL);
  CHECK(test_le64(r.data) == 0 && test_le64(r.data + 8) == 0);
  const uint32_t pm = service("pm:shell");
  (void)call_ex(pm, 6, NULL, 0, NULL, PM_RESULT_PROCESS_NOT_FOUND);
  const uint32_t fs = service("fsp-srv");
  const uint8_t space = 1;
  const uint32_t reader = object(fs, 61, &space, sizeof(space));
  r = call(reader, 0, NULL, 0, &m);
  CHECK(test_le64(r.data) == 0);
  /* BIS: an empty, writable partition. */
  const uint32_t user_partition = 30;
  Test_Ipc_Message x = with_x(SCRATCH(0x9200), 0x300);
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x9200), "\0", 1));
  const Test_Ipc_Reply bis = call(fs, 11, &user_partition, sizeof(user_partition), &x);
  CHECK(bis.move_count == 1);
  /* ncm: storages open and are empty. */
  const uint32_t ncm = service("ncm");
  const uint8_t storage_id = 4; /* SdCard */
  const uint32_t storage = object(ncm, 4, &storage_id, sizeof(storage_id));
  r = call(storage, 12, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 0);
  const uint32_t database = object(ncm, 5, &storage_id, sizeof(storage_id));
  r = call(database, 7, NULL, 0, &m);
  CHECK(test_le32(r.data) == 0 && test_le32(r.data + 4) == 0);
  /* es: no tickets. */
  const uint32_t es = service("es");
  r = call(es, 9, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 0);
  /* usb:ds (11.0.0+ layout): detached. */
  const uint32_t usb = service("usb:ds");
  const uint32_t ds = object(usb, 0, NULL, 0);
  r = call(ds, 3, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 0);
  /* usb:hs (2.0.0+ layout): nothing plugged in; events are real handles
   * (a USB-drive thread waits on them), acquiring fails. */
  const uint32_t hs = service("usb:hs");
  CHECK(call(hs, 0, NULL, 0, NULL).result == 0);
  r = call(hs, 2, NULL, 0, NULL);
  CHECK(r.result == 0 && test_le32(r.data) == 0);
  r = call(hs, 4, NULL, 0, NULL);
  CHECK(r.result == 0 && r.copy_count == 1);
  r = call(hs, 6, NULL, 0, NULL);
  CHECK(r.result == 0 && r.copy_count == 1);
  (void)call_ex(hs, 7, NULL, 0, NULL, USB_RESULT_NOT_FOUND);
}


/* Software keyboard over a domain session, the way libnx's swkbdShow
 * drives it: storages pushed as input objects, Start, the host answers
 * the pending request, then PopOutData returns {close result, UTF-16}. */
static Test_Ipc_Reply dom(uint32_t session, uint32_t object, uint32_t command, const void *payload, uint32_t size,
                          const Test_Ipc_Message *extra, uint32_t out_size) {
  Test_Ipc_Message m;
  if (extra) m = *extra;
  else memset(&m, 0, sizeof(m));
  m.framing = TEST_IPC_CMIF;
  m.command_id = command;
  m.payload = payload;
  m.payload_size = size;
  m.domain = true;
  m.domain_type = 1;
  m.object_id = object;
  CHECK(ipc_fixture_send(&g_emu, session, &m, g_reply) == 0);
  Test_Ipc_Reply reply;
  test_ipc_parse_reply(g_reply, TEST_IPC_CMIF, true, out_size, &reply);
  return reply;
}

static uint32_t dom_storage(uint32_t session, uint32_t creator, const void *data, uint32_t size) {
  const uint64_t size64 = size;
  Test_Ipc_Reply r = dom(session, creator, 10, &size64, sizeof(size64), NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  const uint32_t storage = r.object_ids[0];
  r = dom(session, storage, 0, NULL, 0, NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  const uint32_t accessor = r.object_ids[0];
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0xA000), data, size));
  Test_Ipc_Message w;
  memset(&w, 0, sizeof(w));
  w.sends[0] = (Test_Ipc_Buffer){SCRATCH(0xA000), size, 0};
  w.send_count = 1;
  const uint64_t offset = 0;
  CHECK(dom(session, accessor, 10, &offset, sizeof(offset), &w, 0).result == 0);
  return storage;
}

static void test_software_keyboard(void) {
  const uint32_t oe = service("appletOE");
  Test_Ipc_Message control;
  memset(&control, 0, sizeof(control));
  control.framing = TEST_IPC_CONTROL;
  control.command_id = IPC_CONTROL_CONVERT_CURRENT_OBJECT_TO_DOMAIN;
  CHECK(ipc_fixture_send(&g_emu, oe, &control, g_reply) == 0);
  const uint64_t pid = 0;
  Test_Ipc_Message pid_msg;
  memset(&pid_msg, 0, sizeof(pid_msg));
  pid_msg.send_pid = true;
  Test_Ipc_Reply r = dom(oe, 1, 0, &pid, sizeof(pid), &pid_msg, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  const uint32_t proxy = r.object_ids[0];
  r = dom(oe, proxy, 11, NULL, 0, NULL, 0);
  const uint32_t creator = r.object_ids[0];
  const uint32_t create_in[2] = {AM_APPLET_SWKBD, 0};
  r = dom(oe, creator, 0, create_in, sizeof(create_in), NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  const uint32_t applet = r.object_ids[0];
  uint8_t common[0x20];
  memset(common, 0, sizeof(common));
  static uint8_t config[0x4C8];
  memset(config, 0, sizeof(config));
  const uint16_t header[] = {'N', 'a', 'm', 'e', 0};
  memcpy(config + 0x3C, header, sizeof(header));
  const uint32_t max_length = 8;
  memcpy(config + 0x3AC, &max_length, 4);
  const uint32_t storages[2] = {dom_storage(oe, creator, common, sizeof(common)),
                                dom_storage(oe, creator, config, sizeof(config))};
  for (uint32_t i = 0; i < 2u; i++) {
    Test_Ipc_Message push;
    memset(&push, 0, sizeof(push));
    push.in_objects[0] = storages[i];
    push.in_object_count = 1;
    CHECK(dom(oe, applet, 100, NULL, 0, &push, 0).result == 0);
  }
  CHECK(dom(oe, applet, 10, NULL, 0, NULL, 0).result == 0);
  r = dom(oe, applet, 1, NULL, 0, NULL, 4);
  CHECK(test_le32(r.data) == 0); /* waiting for the host */
  Am_Text_Request request;
  CHECK(emulator_text_request(&g_emu, &request) && strcmp(request.header, "Name") == 0 && request.max_length == 8);
  emulator_text_respond(&g_emu, "Voland in a browser", true);
  CHECK(!emulator_text_request(&g_emu, NULL));
  r = dom(oe, applet, 1, NULL, 0, NULL, 4);
  CHECK(test_le32(r.data) == 1);
  CHECK(dom(oe, applet, 30, NULL, 0, NULL, 0).result == 0);
  r = dom(oe, applet, 101, NULL, 0, NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  r = dom(oe, r.object_ids[0], 0, NULL, 0, NULL, 0);
  const uint32_t out_accessor = r.object_ids[0];
  Test_Ipc_Message rd;
  memset(&rd, 0, sizeof(rd));
  rd.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xB800), AM_SWKBD_OUTPUT_BYTES, 0};
  rd.receive_count = 1;
  const uint64_t offset = 0;
  CHECK(dom(oe, out_accessor, 11, &offset, sizeof(offset), &rd, 0).result == 0);
  uint8_t out[24];
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0xB800), out, sizeof(out)));
  uint16_t text[9];
  memcpy(text, out + 4, sizeof(text));
  CHECK(test_le32(out) == 0); /* accepted */
  CHECK(text[0] == 'V' && text[7] == 'i' && text[8] == 0); /* "Voland i": cut at max length 8 */
  /* No more output. */
  CHECK(dom(oe, applet, 101, NULL, 0, NULL, 0).result == AM_RESULT_NO_DATA_IN_CHANNEL);
}

/* MiiEdit's AppendMii ("Create a Mii") over a domain session: the applet
 * reports a created Mii at database index 0, and the Mii database then
 * lists it (SourceFlag_Database) before the six default Miis. */
static void test_mii_edit_append(void) {
  const uint32_t oe = service("appletOE");
  Test_Ipc_Message control;
  memset(&control, 0, sizeof(control));
  control.framing = TEST_IPC_CONTROL;
  control.command_id = IPC_CONTROL_CONVERT_CURRENT_OBJECT_TO_DOMAIN;
  CHECK(ipc_fixture_send(&g_emu, oe, &control, g_reply) == 0);
  const uint64_t pid = 0;
  Test_Ipc_Message pid_msg;
  memset(&pid_msg, 0, sizeof(pid_msg));
  pid_msg.send_pid = true;
  Test_Ipc_Reply r = dom(oe, 1, 0, &pid, sizeof(pid), &pid_msg, 0);
  const uint32_t proxy = r.object_ids[0];
  r = dom(oe, proxy, 11, NULL, 0, NULL, 0);
  const uint32_t creator = r.object_ids[0];
  const uint32_t create_in[2] = {AM_APPLET_MII_EDIT, 0};
  r = dom(oe, creator, 0, create_in, sizeof(create_in), NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  const uint32_t applet = r.object_ids[0];
  static uint8_t input[0x100];
  memset(input, 0, sizeof(input));
  const uint32_t version = 3, append_mii = 1;
  memcpy(input, &version, 4);
  memcpy(input + 4, &append_mii, 4);
  Test_Ipc_Message push;
  memset(&push, 0, sizeof(push));
  push.in_objects[0] = dom_storage(oe, creator, input, sizeof(input));
  push.in_object_count = 1;
  CHECK(dom(oe, applet, 100, NULL, 0, &push, 0).result == 0);
  CHECK(dom(oe, applet, 10, NULL, 0, NULL, 0).result == 0); /* Start */
  r = dom(oe, applet, 1, NULL, 0, NULL, 4);
  CHECK(test_le32(r.data) == 1); /* completed */
  r = dom(oe, applet, 101, NULL, 0, NULL, 0);
  CHECK(r.result == 0 && r.object_count == 1);
  r = dom(oe, r.object_ids[0], 0, NULL, 0, NULL, 0);
  Test_Ipc_Message rd;
  memset(&rd, 0, sizeof(rd));
  rd.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xB800), 0x20, 0};
  rd.receive_count = 1;
  const uint64_t offset = 0;
  CHECK(dom(oe, r.object_ids[0], 11, &offset, sizeof(offset), &rd, 0).result == 0);
  CHECK(rd32(SCRATCH(0xB800)) == 0 && rd32(SCRATCH(0xB804)) == 0); /* success, index 0 */

  const uint32_t mii = service("mii:u");
  const uint32_t database = object(mii, 0, NULL, 0);
  const uint32_t database_flag = 1, both = 3;
  r = call(database, 0, &database_flag, sizeof(database_flag), NULL);
  CHECK(test_le32(r.data) == 1); /* IsUpdated: once */
  r = call(database, 0, &database_flag, sizeof(database_flag), NULL);
  CHECK(test_le32(r.data) == 0);
  r = call(database, 2, &database_flag, sizeof(database_flag), NULL);
  CHECK(test_le32(r.data) == 1);
  r = call(database, 2, &both, sizeof(both), NULL);
  CHECK(test_le32(r.data) == 7);
  Test_Ipc_Message list;
  memset(&list, 0, sizeof(list));
  list.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xC000), 7u * 0x5Cu, 0};
  list.receive_count = 1;
  r = call(database, 3, &both, sizeof(both), &list); /* Get: CharInfoElement {CharInfo, u32 source} */
  CHECK(test_le32(r.data) == 7);
  CHECK(rd32(SCRATCH(0xC000) + 0x58) == 0 && rd32(SCRATCH(0xC000) + 0x5C + 0x58) == 1); /* Database, then Default */
  uint16_t name[6];
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0xC000) + 0x10, name, sizeof(name)));
  CHECK(name[0] == 'P' && name[5] == 'r');
}

static void test_audren(void) {
  const uint32_t manager = service("audren:u");
  /* AudioRendererParameter {48000, 240, mix buffers 2, submixes 0, voices 2,
   * sinks 1, effects 0, ..., revision @0x30} + pad + work size + aruid. */
  uint8_t param[0x48];
  memset(param, 0, sizeof(param));
  put32(param + 0x00, 48000);
  put32(param + 0x04, 240);
  put32(param + 0x08, 2);
  put32(param + 0x10, 2);
  put32(param + 0x14, 1);
  put32(param + 0x30, 0x34564552u); /* REV4 */
  Test_Ipc_Reply r = call(manager, 1, param, 0x34, NULL);
  CHECK(test_le64(r.data) >= 0x10000u && (test_le64(r.data) & 0xFFFu) == 0);
  const uint32_t renderer = object(manager, 0, param, sizeof(param));
  r = call(renderer, 0, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 48000);
  r = call(renderer, 1, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 240);

  enum { MEMPOOLS = 8, VOICES = 2, FRAMES = 480 };
  static uint8_t in[0x40 + 0x10 + MEMPOOLS * 0x20 + VOICES * 0x70 + VOICES * 0x170 + 0x930 + 0x140 + 0x10];
  memset(in, 0, sizeof(in));
  put32(in + 0x00, 0x34564552u);
  put32(in + 0x04, 0x10);
  put32(in + 0x08, MEMPOOLS * 0x20);
  put32(in + 0x0C, VOICES * 0x170);
  put32(in + 0x10, VOICES * 0x70);
  put32(in + 0x18, 0x930);
  put32(in + 0x1C, 0x140);
  put32(in + 0x20, 0x10);
  put32(in + 0x3C, sizeof(in));
  uint8_t *p = in + 0x40 + 0x10;
  put32(p + 0x10, 4); /* mempool 0: RequestAttach */
  p += MEMPOOLS * 0x20;
  uint8_t *channels = p;
  put32(channels, 0);
  putf(channels + 4, 1.0f);
  putf(channels + 8, 0.5f);
  channels[0x64] = 1;
  put32(channels + 0x70, 1);
  p += VOICES * 0x70;
  uint8_t *voice = p;
  put32(voice + 0x00, 0);
  voice[0x08] = 1; /* new */
  voice[0x09] = 1; /* used */
  voice[0x0A] = 0; /* started */
  voice[0x0B] = 2; /* PCM16 */
  put32(voice + 0x0C, 48000);
  put32(voice + 0x18, 1);
  putf(voice + 0x1C, 1.0f);
  putf(voice + 0x20, 1.0f);
  put32(voice + 0x3C, 1);   /* one wave buffer at head 0 */
  put32(voice + 0x58, 0);   /* final mix */
  put64(voice + 0x60, SCRATCH(0x20000));
  put64(voice + 0x68, FRAMES * 2u);
  put32(voice + 0x70, 0);
  put32(voice + 0x74, FRAMES);
  put32(voice + 0x140, 0);  /* channel resource 0 */
  put32(voice + 0x170, 1);  /* voice 1: unused */
  p += VOICES * 0x170;
  putf(p + 0x00, 1.0f);
  put32(p + 0x04, 48000);
  put32(p + 0x08, 2);
  p[0x0C] = 1;
  put32(p + 0x10, 0);
  put32(p + 0x924, 0x7FFFFFFFu);
  p += 0x930;
  p[0] = 1; /* device sink */
  p[1] = 1;
  memcpy(p + 0x20, "MainAudioOut", 12);
  put32(p + 0x120, 2);
  p[0x124] = 0;
  p[0x125] = 1;
  int16_t pcm[FRAMES];
  for (int i = 0; i < FRAMES; i++) pcm[i] = (int16_t)(i * 50);
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x20000), pcm, sizeof(pcm)));
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x10000), in, sizeof(in)));
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.receives[0] = (Test_Ipc_Buffer){SCRATCH(0x18000), 0x1000, 0};
  m.receives[1] = (Test_Ipc_Buffer){SCRATCH(0x19000), 0, 0};
  m.receive_count = 2;
  m.sends[0] = (Test_Ipc_Buffer){SCRATCH(0x10000), sizeof(in), 0};
  m.send_count = 1;
  (void)call(renderer, 4, NULL, 0, &m);
  uint8_t out[0x200];
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0x18000), out, sizeof(out)));
  CHECK(test_le32(out + 0x08) == MEMPOOLS * 0x10 && test_le32(out + 0x0C) == VOICES * 0x10);
  CHECK(test_le32(out + 0x40) == 5); /* mempool 0 attached */

  r = call(renderer, 7, NULL, 0, NULL);
  Kernel_Event *event = handle_table_get(&g_emu.process.handles, r.copy_handles[0], KERNEL_OBJECT_EVENT_READABLE);
  CHECK(event && !event->signaled);
  audio_ring_reset();
  (void)call(renderer, 5, NULL, 0, NULL); /* Start */
  const uint64_t t0 = g_emu.scheduler.ticks;
  audren_update(g_emu.audren, &g_emu.hle, t0 + 2u * AUDREN_TICKS_PER_FRAME);
  CHECK(event->signaled && g_emu.audren->frames_rendered == 2);
  static float ring[FRAMES * 2];
  CHECK(audio_ring_drain(ring, FRAMES) == FRAMES);
  CHECK(ring[2 * 100] == 5000.0f / 32768.0f && ring[2 * 100 + 1] == 2500.0f / 32768.0f);

  /* The next update reports the buffer consumed and 480 samples played. */
  voice[0x08] = 0;
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0x10000), in, sizeof(in)));
  (void)call(renderer, 4, NULL, 0, &m);
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0x18000), out, sizeof(out)));
  const uint32_t voice_out = 0x40 + MEMPOOLS * 0x10;
  CHECK(test_le64(out + voice_out) == FRAMES && test_le32(out + voice_out + 8) == 1);
  r = call(renderer, 3, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 0); /* started */
  (void)call(renderer, 6, NULL, 0, NULL);
  CHECK(audren_next_wake(g_emu.audren) == UINT64_MAX);
}

static void test_audout(void) {
  const uint32_t manager = service("audout:u");
  const uint32_t open_in[4] = {48000, 0x00020000, 0, 0};
  Test_Ipc_Message m;
  memset(&m, 0, sizeof(m));
  m.sends[0] = (Test_Ipc_Buffer){SCRATCH(0xB000), 0x100, 0};
  m.send_count = 1;
  m.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xB100), 0x100, 0};
  m.receive_count = 1;
  Test_Ipc_Reply r = call(manager, 1, open_in, sizeof(open_in), &m);
  CHECK(r.move_count == 1 && test_le32(r.data) == 48000 && test_le32(r.data + 4) == 2 && test_le32(r.data + 8) == 2);
  const uint32_t out = r.move_handles[0];
  r = call(out, 4, NULL, 0, NULL);
  Kernel_Event *event = handle_table_get(&g_emu.process.handles, r.copy_handles[0], KERNEL_OBJECT_EVENT_READABLE);
  CHECK(event && !event->signaled);

  /* 480 stereo frames (10ms): left = i, right = -i. */
  enum { FRAMES = 480 };
  int16_t pcm[FRAMES * 2];
  for (int i = 0; i < FRAMES; i++) {
    pcm[i * 2] = (int16_t)(i * 50);
    pcm[i * 2 + 1] = (int16_t)(-i * 50);
  }
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0xC000), pcm, sizeof(pcm)));
  const uint64_t desc[5] = {0, SCRATCH(0xC000), sizeof(pcm), sizeof(pcm), 0};
  CHECK_OK(vmm_write_block(g_emu.vmm, SCRATCH(0xB200), desc, sizeof(desc)));
  Test_Ipc_Message a;
  memset(&a, 0, sizeof(a));
  a.sends[0] = (Test_Ipc_Buffer){SCRATCH(0xB200), sizeof(desc), 0};
  a.send_count = 1;
  const uint64_t tag = 0xA0D10;
  (void)call(out, 3, &tag, sizeof(tag), &a);
  r = call(out, 6, &tag, sizeof(tag), NULL);
  CHECK(r.data[0] == 1);

  audio_ring_reset();
  (void)call(out, 1, NULL, 0, NULL); /* Start */
  const uint64_t t0 = g_emu.scheduler.ticks;
  audout_update(&g_emu.audout, &g_emu.hle, t0 + 240u * AUDOUT_TICKS_PER_FRAME);  /* half */
  CHECK(!event->signaled && g_emu.audout.played_frames == 240);
  audout_update(&g_emu.audout, &g_emu.hle, t0 + 480u * AUDOUT_TICKS_PER_FRAME + 399u); /* the rest, +399 ticks */
  CHECK(event->signaled && g_emu.audout.played_frames == 480 && g_emu.audout.tick_remainder == 399);
  static float ring[FRAMES * 2];
  CHECK(audio_ring_drain(ring, FRAMES) == FRAMES);
  CHECK(ring[0] == 0.0f && ring[2 * 100] == 5000.0f / 32768.0f && ring[2 * 100 + 1] == -5000.0f / 32768.0f);

  /* Released: the tag comes back once. */
  Test_Ipc_Message b;
  memset(&b, 0, sizeof(b));
  b.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xB300), 8, 0};
  b.receive_count = 1;
  r = call(out, 5, NULL, 0, &b);
  CHECK(test_le32(r.data) == 1 && rd64(SCRATCH(0xB300)) == tag);
  /* Nothing released: the slot reads as null (the SDK returns slot 0
   * without checking the count). */
  CHECK_OK(vmm_write64(g_emu.vmm, SCRATCH(0xB300), 0xDEADBEEFull));
  r = call(out, 5, NULL, 0, &b);
  CHECK(test_le32(r.data) == 0 && rd64(SCRATCH(0xB300)) == 0);
  r = call(out, 10, NULL, 0, NULL);
  CHECK(test_le64(r.data) == 480);
}

static void test_acc(void) {
  const uint32_t acc = service("acc:u0");
  Test_Ipc_Reply r = call(acc, 0, NULL, 0, NULL);
  CHECK(test_le32(r.data) == 1);
  r = call(acc, 4, NULL, 0, NULL);
  uint8_t uid[16];
  memcpy(uid, r.data, sizeof(uid));
  uint8_t ours[16];
  acc_user_uid(ours);
  CHECK(memcmp(uid, ours, 16) == 0);
  const uint32_t profile = object(acc, 5, uid, sizeof(uid));
  r = call(profile, 1, NULL, 0, NULL);
  CHECK(memcmp(r.data, ours, 16) == 0 && strcmp((const char *)r.data + 24, "Player") == 0);
  r = call(profile, 10, NULL, 0, NULL);
  const uint32_t size = test_le32(r.data);
  CHECK(size == k_acc_profile_icon_size);
  Test_Ipc_Message b;
  memset(&b, 0, sizeof(b));
  b.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xD000), 0x2000, 0};
  b.receive_count = 1;
  r = call(profile, 11, NULL, 0, &b);
  CHECK(test_le32(r.data) == size && rd32(SCRATCH(0xD000)) == 0xE0FFD8FFu); /* JPEG SOI + APP0 */
  uint8_t other[16] = {1};
  (void)call_ex(acc, 5, other, sizeof(other), NULL, (100u << 9) | 124u);

  /* The preselected user, once, as the launch parameter libnx parses. */
  const uint32_t oe = service("appletOE");
  const uint64_t reserved = 0;
  const uint32_t proxy = object(oe, 0, &reserved, sizeof(reserved));
  const uint32_t functions = object(proxy, 20, NULL, 0);
  const uint32_t kind = 2;
  r = call(functions, 1, &kind, sizeof(kind), NULL);
  CHECK(r.move_count == 1);
  const uint32_t accessor = object(r.move_handles[0], 0, NULL, 0);
  Test_Ipc_Message rb;
  memset(&rb, 0, sizeof(rb));
  rb.receives[0] = (Test_Ipc_Buffer){SCRATCH(0xF000), 0x88, 0};
  rb.receive_count = 1;
  const uint64_t zero = 0;
  (void)call(accessor, 11, &zero, sizeof(zero), &rb);
  CHECK(rd32(SCRATCH(0xF000)) == 0xC79497CAu && (rd32(SCRATCH(0xF004)) & 0xFF) == 1);
  uint8_t stored[16];
  CHECK_OK(vmm_read_block(g_emu.vmm, SCRATCH(0xF008), stored, 16));
  CHECK(memcmp(stored, ours, 16) == 0);
  (void)call_ex(functions, 1, &kind, sizeof(kind), NULL, AM_RESULT_NO_DATA_IN_CHANNEL);
}

int main(void) {
  ipc_fixture_boot(&g_emu);
  g_scratch = g_emu.process.main_thread_stack.base + 0x10000;
  test_vi();
  test_fs();
  test_time();
  test_set_apm_am();
  test_audout();
  test_audren();
  test_sd_manifest();
  test_system_queries();
  test_pctl();
  test_thread_activity();
  test_core_mask_moves_ideal_core();
  test_thread_reclaim();
  test_sdk_startup_services();
  test_sdk_behaviours();
  test_software_keyboard();
  test_mii_edit_append();
  test_acc();
  emulator_destroy(&g_emu);
  printf("[services_test] passed\n");
  return 0;
}
