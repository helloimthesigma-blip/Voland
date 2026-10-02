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
  const uint8_t *region = (const uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base;
  const uint8_t *pixels = region + LAYOUT_FRAMEBUFFER_HEADER_BYTES;
  uint32_t meta[4];
  memcpy(meta, region + FRAMEBUFFER_OFFSET_METADATA, sizeof(meta));
  CHECK(meta[0] == SURFACE_W && meta[1] == SURFACE_H && meta[2] == SURFACE_W * 4u && meta[3] == FRAMEBUFFER_FORMAT_RGBA8);
  CHECK(memcmp(pixels, linear, sizeof(linear)) == 0);
  CHECK(!release->signaled); /* the only buffer is on screen */

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
  (void)call_ex(fsp, 200, NULL, 0, NULL, FS_RESULT_TARGET_NOT_FOUND);
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
}


/* ns:am2 (no installed titles), pdm:qry (no history), pm:shell (no other
 * application), fsp-srv save-data info readers (empty). */
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
  r = call(out, 5, NULL, 0, &b);
  CHECK(test_le32(r.data) == 0);
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
  test_acc();
  emulator_destroy(&g_emu);
  printf("[services_test] passed\n");
  return 0;
}
